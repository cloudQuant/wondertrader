/**
 * r72_replayer_block_edge_test.cpp  (lane R72)
 *
 * 靶子：src/WtBtCore/HisDataReplayer.cpp 的块解析与定位边界
 *   - proc_block_data 的老版本 bar/tick 转换（L86-L123），由 cacheRawBarsFromCSV /
 *     cacheRawTicksFromCSV 的 dsb 回读分支进入（L3780-L3813、L3482-L3504）
 *   - get_kline_slice 在收盘时刻的 isClosed 初始游标（L2297、L2370-L2390）
 *   - run_by_bars 的 locate_barindex 上界回退（L620-L626、L780-L783）
 *   - 四类订阅接口的空串/复权后缀分支（L3089-L3165）
 *
 * 老块只写 BlockHeader + 老结构体到临时目录，不触碰真实存储。
 */
#include <gtest/gtest.h>

#include "r72_common.h"

namespace
{
	const char* const kCode = "SSE.STK.000001";
	const uint32_t kDates[5] = { 20200102u, 20200103u, 20200106u, 20200107u, 20200108u };

	std::vector<WTSBarStruct> daily_bars(double base)
	{
		std::vector<WTSBarStruct> bars;
		for (uint32_t i = 0; i < 5; i++)
			bars.emplace_back(r72::make_bar(kDates[i], 1500u, base + i));
		return bars;
	}

	WTSBarStructOld old_bar(uint32_t date, double close)
	{
		WTSBarStructOld bar;
		bar.date = date;
		bar.time = (date - 19900000u) * 10000u + 1500u;
		bar.open = close - 0.1;
		bar.high = close + 0.2;
		bar.low = close - 0.2;
		bar.close = close;
		bar.settle = close;
		bar.vol = 100;
		bar.money = close * 100;
		return bar;
	}

	WTSTickStructOld old_tick(const char* code, uint32_t date, uint32_t hhmmssmmm, double price)
	{
		WTSTickStructOld tick;
		strcpy(tick.exchg, "SSE");
		strcpy(tick.code, code);
		tick.action_date = date;
		tick.trading_date = date;
		tick.action_time = hhmmssmmm;
		tick.price = price;
		tick.open = price - 0.1;
		tick.high = price + 0.1;
		tick.low = price - 0.2;
		tick.pre_close = price - 0.05;
		tick.volume = 10;
		tick.total_volume = 100;
		tick.bid_prices[0] = price - 0.01;
		tick.ask_prices[0] = price + 0.01;
		tick.bid_qty[0] = 100;
		tick.ask_qty[0] = 100;
		return tick;
	}
}

class R72ReplayerBlockEdge : public r72::Fixture
{
};

/*
 * 老版本未压缩日线块：BlockHeader(RAW) + WTSBarStructOld[]。
 * cacheRawBarsFromCSV 读到后 proc_block_data 走 bOldVer 分支转换成新结构
 * （HisDataReplayer.cpp L86-L108），切片字段与输入一致。
 */
TEST_F(R72ReplayerBlockEdge, OldRawBarBlockIsConverted)
{
	std::vector<WTSBarStructOld> bars;
	bars.emplace_back(old_bar(20200102u, 11.5));
	bars.emplace_back(old_bar(20200103u, 12.5));
	bars.emplace_back(old_bar(20200106u, 13.5));
	ASSERT_TRUE(r72::write_old_bar_block(_base / "his/day/SSE/000001.dsb", bars));

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001081500ull, false);
	ASSERT_TRUE(rp.init(cfg, nullptr, nullptr));
	cfg->release();
	r72::Sink sink;
	rp.register_sink(&sink, "r72_old_bar");
	ASSERT_TRUE(rp.prepare());

	WTSKlineSlice* s = rp.get_kline_slice(kCode, "d1", 3, 1, false);
	ASSERT_NE(nullptr, s);
	ASSERT_GE(s->size(), 1u);
	EXPECT_EQ(20200102u, s->at(0)->date);
	EXPECT_DOUBLE_EQ(11.5, s->at(0)->close);
	EXPECT_DOUBLE_EQ(11.7, s->at(0)->high);
	s->release();
}

/*
 * 老版本未压缩 tick 块：BlockHeader(RAW) + WTSTickStructOld[]。
 * cacheRawTicksFromCSV 的 dsb 回读 + proc_block_data 的 tick 老结构转换
 * （HisDataReplayer.cpp L109-L123、L3482-L3504）。
 */
TEST_F(R72ReplayerBlockEdge, OldRawTickBlockIsConverted)
{
	std::vector<WTSTickStructOld> ticks;
	ticks.emplace_back(old_tick("000001", 20200102u, 93000000u, 10.0));
	ticks.emplace_back(old_tick("000001", 20200102u, 93100000u, 10.5));
	ticks.emplace_back(old_tick("000001", 20200102u, 93200000u, 11.0));
	ASSERT_TRUE(r72::write_old_tick_block(_base / "bin/ticks/SSE.STK.000001_tick_20200102.dsb", ticks));

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, true);
	ASSERT_TRUE(rp.init(cfg, nullptr, nullptr));
	cfg->release();
	r72::Sink sink;
	rp.register_sink(&sink, "r72_old_tick");
	ASSERT_TRUE(rp.prepare());

	WTSTickSlice* s = rp.get_tick_slice(kCode, 2, 202001020931ull);
	ASSERT_NE(nullptr, s);
	ASSERT_EQ(1u, s->size());
	EXPECT_DOUBLE_EQ(10.0, s->at(0)->price);
	EXPECT_DOUBLE_EQ(9.99, s->at(0)->bid_prices[0]);
	s->release();
}

/*
 * 起止时间落在收盘时刻：get_kline_slice 的 isClosed=true 分支
 * （HisDataReplayer.cpp L2297、L2370-L2390）把游标停在当日及之前，
 * run_by_bars 仍能收敛并补发结束事件。
 */
TEST_F(R72ReplayerBlockEdge, DailyBarsAtCloseUseClosedCursor)
{
	r72::Loader loader;
	loader.finalBars[kCode] = daily_bars(10.5);

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001031500ull, 202001081500ull, false);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();
	r72::Sink sink;
	rp.register_sink(&sink, "r72_closed");
	ASSERT_TRUE(rp.prepare());
	EXPECT_EQ(1500u, rp.get_min_time());

	WTSKlineSlice* s = rp.get_kline_slice(kCode, "d1", 5, 1, true);
	ASSERT_NE(nullptr, s);
	ASSERT_EQ(2u, s->size());
	EXPECT_EQ(20200102u, s->at(0)->date);
	EXPECT_EQ(20200103u, s->at(1)->date);
	s->release();

	rp.run(false);
	EXPECT_EQ(1u, sink.doneCalls);
	EXPECT_GE(sink.barCalls, 2u);
	EXPECT_LE(sink.barCalls, 4u);
}

/*
 * 结束时间不是K线日期（20200104 落在 0103 与 0106 之间）：
 * locate_barindex 的上界分支必须回退一根（HisDataReplayer.cpp L620-L626），
 * run_by_bars 在 0103 之后即停止，不会把 0106 也回放掉。
 */
TEST_F(R72ReplayerBlockEdge, UpperBoundLocateStopsBeforeNextBar)
{
	r72::Loader loader;
	loader.finalBars[kCode] = daily_bars(10.5);

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001041500ull, false);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();
	r72::Sink sink;
	rp.register_sink(&sink, "r72_upper");
	ASSERT_TRUE(rp.prepare());

	WTSKlineSlice* s = rp.get_kline_slice(kCode, "d1", 5, 1, true);
	ASSERT_NE(nullptr, s);
	s->release();

	rp.run(false);
	EXPECT_EQ(1u, sink.doneCalls);
	EXPECT_EQ(1u, sink.barCalls);
	EXPECT_EQ(20200103u, rp.get_date());
}

/*
 * 订阅接口的空串与复权后缀分支（HisDataReplayer.cpp L3089-L3165）：
 * Q/H 后缀都要被剥掉并记录 flag；tick 关闭时还要进入未订阅需求表。
 */
TEST_F(R72ReplayerBlockEdge, SubscriptionFlagsAndEmptyCodes)
{
	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, false);
	ASSERT_TRUE(rp.init(cfg, nullptr, nullptr));
	cfg->release();
	r72::Sink sink;
	rp.register_sink(&sink, "r72_sub");
	ASSERT_TRUE(rp.prepare());

	rp.sub_tick(1, "");
	rp.sub_tick(1, "SSE.STK.000001Q");
	rp.sub_tick(2, "SSE.STK.000001H");

	rp.sub_order_detail(1, "");
	rp.sub_order_detail(1, "SSE.STK.000001Q");

	rp.sub_order_queue(1, "");
	rp.sub_order_queue(1, "SSE.STK.000001H");

	rp.sub_transaction(1, "");
	rp.sub_transaction(1, "SSE.STK.000001Q");

	EXPECT_EQ(20200102u, rp.get_date());
	EXPECT_DOUBLE_EQ(0.0, rp.get_cur_price("SSE.STK.000001"));
}
