#include "ResponseCache.h"

namespace SmartMet
{
ResponseCache::ResponseCache(std::size_t memoryCacheSize,
                             std::size_t filesystemCacheSize,
                             const std::filesystem::path& fileCachePath)
    : itsMetaDataCache((memoryCacheSize + filesystemCacheSize) /
                       8192)  // Buffer cache sizes are in bytes, this in units
      ,
      itsBufferCache(memoryCacheSize, filesystemCacheSize, fileCachePath)
{
}

std::pair<std::shared_ptr<std::string>, ResponseCache::CachedResponseMetaData>
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
                                       const std::shared_ptr<std::string>& buffer)
{
  boost::hash<std::string> string_hash;

  // C++11 does not allow aggregate initialization when the struct has default initializers.
  CachedResponseMetaData data;
  data.buffer_hash = string_hash(*buffer);
  data.mime_type = mime_type;
  data.etag = etag;
  data.cache_control = cache_control;
  data.expires = expires;
  data.vary = vary;
  data.access_control_allow_origin = access_control_allow_origin;
  data.content_encoding = content_encoding;

  itsMetaDataCache.insert(etag, data);

  itsBufferCache.insert(data.buffer_hash, buffer);
}
}  // namespace SmartMet
