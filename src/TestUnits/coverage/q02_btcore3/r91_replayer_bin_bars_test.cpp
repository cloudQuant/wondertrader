/**
 * r91_replayer_bin_bars_test.cpp  (lane R91 · wave17)
 *
 * 靶子：HisDataReplayer 的 bin 存储装载面（公开 API：init/prepare/get_kline_slice/run）
 *   - cacheRawBarsFromBin 正常日线装载              HisDataReplayer.cpp:4484-4605
 *   - 装载失败的 NULL 切片与空数据日期保持           :4560-4567、2287-2288
 *   - cacheAdjustedStkBarsFromBin 前/后复权拼接      :4236-4481
 *   - getAdjFactors 按需向 IBtDataLoader 取因子      :4192-4234
 *   - cacheIntegratedFutBarsFromBin 主力连续合并     :3900-4190
 *   - run_by_bars 日线推进/闭盘                      :771-938
 *
 * 只使用公开 API；不碰私有 bin/cache*FromBin 声明，不镜像类布局。
 */
#include "r91_common.h"

using R91Fixture = r91::Fixture;

namespace
{
	const char* const kStock = "SSE.STK.600000";
	const uint32_t kDay1 = 20200102u;	// 周四
	const uint32_t kDay2 = 20200103u;	// 周五
	const uint32_t kDay3 = 20200106u;	// 周一
	const uint32_t kDay4 = 20200107u;	// 周二

	const r91::BarRec* bar_of(const r91::Sink& sink, uint32_t date)
	{
		for (const r91::BarRec& bar : sink.bars)
		{
			if (bar.date == date)
				return &bar;
		}
		return NULL;
	}
}

/*
 * 生产: HisDataReplayer.cpp:4484-4605（cacheRawBarsFromBin 正常臂）
 *       2287-2288、2336-2411（首次定位）、2461-2467（切片）
 *       771-938（run_by_bars 日线闭盘）
 */
TEST_F(R91Fixture, R91BinBars_RawDailyBarsSliceAndReplay)
{
	std::vector<WTSBarStruct> bars;
	bars.push_back(r91::mk_bar(20191231u, 1500u, 99.0, 100.0, 98.0, 100.0, 10.0));
	bars.push_back(r91::mk_bar(kDay1, 1500u, 100.0, 102.0, 99.0, 101.0, 20.0));
	bars.push_back(r91::mk_bar(kDay2, 1500u, 101.0, 103.0, 100.0, 102.0, 30.0));
	ASSERT_TRUE(r91::write_bars(_base, "SSE", "600000", "day", bars));

	r91::CfgOpts opts;
	opts.mode = "bin";
	opts.tick = false;
	WTSVariant* cfg = r91::make_cfg(_root, _base, 202001020930ull, 202001031500ull, opts);

	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg, NULL, NULL));
	cfg->release();

	ASSERT_NE(nullptr, replayer.get_commodity_info(kStock));
	ASSERT_NE(nullptr, replayer.get_session_info(kStock, true));
	ASSERT_NE(nullptr, replayer.get_session_info("SD0930", false));

	r91::Sink sink;
	replayer.register_sink(&sink, "r91_raw");

	ASSERT_TRUE(replayer.prepare());
	EXPECT_EQ(1u, sink.initCalls);

	// 首次定位：首根是 20191231，当前交易日 20200102 -> 游标停在 1，切片给 1 根
	WTSKlineSlice* slice = replayer.get_kline_slice(kStock, "d", 3, 1, false);
	ASSERT_NE(nullptr, slice);
	ASSERT_EQ(1, slice->size());
	EXPECT_DOUBLE_EQ(100.0, slice->at(0)->close);
	slice->release();

	replayer.run(false);

	ASSERT_EQ(2u, sink.bars.size());
	const r91::BarRec* first = bar_of(sink, kDay1);
	const r91::BarRec* second = bar_of(sink, kDay2);
	ASSERT_NE(nullptr, first);
	ASSERT_NE(nullptr, second);
	EXPECT_DOUBLE_EQ(101.0, first->close);
	EXPECT_DOUBLE_EQ(102.0, second->close);
	EXPECT_EQ(kDay2, replayer.get_date());
	EXPECT_EQ(kDay2, replayer.get_trading_date());
}

/*
 * 生产: HisDataReplayer.cpp:4497-4500（进入即建空缓存）、4536-4567（loader/存储都失败）
 *       2287-2288（返回 NULL 切片）；prepare 不改变时间轴 661-664
 */
TEST_F(R91Fixture, R91BinBars_MissingBinBarsYieldNullSlice)
{
	r91::CfgOpts opts;
	opts.mode = "bin";
	opts.tick = false;
	WTSVariant* cfg = r91::make_cfg(_root, _base, 202001020930ull, 202001031500ull, opts);

	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg, NULL, NULL));
	cfg->release();

	r91::Sink sink;
	replayer.register_sink(&sink, "r91_missing");
	ASSERT_TRUE(replayer.prepare());

	EXPECT_EQ(nullptr, replayer.get_kline_slice(kStock, "d", 3, 1, false));
	EXPECT_EQ(kDay1, replayer.get_date());
	EXPECT_EQ(kDay1, replayer.get_trading_date());
	EXPECT_EQ(0u, sink.bars.size());
}

/*
 * 生产: HisDataReplayer.cpp:4236-4481（复权股票：已复权段 + 未复权段拼接）
 *       4192-4234（getAdjFactors 走 IBtDataLoader）、4484-4517（按后缀分流）
 *       2298-2326 不触发（times==1）
 */
TEST_F(R91Fixture, R91BinBars_AdjustedForwardBarsConcatenate)
{
	std::vector<WTSBarStruct> adjusted;
	adjusted.push_back(r91::mk_bar(20191230u, 1500u, 10.0, 10.2, 9.8, 10.0, 100.0));
	adjusted.push_back(r91::mk_bar(20191231u, 1500u, 10.5, 10.7, 10.3, 10.5, 110.0));
	ASSERT_TRUE(r91::write_bars(_base, "SSE", "600000-", "day", adjusted));

	std::vector<WTSBarStruct> raw;
	raw.push_back(r91::mk_bar(kDay1, 1500u, 20.0, 20.5, 19.5, 20.0, 200.0));
	raw.push_back(r91::mk_bar(kDay2, 1500u, 21.0, 21.5, 20.5, 21.0, 210.0));
	ASSERT_TRUE(r91::write_bars(_base, "SSE", "600000", "day", raw));

	r91::Loader loader;
	loader.factors["SSE.STK.600000"].push_back(std::make_pair(20191231u, 0.5));

	r91::CfgOpts opts;
	opts.mode = "bin";
	opts.tick = false;
	opts.adjustFlag = 7;
	WTSVariant* cfg = r91::make_cfg(_root, _base, 202001020930ull, 202001031500ull, opts);

	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();

	r91::Sink sink;
	replayer.register_sink(&sink, "r91_fwd");
	ASSERT_TRUE(replayer.prepare());

	// 已复权两段 20191230/31 与未复权两段 20200102/03 按日期拼接
	WTSKlineSlice* slice = replayer.get_kline_slice("SSE.STK.600000-", "d", 10, 1, false);
	ASSERT_NE(nullptr, slice);
	ASSERT_EQ(2, slice->size());
	EXPECT_DOUBLE_EQ(10.0, slice->at(0)->close);
	EXPECT_DOUBLE_EQ(10.5, slice->at(1)->close);
	slice->release();

	replayer.run(false);

	ASSERT_EQ(2u, sink.bars.size());
	EXPECT_EQ(kDay1, sink.bars[0].date);
	EXPECT_DOUBLE_EQ(20.0, sink.bars[0].close);
	EXPECT_EQ(kDay2, sink.bars[1].date);
	EXPECT_DOUBLE_EQ(21.0, sink.bars[1].close);
}

/*
 * 生产: HisDataReplayer.cpp:4271-4297（复权块缺失 -> 只走未复权段）
 *       4399-4446（因子倒序应用，adjust_flag 1/2/4 全部生效）
 *       4448-4451（无因子时跳过复权）、4484-4517（按后缀分流）
 */
TEST_F(R91Fixture, R91BinBars_AdjustedBackwardBarsApplyFactorAndSkipMissingAdjusted)
{
	std::vector<WTSBarStruct> raw;
	raw.push_back(r91::mk_bar(20191230u, 1500u, 10.0, 10.2, 9.8, 10.0, 100.0));
	raw.push_back(r91::mk_bar(kDay1, 1500u, 20.0, 20.5, 19.5, 20.0, 200.0));
	ASSERT_TRUE(r91::write_bars(_base, "SSE", "600001", "day", raw));

	{
		// 后复权 + 无 600001+.dsb：整段按 0.5 缩放
		r91::Loader loader;
		loader.factors["SSE.STK.600001"].push_back(std::make_pair(20191230u, 0.5));

		r91::CfgOpts opts;
		opts.mode = "bin";
		opts.tick = false;
		opts.adjustFlag = 7;
		WTSVariant* cfg = r91::make_cfg(_root, _base, 202001020930ull, 202001021500ull, opts);

		HisDataReplayer replayer;
		ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
		cfg->release();

		r91::Sink sink;
		replayer.register_sink(&sink, "r91_bwd");
		ASSERT_TRUE(replayer.prepare());

		WTSKlineSlice* slice = replayer.get_kline_slice("SSE.STK.600001+", "d", 5, 1, false);
		ASSERT_NE(nullptr, slice);
		ASSERT_EQ(1, slice->size());
		EXPECT_DOUBLE_EQ(5.0, slice->at(0)->close);
		EXPECT_DOUBLE_EQ(200.0, slice->at(0)->vol);
		EXPECT_DOUBLE_EQ(500.0, slice->at(0)->money);
		EXPECT_DOUBLE_EQ(100.0, slice->at(0)->hold);
		EXPECT_DOUBLE_EQ(10.0, slice->at(0)->add);
		slice->release();

		replayer.run(false);
		ASSERT_EQ(1u, sink.bars.size());
		EXPECT_EQ(kDay1, sink.bars[0].date);
		EXPECT_DOUBLE_EQ(10.0, sink.bars[0].close);
		EXPECT_DOUBLE_EQ(400.0, sink.bars[0].vol);
		EXPECT_DOUBLE_EQ(2000.0, sink.bars[0].money);
		EXPECT_DOUBLE_EQ(200.0, sink.bars[0].hold);
		EXPECT_DOUBLE_EQ(20.0, sink.bars[0].add);
	}

	{
		// 后复权 + loader 无该合约因子：跳过复权，K线保持原值
		std::vector<WTSBarStruct> raw2;
		raw2.push_back(r91::mk_bar(20191230u, 1500u, 10.0, 10.2, 9.8, 10.0, 100.0));
		raw2.push_back(r91::mk_bar(kDay1, 1500u, 20.0, 20.5, 19.5, 20.0, 200.0));
		ASSERT_TRUE(r91::write_bars(_base, "SSE", "600002", "day", raw2));

		r91::Loader loader;
		r91::CfgOpts opts;
		opts.mode = "bin";
		opts.tick = false;
		WTSVariant* cfg = r91::make_cfg(_root, _base, 202001020930ull, 202001021500ull, opts);

		HisDataReplayer replayer;
		ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
		cfg->release();

		r91::Sink sink;
		replayer.register_sink(&sink, "r91_nofact");
		ASSERT_TRUE(replayer.prepare());

		WTSKlineSlice* slice = replayer.get_kline_slice("SSE.STK.600002+", "d", 5, 1, false);
		ASSERT_NE(nullptr, slice);
		ASSERT_EQ(1, slice->size());
		EXPECT_DOUBLE_EQ(10.0, slice->at(0)->close);
		EXPECT_DOUBLE_EQ(100.0, slice->at(0)->vol);
		slice->release();

		replayer.run(false);
		ASSERT_EQ(1u, sink.bars.size());
		EXPECT_DOUBLE_EQ(20.0, sink.bars[0].close);
	}
}

/*
 * 生产: HisDataReplayer.cpp:3900-4190（custom rule 主力连续：主段 + 分月段合并）
 *       3946-3977（主段 CFFEX.IF_HOT- 读取）、4001-4004（前复权基础因子）
 *       4007-4164（倒序分段、边界裁剪、复权与 adjust_flag）、4166-4187（组装）
 *       4192-4234 不触发（区间因子来自规则，不走股票因子表）
 */
TEST_F(R91Fixture, R91BinBars_IntegratedForwardHotBarsMergeSections)
{
	// 主力段：2019/12/31、2020/01/02；分月：IF2001(0103)、IF2002(0106/0107)
	std::vector<WTSBarStruct> hot;
	hot.push_back(r91::mk_bar(20191231u, 1500u, 99.0, 100.0, 98.0, 100.0, 10.0));
	hot.push_back(r91::mk_bar(kDay1, 1500u, 100.0, 102.0, 99.0, 101.0, 10.0));
	ASSERT_TRUE(r91::write_bars(_base, "CFFEX", "CFFEX.IF_HOT-", "day", hot));

	std::vector<WTSBarStruct> if2001;
	if2001.push_back(r91::mk_bar(kDay2, 1500u, 200.0, 210.0, 190.0, 200.0, 100.0));
	ASSERT_TRUE(r91::write_bars(_base, "CFFEX", "IF2001", "day", if2001));

	std::vector<WTSBarStruct> if2002;
	if2002.push_back(r91::mk_bar(kDay3, 1500u, 300.0, 310.0, 290.0, 300.0, 100.0));
	if2002.push_back(r91::mk_bar(kDay4, 1500u, 310.0, 320.0, 300.0, 310.0, 100.0));
	ASSERT_TRUE(r91::write_bars(_base, "CFFEX", "IF2002", "day", if2002));

	const std::string rules = _base.string() + "/rules_hot.json";
	ASSERT_TRUE(r91::write_text(rules,
		"{\"CFFEX\":{\"IF\":["
		"{\"date\":20200103,\"from\":\"\",\"to\":\"IF2001\",\"oldclose\":0.0,\"newclose\":100.0},"
		"{\"date\":20200106,\"from\":\"IF2001\",\"to\":\"IF2002\",\"oldclose\":200.0,\"newclose\":100.0}"
		"]}}"));

	r91::CfgOpts opts;
	opts.mode = "bin";
	opts.tick = false;
	opts.adjustFlag = 7;
	opts.commodities.push_back((_root / "dist/common/stk_comms.json").string());
	opts.commodities.push_back((_root / "dist/common/commodities.json").string());
	opts.rules.push_back(std::make_pair(std::string("HOT"), rules));

	WTSVariant* cfg = r91::make_cfg(_root, _base, 202001020930ull, 202001071500ull, opts);

	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg, NULL, NULL));
	cfg->release();

	r91::Sink sink;
	replayer.register_sink(&sink, "r91_hot");
	ASSERT_TRUE(replayer.prepare());

	WTSKlineSlice* slice = replayer.get_kline_slice("CFFEX.IF.HOT-", "d", 10, 1, true);
	ASSERT_NE(nullptr, slice);
	slice->release();

	replayer.run(false);

	ASSERT_EQ(4u, sink.bars.size());
	EXPECT_EQ(kDay1, sink.bars[0].date);
	EXPECT_DOUBLE_EQ(101.0, sink.bars[0].close);	// 主力段原值
	EXPECT_EQ(kDay2, sink.bars[1].date);
	EXPECT_DOUBLE_EQ(100.0, sink.bars[1].close);	// 200 * (1/2)
	EXPECT_DOUBLE_EQ(200.0, sink.bars[1].vol);		// 100 / 0.5
	EXPECT_DOUBLE_EQ(10000.0, sink.bars[1].money);	// 20000 * 0.5
	EXPECT_DOUBLE_EQ(100.0, sink.bars[1].hold);		// 50 / 0.5
	EXPECT_DOUBLE_EQ(10.0, sink.bars[1].add);		// 5 / 0.5
	EXPECT_EQ(kDay3, sink.bars[2].date);
	EXPECT_DOUBLE_EQ(300.0, sink.bars[2].close);	// 第二段因子 2/2
	EXPECT_EQ(kDay4, sink.bars[3].date);
	EXPECT_DOUBLE_EQ(310.0, sink.bars[3].close);
}
