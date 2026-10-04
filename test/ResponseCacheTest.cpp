#define BOOST_TEST_MODULE "ResponseCacheTest"

#include "../frontend/ResponseCache.h"
#include <boost/test/included/unit_test.hpp>
#include <filesystem>
#include <memory>
#include <string>

using SmartMet::ResponseCache;

namespace
{
std::filesystem::path cachedir()
{
  auto dir = std::filesystem::temp_directory_path() /
             ("frontend-responsecache-test-" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  return dir;
}

std::shared_ptr<std::string> buffer(const std::string& content)
{
  return std::make_shared<std::string>(content);
}

void insert(ResponseCache& cache,
            const std::string& etag,
            const std::string& encoding,
            const std::string& content)
{
  cache.insertCachedBuffer(
      etag, "image/png", "max-age=60", "", "Accept-Encoding", "*", encoding, buffer(content));
}
}  // namespace

BOOST_AUTO_TEST_CASE(insert_and_find)
{
  auto dir = cachedir();
  ResponseCache cache(1000000, 0, dir);

  insert(cache, "\"abc\"", "", "plain content");
  auto result = cache.getCachedBuffer("\"abc\"", "");
  BOOST_REQUIRE(result.first);
  BOOST_CHECK_EQUAL(*result.first, "plain content");
  BOOST_CHECK_EQUAL(result.second.etag, "\"abc\"");
  BOOST_CHECK_EQUAL(result.second.mime_type, "image/png");
  BOOST_CHECK_EQUAL(result.second.cache_control, "max-age=60");
  BOOST_CHECK_EQUAL(result.second.vary, "Accept-Encoding");
  BOOST_CHECK_EQUAL(result.second.access_control_allow_origin, "*");
  BOOST_CHECK_EQUAL(result.second.content_encoding, "");

  // Unknown ETags are not found
  BOOST_CHECK(!cache.getCachedBuffer("\"xyz\"", "").first);

  std::filesystem::remove_all(dir);
}

BOOST_AUTO_TEST_CASE(encodings_are_separate_entries)
{
  auto dir = cachedir();
  ResponseCache cache(1000000, 0, dir);

  insert(cache, "\"abc\"", "", "identity");
  insert(cache, "\"abc\"", "gzip", "gzipped");
  insert(cache, "\"abc\"", "zstd", "zstded");

  BOOST_CHECK_EQUAL(*cache.getCachedBuffer("\"abc\"", "").first, "identity");
  BOOST_CHECK_EQUAL(*cache.getCachedBuffer("\"abc\"", "gzip").first, "gzipped");
  BOOST_CHECK_EQUAL(*cache.getCachedBuffer("\"abc\"", "zstd").first, "zstded");
  BOOST_CHECK_EQUAL(cache.getCachedBuffer("\"abc\"", "gzip").second.content_encoding, "gzip");
  BOOST_CHECK(!cache.getCachedBuffer("\"abc\"", "br").first);

  // The ETag and the encoding cannot be confused by concatenation
  insert(cache, "\"ab", "c\"gzip", "tricky");
  BOOST_CHECK_EQUAL(*cache.getCachedBuffer("\"abc\"", "gzip").first, "gzipped");

  std::filesystem::remove_all(dir);
}

BOOST_AUTO_TEST_CASE(identical_content_is_shared)
{
  auto dir = cachedir();
  ResponseCache cache(1000000, 0, dir);

  // Different ETags with identical content share the buffer
  insert(cache, "\"one\"", "", "same bytes");
  insert(cache, "\"two\"", "", "same bytes");
  auto r1 = cache.getCachedBuffer("\"one\"", "");
  auto r2 = cache.getCachedBuffer("\"two\"", "");
  BOOST_REQUIRE(r1.first && r2.first);
  BOOST_CHECK_EQUAL(r1.second.buffer_hash, r2.second.buffer_hash);
  BOOST_CHECK_EQUAL(*r1.first, *r2.first);

  std::filesystem::remove_all(dir);
}

BOOST_AUTO_TEST_CASE(first_insert_wins)
{
  auto dir = cachedir();
  ResponseCache cache(1000000, 0, dir);

  // An ETag identifies one representation, hence a repeated insert keeps the cached entry
  insert(cache, "\"abc\"", "", "old");
  insert(cache, "\"abc\"", "", "new");
  BOOST_CHECK_EQUAL(*cache.getCachedBuffer("\"abc\"", "").first, "old");

  std::filesystem::remove_all(dir);
}
