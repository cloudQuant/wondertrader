#include <Includes/ITraderApi.h>
#include <Includes/WTSTradeDef.hpp>
#include <cstdio>

namespace {
int insert_count = 0;
class FixtureTrader final : public wtp::ITraderApi {
public:
    bool init(wtp::WTSVariant*) override { return true; }
    bool makeEntrustID(char* buffer, int length) override { std::snprintf(buffer, length, "fixture"); return true; }
    int orderInsert(wtp::WTSEntrust*) override { ++insert_count; return 0; }
};
}
extern "C" wtp::ITraderApi* createTrader() { return new FixtureTrader; }
extern "C" void deleteTrader(wtp::ITraderApi*& trader) { delete trader; trader = nullptr; }
extern "C" int fixtureInsertCount() { return insert_count; }
