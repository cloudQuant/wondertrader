/*!
 * \file r123_uft_ticker_engine_test.cpp
 * \brief 第二十波 R123 - WtUftRtTicker 自动闭合分钟 + WtUftEngine 产品时段/推送行情
 *
 * 覆盖（只读生产代码）:
 *  - WtUftTicker.cpp:202-240：工作线程到点自动触发 on_minute_end、跨零点判定、
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

#include "Includes/WTSVariant.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namespace wtp;

namespace {

const char* kAg = "SHFE.ag2406";

// WtUftTicker.cpp:202-240 自动闭合：首笔 tick 只定位 _cur_pos 与 _next_check_time，
// 1ms 后工作线程自动闭合 09:30 分钟；不会把首笔转发给 engine.on_tick。
TEST(R123UftTicker, auto_close_after_check_time)
{
	r123::SessionBaseDataMgr bd;
	WTSSessionInfo* session = WTSSessionInfo::create("R123ALL", "r123 allday", 0);
	ASSERT_NE(nullptr, session);
	session->addTradingSection(0, 2400);
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

		uint32_t minTime = 0;
		for (int i = 0; i < 100; i++)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			minTime = engine.get_min_time();
			if (minTime == 931)
				break;
		}

		EXPECT_EQ(931u, minTime);
		EXPECT_EQ(20991231u, engine.get_date());
		EXPECT_EQ(931u, engine.get_raw_time());
		EXPECT_EQ(0u, engine.get_secs());
		// 首笔 tick 只用于定位分钟位置，自动闭合前不会转发给数据管理器
		EXPECT_EQ(nullptr, engine.get_last_tick(1, kAg));

		ticker.stop();
	}

	session->release();
}

// WtUftEngine.cpp:338-342 产品配置里的 session -> ticker.init；
// :358-362 handle_push_quote -> ticker.on_tick；第二笔 tick 推进分钟后
// 走 WtUftTicker.cpp:118-148 的 engine.on_tick + set_date_time + set_trading_date。
TEST(R123UftEngine, product_session_run_and_push_quote)
{
	r123::SessionBaseDataMgr bd;
	WTSSessionInfo* session = WTSSessionInfo::create("R123PROD", "r123 product", 0);
	ASSERT_NE(nullptr, session);
	session->addTradingSection(0, 2400);
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
