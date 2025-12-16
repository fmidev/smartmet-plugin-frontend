#include "BackendInfoResponse.h"
#include <macgyver/Exception.h>
#include <macgyver/Join.h>
#include <macgyver/StringConversion.h>
#include <macgyver/TimeFormatter.h>
#include <macgyver/TimeParser.h>
#include <boost/algorithm/string.hpp>
#include <algorithm>

using namespace SmartMet::Plugin::Frontend;

BackendInfoResponse::BackendInfoResponse(
    const Json::Value& jsonObject,
    parser_t recordFactory,
    const BackendInfoFilter& recordFilter,
    const std::string& timeFormat)
try
{
  if (!jsonObject.isArray())
    throw Fmi::Exception(BCP, "Invalid JSON object for BackendInfoResponse");

  for (const Json::Value& item : jsonObject)
  {
    //std::cout << "Processing item: " << item.toStyledString() << std::endl;
    auto record = recordFactory(item, timeFormat);
    if (record and recordFilter(*record))
    {
      auto& dest = records[record->get_producer()];
      dest.push_back(std::move(record));
    }
  }
  for (auto& [producer, recordsVec] : records)
  {
    //std::cout << "Sorting records for producer: " << producer << std::endl;
    std::sort(recordsVec.begin(), recordsVec.end(), [](const auto& a, const auto& b) { return *b < *a; } );
  }
  // No summary, just one response from backend
  summary_size++;

  if (!records.empty())
  {
    // Set title from the first record
    const auto& firstRecord = records.begin()->second;
    title = firstRecord.front()->get_title();
  }
}
catch (...)
{
  Fmi::Exception ex = Fmi::Exception::Trace(BCP, "Operation failed!");
  std::cout << ex << std::endl;
  throw ex;
}


BackendInfoResponse::BackendInfoResponse(
    const std::vector<std::shared_ptr<BackendInfoResponse>>& responses)
try
{
  if (responses.empty())
    return;

  // Extract producers common for all responses
  bool start = true;
  std::set<std::string> commonProducers = responses.front()->get_producers();
  for (const auto& response : responses)
  {
    if (!response)
      continue;

    if (start)
    {
      start = false;
      commonProducers = response->get_producers();
      title = response->title;
    }
    else
    {
      std::set<std::string> currentProducers = response->get_producers();
      std::set<std::string> intersection;
      std::set_intersection(commonProducers.begin(), commonProducers.end(),
                            currentProducers.begin(), currentProducers.end(),
                            std::inserter(intersection, intersection.begin()));
      commonProducers = std::move(intersection);
    }
    summary_size++;
  }

  // For each common producer, collect records available from all responses
  for (const auto& producer : commonProducers)
  {
    start = true;
    std::vector<std::shared_ptr<BackendInfoRec>> mergedRecords;
    for (const auto& response : responses)
    {
      if (start)
      {
        start = false;
        mergedRecords = response->records.at(producer);
      }
      else
      {
        // Data are sorted and as result one can use std::set_intersection
        //  to find common records
        std::vector<std::shared_ptr<BackendInfoRec>> tempRecords;
        const auto& currentRecords = response->records.at(producer);
        // Find common records between mergedRecords and currentRecords
        std::set_intersection(
          mergedRecords.begin(), mergedRecords.end(),
          currentRecords.begin(), currentRecords.end(),
          std::back_inserter(tempRecords),
          [](const std::shared_ptr<BackendInfoRec>& a,
             const std::shared_ptr<BackendInfoRec>& b)
          {
            return *a < *b;
          });
        mergedRecords = std::move(tempRecords);
      }
    }

    if (!mergedRecords.empty())
      records[producer] = std::move(mergedRecords);
  }
}
catch (...)
{
  throw Fmi::Exception::Trace(BCP, "Operation failed!");
}


BackendInfoResponse::BackendInfoResponse(
    const BackendInfoResponse& other,
    const BackendInfoFilter& recordFilter)
try
{
  for (const auto& [producer, recordsVec] : other.records)
  {
    for (const auto& record : recordsVec)
    {
      if (recordFilter(*record))
      {
        records[producer].push_back(record);
      }
    }
  }
}
catch (...)
{
  throw Fmi::Exception::Trace(BCP, "Operation failed!");
}


BackendInfoResponse::~BackendInfoResponse() = default;


std::set<std::string> BackendInfoResponse::get_producers() const
{
  try
  {
    std::set<std::string> producers;
    for (const auto& [producer, _] : records)
    {
      producers.insert(producer);
    }
    return producers;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}


const std::vector<std::shared_ptr<BackendInfoRec>>&
BackendInfoResponse::get_records(const std::string& producer) const
{
  try
  {
    auto it = records.find(producer);
    if (it == records.end())
    {
      static const std::vector<std::shared_ptr<BackendInfoRec>> emptyVector;
      return emptyVector;
    }
    return it->second;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}


std::unique_ptr<SmartMet::Spine::Table> BackendInfoResponse::to_table(const std::string& timeFormat) const
{
  try
  {
    auto table = std::make_unique<SmartMet::Spine::Table>();

    // If there are no records, return empty table
    if (records.empty())
      return table;

    // Get column names from the first record
    const auto& firstRecord = records.begin()->second;
    // Assuming all records have the same structure. Map values are
    // vectors of unique_ptrs and are guaranteed to have at least one element.
    std::vector<std::string> columnNames = firstRecord.front()->get_names();
    table->setTitle(title);
    table->setNames(columnNames);

    // Fill table rows
    int rowIndex = 0;
    for (const auto& [producer, recordsVec] : records)
    {
      for (const auto& record : recordsVec)
      {
        std::vector<std::string> row = record->as_vector(timeFormat);
        int columnIndex = 0;
        for (auto& value : row)
        {
          // Trim whitespace from each value
          boost::algorithm::trim(value);
          table->set(columnIndex++, rowIndex, value);
        }
        ++rowIndex;
      }
    }

    return table;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}


Json::Value BackendInfoResponse::as_json(const std::string& timeFormat) const
{
  try
  {
    Json::Value jsonObject;
    for (const auto& [producer, recordsVec] : records)
    {
      Json::Value producerArray(Json::arrayValue);
      for (const auto& record : recordsVec)
      {
        producerArray.append(record->as_json(timeFormat));
      }
      jsonObject[producer] = producerArray;
    }
    return jsonObject;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}