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
    int digit;
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
      {
        const std::size_t eol = itsPending.find("\r\n", pos);
        if (eol == std::string::npos)
        {
          wantMore = true;
          break;
        }

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
          break;
        }

        pos = eol + 2;

        if (size == 0)
        {
          itsState = State::TRAILER;
          break;
        }

        itsChunkRemaining = size;
        itsState = State::DATA;
        break;
      }

      case State::DATA:
      {
        const std::size_t available = itsPending.size() - pos;
        if (available == 0)
        {
          wantMore = true;
          break;
        }

        const std::size_t take = std::min(itsChunkRemaining, available);
        theBody.append(itsPending, pos, take);
        pos += take;
        itsChunkRemaining -= take;
        if (itsChunkRemaining == 0)
          itsState = State::DATA_CRLF;
        break;
      }

      case State::DATA_CRLF:
      {
        if (itsPending.size() - pos < 2)
        {
          wantMore = true;
          break;
        }
        if (itsPending.compare(pos, 2, "\r\n") != 0)
        {
          status = Status::FAILED;
          break;
        }
        pos += 2;
        itsState = State::SIZE;
        break;
      }

      case State::TRAILER:
      {
        // Trailer field lines, up to the blank line that ends the message. They
        // are consumed and dropped: the client response's head has been sent
        // long before this point, so there is nowhere left to put them.
        const std::size_t eol = itsPending.find("\r\n", pos);
        if (eol == std::string::npos)
        {
          wantMore = true;
          break;
        }

        if (eol == pos)
          status = Status::COMPLETE;
        pos = eol + 2;
        break;
      }
    }
  }

  itsPending.erase(0, pos);
  return status;
}

}  // namespace SmartMet
