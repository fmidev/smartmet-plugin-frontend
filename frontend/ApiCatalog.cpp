#include "ApiCatalog.h"
#include <fmt/format.h>
#include <macgyver/Exception.h>
#include <spine/Convenience.h>
#include <spine/TcpMultiQuery.h>
#include <algorithm>
#include <iostream>
#include <map>
#include <sstream>

namespace SmartMet
{
namespace Plugin
{
namespace Frontend
{
namespace
{
namespace OA = Spine::OpenAPI;

constexpr std::chrono::seconds CACHE_TIME{60};
constexpr int BACKEND_TIMEOUT_SECONDS = 5;

// Without a Host header the backends answer with relative URLs, which are then resolved
// against the URL of the frontend
const char* const BACKEND_REQUEST =
    "GET /api?f=json HTTP/1.0\r\n"
    "Accept: application/linkset+json\r\n"
    "Connection: close\r\n"
    "\r\n";

}  // namespace

ApiCatalog::ApiCatalog(OA::CatalogSettings settings, SputnikGetter sputnik)
    : itsCatalog(std::move(settings)), itsSputnik(std::move(sputnik))
{
}

void ApiCatalog::handle(const Spine::HTTP::Request& theRequest, Spine::HTTP::Response& theResponse)
{
  try
  {
    // The file requests do not need the entries
    const auto& resource = theRequest.getResource();
    const bool files = (resource.size() > 10 && resource.compare(0, 10, "/api/docs/") == 0) ||
                       (resource.size() > 14 && resource.compare(0, 14, "/api/explorer/") == 0) ||
                       resource == "/api/explorer";
    const auto list = (files ? std::vector<OA::CatalogEntry>() : entries());

    if (!itsCatalog.handle(theRequest, theResponse, list))
    {
      theResponse.setStatus(Spine::HTTP::Status::not_found);
      theResponse.setHeader("Content-Type", "text/plain; charset=UTF-8");
      theResponse.setContent("Not found\n");
    }
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Failed to serve the API catalog");
  }
}

ApiCatalog::Backends ApiCatalog::backends() const
{
  Backends ret;
  auto* sputnik = itsSputnik();
  if (sputnik == nullptr)
    return ret;
  for (const auto& backend : sputnik->getBackendList("api"))
    ret.emplace(backend.get<1>(), backend.get<2>());
  return ret;
}

std::vector<OA::CatalogEntry> ApiCatalog::entries()
{
  try
  {
    const auto current = backends();
    const auto now = std::chrono::steady_clock::now();

    {
      std::lock_guard<std::mutex> lock(itsMutex);
      if (itsValid && now < itsExpires && current == itsBackends)
        return itsEntries;
    }

    // Serve the old catalog while another request refreshes it
    std::unique_lock<std::mutex> refresh(itsRefreshMutex, std::try_to_lock);
    if (!refresh.owns_lock())
    {
      {
        std::lock_guard<std::mutex> lock(itsMutex);
        if (itsValid)
          return itsEntries;
      }
      refresh.lock();
      std::lock_guard<std::mutex> lock(itsMutex);
      if (itsValid && current == itsBackends)
        return itsEntries;
    }

    auto merged = collect(current);

    // The APIs of plugins running in the frontend itself
    for (auto& entry : OA::localCatalogEntries())
    {
      if (std::none_of(merged.begin(),
                       merged.end(),
                       [&entry](const OA::CatalogEntry& e) { return e.root == entry.root; }))
        merged.push_back(std::move(entry));
    }
    std::sort(merged.begin(),
              merged.end(),
              [](const OA::CatalogEntry& a, const OA::CatalogEntry& b) { return a.root < b.root; });

    std::lock_guard<std::mutex> lock(itsMutex);
    itsValid = true;
    itsExpires = std::chrono::steady_clock::now() + CACHE_TIME;
    itsBackends = current;
    itsEntries = merged;
    return merged;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Failed to build the API catalog");
  }
}

std::vector<OA::CatalogEntry> ApiCatalog::collect(const Backends& theBackends)
{
  try
  {
    std::map<std::string, OA::CatalogEntry> merged;
    if (theBackends.empty())
      return {};

    Spine::TcpMultiQuery query(BACKEND_TIMEOUT_SECONDS);
    std::vector<std::pair<std::string, std::string>> ids;  // query id and backend name
    for (const auto& backend : theBackends)
    {
      const auto id = fmt::format("{:05d}", ids.size());
      ids.emplace_back(id, fmt::format("{}:{}", backend.first, backend.second));
      query.add_query(id, backend.first, std::to_string(backend.second), BACKEND_REQUEST);
    }
    query.execute();

    // Backends are visited in a fixed order, so the first title of an API is always the same
    for (const auto& id : ids)
    {
      const auto result = query[id.first];
      if (result.error_code)
      {
        std::cerr << fmt::format("Frontend: failed to get the API catalog of backend {}: {}\n",
                                 id.second,
                                 result.error_code.message());
        continue;
      }

      auto parsed = Spine::HTTP::parseResponseFull(result.body);
      auto& response = std::get<1>(parsed);
      if (std::get<0>(parsed) != Spine::HTTP::ParsingStatus::COMPLETE || !response ||
          response->getStatus() != Spine::HTTP::Status::ok)
      {
        std::cerr << fmt::format("Frontend: invalid API catalog response from backend {}\n",
                                 id.second);
        continue;
      }

      Json::Value json;
      Json::CharReaderBuilder builder;
      std::string errors;
      std::istringstream in(response->getDecodedContent());
      if (!Json::parseFromStream(builder, in, &json, &errors))
      {
        std::cerr << fmt::format(
            "Frontend: failed to parse the API catalog of backend {}: {}\n", id.second, errors);
        continue;
      }

      for (auto& entry : OA::parseCatalog(json))
      {
        auto pos = merged.find(entry.root);
        if (pos == merged.end())
          merged.emplace(entry.root, std::move(entry));
        else
          pos->second.docs = (pos->second.docs || entry.docs);
      }
    }

    std::vector<OA::CatalogEntry> ret;
    ret.reserve(merged.size());
    for (auto& item : merged)
      ret.push_back(std::move(item.second));
    return ret;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Failed to collect the API catalogs of the backends");
  }
}

}  // namespace Frontend
}  // namespace Plugin
}  // namespace SmartMet
