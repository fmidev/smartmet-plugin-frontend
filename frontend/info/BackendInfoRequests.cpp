#include "BackendInfoRequests.h"
#include "QEngineInfoRec.h"
#include "GridGenerationsInfoRec.h"
#include <macgyver/Exception.h>
#include <macgyver/TimeFormatter.h>
#include <spine/HTTP.h>
#include <spine/Reactor.h>
#include <spine/Table.h>
#include <spine/TcpMultiQuery.h>
#include <engines/sputnik/Engine.h>
#include <fmt/format.h>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/thread.hpp>

using namespace SmartMet::Plugin::Frontend;
using namespace std::string_literals;



namespace
{

struct BackendInfoRequestsSetupInfo
{
    const std::string request;
    bool supports_param_ids;        // Whether this request supports type=json/table
    std::function<std::shared_ptr<BackendInfoRec>(const Json::Value&, const std::string& timeFormat)> recordFactory;
    std::string title;
};


// Map of supported frontend info requests to backend ones
// Currently identicatical, but this allows future changes if needed
//
// Always use "iso" time format for backend requests; frontend can reformat as needed.
// No need ti different formats here.
const std::map<std::string, BackendInfoRequestsSetupInfo> backendInfoRequestMap = {
    {
        "qengine",
        {
            "qengine"
            , true
            , [](const Json::Value& jsonObject, const std::string& timeFormat)
              {
                  return std::make_shared<QEngineInfoRec>(jsonObject, "iso"s);
              }
            , "Available Querydata"
        }
    }

    ,{
        "gridgenerations",
        {
            "gridgenerations"
            , false
            , [](const Json::Value& jsonObject, const std::string& timeFormat)
              {
                  return std::make_shared<GridGenerationsInfoRec>(jsonObject, "iso"s);
              }
            , "Available Grid Generations"
        }
    }

    ,{
        "gridgenerationsqd"
        , {
            "gridgenerationsqd"
            , false
            , [](const Json::Value& jsonObject, const std::string& timeFormat)
              {
                  return std::make_shared<GridGenerationsInfoRec>(jsonObject, "iso"s);
              }
            , "Available QD Grid Generations"
        }
    }
};

} // anonymousnamespace



BackendInfoRequests::BackendInfoRequests()
{
}



BackendInfoRequests::~BackendInfoRequests() = default;



std::shared_ptr<BackendInfoResponse>
BackendInfoRequests::perform_backend_info_request(
    std::vector<BackendAddr>& backends,
    const SmartMet::Spine::HTTP::Request& frontendRequest)
try
{
    RequestInfo ri(frontendRequest);
    std::shared_ptr<BackendInfoResponse> result;
    auto responses = collect_backend_info_responses(backends, ri);
    if (responses.empty())
        return result;

    // Merge responses
    result = std::make_shared<BackendInfoResponse>(responses);
    return result;
}
catch (...)
{
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
}


std::vector<std::shared_ptr<BackendInfoResponse>> BackendInfoRequests::collect_backend_info_responses(
    const std::vector<BackendAddr>& backends,
    const RequestInfo& ri)
try
{
    constexpr const int backend_timeout_sec = 10;

    std::vector<std::shared_ptr<BackendInfoResponse>> responses;

    int counter = 0;
    std::vector<std::pair<std::string, std::string>> id_mapping;
    Spine::TcpMultiQuery multi_query(backend_timeout_sec);
    for (const auto& backend : backends)
    {
        const std::string& host = backend.get<1>();
        const int port = backend.get<2>();
        SmartMet::Spine::HTTP::Request request = build_backend_request(host, port, ri);
        const std::string request_str = request.toString();
        const std::string id = fmt::format("{0:05d}", ++counter);
        id_mapping.emplace_back(std::make_pair(fmt::format("{}:{}", host, port), id));
        multi_query.add_query(id, host, std::to_string(port), request_str);

        std::cout << "Frontend::getBackendMessages: sending backend info request to "
                  << host << ":" << port << " for '" << ri.what << "'" << std::endl;
        std::cout << request_str << std::endl;
    }

    multi_query.execute();

    for (auto &item : id_mapping)
    {
        const auto result = multi_query[item.second];
        if (result.error_code)
        {
            std::cout << "Frontend::getBackendMessages: failed to get response from backend "
                      << item.first << ": " << result.error_code.message() << std::endl;
            continue;
        }

        std::string rawResponse = result.body;
        auto response_info = SmartMet::Spine::HTTP::parseResponseFull(rawResponse);
        const auto status = std::get<0>(response_info);
        auto& response_ptr = std::get<1>(response_info);

        if (status != SmartMet::Spine::HTTP::ParsingStatus::COMPLETE)
        {
            std::cout << "Frontend::getBackendMessages: failed to parse response from backend "
                      << item.first << std::endl;
            continue;
        }

        if (response_ptr->getStatus() != SmartMet::Spine::HTTP::Status::ok)
        {
            std::cout << "Frontend::getBackendMessages: backend "
                      << item.first << " returned HTTP status "
                      << static_cast<int>(response_ptr->getStatus()) << std::endl;
            continue;
        }

        try
        {
            Json::Value jsonRoot;
            Json::CharReaderBuilder builder;
            std::string errs;
            const std::string content = response_ptr->getDecodedContent();
            std::istringstream ss(content);
            if (!Json::parseFromStream(builder, ss, &jsonRoot, &errs))
            {
                std::cout << "Frontend::getBackendMessages: failed to parse JSON response from backend "
                          << item.first << ": " << errs << std::endl;
                continue;
            }

            BackendInfoFilter recordFilter = create_record_filter(ri);
            auto backendResponse = std::make_shared<BackendInfoResponse>(
                jsonRoot,
                ri.recordFactory,
                recordFilter,
                ri.timeformat);
            responses.push_back(backendResponse);
        }
        catch (...)
        {
            std::cout << Fmi::Exception::Trace(BCP,
                "Frontend::getBackendMessages: exception while processing response from backend ");
        }
    }
    return responses;
}
catch (...)
{
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
}



BackendInfoFilter BackendInfoRequests::create_record_filter(const RequestInfo& ri)
{
    BackendInfoFilter filter(ri.producer, ri.parameters, true);
    return filter;
}



SmartMet::Spine::HTTP::Request
BackendInfoRequests::build_backend_request(
    const std::string& host,
    int port,
    const RequestInfo& ri)
{
    SmartMet::Spine::HTTP::Request request;

    request.setMethod(SmartMet::Spine::HTTP::RequestMethod::GET);
    request.setResource("/info"s);
    request.setHeader("Host", host);
    request.setHeader("Connection", "close");
    request.setHeader("Content-Type", "application/x-www-form-urlencoded");
    request.setHeader("Accept-Encoding", "gzip, deflate, zstd, lzma");
    request.addParameter("what", ri.what);
    request.addParameter("format", "json");
    request.addParameter("timeformat", "iso"s); // Use ISO format for backend requests

    return request;
}



BackendInfoRequests::RequestInfo::RequestInfo(const SmartMet::Spine::HTTP::Request& httpRequest)
try
{
    const std::optional<std::string> opt_what = httpRequest.getParameter("what");
    if (not opt_what)
        throw Fmi::Exception(BCP, "Missing 'what' parameter in backend info request");
    what = *opt_what;

    auto it = backendInfoRequestMap.find(what);
    if (it == backendInfoRequestMap.end())
        throw Fmi::Exception(BCP, "Unsupported 'what' parameter value '" + what +
                                     "' in backend info request");

    const std::optional<std::string> opt_timeformat = httpRequest.getParameter("timeformat");
    if (opt_timeformat)
        timeformat = *opt_timeformat;
    else
        timeformat = "sql"; // Default time format

    // Verify that timeformat is valid (no use continuing to fail at end)
    std::unique_ptr<Fmi::TimeFormatter> formatter(Fmi::TimeFormatter::create(timeformat));
    (void)formatter; // Avoid unused variable warning

    const auto& setupInfo = it->second;

    request = setupInfo.request;

    producer = httpRequest.getParameter("producer");

    recordFactory = setupInfo.recordFactory;

    if (setupInfo.supports_param_ids)
    {
        const std::optional<std::string> opt_type = httpRequest.getParameter("type");
        type = opt_type ? *opt_type : "name"s;
    }
    else
    {
        type = "name"s;
    }

    for (const auto& item : httpRequest.getParameterList("param"))
    {
        std::vector<std::string> temp;
        boost::algorithm::split(temp, item, boost::is_any_of(" ,"), boost::token_compress_on);
        for (const auto& param : temp)
        {
            const auto name = boost::algorithm::trim_copy(param);
            if (name != "")
                parameters.push_back(name);
        }
    }
}
catch (...)
{
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
}
