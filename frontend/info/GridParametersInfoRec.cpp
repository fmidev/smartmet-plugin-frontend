#include "GridParametersInfoRec.h"

#include <macgyver/Exception.h>
#include <macgyver/Join.h>

using namespace SmartMet::Plugin::Frontend;
using namespace std::string_literals;

GridParametersInfoRec::GridParametersInfoRec(
    const Json::Value& jsonObject,
    const std::string& timeFormat)
try
    : BackendInfoRec(jsonObject, timeFormat)
    , producer(get_string_field(jsonObject, "Producer"))
    , fmiParameterName(get_string_field(jsonObject, "FmiParameterName"))
    , fmiParameterId(static_cast<int>(get_integer_field(jsonObject, "FmiParameterId")))
    , newbaseParameterName(get_string_field(jsonObject, "NewbaseParameterName", std::nullopt))
    , newbaseParameterId(get_integer_field(jsonObject, "NewbaseParameterId", std::nullopt))
    , description(get_string_field(jsonObject, "Description"))
{
}
catch (...)
{
    Fmi::Exception ex = Fmi::Exception::Trace(BCP, "Operation failed!");
    ex.addParameter("JSON", jsonObject.toStyledString());
    std::cerr << "Error constructing GridParametersInfoRec:" << std::endl;
    std::cerr << ex << std::endl;
    throw ex;
}

GridParametersInfoRec::~GridParametersInfoRec() = default;

std::vector<std::string> GridParametersInfoRec::as_vector(const std::string& timeFormat) const
try
{
    std::vector<std::string> result;
    result.push_back(producer);
    result.push_back(fmiParameterName);
    result.push_back(Fmi::to_string(fmiParameterId));
    result.push_back(newbaseParameterName.value_or("nan"));
    result.push_back(newbaseParameterId.has_value() ? Fmi::to_string(newbaseParameterId.value()) : "nan");
    result.push_back(description);
    return result;
}
catch (...)
{
  throw Fmi::Exception::Trace(BCP, "Operation failed!");
}

Json::Value GridParametersInfoRec::as_json(const std::string& timeFormat) const
try
{
    Json::Value jsonObject;
    jsonObject["Producer"] = producer;
    jsonObject["FmiParameterName"] = fmiParameterName;
    jsonObject["FmiParameterId"] = fmiParameterId;
    if (newbaseParameterName.has_value())
        jsonObject["NewbaseParameterName"] = newbaseParameterName.value();
    else
        jsonObject["NewbaseParameterName"] = Json::nullValue;

    if (newbaseParameterId.has_value())
        jsonObject["NewbaseParameterId"] = newbaseParameterId.value();
    else
        jsonObject["NewbaseParameterId"] = Json::nullValue;

    jsonObject["Description"] = description;
    return jsonObject;
}
catch (...)
{
  throw Fmi::Exception::Trace(BCP, "Operation failed!");
}

std::string GridParametersInfoRec::get_title() const
{
    return "Grid parameter information";
}

const std::vector<std::string> GridParametersInfoRec::get_names() const
{
    return {
        "Producer",
        "FmiParameterName",
        "FmiParameterId",
        "NewbaseParameterName",
        "NewbaseParameterId",
        "Description"
    };
}

bool GridParametersInfoRec::operator < (const BackendInfoRec& other) const
{
    const auto* o = dynamic_cast<const GridParametersInfoRec*>(&other);
    if (o == nullptr)
        throw Fmi::Exception(BCP, "Incompatible types when comparing GridParametersInfoRec");

    if (producer != o->producer)
        return producer < o->producer;
    return fmiParameterName < o->fmiParameterName;
}

const std::string& GridParametersInfoRec::get_producer() const
{
    return producer;
}

const std::vector<std::string>& GridParametersInfoRec::get_parameters() const
{
    // Not actually used for this record type, but return the parameter name as a single-item vector to allow filtering by parameter name
    static const std::vector<std::string> params = {"FmiParameterName"};
    return params;
}

bool GridParametersInfoRec::contains_parameters(const std::vector<std::string>&, bool all) const
{
    // This record type doesn't have a list of parameters, so it only matches if no parameters are specified in the filter
    return false;
}
