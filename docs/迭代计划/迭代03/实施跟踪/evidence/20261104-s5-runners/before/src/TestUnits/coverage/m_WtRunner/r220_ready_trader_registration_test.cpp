#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
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

#include "Includes/ILogHandler.h"
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
#include "WtRunner/WtRunner.h"
#undef private

#include "Includes/WTSVariant.hpp"

#include <string>
#include <utility>

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

WTSVariant* makeExecuters()
{
	WTSVariant* executers = WTSVariant::createArray();
	WTSVariant* local = WTSVariant::createObject();
	local->append("id", "r220_local");
	local->append("active", true);
	local->append("name", "local");
	local->append("trader", "r220_trader");
	local->append("scale", 1.0);
	executers->append(local, false);

	WTSVariant* diff = WTSVariant::createObject();
	diff->append("id", "r220_diff");
	diff->append("active", true);
	diff->append("name", "diff");
	diff->append("trader", "r220_trader");
	diff->append("scale", 1.0);
	executers->append(diff, false);
	return executers;
}

struct RunnerSetup
{
	std::unique_ptr<WtRunner> runner{new WtRunner()};
	TraderAdapterPtr trader{new TraderAdapter()};

	void registerExecuters(bool ready)
	{
		trader->_state = ready ? TraderAdapter::AS_ALLREADY : TraderAdapter::AS_NOTLOGIN;
		ASSERT_TRUE(runner->_traders.addAdapter("r220_trader", trader));
		WTSVariant* executers = makeExecuters();
		ASSERT_TRUE(runner->initExecuters(executers));
		executers->release();
	}

	WtLocalExecuter* local() const
	{
		return dynamic_cast<WtLocalExecuter*>(runner->_cta_engine.findExecuter("r220_local").get());
	}

	WtDiffExecuter* diff() const
	{
		return dynamic_cast<WtDiffExecuter*>(runner->_cta_engine.findExecuter("r220_diff").get());
	}
};
}

TEST(r220_wtrunner_trader_binding, adapter_registers_sink_before_initial_ready_and_lost)
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

TEST(r220_wtrunner_trader_binding, callback_shutdown_keeps_sink_owners_until_external_drain)
{
	RunnerSetup setup;
	setup.registerExecuters(false);
	ShutdownOnReadySink sink([&setup]() { return setup.runner->shutdownRuntime(false); });
	setup.trader->addSink(&sink);

	setup.trader->_state = TraderAdapter::AS_TRADES_QRYED;
	setup.trader->onRspAccount(nullptr);

	ASSERT_TRUE(sink.called);
	EXPECT_FALSE(sink.result);
	EXPECT_NE(nullptr, setup.runner->_traders.getAdapter("r220_trader"));
	EXPECT_NE(nullptr, setup.runner->_cta_engine.findExecuter("r220_local"));

	EXPECT_TRUE(setup.runner->shutdownRuntime(false));
	EXPECT_NE(nullptr, setup.runner->_traders.getAdapter("r220_trader"));
	EXPECT_NE(nullptr, setup.runner->_cta_engine.findExecuter("r220_local"));
	EXPECT_EQ(setup.trader.get(), setup.local()->_trader);
}

TEST(r220_wtrunner_trader_binding, ready_adapter_initially_notifies_each_registered_executor)
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

TEST(r220_wtrunner_trader_binding, post_registration_ready_and_lost_events_follow_adapter_state)
{
	RunnerSetup setup;
	setup.registerExecuters(false);
	ASSERT_NE(nullptr, setup.local());
	ASSERT_NE(nullptr, setup.diff());
	EXPECT_TRUE(isRegistered(*setup.trader, setup.local()));
	EXPECT_TRUE(isRegistered(*setup.trader, setup.diff()));
	EXPECT_FALSE(setup.local()->_channel_ready.load());
	EXPECT_FALSE(setup.diff()->_channel_ready.load());

	// Exercise the same adapter transition that notifies all registered sinks.
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
