/**
 * r103_replayer_loader_dsb_test.cpp  (lane R103 · wave18)
 *
 * 靶子：HisDataReplayer 的 loader 最终K线 + 自动转储/回读公开面
 *   - cacheFinalBarsFromLoader 的 ruleTag 文件名臂（含复权后缀）  HisDataReplayer.cpp:3617-3623
 *   - isAutoTrans=true：首次 loader 落盘、clear_cache 后从 dsb 直读 :3634-3668
 *   - m5 自动转储的 BlockType 选择                              :3704-3712
 *
 * 只走公开 API（init/prepare/get_kline_slice/clear_cache）；落盘仅在临时目录。
 */
#include "r103_common.h"

#include <filesystem>

using R103Fixture = r91::Fixture;

namespace
{
	const char* const kStock = "SSE.STK.600000";
	const char* const kM5 = "SSE.STK.600002";
	const char* const kHot = "CFFEX.IF.HOT-";

	std::vector<WTSBarStruct> two_day_bars(double base)
	{
		std::vector<WTSBarStruct> bars;
		bars.push_back(r91::mk_bar(20191231u, 1500u, base, base + 1.0, base - 1.0, base, 10.0));
		bars.push_back(r91::mk_bar(20200102u, 1500u, base + 1.0, base + 2.0, base, base + 1.0, 20.0));
		return bars;
	}
}

/*
 * 生产: HisDataReplayer.cpp:3617-3623（ruleTag + exright 的 dsb 文件名）
 *       3634-3668（autoTrans 命中已有 dsb 的直读臂）
 *       3704-3712（m5 转储 BlockType）
 *       2287-2295（装载成功后的 session 校验）
 */
TEST_F(R103Fixture, R103Replayer_LoaderAutoTransDsbAndRuleFileName)
{
	r103::Loader loader;
	loader.autoTrans = true;
	loader.dayBars[kStock] = two_day_bars(100.0);
	loader.dayBars[kHot] = two_day_bars(200.0);
	loader.m5Bars[kM5] = {
		r91::mk_bar(20200102u, 935u, 10.0, 10.2, 9.8, 10.1, 100.0),
		r91::mk_bar(20200102u, 940u, 10.1, 10.3, 9.9, 10.2, 110.0),
		r91::mk_bar(20200102u, 945u, 10.2, 10.4, 10.0, 10.3, 120.0)
	};

	const std::filesystem::path rules = _base / "rules_hot.json";
	ASSERT_TRUE(r91::write_text(rules,
		"{\"CFFEX\":{\"IF\":["
		"{\"date\":20200103,\"from\":\"\",\"to\":\"IF2001\",\"oldclose\":0.0,\"newclose\":100.0}"
		"]}}"));

	r91::CfgOpts opts;
	opts.mode = "bin";
	opts.tick = false;
	opts.commodities.push_back((_root / "dist/common/stk_comms.json").string());
	opts.commodities.push_back((_root / "dist/common/commodities.json").string());
	opts.rules.push_back(std::make_pair(std::string("HOT"), rules.string()));

	WTSVariant* cfg = r103::make_cfg(_root, _base, 202001020930ull, 202001021500ull, opts);
	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();

	r103::Sink sink;
	replayer.register_sink(&sink, "r103_autotrans");
	ASSERT_TRUE(replayer.prepare());

	// 第一次：loader 给出日线，并转储 his/day/SSE/600000.dsb
	WTSKlineSlice* slice = replayer.get_kline_slice(kStock, "d", 5, 1, false);
	ASSERT_NE(nullptr, slice);
	ASSERT_EQ(1, slice->size());
	EXPECT_DOUBLE_EQ(100.0, slice->at(0)->close);
	slice->release();
	ASSERT_TRUE(std::filesystem::exists(_base / "his/day/SSE/600000.dsb"));
	EXPECT_EQ(1u, loader.finalCalls[kStock]);

	// 第二次：缓存已清空，但 dsb 存在 -> 直接解压读取（L3634-3668），不再调 loader
	replayer.clear_cache();
	slice = replayer.get_kline_slice(kStock, "d", 5, 1, false);
	ASSERT_NE(nullptr, slice);
	ASSERT_EQ(1, slice->size());
	EXPECT_DOUBLE_EQ(100.0, slice->at(0)->close);
	slice->release();
	EXPECT_EQ(1u, loader.finalCalls[kStock]);

	// ruleTag + 前复权：文件名必须是 CFFEX.IF_HOT-.dsb
	slice = replayer.get_kline_slice(kHot, "d", 5, 1, false);
	ASSERT_NE(nullptr, slice);
	ASSERT_EQ(1, slice->size());
	EXPECT_DOUBLE_EQ(200.0, slice->at(0)->close);
	slice->release();
	ASSERT_TRUE(std::filesystem::exists(_base / "his/day/CFFEX/CFFEX.IF_HOT-.dsb"));
	EXPECT_EQ(1u, loader.finalCalls[kHot]);

	replayer.clear_cache();
	slice = replayer.get_kline_slice(kHot, "d", 5, 1, false);
	ASSERT_NE(nullptr, slice);
	ASSERT_EQ(1, slice->size());
	EXPECT_DOUBLE_EQ(200.0, slice->at(0)->close);
	slice->release();
	EXPECT_EQ(1u, loader.finalCalls[kHot]);

	// m5 自动转储走 BT_HIS_Minute5 分支
	slice = replayer.get_kline_slice(kM5, "m", 5, 1, false);
	ASSERT_NE(nullptr, slice);
	EXPECT_GE(slice->size(), 1);
	slice->release();
	ASSERT_TRUE(std::filesystem::exists(_base / "his/min5/SSE/600002.dsb"));
	EXPECT_EQ(1u, loader.finalCalls[kM5]);
}
