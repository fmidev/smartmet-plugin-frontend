#pragma once

#include "ResponseCache.h"

#include <boost/asio.hpp>
#include <filesystem>
#include <boost/functional/hash.hpp>
#include <memory>
#include <boost/thread/condition.hpp>
#include <boost/thread/thread.hpp>

#include <spine/HTTP.h>
#include <spine/Reactor.h>

#include <macgyver/Cache.h>

namespace SmartMet
{
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
        std::size_t uncompressedMemoryCacheSize,
        std::size_t uncompressedFilesystemCacheSize,
        const std::filesystem::path& uncompressedFileCachePath,
        std::size_t compressedMemoryCacheSize,
        std::size_t compressedFilesystemCacheSize,
        const std::filesystem::path& compressedFileCachePath,
        int theBackendThreadCount,
        int theBackendTimeoutInSeconds);

  static std::shared_ptr<Proxy>
  create(std::size_t uncompressedMemoryCacheSize,
        std::size_t uncompressedFilesystemCacheSize,
        const std::filesystem::path& uncompressedFileCachePath,
        std::size_t compressedMemoryCacheSize,
        std::size_t compressedFilesystemCacheSize,
        const std::filesystem::path& compressedFileCachePath,
        int theBackendThreadCount,
        int theBackendTimeoutInSeconds);

  // Method to do HTTP transfer between requesting client and abackend
  // at the provided IP address - with optional port (defaults to 80)
  ProxyStatus HTTPForward(Spine::Reactor& theReactor,
                          const Spine::HTTP::Request& theRequest,
                          Spine::HTTP::Response& TheResponse,
                          const std::string& theBackendIP,
                          int theBackendPort,
                          const std::string& theBackendURI,
                          const std::string& theHostName);

  ResponseCache& getCache(ResponseCache::ContentEncodingType type);

  void shutdown();

 private:
  ResponseCache itsUncompressedResponseCache;
  ResponseCache itsCompressedResponseCache;

  boost::asio::io_context backendIoService;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> idler;
  boost::thread_group itsBackendThreads;

  int itsBackendTimeoutInSeconds;
};
}  // namespace SmartMet
