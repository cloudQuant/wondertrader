// BUG-I02-NEW-034 (card 9a=A+C): risk-monitor stop/quiescence and the
// borrowed WTSPortFundInfo. Real WtSelEngine/WtEngine code and a real dlopen of
// a local fake monitor plugin (fake_riskmon.cpp) loaded through init_riskmon.
// run.sh places exactly one variant as ./libFakeRiskMon.dylib in the cwd.
#include <gtest/gtest.h>

#include <dlfcn.h>

#include <chrono>
#include <string>
#include <thread>

#include "Includes/RiskMonDefs.h"
#include "Includes/WTSRiskDef.hpp"
#include "Includes/WTSVariant.hpp"
#include "WtCore/WtSelEngine.h"

using namespace wtp;

namespace {

class ProbeEngine : public WtSelEngine
{
public:
	WTSPortFundInfo* fund() const { return _port_fund; }
	bool riskInit(WTSVariant* cfg) { return init_riskmon(cfg); }
};

WTSVariant* riskCfg()
{
	WTSVariant* risk = WTSVariant::createObject();
	risk->append("active", true);
	risk->append("module", "FakeRiskMon");
	risk->append("name", "fake");
	return risk;
}

WTSVariant* engineCfg()
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("fees", "");
	WTSVariant* risk = riskCfg();
	cfg->append("riskmon", risk, false);
	return cfg;
}

struct Module
{
	Module() : handle(dlopen("./libFakeRiskMon.dylib", RTLD_NOW)) {}
	~Module() { if (handle) dlclose(handle); }
	int get(const char* name) const
	{
		auto fn = handle ? reinterpret_cast<int (*)()>(dlsym(handle, name)) : nullptr;
		return fn ? fn() : -1000;
	}
	void* handle;
};

void waitFor(const Module& m, const char* counter, int atLeast, int ms)
{
	for (int i = 0; i < ms && m.get(counter) < atLeast; ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

} // namespace

// Unproven plugin (no IRiskMonPluginV2 capability): its worker keeps calling the
// context after stop(). The engine must neither be reached after destruction
// nor release the fund the worker reads; the monitor is not deleted.
TEST(S5RiskMonQuiesce, UnprovenMonitorIsQuarantinedAndOutlivesEngineSafely)
{
	Module m;
	ASSERT_NE(nullptr, m.handle);
	WTSVariant* cfg = engineCfg();
	ProbeEngine* engine = new ProbeEngine();
	engine->init(cfg, nullptr, nullptr, nullptr, nullptr);
	ASSERT_NE(nullptr, engine->fund());
	waitFor(m, "fixture_ctx_calls", 3, 2000);
	ASSERT_GE(m.get("fixture_ctx_calls"), 3);

	delete engine;
	EXPECT_EQ(1, m.get("fixture_stops"));
	waitFor(m, "fixture_worker_done", 1, 3000);
	EXPECT_EQ(1, m.get("fixture_worker_done"));
	EXPECT_GT(m.get("fixture_late_calls"), 0) << "worker used its context after stop()";
	EXPECT_EQ(0, m.get("fixture_null_funds")) << "detached context still lends the retained fund";
	EXPECT_EQ(0, m.get("fixture_monitor_deletes")) << "unproven monitor is quarantined, not deleted";
	EXPECT_EQ(0, m.get("fixture_factory_deletes")) << "its factory stays with the quarantined module";
	cfg->release();
}

// Proven plugin: stop() joins; afterwards the monitor is deleted through its
// factory and the engine's fund reference is released exactly once.
TEST(S5RiskMonQuiesce, ProvenMonitorIsStoppedThenFundAndMonitorReleased)
{
	Module m;
	ASSERT_NE(nullptr, m.handle);
	WTSVariant* cfg = engineCfg();
	ProbeEngine* engine = new ProbeEngine();
	engine->init(cfg, nullptr, nullptr, nullptr, nullptr);
	WTSPortFundInfo* fund = engine->fund();
	ASSERT_NE(nullptr, fund);
	fund->retain();
	waitFor(m, "fixture_ctx_calls", 3, 2000);

	delete engine;
	EXPECT_EQ(1, m.get("fixture_stops"));
	EXPECT_EQ(1, m.get("fixture_worker_done"));
	EXPECT_EQ(0, m.get("fixture_late_calls"));
	EXPECT_EQ(1, m.get("fixture_monitor_deletes"));
	// I03-NEW-062: the factory is freed through the verified by-value deleter.
	EXPECT_EQ(1, m.get("fixture_factory_deletes"));
	EXPECT_EQ(1u, fund->retainCount()) << "engine released its own reference after the proven stop";
	fund->release();
	cfg->release();
}

// Card 9c=A: a second risk-monitor initialization keeps the original monitor.
TEST(S5RiskMonQuiesce, RepeatedRiskMonitorInitIsRejected)
{
	Module m;
	ASSERT_NE(nullptr, m.handle);
	WTSVariant* cfg = engineCfg();
	{
		ProbeEngine engine;
		engine.init(cfg, nullptr, nullptr, nullptr, nullptr);
		ASSERT_EQ(1, m.get("fixture_creates"));
		WTSVariant* again = riskCfg();
		EXPECT_FALSE(engine.riskInit(again));
		again->release();
		EXPECT_EQ(1, m.get("fixture_creates"));
	}
	cfg->release();
}
