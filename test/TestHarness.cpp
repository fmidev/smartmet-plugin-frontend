#include "TestHarness.h"

#include <boost/algorithm/string.hpp>
#include <macgyver/Base64.h>
#include <macgyver/Exception.h>
#include <arpa/inet.h>
#include <signal.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
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
 *  Get TCP/IP port which specified process is listening on, or -1 if it is not
 *  listening on any.
 *
 *  This is done by parsing /usr/bin/ss output. Only the lines naming this very
 *  process are looked at: a bare PID match would also accept a line that merely
 *  happens to contain those digits in a port number.
 */
int try_get_process_port(pid_t pid)
{
    const std::string marker = "pid=" + std::to_string(pid) + ",";
    FILE* pipe = popen("ss -lntp 2>/dev/null", "r");
    if (!pipe)
    {
        return -1;
    }

    char buffer[512];
    int port = -1;
    while (port == -1 && fgets(buffer, sizeof(buffer), pipe) != nullptr)
    {
        const std::string line(buffer);
        if (line.find(marker) == std::string::npos)
        {
            continue;
        }

        // State Recv-Q Send-Q Local-Address:Port Peer-Address:Port Process
        std::vector<std::string> fields;
        ba::split(fields, line, ba::is_space(), ba::token_compress_on);
        if (fields.size() < 4)
        {
            continue;
        }

        const std::string& local = fields[3];
        const std::size_t colon = local.rfind(':');
        if (colon == std::string::npos)
        {
            continue;
        }

        try
        {
            port = std::stoi(local.substr(colon + 1));
        }
        catch (const std::exception&)
        {
            // Not a port after all: keep looking
        }
    }
    pclose(pipe);

    return port;
}

/**
 *  Get TCP/IP port which specified process is listening on.
 */
int get_process_port(pid_t pid)
try
{
    const int port = try_get_process_port(pid);
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
 *  Which smartmetd to run. Overridable so that a locally built server can be
 *  tested without installing it, which is the difference between measuring the
 *  change you just made and measuring the one that is already deployed.
 */
std::string smartmetd_path()
{
    const char* const from_environment = ::getenv("SMARTMETD");
    return (from_environment != nullptr && *from_environment != '\0') ? from_environment
                                                                     : "/usr/sbin/smartmetd";
}

/**
 *  Which gdb to run. Overridable the same way as smartmetd, so that a wrapper
 *  or a different build of gdb can be used without editing the tests.
 */
std::string gdb_path()
{
    const char* const from_environment = ::getenv("GDB");
    return (from_environment != nullptr && *from_environment != '\0') ? from_environment : "gdb";
}

namespace
{

pid_t parent_pid_of(pid_t pid)
{
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!std::getline(in, line))
    {
        return -1;
    }

    // The comm field is parenthesized and may contain spaces, so the fields
    // after it are only safe to read from the last ')'.
    const std::size_t comm_end = line.rfind(')');
    if (comm_end == std::string::npos)
    {
        return -1;
    }

    std::istringstream rest(line.substr(comm_end + 1));
    std::string state;
    pid_t ppid = -1;
    if (!(rest >> state >> ppid))
    {
        return -1;
    }
    return ppid;
}

bool is_descendant_of(pid_t pid, pid_t ancestor)
{
    for (int depth = 0; depth < 16 && pid > 1; depth++)
    {
        pid = parent_pid_of(pid);
        if (pid == ancestor)
        {
            return true;
        }
    }
    return false;
}

/**
 *  The port a descendant of the given process listens on, or -1.
 *
 *  gdb's inferior is a process of its own, and it only exists once "run" has
 *  been typed, so the port cannot be asked for at start like it is for the
 *  processes the tests start themselves.
 */
int find_descendant_port(pid_t ancestor)
try
{
    for (const auto& entry : std::filesystem::directory_iterator("/proc"))
    {
        const std::string name = entry.path().filename().string();
        if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos)
        {
            continue;
        }

        const pid_t pid = static_cast<pid_t>(std::stol(name));
        if (pid == ancestor || !is_descendant_of(pid, ancestor))
        {
            continue;
        }

        const int port = try_get_process_port(pid);
        if (port > 0)
        {
            return port;
        }
    }
    return -1;
}
catch (const std::exception&)
{
    // Processes come and go under /proc while it is being read: not an error
    return -1;
}

}  // anonymous namespace

int run_under_gdb(const std::string& command,
                  const std::vector<std::string>& args,
                  const std::string& process_name)
{
    const std::string gdb = gdb_path();

    std::vector<std::string> gdb_args = {"--args", command};
    gdb_args.insert(gdb_args.end(), args.begin(), args.end());

    std::cout << "Starting " << process_name << " under " << gdb << ':';
    for (const auto& arg : gdb_args)
    {
        std::cout << ' ' << arg;
    }
    std::cout << "\nType \"run\" at the (gdb) prompt to start it, \"quit\" when done." << std::endl;

    const pid_t pid = fork();
    if (pid < 0)
    {
        throw std::runtime_error("Failed to fork gdb process");
    }

    if (pid == 0)
    {
        // No redirection here, unlike start_background_process(): gdb is meant to
        // be typed at, so it inherits the terminal
        std::vector<char*> c_args;
        c_args.push_back(const_cast<char*>(gdb.c_str()));
        for (const auto& arg : gdb_args)
        {
            c_args.push_back(const_cast<char*>(arg.c_str()));
        }
        c_args.push_back(nullptr);

        execvp(gdb.c_str(), c_args.data());
        std::cerr << "Failed to execute " << gdb << ": " << std::strerror(errno) << std::endl;
        _exit(127);
    }

    // The inferior picks its port with --port=0, and only once it is run, so it
    // is polled for and reported whenever it changes - a second "run" gets a
    // different port and the old one would send the requests nowhere.
    std::atomic<bool> finished{false};
    std::thread port_reporter(
        [pid, &process_name, &finished]()
        {
            int reported = -1;
            while (!finished.load())
            {
                const int port = find_descendant_port(pid);
                if (port != reported)
                {
                    reported = port;
                    if (port > 0)
                    {
                        std::cout << "\n" << process_name << " under gdb is listening on port "
                                  << port << std::endl;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        });

    int status = 0;
    const pid_t waited = waitpid(pid, &status, 0);
    finished.store(true);
    port_reporter.join();

    if (waited != pid)
    {
        std::cerr << "Failed to wait for gdb: " << std::strerror(errno) << std::endl;
        return -1;
    }

    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }

    if (WIFSIGNALED(status))
    {
        const int sig = WTERMSIG(status);
        std::cout << "gdb terminated by signal " << sig << " (" << strsignal(sig) << ')'
                  << std::endl;
    }
    return -1;
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
            smartmetd_path(),
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
        smartmetd_path(),
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

// ----------------------------------------------------------------------
// HttpConnection
// ----------------------------------------------------------------------

namespace
{
// Case-insensitive search for a header value in a response head
std::string header_value(const std::string& head_lowercase,
                         const std::string& head,
                         const std::string& name_lowercase)
{
    std::size_t pos = 0;
    while ((pos = head_lowercase.find("\r\n", pos)) != std::string::npos)
    {
        const std::size_t line = pos + 2;
        const std::size_t line_end = head_lowercase.find("\r\n", line);
        if (line_end == std::string::npos)
        {
            break;
        }

        if (head_lowercase.compare(line, name_lowercase.size(), name_lowercase) == 0 &&
            head_lowercase[line + name_lowercase.size()] == ':')
        {
            const std::size_t value = line + name_lowercase.size() + 1;
            return ba::trim_copy(head.substr(value, line_end - value));
        }

        pos = line_end;
    }
    return {};
}
}  // namespace

HttpConnection::~HttpConnection()
{
    close_connection();
}

void HttpConnection::close_connection()
{
    if (itsFd >= 0)
    {
        close(itsFd);
        itsFd = -1;
    }
    itsBuffer.clear();
}

bool HttpConnection::open(int port, int recv_timeout_seconds, bool quick_ack)
{
    close_connection();
    itsRecvTimeoutSeconds = recv_timeout_seconds;
    itsQuickAck = quick_ack;

    itsFd = socket(AF_INET, SOCK_STREAM, 0);
    if (itsFd < 0)
    {
        return false;
    }

    if (recv_timeout_seconds > 0)
    {
        timeval tv{};
        tv.tv_sec = recv_timeout_seconds;
        setsockopt(itsFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    const int one = 1;
    setsockopt(itsFd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1 ||
        connect(itsFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
    {
        close_connection();
        return false;
    }

    return true;
}

bool HttpConnection::send_all(const std::string& data)
{
    std::size_t sent_total = 0;
    while (sent_total < data.size())
    {
        const ssize_t sent = send(itsFd, data.data() + sent_total, data.size() - sent_total, 0);
        if (sent <= 0)
        {
            return false;
        }
        sent_total += static_cast<std::size_t>(sent);
    }
    return true;
}

bool HttpConnection::fill_buffer()
{
    if (itsQuickAck)
    {
        // Not sticky: the kernel clears it once it has been acted on, so it has to
        // be set again before every read.
        const int one = 1;
        setsockopt(itsFd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));
    }

    char buffer[16384];
    const ssize_t received = recv(itsFd, buffer, sizeof(buffer), 0);
    if (received <= 0)
    {
        return false;
    }
    itsBuffer.append(buffer, static_cast<std::size_t>(received));
    return true;
}

int HttpConnection::get(const std::string& target, std::size_t& response_bytes)
{
    response_bytes = 0;

    if (itsFd < 0)
    {
        return -1;
    }

    // No Connection header: HTTP/1.1 keeps the connection unless told otherwise,
    // which is the whole point of this class.
    const std::string request = "GET " + target +
                                " HTTP/1.1\r\n"
                                "Host: localhost\r\n"
                                "\r\n";
    if (!send_all(request))
    {
        close_connection();
        return -1;
    }

    // Head first
    std::size_t head_end = itsBuffer.find("\r\n\r\n");
    while (head_end == std::string::npos)
    {
        if (!fill_buffer())
        {
            close_connection();
            return -1;
        }
        head_end = itsBuffer.find("\r\n\r\n");
    }

    const std::size_t head_size = head_end + 4;
    const std::string head = itsBuffer.substr(0, head_size);
    const std::string head_lc = ba::to_lower_copy(head);

    int status = -1;
    if (head.compare(0, 5, "HTTP/") == 0)
    {
        const std::size_t space = head.find(' ');
        if (space != std::string::npos)
        {
            try
            {
                status = std::stoi(head.substr(space + 1, 3));
            }
            catch (...)
            {
                status = -1;
            }
        }
    }

    if (status < 0)
    {
        close_connection();
        return -1;
    }

    const std::string transfer_encoding = header_value(head_lc, head, "transfer-encoding");
    const std::string content_length = header_value(head_lc, head, "content-length");

    // The server ends persistence by saying so, and it does say so: after
    // keepalive.maxrequests responses on one connection, and on any reply it
    // cannot frame. A client that ignores this reads the close as a failure and
    // reports the server's correct behaviour as an error.
    const bool server_closes =
        ba::to_lower_copy(header_value(head_lc, head, "connection")).find("close") !=
        std::string::npos;

    std::size_t body_size = 0;

    if (!transfer_encoding.empty())
    {
        if (ba::to_lower_copy(transfer_encoding).find("chunked") == std::string::npos)
        {
            close_connection();  // A framing we cannot follow
            return -1;
        }

        // Walk the chunks, reading more whenever the buffer runs out mid-chunk
        std::size_t pos = head_size;
        while (true)
        {
            std::size_t eol = itsBuffer.find("\r\n", pos);
            while (eol == std::string::npos)
            {
                if (!fill_buffer())
                {
                    close_connection();
                    return -1;
                }
                eol = itsBuffer.find("\r\n", pos);
            }

            std::size_t chunk_size = 0;
            try
            {
                chunk_size = std::stoul(itsBuffer.substr(pos, eol - pos), nullptr, 16);
            }
            catch (...)
            {
                close_connection();
                return -1;
            }

            // chunk data + its CRLF, or for the last chunk the trailer's blank line
            const std::size_t needed = eol + 2 + chunk_size + 2;
            while (itsBuffer.size() < needed)
            {
                if (!fill_buffer())
                {
                    close_connection();
                    return -1;
                }
            }

            pos = needed;
            body_size += chunk_size;

            if (chunk_size == 0)
            {
                break;
            }
        }

        response_bytes = pos;
        itsBuffer.erase(0, pos);
        if (server_closes)
        {
            close_connection();
        }
        return status;
    }

    if (!content_length.empty())
    {
        try
        {
            body_size = std::stoul(content_length);
        }
        catch (...)
        {
            close_connection();
            return -1;
        }

        while (itsBuffer.size() < head_size + body_size)
        {
            if (!fill_buffer())
            {
                close_connection();
                return -1;
            }
        }

        response_bytes = head_size + body_size;
        itsBuffer.erase(0, response_bytes);
        if (server_closes)
        {
            close_connection();
        }
        return status;
    }

    // Neither: the body ends when the connection does, so there is no next
    // response to keep this connection for.
    while (fill_buffer())
    {
    }
    response_bytes = itsBuffer.size();
    close_connection();
    return status;
}

}  // namespace TestHarness
