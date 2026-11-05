// BUG-R42-001 / NEW-034 sub-issue: ownership of the WTSPortFundInfo created by
// WtEngine::load_datas(). Real WtSelEngine/WtEngine code; the only fakes are a
// risk monitor and its factory (no real plugin, provider or trading).
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "Includes/RiskMonDefs.h"
#include "Includes/WTSRiskDef.hpp"
#include "Includes/WTSVariant.hpp"
#include "WtCore/WtHelper.h"
#include "WtCore/WtSelEngine.h"

using namespace wtp;

namespace {

class ProbeEngine : public WtSelEngine
{
public:
	WTSPortFundInfo* fund() const { return _port_fund; }
	void reload() { load_datas(); }
};

WTSVariant* minimalConfig()
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("fees", "");
	return cfg;
}

class FakeRiskMon : public WtRiskMonitor
{
public:
	const char* getName() override { return "fake"; }
	const char* getFactName() override { return "fake-fact"; }
	void stop() override
	{
		++stops;
		// Retain count of the observed fund at the moment the engine stops the
		// monitor: the engine must still hold its own reference here.
		refsAtStop = observed ? observed->retainCount() : 0;
	}
	unsigned stops = 0;
	uint32_t refsAtStop = 0;
	WTSPortFundInfo* observed = nullptr;
};

class FakeRiskFact : public IRiskMonitorFact
{
public:
	const char* getName() override { return "fake-fact"; }
	void enumRiskMonitors(FuncEnumRiskMonCallback) override {}
	WtRiskMonitor* createRiskMonotor(const char*) override { return nullptr; }
	bool deleteRiskMonotor(WtRiskMonitor*) override { ++deletes; return true; }
	unsigned deletes = 0;
};

} // namespace

TEST(R42FundOwner, DestroyWithoutInitIsSafe)
{
	ProbeEngine engine;
	EXPECT_EQ(nullptr, engine.fund());
}

// LSan decides this case: with the leak, the 144-byte WTSPortFundInfo from
// load_datas() is reported at process exit.
TEST(R42FundOwner, InitThenDestroyLeavesNoFundAllocation)
{
	WTSVariant* cfg = minimalConfig();
	{
		ProbeEngine engine;
		engine.init(cfg, nullptr, nullptr, nullptr, nullptr);
		ASSERT_NE(nullptr, engine.fund());
		EXPECT_TRUE(engine.fee_config_valid());
	}
	cfg->release();
}

TEST(R42FundOwner, EngineReleasesItsOwnReferenceOnDestruction)
{
	WTSVariant* cfg = minimalConfig();
	WTSPortFundInfo* fund = nullptr;
	{
		ProbeEngine engine;
		engine.init(cfg, nullptr, nullptr, nullptr, nullptr);
		fund = engine.fund();
		ASSERT_NE(nullptr, fund);
		fund->retain(); // observer reference held by the test
		EXPECT_EQ(2u, fund->retainCount());
	}
	EXPECT_EQ(1u, fund->retainCount()); // only the observer remains
	fund->release();
	cfg->release();
}

TEST(R42FundOwner, RepeatedLoadKeepsTheLentFundObject)
{
	WTSVariant* cfg = minimalConfig();
	{
		ProbeEngine engine;
		engine.init(cfg, nullptr, nullptr, nullptr, nullptr);
		WTSPortFundInfo* lent = engine.fund();
		ASSERT_NE(nullptr, lent);
		lent->fundInfo()._balance = 123.0;
		engine.reload();
		EXPECT_EQ(lent, engine.fund());
		EXPECT_DOUBLE_EQ(0.0, engine.fund()->fundInfo()._balance); // contents reset in place
		EXPECT_EQ(1u, engine.fund()->retainCount());
	}
	cfg->release();
}

TEST(R42FundOwner, RiskMonitorIsStoppedBeforeFundRelease)
{
	// Monitor and factory outlive the engine that holds the wrapper.
	FakeRiskFact fact;
	FakeRiskMon mon;
	WTSVariant* cfg = minimalConfig();
	WTSPortFundInfo* fund = nullptr;
	{
		WtRiskMonPtr wrapper(new WtRiskMonWrapper(&mon, &fact));
		ProbeEngine engine;
		engine.init(cfg, nullptr, nullptr, nullptr, nullptr);
		engine.setRiskMonitor(wrapper);
		wrapper.reset(); // the engine holds the last wrapper reference
		fund = engine.fund();
		ASSERT_NE(nullptr, fund);
		fund->retain();
		mon.observed = fund;
	}
	EXPECT_EQ(1u, mon.stops);
	EXPECT_EQ(2u, mon.refsAtStop); // engine reference still held while stopping
	EXPECT_EQ(1u, fact.deletes);
	EXPECT_EQ(1u, fund->retainCount());
	fund->release();
	cfg->release();
}

int main(int argc, char** argv)
{
	// Set the generated directory before any WtHelper directory getter caches
	// it; done in main(), not in a static initializer.
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	const auto dir = std::filesystem::temp_directory_path() / ("wt-i03-r42-fund-" + std::to_string(stamp));
	std::filesystem::create_directories(dir);
	WtHelper::setGenerateDir((dir.string() + "/").c_str());
	::testing::InitGoogleTest(&argc, argv);
	const int rc = RUN_ALL_TESTS();
	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
	return rc;
}
