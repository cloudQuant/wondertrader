/*!
 * p03_cta_trade_path_test.cpp
 * Lane p03: CtaMocker 下单—成交—持仓—盈亏推进主路径
 * 用固定日期/价格 + 手工注册的 WTSCommodityInfo/费用模板驱动:
 * stra_enter_* -> append_signal -> proc_tick -> do_set_position -> update_dyn_profit
 * 断言成交均价、滑点、T+1 冻结、平今/平昨费用归属、平仓盈亏与资金账户累计
 */
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>

#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSObject.hpp"
#include "Share/CodeHelper.hpp"
#include "WtBtCore/WtHelper.h"

#define private public
#define protected public
#include "WtBtCore/HisDataReplayer.h"
#include "WtBtCore/CtaMocker.h"
#undef protected
#undef private

namespace
{
	// 注册一个期货合约(SHFE.rb)与一个股票合约(SSE.600001)到回放器的基础数据管理器
	static void add_comm(HisDataReplayer& rp, const char* exchg, const char* pid,
		uint32_t volScale, double pxTick, TradingMode mode)
	{
		WTSCommodityInfo* cInfo = WTSCommodityInfo::create(pid, pid, exchg, "TRADING", "CHINA");
		cInfo->setVolScale(volScale);
		cInfo->setPriceTick(pxTick);
		cInfo->setTradingMode(mode);
		// getCommodity(exchg,pid) 走 fullpid "EXCHG.pid"
		rp._bd_mgr._fullpid_map->add(cInfo->getFullPid(), cInfo, true);
	}

	static void set_fee(HisDataReplayer& rp, const char* fullPid, double dOpen, double dClose, double dCloseToday)
	{
		HisDataReplayer::FeeItem& f = rp._fee_map[fullPid];
		f._by_volume = true;
		f._open = dOpen;
		f._close = dClose;
		f._close_today = dCloseToday;
		f._min_fee = 0.0;
	}
}

class P03CtaTradePath : public ::testing::Test
{
protected:
	HisDataReplayer _rp;
	void SetUp() override
	{
		auto dir = std::filesystem::temp_directory_path() / "p03_cta_trade_path";
		std::filesystem::create_directories(dir);
		WtHelper::setOutputDir(dir.string().c_str());

		// 固定时钟(不断言墙钟): 2024-01-02 09:30:00, 交易日 2024-01-02
		_rp._cur_date = 20240102;
		_rp._cur_time = 93000;
		_rp._cur_secs = 0;
		_rp._cur_tdate = 20240102;

		add_comm(_rp, "SHFE", "rb", 10, 1.0, TM_Both);
		add_comm(_rp, "SSE", "600001", 1, 0.01, TM_LongT1);
		set_fee(_rp, "SHFE.rb", 1.0, 2.0, 3.0);
		set_fee(_rp, "SSE.600001", 0.5, 0.5, 0.5);
	}

	void TearDown() override
	{
		WtHelper::setOutputDir("");
		std::error_code ec;
		std::filesystem::remove_all(std::filesystem::temp_directory_path() / "p03_cta_trade_path", ec);
	}
};

// 未知合约: stra_enter_long/stra_set_position 早退, 不产生信号
TEST_F(P03CtaTradePath, EnterOnUnknownCodeIsRejected)
{
	CtaMocker mocker(&_rp, "p03_unknown", 0, false);
	mocker.stra_enter_long("DCE.m.2405", 3, "t1");
	mocker.stra_enter_short("DCE.m.2405", 3, "t2");
	mocker.stra_set_position("DCE.m.2405", 3, "t3");
	EXPECT_TRUE(mocker._sig_map.empty());
	EXPECT_TRUE(mocker._pos_map.empty());
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position("DCE.m.2405"));
}

// 多头推进: 开仓3手(2跳滑点) -> 当日部分平仓(平今费) -> 次日平昨 -> 反手做空
TEST_F(P03CtaTradePath, FuturesOpenCloseTodayCloseYesterdayReversal)
{
	const char* code = "SHFE.rb.2405";
	// 无滑点场景先验证均价与盈亏基数
	CtaMocker mocker(&_rp, "p03_fut", 0, false);
	mocker._price_map[code] = 3800.0;

	mocker.stra_enter_long(code, 3, "open3");
	ASSERT_EQ(1u, mocker._sig_map.count(code));
	const CtaMocker::SigInfo& sig = mocker._sig_map[code];
	EXPECT_DOUBLE_EQ(3.0, sig._volume);
	EXPECT_DOUBLE_EQ(3800.0, sig._sigprice);
	EXPECT_DOUBLE_EQ(0.0, sig._desprice);
	EXPECT_EQ((uint64_t)20240102 * 1000000000ULL + (uint64_t)93000 * 100000ULL, sig._gentime);
	// 非调度状态下 sigtype=1
	EXPECT_EQ(1u, sig._sigtype);

	// 以 3810 触发成交
	mocker.proc_tick(code, 3800.0, 3810.0);
	EXPECT_TRUE(mocker._sig_map.empty());
	EXPECT_DOUBLE_EQ(3.0, mocker.stra_get_position(code));
	// 无滑点: 成交价 3810, 均价 3810
	EXPECT_DOUBLE_EQ(3810.0, mocker.stra_get_position_avgpx(code));

	CtaMocker::PosInfo& pInfo = mocker._pos_map[code];
	ASSERT_EQ(1u, pInfo._details.size());
	const CtaMocker::DetailInfo& d = pInfo._details[0];
	EXPECT_TRUE(d._long);
	EXPECT_DOUBLE_EQ(3810.0, d._price);
	EXPECT_DOUBLE_EQ(3.0, d._volume);
	EXPECT_EQ((uint64_t)20240102 * 10000ULL + 93000, d._opentime);
	EXPECT_EQ(20240102u, d._opentdate);
	EXPECT_STREQ("open3", d._opentag);
	// 期货双向, 不冻结
	EXPECT_DOUBLE_EQ(0.0, pInfo._frozen);
	EXPECT_DOUBLE_EQ(3.0, pInfo.valid());

	// 浮动盈亏: 现价 3805 时 3*(3805-3810)*10 = -150
	mocker.update_dyn_profit(code, 3805.0);
	EXPECT_DOUBLE_EQ(-150.0, pInfo._dynprofit);
	EXPECT_DOUBLE_EQ(-150.0, mocker._fund_info._total_dynprofit);
	// 开仓费: 按量 1.0/手 * 3 = 3.0
	EXPECT_DOUBLE_EQ(3.0, mocker._fund_info._total_fees);

	// 平今: 同一交易日以 3820 目标减到 1 手
	mocker.append_signal(code, 1.0, "cut1", 0.0, 0);
	mocker.proc_tick(code, 3805.0, 3820.0);
	EXPECT_DOUBLE_EQ(1.0, mocker.stra_get_position(code));
	// 平仓盈亏: (3820-3810)*2*10 = 200, 平今费 3.0/手*2 = 6.0
	EXPECT_DOUBLE_EQ(200.0, pInfo._closeprofit);
	EXPECT_DOUBLE_EQ(200.0, mocker._total_closeprofit);
	EXPECT_DOUBLE_EQ(200.0, mocker._fund_info._total_profit);
	EXPECT_DOUBLE_EQ(9.0, mocker._fund_info._total_fees);
	EXPECT_EQ((uint64_t)20240102 * 10000ULL + 93000, pInfo._last_exittime);
	ASSERT_EQ(1u, pInfo._details.size());
	EXPECT_DOUBLE_EQ(1.0, pInfo._details[0]._volume);

	// 平昨: 推进到下一交易日后全平
	_rp._cur_date = 20240103;
	_rp._cur_tdate = 20240103;
	mocker.append_signal(code, 0.0, "flat", 0.0, 0);
	mocker.proc_tick(code, 3830.0, 3830.0);
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(code));
	// 平昨盈亏 (3830-3810)*1*10 = 200, 累计 400; 平昨费 2.0/手*1 = 2, 累计 11
	EXPECT_DOUBLE_EQ(400.0, mocker._total_closeprofit);
	EXPECT_DOUBLE_EQ(400.0, mocker._fund_info._total_profit);
	EXPECT_DOUBLE_EQ(11.0, mocker._fund_info._total_fees);
	EXPECT_TRUE(pInfo._details.empty());
	// 仓位清零时浮盈同步清零
	EXPECT_DOUBLE_EQ(0.0, pInfo._dynprofit);
	EXPECT_DOUBLE_EQ(0.0, mocker._fund_info._total_dynprofit);

	// 反手做空 2 手: 明细方向为空头
	mocker.append_signal(code, -2.0, "short2", 0.0, 0);
	mocker.proc_tick(code, 3790.0, 3790.0);
	EXPECT_DOUBLE_EQ(-2.0, mocker.stra_get_position(code));
	ASSERT_EQ(1u, pInfo._details.size());
	EXPECT_FALSE(pInfo._details[0]._long);
	EXPECT_DOUBLE_EQ(3790.0, pInfo._details[0]._price);
	// 空单浮盈按 CtaMocker.cpp:989 的式子算：vol*(price-open)*volScale*(±1)
	// = 2*(3790-3780)*10 = 200（原注释写成 20、断言也写 20，是漏乘了 2 手）。
	// 裁定见 reports/FAIL裁定记录-20261027.md（A-06）。产品值 200 与输入一致。
	mocker.update_dyn_profit(code, 3780.0);
	EXPECT_DOUBLE_EQ(200.0, pInfo._dynprofit);
	// 空单开仓费 1.0*2 = 2 -> 累计 13
	EXPECT_DOUBLE_EQ(13.0, mocker._fund_info._total_fees);
}

// 滑点推进: 2 个价差滑点在买入时抬价、卖出时压价
TEST_F(P03CtaTradePath, FixedSlippageAppliedToBothSides)
{
	const char* code = "SHFE.rb.2405";
	CtaMocker mocker(&_rp, "p03_slp", 2, false);   // 2 跳滑点, pxTick=1.0
	mocker._price_map[code] = 3800.0;

	mocker.stra_enter_long(code, 2, "slp");
	mocker.proc_tick(code, 3800.0, 3810.0);
	// 买入滑点: 3810 + 2*1 = 3812
	const CtaMocker::PosInfo& pInfo = mocker._pos_map[code];
	ASSERT_EQ(1u, pInfo._details.size());
	EXPECT_DOUBLE_EQ(3812.0, pInfo._details[0]._price);
	EXPECT_DOUBLE_EQ(3812.0, mocker.stra_get_position_avgpx(code));
	// 浮亏: 2*(3812->以3812记)后按现价 3812 更新为 0
	mocker.update_dyn_profit(code, 3812.0);
	EXPECT_DOUBLE_EQ(0.0, pInfo._dynprofit);
	// 现价 3800: 2*(3800-3812)*10 = -240, 明细记录最大浮亏
	mocker.update_dyn_profit(code, 3800.0);
	EXPECT_DOUBLE_EQ(-240.0, pInfo._dynprofit);
	EXPECT_DOUBLE_EQ(-240.0, pInfo._details[0]._max_loss);
	EXPECT_DOUBLE_EQ(3812.0, pInfo._details[0]._max_price);
	EXPECT_DOUBLE_EQ(3800.0, pInfo._details[0]._min_price);

	// 平仓滑点: 目标 0 手, 现价 3820 -> 卖出成交价 3818
	mocker.append_signal(code, 0.0, "slpclose", 0.0, 0);
	mocker.proc_tick(code, 3800.0, 3820.0);
	// 平仓盈亏: (3818-3812)*2*10 = 120
	EXPECT_DOUBLE_EQ(120.0, mocker._total_closeprofit);
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(code));
}

// 股票 T+1: 冻结语义与禁做空拒绝
TEST_F(P03CtaTradePath, StockT1FrozenAndShortRejected)
{
	const char* code = "SSE.600001";
	CtaMocker mocker(&_rp, "p03_stk", 0, false);
	mocker._price_map[code] = 10.0;

	mocker.stra_enter_long(code, 100, "buy1");
	mocker.proc_tick(code, 10.0, 10.05);
	EXPECT_DOUBLE_EQ(100.0, mocker.stra_get_position(code, false));
	// T+1: 当日买入全部冻结, 有效仓位 0
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(code, true));
	const CtaMocker::PosInfo& pInfo = mocker._pos_map[code];
	EXPECT_DOUBLE_EQ(100.0, pInfo._frozen);
	// valid() = 总量 - 冻结（CtaMocker.cpp:2024）。T+1 当日全冻结 ⇒ 100-100=0，
	// 与本用例上一行注释「有效仓位 0」一致；原来的 100.0 是笔误（A-06）。
	EXPECT_DOUBLE_EQ(0.0, pInfo.valid());

	// 股票禁做空: stra_enter_short 早退, 无信号
	mocker.stra_enter_short(code, 10, "sh");
	EXPECT_TRUE(mocker._sig_map.empty());

	// 目标仓位低于冻结量: 拒绝
	mocker.stra_set_position(code, 50, "lt");
	EXPECT_TRUE(mocker._sig_map.empty());
	// 目标与当前一致: 早退
	mocker.stra_set_position(code, 100, "same");
	EXPECT_TRUE(mocker._sig_map.empty());

	// 次日解冻由外部推进, 这里手工减冻验证加仓冻结累计
	const_cast<CtaMocker::PosInfo&>(pInfo)._frozen = 0;
	mocker.stra_set_position(code, 200, "up2");
	ASSERT_EQ(1u, mocker._sig_map.count(code));
	mocker.proc_tick(code, 10.05, 10.10);
	EXPECT_DOUBLE_EQ(200.0, mocker.stra_get_position(code, false));
	EXPECT_DOUBLE_EQ(100.0, pInfo._frozen);   // 新增 100 冻结
	EXPECT_DOUBLE_EQ(100.0, mocker.stra_get_position(code, true));
	// 两笔明细 FIFO: 100@10.05 + 100@10.10, 均价 10.075
	EXPECT_DOUBLE_EQ(10.075, mocker.stra_get_position_avgpx(code));
	// 开仓费 0.5/手: 累计 50+50 = 100
	EXPECT_DOUBLE_EQ(100.0, mocker._fund_info._total_fees);
}

// 条件单(限价/止损)登记与触发: 价格未越过目标时不成交, 越过时按信号推进
TEST_F(P03CtaTradePath, ConditionalEntrustTriggeredByTickRange)
{
	const char* code = "SHFE.rb.2405";
	CtaMocker mocker(&_rp, "p03_cond", 0, false);
	mocker._price_map[code] = 3800.0;

	// 限价买入: 价格 <= 3790 才触发
	mocker.stra_enter_long(code, 2, "limit", 3790.0, 0.0);
	EXPECT_TRUE(mocker._sig_map.empty());
	ASSERT_EQ(1u, mocker._condtions.count(code));
	EXPECT_EQ(1u, mocker._condtions[code].size());

	// 3800 -> 3805 不触发
	mocker.proc_tick(code, 3800.0, 3805.0);
	EXPECT_EQ(1u, mocker._condtions[code].size());
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(code));

	// 3805 -> 3785 越过限价, 触发并以目标价成交
	mocker.proc_tick(code, 3805.0, 3785.0);
	EXPECT_TRUE(mocker._sig_map.count(code) > 0);
	mocker.proc_tick(code, 3785.0, 3785.0);
	EXPECT_DOUBLE_EQ(2.0, mocker.stra_get_position(code));
}

// 疑似缺陷类: calc_fee 按额计费不判空 commInfo(禁用, 见 BUG-P03-001)
// update_dyn_profit 对"有持仓但合约信息缺失"同样直接解引用(CtaMocker.cpp L985),
// 该形态在正常链路不可达(持仓必然先注册合约), 只在 calc_fee 路径复现整类空解引用
TEST_F(P03CtaTradePath, DISABLED_CalcFeeByAmountCrashesWhenCommodityMissing)
{
	// 只为 DCE.m 登记费用模板(按额计费), 不注册合约信息
	set_fee(_rp, "DCE.m", 0.00003, 0.00003, 0.00003);
	HisDataReplayer::FeeItem& f = _rp._fee_map["DCE.m"];
	f._by_volume = false;
	// HisDataReplayer.cpp L3007: price*qty*commInfo->getVolScale(), commInfo 为 NULL -> 段错误
	(void)_rp.calc_fee("DCE.m.2405", 3000.0, 2.0, 0);
	FAIL();
}
