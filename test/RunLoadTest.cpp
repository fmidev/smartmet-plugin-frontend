/**
 *  Load driver for profiling the frontend.
 *
 *  Not a test: nothing here passes or fails. It stands up the same cluster the
 *  other two programs use, keeps the frontend as busy as the client machine can
 *  manage, and - if perf is available - records the frontend while it happens.
 *  The output is a perf.data plus the numbers needed to read it honestly.
 *
 *  Two of those numbers matter more than the throughput:
 *
 *    - CPU seconds burned by the frontend against the backends. A profile of the
 *      frontend is only interesting while the frontend is what is busy, and with
 *      an 80 kB response from a test backend that is not a given.
 *    - CPU seconds burned by this program. Client and servers share the machine
 *      here, so a client that saturates it is measuring itself.
 *
 *  Usage: ./RunLoadTest [options]
 *
 *    --seconds=N                  how long to hold the load (default 20)
 *    --threads=N                  concurrent connections (default 8)
 *    --requests-per-connection=N  reconnect every N requests (0 = never, default)
 *    --target=PATH                what to ask for
 *    --frontend-config=PATH       which frontend to profile
 *    --perf=yes|no|auto           record the frontend (default auto)
 *    --perf-freq=N                sampling frequency (default 999)
 *    --perf-output=PATH           where to write it (default log/perf.data)
 */

#include "TestHarness.h"

#include <macgyver/Exception.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace TestHarness;

namespace
{

struct Options
{
    int seconds = 20;
    int threads = 8;
    int requests_per_connection = 0;
    std::string target = "/info?what=obsparameters";
    std::string frontend_config = "cnf/reactor_frontend.conf";
    std::string perf = "auto";
    int perf_freq = 999;
    std::string perf_output = "log/perf.data";
};

bool starts_with(const std::string& text, const std::string& prefix)
{
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

Options parse_options(int argc, char** argv)
{
    Options options;
    options.threads =
        std::max(1, std::min(8, static_cast<int>(std::thread::hardware_concurrency())));

    for (int i = 1; i < argc; i++)
    {
        const std::string arg = argv[i];
        if (starts_with(arg, "--seconds="))
            options.seconds = std::stoi(arg.substr(10));
        else if (starts_with(arg, "--threads="))
            options.threads = std::stoi(arg.substr(10));
        else if (starts_with(arg, "--requests-per-connection="))
            options.requests_per_connection = std::stoi(arg.substr(26));
        else if (starts_with(arg, "--target="))
            options.target = arg.substr(9);
        else if (starts_with(arg, "--frontend-config="))
            options.frontend_config = arg.substr(18);
        else if (starts_with(arg, "--perf="))
            options.perf = arg.substr(7);
        else if (starts_with(arg, "--perf-freq="))
            options.perf_freq = std::stoi(arg.substr(12));
        else if (starts_with(arg, "--perf-output="))
            options.perf_output = arg.substr(14);
        else
            throw std::runtime_error("Unknown option: " + arg);
    }

    return options;
}

/*!
 * \brief CPU seconds a process has used so far, from /proc
 */
double process_cpu_seconds(pid_t pid)
{
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    if (!in)
    {
        return 0;
    }

    // The comm field can contain spaces and parentheses, so start after the last ')'
    std::string line;
    std::getline(in, line);
    const std::size_t tail = line.rfind(')');
    if (tail == std::string::npos)
    {
        return 0;
    }

    std::istringstream fields(line.substr(tail + 1));
    std::string field;
    unsigned long long utime = 0;
    unsigned long long stime = 0;
    // After the ')' the next field is state, so utime is the 12th and stime the 13th
    for (int i = 1; i <= 13 && fields >> field; i++)
    {
        if (i == 12)
            utime = std::stoull(field);
        else if (i == 13)
            stime = std::stoull(field);
    }

    return static_cast<double>(utime + stime) / static_cast<double>(sysconf(_SC_CLK_TCK));
}

double own_cpu_seconds()
{
    return process_cpu_seconds(getpid());
}

struct ThreadResult
{
    long long requests = 0;
    long long errors = 0;
    long long connections = 0;
    unsigned long long bytes = 0;
    std::vector<float> latencies_ms;
};

void load_thread(int port, const Options& options, std::atomic<bool>& stop, ThreadResult& result)
{
    HttpConnection connection;
    long long on_this_connection = 0;

    // A generous bound: this is about throughput, and a request that takes tens
    // of seconds means something is wrong rather than slow.
    const int recv_timeout_seconds = 30;

    result.latencies_ms.reserve(1 << 16);

    while (!stop.load(std::memory_order_relaxed))
    {
        if (!connection.is_open())
        {
            // Reconnecting is normal, not a failure: the server ends persistence
            // after keepalive.maxrequests responses and the client honours that.
            if (!connection.open(port, recv_timeout_seconds))
            {
                ++result.errors;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            ++result.connections;
            on_this_connection = 0;
        }

        const auto start = std::chrono::steady_clock::now();
        std::size_t bytes = 0;
        const int status = connection.get(options.target, bytes);
        const auto elapsed = std::chrono::steady_clock::now() - start;

        ++result.requests;
        result.bytes += bytes;
        result.latencies_ms.push_back(
            static_cast<float>(std::chrono::duration<double, std::milli>(elapsed).count()));

        if (status != 200)
        {
            ++result.errors;
        }

        ++on_this_connection;
        if (options.requests_per_connection > 0 &&
            on_this_connection >= options.requests_per_connection)
        {
            connection.close_connection();
        }
    }
}

double percentile(const std::vector<float>& sorted, double fraction)
{
    if (sorted.empty())
    {
        return 0;
    }
    const std::size_t index = static_cast<std::size_t>(fraction * (sorted.size() - 1));
    return sorted[index];
}

bool perf_is_usable()
{
    // Report rather than guess: a perf that cannot attach exits non-zero, and the
    // load figures are worth having either way.
    return access("/usr/bin/perf", X_OK) == 0 || access("/bin/perf", X_OK) == 0;
}

}  // namespace

int main(int argc, char** argv)
{
    std::vector<std::pair<pid_t, int>> backends;
    pid_t frontend_pid = 1;
    int frontend_port = -1;

    try
    {
        const Options options = parse_options(argc, argv);

        std::filesystem::create_directories("log/b1");
        std::filesystem::create_directories("log/b2");
        std::filesystem::create_directories("log/frontend");

        backends = start_backends({"cnf/reactor_backend1.conf", "cnf/reactor_backend2.conf"},
                                  "log/load-backend");
        for (const auto& [pid, port] : backends)
        {
            (void)pid;
            wait_for_ready(port, "Backend");
        }

        std::tie(frontend_pid, frontend_port) =
            start_frontend(options.frontend_config, "log/load-frontend.log");
        wait_for_ready(frontend_port, "Frontend");
        std::this_thread::sleep_for(std::chrono::seconds(4));

        // Warm up: fill the backend connection pool, page the code in, and make
        // sure the target is something the cluster actually answers before
        // spending a minute measuring it.
        {
            HttpConnection warmup;
            if (!warmup.open(frontend_port, 30))
            {
                throw std::runtime_error("Could not connect to the frontend");
            }

            std::size_t bytes = 0;
            int status = -1;
            for (int i = 0; i < 20; i++)
            {
                status = warmup.get(options.target, bytes);
                if (status != 200)
                {
                    break;
                }
            }

            if (status != 200)
            {
                throw std::runtime_error("Warm-up request for " + options.target +
                                         " returned status " + std::to_string(status));
            }

            std::cout << "Target " << options.target << " answers with " << bytes
                      << " bytes per response" << std::endl;
        }

        const bool want_perf = options.perf == "yes" || (options.perf == "auto" && perf_is_usable());
        pid_t perf_pid = -1;

        const double frontend_cpu_before = process_cpu_seconds(frontend_pid);
        std::vector<double> backend_cpu_before;
        for (const auto& [pid, port] : backends)
        {
            (void)port;
            backend_cpu_before.push_back(process_cpu_seconds(pid));
        }
        const double own_cpu_before = own_cpu_seconds();

        if (want_perf)
        {
            perf_pid = start_background_process("perf",
                                                {"record",
                                                 "-F",
                                                 std::to_string(options.perf_freq),
                                                 "-g",
                                                 "-p",
                                                 std::to_string(frontend_pid),
                                                 "-o",
                                                 options.perf_output,
                                                 "--",
                                                 "sleep",
                                                 std::to_string(options.seconds)},
                                                "log/perf-record.log");
            std::cout << "Recording the frontend with perf for " << options.seconds << " s"
                      << std::endl;
            // Let perf get its counters open before the load starts
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        else if (options.perf != "no")
        {
            std::cout << "perf not found, running the load without a profile" << std::endl;
        }

        std::cout << "Loading " << options.threads << " connections for " << options.seconds
                  << " s" << std::endl;

        std::atomic<bool> stop{false};
        std::vector<ThreadResult> results(options.threads);
        std::vector<std::thread> threads;

        const auto load_start = std::chrono::steady_clock::now();
        for (int i = 0; i < options.threads; i++)
        {
            threads.emplace_back(
                load_thread, frontend_port, std::cref(options), std::ref(stop), std::ref(results[i]));
        }

        std::this_thread::sleep_for(std::chrono::seconds(options.seconds));
        stop.store(true, std::memory_order_relaxed);
        for (auto& thread : threads)
        {
            thread.join();
        }
        const double load_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - load_start).count();

        const double frontend_cpu = process_cpu_seconds(frontend_pid) - frontend_cpu_before;
        double backend_cpu = 0;
        for (std::size_t i = 0; i < backends.size(); i++)
        {
            backend_cpu += process_cpu_seconds(backends[i].first) - backend_cpu_before[i];
        }
        const double own_cpu = own_cpu_seconds() - own_cpu_before;

        if (perf_pid > 1)
        {
            int status = 0;
            waitpid(perf_pid, &status, 0);
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            {
                std::cout << "perf record failed, see log/perf-record.log" << std::endl;
                perf_pid = -1;
            }
        }

        long long requests = 0;
        long long errors = 0;
        long long connections = 0;
        unsigned long long bytes = 0;
        std::vector<float> latencies;
        for (const auto& result : results)
        {
            requests += result.requests;
            errors += result.errors;
            connections += result.connections;
            bytes += result.bytes;
            latencies.insert(latencies.end(), result.latencies_ms.begin(), result.latencies_ms.end());
        }
        std::sort(latencies.begin(), latencies.end());

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "\n=== Load ===\n";
        std::cout << "  target            " << options.target << '\n';
        std::cout << "  duration          " << load_seconds << " s\n";
        std::cout << "  concurrency       " << options.threads << '\n';
        std::cout << "  requests          " << requests << " (" << errors << " not 200)\n";
        std::cout << "  connections used  " << connections
                  << ", i.e. one per " << (connections > 0 ? requests / connections : 0)
                  << " requests (the server ends persistence every"
                     " keepalive.maxrequests)\n";
        std::cout << "  throughput        " << requests / load_seconds << " requests/s, "
                  << static_cast<double>(bytes) / load_seconds / (1024 * 1024) << " MiB/s\n";
        std::cout << "  latency           p50 " << percentile(latencies, 0.50) << " ms, p90 "
                  << percentile(latencies, 0.90) << " ms, p99 " << percentile(latencies, 0.99)
                  << " ms, max " << (latencies.empty() ? 0.0 : latencies.back()) << " ms\n";

        std::cout << "\n=== CPU seconds during the load ===\n";
        std::cout << "  frontend          " << frontend_cpu << "  ("
                  << frontend_cpu / load_seconds << " cores)\n";
        std::cout << "  backends          " << backend_cpu << "  ("
                  << backend_cpu / load_seconds << " cores)\n";
        std::cout << "  this load driver  " << own_cpu << "  (" << own_cpu / load_seconds
                  << " cores)\n";

        // Saying which of the three was the bottleneck is the difference between a
        // profile that means something and one that flatters whichever process
        // happened to be sampled.
        if (backend_cpu > frontend_cpu * 1.5)
        {
            std::cout << "  NOTE: the backends used more CPU than the frontend, so this load is\n"
                         "        backend-bound and the frontend profile is of a process that was\n"
                         "        mostly waiting. Try a cheaper target.\n";
        }
        if (own_cpu > frontend_cpu)
        {
            std::cout << "  NOTE: this load driver used more CPU than the frontend. It shares the\n"
                         "        machine with the servers, so some of what you measure is the\n"
                         "        client. Fewer threads, or drive the load from another host.\n";
        }

        if (perf_pid > 1)
        {
            std::cout << "\n=== Profile ===\n";
            std::cout << "  " << options.perf_output << "\n\n";
            std::cout << "  perf report -i " << options.perf_output
                      << " --stdio --sort=dso            # where the time goes, by object\n";
            std::cout << "  perf report -i " << options.perf_output
                      << " --stdio --no-children -g none # self time, hottest first\n";
            std::cout << "  perf report -i " << options.perf_output
                      << " --stdio -g graph,1,caller     # call graph\n";
        }

        const bool frontend_ok =
            terminate_and_wait_process("Frontend", frontend_pid, frontend_port);
        const bool backends_ok = stop_backends_checked(backends);

        if (!frontend_ok || !backends_ok)
        {
            std::cout << "\nWARNING: a server did not shut down cleanly" << std::endl;
            return 1;
        }

        return 0;
    }
    catch (...)
    {
        std::cout << Fmi::Exception::Trace(BCP, "The load run failed") << std::endl;
        try
        {
            if (frontend_pid > 1)
            {
                (void)terminate_and_wait_process("Frontend", frontend_pid, frontend_port);
            }
            (void)stop_backends_checked(backends);
        }
        catch (const std::exception& e)
        {
            std::cerr << e.what() << '\n';
        }
        return 1;
    }
}
