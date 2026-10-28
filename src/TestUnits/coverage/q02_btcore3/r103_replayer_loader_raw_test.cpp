/**
 * r103_replayer_loader_raw_test.cpp  (lane R103 · wave18)
 *
 * 靶子：HisDataReplayer 的 loader 抛异常与复权/原始K线装载公开面
 *   - get_kline_slice 捕获 loader 异常的两条 catch          HisDataReplayer.cpp:2229-2243
 *   - cacheRawBarsFromBin 的 loader loadRawHisBars 臂          :4525-4534
 *   - cacheAdjustedStkBarsFromBin 的 loader 原始K线臂          :4322-4334
 *   - 复权分钟线的 lastQTime/边界/日期比较                     :4291-4292、4306-4311、4361-4369
 *   - 因子日期晚于全部K线时的 continue                          :4408-4413
 *   - 复权K线不足/为空时的 break/continue 与空切片              :4277-4285、4347-4355、2407-2410
 *
 * 只走公开 API；loader 为测试内实现，不触真实存储以外的路径。
 */
#include "r103_common.h"

using R103Fixture = r91::Fixture;

namespace
{
	const char* const kThrowStd = "SSE.STK.600010";
	const char* const kThrowOther = "SSE.STK.600011";
	const char* const kAdjDay = "SSE.STK.600020";
	const char* const kAdjMin = "SSE.STK.600021";
	const char* const kRawDay = "SSE.STK.600024";
}

/*
 * 生产: HisDataReplayer.cpp:2229-2243（loader 抛 std::exception / ... 的 catch）
 *       4484-4534（cacheRawBarsFromBin 的 loader 原始K线臂）
 *       4236-4481（复权股票：loader 原始段 + 因子应用/跳过）
 *       4277-4285（复权块为空）、4347-4355（原始段缺失/空）
 *       2407-2410（首次定位落在数据末尾 -> NULL）
 */
TEST_F(R103Fixture, R103Replayer_LoaderThrowsAndRawAdjustedBars)
{
	r103::Loader loader;
	loader.throwStd[kThrowStd] = true;
	loader.throwOther[kThrowOther] = true;

	loader.rawBars[kAdjDay] = {
		r91::mk_bar(20191231u, 1500u, 100.0, 101.0, 99.0, 100.0, 10.0),
		r91::mk_bar(20200102u, 1500u, 200.0, 201.0, 199.0, 200.0, 20.0)
	};
	loader.factors[kAdjDay] = { std::make_pair(20200102u, 1.0) };

	loader.rawBars[kAdjMin] = {
		r91::mk_bar(20200102u, 931u, 10.0, 10.2, 9.8, 10.0, 1.0),
		r91::mk_bar(20200102u, 932u, 20.0, 20.2, 19.8, 20.0, 2.0)
	};
	loader.factors[kAdjMin] = {
		std::make_pair(20200102u, 1.0),
		std::make_pair(20200103u, 1.0)	// 晚于全部K线 -> pBar->date < bar.date, continue
	};

	loader.rawBars[kRawDay] = {
		r91::mk_bar(20191231u, 1500u, 300.0, 301.0, 299.0, 300.0, 30.0),
		r91::mk_bar(20200102u, 1500u, 301.0, 302.0, 300.0, 301.0, 31.0)
	};

	// 空块：只有 BlockHeader，读出来后 barcnt==0
	ASSERT_TRUE(r91::write_bars(_base, "SSE", "600023-", "day", {}));

	r91::CfgOpts opts;
	opts.mode = "bin";
	opts.tick = false;
	WTSVariant* cfg = r103::make_cfg(_root, _base, 202001020930ull, 202001021500ull, opts);
	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();

	r103::Sink sink;
	replayer.register_sink(&sink, "r103_loader_raw");
	ASSERT_TRUE(replayer.prepare());

	// 两条 loader 异常都被 get_kline_slice 吞掉并回落，最终无数据 -> NULL
	EXPECT_EQ(nullptr, replayer.get_kline_slice(kThrowStd, "d", 3, 1, false));
	EXPECT_EQ(nullptr, replayer.get_kline_slice(kThrowOther, "d", 3, 1, false));

	// 前复权日线：只给未复权段，因子 1.0 -> close 保持 100
	WTSKlineSlice* slice = replayer.get_kline_slice("SSE.STK.600020-", "d", 5, 1, false);
	ASSERT_NE(nullptr, slice);
	ASSERT_EQ(1, slice->size());
	EXPECT_DOUBLE_EQ(100.0, slice->at(0)->close);
	slice->release();
	EXPECT_EQ(1u, loader.rawCalls[kAdjDay]);

	// 前复权分钟线：raw loader + 晚间因子被 continue 跳过，0102 因子 1.0 生效
	slice = replayer.get_kline_slice("SSE.STK.600021-", "m", 5, 1, false);
	ASSERT_NE(nullptr, slice);
	ASSERT_EQ(1, slice->size());
	EXPECT_DOUBLE_EQ(10.0, slice->at(0)->close);
	slice->release();
	EXPECT_EQ(1u, loader.rawCalls[kAdjMin]);

	// 复权段和原始段都没有：函数返回 true 但缓存为空 -> 首次定位返回 NULL
	EXPECT_EQ(nullptr, replayer.get_kline_slice("SSE.STK.600022-", "d", 5, 1, false));
	// 复权段存在但 barcnt==0
	EXPECT_EQ(nullptr, replayer.get_kline_slice("SSE.STK.600023-", "d", 5, 1, false));
	// 普通日线走 cacheRawBarsFromBin 的 loader 原始臂
	slice = replayer.get_kline_slice(kRawDay, "d", 5, 1, false);
	ASSERT_NE(nullptr, slice);
	ASSERT_EQ(1, slice->size());
	EXPECT_DOUBLE_EQ(300.0, slice->at(0)->close);
	slice->release();
	EXPECT_EQ(1u, loader.rawCalls[kRawDay]);
}
