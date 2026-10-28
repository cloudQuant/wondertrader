/**
 * r80_replayer_slice_unsub_test.cpp  (lane R80)
 *
 * 靶子：src/WtBtCore/HisDataReplayer.cpp
 *   - L2384-L2387 / L2419-L2459 首次定位落在「未来首根」时 _cursor=0 的日线分支
 *   - L3167-L3257 checkUnbars + cacheRawBarsFromCSV(bSubbed=false)
 *   - L1464-L1533 simTickWithUnsubBars 的日线臂
 *   - L3737-L3899 cacheRawBarsFromCSV 的 ruleTag 期货命名臂（CFFEX.IF_HOT.dsb）
 *   - L3034-L3087 get_day_price 的 _day_cache 分支
 *
 * 全部走公开 API；行情只写系统临时目录的 csv，并断言转储文件名。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <WtBtCore/HisDataReplayer.h>

#include "r80_common.h"

namespace
{
	const char* const kSub = "SSE.STK.000002";

	std::vector<WTSBarStruct> three_days(double base)
	{
		return {
			r80::mk_bar(20200101u, 0, base, base + 1.0, base - 1.0, base + 0.5, 100.0),
			r80::mk_bar(20200102u, 0, base + 1.0, base + 2.0, base + 0.5, base + 1.5, 110.0),
			r80::mk_bar(20200103u, 0, base + 2.0, base + 3.0, base + 1.5, base + 2.5, 120.0)
		};
	}
}

class R80SliceUnsub : public r80::Fixture
{
};

/*
 * stime=1/1，而日线首根为 1/6：lower_bound 命中首根后 eIdx==0 且
 * curBar.date > _cur_tdate，走 L2384-L2387 把 _cursor 置 0 并返回 NULL；
 * 第二次调用命中 L2419 的 cursor==0 守卫，仍然返回 NULL。
 */
TEST_F(R80SliceUnsub, FutureFirstDailyBarYieldsNullSlice)
{
	const std::vector<WTSBarStruct> bars = {
		r80::mk_bar(20200106u, 0, 10.0, 11.0, 9.5, 10.5, 100.0),
		r80::mk_bar(20200107u, 0, 10.5, 11.5, 10.0, 11.0, 120.0)
	};
	ASSERT_TRUE(r80::write_bars_csv(_base, "SSE.STK.000001_d", bars, false));

	HisDataReplayer rp;
	r80::CfgOpts opts;
	WTSVariant* cfg = r80::make_cfg(_root, _base, 202001010930ull, 202001071500ull, opts);
	ASSERT_TRUE(rp.init(cfg));
	cfg->release();

	r80::Sink sink;
	rp.register_sink(&sink, "r80_future");
	uint32_t nulls = 0;
	sink.onInit = [&rp, &nulls]() {
		if (rp.get_kline_slice("SSE.STK.000001", "d", 10, 1, true) == NULL)
			nulls++;
		if (rp.get_kline_slice("SSE.STK.000001", "d", 10) == NULL)
			nulls++;
	};

	ASSERT_TRUE(rp.prepare());
	EXPECT_EQ(2u, nulls);
}

/*
 * 主合约分钟回放 + 未订阅合约（sub_tick 且 tick 关闭）：
 * prepare 的 checkUnbars 用 bSubbed=false 装载 000002（L3211-L3220、L3798-L3799），
 * run 中 simTickWithUnsubBars 的分钟臂按 barTime==nowTime 出 4 笔 OHLC tick。
 */
TEST_F(R80SliceUnsub, UnsubscribedMinuteBarsFeedSimTicks)
{
	const std::vector<WTSBarStruct> bars = {
		r80::mk_bar(20200102u, 930, 20.0, 20.5, 19.5, 20.2, 100.0),
		r80::mk_bar(20200102u, 931, 21.0, 22.0, 20.5, 21.5, 110.0),
		r80::mk_bar(20200102u, 932, 22.0, 23.0, 21.5, 22.5, 120.0)
	};
	ASSERT_TRUE(r80::write_bars_csv(_base, "SSE.STK.000001_m1", bars, true));
	ASSERT_TRUE(r80::write_bars_csv(_base, "SSE.STK.000002_m1", bars, true));

	HisDataReplayer rp;
	r80::CfgOpts opts;
	WTSVariant* cfg = r80::make_cfg(_root, _base, 202001020930ull, 202001021500ull, opts);
	ASSERT_TRUE(rp.init(cfg));
	cfg->release();

	r80::Sink sink;
	rp.register_sink(&sink, "r80_unsub");
	sink.onInit = [&rp, &sink]() {
		sink.slice = rp.get_kline_slice("SSE.STK.000001", "m", 1, 1, true);
	};
	rp.sub_tick(1, "SSE.STK.000002");

	ASSERT_TRUE(rp.prepare());
	ASSERT_NE(nullptr, sink.slice);
	rp.run(false);

	// 主合约分钟线：首根 930 作为历史，回放 931、932
	ASSERT_EQ(2u, sink.bars.size());
	EXPECT_EQ(931u, sink.bars[0].time);
	EXPECT_EQ(932u, sink.bars[1].time);

	// 未订阅合约只在 931、932 各出 4 笔 tick（930 被 checkUnbars 初始游标当历史）
	std::vector<r80::TickRec> subTicks;
	for (const r80::TickRec& rec : sink.ticks)
	{
		if (rec.code == kSub)
			subTicks.push_back(rec);
	}
	ASSERT_EQ(8u, subTicks.size());
	for (uint32_t d = 0; d < 2; d++)
	{
		const uint32_t tm = (d == 0) ? 931u : 932u;
		const double open = (d == 0) ? 21.0 : 22.0;
		const double high = (d == 0) ? 22.0 : 23.0;
		const double low = (d == 0) ? 20.5 : 21.5;
		const double close = (d == 0) ? 21.5 : 22.5;
		for (uint32_t i = 0; i < 4; i++)
		{
			const r80::TickRec& rec = subTicks[d * 4 + i];
			EXPECT_EQ(20200102u, rec.date);
			EXPECT_EQ(tm, rec.time);
			EXPECT_EQ(i, rec.pxType);
			const double want = (i == 0) ? open : (i == 1) ? high : (i == 2) ? low : close;
			EXPECT_DOUBLE_EQ(want, rec.price);
		}
	}

	sink.slice->release();
	sink.slice = NULL;
}

/*
 * CFFEX.IF.HOT 日线：期货 + ruleTag 分支把文件命名为 CFFEX.IF_HOT.dsb
 * （L3762-L3771），随后正常回放，日线臂每根出 4 笔 tick。
 */
TEST_F(R80SliceUnsub, HotRuleFutureCsvUsesRuleDsbName)
{
	const std::vector<WTSBarStruct> bars = three_days(100.0);
	ASSERT_TRUE(r80::write_bars_csv(_base, "CFFEX.IF.HOT_d", bars, false));

	HisDataReplayer rp;
	r80::CfgOpts opts;
	opts.commodity = (_root / "dist/common/commodities.json").string();
	opts.hot = (_root / "dist/common/hots.json").string();
	WTSVariant* cfg = r80::make_cfg(_root, _base, 202001010930ull, 202001031500ull, opts);
	ASSERT_TRUE(rp.init(cfg));
	cfg->release();

	r80::Sink sink;
	rp.register_sink(&sink, "r80_hot");
	sink.onInit = [&rp, &sink]() {
		sink.slice = rp.get_kline_slice("CFFEX.IF.HOT", "d", 10, 1, true);
	};

	ASSERT_TRUE(rp.prepare());
	ASSERT_NE(nullptr, sink.slice);
	EXPECT_EQ(1u, sink.slice->size());
	EXPECT_DOUBLE_EQ(100.5, sink.slice->at(0)->close);

	const std::filesystem::path dsb = _base / "his" / "day" / "CFFEX" / "CFFEX.IF_HOT.dsb";
	EXPECT_TRUE(std::filesystem::exists(dsb));

	rp.run(false);
	ASSERT_EQ(8u, sink.ticks.size());
	for (const r80::TickRec& rec : sink.ticks)
		EXPECT_EQ(std::string("CFFEX.IF.HOT"), rec.code);
	EXPECT_EQ(1u, sink.doneCalls);

	sink.slice->release();
	sink.slice = NULL;
}

/*
 * tick 关闭时日线回放：get_day_price 越过 tick 分支直接读 _day_cache，
 * 四个 flag + default 与 get_cur_price 都必须返回当日累计值。
 */
TEST_F(R80SliceUnsub, DayPriceReadsSimulatedDayCache)
{
	const std::vector<WTSBarStruct> bars = three_days(10.0);
	ASSERT_TRUE(r80::write_bars_csv(_base, "SSE.STK.000001_d", bars, false));

	HisDataReplayer rp;
	r80::CfgOpts opts;
	WTSVariant* cfg = r80::make_cfg(_root, _base, 202001010930ull, 202001031500ull, opts);
	ASSERT_TRUE(rp.init(cfg));
	cfg->release();

	r80::Sink sink;
	sink.rp = &rp;
	rp.register_sink(&sink, "r80_daypx");
	sink.onInit = [&rp, &sink]() {
		sink.slice = rp.get_kline_slice("SSE.STK.000001", "d", 10, 1, true);
	};
	std::vector<std::vector<double>> snapshots;
	sink.onBarClose = [&rp, &snapshots](const r80::BarRec&) {
		std::vector<double> px;
		px.push_back(rp.get_day_price("SSE.STK.000001", 0));
		px.push_back(rp.get_day_price("SSE.STK.000001", 1));
		px.push_back(rp.get_day_price("SSE.STK.000001", 2));
		px.push_back(rp.get_day_price("SSE.STK.000001", 3));
		px.push_back(rp.get_day_price("SSE.STK.000001", 9));
		px.push_back(rp.get_cur_price("SSE.STK.000001"));
		snapshots.push_back(px);
	};

	ASSERT_TRUE(rp.prepare());
	ASSERT_NE(nullptr, sink.slice);
	rp.run(false);

	ASSERT_EQ(2u, snapshots.size());
	const double expect[2][5] = {
		{ 11.0, 12.0, 10.5, 11.5, 0.0 },
		{ 12.0, 13.0, 11.5, 12.5, 0.0 }
	};
	for (uint32_t d = 0; d < 2; d++)
	{
		for (uint32_t i = 0; i < 5; i++)
			EXPECT_DOUBLE_EQ(expect[d][i], snapshots[d][i]) << "day " << d << " flag " << i;
		EXPECT_DOUBLE_EQ(expect[d][3], snapshots[d][5]);
	}
	// 未缓存合约与未缓存价格
	EXPECT_DOUBLE_EQ(0.0, rp.get_day_price("SSE.STK.999999", 0));
	EXPECT_DOUBLE_EQ(0.0, rp.get_cur_price("SSE.STK.999999"));

	sink.slice->release();
	sink.slice = NULL;
}
