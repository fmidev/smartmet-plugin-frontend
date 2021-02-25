#include "ResponseCache.h"

namespace SmartMet
{
ResponseCache::ResponseCache(std::size_t memoryCacheSize,
                             std::size_t filesystemCacheSize,
                             const boost::filesystem::path& fileCachePath)
    : itsMetaDataCache((memoryCacheSize + filesystemCacheSize) /
                       8192)  // Buffer cache sizes are in bytes, this in units
      ,
      itsBufferCache(memoryCacheSize, filesystemCacheSize, fileCachePath)
{
}

std::pair<boost::shared_ptr<std::string>, ResponseCache::CachedResponseMetaData>
ResponseCache::getCachedBuffer(const std::string& etag)
{
  auto mdata = itsMetaDataCache.find(etag);

  if (mdata)
  {
    std::size_t bufferhash = mdata->buffer_hash;

    auto buffer = itsBufferCache.find(bufferhash);

    return std::make_pair(buffer, *mdata);
  }

  return {};
}

void ResponseCache::insertCachedBuffer(const std::string& etag,
                                       const std::string& mime_type,
                                       const std::string& cache_control,
                                       const std::string& expires,
                                       const std::string& vary,
                                       const std::string& access_control_allow_origin,
                                       ResponseCache::ContentEncodingType content_encoding,
                                       const boost::shared_ptr<std::string>& buffer)
{
  boost::hash<std::string> string_hash;

  std::size_t buffer_hash = string_hash(*buffer);

  itsMetaDataCache.insert(etag,
                          {buffer_hash,
                           mime_type,
                           etag,
                           cache_control,
                           expires,
                           vary,
                           access_control_allow_origin,
                           content_encoding});

  itsBufferCache.insert(buffer_hash, buffer);
}
}  // namespace SmartMet
