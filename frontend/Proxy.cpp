#include "Proxy.h"
#include "LowLatencyGatewayStreamer.h"
#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/thread.hpp>
#include <fmt/format.h>
#include <macgyver/Exception.h>
#include <macgyver/ThreadName.h>
#include <macgyver/TimeFormatter.h>
#include <spine/Convenience.h>
#include <algorithm>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

namespace SmartMet
{
namespace
{
enum class BackendDenyReason
{
  NONE,
  SHUTDOWN,
  HIGH_LOAD
};

// Decide whether the backend refused the request rather than answering it.
//
// This used to sniff the raw first bytes of the response, because that was all
// the gateway streamer exposed and the headers were not necessarily buffered
// yet. It now works on the parsed head, which covers the same three encodings
// more simply:
//
//   - the legacy 4-digit status lines, which Spine parses straight into the
//     high_load / shutdown statuses;
//   - 503 with the X-SmartMet-Error field;
//   - 503 with one of the SmartMet reason phrases, for a backend old enough to
//     send neither of the above.
BackendDenyReason parseBackendDenyReason(const Spine::HTTP::Response& response)
{
  const auto status = response.getStatus();

  if (status == Spine::HTTP::Status::shutdown)
    return BackendDenyReason::SHUTDOWN;
  if (status == Spine::HTTP::Status::high_load)
    return BackendDenyReason::HIGH_LOAD;

  if (status != Spine::HTTP::Status::service_unavailable)
    return BackendDenyReason::NONE;

  auto smartnetError = response.getHeader(std::string(Spine::HTTP::smartmet_error_header));
  if (smartnetError)
  {
    const std::string value = boost::algorithm::trim_copy(*smartnetError);
    if (value == "3210" || boost::algorithm::iequals(value, "shutdown"))
      return BackendDenyReason::SHUTDOWN;
    if (value == "1234" || boost::algorithm::iequals(value, "high_load"))
      return BackendDenyReason::HIGH_LOAD;
  }

  const std::string& reason = response.getReasonPhrase();
  if (reason.find("Shutdown in progress") != std::string::npos)
    return BackendDenyReason::SHUTDOWN;
  if (reason.find("High Load in Backend Server") != std::string::npos)
    return BackendDenyReason::HIGH_LOAD;

  return BackendDenyReason::NONE;
}
}  // namespace

Proxy::Proxy(Proxy::Private,
             std::size_t memoryCacheSize,
             std::size_t filesystemCacheSize,
             const std::filesystem::path& fileCachePath,
             int theBackendThreadCount,
             int theBackendTimeoutInSeconds,
             int theShutdownGracePeriodInSeconds,
             bool theBackendKeepAlive,
             std::size_t theMaxIdleConnectionsPerBackend,
             int theBackendIdleTimeoutInSeconds)
    : itsResponseCache(memoryCacheSize, filesystemCacheSize, fileCachePath),
      backendIoService(theBackendThreadCount),
      idler(backendIoService.get_executor()),
      itsBackendConnections(theBackendKeepAlive,
                            theMaxIdleConnectionsPerBackend,
                            theBackendIdleTimeoutInSeconds),
      itsBackendTimeoutInSeconds(theBackendTimeoutInSeconds),
      itsShutdownGracePeriodInSeconds(theShutdownGracePeriodInSeconds)
{
  std::cout << fmt::format(fmt::runtime("Backend ASIO pool size = {}"), theBackendThreadCount) << std::endl;
  std::cout << fmt::format(fmt::runtime("Backend timeout = {} seconds"), itsBackendTimeoutInSeconds) << std::endl;
  std::cout << fmt::format(fmt::runtime("Backend shutdown grace period = {} seconds"),
                           itsShutdownGracePeriodInSeconds)
            << std::endl;
  if (theBackendKeepAlive)
    std::cout << fmt::format(
                     fmt::runtime("Backend keep-alive enabled: up to {} idle connections per "
                                  "backend, idle timeout {} seconds"),
                     theMaxIdleConnectionsPerBackend,
                     theBackendIdleTimeoutInSeconds)
              << std::endl;
  else
    std::cout << "Backend keep-alive disabled" << std::endl;
  try
  {
    for (int i = 0; i < theBackendThreadCount; ++i)
    {
      // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
      itsBackendThreads.add_thread(new boost::thread(
          [this, i]()
          {
            Fmi::set_thread_name(fmt::format("front-be-{}", i + 1));
            this->backendIoService.run();
          }));
    }
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

std::shared_ptr<Proxy>
Proxy::create(std::size_t memoryCacheSize,
        std::size_t filesystemCacheSize,
        const std::filesystem::path& fileCachePath,
        int theBackendThreadCount,
        int theBackendTimeoutInSeconds,
        int theShutdownGracePeriodInSeconds,
        bool theBackendKeepAlive,
        std::size_t theMaxIdleConnectionsPerBackend,
        int theBackendIdleTimeoutInSeconds)
{
  return std::make_shared<Proxy>(
        Private(),
        memoryCacheSize,
        filesystemCacheSize,
        fileCachePath,
        theBackendThreadCount,
        theBackendTimeoutInSeconds,
        theShutdownGracePeriodInSeconds,
        theBackendKeepAlive,
        theMaxIdleConnectionsPerBackend,
        theBackendIdleTimeoutInSeconds);
}

ResponseCache& Proxy::getCache()
{
  return itsResponseCache;
}

void Proxy::closeBackendConnections(const std::string& theIP, unsigned short thePort)
{
  itsBackendConnections.closeBackend(theIP, thePort);
}

void Proxy::registerStreamerStart(const std::shared_ptr<LowLatencyGatewayStreamer>& theStreamer)
{
  boost::unique_lock<boost::mutex> lock(itsStreamerCountMutex);
  ++itsActiveStreamerCount;
  itsActiveStreamers.emplace_back(theStreamer);
}

void Proxy::registerStreamerStop()
{
  boost::unique_lock<boost::mutex> lock(itsStreamerCountMutex);
  if (itsActiveStreamerCount > 0)
    --itsActiveStreamerCount;

  // Drop the streamers that have gone. Without this the vector keeps one entry
  // per request ever served, and a weak_ptr to a make_shared object holds that
  // object's whole allocation - a LowLatencyGatewayStreamer is 9 kB, most of it
  // the socket buffer - so the frontend would grow without bound and shutdown
  // would scan the entire history of the process. This runs while the streamer
  // being counted out is still executing its own destructor, so its weak_ptr has
  // already expired and is swept with the rest.
  itsActiveStreamers.erase(
      std::remove_if(itsActiveStreamers.begin(),
                     itsActiveStreamers.end(),
                     [](const std::weak_ptr<LowLatencyGatewayStreamer>& theStreamer)
                     { return theStreamer.expired(); }),
      itsActiveStreamers.end());

  if (itsActiveStreamerCount == 0)
    itsStreamerDrainedCond.notify_all();
}

namespace
{
// How long to give forcibly aborted streams to actually unwind (client-side consumer
// threads poll on a ~100 ms cadence) before giving up on the wait entirely.
const int postAbortSettleSeconds = 2;
}  // namespace

void Proxy::shutdown()
{
  try
  {
    // Stop accepting new gateway streams. HTTP::transport() checks isShuttingDown()
    // before creating a LowLatencyGatewayStreamer.
    itsShuttingDown = true;

    // Wait for in-flight gateway streams to finish while backendIoService threads are
    // still running to service them. A streamer whose backend I/O thread pool has already
    // stopped can never finish, and each one keeps this Proxy (and this plugin's shared
    // library) referenced via its shared_ptr<Proxy> past the point the Reactor unloads it
    // - which is what caused the boost::mutex/segfault crashes on shutdown.
    //
    // A response can be gigabytes in size (weather model data), so it may take far longer
    // to finish than a process manager allows for shutdown (e.g. systemd's TimeoutStopSec).
    // Waiting it out would just mean the whole process gets SIGKILLed with the response lost
    // anyway, so only give streams a bounded grace period to finish on their own; anything
    // still running after that is aborted so the client gets a prompt error and shutdown can
    // complete in time.
    {
      boost::unique_lock<boost::mutex> lock(itsStreamerCountMutex);
      const int stepMillis = 200;
      const int graceSteps = std::max(1, itsShutdownGracePeriodInSeconds) * 1000 / stepMillis;
      int steps = 0;
      while (itsActiveStreamerCount > 0 && steps < graceSteps)
      {
        itsStreamerDrainedCond.timed_wait(lock, boost::posix_time::milliseconds(stepMillis));
        ++steps;
      }

      if (itsActiveStreamerCount > 0)
      {
        std::cout << fmt::format(
                         "{} WARNING: {} gateway stream(s) still active after {}s grace period, "
                         "aborting them",
                         Spine::log_time_str(),
                         itsActiveStreamerCount,
                         itsShutdownGracePeriodInSeconds)
                  << std::endl;

        for (const auto& weakStreamer : itsActiveStreamers)
          if (auto streamer = weakStreamer.lock())
            streamer->abortForShutdown();

        const int settleSteps = std::max(1, postAbortSettleSeconds) * 1000 / stepMillis;
        steps = 0;
        while (itsActiveStreamerCount > 0 && steps < settleSteps)
        {
          itsStreamerDrainedCond.timed_wait(lock, boost::posix_time::milliseconds(stepMillis));
          ++steps;
        }

        if (itsActiveStreamerCount > 0)
          std::cout << fmt::format(
                           "{} WARNING: Proxy shutdown proceeding with {} gateway stream(s) "
                           "still unfinished",
                           Spine::log_time_str(),
                           itsActiveStreamerCount)
                    << std::endl;
      }
    }

    // Idle pooled connections have no pending operations, so nothing will ever
    // close them on their own. Do it before the io_context stops.
    itsBackendConnections.closeAll();

    backendIoService.stop();
    std::cout << fmt::format("{}  -- Shutdown requested (Proxy)", Spine::log_time_str())
              << std::endl;
    itsBackendThreads.interrupt_all();
    itsBackendThreads.join_all();
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

Proxy::ProxyStatus Proxy::HTTPForward(Spine::Reactor& theReactor,
                                      const Spine::HTTP::Request& theRequest,
                                      Spine::HTTP::Response& theResponse,
                                      const std::string& theBackendIP,
                                      int theBackendPort,
                                      const std::string& theBackendURI,
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

    Spine::HTTP::Request fwdRequest = theRequest;

    // Add frontend-related stuff

    fwdRequest.setResource(theBackendURI);

    fwdRequest.setHeader("X-Forwarded-For", theRequestOriginIP);

    // Hop-by-hop fields describe the connection they arrived on, not the one
    // being opened here (RFC 9110 7.6.1). The server strips them before a plugin
    // sees the request, so this is belt and braces - but the frontend must not
    // pass on the client's connection preferences either way.
    fwdRequest.removeHeader("Connection");
    fwdRequest.removeHeader("Keep-Alive");
    fwdRequest.removeHeader("Proxy-Connection");
    fwdRequest.removeHeader("TE");
    fwdRequest.removeHeader("Trailer");
    fwdRequest.removeHeader("Upgrade");

    // The client's body is already in hand, so the 100-continue negotiation is
    // over and done with at that hop. Forwarding the expectation would only make
    // the backend answer with an interim "100 Continue" that this frontend has no
    // use for - and that would be read as the response head, leaving the
    // connection one message out of step for good.
    fwdRequest.removeHeader("Expect");

    // Keep-alive towards the backend is negotiated on the backend connection
    // alone and says nothing about the client's: the frontend re-frames the
    // response, so the client connection persists (or not) regardless of this hop.
    //
    // HTTP/1.1 is persistent by default, so a request forwarded as 1.1 needs no
    // header at all. An HTTP/1.0 request has no persistence to ask for, and
    // upgrading it here would change what the backend is allowed to answer with
    // (chunked framing, interim responses), so it keeps the explicit close.
    if (!itsBackendConnections.isEnabled() || fwdRequest.getVersion() != "1.1")
      fwdRequest.setHeader("Connection", "close");

    // Establish used protocol. At FMI this is normally set by the F5 load balancer,
    // but in some environments the Frontend server must do this by itself
    auto protocol = fwdRequest.getHeader("X-Forwarded-Proto");
    if (!protocol)
    {
      const auto* proto = (theReactor.isEncrypted() ? "https" : "http");
      fwdRequest.setHeader("X-Forwarded-Proto", proto);
    }

    std::shared_ptr<Proxy> sptr = shared_from_this();
    std::shared_ptr<LowLatencyGatewayStreamer> responseStreamer =
        LowLatencyGatewayStreamer::create(sptr,
                                          theReactor,
                                          theHostName,
                                          theBackendIP,
                                          theBackendPort,
                                          itsBackendTimeoutInSeconds,
                                          fwdRequest);

    // Begin backend negotiation
    bool success = responseStreamer->sendAndListen();
    if (!success)
    {
      return ProxyStatus::PROXY_FAIL_REMOTE_HOST;
    }

    // Wait for the backend's response head: the client response cannot be built
    // or framed without it, and the denial statuses are read from it.
    const Spine::HTTP::Response* backendHead = responseStreamer->waitForResponseHead();
    if (backendHead == nullptr)
    {
      // The backend never produced a parseable response. The stream has failed
      // or timed out to get here, so this is a belt-and-braces release of
      // whatever it may still be holding.
      responseStreamer->abort("no response head from the backend");
      return ProxyStatus::PROXY_FAIL_REMOTE_HOST;
    }

    const auto denyReason = parseBackendDenyReason(*backendHead);
    if (denyReason != BackendDenyReason::NONE)
    {
      const char* what =
          (denyReason == BackendDenyReason::SHUTDOWN ? "shutting down" : "has high load");

      std::cout << fmt::format("{} *** Remote {}:{} {}, resending to another backend",
                               Spine::log_time_str(),
                               theHostName,
                               theBackendPort,
                               what)
                << std::endl;

      // The denial is answered from another backend, so nothing will ever read
      // this stream. A denial reply is normally short enough to have arrived
      // whole, in which case the stream has already finished and this does
      // nothing - keeping its pooled connection. One that has not finished would
      // otherwise sit there holding a backend socket until the process ends, and
      // under cluster-wide high load every retry would leave another behind.
      responseStreamer->abort("backend denied the request");

      return ProxyStatus::PROXY_FAIL_REMOTE_DENIED;
    }

    // Re-emit the backend's head as this frontend's own response instead of
    // forwarding the backend's bytes verbatim. The server then frames the
    // response and negotiates the client connection exactly as it does for any
    // other plugin response, which is what keeps the two hops' keep-alive
    // states independent: the backend connection is still one request per
    // connection, but the client's need not be.
    if (const auto* cached = responseStreamer->getCompleteResponse())
    {
      // Served from the frontend cache: there is no backend body to stream
      theResponse = *cached;
    }
    else
    {
      theResponse.setStatus(backendHead->getStatus());
      for (const auto& header : backendHead->getHeaders())
        theResponse.setHeader(header.first, header.second);

      if (responseStreamer->getBodyFraming() == LowLatencyGatewayStreamer::BodyFraming::LENGTH)
      {
        // Length known up front, so the client gets a Content-Length too
        theResponse.setContent(responseStreamer, responseStreamer->getDeclaredBodyLength());
      }
      else
      {
        // Chunked, or a backend that only signals the end by closing. The length
        // is unknown here either way, so the server sends it chunked - and a
        // chunked response is self-delimiting, so the client connection survives
        // a backend connection that could not be reused.
        theResponse.setContent(responseStreamer);
      }
    }

    // Set the originating backend information
    theResponse.itsOriginatingBackend = theHostName;
    theResponse.itsBackendPort = theBackendPort;

    return ProxyStatus::PROXY_SUCCESS;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

}  // namespace SmartMet
