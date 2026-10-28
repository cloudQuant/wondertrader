/**
 * r190_his_residual_test.cpp  (lane R190 · wave27)
 *
 * 靶子：HisDataReplayer.cpp 中仍可达的公开面残余行。
 * 每个用例注释标明生产文件:行段；全部只走公开 API，独立、确定性，
 * 不起线程、不读挂钟；临时文件只落系统临时目录，由 r72::Fixture TearDown 清理。
 *
 * ExecMocker.cpp 的 init 成功段（L71-85）与 _exec_unit 非空回调族本轮未覆盖：
 * btcov 执行体内加载真实 libWtExeFact 的既有用例均 SIGSEGV（R111/R121/R180
 * 已登记的崩溃点），公开面没有不触碰该崩溃点的安全接缝。详见 bugs/R190.md。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include <WtBtCore/HisDataReplayer.h>

#include "r190_common.h"

namespace
{
	const char* const kCu = "SHFE.cu.2406";
	const char* const kStk = "SSE.STK.000001";
	const char* const kStkQfq = "SSE.STK.000001+";
	const char* const kIc = "DCE.i.2409";
	const char* const kJ = "DCE.j.2409";
	const char* const kRb = "SHFE.rb.2405";

	namespace fs = std::filesystem;

	using R190Fixture = r72::Fixture;
}

// 生产: HisDataReplayer.cpp:1633（getNextTransTime 在 csv 模式 check 失败时 continue）
//       1681（getNextOrdDtlTime 同上）、1729（getNextOrdQueTime 同上）
//       1803/1829/1883（replayHftDatasByDay 中空明细/成交/队列列表 continue）
//       1856（tick 订阅存在但该合约无 tick 数据时 continue）
// 只给 A 提供 tick 数据，B 无 tick；C/D/E 只订阅无数据。run_by_ticks 走一天的
// 回放，空列表分支全部执行且只回调 1 笔 tick。
TEST_F(R190Fixture, R190His_TickReplayEmptyHftListsContinue)
{
	r72::Loader loader;
	loader.ticks[std::string(kCu) + "#20200102"] = {
		r72::make_tick_struct(kCu, 20200102u, 93100000u, 70000.0, 69990.0, 70010.0)
	};

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, true);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();

	r72::Sink sink;
	rp.register_sink(&sink, "r190_hft_empty");
	ASSERT_TRUE(rp.prepare());

	rp.sub_tick(0, kCu);
	rp.sub_tick(0, kIc);			// 无 tick 数据 -> L1856
	rp.sub_order_detail(0, kJ);	// csv 模式无 L2 -> L1681 + L1803
	rp.sub_order_queue(0, kRb);	// csv 模式无 L2 -> L1729 + L1883
	rp.sub_transaction(0, kStk);	// csv 模式无 L2 -> L1633 + L1829

	rp.run(false);

	EXPECT_EQ(1u, sink.tickCalls);
	EXPECT_EQ(1u, sink.beginCalls);
	EXPECT_EQ(1u, sink.endCalls);
	EXPECT_EQ(1u, sink.doneCalls);
}

// 生产: HisDataReplayer.cpp:1561（getNextTickTime 首笔定位时 cursor 越过 count 的 break）
// 单笔 tick 落在 12:00 午休，不在任何交易小节；checkAllTicks 仍返回 true，
// 但 getNextTickTime 的定位循环扫到尾部后 break，当天没有任何 tick 回调。
TEST_F(R190Fixture, R190His_OutOfHoursTickYieldsNoReplay)
{
	r72::Loader loader;
	loader.ticks[std::string(kCu) + "#20200102"] = {
		r72::make_tick_struct(kCu, 20200102u, 120000000u, 70000.0, 69990.0, 70010.0)
	};

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, true);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();

	r72::Sink sink;
	rp.register_sink(&sink, "r190_out_of_hours");
	ASSERT_TRUE(rp.prepare());
	rp.sub_tick(0, kCu);

	rp.run(false);

	EXPECT_EQ(0u, sink.tickCalls);
	EXPECT_EQ(1u, sink.beginCalls);
	EXPECT_EQ(1u, sink.doneCalls);
}

// 生产: HisDataReplayer.cpp:2424-2426（日线 cursor 已初始化后跨多个交易日追平）
// 先取一次日线把 cursor 定位在 20200102，随后无 tick 的 run_by_ticks 把
// _cur_tdate 推到 20200109；再次取同一 key，while 循环连续跨过 03/06/07/08
// 四根 bar，cursor 到达 count 时由 L2426 break。
TEST_F(R190Fixture, R190His_DailyCursorCatchesUpAfterTickRun)
{
	r72::Loader loader;
	loader.finalBars[kCu] = {
		r190::make_daily(20200102u, 70000.0),
		r190::make_daily(20200103u, 70010.0),
		r190::make_daily(20200106u, 70020.0),
		r190::make_daily(20200107u, 70030.0),
		r190::make_daily(20200108u, 70040.0),
	};

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001081500ull, true);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();

	r72::Sink sink;
	rp.register_sink(&sink, "r190_daily_cursor");
	ASSERT_TRUE(rp.prepare());

	WTSKlineSlice* first = rp.get_kline_slice(kCu, "d", 5, 1, false);
	ASSERT_NE(nullptr, first);
	first->release();

	rp.run(false);	// 没有 tick 订阅：只推进 _cur_tdate
	EXPECT_EQ(1u, sink.doneCalls);

	WTSKlineSlice* catchup = rp.get_kline_slice(kCu, "d", 5, 1, false);
	ASSERT_NE(nullptr, catchup);
	EXPECT_EQ(5u, catchup->size());
	catchup->release();
}

// 生产: HisDataReplayer.cpp:1517-1524（simTickWithUnsubBars 日线跳过旧 bar）
//       1507-1508（日线模拟 tick 的 low 取 min 分支）
// tick=false 时 checkUnbars 把复权查询引入的原始股票日线放进 _unbars_cache。
// 主 K 线只有 02/06 两根，06 当天未订阅列表的 04/05/06 依次被跳过并模拟。
TEST_F(R190Fixture, R190His_UnbarsDailySkipStaleBars)
{
	r72::Loader loader;
	loader.finalBars[kCu] = {
		r190::make_daily(20200102u, 70000.0),
		r190::make_daily(20200106u, 70050.0),
	};
	loader.finalBars[kStk] = {
		r190::make_daily(20200103u, 10.0),
		r190::make_daily(20200104u, 10.1),
		r190::make_daily(20200105u, 10.2),
		r190::make_daily(20200106u, 10.3),
	};

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001061500ull, false);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();

	r72::Sink sink;
	rp.register_sink(&sink, "r190_unbars_skip");
	ASSERT_TRUE(rp.prepare());

	WTSKlineSlice* main = rp.get_kline_slice(kCu, "d", 5, 1, true);
	ASSERT_NE(nullptr, main);
	main->release();

	// 复权代码只用来登记 _unsubbed_in_need，本身没有 qfq 数据
	EXPECT_EQ(nullptr, rp.get_kline_slice(kStkQfq, "d", 5, 1, false));

	rp.run(false);

	EXPECT_GT(sink.tickCalls, 0u);
	EXPECT_EQ(1u, sink.barCalls);
	EXPECT_EQ(1u, sink.doneCalls);
}

// 生产: HisDataReplayer.cpp:3651-3652（cacheFinalBarsFromLoader 命中 dsb 时
//       写 _unbars_cache 的 bSubbed=false 分支）
// 第一个回放器 autoTrans=true，checkUnbars 把未订阅股票日线转储为
// his/day/SSE/000001.dsb；第二个回放器同目录、loader 不再提供该合约，
// checkUnbars 直接读 dsb，loader 调用次数保持不变。
TEST_F(R190Fixture, R190His_UnbarsDsbDirectReadReusesCache)
{
	const fs::path dsb = _base / "his" / "day" / "SSE" / "000001.dsb";
	std::vector<WTSBarStruct> stkBars = {
		r190::make_daily(20200102u, 10.0),
		r190::make_daily(20200103u, 10.1),
		r190::make_daily(20200106u, 10.2),
		r190::make_daily(20200107u, 10.3),
		r190::make_daily(20200108u, 10.4),
	};
	std::vector<WTSBarStruct> cuBars = {
		r190::make_daily(20200102u, 70000.0),
		r190::make_daily(20200103u, 70010.0),
		r190::make_daily(20200106u, 70020.0),
		r190::make_daily(20200107u, 70030.0),
		r190::make_daily(20200108u, 70040.0),
	};

	{
		r72::Loader loader;
		loader.autoTrans = true;
		loader.finalBars[kCu] = cuBars;
		loader.finalBars[kStk] = stkBars;

		HisDataReplayer rp;
		WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001081500ull, false);
		ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
		cfg->release();

		r72::Sink sink;
		rp.register_sink(&sink, "r190_unbars_write");
		ASSERT_TRUE(rp.prepare());

		WTSKlineSlice* main = rp.get_kline_slice(kCu, "d", 5, 1, true);
		ASSERT_NE(nullptr, main);
		main->release();
		EXPECT_EQ(nullptr, rp.get_kline_slice(kStkQfq, "d", 5, 1, false));

		rp.run(false);
		ASSERT_TRUE(fs::exists(dsb)) << dsb;
		EXPECT_EQ(1u, sink.doneCalls);
	}

	{
		r72::Loader loader;
		loader.autoTrans = true;
		loader.finalBars[kCu] = cuBars;

		HisDataReplayer rp;
		WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001081500ull, false);
		ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
		cfg->release();

		r72::Sink sink;
		rp.register_sink(&sink, "r190_unbars_read");
		ASSERT_TRUE(rp.prepare());

		WTSKlineSlice* main = rp.get_kline_slice(kCu, "d", 5, 1, true);
		ASSERT_NE(nullptr, main);
		main->release();
		EXPECT_EQ(nullptr, rp.get_kline_slice(kStkQfq, "d", 5, 1, false));

		const uint32_t callsBefore = loader.finalCalls;
		rp.run(false);
		EXPECT_EQ(callsBefore, loader.finalCalls);
		EXPECT_EQ(1u, sink.doneCalls);
	}
}

// 生产: HisDataReplayer.cpp:3798-3799（cacheRawBarsFromCSV 命中 dsb 时
//       写 _unbars_cache 的 bSubbed=false 分支）
//       3766-3768（带自定义规则且复权的期货 dsb 文件名拼接）
// loader 不提供股票日线且 autoTrans=false，但磁盘上预置合法压缩块：
// checkUnbars 先走 loader 失败，再走 csv 转储直读，bSubbed=false 分支落库。
// 同一回放器再查一次 SHFE.rb.HOT+，走规则代码的 dsb 文件名分支。
TEST_F(R190Fixture, R190His_UnbarsCsvDsbDirectRead)
{
	const fs::path dsb = _base / "his" / "day" / "SSE" / "000001.dsb";
	ASSERT_TRUE(r190::write_cmp_day_dsb(dsb, 5, 10.0));

	const fs::path hotDsb =
		_base / "his" / "day" / "SHFE" / "SHFE.rb_HOT+.dsb";
	ASSERT_TRUE(r190::write_cmp_day_dsb(hotDsb, 3, 3800.0));

	const fs::path ruleFile = _base / "custom_rules.json";
	ASSERT_TRUE(r72::write_text(ruleFile,
		"{\"SHFE\":{\"rb\":[{\"date\":20200102,\"from\":\"\","
		"\"to\":\"2406\",\"oldclose\":0,\"newclose\":1}]}}"));

	r72::Loader loader;
	loader.autoTrans = false;
	loader.finalBars[kCu] = {
		r190::make_daily(20200102u, 70000.0),
		r190::make_daily(20200103u, 70010.0),
		r190::make_daily(20200106u, 70020.0),
		r190::make_daily(20200107u, 70030.0),
		r190::make_daily(20200108u, 70040.0),
	};

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001081500ull, false);
	WTSVariant* rules = WTSVariant::createObject();
	rules->append("HOT", ruleFile.string().c_str());
	cfg->get("basefiles")->append("rules", rules);
	rules->release();
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();

	r72::Sink sink;
	rp.register_sink(&sink, "r190_unbars_csv");
	ASSERT_TRUE(rp.prepare());

	WTSKlineSlice* main = rp.get_kline_slice(kCu, "d", 5, 1, true);
	ASSERT_NE(nullptr, main);
	main->release();
	EXPECT_EQ(nullptr, rp.get_kline_slice(kStkQfq, "d", 5, 1, false));

	const uint32_t callsBefore = loader.finalCalls;
	rp.run(false);

	// checkUnbars 对股票原始代码再问一次 loader，失败后落到 csv 直读
	EXPECT_EQ(callsBefore + 1, loader.finalCalls);
	EXPECT_EQ(1u, sink.doneCalls);

	// 规则+复权期货代码：走到 L3766-L3768 的文件名拼接；run 结束后 _cur_tdate
	// 已越过块内日期，切片可为 NULL，但 dsb 路径分支必须已执行。
	WTSKlineSlice* hot = rp.get_kline_slice("SHFE.rb.HOT+", "d", 2, 1, false);
	if (hot != nullptr)
		hot->release();
}

// 生产: HisDataReplayer.cpp:1174-1187（run_by_tasks 换日时按 session offset 推进日期）
// TRADING 会话 offset=300>0，分钟任务跨两个交易日必然进入 L1175/L1178/L1179。
// 空行情下只推进时钟与交易日，不产生 tick/bar 回调。
TEST_F(R190Fixture, R190His_MinuteTaskAdvancesAcrossDays)
{
	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001031500ull, false);
	ASSERT_TRUE(rp.init(cfg));
	cfg->release();

	r72::Sink sink;
	rp.register_sink(&sink, "r190_task");
	ASSERT_TRUE(rp.prepare());

	rp.register_task(1, 5, 5, "min");
	rp.run(false);

	EXPECT_GE(sink.beginCalls, 2u);
	EXPECT_GE(sink.endCalls, 2u);
	EXPECT_EQ(1u, sink.doneCalls);
	EXPECT_GT(rp.get_trading_date(), 20200103u);
}

// 生产: HisDataReplayer.cpp:1309-1316（simTicks 分钟 bar 落后于当前时间时
//       连续步进 cursor，到尾部 break）
// 主 K 线只有 09:35/13:05 两根；未订阅列表的股票 m5 三根 09:36-09:38
// 在 13:05 的 simTicks 中全部过期，cursor 从 1 步进到 3 后 break。
TEST_F(R190Fixture, R190His_MinuteSimTicksSkipsStaleBars)
{
	r72::Loader loader;
	loader.finalBars[kCu] = {
		r190::make_minute(20200102u, 935u, 70000.0),
		r190::make_minute(20200102u, 1305u, 70100.0),
	};
	loader.finalBars[kStk] = {
		r190::make_minute(20200102u, 936u, 10.0),
		r190::make_minute(20200102u, 937u, 10.1),
		r190::make_minute(20200102u, 938u, 10.2),
	};

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020935ull, 202001021305ull, false);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();

	r72::Sink sink;
	rp.register_sink(&sink, "r190_min_skip");
	ASSERT_TRUE(rp.prepare());

	WTSKlineSlice* other = rp.get_kline_slice(kStk, "m", 5, 5, false);
	ASSERT_NE(nullptr, other);
	other->release();

	WTSKlineSlice* main = rp.get_kline_slice(kCu, "m", 5, 5, true);
	ASSERT_NE(nullptr, main);
	main->release();

	rp.run(false);

	EXPECT_GT(sink.tickCalls, 0u);
	EXPECT_EQ(1u, sink.barCalls);
	EXPECT_EQ(1u, sink.doneCalls);
}
