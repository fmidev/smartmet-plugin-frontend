#define BOOST_TEST_MODULE "ChunkedBodyDecoderTest"

#include "../frontend/ChunkedBodyDecoder.h"
#include <boost/test/included/unit_test.hpp>
#include <string>

using SmartMet::ChunkedBodyDecoder;
using Status = ChunkedBodyDecoder::Status;

namespace
{
// Feed the whole input at once
Status decode(const std::string& input, std::string& body)
{
  ChunkedBodyDecoder decoder;
  return decoder.feed(input.data(), input.size(), body);
}

// Feed the input one byte at a time, as if every byte arrived in its own read
Status decode_bytewise(const std::string& input, std::string& body)
{
  ChunkedBodyDecoder decoder;
  Status status = Status::NEED_MORE;
  for (char c : input)
  {
    status = decoder.feed(&c, 1, body);
    if (status != Status::NEED_MORE)
      break;
  }
  return status;
}

const std::string wikipedia = "4\r\nWiki\r\n7\r\npedia i\r\nB\r\nn \r\nchunks.\r\n0\r\n\r\n";
}  // namespace

BOOST_AUTO_TEST_CASE(complete_message)
{
  std::string body;
  BOOST_CHECK(decode(wikipedia, body) == Status::COMPLETE);
  BOOST_CHECK_EQUAL(body, "Wikipedia in \r\nchunks.");
}

BOOST_AUTO_TEST_CASE(fragmented_message)
{
  std::string body;
  BOOST_CHECK(decode_bytewise(wikipedia, body) == Status::COMPLETE);
  BOOST_CHECK_EQUAL(body, "Wikipedia in \r\nchunks.");

  // Every split point gives the same result
  for (std::size_t split = 1; split < wikipedia.size(); split++)
  {
    ChunkedBodyDecoder decoder;
    std::string body2;
    auto status = decoder.feed(wikipedia.data(), split, body2);
    BOOST_CHECK(status == Status::NEED_MORE);
    status = decoder.feed(wikipedia.data() + split, wikipedia.size() - split, body2);
    BOOST_CHECK(status == Status::COMPLETE);
    BOOST_CHECK_EQUAL(body2, "Wikipedia in \r\nchunks.");
  }
}

BOOST_AUTO_TEST_CASE(incomplete_message)
{
  std::string body;
  ChunkedBodyDecoder decoder;
  std::string input = "5\r\nhel";
  BOOST_CHECK(decoder.feed(input.data(), input.size(), body) == Status::NEED_MORE);
  BOOST_CHECK_EQUAL(body, "hel");

  // A chunk header cut in half is held back
  input = "lo\r\n1";
  BOOST_CHECK(decoder.feed(input.data(), input.size(), body) == Status::NEED_MORE);
  BOOST_CHECK_EQUAL(body, "hello");
  BOOST_CHECK_EQUAL(decoder.pending(), 1U);

  input = "0\r\n";
  BOOST_CHECK(decoder.feed(input.data(), input.size(), body) == Status::NEED_MORE);
  std::string data(16, 'x');
  data += "\r\n0\r\n\r\n";
  BOOST_CHECK(decoder.feed(data.data(), data.size(), body) == Status::COMPLETE);
  BOOST_CHECK_EQUAL(body, "hello" + std::string(16, 'x'));
}

BOOST_AUTO_TEST_CASE(extensions_case_and_trailers)
{
  std::string body;
  // Chunk extensions are ignored, hex digits may be upper or lower case, trailers are dropped
  std::string input =
      "a;name=value\r\n0123456789\r\nA\r\nabcdefghij\r\n0;last\r\nX-Trailer: 1\r\nY: 2\r\n\r\n";
  BOOST_CHECK(decode(input, body) == Status::COMPLETE);
  BOOST_CHECK_EQUAL(body, "0123456789abcdefghij");

  body.clear();
  BOOST_CHECK(decode_bytewise(input, body) == Status::COMPLETE);
  BOOST_CHECK_EQUAL(body, "0123456789abcdefghij");
}

BOOST_AUTO_TEST_CASE(empty_body)
{
  std::string body;
  BOOST_CHECK(decode("0\r\n\r\n", body) == Status::COMPLETE);
  BOOST_CHECK(body.empty());
}

BOOST_AUTO_TEST_CASE(malformed_framing)
{
  std::string body;
  // Not a hexadecimal size
  BOOST_CHECK(decode("xyz\r\nabc\r\n0\r\n\r\n", body) == Status::FAILED);
  // Empty size line
  BOOST_CHECK(decode("\r\nabc\r\n0\r\n\r\n", body) == Status::FAILED);
  // Negative size
  BOOST_CHECK(decode("-5\r\nhello\r\n0\r\n\r\n", body) == Status::FAILED);
  // Chunk data longer than announced
  BOOST_CHECK(decode("3\r\nhello\r\n0\r\n\r\n", body) == Status::FAILED);
  // Too large sizes must not overflow
  BOOST_CHECK(decode("ffffffffffffffffff\r\n", body) == Status::FAILED);
  BOOST_CHECK(decode("fffffffffffffff\r\n", body) == Status::FAILED);
}
