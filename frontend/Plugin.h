// ======================================================================
/*!
 * \brief SmartMet Frontend plugin interface
 */
// ======================================================================

#pragma once

#include "HTTP.h"
#include <macgyver/CacheStats.h>
#include <spine/HTTP.h>
#include <spine/Reactor.h>
#include <spine/SmartMetPlugin.h>
#include <spine/Table.h>
#include <memory>
#include <utility>

namespace SmartMet
{
namespace Plugin
{
namespace Frontend
{
class Plugin : public SmartMetPlugin
{
 public:
  Plugin(Spine::Reactor* theReactor, const char* theConfig);
  ~Plugin() override = default;

  Plugin(const Plugin& other) = delete;
  Plugin(Plugin&& other) = delete;
  Plugin& operator=(const Plugin& other) = delete;
  Plugin& operator=(Plugin&& other) = delete;

  const std::string& getPluginName() const override;
  int getRequiredAPIVersion() const override;
  bool queryIsFast(const Spine::HTTP::Request& theRequest) const override;
  bool isAdminQuery(const SmartMet::Spine::HTTP::Request& theRequest) const override;

 protected:
  void init() override;
  void shutdown() override;
  void requestHandler(Spine::Reactor& theReactor,
                      const Spine::HTTP::Request& theRequest,
                      Spine::HTTP::Response& theResponse) override;

  void baseContentHandler(Spine::Reactor& theReactor,
                          const Spine::HTTP::Request& theRequest,
                          Spine::HTTP::Response& theResponse);

 private:
  std::unique_ptr<HTTP> itsHTTP;
  const std::string itsModuleName;

  std::string itsUsername;
  std::string itsPassword;

  mutable Spine::MutexType itsPauseMutex;
  mutable bool itsPaused{false};
  mutable std::optional<Fmi::DateTime> itsPauseDeadLine{};

  void registerAdminRequests(Spine::Reactor& theReactor);

  bool isPaused() const;

  bool authenticateRequest(const Spine::HTTP::Request& theRequest,
                           Spine::HTTP::Response& theResponse);

  /**
   * @brief Request Sputnik engine
   *
   * @return Engine::Sputnik::Engine* Pointer to the engine or nullptr in case of an error
   */
  Engine::Sputnik::Engine* getSputnikEngine();

  void requestClusterInfo(Spine::Reactor& theReactor,
                          const Spine::HTTP::Request& theRequest,
                          Spine::HTTP::Response& theResponse);

  std::unique_ptr<Spine::Table> requestBackendInfo(Spine::Reactor& theReactor,
                                            const Spine::HTTP::Request& theRequest);

  void requestQEngineStatus(Spine::Reactor& theReactor,
                            const Spine::HTTP::Request& theRequest,
                            Spine::HTTP::Response& theResponse);

  std::unique_ptr<Spine::Table> requestStatus(Spine::Reactor &theReactor,
                                              const Spine::HTTP::Request &theRequest,
                                              const std::string &what);

  std::unique_ptr<Spine::Table> requestActiveBackends(Spine::Reactor& theReactor,
                                                      const Spine::HTTP::Request &theRequest);

  std::string pauseUntil(const Fmi::DateTime& theTime);

  std::string requestPause(const SmartMet::Spine::HTTP::Request& theRequest);

  std::string requestContinue(const SmartMet::Spine::HTTP::Request& theRequest);

  static std::pair<std::string, bool> listRequests(Spine::Reactor& theReactor,
                                                   const Spine::HTTP::Request& theRequest,
                                                   Spine::HTTP::Response& theResponse);
  Fmi::Cache::CacheStatistics getCacheStats() const final;
};  // class Plugin

}  // namespace Frontend
}  // namespace Plugin
}  // namespace SmartMet

// ======================================================================
