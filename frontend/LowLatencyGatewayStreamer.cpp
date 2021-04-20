#include "LowLatencyGatewayStreamer.h"
#include "Proxy.h"
#include <macgyver/StringConversion.h>
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
    return Fmi::to_http_string(boost::posix_time::second_clock::universal_time());
  }
  catch (...)
  {
    throw Fmi::Exception(BCP, "Failed to build HTTP response date");
  }
}

std::string contentEnumToString(ResponseCache::ContentEncodingType type)
{
  try
  {
    switch (type)
    {
      case ResponseCache::ContentEncodingType::GZIP:
        return "gzip";
      case ResponseCache::ContentEncodingType::NONE:
      default:
        return "";
    }
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

// Return the most applicable content encoding for this request
ResponseCache::ContentEncodingType clientAcceptsContentEncoding(const Spine::HTTP::Request& request)
{
  try
  {
    auto accept_encoding = request.getHeader("Accept-Encoding");
    if (accept_encoding)
    {
      if (*accept_encoding == "*")
        return ResponseCache::ContentEncodingType::GZIP;  // Accepts everything, send zipped

      if (boost::algorithm::contains(*accept_encoding, "gzip"))
        return ResponseCache::ContentEncodingType::GZIP;
      return ResponseCache::ContentEncodingType::NONE;
    }
    return ResponseCache::ContentEncodingType::NONE;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

Spine::HTTP::Response buildCacheResponse(const Spine::HTTP::Request& originalRequest,
                                         const boost::shared_ptr<std::string>& cachedBuffer,
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

    // If client sent If-Modified-Since or If-None-Match - headers, respond with Not Modified.

    auto if_none_match = originalRequest.getHeader("If-None-Match");
    auto if_modified_since = originalRequest.getHeader("If-Modified-Since");

    // This block prepares the client response
    if ((if_none_match && *if_none_match == metadata.etag) || if_modified_since)
    {
      response.setStatus(Spine::HTTP::Status::not_modified);
    }
    else
    {
      // No If-None_match, If-Modified-Since or ETag mismatched

      response.setHeader("Content-Type", metadata.mime_type);
      if (metadata.content_encoding != ResponseCache::ContentEncodingType::NONE)
        response.setHeader("Content-Encoding", contentEnumToString(metadata.content_encoding));
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

}  // namespace

LowLatencyGatewayStreamer::~LowLatencyGatewayStreamer()
{
  itsReactor.stopBackendRequest(itsHostName, itsPort);
}

LowLatencyGatewayStreamer::LowLatencyGatewayStreamer(const boost::shared_ptr<Proxy>& theProxy,
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
}

// Begin backend communication
bool LowLatencyGatewayStreamer::sendAndListen()
{
  try
  {
    ip::tcp::endpoint theEnd(boost::asio::ip::address::from_string(itsIP), itsPort);
    boost::system::error_code err;
    itsBackendSocket.connect(theEnd, err);

    if (!!err)
    {
      std::cout << boost::posix_time::second_clock::local_time() << " Backend connection to "
                << itsIP << " failed with message '" << err.message() << "'" << std::endl;

      return false;
    }

    // We have determined that this option significantly improves frontend latency
    boost::asio::ip::tcp::no_delay no_delay_option(true);
    itsBackendSocket.set_option(no_delay_option);

    // Attempt to write to the socket

    // This header signals we query ETag from the backend
    itsOriginalRequest.setHeader("X-Request-ETag", "true");

    std::string content = itsOriginalRequest.toString();
    boost::asio::write(itsBackendSocket, boost::asio::buffer(content), err);
    if (!!err)
    {
      std::cout << boost::posix_time::second_clock::local_time() << " Backend write to " << itsIP
                << " failed with message '" << err.message() << "'" << std::endl;

      return false;
    }

    // Remove cache query header, it is no longer needed
    itsOriginalRequest.removeHeader("X-Request-ETag");

    // Start the timeout timer
    itsTimeoutTimer = boost::make_shared<boost::asio::deadline_timer>(
        itsProxy->backendIoService, boost::posix_time::seconds(itsBackendTimeoutInSeconds));

    itsTimeoutTimer->async_wait([me = shared_from_this()](const boost::system::error_code& err)
                                { me->handleTimeout(err); });

    // Start to listen for the reply, headers not yet received
    itsBackendSocket.async_read_some(boost::asio::buffer(itsSocketBuffer),
                                     [me = shared_from_this()](const boost::system::error_code& err,
                                                               std::size_t bytes_transferred)
                                     { me->readCacheResponse(err, bytes_transferred); });

    return true;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
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

      // Reset timeout timer
      itsTimeoutTimer->expires_from_now(boost::posix_time::seconds(itsBackendTimeoutInSeconds));
    }

    return returnedBuffer;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

std::string LowLatencyGatewayStreamer::getPeekString(int pos, int len)
{
  try
  {
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

    if (itsClientDataBuffer.empty())
      return "";

    return itsClientDataBuffer.substr(pos, len);
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
      handleError(error);
      return;
    }

    itsResponseHeaderBuffer.append(itsSocketBuffer.begin(), bytes_transferred);

    // Attempt to parse the response headers
    auto ret = Spine::HTTP::parseResponse(itsResponseHeaderBuffer);
    switch (std::get<0>(ret))
    {
      case Spine::HTTP::ParsingStatus::FAILED:
      {
        // Garbled response, handle error
        std::cout << boost::posix_time::second_clock::local_time() << " Cache query to backend at "
                  << itsIP << ":" << itsPort << " returned garbled response." << std::endl
                  << "Query: " << std::endl
                  << itsOriginalRequest.getQueryString() << std::endl
                  << "Response buffer: " << std::endl
                  << itsResponseHeaderBuffer << std::endl;

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
        itsTimeoutTimer->expires_from_now(boost::posix_time::seconds(itsBackendTimeoutInSeconds));

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

          itsClientDataBuffer = itsResponseHeaderBuffer;

          // Go to data response loop
          itsBackendSocket.async_read_some(
              boost::asio::buffer(itsSocketBuffer),
              [me = shared_from_this()](const boost::system::error_code& err,
                                        std::size_t bytes_transferred)
              { me->readDataResponse(err, bytes_transferred); });

          // Reset timeout timer
          itsTimeoutTimer->expires_from_now(boost::posix_time::seconds(itsBackendTimeoutInSeconds));

          itsDataAvailableEvent.notify_one();  // Tell consumer thread to proceed
        }
        else
        {
          std::string etag = *etagHeader;

          // See if we should send content-encoded response
          auto accepted_content_type = clientAcceptsContentEncoding(itsOriginalRequest);

          std::pair<boost::shared_ptr<std::string>, ResponseCache::CachedResponseMetaData> result;

          // Try compressed cache first if allowed
          if (accepted_content_type == ResponseCache::ContentEncodingType::GZIP)
          {
            auto& c_cache = itsProxy->getCache(ResponseCache::ContentEncodingType::GZIP);
            result = c_cache.getCachedBuffer(etag);
          }

          // Next try uncompressed cache if we found no compressed match
          if (!result.first)
          {
            auto& u_cache = itsProxy->getCache(ResponseCache::ContentEncodingType::NONE);
            result = u_cache.getCachedBuffer(etag);
          }

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

          auto clientResponse = buildCacheResponse(itsOriginalRequest, response_buffer, metadata);

          itsClientDataBuffer = clientResponse.toString();

          itsGatewayStatus =
              GatewayStatus::FINISHED;  // Entire response content generated, we are done!

          // Explicitly close the socket here, since ASIO doesn't know the backend conversation is
          // finished
          // Backend socket will leak without this
          boost::system::error_code ignored_error;
          itsBackendSocket.close(ignored_error);

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
    // Close the socket since we make a new connection to the backend
    // SmartMet doesn't currently support request pipelining
    boost::system::error_code err;
    itsBackendSocket.close(err);

    // Clear buffers just in case.
    itsClientDataBuffer.clear();
    itsResponseHeaderBuffer.clear();
    itsCachedContent.clear();

    ip::tcp::endpoint theEnd(boost::asio::ip::address::from_string(itsIP), itsPort);

    itsBackendSocket.connect(theEnd, err);

    if (!!err)
    {
      std::cout << boost::posix_time::second_clock::local_time() << " Backend connection to "
                << itsIP << " failed with message '" << err.message() << "'" << std::endl;

      itsGatewayStatus = GatewayStatus::FAILED;

      return;
    }

    std::string buffer = itsOriginalRequest.toString();

    boost::asio::write(itsBackendSocket, boost::asio::buffer(buffer), err);

    if (!err)
    {
      // Start to listen for the reply, headers not yet received
      itsBackendSocket.async_read_some(
          boost::asio::buffer(itsSocketBuffer),
          [me = shared_from_this()](const boost::system::error_code& err,
                                    std::size_t bytes_transferred)
          { me->readDataResponseHeaders(err, bytes_transferred); });

      // Reset timeout timer
      itsTimeoutTimer->expires_from_now(boost::posix_time::seconds(itsBackendTimeoutInSeconds));
    }
    else
    {
      std::cout << boost::posix_time::second_clock::local_time() << " Backend write to " << itsIP
                << " failed with message '" << err.message() << "'" << std::endl;

      itsGatewayStatus = GatewayStatus::FAILED;
    }
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
      handleError(error);
      return;
    }

    itsResponseHeaderBuffer.append(itsSocketBuffer.begin(), bytes_transferred);

    auto ret = Spine::HTTP::parseResponse(itsResponseHeaderBuffer);
    switch (std::get<0>(ret))
    {
      case Spine::HTTP::ParsingStatus::FAILED:
      {
        // Garbled response, handle error
        std::cout << boost::posix_time::second_clock::local_time() << " Data query to backend at "
                  << itsIP << ":" << itsPort << " return garbled response" << std::endl;

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
        itsTimeoutTimer->expires_from_now(boost::posix_time::seconds(itsBackendTimeoutInSeconds));

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
            // Cacheable response, build cache metadata

            ResponseCache::CachedResponseMetaData meta;
            meta.mime_type = *mime;
            meta.etag = *etag;

            auto expires = responsePtr->getHeader("Expires");
            if (expires)
              meta.expires = *expires;

            auto cache_control = responsePtr->getHeader("Cache-Control");
            if (cache_control)
              meta.cache_control = *cache_control;

            auto vary = responsePtr->getHeader("Vary");
            if (vary)
              meta.vary = *vary;

            auto access_control_allow_origin =
                responsePtr->getHeader("Access-Control-Allow-Origin");
            if (access_control_allow_origin)
              meta.access_control_allow_origin = *access_control_allow_origin;

            meta.content_encoding = ResponseCache::ContentEncodingType::NONE;
            auto content_encoding = responsePtr->getHeader("Content-Encoding");
            if (content_encoding && boost::algorithm::contains(*content_encoding, "gzip"))
              meta.content_encoding = ResponseCache::ContentEncodingType::GZIP;

            // Store for later use when writing to cache
            itsBackendMetadata = meta;

            auto parse_end_iter = std::get<2>(ret);

            std::string bodyThusFar = std::string(parse_end_iter, itsResponseHeaderBuffer.cend());

            // Content to be cached is stored separately from the entire stream
            itsCachedContent = bodyThusFar;
          }

          // This data is ready to be sent to client
          itsClientDataBuffer = itsResponseHeaderBuffer;

          itsBackendSocket.async_read_some(
              boost::asio::buffer(itsSocketBuffer),
              [me = shared_from_this()](const boost::system::error_code& err,
                                        std::size_t bytes_transferred)
              { me->readDataResponse(err, bytes_transferred); });
        }
        else
        {
          // No ETag, response is not cacheable
          itsResponseIsCacheable = false;

          itsClientDataBuffer = itsResponseHeaderBuffer;
          itsBackendSocket.async_read_some(
              boost::asio::buffer(itsSocketBuffer),
              [me = shared_from_this()](const boost::system::error_code& err,
                                        std::size_t bytes_transferred)
              { me->readDataResponse(err, bytes_transferred); });
        }

        // Reset timeout timer
        itsTimeoutTimer->expires_from_now(boost::posix_time::seconds(itsBackendTimeoutInSeconds));

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
      itsClientDataBuffer.append(itsSocketBuffer.begin(), bytes_transferred);

      if (itsResponseIsCacheable)
      {
        itsCachedContent.append(itsSocketBuffer.begin(), bytes_transferred);

        if (itsCachedContent.size() > proxy_max_cached_buffer_size)
        {
          // Overflow, do not cache this response
          itsResponseIsCacheable = false;
          itsCachedContent.clear();
        }
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
      itsTimeoutTimer->expires_from_now(boost::posix_time::seconds(itsBackendTimeoutInSeconds));
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

// Function to handle timeouts
void LowLatencyGatewayStreamer::handleTimeout(const boost::system::error_code& err)
{
  try
  {
    boost::unique_lock<boost::mutex> lock(itsMutex);
    if (err != boost::asio::error::operation_aborted)
    {
      // Cancel pending async tasks
      // This means readSocket will be called with operation_aborted - error
      itsHasTimedOut = true;
      itsResponseIsCacheable = false;
    }

    // The timer was pushed back
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
      // Clean shutdown

      // Call caching functionality here using the backend buffering thread
      // We do not want to accidentally block any server threads
      if (itsResponseIsCacheable && !itsCachedContent.empty() && !itsHasTimedOut)
      {
        // Non-empty and cacheable string. Cache it

        auto& cache = itsProxy->getCache(itsBackendMetadata.content_encoding);
        cache.insertCachedBuffer(itsBackendMetadata.etag,
                                 itsBackendMetadata.mime_type,
                                 itsBackendMetadata.cache_control,
                                 itsBackendMetadata.expires,
                                 itsBackendMetadata.vary,
                                 itsBackendMetadata.access_control_allow_origin,
                                 itsBackendMetadata.content_encoding,
                                 boost::make_shared<std::string>(itsCachedContent));
      }

      itsGatewayStatus = GatewayStatus::FINISHED;
    }
    else if (err == boost::asio::error::operation_aborted)
    {
      // Backend timed out or client disconnected
      if (itsHasTimedOut)
      {
        std::cout << boost::posix_time::second_clock::local_time() << " Connection to backend at "
                  << itsIP << ":" << itsPort << " timed out in " << itsBackendTimeoutInSeconds
                  << " seconds" << std::endl;

        itsGatewayStatus = GatewayStatus::FAILED;
      }

      // If operation_aborted is fired but itHasTimedOut is not set,
      // the client has disconnected and the connection is destructing.
      // Gatewaystatus does not matter in this case
    }
    else
    {
      std::cout << boost::posix_time::second_clock::local_time() << " Connection to backend at "
                << itsIP << ":" << itsPort << " abnormally terminated. Reason: " << err.message()
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
