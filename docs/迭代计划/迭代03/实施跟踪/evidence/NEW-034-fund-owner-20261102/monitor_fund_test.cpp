#include <gtest/gtest.h>
#include <Includes/WTSRiskDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <WtCore/WtHelper.h>
#include <WtCore/WtSelEngine.h>
#include <filesystem>
#include <string>
#include <unistd.h>

using namespace wtp;

class FundObserver : public WtRiskMonitor {
public:
    WTSPortFundInfo* borrowed = nullptr;
    int stops = 0;
    const char* getName() override { return "observer"; }
    const char* getFactName() override { return "test"; }
    void stop() override { ++stops; }
    void capture(WtPortContext* ctx) { borrowed = ctx->getFundInfo(); }
};

class ObserverFactory : public IRiskMonitorFact {
public:
    const char* getName() override { return "test"; }
    void enumRiskMonitors(FuncEnumRiskMonCallback) override {}
    WtRiskMonitor* createRiskMonotor(const char*) override { return new FundObserver; }
    bool deleteRiskMonotor(WtRiskMonitor* unit) override { delete unit; return true; }
};

TEST(FundOwner, ExistingReleaseDoesNotStopExternallyHeldMonitor) {
    auto p = std::filesystem::temp_directory_path() / ("wt-fund-owner-" + std::to_string(getpid()));
    std::filesystem::create_directories(p);
    WtHelper::setGenerateDir((p.string() + "/").c_str());
    ObserverFactory factory;
    auto* observer = static_cast<FundObserver*>(factory.createRiskMonotor(""));
    WtRiskMonPtr wrapper(new WtRiskMonWrapper(observer, &factory));
    WTSPortFundInfo* borrowed = nullptr;
    {
        WtSelEngine engine;
        WTSVariant* cfg = WTSVariant::createObject();
        cfg->append("fees", "");
        engine.init(cfg, nullptr, nullptr, nullptr, nullptr);
        cfg->release();
        engine.setRiskMonitor(wrapper);
        observer->capture(&engine);
        borrowed = observer->borrowed;
        ASSERT_NE(borrowed, nullptr);
        engine.release();
        EXPECT_EQ(observer->stops, 0);
    }
    EXPECT_EQ(observer->stops, 0);
    EXPECT_EQ(observer->borrowed, borrowed);
    wrapper.reset();
    borrowed->release(); // Return the leaked reference only after the observer is destroyed.
}
