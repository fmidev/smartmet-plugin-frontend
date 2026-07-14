#pragma once

#include <filesystem>
#include <macgyver/Cache.h>
#include <spine/SmartMetCache.h>
#include <string>

namespace SmartMet
{
class ResponseCache
{
 public:
  struct CachedResponseMetaData
  {
    std::size_t buffer_hash = 0UL;
    std::string mime_type;
    std::string etag;
    std::string cache_control;
    std::string expires;
    std::string vary;
    std::string access_control_allow_origin;
    // Content-Encoding of the cached buffer: "" (identity), "gzip", "zstd", ...
    std::string content_encoding;
  };

  ResponseCache(std::size_t memoryCacheSize,
                std::size_t filesystemCacheSize,
                const std::filesystem::path& fileCachePath);

  // Cache variants of the same resource (same ETag) but different Content-Encoding are
  // stored side by side, keyed by (etag, content_encoding). An empty content_encoding
  // means the identity (uncompressed) representation.
  std::pair<std::shared_ptr<std::string>, CachedResponseMetaData> getCachedBuffer(
      const std::string& etag, const std::string& content_encoding);

  void insertCachedBuffer(const std::string& etag,
                          const std::string& mime_type,
                          const std::string& cache_control,
                          const std::string& expires,
                          const std::string& vary,
                          const std::string& access_control_allow_origin,
                          const std::string& content_encoding,
                          const std::shared_ptr<std::string>& buffer);

  Fmi::Cache::CacheStats getMetaDataCacheStats() const { return itsMetaDataCache.statistics(); }
  Fmi::Cache::CacheStats getMemoryCacheStats() const
  {
    return itsBufferCache.getMemoryCacheStats();
  }
  Fmi::Cache::CacheStats getFileCacheStats() const { return itsBufferCache.getFileCacheStats(); }

 private:
  // Metadata cache key combining the ETag and the content encoding, so that all
  // encodings of a resource live in a single cache.
  static std::string makeKey(const std::string& etag, const std::string& content_encoding);

  // Cache (ETag, encoding) -> Bufferhash
  using MetaDataCache = Fmi::Cache::Cache<std::string, CachedResponseMetaData>;

  // Cache Bufferhash -> Buffer
  using BufferCache = Spine::SmartMetCache;

  MetaDataCache itsMetaDataCache;

  BufferCache itsBufferCache;
};
}  // namespace SmartMet
