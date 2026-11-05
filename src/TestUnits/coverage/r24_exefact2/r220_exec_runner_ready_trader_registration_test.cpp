#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/core/noncopyable.hpp>
#include "Includes/ExecuteDefs.h"
#include "Includes/FasterDefs.h"
#include "Includes/ITraderApi.h"
#include "Share/DLLHelper.hpp"
#include "Share/BoostFile.hpp"
#include "Share/StdUtils.hpp"
#include "Share/SpinMutex.hpp"

#define private public
#include "WtCore/TraderAdapter.h"
#undef private

#include "WtCore/ITrdNotifySink.h"
#include "WtCore/IExecCommand.h"
#include "WtCore/ExecUnitInitStatus.h"
#include "WtCore/WtExecuterFactory.h"
#include <Share/threadpool.hpp>

#define private public
#include "WtCore/WtLocalExecuter.h"
#include "WtCore/WtDiffExecuter.h"
#undef private

#include "WtCore/WtExecMgr.h"
#include "WtCore/ParserAdapter.h"
#include "WtCore/ActionPolicyMgr.h"
#include "WTSTools/WTSHotMgr.h"
#include "WTSTools/WTSBaseDataMgr.h"
#include "WtExecMon/WtSimpDataMgr.h"
#define private public
#include "WtExecMon/WtExecRunner.h"
#undef private

#include "Includes/WTSVariant.hpp"

using namespace wtp;

namespace
{
bool isRegistered(TraderAdapter& trader, ITrdNotifySink* sink)
{
	for (ITrdNotifySink* registered : trader._sinks)
	{
		if (registered == sink)
			return true;
	}
	return false;
}

class RegistrationOrderSink : public ITrdNotifySink
{
public:
	explicit RegistrationOrderSink(TraderAdapter& trader) : _trader(trader) {}

	void on_trade(uint32_t, const char*, bool, double, double) override {}
	void on_order(uint32_t, const char*, bool, double, double, double, bool) override {}
	void on_channel_ready() override
	{
		ready_seen_registered = isRegistered(_trader, this);
		events.push_back("ready");
	}
	void on_channel_lost() override { events.push_back("lost"); }

	bool ready_seen_registered = false;
	std::vector<std::string> events;

private:
	TraderAdapter& _trader;
};

class ShutdownOnReadySink : public ITrdNotifySink
{
public:
	explicit ShutdownOnReadySink(std::function<bool()> shutdown) : _shutdown(std::move(shutdown)) {}
	void on_trade(uint32_t, const char*, bool, double, double) override {}
	void on_order(uint32_t, const char*, bool, double, double, double, bool) override {}
	void on_channel_ready() override { result = _shutdown(); called = true; }
	void on_channel_lost() override {}

	bool called = false;
	bool result = true;
private:
	std::function<bool()> _shutdown;
};

// 卡8b=A（迭代03 s5-runners）：local/diff 条目须带含 default 的 policy，否则 WtExecRunner 在
// 构造任何执行器之前整表拒绝（原夹具缺 policy，.before 见 s5-runners 证据）。
void appendDefaultPolicy(WTSVariant* executer)
{
	WTSVariant* policy = WTSVariant::createObject();
	WTSVariant* def = WTSVariant::createObject();
	def->append("name", "WtExeFact.WtMinImpactExeUnit");
	policy->append("default", def, false);
	executer->append("policy", policy, false);
}

WTSVariant* makeRunnerConfig()
{
	WTSVariant* executers = WTSVariant::createArray();
	WTSVariant* local = WTSVariant::createObject();
	local->append("id", "r220_exec_local");
	local->append("active", true);
	local->append("name", "local");
	local->append("trader", "r220_exec_trader");
	local->append("scale", 1.0);
	appendDefaultPolicy(local);
	executers->append(local, false);

	WTSVariant* diff = WTSVariant::createObject();
	diff->append("id", "r220_exec_diff");
	diff->append("active", true);
	diff->append("name", "diff");
	diff->append("trader", "r220_exec_trader");
	diff->append("scale", 1.0);
	appendDefaultPolicy(diff);
	executers->append(diff, false);

	WTSVariant* root = WTSVariant::createObject();
	root->append("executers", executers, false);
	return root;
}

struct RunnerSetup
{
	WtExecRunner runner;
	TraderAdapterPtr trader{new TraderAdapter()};

	void registerExecuters(bool ready)
	{
		trader->_state = ready ? TraderAdapter::AS_ALLREADY : TraderAdapter::AS_NOTLOGIN;
		ASSERT_TRUE(runner._traders.addAdapter("r220_exec_trader", trader));
		WTSVariant* cfg = makeRunnerConfig();
		ASSERT_TRUE(runner.initExecuters(cfg));
		cfg->release();
	}

	WtLocalExecuter* local() const
	{
		return dynamic_cast<WtLocalExecuter*>(runner._exe_mgr.find_executer("r220_exec_local").get());
	}

	WtDiffExecuter* diff() const
	{
		return dynamic_cast<WtDiffExecuter*>(runner._exe_mgr.find_executer("r220_exec_diff").get());
	}
};
}

TEST(r220_exec_runner_trader_binding, adapter_registers_sink_before_initial_ready_and_lost)
{
	TraderAdapter trader;
	trader._state = TraderAdapter::AS_ALLREADY;
	RegistrationOrderSink sink(trader);

	trader.addSink(&sink);
	trader.handleEvent(WTE_Close, 0);

	EXPECT_TRUE(sink.ready_seen_registered);
	EXPECT_EQ((std::vector<std::string>{"ready", "lost"}), sink.events);
	EXPECT_FALSE(trader.isReady());
}

TEST(r220_exec_runner_trader_binding, callback_shutdown_keeps_executers_until_external_drain)
{
	RunnerSetup setup;
	setup.registerExecuters(false);
	ShutdownOnReadySink sink([&setup]() { return setup.runner.shutdownRuntime(); });
	setup.trader->addSink(&sink);

	setup.trader->_state = TraderAdapter::AS_TRADES_QRYED;
	setup.trader->onRspAccount(nullptr);

	ASSERT_TRUE(sink.called);
	EXPECT_FALSE(sink.result);
	EXPECT_NE(nullptr, setup.runner._traders.getAdapter("r220_exec_trader"));
	EXPECT_NE(nullptr, setup.runner._exe_mgr.find_executer("r220_exec_local"));

	EXPECT_TRUE(setup.runner.shutdownRuntime());
	EXPECT_NE(nullptr, setup.runner._traders.getAdapter("r220_exec_trader"));
	EXPECT_NE(nullptr, setup.runner._exe_mgr.find_executer("r220_exec_local"));
	EXPECT_EQ(setup.trader.get(), setup.local()->_trader);
}

TEST(r220_exec_runner_trader_binding, ready_adapter_initially_notifies_each_registered_executor)
{
	RunnerSetup setup;
	setup.registerExecuters(true);
	ASSERT_NE(nullptr, setup.local());
	ASSERT_NE(nullptr, setup.diff());

	EXPECT_TRUE(isRegistered(*setup.trader, setup.local()));
	EXPECT_TRUE(isRegistered(*setup.trader, setup.diff()));
	EXPECT_TRUE(setup.local()->_channel_ready.load());
	EXPECT_TRUE(setup.diff()->_channel_ready.load());
	EXPECT_EQ(uint64_t(1), setup.diff()->_channel_epoch.load());
}

TEST(r220_exec_runner_trader_binding, post_registration_ready_and_lost_events_follow_adapter_state)
{
	RunnerSetup setup;
	setup.registerExecuters(false);
	ASSERT_NE(nullptr, setup.local());
	ASSERT_NE(nullptr, setup.diff());
	EXPECT_TRUE(isRegistered(*setup.trader, setup.local()));
	EXPECT_TRUE(isRegistered(*setup.trader, setup.diff()));
	EXPECT_FALSE(setup.local()->_channel_ready.load());
	EXPECT_FALSE(setup.diff()->_channel_ready.load());

	setup.trader->_state = TraderAdapter::AS_TRADES_QRYED;
	setup.trader->onRspAccount(nullptr);
	EXPECT_TRUE(setup.local()->_channel_ready.load());
	EXPECT_TRUE(setup.diff()->_channel_ready.load());
	EXPECT_EQ(uint64_t(1), setup.local()->_channel_epoch.load());
	EXPECT_EQ(uint64_t(1), setup.diff()->_channel_epoch.load());

	setup.trader->handleEvent(WTE_Close, 0);
	EXPECT_FALSE(setup.local()->_channel_ready.load());
	EXPECT_FALSE(setup.diff()->_channel_ready.load());
	EXPECT_EQ(uint64_t(2), setup.local()->_channel_epoch.load());
	EXPECT_EQ(uint64_t(2), setup.diff()->_channel_epoch.load());
}
