#pragma once

#include <vector>
#include <string>
#include <optional>
#include "BackendInfoRec.h"

namespace SmartMet
{
namespace Plugin
{
namespace Frontend
{

class BackendInfoFilter
{
public:
    BackendInfoFilter(
        std::optional<std::string> producer = std::nullopt,
        const std::vector<std::string>& parameters = {});

    ~BackendInfoFilter();

    virtual bool operator()(const BackendInfoRec& record) const;

private:
    const std::optional<std::string> itsProducer;
    const std::vector<std::string> itsParameters;
};

}  // namespace Frontend
}  // namespace Plugin
}  // namespace SmartMet