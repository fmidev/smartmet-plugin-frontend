// ======================================================================
/*!
 * \brief The API catalog of the cluster
 *
 * Serves /api, /.well-known/api-catalog and /api/docs with the APIs of all
 * backends: the catalogs the backends serve at /api are fetched and merged.
 * The merged catalog is cached for a minute, and rebuilt at once when the set
 * of backends serving a catalog changes.
 */
// ======================================================================

#pragma once

#include <engines/sputnik/Engine.h>
#include <spine/HTTP.h>
#include <spine/OpenApi.h>
#include <chrono>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace SmartMet
{
namespace Plugin
{
namespace Frontend
{
class ApiCatalog
{
 public:
  using SputnikGetter = std::function<Engine::Sputnik::Engine*()>;

  ApiCatalog(Spine::OpenAPI::CatalogSettings settings, SputnikGetter sputnik);

  const Spine::OpenAPI::CatalogSettings& settings() const { return itsCatalog.settings(); }

  void handle(const Spine::HTTP::Request& theRequest, Spine::HTTP::Response& theResponse);

  // The merged catalog of the backends and of the APIs of this server
  std::vector<Spine::OpenAPI::CatalogEntry> entries();

 private:
  using Backends = std::set<std::pair<std::string, int>>;  // IP and port

  Backends backends() const;
  static std::vector<Spine::OpenAPI::CatalogEntry> collect(const Backends& theBackends);

  Spine::OpenAPI::Catalog itsCatalog;
  SputnikGetter itsSputnik;

  std::mutex itsRefreshMutex;  // one refresh at a time
  std::mutex itsMutex;         // protects the members below
  bool itsValid = false;
  std::chrono::steady_clock::time_point itsExpires;
  Backends itsBackends;
  std::vector<Spine::OpenAPI::CatalogEntry> itsEntries;
};

}  // namespace Frontend
}  // namespace Plugin
}  // namespace SmartMet
