/**
 * r103_replayer_day_period_test.cpp  (lane R103 · wave18)
 *
 * 靶子：日线主图下的多周期游标与日线切片定位
 *   - get_kline_slice 日线后置定位的 cursor 前进      HisDataReplayer.cpp:2412-2439
 *   - 若 cursor>count，切片起点 sIdx 前移             :2461-2463
 *   - simTicks 日线臂跳过多日                           :1324-1378
 *   - simTicks 分钟臂跳过多分钟                         :1248-1316
 *   - simTickWithUnsubBars 日线臂跳过历史日             :1464-1524
 *   - run(true) 的日线 dump_btstate 周期名             :518-555、771-938
 *
 * 只走公开 API；loader 提供全部内存K线。
 */
#include "r103_common.h"

using R103Fixture = r91::Fixture;

namespace
{
	const char* const kMain = "SSE.STK.000001";
	const char* const kSubDay = "SSE.STK.000002";
	const char* const kSubMin = "SSE.STK.000002";
	const char* const kUnsub = "SSE.STK.000003";
	const char* const kTail = "SSE.STK.000004";
	const char* const kClosed = "SSE.STK.000005";

	bool has_bar(const std::vector<r91::BarRec>& bars, const char* code, uint32_t date)
	{
		for (const r91::BarRec& bar : bars)
		{
			if (bar.code == code && bar.date == date)
				return true;
		}
		return false;
	}
}

/*
 * 生产: HisDataReplayer.cpp:2412-2439（第二次日线切片时 cursor 从 0103 推到 0106）
 *       2461-2463（count=1 时 sIdx 前移）
 *       1324-1378（日线 simTicks 在 0106 跳过 0103 -> size 结束）
 *       1248-1316（分钟 simTicks 在 0102 收盘一次跳完 09:31/09:32/09:33）
 *       1464-1524（未订阅日线跳到 0103 -> size 结束）
 *       518-555（run(true) 日线 dump）
 */
TEST_F(R103Fixture, R103Replayer_DayMultiPeriodCursorAndDump)
{
	r103::Loader loader;
	loader.dayBars[kMain] = {
		r91::mk_bar(20200102u, 1500u, 10.0, 11.0, 9.0, 10.5, 100.0),
		r91::mk_bar(20200106u, 1500u, 11.0, 12.0, 10.0, 11.5, 110.0)
	};
	loader.dayBars[kSubDay] = {
		r91::mk_bar(20191230u, 1500u, 20.0, 21.0, 19.0, 20.0, 10.0),
		r91::mk_bar(20191231u, 1500u, 20.5, 21.5, 19.5, 20.5, 11.0),
		r91::mk_bar(20200102u, 1500u, 21.0, 22.0, 20.0, 21.0, 12.0),
		r91::mk_bar(20200103u, 1500u, 21.5, 22.5, 20.5, 21.5, 13.0),
		r91::mk_bar(20200106u, 1500u, 22.0, 23.0, 21.0, 22.0, 14.0)
	};
	loader.m1Bars[kSubMin] = {
		r91::mk_bar(20200102u, 931u, 30.0, 31.0, 29.0, 30.5, 1.0),
		r91::mk_bar(20200102u, 932u, 30.5, 31.5, 29.5, 31.0, 2.0),
		r91::mk_bar(20200102u, 933u, 31.0, 32.0, 30.0, 31.5, 3.0)
	};
	loader.dayBars[kUnsub] = {
		r91::mk_bar(20191230u, 1500u, 40.0, 41.0, 39.0, 40.0, 10.0),
		r91::mk_bar(20191231u, 1500u, 40.5, 41.5, 39.5, 40.5, 11.0),
		r91::mk_bar(20200102u, 1500u, 41.0, 42.0, 40.0, 41.0, 12.0),
		r91::mk_bar(20200103u, 1500u, 41.5, 42.5, 40.5, 41.5, 13.0)
	};
	loader.dayBars[kTail] = {
		r91::mk_bar(20191231u, 1500u, 50.0, 51.0, 49.0, 50.0, 10.0),
		r91::mk_bar(20200103u, 1500u, 50.5, 51.5, 49.5, 50.5, 11.0)
	};
	loader.dayBars[kClosed] = {
		r91::mk_bar(20191231u, 1500u, 60.0, 61.0, 59.0, 60.0, 10.0),
		r91::mk_bar(20200103u, 1500u, 60.5, 61.5, 59.5, 60.5, 11.0),
		r91::mk_bar(20200106u, 1500u, 61.0, 62.0, 60.0, 61.0, 12.0)
	};

	r91::CfgOpts opts;
	opts.mode = "bin";
	opts.tick = false;
	WTSVariant* cfg = r103::make_cfg(_root, _base, 202001020930ull, 202001061500ull, opts);
	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();

	r103::Sink sink;
	sink.rp = &replayer;
	sink.onSessionBegin = [&replayer](uint32_t date) {
		if (date != 20200106u)
			return;
		WTSKlineSlice* slice = replayer.get_kline_slice(kSubDay, "d", 1, 1, false);
		if (slice != NULL)
		{
			EXPECT_EQ(1, slice->size());
			EXPECT_DOUBLE_EQ(21.5, slice->at(0)->close);
			slice->release();
		}
	};
	sink.onTick = [&replayer](const r91::TickRec& rec) {
		if (rec.code != kClosed || rec.date != 20200106u)
			return;
		WTSKlineSlice* slice = replayer.get_kline_slice(kClosed, "d", 1, 1, false);
		if (slice != NULL)
		{
			EXPECT_EQ(1, slice->size());
			EXPECT_DOUBLE_EQ(61.0, slice->at(0)->close);
			slice->release();
		}
	};
	replayer.register_sink(&sink, "r103_periods");
	ASSERT_TRUE(replayer.prepare());

	WTSKlineSlice* mainSlice = replayer.get_kline_slice(kMain, "d", 5, 1, true);
	ASSERT_NE(nullptr, mainSlice);
	ASSERT_EQ(1, mainSlice->size());
	EXPECT_DOUBLE_EQ(10.5, mainSlice->at(0)->close);
	mainSlice->release();

	WTSKlineSlice* subDay = replayer.get_kline_slice(kSubDay, "d", 5, 1, false);
	ASSERT_NE(nullptr, subDay);
	EXPECT_EQ(2, subDay->size());
	subDay->release();

	WTSKlineSlice* subMin = replayer.get_kline_slice(kSubMin, "m", 1, 1, false);
	ASSERT_NE(nullptr, subMin);
	EXPECT_EQ(1, subMin->size());
	subMin->release();

	WTSKlineSlice* tail = replayer.get_kline_slice(kTail, "d", 5, 1, false);
	ASSERT_NE(nullptr, tail);
	EXPECT_EQ(1, tail->size());
	tail->release();

	WTSKlineSlice* closed = replayer.get_kline_slice(kClosed, "d", 5, 1, false);
	ASSERT_NE(nullptr, closed);
	EXPECT_EQ(1, closed->size());
	closed->release();

	replayer.sub_tick(9u, kUnsub);

	replayer.run(true);

	// 0102 收盘：订阅的 B 日线闭盘；0106 收盘：主图与 B 各一根
	EXPECT_GE(sink.bars.size(), 3u);
	EXPECT_TRUE(has_bar(sink.bars, kSubDay, 20200102u));
	EXPECT_TRUE(has_bar(sink.bars, kMain, 20200106u));
	EXPECT_TRUE(has_bar(sink.bars, kSubDay, 20200106u));
	EXPECT_EQ(1u, sink.doneCalls);

	// run(true) 的日线 dump_btstate 周期名 "d"
	EXPECT_TRUE(std::filesystem::exists(_out / "r103_periods" / "btenv.json"));

	EXPECT_GE(loader.finalCalls[kUnsub], 1u);
}
