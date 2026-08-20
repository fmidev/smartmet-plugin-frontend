#include "TestHarness.h"

#include <boost/algorithm/string.hpp>
#include <macgyver/Base64.h>
#include <macgyver/Exception.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace ba = boost::algorithm;

namespace TestHarness
{

/**
 *  Starts a background process with the given command and arguments.
 *  The process's output is redirected to the specified log file.
 */
pid_t start_background_process(
    const std::string& command,
    const std::vector<std::string>& args,
    const std::string& log_file)
try
{
    pid_t pid = fork();
    if (pid < 0)
    {
        throw std::runtime_error("Failed to fork process");
    }
    else if (pid == 0)
    {
        // In child process
        std::vector<char*> c_args;
        c_args.push_back(const_cast<char*>(command.c_str()));
        for (const auto& arg : args)
        {
            c_args.push_back(const_cast<char*>(arg.c_str()));
        }
        c_args.push_back(nullptr);

        // Redirect stdout and stderr to log file
        freopen(log_file.c_str(), "a", stdout);
        freopen(log_file.c_str(), "a", stderr);

        execvp(command.c_str(), c_args.data());
        // If execvp returns, it must have failed
        std::cerr << "Failed to execute command: " << command << std::endl;
        exit(1);
    }
    // In parent process, return child's PID
    return pid;
}
catch (...)
{
    std::cout << Fmi::Exception(BCP, "Failed to start background process: " + command) << std::endl;
    throw; // Rethrow to allow handling in main
}

/**
 *  Get TCP/IP port which specified process is listening on.
 *
 *  This is done by from /usr/bin/ss output REGEX parsing.
 *  Ignore also UDP ports, as they are not used in this test.
 */
int get_process_port(pid_t pid)
try
{
    std::string command = "ss -lntp 2>/dev/null | grep " + std::to_string(pid) + " | grep -v udp";
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe)
    {
        throw std::runtime_error("Failed to run command: " + command);
    }

    char buffer[128];
    int port = -1;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
    {
        std::string line(buffer);
        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos)
        {
            size_t space_pos = line.find(' ', colon_pos);
            if (space_pos != std::string::npos)
            {
                port = std::stoi(line.substr(colon_pos + 1, space_pos - colon_pos - 1));
                break;
            }
        }
    }
    pclose(pipe);

    if (port == -1)
    {
        throw std::runtime_error("Failed to find listening port for PID: " + std::to_string(pid));
    }
    return port;
}
catch (...)
{
    std::cout << Fmi::Exception(BCP, "Failed to get process port for PID: " + std::to_string(pid)) << std::endl;
    throw; // Rethrow to allow handling in main
}

/**
 *   Start smartmet backend processes and return their PIDs and ports.
 */
std::vector<std::pair<pid_t, int>> start_backends(const std::vector<std::string>& config_files,
                                                  const std::string& log_prefix)
try
{
    int counter = 0;
    std::vector<std::pair<pid_t, int>> backends;
    for (const auto& config : config_files)
    {
        pid_t pid = start_background_process(
            "/usr/sbin/smartmetd",
            {
                "--configfile", config,
                "--port=0" // Let the backend choose an available port
            },
            log_prefix + std::to_string(++counter) + ".log");
        backends.emplace_back(pid, -1); // Temporarily store -1 for port until we retrieve it
    }

    // Give the processes some time to start and listen on ports
    // Ports should be available soon after process start, but we add a small delay to be safe and
    std::this_thread::sleep_for(std::chrono::seconds(1));

    for (auto& item : backends)
    {
        pid_t pid = item.first;
        int port = get_process_port(pid);
        item.second = port;
        std::cout << "Started backend with PID: " << pid << " on port: " << port << std::endl;
    }
    return backends;
}
catch (...)
{
    std::cout << Fmi::Exception(BCP, "Failed to start backend processes") << std::endl;
    throw; // Rethrow to allow handling in main
}

void stop_backends(const std::vector<std::pair<pid_t, int>>& backends)
try
{
    for (const auto& [pid, port] : backends)
    {
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        std::cout << "Stopped backend with PID: " << pid << " on port: " << port << std::endl;
    }
}
catch (...)
{
    std::cout << Fmi::Exception(BCP, "Failed to stop backend processes      ") << std::endl;
}

bool report_process_exit_status(const std::string& process_name,
                                pid_t pid,
                                int port,
                                int status)
{
    if (WIFEXITED(status))
    {
        const int exit_code = WEXITSTATUS(status);
        if (exit_code == 0)
        {
            std::cout << process_name << " with PID: " << pid << " on port: " << port
                      << " exited normally" << std::endl;
            return true;
        }

        std::cout << process_name << " with PID: " << pid << " on port: " << port
                  << " exited with non-zero code: " << exit_code << std::endl;
        return false;
    }

    if (WIFSIGNALED(status))
    {
        const int sig = WTERMSIG(status);
        std::cout << process_name << " with PID: " << pid << " on port: " << port
                  << " terminated by signal " << sig << " (" << strsignal(sig) << ")";
        if (sig == SIGSEGV)
        {
            std::cout << " [SIGSEGV]";
        }
        std::cout << std::endl;
        return false;
    }

    std::cout << process_name << " with PID: " << pid << " on port: " << port
              << " ended in unknown state" << std::endl;
    return false;
}

bool terminate_and_wait_process(const std::string& process_name, pid_t pid, int port)
{
    int status = 0;
    const pid_t first_wait = waitpid(pid, &status, WNOHANG);
    if (first_wait == pid)
    {
        return report_process_exit_status(process_name, pid, port, status);
    }

    if (first_wait == -1)
    {
        std::cerr << "Failed to query status for " << process_name << " PID " << pid
                  << ": " << std::strerror(errno) << std::endl;
        return false;
    }

    if (kill(pid, SIGTERM) == -1 && errno != ESRCH)
    {
        std::cerr << "Failed to send SIGTERM to " << process_name << " PID " << pid
                  << ": " << std::strerror(errno) << std::endl;
        return false;
    }

    const pid_t waited = waitpid(pid, &status, 0);
    if (waited == -1)
    {
        std::cerr << "Failed to wait " << process_name << " PID " << pid
                  << ": " << std::strerror(errno) << std::endl;
        return false;
    }

    return report_process_exit_status(process_name, pid, port, status);
}

bool stop_backends_checked(const std::vector<std::pair<pid_t, int>>& backends)
{
    bool all_ok = true;
    for (const auto& [pid, port] : backends)
    {
        const bool ok = terminate_and_wait_process("Backend", pid, port);
        all_ok = all_ok && ok;
    }
    return all_ok;
}

std::pair<pid_t, int> start_frontend(const std::string& config_file,
                                     const std::string& log_file)
try
{
    pid_t pid = start_background_process(
        "/usr/sbin/smartmetd",
        {
            "--configfile", config_file,
            "--port=0" // Let the frontend choose an available port
        },
        log_file);
    sleep(1); // Give the process some time to start and listen on the port
    int port = get_process_port(pid);
    std::cout << "Started frontend with PID: " << pid << " on port: " << port << std::endl;
    return {pid, port};
}
catch (...)
{
    std::cout << Fmi::Exception(BCP, "Failed to start frontend process") << std::endl;
    throw; // Rethrow to allow handling in main
}

std::string send_raw_http_request(int port,
                                  const std::string& request_text,
                                  int recv_timeout_seconds)
{
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0)
    {
        throw std::runtime_error("Failed to create socket");
    }

    if (recv_timeout_seconds > 0)
    {
        timeval tv{};
        tv.tv_sec = recv_timeout_seconds;
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1)
    {
        close(socket_fd);
        throw std::runtime_error("Failed to parse loopback address");
    }

    if (connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
    {
        close(socket_fd);
        throw std::runtime_error("Failed to connect to frontend on port " + std::to_string(port));
    }

    std::size_t sent_total = 0;
    while (sent_total < request_text.size())
    {
        ssize_t sent = send(socket_fd,
                            request_text.data() + sent_total,
                            request_text.size() - sent_total,
                            0);
        if (sent < 0)
        {
            close(socket_fd);
            throw std::runtime_error("Failed to send HTTP request");
        }
        sent_total += static_cast<std::size_t>(sent);
    }

    std::string response;
    char buffer[4096];
    while (true)
    {
        ssize_t received = recv(socket_fd, buffer, sizeof(buffer), 0);
        if (received == 0)
        {
            break;
        }
        if (received < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                // The receive timeout expired. Whatever arrived so far is the
                // answer - most usefully nothing at all, which the caller reads
                // as "this request was never answered".
                break;
            }
            close(socket_fd);
            throw std::runtime_error("Failed to receive HTTP response");
        }
        response.append(buffer, static_cast<std::size_t>(received));
    }

    close(socket_fd);
    return response;
}

std::string extract_http_body(const std::string& response)
{
    std::size_t separator = response.find("\r\n\r\n");
    if (separator != std::string::npos)
    {
        return response.substr(separator + 4);
    }

    separator = response.find("\n\n");
    if (separator != std::string::npos)
    {
        return response.substr(separator + 2);
    }

    std::cout << "--- Body:\n" << response << "\n--- End of body" << std::endl;
    throw std::runtime_error("HTTP response does not contain header/body separator");
}

void wait_for_ready(int port, const std::string& process_name, int max_wait_seconds)
{
    const std::string request = "GET /admin?what=waitforready&timeout=1 HTTP/1.0\r\n\r\n";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(max_wait_seconds);

    std::string last_result;
    while (std::chrono::steady_clock::now() < deadline)
    {
        try
        {
            const std::string response = send_raw_http_request(port, request);
            const std::string body = ba::trim_copy(extract_http_body(response));
            const std::string body_lc = ba::to_lower_copy(body);

            if (ba::starts_with(body_lc, "ready"))
            {
                std::cout << process_name << " on port " << port << " is ready: " << body
                          << std::endl;
                return;
            }

            last_result = body;
        }
        catch (const std::exception& e)
        {
            last_result = e.what();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    throw std::runtime_error("Timeout waiting for " + process_name + " on port " +
                             std::to_string(port) +
                             " to become ready. Last result: " + last_result);
}


int http_status_code(const std::string& response)
{
    // "HTTP/1.1 200 OK". An empty or truncated response means the request was
    // answered by the connection being dropped, which is not a status at all.
    if (response.size() < 12 || response.compare(0, 5, "HTTP/") != 0)
    {
        return -1;
    }

    const std::size_t space = response.find(' ');
    if (space == std::string::npos)
    {
        return -1;
    }

    try
    {
        return std::stoi(response.substr(space + 1, 3));
    }
    catch (...)
    {
        return -1;
    }
}

std::string http_get(int port,
                     const std::string& target,
                     const std::string& extra_headers,
                     int recv_timeout_seconds)
{
    // "Connection: close" is about this hop only. It keeps send_raw_http_request()
    // from waiting out the frontend's idle timeout, and says nothing about the
    // connections the frontend keeps to its backends.
    const std::string request = "GET " + target + " HTTP/1.1\r\n"
                                "Host: localhost\r\n" +
                                extra_headers +
                                "Connection: close\r\n"
                                "\r\n";
    return send_raw_http_request(port, request, recv_timeout_seconds);
}

std::string basic_auth_header(const std::string& user, const std::string& password)
{
    return "Authorization: Basic " + Fmi::Base64::encode(user + ":" + password) + "\r\n";
}

}  // namespace TestHarness
