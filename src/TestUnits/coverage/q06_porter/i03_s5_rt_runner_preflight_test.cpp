/*
 * i03_s5_rt_runner_preflight_test.cpp (迭代03 s5-runners)
 *
 * 关联：BUG-R134-003、BUG-WSN06-001（卡8b=A runner 层全表预检、SEL 路由、
 * initExecuters 失败不泄漏且传播）、BUG-WSN06-002（策略 init(false)/异常传播，
 * 全批成功后才发布）、BUG-I02-NEW-025（未知 SEL 工厂受控失败、无部分 context）、
 * BUG-R42-002（未知 trader 创建失败无残留、版本化数量查询可区分失败）。
 *
 * 只用本地 fake：不加载 trader/parser 模块、不连接柜台、不下单。工厂插件为
 * 覆盖率构建产出的本地策略插件（WT_R6_*_FACTORY_PLUGIN），复制到独立临时目录。
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <unistd.h>

#include "Includes/ExecuteDefs.h"
#include "Includes/FasterDefs.h"
#include "Includes/ITraderApi.h"
#include "Share/DLLHelper.hpp"
#include "Share/BoostFile.hpp"
#include "Share/StdUtils.hpp"
#include "Share/SpinMutex.hpp"
#include "Includes/WTSVariant.hpp"
#include <Share/threadpool.hpp>

#include "WtCore/ITrdNotifySink.h"
#include "WtCore/IExecCommand.h"
#include "WtCore/ExecCommandQuiesce.h"
#include "WtCore/ExecUnitInitStatus.h"
#include "WtCore/WtExecuterFactory.h"

#define private public
#include "WtCore/TraderAdapter.h"
#undef private

#include "WtCore/EventNotifier.h"
#include "WtCore/CtaStrategyMgr.h"
#include "WtCore/HftStrategyMgr.h"
#include "WtCore/SelStrategyMgr.h"
#include "WtCore/WtCtaEngine.h"
#include "WtCore/WtHftEngine.h"
#include "WtCore/HftStraContext.h"
#include "WtCore/CtaStraContext.h"
#include "WtCore/SelStraContext.h"
#include "WtCore/WtSelEngine.h"
#include "WtCore/WtLocalExecuter.h"
#include "WtCore/WtDiffExecuter.h"
#include "WtCore/WtArbiExecuter.h"
#include "WtCore/WtDistExecuter.h"
#include "WtCore/ParserAdapter.h"
#include "WtCore/WtDtMgr.h"
#include "WtCore/WtHelper.h"
#include "WtCore/ActionPolicyMgr.h"
#include "WTSTools/WTSHotMgr.h"
#include "WTSTools/WTSBaseDataMgr.h"

#define private public
#include "../../../WtPorter/WtRtRunner.h"
#undef private
#include "WtPorter/ExpHftContext.h"
#include "WtPorter/WtPorter.h"

#if !defined(WT_R6_CTA_FACTORY_PLUGIN) || !defined(WT_R6_SEL_FACTORY_PLUGIN) || !defined(WT_R6_HFT_FACTORY_PLUGIN)
#error "WT_R6_*_FACTORY_PLUGIN must point to the isolated real factory plugins"
#endif

USING_NS_WTP;

extern WtRtRunner& getRunner();
TraderAdapterPtr i03s5_register_test_trader(WtRtRunner& runner, const char* id);

namespace
{
namespace stdfs = std::filesystem;

class TempDir
{
public:
	explicit TempDir(const char* tag)
	{
		static std::atomic<unsigned> next{ 0 };
		_path = stdfs::temp_directory_path() / ("i03s5_rt_" + std::string(tag) + "_" +
			std::to_string((long)::getpid()) + "_" + std::to_string(++next));
		std::error_code ec;
		stdfs::remove_all(_path, ec);
		stdfs::create_directories(_path, ec);
		write("actions.json", "{\"default\":{}}");
		write("sessions.yaml", "ALLDAY:\n    name: AllDay\n    offset: 0\n    sections:\n"
			"        - from: 0\n          to: 0\n");
	}
	~TempDir()
	{
		std::error_code ec;
		stdfs::remove_all(_path, ec);
	}
	std::string str() const { return _path.generic_string(); }
	std::string file(const char* name) const { return (_path / name).generic_string(); }
	void write(const char* name, const std::string& body) const
	{
		std::ofstream ofs(_path / name, std::ios::binary | std::ios::trunc);
		ofs << body;
	}
	// Copies a factory plugin into its own subdirectory; returns the folder with a separator.
	std::string plugin_dir(const char* sub, const char* plugin) const
	{
		const stdfs::path dir = _path / sub;
		std::error_code ec;
		stdfs::create_directories(dir, ec);
		const stdfs::path source(plugin);
		stdfs::copy_file(source, dir / source.filename(), stdfs::copy_options::overwrite_existing, ec);
		return dir.generic_string() + "/";
	}

private:
	stdfs::path _path;
};

const char* kValidPolicy = "\"policy\":{\"default\":{\"name\":\"WtExeFact.WtMinImpactExeUnit\"}}";

std::string executer(const char* kind, const char* id, bool valid = true)
{
	return std::string("{\"active\":true,\"name\":\"") + kind + "\",\"id\":\"" + id + "\",\"scale\":1" +
		(valid ? std::string(",") + kValidPolicy : std::string()) + "}";
}

std::string config(const TempDir& dir, const char* engine, const std::string& executers,
	const std::string& strategies)
{
	return "{\"basefiles\":{\"session\":\"" + dir.file("sessions.yaml") + "\"},"
		"\"env\":{\"name\":\"" + std::string(engine) + "\",\"product\":{\"session\":\"ALLDAY\"}},"
		"\"data\":{},\"bspolicy\":\"" + dir.file("actions.json") + "\","
		"\"parsers\":[],\"traders\":[]" +
		(executers.empty() ? std::string() : ",\"executers\":" + executers) +
		(strategies.empty() ? std::string() : ",\"strategies\":" + strategies) + "}";
}

// Heap runner, intentionally released (not deleted) so destruction never
// depends on a partially initialized graph.
WtRtRunner* new_runner(const TempDir& dir)
{
	WtRtRunner* runner = new WtRtRunner();
	runner->init(dir.file("absent_logcfg.yaml").c_str(), true, dir.str().c_str());
	return runner;
}

WtRtRunner::LifecycleState state_of(WtRtRunner& runner)
{
	std::lock_guard<std::mutex> lock(runner._status_config_mutex);
	return runner._lifecycle_state;
}

uint32_t next_sel_context_id()
{
	SelStraContext* probe = new SelStraContext(nullptr, "i03s5_sel_probe", 0); // leaked id probe
	return probe->id() + 1;
}

uint32_t next_cta_context_id()
{
	CtaStraContext* probe = new CtaStraContext(nullptr, "i03s5_cta_probe", 0); // leaked id probe
	return probe->id() + 1;
}
}

// ---------------------------------------------------------------------------
// Runner-level executer preflight (card 8b=A)
// ---------------------------------------------------------------------------

TEST(I03S5RtRunnerPreflight, InvalidExecuterInMiddleRejectsBeforeMutationAndConfigIsRetryable)
{
	TempDir dir("exec_mid");
	WtRtRunner* runner = new_runner(dir);
	const std::string bad = "[" + executer("local", "i03s5_ok_a") + "," + executer("local", "i03s5_bad", false) +
		"," + executer("diff", "i03s5_ok_b") + "]";
	EXPECT_FALSE(runner->config(config(dir, "cta", bad, "").c_str(), false));
	EXPECT_EQ(nullptr, runner->_config) << "a pure preflight rejection must keep config() retryable";
	EXPECT_EQ(nullptr, runner->_cta_engine.findExecuter("i03s5_ok_a").get());
	EXPECT_EQ(nullptr, runner->_sel_engine.findExecuter("i03s5_ok_a").get());
	EXPECT_EQ(nullptr, runner->_engine);
	EXPECT_EQ(WtRtRunner::LifecycleState::UNCONFIGURED, state_of(*runner));

	const std::string good = "[" + executer("local", "i03s5_ok_a") + "," + executer("diff", "i03s5_ok_b") + "]";
	ASSERT_TRUE(runner->config(config(dir, "cta", good, "").c_str(), false));
	EXPECT_NE(nullptr, runner->_cta_engine.findExecuter("i03s5_ok_a").get());
	EXPECT_NE(nullptr, runner->_cta_engine.findExecuter("i03s5_ok_b").get());
	runner->release();
	EXPECT_EQ(WtRtRunner::LifecycleState::RELEASED, state_of(*runner));
}

TEST(I03S5RtRunnerPreflight, UnknownExecuterKindIsRejectedInsteadOfBecomingDist)
{
	TempDir dir("exec_kind");
	WtRtRunner* runner = new_runner(dir);
	const std::string list = "[{\"active\":true,\"name\":\"bogus\",\"id\":\"i03s5_kind\",\"scale\":1}]";
	EXPECT_FALSE(runner->config(config(dir, "cta", list, "").c_str(), false));
	EXPECT_EQ(nullptr, runner->_cta_engine.findExecuter("i03s5_kind").get());
	EXPECT_EQ(nullptr, runner->_config);
	runner->release();
}

TEST(I03S5RtRunnerPreflight, MissingExecuterSidecarFailsConfig)
{
	TempDir dir("exec_file");
	WtRtRunner* runner = new_runner(dir);
	const std::string sidecar = "\"" + dir.file("absent_executers.json") + "\"";
	EXPECT_FALSE(runner->config(config(dir, "cta", sidecar, "").c_str(), false));
	EXPECT_EQ(nullptr, runner->_config);
	runner->release();
}

TEST(I03S5RtRunnerPreflight, SidecarExecutersAndRoutersAreValidatedAndLoaded)
{
	TempDir dir("exec_sidecar");
	dir.write("executers.json", "{\"executers\":[" + executer("local", "i03s5_side") + "],"
		"\"routers\":[{\"strategy\":\"x\",\"executer\":\"i03s5_side\"}]}");
	WtRtRunner* runner = new_runner(dir);
	ASSERT_TRUE(runner->config(config(dir, "cta", "\"" + dir.file("executers.json") + "\"", "").c_str(), false));
	EXPECT_NE(nullptr, runner->_cta_engine.findExecuter("i03s5_side").get());
	runner->release();
}

TEST(I03S5RtRunnerPreflight, InvalidRouterRuleFailsConfig)
{
	TempDir dir("exec_router");
	// Rule without "strategy": the executer manager rejects it.
	dir.write("executers.json", "{\"executers\":[" + executer("local", "i03s5_route") + "],"
		"\"routers\":[{\"executer\":\"i03s5_route\"}]}");
	WtRtRunner* runner = new_runner(dir);
	EXPECT_FALSE(runner->config(config(dir, "cta", "\"" + dir.file("executers.json") + "\"", "").c_str(), false));
	runner->run(true);
	EXPECT_NE(WtRtRunner::LifecycleState::RUNNING, state_of(*runner));
	runner->release();
}

TEST(I03S5RtRunnerPreflight, DiffInitFailureAfterValidationFailsConfigAndRetainsHostForQuiesce)
{
	TempDir dir("exec_diff_init");
	WtRtRunner* runner = new_runner(dir);
	// Corrupt Diff snapshot: validation passes, init() then fails reading it.
	const std::string snapshot = std::string(WtHelper::getExecDataDir()) + "i03s5_bad_diff.json";
	{
		std::ofstream ofs(snapshot, std::ios::binary | std::ios::trunc);
		ofs << "{not json";
	}
	const std::string list = "[" + executer("local", "i03s5_diff_peer") + "," + executer("diff", "i03s5_bad_diff") + "]";
	EXPECT_FALSE(runner->config(config(dir, "cta", list, "").c_str(), false));
	// Published before init: the failed host is owned by the manager (no leak)
	// and is quiesced by release(); a later run is refused.
	EXPECT_NE(nullptr, runner->_cta_engine.findExecuter("i03s5_bad_diff").get());
	runner->run(true);
	EXPECT_NE(WtRtRunner::LifecycleState::RUNNING, state_of(*runner));
	runner->release();
	EXPECT_EQ(WtRtRunner::LifecycleState::RELEASED, state_of(*runner));
	std::error_code ec;
	stdfs::remove(snapshot, ec);
}

// ---------------------------------------------------------------------------
// SEL routing (BUG-R134-003 / WSN06-001 / NEW-025)
// ---------------------------------------------------------------------------

namespace
{
std::string sel_strategy(const char* id, const char* name, const char* period, bool withParams = true)
{
	return std::string("{\"active\":true,\"id\":\"") + id + "\",\"name\":\"" + name + "\",\"period\":\"" +
		period + "\",\"date\":0,\"time\":1500" +
		(withParams ? ",\"params\":{\"days\":3,\"k1\":0.5,\"k2\":0.5,\"period\":\"d1\",\"count\":10,\"codes\":\"\"}" : "") + "}";
}
}

TEST(I03S5RtRunnerSel, ValidSelConfigPublishesSelContextAndExecutersWithoutReadingCta)
{
	TempDir dir("sel_ok");
	WtRtRunner* runner = new_runner(dir);
	ASSERT_TRUE(runner->addSelFactories(dir.plugin_dir("sel", WT_R6_SEL_FACTORY_PLUGIN).c_str()));
	const std::string strategies = "{\"sel\":[" + sel_strategy("i03s5_sel_ok", "WtSelStraFact.DualThrustSelection", "d") +
		"],\"cta\":[{\"active\":true,\"id\":\"i03s5_cta_never\",\"name\":\"Missing.Factory\"}]}";
	const uint32_t expected = next_sel_context_id();
	const std::string list = "[" + executer("local", "i03s5_sel_exec") + "]";
	ASSERT_TRUE(runner->config(config(dir, "sel", list, strategies).c_str(), false));
	EXPECT_NE(nullptr, runner->_sel_mgr.getStrategy("i03s5_sel_ok").get());
	EXPECT_NE(nullptr, runner->getSelContext(expected).get());
	EXPECT_EQ(nullptr, runner->getCtaContext(expected).get());
	EXPECT_NE(nullptr, runner->_sel_engine.findExecuter("i03s5_sel_exec").get());
	EXPECT_EQ(nullptr, runner->_cta_engine.findExecuter("i03s5_sel_exec").get());
	runner->release();
}

TEST(I03S5RtRunnerSel, UnknownSelFactoryFailsWithoutPartialContext)
{
	TempDir dir("sel_unknown");
	WtRtRunner* runner = new_runner(dir);
	ASSERT_TRUE(runner->addSelFactories(dir.plugin_dir("sel", WT_R6_SEL_FACTORY_PLUGIN).c_str()));
	const std::string strategies = "{\"sel\":[" + sel_strategy("i03s5_sel_first", "WtSelStraFact.DualThrustSelection", "d") +
		"," + sel_strategy("i03s5_sel_missing", "WtSelStraFact.NoSuchStrategy", "d") + "]}";
	const uint32_t expected = next_sel_context_id();
	EXPECT_FALSE(runner->config(config(dir, "sel", "", strategies).c_str(), false));
	EXPECT_EQ(nullptr, runner->getSelContext(expected).get());
	EXPECT_EQ(nullptr, runner->getSelContext(expected + 1).get());
	runner->release();
}

TEST(I03S5RtRunnerSel, SelInitFalseFailsWholeBatchBeforePublication)
{
	TempDir dir("sel_initfalse");
	WtRtRunner* runner = new_runner(dir);
	ASSERT_TRUE(runner->addSelFactories(dir.plugin_dir("sel", WT_R6_SEL_FACTORY_PLUGIN).c_str()));
	// The demo SEL strategy returns false from init() without a params object.
	const std::string strategies = "{\"sel\":[" + sel_strategy("i03s5_sel_good", "WtSelStraFact.DualThrustSelection", "d") +
		"," + sel_strategy("i03s5_sel_noparams", "WtSelStraFact.DualThrustSelection", "d", false) + "]}";
	const uint32_t expected = next_sel_context_id();
	EXPECT_FALSE(runner->config(config(dir, "sel", "", strategies).c_str(), false));
	EXPECT_EQ(nullptr, runner->getSelContext(expected).get());
	runner->run(true);
	EXPECT_NE(WtRtRunner::LifecycleState::RUNNING, state_of(*runner));
	runner->release();
}

TEST(I03S5RtRunnerSel, InvalidSelPeriodIsRejectedByPreflight)
{
	TempDir dir("sel_period");
	WtRtRunner* runner = new_runner(dir);
	const std::string strategies = "{\"sel\":[" + sel_strategy("i03s5_sel_period", "WtSelStraFact.DualThrustSelection", "d1") + "]}";
	EXPECT_FALSE(runner->config(config(dir, "sel", "", strategies).c_str(), false));
	EXPECT_EQ(nullptr, runner->_config);
	runner->release();
}

// ---------------------------------------------------------------------------
// Strategy init(false) propagation (BUG-WSN06-002)
// ---------------------------------------------------------------------------

TEST(I03S5RtRunnerStrategies, CtaInitFalseFailsConfigAndPublishesNoContext)
{
	TempDir dir("cta_initfalse");
	WtRtRunner* runner = new_runner(dir);
	ASSERT_TRUE(runner->addCtaFactories(dir.plugin_dir("cta", WT_R6_CTA_FACTORY_PLUGIN).c_str()));
	// DualThrust rejects a missing params object (BUG-WSN06-004 strict init).
	const std::string strategies = "{\"cta\":[{\"active\":true,\"id\":\"i03s5_cta_dt\","
		"\"name\":\"WtCtaStraFact.DualThrust\"}]}";
	const uint32_t expected = next_cta_context_id();
	EXPECT_FALSE(runner->config(config(dir, "cta", "", strategies).c_str(), false));
	EXPECT_EQ(nullptr, runner->getCtaContext(expected).get());
	runner->run(true);
	EXPECT_NE(WtRtRunner::LifecycleState::RUNNING, state_of(*runner));
	runner->release();
}

TEST(I03S5RtRunnerStrategies, HftInitFalseFailsBatchWithoutContextOrTraderSink)
{
	TempDir dir("hft_initfalse");
	WtRtRunner* runner = new_runner(dir);
	ASSERT_TRUE(runner->addHftFactories(dir.plugin_dir("hft", WT_R6_HFT_FACTORY_PLUGIN).c_str()));
	TraderAdapterPtr trader = i03s5_register_test_trader(*runner, "i03s5_hft_trader");
	ASSERT_TRUE(trader != nullptr);

	WTSVariant* cfg = WTSVariant::createObject();
	WTSVariant* strategies = WTSVariant::createObject();
	WTSVariant* hft = WTSVariant::createArray();
	for (const char* id : { "i03s5_hft_good", "i03s5_hft_noparams" })
	{
		WTSVariant* unit = WTSVariant::createObject();
		unit->append("active", true);
		unit->append("id", id);
		unit->append("name", "WtHftStraFact.SimpleHft");
		unit->append("trader", "i03s5_hft_trader");
		if (std::string(id) == "i03s5_hft_good")
		{
			WTSVariant* params = WTSVariant::createObject();
			params->append("code", "SHFE.rb.2510");
			unit->append("params", params, false);
		}
		hft->append(unit, false);
	}
	strategies->append("hft", hft, false);
	cfg->append("strategies", strategies, false);
	runner->_config = cfg;
	HftStraContext probe(nullptr, "i03s5_hft_probe", false, 0);
	const uint32_t next = probe.id() + 1;
	EXPECT_FALSE(runner->initHftStrategies());
	EXPECT_EQ(nullptr, runner->_hft_engine.getContext(next).get());
	EXPECT_EQ(nullptr, runner->_hft_engine.getContext(next + 1).get());
	EXPECT_TRUE(trader->_sinks.empty());
}

// ---------------------------------------------------------------------------
// Startup exception fails closed (BUG-WSN06-001 on_init isolation, runner side)
// ---------------------------------------------------------------------------

namespace
{
std::atomic<int> g_throwing_init_calls{ 0 };
void PORTER_FLAG throwing_hft_init(CtxHandler)
{
	g_throwing_init_calls.fetch_add(1);
	throw std::runtime_error("i03s5 strategy on_init failure");
}
}

TEST(I03S5RtRunnerStartup, StrategyOnInitExceptionReleasesPartiallyStartedRuntime)
{
	TempDir dir("startup_throw");
	WtRtRunner* runner = new_runner(dir);
	ASSERT_TRUE(runner->config(config(dir, "hft", "", "").c_str(), false));
	ASSERT_TRUE(i03s5_register_test_trader(*runner, "i03s5_startup_trader") != nullptr);
	// ExpHftContext forwards on_init to the process porter's callbacks.
	register_hft_callbacks(&throwing_hft_init, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
	ASSERT_NE(0u, runner->createHftContext("i03s5_startup_ctx", "i03s5_startup_trader", false, 0));
	g_throwing_init_calls.store(0);
	runner->run(true);
	register_hft_callbacks(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
	EXPECT_EQ(1, g_throwing_init_calls.load());
	EXPECT_EQ(WtRtRunner::LifecycleState::RELEASED, state_of(*runner))
		<< "a startup exception must not leave the runtime marked RUNNING";
	runner->run(true);
	EXPECT_EQ(WtRtRunner::LifecycleState::RELEASED, state_of(*runner));
}

TEST(I03S5RtRunnerStartup, SecondRunIsRejectedAfterSuccessfulStart)
{
	TempDir dir("second_run");
	WtRtRunner* runner = new_runner(dir);
	ASSERT_TRUE(runner->config(config(dir, "hft", "", "").c_str(), false));
	runner->run(true);
	EXPECT_EQ(WtRtRunner::LifecycleState::RUNNING, state_of(*runner));
	runner->run(true); // R10-004: repeated run is refused, state unchanged
	EXPECT_EQ(WtRtRunner::LifecycleState::RUNNING, state_of(*runner));
	runner->release();
	EXPECT_EQ(WtRtRunner::LifecycleState::RELEASED, state_of(*runner));
}

// ---------------------------------------------------------------------------
// BUG-R42-002: unknown trader creation and distinguishable quantity queries
// ---------------------------------------------------------------------------

TEST(I03S5R42HftQuantity, UnknownTraderCreatesNothingAndRegisteredTraderBinds)
{
	TempDir dir("r42_capi");
	WtRtRunner* runner = new_runner(dir);
	ASSERT_TRUE(runner->config(config(dir, "hft", "", "").c_str(), false));

	HftStraContext probe(nullptr, "i03s5_r42_probe", false, 0);
	const uint32_t next = probe.id() + 1;
	EXPECT_EQ(0u, runner->createHftContext("i03s5_r42_unknown", "i03s5_absent_trader", false, 0));
	EXPECT_EQ(0u, runner->createHftContext("i03s5_r42_empty", "", false, 0));
	EXPECT_EQ(0u, runner->createHftContext("i03s5_r42_null", nullptr, false, 0));
	EXPECT_EQ(nullptr, runner->getHftContext(next).get()) << "rejected creation must not register a context";

	TraderAdapterPtr trader = i03s5_register_test_trader(*runner, "i03s5_r42_trader");
	ASSERT_TRUE(trader != nullptr);
	const uint32_t bound = runner->createHftContext("i03s5_r42_bound", "i03s5_r42_trader", false, 0);
	ASSERT_NE(0u, bound);
	ExpHftContext* ctx = dynamic_cast<ExpHftContext*>(runner->getHftContext(bound).get());
	ASSERT_NE(nullptr, ctx);
	EXPECT_TRUE(ctx->isTraderBound());
	EXPECT_EQ(1u, trader->_sinks.size());
	runner->release();
}


// ---------------------------------------------------------------------------
// I03-NEW-064: contexts only on the engine the runner configured (NEW-130 path)
// ---------------------------------------------------------------------------

TEST(I03S5RtRunnerEngineScope, ContextsAreRejectedOnUnconfiguredSiblingEngines)
{
	TempDir dir("engine_scope");
	WtRtRunner* unconfigured = new_runner(dir);
	EXPECT_EQ(0u, unconfigured->createCtaContext("i03s5_scope_cta0", 0));
	EXPECT_EQ(0u, unconfigured->createSelContext("i03s5_scope_sel0", 0, 1500, "d", 0));
	ASSERT_TRUE(i03s5_register_test_trader(*unconfigured, "i03s5_scope_trader0") != nullptr);
	EXPECT_EQ(0u, unconfigured->createHftContext("i03s5_scope_hft0", "i03s5_scope_trader0", false, 0));

	WtRtRunner* runner = new_runner(dir);
	ASSERT_TRUE(runner->config(config(dir, "cta", "", "").c_str(), false));
	ASSERT_TRUE(i03s5_register_test_trader(*runner, "i03s5_scope_trader") != nullptr);
	EXPECT_EQ(0u, runner->createSelContext("i03s5_scope_sel", 0, 1500, "d", 0))
		<< "the SEL engine of a CTA runner was never initialized";
	EXPECT_EQ(0u, runner->createHftContext("i03s5_scope_hft", "i03s5_scope_trader", false, 0))
		<< "the HFT engine of a CTA runner was never initialized";
	const uint32_t cta = runner->createCtaContext("i03s5_scope_cta", 0);
	ASSERT_NE(0u, cta);
	EXPECT_NE(nullptr, runner->getCtaContext(cta).get());
	runner->release();
}
