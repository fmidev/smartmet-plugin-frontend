#pragma once

#include "BackendConnectionPool.h"
#include "ResponseCache.h"

#include <atomic>
#include <boost/asio.hpp>
#include <filesystem>
#include <boost/functional/hash.hpp>
#include <memory>
#include <vector>
#include <boost/thread/condition.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/thread.hpp>

#include <spine/HTTP.h>
#include <spine/Reactor.h>

#include <macgyver/Cache.h>

namespace SmartMet
{
class LowLatencyGatewayStreamer;

class Proxy : public std::enable_shared_from_this<Proxy>
{
  friend class LowLatencyGatewayStreamer;

  struct Private{ explicit Private() = default; };

 public:
  // Return codes for proxy transactions. Based on these return values
  // the service or host is removed from the Services list if there
  // is any kind of problem connecting to or received data from the backend.
  enum class ProxyStatus
  {
    PROXY_SUCCESS,
    PROXY_FAIL_REMOTE_HOST = 200,    // socket closed etc
    PROXY_FAIL_SERVICE = 300,        // service not found etc
    PROXY_FAIL_REMOTE_DENIED = 400,  // backend shutting down or too high load
    PROXY_INTERNAL_ERROR = 500
  };

  Proxy(Private,
        std::size_t memoryCacheSize,
        std::size_t filesystemCacheSize,
        const std::filesystem::path& fileCachePath,
        int theBackendThreadCount,
        int theBackendTimeoutInSeconds,
        int theShutdownGracePeriodInSeconds,
        bool theBackendKeepAlive,
        std::size_t theMaxIdleConnectionsPerBackend,
        int theBackendIdleTimeoutInSeconds);

  static std::shared_ptr<Proxy>
  create(std::size_t memoryCacheSize,
        std::size_t filesystemCacheSize,
        const std::filesystem::path& fileCachePath,
        int theBackendThreadCount,
        int theBackendTimeoutInSeconds,
        int theShutdownGracePeriodInSeconds,
        bool theBackendKeepAlive,
        std::size_t theMaxIdleConnectionsPerBackend,
        int theBackendIdleTimeoutInSeconds);

  // Method to do HTTP transfer between requesting client and abackend
  // at the provided IP address - with optional port (defaults to 80)
  ProxyStatus HTTPForward(Spine::Reactor& theReactor,
                          const Spine::HTTP::Request& theRequest,
                          Spine::HTTP::Response& TheResponse,
                          const std::string& theBackendIP,
                          int theBackendPort,
                          const std::string& theBackendURI,
                          const std::string& theHostName);

  // Single response cache holding all content encodings (identity, gzip, zstd, ...),
  // keyed internally by (ETag, encoding).
  ResponseCache& getCache();

  // Idle backend connections kept for reuse. Empty and inert when backend
  // keep-alive is switched off.
  BackendConnectionPool& getBackendConnectionPool() { return itsBackendConnections; }
  const BackendConnectionPool& getBackendConnectionPool() const { return itsBackendConnections; }

  // Drop the idle connections to a backend that is being retired: they lead
  // somewhere that is no longer answering, and would be tried first.
  void closeBackendConnections(const std::string& theIP, unsigned short thePort);

  void shutdown();

  // True once shutdown() has been called. Checked by HTTP::transport() so no new
  // LowLatencyGatewayStreamer is started after shutdown begins draining existing ones.
  bool isShuttingDown() const noexcept { return itsShuttingDown; }

  // Bookkeeping used by LowLatencyGatewayStreamer so shutdown() can wait for every
  // in-flight gateway stream to finish before stopping the backend I/O threads.
  void registerStreamerStart(const std::shared_ptr<LowLatencyGatewayStreamer>& theStreamer);
  void registerStreamerStop();

 private:
  ResponseCache itsResponseCache;

  boost::asio::io_context backendIoService;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> idler;
  boost::thread_group itsBackendThreads;

  // Declared after backendIoService so that the sockets it holds are destroyed
  // before the io_context they belong to
  BackendConnectionPool itsBackendConnections;

  int itsBackendTimeoutInSeconds;

  // How long shutdown() waits for in-flight gateway streams to finish on their own before
  // forcibly aborting them. A multi-gigabyte weather model response can take far longer to
  // stream than a process manager's stop timeout (e.g. systemd's TimeoutStopSec), so waiting
  // for it to finish naturally would just mean the whole process gets SIGKILLed anyway with
  // the response lost either way. Aborting well within that budget lets shutdown complete
  // cleanly and lets the client see an error promptly instead of a silently dropped connection.
  int itsShutdownGracePeriodInSeconds;

  std::atomic<bool> itsShuttingDown{false};

  // Number of currently live LowLatencyGatewayStreamer instances. shutdown() waits for
  // this to reach zero before stopping backendIoService, since a streamer whose backend
  // thread pool has already stopped can never finish, and its shared_ptr keeps this Proxy
  // (and this plugin's shared library) alive past the point the Reactor unloads it.
  boost::mutex itsStreamerCountMutex;
  boost::condition_variable itsStreamerDrainedCond;
  std::size_t itsActiveStreamerCount = 0;

  // Weak references to currently live streamers, used only to forcibly abort them if they
  // are still running once the shutdown grace period has elapsed.
  std::vector<std::weak_ptr<LowLatencyGatewayStreamer>> itsActiveStreamers;
};
}  // namespace SmartMet
