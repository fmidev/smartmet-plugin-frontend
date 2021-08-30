// ======================================================================
/*!
 * \brief SmartMet Frontend plugin interface
 */
// ======================================================================

#pragma once

#include "HTTP.h"
#include <spine/HTTP.h>
#include <spine/Reactor.h>
#include <spine/SmartMetPlugin.h>
#include <macgyver/CacheStats.h>
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
  ~Plugin() = default;

  const std::string& getPluginName() const;
  int getRequiredAPIVersion() const;
  bool queryIsFast(const Spine::HTTP::Request& theRequest) const;

 protected:
  void init();
  void shutdown();
  void requestHandler(Spine::Reactor& theReactor,
                      const Spine::HTTP::Request& theRequest,
                      Spine::HTTP::Response& theResponse);

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
  mutable boost::optional<boost::posix_time::ptime> itsPauseDeadLine{};

  std::pair<std::string, bool> request(Spine::Reactor& theReactor,
                                       const Spine::HTTP::Request& theRequest,
                                       Spine::HTTP::Response& theResponse);

  bool isPaused() const;

  bool authenticateRequest(const Spine::HTTP::Request& theRequest,
                           Spine::HTTP::Response& theResponse);

  std::pair<std::string, bool> pauseUntil(const boost::posix_time::ptime& theTime);

  std::pair<std::string, bool> requestPause(SmartMet::Spine::Reactor& theReactor,
                                            const SmartMet::Spine::HTTP::Request& theRequest);

  std::pair<std::string, bool> requestContinue(SmartMet::Spine::Reactor& theReactor,
                                               const SmartMet::Spine::HTTP::Request& theRequest);
  std::pair<std::string, bool> listRequests(Spine::Reactor &theReactor,
											const Spine::HTTP::Request &theRequest,
											Spine::HTTP::Response &theResponse);
  Fmi::Cache::CacheStatistics getCacheStats() const;	
};  // class Plugin

}  // namespace Frontend
}  // namespace Plugin
}  // namespace SmartMet

// ======================================================================
