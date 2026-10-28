/**
 * r103_replayer_minute_slice_test.cpp  (lane R103 · wave18)
 *
 * 靶子：分钟K线回放的切片定位/游标推进与日价格缓存
 *   - get_kline_slice 分钟后置定位的游标前进与 sIdx   HisDataReplayer.cpp:2441-2463
 *   - simTickWithUnsubBars 分钟臂的游标跳过            :1389-1460
 *   - simTicks 分钟臂生成 _day_cache 后的 get_day_price :1243-1387、3034-3087
 *
 * 只走公开 API；主K线与未订阅合约数据都由测试 loader 提供。
 */
#include "r103_common.h"

using R103Fixture = r91::Fixture;

namespace
{
	const char* const kMain = "SSE.STK.000001";
	const char* const kUnsub = "SSE.STK.000002";
}

/*
 * 生产: HisDataReplayer.cpp:2441-2463（minutes 游标在第二次切片时前进）
 *       1389-1460（未订阅分钟K线游标跳过 09:31）
 *       1243-1387（分钟 simTicks 开高低收）
 *       3034-3087（tick 关闭时 get_day_price 读 _day_cache）
 */
TEST_F(R103Fixture, R103Replayer_MinuteSliceCursorAndDayPrice)
{
	r103::Loader loader;
	loader.m1Bars[kMain] = {
		r91::mk_bar(20200102u, 931u, 9.0, 10.0, 8.0, 9.5, 100.0),
		r91::mk_bar(20200102u, 932u, 10.0, 12.0, 9.0, 11.0, 200.0),
		r91::mk_bar(20200102u, 935u, 11.0, 13.0, 10.0, 12.0, 300.0)
	};
	loader.m1Bars[kUnsub] = {
		r91::mk_bar(20200102u, 925u, 5.0, 6.0, 4.0, 5.5, 10.0),
		r91::mk_bar(20200102u, 931u, 6.0, 7.0, 5.0, 6.5, 20.0),
		r91::mk_bar(20200102u, 932u, 7.0, 8.0, 6.0, 7.5, 30.0)
	};

	r91::CfgOpts opts;
	opts.mode = "bin";
	opts.tick = false;
	WTSVariant* cfg = r103::make_cfg(_root, _base, 202001020930ull, 202001021500ull, opts);
	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();

	r103::Sink sink;
	sink.rp = &replayer;
	sink.collectDayPrice = true;
	sink.dayPriceCode = kMain;
	sink.onBarClose = [&replayer](const r91::BarRec& rec) {
		if (rec.code != kMain || rec.time != 932u)
			return;
		// 第二次切片：cursor 已初始化，必须前进到 09:32 再截取
		WTSKlineSlice* slice = replayer.get_kline_slice(kMain, "m", 1, 1, false);
		if (slice != NULL)
		{
			EXPECT_EQ(1, slice->size());
			EXPECT_DOUBLE_EQ(11.0, slice->at(0)->close);
			slice->release();
		}
	};
	replayer.register_sink(&sink, "r103_min_slice");
	ASSERT_TRUE(replayer.prepare());

	WTSKlineSlice* mainSlice = replayer.get_kline_slice(kMain, "m", 1, 1, true);
	ASSERT_NE(nullptr, mainSlice);
	ASSERT_EQ(1, mainSlice->size());
	EXPECT_DOUBLE_EQ(9.5, mainSlice->at(0)->close);
	mainSlice->release();

	replayer.sub_tick(3u, kUnsub);

	replayer.run(false);

	// 09:31 被初始游标当历史，只回放 09:32；随后第二次切片又推进到 09:35 之前
	ASSERT_EQ(1u, sink.bars.size());
	EXPECT_EQ(932u, sink.bars[0].time);
	EXPECT_DOUBLE_EQ(11.0, sink.bars[0].close);

	// 未订阅合约在 09:32 跳过 09:25/09:31 两段游标后模拟 tick
	uint32_t unsubTicks = 0;
	for (const r91::TickRec& rec : sink.ticks)
	{
		if (rec.code == kUnsub)
			unsubTicks++;
	}
	ASSERT_EQ(4u, unsubTicks);

	// 09:32 收盘 tick 时 _day_cache 已是该分钟的 OHLC
	auto it = sink.dayPrices.find(932u);
	ASSERT_NE(sink.dayPrices.end(), it);
	ASSERT_EQ(5u, it->second.size());
	EXPECT_DOUBLE_EQ(10.0, it->second[0]);
	EXPECT_DOUBLE_EQ(12.0, it->second[1]);
	EXPECT_DOUBLE_EQ(9.0, it->second[2]);
	EXPECT_DOUBLE_EQ(11.0, it->second[3]);
	EXPECT_DOUBLE_EQ(0.0, it->second[4]);
}
