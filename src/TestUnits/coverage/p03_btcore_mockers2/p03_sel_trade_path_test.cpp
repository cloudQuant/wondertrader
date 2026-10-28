/*!
 * p03_sel_trade_path_test.cpp
 * Lane p03: SelMocker 下单—成交—持仓—盈亏推进主路径(股票选股模拟)
 * stra_set_position -> append_signal -> proc_tick -> do_set_position -> update_dyn_profit
 * 固定输入: 2024-01-02 交易日, SSE.600000 股票合约(T+1, volscale=1), 无滑点
 */
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <utility>

#include "Includes/WTSContractInfo.hpp"
#include "WtBtCore/WtHelper.h"

#define private public
#define protected public
#include "WtBtCore/HisDataReplayer.h"
#include "WtBtCore/SelMocker.h"
#undef protected
#undef private

namespace
{
// 浮点金额断言口径说明（主控裁定，见 docs/迭代计划/迭代01/reports/FAIL裁定记录-20261027.md）：
// 动盈亏/平仓盈亏是多步累加的结果，实测残差量级 1e-13 ~ 8ULP（例：-0.2 vs -0.20000000000000018、
// 5.0 vs 5.0000000000000711）。EXPECT_DOUBLE_EQ 默认 4ULP 容差比这个更紧，会把正常的
// 累加顺序差判成失败。这几处因此改成 EXPECT_NEAR(..., 1e-9)：
// 容差比业务上最小的可检查量（一手一分钱）小 7 个数量级，仍然能被任何真实口径错误证伪。
// 这不是把断言改成「非空即过」——比较值本身没动。

	static void sel_add_comm(HisDataReplayer& rp, const char* exchg, const char* pid,
		uint32_t volScale, double pxTick, TradingMode mode)
	{
		WTSCommodityInfo* cInfo = WTSCommodityInfo::create(pid, pid, exchg, "TRADING", "CHINA");
		cInfo->setVolScale(volScale);
		cInfo->setPriceTick(pxTick);
		cInfo->setTradingMode(mode);
		rp._bd_mgr._fullpid_map->add(cInfo->getFullPid(), cInfo, true);
	}

	static void sel_set_fee(HisDataReplayer& rp, const char* fullPid, double dOpen, double dClose)
	{
		HisDataReplayer::FeeItem& f = rp._fee_map[fullPid];
		f._by_volume = true;
		f._open = dOpen;
		f._close = dClose;
		f._close_today = dClose;
		f._min_fee = 0.0;
	}
}

class P03SelTradePath : public ::testing::Test
{
protected:
	HisDataReplayer _rp;
	void SetUp() override
	{
		auto dir = std::filesystem::temp_directory_path() / "p03_sel_trade_path";
		std::filesystem::create_directories(dir);
		WtHelper::setOutputDir(dir.string().c_str());

		_rp._cur_date = 20240102;
		_rp._cur_time = 93500;
		_rp._cur_secs = 0;
		_rp._cur_tdate = 20240102;

		sel_add_comm(_rp, "SSE", "600000", 1, 0.01, TM_LongT1);
		// 双向可开合约用于覆盖反向路径
		sel_add_comm(_rp, "SSE", "510050", 1, 0.001, TM_Both);
		sel_set_fee(_rp, "SSE.600000", 0.5, 0.5);
		sel_set_fee(_rp, "SSE.510050", 1.0, 1.0);
	}

	void TearDown() override
	{
		WtHelper::setOutputDir("");
		std::error_code ec;
		std::filesystem::remove_all(std::filesystem::temp_directory_path() / "p03_sel_trade_path", ec);
	}
};

TEST_F(P03SelTradePath, SetPositionGuardBranches)
{
	const char* code = "SSE.600000";
	SelMocker sel(&_rp, "p03_sel_guard", 0);

	// 未知合约早退
	sel.stra_set_position("SSE.000000", 100, "x");
	EXPECT_TRUE(sel._sig_map.empty());

	// 目标 0 且当前 0: 相等早退
	sel.stra_set_position(code, 0, "noop");
	EXPECT_TRUE(sel._sig_map.empty());

	// 股票禁做空
	sel.stra_set_position(code, -10, "short");
	EXPECT_TRUE(sel._sig_map.empty());

	// 建仓后: 目标低于冻结量被拒绝
	sel._price_map[code] = std::make_pair(10.0, 10.0);
	sel.stra_set_position(code, 100, "buy");
	ASSERT_EQ(1u, sel._sig_map.count(code));
	// 非调度状态下单: 信号直接标记为已触发
	EXPECT_TRUE(sel._sig_map[code]._triggered);
	EXPECT_DOUBLE_EQ(100.0, sel._sig_map[code]._volume);
	EXPECT_DOUBLE_EQ(10.0, sel._sig_map[code]._sigprice);

	sel.proc_tick(code, 10.0, 10.01);
	EXPECT_TRUE(sel._sig_map.empty());
	EXPECT_DOUBLE_EQ(100.0, sel.stra_get_position(code, false));
	// T+1: 当日买入冻结
	const SelMocker::PosInfo& pInfo = sel._pos_map[code];
	EXPECT_DOUBLE_EQ(100.0, pInfo._frozen);
	EXPECT_DOUBLE_EQ(0.0, sel.stra_get_position(code, true));
	EXPECT_DOUBLE_EQ(0.0, pInfo.valid());

	sel.stra_set_position(code, 50, "lt");
	EXPECT_TRUE(sel._sig_map.empty());
	sel.stra_set_position(code, 100, "same");
	EXPECT_TRUE(sel._sig_map.empty());
}

TEST_F(P03SelTradePath, OpenDynProfitThenTrimAndFund)
{
	const char* code = "SSE.600000";
	SelMocker sel(&_rp, "p03_sel_trades", 0);
	sel._price_map[code] = std::make_pair(10.0, 10.0);

	sel.stra_set_position(code, 100, "buy100");
	sel.proc_tick(code, 10.0, 10.01);
	SelMocker::PosInfo& pInfo = sel._pos_map[code];
	ASSERT_EQ(1u, pInfo._details.size());
	const SelMocker::DetailInfo& d = pInfo._details[0];
	EXPECT_TRUE(d._long);
	EXPECT_DOUBLE_EQ(10.01, d._price);
	EXPECT_DOUBLE_EQ(100.0, d._volume);
	EXPECT_EQ((uint64_t)20240102 * 10000ULL + 93500, d._opentime);
	EXPECT_EQ(20240102u, d._opentdate);
	EXPECT_STREQ("buy100", d._opentag);
	// 开仓按量费 0.5*100 = 50
	EXPECT_DOUBLE_EQ(50.0, sel._fund_info._total_fees);
	// 开仓时间/入仓价透传
	EXPECT_EQ(d._opentime, sel.stra_get_last_entertime(code));
	EXPECT_DOUBLE_EQ(10.01, sel.stra_get_last_enterprice(code));

	// 浮盈推进: 现价 10.06 -> 100*(10.06-10.01) = 5
	sel.proc_tick(code, 10.01, 10.06);
	EXPECT_NEAR(5.0, pInfo._dynprofit, 1e-9);
	EXPECT_NEAR(5.0, sel._fund_info._total_dynprofit, 1e-9);
	EXPECT_DOUBLE_EQ(10.06, pInfo._details[0]._max_price);

	// 次日解冻后减仓 40 手, 成交价 10.04
	pInfo._frozen = 0;
	sel.stra_set_position(code, 60, "cut40");
	sel.proc_tick(code, 10.06, 10.04);
	EXPECT_DOUBLE_EQ(60.0, sel.stra_get_position(code, false));
	// 平掉最早明细 40 手: profit = (10.04-10.01)*40*1 = 1.2
	EXPECT_NEAR(1.2, pInfo._closeprofit, 1e-9);
	EXPECT_NEAR(1.2, sel._fund_info._total_profit, 1e-9);
	// 平仓费 0.5*40 = 20, 累计 70
	EXPECT_DOUBLE_EQ(70.0, sel._fund_info._total_fees);
	// 明细剩余 60
	ASSERT_EQ(1u, pInfo._details.size());
	EXPECT_DOUBLE_EQ(60.0, pInfo._details[0]._volume);
	// 浮盈按剩余手数与**当前价**重算：本步 proc_tick(10.06, 10.04) 把现价推到 10.04，
	// 剩余 60 手开仓 10.01 ⇒ 60*(10.04-10.01)=1.8。原期望 4.0 既不是这个价（10.06 时是 3.0）
	// 也不是等比缩放的算法，属本用例自己的算术笔误；产品值 1.8 与输入相符。裁定见
	// docs/迭代计划/迭代01/reports/FAIL裁定记录-20261027.md（A-05）
	EXPECT_NEAR(1.8, pInfo._dynprofit, 1e-9);	// 裁：原期望 4.0 与本用例输入不一致
	EXPECT_EQ((uint64_t)20240102 * 10000ULL + 93500, pInfo._last_exittime);

	// 全平: 仓位清零且浮盈清零
	sel.stra_set_position(code, 0, "flat");
	sel.proc_tick(code, 10.04, 10.10);
	EXPECT_DOUBLE_EQ(0.0, sel.stra_get_position(code, false));
	EXPECT_DOUBLE_EQ(0.0, pInfo._dynprofit);
	EXPECT_TRUE(pInfo._details.empty());
	// 尾仓 60 手以 10.10 平: (10.10-10.01)*60 = 5.4, 累计平仓盈亏 6.6
	EXPECT_NEAR(6.6, pInfo._closeprofit, 1e-9);
	EXPECT_NEAR(6.6, sel._fund_info._total_profit, 1e-9);
}

TEST_F(P03SelTradePath, EtfReversalFromLongToShort)
{
	const char* code = "SSE.510050";
	SelMocker sel(&_rp, "p03_sel_rev", 0);
	sel._price_map[code] = std::make_pair(2.5, 2.5);

	sel.stra_set_position(code, 10, "l10");
	sel.proc_tick(code, 2.5, 2.52);
	SelMocker::PosInfo& pInfo = sel._pos_map[code];
	// 双向合约不冻结
	EXPECT_DOUBLE_EQ(0.0, pInfo._frozen);
	EXPECT_DOUBLE_EQ(10.0, pInfo.valid());

	// 直接翻空到 -6: 先平 10 手, 剩余额度 16 反向开仓
	sel.stra_set_position(code, -6, "s6");
	sel.proc_tick(code, 2.52, 2.50);
	EXPECT_DOUBLE_EQ(-6.0, sel.stra_get_position(code, false));
	// 平多 10 手: (2.50-2.52)*10 = -0.2
	EXPECT_NEAR(-0.2, pInfo._closeprofit, 1e-9);
	ASSERT_EQ(1u, pInfo._details.size());
	EXPECT_FALSE(pInfo._details[0]._long);
	EXPECT_DOUBLE_EQ(2.50, pInfo._details[0]._price);
	EXPECT_DOUBLE_EQ(6.0, pInfo._details[0]._volume);
	// 空单浮盈: 现价 2.49 -> (2.50-2.49)*6 = 0.06
	sel.update_dyn_profit(code, 2.49);
	EXPECT_NEAR(0.06, pInfo._dynprofit, 1e-9);
	// 费用: 开 10*1 + 平 10*1 + 反手开 6*1 = 26
	EXPECT_DOUBLE_EQ(26.0, sel._fund_info._total_fees);
}
