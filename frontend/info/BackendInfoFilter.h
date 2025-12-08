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
    /**
     * @brief Constructor with optional filtering criteria
     *
     * @param producer Optional producer name to filter by (absent = no filtering - all producers accepted)
     * @param parameters List of parameters to filter by (empty = no filtering - all parameters accepted)
     * @param all If true, all parameters must be present; if false, at least one parameter must be present
     */
    BackendInfoFilter(
        std::optional<std::string> producer = std::nullopt,
        const std::vector<std::string>& parameters = {},
        bool all = true);

    ~BackendInfoFilter();

    virtual bool operator()(const BackendInfoRec& record) const;

private:
    const std::optional<std::string> itsProducer;
    const std::vector<std::string> itsParameters;
    const bool itsAll;
};

}  // namespace Frontend
}  // namespace Plugin
}  // namespace SmartMet