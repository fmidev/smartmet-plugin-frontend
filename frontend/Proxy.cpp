#include "Proxy.h"
#include "LowLatencyGatewayStreamer.h"
#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/foreach.hpp>
#include <boost/make_shared.hpp>
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
      idler(backendIoService),
      itsBackendTimeoutInSeconds(theBackendTimeoutInSeconds)
{
  std::cout << fmt::format("Backend ASIO pool size = {}", theBackendThreadCount) << std::endl;
  std::cout << fmt::format("Backend timeout = {} seconds", itsBackendTimeoutInSeconds) << std::endl;
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
    std::cout << fmt::format("{}  -- Shutdown requested (Proxy)", Spine::log_time_str())
              << std::endl;
    itsBackendThreads.interrupt_all();
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
    std::shared_ptr<LowLatencyGatewayStreamer> responseStreamer(
        new LowLatencyGatewayStreamer(sptr,
                                      theReactor,
                                      theHostName,
                                      theBackendIP,
                                      theBackendPort,
                                      itsBackendTimeoutInSeconds,
                                      fwdRequest));

    // Begin backend negotiation
    bool success = responseStreamer->sendAndListen();
    if (!success)
    {
      return ProxyStatus::PROXY_FAIL_REMOTE_HOST;
    }

    // This is a gateway response. So the only way to find out the HTTP message
    // status is to read it from the byte stream.

    // Note 1: Spine::HTTP::Status::shutdown = 3210
    // Note 2: Spine::HTTP::Status::highload = 1234
    // Note 3: "HTTP/1.x " is 9 characters long

    std::string httpStatus = responseStreamer->getPeekString(9, 4);
    if (httpStatus == "3210")
    {
      std::cout << fmt::format("{} *** Remote {}:{} shutting down, resending to another backend",
                               Spine::log_time_str(),
                               theHostName,
                               theBackendPort)
                << std::endl;
      return ProxyStatus::PROXY_FAIL_REMOTE_DENIED;
    }

    if (httpStatus == "1234")
    {
      std::cout << fmt::format("{} *** Remote {}:{} has high load, resending to another backend",
                               Spine::log_time_str(),
                               theHostName,
                               theBackendPort)
                << std::endl;
      return ProxyStatus::PROXY_FAIL_REMOTE_DENIED;
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
