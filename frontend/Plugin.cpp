// ======================================================================
/*!
 * \brief SmartMet Frontend plugin implementation
 */
// ======================================================================

#include "Plugin.h"
#include "HTTP.h"

#include <engines/sputnik/Engine.h>
#include <engines/sputnik/Services.h>
#include <spine/Convenience.h>
#include <spine/Table.h>
#include <spine/TableFormatterFactory.h>
#include <spine/TableFormatterOptions.h>
#include <spine/ParameterFactory.h>
#include <spine/SmartMet.h>
#include <spine/Exception.h>

#include <macgyver/TimeFormatter.h>

#include <json_spirit/json_spirit.h>

#include <boost/bind.hpp>
#include <boost/asio.hpp>
#include <boost/foreach.hpp>
#include <boost/algorithm/string.hpp>

#include <sstream>
#include <stdexcept>
#include <utility>

using namespace std;
using namespace boost::posix_time;
namespace bip = boost::asio::ip;

namespace SmartMet
{
namespace Plugin
{
namespace Frontend
{
struct QEngineFile;

// QEngine reporting typedefs
typedef std::list<QEngineFile> ProducerFiles;               // list of files per producer
typedef std::map<std::string, ProducerFiles> BackendFiles;  // key is producer
typedef std::map<std::string, BackendFiles> AllFiles;

struct QEngineFile
{
  string producer;
  list<string> aliases;
  string refreshInterval;
  string path;
  list<string> parameters;
  string projection;
  string originTime;
  string minTime;
  string maxTime;

  QEngineFile(const string &theProducer,
              const string thePath,
              const list<string> &theParameters,
              const string &theOriginTime,
              const string &theMinTime,
              const string &theMaxTime)
      : producer(theProducer),
        path(thePath),
        parameters(theParameters),
        originTime(theOriginTime),
        minTime(theMinTime),
        maxTime(theMaxTime)
  {
  }

  QEngineFile() : producer(), path(), parameters(), originTime(), minTime(), maxTime() {}
};

QEngineFile buildQEngineFile(const json_spirit::mObject &jsonObject)
{
  try
  {
    std::string producer = jsonObject.find("Producer")->second.get_value<std::string>();
    std::string path = jsonObject.find("Path")->second.get_value<std::string>();
    std::string originTime = jsonObject.find("OriginTime")->second.get_value<std::string>();
    std::string minTime = jsonObject.find("MinTime")->second.get_value<std::string>();
    std::string maxTime = jsonObject.find("MaxTime")->second.get_value<std::string>();
    string params = jsonObject.find("Parameters")->second.get_value<std::string>();

    list<string> paramlist;

    boost::algorithm::split(
        paramlist, params, boost::algorithm::is_any_of(" ,"), boost::token_compress_on);

    return QEngineFile(producer, path, paramlist, originTime, minTime, maxTime);
  }
  catch (...)
  {
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
  }
}

bool qEngineSort(const QEngineFile &first, const QEngineFile &second)
{
  try
  {
    if (first.originTime != second.originTime)
      return first.originTime < second.originTime;
    else
      return (first.path < second.path);
  }
  catch (...)
  {
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
  }
}

bool producerHasParam(const QEngineFile &file, const string &param)
{
  try
  {
    for (auto it = file.parameters.begin(); it != file.parameters.end(); ++it)
    {
      if (param == *it)
      {
        return true;
      }
    }

    return false;
  }
  catch (...)
  {
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Reply to / requests
 */
// ----------------------------------------------------------------------

// this is the content handler for URL /
void baseContentHandler(SmartMet::Spine::Reactor & /* theReactor */,
                        const SmartMet::Spine::HTTP::Request & /* theRequest */,
                        SmartMet::Spine::HTTP::Response &theResponse)
{
  try
  {
    theResponse.setStatus(SmartMet::Spine::HTTP::Status::ok);
    theResponse.setContent("SmartMet Server\n");
  }
  catch (...)
  {
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Reply to sleep requests
 */
// ----------------------------------------------------------------------

#ifndef NDEBUG
void sleep(SmartMet::Spine::Reactor & /* theReactor */,
           const SmartMet::Spine::HTTP::Request &theRequest,
           SmartMet::Spine::HTTP::Response &theResponse)
{
  try
  {
    unsigned long t = SmartMet::Spine::optional_unsigned_long(theRequest.getParameter("t"), 1);

    if (t > 0)
      ::sleep(t);

    theResponse.setStatus(SmartMet::Spine::HTTP::Status::ok);
    theResponse.setContent("SmartMet Server\n");
  }
  catch (...)
  {
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
  }
}
#endif

// ----------------------------------------------------------------------
/*!
 * \brief Perform a clusterinfo query
 */
// ----------------------------------------------------------------------

pair<string, bool> requestClusterInfo(SmartMet::Spine::Reactor &theReactor,
                                      const SmartMet::Spine::HTTP::Request & /* theRequest */,
                                      SmartMet::Spine::HTTP::Response & /* theResponse */)
{
  try
  {
    ostringstream out;

    auto engine = theReactor.getSingleton("Sputnik", NULL);
    if (!engine)
    {
      out << "Sputnik engine is not available" << endl;
      return make_pair(out.str(), false);
    }

    auto *sputnik = reinterpret_cast<SmartMet::Engine::Sputnik::Engine *>(engine);
    sputnik->status(out);
    return make_pair(out.str(), true);
  }
  catch (...)
  {
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Perform a backends query
 */
// ----------------------------------------------------------------------

pair<string, bool> requestBackendInfo(SmartMet::Spine::Reactor &theReactor,
                                      const SmartMet::Spine::HTTP::Request &theRequest,
                                      SmartMet::Spine::HTTP::Response & /* theResponse */)
{
  try
  {
    string service = SmartMet::Spine::optional_string(theRequest.getParameter("service"), "");
    string format = SmartMet::Spine::optional_string(theRequest.getParameter("format"), "debug");

    ostringstream out;

    auto engine = theReactor.getSingleton("Sputnik", NULL);
    if (!engine)
    {
      out << "Sputnik engine is not available" << endl;
      return make_pair(out.str(), false);
    }

    auto *sputnik = reinterpret_cast<SmartMet::Engine::Sputnik::Engine *>(engine);

    boost::shared_ptr<SmartMet::Spine::Table> table = sputnik->backends(service);

    boost::shared_ptr<SmartMet::Spine::TableFormatter> formatter(
        SmartMet::Spine::TableFormatterFactory::create(format));
    SmartMet::Spine::TableFormatter::Names names;
    names.push_back("Backend");
    names.push_back("IP");
    names.push_back("Port");

    formatter->format(out, *table, names, theRequest, SmartMet::Spine::TableFormatterOptions());

    return make_pair(out.str(), true);
  }
  catch (...)
  {
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Find latest spine QEngine contents and build output
 */
// ----------------------------------------------------------------------

BackendFiles buildSpineQEngineContents(
    const std::list<std::pair<std::string, std::string> > &backendContents)
{
  try
  {
    AllFiles theFiles;

    BOOST_FOREACH (auto &contentPair, backendContents)
    {
      // Individual backend contents
      BackendFiles theseFiles;

      json_spirit::mValue jvalue;

      if (!read(contentPair.second, jvalue))
      {
        throw SmartMet::Spine::Exception(BCP, "JSON deserialization failed");
      }

      auto toplevelarray = jvalue.get_array();
      BOOST_FOREACH (auto &producerValue, toplevelarray)
      {
        auto &jsonObject = producerValue.get_obj();

        QEngineFile thisFile = buildQEngineFile(jsonObject);

        BackendFiles::iterator it = theseFiles.find(thisFile.producer);
        if (it == theseFiles.end())
        {
          ProducerFiles thisProducer;
          thisProducer.push_back(thisFile);
          theseFiles.insert(make_pair(thisFile.producer, thisProducer));
        }
        else
        {
          it->second.push_back(thisFile);
        }
      }

      theFiles.insert(std::make_pair(contentPair.first, theseFiles));
    }

    // Find maximum size of the file list
    std::size_t maxsize = 0;
    for (auto backendIt = theFiles.begin(); backendIt != theFiles.end(); ++backendIt)
    {
      for (auto producerIt = backendIt->second.begin(); producerIt != backendIt->second.end();
           ++producerIt)
      {
        maxsize = std::max(maxsize, producerIt->second.size());
      }
    }

    BackendFiles spineFiles;

    for (auto backendIt = theFiles.begin(); backendIt != theFiles.end(); ++backendIt)
    {
      for (auto producerIt = backendIt->second.begin(); producerIt != backendIt->second.end();
           ++producerIt)
      {
        BackendFiles::iterator outputIt = spineFiles.find(producerIt->first);
        if (outputIt == spineFiles.end())
        {
          spineFiles.insert(make_pair(producerIt->first, producerIt->second));
        }
        else
        {
          ProducerFiles tempResult(maxsize);
          auto last_modified = std::set_intersection(outputIt->second.begin(),
                                                     outputIt->second.end(),
                                                     producerIt->second.begin(),
                                                     producerIt->second.end(),
                                                     tempResult.begin(),
                                                     qEngineSort);
          outputIt->second = ProducerFiles(tempResult.begin(), last_modified);
        }
      }
    }

    return spineFiles;
  }
  catch (...)
  {
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Gets backend QEngine contents as pair<Backend name, Json-serial>
 */
// ----------------------------------------------------------------------

list<pair<string, string> > getBackendQEngineStatuses(SmartMet::Spine::Reactor &theReactor)
{
  try
  {
    // The presence of pointforecast means QEngine is running
    string service = "pointforecast";

    auto engine = theReactor.getSingleton("Sputnik", NULL);
    if (!engine)
    {
      throw SmartMet::Spine::Exception(BCP, "Sputnik service discovery not available");
    }

    auto *sputnik = reinterpret_cast<SmartMet::Engine::Sputnik::Engine *>(engine);

    // Get the backends which provide the requested service
    auto backendList = sputnik->getBackendList(service);  // type is SmartMet::Services::BackendList

    // Get Qengine status from backends
    boost::asio::io_service io_service;
    bip::tcp::resolver resolver(io_service);
    std::list<std::pair<std::string, std::string> > qEngineContentList;
    BOOST_FOREACH (auto &backend, backendList)
    {
      bip::tcp::resolver::query query(backend.get<1>(),
                                      boost::lexical_cast<std::string>(backend.get<2>()));
      bip::tcp::resolver::iterator endpoint = resolver.resolve(query);

      bip::tcp::socket socket(io_service);
      boost::asio::connect(socket, endpoint);

      boost::asio::streambuf request;
      std::ostream request_stream(&request);
      //	  request_stream << "GET " << "/" << backend.get<0>() <<
      //"/admin?what=qengine&format=json"
      //<< " HTTP/1.0\r\n";
      request_stream << "GET "
                     << "/admin?what=qengine&format=json"
                     << " HTTP/1.0\r\n";
      request_stream << "Accept: */*\r\n";
      request_stream << "Connection: close\r\n\r\n";

      //		  std::cout << text.str() << std::endl;

      boost::asio::write(socket, request);

      boost::asio::streambuf response;
      boost::system::error_code error;
      while (boost::asio::read(socket, response, boost::asio::transfer_at_least(1), error))
      {
        if (error == boost::asio::error::eof)  // Reads until socket is closed
          break;

        // HERE WE NEED ERROR HANDLING
      }

      std::stringstream responseStream;
      responseStream << &response;

      string rawResponse = responseStream.str();
      size_t bodyStart = rawResponse.find("\r\n\r\n");
      string body = rawResponse.substr(bodyStart);

      qEngineContentList.push_back(std::make_pair(backend.get<0>(), body));
    }

    return qEngineContentList;
  }
  catch (...)
  {
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Request producers which provide given parameter
 */
// ----------------------------------------------------------------------

pair<string, bool> requestQEngineStatus(SmartMet::Spine::Reactor &theReactor,
                                        const SmartMet::Spine::HTTP::Request &theRequest,
                                        SmartMet::Spine::HTTP::Response & /* theResponse */)
{
  try
  {
    string inputType = SmartMet::Spine::optional_string(theRequest.getParameter("type"), "name");
    string format = SmartMet::Spine::optional_string(theRequest.getParameter("format"), "debug");

    // This contains the found producers
    list<QEngineFile> iHasAllParameters;
    string input = SmartMet::Spine::optional_string(theRequest.getParameter("param"), "");

    std::size_t tokens = 0;
    vector<string> paramTokens;

    if (input != "")
    {
      // Parse input parameter names
      boost::algorithm::split(
          paramTokens, input, boost::algorithm::is_any_of(","), boost::token_compress_on);
      tokens = paramTokens.size();
    }

    // Obtain backend QEngine statuses
    list<pair<string, string> > qEngineContentList = getBackendQEngineStatuses(theReactor);
    BackendFiles result = buildSpineQEngineContents(qEngineContentList);

    if (tokens == 0)
    {
      // Zero parameter tokens, print list of all spine producers
      // Build the result table
      ostringstream out;
      SmartMet::Spine::Table table;
      std::size_t row = 0;
      BOOST_FOREACH (auto &pair, result)
      {
        std::size_t column = 0;

        table.set(column, row, pair.first);
        ++column;

        table.set(column, row, (--pair.second.end())->path);
        ++column;

        table.set(column, row, (--pair.second.end())->originTime);
        ++column;

        table.set(column, row, (--pair.second.end())->minTime);
        ++column;

        table.set(column, row, (--pair.second.end())->maxTime);

        ++row;
      }

      SmartMet::Spine::TableFormatter::Names theNames;
      theNames.push_back("Producer");
      theNames.push_back("Path");
      theNames.push_back("OriginTime");
      theNames.push_back("MinTime");
      theNames.push_back("MaxTime");

      std::unique_ptr<SmartMet::Spine::TableFormatter> formatter(
          SmartMet::Spine::TableFormatterFactory::create(format));
      formatter->format(out, table, theNames, theRequest, SmartMet::Spine::TableFormatterOptions());

      return make_pair(out.str(), true);
    }

    else
    {
      // There are some parameter tokens, return spine producers providing these parameters
      if (inputType == "name")
      {
        BOOST_FOREACH (auto &pair, result)
        {
          // Check if producer contains parameter
          // Get latest file
          QEngineFile &latest = *(--pair.second.end());
          unsigned int matches = 0;
          BOOST_FOREACH (auto &param, paramTokens)
          {
            if (producerHasParam(latest, param))
            {
              ++matches;
            }
          }

          if (matches == tokens)
          // Has all specified parameters
          {
            iHasAllParameters.push_back(latest);
          }
        }
      }
      else if (inputType == "id")
      {
        BOOST_FOREACH (auto &pair, result)
        {
          // Check if producer contains parameter
          // Get latest file
          QEngineFile &latest = *(--pair.second.end());
          unsigned int matches = 0;
          BOOST_FOREACH (auto &param, paramTokens)
          {
            // If param type is id, but input cannot be cast into int, simply ignore
            int paramId;
            try
            {
              paramId = boost::lexical_cast<int>(param);
            }
            catch (bad_cast &)
            {
              ++matches;
              continue;
            }
            std::string paramString = SmartMet::Spine::ParameterFactory::instance().name(paramId);
            if (producerHasParam(latest, paramString))
            {
              ++matches;
            }
          }

          if (matches == tokens)
          // Has all specified parameters
          {
            iHasAllParameters.push_back(latest);
          }
        }
      }
      else
      {
        ostringstream oss;
        oss << "Invalid input type " << inputType;
        throw SmartMet::Spine::Exception(BCP, oss.str());
      }

      // Sort results by origintime
      iHasAllParameters.sort(boost::bind(qEngineSort, _2, _1));

      // Build result table
      SmartMet::Spine::Table table;
      std::size_t row = 0;
      BOOST_FOREACH (auto &file, iHasAllParameters)
      {
        std::size_t column = 0;

        table.set(column, row, file.producer);
        ++column;

        table.set(column, row, file.path);
        ++column;

        table.set(column, row, file.originTime);
        ++column;

        ++row;
      }

      SmartMet::Spine::TableFormatter::Names theNames;
      theNames.push_back("Producer");
      theNames.push_back("Path");
      theNames.push_back("OriginTime");

      ostringstream out;

      std::unique_ptr<SmartMet::Spine::TableFormatter> formatter(
          SmartMet::Spine::TableFormatterFactory::create(format));
      formatter->format(out, table, theNames, theRequest, SmartMet::Spine::TableFormatterOptions());

      return make_pair(out.str(), true);
    }
  }
  catch (...)
  {
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Perform an Admin query
 */
// ----------------------------------------------------------------------

pair<string, bool> Plugin::request(SmartMet::Spine::Reactor &theReactor,
                                   const SmartMet::Spine::HTTP::Request &theRequest,
                                   SmartMet::Spine::HTTP::Response &theResponse)
{
  try
  {
    // Check that incoming IP is in the whitelist

    string what = SmartMet::Spine::optional_string(theRequest.getParameter("what"), "");

    if (what.empty())
      return make_pair("No request specified", false);

    if (what == "clusterinfo")
      return requestClusterInfo(theReactor, theRequest, theResponse);

    if (what == "backends")
      return requestBackendInfo(theReactor, theRequest, theResponse);

    if (what == "qengine")
      return requestQEngineStatus(theReactor, theRequest, theResponse);

    return make_pair("Unknown request: '" + what + "'", false);
  }
  catch (...)
  {
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Plugin constructor
 */
// ----------------------------------------------------------------------

Plugin::Plugin(SmartMet::Spine::Reactor *theReactor, const char *theConfig)
    : SmartMetPlugin(), itsModuleName("Frontend")
{
  try
  {
    if (theReactor->getRequiredAPIVersion() != SMARTMET_API_VERSION)
      throw SmartMet::Spine::Exception(BCP, "Frontend and Server API version mismatch");

    itsHTTP = new SmartMet::Plugin::Frontend::HTTP(theReactor, theConfig);

    if (!theReactor->addContentHandler(
            this, "/admin", boost::bind(&Plugin::callRequestHandler, this, _1, _2, _3)))
      throw SmartMet::Spine::Exception(BCP, "Failed to register admin content handler");

    if (!theReactor->addContentHandler(this, "/", boost::bind(&baseContentHandler, _1, _2, _3)))
      throw SmartMet::Spine::Exception(BCP, "Failed to register base content handler");

#ifndef NDEBUG
    if (!theReactor->addContentHandler(this, "/sleep", boost::bind(&sleep, _1, _2, _3)))
      throw SmartMet::Spine::Exception(BCP, "Failed to register sleep content handler");
#endif
  }
  catch (...)
  {
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Destructor
 */
// ----------------------------------------------------------------------

Plugin::~Plugin()
{
  try
  {
    delete itsHTTP;
  }
  catch (...)
  {
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Initializator (trivial)
 */
// ----------------------------------------------------------------------

void Plugin::init()
{
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
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Return the plugin name
 */
// ----------------------------------------------------------------------

const string &Plugin::getPluginName() const
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

void Plugin::requestHandler(SmartMet::Spine::Reactor &theReactor,
                            const SmartMet::Spine::HTTP::Request &theRequest,
                            SmartMet::Spine::HTTP::Response &theResponse)
{
  try
  {
    // Default expiration time

    const int expires_seconds = 1;
    bool isdebug = false;

    // Now

    ptime t_now = second_clock::universal_time();

    try
    {
      pair<string, bool> response = request(theReactor, theRequest, theResponse);

      if (response.second)
      {
        theResponse.setStatus(SmartMet::Spine::HTTP::Status::ok);
      }
      else
      {
        theResponse.setStatus(SmartMet::Spine::HTTP::Status::not_implemented);
      }

      // Make the response HTML in debug mode

      string format = SmartMet::Spine::optional_string(theRequest.getParameter("format"), "debug");

      string ret = response.first;
      if (format == "debug")
      {
        isdebug = true;
        ret = ("<html><head><title>SmartMet Admin</title></head><body>" + ret + "</body></html>");
      }
      theResponse.setContent(ret);

      boost::shared_ptr<SmartMet::Spine::TableFormatter> formatter(
          SmartMet::Spine::TableFormatterFactory::create(format));
      theResponse.setHeader("Content-Type", formatter->mimetype().c_str());

      // Build cache expiration time info

      ptime t_expires = t_now + seconds(expires_seconds);

      // The headers themselves

      boost::shared_ptr<Fmi::TimeFormatter> tformat(Fmi::TimeFormatter::create("http"));

      string cachecontrol = "public, max-age=" + boost::lexical_cast<std::string>(expires_seconds);
      string expiration = tformat->format(t_expires);
      string modification = tformat->format(t_now);

      theResponse.setHeader("Cache-Control", cachecontrol.c_str());
      theResponse.setHeader("Expires", expiration.c_str());
      theResponse.setHeader("Last-Modified", modification.c_str());

      if (response.first.size() == 0)
      {
        cerr << "Warning: Empty input for request " << theRequest.getQueryString() << " from "
             << theRequest.getClientIP() << endl;
      }

#ifdef MYDEBUG
      cout << "Output:" << endl << response << endl;
#endif
    }
    /*
     * Cannot find a source that is actually throwing this exception
    catch (SmartMet::Spine::Exceptions::NotAuthorizedError &)
    {
      // Blocked by ip filter, masquerade as bad request
      theResponse.setStatus(SmartMet::Spine::HTTP::Status::bad_request, true);
      cerr << boost::posix_time::second_clock::local_time()
           << " Attempt to access frontend admin from " << theRequest.getClientIP()
           << ". Not in whitelist." << endl;
    }
    */
    catch (...)
    {
      // Catching all exceptions

      SmartMet::Spine::Exception exception(BCP, "Request processing exception!", NULL);
      exception.addParameter("URI", theRequest.getURI());

      std::cerr << exception.getStackTrace();

      if (isdebug)
      {
        // Delivering the exception information as HTTP content
        std::string fullMessage = exception.getHtmlStackTrace();
        theResponse.setContent(fullMessage);
        theResponse.setStatus(SmartMet::Spine::HTTP::Status::ok);
      }
      else
      {
        theResponse.setStatus(SmartMet::Spine::HTTP::Status::bad_request);
      }

      // Adding the first exception information into the response header

      std::string firstMessage = exception.what();
      boost::algorithm::replace_all(firstMessage, "\n", " ");
      firstMessage = firstMessage.substr(0, 300);
      theResponse.setHeader("X-Frontend-Error", firstMessage.c_str());
    }
#if 0
    catch (...)
    {
      cerr << boost::posix_time::second_clock::local_time() << " error: " << e.what() << endl
           << "Query: " << theRequest.getURI() << endl;

      string msg = string("Error: ") + e.what();
      theResponse.setContent(msg);
      theResponse.setStatus(SmartMet::Spine::HTTP::Status::internal_server_error);
      // Remove newlines, make sure length is reasonable
      boost::algorithm::replace_all(msg, "\n", " ");
      msg = msg.substr(0, 100);
      theResponse.setHeader("X-Admin-Error", msg.c_str());
    }
    catch (...)
    {
      cerr << boost::posix_time::second_clock::local_time() << " error: "
           << "Unknown exception" << endl
           << "Query: " << theRequest.getURI() << endl;

      theResponse.setHeader("X-Admin-Error", "Unknown exception");
      string msg = "Error: Unknown exception";
      theResponse.setContent(msg);
      theResponse.setStatus(SmartMet::Spine::HTTP::Status::internal_server_error);
    }
#endif
  }
  catch (...)
  {
    throw SmartMet::Spine::Exception(BCP, "Operation failed!", NULL);
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Performance query implementation.
 *
 * Frontend must always respond quickly to F5
 */
// ----------------------------------------------------------------------

bool Plugin::queryIsFast(const SmartMet::Spine::HTTP::Request & /* theRequest */) const
{
  return true;
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
