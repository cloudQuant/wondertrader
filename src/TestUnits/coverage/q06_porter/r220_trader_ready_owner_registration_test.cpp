#include <unistd.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Includes/ExecuteDefs.h"
#include "Includes/FasterDefs.h"
#include "Includes/ITraderApi.h"
#include "Share/DLLHelper.hpp"
#include "Share/BoostFile.hpp"
#include "Share/StdUtils.hpp"
#include "Share/SpinMutex.hpp"
#include "Includes/WTSVariant.hpp"
#include <Share/threadpool.hpp>

// These headers are dependencies of the classes whose internals this focused
// test inspects. Include them before redefining `private` so the test macro only
// changes the target class declarations.
#include "WtCore/ITrdNotifySink.h"
#include "WtCore/IExecCommand.h"
#include "WtCore/ExecCommandQuiesce.h"
#include "WtCore/ExecUnitInitStatus.h"
#include "WtCore/WtExecuterFactory.h"

#define private public
#include "WtCore/WtLocalExecuter.h"
#include "WtCore/WtDiffExecuter.h"
#include "WtCore/WtArbiExecuter.h"
#include "WtCore/TraderAdapter.h"
#undef private

#include "WtCore/EventNotifier.h"
#include "WtCore/CtaStrategyMgr.h"
#include "WtCore/HftStrategyMgr.h"
#include "WtCore/SelStrategyMgr.h"
#include "WtCore/WtCtaEngine.h"
#include "WtCore/WtHftEngine.h"
#include "WtCore/HftStraContext.h"
#include "WtCore/WtSelEngine.h"
#include "WtCore/WtDistExecuter.h"
#include "WtCore/ParserAdapter.h"
#include "WtCore/WtDtMgr.h"
#include "WtCore/ActionPolicyMgr.h"
#include "WTSTools/WTSHotMgr.h"
#include "WTSTools/WTSBaseDataMgr.h"

#define private public
#include "../../../WtPorter/WtRtRunner.h"
#undef private

USING_NS_WTP;

namespace
{
bool hasSink(const TraderAdapter& trader, ITrdNotifySink* sink)
{
	for (ITrdNotifySink* registered : trader._sinks)
	{
		if (registered == sink)
			return true;
	}
	return false;
}

WTSVariant* makeExecuterConfig(const char* name, const char* id)
{
	WTSVariant* config = WTSVariant::createObject();
	config->append("active", true);
	config->append("name", name);
	config->append("id", id);
	config->append("trader", "r220_ready_trader");
	config->append("scale", 1.0);
	// 卡8b=A（迭代03）：local/diff/arbi 须带含 default 的 policy，否则 eager 校验拒绝整表。
	WTSVariant* policy = WTSVariant::createObject();
	WTSVariant* def = WTSVariant::createObject();
	def->append("name", "WtExeFact.WtMinImpactExeUnit");
	policy->append("default", def, false);
	config->append("policy", policy, false);
	return config;
}

TEST(r220_wtrt_runner_ready_owner_registration, ready_callback_leaves_each_sink_owned_and_ready)
{
	// Use a heap runner because this focused setup does not initialize its full
	// config graph. The test only exercises initExecuters and its owner registry.
	WtRtRunner* runner = new WtRtRunner();
	TraderAdapterPtr trader(new TraderAdapter());
	trader->_state = TraderAdapter::AS_ALLREADY;
	ASSERT_TRUE(runner->_traders.addAdapter("r220_ready_trader", trader));

	WTSVariant* config = WTSVariant::createArray();
	config->append(makeExecuterConfig("local", "r220_ready_local"), false);
	config->append(makeExecuterConfig("diff", "r220_ready_diff"), false);
	config->append(makeExecuterConfig("arbi", "r220_ready_arbi"), false);
	ASSERT_TRUE(runner->initExecuters(config));
	config->release();

	for (const char* id : {"r220_ready_local", "r220_ready_diff", "r220_ready_arbi"})
	{
		ExecCmdPtr owner = runner->_cta_engine.findExecuter(id);
		ASSERT_NE(nullptr, owner.get()) << id;
		ITrdNotifySink* sink = dynamic_cast<ITrdNotifySink*>(owner.get());
		ASSERT_NE(nullptr, sink) << id;
		EXPECT_TRUE(hasSink(*trader, sink)) << id;
	}

	auto local = std::dynamic_pointer_cast<WtLocalExecuter>(
		runner->_cta_engine.findExecuter("r220_ready_local"));
	auto diff = std::dynamic_pointer_cast<WtDiffExecuter>(
		runner->_cta_engine.findExecuter("r220_ready_diff"));
	auto arbi = std::dynamic_pointer_cast<WtArbiExecuter>(
		runner->_cta_engine.findExecuter("r220_ready_arbi"));
	ASSERT_NE(nullptr, local);
	ASSERT_NE(nullptr, diff);
	ASSERT_NE(nullptr, arbi);
	EXPECT_TRUE(local->_channel_ready.load());
	EXPECT_TRUE(diff->_channel_ready.load());
	EXPECT_TRUE(arbi->_channel_ready.load());
}

TEST(r220_wtrt_runner_ready_owner_registration, hft_unknown_trader_preflight_has_no_partial_context)
{
	const auto dir = std::filesystem::temp_directory_path() /
		("r42_hft_factory_" + std::to_string(static_cast<long>(::getpid())) + "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(dir);
	const auto plugin = dir / std::filesystem::path(WT_R6_HFT_FACTORY_PLUGIN).filename();
	std::filesystem::copy_file(WT_R6_HFT_FACTORY_PLUGIN, plugin,
		std::filesystem::copy_options::overwrite_existing);
	WtRtRunner* runner = new WtRtRunner();
	ASSERT_TRUE(runner->_hft_mgr.loadFactories(dir.string().c_str()));
	TraderAdapterPtr trader(new TraderAdapter());
	ASSERT_TRUE(runner->_traders.addAdapter("known", trader));

	WTSVariant* config = WTSVariant::createObject();
	WTSVariant* strategies = WTSVariant::createObject();
	WTSVariant* hft = WTSVariant::createArray();
	for (const auto& entry : {std::pair<const char*, const char*>("first", "known"),
		std::pair<const char*, const char*>("second", "unknown")})
	{
		WTSVariant* unit = WTSVariant::createObject();
		unit->append("active", true);
		unit->append("id", entry.first);
		unit->append("name", "WtHftStraFact.SimpleHft");
		unit->append("trader", entry.second);
		hft->append(unit, false);
	}
	strategies->append("hft", hft, false);
	config->append("strategies", strategies, false);
	runner->_config = config;
	HftStraContext probe(nullptr, "r42_probe", false, 0);
	const auto next = probe.id() + 1;
	EXPECT_FALSE(runner->initHftStrategies());
	EXPECT_EQ(nullptr, runner->_hft_engine.getContext(next).get());
	EXPECT_EQ(nullptr, runner->_hft_mgr.getStrategy("first").get());
	EXPECT_TRUE(trader->_sinks.empty());
	std::filesystem::remove_all(dir);
}
}
