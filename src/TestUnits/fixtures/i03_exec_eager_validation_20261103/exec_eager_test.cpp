// Iteration 03 / card 8b=A: eager executer configuration validation.
// BUG-R10-001 / BUG-P02-002. Exercised separately for WtLocalExecuter,
// WtDiffExecuter and WtArbiExecuter through their public init() entry; the
// test never calls the new validateConfig() directly so the same file builds
// against the pre-change baseline as a red control.
#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Includes/ExecuteDefs.h"
#include "Includes/WTSVariant.hpp"
#include "Share/ExecutionGroups.hpp"
#include "Share/SpinMutex.hpp"
#include "Share/threadpool.hpp"
#include "WtCore/ExecCommandQuiesce.h"
#include "WtCore/ExecUnitInitStatus.h"
#include "WtCore/IExecCommand.h"
#include "WtCore/ITrdNotifySink.h"
#include "Includes/IExecPluginV2.h"
#include "Share/DLLHelper.hpp"
#include <boost/core/noncopyable.hpp>
#include "WTSUtils/WTSCfgLoader.h"

// White-box access only to the three executer classes' side-effect state.
#define private public
#define protected public
#include "WtCore/WtExecuterFactory.h"
#include "WtCore/WtLocalExecuter.h"
#include "WtCore/WtDiffExecuter.h"
#include "WtCore/WtArbiExecuter.h"
#undef protected
#undef private
#include "WtCore/WtExecMgr.h"

using namespace wtp;

namespace {

WTSVariant* jsonCfg(const std::string& text)
{
	return WTSCfgLoader::load_from_content(text, false);
}

// Holds every constructed executer in one manager so each one is quiesced
// before destruction (the executer destructors fail closed otherwise).
struct I03ExecHarness
{
	WtExecuterFactory factory;
	WtExecuterMgr manager;
	~I03ExecHarness()
	{
		WtExecQuiesceCoordinator coordinator({ &manager });
		EXPECT_TRUE(coordinator.quiesce_all());
		EXPECT_TRUE(coordinator.is_complete());
	}
	std::shared_ptr<WtLocalExecuter> local(const char* id)
	{
		auto e = std::make_shared<WtLocalExecuter>(&factory, id, nullptr);
		manager.add_executer(e);
		return e;
	}
	std::shared_ptr<WtDiffExecuter> diff(const char* id)
	{
		auto e = std::make_shared<WtDiffExecuter>(&factory, id, nullptr, nullptr);
		manager.add_executer(e);
		return e;
	}
	std::shared_ptr<WtArbiExecuter> arbi(const char* id)
	{
		auto e = std::make_shared<WtArbiExecuter>(&factory, id, nullptr);
		manager.add_executer(e);
		return e;
	}
	// Units ever created by this factory and still alive (white-box read; the
	// public snapshot requires closed admissions).
	std::size_t livePluginUnits()
	{
		std::lock_guard<std::mutex> lock(factory._live_units_mutex);
		std::size_t alive = 0;
		for (const auto& unit : factory._live_units)
			alive += unit.expired() ? 0 : 1;
		return alive;
	}
};

// Each body is a full executer config that is invalid only in the policy or
// clear section. poolsize:2 makes a premature thread-pool side effect visible.
struct InvalidCase
{
	const char* label;
	const char* json;
	bool localArbiOnly; // clear section is not read by Diff
};

const InvalidCase kInvalid[] = {
	{ "missing policy", R"({"scale":1,"poolsize":2})", false },
	{ "policy not object", R"({"scale":1,"poolsize":2,"policy":"WtExeFact.WtMinImpactExeUnit"})", false },
	{ "missing default (commodity entry only)",
		R"({"scale":1,"poolsize":2,"policy":{"SHFE.ag":{"name":"WtExeFact.WtMinImpactExeUnit"}}})", false },
	{ "default not object", R"({"scale":1,"poolsize":2,"policy":{"default":"x"}})", false },
	{ "default without name", R"({"scale":1,"poolsize":2,"policy":{"default":{"offset":1}}})", false },
	{ "default name not string", R"({"scale":1,"poolsize":2,"policy":{"default":{"name":5}}})", false },
	{ "name without separator", R"({"scale":1,"poolsize":2,"policy":{"default":{"name":"WtMinImpactExeUnit"}}})", false },
	{ "name with empty factory", R"({"scale":1,"poolsize":2,"policy":{"default":{"name":".Unit"}}})", false },
	{ "name with empty unit", R"({"scale":1,"poolsize":2,"policy":{"default":{"name":"Fact."}}})", false },
	{ "name with extra part", R"({"scale":1,"poolsize":2,"policy":{"default":{"name":"A.B.C"}}})", false },
	// The invalid entry is not the default one: lazy lookup for other codes would
	// never reach it, eager validation must still reject the whole config.
	{ "invalid commodity-specific entry",
		R"({"scale":1,"poolsize":2,"policy":{"default":{"name":"WtExeFact.WtMinImpactExeUnit"},"SHFE.ag":{"lots":1}}})", false },
	{ "clear not object",
		R"({"scale":1,"poolsize":2,"clear":true,"policy":{"default":{"name":"WtExeFact.WtMinImpactExeUnit"}}})", true },
	{ "clear includes non-string",
		R"({"scale":1,"poolsize":2,"clear":{"active":true,"includes":[1]},"policy":{"default":{"name":"WtExeFact.WtMinImpactExeUnit"}}})", true },
};

const char* kValid =
	R"({"scale":1,"poolsize":2,"clear":{"active":true,"includes":["SHFE.ag"],"excludes":"SHFE.rb"},)"
	R"("policy":{"default":{"name":"WtExeFact.WtMinImpactExeUnit"},"SHFE.ag":{"name":"WtExeFact.WtTWapExeUnit"}}})";

} // namespace

TEST(I03ExecEagerValidation, LocalRejectsEveryInvalidPolicyWithoutSideEffects)
{
	I03ExecHarness h;
	int idx = 0;
	for (const InvalidCase& c : kInvalid)
	{
		WTSVariant* cfg = jsonCfg(c.json);
		ASSERT_NE(cfg, nullptr) << c.label;
		auto e = h.local(("i03_local_bad_" + std::to_string(idx++)).c_str());
		EXPECT_FALSE(e->init(cfg)) << c.label;
		EXPECT_EQ(e->_config, nullptr) << c.label;
		EXPECT_FALSE(static_cast<bool>(e->_pool)) << c.label;
		EXPECT_TRUE(e->_groups.empty()) << c.label;
		EXPECT_TRUE(e->_clear_includes.empty()) << c.label;
		EXPECT_TRUE(e->_unit_map.empty()) << c.label;
		EXPECT_EQ(cfg->retainCount(), 1u) << c.label;
		cfg->release();
	}
	EXPECT_EQ(h.livePluginUnits(), 0u);
}

TEST(I03ExecEagerValidation, DiffRejectsEveryInvalidPolicyWithoutSideEffects)
{
	I03ExecHarness h;
	int idx = 0;
	for (const InvalidCase& c : kInvalid)
	{
		if (c.localArbiOnly)
			continue;
		WTSVariant* cfg = jsonCfg(c.json);
		ASSERT_NE(cfg, nullptr) << c.label;
		auto e = h.diff(("i03_diff_bad_" + std::to_string(idx++)).c_str());
		EXPECT_FALSE(e->init(cfg)) << c.label;
		EXPECT_EQ(e->_config, nullptr) << c.label;
		EXPECT_FALSE(static_cast<bool>(e->_pool)) << c.label;
		EXPECT_TRUE(e->_unit_map.empty()) << c.label;
		EXPECT_EQ(cfg->retainCount(), 1u) << c.label;
		cfg->release();
	}
	EXPECT_EQ(h.livePluginUnits(), 0u);
}

TEST(I03ExecEagerValidation, ArbiRejectsEveryInvalidPolicyWithoutSideEffects)
{
	I03ExecHarness h;
	int idx = 0;
	for (const InvalidCase& c : kInvalid)
	{
		WTSVariant* cfg = jsonCfg(c.json);
		ASSERT_NE(cfg, nullptr) << c.label;
		auto e = h.arbi(("i03_arbi_bad_" + std::to_string(idx++)).c_str());
		EXPECT_FALSE(e->init(cfg)) << c.label;
		EXPECT_EQ(e->_config, nullptr) << c.label;
		EXPECT_FALSE(static_cast<bool>(e->_pool)) << c.label;
		EXPECT_TRUE(e->_groups.empty()) << c.label;
		EXPECT_TRUE(e->_clear_includes.empty()) << c.label;
		EXPECT_TRUE(e->_unit_map.empty()) << c.label;
		EXPECT_EQ(cfg->retainCount(), 1u) << c.label;
		cfg->release();
	}
	EXPECT_EQ(h.livePluginUnits(), 0u);
}

TEST(I03ExecEagerValidation, ValidConfigsStillInitializeAllThreeKinds)
{
	I03ExecHarness h;
	WTSVariant* cfg = jsonCfg(kValid);
	ASSERT_NE(cfg, nullptr);
	auto local = h.local("i03_local_ok");
	auto diff = h.diff("i03_diff_ok");
	auto arbi = h.arbi("i03_arbi_ok");
	EXPECT_TRUE(local->init(cfg));
	EXPECT_TRUE(diff->init(cfg));
	EXPECT_TRUE(arbi->init(cfg));
	EXPECT_EQ(local->_config, cfg);
	EXPECT_EQ(diff->_config, cfg);
	EXPECT_EQ(arbi->_config, cfg);
	EXPECT_TRUE(static_cast<bool>(local->_pool));
	EXPECT_TRUE(static_cast<bool>(diff->_pool));
	EXPECT_TRUE(static_cast<bool>(arbi->_pool));
	EXPECT_EQ(local->_clear_includes.count("SHFE.ag"), 1u);
	EXPECT_EQ(arbi->_clear_excludes.count("SHFE.rb"), 1u);
	// init never creates execution units; they stay lazy.
	EXPECT_EQ(h.livePluginUnits(), 0u);
	cfg->release();
}

TEST(I03ExecEagerValidation, ValidDefaultOnlyPolicyIsAccepted)
{
	I03ExecHarness h;
	WTSVariant* cfg = jsonCfg(R"({"scale":2,"policy":{"default":{"name":"WtExeFact.WtMinImpactExeUnit"}}})");
	ASSERT_NE(cfg, nullptr);
	EXPECT_TRUE(h.local("i03_local_def")->init(cfg));
	EXPECT_TRUE(h.diff("i03_diff_def")->init(cfg));
	EXPECT_TRUE(h.arbi("i03_arbi_def")->init(cfg));
	cfg->release();
}
