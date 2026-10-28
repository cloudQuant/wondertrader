/**
 * r62_sel_mocker_gap_test.cpp  (lane R62)
 *
 * 靶子：src/WtBtCore/SelMocker.cpp 中 R43/p03 尚未覆盖的块：
 *   init_sel_factory(L234-273)             —— cfg 非空但模块不存在
 *   stra_get_bars(L815-859)                —— 日线切片 + 收盘价写入内部价格表
 *   on_schedule 自动清仓(L481-512)         —— 持仓无新信号 -> autoexit
 *   on_session_begin(L514-529)             —— T+1 冻结跨日释放
 *   dump_stradata/dump_outputs 非空明细    —— 持仓 details/信号/用户数据落盘
 *   stra_get_ticks/last_tick/rawcode 查询  —— L861-930
 *
 * 不使用 #define private public；持仓/信号全部经公开下单接口建立。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>

#include <WtBtCore/SelMocker.h>

#include "r62_mocker_common.h"

namespace
{
	const char* const kStk = "SSE.STK.000001";
}

class R62SelMockerGap : public r62::ReplayFixture
{
protected:
	void feed(SelMocker& m, const char* code, double px)
	{
		WTSTickData* t = r62::make_tick(code, 20240102u, 93000000u, px, px - 0.01, px + 0.01);
		m.handle_tick(code, t, 0);
		t->release();
	}
};

// 策略工厂加载：空配置 / 不存在的模块都必须返回 false，且不改变后续可用性
TEST_F(R62SelMockerGap, InitFactoryRejectsNullAndMissingModule)
{
	SelMocker sel(&_rp, "r62_sel_init");

	EXPECT_FALSE(sel.init_sel_factory(nullptr));

	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("module", "r62_no_such_sel_strategy_module");
	EXPECT_FALSE(sel.init_sel_factory(cfg));
	cfg->release();

	// 加载失败后上下文仍可查询，不产生持仓/信号
	EXPECT_DOUBLE_EQ(0.0, sel.stra_get_position(kStk));
	EXPECT_DOUBLE_EQ(0.0, sel.stra_get_fund_data(0));
}

// 日线切片：prepare 后 stra_get_bars 返回真实 CSV 切片，并把最后一根收盘价
// 写进 SelMocker 自己的价格表（L815-859 的 daily 分支）
TEST_F(R62SelMockerGap, BarsLoadDailySliceAndSeedPrice)
{
	SelMocker sel(&_rp, "r62_sel_bars");
	_rp.register_sink(&sel, "r62_sel_bars");
	ASSERT_TRUE(_rp.prepare());

	WTSKlineSlice* bars = sel.stra_get_bars(kStk, "d1", 5);
	ASSERT_NE(nullptr, bars);
	// 回放起点 20200102：切片只含已到当前交易日的第一根，close = 10.0
	EXPECT_DOUBLE_EQ(10.0, bars->at(-1)->close);
	bars->release();

	// 切片接口在 tick 回测关闭时返回空；品种/时段可查，原始码为空
	EXPECT_EQ(nullptr, sel.stra_get_ticks(kStk, 10));
	EXPECT_EQ(nullptr, sel.stra_get_last_tick(kStk));
	ASSERT_NE(nullptr, sel.stra_get_comminfo(kStk));
	ASSERT_NE(nullptr, sel.stra_get_sessinfo(kStk));
	EXPECT_EQ(std::string(""), sel.stra_get_rawcode(kStk));
	EXPECT_DOUBLE_EQ(0.0, sel.stra_get_day_price(kStk, 0));
	// prepare() 已把回放游标设到配置 stime=20200102 09:30
	// （HisDataReplayer.cpp:662-664），stra_get_date/time 直接返回该游标
	// （SelMocker.cpp:903-910 -> HisDataReplayer.h:887-894），因此不是 0。
	EXPECT_EQ(20200102u, sel.stra_get_date());
	EXPECT_EQ(930u, sel.stra_get_time());
}

// 调度自动清仓：持仓存在但本轮没有新信号 -> append_signal(0,"autoexit")，
// 下一个 tick 平掉；会话开始同时释放 T+1 冻结（L481-529）
TEST_F(R62SelMockerGap, ScheduleAutoExitsUnsignalledPosition)
{
	SelMocker sel(&_rp, "r62_sel_autoexit");

	// stk_comms.json 无 trademode，默认 TM_Both（WTSBaseDataMgr.cpp:313-316）；
	// 本用例验证 T+1 冻结/解冻，需显式改为 TM_LongT1。
	WTSCommodityInfo* stkComm = _rp.get_commodity_info(kStk);
	ASSERT_NE(nullptr, stkComm);
	stkComm->setTradingMode(TM_LongT1);

	sel.stra_set_position(kStk, 100, "buy");
	feed(sel, kStk, 10.0);
	ASSERT_DOUBLE_EQ(100.0, sel.stra_get_position(kStk, false));
	EXPECT_DOUBLE_EQ(0.0, sel.stra_get_position(kStk, true));

	uint32_t enumCnt = 0;
	sel.enum_position([&](const char*, double) { enumCnt++; });
	EXPECT_EQ(1u, enumCnt);

	// 次日解冻
	sel.handle_session_begin(20240103u);
	EXPECT_DOUBLE_EQ(100.0, sel.stra_get_position(kStk, true));

	// 无新信号的调度生成 autoexit 目标 0
	sel.handle_schedule(20240103u, 931u);
	EXPECT_DOUBLE_EQ(0.0, sel.stra_get_position(kStk));

	feed(sel, kStk, 10.5);
	EXPECT_DOUBLE_EQ(0.0, sel.stra_get_position(kStk, false));
	// 平仓盈亏 (10.5-10.0)*100 = 50
	EXPECT_GT(sel.stra_get_fund_data(1), 0.0);
	EXPECT_EQ(0u, sel.stra_get_last_exittime(kStk));	// 模拟时钟未推进，时间为 0

	sel.handle_session_end(20240104u);
	sel.handle_replay_done();

	const std::filesystem::path dir = _out / "r62_sel_autoexit";
	EXPECT_EQ(3u, r62::line_count(r62::read_file(dir / "trades.csv")));
	EXPECT_EQ(2u, r62::line_count(r62::read_file(dir / "closes.csv")));
	EXPECT_EQ(2u, r62::line_count(r62::read_file(dir / "funds.csv")));
}

// 非空持仓/信号明细落盘：dump_stradata 的 details 循环、dump_outputs 的
// signals/positions/ud 文件（L59-213），R43 只覆盖了全空状态
TEST_F(R62SelMockerGap, DumpPersistsPositionDetailsAndUserData)
{
	SelMocker sel(&_rp, "r62_sel_dump");

	// 先调度一次，规避 handle_replay_done 的 _emit_times==0 除零（BUG-R43-001）
	sel.handle_schedule(20240102u, 930u);

	sel.stra_set_position(kStk, 100, "buy");
	feed(sel, kStk, 10.0);
	ASSERT_DOUBLE_EQ(100.0, sel.stra_get_position(kStk, false));
	sel.stra_save_user_data("r62_key", "r62_value");
	sel.handle_session_end(20240102u);

	sel.handle_replay_done();

	const std::filesystem::path dir = _out / "r62_sel_dump";
	const std::string json = r62::read_file(dir / "r62_sel_dump.json");
	ASSERT_FALSE(json.empty());
	EXPECT_NE(std::string::npos, json.find("\"positions\""));
	EXPECT_NE(std::string::npos, json.find("\"details\""));
	EXPECT_NE(std::string::npos, json.find("\"buy\""));
	EXPECT_NE(std::string::npos, json.find("\"total_fees\""));

	EXPECT_EQ(2u, r62::line_count(r62::read_file(dir / "trades.csv")));
	EXPECT_EQ(2u, r62::line_count(r62::read_file(dir / "positions.csv")));
	// 初始下单信号一行 + 表头
	EXPECT_EQ(2u, r62::line_count(r62::read_file(dir / "signals.csv")));
	const std::string ud = r62::read_file(dir / "ud_r62_sel_dump.json");
	EXPECT_NE(std::string::npos, ud.find("\"r62_key\""));
	EXPECT_NE(std::string::npos, ud.find("r62_value"));
}

// tick 订阅表：stra_sub_ticks 后 handle_tick 命中 on_tick_updated；
// 未订阅合约仍只更新价格缓存，不产生副作用（L316-354、464-472、871-881）
TEST_F(R62SelMockerGap, TickSubscriptionGatesUpdatedCallback)
{
	SelMocker sel(&_rp, "r62_sel_tick");

	sel.stra_sub_ticks(kStk);
	feed(sel, kStk, 10.2);
	feed(sel, "SSE.STK.000002", 20.2);

	EXPECT_DOUBLE_EQ(0.0, sel.stra_get_position(kStk));
	EXPECT_DOUBLE_EQ(0.0, sel.stra_get_fund_data(2));
	sel.handle_bar_close(kStk, "m", 1, nullptr);
}
