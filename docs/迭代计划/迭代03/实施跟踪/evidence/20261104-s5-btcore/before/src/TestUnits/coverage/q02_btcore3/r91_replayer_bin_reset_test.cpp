/**
 * r91_replayer_bin_reset_test.cpp  (lane R91 · wave17)
 *
 * 靶子：
 *   - prepare -> reset 对已装载的 bars/tick/Level2 四类缓存游标复位
 *     HisDataReplayer.cpp:459-516、649-675
 *   - clear_cache 清空后再次从 bin 存储装载                 :436-457
 *   - 空代码/空数据/无 tick 模式下的公开查询与费率计算       :2828-2879、2946-3019
 *   - get_day_price 的空 day_cache 分支、get_cur_price       :3034-3087、3021-3032
 *   - get_rawcode 的 custom rule 分支                        :2921-2931
 *
 * 只使用公开 API；不碰私有 bin/cache*FromBin 声明，不镜像类布局。
 */
#include "r91_common.h"

using R91Fixture = r91::Fixture;

namespace
{
	const char* const kCode = "SSE.STK.000001";
	const uint32_t kDay1 = 20200102u;
	const uint32_t kDay2 = 20200103u;

	uint32_t count_ticks(const std::vector<r91::TickRec>& ticks, uint32_t date)
	{
		uint32_t count = 0;
		for (const r91::TickRec& rec : ticks)
		{
			if (rec.date == date)
				count++;
		}
		return count;
	}
}

/*
 * 生产: HisDataReplayer.cpp:462-516（reset 四类 HFT 缓存 + bars 游标）
 *       436-457（clear_cache 全清）、649-675（prepare 内 reset）
 *       771-938（三轮 run_by_bars 均应产出相同闭盘序列）
 */
TEST_F(R91Fixture, R91BinReset_ReplayTwiceThenClearCacheReloads)
{
	std::vector<WTSBarStruct> bars;
	bars.push_back(r91::mk_bar(20191231u, 1500u, 99.0, 100.0, 98.0, 100.0, 10.0));
	bars.push_back(r91::mk_bar(kDay1, 1500u, 100.0, 102.0, 99.0, 101.0, 20.0));
	bars.push_back(r91::mk_bar(kDay2, 1500u, 101.0, 103.0, 100.0, 102.0, 30.0));
	ASSERT_TRUE(r91::write_bars(_base, "SSE", "000001", "day", bars));

	std::vector<WTSTickStruct> ticks;
	ticks.push_back(r91::mk_tick(kDay1, 931u, 10.0, 100.0, "000001"));
	ticks.push_back(r91::mk_tick(kDay1, 932u, 11.0, 100.0, "000001"));
	ticks.push_back(r91::mk_tick(kDay1, 933u, 12.0, 100.0, "000001"));
	ticks.push_back(r91::mk_tick(kDay1, 1501u, 99.0, 1.0, "000001"));
	ticks.push_back(r91::mk_tick(kDay1, 1502u, 98.0, 1.0, "000001"));
	ASSERT_TRUE(r91::write_ticks(_base, "SSE", "000001", kDay1, ticks));

	std::vector<WTSOrdDtlStruct> details;
	details.push_back(r91::mk_orddtl(kDay1, 931u, 10.1, 1u, "000001"));
	details.push_back(r91::mk_orddtl(kDay1, 932u, 10.2, 2u, "000001"));
	details.push_back(r91::mk_orddtl(kDay1, 1501u, 0.0, 0u, "000001"));
	ASSERT_TRUE(r91::write_orddtl(_base, "SSE", "000001", kDay1, details));

	std::vector<WTSOrdQueStruct> queues;
	queues.push_back(r91::mk_ordque(kDay1, 931u, 9.9, 11u, "000001"));
	queues.push_back(r91::mk_ordque(kDay1, 932u, 9.8, 12u, "000001"));
	queues.push_back(r91::mk_ordque(kDay1, 1501u, 0.0, 0u, "000001"));
	ASSERT_TRUE(r91::write_ordque(_base, "SSE", "000001", kDay1, queues));

	std::vector<WTSTransStruct> trans;
	trans.push_back(r91::mk_trans(kDay1, 931u, 10.05, 21u, "000001"));
	trans.push_back(r91::mk_trans(kDay1, 932u, 10.15, 22u, "000001"));
	trans.push_back(r91::mk_trans(kDay1, 1501u, 0.0, 0u, "000001"));
	ASSERT_TRUE(r91::write_trans(_base, "SSE", "000001", kDay1, trans));

	r91::CfgOpts opts;
	opts.mode = "bin";
	opts.tick = true;
	WTSVariant* cfg = r91::make_cfg(_root, _base, 202001020930ull, 202001031500ull, opts);

	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg, NULL, NULL));
	cfg->release();

	r91::Sink sink;
	replayer.register_sink(&sink, "r91_reset");

	for (int round = 0; round < 3; round++)
	{
		if (round == 2)
			replayer.clear_cache();

		ASSERT_TRUE(replayer.prepare());
		replayer.sub_tick(1u, kCode);
		replayer.sub_order_detail(2u, kCode);
		replayer.sub_order_queue(3u, kCode);
		replayer.sub_transaction(4u, kCode);

		WTSKlineSlice* slice = replayer.get_kline_slice(kCode, "d", 10, 1, true);
		ASSERT_NE(nullptr, slice);
		slice->release();

		replayer.run(false);

		const uint32_t expected = (uint32_t)(round + 1);
		EXPECT_EQ(2u * expected, sink.bars.size());
		EXPECT_EQ(3u * expected, count_ticks(sink.ticks, kDay1));
		EXPECT_EQ(2u * expected, sink.details.size());
		EXPECT_EQ(2u * expected, sink.queues.size());
		EXPECT_EQ(2u * expected, sink.trans.size());
	}

	EXPECT_EQ(3u, sink.initCalls);
	EXPECT_EQ(6u, sink.beginCalls);
	EXPECT_EQ(6u, sink.endCalls);
	EXPECT_EQ(3u, sink.doneCalls);
	EXPECT_EQ(kDay2, replayer.get_date());
}

/*
 * 生产: HisDataReplayer.cpp:2828-2879（checkTicks 空代码/空缓存）
 *       2470-2476（tick 关闭时直接 NULL）、2881-2889（get_last_tick 空代码）
 *       2983-3019（calc_fee 的按量/按额/minfee/default offset）
 *       3021-3032、3034-3087（空缓存价格）、2921-2931（custom rule rawcode）
 *       2915-2919、2933-2944（商品/会话查询）、3089-3165（空订阅早退）
 */
TEST_F(R91Fixture, R91BinMisc_EmptyInputsFeeAndInfoQueries)
{
	const std::string rules = _base.string() + "/rules_rawcode.json";
	ASSERT_TRUE(r91::write_text(rules,
		"{\"CFFEX\":{\"IF\":["
		"{\"date\":20200103,\"from\":\"\",\"to\":\"IF2001\",\"oldclose\":0.0,\"newclose\":100.0},"
		"{\"date\":20200106,\"from\":\"IF2001\",\"to\":\"IF2002\",\"oldclose\":200.0,\"newclose\":100.0}"
		"]}}"));

	const std::string fees = _base.string() + "/fees.json";
	ASSERT_TRUE(r91::write_text(fees,
		"{\"CFFEX.IF\":{\"byvolume\":true,\"open\":2.0,\"close\":3.0,\"closetoday\":4.0},"
		"\"SSE.STK\":{\"byvolume\":false,\"open\":0.0001,\"close\":0.0002,"
		"\"closetoday\":0.0003,\"minfee\":5.0}}"));

	r91::CfgOpts opts;
	opts.mode = "csv";
	opts.tick = false;
	opts.fees = fees;
	opts.rules.push_back(std::make_pair(std::string("HOT"), rules));
	opts.commodities.push_back((_root / "dist/common/stk_comms.json").string());
	opts.commodities.push_back((_root / "dist/common/commodities.json").string());
	WTSVariant* cfg = r91::make_cfg(_root, _base, 202001020930ull, 202001021500ull, opts);

	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg, NULL, NULL));
	cfg->release();

	EXPECT_FALSE(replayer.is_tick_enabled());
	EXPECT_TRUE(replayer.is_tick_simulated());

	// 空代码 / tick 关闭：tick 入口不得读缓存
	EXPECT_EQ(nullptr, replayer.get_tick_slice("", 1u, 0ull));
	EXPECT_EQ(nullptr, replayer.get_tick_slice("SSE.STK.600000", 1u, 0ull));
	EXPECT_EQ(nullptr, replayer.get_last_tick(""));

	// 空订阅输入全部早退，不产生崩溃
	replayer.sub_tick(1u, "");
	replayer.sub_order_detail(1u, "");
	replayer.sub_order_queue(1u, "");
	replayer.sub_transaction(1u, "");

	// 无行情缓存的价格查询
	EXPECT_DOUBLE_EQ(0.0, replayer.get_day_price("SSE.STK.600000", 0));
	EXPECT_DOUBLE_EQ(0.0, replayer.get_day_price("SSE.STK.600000", 4));
	EXPECT_DOUBLE_EQ(0.0, replayer.get_cur_price("SSE.STK.600000"));
	replayer.update_price("SSE.STK.600000", 12.5);
	EXPECT_DOUBLE_EQ(12.5, replayer.get_cur_price("SSE.STK.600000"));

	// 基础信息
	ASSERT_NE(nullptr, replayer.get_session_info("SD0930", false));
	ASSERT_NE(nullptr, replayer.get_session_info("SSE.STK.600000", true));
	EXPECT_EQ(nullptr, replayer.get_session_info("NOPE", false));
	ASSERT_NE(nullptr, replayer.get_commodity_info("SSE.STK.600000"));
	EXPECT_EQ(nullptr, replayer.get_commodity_info("SSE.NOPE.999999"));

	// rawcode：普通合约返回空；HOT 走 custom rule，当前日期取最后一段 IF2002
	EXPECT_EQ(std::string(), replayer.get_rawcode("SSE.STK.600000"));
	EXPECT_EQ(std::string("CFFEX.IF.2002"), replayer.get_rawcode("CFFEX.IF.HOT"));

	// 费率：按量 / 按额 / 三档 offset / 非法 offset / 最低手续费
	EXPECT_DOUBLE_EQ(4.0, replayer.calc_fee("CFFEX.IF.2001", 100.0, 2.0, 0));
	EXPECT_DOUBLE_EQ(6.0, replayer.calc_fee("CFFEX.IF.2001", 100.0, 2.0, 1));
	EXPECT_DOUBLE_EQ(8.0, replayer.calc_fee("CFFEX.IF.2001", 100.0, 2.0, 2));
	EXPECT_DOUBLE_EQ(0.0, replayer.calc_fee("CFFEX.IF.2001", 100.0, 2.0, 3));
	EXPECT_DOUBLE_EQ(5.0, replayer.calc_fee("SSE.STK.600000", 10.0, 100.0, 0));
	EXPECT_DOUBLE_EQ(5.0, replayer.calc_fee("SSE.STK.600000", 10.0, 100.0, 1));
	EXPECT_DOUBLE_EQ(5.0, replayer.calc_fee("SSE.STK.600000", 10.0, 100.0, 2));
	EXPECT_DOUBLE_EQ(0.0, replayer.calc_fee("SSE.STK.600000", 10.0, 100.0, 3));
}
