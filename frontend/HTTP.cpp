#include <boost/bind.hpp>
#include <iostream>
#include <stdexcept>

#include <boost/shared_ptr.hpp>
#include <boost/foreach.hpp>
#include <boost/make_shared.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include <spine/Exception.h>
#include <spine/Reactor.h>

#include <engines/sputnik/Engine.h>

#include "HTTP.h"
#include "Proxy.h"

namespace SmartMet
{
namespace Plugin
{
namespace Frontend
{
Proxy::ProxyStatus HTTP::transport(const SmartMet::Spine::HTTP::Request &theRequest,
                                   SmartMet::Spine::HTTP::Response &theResponse)
{
  try
  {
    // Choose the backend host by URI
    std::string theResource(theRequest.getResource());

    BackendServicePtr theService = itsSputnikProcess->itsServices.getService(theResource);

    if (theService.get() == 0)
    {
      // 404 Service Not Found
      theResponse.setStatus(SmartMet::Spine::HTTP::Status::not_found, true);

      return Proxy::ProxyStatus::PROXY_FAIL_SERVICE;
    }

    // BackendServer where we're connecting to
    const boost::shared_ptr<BackendServer> theHost = theService->Backend();

    if (theHost.get() == 0)
    {
      std::cout << boost::posix_time::second_clock::local_time() << " Service backend value is null"
                << std::endl;

      // 502 Service Not Found
      theResponse.setStatus(SmartMet::Spine::HTTP::Status::bad_gateway, true);
      return Proxy::ProxyStatus::PROXY_FAIL_SERVICE;
    }

    // See if this backend is set as 'temporarily unconscious'
    if (!itsSputnikProcess->itsServices.queryBackendAlive(theHost->Name(), theHost->Port()))
    {
      itsSputnikProcess->itsServices.removeBackend(theHost->Name(), theHost->Port());

      std::cout << boost::posix_time::second_clock::local_time() << " Backend " << theHost->Name()
                << " is marked as dead. Retiring backend server." << std::endl;

      return Proxy::ProxyStatus::PROXY_FAIL_REMOTE_HOST;
    }

    // Use Proxy class to forward the request to backend server
    Proxy::ProxyStatus proxyStatus = itsProxy->HTTPForward(theRequest,
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
      std::cout << boost::posix_time::second_clock::local_time()
                << " Backend Server connection failed, retiring the backend server." << std::endl;

      itsSputnikProcess->itsServices.removeBackend(theHost->Name(), theHost->Port());
    }
    else
    {
      // Signal that a connection has been sent to the backend (for throttle bookkeeping)
      itsSputnikProcess->itsServices.signalBackendConnection(theHost->Name(), theHost->Port());
    }

    return proxyStatus;
  }
  catch (...)
  {
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
  }
}

void HTTP::requestHandler(SmartMet::Spine::Reactor & /* theReactor */,
                          const SmartMet::Spine::HTTP::Request &theRequest,
                          SmartMet::Spine::HTTP::Response &theResponse)
{
  try
  {
    Proxy::ProxyStatus theStatus;

    // Try to send the request until it is sent or no backends are available.
    // Currently we do not resend on PROXY_FAIL_REMOTE_HOST since the request
    // may have crashed the backend.
    do
    {
      theStatus = transport(theRequest, theResponse);

      if (theStatus == Proxy::ProxyStatus::PROXY_FAIL_REMOTE_SHUTDOWN)
        std::cout << "####### RESENDING ########\n";
    } while (theStatus == Proxy::ProxyStatus::PROXY_FAIL_REMOTE_SHUTDOWN);

    return;
  }
  catch (...)
  {
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
  }
}

HTTP::HTTP(SmartMet::Spine::Reactor *theReactor, const char *theConfig)
{
  try
  {
    // Banner
    std::cout << "\t+ HTTP Cluster Gateway "
              << "(compiled on " __DATE__ " " __TIME__ ")" << std::endl;

    // Launch a new instance of Sputnik on network ItsNetworkAddress
    itsSputnikProcess = reinterpret_cast<SmartMet::Engine::Sputnik::Engine *>(
        theReactor->getSingleton("Sputnik", (void *)NULL));

    // Throw error if instance could not be created
    if (itsSputnikProcess == NULL)
      throw SmartMet::Spine::Exception(BCP, "HTTP plugin could not find Sputnik instance");

    // Start Sputnik in frontend mode with call-back function
    itsSputnikProcess->launch(SmartMet::Engine::Sputnik::Frontend, theReactor);

    // Start the "Catcher in the Rye" process in SmartMet core
    theReactor->setNoMatchHandler(boost::bind(&HTTP::requestHandler, this, _1, _2, _3));

    // Get hold of the reactor pointer
    this->itsReactor = theReactor;

    libconfig::Config config;
    unsigned long long memorySize, filesystemSize, uncomMemorySize, uncomFilesystemSize;
    const char *filesystemCachePath, *uncomFilesystemCachePath;

    try
    {
      config.readFile(theConfig);

      config.lookupValue("compressed_cache.memory_bytes", memorySize);
      config.lookupValue("compressed_cache.filesystem_bytes", filesystemSize);
      config.lookupValue("compressed_cache.directory", filesystemCachePath);
      config.lookupValue("uncompressed_cache.memory_bytes", uncomMemorySize);
      config.lookupValue("uncompressed_cache.filesystem_bytes", uncomFilesystemSize);
      config.lookupValue("uncompressed_cache.directory", uncomFilesystemCachePath);
    }
    catch (libconfig::ParseException &e)
    {
      throw SmartMet::Spine::Exception(BCP,
                                       std::string("Configuration error ' ") + e.getError() +
                                           "' on line " +
                                           boost::lexical_cast<std::string>(e.getLine()));
    }
    catch (libconfig::ConfigException &)
    {
      throw SmartMet::Spine::Exception(BCP, "Configuration error");
    }
    catch (...)
    {
      throw SmartMet::Spine::Exception(BCP, "Configuration error!", NULL);
    }

    itsProxy = boost::make_shared<Proxy>(memorySize,
                                         filesystemSize,
                                         boost::filesystem::path(filesystemCachePath),
                                         uncomMemorySize,
                                         uncomFilesystemSize,
                                         boost::filesystem::path(uncomFilesystemCachePath));
  }
  catch (...)
  {
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
  }
}

HTTP::~HTTP()
{
  try
  {
    // Banner
    std::cout << "\t+ HTTP plugin shutting down" << std::endl;

    // Must remove the Catcher in the Rye hook from SmartMet core
    // to avoid calling unloaded code.
    this->itsReactor->setNoMatchHandler(0);

    // Close Sputnik instance
    // delete itsSputnikProcess;
  }
  catch (...)
  {
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
  }
}

void HTTP::shutdown()
{
  try
  {
    std::cout << "  -- Shutdown requested (HTTP)\n";
    itsProxy->shutdown();
  }
  catch (...)
  {
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
  }
}

}  // namespace Frontend
}  // namespace Plugin
}  // namespace SmartMet
