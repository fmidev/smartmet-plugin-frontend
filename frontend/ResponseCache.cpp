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

std::string ResponseCache::makeKey(const std::string& etag, const std::string& content_encoding)
{
  // The unit separator (0x1F) cannot appear in a valid HTTP ETag, so it is a safe
  // delimiter between the ETag and the encoding token.
  return etag + '\x1f' + content_encoding;
}

std::pair<std::shared_ptr<std::string>, ResponseCache::CachedResponseMetaData>
ResponseCache::getCachedBuffer(const std::string& etag, const std::string& content_encoding)
{
  auto mdata = itsMetaDataCache.find(makeKey(etag, content_encoding));

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
                                       const std::string& content_encoding,
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

  itsMetaDataCache.insert(makeKey(etag, content_encoding), data);

  itsBufferCache.insert(data.buffer_hash, buffer);
}
}  // namespace SmartMet
