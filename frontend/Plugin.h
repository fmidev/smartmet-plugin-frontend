// ======================================================================
/*!
 * \brief SmartMet Frontend plugin interface
 */
// ======================================================================

#pragma once

#include <macgyver/Cache.h>
#include <spine/HTTP.h>
#include <spine/Reactor.h>
#include <spine/SmartMetPlugin.h>

#include <utility>

#include "HTTP.h"

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
  virtual ~Plugin();

  const std::string& getPluginName() const;
  int getRequiredAPIVersion() const;
  bool queryIsFast(const Spine::HTTP::Request& theRequest) const;

 protected:
  void init();
  void shutdown();
  void requestHandler(Spine::Reactor& theReactor,
                      const Spine::HTTP::Request& theRequest,
                      Spine::HTTP::Response& theResponse);

 private:
  HTTP* itsHTTP;
  const std::string itsModuleName;

  std::pair<std::string, bool> request(Spine::Reactor& theReactor,
                                       const Spine::HTTP::Request& theRequest,
                                       Spine::HTTP::Response& theResponse);

};  // class Plugin

}  // namespace Frontend
}  // namespace Plugin
}  // namespace SmartMet

// ======================================================================
