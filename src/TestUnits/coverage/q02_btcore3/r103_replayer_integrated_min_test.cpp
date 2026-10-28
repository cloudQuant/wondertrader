/**
 * r103_replayer_integrated_min_test.cpp  (lane R103 · wave18)
 *
 * 靶子：cacheIntegratedFutBarsFromBin 的分钟线/后复权公开面（经 get_kline_slice 进入）
 *   - 分钟主段的 pname/lastHotTime                       HisDataReplayer.cpp:3910-3916、3968-3969
 *   - 分钟分段边界 getBoundaryTime 与 bAllCovered           :4015-4034
 *   - 分月合约经 loader loadRawHisBars 装载                 :4056-4068
 *   - 分钟 lower_bound 的 period 分支与右边界回退            :4090-4123
 *   - 后复权（_exright==2）的 _factor 记录                   :4000-4004
 *   - 主段+分段倒序拼接与回放                               :4166-4187、771-938
 *
 * 只走公开 API；规则文件、K线文件与 loader 全部在临时目录/内存。
 */
#include "r103_common.h"

using R103Fixture = r91::Fixture;

namespace
{
	const char* const kHotMinus = "CFFEX.IF.HOT-";
	const char* const kHotPlus = "CFFEX.IF.HOT+";
	const uint32_t kDay1 = 20200102u;
	const uint32_t kDay2 = 20200106u;

	std::vector<WTSBarStruct> hot_minute_bars()
	{
		std::vector<WTSBarStruct> bars;
		bars.push_back(r91::mk_bar(kDay1, 930u, 99.0, 100.0, 98.0, 100.0, 10.0));
		bars.push_back(r91::mk_bar(kDay1, 1500u, 100.0, 106.0, 99.0, 105.0, 20.0));
		bars.push_back(r91::mk_bar(kDay2, 1000u, 105.0, 111.0, 104.0, 110.0, 30.0));
		return bars;
	}

	std::vector<WTSBarStruct> if2002_minute_bars()
	{
		std::vector<WTSBarStruct> bars;
		bars.push_back(r91::mk_bar(kDay2, 930u, 390.0, 401.0, 389.0, 400.0, 100.0));
		bars.push_back(r91::mk_bar(kDay2, 1400u, 400.0, 411.0, 399.0, 410.0, 110.0));
		bars.push_back(r91::mk_bar(20200107u, 930u, 410.0, 421.0, 409.0, 420.0, 120.0));
		return bars;
	}
}

/*
 * 生产: HisDataReplayer.cpp:3900-4190（分钟主力连续：主段 + 分月段）
 *       3910-3916、3968-3969、4000-4004、4015-4034、4056-4068、4090-4123
 *       4166-4187（倒序拼接）、771-938（run_by_bars 分钟回放）
 */
TEST_F(R103Fixture, R103Replayer_IntegratedMinuteHotBars)
{
	ASSERT_TRUE(r91::write_bars(_base, "CFFEX", "CFFEX.IF_HOT-", "min1", hot_minute_bars()));
	ASSERT_TRUE(r91::write_bars(_base, "CFFEX", "CFFEX.IF_HOT+", "min1", hot_minute_bars()));

	const std::filesystem::path rules = _base / "rules_hot.json";
	ASSERT_TRUE(r91::write_text(rules,
		"{\"CFFEX\":{\"IF\":["
		"{\"date\":20200103,\"from\":\"\",\"to\":\"IF2001\",\"oldclose\":0.0,\"newclose\":100.0},"
		"{\"date\":20200106,\"from\":\"IF2001\",\"to\":\"IF2002\",\"oldclose\":400.0,\"newclose\":300.0}"
		"]}}"));

	r103::Loader loader;
	loader.rawBars["CFFEX.IF.2001"] = {
		r91::mk_bar(20200103u, 930u, 290.0, 301.0, 289.0, 300.0, 90.0)
	};
	loader.rawBars["CFFEX.IF.2002"] = if2002_minute_bars();

	r91::CfgOpts opts;
	opts.mode = "bin";
	opts.tick = false;
	opts.commodities.push_back((_root / "dist/common/stk_comms.json").string());
	opts.commodities.push_back((_root / "dist/common/commodities.json").string());
	opts.rules.push_back(std::make_pair(std::string("HOT"), rules.string()));

	WTSVariant* cfg = r103::make_cfg(_root, _base, 202001020930ull, 202001071500ull, opts);
	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();

	r103::Sink sink;
	replayer.register_sink(&sink, "r103_hot_min");
	ASSERT_TRUE(replayer.prepare());

	WTSKlineSlice* slice = replayer.get_kline_slice(kHotMinus, "m", 10, 1, true);
	ASSERT_NE(nullptr, slice);
	ASSERT_EQ(1, slice->size());
	EXPECT_DOUBLE_EQ(100.0, slice->at(0)->close);
	slice->release();

	// 0106 09:30 早于主力最后一根 10:00 -> bAllCovered，分段只取 IF2002 的 14:00 一根
	replayer.run(false);

	ASSERT_EQ(3u, sink.bars.size());
	EXPECT_EQ(kDay1, sink.bars[0].date);
	EXPECT_EQ(1500u, sink.bars[0].time);
	EXPECT_DOUBLE_EQ(105.0, sink.bars[0].close);
	EXPECT_EQ(kDay2, sink.bars[1].date);
	EXPECT_EQ(1000u, sink.bars[1].time);
	EXPECT_DOUBLE_EQ(110.0, sink.bars[1].close);
	EXPECT_EQ(kDay2, sink.bars[2].date);
	EXPECT_EQ(1400u, sink.bars[2].time);
	EXPECT_DOUBLE_EQ(410.0, sink.bars[2].close);
	EXPECT_EQ(1u, loader.rawCalls["CFFEX.IF.2002"]);
	EXPECT_EQ(0u, loader.rawCalls["CFFEX.IF.2001"]);

	// 后复权（+）：走 L4004 的 _factor 记录，主段不缩放、分段按 0.75 缩放
	slice = replayer.get_kline_slice(kHotPlus, "m", 10, 1, false);
	ASSERT_NE(nullptr, slice);
	ASSERT_EQ(4, slice->size());
	EXPECT_DOUBLE_EQ(100.0, slice->at(0)->close);
	EXPECT_DOUBLE_EQ(307.5, slice->at(3)->close);
	slice->release();
}
