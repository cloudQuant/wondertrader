/**
 * r62_uft_mocker_gap_test.cpp  (lane R62)
 *
 * 靶子：src/WtBtCore/UftMocker.cpp 在 p08 之后仍未覆盖的块：
 *   init_uft_factory(L130-167)          —— use_newpx/error_rate/match_this_tick 解析后模块失败
 *   on_tick 两条撮合顺序分支(L228-287)  —— match_this_tick = true/false
 *   on_trade -> update_position(L1155+) —— 开仓/平仓/平今的真实成交链
 *   update_dyn_profit(L828-887)         —— 多空持仓 + 真实 bid/ask tick
 *   stra_sub_xxx/切片查询(L977-1094)      —— 订阅、tick/ordque/orddtl/trans 切片
 *   dump_outputs(L1112-1153)            —— 有成交/平仓时的落盘
 *
 * 不使用 #define private public；持仓通过对 public 的 on_trade 注入。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>

#include <WtBtCore/UftMocker.h>

#include "r62_mocker_common.h"

namespace
{
	const char* const kRb = "SHFE.rb.2405";
	const char* const kCu = "SHFE.cu.2406";
	const char* const kAg = "SHFE.ag.2406";
}

class R62UftMockerGap : public r62::ReplayFixture
{
protected:
	void feed(UftMocker& m, const char* code, double px, double bid, double ask)
	{
		WTSTickData* t = r62::make_tick(code, 20240102u, 93000000u, px, bid, ask);
		m.handle_tick(code, t, 0);
		t->release();
	}
};

// 工厂配置先解析匹配参数，再因模块不存在失败；match_this_tick=true 时
// on_tick 走「先策略后订单」分支，无策略/无订单也必须安全（L130-167、228-287）
TEST_F(R62UftMockerGap, InitFactoryParsesFlagsThenFailsMissingModule)
{
	UftMocker mocker(&_rp, "r62_uft_init");

	EXPECT_FALSE(mocker.init_uft_factory(nullptr));

	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("module", "r62_no_such_uft_strategy_module");
	cfg->append("use_newpx", true);
	cfg->append("error_rate", static_cast<uint32_t>(0));
	cfg->append("match_this_tick", true);
	EXPECT_FALSE(mocker.init_uft_factory(cfg));
	cfg->release();

	mocker.stra_sub_ticks(kRb);
	feed(mocker, kRb, 3800.0, 3799.0, 3801.0);

	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kRb));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_undone(kRb));
}

// 成交回写 + tick 浮盈：开多/平昨/平今、开空/平空全链（L828-887、1155-1321）
TEST_F(R62UftMockerGap, TradeLifecycleAndTickProfit)
{
	UftMocker mocker(&_rp, "r62_uft_trade");

	// 多开 3 手
	mocker.on_trade(1u, kRb, true, 0, 3.0, 3800.0);
	EXPECT_DOUBLE_EQ(3.0, mocker.stra_get_position(kRb, false, 1));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kRb, false, 2));
	EXPECT_DOUBLE_EQ(3.0, mocker.stra_get_position(kRb, false, 3));
	EXPECT_DOUBLE_EQ(3.0, mocker.stra_get_local_position(kRb));

	// 真实 tick 驱动 update_dyn_profit（多头按买一价）
	feed(mocker, kRb, 3799.0, 3799.0, 3801.0);
	EXPECT_DOUBLE_EQ(3.0, mocker.stra_get_position(kRb));

	// 平昨 1 手、平今 1 手
	mocker.on_trade(2u, kRb, true, 1, 1.0, 3810.0);
	EXPECT_DOUBLE_EQ(2.0, mocker.stra_get_position(kRb));
	mocker.on_trade(3u, kRb, true, 2, 1.0, 3820.0);
	EXPECT_DOUBLE_EQ(1.0, mocker.stra_get_position(kRb));

	// 空开 2 手，再平 1 手
	mocker.on_trade(4u, kCu, false, 0, 2.0, 70000.0);
	EXPECT_DOUBLE_EQ(-2.0, mocker.stra_get_position(kCu, false, 3));
	EXPECT_DOUBLE_EQ(2.0, mocker.stra_get_position(kCu, false, 2));
	feed(mocker, kCu, 69900.0, 69900.0, 70100.0);
	mocker.on_trade(5u, kCu, false, 1, 1.0, 69900.0);
	EXPECT_DOUBLE_EQ(-1.0, mocker.stra_get_position(kCu));

	// 日期/时间只透传回放器（未回放时为 0），不读挂钟
	EXPECT_EQ(0u, mocker.stra_get_date());
	EXPECT_EQ(0u, mocker.stra_get_time());
	EXPECT_EQ(0u, mocker.stra_get_secs());
	ASSERT_NE(nullptr, mocker.stra_get_comminfo(kRb));
}

// on_tick 的订单循环：无缓存 tick 时 procOrder 早退，订单保留；
// stra_cancel_all 的任务在下一次 on_tick 的 procTask 中排空（L228-287、407-471）
TEST_F(R62UftMockerGap, TickProcessesOrdersAndDrainsCancelTasks)
{
	UftMocker mocker(&_rp, "r62_uft_orders");

	uint32_t lid = mocker.stra_enter_long(kAg, 6000.0, 2.0);
	ASSERT_NE(0u, lid);
	EXPECT_DOUBLE_EQ(2.0, mocker.stra_get_undone(kAg));

	// match_this_tick=false（默认）：先处理订单再回调策略；replayer 无 tick 缓存 -> 不成交
	feed(mocker, kAg, 6000.0, 5999.0, 6001.0);
	EXPECT_DOUBLE_EQ(2.0, mocker.stra_get_undone(kAg));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kAg));

	// 按合约撤单：任务入队，下一次 on_tick 排空后订单消失
	EXPECT_TRUE(mocker.stra_cancel_all(kAg).empty());
	EXPECT_TRUE(mocker.stra_cancel(lid));
	feed(mocker, kRb, 3800.0, 3799.0, 3801.0);
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_undone(kAg));
}

// 订阅与切片查询：stra_sub_* 全部转发；tick 关闭时切片为空；
// Level2 回调在无策略时安全（L977-1094、299-330）
TEST_F(R62UftMockerGap, SubscriptionsAndSliceQueries)
{
	UftMocker mocker(&_rp, "r62_uft_query");
	_rp.register_sink(&mocker, "r62_uft_query");
	ASSERT_TRUE(_rp.prepare());

	mocker.stra_sub_ticks(kCu);
	mocker.stra_sub_order_queues(kCu);
	mocker.stra_sub_order_details(kCu);
	mocker.stra_sub_transactions(kCu);

	WTSKlineSlice* bars = mocker.stra_get_bars(kCu, "d1", 5);
	ASSERT_NE(nullptr, bars);
	EXPECT_DOUBLE_EQ(10.0, bars->at(-1)->close);
	bars->release();

	EXPECT_EQ(nullptr, mocker.stra_get_ticks(kCu, 5));
	EXPECT_EQ(nullptr, mocker.stra_get_order_queue(kCu, 5));
	EXPECT_EQ(nullptr, mocker.stra_get_order_detail(kCu, 5));
	EXPECT_EQ(nullptr, mocker.stra_get_transaction(kCu, 5));
	EXPECT_EQ(nullptr, mocker.stra_get_last_tick(kCu));

	WTSBarStruct bar = r62::make_bar(20200102u, 931u, 11.0);
	mocker.handle_order_queue(kCu, nullptr);
	mocker.handle_order_detail(kCu, nullptr);
	mocker.handle_transaction(kCu, nullptr);
	mocker.handle_bar_close(kCu, "m1", 1, &bar);
	mocker.handle_schedule(20200102u, 931u);
}

// 有成交/平仓时的落盘：trades/closes 非空，funds/positions 只有表头
// （未调用会写会话汇总的接口），L1112-1153
TEST_F(R62UftMockerGap, DumpOutputsAfterTrades)
{
	UftMocker mocker(&_rp, "r62_uft_dump");

	mocker.on_trade(1u, kRb, true, 0, 2.0, 3800.0);
	mocker.on_trade(2u, kRb, true, 1, 2.0, 3810.0);
	mocker.handle_replay_done();

	const std::filesystem::path dir = _out / "r62_uft_dump";
	EXPECT_EQ(3u, r62::line_count(r62::read_file(dir / "trades.csv")));
	EXPECT_EQ(2u, r62::line_count(r62::read_file(dir / "closes.csv")));
	EXPECT_EQ(1u, r62::line_count(r62::read_file(dir / "funds.csv")));
	EXPECT_EQ(1u, r62::line_count(r62::read_file(dir / "positions.csv")));
	EXPECT_NE(std::string::npos, r62::read_file(dir / "trades.csv").find("CLOSE"));
}
