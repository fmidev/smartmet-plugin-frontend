#include <spine/Reactor.h>
#include <spine/SmartMetPlugin.h>
#include <libconfig.h++>
#include <memory>
#include <stdexcept>
#include <string>

namespace SmartMet
{
namespace Plugin
{
namespace FrontendDenyTest
{
// ----------------------------------------------------------------------
/*!
 * \brief A response delivered in pieces, with no length known up front
 *
 * The one source of a chunked response in these tests. Nothing else either repo
 * installs streams: the plugins here answer in a single buffered response, so
 * without this the server's chunked reply path and the frontend's chunked body
 * decoder were exercised by nothing but unit tests.
 *
 * The content is deterministic - chunk n is its index repeated - so a test can
 * say exactly what the whole body should be without carrying a fixture.
 */
// ----------------------------------------------------------------------

class TestStreamer : public Spine::HTTP::ContentStreamer
{
 public:
  TestStreamer(std::size_t theChunks, std::size_t theChunkSize)
      : itsChunksLeft(theChunks), itsChunkSize(theChunkSize)
  {
  }

  std::string getChunk() override
  {
    if (itsChunksLeft == 0)
    {
      setStatus(StreamerStatus::EXIT_OK);
      return {};
    }

    --itsChunksLeft;
    return std::string(itsChunkSize, static_cast<char>('a' + (itsChunkIndex++ % 26)));
  }

 private:
  std::size_t itsChunksLeft;
  std::size_t itsChunkSize;
  std::size_t itsChunkIndex = 0;
};

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

    // Deliberately not affected by the deny mode: this one is about framing, and
    // both backends have to answer it whichever of them the frontend picks.
    if (!itsReactor->addContentHandler(
            this,
            "/streamtest",
            [this](Spine::Reactor& theReactor,
                   const Spine::HTTP::Request& theRequest,
                   Spine::HTTP::Response& theResponse)
            { streamHandler(theReactor, theRequest, theResponse); }))
    {
      throw Fmi::Exception(BCP, "Failed to register FrontendDenyTest stream handler");
    }
  }

  // ------------------------------------------------------------------
  /*!
   * \brief Answer with a streamed response of a length nobody knows in advance
   *
   * Which is what makes the server frame it with Transfer-Encoding: chunked.
   * "chunks" and "size" are settable so a test can ask for a body long enough to
   * take several chunks over the wire.
   */
  // ------------------------------------------------------------------
  void streamHandler(Spine::Reactor&,
                     const Spine::HTTP::Request& theRequest,
                     Spine::HTTP::Response& theResponse)
  {
    std::size_t chunks = 8;
    std::size_t size = 32768;

    auto requestedChunks = theRequest.getParameter("chunks");
    if (requestedChunks)
      chunks = std::stoul(*requestedChunks);

    auto requestedSize = theRequest.getParameter("size");
    if (requestedSize)
      size = std::stoul(*requestedSize);

    theResponse.setStatus(Spine::HTTP::Status::ok);
    theResponse.setHeader("Content-Type", "text/plain");

    // No length given, so the server has to chunk it
    theResponse.setContent(std::make_shared<TestStreamer>(chunks, size));
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
