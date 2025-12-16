#pragma once

#include "BackendInfoResponse.h"
#include "BackendInfoFilter.h"
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <macgyver/DateTime.h>
#include <spine/HTTP.h>
#include <spine/Reactor.h>
#include <spine/Table.h>
#include <engines/sputnik/Engine.h>
#include <json/json.h>

namespace SmartMet
{
namespace Plugin
{
namespace Frontend
{

class BackendInfoRequests
{
public:
    using BackendAddr = typename SmartMet::Services::BackendList::value_type;

    BackendInfoRequests();

    virtual ~BackendInfoRequests();

    void register_requests(SmartMet::Spine::Reactor& rector);

    std::shared_ptr<BackendInfoResponse>
    perform_backend_info_request(
        std::vector<BackendAddr>& backends,
        const SmartMet::Spine::HTTP::Request& frontendRequest);

private:

    std::unique_ptr<SmartMet::Spine::Table>
    handle_table_request(
        SmartMet::Spine::Reactor& reactor,
        const SmartMet::Spine::HTTP::Request& request);

    void
    handle_json_request(
        SmartMet::Spine::Reactor& reactor,
        const SmartMet::Spine::HTTP::Request& request,
        SmartMet::Spine::HTTP::Response& response);

    struct RequestInfo
    {
        std::string what;
        std::string request;
        std::optional<std::string> producer;
        std::string timeformat;
        std::string type;
        std::vector<std::string> parameters;
        std::function<std::shared_ptr<BackendInfoRec>(const Json::Value&, const std::string& timeFormat)> recordFactory;

        RequestInfo(const SmartMet::Spine::HTTP::Request& request);
    };

    std::vector<std::shared_ptr<BackendInfoResponse>> collect_backend_info_responses(
        const std::vector<BackendAddr>& backends,
        const RequestInfo& ri);

    BackendInfoFilter create_record_filter(const RequestInfo& ri);

    SmartMet::Spine::HTTP::Request build_backend_request(
        const std::string& host,
        int port,
        const RequestInfo& ri);
};


}  // namespace Frontend
}  // namespace Plugin
}  // namespace SmartMet
