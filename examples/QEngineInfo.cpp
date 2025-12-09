#include "BackendInfoRequests.h"
#include <macgyver/Exception.h>
#include <fmt/format.h>
#include <iostream>
#include <dlfcn.h>

using namespace SmartMet::Plugin::Frontend;
using namespace std::string_literals;

int main(int argc, char*argv[])
try
{
    if (argc < 2)
    {
        std::cerr << "Usage: QEngineInfoRequest <host1:port1> [host2:port2 [...]]" << std::endl;
        return -1;
    }

    BackendInfoRequests backendInfoRequests;

    std::vector<BackendInfoRequests::BackendAddr> backends;
    for (int i = 1; i < argc; ++i)
    {
        std::string host;
        int port = 0;
        std::string arg = argv[i];
        auto pos = arg.find(':');
        const auto id = fmt::format("{:04}", i);
        if (pos != std::string::npos)
        {
            host = arg.substr(0, pos);
            port = std::stoi(arg.substr(pos + 1));
            backends.emplace_back(id, host, port);
        }
        else
        {
            host = arg;
            port = 80;  // Default port if not specified
            backends.emplace_back(id, host, port);
        }
    }

    SmartMet::Spine::HTTP::Request req;
    req.setParameter("what", "qengine");
    req.setParameter("timeformat", "iso");
    req.setResource("/info");

    auto response = backendInfoRequests.perform_backend_info_request(backends, req);
    if (response)
    {
        std::cout << response->as_json().toStyledString() << std::endl;
    }
    else
    {
        std::cout << "No response received from backend(s)." << std::endl;
    }

    return 0;
}
catch (const std::exception& ex)
{
    std::cerr << "Error: " << ex.what() << std::endl;
    return -1;
}

