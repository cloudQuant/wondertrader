/**
 * s1_k25_sel_isolation_test.cpp - BUG-R10-004, decision K25=A
 * (iteration 03, slice1 final gaps)
 *
 * K25=A: SEL schedules run synchronously on the ticker strand that also
 * delivers ticks, so a context's on_schedule and on_tick never overlap and
 * re-entrant ticker events are queued to the safe point. The original R10-004
 * acceptance also requires:
 *   - schedule/tick exceptions are isolated per strategy and reported; no
 *     std::terminate and the other strategies keep running;
 *   - A <-> B synchronous re-entry does not deadlock and every queued event is
 *     delivered exactly once, in order;
 *   - stop from inside a strategy callback does not self-join and the ticker
 *     can still be stopped/destroyed afterwards.
 * Only local fakes are used; no provider or network.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "q01_sel_env.h"

#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSDataDef.hpp>
#include <Share/TimeUtils.hpp>
#include <WtCore/WtDtMgr.h>
#include <WtCore/WtHelper.h>
#include <WtCore/WtSelEngine.h>
#include <WtCore/WtSelTicker.h>

using namespace wtp;

namespace
{
const char* kS1Code = "SHFE.ag.2512";
const char* kS1OtherCode = "SHFE.au.2512";

class S1K25Engine final : public WtSelEngine
{
public:
	void attach(IBaseDataMgr* bd, IHotMgr* hot, WtDtMgr* dm)
	{
		_base_data_mgr = bd;
		_hot_mgr = hot;
		_data_mgr = dm;
		set_date_time(20250110, 930, 0);
		set_trading_date(20250110);
	}
	void setReady() { _ready = true; }
};

/// Records its callbacks; optionally throws from the schedule or tick handler
/// and runs a hook inside the tick handler (used for re-entry and stop).
class S1K25Ctx final : public q01sel::SelCtx
{
public:
	S1K25Ctx(WtSelEngine* engine, const char* name) : q01sel::SelCtx(engine, name) {}

	void on_strategy_schedule(uint32_t curDate, uint32_t curTime) override
	{
		++schedule_calls;
		if (throw_std_on_schedule)
			throw std::runtime_error("s1 schedule failure");
		if (throw_other_on_schedule)
			throw 42;
		q01sel::SelCtx::on_strategy_schedule(curDate, curTime);
	}

	void on_tick_updated(const char* stdCode, WTSTickData* newTick) override
	{
		++tick_calls;
		order.push_back(name_tag + ":" + stdCode);
		if (throw_on_tick)
			throw std::runtime_error("s1 tick failure");
		if (on_tick_hook)
			on_tick_hook(stdCode);
		q01sel::SelCtx::on_tick_updated(stdCode, newTick);
	}

	std::string name_tag;
	bool throw_std_on_schedule = false;
	bool throw_other_on_schedule = false;
	bool throw_on_tick = false;
	std::function<void(const char*)> on_tick_hook;
	uint32_t schedule_calls = 0;
	uint32_t tick_calls = 0;
	std::vector<std::string> order;
};

struct S1K25Market
{
	S1K25Market()
	{
		bd.regisSessionWithTime("TRADING", 0, 0);
		WTSCommodityInfo* ag = q01sel::makeComm("SHFE", "ag", "TRADING", TM_Both);
		ag->setSessionInfo(bd.session("TRADING"));
		bd.regis(ag, q01sel::makeCt("SHFE", "ag", "ag2512"));
		WTSCommodityInfo* au = q01sel::makeComm("SHFE", "au", "TRADING", TM_Both);
		au->setSessionInfo(bd.session("TRADING"));
		bd.regis(au, q01sel::makeCt("SHFE", "au", "au2512"));
	}

	// Ticks reach the ticker with standard codes (the parser adapter normalizes them).
	WTSTickData* tick(const char* code, uint32_t date, uint32_t time, double price)
	{
		const std::string stdCode = std::string("SHFE.") + std::string(code, 2) + "." + std::string(code + 2);
		WTSTickData* t = q01sel::makeTick("SHFE", stdCode.c_str(), date, date, time, price);
		t->setContractInfo(bd.getContract(code, "SHFE"));
		return t;
	}

	q01sel::BaseDataMgr bd;
	q01sel::HotMgr hot;
	WtDtMgr dm;
};

template <typename Pred>
bool s1Eventually(Pred pred, std::chrono::milliseconds limit)
{
	const auto deadline = std::chrono::steady_clock::now() + limit;
	while (!pred())
	{
		if (std::chrono::steady_clock::now() >= deadline)
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return true;
}
}	// namespace

// A throwing schedule (std and non-std exception) is contained by the engine;
// the other strategy's task still fires once, and nothing re-fires.
TEST(S1K25SelIsolation, ThrowingScheduleIsIsolatedAndOtherStrategiesStillRun)
{
	q01sel::Q01TempEnv tmp("s1_k25_sched");
	S1K25Market mkt;
	std::unique_ptr<S1K25Engine> engine(new S1K25Engine());
	engine->attach(&mkt.bd, &mkt.hot, &mkt.dm);

	std::shared_ptr<S1K25Ctx> a(new S1K25Ctx(engine.get(), "s1_k25_a"));
	std::shared_ptr<S1K25Ctx> b(new S1K25Ctx(engine.get(), "s1_k25_b"));
	std::shared_ptr<S1K25Ctx> c(new S1K25Ctx(engine.get(), "s1_k25_c"));
	a->throw_std_on_schedule = true;
	c->throw_other_on_schedule = true;
	engine->addContext(a, 20250110, 931, TPT_Daily);
	engine->addContext(b, 20250110, 931, TPT_Daily);
	engine->addContext(c, 20250110, 931, TPT_Daily);

	EXPECT_NO_THROW(engine->on_minute_end(20250110, 930));
	EXPECT_EQ(1u, a->schedule_calls);
	EXPECT_EQ(1u, b->schedule_calls);
	EXPECT_EQ(1u, c->schedule_calls);
	EXPECT_EQ(1u, b->schedule_fires) << "the healthy strategy completed its schedule";
	EXPECT_EQ(0u, a->schedule_fires);

	// Same minute again: every task (including the failed ones) fired at most once.
	EXPECT_NO_THROW(engine->on_minute_end(20250110, 930));
	EXPECT_EQ(1u, a->schedule_calls);
	EXPECT_EQ(1u, b->schedule_calls);
	EXPECT_EQ(1u, c->schedule_calls);
	engine->release();
}

// A throwing tick handler is contained per strategy: the exception does not
// leave the engine and the other subscribed strategy still receives the tick.
TEST(S1K25SelIsolation, ThrowingTickHandlerIsIsolatedAndOtherStrategiesStillReceiveTicks)
{
	q01sel::Q01TempEnv tmp("s1_k25_tick");
	S1K25Market mkt;
	std::unique_ptr<S1K25Engine> engine(new S1K25Engine());
	engine->attach(&mkt.bd, &mkt.hot, &mkt.dm);

	std::shared_ptr<S1K25Ctx> a(new S1K25Ctx(engine.get(), "s1_k25_tick_a"));
	std::shared_ptr<S1K25Ctx> b(new S1K25Ctx(engine.get(), "s1_k25_tick_b"));
	a->throw_on_tick = true;
	engine->addContext(a, 20250110, 1500, TPT_Daily);
	engine->addContext(b, 20250110, 1500, TPT_Daily);
	a->stra_sub_ticks(kS1Code);
	b->stra_sub_ticks(kS1Code);
	engine->setReady();

	WTSTickData* t1 = mkt.tick("ag2512", 20250110, 93000000, 3500.0);
	EXPECT_NO_THROW(engine->on_tick(kS1Code, t1));
	t1->release();
	EXPECT_EQ(1u, a->tick_calls);
	EXPECT_EQ(1u, b->tick_calls) << "the healthy strategy still receives the tick";

	WTSTickData* t2 = mkt.tick("ag2512", 20250110, 93001000, 3501.0);
	EXPECT_NO_THROW(engine->on_tick(kS1Code, t2));
	t2->release();
	EXPECT_EQ(2u, a->tick_calls) << "the failing strategy is not unsubscribed";
	EXPECT_EQ(2u, b->tick_calls);
	engine->release();
}

// A <-> B synchronous re-entry through the SEL ticker strand: A's tick handler
// feeds B's contract back into the ticker and B's handler feeds A's contract
// once more. Each re-entrant tick is queued (not run in place), delivered once
// after the current event returns, in FIFO order, and nothing deadlocks.
TEST(S1K25SelIsolation, ReentrantTicksBetweenTwoStrategiesAreQueuedAndDeliveredOnce)
{
	q01sel::Q01TempEnv tmp("s1_k25_reentry");
	S1K25Market mkt;
	std::unique_ptr<S1K25Engine> engine(new S1K25Engine());
	engine->attach(&mkt.bd, &mkt.hot, &mkt.dm);
	std::unique_ptr<WtSelRtTicker> ticker(new WtSelRtTicker(engine.get()));
	ticker->init(nullptr, "TRADING");	// not running: every tick goes straight to the engine

	std::vector<std::string> order;
	std::atomic<int> depth{ 0 };
	bool nestedInA = false;
	bool nestedInB = false;
	std::shared_ptr<S1K25Ctx> a(new S1K25Ctx(engine.get(), "s1_k25_re_a"));
	std::shared_ptr<S1K25Ctx> b(new S1K25Ctx(engine.get(), "s1_k25_re_b"));
	a->name_tag = "A";
	b->name_tag = "B";
	engine->addContext(a, 20250110, 1500, TPT_Daily);
	engine->addContext(b, 20250110, 1500, TPT_Daily);
	a->stra_sub_ticks(kS1Code);
	b->stra_sub_ticks(kS1OtherCode);
	engine->setReady();

	uint32_t aSeen = 0;
	a->on_tick_hook = [&](const char*) {
		if (depth.fetch_add(1) != 0)
			nestedInA = true;
		order.push_back("A-enter");
		if (++aSeen == 1)
		{
			WTSTickData* toB = mkt.tick("au2512", 20250110, 93000500, 450.0);
			ticker->on_tick(toB);	// re-entrant: must be queued, not run here
			toB->release();
		}
		order.push_back("A-exit");
		depth.fetch_sub(1);
	};
	uint32_t bSeen = 0;
	b->on_tick_hook = [&](const char*) {
		if (depth.fetch_add(1) != 0)
			nestedInB = true;
		order.push_back("B-enter");
		if (++bSeen == 1)
		{
			WTSTickData* backToA = mkt.tick("ag2512", 20250110, 93000600, 3502.0);
			ticker->on_tick(backToA);	// B -> A re-entry, queued behind B
			backToA->release();
		}
		order.push_back("B-exit");
		depth.fetch_sub(1);
	};

	std::atomic<bool> finished{ false };
	std::thread feeder([&]() {
		WTSTickData* first = mkt.tick("ag2512", 20250110, 93000000, 3500.0);
		ticker->on_tick(first);
		first->release();
		finished.store(true);
	});
	const bool returned = s1Eventually([&] { return finished.load(); }, std::chrono::milliseconds(5000));
	if (!returned)
	{
		feeder.detach();	// deadlocked: leak the fixture rather than destroy it under the thread
		engine.release();
		ticker.release();
		FAIL() << "A <-> B re-entry deadlocked";
	}
	feeder.join();

	EXPECT_FALSE(nestedInA) << "a re-entrant tick ran inside A's handler";
	EXPECT_FALSE(nestedInB) << "a re-entrant tick ran inside B's handler";
	EXPECT_EQ(2u, a->tick_calls);
	EXPECT_EQ(1u, b->tick_calls);
	const std::vector<std::string> expected = { "A-enter", "A-exit", "B-enter", "B-exit", "A-enter", "A-exit" };
	EXPECT_EQ(expected, order);

	ticker->stop();
	ticker.reset();
	engine->release();
}

// stop() from inside a strategy callback (on the strand owner) only requests the
// stop: it neither self-joins nor waits for its own event; the ticker is then
// stopped and destroyed by the owner within a bound.
TEST(S1K25SelIsolation, StopFromInsideAStrategyCallbackDoesNotSelfJoin)
{
	q01sel::Q01TempEnv tmp("s1_k25_stop");
	S1K25Market mkt;
	std::unique_ptr<S1K25Engine> engine(new S1K25Engine());
	engine->attach(&mkt.bd, &mkt.hot, &mkt.dm);
	const uint32_t today = TimeUtils::getCurDate();
	engine->set_date_time(today, 930, 0);
	engine->set_trading_date(today);

	WtSelRtTicker* ticker = new WtSelRtTicker(engine.get());
	ticker->init(nullptr, "TRADING");
	std::shared_ptr<S1K25Ctx> a(new S1K25Ctx(engine.get(), "s1_k25_stop_a"));
	engine->addContext(a, today, 1500, TPT_Daily);
	a->stra_sub_ticks(kS1Code);
	ASSERT_TRUE(ticker->run());	// worker thread running (engine on_init -> _ready)
	engine->setReady();

	std::atomic<bool> stopReturned{ false };
	a->on_tick_hook = [&](const char*) {
		ticker->stop();	// on the strand owner: must not join itself
		stopReturned.store(true);
	};

	std::atomic<bool> tickDone{ false };
	std::thread feeder([&]() {
		const uint32_t tickDate = TimeUtils::getNextDate(today, 1);
		WTSTickData* t = mkt.tick("ag2512", tickDate, 93000000, 3500.0);
		ticker->on_tick(t);
		t->release();
		tickDone.store(true);
	});
	const bool returned = s1Eventually([&] { return tickDone.load(); }, std::chrono::milliseconds(8000));
	if (!returned)
	{
		feeder.detach();
		engine.release();
		FAIL() << "stop() inside the strategy callback blocked the strand";
	}
	feeder.join();
	EXPECT_TRUE(stopReturned.load());
	EXPECT_EQ(1u, a->tick_calls);

	std::atomic<bool> destroyed{ false };
	std::thread destroyer([&]() {
		delete ticker;	// destructor stops (idempotent) and joins the worker
		destroyed.store(true);
	});
	const bool joined = s1Eventually([&] { return destroyed.load(); }, std::chrono::milliseconds(8000));
	if (!joined)
	{
		destroyer.detach();
		engine.release();
		FAIL() << "ticker destruction did not complete after an in-callback stop";
	}
	destroyer.join();
	engine->release();
}
