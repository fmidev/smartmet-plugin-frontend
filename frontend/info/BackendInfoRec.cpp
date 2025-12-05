#include "BackendInfoRec.h"
#include <macgyver/Exception.h>
#include <macgyver/StringConversion.h>
#include <macgyver/TimeFormatter.h>
#include <macgyver/TimeParser.h>
#include <boost/algorithm/string.hpp>
#include <algorithm>

using namespace SmartMet::Plugin::Frontend;

BackendInfoRec::BackendInfoRec(const Json::Value& jsonObject, const std::string& time_format)
try
    : time_format(time_format)
    , time_formatter(Fmi::TimeFormatter::create(time_format))
{
    if (jsonObject.type() != Json::ValueType::objectValue)
    {
        Fmi::Exception ex = Fmi::Exception::Trace(BCP, "Invalid JSON object for BackendInfoRec");
        ex.addParameter("JSON", jsonObject.toStyledString().substr(0, 200));
        throw ex;
    }
}
catch (...)
{
  throw Fmi::Exception::Trace(BCP, "Operation failed!");
}

BackendInfoRec::~BackendInfoRec() = default;


std::string BackendInfoRec::format_datetime(const Fmi::DateTime& dt) const
{
  try
  {
    return time_formatter->format(dt);
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Failed to format DateTime");
  }
}


std::string BackendInfoRec::get_string_field(const Json::Value& jsonObject,
                                             const std::string& fieldName,
                                             const std::optional<std::string>& defaultValue)
{
  if (!jsonObject.isMember(fieldName))
  {
    if (defaultValue)
      return *defaultValue;

    throw Fmi::Exception(BCP, "Missing field '" + fieldName + "' in JSON object");
  }

  return jsonObject[fieldName].asString();
}

Fmi::DateTime BackendInfoRec::get_datetime_field(const Json::Value& jsonObject,
                                                  const std::string& fieldName) const
{
  std::string dateTimeStr = get_string_field(jsonObject, fieldName, std::optional<std::string>("nan"));
  try
  {
    Fmi::DateTime dt;  // to suppress unused variable
    if (dateTimeStr != "nan")
      dt = Fmi::TimeParser::parse(dateTimeStr, time_format);
    return dt;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Failed to parse datetime field '" + fieldName +
                                 "' with value '" + dateTimeStr + "'");
  }
}


int64_t BackendInfoRec::get_integer_field(const Json::Value& jsonObject,
                                           const std::string& fieldName,
                                           const std::optional<int64_t>& defaultValue) const
{
  if (!jsonObject.isMember(fieldName))
  {
    if (defaultValue)
      return *defaultValue;

    throw Fmi::Exception(BCP, "Missing field '" + fieldName + "' in JSON object");
  }

  try
  {
    return jsonObject[fieldName].asInt64();
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Failed to parse integer field '" + fieldName + "'");
  }
}

std::vector<std::string>
BackendInfoRec::get_string_vector_field(const Json::Value& jsonObject,
                                        const std::string& fieldName,
                                        const std::string& separators)
{
  std::vector<std::string> result;
  if (!jsonObject.isMember(fieldName))
    return result;

  std::string tmp = boost::algorithm::trim_copy(jsonObject[fieldName].asString());
  if (tmp.empty())
    return result;

  boost::algorithm::split(
      result, tmp, boost::algorithm::is_any_of(separators), boost::token_compress_on);
  for (std::string& strValue : result)
  {
    boost::algorithm::trim(strValue);
  }
  return result;
}


std::vector<int64_t>
BackendInfoRec::get_integer_vector_field(const Json::Value& jsonObject,
                                          const std::string& fieldName,
                                          const std::string& separators)
{
  std::vector<int64_t> result;
  if (!jsonObject.isMember(fieldName))
    return result;

  const Json::Value& fieldValue = jsonObject[fieldName];

  //std::cout << "Parsing integer vector field '" << fieldName
  //          << "' with value: " << fieldValue.toStyledString()
  //          << " type=" << int(fieldValue.type()) << std::endl;

  switch (fieldValue.type())
  {
    case Json::ValueType::nullValue:
      return result;
    case Json::ValueType::intValue:
    case Json::ValueType::uintValue:
      result.push_back(jsonObject[fieldName].asInt64());
      return result;

    case Json::ValueType::stringValue:
      {
        std::string tmp = jsonObject[fieldName].asString();
        std::vector<std::string> strValues;
        boost::algorithm::split(
            strValues, tmp, boost::algorithm::is_any_of(separators), boost::token_compress_on);
        for (const auto& strValue : strValues)
        {
          if (strValue != "-")
            result.push_back(Fmi::stol(strValue));
        }
      }
      return result;
    default:
      throw Fmi::Exception(BCP, "Failed to parse integer vector field '" + fieldName + "'");
  }
}
