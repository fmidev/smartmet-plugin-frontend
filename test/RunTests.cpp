#include "TestHarness.h"

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
#include <unistd.h>
#include <getopt.h>

namespace ba = boost::algorithm;

using namespace TestHarness;


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
    for (const auto& entry : fs::recursive_directory_iterator(input_dir))
    {
        if (entry.is_regular_file() && (ba::ends_with(entry.path().string(), ".get") ||
                                    ba::ends_with(entry.path().string(), ".options") ||
                                    ba::ends_with(entry.path().string(), ".post")))
        {
            test_files.push_back(fs::relative(entry.path(), input_dir));
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

        const fs::path& input_file = input_dir / test_file;
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

// ----------------------------------------------------------------------
/*!
 * \brief Check that backend connections are actually being reused
 *
 * Connection reuse is invisible in the responses - the same bytes come back,
 * only without a TCP handshake in front of them - so the only way to tell a
 * working pool from one that opens a connection every time is to ask the
 * frontend.
 *
 * The requests are sent as HTTP/1.1: the frontend forwards the client's protocol
 * version to the backend, and an HTTP/1.0 request (which is what the file-driven
 * tests above send) has no persistence to offer.
 */
// ----------------------------------------------------------------------

bool check_backend_connection_reuse(int frontend_port)
{
    // "Connection: close" applies to this hop only - it keeps the read loop above
    // from waiting out the frontend's idle timeout - and says nothing about the
    // frontend's own connections to the backends.
    const std::string request =
        "GET /timeseries?starttime=200808051200&places=Helsinki&param=name,time HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n";

    const int request_count = 5;
    for (int i = 0; i < request_count; i++)
    {
        const std::string response = send_raw_http_request(frontend_port, request);
        if (response.find(" 200 ") == std::string::npos)
        {
            std::cout << "Backend connection reuse check: request " << (i + 1)
                      << " did not return 200" << std::endl;
            return false;
        }
    }

    const std::string stats = send_raw_http_request(
        frontend_port, "GET /admin?what=backendconnections HTTP/1.0\r\n\r\n");

    const std::string marker = "connections reused";
    const std::size_t at = stats.find(marker);
    if (at == std::string::npos)
    {
        std::cout << "Backend connection reuse check: no reuse counter in the admin reply"
                  << std::endl;
        return false;
    }

    long reused = -1;
    const std::size_t value_start = stats.find_first_of("0123456789", at + marker.size());
    if (value_start != std::string::npos)
        reused = std::stol(stats.substr(value_start));

    if (reused <= 0)
    {
        std::cout << "Backend connection reuse check: " << request_count
                  << " requests reused " << reused << " connections" << std::endl;
        return false;
    }

    std::cout << "Backend connections reused: " << reused << std::endl;
    return true;
}

// ----------------------------------------------------------------------
/*!
 * \brief The configuration files of the test cluster
 *
 * The debug targets start a subset of the very same cluster the tests use, so
 * that what is being debugged is what is being tested.
 */
// ----------------------------------------------------------------------

const std::vector<std::string> backend_configs = {
    "cnf/reactor_backend1.conf",
    "cnf/reactor_backend2.conf"
};

const std::string frontend_config = "cnf/reactor_frontend.conf";

void create_log_directories()
{
    std::filesystem::create_directories("log/b1");
    std::filesystem::create_directories("log/b2");
    std::filesystem::create_directories("log/frontend");
}

// ----------------------------------------------------------------------
/*!
 * \brief Debug one backend under gdb, and nothing else
 *
 * A backend that fails to start takes the whole test run with it, and as a
 * child process with its output in a log file there is nothing to type at. This
 * starts the one backend asked for, under gdb, on the terminal - no second
 * backend and no frontend, because neither is any use while the one being
 * debugged is sitting at a breakpoint.
 */
// ----------------------------------------------------------------------

int run_debug_backend(int index)
{
    create_log_directories();

    const std::string& config = backend_configs.at(index - 1);
    std::cout << "Debugging backend " << index << " (" << config << ")" << std::endl;

    const int status = run_under_gdb(smartmetd_path(),
                                     {"--configfile", config, "--port=0"},
                                     "Backend " + std::to_string(index));

    return status == 0 ? 0 : 1;
}

// ----------------------------------------------------------------------
/*!
 * \brief Debug the frontend under gdb against real backends
 *
 * The frontend is not worth much on its own: it forwards, and without backends
 * to forward to every request comes back as an error. So the backends are
 * started the way the tests start them - in the background, with their output
 * in log/ - and only the frontend is put under gdb.
 *
 * Sputnik discovery takes a few seconds after the frontend is up, so a request
 * made the moment the gdb prompt appears may still find no backend.
 */
// ----------------------------------------------------------------------

int run_debug_frontend()
{
    create_log_directories();

    std::vector<std::pair<pid_t, int>> backends;
    try
    {
        backends = start_backends(backend_configs);
    }
    catch (...)
    {
        std::cout << Fmi::Exception::Trace(BCP, "Failed to start the backends") << std::endl;
        (void)stop_backends_checked(backends);
        return 1;
    }

    // A backend that never becomes ready is a warning here rather than an error:
    // debugging the frontend against a broken cluster is a legitimate thing to
    // do, and refusing to start gdb would take away the only tool for it.
    for (const auto& [pid, port] : backends)
    {
        (void)pid;
        try
        {
            // A shorter wait than the tests use: here it is only there to keep the
            // gdb prompt from appearing before the backends can answer, and a
            // backend that is not up in 20 seconds is not coming up at all
            wait_for_ready(port, "Backend", 20);
        }
        catch (const std::exception& e)
        {
            std::cout << "WARNING: backend on port " << port << " is not ready: " << e.what()
                      << std::endl;
        }
    }

    std::cout << "Debugging frontend (" << frontend_config << ")" << std::endl;

    int status = -1;
    try
    {
        status = run_under_gdb(smartmetd_path(),
                               {"--configfile", frontend_config, "--port=0"},
                               "Frontend");
    }
    catch (...)
    {
        std::cout << Fmi::Exception::Trace(BCP, "Failed to run the frontend under gdb") << std::endl;
    }

    const bool backends_ok = stop_backends_checked(backends);

    return (status == 0 && backends_ok) ? 0 : 1;
}

int run_test_cluster()
{
    std::vector<std::pair<pid_t, int>> backends;
    pid_t frontend_pid = 1;
    int frontend_port = -1;

    // Abort test if execution last for more than 5 minutes to avoid hanging indefinitely
    alarm(300); // 300 seconds = 5 minutes

    try
    {
        // Create log directories if they don't exist
        create_log_directories();

        // Start backend processes
        backends = start_backends(backend_configs);

        for (const auto& [pid, port] : backends)
        {
            (void)pid;
            wait_for_ready(port, "Backend");
        }

        // Start frontend process
        std::tie(frontend_pid, frontend_port) = start_frontend(frontend_config);

        wait_for_ready(frontend_port, "Frontend");

        // Unfortunetely we must additionally wait for communication between frontend and backends
        // before we can start testing. Otherwise frontend will not be able to handle requests properly
        // and tests will fail.
        std::this_thread::sleep_for(std::chrono::seconds(4));

        bool tests_ok = run_tests(frontend_port);

        if (tests_ok)
            tests_ok = check_backend_connection_reuse(frontend_port);

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

void usage(const char* program)
{
    std::cout << "Usage: " << program << " [options]\n"
              << "\n"
              << "  With no options the whole cluster is started and the tests are run.\n"
              << "\n"
              << "  --debug-backend N  start only backend N (1.."
              << backend_configs.size() << ") under gdb and run no tests\n"
              << "  --debug-frontend   start the backends, then the frontend under gdb,\n"
              << "                     and run no tests\n"
              << "  --help             this text\n"
              << "\n"
              << "  $SMARTMETD chooses which server is run, $GDB which debugger."
              << std::endl;
}

int main(int argc, char* argv[])
{
    int debug_backend = 0;
    bool debug_frontend = false;

    const struct option long_options[] = {
        {"debug-backend", required_argument, nullptr, 'b'},
        {"debug-frontend", no_argument, nullptr, 'f'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int opt = 0;
    while ((opt = getopt_long(argc, argv, "", long_options, nullptr)) != -1)
    {
        switch (opt)
        {
            case 'b':
                try
                {
                    debug_backend = std::stoi(optarg);
                }
                catch (const std::exception&)
                {
                    debug_backend = 0;
                }
                if (debug_backend < 1 ||
                    static_cast<std::size_t>(debug_backend) > backend_configs.size())
                {
                    std::cout << "Invalid backend number: " << optarg << std::endl;
                    usage(argv[0]);
                    return 1;
                }
                break;

            case 'f':
                debug_frontend = true;
                break;

            case 'h':
                usage(argv[0]);
                return 0;

            default:
                usage(argv[0]);
                return 1;
        }
    }

    if (optind < argc)
    {
        std::cout << "Unexpected argument: " << argv[optind] << std::endl;
        usage(argv[0]);
        return 1;
    }

    if (debug_backend != 0 && debug_frontend)
    {
        std::cout << "--debug-backend and --debug-frontend are mutually exclusive" << std::endl;
        return 1;
    }

    // No alarm() in the debug modes: sitting at a breakpoint is what they are for

    if (debug_backend != 0)
    {
        return run_debug_backend(debug_backend);
    }

    if (debug_frontend)
    {
        return run_debug_frontend();
    }

    return run_test_cluster();
}
