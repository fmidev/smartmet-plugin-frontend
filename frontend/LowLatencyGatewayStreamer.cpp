#include "LowLatencyGatewayStreamer.h"
#include "Proxy.h"
#include <fmt/format.h>
#include <macgyver/StringConversion.h>
#include <spine/Convenience.h>
#include <iostream>
#include <sstream>
#include <string>

namespace SmartMet
{
namespace ip = boost::asio::ip;

namespace
{
#ifndef PROXY_MAX_BUFFER_SIZE
const std::size_t proxy_max_buffer_size = 16777216;  // 16 MB
#else
const std::size_t proxy_max_buffer_size = PROXY_MAX_BUFFER_SIZE;
#endif

#ifndef PROXY_MAX_CACHED_BUFFER_SIZE
const std::size_t proxy_max_cached_buffer_size = 20971520;  // 20 MB
#else
const std::size_t proxy_max_cached_buffer_size = PROXY_MAX_CACHED_BUFFER_SIZE;
#endif

// Format response header date as in "Fri, 27 Jul 2018 11:26:04 GMT"

std::string makeDateString()
{
  try
  {
    return Fmi::to_http_string(Fmi::SecondClock::universal_time());
  }
  catch (...)
  {
    throw Fmi::Exception(BCP, "Failed to build HTTP response date");
  }
}

// Return the content encoding to serve for this request, as a Content-Encoding token
// ("gzip", "zstd", ...) or "" for the identity (uncompressed) representation.
std::string clientAcceptsContentEncoding(const Spine::HTTP::Request& request)
{
  try
  {
    auto accept_encoding = request.getHeader("Accept-Encoding");
    if (accept_encoding)
    {
      // Mirror the backend's preference order (see Server::select_content_encoding):
      // prefer zstd, fall back to gzip, so the cached variant matches what the backend produced.
      if (boost::algorithm::contains(*accept_encoding, "zstd"))
        return "zstd";

      if (*accept_encoding == "*")
        return "gzip";  // Accepts everything, send zipped

      if (boost::algorithm::contains(*accept_encoding, "gzip"))
        return "gzip";
      return "";
    }
    return "";
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

// Build metadata part of the response

ResponseCache::CachedResponseMetaData build_metadata(const Spine::HTTP::Response& response)
{
  ResponseCache::CachedResponseMetaData meta;

  // Safe to dereference, checked earlier
  meta.mime_type = *response.getHeader("Content-Type");
  meta.etag = *response.getHeader("ETag");

  auto expires = response.getHeader("Expires");
  if (expires)
    meta.expires = *expires;

  auto cache_control = response.getHeader("Cache-Control");
  if (cache_control)
    meta.cache_control = *cache_control;

  auto vary = response.getHeader("Vary");
  if (vary)
    meta.vary = *vary;

  auto access_control_allow_origin = response.getHeader("Access-Control-Allow-Origin");
  if (access_control_allow_origin)
    meta.access_control_allow_origin = *access_control_allow_origin;

  // Store the backend's Content-Encoding verbatim (lowercased/trimmed) so any codec
  // can be cached; an empty value means the identity representation.
  auto content_encoding = response.getHeader("Content-Encoding");
  if (content_encoding)
  {
    meta.content_encoding = *content_encoding;
    boost::algorithm::to_lower(meta.content_encoding);
    boost::algorithm::trim(meta.content_encoding);
  }

  return meta;
}

Spine::HTTP::Response buildCacheResponse(const Spine::HTTP::Request& originalRequest,
                                         const std::shared_ptr<std::string>& cachedBuffer,
                                         const ResponseCache::CachedResponseMetaData& metadata)
{
  try
  {
    Spine::HTTP::Response response;

    response.setHeader("Date", makeDateString());
    response.setHeader("Server", "SmartMet Synapse (" __TIME__ " " __DATE__ ")");
    response.setHeader("X-Frontend-Server", boost::asio::ip::host_name());

    if (response.getVersion() == "1.1")
    {
      response.setHeader("Connection",
                         "close");  // Current implementation is one-request-per-Connection
    }

    // The cache related response headers should be the same for 200 OK responses
    // and 304 Not Modified responses. RFC7232: "The server generating a 304 response MUST generate
    // any of the following header fields that would have been sent in a 200 (OK) response to the
    // same request: Cache-Control, Content-Location, Date, ETag, Expires, and Vary."

    if (!metadata.expires.empty())
      response.setHeader("Expires", metadata.expires);
    else
      response.setHeader("Expires", "Thu, 01 Jan 1970 00:00:00 GMT");

    if (!metadata.cache_control.empty())
      response.setHeader("Cache-Control", metadata.cache_control);
    else
      response.setHeader("Cache-Control", "must-revalidate");

    if (!metadata.vary.empty())
      response.setHeader("Vary", metadata.vary);
    else
      response.setHeader("Vary", "Accept-Encoding");

    if (!metadata.access_control_allow_origin.empty())
      response.setHeader("Access-Control-Allow-Origin", metadata.access_control_allow_origin);

    // A cache hit means the ETag we hold is the current representation, so
    // advertise it on every response (200 OK, 304 Not Modified and 412
    // Precondition Failed alike). RFC 7232 requires the ETag to be present on
    // a 304 response, and the current code omitted it there.
    if (!metadata.etag.empty() && metadata.etag != "0")
      response.setHeader("ETag", metadata.etag);

    // Decide whether the client already holds the current representation and
    // may be answered with "304 Not Modified", whether a conditional
    // precondition failed and we must reply "412 Precondition Failed", or
    // whether the full body has to be returned.
    //
    // ETagFilter::evaluate() interprets the If-Match / If-None-Match request
    // headers (several entity-tags, the "*" wildcard, weak/strong comparison)
    // and returns {full_response_required, suggested_status} per RFC 7232.
    //
    // Per RFC 7232 the If-Modified-Since header MUST be ignored when an
    // entity-tag precondition is present, so it is only consulted otherwise.
    // The frontend cache is validated purely by ETag and keeps no
    // Last-Modified timestamp, so the date itself cannot be compared: a cache
    // hit means the requested ETag is the current one, so a bare
    // If-Modified-Since request is honoured with "304 Not Modified".

    Spine::HTTP::ETagFilter etag_filter(originalRequest);

    auto [full_response_required, suggested_status] = etag_filter.evaluate(metadata.etag);

    if (full_response_required && !etag_filter.has_if_match() && !etag_filter.has_if_none_match() &&
        originalRequest.getHeader("If-Modified-Since"))
    {
      full_response_required = false;
      suggested_status = Spine::HTTP::Status::not_modified;
    }

    // This block prepares the client response
    if (!full_response_required)
    {
      // 304 Not Modified or 412 Precondition Failed: no body
      response.setStatus(suggested_status);
    }
    else
    {
      // No matching precondition: return the full cached body

      response.setHeader("Content-Type", metadata.mime_type);
      if (!metadata.content_encoding.empty())
        response.setHeader("Content-Encoding", metadata.content_encoding);
      response.setHeader("Content-Length", std::to_string(cachedBuffer->size()));

      response.setHeader("X-Frontend-Cache-Hit", "true");

      response.setStatus(Spine::HTTP::Status::ok);
      response.setContent(cachedBuffer);
    }

    return response;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

// Serialise a request for the backend with a Content-Length that matches the
// bytes actually produced.
//
// Request::toString() re-encodes form-urlencoded parameters rather than echoing
// the body it was given, so the body it emits can differ in length from the
// Content-Length the client sent: "a=b+c" comes back as "a=b%20c", 9 bytes
// declared against 11 produced. The backend then frames the body by the stale
// length - truncating it, or rejecting the request outright, depending on its
// parser - and on a connection that gets reused the leftover bytes would be
// read as the start of the next request.
std::string serialiseRequest(Spine::HTTP::Request& request)
{
  std::string message = request.toString();

  const std::size_t headEnd = message.find("\r\n\r\n");
  if (headEnd == std::string::npos)
    return message;

  const std::size_t bodyLength = message.size() - headEnd - 4;
  const std::string actual = Fmi::to_string(bodyLength);
  const auto declared = request.getHeader("Content-Length");

  if (declared && *declared == actual)
    return message;
  if (!declared && bodyLength == 0)
    return message;

  // The serialised body does not depend on the Content-Length field, so one
  // more pass is enough to make the two agree.
  request.setHeader("Content-Length", actual);
  return request.toString();
}

// Hop-by-hop fields describe a single connection and must not be forwarded
// through a proxy (RFC 9110 7.6.1). Whatever the Connection header itself names
// is hop-by-hop too.
void stripHopByHopHeaders(Spine::HTTP::Response& response)
{
  static const char* const hopByHop[] = {"Connection",
                                         "Keep-Alive",
                                         "Proxy-Connection",
                                         "Proxy-Authenticate",
                                         "Proxy-Authorization",
                                         "TE",
                                         "Trailer",
                                         "Upgrade"};

  std::vector<std::string> listed;
  auto connection = response.getHeader("Connection");
  if (connection)
  {
    std::vector<std::string> tokens;
    boost::algorithm::split(tokens, *connection, boost::algorithm::is_any_of(","));
    for (auto& token : tokens)
    {
      boost::algorithm::trim(token);
      if (!token.empty())
        listed.push_back(token);
    }
  }

  for (const auto* name : hopByHop)
    response.removeHeader(name);
  for (const auto& name : listed)
    response.removeHeader(name);
}

// Does a comma separated header field list contain this token? Used on the
// Connection field, whose tokens are case insensitive and may be surrounded by
// whitespace.
bool hasHeaderToken(const std::string& theValue, const std::string& theToken)
{
  std::vector<std::string> tokens;
  boost::algorithm::split(tokens, theValue, boost::algorithm::is_any_of(","));
  for (auto& token : tokens)
  {
    boost::algorithm::trim(token);
    if (boost::algorithm::iequals(token, theToken))
      return true;
  }
  return false;
}

// Did the connection die under us, as opposed to the request failing on its own
// merits? Only these are worth replaying: a connection taken from the pool can
// have been closed by the backend between the liveness check and the write.
bool isDroppedConnection(const boost::system::error_code& theError)
{
  return theError == boost::asio::error::eof || theError == boost::asio::error::connection_reset ||
         theError == boost::asio::error::connection_aborted ||
         theError == boost::asio::error::broken_pipe;
}

// Statuses that are framed by the status code itself: they carry no body, and
// carry no framing header field either (RFC 9112 6.3). The distinction only
// started to matter once backend connections became persistent - before that,
// "the backend closed" was an adequate, if slow, way to learn a body had ended.
bool statusHasNoBody(Spine::HTTP::Status theStatus)
{
  const auto code = static_cast<int>(theStatus);
  return (code >= 100 && code < 200) || theStatus == Spine::HTTP::Status::no_content ||
         theStatus == Spine::HTTP::Status::not_modified;
}

// Strict decimal, as Content-Length requires: anything a proxy and this
// frontend could read differently is treated as no length at all.
bool parseContentLength(const std::string& value, std::size_t& result)
{
  const std::string trimmed = boost::algorithm::trim_copy(value);
  if (trimmed.empty() || trimmed.size() > 19)
    return false;

  std::size_t length = 0;
  for (const char c : trimmed)
  {
    if (c < '0' || c > '9')
      return false;
    length = length * 10 + static_cast<std::size_t>(c - '0');
  }
  result = length;
  return true;
}

}  // namespace

LowLatencyGatewayStreamer::~LowLatencyGatewayStreamer()
{
  if (!itsFinishing)
    itsReactor.stopBackendRequest(itsHostName, itsPort);
  itsProxy->registerStreamerStop();
}

void LowLatencyGatewayStreamer::abort(const std::string& theReason)
{
  try
  {
    boost::unique_lock<boost::mutex> lock(itsMutex);

    if (itsGatewayStatus != GatewayStatus::ONGOING)
      return;  // Already finished or failed on its own, nothing to do

    std::cout << fmt::format("{} Aborting gateway stream to {}:{} ({}), response will be lost",
                             Spine::log_time_str(),
                             itsIP,
                             itsPort,
                             theReason)
              << std::endl;

    // Never cache a response we are truncating ourselves
    itsResponseIsCacheable = false;
    itsGatewayStatus = GatewayStatus::FAILED;

    boost::system::error_code ignored_error;
    itsBackendSocket.close(ignored_error);
    if (itsTimeoutTimer)
      itsTimeoutTimer->cancel();

    markFinishing();  // Remove backend communication from load balancing

    // Both waiters: a request thread may still be waiting for the head, and the
    // server's writer for the next chunk
    itsHeadReadyEvent.notify_all();
    itsDataAvailableEvent.notify_all();
  }
  catch (...)
  {
    Fmi::Exception ex(BCP, "LowLatencyGatewayStreamer::abort aborted", nullptr);
    ex.printError();
    // Must not throw or execution will terminate
  }
}

LowLatencyGatewayStreamer::LowLatencyGatewayStreamer(Private,
                                                     const std::shared_ptr<Proxy> theProxy,
                                                     Spine::Reactor& theReactor,
                                                     std::string theHostName,
                                                     std::string theIP,
                                                     unsigned short thePort,
                                                     int theBackendTimeoutInSeconds,
                                                     const Spine::HTTP::Request& theOriginalRequest)
    : itsOriginalRequest(theOriginalRequest),
      itsSocketBuffer(),
      itsHostName(std::move(theHostName)),
      itsIP(std::move(theIP)),
      itsPort(thePort),
      itsBackendSocket(theProxy->backendIoService),
      itsBackendTimeoutInSeconds(theBackendTimeoutInSeconds),
      itsProxy(theProxy),
      itsReactor(theReactor)
{
  itsReactor.startBackendRequest(itsHostName, itsPort);
}

// Factory method
std::shared_ptr<LowLatencyGatewayStreamer> LowLatencyGatewayStreamer::create(
    const std::shared_ptr<Proxy> theProxy,
    Spine::Reactor& theReactor,
    const std::string& theHostName,
    const std::string& theIP,
    unsigned short thePort,
    int theBackendTimeoutInSeconds,
    const Spine::HTTP::Request& theOriginalRequest)
{
  auto streamer = std::make_shared<LowLatencyGatewayStreamer>(Private(),
                                                              theProxy,
                                                              theReactor,
                                                              theHostName,
                                                              theIP,
                                                              thePort,
                                                              theBackendTimeoutInSeconds,
                                                              theOriginalRequest);
  theProxy->registerStreamerStart(streamer);
  return streamer;
}

// Mark the communication almost finished for load balancing purposes
void LowLatencyGatewayStreamer::markFinishing()
{
  if (!itsFinishing)
  {
    itsFinishing = true;
    itsReactor.stopBackendRequest(itsHostName, itsPort);
  }
}

// Open a connection to the backend, an idle pooled one for preference
bool LowLatencyGatewayStreamer::openBackendConnection(bool theAllowPool)
{
  try
  {
    boost::system::error_code err;
    itsBackendSocket.close(err);

    itsConnectionFromPool = theAllowPool && itsProxy->getBackendConnectionPool().acquire(
                                                itsIP, itsPort, itsBackendSocket);

    if (!itsConnectionFromPool)
    {
      ip::tcp::endpoint theEnd(boost::asio::ip::make_address(itsIP), itsPort);
      itsBackendSocket.connect(theEnd, err);

      if (!!err)
      {
        std::cout << fmt::format("{} Backend connection to {} failed with message '{}'",
                                 Spine::log_time_str(),
                                 itsIP,
                                 err.message())
                  << std::endl;
        return false;
      }

      itsProxy->getBackendConnectionPool().recordFreshConnection();
    }

    // We have determined that this option significantly improves frontend latency.
    // A pooled connection already carries it; setting it again is harmless.
    itsBackendSocket.set_option(ip::tcp::no_delay(true), err);

    return true;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

bool LowLatencyGatewayStreamer::connectAndSend(const std::string& theContent, bool theAllowPool)
{
  try
  {
    // Kept so the request can be sent again if the connection turns out to have
    // been closed by the backend while it sat idle in the pool
    itsSentRequest = theContent;
    itsAnyResponseBytes = false;
    itsResponseHeaderBuffer.clear();

    bool allowPool = theAllowPool;

    for (int attempt = 0; attempt < 2; ++attempt)
    {
      if (!openBackendConnection(allowPool))
        return false;  // Connecting itself failed: the backend is unreachable

      boost::system::error_code err;
      boost::asio::write(itsBackendSocket, boost::asio::buffer(itsSentRequest), err);

      if (!err)
        return true;

      if (!itsConnectionFromPool)
      {
        std::cout << fmt::format("{} Backend write to {} failed with message '{}'",
                                 Spine::log_time_str(),
                                 itsIP,
                                 err.message())
                  << std::endl;
        return false;
      }

      // A pooled connection the backend had already closed. Nothing of the
      // request reached it, so sending it again is safe.
      boost::system::error_code ignored;
      itsBackendSocket.close(ignored);
      itsReplayed = true;
      itsProxy->getBackendConnectionPool().recordReplay();
      allowPool = false;
    }

    return false;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

bool LowLatencyGatewayStreamer::retryOnFreshConnection(const boost::system::error_code& theError)
{
  try
  {
    // Only a reused connection that died before answering is worth replaying.
    // Once a byte of the response has arrived the failure is the backend's, and
    // sending the request twice could mean doing its work twice.
    if (!itsConnectionFromPool || itsReplayed || itsAnyResponseBytes)
      return false;
    if (itsSentRequest.empty() || !isDroppedConnection(theError))
      return false;

    itsReplayed = true;
    itsProxy->getBackendConnectionPool().recordReplay();

    boost::system::error_code err;
    itsBackendSocket.close(err);

    ip::tcp::endpoint theEnd(boost::asio::ip::make_address(itsIP), itsPort);
    itsBackendSocket.connect(theEnd, err);
    if (!!err)
    {
      std::cout << fmt::format("{} Backend reconnection to {} failed with message '{}'",
                               Spine::log_time_str(),
                               itsIP,
                               err.message())
                << std::endl;
      return false;
    }

    itsConnectionFromPool = false;
    itsProxy->getBackendConnectionPool().recordFreshConnection();
    itsBackendSocket.set_option(ip::tcp::no_delay(true), err);

    boost::asio::write(itsBackendSocket, boost::asio::buffer(itsSentRequest), err);
    if (!!err)
    {
      std::cout << fmt::format("{} Backend write to {} failed with message '{}'",
                               Spine::log_time_str(),
                               itsIP,
                               err.message())
                << std::endl;
      return false;
    }

    itsResponseHeaderBuffer.clear();

    std::shared_ptr<LowLatencyGatewayStreamer> me = shared_from_this();
    if (itsExchange == Exchange::ETAG_PROBE)
      itsBackendSocket.async_read_some(
          boost::asio::buffer(itsSocketBuffer),
          [me](const boost::system::error_code& err, std::size_t bytes_transferred)
          { me->readCacheResponse(err, bytes_transferred); });
    else
      itsBackendSocket.async_read_some(
          boost::asio::buffer(itsSocketBuffer),
          [me](const boost::system::error_code& err, std::size_t bytes_transferred)
          { me->readDataResponseHeaders(err, bytes_transferred); });

    extendBackendDeadline();

    return true;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

// Begin backend communication
bool LowLatencyGatewayStreamer::sendAndListen()
{
  try
  {
    itsExchange = Exchange::ETAG_PROBE;

    // This header signals we query ETag from the backend
    itsOriginalRequest.setHeader("X-Request-ETag", "true");

    const std::string content = serialiseRequest(itsOriginalRequest);

    // Remove cache query header, it is no longer needed
    itsOriginalRequest.removeHeader("X-Request-ETag");

    if (!connectAndSend(content))
      return false;

    // Start the timeout timer
    itsTimeoutTimer = std::make_shared<DeadlineTimer>(itsProxy->backendIoService);
    extendBackendDeadline();
    armTimeoutTimer();

    // Start to listen for the reply, headers not yet received
    std::shared_ptr<LowLatencyGatewayStreamer> me = shared_from_this();
    itsBackendSocket.async_read_some(
        boost::asio::buffer(itsSocketBuffer),
        [me](const boost::system::error_code& err, std::size_t bytes_transferred)
        { me->readCacheResponse(err, bytes_transferred); });

    return true;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

bool LowLatencyGatewayStreamer::publishResponseHead(const Spine::HTTP::Response& theHead,
                                                    const std::string& theBodySoFar)
{
  auto head = std::make_unique<Spine::HTTP::Response>(theHead);

  // Work out how the backend delimits its body before the framing fields are
  // taken out of what the client will see.
  auto transferEncoding = head->getHeader("Transfer-Encoding");
  auto contentLength = head->getHeader("Content-Length");

  if (statusHasNoBody(head->getStatus()))
  {
    // No body at all, and no header field saying so. Treating this as a
    // zero-length body ends the exchange here instead of waiting for a close
    // that a persistent backend connection is never going to bring.
    itsBodyFraming = BodyFraming::LENGTH;
    itsDeclaredBodyLength = 0;
  }
  else if (transferEncoding)
  {
    itsBodyFraming = BodyFraming::CHUNKED;
  }
  else if (contentLength && parseContentLength(*contentLength, itsDeclaredBodyLength))
  {
    itsBodyFraming = BodyFraming::LENGTH;
  }
  else
  {
    // No usable framing header: the body ends when the backend closes. The
    // client response is framed independently (the server sends it chunked), so
    // the client connection survives a backend connection that cannot.
    itsBodyFraming = BodyFraming::UNTIL_CLOSE;
  }

  // Read before the Connection field is stripped: whether this connection may
  // serve another request is the last thing that field is good for here.
  itsBackendMayPersist = backendAllowsReuse(theHead);

  stripHopByHopHeaders(*head);

  // The frontend frames the client response itself and may not frame it the
  // same way, so the backend's framing fields must not be forwarded.
  head->removeHeader("Content-Length");
  head->removeHeader("Transfer-Encoding");

  itsBackendHead = std::move(head);
  itsHeadReady = true;

  // Fed even when nothing followed the head, so that a response declaring no
  // body at all is recognised as complete instead of waiting for a close.
  bool finished = consumeBodyBytes(theBodySoFar.data(), theBodySoFar.size());
  if (finished)
    finishBackendResponse();
  else if (itsGatewayStatus == GatewayStatus::FAILED)
    finished = true;  // The framing was rejected; there is nothing left to read

  itsHeadReadyEvent.notify_all();

  return finished;
}

void LowLatencyGatewayStreamer::appendBodyBytes(const char* data, std::size_t length)
{
  if (length == 0)
    return;

  itsClientDataBuffer.append(data, length);
  itsBodyBytesDecoded += length;

  if (itsResponseIsCacheable)
  {
    itsCachedContent.append(data, length);
    if (itsCachedContent.size() > proxy_max_cached_buffer_size)
    {
      // Overflow, do not cache this response
      itsResponseIsCacheable = false;
      itsCachedContent.clear();
    }
  }
}

bool LowLatencyGatewayStreamer::consumeBodyBytes(const char* data, std::size_t length)
{
  switch (itsBodyFraming)
  {
    case BodyFraming::LENGTH:
    {
      // Never hand on more than was announced: anything beyond the declared
      // length belongs to the next response on that backend connection.
      const std::size_t remaining = itsDeclaredBodyLength - itsBodyBytesDecoded;
      if (length > remaining)
      {
        // Where the next message on this connection would begin is now a guess,
        // so the connection cannot be handed on
        itsBodyOverrun = true;
      }
      appendBodyBytes(data, std::min(remaining, length));
      itsBodyComplete = itsBodyBytesDecoded >= itsDeclaredBodyLength;
      return itsBodyComplete;
    }

    case BodyFraming::CHUNKED:
    {
      std::string decoded;
      const auto status = itsChunkDecoder.feed(data, length, decoded);
      appendBodyBytes(decoded.data(), decoded.size());

      if (status == ChunkedBodyDecoder::Status::FAILED)
      {
        std::cout << fmt::format("{} Backend at {}:{} sent a malformed chunked body",
                                 Spine::log_time_str(),
                                 itsIP,
                                 itsPort)
                  << std::endl;
        itsResponseIsCacheable = false;
        itsGatewayStatus = GatewayStatus::FAILED;
        return false;
      }

      itsBodyComplete = (status == ChunkedBodyDecoder::Status::COMPLETE);
      return itsBodyComplete;
    }

    case BodyFraming::UNTIL_CLOSE:
    default:
      appendBodyBytes(data, length);
      return false;  // Only the backend closing can end this body
  }
}

void LowLatencyGatewayStreamer::storeInCacheIfEligible()
{
  // Call caching functionality here using the backend buffering thread
  // We do not want to accidentally block any server threads
  if (itsResponseIsCacheable && !itsCachedContent.empty() && !itsHasTimedOut)
  {
    auto& cache = itsProxy->getCache();
    cache.insertCachedBuffer(itsBackendMetadata.etag,
                             itsBackendMetadata.mime_type,
                             itsBackendMetadata.cache_control,
                             itsBackendMetadata.expires,
                             itsBackendMetadata.vary,
                             itsBackendMetadata.access_control_allow_origin,
                             itsBackendMetadata.content_encoding,
                             std::make_shared<std::string>(itsCachedContent));
  }
}

bool LowLatencyGatewayStreamer::backendAllowsReuse(const Spine::HTTP::Response& theHead) const
{
  if (!itsProxy->getBackendConnectionPool().isEnabled())
    return false;

  auto connection = theHead.getHeader("Connection");
  const std::string field = connection ? *connection : std::string();

  // The two protocol versions say the opposite thing by default: HTTP/1.1 keeps
  // the connection unless told otherwise, HTTP/1.0 drops it unless asked.
  if (theHead.getVersion() == "1.0")
    return hasHeaderToken(field, "keep-alive");

  return !hasHeaderToken(field, "close");
}

bool LowLatencyGatewayStreamer::probeLeftCleanConnection(
    const Spine::HTTP::Response& theHead, std::string::const_iterator theBodyStart) const
{
  // Plugins answer the ETag probe with "204 No Content", so the head is the
  // whole message and the connection is left exactly at a message boundary.
  // Anything else - a body we are about to abandon, or bytes past the head -
  // and the request that follows would be read against the wrong offset.
  if (theBodyStart != itsResponseHeaderBuffer.cend())
    return false;

  const auto status = theHead.getStatus();
  const bool bodyless =
      (status == Spine::HTTP::Status::no_content || status == Spine::HTTP::Status::not_modified);

  if (!bodyless)
  {
    if (theHead.getHeader("Transfer-Encoding"))
      return false;

    auto length = theHead.getHeader("Content-Length");
    std::size_t declared = 0;
    if (!length || !parseContentLength(*length, declared) || declared != 0)
      return false;
  }

  return backendAllowsReuse(theHead);
}

void LowLatencyGatewayStreamer::releaseBackendConnection(bool theReusable)
{
  boost::system::error_code ignored_error;

  // A connection that timed out is not at a boundary we can trust, whatever the
  // framing said, since the timer fires on a connection that stopped delivering.
  if (theReusable && !itsHasTimedOut && itsBackendSocket.is_open())
    itsProxy->getBackendConnectionPool().release(itsIP, itsPort, std::move(itsBackendSocket));
  else
    itsBackendSocket.close(ignored_error);
}

void LowLatencyGatewayStreamer::finishBackendResponse()
{
  // The response is complete because its own framing says so, without waiting
  // for the backend to close the socket. That is what makes the backend
  // connection reusable: the socket sits at a message boundary, so the next
  // request can go out on it instead of paying for another handshake.
  storeInCacheIfEligible();

  itsGatewayStatus = GatewayStatus::FINISHED;

  const bool atMessageBoundary =
      !itsBodyOverrun &&
      (itsBodyFraming == BodyFraming::LENGTH ||
       (itsBodyFraming == BodyFraming::CHUNKED && itsChunkDecoder.pending() == 0));

  releaseBackendConnection(itsBackendMayPersist && atMessageBoundary);

  if (itsTimeoutTimer)
    itsTimeoutTimer->cancel();

  markFinishing();
}

const Spine::HTTP::Response* LowLatencyGatewayStreamer::waitForResponseHead()
{
  try
  {
    boost::unique_lock<boost::mutex> lock(itsMutex);

    // Bounded by the backend timeout, which the read handlers keep pushing back
    // while data is flowing. A backend that goes quiet is caught by
    // handleTimeout(), which sets FAILED and wakes this wait.
    while (!itsHeadReady && itsGatewayStatus == GatewayStatus::ONGOING)
      itsHeadReadyEvent.timed_wait(lock, boost::posix_time::milliseconds(100));

    if (itsCompleteResponse)
      return itsCompleteResponse.get();

    return itsBackendHead.get();
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

const Spine::HTTP::Response* LowLatencyGatewayStreamer::getCompleteResponse() const
{
  return itsCompleteResponse.get();
}

// This function is called by the server when it can send more data
std::string LowLatencyGatewayStreamer::getChunk()
{
  try
  {
    std::string returnedBuffer;
    boost::unique_lock<boost::mutex> lock(itsMutex);
    if (itsClientDataBuffer.empty())
    {
      switch (itsGatewayStatus)
      {
        case GatewayStatus::ONGOING:
          // Backend socket is open, but no data read. Slow connection to backend?
          // Do a timed wait on the condition variable
          itsDataAvailableEvent.timed_wait(lock, boost::posix_time::milliseconds(100));
          break;

        case GatewayStatus::FINISHED:
          setStatus(ContentStreamer::StreamerStatus::EXIT_OK);
          break;

        case GatewayStatus::FAILED:
          setStatus(ContentStreamer::StreamerStatus::EXIT_ERROR);
          break;
      }
    }

    returnedBuffer = itsClientDataBuffer;
    itsClientDataBuffer.clear();

    if (itsBackendBufferFull)
    {
      itsBackendBufferFull = false;

      // Backend buffer was full
      // Schedule new read from the socket now that we have extracted the buffer
      itsBackendSocket.async_read_some(
          boost::asio::buffer(itsSocketBuffer),
          [me = shared_from_this()](const boost::system::error_code& err,
                                    std::size_t bytes_transferred)
          { me->readDataResponse(err, bytes_transferred); });

      // Reset timeout timer. It was cancelled outright when the buffer filled up,
      // so this one has to be re-armed and not merely pushed back.
      extendBackendDeadline();
      armTimeoutTimer();
    }

    return returnedBuffer;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

void LowLatencyGatewayStreamer::readCacheResponse(const boost::system::error_code& error,
                                                  std::size_t bytes_transferred)
{
  try
  {
    boost::unique_lock<boost::mutex> lock(itsMutex);

    if (!!error)
    {
      if (retryOnFreshConnection(error))
        return;
      handleError(error);
      return;
    }

    if (bytes_transferred > 0)
      itsAnyResponseBytes = true;

    itsResponseHeaderBuffer.append(itsSocketBuffer.begin(), bytes_transferred);

    // Attempt to parse the response headers
    auto ret = Spine::HTTP::parseResponse(itsResponseHeaderBuffer);
    switch (std::get<0>(ret))
    {
      case Spine::HTTP::ParsingStatus::FAILED:
      {
        // Garbled response, handle error
        std::cout << fmt::format(
                         "{} Cache query to backend at {}:{} returned garbled response.\nQuery: "
                         "{}\nResponse buffer: {}",
                         Spine::log_time_str(),
                         itsIP,
                         itsPort,
                         itsOriginalRequest.getQueryString(),
                         itsResponseHeaderBuffer)
                  << std::endl;

        itsGatewayStatus = GatewayStatus::FAILED;

        break;
      }
      case Spine::HTTP::ParsingStatus::INCOMPLETE:
      {
        // Partial response, read more data

        itsBackendSocket.async_read_some(
            boost::asio::buffer(itsSocketBuffer),
            [me = shared_from_this()](const boost::system::error_code& err,
                                      std::size_t bytes_transferred)
            { me->readCacheResponse(err, bytes_transferred); });

        // Reset timeout timer
        extendBackendDeadline();

        break;
      }
      case Spine::HTTP::ParsingStatus::COMPLETE:
      {
        // Successfull parse.
        auto&& responsePtr = std::get<1>(ret);

        // See if backend responded with ETag
        auto etagHeader = responsePtr->getHeader("ETag");
        if (!etagHeader)
        {
          // Backend responded without the ETag-header, this plugin doesn't support frontend
          // caching. Pass response through as before

          itsResponseIsCacheable = false;

          const bool complete = publishResponseHead(
              *responsePtr, std::string(std::get<2>(ret), itsResponseHeaderBuffer.cend()));

          if (!complete)
          {
            // Go to data response loop
            itsBackendSocket.async_read_some(
                boost::asio::buffer(itsSocketBuffer),
                [me = shared_from_this()](const boost::system::error_code& err,
                                          std::size_t bytes_transferred)
                { me->readDataResponse(err, bytes_transferred); });

            // Reset timeout timer
            extendBackendDeadline();
          }

          markFinishing();  // Remove backend communication from load balancing

          itsDataAvailableEvent.notify_one();  // Tell consumer thread to proceed
        }
        else
        {
          std::string etag = *etagHeader;

          // The probe is normally answered with a bodyless "204 No Content", so
          // the connection is at a message boundary and can carry whatever comes
          // next - the content request on a cache miss, or another request
          // entirely on a cache hit.
          itsProbeConnectionReusable = probeLeftCleanConnection(*responsePtr, std::get<2>(ret));

          // See if we should send a content-encoded response
          auto accepted_content_type = clientAcceptsContentEncoding(itsOriginalRequest);

          auto& cache = itsProxy->getCache();

          // Try the client's preferred encoding first
          auto result = cache.getCachedBuffer(etag, accepted_content_type);

          // Fall back to the identity (uncompressed) representation if there is no encoded match
          if (!result.first && !accepted_content_type.empty())
            result = cache.getCachedBuffer(etag, "");

          if (!result.first)
          {
            // No match from either cache, request the data
            sendContentRequest();
            return;
          }

          // Found from the buffer cache

          // Make sure cached responses are not re-cached
          itsResponseIsCacheable = false;

          auto metadata = result.second;
          auto response_buffer = result.first;

          // Note: The back end may update expiration times in its "not modified" responses. Hence
          // we must update the cached response too. Note that we do not modify the cached object
          // itself, only this particular response. We do not expect plugins to modify
          // their cache_control flags, since we expect plugins to use Expires instead
          // of Cache-Control: max-age

          auto expiresHeader = responsePtr->getHeader("Expires");
          if (expiresHeader)
            metadata.expires = *expiresHeader;

          // A cache hit needs no backend body at all, so the whole response is
          // handed over as one object and the caller answers it as an ordinary
          // buffered response - framed, compressible and safe to keep the
          // client connection for, exactly like any other plugin response.
          itsCompleteResponse = std::make_unique<Spine::HTTP::Response>(
              buildCacheResponse(itsOriginalRequest, response_buffer, metadata));
          itsHeadReady = true;
          itsHeadReadyEvent.notify_all();

          itsGatewayStatus =
              GatewayStatus::FINISHED;  // Entire response content generated, we are done!

          // ASIO doesn't know the backend conversation is finished, so the socket
          // has to be dealt with explicitly here or it leaks. A probe that left
          // the connection at a message boundary is worth keeping: the response
          // came from our own cache, so the backend connection is untouched and
          // ready for the next request.
          releaseBackendConnection(itsProbeConnectionReusable);

          markFinishing();  // Remove backend communication from load balancing

          itsDataAvailableEvent.notify_one();  // Tell consumer thread to proceed
        }
      }
      break;
    }
  }
  catch (...)
  {
    Fmi::Exception ex(BCP, "LowLatencyGatewayStreamer::readCacheResponse aborted", nullptr);
    ex.printError();
    // Must not throw or execution will terminate
  }
}

void LowLatencyGatewayStreamer::sendContentRequest()
{
  try
  {
    // A second exchange on its own connection, so it gets its own replay budget
    itsExchange = Exchange::CONTENT;
    itsReplayed = false;
    bool allowPool = true;

    // Clear buffers just in case.
    itsClientDataBuffer.clear();
    itsResponseHeaderBuffer.clear();
    itsCachedContent.clear();

    const std::string buffer = serialiseRequest(itsOriginalRequest);

    if (itsProbeConnectionReusable)
    {
      // The ETag probe was answered without a body, so its connection is at a
      // message boundary and the content request goes out on it directly. This
      // is what removes the second TCP handshake a cache miss used to cost.
      itsSentRequest = buffer;
      itsAnyResponseBytes = false;
      itsConnectionFromPool = true;  // Reused, so worth one replay if it is dead
      itsProxy->getBackendConnectionPool().recordDirectReuse();

      boost::system::error_code err;
      boost::asio::write(itsBackendSocket, boost::asio::buffer(itsSentRequest), err);

      if (!err)
      {
        itsBackendSocket.async_read_some(
            boost::asio::buffer(itsSocketBuffer),
            [me = shared_from_this()](const boost::system::error_code& err,
                                      std::size_t bytes_transferred)
            { me->readDataResponseHeaders(err, bytes_transferred); });

        extendBackendDeadline();
        return;
      }

      // The backend closed the probe connection after answering it after all.
      // Nothing of the request got through, so it is safe to send again - but on
      // a connection we know is new. Taking another pooled one could lose the
      // same race a second time, and the replay budget is now spent.
      itsReplayed = true;
      allowPool = false;
    }

    if (!connectAndSend(buffer, allowPool))
    {
      itsGatewayStatus = GatewayStatus::FAILED;
      return;
    }

    // Start to listen for the reply, headers not yet received
    itsBackendSocket.async_read_some(boost::asio::buffer(itsSocketBuffer),
                                     [me = shared_from_this()](const boost::system::error_code& err,
                                                               std::size_t bytes_transferred)
                                     { me->readDataResponseHeaders(err, bytes_transferred); });

    // Reset timeout timer
    extendBackendDeadline();
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

void LowLatencyGatewayStreamer::readDataResponseHeaders(const boost::system::error_code& error,
                                                        std::size_t bytes_transferred)
{
  try
  {
    // Parse response headers for possible cache insertion
    boost::unique_lock<boost::mutex> lock(itsMutex);

    if (!!error)
    {
      if (retryOnFreshConnection(error))
        return;
      handleError(error);
      return;
    }

    if (bytes_transferred > 0)
      itsAnyResponseBytes = true;

    itsResponseHeaderBuffer.append(itsSocketBuffer.begin(), bytes_transferred);

    auto ret = Spine::HTTP::parseResponse(itsResponseHeaderBuffer);
    switch (std::get<0>(ret))
    {
      case Spine::HTTP::ParsingStatus::FAILED:
      {
        // Garbled response, handle error
        std::cout << fmt::format("{} Data query to backend at {}:{} returned garbled response",
                                 Spine::log_time_str(),
                                 itsIP,
                                 itsPort)
                  << std::endl;
        itsGatewayStatus = GatewayStatus::FAILED;
        return;
      }

      case Spine::HTTP::ParsingStatus::INCOMPLETE:
      {
        // Partial response, read more data

        itsBackendSocket.async_read_some(
            boost::asio::buffer(itsSocketBuffer),
            [me = shared_from_this()](const boost::system::error_code& err,
                                      std::size_t bytes_transferred)
            { me->readDataResponseHeaders(err, bytes_transferred); });

        // Reset timeout timer
        extendBackendDeadline();

        return;
      }
      case Spine::HTTP::ParsingStatus::COMPLETE:
      {
        // Headers parsed, determine if we should attempt cache insertion
        auto&& responsePtr = std::get<1>(ret);

        auto etag = responsePtr->getHeader("ETag");

        if (etag)
        {
          // ETag received, this response may be cacheable

          // Determine cacheability
          auto mime = responsePtr->getHeader("Content-Type");
          auto transfer_encoding = responsePtr->getHeader("Transfer-Encoding");
          auto status = responsePtr->getStatus();

          if (!mime || transfer_encoding || status != Spine::HTTP::Status::ok)
          {
            // No MIME, or has transfer-encoding.
            // MIME is required, and transfer-encoded responses are typically large (and not
            // necessarily supported by clients). Do not cache these
            // Also, do not cache non-ok responses
            itsResponseIsCacheable = false;
          }
          else
          {
            // Cacheable response, build cache metadata and store it for later use when writing to
            // the cache
            itsBackendMetadata = build_metadata(*responsePtr);
          }
        }
        else
        {
          // No ETag, response is not cacheable
          itsResponseIsCacheable = false;
        }

        const bool complete = publishResponseHead(
            *responsePtr, std::string(std::get<2>(ret), itsResponseHeaderBuffer.cend()));

        if (!complete)
        {
          itsBackendSocket.async_read_some(
              boost::asio::buffer(itsSocketBuffer),
              [me = shared_from_this()](const boost::system::error_code& err,
                                        std::size_t bytes_transferred)
              { me->readDataResponse(err, bytes_transferred); });

          // Reset timeout timer
          extendBackendDeadline();
        }

        itsDataAvailableEvent.notify_one();  // Tell consumer thread to proceed

        break;
      }
    }
  }
  catch (...)
  {
    Fmi::Exception ex(BCP, "LowLatencyGatewayStreamer::readDataResponseHeaders aborted", nullptr);
    ex.printError();
    // Must not throw or execution will terminate
  }
}

void LowLatencyGatewayStreamer::readDataResponse(const boost::system::error_code& error,
                                                 std::size_t bytes_transferred)
{
  try
  {
    boost::unique_lock<boost::mutex> lock(itsMutex);

    if (!!error)
    {
      handleError(error);
    }
    else
    {
      if (bytes_transferred > 0)
        itsAnyResponseBytes = true;

      if (consumeBodyBytes(itsSocketBuffer.data(), bytes_transferred))
      {
        // The body's own framing says it is complete, so there is no need to
        // wait for the backend to close the socket.
        finishBackendResponse();
        itsDataAvailableEvent.notify_one();
        return;
      }

      if (itsGatewayStatus == GatewayStatus::FAILED)
      {
        // The decoder rejected the backend's framing
        itsDataAvailableEvent.notify_one();
        return;
      }

      if (itsClientDataBuffer.size() > proxy_max_buffer_size)
      {
        // Too much data in buffer
        // Signal the consumer thread to schedule the next read when buffer is extracted
        itsBackendBufferFull = true;
        itsTimeoutTimer->cancel();
        return;
      }
      // Go back to listen the socket
      itsBackendSocket.async_read_some(
          boost::asio::buffer(itsSocketBuffer),
          [me = shared_from_this()](const boost::system::error_code& err,
                                    std::size_t bytes_transferred)
          { me->readDataResponse(err, bytes_transferred); });

      // Reset timeout timer
      extendBackendDeadline();
    }

    itsDataAvailableEvent.notify_one();  // Tell consumer thread to proceed
  }
  catch (...)
  {
    Fmi::Exception ex(BCP, "LowLatencyGatewayStreamer::readDataResponse aborted", nullptr);
    ex.printError();
    // Must not throw or execution will terminate
  }
}

void LowLatencyGatewayStreamer::extendBackendDeadline()
{
  itsDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(itsBackendTimeoutInSeconds);
}

void LowLatencyGatewayStreamer::armTimeoutTimer()
{
  itsTimeoutTimer->expires_at(itsDeadline);
  itsTimeoutTimer->async_wait([me = shared_from_this()](const boost::system::error_code& err)
                              { me->handleTimeout(err); });
}

// ----------------------------------------------------------------------
/*!
 * \brief The backend has gone quiet for longer than it is allowed to
 *
 * This has to end the exchange itself, not just make a note of it. Both waiters
 * - the request thread in waitForResponseHead(), and the server's writer in
 * getChunk() - loop while the status says ONGOING, so a backend that accepts a
 * connection and then says nothing would otherwise hold a request thread for as
 * long as the process lives.
 */
// ----------------------------------------------------------------------

void LowLatencyGatewayStreamer::handleTimeout(const boost::system::error_code& err)
{
  try
  {
    boost::unique_lock<boost::mutex> lock(itsMutex);

    if (err == boost::asio::error::operation_aborted)
      return;  // Cancelled because the exchange finished, or re-armed under us

    if (itsGatewayStatus != GatewayStatus::ONGOING)
      return;  // Finished between the timer firing and this handler running

    if (std::chrono::steady_clock::now() < itsDeadline)
    {
      // The backend has been heard from since this wait was armed, so the
      // deadline moved. Wait for the new one instead of firing.
      armTimeoutTimer();
      return;
    }

    std::cout << fmt::format("{} Connection to backend at {}:{} timed out in {} seconds",
                             Spine::log_time_str(),
                             itsIP,
                             itsPort,
                             itsBackendTimeoutInSeconds)
              << std::endl;

    itsHasTimedOut = true;
    itsResponseIsCacheable = false;
    itsGatewayStatus = GatewayStatus::FAILED;

    // Closing cancels whatever read is outstanding. Its handler will see
    // operation_aborted and, finding itsHasTimedOut set, stop there.
    boost::system::error_code ignored_error;
    itsBackendSocket.close(ignored_error);

    markFinishing();  // Remove backend communication from load balancing

    itsHeadReadyEvent.notify_all();
    itsDataAvailableEvent.notify_all();
  }
  catch (...)
  {
    Fmi::Exception ex(BCP, "LowLatencyGatewayStreamer::handleTimeout aborted", nullptr);
    ex.printError();
    // Must not throw or execution will terminate
  }
}

void LowLatencyGatewayStreamer::handleError(const boost::system::error_code& err)
{
  try
  {
    // Socket has been closed or is borked
    if (err == boost::asio::error::eof)
    {
      if (itsHeadReady && itsBodyFraming != BodyFraming::UNTIL_CLOSE && !itsBodyComplete)
      {
        // The backend announced a length, or chunked framing, and then closed
        // before delivering it. Previously EOF always meant success, so a
        // truncated response was cached and passed off to the client as
        // complete.
        std::cout << fmt::format("{} Backend at {}:{} closed before its response body was complete",
                                 Spine::log_time_str(),
                                 itsIP,
                                 itsPort)
                  << std::endl;

        itsResponseIsCacheable = false;
        itsGatewayStatus = GatewayStatus::FAILED;
      }
      else
      {
        // Clean shutdown
        storeInCacheIfEligible();
        itsGatewayStatus = GatewayStatus::FINISHED;
      }
    }
    else if (err == boost::asio::error::operation_aborted)
    {
      // The read was cancelled. Either handleTimeout() closed the socket - in
      // which case it has already reported the timeout and failed the stream -
      // or the client disconnected and this connection is destructing, where the
      // gateway status no longer matters.
    }
    else
    {
      std::cout << fmt::format(
                       "{} Connection to backend at {}:{} abnormally terminated. Reason: '{}'",
                       Spine::log_time_str(),
                       itsIP,
                       itsPort,
                       err.message())
                << std::endl;

      itsGatewayStatus = GatewayStatus::FAILED;
    }

    itsTimeoutTimer->cancel();
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

}  // namespace SmartMet
