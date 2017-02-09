#include <string>
#include <iostream>
#include <sstream>

#include <spine/Exception.h>
#include <boost/thread.hpp>
#include <boost/asio.hpp>
#include <boost/make_shared.hpp>
#include <boost/foreach.hpp>
#include <boost/algorithm/string.hpp>

#include <macgyver/TimeFormatter.h>

#include "Proxy.h"

namespace SmartMet
{
#ifndef PROXY_MAX_BUFFER_SIZE
#define PROXY_MAX_BUFFER_SIZE 16777216  // 16 MB
#endif

#ifndef PROXY_MAX_CACHED_BUFFER_SIZE
#define PROXY_MAX_CACHED_BUFFER_SIZE 20971520  // 20 MB
#endif

#ifndef BACKEND_TIMEOUT_SECONDS
#define BACKEND_TIMEOUT_SECONDS 600
#endif

namespace ip = boost::asio::ip;

namespace
{
std::string makeDateString()
{
  try
  {
    boost::posix_time::time_facet* lf(
        new boost::posix_time::time_facet("%a, %d %b %Y %H:%M:%S GMT"));
    std::stringstream ss;
    ss.imbue(std::locale(ss.getloc(), lf));
    ss << boost::posix_time::second_clock::universal_time();
    return ss.str();
  }
  catch (...)
  {
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
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
        return "";
      default:  // Unreachable, must be here because compiler
        return "";
    }
  }
  catch (...)
  {
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
  }
}

// Return the most applicable content encoding for this request
ResponseCache::ContentEncodingType clientAcceptsContentEncoding(
    const SmartMet::Spine::HTTP::Request& request)
{
  try
  {
    auto accept_encoding = request.getHeader("Accept-Encoding");
    if (accept_encoding)
    {
      if (*accept_encoding == "*")
        return ResponseCache::ContentEncodingType::GZIP;  // Accepts everything, send zipped

      if (boost::algorithm::contains(*accept_encoding, "gzip"))
      {
        return ResponseCache::ContentEncodingType::GZIP;
      }
      else
      {
        return ResponseCache::ContentEncodingType::NONE;
      }
    }
    else
    {
      return ResponseCache::ContentEncodingType::NONE;
    }
  }
  catch (...)
  {
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
  }
}

SmartMet::Spine::HTTP::Response buildClientResponse(
    const SmartMet::Spine::HTTP::Request& originalRequest,
    boost::shared_ptr<std::string> cachedBuffer,
    const ResponseCache::CachedResponseMetaData& metadata)
{
  try
  {
    SmartMet::Spine::HTTP::Response clientResponse;

    clientResponse.setHeader("Date", makeDateString());
    clientResponse.setHeader("Server", "SmartMet Synapse (" __TIME__ " " __DATE__ ")");
    clientResponse.setHeader("Vary", "Accept-Encoding");
    clientResponse.setHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    clientResponse.setHeader("Expires", "Thu, 01 Jan 1970 00:00:00 GMT");

    if (clientResponse.getVersion() == "1.1")
    {
      clientResponse.setHeader("Connection",
                               "close");  // Current implementation is one-request-per-Connection
    }

    // Cache-related headers
    //#############################################################
    // std::unique_ptr<Fmi::TimeFormatter> fmt(Fmi::TimeFormatter::create("http"));
    // const int expires_seconds = 60;
    // auto now = boost::posix_time::second_clock::universal_time();
    // std::string expires = fmt->format(now + boost::posix_time::seconds(expires_seconds));

    // clientResponse.setHeader("Expires", expires);
    //#############################################################

    // If client sent If-Modified-Since or If-None-Match - headers, respond with Not Modified.
    auto if_none_match = originalRequest.getHeader("If-None-Match");
    auto if_modified_since = originalRequest.getHeader("If-Modified-Since");

    // This block prepares the client response
    if (if_none_match)
    {
      std::string presented_etag = *if_none_match;
      if (presented_etag == metadata.etag)
      {
        clientResponse.setStatus(SmartMet::Spine::HTTP::Status::not_modified);
      }
      else
      {
        // Mismatched ETag, send response with the cached content
        // Set content metadata headers
        clientResponse.setHeader("Content-Type", metadata.mime_type);
        if (metadata.content_encoding != ResponseCache::ContentEncodingType::NONE)
          clientResponse.setHeader("Content-Encoding",
                                   contentEnumToString(metadata.content_encoding));
        clientResponse.setHeader("Content-Length", std::to_string(cachedBuffer->size()));

        clientResponse.setHeader("X-Frontend-Cache-Hit", "true");

        clientResponse.setStatus(SmartMet::Spine::HTTP::Status::ok);
        clientResponse.setContent(cachedBuffer);
      }
    }
    else
    {
      if (if_modified_since)
      {
        // Send Not modified
        clientResponse.setStatus(SmartMet::Spine::HTTP::Status::not_modified);
      }
      else
      {
        // No cache headers, send response with the cached content
        // Set content metadata headers
        clientResponse.setHeader("Content-Type", metadata.mime_type);
        if (metadata.content_encoding != ResponseCache::ContentEncodingType::NONE)
          clientResponse.setHeader("Content-Encoding",
                                   contentEnumToString(metadata.content_encoding));
        clientResponse.setHeader("Content-Length", std::to_string(cachedBuffer->size()));

        clientResponse.setHeader("X-Frontend-Cache-Hit", "true");

        clientResponse.setStatus(SmartMet::Spine::HTTP::Status::ok);
        clientResponse.setContent(cachedBuffer);
      }
    }

    return clientResponse;
  }
  catch (...)
  {
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
  }
}
}

class LowLatencyGatewayStreamer : public SmartMet::Spine::HTTP::ContentStreamer,
                                  public boost::enable_shared_from_this<LowLatencyGatewayStreamer>
{
 public:
  enum class GatewayStatus
  {
    ONGOING,
    FINISHED,
    FAILED
  };

  LowLatencyGatewayStreamer(boost::shared_ptr<SmartMet::Proxy> theProxy,
                            const std::string& theIP,
                            unsigned short thePort,
                            const SmartMet::Spine::HTTP::Request& theOriginalRequest);
  virtual ~LowLatencyGatewayStreamer();

  // Begin backend operations
  bool sendAndListen();

  virtual std::string getChunk();
  virtual std::string getPeekString(int pos, int len);

 private:
  // Requests content from backend
  void sendContentRequest();

  // This buffers backend response stream
  void readDataResponse(const boost::system::error_code& error, std::size_t bytes_transferred);

  // This buffers backend headers stream
  void readDataResponseHeaders(const boost::system::error_code& error,
                               std::size_t bytes_transferred);

  // This buffers backend response to cache query
  void readCacheResponse(const boost::system::error_code& error, std::size_t bytes_transferred);

  // Function to handle timeouts
  void handleTimeout(const boost::system::error_code& err);

  // Function to handle errors in backend communication
  void handleError(const boost::system::error_code& err);

  // Flag to indicate if we should cache the response content
  bool itsResponseIsCacheable = true;

  // Flag to indicate backend response buffer is full and needs to be extracted by the server
  bool itsBackendBufferFull = false;

  // Saved request originating from the client
  SmartMet::Spine::HTTP::Request itsOriginalRequest;

  // Buffer for socket operations
  boost::array<char, 8192> itsSocketBuffer;

  // This buffer will be sent to client
  std::string itsClientDataBuffer;

  // This buffer will hold backend headers
  std::string itsResponseHeaderBuffer;

  // This buffer will go to the frontend cache
  std::string itsCachedContent;

  // Metdata related to the cached response
  ResponseCache::CachedResponseMetaData itsBackendMetadata;

  // Gateway stream status
  GatewayStatus itsGatewayStatus;

  // Backend IP
  std::string itsIP;

  // Backend port
  unsigned short itsPort;

  // Mutex for commmuncation with the server
  boost::mutex itsMutex;

  // Condition to signal data is available for the server
  boost::condition_variable itsDataAvailableEvent;

  // Socket
  ip::tcp::socket itsBackendSocket;

  // Timer for backend timeouts
  boost::shared_ptr<boost::asio::deadline_timer> itsTimeoutTimer;

  // Flag to signal backend connection has timed tou
  bool itsHasTimedOut;

  // Handle to the proxy (contains caches, etc)
  boost::shared_ptr<SmartMet::Proxy> itsProxy;
};

LowLatencyGatewayStreamer::~LowLatencyGatewayStreamer()
{
}
LowLatencyGatewayStreamer::LowLatencyGatewayStreamer(
    boost::shared_ptr<SmartMet::Proxy> theProxy,
    const std::string& theIP,
    unsigned short thePort,
    const SmartMet::Spine::HTTP::Request& theOriginalRequest)
    : ContentStreamer(),
      itsOriginalRequest(theOriginalRequest),
      itsGatewayStatus(GatewayStatus::ONGOING),
      itsIP(theIP),
      itsPort(thePort),
      itsBackendSocket(theProxy->backendIoService),
      itsHasTimedOut(false),
      itsProxy(theProxy)
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

    if (err)
    {
      std::cout << boost::posix_time::second_clock::local_time() << " Backend connection to "
                << itsIP << " failed with message '" << err.message() << "'" << std::endl;

      return false;
    }
    else
    {
      // We have determined that this option significantly improves frontend latency
      boost::asio::ip::tcp::no_delay no_delay_option(true);
      itsBackendSocket.set_option(no_delay_option);

      // Attempt to write to the socket

      // This header signals we query ETag from the backend
      itsOriginalRequest.setHeader("X-Request-ETag", "true");

      std::string content = itsOriginalRequest.toString();
      boost::asio::write(itsBackendSocket, boost::asio::buffer(content), err);
      if (err)
      {
        std::cout << boost::posix_time::second_clock::local_time() << " Backend write to " << itsIP
                  << " failed with message '" << err.message() << "'" << std::endl;

        return false;
      }

      // Remove cache query header, it is no longer needed
      itsOriginalRequest.removeHeader("X-Request-ETag");
    }

    // Start the timeout timer
    itsTimeoutTimer.reset(new boost::asio::deadline_timer(
        itsProxy->backendIoService, boost::posix_time::seconds(BACKEND_TIMEOUT_SECONDS)));

    itsTimeoutTimer->async_wait(
        boost::bind(&LowLatencyGatewayStreamer::handleTimeout, shared_from_this(), _1));

    // Start to listen for the reply
    itsBackendSocket.async_read_some(boost::asio::buffer(itsSocketBuffer),
                                     boost::bind(&LowLatencyGatewayStreamer::readCacheResponse,
                                                 shared_from_this(),
                                                 _1,
                                                 _2));  // Headers not yet received

    return true;
  }
  catch (...)
  {
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
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
          boost::bind(&LowLatencyGatewayStreamer::readDataResponse, shared_from_this(), _1, _2));

      // Reset timeout timer
      itsTimeoutTimer->expires_from_now(boost::posix_time::seconds(BACKEND_TIMEOUT_SECONDS));
    }

    return returnedBuffer;
  }
  catch (...)
  {
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
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
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
  }
}

void LowLatencyGatewayStreamer::readCacheResponse(const boost::system::error_code& error,
                                                  std::size_t bytes_transferred)
{
  try
  {
    boost::unique_lock<boost::mutex> lock(itsMutex);

    if (!error)
    {
      itsResponseHeaderBuffer.append(itsSocketBuffer.begin(), bytes_transferred);

      // Attempt to parse the response headers
      auto ret = SmartMet::Spine::HTTP::parseResponse(itsResponseHeaderBuffer);
      switch (std::get<0>(ret))
      {
        case SmartMet::Spine::HTTP::ParsingStatus::FAILED:
          // Garbled response, handle error
          std::cout << boost::posix_time::second_clock::local_time()
                    << " Cache query to backend at " << itsIP << ":" << itsPort
                    << " returned garbled response." << std::endl
                    << "Query: " << std::endl
                    << itsOriginalRequest.getQueryString() << std::endl
                    << "Response buffer: " << std::endl
                    << itsResponseHeaderBuffer << std::endl;

          itsGatewayStatus = GatewayStatus::FAILED;
          break;

        case SmartMet::Spine::HTTP::ParsingStatus::INCOMPLETE:
          // Partial response, read more data

          itsBackendSocket.async_read_some(
              boost::asio::buffer(itsSocketBuffer),
              boost::bind(
                  &LowLatencyGatewayStreamer::readCacheResponse, shared_from_this(), _1, _2));

          // Reset timeout timer
          itsTimeoutTimer->expires_from_now(boost::posix_time::seconds(BACKEND_TIMEOUT_SECONDS));

          break;

        case SmartMet::Spine::HTTP::ParsingStatus::COMPLETE:

          // Successfull parse.
          auto&& responsePtr = std::get<1>(ret);

          // See if backend responded with ETag
          auto cacheHeader = responsePtr->getHeader("ETag");
          if (!cacheHeader)
          {
            // Backend responded without the ETag-header, this plugin doesn't support frontend
            // caching
            // Pass response through as before

            itsResponseIsCacheable = false;

            itsClientDataBuffer = itsResponseHeaderBuffer;

            // Go to data response loop
            itsBackendSocket.async_read_some(
                boost::asio::buffer(itsSocketBuffer),
                boost::bind(
                    &LowLatencyGatewayStreamer::readDataResponse, shared_from_this(), _1, _2));

            // Reset timeout timer
            itsTimeoutTimer->expires_from_now(boost::posix_time::seconds(BACKEND_TIMEOUT_SECONDS));

            itsDataAvailableEvent.notify_one();  // Tell consumer thread to proceed
          }
          else
          {
            std::string etag = *cacheHeader;

            // See if we should send content-encoded response
            auto accepted_content_type = clientAcceptsContentEncoding(itsOriginalRequest);

            std::pair<boost::shared_ptr<std::string>, ResponseCache::CachedResponseMetaData> result;

            switch (accepted_content_type)
            {
              case ResponseCache::ContentEncodingType::GZIP:
              {
                // We can try both compressed and uncompressed caches
                auto& c_cache = itsProxy->getCache(ResponseCache::ContentEncodingType::GZIP);
                result = c_cache.getCachedBuffer(etag);
                if (!result.first)
                {
                  // No match from compressed cache
                  auto& u_cache = itsProxy->getCache(ResponseCache::ContentEncodingType::NONE);
                  result = u_cache.getCachedBuffer(etag);
                  if (!result.first)
                  {
                    // No match from either cache, request the data
                    sendContentRequest();
                    return;
                  }
                }
              }
              break;

              case ResponseCache::ContentEncodingType::NONE:
              {
                // Client not accepting compressed data, only check uncompressed cache
                auto& cache = itsProxy->getCache(ResponseCache::ContentEncodingType::NONE);
                result = cache.getCachedBuffer(etag);
                if (!result.first)
                {
                  // No match from either cache, request the data
                  sendContentRequest();
                  return;
                }
              }
              break;
            }

            // Found from the buffer cache

            // Make sure cached responses are not re-cached
            itsResponseIsCacheable = false;

            auto metadata = result.second;
            auto response_buffer = result.first;

            auto clientResponse =
                buildClientResponse(itsOriginalRequest, response_buffer, metadata);

            itsClientDataBuffer = clientResponse.toString();

            itsGatewayStatus =
                GatewayStatus::FINISHED;  // Entire response content generated, we are done!

            // Explicitly close the socket here, since ASIO doesn't know the backend conversation is
            // finished
            // Backend socket will leak without this
            boost::system::error_code err;
            itsBackendSocket.close(err);

            itsDataAvailableEvent.notify_one();  // Tell consumer thread to proceed
          }

          break;
      }
    }
    else
    {
      handleError(error);
    }
  }
  catch (...)
  {
    std::cerr << "Operation failed! LowLatencyGatewayStreamer::readCacheResponse aborted"
              << std::endl;
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

    if (err)
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
      // Start to listen for the reply
      itsBackendSocket.async_read_some(
          boost::asio::buffer(itsSocketBuffer),
          boost::bind(&LowLatencyGatewayStreamer::readDataResponseHeaders,
                      shared_from_this(),
                      _1,
                      _2));  // Headers not yet received

      // Reset timeout timer
      itsTimeoutTimer->expires_from_now(boost::posix_time::seconds(BACKEND_TIMEOUT_SECONDS));
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
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
  }
}

void LowLatencyGatewayStreamer::readDataResponseHeaders(const boost::system::error_code& error,
                                                        std::size_t bytes_transferred)
{
  try
  {
    // Parse response headers for possible cache insertion
    boost::unique_lock<boost::mutex> lock(itsMutex);

    if (!error)
    {
      itsResponseHeaderBuffer.append(itsSocketBuffer.begin(), bytes_transferred);

      auto ret = SmartMet::Spine::HTTP::parseResponse(itsResponseHeaderBuffer);
      switch (std::get<0>(ret))
      {
        case SmartMet::Spine::HTTP::ParsingStatus::FAILED:
          // Garbled response, handle error
          std::cout << boost::posix_time::second_clock::local_time() << " Data query to backend at "
                    << itsIP << ":" << itsPort << " return garbled response" << std::endl;

          itsGatewayStatus = GatewayStatus::FAILED;
          return;

        case SmartMet::Spine::HTTP::ParsingStatus::INCOMPLETE:
          // Partial response, read more data

          itsBackendSocket.async_read_some(
              boost::asio::buffer(itsSocketBuffer),
              boost::bind(
                  &LowLatencyGatewayStreamer::readDataResponseHeaders, shared_from_this(), _1, _2));

          // Reset timeout timer
          itsTimeoutTimer->expires_from_now(boost::posix_time::seconds(BACKEND_TIMEOUT_SECONDS));

          return;

        case SmartMet::Spine::HTTP::ParsingStatus::COMPLETE:

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

            if (!mime || transfer_encoding || status != SmartMet::Spine::HTTP::Status::ok)
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

              auto content_encoding = responsePtr->getHeader("Content-Encoding");

              if (!content_encoding)
              {
                meta.content_encoding = ResponseCache::ContentEncodingType::NONE;
              }
              else
              {
                if (boost::algorithm::contains(*content_encoding, "gzip"))
                {
                  meta.content_encoding = ResponseCache::ContentEncodingType::GZIP;
                }
                else
                {
                  meta.content_encoding = ResponseCache::ContentEncodingType::NONE;
                }
              }

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
                boost::bind(
                    &LowLatencyGatewayStreamer::readDataResponse, shared_from_this(), _1, _2));
          }
          else
          {
            // No ETag, response is not cacheable
            itsResponseIsCacheable = false;

            itsClientDataBuffer = itsResponseHeaderBuffer;
            itsBackendSocket.async_read_some(
                boost::asio::buffer(itsSocketBuffer),
                boost::bind(
                    &LowLatencyGatewayStreamer::readDataResponse, shared_from_this(), _1, _2));
          }

          // Reset timeout timer
          itsTimeoutTimer->expires_from_now(boost::posix_time::seconds(BACKEND_TIMEOUT_SECONDS));

          itsDataAvailableEvent.notify_one();  // Tell consumer thread to proceed

          break;
      }
    }
    else
    {
      handleError(error);
    }
  }
  catch (...)
  {
    std::cerr << "Operation failed! LowLatencyGatewayStreamer::readDataResponseHeaders aborted"
              << std::endl;
  }
}

void LowLatencyGatewayStreamer::readDataResponse(const boost::system::error_code& error,
                                                 std::size_t bytes_transferred)
{
  try
  {
    boost::unique_lock<boost::mutex> lock(itsMutex);

    if (!error)
    {
      itsClientDataBuffer.append(itsSocketBuffer.begin(), bytes_transferred);

      if (itsResponseIsCacheable)
      {
        itsCachedContent.append(itsSocketBuffer.begin(), bytes_transferred);

        if (itsCachedContent.size() > PROXY_MAX_CACHED_BUFFER_SIZE)
        {
          // Overflow, do not cache this response
          itsResponseIsCacheable = false;
          itsCachedContent.clear();
        }
      }

      if (itsClientDataBuffer.size() > PROXY_MAX_BUFFER_SIZE)
      {
        // Too much data in buffer
        // Signal the consumer thread to schedule the next read when buffer is extracted
        itsBackendBufferFull = true;
        itsTimeoutTimer->cancel();
        return;
      }
      else
      {
        // Go back to listen the socket
        itsBackendSocket.async_read_some(
            boost::asio::buffer(itsSocketBuffer),
            boost::bind(&LowLatencyGatewayStreamer::readDataResponse, shared_from_this(), _1, _2));

        // Reset timeout timer
        itsTimeoutTimer->expires_from_now(boost::posix_time::seconds(BACKEND_TIMEOUT_SECONDS));
      }
    }
    else
    {
      handleError(error);
    }

    itsDataAvailableEvent.notify_one();  // Tell consumer thread to proceed
  }
  catch (...)
  {
    std::cerr << "Operation failed! LowLatencyGatewayStreamer::readDataResponse aborted"
              << std::endl;
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
      boost::system::error_code ignored_error;
      itsBackendSocket.cancel(ignored_error);
      itsResponseIsCacheable = false;
    }

    // The timer was pushed back
  }
  catch (...)
  {
    std::cerr << "Operation failed! LowLatencyGatewayStreamer::handleTimeout aborted" << std::endl;
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
                  << itsIP << ":" << itsPort << " timed out" << std::endl;

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
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
  }
}

Proxy::Proxy(std::size_t uncompressedMemoryCacheSize,
             std::size_t uncompressedFilesystemCacheSize,
             const boost::filesystem::path& uncompressedFileCachePath,
             std::size_t compressedMemoryCacheSize,
             std::size_t compressedFilesystemCacheSize,
             const boost::filesystem::path& compressedFileCachePath)
    : itsUncompressedResponseCache(
          uncompressedMemoryCacheSize, uncompressedFilesystemCacheSize, uncompressedFileCachePath),
      itsCompressedResponseCache(
          compressedMemoryCacheSize, compressedFilesystemCacheSize, compressedFileCachePath),
      backendIoService(20),
      idler(backendIoService)
{
  try
  {
    for (int i = 0; i < 20; ++i)
    {
      itsBackendThreads.add_thread(
          new boost::thread(boost::bind(&boost::asio::io_service::run, &backendIoService)));
    }
  }
  catch (...)
  {
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
  }
}

ResponseCache& Proxy::getCache(ResponseCache::ContentEncodingType type)
{
  try
  {
    switch (type)
    {
      case ResponseCache::ContentEncodingType::GZIP:
        return itsCompressedResponseCache;
      case ResponseCache::ContentEncodingType::NONE:
        return itsUncompressedResponseCache;
      default:  // Unreachable, compiler needs this
        return itsUncompressedResponseCache;
    }
  }
  catch (...)
  {
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
  }
}

void Proxy::shutdown()
{
  try
  {
    std::cout << "  -- Shutdown requested (Proxy)\n";
    itsBackendThreads.interrupt_all();
  }
  catch (...)
  {
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
  }
}

Proxy::ProxyStatus Proxy::HTTPForward(const SmartMet::Spine::HTTP::Request& theRequest,
                                      SmartMet::Spine::HTTP::Response& theResponse,
                                      std::string& theBackendIP,
                                      int theBackendPort,
                                      std::string& theBackendURI,
                                      const std::string& theHostName)
{
  try
  {
    // Try to resolve the requesters origin IP
    std::string theRequestOriginIP;
    auto originIP = theRequest.getHeader("X-Forwarded-For");
    if (!originIP)
    {
      // No proxy forwardign header, the the requesters IP
      theRequestOriginIP = theRequest.getClientIP();
    }
    else
    {
      theRequestOriginIP = *originIP;
    }

    // Clone the incoming request

    SmartMet::Spine::HTTP::Request fwdRequest = theRequest;

    // Add frontend-related stuff

    fwdRequest.setResource(theBackendURI);

    fwdRequest.setHeader("X-Forwarded-For", theRequestOriginIP);

    fwdRequest.setHeader("Connection", "close");

    boost::shared_ptr<LowLatencyGatewayStreamer> responseStreamer(new LowLatencyGatewayStreamer(
        shared_from_this(), theBackendIP, static_cast<unsigned short>(theBackendPort), fwdRequest));

    // Begin backend negotiation
    bool success = responseStreamer->sendAndListen();
    if (!success)
    {
      return ProxyStatus::PROXY_FAIL_REMOTE_HOST;
    }

    // This is a gateway response. So the only way to find out the HTTP message
    // status is to read it from the byte stream.

    // Note 1: SmartMet::Spine::HTTP::Status::shutdown = 3210
    // Note 2: "HTTP/1.x " is 9 characters long

    std::string httpStatus = responseStreamer->getPeekString(9, 4);
    if (httpStatus == "3210")
    {
      // The backend is shutting down.
      std::cout << "*** Remote shutting down, resending to another backend\n";
      return ProxyStatus::PROXY_FAIL_REMOTE_SHUTDOWN;
    }

    theResponse.setContent(responseStreamer);
    theResponse.isGatewayResponse =
        true;  // This response is gateway response, it will be sent as a byte stream
    theResponse.setStatus(SmartMet::Spine::HTTP::Status::ok);

    // Set the originating backend information
    theResponse.itsOriginatingBackend = theHostName;
    theResponse.itsBackendPort = theBackendPort;

    return ProxyStatus::PROXY_SUCCESS;
  }
  catch (...)
  {
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
  }
}

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
