#include <spine/Reactor.h>
#include <spine/SmartMetPlugin.h>
#include <libconfig.h++>
#include <stdexcept>

namespace SmartMet
{
namespace Plugin
{
namespace FrontendDenyTest
{
class Plugin : public SmartMetPlugin
{
 public:
  Plugin(Spine::Reactor* theReactor, const char* theConfig)
      : itsMode(Mode::OK), itsReactor(theReactor)
  {
    if (theReactor->getRequiredAPIVersion() != SMARTMET_API_VERSION)
      throw Fmi::Exception(BCP, "Backend and Server API version mismatch");

    if (theConfig != nullptr)
    {
      loadModeFromConfig(theConfig);
    }
  }

  ~Plugin() override = default;

  const std::string& getPluginName() const override
  {
    static const std::string name = "FrontendDenyTest";
    return name;
  }

  int getRequiredAPIVersion() const override { return SMARTMET_API_VERSION; }

  bool queryIsFast(const Spine::HTTP::Request&) const override { return true; }

 protected:
  void init() override
  {
    if (!itsReactor->addContentHandler(
            this,
            "/denytest",
            [this](Spine::Reactor& theReactor,
                   const Spine::HTTP::Request& theRequest,
                   Spine::HTTP::Response& theResponse)
            { requestHandler(theReactor, theRequest, theResponse); }))
    {
      throw Fmi::Exception(BCP, "Failed to register FrontendDenyTest content handler");
    }
  }

  void shutdown() override {}

  void requestHandler(Spine::Reactor&,
                      const Spine::HTTP::Request&,
                      Spine::HTTP::Response& theResponse) override
  {
    switch (itsMode)
    {
      case Mode::HIGH_LOAD:
        theResponse.setStatus(Spine::HTTP::Status::high_load);
        theResponse.setContent("denytest backend high_load\n");
        break;

      case Mode::SHUTDOWN:
        theResponse.setStatus(Spine::HTTP::Status::shutdown);
        theResponse.setContent("denytest backend shutdown\n");
        break;

      case Mode::OK:
      default:
        theResponse.setStatus(Spine::HTTP::Status::ok);
        theResponse.setContent("denytest backend ok\n");
        break;
    }
  }

 private:
  enum class Mode
  {
    OK,
    HIGH_LOAD,
    SHUTDOWN
  };

  void loadModeFromConfig(const char* configPath)
  {
    libconfig::Config cfg;
    cfg.readFile(configPath);

    std::string mode = "ok";
    (void)cfg.lookupValue("mode", mode);

    if (mode == "high_load")
      itsMode = Mode::HIGH_LOAD;
    else if (mode == "shutdown")
      itsMode = Mode::SHUTDOWN;
    else
      itsMode = Mode::OK;
  }

  Mode itsMode;
  Spine::Reactor* itsReactor;
};

}  // namespace FrontendDenyTest
}  // namespace Plugin
}  // namespace SmartMet

extern "C" SmartMetPlugin* create(SmartMet::Spine::Reactor* them, const char* config)
{
  return new SmartMet::Plugin::FrontendDenyTest::Plugin(them, config);
}

extern "C" void destroy(SmartMetPlugin* us)
{
  delete us;
}
