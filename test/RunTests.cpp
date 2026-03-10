#include <iostream>
#include <iomanip>
#include <filesystem>
#include <vector>
#include <string>
#include <stdexcept>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <dtl/dtl.hpp>
#include <boost/algorithm/string.hpp>
#include <macgyver/AnsiEscapeCodes.h>
#include <macgyver/Exception.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <cerrno>
#include <cstring>

namespace ba = boost::algorithm;


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
std::vector<std::pair<pid_t, int>> start_backends(
    const std::vector<std::string>& config_files)
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
            "log/backend" + std::to_string(++counter) + ".log");
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

std::pair<pid_t, int> start_frontend(const std::string& config_file)
try
{
    pid_t pid = start_background_process(
        "/usr/sbin/smartmetd",
        {
            "--configfile", config_file,
            "--port=0" // Let the frontend choose an available port
        },
        "log/frontend.log");
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

std::string read_file_to_string(const std::filesystem::path& path)
{
    std::ifstream in(path);
    if (!in)
    {
        throw std::runtime_error("Failed to open file: " + path.string());
    }

    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

std::string normalize_line_endings(const std::string& text)
{
    std::string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); i++)
    {
        if (text[i] == '\r')
        {
            continue;
        }
        result.push_back(text[i]);
    }
    return result;
}

std::vector<std::string> read_file_lines_trimmed(const std::filesystem::path& path)
{
    std::ifstream input(path);
    std::vector<std::string> result;
    std::string line;
    while (std::getline(input, line))
    {
        result.push_back(ba::trim_right_copy_if(line, ba::is_any_of("\r\n")));
    }
    return result;
}

std::string get_diff(const std::filesystem::path& expected, const std::filesystem::path& actual)
{
    const auto f1 = read_file_lines_trimmed(expected);
    const auto f2 = read_file_lines_trimmed(actual);
    dtl::Diff<std::string> d(f1, f2);
    d.compose();
    d.composeUnifiedHunks();

    std::ostringstream out;
    d.printUnifiedFormat(out);
    std::string ret = out.str();

    if (ret.size() > 5000)
    {
        return "  Diff size " + std::to_string(ret.size()) + " is too big (>5000)";
    }
    return "\n" + ret;
}

void write_string_to_file(const std::filesystem::path& path, const std::string& content)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    if (!out)
    {
        throw std::runtime_error("Failed to open file for writing: " + path.string());
    }
    out << content;
}

std::string make_http_request_text(const std::filesystem::path& input_file,
                                   const std::string& input)
{
    if (ba::ends_with(input_file.string(), ".get") || ba::ends_with(input_file.string(), ".options"))
    {
        std::vector<std::string> lines;
        ba::split(lines, input, ba::is_any_of("\r\n"), ba::token_compress_on);
        return ba::trim_copy(ba::join(lines, "\r\n")) + "\r\n\r\n";
    }

    std::string normalized;
    normalized.reserve(input.size());

    for (std::size_t i = 0; i < input.size(); i++)
    {
        if (input[i] == '\r')
        {
            continue;
        }

        if (input[i] == '\n')
        {
            normalized += "\r\n";
            continue;
        }

        normalized.push_back(input[i]);
    }

    return normalized;
}

std::string send_raw_http_request(int port, const std::string& request_text)
{
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0)
    {
        throw std::runtime_error("Failed to create socket");
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

std::string remove_date_header_from_http_response(const std::string& response)
{
    const std::size_t separator = response.find("\r\n\r\n");
    if (separator == std::string::npos)
    {
        return response;
    }

    const std::string headers = response.substr(0, separator);
    const std::string body = response.substr(separator + 4);

    std::vector<std::string> lines;
    ba::split(lines, headers, ba::is_any_of("\r\n"), ba::token_compress_on);

    std::vector<std::string> filtered;
    filtered.reserve(lines.size());
    for (const auto& line : lines)
    {
        const std::string lower = ba::to_lower_copy(line);
        if (ba::starts_with(lower, "date:"))
        {
            continue;
        }
        filtered.push_back(line);
    }

    return ba::join(filtered, "\r\n") + "\r\n\r\n" + body;
}

bool run_tests(int frontend_port)
{
    namespace fs = std::filesystem;

    const fs::path input_dir("input");
    const fs::path output_dir("output");
    const fs::path failure_dir("failures");

    if (!fs::exists(input_dir) || !fs::is_directory(input_dir))
    {
        throw std::runtime_error("Input directory does not exist: " + input_dir.string());
    }
    if (!fs::exists(output_dir) || !fs::is_directory(output_dir))
    {
        throw std::runtime_error("Output directory does not exist: " + output_dir.string());
    }

    std::vector<fs::path> test_files;
    for (const auto& entry : fs::directory_iterator(input_dir))
    {
        if (entry.is_regular_file())
        {
            test_files.push_back(entry.path().filename());
        }
    }

    std::sort(test_files.begin(), test_files.end());
    if (test_files.empty())
    {
        throw std::runtime_error("No test input files found in: " + input_dir.string());
    }

    std::size_t passed = 0;
    std::size_t failed = 0;

    const bool is_tty = isatty(fileno(stdout));
    const std::string fg_fn = is_tty ? ANSI_FG_CYAN : "";
    const std::string fg_red = is_tty ? ANSI_FG_RED : "";
    const std::string fg_green = is_tty ? ANSI_FG_GREEN : "";
    const std::string fg_default = is_tty ? ANSI_FG_DEFAULT : "";

    std::cout << "Running tests against frontend on port " << frontend_port << std::endl;

    for (const auto& test_file : test_files)
    {
        std::ostringstream out;

        const fs::path input_file = input_dir / test_file;
        const fs::path expected_output_file = output_dir / test_file;

        out << fg_fn << test_file.string() << fg_default << ' ' << std::setw(50 - test_file.string().size())
            << std::setfill('.') << ". " << std::flush;

        try
        {
            const std::string raw_request = read_file_to_string(input_file);
            const std::string request = make_http_request_text(input_file, raw_request);
            const std::string response = send_raw_http_request(frontend_port, request);

            std::string actual_result;
            if (ba::ends_with(test_file.string(), ".options"))
            {
                actual_result = normalize_line_endings(remove_date_header_from_http_response(response));
            }
            else
            {
                actual_result = normalize_line_endings(extract_http_body(response));
            }

            if (!fs::exists(expected_output_file))
            {
                const fs::path failure_file = failure_dir / test_file;
                write_string_to_file(failure_file, actual_result);
                std::cout << out.str() << fg_red << "  FAIL: expected output file missing: "
                    << expected_output_file.string()
                    << fg_default << std::endl;
                failed++;
                continue;
            }

            const std::string expected_result = normalize_line_endings(read_file_to_string(expected_output_file));

            if (actual_result == expected_result)
            {
                std::cout << out.str() << fg_green << "  PASS" << fg_default << std::endl;
                passed++;
            }
            else
            {
                const fs::path failure_file = failure_dir / test_file;
                write_string_to_file(failure_file, actual_result);

                std::cout << out.str() << fg_red << "  FAIL: response body differs from expected output" << fg_default << std::endl;
                std::cout << "  Expected size: " << expected_result.size()
                          << ", actual size: " << actual_result.size() << std::endl;
                std::cout << get_diff(expected_output_file, failure_file) << std::endl;
                failed++;
            }
        }
        catch (...)
        {
            std::cout << out.str() << fg_red << "  FAIL: exception occurred" << fg_default << std::endl;
            std::cout << Fmi::Exception::Trace(BCP, "Exception details: ") << std::endl;
            failed++;
        }
    }

    std::cout << "Test summary: " << passed << " passed, " << failed << " failed" << std::endl;
    return failed == 0;
}

int main()
{
    std::vector<std::pair<pid_t, int>> backends;
    pid_t frontend_pid = 1;
    int frontend_port = -1;

    // Abort test if execution last for more than 5 minutes to avoid hanging indefinitely
    alarm(300); // 300 seconds = 5 minutes

    try
    {
        // Create log directories if they don't exist
        std::filesystem::create_directories("log/b1");
        std::filesystem::create_directories("log/b2");
        std::filesystem::create_directories("log/frontend");

        // Start backend processes
        std::vector<std::string> backend_configs = {
            "cnf/reactor_backend1.conf",
            "cnf/reactor_backend2.conf"
        };
        backends = start_backends(backend_configs);

        // Start frontend process
        std::tie(frontend_pid, frontend_port) = start_frontend("cnf/reactor_frontend.conf");

        std::this_thread::sleep_for(std::chrono::seconds(5)); // Give processes some time to stabilize

        bool tests_ok = run_tests(frontend_port);

        // Stop all processes after tests are done
        const bool frontend_ok = terminate_and_wait_process("Frontend", frontend_pid, frontend_port);
        const bool backends_ok = stop_backends_checked(backends);

        tests_ok = tests_ok && frontend_ok && backends_ok;

        if (!frontend_ok)
        {
            std::cout << "FAIL: Frontend terminated abnormally" << std::endl;
        }
        if (!backends_ok)
        {
            std::cout << "FAIL: One or more backends terminated abnormally" << std::endl;
        }

        return tests_ok ? 0 : 1;
    }
    catch (...)
    {
        std::cout << Fmi::Exception::Trace(BCP, "An error occurred during test execution") << std::endl;
        try
        {
            std::cout << "Failed: cleaning up processes..." << std::endl;
            if (frontend_pid > 1)
            {
                (void)terminate_and_wait_process("Frontend", frontend_pid, frontend_port);
            }
            (void)stop_backends_checked(backends);
        }
        catch(const std::exception& e)
        {
            std::cerr << e.what() << '\n';
        }
        return 1;
    }
}
