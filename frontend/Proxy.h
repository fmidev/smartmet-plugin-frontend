#pragma once

#include "ResponseCache.h"

#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include <boost/functional/hash.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/thread/condition.hpp>
#include <boost/thread/thread.hpp>

#include <spine/HTTP.h>
#include <spine/Reactor.h>

#include <macgyver/Cache.h>

namespace SmartMet
{
class Proxy : public boost::enable_shared_from_this<Proxy>
{
  friend class LowLatencyGatewayStreamer;

 public:
  // Return codes for proxy transactions. Based on these return values
  // the service or host is removed from the Services list if there
  // is any kind of problem connecting to or received data from the backend.
  enum class ProxyStatus
  {
    PROXY_SUCCESS,
    PROXY_FAIL_REMOTE_HOST = 200,   // socket closed etc
    PROXY_FAIL_SERVICE = 300,       // service not found etc
    PROXY_FAIL_REMOTE_DENIED = 400  // backend shutting down or too high load
  };

  Proxy(std::size_t uncompressedMemoryCacheSize,
        std::size_t uncompressedFilesystemCacheSize,
        const boost::filesystem::path& uncompressedFileCachePath,
        std::size_t compressedMemoryCacheSize,
        std::size_t compressedFilesystemCacheSize,
        const boost::filesystem::path& compressedFileCachePath);

  // Method to do HTTP transfer between requesting client and abackend
  // at the provided IP address - with optional port (defaults to 80)
  ProxyStatus HTTPForward(const Spine::HTTP::Request& theRequest,
                          Spine::HTTP::Response& TheResponse,
                          std::string& theBackendIP,
                          int theBackendPort,
                          std::string& theBackendURI,
                          const std::string& theHostName);

  ResponseCache& getCache(ResponseCache::ContentEncodingType type);

  void shutdown();

 private:
  ResponseCache itsUncompressedResponseCache;

  ResponseCache itsCompressedResponseCache;

  boost::asio::io_service backendIoService;

  boost::asio::io_service::work idler;

  boost::thread_group itsBackendThreads;
};
}  // namespace SmartMet
