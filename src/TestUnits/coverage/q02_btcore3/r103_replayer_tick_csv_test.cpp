/**
 * r103_replayer_tick_csv_test.cpp  (lane R103 · wave18)
 *
 * 靶子：
 *   - proc_block_data 对 RAW_V2 + bKeepHead=false 的直通臂   HisDataReplayer.cpp:48-61
 *   - cacheRawTicksFromCSV 读取 bin/ticks/<code>_tick_<date>.dsb :3470-3504
 *   - get_tick_slice etime 落在两笔之间时的回退                 :2497-2519
 *   - get_kline_slice 分钟首次定位：未来首根回退/全历史返回 NULL :2393-2410
 *
 * 只走公开 API；tick dsb 由测试按 RAW_V2 块写入临时目录。
 */
#include "r103_common.h"

using R103Fixture = r91::Fixture;

namespace
{
	const char* const kCode = "SSE.STK.000001";
	const char* const kFuture = "SSE.STK.000010";
	const char* const kPast = "SSE.STK.000011";
}

/*
 * 生产: HisDataReplayer.cpp:48-61（RAW_V2 无块头直通）
 *       3470-3504（csv 模式的 bin/ticks dsb 读取）
 *       2497-2519（etime 位于 09:31/09:33 之间 -> lower_bound 回退）
 */
TEST_F(R103Fixture, R103Replayer_CsvTickDsbAndSliceFallback)
{
	std::vector<WTSTickStruct> ticks;
	ticks.push_back(r91::mk_tick(20200102u, 931u, 10.0, 100.0, "000001"));
	ticks.push_back(r91::mk_tick(20200102u, 933u, 11.0, 100.0, "000001"));
	ASSERT_TRUE(r91::write_block(_base / "bin/ticks/SSE.STK.000001_tick_20200102.dsb",
		BT_HIS_Ticks, ticks));

	r91::CfgOpts opts;
	opts.mode = "csv";
	opts.tick = true;
	WTSVariant* cfg = r103::make_cfg(_root, _base, 202001020930ull, 202001021500ull, opts);
	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg, NULL, NULL));
	cfg->release();

	r103::Sink sink;
	replayer.register_sink(&sink, "r103_csv_tick");
	ASSERT_TRUE(replayer.prepare());

	// 09:32 落在两笔之间：lower_bound 命中 09:33，回退后 cursor==1 -> NULL
	EXPECT_EQ(nullptr, replayer.get_tick_slice(kCode, 2u, 202001020932ull));
	// 复位 tick 缓存后按 09:33 重新定位，取回 09:31 一笔
	replayer.clear_cache();
	WTSTickSlice* slice = replayer.get_tick_slice(kCode, 2u, 202001020933ull);
	ASSERT_NE(nullptr, slice);
	ASSERT_EQ(1, slice->size());
	EXPECT_EQ(931u, slice->at(0)->action_time / 100000u);
	EXPECT_DOUBLE_EQ(10.0, slice->at(0)->price);
	slice->release();

	// 分钟首次定位边界（另一实例，避免 tick 游标互相影响）
	{
		r103::Loader loader;
		loader.m1Bars[kFuture] = {
			r91::mk_bar(20200102u, 925u, 5.0, 6.0, 4.0, 5.5, 10.0),
			r91::mk_bar(20200102u, 931u, 6.0, 7.0, 5.0, 6.5, 20.0),
			r91::mk_bar(20200102u, 932u, 7.0, 8.0, 6.0, 7.5, 30.0)
		};
		loader.m1Bars[kPast] = {
			r91::mk_bar(20200102u, 925u, 8.0, 9.0, 7.0, 8.5, 10.0),
			r91::mk_bar(20200102u, 927u, 9.0, 10.0, 8.0, 9.5, 20.0)
		};

		r91::CfgOpts mopts;
		mopts.mode = "bin";
		mopts.tick = true;
		WTSVariant* mcfg = r103::make_cfg(_root, _base, 202001020930ull, 202001021500ull, mopts);
		HisDataReplayer locator;
		ASSERT_TRUE(locator.init(mcfg, NULL, &loader));
		mcfg->release();

		r103::Sink lsink;
		locator.register_sink(&lsink, "r103_first_locate");
		ASSERT_TRUE(locator.prepare());

		// 首根 09:25 早于当前 09:30，lower_bound 命中 09:31 -> 回退到 09:25
		WTSKlineSlice* first = locator.get_kline_slice(kFuture, "m", 1, 1, false);
		ASSERT_NE(nullptr, first);
		ASSERT_EQ(1, first->size());
		EXPECT_DOUBLE_EQ(5.5, first->at(0)->close);
		first->release();

		// 全部K线都早于当前 09:30，lower_bound 返回 end -> NULL
		EXPECT_EQ(nullptr, locator.get_kline_slice(kPast, "m", 1, 1, false));
	}
}
