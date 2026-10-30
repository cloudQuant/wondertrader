#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
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
}
