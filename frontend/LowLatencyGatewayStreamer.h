#pragma once

#include "ResponseCache.h"
#include <boost/asio.hpp>
#include <memory>
#include <spine/HTTP.h>
#include <spine/Reactor.h>

namespace SmartMet
{
class Proxy;

class LowLatencyGatewayStreamer : public Spine::HTTP::ContentStreamer,
                                  public boost::enable_shared_from_this<LowLatencyGatewayStreamer>
{
  struct Private { explicit Private() = default; };

 public:
  enum class GatewayStatus
  {
    ONGOING,
    FINISHED,
    FAILED
  };

  LowLatencyGatewayStreamer(Private,
                            const std::shared_ptr<Proxy> theProxy,
                            Spine::Reactor& theReactor,
                            std::string theHostName,
                            std::string theIP,
                            unsigned short thePort,
                            int theBackendTimeoutInSeconds,
                            const Spine::HTTP::Request& theOriginalRequest);

  static std::shared_ptr<LowLatencyGatewayStreamer>
  create(const std::shared_ptr<Proxy> theProxy,
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

  std::string getChunk() override;
  virtual std::string getPeekString(int pos, int len);

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
