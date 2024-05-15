#include "HTTP.h"
#include "Proxy.h"
#include <boost/algorithm/string.hpp>
#include <boost/foreach.hpp>
#include <boost/make_shared.hpp>
#include <boost/shared_ptr.hpp>
#include <engines/sputnik/Engine.h>
#include <fmt/format.h>
#include <macgyver/Exception.h>
#include <macgyver/StringConversion.h>
#include <spine/ConfigTools.h>
#include <spine/Convenience.h>
#include <spine/Reactor.h>
#include <iostream>
#include <stdexcept>

namespace ba = boost::algorithm;

namespace SmartMet
{
namespace Plugin
{
namespace Frontend
{
namespace
{
std::size_t parse_size(const libconfig::Setting &setting, const char *name)
{
  switch (setting.getType())
  {
    case libconfig::Setting::TypeInt:
      return static_cast<unsigned int>(setting);
    case libconfig::Setting::TypeInt64:
      return static_cast<unsigned long>(setting);
    case libconfig::Setting::TypeString:
      return Fmi::stosz(static_cast<const char *>(setting));
    default:
      throw Fmi::Exception(BCP, "Invalid type for size setting").addParameter("Setting", name);
  }
}

}  // namespace

Proxy::ProxyStatus HTTP::transport(Spine::Reactor &theReactor,
                                   const Spine::HTTP::Request &theRequest,
                                   Spine::HTTP::Response &theResponse)
{
  try
  {
    // Choose the backend host by URI
    BackendServicePtr theService = itsSputnikProcess->getServices().getService(theRequest);

    if (theService.get() == nullptr)
    {
      // 404 Service Not Found
      theResponse.setStatus(Spine::HTTP::Status::not_found, true);

      return Proxy::ProxyStatus::PROXY_FAIL_SERVICE;
    }

    // BackendServer where we're connecting to
    const std::shared_ptr<BackendServer> theHost = theService->Backend();

    if (!theHost)
    {
      std::cout << fmt::format("{} Service backend value is null", Spine::log_time_str())
                << std::endl;

      // 502 Service Not Found
      theResponse.setStatus(Spine::HTTP::Status::bad_gateway, true);
      return Proxy::ProxyStatus::PROXY_FAIL_SERVICE;
    }

    // See if this backend is set as 'temporarily unconscious'
    if (!itsSputnikProcess->getServices().queryBackendAlive(theHost->Name(), theHost->Port()))
    {
      itsSputnikProcess->getServices().removeBackend(theHost->Name(), theHost->Port());
      theReactor.removeBackendRequests(theHost->Name(), theHost->Port());

      std::cout << fmt::format("{} Backend {}:{} is marked as dead. Retiring backend server.",
                               Spine::log_time_str(),
                               theHost->Name(),
                               theHost->Port())
                << std::endl;

      return Proxy::ProxyStatus::PROXY_FAIL_REMOTE_HOST;
    }

    // Forward the request keeping account of how many active requests each backend has.
    // The destructor of the streamer created by the proxy will decrement the count.

    Proxy::ProxyStatus proxyStatus = Proxy::ProxyStatus::PROXY_SUCCESS;

    // Use Proxy class to forward the request to backend server
    std::string resource = theRequest.getResource();

    const std::string hostName = theHost->Name();
    const std::string hostPrefix = "/" + hostName;

    if (theService->DefinesPrefix())
    {
      if (ba::starts_with(resource, theService->URI()))
      {
        // Direct match IRU prefix: can use initial resource URI.
      }
      else if (ba::starts_with(resource, hostPrefix + "/"))
      {
        // Begins with host prefix + "/". Strip it from resource URI, but leave final '/'
        resource = resource.substr(hostPrefix.length());
      }
      if (not ba::starts_with(resource, theService->URI()))
      {
        // Something unexpected happened.
        std::cout << fmt::format(
                         "{} Request resource '{}' does not beging with either of '{}' and '{}'",
                         Spine::log_time_str(),
                         theRequest.getResource(),
                         theService->URI(),
                         hostPrefix + theService->URI())
                  << std::endl;
        return Proxy::ProxyStatus::PROXY_INTERNAL_ERROR;
      }
    }
    else
    {
      if (resource == theService->URI())
      {
        // Direct match: can use initial resource URI.
      }
      else if (resource == hostPrefix + theService->URI())
      {
        // Host prefix found. Remove it when sending request to backend
        resource = theService->URI();
      }
      else
      {
        // Something unexpected happened.
        std::cout << fmt::format("{} Request resource '{}' is neither of '{}' and '{}'",
                                 Spine::log_time_str(),
                                 theRequest.getResource(),
                                 theService->URI(),
                                 hostPrefix + theService->URI())
                  << std::endl;
        return Proxy::ProxyStatus::PROXY_INTERNAL_ERROR;
      }
    }

    proxyStatus = itsProxy->HTTPForward(theReactor,
                                        theRequest,
                                        theResponse,
                                        theHost->IP(),
                                        theHost->Port(),
                                        resource,
                                        theHost->Name());

    // Check the Proxy status
    if (proxyStatus != Proxy::ProxyStatus::PROXY_SUCCESS)
    {
      // Immediately remove the backend server from the service providing pool
      // if there was a problem connecting to backend server.
      std::cout << fmt::format(
                       "{} Backend Server connection to {}:{} failed, retiring the backend server.",
                       Spine::log_time_str(),
                       theHost->Name(),
                       theHost->Port())
                << std::endl;

      itsSputnikProcess->getServices().removeBackend(theHost->Name(), theHost->Port());
      theReactor.removeBackendRequests(theHost->Name(), theHost->Port());
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
        std::cout << fmt::format("{} Resending URI {}", Spine::log_time_str(), theRequest.getURI())
                  << std::endl;
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
  using namespace boost::placeholders;
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
    theReactor->setNoMatchHandler([this](Spine::Reactor &theReactor,
                                         const Spine::HTTP::Request &theRequest,
                                         Spine::HTTP::Response &theResponse)
                                  { requestHandler(theReactor, theRequest, theResponse); });

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
      Spine::expandVariables(config);

      const char *comp_mem_bytes = "compressed_cache.memory_bytes";
      const char *comp_file_bytes = "compressed_cache.filesystem_bytes";
      const char *uncomp_mem_bytes = "uncompressed_cache.memory_bytes";
      const char *uncomp_file_bytes = "uncompressed_cache.filesystem_bytes";

      config.lookupValue("compressed_cache.directory", filesystemCachePath);
      if (config.exists(comp_mem_bytes))
        memorySize = parse_size(config.lookup(comp_mem_bytes), comp_mem_bytes);
      if (config.exists(comp_file_bytes))
        filesystemSize = parse_size(config.lookup(comp_file_bytes), comp_file_bytes);

      config.lookupValue("uncompressed_cache.directory", uncomFilesystemCachePath);

      if (config.exists(uncomp_mem_bytes))
        uncomMemorySize = parse_size(config.lookup(uncomp_mem_bytes), uncomp_mem_bytes);
      if (config.exists(uncomp_file_bytes))
        uncomFilesystemSize = parse_size(config.lookup(uncomp_file_bytes), uncomp_file_bytes);

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
