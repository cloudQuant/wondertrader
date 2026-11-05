/*!
 * \file r123_uft_ticker_engine_test.cpp
 * \brief 第二十波 R123 - WtUftRtTicker 自动闭合分钟 + WtUftEngine 产品时段/推送行情
 *
 * 覆盖（只读生产代码）:
 *  - WtUftTicker.cpp:202-240：首笔 tick 送入引擎并启动分钟调度，工作线程到点自动触发
 *    on_minute_end、跨零点判定、
 *    交易日收盘判定和 set_date_time。用 action_time=HHMM:59.999 让 left_ticks=1ms，
 *    线程下一轮（10ms 休眠后）必然进入自动闭合；时段取 0-2400，闭合后仍在交易时段，
 *    stop() 无需等待 10s 休息分支。
 *  - WtUftEngine.cpp:338-342：_cfg->product.session 存在时用产品时段初始化 ticker。
 *  - WtUftEngine.cpp:358-362：handle_push_quote 把 tick 转发给 ticker。
 *  - WtUftTicker.cpp:118-148：第二笔 tick 推进分钟后 engine.on_tick/set_date_time/set_trading_date。
 *
 * 线程约束：只起 WtUftRtTicker 自己的 1 个线程；断言后立即 stop()/join，
 * 单轮等待上限 500ms，不接网络/柜台/共享内存。
 */
#include "r123_uft_common.h"

#include "WtUftCore/WtUftDtMgr.h"
#include "WtUftCore/WtUftEngine.h"
#include "WtUftCore/WtUftTicker.h"
#include "WtUftCore/UftStraContext.h"

#include "Includes/WTSVariant.hpp"
#include "Includes/UftStrategyDefs.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namespace wtp;

namespace wtp
{
struct UftFirstTickAutoCloseTestAccess
{
	static void get_state(WtUftRtTicker& ticker, uint32_t& date, uint32_t& time, uint32_t& curPos)
	{
		std::lock_guard<std::mutex> lock(ticker._callback_mtx);
		date = ticker._date;
		time = ticker._time;
		curPos = ticker._cur_pos;
	}

	static void set_clock(WtUftRtTicker& ticker, uint32_t date, uint32_t time)
	{
		std::lock_guard<std::mutex> lock(ticker._callback_mtx);
		ticker._date = date;
		ticker._time = time;
	}

	static bool auto_close_finished(WtUftRtTicker& ticker)
	{
		// The worker holds this lock through set_date_time. Acquiring it here makes
		// subsequent engine time reads observe the completed auto-close update.
		std::lock_guard<std::mutex> lock(ticker._callback_mtx);
		return ticker._cur_pos != 0
			&& ticker._last_emit_pos.load(std::memory_order_acquire) >= ticker._cur_pos;
	}
};
}

namespace {

const char* kAg = "SHFE.ag2406";

struct R123SessionGuard
{
	WTSSessionInfo* _session;
	~R123SessionGuard() { if (_session != nullptr) _session->release(); }
};

class FirstQuoteClockStrategy : public UftStrategy
{
public:
	explicit FirstQuoteClockStrategy(WtUftEngine* engine)
		: UftStrategy("r123-first-quote-clock"), _engine(engine)
	{
	}

	const char* getName() override { return "r123-first-quote-clock"; }
	const char* getFactName() override { return "r123-test"; }
	void on_init(IUftStraCtx* /*ctx*/) override {}
	void on_tick(IUftStraCtx* ctx, const char* /*stdCode*/, WTSTickData* tick) override
	{
		++_tick_count;
		_context_date = ctx->stra_get_date();
		_context_time = ctx->stra_get_time();
		_context_secs = ctx->stra_get_secs();
		_engine_trading_date = _engine->get_trading_date();
		_tick_action_time = tick->actiontime();
	}

	WtUftEngine* _engine;
	uint32_t _tick_count = 0;
	uint32_t _context_date = 0;
	uint32_t _context_time = 0;
	uint32_t _context_secs = 0;
	uint32_t _engine_trading_date = 0;
	uint32_t _tick_action_time = 0;
};

TEST(R123UftTicker, firstSameDayQuoteBeforeStartupClockSetsTimeBeforeStrategy)
{
	r123::SessionBaseDataMgr bd;
	WTSSessionInfo* session = WTSSessionInfo::create("R123FIRST", "r123 first quote", 0);
	ASSERT_NE(nullptr, session);
	R123SessionGuard sessionGuard{ session };
	// 卡2 2b=A（用户裁定 2026-11-03）：全天时段只能写 0-0，0-2400 被拒（会话无时段）；迭代03 第五会话 int2 归因更新。
	session->addTradingSection(0, 0);
	bd._session = session;
	ASSERT_NE(nullptr, bd.add("SHFE", "ag", "ag2406", 0, 0,
		CM_OpenCover, TM_Both, 10));

	{
		WtUftDtMgr dtMgr;
		WtUftEngine engine;
		engine.init(nullptr, &bd, &dtMgr, nullptr);
		engine.set_date_time(20000101, 500, 7, 499);
		engine.set_trading_date(20000102);

		FirstQuoteClockStrategy strategy(&engine);
		auto context = std::make_shared<UftStraContext>(&engine, "r123-first-quote-clock-context");
		context->set_strategy(&strategy);
		context->stra_sub_ticks(kAg);
		engine.addContext(context);

		WtUftRtTicker ticker(&engine);
		ticker.init("R123FIRST");
		// Inject an exact startup clock so this exercises the same-day-late first
		// quote deterministically without waiting for or depending on wall-clock time.
		UftFirstTickAutoCloseTestAccess::set_clock(ticker, 20991231, 100000000);
		ticker.run();

		WTSTickData* firstTick = r123::makeTick(20991231, 95900000, 10.0);
		ASSERT_NE(nullptr, firstTick);
		ticker.on_tick(firstTick);
		firstTick->release();

		ASSERT_EQ(1u, strategy._tick_count);
		EXPECT_EQ(20991231u, strategy._context_date);
		EXPECT_EQ(959u, strategy._context_time);
		EXPECT_EQ(0u, strategy._context_secs);
		EXPECT_EQ(20991231u, strategy._engine_trading_date);
		EXPECT_EQ(95900000u, strategy._tick_action_time);
		EXPECT_EQ(20991231u, engine.get_date());
		EXPECT_EQ(959u, engine.get_raw_time());
		EXPECT_EQ(0u, engine.get_secs());
		EXPECT_EQ(20991231u, engine.get_trading_date());

		// Once the first quote establishes the clock, a late quote is still delivered
		// but must not move the engine date/time/trading date backward.
		WTSTickData* lateTick = r123::makeTick(20991231, 95800000, 9.0);
		ASSERT_NE(nullptr, lateTick);
		ticker.on_tick(lateTick);
		lateTick->release();
		EXPECT_EQ(2u, strategy._tick_count);
		EXPECT_EQ(20991231u, engine.get_date());
		EXPECT_EQ(959u, engine.get_raw_time());
		EXPECT_EQ(0u, engine.get_secs());
		EXPECT_EQ(20991231u, engine.get_trading_date());

		ticker.stop();
	}
}

TEST(R123UftTicker, firstBreakQuoteDoesNotInitializeClockOrMinuteBaseline)
{
	r123::SessionBaseDataMgr bd;
	WTSSessionInfo* session = WTSSessionInfo::create("R123LUNCHFIRST", "r123 lunch first", 0);
	ASSERT_NE(nullptr, session);
	R123SessionGuard sessionGuard{ session };
	session->addTradingSection(900, 1130);
	session->addTradingSection(1300, 1500);
	bd._session = session;
	ASSERT_NE(nullptr, bd.add("SHFE", "ag", "ag2406", 0, 0,
		CM_OpenCover, TM_Both, 10));

	{
		WtUftDtMgr dtMgr;
		WtUftEngine engine;
		engine.init(nullptr, &bd, &dtMgr, nullptr);
		engine.set_date_time(20000101, 500, 7, 499);
		engine.set_trading_date(20000102);

		FirstQuoteClockStrategy strategy(&engine);
		auto context = std::make_shared<UftStraContext>(&engine, "r123-lunch-first-context");
		context->set_strategy(&strategy);
		context->stra_sub_ticks(kAg);
		engine.addContext(context);

		WtUftRtTicker ticker(&engine);
		ticker.init("R123LUNCHFIRST");
		UftFirstTickAutoCloseTestAccess::set_clock(ticker, 20991231, 123000000);
		ticker.run();
		const uint32_t engineDateBefore = engine.get_date();
		const uint32_t engineMinBefore = engine.get_min_time();
		const uint32_t engineRawBefore = engine.get_raw_time();
		const uint32_t engineSecsBefore = engine.get_secs();
		const uint32_t engineTDateBefore = engine.get_trading_date();

		// Lunch quote arrives before the first valid quote: forward it, but keep
		// both the startup clock and the empty minute baseline unchanged.
		WTSTickData* lunchTick = r123::makeTick(20991231, 120000000, 10.0);
		ASSERT_NE(nullptr, lunchTick);
		ticker.on_tick(lunchTick);
		lunchTick->release();
		EXPECT_EQ(1u, strategy._tick_count);
		EXPECT_EQ(engineDateBefore, strategy._context_date);
		EXPECT_EQ(engineRawBefore, strategy._context_time);
		EXPECT_EQ(engineSecsBefore, strategy._context_secs);
		EXPECT_EQ(engineTDateBefore, strategy._engine_trading_date);
		EXPECT_EQ(120000000u, strategy._tick_action_time);
		EXPECT_EQ(engineDateBefore, engine.get_date());
		EXPECT_EQ(engineMinBefore, engine.get_min_time());
		EXPECT_EQ(engineRawBefore, engine.get_raw_time());
		EXPECT_EQ(engineSecsBefore, engine.get_secs());
		EXPECT_EQ(engineTDateBefore, engine.get_trading_date());
		uint32_t tickerDate = 0, tickerTime = 0, minutePosition = 0;
		UftFirstTickAutoCloseTestAccess::get_state(ticker, tickerDate, tickerTime, minutePosition);
		EXPECT_EQ(20991231u, tickerDate);
		EXPECT_EQ(123000000u, tickerTime);
		EXPECT_EQ(0u, minutePosition);

		// The next legal quote after lunch must establish the first valid baseline.
		WTSTickData* firstValidTick = r123::makeTick(20991231, 130000000, 11.0);
		ASSERT_NE(nullptr, firstValidTick);
		ticker.on_tick(firstValidTick);
		firstValidTick->release();
		EXPECT_EQ(2u, strategy._tick_count);
		EXPECT_EQ(20991231u, strategy._context_date);
		EXPECT_EQ(1300u, strategy._context_time);
		EXPECT_EQ(0u, strategy._context_secs);
		EXPECT_EQ(20991231u, strategy._engine_trading_date);
		EXPECT_EQ(20991231u, engine.get_date());
		EXPECT_EQ(1300u, engine.get_raw_time());
		EXPECT_EQ(0u, engine.get_secs());
		EXPECT_EQ(20991231u, engine.get_trading_date());
		UftFirstTickAutoCloseTestAccess::get_state(ticker, tickerDate, tickerTime, minutePosition);
		EXPECT_EQ(20991231u, tickerDate);
		EXPECT_EQ(130000000u, tickerTime);
		EXPECT_NE(0u, minutePosition);
		ticker.stop();
	}
}

TEST(R123UftTicker, breakQuoteAfterMinuteBaselineDoesNotMoveClockOrPosition)
{
	r123::SessionBaseDataMgr bd;
	WTSSessionInfo* session = WTSSessionInfo::create("R123LUNCHACTIVE", "r123 lunch active", 0);
	ASSERT_NE(nullptr, session);
	R123SessionGuard sessionGuard{ session };
	session->addTradingSection(900, 1130);
	session->addTradingSection(1300, 1500);
	bd._session = session;
	ASSERT_NE(nullptr, bd.add("SHFE", "ag", "ag2406", 0, 0,
		CM_OpenCover, TM_Both, 10));

	{
		WtUftDtMgr dtMgr;
		WtUftEngine engine;
		engine.init(nullptr, &bd, &dtMgr, nullptr);
		engine.set_date_time(20000101, 500, 7, 499);

		FirstQuoteClockStrategy strategy(&engine);
		auto context = std::make_shared<UftStraContext>(&engine, "r123-lunch-active-context");
		context->set_strategy(&strategy);
		context->stra_sub_ticks(kAg);
		engine.addContext(context);

		WtUftRtTicker ticker(&engine);
		ticker.init("R123LUNCHACTIVE");
		UftFirstTickAutoCloseTestAccess::set_clock(ticker, 20991231, 90000000);
		ticker.run();

		WTSTickData* morningTick = r123::makeTick(20991231, 110000000, 10.0);
		ASSERT_NE(nullptr, morningTick);
		ticker.on_tick(morningTick);
		morningTick->release();
		ASSERT_EQ(1u, strategy._tick_count);
		ASSERT_EQ(20991231u, engine.get_date());
		ASSERT_EQ(1100u, engine.get_raw_time());
		ASSERT_EQ(0u, engine.get_secs());
		ASSERT_EQ(20991231u, engine.get_trading_date());
		uint32_t baselineDate = 0, baselineTime = 0, baselinePos = 0;
		UftFirstTickAutoCloseTestAccess::get_state(ticker, baselineDate, baselineTime, baselinePos);
		ASSERT_EQ(20991231u, baselineDate);
		ASSERT_EQ(110000000u, baselineTime);
		ASSERT_NE(0u, baselinePos);

		// A later timestamp in the lunch break is forwarded without changing the
		// established market clock or minute position.
		WTSTickData* lunchTick = r123::makeTick(20991231, 120000000, 11.0);
		ASSERT_NE(nullptr, lunchTick);
		ticker.on_tick(lunchTick);
		lunchTick->release();
		EXPECT_EQ(2u, strategy._tick_count);
		EXPECT_EQ(20991231u, strategy._context_date);
		EXPECT_EQ(1100u, strategy._context_time);
		EXPECT_EQ(0u, strategy._context_secs);
		EXPECT_EQ(20991231u, strategy._engine_trading_date);
		EXPECT_EQ(120000000u, strategy._tick_action_time);
		EXPECT_EQ(20991231u, engine.get_date());
		EXPECT_EQ(1100u, engine.get_raw_time());
		EXPECT_EQ(0u, engine.get_secs());
		EXPECT_EQ(20991231u, engine.get_trading_date());
		uint32_t tickerDate = 0, tickerTime = 0, minutePosition = 0;
		UftFirstTickAutoCloseTestAccess::get_state(ticker, tickerDate, tickerTime, minutePosition);
		EXPECT_EQ(baselineDate, tickerDate);
		EXPECT_EQ(baselineTime, tickerTime);
		EXPECT_EQ(baselinePos, minutePosition);

		// The first regular afternoon quote advances from the morning baseline.
		WTSTickData* afternoonTick = r123::makeTick(20991231, 130000000, 12.0);
		ASSERT_NE(nullptr, afternoonTick);
		ticker.on_tick(afternoonTick);
		afternoonTick->release();
		EXPECT_EQ(3u, strategy._tick_count);
		EXPECT_EQ(20991231u, engine.get_date());
		EXPECT_EQ(1300u, engine.get_raw_time());
		EXPECT_EQ(0u, engine.get_secs());
		EXPECT_EQ(20991231u, engine.get_trading_date());
		UftFirstTickAutoCloseTestAccess::get_state(ticker, tickerDate, tickerTime, minutePosition);
		EXPECT_EQ(20991231u, tickerDate);
		EXPECT_EQ(130000000u, tickerTime);
		EXPECT_GT(minutePosition, baselinePos);
		ticker.stop();
	}
}

// WtUftTicker.cpp:202-240 首笔 tick 转发并设置调度，1ms 后工作线程自动闭合 09:30 分钟。
TEST(R123UftTicker, auto_close_after_check_time)
{
	r123::SessionBaseDataMgr bd;
	WTSSessionInfo* session = WTSSessionInfo::create("R123ALL", "r123 allday", 0);
	ASSERT_NE(nullptr, session);
	// 卡2 2b=A（用户裁定 2026-11-03）：全天时段只能写 0-0，0-2400 被拒（会话无时段）；迭代03 第五会话 int2 归因更新。
	session->addTradingSection(0, 0);
	bd._session = session;

	{
		WtUftDtMgr dtMgr;
		WtUftEngine engine;
		engine.init(nullptr, &bd, &dtMgr, nullptr);
		// 把引擎时间钉死，避免用挂钟做期望值
		engine.set_date_time(20000101, 0, 0, 0);

		WtUftRtTicker ticker(&engine);
		ticker.init("R123ALL");
		ticker.run();

		// 09:30:59.999 -> left_ticks = 1，线程下一轮必然满足 now >= _next_check_time
		WTSTickData* t = r123::makeTick(20991231, 93059999, 10.0);
		ASSERT_NE(nullptr, t);
		ticker.on_tick(t);
		t->release();

		bool autoClosed = false;
		for (int i = 0; i < 100; i++)
		{
			if (UftFirstTickAutoCloseTestAccess::auto_close_finished(ticker))
			{
				autoClosed = true;
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}

		ASSERT_TRUE(autoClosed);
		EXPECT_EQ(931u, engine.get_min_time());
		EXPECT_EQ(20991231u, engine.get_date());
		EXPECT_EQ(931u, engine.get_raw_time());
		EXPECT_EQ(0u, engine.get_secs());
		// 首笔 tick 在定位分钟后立即进入引擎，自动闭合继续沿用原来的时段调度。
		WTSTickData* firstTick = engine.get_last_tick(1, kAg);
		ASSERT_NE(nullptr, firstTick);
		EXPECT_DOUBLE_EQ(10.0, firstTick->price());
		firstTick->release();

		// 后续分钟行情继续更新缓存价格。
		WTSTickData* secondTick = r123::makeTick(20991231, 93100000, 11.0);
		ASSERT_NE(nullptr, secondTick);
		ticker.on_tick(secondTick);
		secondTick->release();
		EXPECT_DOUBLE_EQ(11.0, engine.get_cur_price(kAg));
		EXPECT_EQ(932u, engine.get_min_time());
		EXPECT_EQ(931u, engine.get_raw_time());
		EXPECT_EQ(0u, engine.get_secs());

		ticker.stop();

		// stop 之后的行情不得再进入引擎或覆盖缓存。
		WTSTickData* stoppedTick = r123::makeTick(20991231, 93200000, 12.0);
		ASSERT_NE(nullptr, stoppedTick);
		ticker.on_tick(stoppedTick);
		stoppedTick->release();
		EXPECT_DOUBLE_EQ(11.0, engine.get_cur_price(kAg));
	}

	session->release();
}

TEST(R123UftTicker, unknownSessionDoesNotStartOrForwardTicks)
{
	r123::SessionBaseDataMgr bd;
	WtUftDtMgr dtMgr;
	WtUftEngine engine;
	engine.init(nullptr, &bd, &dtMgr, nullptr);
	engine.set_date_time(20240315, 93000, 21, 93000);

	WtUftRtTicker ticker(&engine);
	ticker.init("R123_MISSING_SESSION");
	ticker.run();

	WTSTickData* tick = r123::makeTick(20240318, 93100000, 11.0);
	ASSERT_NE(nullptr, tick);
	ticker.on_tick(tick);
	tick->release();

	EXPECT_EQ(nullptr, engine.get_last_tick(1, kAg));
	EXPECT_EQ(20240315u, engine.get_date());
	EXPECT_EQ(93000u, engine.get_raw_time());
	ticker.stop();
}

// WtUftEngine.cpp:338-342 产品配置里的 session -> ticker.init；
// :358-362 handle_push_quote -> ticker.on_tick；第二笔 tick 推进分钟后
// 走 WtUftTicker.cpp:118-148 的 engine.on_tick + set_date_time + set_trading_date。
TEST(R123UftEngine, product_session_run_and_push_quote)
{
	r123::SessionBaseDataMgr bd;
	WTSSessionInfo* session = WTSSessionInfo::create("R123PROD", "r123 product", 0);
	ASSERT_NE(nullptr, session);
	// 卡2 2b=A（用户裁定 2026-11-03）：全天时段只能写 0-0，0-2400 被拒（会话无时段）；迭代03 第五会话 int2 归因更新。
	session->addTradingSection(0, 0);
	bd._session = session;

	WTSVariant* cfg = WTSVariant::createObject();
	WTSVariant* product = WTSVariant::createObject();
	product->append("session", "R123PROD");
	cfg->append("product", product);

	{
		WtUftDtMgr dtMgr;
		WtUftEngine engine;
		engine.init(cfg, &bd, &dtMgr, nullptr);
		engine.run();

		WTSTickData* t1 = r123::makeTick(20991231, 93000000, 10.0);
		ASSERT_NE(nullptr, t1);
		engine.handle_push_quote(t1);
		t1->release();

		WTSTickData* t2 = r123::makeTick(20991231, 93100000, 11.0);
		ASSERT_NE(nullptr, t2);
		engine.handle_push_quote(t2);
		t2->release();

		// 931 推进到 932，ticker 在 block 内先 on_minute_end 再 set_date_time(..., 932, 500, 931)
		EXPECT_EQ(20991231u, engine.get_date());
		EXPECT_EQ(932u, engine.get_min_time());
		EXPECT_EQ(931u, engine.get_raw_time());
		EXPECT_DOUBLE_EQ(11.0, engine.get_cur_price(kAg));
	}

	cfg->release();
	session->release();
}

/*!
 * 缺陷证据（BUG-R123-001，启用即失败，禁止在 CI 打开）：
 * WtUftEngine.cpp:209-243 的 get_kline_slice 在函数首行 `return NULL;`，
 * 后面的品种解析、_bar_sub_map 订阅登记、周期换算全部不可达，UftStraContext::stra_get_bars
 * 因此永远拿不到 K 线。这里用合法合约请求 bars，期望非空，实际必为 NULL。
 */
TEST(R123UftEngine, DISABLED_get_kline_slice_never_reaches_subscription)
{
	r123::SessionBaseDataMgr bd;
	bd.add("SHFE", "ag", "ag2406");

	WtUftDtMgr dtMgr;
	WtUftEngine engine;
	engine.init(nullptr, &bd, &dtMgr, nullptr);

	WTSKlineSlice* bars = engine.get_kline_slice(1, "SHFE.ag2406", "m", 10, 1, 0);
	EXPECT_NE(nullptr, bars);
	if (bars != nullptr)
		bars->release();
}

}	// namespace
