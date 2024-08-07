// ======================================================================
/*!
 * \brief SmartMet Frontend plugin implementation
 */
// ======================================================================

#include "Plugin.h"
#include "HTTP.h"
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
#include <sstream>
#include <stdexcept>
#include <utility>

namespace SmartMet
{
namespace Plugin
{
namespace Frontend
{
namespace
{
bool sortRequestVector(const std::pair<std::string, std::string> &pair1,
                       const std::pair<std::string, std::string> &pair2)
{
  return pair1.first < pair2.first;
}

std::vector<std::pair<std::string, std::string>> getRequests()
{
  std::vector<std::pair<std::string, std::string>> ret = {
      {"qengine", "Available querydata"},
      {"gridgenerations", "Available grid generations"},
      {"gridgenerationsqd", "Available grid newbase generations"},
      {"backends", "Backend information"},
      {"activerequests", "Currently active requests"},
      {"activebackends", "Currently active backends"},
      {"cachestats", "Cache statistics"}};

  return ret;
}
}  // namespace
struct QEngineFile;

// QEngine reporting types
using ProducerFiles = std::list<QEngineFile>;               // list of files per producer
using BackendFiles = std::map<std::string, ProducerFiles>;  // key is producer
using AllFiles = std::map<std::string, BackendFiles>;

using TimeCounter = std::map<std::string, uint>;

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

  QEngineFile() = default;
};

QEngineFile build_qengine_file(const Json::Value &jsonObject)
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

    return {producer, path, paramlist, originTime, minTime, maxTime};
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

bool qengine_sort(const QEngineFile &lhs, const QEngineFile &rhs)
{
  try
  {
    if (lhs.originTime != rhs.originTime)
      return lhs.originTime < rhs.originTime;
    return (lhs.path < rhs.path);
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

bool producer_has_param(const QEngineFile &file, const std::string &param)
{
  try
  {
    auto match = std::find(file.parameters.begin(), file.parameters.end(), param);
    return match != file.parameters.end();
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

// Find longest list of files
std::size_t max_filelist_size(const AllFiles &theFiles)
{
  std::size_t maxsize = 0;
  for (const auto &backend : theFiles)
  {
    for (const auto &prod : backend.second)
      maxsize = std::max(maxsize, prod.second.size());
  }
  return maxsize;
}

// Collect all files for that wanted producer or all producers if the given producer is empty

AllFiles collect_files(const std::list<std::pair<std::string, std::string>> &backendContents,
                       const std::string &producer)
{
  AllFiles files;
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
        QEngineFile thisFile = build_qengine_file(jsonObject);

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

    files.insert(std::make_pair(contentPair.first, theseFiles));
  }
  return files;
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

    std::shared_ptr<Spine::Table> table = sputnik->backends(service);

    std::shared_ptr<Spine::TableFormatter> formatter(
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

    auto now = Fmi::MicrosecClock::universal_time();

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
    const std::list<std::pair<std::string, std::string>> &backendContents,
    const std::string &producer)
{
  try
  {
    AllFiles theFiles = collect_files(backendContents, producer);

    auto maxsize = max_filelist_size(theFiles);

    BackendFiles spineFiles;

    for (const auto &backend : theFiles)
    {
      for (const auto &prod : backend.second)
      {
        auto outputIt = spineFiles.find(prod.first);
        if (outputIt == spineFiles.end())
        {
          spineFiles.insert(std::make_pair(prod.first, prod.second));
        }
        else
        {
          ProducerFiles tempResult(maxsize);
          auto last_modified = std::set_intersection(outputIt->second.begin(),
                                                     outputIt->second.end(),
                                                     prod.second.begin(),
                                                     prod.second.end(),
                                                     tempResult.begin(),
                                                     qengine_sort);
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

std::list<std::pair<std::string, std::string>> getBackendQEngineStatuses(
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

    Spine::TcpMultiQuery multi_query(5);

    // FIXME: Why backendList contains repeated addresses? Work around that for now
    int counter = 0;
    std::vector<std::pair<std::string, std::string>> id_mapping;

    for (auto &backend : backendList)
    {
      std::ostringstream request_stream;
      request_stream << "GET "
                     << "/admin?what=qengine&format=json";
      if (!theTimeFormat.empty())
        request_stream << "&timeformat=" << theTimeFormat;
      request_stream << " HTTP/1.0\r\n";
      request_stream << "Accept: */*\r\n";
      request_stream << "Connection: close\r\n\r\n";

      const std::string id = fmt::format("{0:05d}", ++counter);
      id_mapping.emplace_back(std::make_pair(backend.get<1>(), id));

      multi_query.add_query(
          id, backend.get<1>(), Fmi::to_string(backend.get<2>()), request_stream.str());
    }

    multi_query.execute();

    std::list<std::pair<std::string, std::string>> qEngineContentList;
    for (auto &item : id_mapping)
    {
      const auto result = multi_query[item.second];
      if (result.error_code)
      {
        std::cout << "Frontend::getBackendQEngineStatuses: failed to get response from backend "
                  << item.first << ": " << result.error_code.message() << std::endl;
        // FIXME: do we need to output error message to qEngineContentList?
      }
      else
      {
        std::string rawResponse = result.body;
        size_t bodyStart = rawResponse.find("\r\n\r\n");
        if (bodyStart == std::string::npos)
        {
          std::cout << "Frontend::getBackendMessages: body not found in response from backend "
                    << item.first << std::endl;
          // FIXME: put something into qEngineContentList indicating an error
        }
        else
        {
          std::string body = rawResponse.substr(bodyStart);

          qEngineContentList.emplace_back(std::make_pair(item.first, body));
        }
      }
    }

    return qEngineContentList;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

std::list<std::pair<std::string, std::string>> getBackendMessages(Spine::Reactor &theReactor,
                                                                  const std::string &url)
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

    Spine::TcpMultiQuery multi_query(5);

    int counter = 0;
    std::vector<std::pair<std::string, std::string>> id_mapping;

    for (auto &backend : backendList)
    {
      std::ostringstream request_stream;
      //    request_stream << "GET " << "/" << backend.get<0>() <<
      //"/admin?what=qengine&format=json"
      //<< " HTTP/1.0\r\n";
      request_stream << "GET " << url << " HTTP/1.0\r\n";
      request_stream << "Accept: */*\r\n";
      request_stream << "Connection: close\r\n\r\n";
      const std::string id = fmt::format("{0:05d}", ++counter);
      id_mapping.emplace_back(std::make_pair(backend.get<1>(), id));

      multi_query.add_query(
          id, backend.get<1>(), Fmi::to_string(backend.get<2>()), request_stream.str());
    }

    multi_query.execute();

    std::list<std::pair<std::string, std::string>> messageList;
    for (auto &item : id_mapping)
    {
      const auto result = multi_query[item.second];
      if (result.error_code)
      {
        std::cout << "Frontend::getBackendMessages: failed to get response from backend "
                  << item.first << ": " << result.error_code.message() << std::endl;
        // FIXME: do we need to output error message to messageList?
      }
      else
      {
        std::string rawResponse = result.body;
        size_t bodyStart = rawResponse.find("\r\n\r\n");
        if (bodyStart == std::string::npos)
        {
          std::cout << "Frontend::getBackendMessages: body not found in response from backend "
                    << item.first << std::endl;
          // FIXME: put something into messageList indicating an error
        }
        else
        {
          std::string body = rawResponse.substr(bodyStart);
          messageList.emplace_back(std::make_pair(item.first, body));
        }
      }
    }

    return messageList;
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
    std::list<std::pair<std::string, std::string>> qEngineContentList =
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
        for (const auto &param : paramTokens)
        {
          if (producer_has_param(latest, param))
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
          std::string paramString = TimeSeries::ParameterFactory::instance().name(paramId);
          if (producer_has_param(latest, paramString))
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
    iHasAllParameters.sort([](const QEngineFile &lhs, const QEngineFile &rhs)
                           { return qengine_sort(lhs, rhs); });

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

std::size_t count_matches(const std::vector<std::string> &inputParamList,
                          const std::vector<std::string> &fields)
{
  if (inputParamList.empty())
    return 0;

  std::set<std::string> paramList1;
  std::set<std::string> paramList2;

  std::string tmp1 = toLowerString(fields[7]);
  std::string tmp2 = toLowerString(fields[8]);
  splitString(tmp1, ',', paramList1);
  splitString(tmp2, ',', paramList2);

  std::size_t matchCount = 0;
  for (const auto &p : inputParamList)
  {
    auto f1 = paramList1.find(p);
    if (f1 != paramList1.end())
      matchCount++;
    else
    {
      auto f2 = paramList2.find(p);
      if (f2 != paramList2.end())
        matchCount++;
    }
  }
  return matchCount;
}

void update_producers(std::map<std::string, TimeCounter> &producers,
                      const std::vector<std::string> &fields)
{
  auto tm = fmt::format(
      "{}:{}:{}:{}:{}:{}", fields[3], fields[4], fields[5], fields[6], fields[1], fields[2]);
  auto prod = producers.find(fields[0]);
  if (prod == producers.end())
  {
    TimeCounter originTimes;
    originTimes.insert(std::pair<std::string, uint>(tm, 1));
    producers.insert(std::pair<std::string, TimeCounter>(fields[0], originTimes));
  }
  else
  {
    auto originTime = prod->second.find(tm);
    if (originTime == prod->second.end())
      prod->second.insert(std::pair<std::string, uint>(tm, 1));
    else
      originTime->second++;
  }
}

std::map<std::string, TimeCounter> extract_producers(
    const std::list<std::pair<std::string, std::string>> &messageList,
    const std::vector<std::string> &inputParamList)
{
  std::map<std::string, TimeCounter> producers;

  for (const auto &b : messageList)
  {
    std::vector<std::string> lines;
    lineSplit(b.second.c_str(), lines);

    for (const auto &line : lines)
    {
      std::vector<std::string> fields;
      splitString(line, ' ', fields);

      if (fields.size() >= 9)
      {
        std::size_t matchCount = count_matches(inputParamList, fields);
        if (inputParamList.size() == matchCount)
          update_producers(producers, fields);
      }
    }
  }

  return producers;
}

std::pair<std::string, bool> requestStatus(Spine::Reactor &theReactor,
                                           const Spine::HTTP::Request &theRequest,
                                           const std::string &what)
{
  try
  {
    std::string format = Spine::optional_string(theRequest.getParameter("format"), "debug");
    std::string producer = Spine::optional_string(theRequest.getParameter("producer"), "");
    std::string timeFormat = Spine::optional_string(theRequest.getParameter("timeformat"), "iso");
    std::string param = Spine::optional_string(theRequest.getParameter("param"), "");
    std::vector<std::string> inputParamList;
    if (!param.empty())
    {
      std::string tmp = toLowerString(param);
      splitString(tmp, ',', inputParamList);
    }

    Spine::Table table;
    std::size_t row = 0;

    std::string url = "/admin?what=" + what + "&format=ascii&timeformat=iso";
    if (!producer.empty())
      url = url + "&producer=" + producer;

    std::unique_ptr<Fmi::TimeFormatter> timeFormatter(Fmi::TimeFormatter::create(timeFormat));

    // Obtain backend QEngine statuses
    std::list<std::pair<std::string, std::string>> messageList =
        getBackendMessages(theReactor, url);

    std::map<std::string, TimeCounter> producers = extract_producers(messageList, inputParamList);

    for (const auto &prod : producers)
    {
      uint cnt = 0;
      for (auto atime = prod.second.rbegin(); atime != prod.second.rend() && cnt == 0; ++atime)
      {
        uint backendCount = messageList.size();

        if (atime->second == backendCount)
        {
          std::vector<std::string> fields;
          splitString(atime->first.c_str(), ':', fields);

          if (fields.size() == 6)
          {
            table.set(0, row, prod.first);
            table.set(1, row, fields[4]);
            table.set(2, row, fields[5]);
            if (!timeFormat.empty() && strcasecmp(timeFormat.c_str(), "iso") != 0 && timeFormatter)
            {
              // Analysis time
              Fmi::DateTime aTime = toTimeStamp(fields[0]);
              table.set(3, row, timeFormatter->format(aTime));

              Fmi::DateTime fTime = toTimeStamp(fields[1]);
              table.set(4, row, timeFormatter->format(fTime));

              Fmi::DateTime lTime = toTimeStamp(fields[2]);
              table.set(5, row, timeFormatter->format(lTime));

              Fmi::DateTime mTime = toTimeStamp(fields[3]);
              table.set(6, row, timeFormatter->format(mTime));
            }
            else
            {
              table.set(3, row, fields[0]);
              table.set(4, row, fields[1]);
              table.set(5, row, fields[2]);
              table.set(6, row, fields[3]);
            }
            cnt++;
            row++;
          }
        }
      }
    }

    Spine::TableFormatter::Names theNames;
    theNames.push_back("Producer");
    theNames.push_back("GeometryId");
    theNames.push_back("TimeSteps");
    theNames.push_back("OriginTime");
    theNames.push_back("MinTime");
    theNames.push_back("MaxTime");
    theNames.push_back("ModificationTime");

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

    if (what == "gridgenerations")
      return requestStatus(theReactor, theRequest, "gridgenerations");

    if (what == "gridgenerationsqd")
      return requestStatus(theReactor, theRequest, "gridgenerationsqd");

    if (what == "activerequests")
      return requestActiveRequests(theReactor, theRequest, theResponse);

    if (what == "activebackends")
      return requestActiveBackends(theReactor, theRequest, theResponse);

    if (what == "pause")
      return requestPause(theReactor, theRequest);

    if (what == "continue")
      return requestContinue(theReactor, theRequest);

    if (what == "list")
      return listRequests(theReactor, theRequest, theResponse);

    if (what == "cachestats")
      return requestCacheStats(theReactor, theRequest, theResponse);

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

std::pair<std::string, bool> Plugin::pauseUntil(const Fmi::DateTime &theTime)
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
      auto deadline = Fmi::SecondClock::universal_time() + duration;
      return pauseUntil(deadline);
    }

    std::cout << Spine::log_time_str() << " *** Frontend paused" << std::endl;
    Spine::WriteLock lock(itsPauseMutex);
    itsPaused = true;
    itsPauseDeadLine = std::nullopt;
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
      auto deadline = Fmi::SecondClock::universal_time() + duration;
      return pauseUntil(deadline);
    }

    std::cout << Spine::log_time_str() << " *** Frontend continues" << std::endl;
    Spine::WriteLock lock(itsPauseMutex);
    itsPaused = false;
    itsPauseDeadLine = std::nullopt;
    return {"Frontend continues", true};
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Lists all requests supported by frontend plugin
 */
// ----------------------------------------------------------------------

std::pair<std::string, bool> Plugin::listRequests(Spine::Reactor & /* theReactor */,
                                                  const Spine::HTTP::Request &theRequest,
                                                  Spine::HTTP::Response &theResponse)
{
  try
  {
    // Parse formatting options
    std::string tableFormat = Spine::optional_string(theRequest.getParameter("format"), "debug");

    if (tableFormat == "wxml")
    {
      std::string response = "Wxml formatting not supported";
      theResponse.setContent(response);
      return {response, false};
    }

    std::unique_ptr<Spine::TableFormatter> tableFormatter(
        Spine::TableFormatterFactory::create(tableFormat));

    Spine::Table resultTable;
    Spine::TableFormatter::Names headers{"Request", "Response"};

    std::vector<std::pair<std::string, std::string>> requests = getRequests();
    std::sort(requests.begin(), requests.end(), sortRequestVector);

    unsigned int row = 0;
    for (const auto &r : requests)
    {
      resultTable.set(0, row, r.first);
      resultTable.set(1, row, r.second);
      row++;
    }

    auto requests_out =
        tableFormatter->format(resultTable, headers, theRequest, Spine::TableFormatterOptions());

    if (tableFormat == "html" || tableFormat == "debug")
      requests_out.insert(0, "<h1>Admin requests</h1>");

    if (tableFormat != "html")
      theResponse.setContent(requests_out);
    else
    {
      // Only insert tags if using human readable mode
      std::string ret =
          "<html><head>"
          "<title>SmartMet Admin</title>"
          "<style>";
      ret +=
          "table { border: 1px solid black; }"
          "td { border: 1px solid black; text-align:right;}"
          "</style>"
          "</head><body>";
      ret += requests_out;
      ret += "</body></html>";
      theResponse.setContent(ret);
    }

    // Make MIME header and content
    std::string mime = tableFormatter->mimetype() + "; charset=UTF-8";

    theResponse.setHeader("Content-Type", mime);

    return {requests_out, true};
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Cache statistics
 */
// ----------------------------------------------------------------------

std::pair<std::string, bool> Plugin::requestCacheStats(Spine::Reactor & /* theReactor */,
                                                       const Spine::HTTP::Request &theRequest,
                                                       Spine::HTTP::Response &theResponse)
{
  try
  {
    std::string tableFormat = Spine::optional_string(theRequest.getParameter("format"), "html");
    std::unique_ptr<Spine::TableFormatter> tableFormatter(
        Spine::TableFormatterFactory::create(tableFormat));
    std::shared_ptr<Spine::Table> table(new Spine::Table());
    Spine::TableFormatter::Names header_names{"#",
                                              "cache_name",
                                              "maxsize",
                                              "size",
                                              "inserts",
                                              "hits",
                                              "misses",
                                              "hitrate",
                                              "hits/min",
                                              "inserts/min",
                                              "created",
                                              "age"};

    auto now = Fmi::MicrosecClock::universal_time();
    auto cache_stats = getCacheStats();

    Spine::Table data_table;

    auto timeFormat = Spine::optional_string(theRequest.getParameter("timeformat"), "sql");
    std::unique_ptr<Fmi::TimeFormatter> timeFormatter(Fmi::TimeFormatter::create(timeFormat));

    size_t row = 1;
    for (const auto &item : cache_stats)
    {
      const auto &name = item.first;
      const auto &stat = item.second;
      auto duration = (now - stat.starttime).total_seconds();
      auto n = stat.hits + stat.misses;
      auto hit_rate = (n == 0 ? 0.0 : stat.hits * 100.0 / n);
      auto hits_per_min = (duration == 0 ? 0.0 : 60.0 * stat.hits / duration);
      auto inserts_per_min = (duration == 0 ? 0.0 : 60.0 * stat.inserts / duration);

      data_table.set(0, row, Fmi::to_string(row));
      data_table.set(1, row, name);
      data_table.set(2, row, Fmi::to_string(stat.maxsize));
      data_table.set(3, row, Fmi::to_string(stat.size));
      data_table.set(4, row, Fmi::to_string(stat.inserts));
      data_table.set(5, row, Fmi::to_string(stat.hits));
      data_table.set(6, row, Fmi::to_string(stat.misses));
      data_table.set(7, row, Fmi::to_string("%.1f", hit_rate));
      data_table.set(8, row, Fmi::to_string("%.1f", hits_per_min));
      data_table.set(9, row, Fmi::to_string("%.1f", inserts_per_min));
      data_table.set(10, row, timeFormatter->format(stat.starttime));
      data_table.set(11, row, Fmi::to_simple_string(now - stat.starttime));
      row++;
    }

    auto cache_stats_output = tableFormatter->format(
        data_table, header_names, theRequest, Spine::TableFormatterOptions());

    if (tableFormat == "html" || tableFormat == "debug")
      cache_stats_output.insert(0, "<h1>CacheStatistics</h1>");

    if (tableFormat != "html")
      theResponse.setContent(cache_stats_output);
    else
    {
      // Only insert tags if using human readable mode
      std::string ret =
          "<html><head>"
          "<title>SmartMet Frontend</title>"
          "<style>";
      ret +=
          "table { border: 1px solid black; }"
          "td { border: 1px solid black; text-align:right;}"
          "</style>"
          "</head><body>";
      ret += cache_stats_output;
      ret += "</body></html>";
      theResponse.setContent(ret);
    }

    // Make MIME header and content
    std::string mime = tableFormatter->mimetype() + "; charset=UTF-8";

    theResponse.setHeader("Content-Type", mime);
    return {cache_stats_output, true};
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

Plugin::Plugin(Spine::Reactor *theReactor, const char *theConfig) : itsModuleName("Frontend")
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

    if (!theReactor->addContentHandler(this,
                                       "/admin",
                                       [this](Spine::Reactor &theReactor,
                                              const Spine::HTTP::Request &theRequest,
                                              Spine::HTTP::Response &theResponse) {
                                         callRequestHandler(theReactor, theRequest, theResponse);
                                       }))
      throw Fmi::Exception(BCP, "Failed to register admin content handler");

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

    Fmi::DateTime t_now = Fmi::SecondClock::universal_time();

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

      std::shared_ptr<Spine::TableFormatter> formatter(
          Spine::TableFormatterFactory::create(format));
      theResponse.setHeader("Content-Type", formatter->mimetype());

      // We allow JSON requests, hence we should enable CORS
      theResponse.setHeader("Access-Control-Allow-Origin", "*");

      // Build cache expiration time info

      Fmi::DateTime t_expires = t_now + Fmi::Seconds(expires_seconds);

      // The headers themselves

      std::string cachecontrol = "public, max-age=" + Fmi::to_string(expires_seconds);
      std::string expiration = Fmi::to_http_string(t_expires);
      std::string modification = Fmi::to_http_string(t_now);

      theResponse.setHeader("Cache-Control", cachecontrol);
      theResponse.setHeader("Expires", expiration);
      theResponse.setHeader("Last-Modified", modification);

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
      std::cerr << Fmi::SecondClock::local_time()
           << " Attempt to access frontend admin from " << theRequest.getClientIP()
           << ". Not in whitelista." << std::endl;
    }
    */
    catch (...)
    {
      // Catching all exceptions

      Fmi::Exception exception(BCP, "Request processing exception!", nullptr);
      exception.addParameter("URI", theRequest.getURI());
      exception.addParameter("ClientIP", theRequest.getClientIP());
      exception.addParameter("HostName", Spine::HostInfo::getHostName(theRequest.getClientIP()));
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
      if (firstMessage.size() > 300)
        firstMessage.resize(300);
      theResponse.setHeader("X-Frontend-Error", firstMessage);
    }
#if 0
    catch (...)
    {
      cerr << Fmi::SecondClock::local_time() << " error: " << e.what() << std::endl
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
      cerr << Fmi::SecondClock::local_time() << " error: "
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
