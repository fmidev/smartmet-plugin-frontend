#include "QEngineInfoRec.h"
#include <macgyver/Exception.h>
#include <macgyver/Join.h>
#include <macgyver/StringConversion.h>

using namespace SmartMet::Plugin::Frontend;
using namespace std::string_literals;

QEngineInfoRec::QEngineInfoRec(const Json::Value& jsonObject, const std::string& timeFormat)
try
    : BackendInfoRec(jsonObject, timeFormat)
    , producer(get_string_field(jsonObject, "Producer"))
    , aliases(get_string_vector_field(jsonObject, "Aliases", " ,"))
    , refreshInterval(get_integer_field(jsonObject, "RI", std::nullopt))
    , path(get_string_field(jsonObject, "Path"))
    , parameters(get_string_vector_field(jsonObject, "Parameters", " ,"))
    , descriptions(get_string_vector_field(jsonObject, "Descriptions", ","))
    , levels(get_integer_vector_field(jsonObject, "Levels", " ,"))
    , projection(get_string_field(jsonObject, "Projection"))
    , originTime(get_datetime_field(jsonObject, "OriginTime"))
    , minTime(get_datetime_field(jsonObject, "MinTime"))
    , maxTime(get_datetime_field(jsonObject, "MaxTime"))
    , loadTime(get_datetime_field(jsonObject, "LoadTime"))
{
}
catch (...)
{
    Fmi::Exception ex = Fmi::Exception::Trace(BCP, "Operation failed!");
    ex.addParameter("JSON", jsonObject.toStyledString());
    std::cerr << "Error constructing QEngineInfoRec:" << std::endl;
    std::cerr << ex << std::endl;
    throw ex;
}

QEngineInfoRec::~QEngineInfoRec() = default;


const std::string& QEngineInfoRec::get_producer() const
{
    return producer;
}


const std::vector<std::string>& QEngineInfoRec::get_parameters() const
{
    return parameters;
}


bool QEngineInfoRec::contains_parameters(const std::vector<std::string>& params, bool all) const
{
    return lookup_parameters(params, all, parameters);
}


std::vector<std::string> QEngineInfoRec::as_vector(const std::string& timeFormat) const
try
{
  std::unique_ptr<Fmi::TimeFormatter> formatter(Fmi::TimeFormatter::create(timeFormat));
  std::vector<std::string> result;
  result.push_back(producer);
  result.push_back(aliases.empty() ? "nan"s : Fmi::join(aliases, ", "));
  result.push_back(Fmi::to_string(refreshInterval));
  result.push_back(path);
  result.push_back(parameters.empty() ? "nan"s : Fmi::join(parameters, ", "));
  result.push_back(descriptions.empty() ? "nan"s : Fmi::join(descriptions, ", "));
  result.push_back(levels.empty() ? "nan"s : Fmi::join(levels, [](auto x){ return Fmi::to_string(x); }, ", "));
  result.push_back(projection);
  result.push_back(formatter->format(originTime));
  result.push_back(formatter->format(minTime));
  result.push_back(formatter->format(maxTime));
  result.push_back(formatter->format(loadTime));
  return result;
}
catch (...)
{
  throw Fmi::Exception::Trace(BCP, "Operation failed!");
}


Json::Value QEngineInfoRec::as_json(const std::string& timeFormat) const
try
{
  std::unique_ptr<Fmi::TimeFormatter> formatter(Fmi::TimeFormatter::create(timeFormat));
  Json::Value jsonObject;
  jsonObject["Producer"] = producer;
  jsonObject["Aliases"] = aliases.empty() ? Json::nullValue : Json::arrayValue;
  for (const auto& alias : aliases)
    jsonObject["Aliases"].append(alias);
  jsonObject["RI"] = refreshInterval;
  jsonObject["Path"] = path;
  jsonObject["Parameters"] = parameters.empty() ? Json::nullValue : Json::arrayValue;
  for (const auto& param : parameters)
    jsonObject["Parameters"].append(param);
  jsonObject["Descriptions"] = descriptions.empty() ? Json::nullValue : Json::arrayValue;
  for (const auto& desc : descriptions)
    jsonObject["Descriptions"].append(desc);
  jsonObject["Levels"] = levels.empty() ? Json::nullValue : Json::arrayValue;
  for (const auto& level : levels)
    jsonObject["Levels"].append(level);
  jsonObject["Projection"] = projection;
  jsonObject["OriginTime"] = formatter->format(originTime);
  jsonObject["MinTime"] = formatter->format(minTime);
  jsonObject["MaxTime"] = formatter->format(maxTime);
  jsonObject["LoadTime"] = formatter->format(loadTime);
  return jsonObject;
}
catch (...)
{
  throw Fmi::Exception::Trace(BCP, "Operation failed!");
}


std::string QEngineInfoRec::get_title() const
{
    return "Available querydata";
}

const std::vector<std::string> QEngineInfoRec::get_names() const
{
    return {
        "producer",
        "aliases",
        "refreshInterval",
        "path",
        "parameters",
        "descriptions",
        "levels",
        "projection",
        "originTime",
        "minTime",
        "maxTime",
        "loadTime"
    };
}

bool QEngineInfoRec::operator < (const BackendInfoRec& other) const
{
  const auto* o = dynamic_cast<const QEngineInfoRec*>(&other);
  if (!o)
    throw Fmi::Exception(BCP, "Invalid comparison between different BackendInfoRec types");

  if (originTime != o->originTime)
    return originTime < o->originTime;

  if (path != o->path)
    return path < o->path;

  if (producer != o->producer)
    return producer < o->producer;

  return false;
}
