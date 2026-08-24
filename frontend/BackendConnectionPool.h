#pragma once

#include <boost/asio.hpp>
#include <boost/thread/mutex.hpp>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace SmartMet
{
// ----------------------------------------------------------------------
/*!
 * \brief Idle backend connections, kept open for the next request
 *
 * The frontend used to open a TCP connection per backend request and ask the
 * backend to close it again ("Connection: close"). Every request therefore paid
 * for a three-way handshake, and a cache miss paid for two: one for the ETag
 * probe and another for the content itself.
 *
 * Now that the frontend re-emits backend responses instead of forwarding their
 * bytes verbatim, the two hops' connection lifetimes are independent, and a
 * backend connection whose response was completely read at a known boundary can
 * be kept for the next request instead of being closed.
 *
 * A pooled connection is only ever handed back when the response framing said
 * the message ended - a Content-Length that was reached, or a chunked body
 * whose terminating chunk arrived - and the backend did not ask for the
 * connection to be closed. Anything else would leave unread bytes on the socket
 * that the next response would be parsed from.
 *
 * \b Staleness is unavoidable, not merely unlikely: the backend closes idle
 * connections on its own keep-alive timeout, and it may do so at the very
 * moment a request is being written. So this class checks liveness before
 * handing a connection out (which catches the common case cheaply) and the
 * caller retries once on a fresh connection when a reused one fails before any
 * response byte arrives (which catches the rest). Neither is sufficient alone.
 */
// ----------------------------------------------------------------------

class BackendConnectionPool
{
 public:
  using Socket = boost::asio::ip::tcp::socket;

  struct BackendStatistics
  {
    std::string ip;
    unsigned short port = 0;
    std::size_t idle = 0;
  };

  struct Statistics
  {
    std::uint64_t hits = 0;       // Exchanges sent on an already-open connection
    std::uint64_t misses = 0;     // TCP connections actually opened
    std::uint64_t returned = 0;   // Connections handed back for reuse
    std::uint64_t discarded = 0;  // Pooled connections found dead or out of sync
    std::uint64_t expired = 0;    // Pooled connections dropped for sitting idle too long
    std::uint64_t replayed = 0;   // Reused connections that died and had to be replayed
    std::uint64_t rejected = 0;   // Connections not returned: pool full
    std::vector<BackendStatistics> backends;
  };

  // theIdleTimeoutSeconds must stay below the backend's own keep-alive timeout
  // (smartmet-server's "keepalive.timeout", 30 s by default), otherwise every
  // pooled connection is dead by the time it is reused.
  BackendConnectionPool(bool theEnabled,
                        std::size_t theMaxIdlePerBackend,
                        int theIdleTimeoutSeconds);

  BackendConnectionPool(const BackendConnectionPool& other) = delete;
  BackendConnectionPool(BackendConnectionPool&& other) = delete;
  BackendConnectionPool& operator=(const BackendConnectionPool& other) = delete;
  BackendConnectionPool& operator=(BackendConnectionPool&& other) = delete;

  bool isEnabled() const { return itsEnabled; }

  // ------------------------------------------------------------------
  /*!
   * \brief Move a live idle connection to this backend into theSocket
   *
   * Returns false if there was none, or none that survived the liveness check,
   * in which case theSocket is left untouched and the caller must connect.
   */
  // ------------------------------------------------------------------
  bool acquire(const std::string& theIP, unsigned short thePort, Socket& theSocket);

  // ------------------------------------------------------------------
  /*!
   * \brief Take a connection back for reuse
   *
   * The socket must be at a message boundary: the previous response fully read,
   * nothing of the next one buffered anywhere but here. The socket is closed
   * instead of stored if the pool for this backend is already full.
   */
  // ------------------------------------------------------------------
  void release(const std::string& theIP, unsigned short thePort, Socket&& theSocket);

  // Counters for the connections the pool never sees. The frontend also reuses a
  // connection directly - the ETag probe's, for the content request that follows
  // a cache miss - and opens fresh ones outside acquire() when replaying, and all
  // of those belong in the same tally to make it mean anything.
  void recordFreshConnection() { ++itsMisses; }
  void recordDirectReuse() { ++itsHits; }

  // A reused connection turned out to be dead and the request was sent again on
  // a fresh one. Counted here because it is a property of the pool's tuning: a
  // steady trickle of these means the idle timeout is too close to the backend's.
  void recordReplay() { ++itsReplayed; }

  // Drop the idle connections to one backend. Called when a backend is retired,
  // since its pooled connections are of no use and would be tried first.
  void closeBackend(const std::string& theIP, unsigned short thePort);

  // Drop every idle connection (shutdown)
  void closeAll();

  Statistics getStatistics() const;

 private:
  struct Key
  {
    std::string ip;
    unsigned short port;

    bool operator<(const Key& other) const
    {
      if (ip != other.ip)
        return ip < other.ip;
      return port < other.port;
    }
  };

  struct Entry
  {
    Socket socket;
    std::chrono::steady_clock::time_point idleSince;
  };

  // Remove connections that have been idle too long. Called with the lock held.
  void expireIdle(std::deque<Entry>& theEntries);

  // Expire every backend's idle connections, but no more often than the idle
  // timeout itself. Without this, connections to a backend that stops being used
  // would hold file descriptors until shutdown: nothing reads them, so the
  // backend closing its end goes unnoticed until someone asks for one.
  void sweepIfDue();

  const bool itsEnabled;
  const std::size_t itsMaxIdlePerBackend;
  const std::chrono::seconds itsIdleTimeout;

  mutable boost::mutex itsMutex;
  std::map<Key, std::deque<Entry>> itsIdleConnections;
  std::chrono::steady_clock::time_point itsLastSweep = std::chrono::steady_clock::now();

  std::atomic<std::uint64_t> itsHits{0};
  std::atomic<std::uint64_t> itsMisses{0};
  std::atomic<std::uint64_t> itsReturned{0};
  std::atomic<std::uint64_t> itsDiscarded{0};
  std::atomic<std::uint64_t> itsExpired{0};
  std::atomic<std::uint64_t> itsReplayed{0};
  std::atomic<std::uint64_t> itsRejected{0};
};

}  // namespace SmartMet
