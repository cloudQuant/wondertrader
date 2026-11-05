/**
 * r170_trader_risk_test.cpp — lane R170 / wave25
 *
 * 目标：src/WtCore/TraderAdapter.cpp 的剩余公开面。
 * 复用 P11 的进程内替身思路，但风控窗口直接装配 private 缓存，避免 buy/cancel
 * 链路（R3/R90 的同类用例在 2026-10-28 全量里 SIGBUS/SIGSEGV，未产出计数）。
 *
 * 覆盖行段：
 *   - checkOrderLimits 频率窗口：:631-658（越界封禁 :643-649、历史裁剪 :653-656）
 *   - checkCancelLimits 频率窗口：:566-593（越界封禁 :578-584、历史裁剪 :588-590）
 *   - onPushOrder 第二次推送为「撤平空今仓」：:2067-2072
 *   - onRspEntrust 错误回报的期权代码分支：:1455-1459、:1487-1492
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include <boost/core/noncopyable.hpp>

#include <Includes/WTSCollection.hpp>
#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSTradeDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <Share/CodeHelper.hpp>
#include <Share/StdUtils.hpp>

#define private public
#include <WtCore/TraderAdapter.h>
#undef private

using namespace wtp;

namespace {

using RiskParams = TraderAdapter::RiskParams;

void enableRisk(TraderAdapter& trader, uint32_t orderBoundary, uint32_t cancelBoundary)
{
    trader._risk_mon_enabled = true;
    trader._stat_map = TraderAdapter::TradeStatMap::create();

    RiskParams p;
    p._order_times_boundary = orderBoundary;
    p._order_stat_timespan = 60;
    p._order_total_limits = 100;
    p._cancel_times_boundary = cancelBoundary;
    p._cancel_stat_timespan = 60;
    p._cancel_total_limits = 99;
    trader._risk_params_map["default"] = p;
}

WTSOrderInfo* makeOrder(WTSContractInfo* ct, WTSOrderState state, uint32_t traded)
{
    WTSOrderInfo* ord = WTSOrderInfo::create();
    ord->setContractInfo(ct);
    ord->setDirection(WDT_SHORT);
    ord->setOffsetType(WOT_CLOSETODAY);
    ord->setOrderFlag(WOF_NOR);
    ord->setOrderState(state);
    ord->setVolume(3);
    ord->setVolTraded(traded);
    ord->setVolLeft(3 - traded);
    ord->setPrice(3510.0);
    ord->setOrderID("O170");
    ord->setEntrustID("E170");
    ord->setUserTag("otp.r170x.77");
    ord->setStateMsg("r170");
    return ord;
}

}  // namespace

//////////////////////////////////////////////////////////////////
// 下单频率窗口：4 条记录 -> times=3 > boundary=2 封禁；
// 3 条记录且 lower_bound 落在中间 -> times=1 放行并裁掉前缀
// src/WtCore/TraderAdapter.cpp:633-658
//////////////////////////////////////////////////////////////////
TEST(R170TraderRisk, OrderWindowBlocksAndPrunes)
{
    TraderAdapter trader;
    enableRisk(trader, 2, 2);

    trader._order_time_cache["SHFE.ag.2512"] = { 10000, 30000, 50000, 70000 };
    EXPECT_FALSE(trader.checkOrderLimits("SHFE.ag.2512"));
    EXPECT_TRUE(trader._exclude_codes.find("SHFE.ag.2512") != trader._exclude_codes.end());
    // 排除队列短路：再次检查直接拒绝，不再进入窗口计算
    EXPECT_FALSE(trader.checkOrderLimits("SHFE.ag.2512"));

    // 另一个合约：窗口内 3 条，times=1 不越界；lower_bound 指向第 2 条，前缀被裁掉
    trader._order_time_cache["SHFE.au.2512"] = { 100000, 200000, 200100 };
    EXPECT_TRUE(trader.checkOrderLimits("SHFE.au.2512"));
    EXPECT_EQ(2u, trader._order_time_cache["SHFE.au.2512"].size());
    EXPECT_EQ(200000u, trader._order_time_cache["SHFE.au.2512"].front());
}

//////////////////////////////////////////////////////////////////
// 撤单频率窗口：越界封禁 + 历史裁剪
// src/WtCore/TraderAdapter.cpp:566-593
//////////////////////////////////////////////////////////////////
TEST(R170TraderRisk, CancelWindowBlocksAndPrunes)
{
    TraderAdapter trader;
    enableRisk(trader, 2, 2);

    trader._cancel_time_cache["SHFE.ag.2512"] = { 10000, 30000, 50000, 70000 };
    EXPECT_FALSE(trader.checkCancelLimits("SHFE.ag.2512"));
    EXPECT_FALSE(trader.checkCancelLimits("SHFE.ag.2512"));

    trader._cancel_time_cache["SHFE.au.2512"] = { 100000, 200000, 200100 };
    EXPECT_TRUE(trader.checkCancelLimits("SHFE.au.2512"));
    EXPECT_EQ(2u, trader._cancel_time_cache["SHFE.au.2512"].size());
    EXPECT_EQ(200000u, trader._cancel_time_cache["SHFE.au.2512"].front());
}

//////////////////////////////////////////////////////////////////
// 同一订单第二次推送为「已撤平空今仓」：走 :2042 的非首次分支，
// s_newavail 回补 qty=volume-volTraded=2，未完成数量同步 -(-2)
// src/WtCore/TraderAdapter.cpp:2067-2072、:2105-2107
//////////////////////////////////////////////////////////////////
TEST(R170TraderRisk, SecondCanceledShortTodayPushRestoresAvail)
{
    WTSCommodityInfo* comm = WTSCommodityInfo::create("ag", "silver", "SHFE", "TRADING", "CHINA");
    ASSERT_NE(nullptr, comm);
    comm->setCategory(CC_Future);
    WTSContractInfo* ct = WTSContractInfo::create("ag2512", "ag2512", "SHFE", "ag");
    ASSERT_NE(nullptr, ct);
    ct->setCommInfo(comm);

    TraderAdapter trader;
    trader._id = "r170x";
    trader._order_pattern = "otp.r170x";
    trader._stat_map = TraderAdapter::TradeStatMap::create();

    WTSOrderInfo* first = makeOrder(ct, WOS_NotTraded_Queuing, 0);
    trader.onPushOrder(first);
    first->release();

    WTSOrderInfo* second = makeOrder(ct, WOS_Canceled, 1);
    trader.onPushOrder(second);
    second->release();

    EXPECT_DOUBLE_EQ(2.0, trader.getPosition("SHFE.ag.2512", true, 2));
    EXPECT_DOUBLE_EQ(2.0, trader.getUndoneQty("SHFE.ag.2512"));

    ct->release();
    comm->release();
}

//////////////////////////////////////////////////////////////////
// 期权错误回报：stdCode 必须走 rawFutOptCodeToStdCode 分支；
// 预置未完成量 5 后，开多错误按 -1 回补到 4
// src/WtCore/TraderAdapter.cpp:1455-1459、:1487-1492
//////////////////////////////////////////////////////////////////
TEST(R170TraderRisk, OptionEntrustErrorUsesOptionStdCode)
{
    WTSCommodityInfo* comm = WTSCommodityInfo::create("ag", "silver", "SHFE", "TRADING", "CHINA");
    ASSERT_NE(nullptr, comm);
    comm->setCategory(CC_FutOption);
    WTSContractInfo* ct = WTSContractInfo::create("ag2512C3200", "ag2512C3200", "SHFE", "ag");
    ASSERT_NE(nullptr, ct);
    ct->setCommInfo(comm);

    WTSEntrust* ent = WTSEntrust::create("ag2512C3200", 1, 3500.0, "SHFE");
    ASSERT_NE(nullptr, ent);
    ent->setContractInfo(ct);
    ent->setDirection(WDT_LONG);
    ent->setOffsetType(WOT_OPEN);
    ent->setVolume(1);

    WTSError* err = WTSError::create(WEC_ORDERINSERT, "r170 rejected");
    ASSERT_NE(nullptr, err);

    const std::string optCode = CodeHelper::rawFutOptCodeToStdCode("ag2512C3200", "SHFE");
    ASSERT_FALSE(optCode.empty());

    TraderAdapter trader;
    trader._id = "r170opt";
    trader._order_pattern = "otp.r170opt";
    trader._undone_qty[optCode] = 5.0;

    trader.onRspEntrust(ent, err);

    EXPECT_DOUBLE_EQ(4.0, trader.getUndoneQty(optCode.c_str()));

    err->release();
    ent->release();
    ct->release();
    comm->release();
}
