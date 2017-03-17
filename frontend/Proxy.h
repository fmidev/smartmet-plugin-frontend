#pragma once

#include <boost/thread/thread.hpp>
#include <boost/thread/condition.hpp>
#include <boost/filesystem.hpp>
#include <boost/asio.hpp>
#include <boost/functional/hash.hpp>
#include <boost/shared_ptr.hpp>

#include <spine/HTTP.h>
#include <spine/SmartMetCache.h>
#include <spine/Reactor.h>
#include <engines/sputnik/Engine.h>

#include <macgyver/Cache.h>

namespace SmartMet
{
class ResponseCache
{
 public:
  enum ContentEncodingType
  {
    NONE,
    GZIP
  };

  struct CachedResponseMetaData
  {
    std::size_t buffer_hash;
    std::string mime_type;
    std::string etag;
    ContentEncodingType content_encoding;
  };

  ResponseCache(std::size_t memoryCacheSize,
                std::size_t filesystemCacheSize,
                const boost::filesystem::path& fileCachePath);

  std::pair<boost::shared_ptr<std::string>, CachedResponseMetaData> getCachedBuffer(
      const std::string& etag);

  void insertCachedBuffer(const std::string& etag,
                          const std::string& mime_type,
                          ContentEncodingType content_encoding,
                          boost::shared_ptr<std::string> buffer);

 private:
  // Cache ETag -> Bufferhash
  typedef Fmi::Cache::Cache<std::string, CachedResponseMetaData> MetaDataCache;

  // Cache Bufferhash -> Buffer
  typedef Spine::SmartMetCache BufferCache;

  MetaDataCache itsMetaDataCache;

  BufferCache itsBufferCache;
};

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
    PROXY_FAIL_REMOTE_HOST = 200,     // socket closed etc
    PROXY_FAIL_SERVICE = 300,         // service not found etc
    PROXY_FAIL_REMOTE_SHUTDOWN = 400  // backend shutting down
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
