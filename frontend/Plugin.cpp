// ======================================================================
/*!
 * \brief SmartMet Frontend plugin implementation
 */
// ======================================================================

#include "Plugin.h"
#include "HTTP.h"
#include "info/BackendInfoRequests.h"
#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/lexical_cast.hpp>
#include <engines/sputnik/Engine.h>
#include <engines/sputnik/Services.h>
#include <fmt/format.h>
#include <grid-files/common/GeneralFunctions.h>
#include <json/json.h>
#include <macgyver/Base64.h>
#include <macgyver/Exception.h>
#include <macgyver/Join.h>
#include <macgyver/StringConversion.h>
#include <macgyver/TimeFormatter.h>
#include <spine/ConfigTools.h>
#include <spine/Convenience.h>
#include <spine/Exceptions.h>
#include <spine/HostInfo.h>
#include <spine/SmartMet.h>
#include <spine/Table.h>
#include <spine/TableFormatterFactory.h>
#include <spine/TableFormatterOptions.h>
#include <spine/TcpMultiQuery.h>
#include <timeseries/ParameterFactory.h>
#include <random>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace SmartMet
{
namespace Plugin
{
namespace Frontend
{

// ----------------------------------------------------------------------
/*!
 * \brief Reply to / requests
 */
// ----------------------------------------------------------------------

// this is the content handler for URL /
void Plugin::baseContentHandler(Spine::Reactor & /* theReactor */,
                                const Spine::HTTP::Request & /* theRequest */,
                                Spine::HTTP::Response &theResponse)
{
  try
  {
    // Must not use word "SmartMet" in paused state, which F5 uses for pattern matching

    theResponse.setStatus(Spine::HTTP::Status::ok);
    if (!isPaused())
      theResponse.setContent("SmartMet Server\n");
    else
    {
      Spine::ReadLock lock(itsPauseMutex);
      if (!itsPauseDeadLine)
        theResponse.setContent("Frontend Paused\n");
      else
        theResponse.setContent("Frontend Paused until " + Fmi::to_iso_string(*itsPauseDeadLine));
    }
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Reply to sleep requests
 */
// ----------------------------------------------------------------------

#ifndef NDEBUG
void sleep(Spine::Reactor & /* theReactor */,
           const Spine::HTTP::Request &theRequest,
           Spine::HTTP::Response &theResponse)
{
  try
  {
    unsigned long t = Spine::optional_unsigned_long(theRequest.getParameter("t"), 1);

    if (t > 0)
      boost::this_thread::sleep(boost::posix_time::seconds(t));

    theResponse.setStatus(Spine::HTTP::Status::ok);
    theResponse.setContent("SmartMet Server\n");
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}
#endif

// ----------------------------------------------------------------------
/*!
 * \brief Gets Sputnik engine pointer or thhrow and exception if not available
 */
// ----------------------------------------------------------------------

Engine::Sputnik::Engine* Frontend::Plugin::getSputnikEngine()
{
    return itsSputnikEngine.get();
}


// ----------------------------------------------------------------------
/*!
 * \brief Perform a clusterinfo query
 */
// ----------------------------------------------------------------------

void Frontend::Plugin::requestClusterInfo(Spine::Reactor& theReactor,
                                          const Spine::HTTP::Request& theRequest,
                                          Spine::HTTP::Response& theResponse)
try
{
    const std::optional<std::string> adminUri = theReactor.getAdminUri();
    const bool full = adminUri && theRequest.getResource() == *adminUri;

    std::string content = "<html><head><title>Cluster info</title></head><body>";

    std::ostringstream out;
    auto* sputnik = getSputnikEngine();
    sputnik->status(out, full);
    theResponse.setContent(out.str());
    theResponse.setHeader("Content-Type", "text/html; charset=utf-8");
    theResponse.setStatus(Spine::HTTP::Status::ok);
}
catch (...)
{
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
}

// ----------------------------------------------------------------------
/*!
 * \brief Perform a backends query
 */
// ----------------------------------------------------------------------

std::unique_ptr<Spine::Table>
Frontend::Plugin::requestBackendInfo(Spine::Reactor& theReactor,
                                     const Spine::HTTP::Request& theRequest)
{
    auto *sputnik = getSputnikEngine();
    const std::optional<std::string> adminUri = theReactor.getAdminUri();
    const bool full = adminUri && theRequest.getResource() == *adminUri;
    std::string service = Spine::optional_string(theRequest.getParameter("service"), "");
    return sputnik->backends(service, full);
}


// ----------------------------------------------------------------------
/*!
 * \brief Perform an active backends query
 */
// ----------------------------------------------------------------------

std::unique_ptr<Spine::Table>
Plugin::requestActiveBackends(Spine::Reactor &theReactor,
                              const Spine::HTTP::Request &theRequest)
{
  try
  {
    const std::optional<std::string> adminUri = theReactor.getAdminUri();
    const bool full = adminUri && theRequest.getResource() == *adminUri;

    std::unique_ptr<Spine::Table> reqTable = std::make_unique<Spine::Table>();

    // Obtain logging information
    auto backends = theReactor.getBackendRequestStatus();

    std::size_t row = 0;
    for (const auto &backend_port : backends)
    {
      const auto &host = backend_port.first;
      for (const auto &port_count : backend_port.second)
      {
        auto port = port_count.first;
        auto count = port_count.second;

        std::size_t column = 0;
        reqTable->set(column++, row, host);
        if (full)
        {
          reqTable->set(column++, row, Fmi::to_string(port));
          reqTable->set(column++, row, Fmi::to_string(count));
        }
        ++row;
      }
    }

    if (full)
      reqTable->setNames({"Host", "Port", "Count"});
    else
      reqTable->setNames({"Host"});

    reqTable->setTitle("Active backends");
    return reqTable;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Forward info request to a randomly selected backend that supports it
 */
// ----------------------------------------------------------------------

void Plugin::requestNoMatchInfo(Spine::Reactor& theReactor,
                                const Spine::HTTP::Request& theRequest,
                                Spine::HTTP::Response& theResponse)
{
  try
  {
    // Get the info request name from the query parameters
    std::string infoRequestName = Spine::optional_string(theRequest.getParameter("what"), "");

    if (infoRequestName.empty())
    {
      theResponse.setStatus(Spine::HTTP::Status::bad_request);
      theResponse.setContent("Missing 'what' parameter for info request");
      return;
    }

    auto* sputnik = getSputnikEngine();

    // Get the list of backends that support this info request
    auto backendList = sputnik->getServices().getInfoRequestBackendList(infoRequestName);

    if (backendList.empty())
    {
      theResponse.setStatus(Spine::HTTP::Status::not_found);
      theResponse.setContent("Not found");
      return;
    }

    // Randomly select a backend from the list
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, backendList.size() - 1);
    auto it = backendList.begin();
    std::advance(it, dis(gen));

    const auto& backend = *it;
    std::string backendHost = backend.get<1>();
    int backendPort = backend.get<2>();

    // Forward the request to the selected backend
    auto proxyStatus = itsHTTP->getProxy()->HTTPForward(
        theReactor,
        theRequest,
        theResponse,
        backendHost,
        backendPort,
        theRequest.getResource(),
        backendHost);

    if (proxyStatus != Proxy::ProxyStatus::PROXY_SUCCESS)
    {
      theResponse.setStatus(Spine::HTTP::Status::bad_gateway);
      theResponse.setContent("Failed to forward request to backend: " + backendHost + ":" + std::to_string(backendPort));
    }
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}


// ----------------------------------------------------------------------
/*!
 * \brief Request producers which provide given parameter
 */
// ----------------------------------------------------------------------

void
Plugin::requestBackendInfoSummary(const Spine::HTTP::Request &theRequest,
                                  Spine::HTTP::Response &theResponse)
try
{
  const std::string what = Spine::optional_string(theRequest.getParameter("what"), "");
  if (what.empty())
  {
    throw Fmi::Exception(BCP, "Missing 'what' parameter for backend info request");
  }

  const std::string format = Spine::optional_string(theRequest.getParameter("format"), "debug");
  const std::string style = Spine::optional_string(theRequest.getParameter("style"), "default");
  const std::string timeFormat = Spine::optional_string(theRequest.getParameter("timeformat"), "sql");

  //---------------------------------------------------------------------------------
  // Get all backends that provide the requested info and erase dumplicated
  //---------------------------------------------------------------------------------
  auto sputnik = itsReactor->getEngine<Engine::Sputnik::Engine>("Sputnik", nullptr);
  auto backendList = sputnik->getInfoRequestBackendList(what);
  std::vector<decltype(backendList)::value_type> backends(backendList.begin(), backendList.end());
  std::sort(backends.begin(),
            backends.end(),
            [](const auto &lhs, const auto &rhs)
            {
              if (boost::get<1>(lhs) != boost::get<1>(rhs))
                return boost::get<1>(lhs) < boost::get<1>(rhs);
              return boost::get<2>(lhs) < boost::get<2>(rhs);
            });
  auto last = std::unique(
      backends.begin(),
      backends.end(),
      [](const auto &lhs, const auto &rhs)
      {
        return boost::get<1>(lhs) == boost::get<1>(rhs) && boost::get<2>(lhs) == boost::get<2>(rhs);
      });
  backends.erase(last, backends.end());

  //---------------------------------------------------------------------------------
  // Build and perform backend info requests and finallt generate the summary response
  //---------------------------------------------------------------------------------
  BackendInfoRequests backendInfoRequests;
  std::shared_ptr<BackendInfoResponse> response =
    backendInfoRequests.perform_backend_info_request(backends, theRequest);

  // Format the response table
  if (style == "default")
  {
    // Backward compatible output format
    Spine::TableFormatterOptions options;
    std::unique_ptr<Spine::TableFormatter> formatter(Spine::TableFormatterFactory::create(format));
    std::unique_ptr<Spine::Table> table;
    if (response)
    {
      table = response->to_table(timeFormat);
      if (response->get_summary_size() > 1)
      {
        std::string title = table->getTitle().value_or("");
        title +=
            " (summary from " + Fmi::to_string(response->get_summary_size()) + " backends)";
        table->setTitle(title);
      }
    }
    else
    {
      table = std::make_unique<Spine::Table>();
      table->setTitle("Backend info summary (no data)");
    }
    const std::string formattedString = formatter->format(*table, {}, theRequest, options);
    const std::string mime = formatter->mimetype() + "; charset=UTF-8";
    theResponse.setContent(formattedString);
    theResponse.setHeader("Content-Type", mime);
    theResponse.setStatus(Spine::HTTP::Status::ok);
  }
  else if (style == "extended")
  {
    // New extended output format
    Json::Value jsonResponse;
    if (response) jsonResponse = response->as_json(timeFormat);
    Json::StreamWriterBuilder writerBuilder;
    writerBuilder["indentation"] = "  ";  // pretty print
    const std::string formattedString = Json::writeString(writerBuilder, jsonResponse);
    theResponse.setContent(formattedString);
    theResponse.setHeader("Content-Type", "application/json; charset=UTF-8");
    theResponse.setStatus(Spine::HTTP::Status::ok);
  }
  else
  {
        throw Fmi::Exception(BCP, "Invalid style '" + style + "' for backend info request");
  }
}
catch (...)
{
  throw Fmi::Exception::Trace(BCP, "Operation failed!");
}

// ----------------------------------------------------------------------
/*!
 * \brief Pause until the given time
 */
// ----------------------------------------------------------------------

std::string Plugin::pauseUntil(const Fmi::DateTime &theTime)
{
  auto timestr = Fmi::to_iso_string(theTime);
  std::cout << Spine::log_time_str() << " *** Frontend paused until " << timestr << std::endl;

  Spine::WriteLock lock(itsPauseMutex);
  itsPaused = true;
  itsPauseDeadLine = theTime;
  return "Paused Frontend until " + timestr;
}

// ----------------------------------------------------------------------
/*!
 * \brief Request a pause in F5 responses
 */
// ----------------------------------------------------------------------

std::string Plugin::requestPause(const Spine::HTTP::Request &theRequest)
{
  try
  {
    // Optional deadline or duration:

    auto time_opt = theRequest.getParameter("time");
    if (time_opt)
    {
      auto deadline = Fmi::TimeParser::parse(*time_opt);
      return pauseUntil(deadline);
    }

    // Optional duration:

    auto duration_opt = theRequest.getParameter("duration");
    if (duration_opt)
    {
      auto duration = Fmi::TimeParser::parse_duration(*duration_opt);
      auto deadline = Fmi::SecondClock::universal_time() + duration;
      return pauseUntil(deadline);
    }

    std::cout << Spine::log_time_str() << " *** Frontend paused" << std::endl;
    Spine::WriteLock lock(itsPauseMutex);
    itsPaused = true;
    itsPauseDeadLine = std::nullopt;
    return "Paused Frontend";
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Request a continue
 */
// ----------------------------------------------------------------------

std::string Plugin::requestContinue(const Spine::HTTP::Request &theRequest)
{
  try
  {
    // Optional deadline or duration:

    auto time_opt = theRequest.getParameter("time");
    if (time_opt)
    {
      auto deadline = Fmi::TimeParser::parse(*time_opt);
      pauseUntil(deadline);
    }

    // Optional duration:

    auto duration_opt = theRequest.getParameter("duration");
    if (duration_opt)
    {
      auto duration = Fmi::TimeParser::parse_duration(*duration_opt);
      auto deadline = Fmi::SecondClock::universal_time() + duration;
      return pauseUntil(deadline);
    }

    std::cout << Spine::log_time_str() << " *** Frontend continues" << std::endl;
    Spine::WriteLock lock(itsPauseMutex);
    itsPaused = false;
    itsPauseDeadLine = std::nullopt;
    return "Frontend continues";
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Return true if Frontend is paused
 */
// ----------------------------------------------------------------------

bool Plugin::isPaused() const
{
  Spine::UpgradeReadLock readlock(itsPauseMutex);
  if (!itsPaused)
    return false;

  if (!itsPauseDeadLine)
    return true;

  auto now = Fmi::MicrosecClock::universal_time();

  if (now < itsPauseDeadLine)
    return true;

  // deadline expired, continue
  std::cout << Spine::log_time_str() << " *** Frontend pause deadline expired, continuing"
            << std::endl;
  Spine::UpgradeWriteLock writelock(readlock);
  itsPaused = false;
  itsPauseDeadLine = std::nullopt;

  return false;
}

// ----------------------------------------------------------------------
/*!
 * \brief Authenticates the request
 */
// ----------------------------------------------------------------------

bool Plugin::authenticateRequest(const Spine::HTTP::Request &theRequest,
                                 Spine::HTTP::Response &theResponse)
{
  try
  {
    auto credentials = theRequest.getHeader("Authorization");

    if (!credentials)
    {
      theResponse.setStatus(Spine::HTTP::Status::unauthorized);
      theResponse.setHeader("WWW-Authenticate", "Basic realm=\"SmartMet Admin\"");
      theResponse.setHeader("Content-Type", "text/html; charset=UTF-8");

      std::string content = "<html><body><h1>401 Unauthorized </h1></body></html>";
      theResponse.setContent(content);

      return false;
    }

    // Parse user and password

    std::vector<std::string> splitHeader;
    std::string trueDigest;
    std::string givenDigest;

    boost::algorithm::split(
        splitHeader, *credentials, boost::is_any_of(" "), boost::token_compress_on);
    if (splitHeader.size() != 2)
    {
      theResponse.setStatus(Spine::HTTP::Status::unauthorized);
      theResponse.setHeader("WWW-Authenticate", "Basic realm=\"SmartMet Admin\"");
      theResponse.setHeader("Content-Type", "text/html; charset=UTF-8");

      std::string content = "<html><body><h1>401 Unauthorized </h1></body></html>";
      theResponse.setContent(content);

      return false;
    }

    givenDigest = splitHeader[1];  // Second field in the header: ( Basic aHR0cHdhdGNoOmY= )

    trueDigest = Fmi::Base64::encode(itsUsername + ":" + itsPassword);

    // Passwords match
    if (trueDigest == givenDigest)
      return true;  // // Main handler can proceed

    // Wrong password, ask it again
    theResponse.setStatus(Spine::HTTP::Status::unauthorized);
    theResponse.setHeader("WWW-Authenticate", "Basic realm=\"SmartMet Admin\"");
    theResponse.setHeader("Content-Type", "text/html; charset=UTF-8");

    std::string content = "<html><body><h1>401 Unauthorized </h1></body></html>";
    theResponse.setContent(content);
    return false;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Plugin constructor
 */
// ----------------------------------------------------------------------

Plugin::Plugin(Spine::Reactor *theReactor, const char *theConfig)
  : itsReactor(theReactor)
  , itsModuleName("Frontend")
{
  try
  {
    if (theReactor->getRequiredAPIVersion() != SMARTMET_API_VERSION)
      throw Fmi::Exception(BCP, "Frontend and Server API version mismatch");

    try
    {
      libconfig::Config config;

      // Enable sensible relative include paths
      std::filesystem::path p = theConfig;
      p.remove_filename();
      config.setIncludeDir(p.c_str());

      config.readFile(theConfig);
      Spine::expandVariables(config);

      if (!config.lookupValue("user", itsUsername) || !config.lookupValue("password", itsPassword))
        throw Fmi::Exception(BCP, std::string("user or password not set in '") + theConfig + "'");
    }
    catch (...)
    {
      Spine::Exceptions::handle("Frontend plugin");
    }

    itsHTTP.reset(new HTTP(theReactor, theConfig));

    // Only register the admin handler if it does not exist yet (it could be defined
    // by top level Spine::Reactor configuration)
    if (theReactor->hasHandlerView("/admin"))
    {
      std::cout << "Warning: Admin handler already exists, not registering Frontend admin handler"
                << std::endl;
    }
    else
    {
      if (!theReactor->addContentHandler(this,
                                        "/admin",
                                         [this](Spine::Reactor &theReactor,
                                                const Spine::HTTP::Request &theRequest,
                                                Spine::HTTP::Response &theResponse) {
                                           callRequestHandler(theReactor, theRequest, theResponse);
                                         }))
        throw Fmi::Exception(BCP, "Failed to register admin content handler");
    }

    if (!theReactor->addContentHandler(this,
                                       "/",
                                       [this](Spine::Reactor &theReactor,
                                              const Spine::HTTP::Request &theRequest,
                                              Spine::HTTP::Response &theResponse) {
                                         baseContentHandler(theReactor, theRequest, theResponse);
                                       }))
      throw Fmi::Exception(BCP, "Failed to register base content handler");

#ifndef NDEBUG
    if (!theReactor->addContentHandler(this,
                                       "/sleep",
                                       [this](Spine::Reactor &theReactor,
                                              const Spine::HTTP::Request &theRequest,
                                              Spine::HTTP::Response &theResponse)
                                       { sleep(theReactor, theRequest, theResponse); }))
      throw Fmi::Exception(BCP, "Failed to register sleep content handler");
#endif
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Initializator
 */
// ----------------------------------------------------------------------

void Plugin::init()
{
  Spine::Reactor* reactor = Spine::Reactor::instance;
  itsSputnikEngine = reactor->getEngine<Engine::Sputnik::Engine>("Sputnik", nullptr);
  registerAdminRequests(*reactor);
}

// ----------------------------------------------------------------------
/*!
 * \brief Shutdown the plugin
 */
// ----------------------------------------------------------------------

void Plugin::shutdown()
{
  try
  {
    std::cout << "  -- Shutdown requested (frontend)\n";
    itsHTTP->shutdown();
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Return the plugin name
 */
// ----------------------------------------------------------------------

const std::string &Plugin::getPluginName() const
{
  return itsModuleName;
}

// ----------------------------------------------------------------------
/*!
 * \brief Return the required version
 */
// ----------------------------------------------------------------------

int Plugin::getRequiredAPIVersion() const
{
  return SMARTMET_API_VERSION;
}

// ----------------------------------------------------------------------
/*!
 * \brief Provide admin capabilities to the front end
 */
// ----------------------------------------------------------------------

void Plugin::requestHandler(Spine::Reactor &theReactor,
                            const Spine::HTTP::Request &theRequest,
                            Spine::HTTP::Response &theResponse)
{
  try
  {
    using namespace SmartMet::Spine;
    theReactor.executeAdminRequest(
        theRequest,
        theResponse,
        [this](const Spine::HTTP::Request& request, Spine::HTTP::Response& response) -> bool
        {
          return authenticateRequest(request, response);
        });
  }
  catch (...)
  {
    // Not expected to be here: theReactor.executeAdminRequest() processes exceptions
    // and should not throw
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}


void Frontend::Plugin::registerAdminRequests(Spine::Reactor& theReactor)
{
  namespace p = std::placeholders;
  using AdminRequestAccess = Spine::Reactor::AdminRequestAccess;

  if (!theReactor.addAdminCustomRequestHandler(
            this,
            "clusterinfo",
            AdminRequestAccess::Public,
             std::bind(&Frontend::Plugin::requestClusterInfo, this, p::_1, p::_2, p::_3),
             "Get cluster info"))
  {
    throw Fmi::Exception(BCP, "Failed to register clusterinfo request handler");
  }

  if (!theReactor.addAdminTableRequestHandler(
            this,
            "backends",
            AdminRequestAccess::Public,
            std::bind(&Frontend::Plugin::requestBackendInfo, this, p::_1, p::_2),
            "Get backend info"))
  {
    throw Fmi::Exception(BCP, "Failed to register backends request handler");
  }

  if (!theReactor.addAdminCustomRequestHandler(
        this,
        "qengine",
        AdminRequestAccess::Public,
        std::bind(&Plugin::requestBackendInfoSummary, this, p::_2, p::_3),
        "Available querydata"))
  {
    throw Fmi::Exception(BCP, "Failed to register qengine request handler");
  }

  if (!theReactor.addAdminCustomRequestHandler(
        this,
        "gridgenerations",
        AdminRequestAccess::Public,
        std::bind(&Plugin::requestBackendInfoSummary, this, p::_2, p::_3),
        "Available producers"))
  {
    throw Fmi::Exception(BCP, "Failed to register gridgenerations request handler");
  }


  if (!theReactor.addAdminCustomRequestHandler(
        this,
        "gridgenerationsqd",
        AdminRequestAccess::Public,
        std::bind(&Plugin::requestBackendInfoSummary, this, p::_2, p::_3),
        "Available producers"))
  {
    throw Fmi::Exception(BCP, "Failed to register gridgenerationsqd request handler");
  }


  if (!theReactor.addAdminTableRequestHandler(
        this,
        "activebackends",
        AdminRequestAccess::Public,
        std::bind(&Plugin::requestActiveBackends, this, p::_1, p::_2),
        "Active backends"))
    {
        throw Fmi::Exception(BCP, "Failed to register activebackends request handler");
    }

  if (!theReactor.addAdminStringRequestHandler(
        this,
        "pause",
        AdminRequestAccess::RequiresAuthentication,
        std::bind(&Plugin::requestPause, this, p::_2),
        "Pause the frontend"))
  {
        throw Fmi::Exception(BCP, "Failed to register pause request handler");
  }

  if (!theReactor.addAdminStringRequestHandler(
        this,
        "continue",
        AdminRequestAccess::RequiresAuthentication,
        std::bind(&Plugin::requestContinue, this, p::_2),
        "Continue the frontend"))
  {
        throw Fmi::Exception(BCP, "Failed to register continue request handler");
  }

  // Register handler for unrecognized admin/info requests
  // This will forward them directly to backends that support the requested info type
  theReactor.addNoMatchAdminRequestHandler(
      std::bind(&Plugin::requestNoMatchInfo, this, p::_1, p::_2, p::_3));
}


// ----------------------------------------------------------------------
/*!
 * \brief Performance query implementation.
 *
 * Frontend must always respond quickly to F5
 */
// ----------------------------------------------------------------------

bool Plugin::queryIsFast(const Spine::HTTP::Request & /* theRequest */) const
{
  return true;
}

bool Plugin::isAdminQuery(const SmartMet::Spine::HTTP::Request & /* theRequest */) const
{
  return false;
}

Fmi::Cache::CacheStatistics Plugin::getCacheStats() const
{
  Fmi::Cache::CacheStatistics ret;

  const ResponseCache &compressed_cache =
      itsHTTP->getProxy()->getCache(ResponseCache::ContentEncodingType::GZIP);
  const ResponseCache &uncompressed_cache =
      itsHTTP->getProxy()->getCache(ResponseCache::ContentEncodingType::NONE);

  ret.insert(std::make_pair("Frontend::compressed_response_cache::meta_data_cache",
                            compressed_cache.getMetaDataCacheStats()));
  ret.insert(std::make_pair("Frontend::compressed_response_cache::memory_cache",
                            compressed_cache.getMemoryCacheStats()));
  ret.insert(std::make_pair("Frontend::compressed_response_cache::file_cache",
                            compressed_cache.getFileCacheStats()));

  ret.insert(std::make_pair("Frontend::uncompressed_response_cache::meta_data_cache",
                            uncompressed_cache.getMetaDataCacheStats()));
  ret.insert(std::make_pair("Frontend::uncompressed_response_cache::memory_cache",
                            uncompressed_cache.getMemoryCacheStats()));
  ret.insert(std::make_pair("Frontend::uncompressed_response_cache::file_cache",
                            uncompressed_cache.getFileCacheStats()));

  return ret;
}

}  // namespace Frontend
}  // namespace Plugin
}  // namespace SmartMet

/*
 * Server knows us through the 'SmartMetPlugin' virtual interface, which
 * the 'Plugin' class implements.
 */

extern "C" SmartMetPlugin *create(SmartMet::Spine::Reactor *them, const char *config)
{
  return new SmartMet::Plugin::Frontend::Plugin(them, config);
}

extern "C" void destroy(SmartMetPlugin *us)
{
  // This will call 'Plugin::~Plugin()' since the destructor is virtual
  delete us;
}

// ======================================================================
