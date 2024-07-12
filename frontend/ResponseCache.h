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
  enum ContentEncodingType
  {
    NONE,
    GZIP
  };

  struct CachedResponseMetaData
  {
    std::size_t buffer_hash = 0UL;
    std::string mime_type;
    std::string etag;
    std::string cache_control;
    std::string expires;
    std::string vary;
    std::string access_control_allow_origin;
    ContentEncodingType content_encoding;
  };

  ResponseCache(std::size_t memoryCacheSize,
                std::size_t filesystemCacheSize,
                const std::filesystem::path& fileCachePath);

  std::pair<std::shared_ptr<std::string>, CachedResponseMetaData> getCachedBuffer(
      const std::string& etag);

  void insertCachedBuffer(const std::string& etag,
                          const std::string& mime_type,
                          const std::string& cache_control,
                          const std::string& expires,
                          const std::string& vary,
                          const std::string& access_control_allow_origin,
                          ContentEncodingType content_encoding,
                          const std::shared_ptr<std::string>& buffer);

  Fmi::Cache::CacheStats getMetaDataCacheStats() const { return itsMetaDataCache.statistics(); }
  Fmi::Cache::CacheStats getMemoryCacheStats() const
  {
    return itsBufferCache.getMemoryCacheStats();
  }
  Fmi::Cache::CacheStats getFileCacheStats() const { return itsBufferCache.getFileCacheStats(); }

 private:
  // Cache ETag -> Bufferhash
  using MetaDataCache = Fmi::Cache::Cache<std::string, CachedResponseMetaData>;

  // Cache Bufferhash -> Buffer
  using BufferCache = Spine::SmartMetCache;

  MetaDataCache itsMetaDataCache;

  BufferCache itsBufferCache;
};
}  // namespace SmartMet
