// ======================================================================
/*!
 * \brief SmartMet Frontend plugin implementation
 */
// ======================================================================

#include "Plugin.h"
#include "HTTP.h"
#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <engines/sputnik/Engine.h>
#include <engines/sputnik/Services.h>
#include <json/json.h>
#include <macgyver/Base64.h>
#include <macgyver/Exception.h>
#include <macgyver/StringConversion.h>
#include <spine/Convenience.h>
#include <spine/ParameterFactory.h>
#include <spine/SmartMet.h>
#include <spine/Table.h>
#include <spine/TableFormatterFactory.h>
#include <spine/TableFormatterOptions.h>
#include <sstream>
#include <stdexcept>
#include <utility>

using boost::posix_time::ptime;
using boost::posix_time::second_clock;
using boost::posix_time::seconds;

namespace bip = boost::asio::ip;

namespace SmartMet
{
namespace Plugin
{
namespace Frontend
{
struct QEngineFile;

// QEngine reporting types
using ProducerFiles = std::list<QEngineFile>;               // list of files per producer
using BackendFiles = std::map<std::string, ProducerFiles>;  // key is producer
using AllFiles = std::map<std::string, BackendFiles>;

struct QEngineFile
{
  std::string producer;
  std::list<std::string> aliases;
  std::string refreshInterval;
  std::string path;
  std::list<std::string> parameters;
  std::string projection;
  std::string originTime;
  std::string minTime;
  std::string maxTime;

  QEngineFile(std::string theProducer,
              std::string thePath,
              std::list<std::string> theParameters,
              std::string theOriginTime,
              std::string theMinTime,
              std::string theMaxTime)
      : producer(std::move(theProducer)),
        path(std::move(thePath)),
        parameters(std::move(theParameters)),
        originTime(std::move(theOriginTime)),
        minTime(std::move(theMinTime)),
        maxTime(std::move(theMaxTime))
  {
  }

  QEngineFile() : producer(), path(), parameters(), originTime(), minTime(), maxTime() {}
};

QEngineFile buildQEngineFile(const Json::Value &jsonObject)
{
  try
  {
    std::string producer = jsonObject["Producer"].asString();
    std::string path = jsonObject["Path"].asString();
    std::string originTime = jsonObject["OriginTime"].asString();
    std::string minTime = jsonObject["MinTime"].asString();
    std::string maxTime = jsonObject["MaxTime"].asString();
    std::string params = jsonObject["Parameters"].asString();

    std::list<std::string> paramlist;

    boost::algorithm::split(
        paramlist, params, boost::algorithm::is_any_of(" ,"), boost::token_compress_on);

    return QEngineFile(producer, path, paramlist, originTime, minTime, maxTime);
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

bool qEngineSort(const QEngineFile &first, const QEngineFile &second)
{
  try
  {
    if (first.originTime != second.originTime)
      return first.originTime < second.originTime;
    return (first.path < second.path);
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

bool producerHasParam(const QEngineFile &file, const std::string &param)
{
  try
  {
    for (const auto &p : file.parameters)
    {
      if (param == p)
        return true;
    }

    return false;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

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
      ::sleep(t);

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
 * \brief Perform a clusterinfo query
 */
// ----------------------------------------------------------------------

std::pair<std::string, bool> requestClusterInfo(Spine::Reactor &theReactor)
{
  try
  {
    std::ostringstream out;

    auto *engine = theReactor.getSingleton("Sputnik", nullptr);
    if (engine == nullptr)
    {
      out << "Sputnik engine is not available" << std::endl;
      return {out.str(), false};
    }

    auto *sputnik = reinterpret_cast<Engine::Sputnik::Engine *>(engine);
    sputnik->status(out);
    return {out.str(), true};
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Perform a backends query
 */
// ----------------------------------------------------------------------

std::pair<std::string, bool> requestBackendInfo(Spine::Reactor &theReactor,
                                                const Spine::HTTP::Request &theRequest)
{
  try
  {
    std::string service = Spine::optional_string(theRequest.getParameter("service"), "");
    std::string format = Spine::optional_string(theRequest.getParameter("format"), "debug");

    auto *engine = theReactor.getSingleton("Sputnik", nullptr);
    if (engine == nullptr)
      return {"Sputnik engine is not available", false};

    auto *sputnik = reinterpret_cast<Engine::Sputnik::Engine *>(engine);

    boost::shared_ptr<Spine::Table> table = sputnik->backends(service);

    boost::shared_ptr<Spine::TableFormatter> formatter(
        Spine::TableFormatterFactory::create(format));
    Spine::TableFormatter::Names names;
    names.push_back("Backend");
    names.push_back("IP");
    names.push_back("Port");

    auto out = formatter->format(*table, names, theRequest, Spine::TableFormatterOptions());

    return {out, true};
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Perform an active requests query
 */
// ----------------------------------------------------------------------

std::pair<std::string, bool> requestActiveRequests(Spine::Reactor &theReactor,
                                                   const Spine::HTTP::Request &theRequest,
                                                   Spine::HTTP::Response &theResponse)
{
  try
  {
    Spine::Table reqTable;
    std::string format =
        SmartMet::Spine::optional_string(theRequest.getParameter("format"), "json");
    std::unique_ptr<Spine::TableFormatter> formatter(Spine::TableFormatterFactory::create(format));

    // Obtain logging information
    auto requests = theReactor.getActiveRequests();

    auto now = boost::posix_time::microsec_clock::universal_time();

    std::size_t row = 0;
    for (const auto &id_info : requests)
    {
      const auto id = id_info.first;
      const auto &time = id_info.second.time;
      const auto &req = id_info.second.request;

      auto duration = now - time;

      std::size_t column = 0;
      reqTable.set(column++, row, Fmi::to_string(id));
      reqTable.set(column++, row, Fmi::to_iso_extended_string(time.time_of_day()));
      reqTable.set(column++, row, Fmi::to_string(duration.total_milliseconds() / 1000.0));
      reqTable.set(column++, row, req.getClientIP());
      reqTable.set(column++, row, req.getURI());
      ++row;
    }

    std::vector<std::string> headers = {"Id", "Time", "Duration", "ClientIP", "RequestString"};
    auto out = formatter->format(reqTable, headers, theRequest, Spine::TableFormatterOptions());

    // Set MIME
    std::string mime = formatter->mimetype() + "; charset=UTF-8";
    theResponse.setHeader("Content-Type", mime);

    return {out, true};
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Perform an active backends query
 */
// ----------------------------------------------------------------------

std::pair<std::string, bool> requestActiveBackends(Spine::Reactor &theReactor,
                                                   const Spine::HTTP::Request &theRequest,
                                                   Spine::HTTP::Response &theResponse)
{
  try
  {
    Spine::Table reqTable;
    std::string format =
        SmartMet::Spine::optional_string(theRequest.getParameter("format"), "json");
    std::unique_ptr<Spine::TableFormatter> formatter(Spine::TableFormatterFactory::create(format));

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
        reqTable.set(column++, row, host);
        reqTable.set(column++, row, Fmi::to_string(port));
        reqTable.set(column++, row, Fmi::to_string(count));
        ++row;
      }
    }

    std::vector<std::string> headers = {"Host", "Port", "Count"};
    auto out = formatter->format(reqTable, headers, theRequest, Spine::TableFormatterOptions());

    // Set MIME
    std::string mime = formatter->mimetype() + "; charset=UTF-8";
    theResponse.setHeader("Content-Type", mime);

    return {out, true};
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Find latest spine QEngine contents and build output
 */
// ----------------------------------------------------------------------

BackendFiles buildSpineQEngineContents(
    const std::list<std::pair<std::string, std::string> > &backendContents,
    const std::string &producer)
{
  try
  {
    AllFiles theFiles;

    for (const auto &contentPair : backendContents)
    {
      // Individual backend contents
      BackendFiles theseFiles;

      Json::Value jvalue;
      Json::Reader reader;

      // Skip servers which returned error HTML or some other unparseable response
      if (reader.parse(contentPair.second, jvalue))
      {
        for (const auto &jsonObject : jvalue)
        {
          QEngineFile thisFile = buildQEngineFile(jsonObject);

          // Keep only desired producer, or all if the requested producer is empty
          if (producer.empty() || producer == thisFile.producer)
          {
            auto it = theseFiles.find(thisFile.producer);
            if (it != theseFiles.end())
              it->second.push_back(thisFile);
            else
            {
              ProducerFiles thisProducer;
              thisProducer.push_back(thisFile);
              theseFiles.insert(std::make_pair(thisFile.producer, thisProducer));
            }
          }
        }
      }

      theFiles.insert(std::make_pair(contentPair.first, theseFiles));
    }

    // Find maximum size of the file list
    std::size_t maxsize = 0;
    for (auto backendIt = theFiles.begin(); backendIt != theFiles.end(); ++backendIt)
    {
      for (const auto &producer : backendIt->second)
        maxsize = std::max(maxsize, producer.second.size());
    }

    BackendFiles spineFiles;

    for (auto backendIt = theFiles.begin(); backendIt != theFiles.end(); ++backendIt)
    {
      for (const auto &producer : backendIt->second)
      {
        auto outputIt = spineFiles.find(producer.first);
        if (outputIt == spineFiles.end())
        {
          spineFiles.insert(std::make_pair(producer.first, producer.second));
        }
        else
        {
          ProducerFiles tempResult(maxsize);
          auto last_modified = std::set_intersection(outputIt->second.begin(),
                                                     outputIt->second.end(),
                                                     producer.second.begin(),
                                                     producer.second.end(),
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
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Gets backend QEngine contents as pair<Backend name, Json-serial>
 */
// ----------------------------------------------------------------------

std::list<std::pair<std::string, std::string> > getBackendQEngineStatuses(
    Spine::Reactor &theReactor, const std::string &theTimeFormat)
{
  try
  {
    auto *engine = theReactor.getSingleton("Sputnik", nullptr);
    if (engine == nullptr)
    {
      throw Fmi::Exception(BCP, "Sputnik service discovery not available");
    }

    auto *sputnik = reinterpret_cast<Engine::Sputnik::Engine *>(engine);

    // Get the backends which provide services
    auto backendList = sputnik->getBackendList();  // type is Services::BackendList

    // Get Qengine status from backends
    boost::asio::io_service io_service;
    bip::tcp::resolver resolver(io_service);
    std::list<std::pair<std::string, std::string> > qEngineContentList;
    for (auto &backend : backendList)
    {
      bip::tcp::resolver::query query(backend.get<1>(), Fmi::to_string(backend.get<2>()));
      bip::tcp::resolver::iterator endpoint = resolver.resolve(query);

      bip::tcp::socket socket(io_service);
      boost::asio::connect(socket, endpoint);

      boost::asio::streambuf request;
      std::ostream request_stream(&request);
      //	  request_stream << "GET " << "/" << backend.get<0>() <<
      //"/admin?what=qengine&format=json"
      //<< " HTTP/1.0\r\n";
      request_stream << "GET "
                     << "/admin?what=qengine&format=json";
      if (!theTimeFormat.empty())
        request_stream << "&timeformat=" << theTimeFormat;
      request_stream << " HTTP/1.0\r\n";
      request_stream << "Accept: */*\r\n";
      request_stream << "Connection: close\r\n\r\n";

      //		  std::cout << text.str() << std::endl;

      boost::asio::write(socket, request);

      boost::asio::streambuf response;
      boost::system::error_code error;

      while (boost::asio::read(socket, response, boost::asio::transfer_at_least(1), error) != 0u)
      {
        if (error == boost::asio::error::eof)  // Reads until socket is closed
          break;

        // Should there be some error handling in here? Now the JSON parser
        // may just fail on a bad response.
      }

      std::stringstream responseStream;
      responseStream << &response;

      std::string rawResponse = responseStream.str();
      size_t bodyStart = rawResponse.find("\r\n\r\n");
      std::string body = rawResponse.substr(bodyStart);

      qEngineContentList.emplace_back(std::make_pair(backend.get<0>(), body));
    }

    return qEngineContentList;
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

std::pair<std::string, bool> requestQEngineStatus(Spine::Reactor &theReactor,
                                                  const Spine::HTTP::Request &theRequest)
{
  try
  {
    std::string inputType = Spine::optional_string(theRequest.getParameter("type"), "name");
    std::string format = Spine::optional_string(theRequest.getParameter("format"), "debug");
    std::string producer = Spine::optional_string(theRequest.getParameter("producer"), "");
    std::string timeformat = Spine::optional_string(theRequest.getParameter("timeformat"), "");

    // This contains the found producers
    std::list<QEngineFile> iHasAllParameters;
    std::string input = Spine::optional_string(theRequest.getParameter("param"), "");

    std::size_t tokens = 0;
    std::vector<std::string> paramTokens;

    if (!input.empty())
    {
      // Parse input parameter names
      boost::algorithm::split(
          paramTokens, input, boost::algorithm::is_any_of(","), boost::token_compress_on);
      tokens = paramTokens.size();
    }

    // Obtain backend QEngine statuses
    std::list<std::pair<std::string, std::string> > qEngineContentList =
        getBackendQEngineStatuses(theReactor, timeformat);
    BackendFiles result = buildSpineQEngineContents(qEngineContentList, producer);

    if (tokens == 0)
    {
      // Zero parameter tokens, print list of all spine producers
      // Build the result table
      Spine::Table table;
      std::size_t row = 0;
      for (auto &pair : result)
      {
        if (pair.second.empty())
        {
          std::cerr << "Warning: producer " << pair.first << " has no content" << std::endl;
          continue;
        }

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

      Spine::TableFormatter::Names theNames;
      theNames.push_back("Producer");
      theNames.push_back("Path");
      theNames.push_back("OriginTime");
      theNames.push_back("MinTime");
      theNames.push_back("MaxTime");

      std::unique_ptr<Spine::TableFormatter> formatter(
          Spine::TableFormatterFactory::create(format));
      auto out = formatter->format(table, theNames, theRequest, Spine::TableFormatterOptions());

      return {out, true};
    }

    // There are some parameter tokens, return spine producers providing these parameters
    if (inputType == "name")
    {
      for (auto &pair : result)
      {
        // Check if producer contains parameter
        // Get latest file
        QEngineFile &latest = *(--pair.second.end());
        unsigned int matches = 0;
        for (auto &param : paramTokens)
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
      for (auto &pair : result)
      {
        // Check if producer contains parameter
        // Get latest file
        QEngineFile &latest = *(--pair.second.end());
        unsigned int matches = 0;
        for (auto &param : paramTokens)
        {
          // If param type is id, but input cannot be cast into int, simply ignore
          int paramId = 0;
          try
          {
            paramId = boost::lexical_cast<int>(param);
          }
          catch (const std::bad_cast &)
          {
            ++matches;
            continue;
          }
          std::string paramString = Spine::ParameterFactory::instance().name(paramId);
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
      std::ostringstream oss;
      oss << "Invalid input type " << inputType;
      throw Fmi::Exception(BCP, oss.str());
    }

    // Sort results by origintime
    iHasAllParameters.sort(boost::bind(qEngineSort, _2, _1));

    // Build result table
    Spine::Table table;
    std::size_t row = 0;
    for (auto &file : iHasAllParameters)
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

    Spine::TableFormatter::Names theNames;
    theNames.push_back("Producer");
    theNames.push_back("Path");
    theNames.push_back("OriginTime");

    std::unique_ptr<Spine::TableFormatter> formatter(Spine::TableFormatterFactory::create(format));
    auto out = formatter->format(table, theNames, theRequest, Spine::TableFormatterOptions());

    return {out, true};
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Perform an Admin query
 */
// ----------------------------------------------------------------------

std::pair<std::string, bool> Plugin::request(Spine::Reactor &theReactor,
                                             const Spine::HTTP::Request &theRequest,
                                             Spine::HTTP::Response &theResponse)
{
  try
  {
    // Check authentication first
    bool hasValidAuthentication = authenticateRequest(theRequest, theResponse);

    if (!hasValidAuthentication)
      return {"", true};

    // We may return JSON, hence we should enable CORS
    theResponse.setHeader("Access-Control-Allow-Origin", "*");

    std::string what = Spine::optional_string(theRequest.getParameter("what"), "");

    if (what.empty())
      return {"No request specified", false};

    if (what == "clusterinfo")
      return requestClusterInfo(theReactor);

    if (what == "backends")
      return requestBackendInfo(theReactor, theRequest);

    if (what == "qengine")
      return requestQEngineStatus(theReactor, theRequest);

    if (what == "activerequests")
      return requestActiveRequests(theReactor, theRequest, theResponse);

    if (what == "activebackends")
      return requestActiveBackends(theReactor, theRequest, theResponse);

    if (what == "pause")
      return requestPause(theReactor, theRequest);

    if (what == "continue")
      return requestContinue(theReactor, theRequest);

    return {"Unknown request: '" + what + "'", false};
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Pause until the given time
 */
// ----------------------------------------------------------------------

std::pair<std::string, bool> Plugin::pauseUntil(const boost::posix_time::ptime &theTime)
{
  auto timestr = Fmi::to_iso_string(theTime);
  std::cout << Spine::log_time_str() << " *** Frontend paused until " << timestr << std::endl;

  Spine::WriteLock lock(itsPauseMutex);
  itsPaused = true;
  itsPauseDeadLine = theTime;
  return {"Paused Frontend until " + timestr, true};
}

// ----------------------------------------------------------------------
/*!
 * \brief Request a pause in F5 responses
 */
// ----------------------------------------------------------------------

std::pair<std::string, bool> Plugin::requestPause(Spine::Reactor & /* theReactor */,
                                                  const Spine::HTTP::Request &theRequest)
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
      auto deadline = boost::posix_time::second_clock::universal_time() + duration;
      return pauseUntil(deadline);
    }

    std::cout << Spine::log_time_str() << " *** Frontend paused" << std::endl;
    Spine::WriteLock lock(itsPauseMutex);
    itsPaused = true;
    itsPauseDeadLine = boost::none;
    return {"Paused Frontend", true};
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

std::pair<std::string, bool> Plugin::requestContinue(Spine::Reactor & /* theReactor */,
                                                     const Spine::HTTP::Request &theRequest)
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
      auto deadline = boost::posix_time::second_clock::universal_time() + duration;
      return pauseUntil(deadline);
    }

    std::cout << Spine::log_time_str() << " *** Frontend continues" << std::endl;
    Spine::WriteLock lock(itsPauseMutex);
    itsPaused = false;
    itsPauseDeadLine = boost::none;
    return {"Frontend continues", true};
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

  auto now = boost::posix_time::microsec_clock::universal_time();

  if (now < itsPauseDeadLine)
    return true;

  // deadline expired, continue
  std::cout << Spine::log_time_str() << " *** Frontend pause deadline expired, continuing"
            << std::endl;
  Spine::UpgradeWriteLock writelock(readlock);
  itsPaused = false;
  itsPauseDeadLine = boost::none;

  return false;
}

// ----------------------------------------------------------------------
/*!
 * \brief Return true if a service requiring authentication is requested
 */
// ----------------------------------------------------------------------

bool isAuthenticationRequired(const Spine::HTTP::Request &theRequest)
{
  try
  {
    std::string what = Spine::optional_string(theRequest.getParameter("what"), "");

    if (what == "pause")
      return true;
    if (what == "continue")
      return true;

    return false;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
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
      // Does not have authentication, lets ask for it if necessary
      if (!isAuthenticationRequired(theRequest))
        return true;

      theResponse.setStatus(Spine::HTTP::Status::unauthorized);
      theResponse.setHeader("WWW-Authenticate", "Basic realm=\"SmartMet Admin\"");
      theResponse.setHeader("Content-Type", "text/html; charset=UTF-8");

      std::string content = "<html><body><h1>401 Unauthorized </h1></body></html>";
      theResponse.setContent(content);

      return false;
    }

    // Parse user and password

    std::vector<std::string> splitHeader;
    std::string truePassword;
    std::string trueUser;
    std::string trueDigest;
    std::string givenDigest;

    boost::algorithm::split(
        splitHeader, *credentials, boost::is_any_of(" "), boost::token_compress_on);

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
    : SmartMetPlugin(), itsModuleName("Frontend")
{
  try
  {
    if (theReactor->getRequiredAPIVersion() != SMARTMET_API_VERSION)
      throw Fmi::Exception(BCP, "Frontend and Server API version mismatch");

    try
    {
      libconfig::Config config;

      // Enable sensible relative include paths
      boost::filesystem::path p = theConfig;
      p.remove_filename();
      config.setIncludeDir(p.c_str());

      config.readFile(theConfig);
      if (!config.lookupValue("user", itsUsername) || !config.lookupValue("password", itsPassword))
        throw Fmi::Exception(BCP, std::string("user or password not set in '") + theConfig + "'");
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

    itsHTTP.reset(new HTTP(theReactor, theConfig));

    if (!theReactor->addContentHandler(
            this, "/admin", boost::bind(&Plugin::callRequestHandler, this, _1, _2, _3)))
      throw Fmi::Exception(BCP, "Failed to register admin content handler");

    if (!theReactor->addContentHandler(
            this, "/", boost::bind(&Plugin::baseContentHandler, this, _1, _2, _3)))
      throw Fmi::Exception(BCP, "Failed to register base content handler");

#ifndef NDEBUG
    if (!theReactor->addContentHandler(this, "/sleep", boost::bind(&sleep, _1, _2, _3)))
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
 * \brief Initializator (trivial)
 */
// ----------------------------------------------------------------------

void Plugin::init() {}

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
    // Default expiration time

    const int expires_seconds = 60;
    bool isdebug = false;

    // Now

    ptime t_now = second_clock::universal_time();

    try
    {
      // Assume OK, the handler will override for example with 401 if necessary
      theResponse.setStatus(Spine::HTTP::Status::ok);

      std::pair<std::string, bool> response = request(theReactor, theRequest, theResponse);

      // Response may have been set directly, and the returned value is empty

      if (response.first.empty())
        return;

      // Make the response HTML in debug mode

      std::string format = Spine::optional_string(theRequest.getParameter("format"), "debug");

      std::string ret = response.first;
      if (format == "debug")
      {
        isdebug = true;
        ret = ("<html><head><title>SmartMet Admin</title></head><body>" + ret + "</body></html>");
      }
      theResponse.setContent(ret);

      boost::shared_ptr<Spine::TableFormatter> formatter(
          Spine::TableFormatterFactory::create(format));
      theResponse.setHeader("Content-Type", formatter->mimetype().c_str());

      // We allow JSON requests, hence we should enable CORS
      theResponse.setHeader("Access-Control-Allow-Origin", "*");

      // Build cache expiration time info

      ptime t_expires = t_now + seconds(expires_seconds);

      // The headers themselves

      std::string cachecontrol = "public, max-age=" + Fmi::to_string(expires_seconds);
      std::string expiration = Fmi::to_http_string(t_expires);
      std::string modification = Fmi::to_http_string(t_now);

      theResponse.setHeader("Cache-Control", cachecontrol.c_str());
      theResponse.setHeader("Expires", expiration.c_str());
      theResponse.setHeader("Last-Modified", modification.c_str());

      if (response.first.empty())
      {
        std::cerr << "Warning: Empty input for request " << theRequest.getQueryString() << " from "
                  << theRequest.getClientIP() << std::endl;
      }

#ifdef MYDEBUG
      std::cout << "Output:" << std::endl << response << std::endl;
#endif
    }
    /*
     * Cannot find a source that is actually throwing this exception
    catch (const Fmi::Exceptions::NotAuthorizedError &)
    {
      // Blocked by ip filter, masquerade as bad request
      theResponse.setStatus(Spine::HTTP::Status::bad_request, true);
      std::cerr << boost::posix_time::second_clock::local_time()
           << " Attempt to access frontend admin from " << theRequest.getClientIP()
           << ". Not in whitelista." << std::endl;
    }
    */
    catch (...)
    {
      // Catching all exceptions

      Fmi::Exception exception(BCP, "Request processing exception!", nullptr);
      exception.addParameter("URI", theRequest.getURI());
      exception.printError();

      if (isdebug)
      {
        // Delivering the exception information as HTTP content
        std::string fullMessage = exception.getHtmlStackTrace();
        theResponse.setContent(fullMessage);
        theResponse.setStatus(Spine::HTTP::Status::ok);
      }
      else
      {
        theResponse.setStatus(Spine::HTTP::Status::bad_request);
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
      cerr << boost::posix_time::second_clock::local_time() << " error: " << e.what() << std::endl
           << "Query: " << theRequest.getURI() << std::endl;

      std::string msg = std::string("Error: ") + e.what();
      theResponse.setContent(msg);
      theResponse.setStatus(Spine::HTTP::Status::internal_server_error);
      // Remove newlines, make sure length is reasonable
      boost::algorithm::replace_all(msg, "\n", " ");
      msg = msg.substr(0, 100);
      theResponse.setHeader("X-Admin-Error", msg.c_str());
    }
    catch (...)
    {
      cerr << boost::posix_time::second_clock::local_time() << " error: "
           << "Unknown exception" << std::endl
           << "Query: " << theRequest.getURI() << std::endl;

      theResponse.setHeader("X-Admin-Error", "Unknown exception");
      std::string msg = "Error: Unknown exception";
      theResponse.setContent(msg);
      theResponse.setStatus(Spine::HTTP::Status::internal_server_error);
    }
#endif
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
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
