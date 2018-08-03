#pragma once

#include <boost/filesystem.hpp>
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
    std::size_t buffer_hash;
    std::string mime_type;
    std::string etag;
    std::string cache_control;
    std::string expires;
    std::string vary;
    ContentEncodingType content_encoding;
  };

  ResponseCache(std::size_t memoryCacheSize,
                std::size_t filesystemCacheSize,
                const boost::filesystem::path& fileCachePath);

  std::pair<boost::shared_ptr<std::string>, CachedResponseMetaData> getCachedBuffer(
      const std::string& etag);

  void insertCachedBuffer(const std::string& etag,
                          const std::string& mime_type,
                          const std::string& cache_control,
                          const std::string& expires,
                          const std::string& vary,
                          ContentEncodingType content_encoding,
                          const boost::shared_ptr<std::string>& buffer);

 private:
  // Cache ETag -> Bufferhash
  using MetaDataCache = Fmi::Cache::Cache<std::string, CachedResponseMetaData>;

  // Cache Bufferhash -> Buffer
  using BufferCache = Spine::SmartMetCache;

  MetaDataCache itsMetaDataCache;

  BufferCache itsBufferCache;
};
}  // namespace SmartMet
