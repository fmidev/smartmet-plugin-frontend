#include "ResponseCache.h"

namespace SmartMet
{
ResponseCache::ResponseCache(std::size_t memoryCacheSize,
                             std::size_t filesystemCacheSize,
                             const boost::filesystem::path& fileCachePath)
    : itsMetaDataCache((memoryCacheSize + filesystemCacheSize) /
                       8)  // Buffer cache sizes are in bytes, this in units
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
  else
  {
    return {};
  }
}

void ResponseCache::insertCachedBuffer(const std::string& etag,
                                       const std::string& mime_type,
                                       ResponseCache::ContentEncodingType content_encoding,
                                       boost::shared_ptr<std::string> buffer)
{
  boost::hash<std::string> string_hash;

  std::size_t buffer_hash = string_hash(*buffer);

  itsMetaDataCache.insert(etag, {buffer_hash, mime_type, etag, content_encoding});

  itsBufferCache.insert(buffer_hash, buffer);
}
}

// namespace SmartMet
