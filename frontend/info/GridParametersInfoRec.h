#pragma once

#include "BackendInfoRec.h"

namespace SmartMet
{
namespace Plugin
{
namespace Frontend
{

/* Sample:
  {
    "#": 1,
    "Producer": "MNWC",
    "FmiParameterName": "ALBEDOBGRND-0TO1",
    "FmiParameterId": 1062,
    "NewbaseParameterName": null,
    "NewbaseParameterId": 0,
    "Unit": "0to1",
    "Description": "Albedo of bare ground"
  },
*/

class GridParametersInfoRec : public BackendInfoRec
{
public:
    const std::string producer;
    const std::string fmiParameterName;
    const int fmiParameterId;
    std::optional<std::string> newbaseParameterName;
    std::optional<int> newbaseParameterId;
    std::string description;

    GridParametersInfoRec(const Json::Value& jsonObject, const std::string& timeFormat);

    std::vector<std::string> as_vector(const std::string& timeFormat) const override;

    Json::Value as_json(const std::string& timeFormat) const override;

    std::string get_title() const override;

    const std::vector<std::string> get_names() const override;

    bool operator < (const BackendInfoRec& other) const override;

    const std::string& get_producer() const override;

    const std::vector<std::string>& get_parameters() const override;

    bool contains_parameters(const std::vector<std::string>& parameters, bool all = true) const override;

    ~GridParametersInfoRec() override;
};

} // namespace Frontend
} // namespace Plugin
} // namespace SmartMet
