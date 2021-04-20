#include "HTTP.h"
#include "Proxy.h"
#include <boost/bind.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/foreach.hpp>
#include <boost/make_shared.hpp>
#include <boost/shared_ptr.hpp>
#include <engines/sputnik/Engine.h>
#include <macgyver/Exception.h>
#include <macgyver/StringConversion.h>
#include <spine/Convenience.h>
#include <spine/Reactor.h>
#include <iostream>
#include <stdexcept>

namespace SmartMet
{
namespace Plugin
{
namespace Frontend
{
class BackendCounter
{
 public:
  BackendCounter(Spine::Reactor &theReactor, const std::string &theHost, int thePort)
      : itsReactor(theReactor), itsHost(theHost), itsPort(thePort)
  {
    itsReactor.startBackendRequest(itsHost, itsPort);
  }

  ~BackendCounter() { itsReactor.stopBackendRequest(itsHost, itsPort); }

 private:
  Spine::Reactor &itsReactor;
  std::string itsHost;
  int itsPort;
};

Proxy::ProxyStatus HTTP::transport(Spine::Reactor &theReactor,
                                   const Spine::HTTP::Request &theRequest,
                                   Spine::HTTP::Response &theResponse)
{
  try
  {
    // Choose the backend host by URI
    std::string theResource(theRequest.getResource());

    BackendServicePtr theService = itsSputnikProcess->getServices().getService(theResource);

    if (theService.get() == nullptr)
    {
      // 404 Service Not Found
      theResponse.setStatus(Spine::HTTP::Status::not_found, true);

      return Proxy::ProxyStatus::PROXY_FAIL_SERVICE;
    }

    // BackendServer where we're connecting to
    const boost::shared_ptr<BackendServer> theHost = theService->Backend();

    if (theHost.get() == nullptr)
    {
      std::cout << Spine::log_time_str() << " Service backend value is null" << std::endl;

      // 502 Service Not Found
      theResponse.setStatus(Spine::HTTP::Status::bad_gateway, true);
      return Proxy::ProxyStatus::PROXY_FAIL_SERVICE;
    }

    // See if this backend is set as 'temporarily unconscious'
    if (!itsSputnikProcess->getServices().queryBackendAlive(theHost->Name(), theHost->Port()))
    {
      itsSputnikProcess->getServices().removeBackend(theHost->Name(), theHost->Port());

      std::cout << Spine::log_time_str() << " Backend " << theHost->Name() << ':' << theHost->Port()
                << " is marked as dead. Retiring backend server." << std::endl;

      return Proxy::ProxyStatus::PROXY_FAIL_REMOTE_HOST;
    }

    // Forward the request keeping account of how many active requests each backend has.
    // The destructor of the streamer created by the proxy will decrement the count.

    Proxy::ProxyStatus proxyStatus = Proxy::ProxyStatus::PROXY_SUCCESS;

    // Scope guard for counting active requests
    BackendCounter counter(theReactor, theHost->Name(), theHost->Port());

    // Use Proxy class to forward the request to backend server
    proxyStatus = itsProxy->HTTPForward(theReactor,
                                        theRequest,
                                        theResponse,
                                        theHost->IP(),
                                        theHost->Port(),
                                        theService->URI(),
                                        theHost->Name());

    // Check the Proxy status
    if (proxyStatus != Proxy::ProxyStatus::PROXY_SUCCESS)
    {
      // Immediately remove the backend server from the service providing pool
      // if there was a problem connecting to backend server.
      std::cout << Spine::log_time_str() << " Backend Server connection to " << theHost->Name()
                << ':' << theHost->Port() << " failed, retiring the backend server." << std::endl;

      itsSputnikProcess->getServices().removeBackend(theHost->Name(), theHost->Port());
    }
    else
    {
      // Signal that a connection has been sent to the backend (for throttle bookkeeping)
      itsSputnikProcess->getServices().signalBackendConnection(theHost->Name(), theHost->Port());
    }

    return proxyStatus;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

void HTTP::requestHandler(Spine::Reactor &theReactor,
                          const Spine::HTTP::Request &theRequest,
                          Spine::HTTP::Response &theResponse)
{
  try
  {
    Proxy::ProxyStatus theStatus;

    // Try to send the request until it is sent or no backends are available.
    // Currently we do not resend on PROXY_FAIL_REMOTE_HOST since the request
    // may have crashed the backend.
    do
    {
      theStatus = transport(theReactor, theRequest, theResponse);

      if (theStatus == Proxy::ProxyStatus::PROXY_FAIL_REMOTE_DENIED)
        std::cout << Spine::log_time_str() << " Resending URI " << theRequest.getURI() << std::endl;
    } while (theStatus == Proxy::ProxyStatus::PROXY_FAIL_REMOTE_DENIED);

    return;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

HTTP::HTTP(Spine::Reactor *theReactor, const char *theConfig)
{
  try
  {
    // Banner
    std::cout << "\t+ HTTP Cluster Gateway "
              << "(compiled on " __DATE__ " " __TIME__ ")" << std::endl;

    // Launch a new instance of Sputnik on network ItsNetworkAddress
    itsSputnikProcess =
        reinterpret_cast<Engine::Sputnik::Engine *>(theReactor->getSingleton("Sputnik", nullptr));

    // Throw error if instance could not be created
    if (itsSputnikProcess == nullptr)
      throw Fmi::Exception(BCP, "HTTP plugin could not find Sputnik instance");

    // Start Sputnik in frontend mode with call-back function
    itsSputnikProcess->launch(Engine::Sputnik::Frontend, theReactor);

    // Start the "Catcher in the Rye" process in SmartMet core
    theReactor->setNoMatchHandler(boost::bind(&HTTP::requestHandler, this, _1, _2, _3));

    // Get hold of the reactor pointer
    this->itsReactor = theReactor;

    libconfig::Config config;
    unsigned long long memorySize = 0;
    unsigned long long filesystemSize = 0;
    unsigned long long uncomMemorySize = 0;
    unsigned long long uncomFilesystemSize = 0;

    // do not use nullptr here or path construction throws
    const char *filesystemCachePath = "";
    const char *uncomFilesystemCachePath = "";

    int backendTimeoutInSeconds = 600;
    int backendThreadCount = 20;

    try
    {
      // Enable sensible relative include paths
      boost::filesystem::path p = theConfig;
      p.remove_filename();
      config.setIncludeDir(p.c_str());

      config.readFile(theConfig);

      config.lookupValue("compressed_cache.memory_bytes", memorySize);
      config.lookupValue("compressed_cache.filesystem_bytes", filesystemSize);
      config.lookupValue("compressed_cache.directory", filesystemCachePath);
      config.lookupValue("uncompressed_cache.memory_bytes", uncomMemorySize);
      config.lookupValue("uncompressed_cache.filesystem_bytes", uncomFilesystemSize);
      config.lookupValue("uncompressed_cache.directory", uncomFilesystemCachePath);
      config.lookupValue("backend.timeout", backendTimeoutInSeconds);
      config.lookupValue("backend.threads", backendThreadCount);
    }
    catch (const libconfig::ParseException &e)
    {
      throw Fmi::Exception(BCP,
                           std::string("Configuration error ' ") + e.getError() + "' on line " +
                               Fmi::to_string(e.getLine()));
    }
    catch (const libconfig::ConfigException &)
    {
      throw Fmi::Exception(BCP, "Configuration error");
    }
    catch (...)
    {
      throw Fmi::Exception::Trace(BCP, "Configuration error!");
    }

    itsProxy = boost::make_shared<Proxy>(memorySize,
                                         filesystemSize,
                                         boost::filesystem::path(filesystemCachePath),
                                         uncomMemorySize,
                                         uncomFilesystemSize,
                                         boost::filesystem::path(uncomFilesystemCachePath),
                                         backendThreadCount,
                                         backendTimeoutInSeconds);
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

HTTP::~HTTP()
{
  // Banner
  std::cout << "\t+ HTTP plugin shutting down" << std::endl;

  // Must remove the Catcher in the Rye hook from SmartMet core
  // to avoid calling unloaded code.
  this->itsReactor->setNoMatchHandler(0);  // NOLINT can't use nullptr in a boost function
}

void HTTP::shutdown()
{
  try
  {
    std::cout << "  -- Shutdown requested (HTTP)" << std::endl;
    itsProxy->shutdown();
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

}  // namespace Frontend
}  // namespace Plugin
}  // namespace SmartMet
