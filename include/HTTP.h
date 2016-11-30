#pragma once
#include <spine/HTTP.h>
#include <spine/Reactor.h>
#include <spine/ConfigBase.h>
#include <engines/sputnik/Engine.h>

#include <boost/shared_ptr.hpp>

#include "Proxy.h"

namespace SmartMet
{
namespace Plugin
{
namespace Frontend
{
class HTTP
{
 private:
  // Pointer to Sputnik instance
  SmartMet::Engine::Sputnik::Engine* itsSputnikProcess;

  // Non-owning
  boost::shared_ptr<Proxy> itsProxy;

  // Access to the Reactor object (non-owning)
  SmartMet::Spine::Reactor* itsReactor;

  Proxy::ProxyStatus transport(const SmartMet::Spine::HTTP::Request& theRequest,
                               SmartMet::Spine::HTTP::Response& theResponse);

 public:
  // Transport
  //
  // Called by HTTP thread, will contact the required HTTP server
  // and transfer the requested URI to the client.
  //
  // Return the HTTP 1.0 code if fails, otherwise returns zero.

  // The Catcher in the Rye for NoMatchCatch (in ContentEngine)
  void requestHandler(SmartMet::Spine::Reactor& theReactor,
                      const SmartMet::Spine::HTTP::Request& theRequest,
                      SmartMet::Spine::HTTP::Response& theResponse);

  // Sputnik Message callback function, called by Sputnik
  void sputnikMessageHandler(std::string* theMessage);

  // Constructor and destructor
  //
  HTTP(SmartMet::Spine::Reactor* theReactor, const char* theConfig);
  ~HTTP();

  void shutdown();
};

}  // namespace Frontend
}  // namespace Plugin
}  // namespace SmartMet
