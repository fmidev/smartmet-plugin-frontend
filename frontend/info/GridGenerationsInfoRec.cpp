#include "GridGenerationsInfoRec.h"

#include <macgyver/Exception.h>
#include <macgyver/Join.h>

using namespace SmartMet::Plugin::Frontend;
using namespace std::string_literals;

GridGenerationsInfoRec::GridGenerationsInfoRec(const Json::Value& jsonObject, const std::string& timeFormat)
try
    : BackendInfoRec(jsonObject, timeFormat)
    , producer(get_string_field(jsonObject, "ProducerName"))
    , geometryId(static_cast<int>(get_integer_field(jsonObject, "GeometryId")))
    , timesteps(static_cast<int>(get_integer_field(jsonObject, "Timesteps")))
    , analysisTime(get_datetime_field(jsonObject, "AnalysisTime"))
    , minTime(get_datetime_field(jsonObject, "MinTime"))
    , maxTime(get_datetime_field(jsonObject, "MaxTime"))
    , modificationTime(get_datetime_field(jsonObject, "ModificationTime"))
    , fmiParameter(get_string_vector_field(jsonObject, "FmiParameter", " ,"))
    , parameterAliases(get_string_vector_field(jsonObject, "ParameterAliases", " ,"))
{
}
catch (...)
{
    Fmi::Exception ex = Fmi::Exception::Trace(BCP, "Operation failed!");
    ex.addParameter("JSON", jsonObject.toStyledString());
    std::cerr << "Error constructing GridGenerationsInfoRec:" << std::endl;
    std::cerr << ex << std::endl;
    throw ex;
}

GridGenerationsInfoRec::~GridGenerationsInfoRec() = default;

std::vector<std::string> GridGenerationsInfoRec::as_vector() const
try
{
    std::vector<std::string> result;
    result.push_back(producer);
    result.push_back(Fmi::to_string(geometryId));
    result.push_back(Fmi::to_string(timesteps));
    result.push_back(format_datetime(analysisTime));
    result.push_back(format_datetime(minTime));
    result.push_back(format_datetime(maxTime));
    result.push_back(format_datetime(modificationTime));
    result.push_back(fmiParameter.empty() ? "nan"s : Fmi::join(fmiParameter, ", "));
    result.push_back(parameterAliases.empty() ? "nan"s : Fmi::join(parameterAliases, ", "));
    return result;
}
catch (...)
{
  throw Fmi::Exception::Trace(BCP, "Operation failed!");
}


Json::Value GridGenerationsInfoRec::as_json() const
try
{
    Json::Value jsonObject;
    jsonObject["ProducerName"] = producer;
    jsonObject["GeometryId"] = geometryId;
    jsonObject["Timesteps"] = timesteps;
    jsonObject["AnalysisTime"] = analysisTime.to_iso_extended_string();
    jsonObject["MinTime"] = minTime.to_iso_extended_string();
    jsonObject["MaxTime"] = maxTime.to_iso_extended_string();
    jsonObject["ModificationTime"] = modificationTime.to_iso_extended_string();
    jsonObject["FmiParameter"] = fmiParameter.empty() ? Json::nullValue : Json::arrayValue;
    for (const auto& param : fmiParameter)
      jsonObject["FmiParameter"].append(param);
    jsonObject["ParameterAliases"] = parameterAliases.empty() ? Json::nullValue : Json::arrayValue;
    for (const auto& alias : parameterAliases)
      jsonObject["ParameterAliases"].append(alias);
    return jsonObject;
}
catch (...)
{
  throw Fmi::Exception::Trace(BCP, "Operation failed!");
}


const std::vector<std::string> GridGenerationsInfoRec::get_names() const
{
    return {
        "Producer",
        "GeometryId",
        "Timesteps",
        "AnalysisTime",
        "MinTime",
        "MaxTime",
        "ModificationTime",
        "FmiParameter",
        "ParameterAliases"
    };
}



bool GridGenerationsInfoRec::operator < (const BackendInfoRec& other) const
try
{
    const auto& o = dynamic_cast<const GridGenerationsInfoRec&>(other);
    if (producer != o.producer)
        return producer < o.producer;
    if (geometryId != o.geometryId)
        return geometryId < o.geometryId;
    if (analysisTime != o.analysisTime)
        return analysisTime < o.analysisTime;
    return false;
}
catch (const std::bad_cast& )
{
    throw Fmi::Exception::Trace(BCP, "Invalid type for BackendInfoRec comparison");
}


const std::string& GridGenerationsInfoRec::get_producer() const
{
    return producer;
}


const std::vector<std::string>& GridGenerationsInfoRec::get_parameters() const
{
    return fmiParameter;
}
