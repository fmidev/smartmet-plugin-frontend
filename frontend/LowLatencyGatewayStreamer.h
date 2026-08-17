#pragma once

#include "ChunkedBodyDecoder.h"
#include "ResponseCache.h"
#include <boost/asio.hpp>
#include <spine/HTTP.h>
#include <spine/Reactor.h>
#include <memory>

namespace SmartMet
{
class Proxy;

class LowLatencyGatewayStreamer : public Spine::HTTP::ContentStreamer,
                                  public std::enable_shared_from_this<LowLatencyGatewayStreamer>
{
  struct Private
  {
    explicit Private() = default;
  };

 public:
  enum class GatewayStatus
  {
    ONGOING,
    FINISHED,
    FAILED
  };

  // How the backend delimits its response body. This is a property of the
  // backend connection only: the client response is framed independently, so a
  // backend that can only signal the end by closing still yields a client
  // response the connection can survive.
  enum class BodyFraming
  {
    LENGTH,       // Content-Length
    CHUNKED,      // Transfer-Encoding: chunked
    UNTIL_CLOSE,  // Neither, so the body ends when the backend closes
  };

  LowLatencyGatewayStreamer(Private,
                            const std::shared_ptr<Proxy> theProxy,
                            Spine::Reactor& theReactor,
                            std::string theHostName,
                            std::string theIP,
                            unsigned short thePort,
                            int theBackendTimeoutInSeconds,
                            const Spine::HTTP::Request& theOriginalRequest);

  static std::shared_ptr<LowLatencyGatewayStreamer> create(
      const std::shared_ptr<Proxy> theProxy,
      Spine::Reactor& theReactor,
      const std::string& theHostName,
      const std::string& theIP,
      unsigned short thePort,
      int theBackendTimeoutInSeconds,
      const Spine::HTTP::Request& theOriginalRequest);

  ~LowLatencyGatewayStreamer() override;

  LowLatencyGatewayStreamer(const LowLatencyGatewayStreamer& other) = delete;
  LowLatencyGatewayStreamer(LowLatencyGatewayStreamer&& other) = delete;
  LowLatencyGatewayStreamer& operator=(const LowLatencyGatewayStreamer& other) = delete;
  LowLatencyGatewayStreamer& operator=(LowLatencyGatewayStreamer&& other) = delete;

  // Begin backend operations
  bool sendAndListen();

  // ------------------------------------------------------------------
  /*!
   * \brief Block until the backend's response head has been parsed
   *
   * The caller needs the status and headers before it can build the client
   * response, and the body cannot be framed without them. Returns nullptr if
   * the exchange failed or timed out before a head arrived.
   *
   * The returned head has the hop-by-hop fields, Content-Length and
   * Transfer-Encoding removed: those describe the backend hop, and the client
   * response is framed by the server, possibly differently.
   */
  // ------------------------------------------------------------------
  const Spine::HTTP::Response* waitForResponseHead();

  // ------------------------------------------------------------------
  /*!
   * \brief The complete response when it was served from the frontend cache
   *
   * A cache hit needs no backend body at all, so it is handed over whole and
   * the caller can answer it as an ordinary buffered response. Returns nullptr
   * for anything that has to be streamed from a backend.
   */
  // ------------------------------------------------------------------
  const Spine::HTTP::Response* getCompleteResponse() const;

  // getChunk() yields the decoded response *body* only; the head is delivered
  // through waitForResponseHead().
  std::string getChunk() override;

  BodyFraming getBodyFraming() const { return itsBodyFraming; }

  // Body length the backend announced. Meaningful only for BodyFraming::LENGTH.
  std::size_t getDeclaredBodyLength() const { return itsDeclaredBodyLength; }

  // Forcibly fail this stream. Called by Proxy::shutdown() once a still-running stream
  // has outlived its shutdown grace period, so the client gets a prompt error and the
  // stream's destructor (and its shared_ptr<Proxy>) is released in time for shutdown to
  // complete rather than being SIGKILLed mid-transfer.
  void abortForShutdown();

 private:
  using DeadlineTimer = boost::asio::basic_waitable_timer<std::chrono::steady_clock>;

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

  // Function to mark the communication to be in finishing stages
  void markFinishing();

  // Publish the parsed backend head and feed it the body bytes already read
  void publishResponseHead(const Spine::HTTP::Response& theHead, const std::string& theBodySoFar);

  // Feed raw backend bytes through the framing decoder. Returns true once the
  // body is known to be complete, without waiting for the backend to close.
  bool consumeBodyBytes(const char* data, std::size_t length);

  // Append decoded body bytes to the client buffer and, when eligible, the cache
  void appendBodyBytes(const char* data, std::size_t length);

  // The backend response is complete: cache it if eligible and release the socket
  void finishBackendResponse();

  // Store the buffered body in the frontend cache when it is eligible
  void storeInCacheIfEligible();

  // Flag to indicate if we should cache the response content
  bool itsResponseIsCacheable = true;

  // Flag to indicate backend response buffer is full and needs to be extracted by the server
  bool itsBackendBufferFull = false;

  // Saved request originating from the client
  Spine::HTTP::Request itsOriginalRequest;

  // Buffer for socket operations
  std::array<char, 8192> itsSocketBuffer;

  // This buffer will be sent to client
  std::string itsClientDataBuffer;

  // This buffer will hold backend headers
  std::string itsResponseHeaderBuffer;

  // This buffer will go to the frontend cache
  std::string itsCachedContent;

  // Metdata related to the cached response
  ResponseCache::CachedResponseMetaData itsBackendMetadata;

  // Gateway stream status
  GatewayStatus itsGatewayStatus = GatewayStatus::ONGOING;

  // Parsed head of the backend response, ready before any body byte is published
  std::unique_ptr<Spine::HTTP::Response> itsBackendHead;

  // A complete response served from the frontend cache, if any
  std::unique_ptr<Spine::HTTP::Response> itsCompleteResponse;

  // True once itsBackendHead or itsCompleteResponse is set, or the exchange failed
  bool itsHeadReady = false;

  // Signals waitForResponseHead()
  boost::condition_variable itsHeadReadyEvent;

  // How the backend delimits its body, and how much of it has been decoded
  BodyFraming itsBodyFraming = BodyFraming::UNTIL_CLOSE;
  std::size_t itsDeclaredBodyLength = 0;
  std::size_t itsBodyBytesDecoded = 0;
  bool itsBodyComplete = false;

  // Removes the chunk framing when the backend used it
  ChunkedBodyDecoder itsChunkDecoder;

  // Backend name
  std::string itsHostName;

  // Backend IP
  std::string itsIP;

  // Backend port
  unsigned short itsPort;

  // Mutex for commmuncation with the server
  boost::mutex itsMutex;

  // Condition to signal data is available for the server
  boost::condition_variable itsDataAvailableEvent;

  // Socket
  boost::asio::ip::tcp::socket itsBackendSocket;

  // Timer for backend timeouts
  std::shared_ptr<DeadlineTimer> itsTimeoutTimer;

  // Flag to signal backend connection has timed out
  bool itsHasTimedOut = false;

  // Backend timeout in seconds
  int itsBackendTimeoutInSeconds;

  // Handle to the proxy (contains caches, etc)
  std::shared_ptr<Proxy> itsProxy;

  // Reference to the reactor for decrementing backend activity count when done
  Spine::Reactor& itsReactor;

  // Boolean to indicate whether the backend task is practically finished, but sockets may remain
  // open etc
  bool itsFinishing = false;
};

}  // namespace SmartMet
