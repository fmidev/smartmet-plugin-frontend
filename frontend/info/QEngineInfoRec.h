#pragma once

#include "BackendInfoRec.h"

namespace SmartMet
{
namespace Plugin
{
namespace Frontend
{

class QEngineInfoRec : public BackendInfoRec
{
public:
    const std::string producer;
    std::vector<std::string> aliases;
    int refreshInterval;
    std::string path;
    std::vector<std::string> parameters;
    std::vector<std::string> descriptions;
    std::vector<int64_t> levels;
    std::string projection;
    Fmi::DateTime originTime;
    Fmi::DateTime minTime;
    Fmi::DateTime maxTime;
    Fmi::DateTime loadTime;

    QEngineInfoRec(const Json::Value& jsonObject, const std::string& timeFormat);

    std::vector<std::string> as_vector(const std::string& timeFormat) const override;

    Json::Value as_json(const std::string& timeFormat) const override;

    std::string get_title() const override;

    const std::vector<std::string> get_names() const override;

    bool operator < (const BackendInfoRec& other) const override;

    const std::string& get_producer() const override;

    const std::vector<std::string>& get_parameters() const override;

    bool contains_parameters(const std::vector<std::string>& parameters, bool all = true) const override;

    ~QEngineInfoRec() override;
};

}  // namespace Frontend
}  // namespace Plugin
}  // namespace SmartMet
