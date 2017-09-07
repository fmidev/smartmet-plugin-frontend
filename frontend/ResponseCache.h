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
}  // namespace SmartMet
