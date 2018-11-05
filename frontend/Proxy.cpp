#include "Proxy.h"
#include "LowLatencyGatewayStreamer.h"
#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/foreach.hpp>
#include <boost/make_shared.hpp>
#include <boost/thread.hpp>
#include <macgyver/TimeFormatter.h>
#include <spine/Convenience.h>
#include <spine/Exception.h>
#include <iostream>
#include <sstream>
#include <string>

namespace SmartMet
{
Proxy::Proxy(std::size_t uncompressedMemoryCacheSize,
             std::size_t uncompressedFilesystemCacheSize,
             const boost::filesystem::path& uncompressedFileCachePath,
             std::size_t compressedMemoryCacheSize,
             std::size_t compressedFilesystemCacheSize,
             const boost::filesystem::path& compressedFileCachePath,
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
  std::cout << "Backend ASIO pool size = " << theBackendThreadCount << std::endl;
  std::cout << "Backend timeout = " << itsBackendTimeoutInSeconds << " seconds" << std::endl;
  try
  {
    for (int i = 0; i < theBackendThreadCount; ++i)
    {
      // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
      itsBackendThreads.add_thread(
          new boost::thread(boost::bind(&boost::asio::io_service::run, &backendIoService)));
    }
  }
  catch (...)
  {
    throw Spine::Exception::Trace(BCP, "Operation failed!");
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
    throw Spine::Exception::Trace(BCP, "Operation failed!");
  }
}

void Proxy::shutdown()
{
  try
  {
    std::cout << Spine::log_time_str() << "  -- Shutdown requested (Proxy)" << std::endl;
    itsBackendThreads.interrupt_all();
  }
  catch (...)
  {
    throw Spine::Exception::Trace(BCP, "Operation failed!");
  }
}

Proxy::ProxyStatus Proxy::HTTPForward(Spine::Reactor& theReactor,
                                      const Spine::HTTP::Request& theRequest,
                                      Spine::HTTP::Response& theResponse,
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

    Spine::HTTP::Request fwdRequest = theRequest;

    // Add frontend-related stuff

    fwdRequest.setResource(theBackendURI);

    fwdRequest.setHeader("X-Forwarded-For", theRequestOriginIP);

    fwdRequest.setHeader("Connection", "close");

    boost::shared_ptr<LowLatencyGatewayStreamer> responseStreamer(
        new LowLatencyGatewayStreamer(shared_from_this(),
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
      std::cout << Spine::log_time_str() << " *** Remote " << theHostName << ':' << theBackendPort
                << " shutting down, resending to another backend" << std::endl;
      return ProxyStatus::PROXY_FAIL_REMOTE_DENIED;
    }

    if (httpStatus == "1234")
    {
      std::cout << Spine::log_time_str() << " *** Remote " << theHostName << ':' << theBackendPort
                << " has high load, resending to another backend" << std::endl;
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
    throw Spine::Exception::Trace(BCP, "Operation failed!");
  }
}

}  // namespace SmartMet
