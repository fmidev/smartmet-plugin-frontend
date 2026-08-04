#define BOOST_TEST_MODULE "ChunkedBodyDecoderTest"

#include "../frontend/ChunkedBodyDecoder.h"
#include <boost/test/included/unit_test.hpp>
#include <string>
#include <vector>

using SmartMet::ChunkedBodyDecoder;
using Status = SmartMet::ChunkedBodyDecoder::Status;

namespace
{
// Feed the whole message in one go
Status decodeAtOnce(const std::string& wire, std::string& body)
{
  ChunkedBodyDecoder decoder;
  return decoder.feed(wire.data(), wire.size(), body);
}

// Feed the message `step` bytes at a time, the way a socket delivers it. The
// decoder must reach the same result no matter where the message is cut, which
// is the whole reason it is incremental.
Status decodeInSteps(const std::string& wire, std::size_t step, std::string& body)
{
  ChunkedBodyDecoder decoder;
  Status status = Status::NEED_MORE;
  for (std::size_t pos = 0; pos < wire.size(); pos += step)
  {
    const std::size_t take = std::min(step, wire.size() - pos);
    status = decoder.feed(wire.data() + pos, take, body);
    if (status != Status::NEED_MORE)
      break;
  }
  return status;
}
}  // namespace

BOOST_AUTO_TEST_CASE(decodes_a_simple_body)
{
  std::string body;
  BOOST_CHECK(decodeAtOnce("5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n", body) == Status::COMPLETE);
  BOOST_CHECK_EQUAL(body, "hello world");
}

BOOST_AUTO_TEST_CASE(survives_every_possible_fragmentation)
{
  const std::string wire = "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";

  // One byte at a time is the worst case: every chunk header, every piece of
  // data and every terminator is split.
  for (std::size_t step = 1; step <= wire.size(); ++step)
  {
    std::string body;
    BOOST_CHECK_MESSAGE(decodeInSteps(wire, step, body) == Status::COMPLETE,
                        "incomplete at step " << step);
    BOOST_CHECK_MESSAGE(body == "hello world", "wrong body at step " << step << ": '" << body << "'");
  }
}

BOOST_AUTO_TEST_CASE(needs_more_until_the_last_chunk_arrives)
{
  ChunkedBodyDecoder decoder;
  std::string body;

  const std::string first = "5\r\nhello\r\n";
  BOOST_CHECK(decoder.feed(first.data(), first.size(), body) == Status::NEED_MORE);
  BOOST_CHECK_EQUAL(body, "hello");

  // A body that stops here is truncated, not complete: the caller must be able
  // to tell those apart, or it would cache and forward a partial response.
  const std::string last = "0\r\n\r\n";
  BOOST_CHECK(decoder.feed(last.data(), last.size(), body) == Status::COMPLETE);
  BOOST_CHECK_EQUAL(body, "hello");
}

BOOST_AUTO_TEST_CASE(accepts_chunk_extensions)
{
  std::string body;
  BOOST_CHECK(decodeAtOnce("5;name=value\r\nhello\r\n0\r\n\r\n", body) == Status::COMPLETE);
  BOOST_CHECK_EQUAL(body, "hello");
}

BOOST_AUTO_TEST_CASE(consumes_the_trailer_section_without_forwarding_it)
{
  std::string body;
  BOOST_CHECK(decodeAtOnce("5\r\nhello\r\n0\r\nX-Trailer: v\r\nX-Other: w\r\n\r\n", body) ==
              Status::COMPLETE);
  BOOST_CHECK_EQUAL(body, "hello");
}

BOOST_AUTO_TEST_CASE(handles_uppercase_and_large_hex_sizes)
{
  std::string body;
  const std::string data(0x1AB, 'x');
  BOOST_CHECK(decodeAtOnce("1AB\r\n" + data + "\r\n0\r\n\r\n", body) == Status::COMPLETE);
  BOOST_CHECK_EQUAL(body.size(), 0x1ABu);
}

BOOST_AUTO_TEST_CASE(handles_an_empty_body)
{
  std::string body;
  BOOST_CHECK(decodeAtOnce("0\r\n\r\n", body) == Status::COMPLETE);
  BOOST_CHECK_EQUAL(body, "");
}

BOOST_AUTO_TEST_CASE(rejects_a_non_hex_chunk_size)
{
  std::string body;
  BOOST_CHECK(decodeAtOnce("zz\r\nhello\r\n0\r\n\r\n", body) == Status::FAILED);
}

BOOST_AUTO_TEST_CASE(rejects_a_chunk_that_is_not_terminated)
{
  // The chunk claims 5 bytes but "helloXX" does not have CRLF after them, so
  // the framing is broken and the rest of the stream cannot be trusted.
  std::string body;
  BOOST_CHECK(decodeAtOnce("5\r\nhelloXX0\r\n\r\n", body) == Status::FAILED);
}

BOOST_AUTO_TEST_CASE(rejects_an_absurd_chunk_size)
{
  std::string body;
  BOOST_CHECK(decodeAtOnce("FFFFFFFFFFFFFFFF\r\n", body) == Status::FAILED);
}

BOOST_AUTO_TEST_CASE(does_not_hold_bytes_back_once_complete)
{
  ChunkedBodyDecoder decoder;
  std::string body;
  const std::string wire = "5\r\nhello\r\n0\r\n\r\n";
  BOOST_CHECK(decoder.feed(wire.data(), wire.size(), body) == Status::COMPLETE);
  BOOST_CHECK_EQUAL(decoder.pending(), 0u);
}

BOOST_AUTO_TEST_CASE(decodes_many_chunks)
{
  std::string wire;
  std::string expected;
  for (int i = 1; i <= 64; ++i)
  {
    const std::string piece(i, static_cast<char>('a' + (i % 26)));
    char header[16];
    std::snprintf(header, sizeof(header), "%x\r\n", i);
    wire += header + piece + "\r\n";
    expected += piece;
  }
  wire += "0\r\n\r\n";

  std::string body;
  BOOST_CHECK(decodeAtOnce(wire, body) == Status::COMPLETE);
  BOOST_CHECK_EQUAL(body, expected);

  // And the same message delivered in awkward 7-byte pieces
  std::string stepped;
  BOOST_CHECK(decodeInSteps(wire, 7, stepped) == Status::COMPLETE);
  BOOST_CHECK_EQUAL(stepped, expected);
}
