/**
 * r62_hft_mocker_gap_test.cpp  (lane R62)
 *
 * 靶子：src/WtBtCore/HftMocker.cpp 在 p08 之后仍未覆盖的块：
 *   init_hft_factory(L215-253)          —— use_newpx/error_rate/match_this_tick 解析后模块失败
 *   on_tick 两条撮合顺序分支(L345-430)  —— match_this_tick = true/false
 *   on_trade -> do_set_position(L1092+) —— 开仓/减仓/全平/反手（T+1 冻结）
 *   update_dyn_profit(L665-699)         —— 多空持仓 + 真实 bid/ask tick
 *   on_session_begin/end(L486-531)      —— 冻结释放与资金日志
 *   stra_get_bars/切片/user_data(L851-1023)、dump_outputs(L1024-1077)
 *
 * 不使用 #define private public；持仓通过对 public 的 on_trade 注入。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>

#include <WtBtCore/HftMocker.h>

#include "r62_mocker_common.h"

namespace
{
	const char* const kRb = "SHFE.rb.2405";
	const char* const kCu = "SHFE.cu.2406";
	const char* const kStk = "SSE.STK.000001";
}

class R62HftMockerGap : public r62::ReplayFixture
{
protected:
	void feed(HftMocker& m, const char* code, double last, double bid, double ask)
	{
		WTSTickData* t = r62::make_tick(code, 20240102u, 93000000u, last, bid, ask);
		m.handle_tick(code, t, 0);
		t->release();
	}
};

// 工厂配置先解析匹配参数，再因模块不存在失败；match_this_tick=true 时
// on_tick 走「先策略后订单」分支，无策略/无订单也必须安全（L215-253、345-430）
TEST_F(R62HftMockerGap, InitFactoryParsesFlagsThenFailsMissingModule)
{
	HftMocker mocker(&_rp, "r62_hft_init");

	EXPECT_FALSE(mocker.init_hft_factory(nullptr));

	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("module", "r62_no_such_hft_strategy_module");
	cfg->append("use_newpx", true);
	cfg->append("error_rate", static_cast<uint32_t>(0));
	cfg->append("match_this_tick", true);
	EXPECT_FALSE(mocker.init_hft_factory(cfg));
	cfg->release();

	mocker.stra_sub_ticks(kRb);
	feed(mocker, kRb, 3800.0, 3799.0, 3801.0);

	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kRb));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_undone(kRb));
}

// 股票 T+1 生命周期：开仓冻结 -> 跨日解冻 -> 部分平仓 -> 全平；
// tick 驱动多头的浮动盈亏（买一价），会话结束/回放结束落盘（L486-531、665-699、1092+）
TEST_F(R62HftMockerGap, StockT1LifecycleWithTickProfitAndDump)
{
	HftMocker mocker(&_rp, "r62_hft_stock");

	// dist/common/stk_comms.json 没有 trademode 字段，解析默认 TM_Both
	// （WTSBaseDataMgr.cpp:313-316）；本用例要验证 T+1 冻结/解冻，必须显式把
	// 该品种改成 TM_LongT1，否则 do_set_position 不会累计 _frozen。
	WTSCommodityInfo* stkComm = _rp.get_commodity_info(kStk);
	ASSERT_NE(nullptr, stkComm);
	stkComm->setTradingMode(TM_LongT1);

	mocker.on_trade(1u, kStk, true, 100.0, 10.0, "");
	EXPECT_DOUBLE_EQ(100.0, mocker.stra_get_position(kStk, false));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kStk, true));
	EXPECT_DOUBLE_EQ(10.0, mocker.stra_get_position_avgpx(kStk));

	feed(mocker, kStk, 10.2, 10.2, 10.3);
	EXPECT_NEAR(20.0, mocker.stra_get_position_profit(kStk), 1e-9);

	mocker.handle_session_begin(20240103u);
	EXPECT_DOUBLE_EQ(100.0, mocker.stra_get_position(kStk, true));

	mocker.on_trade(2u, kStk, false, 40.0, 10.5, "");
	EXPECT_DOUBLE_EQ(60.0, mocker.stra_get_position(kStk));
	// 平 40 手盈利 20，剩余 60 手浮盈按 10.2 的旧值等比缩放为 12
	EXPECT_NEAR(12.0, mocker.stra_get_position_profit(kStk), 1e-9);

	mocker.on_trade(3u, kStk, false, 60.0, 10.8, "");
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kStk));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position_profit(kStk));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position_avgpx(kStk));

	mocker.handle_session_end(20240104u);
	mocker.handle_replay_done();

	const std::filesystem::path dir = _out / "r62_hft_stock";
	EXPECT_EQ(4u, r62::line_count(r62::read_file(dir / "trades.csv")));
	EXPECT_EQ(3u, r62::line_count(r62::read_file(dir / "closes.csv")));
	EXPECT_EQ(2u, r62::line_count(r62::read_file(dir / "funds.csv")));
	EXPECT_EQ(1u, r62::line_count(r62::read_file(dir / "positions.csv")));
	EXPECT_NE(std::string::npos, r62::read_file(dir / "trades.csv").find("CLOSE"));
}

// 空头：开仓均价、tick 浮亏（卖一价）、部分平仓盈亏符号（L665-699、1092+）
TEST_F(R62HftMockerGap, ShortPositionProfitSignAndPartialClose)
{
	HftMocker mocker(&_rp, "r62_hft_short");

	mocker.on_trade(1u, kCu, false, 2.0, 70000.0, "");
	EXPECT_DOUBLE_EQ(-2.0, mocker.stra_get_position(kCu));
	// 空头均价是有符号口径（q02 已固化），这里不重复断言
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position_profit(kCu));

	// 空头按卖一价 70100 计浮亏：2*(70000-70100)*5 = -1000
	feed(mocker, kCu, 70100.0, 70090.0, 70100.0);
	EXPECT_DOUBLE_EQ(-1000.0, mocker.stra_get_position_profit(kCu));

	// 以 69900 平 1 手：空头盈利 500，剩余 1 手浮盈等比缩放为 -500
	mocker.on_trade(2u, kCu, true, 1.0, 69900.0, "");
	EXPECT_DOUBLE_EQ(-1.0, mocker.stra_get_position(kCu));
	EXPECT_DOUBLE_EQ(-500.0, mocker.stra_get_position_profit(kCu));

	// 全平后均价归零
	mocker.on_trade(3u, kCu, true, 1.0, 69500.0, "");
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kCu));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position_avgpx(kCu));
}

// 订阅/切片/user_data：tick 关闭时 tick/Level2 切片为空；日线可读；
// 用户数据写后读并落入 ud_*.json（L851-1077）
TEST_F(R62HftMockerGap, SubscriptionsSlicesAndUserDataDump)
{
	HftMocker mocker(&_rp, "r62_hft_query");
	_rp.register_sink(&mocker, "r62_hft_query");
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
	EXPECT_EQ(std::string(""), mocker.stra_get_rawcode(kCu));
	ASSERT_NE(nullptr, mocker.stra_get_comminfo(kCu));
	// prepare() 已把回放游标设到配置 stime=20200102 09:30
	// （HisDataReplayer.cpp:662-664），三个 getter 直接返回 _cur_date/_cur_time
	// （HisDataReplayer.h:887-901），因此不是 0。
	EXPECT_EQ(20200102u, mocker.stra_get_date());
	EXPECT_EQ(930u, mocker.stra_get_time());
	EXPECT_EQ(0u, mocker.stra_get_secs());

	EXPECT_EQ(std::string("def"), mocker.stra_load_user_data("r62_missing", "def"));
	mocker.stra_save_user_data("r62_hft_key", "r62_hft_value");
	EXPECT_EQ(std::string("r62_hft_value"), mocker.stra_load_user_data("r62_hft_key", "def"));

	WTSBarStruct bar = r62::make_bar(20200102u, 931u, 10.5);
	mocker.handle_order_queue(kCu, nullptr);
	mocker.handle_order_detail(kCu, nullptr);
	mocker.handle_transaction(kCu, nullptr);
	mocker.handle_bar_close(kCu, "m1", 1, &bar);
	mocker.handle_schedule(20200102u, 931u);

	mocker.handle_replay_done();
	const std::string ud = r62::read_file(_out / "r62_hft_query" / "ud_r62_hft_query.json");
	ASSERT_FALSE(ud.empty());
	EXPECT_NE(std::string::npos, ud.find("\"r62_hft_key\""));
	EXPECT_NE(std::string::npos, ud.find("r62_hft_value"));
}

// 未订阅、未持仓时的 tick 只更新价格缓存；事件分发与日志族无策略安全
// （L345-475、989-1022）
TEST_F(R62HftMockerGap, TickWithoutSubscriptionAndLoggingAreSideEffectFree)
{
	HftMocker mocker(&_rp, "r62_hft_tick");

	feed(mocker, kRb, 3800.0, 3799.0, 3801.0);
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kRb));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position_profit(kRb));

	mocker.stra_log_info("r62 info");
	mocker.stra_log_debug("r62 debug");
	mocker.stra_log_warn("r62 warn");
	mocker.stra_log_error("r62 error");
	mocker.enable_hook(false);

	WTSBarStruct bar = r62::make_bar(20240102u, 931u, 3805.0);
	mocker.handle_bar_close(kRb, "m1", 1, &bar);
	mocker.handle_schedule(20240102u, 931u);
}
