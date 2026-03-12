#include "Proxy.h"
#include "LowLatencyGatewayStreamer.h"
#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/thread.hpp>
#include <fmt/format.h>
#include <macgyver/Exception.h>
#include <macgyver/TimeFormatter.h>
#include <spine/Convenience.h>
#include <iostream>
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

BackendDenyReason parseBackendDenyReason(const std::string& responsePrefix)
{
  if (responsePrefix.size() >= 13)
  {
    const std::string legacyStatus = responsePrefix.substr(9, 4);
    if (legacyStatus == "3210")
      return BackendDenyReason::SHUTDOWN;
    if (legacyStatus == "1234")
      return BackendDenyReason::HIGH_LOAD;
  }

  // New wire format may use HTTP 503 with custom reason phrase and optional X-SmartNet-Error.
  // Check status line first so detection works even if full headers are not yet buffered.
  const auto line_end = responsePrefix.find("\r\n");
  if (line_end != std::string::npos)
  {
    const std::string statusLine = responsePrefix.substr(0, line_end);
    if (statusLine.find(" 503 ") != std::string::npos)
    {
      if (statusLine.find("Shutdown in progress") != std::string::npos)
        return BackendDenyReason::SHUTDOWN;

      if (statusLine.find("High Load in Backend Server") != std::string::npos)
        return BackendDenyReason::HIGH_LOAD;
    }
  }

  auto parsed = Spine::HTTP::parseResponse(responsePrefix);
  if (std::get<0>(parsed) != Spine::HTTP::ParsingStatus::COMPLETE)
    return BackendDenyReason::NONE;

  const auto& response = std::get<1>(parsed);
  if (!response || response->getStatus() != Spine::HTTP::Status::service_unavailable)
    return BackendDenyReason::NONE;

  auto smartnetError = response->getHeader("X-SmartNet-Error");
  if (!smartnetError)
    return BackendDenyReason::NONE;

  const std::string value = boost::algorithm::trim_copy(*smartnetError);
  if (value == "3210" || boost::algorithm::iequals(value, "shutdown"))
    return BackendDenyReason::SHUTDOWN;

  if (value == "1234" || boost::algorithm::iequals(value, "high_load"))
    return BackendDenyReason::HIGH_LOAD;

  return BackendDenyReason::NONE;
}
}  // namespace

Proxy::Proxy(Proxy::Private,
             std::size_t uncompressedMemoryCacheSize,
             std::size_t uncompressedFilesystemCacheSize,
             const std::filesystem::path& uncompressedFileCachePath,
             std::size_t compressedMemoryCacheSize,
             std::size_t compressedFilesystemCacheSize,
             const std::filesystem::path& compressedFileCachePath,
             int theBackendThreadCount,
             int theBackendTimeoutInSeconds)
    : itsUncompressedResponseCache(
          uncompressedMemoryCacheSize, uncompressedFilesystemCacheSize, uncompressedFileCachePath),
      itsCompressedResponseCache(
          compressedMemoryCacheSize, compressedFilesystemCacheSize, compressedFileCachePath),
      backendIoService(theBackendThreadCount),
      idler(backendIoService.get_executor()),
      itsBackendTimeoutInSeconds(theBackendTimeoutInSeconds)
{
  std::cout << fmt::format(fmt::runtime("Backend ASIO pool size = {}"), theBackendThreadCount) << std::endl;
  std::cout << fmt::format(fmt::runtime("Backend timeout = {} seconds"), itsBackendTimeoutInSeconds) << std::endl;
  try
  {
    for (int i = 0; i < theBackendThreadCount; ++i)
    {
      // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
      itsBackendThreads.add_thread(new boost::thread([this]() { this->backendIoService.run(); }));
    }
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

std::shared_ptr<Proxy>
Proxy::create(std::size_t uncompressedMemoryCacheSize,
        std::size_t uncompressedFilesystemCacheSize,
        const std::filesystem::path& uncompressedFileCachePath,
        std::size_t compressedMemoryCacheSize,
        std::size_t compressedFilesystemCacheSize,
        const std::filesystem::path& compressedFileCachePath,
        int theBackendThreadCount,
        int theBackendTimeoutInSeconds)
{
  return std::make_shared<Proxy>(
        Private(),
        uncompressedMemoryCacheSize,
        uncompressedFilesystemCacheSize,
        uncompressedFileCachePath,
        compressedMemoryCacheSize,
        compressedFilesystemCacheSize,
        compressedFileCachePath,
        theBackendThreadCount,
        theBackendTimeoutInSeconds);
}

ResponseCache& Proxy::getCache(ResponseCache::ContentEncodingType type)
{
  try
  {
    switch (type)
    {
      case ResponseCache::ContentEncodingType::GZIP:
        return itsCompressedResponseCache;
      default:
        return itsUncompressedResponseCache;
    }
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

void Proxy::shutdown()
{
  try
  {
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

    // This is a gateway response. To detect backend denial statuses before streaming,
    // inspect the beginning of the response byte stream.
    // Supports both legacy 4-digit status lines and 503 + X-SmartNet-Error.
    std::string responsePrefix = responseStreamer->getPeekString(0, 4096);
    switch (parseBackendDenyReason(responsePrefix))
    {
      case BackendDenyReason::SHUTDOWN:
        std::cout << fmt::format("{} *** Remote {}:{} shutting down, resending to another backend",
                                 Spine::log_time_str(),
                                 theHostName,
                                 theBackendPort)
                  << std::endl;
        return ProxyStatus::PROXY_FAIL_REMOTE_DENIED;

      case BackendDenyReason::HIGH_LOAD:
        std::cout << fmt::format("{} *** Remote {}:{} has high load, resending to another backend",
                                 Spine::log_time_str(),
                                 theHostName,
                                 theBackendPort)
                  << std::endl;
        return ProxyStatus::PROXY_FAIL_REMOTE_DENIED;

      case BackendDenyReason::NONE:
        break;
    }

    theResponse.setContent(responseStreamer);
    theResponse.isGatewayResponse =
        true;  // This response is gateway response, it will be sent as a byte stream
    theResponse.setStatus(Spine::HTTP::Status::ok);

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
