#include "ChunkedBodyDecoder.h"
#include <boost/algorithm/string.hpp>
#include <algorithm>

namespace SmartMet
{
namespace
{
// Longest chunk size worth believing. The frontend bounds the buffered response
// separately; this only keeps a hostile chunk header from overflowing the
// accumulator.
constexpr std::size_t max_chunk_size = std::size_t(1) << 40;

bool parseChunkSize(const std::string& field, std::size_t& result)
{
  if (field.empty() || field.size() > 15)
    return false;

  std::size_t size = 0;
  for (const char c : field)
  {
    int digit = 0;
    if (c >= '0' && c <= '9')
      digit = c - '0';
    else if (c >= 'a' && c <= 'f')
      digit = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
      digit = c - 'A' + 10;
    else
      return false;

    size = size * 16 + static_cast<std::size_t>(digit);
    if (size > max_chunk_size)
      return false;
  }

  result = size;
  return true;
}
}  // namespace

// Read the "chunk-size [ ; chunk-ext ]" line. Returns true if more data is needed.
bool ChunkedBodyDecoder::readSizeLine(std::size_t& pos, Status& status)
{
  const std::size_t eol = itsPending.find("\r\n", pos);
  if (eol == std::string::npos)
    return true;

  // chunk-size [ ";" chunk-ext ]
  std::string field = itsPending.substr(pos, eol - pos);
  const std::size_t ext = field.find(';');
  if (ext != std::string::npos)
    field.resize(ext);
  boost::algorithm::trim(field);

  std::size_t size = 0;
  if (!parseChunkSize(field, size))
  {
    status = Status::FAILED;
    return false;
  }

  pos = eol + 2;

  if (size == 0)
  {
    itsState = State::TRAILER;
    return false;
  }

  itsChunkRemaining = size;
  itsState = State::DATA;
  return false;
}

// Read chunk data into the body. Returns true if more data is needed.
bool ChunkedBodyDecoder::readData(std::size_t& pos, std::string& theBody)
{
  const std::size_t available = itsPending.size() - pos;
  if (available == 0)
    return true;

  const std::size_t take = std::min(itsChunkRemaining, available);
  theBody.append(itsPending, pos, take);
  pos += take;
  itsChunkRemaining -= take;
  if (itsChunkRemaining == 0)
    itsState = State::DATA_CRLF;
  return false;
}

// Read the CRLF ending the chunk data. Returns true if more data is needed.
bool ChunkedBodyDecoder::readDataCrlf(std::size_t& pos, Status& status)
{
  if (itsPending.size() - pos < 2)
    return true;
  if (itsPending.compare(pos, 2, "\r\n") != 0)
  {
    status = Status::FAILED;
    return false;
  }
  pos += 2;
  itsState = State::SIZE;
  return false;
}

// Read a trailer field line. Returns true if more data is needed.
bool ChunkedBodyDecoder::readTrailer(std::size_t& pos, Status& status)
{
  // Trailer field lines, up to the blank line that ends the message. They
  // are consumed and dropped: the client response's head has been sent
  // long before this point, so there is nowhere left to put them.
  const std::size_t eol = itsPending.find("\r\n", pos);
  if (eol == std::string::npos)
    return true;

  if (eol == pos)
    status = Status::COMPLETE;
  pos = eol + 2;
  return false;
}

ChunkedBodyDecoder::Status ChunkedBodyDecoder::feed(const char* data,
                                                    std::size_t length,
                                                    std::string& theBody)
{
  itsPending.append(data, length);

  std::size_t pos = 0;
  Status status = Status::NEED_MORE;
  bool wantMore = false;

  while (status == Status::NEED_MORE && !wantMore)
  {
    switch (itsState)
    {
      case State::SIZE:
        wantMore = readSizeLine(pos, status);
        break;
      case State::DATA:
        wantMore = readData(pos, theBody);
        break;
      case State::DATA_CRLF:
        wantMore = readDataCrlf(pos, status);
        break;
      case State::TRAILER:
        wantMore = readTrailer(pos, status);
        break;
    }
  }

  itsPending.erase(0, pos);
  return status;
}

}  // namespace SmartMet
