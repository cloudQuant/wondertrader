#include "q04_trader_fixture.h"

#include "WtUftCore/UftStraContext.h"
#include "WtUftCore/WtUftEngine.h"
#include "WtUftCore/WtUftTicker.h"

#include "Includes/WTSSessionInfo.hpp"
#include "Includes/UftStrategyDefs.h"
#include "Includes/WTSVariant.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <atomic>
#include <climits>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

using namespace wtp;

namespace wtp
{
struct UftTickerShutdownTestAccess
{
	static const uint64_t kQuoteGateClosed = (uint64_t(1) << 63);
	static const uint64_t kQuoteGateCountMask = ~kQuoteGateClosed;

	static bool prime_worker_for_session_close(WtUftEngine& engine)
	{
		WtUftRtTicker* ticker = nullptr;
		{
			std::lock_guard<std::mutex> lock(engine._ticker_mtx);
			ticker = engine._tm_ticker;
		}
		if (ticker == nullptr)
			return false;

		// Called from the synchronous session-begin callback, before run() starts
		// the worker, so the real worker callback can be triggered deterministically.
		std::lock_guard<std::mutex> callback_lock(ticker->_callback_mtx);
		ticker->_time = 100000;
		ticker->_cur_pos = 1;
		ticker->_last_emit_pos.store(0, std::memory_order_release);
		ticker->_next_check_time.store(0, std::memory_order_release);
		return true;
	}

	static bool engine_still_owns_ticker(WtUftEngine& engine)
	{
		std::lock_guard<std::mutex> lock(engine._ticker_mtx);
		return engine._tm_ticker != nullptr;
	}

	static bool startup_claim_owned_by_current_thread(WtUftEngine& engine)
	{
		std::lock_guard<std::mutex> lock(engine._ticker_mtx);
		return engine._run_active && engine._run_thread_id == std::this_thread::get_id();
	}

	static bool wait_until_quote_is_inflight(WtUftEngine& engine, std::chrono::milliseconds timeout)
	{
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + timeout;
		while (std::chrono::steady_clock::now() < deadline)
		{
			if ((engine._quote_gate.load(std::memory_order_acquire) & kQuoteGateCountMask) != 0)
				return true;
			std::this_thread::yield();
		}
		return (engine._quote_gate.load(std::memory_order_acquire) & kQuoteGateCountMask) != 0;
	}

	static bool wait_until_quote_gate_closed(WtUftEngine& engine, std::chrono::milliseconds timeout)
	{
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + timeout;
		while (std::chrono::steady_clock::now() < deadline)
		{
			if ((engine._quote_gate.load(std::memory_order_acquire) & kQuoteGateClosed) != 0)
				return true;
			std::this_thread::yield();
		}
		return (engine._quote_gate.load(std::memory_order_acquire) & kQuoteGateClosed) != 0;
	}

	static uint64_t quote_gate_count(WtUftEngine& engine)
	{
		return engine._quote_gate.load(std::memory_order_acquire) & kQuoteGateCountMask;
	}

	static std::unique_lock<std::mutex> lock_engine_lifecycle(WtUftEngine& engine)
	{
		return std::unique_lock<std::mutex>(engine._ticker_mtx);
	}

	static bool prime_worker_for_offsession_wait(WtUftEngine& engine)
	{
		WtUftRtTicker* ticker = nullptr;
		{
			std::lock_guard<std::mutex> lock(engine._ticker_mtx);
			ticker = engine._tm_ticker;
		}
		if (ticker == nullptr)
			return false;

		std::lock_guard<std::mutex> callback_lock(ticker->_callback_mtx);
		// Exercise a real lunch-break timestamp through isInTradingTime().
		ticker->_time = 120000000;
		return true;
	}

	static bool wait_until_worker_is_waiting(WtUftRtTicker& ticker, std::chrono::milliseconds timeout)
	{
		if (ticker._engine == nullptr)
			return false;
		std::unique_lock<std::mutex> lock(ticker._engine->_ticker_wake_mtx);
		return ticker._engine->_ticker_wake_cv.wait_for(lock, timeout, [&ticker]() {
			return ticker._engine->_ticker_worker_waiting;
		});
	}

	static bool wait_until_engine_worker_is_waiting(WtUftEngine& engine, std::chrono::milliseconds timeout)
	{
		WtUftRtTicker* ticker = nullptr;
		{
			std::lock_guard<std::mutex> lock(engine._ticker_mtx);
			ticker = engine._tm_ticker;
		}
		return ticker != nullptr && wait_until_worker_is_waiting(*ticker, timeout);
	}

	static bool wait_until_engine_worker_exits(WtUftEngine& engine, std::chrono::milliseconds timeout)
	{
		WtUftRtTicker* ticker = nullptr;
		{
			std::lock_guard<std::mutex> lock(engine._ticker_mtx);
			ticker = engine._tm_ticker;
		}
		if (ticker == nullptr)
			return false;

		std::unique_lock<std::mutex> lock(ticker->_state_mtx);
		return ticker->_state_cv.wait_for(lock, timeout, [ticker]() {
			return ticker->_worker_thread_id == std::thread::id();
		});
	}

	static bool request_stop_while_lifecycle_lock_is_held(WtUftEngine& engine)
	{
		std::lock_guard<std::mutex> lock(engine._ticker_mtx);
		engine.request_stop();
		return true;
	}

	static bool notification_before_wait_is_observed(WtUftRtTicker& ticker)
	{
		const uint64_t generation = ticker.capture_wake_generation();
		ticker.notify_worker();
		return ticker.wait_for_worker_wakeup(generation, std::chrono::milliseconds(1));
	}
};
}

namespace {

class StrictSessionBaseDataMgr : public q04::FakeBaseDataMgr
{
public:
	WTSSessionInfo* _session = nullptr;

	WTSSessionInfo* getSession(const char* sid) override
	{
		return _session != nullptr && sid != nullptr && std::strcmp(sid, _session->id()) == 0
			? _session : nullptr;
	}
};

class InitCountingStrategy : public UftStrategy
{
public:
	explicit InitCountingStrategy(const char* id)
		: UftStrategy(id)
	{}

	const char* getName() override { return "session_gate_test"; }
	const char* getFactName() override { return "session_gate_test"; }
	void on_init(IUftStraCtx* /*ctx*/) override { ++_init_count; }

	uint32_t _init_count = 0;
};

WTSVariant* makeEngineConfig(const char* sessionID)
{
	WTSVariant* cfg = WTSVariant::createObject();
	WTSVariant* product = WTSVariant::createObject();
	product->append("session", sessionID);
	cfg->append("product", product);
	product->release();
	return cfg;
}

void addStrategyContext(WtUftEngine& engine, InitCountingStrategy& strategy)
{
	std::shared_ptr<UftStraContext> ctx(new UftStraContext(&engine, strategy.id()));
	ctx->set_strategy(&strategy);
	engine.addContext(ctx);
}

struct ShutdownGate
{
	std::mutex mutex;
	std::condition_variable condition;
	bool session_begin_primed = false;
	bool session_end_called = false;
	bool callback_wait_result = true;
	bool callback_preserved_ticker = false;
};

struct EngineRunGate
{
	std::mutex mutex;
	std::condition_variable condition;
	bool init_entered = false;
	bool release_init = false;
	bool controller_wait_returned = false;
	bool controller_wait_result = false;
	bool callback_wait_result = true;
	std::atomic<uint32_t> init_count{ 0 };
	std::atomic<uint32_t> session_begin_count{ 0 };
	bool startup_claim_observed = false;
	bool second_run_returned_before_init_release = false;
};

struct ReentrantCallbackGate
{
	std::mutex mutex;
	std::condition_variable condition;
	bool quote_returned_from_init = false;
	bool nested_run_returned_from_session_begin = false;
	bool offsession_worker_primed = false;
};

class ReentrantStopStrategy : public UftStrategy
{
public:
	ReentrantStopStrategy(const char* id, ShutdownGate& gate)
		: UftStrategy(id), _engine(nullptr), _gate(gate)
	{}

	void bind_engine(WtUftEngine& engine) { _engine = &engine; }

	const char* getName() override { return "ticker_shutdown_reentry_test"; }
	const char* getFactName() override { return "ticker_shutdown_reentry_test"; }
	void on_init(IUftStraCtx* /*ctx*/) override {}

	void on_session_begin(IUftStraCtx* /*ctx*/, uint32_t /*tdate*/) override
	{
		const bool primed = UftTickerShutdownTestAccess::prime_worker_for_session_close(*_engine);
		std::lock_guard<std::mutex> lock(_gate.mutex);
		_gate.session_begin_primed = primed;
		_gate.condition.notify_all();
	}

	void on_session_end(IUftStraCtx* /*ctx*/, uint32_t /*tdate*/) override
	{
		_engine->request_stop();
		const bool wait_result = _engine->wait_stopped();
		const bool preserved_ticker = UftTickerShutdownTestAccess::engine_still_owns_ticker(*_engine);
		std::lock_guard<std::mutex> lock(_gate.mutex);
		_gate.callback_wait_result = wait_result;
		_gate.callback_preserved_ticker = preserved_ticker;
		_gate.session_end_called = true;
		_gate.condition.notify_all();
	}

private:
	WtUftEngine* _engine;
	ShutdownGate& _gate;
};

class GatedRunStrategy : public UftStrategy
{
public:
	GatedRunStrategy(const char* id, EngineRunGate& gate, bool stop_in_init)
		: UftStrategy(id), _engine(nullptr), _gate(gate), _stop_in_init(stop_in_init)
	{}

	void bind_engine(WtUftEngine& engine) { _engine = &engine; }

	const char* getName() override { return "gated_engine_run_test"; }
	const char* getFactName() override { return "gated_engine_run_test"; }

	void on_init(IUftStraCtx* /*ctx*/) override
	{
		++_gate.init_count;
		const bool startup_claimed = UftTickerShutdownTestAccess::startup_claim_owned_by_current_thread(*_engine);
		{
			std::unique_lock<std::mutex> lock(_gate.mutex);
			_gate.startup_claim_observed = startup_claimed;
			_gate.init_entered = true;
			_gate.condition.notify_all();
			_gate.condition.wait(lock, [this]() { return _gate.release_init; });
		}

		if (_stop_in_init)
		{
			_engine->request_stop();
			const bool wait_result = _engine->wait_stopped();
			std::lock_guard<std::mutex> lock(_gate.mutex);
			_gate.callback_wait_result = wait_result;
			_gate.condition.notify_all();
		}
	}

	void on_session_begin(IUftStraCtx* /*ctx*/, uint32_t /*tdate*/) override
	{
		++_gate.session_begin_count;
	}

private:
	WtUftEngine* _engine;
	EngineRunGate& _gate;
	bool _stop_in_init;
};

class ReentrantEngineStrategy : public UftStrategy
{
public:
	ReentrantEngineStrategy(const char* id, ReentrantCallbackGate& gate)
		: UftStrategy(id), _engine(nullptr), _gate(gate)
	{}

	void bind_engine(WtUftEngine& engine) { _engine = &engine; }
	const char* getName() override { return "engine_lifecycle_reentry_test"; }
	const char* getFactName() override { return "engine_lifecycle_reentry_test"; }

	void on_init(IUftStraCtx* /*ctx*/) override
	{
		_engine->handle_push_quote(nullptr);
		std::lock_guard<std::mutex> lock(_gate.mutex);
		_gate.quote_returned_from_init = true;
		_gate.condition.notify_all();
	}

	void on_session_begin(IUftStraCtx* /*ctx*/, uint32_t /*tdate*/) override
	{
		const bool primed = UftTickerShutdownTestAccess::prime_worker_for_offsession_wait(*_engine);
		_engine->run();
		std::lock_guard<std::mutex> lock(_gate.mutex);
		_gate.offsession_worker_primed = primed;
		_gate.nested_run_returned_from_session_begin = true;
		_gate.condition.notify_all();
	}

private:
	WtUftEngine* _engine;
	ReentrantCallbackGate& _gate;
};

} // namespace

TEST(UftEngineSessionGate, unknownSessionSkipsStrategyInitialization)
{
	StrictSessionBaseDataMgr bd;
	InitCountingStrategy strategy("unknown_session_strategy");
	WtUftEngine engine;
	WTSVariant* cfg = makeEngineConfig("UFT_SESSION_DOES_NOT_EXIST");
	engine.init(cfg, &bd, nullptr, nullptr);
	cfg->release();
	addStrategyContext(engine, strategy);

	engine.run();

	EXPECT_EQ(0u, strategy._init_count);
}

TEST(UftEngineSessionGate, knownSessionPreservesStrategyInitialization)
{
	StrictSessionBaseDataMgr bd;
	WTSSessionInfo* session = WTSSessionInfo::create("UFT_SESSION_VALID", "session gate test", 0);
	ASSERT_NE(nullptr, session);
	// 卡2 2b=A（用户裁定 2026-11-03）：全天时段只能写 0-0，0-2400 被拒（会话无时段）；R12-13 后 ticker 对无法映射的交易日 fail-closed（迭代03 第五会话）。
	session->addTradingSection(0, 0);
	bd._session = session;

	InitCountingStrategy strategy("known_session_strategy");
	{
		WtUftEngine engine;
		WTSVariant* cfg = makeEngineConfig("UFT_SESSION_VALID");
		engine.init(cfg, &bd, nullptr, nullptr);
		cfg->release();
		addStrategyContext(engine, strategy);

		engine.run();

		EXPECT_EQ(1u, strategy._init_count);
	}
	session->release();
}

TEST(UftTickerLifecycle, repeatedStopCallsJoinWorkerOnce)
{
	StrictSessionBaseDataMgr bd;
	WTSSessionInfo* session = WTSSessionInfo::create("UFT_TICKER_STOP", "ticker stop test", 0);
	ASSERT_NE(nullptr, session);
	// 卡2 2b=A（用户裁定 2026-11-03）：全天时段只能写 0-0，0-2400 被拒（会话无时段）；R12-13 后 ticker 对无法映射的交易日 fail-closed（迭代03 第五会话）。
	session->addTradingSection(0, 0);
	bd._session = session;

	{
		WtUftEngine engine;
		engine.init(nullptr, &bd, nullptr, nullptr);
		WtUftRtTicker ticker(&engine);
		ticker.init("UFT_TICKER_STOP");
		ticker.run();

		ticker.stop();
		EXPECT_NO_THROW(ticker.stop());
	}

	session->release();
}

TEST(UftTickerLifecycle, notificationBeforeWaitIsNotLost)
{
	WtUftEngine engine;
	WtUftRtTicker ticker(&engine);

	EXPECT_TRUE(UftTickerShutdownTestAccess::notification_before_wait_is_observed(ticker));
}

TEST(UftTickerLifecycle, callbackRequestsStopAndControllerJoinsWorker)
{
	StrictSessionBaseDataMgr bd;
	WTSSessionInfo* session = WTSSessionInfo::create("UFT_TICKER_REENTRANT_STOP", "ticker reentrant stop", 0);
	ASSERT_NE(nullptr, session);
	session->addTradingSection(1, 2);
	bd._session = session;

	ShutdownGate gate;
	ReentrantStopStrategy strategy("reentrant_stop_strategy", gate);
	{
		WtUftEngine engine;
		strategy.bind_engine(engine);
		WTSVariant* cfg = makeEngineConfig("UFT_TICKER_REENTRANT_STOP");
		engine.init(cfg, &bd, nullptr, nullptr);
		cfg->release();
		std::shared_ptr<UftStraContext> ctx(new UftStraContext(&engine, strategy.id()));
		ctx->set_strategy(&strategy);
		engine.addContext(ctx);

		engine.run();
		{
			std::unique_lock<std::mutex> lock(gate.mutex);
			EXPECT_TRUE(gate.condition.wait_for(lock, std::chrono::seconds(5), [&gate]() {
				return gate.session_begin_primed && gate.session_end_called;
			}));
			EXPECT_FALSE(gate.callback_wait_result);
			EXPECT_TRUE(gate.callback_preserved_ticker);
		}

		EXPECT_TRUE(engine.wait_stopped());
		EXPECT_TRUE(engine.wait_stopped());
		engine.stop();
	}

	session->release();
}

TEST(UftEngineLifecycle, stopDrainsGatedInitializationAndCallbackWaitDoesNotSelfWait)
{
	StrictSessionBaseDataMgr bd;
	WTSSessionInfo* session = WTSSessionInfo::create("UFT_ENGINE_STOP_DURING_INIT", "engine init stop", 0);
	ASSERT_NE(nullptr, session);
	// 卡2 2b=A（用户裁定 2026-11-03）：全天时段只能写 0-0，0-2400 被拒（会话无时段）；R12-13 后 ticker 对无法映射的交易日 fail-closed（迭代03 第五会话）。
	session->addTradingSection(0, 0);
	bd._session = session;

	EngineRunGate gate;
	GatedRunStrategy strategy("gated_init_stop_strategy", gate, true);
	{
		WtUftEngine engine;
		strategy.bind_engine(engine);
		WTSVariant* cfg = makeEngineConfig("UFT_ENGINE_STOP_DURING_INIT");
		engine.init(cfg, &bd, nullptr, nullptr);
		cfg->release();
		std::shared_ptr<UftStraContext> ctx(new UftStraContext(&engine, strategy.id()));
		ctx->set_strategy(&strategy);
		engine.addContext(ctx);

		std::thread runner([&engine]() { engine.run(); });
		bool init_entered = false;
		{
			std::unique_lock<std::mutex> lock(gate.mutex);
			init_entered = gate.condition.wait_for(lock, std::chrono::seconds(5), [&gate]() {
				return gate.init_entered;
			});
		}
		EXPECT_TRUE(init_entered);
		EXPECT_TRUE(gate.startup_claim_observed);
		std::atomic<bool> quote_returned(false);
		std::thread quote([&engine, &quote_returned]() {
			engine.handle_push_quote(nullptr);
			quote_returned.store(true, std::memory_order_release);
		});
		EXPECT_TRUE(UftTickerShutdownTestAccess::wait_until_quote_is_inflight(
			engine, std::chrono::seconds(5)));
		engine.request_stop();

		std::thread controller([&engine, &gate]() {
			const bool stopped = engine.wait_stopped();
			std::lock_guard<std::mutex> lock(gate.mutex);
			gate.controller_wait_result = stopped;
			gate.controller_wait_returned = true;
			gate.condition.notify_all();
		});

		bool returned_before_init = false;
		{
			std::unique_lock<std::mutex> lock(gate.mutex);
			returned_before_init = gate.condition.wait_for(lock, std::chrono::milliseconds(100), [&gate]() {
				return gate.controller_wait_returned;
			});
			gate.release_init = true;
			gate.condition.notify_all();
		}
		EXPECT_FALSE(returned_before_init);

		runner.join();
		quote.join();
		controller.join();
		EXPECT_TRUE(quote_returned.load(std::memory_order_acquire));
		EXPECT_TRUE(gate.controller_wait_returned);
		EXPECT_TRUE(gate.controller_wait_result);
		EXPECT_FALSE(gate.callback_wait_result);
		EXPECT_EQ(1u, gate.init_count.load());
		EXPECT_EQ(0u, gate.session_begin_count.load());
		EXPECT_TRUE(engine.wait_stopped());
	}

	session->release();
}

TEST(UftEngineLifecycle, concurrentRunCallsInitializeStrategiesOnce)
{
	StrictSessionBaseDataMgr bd;
	WTSSessionInfo* session = WTSSessionInfo::create("UFT_ENGINE_CONCURRENT_RUN", "engine concurrent run", 0);
	ASSERT_NE(nullptr, session);
	// 卡2 2b=A（用户裁定 2026-11-03）：全天时段只能写 0-0，0-2400 被拒（会话无时段）；R12-13 后 ticker 对无法映射的交易日 fail-closed（迭代03 第五会话）。
	session->addTradingSection(0, 0);
	bd._session = session;

	EngineRunGate gate;
	GatedRunStrategy strategy("gated_concurrent_run_strategy", gate, false);
	{
		WtUftEngine engine;
		strategy.bind_engine(engine);
		WTSVariant* cfg = makeEngineConfig("UFT_ENGINE_CONCURRENT_RUN");
		engine.init(cfg, &bd, nullptr, nullptr);
		cfg->release();
		std::shared_ptr<UftStraContext> ctx(new UftStraContext(&engine, strategy.id()));
		ctx->set_strategy(&strategy);
		engine.addContext(ctx);

		std::thread first_run([&engine]() { engine.run(); });
		{
			std::unique_lock<std::mutex> lock(gate.mutex);
			EXPECT_TRUE(gate.condition.wait_for(lock, std::chrono::seconds(5), [&gate]() {
				return gate.init_entered;
			}));
		}

		std::thread second_run([&engine, &gate]() {
			engine.run();
			std::lock_guard<std::mutex> lock(gate.mutex);
			gate.second_run_returned_before_init_release = !gate.release_init;
			gate.condition.notify_all();
		});
		{
			std::unique_lock<std::mutex> lock(gate.mutex);
			EXPECT_TRUE(gate.condition.wait_for(lock, std::chrono::seconds(5), [&gate]() {
				return gate.second_run_returned_before_init_release;
			}));
			gate.release_init = true;
			gate.condition.notify_all();
		}

		first_run.join();
		second_run.join();
		EXPECT_EQ(1u, gate.init_count.load());
		EXPECT_EQ(1u, gate.session_begin_count.load());
		EXPECT_TRUE(gate.startup_claim_observed);
		EXPECT_TRUE(gate.second_run_returned_before_init_release);
		engine.request_stop();
		EXPECT_TRUE(engine.wait_stopped());
	}

	session->release();
}

TEST(UftEngineLifecycle, startupAndSessionCallbacksCanReenterEngineAndStopWakesWorker)
{
	StrictSessionBaseDataMgr bd;
	WTSSessionInfo* session = WTSSessionInfo::create("UFT_ENGINE_REENTRANT_CALLBACKS", "engine reentry", 0);
	ASSERT_NE(nullptr, session);
	session->addTradingSection(900, 1130);
	session->addTradingSection(1300, 1500);
	bd._session = session;

	ReentrantCallbackGate gate;
	ReentrantEngineStrategy strategy("reentrant_engine_callbacks_strategy", gate);
	{
		WtUftEngine engine;
		strategy.bind_engine(engine);
		WTSVariant* cfg = makeEngineConfig("UFT_ENGINE_REENTRANT_CALLBACKS");
		engine.init(cfg, &bd, nullptr, nullptr);
		cfg->release();
		std::shared_ptr<UftStraContext> ctx(new UftStraContext(&engine, strategy.id()));
		ctx->set_strategy(&strategy);
		engine.addContext(ctx);

		engine.run();
		{
			std::lock_guard<std::mutex> lock(gate.mutex);
			EXPECT_TRUE(gate.quote_returned_from_init);
			EXPECT_TRUE(gate.nested_run_returned_from_session_begin);
			EXPECT_TRUE(gate.offsession_worker_primed);
		}

		ASSERT_TRUE(UftTickerShutdownTestAccess::wait_until_engine_worker_is_waiting(
			engine, std::chrono::seconds(5)));
		const std::chrono::steady_clock::time_point stop_requested_at = std::chrono::steady_clock::now();
		EXPECT_TRUE(UftTickerShutdownTestAccess::request_stop_while_lifecycle_lock_is_held(engine));
		const bool worker_exited = UftTickerShutdownTestAccess::wait_until_engine_worker_exits(
			engine, std::chrono::seconds(5));
		const std::chrono::steady_clock::duration stop_elapsed =
			std::chrono::steady_clock::now() - stop_requested_at;
		EXPECT_TRUE(worker_exited);
		EXPECT_LT(stop_elapsed, std::chrono::seconds(5));
		EXPECT_TRUE(engine.wait_stopped());
	}

	session->release();
}

TEST(UftEngineLifecycle, stopBeforeRunSkipsStrategyInitialization)
{
	StrictSessionBaseDataMgr bd;
	WTSSessionInfo* session = WTSSessionInfo::create("UFT_ENGINE_STOP_BEFORE_RUN", "engine stop before run", 0);
	ASSERT_NE(nullptr, session);
	// 卡2 2b=A（用户裁定 2026-11-03）：全天时段只能写 0-0，0-2400 被拒（会话无时段）；R12-13 后 ticker 对无法映射的交易日 fail-closed（迭代03 第五会话）。
	session->addTradingSection(0, 0);
	bd._session = session;

	InitCountingStrategy strategy("stop_before_run_strategy");
	{
		WtUftEngine engine;
		WTSVariant* cfg = makeEngineConfig("UFT_ENGINE_STOP_BEFORE_RUN");
		engine.init(cfg, &bd, nullptr, nullptr);
		cfg->release();
		addStrategyContext(engine, strategy);

		engine.request_stop();
		engine.run();
		EXPECT_EQ(0u, strategy._init_count);
		EXPECT_TRUE(engine.wait_stopped());
	}

	session->release();
}

TEST(UftEngineLifecycle, waitClosesQuoteAdmissionBeforeCheckingForZero)
{
	WtUftEngine engine;
	// Keep wait_stopped out of its zero-count check after it closes quote admission.
	std::unique_lock<std::mutex> lifecycle_lock = UftTickerShutdownTestAccess::lock_engine_lifecycle(engine);
	std::atomic<bool> wait_returned(false);
	std::atomic<bool> wait_result(false);
	std::thread waiter([&engine, &wait_returned, &wait_result]() {
		wait_result.store(engine.wait_stopped(), std::memory_order_release);
		wait_returned.store(true, std::memory_order_release);
	});

	const bool gate_closed_while_waiter_blocked = UftTickerShutdownTestAccess::wait_until_quote_gate_closed(
		engine, std::chrono::seconds(5));
	std::mutex quote_result_mutex;
	std::condition_variable quote_result_cv;
	bool quote_returned = false;
	std::thread quote([&engine, &quote_result_mutex, &quote_result_cv, &quote_returned]() {
		engine.handle_push_quote(nullptr);
		{
			std::lock_guard<std::mutex> lock(quote_result_mutex);
			quote_returned = true;
		}
		quote_result_cv.notify_all();
	});

	bool quote_returned_before_lifecycle_unlock = false;
	{
		std::unique_lock<std::mutex> lock(quote_result_mutex);
		quote_returned_before_lifecycle_unlock = quote_result_cv.wait_for(
			lock, std::chrono::milliseconds(250), [&quote_returned]() { return quote_returned; });
	}
	lifecycle_lock.unlock();
	quote.join();
	waiter.join();

	EXPECT_TRUE(gate_closed_while_waiter_blocked);
	EXPECT_TRUE(quote_returned_before_lifecycle_unlock);
	EXPECT_TRUE(wait_returned.load(std::memory_order_acquire));
	EXPECT_TRUE(wait_result.load(std::memory_order_acquire));
	EXPECT_TRUE(UftTickerShutdownTestAccess::wait_until_quote_gate_closed(engine, std::chrono::milliseconds(0)));
	engine.handle_push_quote(nullptr);
	EXPECT_EQ(0u, UftTickerShutdownTestAccess::quote_gate_count(engine));
}
