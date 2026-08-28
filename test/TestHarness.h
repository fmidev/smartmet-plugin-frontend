/**
 *  Shared plumbing for the frontend's integration test programs.
 *
 *  RunTests drives the file-based request/response comparisons; RunClusterTests
 *  drives what happens to a cluster when backends come and go. Both need the same
 *  things underneath: start a smartmetd, find out which port it picked, talk HTTP
 *  to it over a raw socket, and shut it down while noticing whether it died badly.
 */

#pragma once

#include <sys/types.h>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace TestHarness
{

/**
 *  Which smartmetd the test programs run: $SMARTMETD if set, otherwise the
 *  installed /usr/sbin/smartmetd.
 */
std::string smartmetd_path();

/**
 *  Starts a background process with the given command and arguments.
 *  The process's output is redirected to the specified log file.
 */
pid_t start_background_process(const std::string& command,
                               const std::vector<std::string>& args,
                               const std::string& log_file);

/**
 *  Get the TCP/IP port the specified process is listening on. The servers are
 *  started with --port=0, so the port is only known once they are up.
 */
int get_process_port(pid_t pid);

/**
 *  The same, but without throwing when the process is not listening (yet):
 *  returns -1 instead. Polling for a port that is expected to appear later is a
 *  normal thing to do, not an error.
 */
int try_get_process_port(pid_t pid);

/**
 *  Which gdb the test programs run: $GDB if set, otherwise "gdb" from $PATH.
 */
std::string gdb_path();

/**
 *  Run a process under an interactive gdb and wait for gdb to exit.
 *
 *  Unlike start_background_process(), this inherits the terminal instead of
 *  redirecting to a log file - the whole point is to type at it. The servers are
 *  started with --port=0, so the port is not known before the inferior runs;
 *  a poller reports it under process_name once it appears.
 *
 *  Returns gdb's exit code, or -1 if it could not be waited for.
 */
int run_under_gdb(const std::string& command,
                  const std::vector<std::string>& args,
                  const std::string& process_name);

/**
 *  Start smartmet backend processes and return their PIDs and ports.
 */
std::vector<std::pair<pid_t, int>> start_backends(const std::vector<std::string>& config_files,
                                                  const std::string& log_prefix = "log/backend");

/**
 *  Start a smartmet frontend process and return its PID and port.
 */
std::pair<pid_t, int> start_frontend(const std::string& config_file,
                                     const std::string& log_file = "log/frontend.log");

void stop_backends(const std::vector<std::pair<pid_t, int>>& backends);

bool report_process_exit_status(const std::string& process_name, pid_t pid, int port, int status);

/**
 *  SIGTERM a process and wait for it, reporting whether it went down cleanly.
 *  A process that has already exited is reaped and reported as it stands.
 */
bool terminate_and_wait_process(const std::string& process_name, pid_t pid, int port);

bool stop_backends_checked(const std::vector<std::pair<pid_t, int>>& backends);

/**
 *  Send a request over a fresh connection and read until the server closes.
 *  The caller is responsible for asking for a close: an HTTP/1.1 request without
 *  "Connection: close" leaves this waiting out the server's idle timeout.
 *
 *  recv_timeout_seconds bounds how long to wait for the server to say anything.
 *  Zero waits forever, which is right for tests that compare responses and wrong
 *  for tests about servers that stop answering: there, a hang has to come back as
 *  an empty response and a failed test rather than as a hung test run.
 */
std::string send_raw_http_request(int port,
                                  const std::string& request_text,
                                  int recv_timeout_seconds = 0);

std::string extract_http_body(const std::string& response);

/**
 *  The status code of a response, or -1 if this is not one. A request that was
 *  answered by the connection being dropped comes back as an empty string, which
 *  is exactly the case worth telling apart from an honest error reply.
 */
int http_status_code(const std::string& response);

/**
 *  GET a target over HTTP/1.1, asking for the connection to be closed afterwards.
 *  extra_headers, if given, must already be CRLF-terminated.
 */
std::string http_get(int port,
                     const std::string& target,
                     const std::string& extra_headers = "",
                     int recv_timeout_seconds = 0);

/**
 *  Authorization header line for an admin request, CRLF-terminated.
 */
std::string basic_auth_header(const std::string& user, const std::string& password);

void wait_for_ready(int port, const std::string& process_name, int max_wait_seconds = 60);

// ----------------------------------------------------------------------
/*!
 * \brief A connection that stays open across requests
 *
 * send_raw_http_request() opens a connection, sends one request and reads until
 * the server closes, which is the right shape for tests that compare responses
 * and the wrong one for load: the client spends its time in the TCP handshake
 * instead of keeping the server busy, and the connection-per-request path is not
 * what real clients do any more.
 *
 * Reading one response of several off the same socket means framing it properly,
 * so this understands Content-Length and chunked bodies. A response it cannot
 * frame closes the connection rather than guessing, because the alternative is
 * reading the next response as the tail of this one.
 */
// ----------------------------------------------------------------------

class HttpConnection
{
 public:
    HttpConnection() = default;
    ~HttpConnection();

    HttpConnection(const HttpConnection& other) = delete;
    HttpConnection& operator=(const HttpConnection& other) = delete;

    /*!
     * \brief Open a connection
     *
     * quick_ack disables the client's delayed ACK (Linux TCP_QUICKACK) for every
     * read on this connection. Measuring the same exchange with it on and off
     * isolates the ACK wait: it is a fixed kernel timer rather than work, so the
     * difference between the two does not move when the machine is loaded, while
     * either measurement on its own does.
     */
    bool open(int port, int recv_timeout_seconds = 0, bool quick_ack = false);
    void close_connection();
    bool is_open() const { return itsFd >= 0; }

    /*!
     * \brief Send a GET on this connection and read exactly one response
     *
     * Returns the status code, or -1 when the exchange failed - in which case the
     * connection has been closed and the caller has to open a new one.
     * response_bytes is the size of the whole message, head included.
     */
    int get(const std::string& target, std::size_t& response_bytes);

 private:
    bool send_all(const std::string& data);
    bool fill_buffer();

    int itsFd = -1;
    int itsRecvTimeoutSeconds = 0;
    bool itsQuickAck = false;

    // Bytes read from the socket but not yet accounted to a response
    std::string itsBuffer;
};

}  // namespace TestHarness
