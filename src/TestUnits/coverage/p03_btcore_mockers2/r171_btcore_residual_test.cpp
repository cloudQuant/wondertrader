/**
 * r171_btcore_residual_test.cpp  (lane R171 · wave25)
 *
 * 靶子：WtBtCore 剩余未覆盖行中，不依赖策略插件、hook 线程与私有注入的可达面。
 * 每个用例注释标明生产文件:行段；全部只走公开 API，独立、确定性，无长线程/墙钟。
 * 临时文件只落系统临时目录，由夹具 TearDown 清理。
 */
#include <gtest/gtest.h>

#include <string>

#include <Includes/WTSDataDef.hpp>
#include <WtBtCore/CtaMocker.h>
#include <WtBtCore/HftMocker.h>
#include <WtBtCore/SelMocker.h>
#include <WtBtCore/UftMocker.h>

#include "r171_common.h"

namespace
{
	const char* const kStk1 = "SSE.STK.000001";
	const char* const kCu = "SHFE.cu.2406";
	const char* const kFut1 = "DCE.i.2409";
	const char* const kFut2 = "DCE.j.2409";
}

using R171ReplayFixture = r161::ReplayFixture;
using R171DeferredFixture = r171::DeferredFixture;

// 生产: SelMocker.cpp:666-689（同向加仓分支；比率滑点 681-689）
// 0->1 走平/反手分支；1->2 时 pInfo._volume*diff>0 进入同向加仓，
// 10bp 比率滑点对 10.0、最小变动 0.01 的股票取整为 0.01，成交价 10.01。
TEST_F(R171ReplayFixture, R171Sel_RatioSlippageOnSameDirectionAdd)
{
	SelMocker sel(&_rp, "r171_sel_ratio", 10, true);

	// 先调度一次，避免 handle_replay_done 里 _emit_times==0（BUG-R171-001）
	sel.handle_schedule(20200102u, 930u);

	sel.stra_set_position(kStk1, 1);
	WTSTickData* t1 = r141::make_tick(kStk1, 20200102u, 93200000u, 10.0, 9.99, 10.01);
	sel.handle_tick(kStk1, t1, 0);
	t1->release();
	ASSERT_DOUBLE_EQ(1.0, sel.stra_get_position(kStk1));

	sel.stra_set_position(kStk1, 2);
	WTSTickData* t2 = r141::make_tick(kStk1, 20200102u, 93200000u, 10.0, 9.99, 10.01);
	sel.handle_tick(kStk1, t2, 0);
	t2->release();
	EXPECT_DOUBLE_EQ(2.0, sel.stra_get_position(kStk1));

	sel.handle_replay_done();
	const std::string trades = r171::read_text(_out / "r171_sel_ratio" / "trades.csv");
	EXPECT_NE(std::string::npos, trades.find("10.01"));
}

// 生产: SelMocker.cpp:737-742（平仓明细循环；742 为 min(明细量,剩余量)==0 的跳过）
// 期货非 T+1：目标仓位 0.1/0.3/0.6/1.0 分四次加上去，得到 0.1/0.2/0.3/0.4
// 四条明细；再降到 0.4（left=0.6）。前三段减完后剩余 5.55e-17（decimal::eq 视为 0），
// 第四条明细 maxQty≈0 必须跳过，而不是记一笔 0 手平仓。
TEST_F(R171ReplayFixture, R171Sel_MultiDetailPartialCloseSkipsZeroQty)
{
	SelMocker sel(&_rp, "r171_sel_details", 0, false);
	sel.handle_schedule(20200102u, 930u);

	WTSTickData* t0 = r141::make_tick(kFut1, 20200102u, 93200000u, 800.0, 799.0, 801.0);
	sel.handle_tick(kFut1, t0, 0);
	t0->release();

	const double targets[4] = { 0.1, 0.3, 0.6, 1.0 };
	for (double target : targets)
	{
		sel.stra_set_position(kFut1, target);
		WTSTickData* t = r141::make_tick(kFut1, 20200102u, 93200000u, 800.0, 799.0, 801.0);
		sel.handle_tick(kFut1, t, 0);
		t->release();
	}
	ASSERT_DOUBLE_EQ(1.0, sel.stra_get_position(kFut1));

	sel.stra_set_position(kFut1, 0.4);
	WTSTickData* t4 = r141::make_tick(kFut1, 20200102u, 93200000u, 800.0, 799.0, 801.0);
	sel.handle_tick(kFut1, t4, 0);
	t4->release();
	EXPECT_DOUBLE_EQ(0.4, sel.stra_get_position(kFut1));

	sel.handle_replay_done();
	const std::string closes = r171::read_text(_out / "r171_sel_details" / "closes.csv");
	// 表头 + 三条实际平仓明细；第四条零量明细若未跳过会多出一行
	EXPECT_EQ(4u, r171::line_count(closes));
}

// 生产: HftMocker.cpp:644-651（on_trade 更新目标仓位）、1149-1154（零量跳过）
// 直接走公开回调 on_trade：买 0.05/0.05/0.1/0.1 得到四条明细；再卖 0.2。
// 前三段刚好减完、剩余 1.39e-17，第四条明细 maxQty≈0 被跳过；closes.csv 表头+3 行。
TEST_F(R171ReplayFixture, R171Hft_MultiDetailPartialCloseSkipsZeroQty)
{
	HftMocker hft(&_rp, "r171_hft_details");

	const double buys[4] = { 0.05, 0.05, 0.1, 0.1 };
	for (uint32_t i = 0; i < 4; i++)
		hft.on_trade(i + 1, kFut1, true, buys[i], 800.0, "open");
	EXPECT_NEAR(0.3, hft.stra_get_position(kFut1), 1e-12);

	hft.on_trade(9u, kFut1, false, 0.2, 800.0, "close");
	EXPECT_NEAR(0.1, hft.stra_get_position(kFut1), 1e-9);

	hft.handle_replay_done();
	const std::string closes = r171::read_text(_out / "r171_hft_details" / "closes.csv");
	// 表头 + 三条平仓明细；剩余一条零量明细不应产生记录
	EXPECT_EQ(4u, r171::line_count(closes));
}

// 生产: CtaMocker.cpp:1694-1699（do_set_position 平仓明细循环；1699 零量跳过）
// 期货非 T+1：目标仓位 0.1/0.3/0.6/1.0 三次同向加仓得到四条明细，再降到 0.4。
// 前三段减完剩余 5.55e-17，第四条明细 maxQty≈0 被跳过，closes.csv 表头+3 行。
TEST_F(R171ReplayFixture, R171Cta_MultiDetailPartialCloseSkipsZeroQty)
{
	CtaMocker cta(&_rp, "r171_cta_details", 0, true, NULL, false);

	WTSTickData* t0 = r141::make_tick(kFut1, 20200102u, 93200000u, 800.0, 799.0, 801.0);
	cta.handle_tick(kFut1, t0, 0);
	t0->release();

	const double targets[4] = { 0.1, 0.3, 0.6, 1.0 };
	for (double target : targets)
	{
		cta.stra_set_position(kFut1, target);
		WTSTickData* t = r141::make_tick(kFut1, 20200102u, 93200000u, 800.0, 799.0, 801.0);
		cta.handle_tick(kFut1, t, 0);
		t->release();
	}
	ASSERT_DOUBLE_EQ(1.0, cta.stra_get_position(kFut1));

	cta.stra_set_position(kFut1, 0.4);
	WTSTickData* t4 = r141::make_tick(kFut1, 20200102u, 93200000u, 800.0, 799.0, 801.0);
	cta.handle_tick(kFut1, t4, 0);
	t4->release();
	EXPECT_DOUBLE_EQ(0.4, cta.stra_get_position(kFut1));

	cta.handle_replay_done();
	const std::string closes = r171::read_text(_out / "r171_cta_details" / "closes.csv");
	EXPECT_EQ(4u, r171::line_count(closes));
}

// 生产: UftMocker.cpp:1030-1047（stra_enum_position 空表路径）、1049-1051（stra_get_price）
// 空持仓表时循环不进入，返回 0 且不触碰 _strategy；stra_get_price 直通回放器价格缓存。
TEST_F(R171ReplayFixture, R171Uft_EnumPositionEmptyAndPrice)
{
	UftMocker uft(&_rp, "r171_uft_enum_empty");

	EXPECT_DOUBLE_EQ(0.0, uft.stra_enum_position(""));
	EXPECT_DOUBLE_EQ(0.0, uft.stra_get_price(kCu));
}

// 生产: UftMocker.cpp:1030-1047（stra_enum_position 的 bAll=false 与 continue）
// 先建立 kFut1 持仓，再用另一个合约 kFut2 查询：strcmp 不等 -> continue，
// 循环结束返回 0。刻意避开“同代码查询”那条会解引用空 _strategy 的路径。
TEST_F(R171ReplayFixture, R171Uft_EnumPositionMismatchSkipsEntry)
{
	UftMocker uft(&_rp, "r171_uft_enum_mismatch");

	ASSERT_EQ(1u, uft.stra_buy(kFut1, 0.0, 1).size());
	WTSTickData* t = r141::make_tick(kFut1, 20200102u, 93200000u, 800.0, 799.0, 801.0);
	uft.handle_tick(kFut1, t, 0);
	t->release();
	ASSERT_DOUBLE_EQ(1.0, uft.stra_get_position(kFut1));

	EXPECT_DOUBLE_EQ(0.0, uft.stra_enum_position(kFut2));
}

// 生产: HisDataReplayer.cpp:3182-3199（checkUnbars 主 K 线周期换算；3190-3192 m5 臂）
//       HisDataReplayer.cpp:3213-3224（m5 文件缺失时安全返回 false）
// 先以复权代码的 isMain=true 查询写入 _main_period="m#5" 与 _unsubbed_in_need，
// 再 prepare（tick=false 才调 checkUnbars），命中 realTimes%5==0 的 KP_Minute5 分支。
TEST_F(R171DeferredFixture, R171HisData_M5MainPeriodInCheckUnbars)
{
	EXPECT_EQ(nullptr, _rp.get_kline_slice("SSE.STK.000001+", "m", 5, 1, true));
	ASSERT_TRUE(_rp.prepare());
	EXPECT_EQ(1u, _sink.initCalls);
}

// 生产: HisDataReplayer.cpp:2298-2325（m5 原始缓存命中后重采样；2321-2325 失败臂）
// 写入三根 m5 csv 后，用 times=0 查询：get_kline_slice 会先加载 m5 原始缓存，
// 再调 WTSDataFactory::extractKlineData(times<=1) 返回 NULL，走 2321-2325 的错误
// 日志并返回 NULL（而不是崩溃或返回脏切片）。
TEST_F(R171ReplayFixture, R171HisData_ResampleTimesZeroRejected)
{
	ASSERT_TRUE(r171::write_m5_csv(_base, kStk1, 10.0));

	EXPECT_EQ(nullptr, _rp.get_kline_slice(kStk1, "m", 10, 0));
}

// 生产: HisDataReplayer.cpp:2899-2904（get_last_tick 的 lower_bound 跨日比较；2901）
// 同一个 tick 块里放 20200102/20200103 两笔，当前日期为 20200102 时，
// lower_bound 会比较到不同 action_date 的元素并执行 2901 的日期分支。
TEST_F(R171ReplayFixture, R171HisData_LastTickCrossDateComparator)
{
	ASSERT_TRUE(r141::write_text(
		_base / "bin" / "ticks" / (std::string(kCu) + "_tick_20200102.dsb"),
		r171::make_cross_date_tick_block(20200103u, 70000.0)));

	WTSTickData* last = _rp.get_last_tick(kCu);
	ASSERT_NE(nullptr, last);
	EXPECT_DOUBLE_EQ(70001.0, last->price());
	last->release();
}

// BUG-R171-001（待审核）：SelMocker::handle_replay_done 直接用 _emit_times 做除数
// （SelMocker.cpp:303-307），构造函数把它置 0；未经过任何 on_schedule 就结束回放时
// 整数除零（SIGFPE）。启用即崩溃，故禁用；保留为缺陷复现占位，不放宽断言。
TEST_F(R171ReplayFixture, DISABLED_R171Sel_ReplayDoneWithoutScheduleDividesByZero)
{
	SelMocker sel(&_rp, "r171_sel_divzero", 0, false);
	sel.handle_replay_done();
	FAIL() << "BUG-R171-001: SelMocker::handle_replay_done divides by _emit_times";
}
