#pragma once
#include "Proxy.h"
#include <memory>
#include <string>

namespace SmartMet::Engine::Sputnik
{
class Engine;
}  // namespace SmartMet::Engine::Sputnik
namespace SmartMet::Spine::HTTP
{
class Request;
class Response;
}  // namespace SmartMet::Spine::HTTP
namespace SmartMet::Spine
{
class Reactor;
}  // namespace SmartMet::Spine

namespace SmartMet
{
namespace Plugin
{
namespace Frontend
{
class HTTP
{
 public:
  // Transport
  //
  // Called by HTTP thread, will contact the required HTTP server
  // and transfer the requested URI to the client.
  //
  // Return the HTTP 1.0 code if fails, otherwise returns zero.

  // The Catcher in the Rye for NoMatchCatch (in ContentEngine)
  void requestHandler(Spine::Reactor& theReactor,
                      const Spine::HTTP::Request& theRequest,
                      Spine::HTTP::Response& theResponse);

  // Sputnik Message callback function, called by Sputnik
  void sputnikMessageHandler(std::string* theMessage);

  // Constructor and destructor
  HTTP(Spine::Reactor* theReactor, const char* theConfig);
  ~HTTP();

  HTTP(const HTTP& other) = delete;
  HTTP(HTTP&& other) = delete;
  HTTP& operator=(const HTTP& other) = delete;
  HTTP& operator=(HTTP&& other) = delete;

  void shutdown();

  const std::shared_ptr<Proxy>& getProxy() const { return itsProxy; }

 private:
  // Pointer to Sputnik instance
  std::shared_ptr<Engine::Sputnik::Engine> itsSputnikProcess;

  // Non-owning
  std::shared_ptr<Proxy> itsProxy;

  // Access to the Reactor object (non-owning)
  Spine::Reactor* itsReactor;

  Proxy::ProxyStatus transport(Spine::Reactor& theReactor,
                               const Spine::HTTP::Request& theRequest,
                               Spine::HTTP::Response& theResponse);
};

}  // namespace Frontend
}  // namespace Plugin
}  // namespace SmartMet
