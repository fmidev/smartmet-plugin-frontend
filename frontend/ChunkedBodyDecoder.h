#pragma once

#include <cstddef>
#include <string>

namespace SmartMet
{
// ----------------------------------------------------------------------
/*!
 * \brief Incremental decoder for a chunked message body
 *
 * The frontend reads backend responses in socket-sized pieces, so the chunk
 * framing has to be removed a fragment at a time: a chunk header, a chunk's
 * data or the trailing CRLF may all be split across reads. feed() is given
 * whatever arrived and appends the decoded body bytes to an output string,
 * keeping the undecoded remainder for the next call.
 *
 * Decoding matters because the frontend re-frames the response for the client:
 * the body it hands the server must be plain bytes, which the server then
 * frames itself (and may frame differently from the backend).
 *
 * The trailer section is consumed but discarded. Forwarding trailer fields
 * would mean appending header fields to a client response whose head has long
 * since been sent.
 */
// ----------------------------------------------------------------------

class ChunkedBodyDecoder
{
 public:
  enum class Status
  {
    NEED_MORE,  // Well formed so far, waiting for the rest
    COMPLETE,   // The zero-sized chunk and its trailer section have been read
    FAILED      // Malformed framing; the body cannot be trusted
  };

  // ------------------------------------------------------------------
  /*!
   * \brief Decode a fragment, appending body bytes to theBody
   *
   * \param data   Bytes as they arrived from the backend
   * \param length Number of bytes
   * \param theBody Decoded body bytes are appended here
   */
  // ------------------------------------------------------------------
  Status feed(const char* data, std::size_t length, std::string& theBody);

  // Bytes still held back because a chunk header or terminator was cut in half
  std::size_t pending() const { return itsPending.size(); }

 private:
  enum class State
  {
    SIZE,       // Reading the "chunk-size [ ; chunk-ext ]" line
    DATA,       // Copying chunk data out
    DATA_CRLF,  // Expecting the CRLF that terminates a chunk's data
    TRAILER     // Reading trailer field lines up to the final blank line
  };

  std::string itsPending;
  State itsState = State::SIZE;
  std::size_t itsChunkRemaining = 0;
};

}  // namespace SmartMet
