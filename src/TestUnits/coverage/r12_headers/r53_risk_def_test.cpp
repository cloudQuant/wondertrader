// Lane R53 - src/Includes/WTSRiskDef.hpp（此前无任何直测用例）
// 覆盖：TradeStatInfo 零初始化、WTSTradeStateInfo 全量访问器与统计聚合、
//       WTSFundStruct 默认哨兵、WTSPortFundInfo 访问器与 fundInfo 原地修改。
// 说明：全部为纯内存对象，不读挂钟、不起线程、不写文件。
#include <gtest/gtest.h>

#include <cfloat>
#include <cstdint>
#include <cstring>
#include <string>

#include "Includes/WTSRiskDef.hpp"
#include "Includes/WTSMarcos.h"

using namespace wtp;

namespace
{
constexpr uint32_t kRiskCodeMaxLen = MAX_INSTRUMENT_LENGTH;
}  // namespace

// WTSRiskDef.hpp:63-69 create + :74 code
TEST(r53_risk_trade_stat, create_zeroes_counters_and_copies_code)
{
	WTSTradeStateInfo* stat = WTSTradeStateInfo::create("SHFE.rb2405");
	ASSERT_NE(stat, nullptr);
	EXPECT_STREQ(stat->code(), "SHFE.rb2405");

	// TradeStatInfo 构造函数 memset 全零（WTSRiskDef.hpp:51-54）
	EXPECT_DOUBLE_EQ(stat->open_volume_long(), 0.0);
	EXPECT_DOUBLE_EQ(stat->close_volume_long(), 0.0);
	EXPECT_DOUBLE_EQ(stat->closet_volume_long(), 0.0);
	EXPECT_DOUBLE_EQ(stat->open_volume_short(), 0.0);
	EXPECT_DOUBLE_EQ(stat->close_volume_short(), 0.0);
	EXPECT_DOUBLE_EQ(stat->closet_volume_short(), 0.0);
	EXPECT_DOUBLE_EQ(stat->total_trade_volumes(), 0.0);

	EXPECT_EQ(stat->orders_buy(), 0u);
	EXPECT_DOUBLE_EQ(stat->ordqty_buy(), 0.0);
	EXPECT_EQ(stat->orders_sell(), 0u);
	EXPECT_DOUBLE_EQ(stat->ordqty_sell(), 0.0);

	EXPECT_EQ(stat->cancels_buy(), 0u);
	EXPECT_DOUBLE_EQ(stat->cancelqty_buy(), 0.0);
	EXPECT_EQ(stat->cancels_sell(), 0u);
	EXPECT_DOUBLE_EQ(stat->cancelqty_sell(), 0.0);
	EXPECT_EQ(stat->total_cancels(), 0u);
	EXPECT_EQ(stat->total_orders(), 0u);
	EXPECT_EQ(stat->infos(), 0u);

	// const 重载（WTSRiskDef.hpp:72）返回同一块统计
	const WTSTradeStateInfo* cstat = stat;
	EXPECT_EQ(&cstat->statInfo(), &stat->statInfo());
	stat->release();
}

// WTSRiskDef.hpp:71-100 通过 statInfo() 原地写入后所有访问器同步
TEST(r53_risk_trade_stat, stat_info_mutation_drives_all_accessors)
{
	WTSTradeStateInfo* stat = WTSTradeStateInfo::create("DCE.i2409");
	ASSERT_NE(stat, nullptr);
	TradeStatInfo& info = stat->statInfo();
	info.l_openvol = 1.0;
	info.l_closevol = 2.0;
	info.l_closetvol = 3.0;
	info.s_openvol = 4.0;
	info.s_closevol = 5.0;
	info.s_closetvol = 6.0;
	info.b_orders = 7;
	info.b_ordqty = 7.5;
	info.s_orders = 8;
	info.s_ordqty = 8.5;
	info.b_cancels = 9;
	info.b_canclqty = 9.5;
	info.s_cancels = 10;
	info.s_canclqty = 10.5;
	info._infos = 11;

	EXPECT_DOUBLE_EQ(stat->open_volume_long(), 1.0);
	EXPECT_DOUBLE_EQ(stat->close_volume_long(), 2.0);
	EXPECT_DOUBLE_EQ(stat->closet_volume_long(), 3.0);
	EXPECT_DOUBLE_EQ(stat->open_volume_short(), 4.0);
	EXPECT_DOUBLE_EQ(stat->close_volume_short(), 5.0);
	EXPECT_DOUBLE_EQ(stat->closet_volume_short(), 6.0);
	EXPECT_DOUBLE_EQ(stat->total_trade_volumes(), 21.0);

	EXPECT_EQ(stat->orders_buy(), 7u);
	EXPECT_DOUBLE_EQ(stat->ordqty_buy(), 7.5);
	EXPECT_EQ(stat->orders_sell(), 8u);
	EXPECT_DOUBLE_EQ(stat->ordqty_sell(), 8.5);
	EXPECT_EQ(stat->total_orders(), 15u);

	EXPECT_EQ(stat->cancels_buy(), 9u);
	EXPECT_DOUBLE_EQ(stat->cancelqty_buy(), 9.5);
	EXPECT_EQ(stat->cancels_sell(), 10u);
	EXPECT_DOUBLE_EQ(stat->cancelqty_sell(), 10.5);
	EXPECT_EQ(stat->total_cancels(), 19u);
	EXPECT_EQ(stat->infos(), 11u);
	stat->release();
}

// WTSRiskDef.hpp:63-69：代码复制边界（31 字符）与引用计数
TEST(r53_risk_trade_stat, code_copy_at_max_length_and_refcount)
{
	const std::string code(kRiskCodeMaxLen - 1, 'R');
	WTSTradeStateInfo* stat = WTSTradeStateInfo::create(code.c_str());
	ASSERT_NE(stat, nullptr);
	EXPECT_EQ(std::strlen(stat->code()), kRiskCodeMaxLen - 1);
	EXPECT_EQ(std::strncmp(stat->code(), code.c_str(), code.size()), 0);
	EXPECT_EQ(stat->retainCount(), 1u);
	EXPECT_EQ(stat->retain(), 2u);
	EXPECT_EQ(stat->retainCount(), 2u);
	stat->release();
	EXPECT_EQ(stat->retainCount(), 1u);
	stat->release();
}

// WTSRiskDef.hpp:107-144：WTSFundStruct 默认值 + WTSPortFundInfo::create
TEST(r53_risk_port_fund, create_defaults_and_sentinels)
{
	WTSPortFundInfo* fund = WTSPortFundInfo::create();
	ASSERT_NE(fund, nullptr);
	EXPECT_DOUBLE_EQ(fund->predynbalance(), 0.0);
	EXPECT_DOUBLE_EQ(fund->balance(), 0.0);
	EXPECT_DOUBLE_EQ(fund->profit(), 0.0);
	EXPECT_DOUBLE_EQ(fund->dynprofit(), 0.0);
	EXPECT_DOUBLE_EQ(fund->fees(), 0.0);
	// 日内极值用 DBL_MAX 作“尚未更新”哨兵（WTSRiskDef.hpp:141-142）
	EXPECT_DOUBLE_EQ(fund->max_dyn_balance(), DBL_MAX);
	EXPECT_DOUBLE_EQ(fund->min_dyn_balance(), DBL_MAX);
	EXPECT_DOUBLE_EQ(fund->max_md_dyn_balance(), 0.0);
	EXPECT_DOUBLE_EQ(fund->min_md_dyn_balance(), 0.0);
	EXPECT_EQ(fund->max_dynbal_time(), 0u);
	EXPECT_EQ(fund->min_dynbal_time(), 0u);
	EXPECT_EQ(fund->last_settle_date(), 0u);
	EXPECT_EQ(fund->max_md_dynbal_date(), 0u);
	EXPECT_EQ(fund->min_md_dynbal_date(), 0u);
	fund->release();
}

// WTSRiskDef.hpp:159-180：fundInfo() 引用与 const 重载
TEST(r53_risk_port_fund, fund_info_mutation_visible_through_accessors)
{
	WTSPortFundInfo* fund = WTSPortFundInfo::create();
	ASSERT_NE(fund, nullptr);
	WTSFundStruct& fs = fund->fundInfo();
	fs._predynbal = 1000.0;
	fs._prebalance = 1100.0;
	fs._balance = 1200.0;
	fs._profit = 50.0;
	fs._dynprofit = -30.0;
	fs._fees = 5.0;
	fs._last_date = 20240506;
	fs._max_dyn_bal = 1300.0;
	fs._max_time = 93000;
	fs._min_dyn_bal = 900.0;
	fs._min_time = 145900;
	fs._update_time = 1715000000;
	fs._max_md_dyn_bal._date = 20240506;
	fs._max_md_dyn_bal._dyn_balance = 1250.0;
	fs._min_md_dyn_bal._date = 20240507;
	fs._min_md_dyn_bal._dyn_balance = 950.0;

	EXPECT_DOUBLE_EQ(fund->predynbalance(), 1000.0);
	EXPECT_DOUBLE_EQ(fund->balance(), 1200.0);
	EXPECT_DOUBLE_EQ(fund->profit(), 50.0);
	EXPECT_DOUBLE_EQ(fund->dynprofit(), -30.0);
	EXPECT_DOUBLE_EQ(fund->fees(), 5.0);
	EXPECT_DOUBLE_EQ(fund->max_dyn_balance(), 1300.0);
	EXPECT_DOUBLE_EQ(fund->min_dyn_balance(), 900.0);
	EXPECT_EQ(fund->max_dynbal_time(), 93000u);
	EXPECT_EQ(fund->min_dynbal_time(), 145900u);
	EXPECT_EQ(fund->last_settle_date(), 20240506u);
	EXPECT_DOUBLE_EQ(fund->max_md_dyn_balance(), 1250.0);
	EXPECT_DOUBLE_EQ(fund->min_md_dyn_balance(), 950.0);
	EXPECT_EQ(fund->max_md_dynbal_date(), 20240506u);
	EXPECT_EQ(fund->min_md_dynbal_date(), 20240507u);
	EXPECT_EQ(fs._update_time, 1715000000);
	EXPECT_EQ(&fund->fundInfo(), &fs);
	const WTSPortFundInfo* cfund = fund;
	EXPECT_EQ(&cfund->fundInfo(), &fs);
	fund->release();
}

// WTSRiskDef.hpp:124-143：DynBalPair / WTSFundStruct 构造函数
TEST(r53_risk_fund_struct, dyn_bal_pair_and_struct_defaults)
{
	WTSFundStruct::DynBalPair pair;
	EXPECT_EQ(pair._date, 0u);
	EXPECT_DOUBLE_EQ(pair._dyn_balance, 0.0);
	pair._date = 20240506;
	pair._dyn_balance = 1.5;
	EXPECT_EQ(pair._date, 20240506u);
	EXPECT_DOUBLE_EQ(pair._dyn_balance, 1.5);

	WTSFundStruct fs;
	EXPECT_DOUBLE_EQ(fs._predynbal, 0.0);
	EXPECT_DOUBLE_EQ(fs._prebalance, 0.0);
	EXPECT_DOUBLE_EQ(fs._balance, 0.0);
	EXPECT_DOUBLE_EQ(fs._profit, 0.0);
	EXPECT_DOUBLE_EQ(fs._dynprofit, 0.0);
	EXPECT_DOUBLE_EQ(fs._fees, 0.0);
	EXPECT_EQ(fs._last_date, 0u);
	EXPECT_EQ(fs._max_time, 0u);
	EXPECT_EQ(fs._min_time, 0u);
	EXPECT_EQ(fs._update_time, 0);
	EXPECT_DOUBLE_EQ(fs._max_dyn_bal, DBL_MAX);
	EXPECT_DOUBLE_EQ(fs._min_dyn_bal, DBL_MAX);
	EXPECT_EQ(fs._max_md_dyn_bal._date, 0u);
	EXPECT_EQ(fs._min_md_dyn_bal._date, 0u);
	EXPECT_DOUBLE_EQ(fs._max_md_dyn_bal._dyn_balance, 0.0);
	EXPECT_DOUBLE_EQ(fs._min_md_dyn_bal._dyn_balance, 0.0);
}
