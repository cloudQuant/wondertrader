/**
 * r200_hisdata_test.cpp  (lane R200 · wave28)
 *
 * 靶子：WtBtCore/HisDataReplayer.cpp 的公开面可达残余行。
 * 每个用例注释标明生产文件:行段；全部只走公开 API，独立、确定性，
 * 不启线程、不读挂钟；临时文件只落系统临时目录，由 r72::Fixture TearDown 清理。
 *
 * ExecMocker.cpp 的 94 行缺口全部位于 init 成功段与 _exec_unit 非空回调，
 * 公开面进入该族必须先加载真实 libWtExeFact；btcov 内既有用例均 SIGSEGV
 * （R111/R121/R180，见 bugs/R200.md），本轮不新增该族用例。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include <WtBtCore/HisDataReplayer.h>

#include "r200_common.h"

namespace
{
	const char* const kCu = "SHFE.cu.2406";
	const char* const kIc = "DCE.i.2409";
	const char* const kJ = "DCE.j.2409";
	const char* const kRb = "SHFE.rb.2405";
	const char* const kStk = "SSE.STK.000001";
	const char* const kStkHfq = "SSE.STK.000001+";

	namespace fs = std::filesystem;
	using R200Fixture = r72::Fixture;
}

// 生产: HisDataReplayer.cpp:1803/1829/1856/1883
// replayHftDatasByDay 中委托明细/成交/委托队列/无 tick 合约的空列表 continue。
// A(cu) 提供 2 笔 tick，B(j) 无 tick；L2 三个订阅在 csv 模式下无数据。
// 2 笔 tick 保证首笔不会被 getNextTickTime 的 cursor>=count 提前跳过。
TEST_F(R200Fixture, R200His_TickReplayEmptyHftListsContinue)
{
	r72::Loader loader;
	loader.ticks[std::string(kCu) + "#20200102"] = {
		r72::make_tick_struct(kCu, 20200102u, 93100000u, 70000.0, 69990.0, 70010.0),
		r72::make_tick_struct(kCu, 20200102u, 93200000u, 70010.0, 70000.0, 70020.0)
	};

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, true);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();

	r72::Sink sink;
	rp.register_sink(&sink, "r200_hft_empty");
	ASSERT_TRUE(rp.prepare());

	rp.sub_tick(0, kCu);
	rp.sub_tick(0, kJ);				// 无 tick -> L1856
	rp.sub_order_detail(0, kJ);		// csv 无 L2 -> L1803
	rp.sub_transaction(0, kStk);		// csv 无 L2 -> L1829
	rp.sub_order_queue(0, kRb);		// csv 无 L2 -> L1883

	rp.run(false);

	EXPECT_EQ(1u, sink.tickCalls);
	EXPECT_EQ(1u, sink.beginCalls);
	EXPECT_EQ(1u, sink.endCalls);
	EXPECT_EQ(1u, sink.doneCalls);
}

// 生产: HisDataReplayer.cpp:1971-1972
// replayHftDatas 的 tick 回放循环中，cursor 已被 checkTicks 置为 UINT_MAX
// 且 count=0 的无数据合约直接 continue。主 K 线为 cu 日线，tick 只在第二个
// 交易日提供 2 笔，j 始终无 tick。
TEST_F(R200Fixture, R200His_BarReplaySkipsEmptyTickList)
{
	r72::Loader loader;
	loader.finalBars[kCu] = {
		r200::make_daily(20200102u, 70000.0),
		r200::make_daily(20200103u, 70010.0)
	};
	loader.ticks[std::string(kCu) + "#20200103"] = {
		r72::make_tick_struct(kCu, 20200103u, 93100000u, 70010.0, 70000.0, 70020.0),
		r72::make_tick_struct(kCu, 20200103u, 93200000u, 70020.0, 70010.0, 70030.0)
	};

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001031500ull, true);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();

	r72::Sink sink;
	rp.register_sink(&sink, "r200_tick_empty");
	ASSERT_TRUE(rp.prepare());

	WTSKlineSlice* main = rp.get_kline_slice(kCu, "d", 5, 1, true);
	ASSERT_NE(nullptr, main);
	main->release();

	rp.sub_tick(0, kCu);
	rp.sub_tick(0, kJ);		// 无 tick，但进入 replayHftDatas 的循环 -> L1972

	rp.run(false);

	EXPECT_GT(sink.tickCalls, 0u);
	EXPECT_EQ(1u, sink.doneCalls);
}

// 生产: HisDataReplayer.cpp:1521（未订阅日线列表在模拟 tick 时游标到尾部 break）
//       1453（未订阅分钟线列表在模拟 tick 时游标到尾部 break）
// 日线段：主 K 线含 20191231 暖场 bar，回测从 02 开始；股票日线 03/04/05
// 在 06 全部过期。分钟段：主 K 线含 09:30 暖场 bar，回测从 09:35 开始；
// 股票 m5 09:36/09:37 在 13:05 全部过期。
TEST_F(R200Fixture, R200His_UnbarsListsExhaustCursors)
{
	{
		r72::Loader loader;
		loader.finalBars[kCu] = {
			r200::make_minute(20191231u, 1500u, 69990.0),
			r200::make_daily(20200102u, 70000.0),
			r200::make_daily(20200103u, 70010.0),
			r200::make_daily(20200106u, 70050.0)
		};
		loader.finalBars[kStk] = {
			r200::make_daily(20200103u, 10.0),
			r200::make_daily(20200104u, 10.1),
			r200::make_daily(20200105u, 10.2)
		};

		HisDataReplayer rp;
		WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001061500ull, false);
		ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
		cfg->release();

		r72::Sink sink;
		rp.register_sink(&sink, "r200_unbars_day");
		ASSERT_TRUE(rp.prepare());

		WTSKlineSlice* main = rp.get_kline_slice(kCu, "d", 5, 1, true);
		ASSERT_NE(nullptr, main);
		main->release();
		EXPECT_EQ(nullptr, rp.get_kline_slice(kStkHfq, "d", 5, 1, false));

		rp.run(false);

		EXPECT_GT(sink.tickCalls, 0u);
		EXPECT_GE(sink.barCalls, 1u);
		EXPECT_EQ(1u, sink.doneCalls);
	}

	{
		r72::Loader loader;
		loader.finalBars[kCu] = {
			r200::make_minute(20200102u, 930u, 69990.0),
			r200::make_minute(20200102u, 935u, 70000.0),
			r200::make_minute(20200102u, 1305u, 70100.0)
		};
		loader.finalBars[kStk] = {
			r200::make_minute(20200102u, 936u, 10.0),
			r200::make_minute(20200102u, 937u, 10.1)
		};

		HisDataReplayer rp;
		WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021305ull, false);
		ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
		cfg->release();

		r72::Sink sink;
		rp.register_sink(&sink, "r200_unbars_min");
		ASSERT_TRUE(rp.prepare());

		WTSKlineSlice* main = rp.get_kline_slice(kCu, "m", 5, 5, true);
		ASSERT_NE(nullptr, main);
		main->release();
		EXPECT_EQ(nullptr, rp.get_kline_slice(kStkHfq, "m", 5, 5, false));

		rp.run(false);

		EXPECT_GT(sink.tickCalls, 0u);
		EXPECT_GE(sink.barCalls, 1u);
		EXPECT_EQ(1u, sink.doneCalls);
	}
}

// 生产: HisDataReplayer.cpp:697-703
// run() 自动选择主 K 线时，同一周期下后出现的更短周期会把主 K 线切过去。
// 查询 m#10 会同时留下原始 m#5 缓存（times=1）和重采样 m#10（times=2）；
// map 中 #m#10 排在 #m#5 之前，于是先选 times=2，再被 times=1 覆盖。
TEST_F(R200Fixture, R200His_AutoMainPicksShorterTimes)
{
	r72::Loader loader;
	std::vector<WTSBarStruct> bars;
	for (uint32_t i = 0; i < 12; ++i)
		bars.push_back(r200::make_minute(20200102u, 930u + i * 5u, 70000.0 + i));

	loader.finalBars[kCu] = bars;

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021030ull, false);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();

	r72::Sink sink;
	rp.register_sink(&sink, "r200_auto_main");
	ASSERT_TRUE(rp.prepare());

	// 先建立并定位原始 m#5 缓存，再查询 m#10 生成 times=2 的重采样缓存。
	WTSKlineSlice* base = rp.get_kline_slice(kCu, "m", 5, 5, false);
	ASSERT_NE(nullptr, base);
	base->release();

	WTSKlineSlice* wide = rp.get_kline_slice(kCu, "m", 2, 10, false);
	ASSERT_NE(nullptr, wide);
	wide->release();

	rp.run(false);

	EXPECT_GE(sink.beginCalls, 1u);
	EXPECT_EQ(1u, sink.doneCalls);
	EXPECT_GE(rp.get_trading_date(), 20200102u);
}

// 生产: HisDataReplayer.cpp:2419-2426
// 非主 K 线的日线游标停留在 20200102 之后的第二根，分钟主 K 线把时钟
// 推到 20200108，再次查询同一 key 时 while 连续步进游标直到倒数第二根。
TEST_F(R200Fixture, R200His_DailyCursorCatchesUpAfterMinuteRun)
{
	r72::Loader loader;
	loader.finalBars[kCu] = {
		r200::make_minute(20200102u, 945u, 70000.0),
		r200::make_minute(20200103u, 945u, 70010.0),
		r200::make_minute(20200106u, 945u, 70020.0),
		r200::make_minute(20200107u, 945u, 70030.0),
		r200::make_minute(20200108u, 945u, 70040.0)
	};
	loader.finalBars[kIc] = {
		r200::make_daily(20200102u, 4000.0),
		r200::make_daily(20200103u, 4010.0),
		r200::make_daily(20200106u, 4020.0),
		r200::make_daily(20200107u, 4030.0),
		r200::make_daily(20200108u, 4040.0)
	};

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020945ull, 202001080945ull, false);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();

	r72::Sink sink;
	rp.register_sink(&sink, "r200_cursor");
	ASSERT_TRUE(rp.prepare());

	WTSKlineSlice* main = rp.get_kline_slice(kCu, "m", 5, 5, true);
	ASSERT_NE(nullptr, main);
	main->release();

	WTSKlineSlice* first = rp.get_kline_slice(kIc, "d", 2, 1, false);
	ASSERT_NE(nullptr, first);
	first->release();

	rp.run(false);
	EXPECT_EQ(1u, sink.doneCalls);

	WTSKlineSlice* catchup = rp.get_kline_slice(kIc, "d", 5, 1, false);
	ASSERT_NE(nullptr, catchup);
	EXPECT_EQ(4u, catchup->size());
	catchup->release();
}

// 生产: HisDataReplayer.cpp:3306-3308（cacheRawTicksFromBin 自定义规则取原始代码）
//       3315-3322（先取 <product>_<rule> 的热力/次主力 tick 包装代码）
// bin 模式下查询 SHFE.rb.HOT：规则命中后先找 rb_HOT 的 tick，再回落到分月代码；
// 临时目录无文件，最终返回 null，但两条命名分支都已执行。
TEST_F(R200Fixture, R200His_BinRuleTickLookup)
{
	const fs::path ruleFile = _base / "custom_rules.json";
	ASSERT_TRUE(r200::write_rules(ruleFile));

	HisDataReplayer rp;
	WTSVariant* cfg = r200::make_bin_cfg(_root, _base, 202001020930ull, 202001021500ull, true, ruleFile);
	ASSERT_TRUE(rp.init(cfg, nullptr, nullptr));
	cfg->release();

	r72::Sink sink;
	rp.register_sink(&sink, "r200_bin_rule");
	ASSERT_TRUE(rp.prepare());

	EXPECT_EQ(nullptr, rp.get_tick_slice("SHFE.rb.HOT", 5, 0));
}

// 生产: HisDataReplayer.cpp:4624-4626
// cache_clear_days=1 时，两个非主 K 线合约在下一个交易日同时进入待清理集合，
// 第二个 key 追加时补逗号。
TEST_F(R200Fixture, R200His_CacheClearJoinsMultipleCodes)
{
	r72::Loader loader;
	loader.finalBars[kCu] = {
		r200::make_daily(20200102u, 70000.0),
		r200::make_daily(20200103u, 70010.0),
		r200::make_daily(20200106u, 70020.0)
	};
	loader.finalBars[kIc] = { r200::make_daily(20200102u, 4000.0) };
	loader.finalBars[kJ] = { r200::make_daily(20200102u, 1000.0) };

	HisDataReplayer rp;
	WTSVariant* cfg = r200::make_csv_cfg(_root, _base,
		202001020930ull, 202001061500ull, false, 1);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();

	r72::Sink sink;
	rp.register_sink(&sink, "r200_cache_clear");
	ASSERT_TRUE(rp.prepare());

	WTSKlineSlice* main = rp.get_kline_slice(kCu, "d", 5, 1, true);
	ASSERT_NE(nullptr, main);
	main->release();

	WTSKlineSlice* second = rp.get_kline_slice(kIc, "d", 5, 1, false);
	ASSERT_NE(nullptr, second);
	second->release();

	WTSKlineSlice* third = rp.get_kline_slice(kJ, "d", 5, 1, false);
	ASSERT_NE(nullptr, third);
	third->release();

	rp.run(false);

	EXPECT_EQ(1u, sink.doneCalls);
	EXPECT_GE(sink.beginCalls, 2u);
}
