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
 * \brief Gets Sputnik engine pointer or thhrow and exception if not available
 */
// ----------------------------------------------------------------------

Engine::Sputnik::Engine* Frontend::Plugin::getSputnikEngine()
{
    Spine::Reactor* reactor = Spine::Reactor::instance;
    void *engine = reactor->getSingleton("Sputnik", nullptr);
    if (engine == nullptr)
    {
        throw Fmi::Exception(BCP, "Sputnik engine is not available");
    }
    Engine::Sputnik::Engine* sputnik = reinterpret_cast<Engine::Sputnik::Engine *>(engine);
    if (sputnik == nullptr)
    {
        throw Fmi::Exception(BCP, "Sputnik engine is not available (dynamic cast failed)");
    }
    return sputnik;
}


// ----------------------------------------------------------------------
/*!
 * \brief Perform a clusterinfo query
 */
// ----------------------------------------------------------------------

void Frontend::Plugin::requestClusterInfo(const Spine::HTTP::Request& theRequest,
                                          Spine::HTTP::Response& theResponse)
try
{
    std::string content = "<html><head><title>Cluster info</title></head><body>";

    std::ostringstream out;
    auto* sputnik = getSputnikEngine();
    sputnik->status(out);
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
    std::string service = Spine::optional_string(theRequest.getParameter("service"), "");
    return sputnik->backends(service);
}


// ----------------------------------------------------------------------
/*!
 * \brief Perform an active backends query
 */
// ----------------------------------------------------------------------

std::unique_ptr<Spine::Table>
Plugin::requestActiveBackends(Spine::Reactor &theReactor)
{
  try
  {
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
        reqTable->set(column++, row, Fmi::to_string(port));
        reqTable->set(column++, row, Fmi::to_string(count));
        ++row;
      }
    }

    std::vector<std::string> headers = {"Host", "Port", "Count"};
    reqTable->setNames(headers);
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

void Plugin::requestQEngineStatus(Spine::Reactor &theReactor,
                                  const Spine::HTTP::Request &theRequest,
                                  Spine::HTTP::Response &theResponse)
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

      theResponse.setContent(out);
      theResponse.setHeader("Content-Type", formatter->mimetype() + "; charset=UTF-8");
      theResponse.setStatus(Spine::HTTP::Status::ok);
      return;
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

    theResponse.setContent(out);
    theResponse.setHeader("Content-Type", formatter->mimetype() + "; charset=UTF-8");
    theResponse.setStatus(Spine::HTTP::Status::ok);
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

std::unique_ptr<Spine::Table>
Plugin::requestStatus(Spine::Reactor &theReactor,
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

    std::unique_ptr<Spine::Table> table = std::make_unique<Spine::Table>();
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
            table->set(0, row, prod.first);
            table->set(1, row, fields[4]);
            table->set(2, row, fields[5]);
            if (!timeFormat.empty() && strcasecmp(timeFormat.c_str(), "iso") != 0 && timeFormatter)
            {
              // Analysis time
              Fmi::DateTime aTime = toTimeStamp(fields[0]);
              table->set(3, row, timeFormatter->format(aTime));

              Fmi::DateTime fTime = toTimeStamp(fields[1]);
              table->set(4, row, timeFormatter->format(fTime));

              Fmi::DateTime lTime = toTimeStamp(fields[2]);
              table->set(5, row, timeFormatter->format(lTime));

              Fmi::DateTime mTime = toTimeStamp(fields[3]);
              table->set(6, row, timeFormatter->format(mTime));
            }
            else
            {
              table->set(3, row, fields[0]);
              table->set(4, row, fields[1]);
              table->set(5, row, fields[2]);
              table->set(6, row, fields[3]);
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

    table->setNames(theNames);
    return table;
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

  if (!theReactor.addAdminCustomRequestHandler(
            this,
            "clusterinfo",
            false,
             std::bind(&Frontend::Plugin::requestClusterInfo, this, p::_2, p::_3),
             "Get cluster info"))
  {
    throw Fmi::Exception(BCP, "Failed to register clusterinfo request handler");
  }

  if (!theReactor.addAdminTableRequestHandler(
            this,
            "backends",
            false,
            std::bind(&Frontend::Plugin::requestBackendInfo, this, p::_1, p::_2),
            "Get backend info"))
  {
    throw Fmi::Exception(BCP, "Failed to register backends request handler");
  }

  if (!theReactor.addAdminCustomRequestHandler(
        this,
        "qengine",
        false,
        std::bind(&Plugin::requestQEngineStatus, this, p::_1, p::_2, p::_3),
        "Available querydata"))
  {
    throw Fmi::Exception(BCP, "Failed to register qengine request handler");
  }

  if (!theReactor.addAdminTableRequestHandler(
        this,
        "gridgenerations",
        false,
        std::bind(&Plugin::requestStatus, this, p::_1, p::_2, "gridgenerations"),
        "Available grid generations"))
  {
    throw Fmi::Exception(BCP, "Failed to register gridgenerations request handler");
  }

  if (!theReactor.addAdminTableRequestHandler(
        this,
        "gridgenerationsqd",
        false,
        std::bind(&Plugin::requestStatus, this, p::_1, p::_2, "gridgenerationsqd"),
        "Available grid newbase generations"))
  {
    throw Fmi::Exception(BCP, "Failed to register gridgenerations request handler");
  }

  if (!theReactor.addAdminTableRequestHandler(
        this,
        "activebackends",
        false,
        std::bind(&Plugin::requestActiveBackends, this, p::_1),
        "Active backends"))
    {
        throw Fmi::Exception(BCP, "Failed to register activebackends request handler");
    }

  if (!theReactor.addAdminStringRequestHandler(
        this,
        "pause",
        true,
        std::bind(&Plugin::requestPause, this, p::_2),
        "Pause the frontend"))
  {
        throw Fmi::Exception(BCP, "Failed to register pause request handler");
  }

  if (!theReactor.addAdminStringRequestHandler(
        this,
        "continue",
        true,
        std::bind(&Plugin::requestContinue, this, p::_2),
        "Continue the frontend"))
  {
        throw Fmi::Exception(BCP, "Failed to register continue request handler");
  }

  if (!theReactor.addAdminTableRequestHandler(
        this,
        "list:frontend",
        false,
        std::bind(&Spine::ContentHandlerMap::getTargetAdminRequests,
                  &theReactor,
                  Spine::ContentHandlerMap::HandlerTarget(this)),
        "List available requests of frontend plugin"))
  {
        throw Fmi::Exception(BCP, "Failed to register list request handler");
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
