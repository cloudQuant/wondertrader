#include <Includes/IParserApi.h>
namespace { int created=0, initialized=0, connected=0;
class SpyParser : public wtp::IParserApi {
public: bool init(wtp::WTSVariant*) override { ++initialized; return true; }
    bool connect() override { ++connected; return true; }
}; }
extern "C" wtp::IParserApi* createParser() { ++created; return new SpyParser; }
extern "C" void deleteParser(wtp::IParserApi*& parser) { delete parser; parser=nullptr; }
extern "C" void spy_reset() { created=initialized=connected=0; }
extern "C" int spy_created() { return created; }
extern "C" int spy_initialized() { return initialized; }
extern "C" int spy_connected() { return connected; }
