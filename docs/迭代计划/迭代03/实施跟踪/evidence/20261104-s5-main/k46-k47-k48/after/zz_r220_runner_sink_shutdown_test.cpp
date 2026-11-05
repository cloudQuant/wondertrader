#include <unistd.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "Includes/ExecuteDefs.h"
#include "Includes/IBaseDataMgr.h"
#include "Includes/HftStrategyDefs.h"
#include "Includes/IParserApi.h"
#include "Includes/ITraderApi.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#define private public
#include "WtCore/HftStrategyMgr.h"
#undef private
#include "WtCore/ITrdNotifySink.h"
#include "WtCore/ExecCommandQuiesce.h"
#include "WtCore/TraderAdapter.h"
#include "WtCore/WtCtaEngine.h"
#include "WtCore/WtEngine.h"
#include "WtCore/WtHftEngine.h"
#include "WtCore/WtSelEngine.h"
#include "Includes/WTSVariant.hpp"
#include "WTSUtils/WTSCfgLoader.h"

#define private public
#include "../../../WtPorter/WtRtRunner.h"
#undef private

extern WtRtRunner& getRunner();

USING_NS_WTP;

class WtRtRunnerSinkShutdownTestAccess
{
public:
	static WtRtRunner::LifecycleState state(WtRtRunner& runner)
	{
		std::lock_guard<std::mutex> lock(runner._status_config_mutex);
		return runner._lifecycle_state;
	}

	static bool teardownPending(WtRtRunner& runner)
	{
		std::lock_guard<std::mutex> lock(runner._status_config_mutex);
		return runner._teardown_pending;
	}

	static bool teardownInputsStopped(WtRtRunner& runner)
	{
		std::lock_guard<std::mutex> lock(runner._status_config_mutex);
		return runner._teardown_inputs_stopped;
	}
};

namespace
{
WtRtRunner* g_release_on_cta_init_runner = nullptr;
std::atomic<bool>* g_cta_init_release_returned = nullptr;
WtRtRunner* g_ext_parser_reentry_runner = nullptr;
std::atomic<bool>* g_ext_parser_reentry_called = nullptr;
std::atomic<bool>* g_ext_parser_duplicate_create_succeeded = nullptr;
WtRtRunner* g_ext_parser_release_runner = nullptr;
std::atomic<bool>* g_ext_parser_release_called = nullptr;

std::filesystem::path make_unique_r220_temp_dir(const char* label)
{
	static std::atomic<uint64_t> sequence{ 0 };
	std::error_code ec;
	const std::filesystem::path tempRoot = std::filesystem::temp_directory_path(ec);
	if (ec)
		return std::filesystem::path();

	for (uint32_t attempt = 0; attempt < 100; ++attempt)
	{
		const auto now = std::chrono::steady_clock::now().time_since_epoch();
		const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
		const uint64_t serial = sequence.fetch_add(1, std::memory_order_relaxed);
		const std::filesystem::path candidate = tempRoot /
			(std::string("wt_r220_") + label + "_" + std::to_string(stamp) + "_" +
				std::to_string(serial));
		ec.clear();
		if (std::filesystem::create_directory(candidate, ec))
			return candidate;
		if (ec)
			return std::filesystem::path();
	}
	return std::filesystem::path();
}

class ScopedR220TempDir
{
public:
	explicit ScopedR220TempDir(const char* label)
		: _path(make_unique_r220_temp_dir(label))
	{}

	~ScopedR220TempDir()
	{
		if (!_path.empty())
		{
			std::error_code ec;
			std::filesystem::remove_all(_path, ec);
		}
	}

	const std::filesystem::path& path() const { return _path; }

private:
	std::filesystem::path _path;
};

void reenter_quote_from_ext_parser_init(uint32_t event, const char* id)
{
	if (event != EVENT_PARSER_INIT || g_ext_parser_reentry_runner == nullptr)
		return;
	const bool duplicateCreated = g_ext_parser_reentry_runner->createExtParser(id);
	if (g_ext_parser_duplicate_create_succeeded != nullptr)
		g_ext_parser_duplicate_create_succeeded->store(duplicateCreated, std::memory_order_release);
	WTSTickStruct tick{};
	g_ext_parser_reentry_runner->on_ext_parser_quote(id, &tick, 0);
	if (g_ext_parser_reentry_called != nullptr)
		g_ext_parser_reentry_called->store(true, std::memory_order_release);
}

void release_runner_from_ext_parser_init(uint32_t event, const char*)
{
	if (event != EVENT_PARSER_INIT || g_ext_parser_release_runner == nullptr)
		return;
	g_ext_parser_release_runner->release();
	if (g_ext_parser_release_called != nullptr)
		g_ext_parser_release_called->store(true, std::memory_order_release);
}

void release_from_cta_init_callback(CtxHandler)
{
	if (g_release_on_cta_init_runner != nullptr)
		g_release_on_cta_init_runner->release();
	if (g_cta_init_release_returned != nullptr)
		g_cta_init_release_returned->store(true, std::memory_order_release);
}

class FailingParserApi : public IParserApi
{
public:
	bool init(WTSVariant*) override { return false; }
};

class ReadyTraderApi : public ITraderApi
{
public:
	bool init(WTSVariant*) override { return true; }
};

class ReadyParserApi : public IParserApi
{
public:
	bool init(WTSVariant*) override { return true; }
};

class QuoteIngressBaseDataMgr : public IBaseDataMgr
{
public:
	QuoteIngressBaseDataMgr()
	{
		_commodity = WTSCommodityInfo::create("rb", "rb", "SHFE", "ALL", "CHINA");
		_commodity->setCategory(CC_Future);
		_contract = WTSContractInfo::create("rb2501", "rb2501", "SHFE", "rb");
		_contract->setCommInfo(_commodity);
	}

	~QuoteIngressBaseDataMgr()
	{
		_contract->release();
		_commodity->release();
	}

	WTSCommodityInfo* getCommodity(const char*) override { return _commodity; }
	WTSCommodityInfo* getCommodity(const char*, const char*) override { return _commodity; }
	WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t = 0) override
	{
		return code != nullptr && exchg != nullptr &&
			std::strcmp(code, _contract->getCode()) == 0 &&
			std::strcmp(exchg, _contract->getExchg()) == 0 ? _contract : nullptr;
	}
	WTSArray* getContracts(const char* = "", uint32_t = 0) override { return nullptr; }
	WTSSessionInfo* getSession(const char*) override { return nullptr; }
	WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return nullptr; }
	WTSArray* getAllSessions() override { return nullptr; }
	bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
	uint32_t calcTradingDate(const char*, uint32_t date, uint32_t, bool = false) override { return date; }
	uint64_t getBoundaryTime(const char*, uint32_t date, bool = false, bool = true) override { return date; }

private:
	WTSCommodityInfo* _commodity = nullptr;
	WTSContractInfo* _contract = nullptr;
};

class BlockingQuoteEngine : public WtEngine
{
public:
	void run() override {}
	void on_bar(const char*, const char*, uint32_t, WTSBarStruct*) override {}
	void release() override {}
	void handle_push_quote(WTSTickData*) override
	{
		std::unique_lock<std::mutex> lock(_mutex);
		++_quote_calls;
		_quote_started = true;
		_changed.notify_all();
		_changed.wait(lock, [this]() { return _allow_quote_return; });
	}
	bool waitForQuoteStart()
	{
		std::unique_lock<std::mutex> lock(_mutex);
		return _changed.wait_for(lock, std::chrono::seconds(2), [this]() {
			return _quote_started;
		});
	}
	void allowQuoteReturn()
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_allow_quote_return = true;
		_changed.notify_all();
	}
	uint32_t quoteCalls() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _quote_calls;
	}

private:
	mutable std::mutex _mutex;
	std::condition_variable _changed;
	bool _quote_started = false;
	bool _allow_quote_return = false;
	uint32_t _quote_calls = 0;
};

class CountingChannelSink : public ITrdNotifySink
{
public:
	void on_trade(uint32_t, const char*, bool, double, double) override {}
	void on_order(uint32_t, const char*, bool, double, double, double, bool) override {}
	void on_channel_ready() override { ++ready_calls; }
	void on_channel_lost() override { ++lost_calls; }
	uint32_t ready_calls = 0;
	uint32_t lost_calls = 0;
};

class CallbackWaitQuiesceCommand : public IExecCommand,
	public IExecCommandQuiesce, public IExecCommandQuiesceStages
{
public:
	CallbackWaitQuiesceCommand()
		: IExecCommand("r220_callback_wait_quiesce") {}

	void requestQuiesce() override {}
	bool canWaitQuiescedOnCurrentThread() const override { return true; }
	bool waitQuiesced() override { return true; }
	bool preflightQuiesceOnCurrentThread() const override { return true; }
	bool drainHostQuiesced() override
	{
		std::unique_lock<std::mutex> lock(_mutex);
		_drain_started = true;
		_changed.notify_all();
		const bool drained = _changed.wait_for(lock, std::chrono::seconds(2), [this]() {
			return _callback_exited;
		});
		_drain_succeeded.store(drained, std::memory_order_release);
		return drained;
	}
	bool snapshotQuiescePluginOwners(std::vector<ExecQuiesceOwnerPtr>& out) const override
	{
		out.clear();
		return true;
	}
	bool completeQuiesceAfterGlobalPluginWait() override { return true; }
	WtExecuterFactory* quiesceFactory() const override { return nullptr; }
	bool waitForDrainStart()
	{
		std::unique_lock<std::mutex> lock(_mutex);
		return _changed.wait_for(lock, std::chrono::seconds(2), [this]() {
			return _drain_started;
		});
	}
	void markCallbackExited()
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_callback_exited = true;
		_changed.notify_all();
	}
	bool drainSucceeded() const { return _drain_succeeded.load(std::memory_order_acquire); }

private:
	mutable std::mutex _mutex;
	std::condition_variable _changed;
	bool _drain_started = false;
	bool _callback_exited = false;
	std::atomic<bool> _drain_succeeded{ false };
};

class ReleaseFromLostSink : public IExecCommand, public ITrdNotifySink,
	public IExecCommandQuiesce, public IExecCommandQuiesceStages
{
public:
	explicit ReleaseFromLostSink(WtRtRunner& runner)
		: IExecCommand("r220_release_from_lost_sink"), _runner(runner) {}

	void on_trade(uint32_t, const char*, bool, double, double) override {}
	void on_order(uint32_t, const char*, bool, double, double, double, bool) override {}
	void on_channel_ready() override {}
	void on_channel_lost() override
	{
		++_lostCalls;
		_runner.release();
	}

	uint32_t lostCalls() const { return _lostCalls; }
	void requestQuiesce() override { _requested = true; }
	bool canWaitQuiescedOnCurrentThread() const override { return true; }
	bool waitQuiesced() override { return _requested; }
	bool preflightQuiesceOnCurrentThread() const override { return true; }
	bool drainHostQuiesced() override { return true; }
	bool snapshotQuiescePluginOwners(std::vector<ExecQuiesceOwnerPtr>& out) const override
	{
		out.clear();
		return true;
	}
	bool completeQuiesceAfterGlobalPluginWait() override { return true; }
	WtExecuterFactory* quiesceFactory() const override { return nullptr; }

private:
	WtRtRunner& _runner;
	uint32_t _lostCalls = 0;
	bool _requested = false;
};

class RetryableQuiesceCommand : public IExecCommand,
	public IExecCommandQuiesce, public IExecCommandQuiesceStages
{
public:
	RetryableQuiesceCommand(const char* name, bool failFirstDrain)
		: IExecCommand(name), _failFirstDrain(failFirstDrain) {}

	void setPeer(const std::shared_ptr<RetryableQuiesceCommand>& peer) { _peer = peer; }
	uint32_t requestCount() const { return _requestCount.load(std::memory_order_acquire); }
	uint32_t drainCount() const { return _drainCount.load(std::memory_order_acquire); }
	bool acknowledged() const { return _acknowledged.load(std::memory_order_acquire); }
	bool allPeersRequestedBeforeDrain() const
	{
		return _allPeersRequestedBeforeDrain.load(std::memory_order_acquire);
	}

	void requestQuiesce() override { _requestCount.fetch_add(1, std::memory_order_acq_rel); }
	bool canWaitQuiescedOnCurrentThread() const override { return true; }
	bool waitQuiesced() override { return true; }
	bool preflightQuiesceOnCurrentThread() const override { return true; }
	bool drainHostQuiesced() override
	{
		std::shared_ptr<RetryableQuiesceCommand> peer = _peer.lock();
		const bool peerRequested = peer != nullptr && peer->requestCount() != 0;
		_allPeersRequestedBeforeDrain.store(peerRequested, std::memory_order_release);
		const uint32_t attempt = _drainCount.fetch_add(1, std::memory_order_acq_rel) + 1;
		return !_failFirstDrain || attempt > 1;
	}
	bool snapshotQuiescePluginOwners(std::vector<ExecQuiesceOwnerPtr>& out) const override
	{
		out.clear();
		return true;
	}
	bool completeQuiesceAfterGlobalPluginWait() override
	{
		_acknowledged.store(true, std::memory_order_release);
		return true;
	}
	WtExecuterFactory* quiesceFactory() const override { return nullptr; }

private:
	bool _failFirstDrain;
	std::weak_ptr<RetryableQuiesceCommand> _peer;
	std::atomic<uint32_t> _requestCount{ 0 };
	std::atomic<uint32_t> _drainCount{ 0 };
	std::atomic<bool> _allPeersRequestedBeforeDrain{ false };
	std::atomic<bool> _acknowledged{ false };
};

class ReleaseOnReadyHftStrategy : public HftStrategy
{
public:
	ReleaseOnReadyHftStrategy(const char* id, WtRtRunner& runner,
		std::atomic<bool>& called, std::atomic<WtRtRunner::LifecycleState>& stateAfterRelease)
		: HftStrategy(id), _runner(runner), _called(called), _stateAfterRelease(stateAfterRelease) {}

	const char* getName() override { return "ReleaseOnReady"; }
	const char* getFactName() override { return "r220_ready_release"; }
	void on_init(IHftStraCtx*) override {}
	void on_channel_ready(IHftStraCtx*) override
	{
		_called.store(true, std::memory_order_release);
		_runner.release();
		_stateAfterRelease.store(WtRtRunnerSinkShutdownTestAccess::state(_runner),
			std::memory_order_release);
	}

private:
	WtRtRunner& _runner;
	std::atomic<bool>& _called;
	std::atomic<WtRtRunner::LifecycleState>& _stateAfterRelease;
};

class ReleaseOnReadyHftFactory : public IHftStrategyFact
{
public:
	ReleaseOnReadyHftFactory(WtRtRunner& runner, std::atomic<bool>& called,
		std::atomic<WtRtRunner::LifecycleState>& stateAfterRelease)
		: _runner(runner), _called(called), _stateAfterRelease(stateAfterRelease) {}

	const char* getName() override { return "r220_ready_release"; }
	void enumStrategy(FuncEnumHftStrategyCallback callback) override
	{
		callback(getName(), "ReleaseOnReady", true);
	}
	HftStrategy* createStrategy(const char* name, const char* id) override
	{
		if (std::string(name) != "ReleaseOnReady")
			return nullptr;
		return new ReleaseOnReadyHftStrategy(id, _runner, _called, _stateAfterRelease);
	}
	bool deleteStrategy(HftStrategy* strategy) override
	{
		delete strategy;
		return true;
	}

private:
	WtRtRunner& _runner;
	std::atomic<bool>& _called;
	std::atomic<WtRtRunner::LifecycleState>& _stateAfterRelease;
};

TEST(r220_wtrt_runner_sink_shutdown, reentrant_release_keeps_owners_until_control_thread_retry)
{
	WtRtRunner runner;
	runner._engine = &runner._cta_engine;
	WtExecQuiesceCoordinator* coordinator = runner._exec_quiesce_coordinator.get();
	ASSERT_NE(nullptr, coordinator);

	ExecCmdPtr owner(new ReleaseFromLostSink(runner));
	std::shared_ptr<ReleaseFromLostSink> sink =
		std::dynamic_pointer_cast<ReleaseFromLostSink>(owner);
	ASSERT_NE(nullptr, sink);
	runner._cta_engine.addExecuter(owner);

	TraderAdapterPtr trader(new TraderAdapter());
	trader->addSink(sink.get());
	ASSERT_TRUE(runner._traders.addAdapter("r220_callback_trader", trader));

	// Drive the real TraderAdapter broadcast path. The sink asks the runner to
	// release from inside the callback that the two-phase barrier must drain.
	trader->handleEvent(WTE_Close, 7);

	EXPECT_EQ(1u, sink->lostCalls());
	EXPECT_EQ(WtRtRunner::LifecycleState::STOPPING,
		WtRtRunnerSinkShutdownTestAccess::state(runner));
	EXPECT_EQ(coordinator, runner._exec_quiesce_coordinator.get());
	EXPECT_TRUE(WtRtRunnerSinkShutdownTestAccess::teardownPending(runner));
	EXPECT_FALSE(WtRtRunnerSinkShutdownTestAccess::teardownInputsStopped(runner));
	EXPECT_EQ(trader.get(), runner._traders.getAdapter("r220_callback_trader").get());
	EXPECT_EQ(sink.get(), runner._cta_engine.findExecuter("r220_release_from_lost_sink").get());

	// A later control-thread call drains the callback and completes release.
	runner.release();
	EXPECT_EQ(WtRtRunner::LifecycleState::RELEASED,
		WtRtRunnerSinkShutdownTestAccess::state(runner));
	EXPECT_EQ(coordinator, runner._exec_quiesce_coordinator.get());
	EXPECT_EQ(nullptr, runner._traders.getAdapter("r220_callback_trader").get());
	EXPECT_EQ(sink.get(), runner._cta_engine.findExecuter("r220_release_from_lost_sink").get());
}

TEST(r220_wtrt_runner_sink_shutdown, quiesces_all_cta_sel_hosts_and_retries_without_releasing_owners)
{
	WtRtRunner runner;
	runner._engine = &runner._cta_engine;
	WtExecQuiesceCoordinator* coordinator = runner._exec_quiesce_coordinator.get();
	ASSERT_NE(nullptr, coordinator);

	std::shared_ptr<RetryableQuiesceCommand> cta(
		new RetryableQuiesceCommand("r220_retry_cta_quiesce", true));
	std::shared_ptr<RetryableQuiesceCommand> sel(
		new RetryableQuiesceCommand("r220_retry_sel_quiesce", false));
	cta->setPeer(sel);
	sel->setPeer(cta);
	runner._cta_engine.addExecuter(ExecCmdPtr(cta));
	ExecCmdPtr selOwner(sel);
	runner._sel_engine.addExecuter(selOwner);

	TraderAdapterPtr trader(new TraderAdapter());
	ASSERT_TRUE(runner._traders.addAdapter("r220_quiesce_retry_trader", trader));

	// A host drain fails once. The runner must retain the same managers and API
	// owners, then retry the same coordinator from a control thread.
	runner.release();
	EXPECT_EQ(WtRtRunner::LifecycleState::STOPPING,
		WtRtRunnerSinkShutdownTestAccess::state(runner));
	EXPECT_EQ(coordinator, runner._exec_quiesce_coordinator.get());
	EXPECT_TRUE(WtRtRunnerSinkShutdownTestAccess::teardownPending(runner));
	EXPECT_FALSE(WtRtRunnerSinkShutdownTestAccess::teardownInputsStopped(runner));
	EXPECT_EQ(2u, cta->requestCount());
	EXPECT_EQ(2u, sel->requestCount());
	EXPECT_EQ(1u, cta->drainCount());
	EXPECT_EQ(1u, sel->drainCount());
	EXPECT_TRUE(cta->allPeersRequestedBeforeDrain());
	EXPECT_TRUE(sel->allPeersRequestedBeforeDrain());
	EXPECT_EQ(cta.get(), runner._cta_engine.findExecuter("r220_retry_cta_quiesce").get());
	EXPECT_EQ(sel.get(), runner._sel_engine.findExecuter("r220_retry_sel_quiesce").get());
	EXPECT_EQ(trader.get(), runner._traders.getAdapter("r220_quiesce_retry_trader").get());

	runner.release();
	EXPECT_EQ(WtRtRunner::LifecycleState::RELEASED,
		WtRtRunnerSinkShutdownTestAccess::state(runner));
	EXPECT_EQ(coordinator, runner._exec_quiesce_coordinator.get());
	EXPECT_EQ(2u, cta->requestCount());
	EXPECT_EQ(2u, sel->requestCount());
	EXPECT_EQ(2u, cta->drainCount());
	EXPECT_EQ(1u, sel->drainCount());
	EXPECT_TRUE(cta->acknowledged());
	EXPECT_TRUE(sel->acknowledged());
	EXPECT_TRUE(runner._cta_engine.getExecuterMgr().wait_quiesced());
	EXPECT_TRUE(runner._sel_engine.getExecuterMgr().wait_quiesced());
	EXPECT_EQ(cta.get(), runner._cta_engine.findExecuter("r220_retry_cta_quiesce").get());
	EXPECT_EQ(sel.get(), runner._sel_engine.findExecuter("r220_retry_sel_quiesce").get());
	EXPECT_EQ(nullptr, runner._traders.getAdapter("r220_quiesce_retry_trader").get());
}

TEST(r220_wtrt_runner_sink_shutdown, config_ready_callback_defers_release_until_config_unwinds)
{
	const std::filesystem::path directory =
		std::filesystem::temp_directory_path() / ("wt_r220_config_ready_release_" + std::to_string(static_cast<long>(::getpid())));
	std::error_code ec;
	std::filesystem::remove_all(directory, ec);
	ASSERT_TRUE(std::filesystem::create_directories(directory, ec) || !ec);
	const std::filesystem::path sessionPath = directory / "sessions.yaml";
	const std::filesystem::path actionPath = directory / "actions.json";
	{
		std::ofstream sessions(sessionPath, std::ios::binary | std::ios::trunc);
		sessions << "ALLDAY:\n"
			"    name: AllDay\n"
			"    offset: 0\n"
			"    sections:\n"
			"        - from: 0\n"
			"          to: 0\n";
		ASSERT_TRUE(sessions.good());
		std::ofstream actions(actionPath, std::ios::binary | std::ios::trunc);
		actions << "{\"default\":{}}";
		ASSERT_TRUE(actions.good());
	}

	WtRtRunner runner;
	std::atomic<bool> callbackCalled{ false };
	std::atomic<WtRtRunner::LifecycleState> callbackStateAfterRelease{
		WtRtRunner::LifecycleState::UNCONFIGURED };
	ReleaseOnReadyHftFactory factory(runner, callbackCalled, callbackStateAfterRelease);
	// The manager has no in-process registration API for test factories.
	HftStrategyMgr::StraFactInfo& factoryInfo = runner._hft_mgr._factories[factory.getName()];
	factoryInfo._fact = &factory;

	const std::string noLogConfig = (directory / "no_log_config.yaml").string();
	ASSERT_TRUE(runner.init(noLogConfig.c_str(), true, directory.string().c_str()));
	ReadyTraderApi api;
	TraderAdapterPtr trader(new TraderAdapter());
	ASSERT_TRUE(trader->initExt("r220_config_ready_trader", &api, nullptr, nullptr));
	trader->onLoginResult(true, "test login", 20261030);
	trader->onRspPosition(nullptr);
	trader->onRspOrders(nullptr);
	trader->onRspTrades(nullptr);
	trader->onRspAccount(nullptr);
	ASSERT_TRUE(trader->isReady());
	ASSERT_TRUE(runner._traders.addAdapter("r220_config_ready_trader", trader));

	const std::string config = std::string("{\"basefiles\":{\"session\":\"") +
		sessionPath.generic_string() + "\"},\"env\":{\"name\":\"hft\",\"product\":{\"session\":\"ALLDAY\"}}," +
		"\"data\":{},\"bspolicy\":\"" + actionPath.generic_string() +
		"\",\"parsers\":[],\"traders\":[],\"executers\":[],\"strategies\":{\"hft\":[{" +
		"\"active\":true,\"id\":\"r220_ready_release\",\"name\":\"r220_ready_release.ReleaseOnReady\"," +
		"\"trader\":\"r220_config_ready_trader\",\"params\":{}}]}}";
	EXPECT_FALSE(runner.config(config.c_str(), false));
	EXPECT_TRUE(callbackCalled.load(std::memory_order_acquire));
	EXPECT_EQ(WtRtRunner::LifecycleState::STOPPING,
		callbackStateAfterRelease.load(std::memory_order_acquire));
	EXPECT_EQ(WtRtRunner::LifecycleState::RELEASED,
		WtRtRunnerSinkShutdownTestAccess::state(runner));
	EXPECT_EQ(nullptr, runner._traders.getAdapter("r220_config_ready_trader").get());

	// Destroy the strategy before the stack-owned fake factory goes away.
	runner._hft_mgr._strategies.clear();
	runner._hft_mgr._factories.clear();
	std::filesystem::remove_all(directory, ec);
	EXPECT_FALSE(static_cast<bool>(ec));
	EXPECT_EQ(WtRtRunner::LifecycleState::RELEASED,
		WtRtRunnerSinkShutdownTestAccess::state(runner));
}

TEST(r220_wtrt_runner_sink_shutdown, parser_without_created_api_is_quarantined_outside_runnable_manager)
{
	WtRtRunner runner;
	runner._engine = &runner._cta_engine;
	WTSVariant* parserRoot = WTSCfgLoader::load_from_content(
		R"json({"parsers":[{"active":true,"id":"r220_failed_parser","module":""}]})json");
	ASSERT_NE(nullptr, parserRoot);
	WTSVariant* parserConfig = parserRoot->get("parsers");
	ASSERT_NE(nullptr, parserConfig);

	EXPECT_FALSE(runner.initParsers(parserConfig));
	parserRoot->release();
	ASSERT_EQ(1U, runner._quarantined_parser_owners.size());
	EXPECT_EQ(nullptr, runner._parsers.getAdapter("r220_failed_parser").get());

	// The failed candidate is not visible to the manager's run/connect loop.
	EXPECT_NO_THROW(runner._parsers.run());

	runner.release();
	EXPECT_EQ(WtRtRunner::LifecycleState::RELEASED,
		WtRtRunnerSinkShutdownTestAccess::state(runner));
	EXPECT_TRUE(runner._quarantined_parser_owners.empty());
}

TEST(r220_wtrt_runner_sink_shutdown, parser_api_failure_keeps_runner_graph_until_drain_proof)
{
	ReadyTraderApi traderApi;
	CountingChannelSink initialSink;
	CountingChannelSink lateSink;
	WtRtRunner runner;
	runner._engine = &runner._cta_engine;
	TraderAdapterPtr trader(new TraderAdapter());
	ASSERT_TRUE(trader->initExt("r220_quarantine_trader", &traderApi, nullptr, nullptr));
	trader->onLoginResult(true, "test login", 20261030);
	trader->onRspPosition(nullptr);
	trader->onRspOrders(nullptr);
	trader->onRspTrades(nullptr);
	trader->onRspAccount(nullptr);
	ASSERT_TRUE(trader->isReady());
	trader->addSink(&initialSink);
	ASSERT_EQ(1U, initialSink.ready_calls);
	ASSERT_TRUE(runner._traders.addAdapter("r220_quarantine_trader", trader));

	ParserAdapterPtr candidate(new ParserAdapter);
	EXPECT_FALSE(candidate->initExt("r220_failed_api_parser", new FailingParserApi(),
		&runner, nullptr, nullptr));
	ASSERT_TRUE(candidate->hasApi());
	runner._quarantined_parser_owners.emplace_back(candidate);
	EXPECT_FALSE(runner.config("r220_missing_reconfigure.json", true));
	EXPECT_EQ(WtRtRunner::LifecycleState::UNCONFIGURED,
		WtRtRunnerSinkShutdownTestAccess::state(runner));
	EXPECT_EQ(candidate.get(), runner._quarantined_parser_owners.front().get());

	runner.release();
	EXPECT_TRUE(WtRtRunnerSinkShutdownTestAccess::teardownPending(runner));
	trader->addSink(&lateSink);
	EXPECT_EQ(0U, lateSink.ready_calls);
	EXPECT_EQ(WtRtRunner::LifecycleState::STOPPING,
		WtRtRunnerSinkShutdownTestAccess::state(runner));
	EXPECT_EQ(&runner._cta_engine, runner._engine);
	EXPECT_EQ(candidate.get(), runner._quarantined_parser_owners.front().get());
	EXPECT_EQ(trader.get(), runner._traders.getAdapter("r220_quarantine_trader").get());
	EXPECT_FALSE(runner.config("r220_missing_reconfigure.json", true));
	EXPECT_EQ(WtRtRunner::LifecycleState::STOPPING,
		WtRtRunnerSinkShutdownTestAccess::state(runner));
	EXPECT_EQ(candidate.get(), runner._quarantined_parser_owners.front().get());

	// This fake API has no worker or callback source. Its test-local release is
	// the fixture's provider-specific drain proof; production has no such proof.
	candidate->release();
	runner._quarantined_parser_owners.clear();
	runner.release();
	EXPECT_EQ(WtRtRunner::LifecycleState::RELEASED,
		WtRtRunnerSinkShutdownTestAccess::state(runner));
}

TEST(r220_wtrt_runner_sink_shutdown, missing_config_preflight_keeps_safe_retry_available)
{
	const std::filesystem::path directory =
		std::filesystem::temp_directory_path() / ("wt_r220_config_preflight_retry_" + std::to_string(static_cast<long>(::getpid())));
	std::error_code ec;
	std::filesystem::remove_all(directory, ec);
	ASSERT_TRUE(std::filesystem::create_directories(directory, ec) || !ec);
	const std::filesystem::path actionPath = directory / "actions.json";
	{
		std::ofstream actions(actionPath, std::ios::binary | std::ios::trunc);
		actions << "{\"default\":{}}";
		ASSERT_TRUE(actions.good());
	}

	WtRtRunner runner;
	const std::string noLogConfig = (directory / "no_log_config.yaml").string();
	ASSERT_TRUE(runner.init(noLogConfig.c_str(), true, directory.string().c_str()));
	EXPECT_FALSE(runner.config((directory / "missing.json").string().c_str(), true));
	EXPECT_EQ(WtRtRunner::LifecycleState::UNCONFIGURED,
		WtRtRunnerSinkShutdownTestAccess::state(runner));

	const std::string validConfig = std::string("{\"basefiles\":{},\"env\":{\"name\":\"cta\"},") +
		"\"data\":{},\"bspolicy\":\"" + actionPath.generic_string() +
		"\",\"strategies\":{\"cta\":[]}}";
	EXPECT_TRUE(runner.config(validConfig.c_str(), false));
	EXPECT_FALSE(runner.config(validConfig.c_str(), false));
	runner.release();
	EXPECT_EQ(WtRtRunner::LifecycleState::RELEASED,
		WtRtRunnerSinkShutdownTestAccess::state(runner));

	std::filesystem::remove_all(directory, ec);
	EXPECT_FALSE(static_cast<bool>(ec));
}

TEST(r220_wtrt_runner_sink_shutdown, callback_reentry_does_not_wait_on_teardown_that_waits_for_callback)
{
	WtRtRunner runner;
	runner._engine = &runner._cta_engine;
	runner._lifecycle_state = WtRtRunner::LifecycleState::RUNNING;
	std::shared_ptr<CallbackWaitQuiesceCommand> command(new CallbackWaitQuiesceCommand);
	ExecCmdPtr commandOwner = command;
	runner._cta_engine.addExecuter(commandOwner);
	std::atomic<bool> callbackReleaseReturned{ false };
	g_release_on_cta_init_runner = &runner;
	g_cta_init_release_returned = &callbackReleaseReturned;
	runner.registerCtaCallbacks(&release_from_cta_init_callback,
		nullptr, nullptr, nullptr, nullptr, nullptr);

	std::thread shutdownThread([&runner]() { runner.release(); });
	const bool drainStarted = command->waitForDrainStart();
	if (!drainStarted)
	{
		command->markCallbackExited();
		shutdownThread.join();
		g_release_on_cta_init_runner = nullptr;
		g_cta_init_release_returned = nullptr;
		FAIL() << "runner teardown did not reach the controlled executor drain";
		return;
	}

	std::thread callbackThread([&runner, &command]() {
		runner.ctx_on_init(202, ET_CTA);
		command->markCallbackExited();
	});
	callbackThread.join();
	shutdownThread.join();
	g_release_on_cta_init_runner = nullptr;
	g_cta_init_release_returned = nullptr;

	const bool drainSucceeded = command->drainSucceeded();
	if (WtRtRunnerSinkShutdownTestAccess::state(runner) != WtRtRunner::LifecycleState::RELEASED)
		runner.release();
	EXPECT_TRUE(callbackReleaseReturned.load(std::memory_order_acquire));
	EXPECT_TRUE(drainSucceeded);
	EXPECT_EQ(WtRtRunner::LifecycleState::RELEASED,
		WtRtRunnerSinkShutdownTestAccess::state(runner));
}

TEST(r220_wtrt_runner_sink_shutdown, external_parser_requires_configured_engine)
{
	WtRtRunner runner;
	EXPECT_FALSE(runner.createExtParser("r220_parser_without_engine"));
	EXPECT_FALSE(runner.createExtParser(""));
}

TEST(r220_wtrt_runner_sink_shutdown, ext_parser_init_callback_can_reenter_quote_registry)
{
	ScopedR220TempDir tempDir("ext_parser_reentry_quote");
	ASSERT_FALSE(tempDir.path().empty());
	const std::filesystem::path directory = tempDir.path();
	const std::filesystem::path sessionPath = directory / "sessions.yaml";
	const std::filesystem::path actionPath = directory / "actions.json";
	{
		std::ofstream sessions(sessionPath, std::ios::binary | std::ios::trunc);
		sessions << "ALLDAY:\n"
			"    name: AllDay\n"
			"    offset: 0\n"
			"    sections:\n"
			"        - from: 0\n"
			"          to: 0\n";
		ASSERT_TRUE(sessions.good());
		std::ofstream actions(actionPath, std::ios::binary | std::ios::trunc);
		actions << "{\"default\":{}}";
		ASSERT_TRUE(actions.good());
	}

	WtRtRunner runner;
	const std::string noLogConfig = (directory / "no_log_config.yaml").string();
	ASSERT_TRUE(runner.init(noLogConfig.c_str(), true, directory.string().c_str()));
	const std::string config = std::string("{\"basefiles\":{\"session\":\"") +
		sessionPath.generic_string() + "\"},\"env\":{\"name\":\"cta\",\"product\":{\"session\":\"ALLDAY\"}}," +
		"\"data\":{},\"bspolicy\":\"" + actionPath.generic_string() +
		"\",\"parsers\":[],\"traders\":[],\"executers\":[]}";
	ASSERT_TRUE(runner.config(config.c_str(), false));
	ASSERT_EQ(WtRtRunner::LifecycleState::CONFIGURED,
		WtRtRunnerSinkShutdownTestAccess::state(runner));
	ASSERT_NE(nullptr, runner._engine);
	ASSERT_NE(nullptr, runner._engine->get_basedata_mgr());

	WtRtRunner& callbackRunner = getRunner();
	const FuncParserEvtCallback previousEventCallback = callbackRunner._cb_parser_evt;
	const FuncParserSubCallback previousSubCallback = callbackRunner._cb_parser_sub;
	callbackRunner.registerParserPorter(&reenter_quote_from_ext_parser_init, nullptr);
	std::atomic<bool> callbackCalled{ false };
	std::atomic<bool> duplicateCreateSucceeded{ false };
	g_ext_parser_reentry_runner = &runner;
	g_ext_parser_reentry_called = &callbackCalled;
	g_ext_parser_duplicate_create_succeeded = &duplicateCreateSucceeded;
	const bool created = runner.createExtParser("r220_reentrant_init_quote");
	g_ext_parser_reentry_runner = nullptr;
	g_ext_parser_reentry_called = nullptr;
	g_ext_parser_duplicate_create_succeeded = nullptr;
	runner.release();
	callbackRunner.registerParserPorter(previousEventCallback, previousSubCallback);

	EXPECT_TRUE(callbackCalled.load(std::memory_order_acquire));
	EXPECT_TRUE(created);
	EXPECT_FALSE(duplicateCreateSucceeded.load(std::memory_order_acquire));
	EXPECT_EQ(WtRtRunner::LifecycleState::RELEASED,
		WtRtRunnerSinkShutdownTestAccess::state(runner));
}

TEST(r220_wtrt_runner_sink_shutdown, ext_parser_init_racing_release_is_quarantined_not_published)
{
	ScopedR220TempDir tempDir("ext_parser_release_race");
	ASSERT_FALSE(tempDir.path().empty());
	const std::filesystem::path directory = tempDir.path();
	const std::filesystem::path sessionPath = directory / "sessions.yaml";
	const std::filesystem::path actionPath = directory / "actions.json";
	{
		std::ofstream sessions(sessionPath, std::ios::binary | std::ios::trunc);
		sessions << "ALLDAY:\n"
			"    name: AllDay\n"
			"    offset: 0\n"
			"    sections:\n"
			"        - from: 0\n"
			"          to: 0\n";
		ASSERT_TRUE(sessions.good());
		std::ofstream actions(actionPath, std::ios::binary | std::ios::trunc);
		actions << "{\"default\":{}}";
		ASSERT_TRUE(actions.good());
	}

	WtRtRunner runner;
	const std::string noLogConfig = (directory / "no_log_config.yaml").string();
	ASSERT_TRUE(runner.init(noLogConfig.c_str(), true, directory.string().c_str()));
	const std::string config = std::string("{\"basefiles\":{\"session\":\"") +
		sessionPath.generic_string() + "\"},\"env\":{\"name\":\"cta\",\"product\":{\"session\":\"ALLDAY\"}}," +
		"\"data\":{},\"bspolicy\":\"" + actionPath.generic_string() +
		"\",\"parsers\":[],\"traders\":[],\"executers\":[]}";
	ASSERT_TRUE(runner.config(config.c_str(), false));
	ASSERT_EQ(WtRtRunner::LifecycleState::CONFIGURED,
		WtRtRunnerSinkShutdownTestAccess::state(runner));
	ASSERT_NE(nullptr, runner._engine);
	ASSERT_NE(nullptr, runner._engine->get_basedata_mgr());

	WtRtRunner& callbackRunner = getRunner();
	const FuncParserEvtCallback previousEventCallback = callbackRunner._cb_parser_evt;
	const FuncParserSubCallback previousSubCallback = callbackRunner._cb_parser_sub;
	callbackRunner.registerParserPorter(&release_runner_from_ext_parser_init, nullptr);
	std::atomic<bool> callbackCalled{ false };
	g_ext_parser_release_runner = &runner;
	g_ext_parser_release_called = &callbackCalled;
	const bool created = runner.createExtParser("r220_init_release_race_parser");
	g_ext_parser_release_runner = nullptr;
	g_ext_parser_release_called = nullptr;

	EXPECT_TRUE(callbackCalled.load(std::memory_order_acquire));
	EXPECT_FALSE(created);
	EXPECT_EQ(WtRtRunner::LifecycleState::STOPPING,
		WtRtRunnerSinkShutdownTestAccess::state(runner));
	EXPECT_TRUE(WtRtRunnerSinkShutdownTestAccess::teardownPending(runner));
	EXPECT_EQ(nullptr, runner._parsers.getAdapter("r220_init_release_race_parser").get());
	EXPECT_EQ(1U, runner._quarantined_parser_owners.size());
	if (!runner._quarantined_parser_owners.empty())
		EXPECT_TRUE(runner._quarantined_parser_owners.front()->hasApi());

	// The test API has no producer; explicitly drain it so the runner can finish.
	for (const ParserAdapterPtr& candidate : runner._quarantined_parser_owners)
		if (candidate != nullptr)
			candidate->release();
	runner._quarantined_parser_owners.clear();
	runner.release();
	callbackRunner.registerParserPorter(previousEventCallback, previousSubCallback);
	EXPECT_EQ(WtRtRunner::LifecycleState::RELEASED,
		WtRtRunnerSinkShutdownTestAccess::state(runner));
}

TEST(r220_wtrt_runner_sink_shutdown, external_parser_quote_is_drained_before_parser_map_release)
{
	QuoteIngressBaseDataMgr baseData;
	BlockingQuoteEngine engine;
	WtRtRunner runner;
	runner._engine = &engine;
	runner._lifecycle_state = WtRtRunner::LifecycleState::RUNNING;

	ParserAdapterPtr parser(new ParserAdapter);
	ASSERT_TRUE(parser->initExt("r220_blocking_quote_parser", new ReadyParserApi,
		&runner, &baseData, nullptr));
	ASSERT_TRUE(runner._parsers.addAdapter("r220_blocking_quote_parser", parser));

	WTSTickStruct tick{};
	std::strcpy(tick.exchg, "SHFE");
	std::strcpy(tick.code, "rb2501");
	tick.action_date = 20261030;
	tick.trading_date = 20261030;

	std::thread quoteThread([&runner, &tick]() {
		runner.on_ext_parser_quote("r220_blocking_quote_parser", &tick, 0);
	});
	if (!engine.waitForQuoteStart())
	{
		engine.allowQuoteReturn();
		quoteThread.join();
		FAIL() << "external parser quote did not reach the controlled engine callback";
		return;
	}

	// Stop closes quote admission. Existing in-flight work keeps the parser,
	// engine and registry owners pinned until the control thread retries.
	runner.release();
	EXPECT_EQ(WtRtRunner::LifecycleState::STOPPING,
		WtRtRunnerSinkShutdownTestAccess::state(runner));
	EXPECT_TRUE(WtRtRunnerSinkShutdownTestAccess::teardownPending(runner));
	EXPECT_EQ(parser.get(), runner._parsers.getAdapter("r220_blocking_quote_parser").get());
	runner.on_ext_parser_quote("r220_blocking_quote_parser", &tick, 0);
	EXPECT_EQ(1U, engine.quoteCalls());

	engine.allowQuoteReturn();
	quoteThread.join();
	runner.release();
	EXPECT_EQ(WtRtRunner::LifecycleState::RELEASED,
		WtRtRunnerSinkShutdownTestAccess::state(runner));
	EXPECT_EQ(nullptr, runner._parsers.getAdapter("r220_blocking_quote_parser").get());
}

TEST(r220_wtrt_runner_sink_shutdown, producer_callback_release_waits_for_control_thread_retry)
{
	WtRtRunner runner;
	runner._engine = &runner._cta_engine;
	runner._lifecycle_state = WtRtRunner::LifecycleState::RUNNING;
	g_release_on_cta_init_runner = &runner;
	runner.registerCtaCallbacks(&release_from_cta_init_callback,
		nullptr, nullptr, nullptr, nullptr, nullptr);

	runner.ctx_on_init(101, ET_CTA);
	g_release_on_cta_init_runner = nullptr;
	EXPECT_TRUE(WtRtRunnerSinkShutdownTestAccess::teardownPending(runner));
	EXPECT_EQ(WtRtRunner::LifecycleState::STOPPING,
		WtRtRunnerSinkShutdownTestAccess::state(runner));
	EXPECT_EQ(&runner._cta_engine, runner._engine);

	runner.release();
	EXPECT_EQ(WtRtRunner::LifecycleState::RELEASED,
		WtRtRunnerSinkShutdownTestAccess::state(runner));
}
} // namespace
