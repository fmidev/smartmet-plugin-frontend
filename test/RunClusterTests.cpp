/**
 *  Regression tests for what a cluster does when backends come and go.
 *
 *  These are separate from RunTests because they are slow rather than because
 *  they are optional: most of the time here is spent waiting for sputnik to
 *  notice a backend leaving and coming back, and for a stalled backend to be
 *  timed out. `make test` stays fast; run these with `make cluster-test`.
 *
 *  What they are guarding, all of which has been broken at some point:
 *
 *    - connections to backends are actually reused, and stay reused across a
 *      backend leaving and rejoining the cluster;
 *    - a backend that stops answering is given up on after backend.timeout,
 *      rather than holding a request handler thread forever;
 *    - a request that cannot be served is answered with a framed HTTP error,
 *      not by dropping the client connection - which with keep-alive would take
 *      whatever else the client had queued on it down as well.
 */

#include "TestHarness.h"

#include <boost/algorithm/string.hpp>
#include <macgyver/AnsiEscapeCodes.h>
#include <macgyver/Exception.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace ba = boost::algorithm;

using namespace TestHarness;

namespace
{
// A cheap query every test backend can answer
const char* const test_query =
    "/timeseries?starttime=200808051200&places=Helsinki&param=name,time";

// Must match "backend.timeout" in cnf/plugins/frontend_cluster.conf
const int backend_timeout_seconds = 8;

// How long any single request here may take before it counts as unanswered. The
// point of several of these tests is that the frontend gives up on a backend
// that has stopped talking, so a request that never comes back has to fail the
// test rather than hang the run - which is exactly what it used to do.
//
// Deliberately far beyond anything correct behaviour can produce, and far beyond
// the ceiling the stall test asserts. A regression has to land unmistakably on
// the wrong side of that ceiling even on a slow machine; a margin of a second or
// two would make the test's verdict a matter of scheduling luck.
const int request_timeout_seconds = 40;

// A request that took at least this long waited on the stalled backend rather
// than being served normally. Set from the backend timeout rather than from some
// small number of seconds, so that a merely slow machine cannot be mistaken for
// a stalled backend.
const int stalled_request_threshold_seconds = backend_timeout_seconds - 2;

// The frontend must give up within this. Well clear of the timeout it should be
// honouring, and well clear of the client's own patience above.
const int give_up_ceiling_seconds = backend_timeout_seconds + 10;

std::string get(int port, const std::string& target, const std::string& extra_headers = "")
{
    return http_get(port, target, extra_headers, request_timeout_seconds);
}

// From the admin block of the test configs. Not a secret, and not meant to be.
const char* const admin_user = "admin";
const char* const admin_password = "admin";

const bool is_tty = isatty(fileno(stdout));
const std::string fg_red = is_tty ? ANSI_FG_RED : "";
const std::string fg_green = is_tty ? ANSI_FG_GREEN : "";
const std::string fg_default = is_tty ? ANSI_FG_DEFAULT : "";

/*!
 *  \brief Print the tail of the frontend's log
 *
 *  A failure here is usually about what the frontend did or failed to do, and on
 *  a CI runner the log is an artifact somebody has to go and find - by which time
 *  the question is cold. Put it where the failure is.
 */
void show_frontend_log(int lines = 25)
{
    std::ifstream log("log/cluster-frontend.log");
    if (!log)
    {
        return;
    }

    std::vector<std::string> tail;
    std::string line;
    while (std::getline(log, line))
    {
        tail.push_back(line);
        if (static_cast<int>(tail.size()) > lines)
        {
            tail.erase(tail.begin());
        }
    }

    std::cout << "        --- last " << tail.size() << " lines of log/cluster-frontend.log:"
              << std::endl;
    for (const auto& text : tail)
    {
        std::cout << "        | " << text << std::endl;
    }
}

void report(const std::string& name, bool ok, const std::string& detail = "")
{
    std::cout << "  " << (ok ? fg_green + "PASS" : fg_red + "FAIL") << fg_default << "  " << name;
    if (!detail.empty())
    {
        std::cout << " (" << detail << ")";
    }
    std::cout << std::endl;

    if (!ok)
    {
        show_frontend_log();
    }
}

/*!
 *  \brief Ask an admin request for output that does not depend on the formatter
 *
 *  Admin tables default to the "debug" formatter, which is styled HTML. Parsing
 *  that means depending on which spine version rendered it: a `<td>` that gains
 *  an attribute silently yields nothing, and a counter that could not be read
 *  looks exactly like a counter that is zero. "ascii" is one "name value" line
 *  per row and has nothing to differ about.
 */
std::string admin_request(int port, const std::string& what, const std::string& format = "")
{
    const std::string target = "/admin?what=" + what + (format.empty() ? "" : "&format=" + format);
    return get(port, target, basic_auth_header(admin_user, admin_password));
}

/**
 *  Pull the name/value pairs out of an admin table asked for in ascii, where
 *  each row is "some name<space>value" and the value is the last field.
 */
std::map<std::string, long> parse_admin_table(const std::string& response)
{
    std::map<std::string, long> values;

    const std::size_t body_start = response.find("\r\n\r\n");
    std::istringstream body(body_start == std::string::npos ? response
                                                           : response.substr(body_start + 4));

    std::string line;
    while (std::getline(body, line))
    {
        const std::size_t split = line.find_last_of(' ');
        if (split == std::string::npos)
        {
            continue;
        }

        try
        {
            values[line.substr(0, split)] = std::stol(line.substr(split + 1));
        }
        catch (...)
        {
            // Not a number, so not one of the counters we are after
        }
    }

    return values;
}

/*!
 *  A pool counter, or -1 when the admin reply could not be read at all - which is
 *  worth telling apart from a counter that is genuinely zero, since subtracting
 *  two unreadable counters gives a plausible-looking nought.
 */
long pool_counter(int frontend_port, const std::string& name)
{
    const std::string response = admin_request(frontend_port, "backendconnections", "ascii");
    const auto values = parse_admin_table(response);
    const auto pos = values.find(name);

    if (pos == values.end())
    {
        std::cout << "  (could not read '" << name << "' from the admin reply: "
                  << (response.empty() ? "<no response>" : response.substr(0, 200)) << ")"
                  << std::endl;
        return -1;
    }

    return pos->second;
}

/**
 *  The backends the frontend currently believes in.
 */
std::set<std::string> known_backends(int frontend_port)
{
    const std::string response = get(frontend_port, "/admin?what=backends");

    std::set<std::string> backends;
    for (const char* name : {"test-backend-1", "test-backend-2"})
    {
        if (response.find(name) != std::string::npos)
        {
            backends.insert(name);
        }
    }
    return backends;
}

struct Traffic
{
    int requests = 0;
    int not_ok = 0;      // Answered, but not with 200
    int dropped = 0;     // Not answered at all: the connection was dropped
};

/**
 *  Keep serving traffic until the condition holds or the deadline passes.
 *
 *  The traffic is the point of the exercise as much as the waiting is: what
 *  matters about a backend leaving the cluster is what the clients see while it
 *  happens.
 */
bool serve_until(int frontend_port,
                 int max_wait_seconds,
                 const std::function<bool()>& done,
                 Traffic& traffic)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(max_wait_seconds);

    while (std::chrono::steady_clock::now() < deadline)
    {
        if (done())
        {
            return true;
        }

        const int status = http_status_code(get(frontend_port, test_query));
        ++traffic.requests;
        if (status < 0)
        {
            ++traffic.dropped;
        }
        else if (status != 200)
        {
            ++traffic.not_ok;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    return done();
}

std::string describe(const Traffic& traffic)
{
    return std::to_string(traffic.requests) + " requests, " + std::to_string(traffic.not_ok) +
           " not 200, " + std::to_string(traffic.dropped) + " dropped";
}

// ----------------------------------------------------------------------

bool test_connections_are_reused(int frontend_port)
{
    const long before = pool_counter(frontend_port, "connections reused");
    const long opened_before = pool_counter(frontend_port, "connections opened");

    const int request_count = 20;
    for (int i = 0; i < request_count; i++)
    {
        if (http_status_code(get(frontend_port, test_query)) != 200)
        {
            report("backend connections are reused", false, "a warm-up request did not return 200");
            return false;
        }
    }

    const long reused = pool_counter(frontend_port, "connections reused") - before;
    const long opened = pool_counter(frontend_port, "connections opened") - opened_before;

    // Each request is at least one backend exchange, and two when the frontend
    // cache misses and has to ask for the content as well - so the bound is set
    // by the cheaper case, which holds whether or not the cache is answering.
    // Whatever the mix, two backends need no more than a connection each.
    const bool ok = reused >= request_count / 2 && opened <= 4;
    report("backend connections are reused",
           ok,
           std::to_string(reused) + " reused, " + std::to_string(opened) + " opened");
    return ok;
}

bool test_paused_backend_is_drained_and_restored(int frontend_port, int backend_port)
{
    Traffic traffic;

    admin_request(backend_port, "pause");

    const bool drained = serve_until(
        frontend_port,
        60,
        [&] { return known_backends(frontend_port).count("test-backend-1") == 0; },
        traffic);

    admin_request(backend_port, "continue");

    const bool restored = serve_until(
        frontend_port,
        60,
        [&] { return known_backends(frontend_port).size() == 2; },
        traffic);

    // Pausing a backend is how an operator takes one out of the pool, so it must
    // cost the clients nothing at all.
    const bool ok = drained && restored && traffic.not_ok == 0 && traffic.dropped == 0;
    report("a paused backend is drained and restored",
           ok,
           (drained ? "" : "never drained; ") + std::string(restored ? "" : "never restored; ") +
               describe(traffic));
    return ok;
}

/**
 *  Stops a process for as long as it is alive, and resumes it whatever happens -
 *  including on an exception. A backend left stopped would make the outer cleanup
 *  block in waitpid() forever.
 */
class StoppedProcess
{
   public:
    explicit StoppedProcess(pid_t pid) : itsPid(pid) { kill(itsPid, SIGSTOP); }
    ~StoppedProcess() { kill(itsPid, SIGCONT); }

    StoppedProcess(const StoppedProcess&) = delete;
    StoppedProcess& operator=(const StoppedProcess&) = delete;

   private:
    pid_t itsPid;
};

bool test_stalled_backend_is_timed_out(int frontend_port, pid_t backend_pid)
{
    // A stopped process still has its listening socket, so the frontend connects
    // and writes happily and then hears nothing back - which is what a wedged
    // backend looks like, and what the backend timeout exists for.
    StoppedProcess stopped(backend_pid);

    double longest = 0;
    int longest_status = 0;
    bool found = false;

    for (int i = 0; i < 25 && !found; i++)
    {
        const auto start = std::chrono::steady_clock::now();
        const int status = http_status_code(get(frontend_port, test_query));
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

        if (seconds >= stalled_request_threshold_seconds)
        {
            found = true;
            longest = seconds;
            longest_status = status;
        }
    }

    if (!found)
    {
        report("a stalled backend is timed out",
               false,
               "no request was routed to the stalled backend");
        return false;
    }

    // Tell "the frontend never gave up" apart from "this client ran out of
    // patience first". They look identical - no status, a long wait - and only the
    // first is a defect. A starved runner produces the second, and reading it as
    // the first is what made an earlier CI failure unexplainable.
    if (longest >= request_timeout_seconds - 1.0)
    {
        report("a stalled backend is timed out",
               false,
               "inconclusive: this client gave up after " + std::to_string(longest).substr(0, 5) +
                   " s, its own limit, so what the frontend would have done is unknown. Either it "
                   "never gave up, or the machine is too loaded to tell.");
        return false;
    }

    // It must give up close to the configured timeout - not never, which is what
    // it did while the timer was never re-armed - and the client must get a
    // framed error rather than a dropped connection.
    const bool timely = longest <= give_up_ceiling_seconds;
    const bool answered = longest_status > 0;

    const bool ok = timely && answered;
    report("a stalled backend is timed out",
           ok,
           "gave up after " + std::to_string(longest).substr(0, 5) + " s with status " +
               std::to_string(longest_status));
    return ok;
}

bool test_dead_backend_is_answered_not_dropped(int frontend_port, pid_t backend_pid)
{
    // SIGKILL rather than SIGTERM: a backend that crashes gets no chance to close
    // its connections, so the frontend is left holding pooled ones that are gone.
    kill(backend_pid, SIGKILL);
    waitpid(backend_pid, nullptr, 0);

    Traffic traffic;
    int timed_out = 0;
    for (int i = 0; i < 30; i++)
    {
        const auto start = std::chrono::steady_clock::now();
        const int status = http_status_code(get(frontend_port, test_query));
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

        ++traffic.requests;
        if (status < 0)
        {
            // An empty reply because the connection was dropped is the defect this
            // test is about. An empty reply because this client stopped waiting is
            // a starved machine, and saying so beats blaming the frontend.
            if (seconds >= request_timeout_seconds - 1.0)
            {
                ++timed_out;
            }
            else
            {
                ++traffic.dropped;
            }

            // The point is already made, and each unanswered request costs the
            // full client timeout. Thirty of those would outlast the alarm and
            // turn a clean failure back into a killed run.
            if (traffic.dropped + timed_out >= 3)
            {
                break;
            }
        }
        else if (status != 200)
        {
            ++traffic.not_ok;
        }
    }

    int recovered = 0;
    for (int i = 0; i < 5; i++)
    {
        if (http_status_code(get(frontend_port, test_query)) == 200)
        {
            ++recovered;
        }
    }

    // Losing the request that was in flight is by design - it is not resent, in
    // case the request is what killed the backend. Losing the connection is not:
    // the client has to get an answer it can read.
    const bool ok = traffic.dropped == 0 && timed_out == 0 && recovered == 5;
    report("a dead backend is answered, not dropped",
           ok,
           describe(traffic) + ", " + std::to_string(recovered) + "/5 recovered" +
               (timed_out > 0 ? ", " + std::to_string(timed_out) +
                                    " inconclusive: this client gave up first, so the machine may "
                                    "simply be too loaded to tell"
                              : ""));
    return ok;
}

/*!
 * \brief A small proxied response must not sit waiting for an ACK
 *
 * A response is written in more than one piece - the head, then the content, and
 * a streamed one in as many pieces as it arrives in. With Nagle's algorithm on
 * the server's socket, the second small piece waits for the client to
 * acknowledge the first, and a client's delayed ACK takes 40 ms on Linux.
 *
 * Nothing showed this until connections became persistent, because the close
 * after each response pushed the pending bytes out with the FIN. Measured on a
 * 1 kB proxied response over a kept-alive connection: 43 ms per request and 181
 * requests/s, against 0.9 ms and 8900 requests/s once the server sets
 * TCP_NODELAY. It needs a keep-alive connection to see at all, which is why this
 * lives here and not among the request/response comparisons.
 */
/*!
 *  Median time for the same request repeated over one kept-alive connection, or
 *  a negative number if any of them did not return 200.
 */
double median_request_milliseconds(int port, int count, bool quick_ack = false)
{
    HttpConnection connection;
    if (!connection.open(port, request_timeout_seconds, quick_ack))
    {
        return -1;
    }

    std::vector<double> milliseconds;
    for (int i = 0; i < count; i++)
    {
        std::size_t bytes = 0;
        const auto start = std::chrono::steady_clock::now();
        const int status = connection.get(test_query, bytes);
        milliseconds.push_back(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count());

        if (status != 200)
        {
            return -1;
        }
    }

    std::sort(milliseconds.begin(), milliseconds.end());
    return milliseconds[milliseconds.size() / 2];
}

bool test_small_response_is_not_delayed(int frontend_port)
{
    // The same exchange measured twice over the frontend: once letting this client
    // delay its ACKs as any client does, once with TCP_QUICKACK forcing them out
    // immediately. Only the ACK wait differs between the two runs, so the gap
    // between them *is* the wait - and being a kernel timer rather than work, it
    // does not grow when the machine is loaded.
    //
    // That matters. Comparing the frontend against a backend, or against a fixed
    // number of milliseconds, both looked fine here and both failed under
    // contention: a healthy proxied request measured 0.7 ms idle and 30 ms with
    // the CPUs oversubscribed, which no fixed line survives.
    const double delayed = median_request_milliseconds(frontend_port, 20, false);
    const double immediate = median_request_milliseconds(frontend_port, 20, true);

    if (delayed < 0 || immediate < 0)
    {
        report("a small response is not held for an ACK", false, "a request did not return 200");
        return false;
    }

    const double waited = delayed - immediate;

    // Half the 40 ms a delayed ACK costs on Linux. Nothing legitimate lives here.
    const bool ok = waited < 20.0;
    report("a small response is not held for an ACK",
           ok,
           std::to_string(delayed).substr(0, 5) + " ms, and " +
               std::to_string(immediate).substr(0, 5) + " ms with the client's delayed ACK off" +
               (ok ? "" : " - does this server set TCP_NODELAY on accepted sockets?"));
    return ok;
}

bool wait_for_all_backends(int frontend_port, int max_wait_seconds = 60)
{
    Traffic ignored;
    const bool ok = serve_until(
        frontend_port, max_wait_seconds, [&] { return known_backends(frontend_port).size() == 2; },
        ignored);
    if (!ok)
    {
        std::cout << "  (both backends did not return to the cluster in time)" << std::endl;
    }
    return ok;
}

}  // namespace

int main()
{
    std::vector<std::pair<pid_t, int>> backends;
    pid_t frontend_pid = 1;
    int frontend_port = -1;

    // These tests spend most of their time waiting, so give them room, but never
    // let a hung one wait forever.
    alarm(600);

    try
    {
        std::filesystem::create_directories("log/b1");
        std::filesystem::create_directories("log/b2");
        std::filesystem::create_directories("log/cluster");

        backends = start_backends({"cnf/reactor_backend1.conf", "cnf/reactor_backend2.conf"},
                                  "log/cluster-backend");

        for (const auto& [pid, port] : backends)
        {
            (void)pid;
            wait_for_ready(port, "Backend");
        }

        std::tie(frontend_pid, frontend_port) =
            start_frontend("cnf/reactor_frontend_cluster.conf", "log/cluster-frontend.log");
        wait_for_ready(frontend_port, "Frontend");

        // The frontend is ready before it has heard from any backend
        std::this_thread::sleep_for(std::chrono::seconds(4));

        std::cout << "Running cluster tests against frontend on port " << frontend_port
                  << std::endl;

        bool ok = test_connections_are_reused(frontend_port);

        ok = test_small_response_is_not_delayed(frontend_port) && ok;

        ok = test_paused_backend_is_drained_and_restored(frontend_port, backends[0].second) && ok;

        ok = wait_for_all_backends(frontend_port) && ok;
        ok = test_stalled_backend_is_timed_out(frontend_port, backends[0].first) && ok;

        // Everything below kills a backend for good, so it goes last. Both have to
        // be back first: retiring the only remaining backend makes sputnik take
        // the frontend down with it.
        ok = wait_for_all_backends(frontend_port) && ok;
        ok = test_dead_backend_is_answered_not_dropped(frontend_port, backends[1].first) && ok;

        // backends[1] was killed by the test itself and already reaped
        backends.pop_back();

        const bool frontend_ok =
            terminate_and_wait_process("Frontend", frontend_pid, frontend_port);
        const bool backends_ok = stop_backends_checked(backends);

        if (!frontend_ok)
        {
            std::cout << "FAIL: Frontend terminated abnormally" << std::endl;
        }
        if (!backends_ok)
        {
            std::cout << "FAIL: One or more backends terminated abnormally" << std::endl;
        }

        ok = ok && frontend_ok && backends_ok;
        std::cout << (ok ? "Cluster tests passed" : "Cluster tests FAILED") << std::endl;
        return ok ? 0 : 1;
    }
    catch (...)
    {
        std::cout << Fmi::Exception::Trace(BCP, "An error occurred during test execution")
                  << std::endl;
        try
        {
            std::cout << "Failed: cleaning up processes..." << std::endl;
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
