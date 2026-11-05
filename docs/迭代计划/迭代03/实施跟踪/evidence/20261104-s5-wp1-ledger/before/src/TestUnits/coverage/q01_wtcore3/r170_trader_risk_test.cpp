/**
 * r170_trader_risk_test.cpp — lane R170 / wave25
 *
 * 目标：src/WtCore/TraderAdapter.cpp 的剩余公开面。
 * 复用 P11 的进程内替身思路，但风控窗口直接装配 private 缓存，避免 buy/cancel
 * 链路（R3/R90 的同类用例在 2026-10-28 全量里 SIGBUS/SIGSEGV，未产出计数）。
 *
 * 覆盖行段：
 *   - checkOrderLimits / checkCancelLimits 频率窗口（迭代03 卡 7：单调时钟滑窗、临时封禁、过期恢复）
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
// 迭代03 卡 7（用户已裁定）更新：原两条用例直接写入 _order_time_cache /
// _cancel_time_cache 并断言「以最后一次操作为窗口终点」「越界写入
// _exclude_codes 永久排除」，正是 7a/7b 否决的旧语义。现改为：
//   - 窗口以注入的单调时钟 now 为终点，已记录请求数 >= N 时拒绝（第 N+1 次）；
//   - 超限只是临时封禁，不进入 _exclude_codes，记录过期后自动恢复并裁剪。
// 记录通过 FrequencyWindow::Reservation 提交，时间来自注入时钟。
//////////////////////////////////////////////////////////////////
namespace {
uint64_t g_r170_now = 1000000;
uint64_t r170Now() { return g_r170_now; }

void r170Record(TraderAdapter& trader, bool isOrder, const char* code, uint64_t at)
{
    auto& window = isOrder ? trader._order_frequency : trader._cancel_frequency;
    decltype(trader._order_frequency)::Reservation r(window, code, 1000, 60, at);
    ASSERT_TRUE(static_cast<bool>(r));
    r.beginSubmission(at);
    r.commit();
}
}  // namespace

TEST(R170TraderRisk, OrderWindowRejectsAtBoundaryAndRecovers)
{
    TraderAdapter trader;
    enableRisk(trader, 2, 2);
    trader._rate_clock = &r170Now;
    const uint64_t t0 = 1000000;

    r170Record(trader, true, "SHFE.ag.2512", t0);
    r170Record(trader, true, "SHFE.ag.2512", t0 + 20000);
    g_r170_now = t0 + 30000;
    EXPECT_FALSE(trader.checkOrderLimits("SHFE.ag.2512"));      // 2 条在窗内 -> 第 3 次拒绝
    EXPECT_FALSE(trader.isTradeEnabled("SHFE.ag.2512"));        // 临时封禁
    EXPECT_TRUE(trader._exclude_codes.find("SHFE.ag.2512") == trader._exclude_codes.end());
    EXPECT_TRUE(trader.checkOrderLimits("SHFE.au.2512"));       // 其他合约不受影响

    g_r170_now = t0 + 60000;                                    // 第一条恰好过期
    EXPECT_TRUE(trader.checkOrderLimits("SHFE.ag.2512"));
    EXPECT_TRUE(trader.isTradeEnabled("SHFE.ag.2512"));
    EXPECT_EQ(1u, trader._order_time_cache["SHFE.ag.2512"].size());
    EXPECT_EQ(t0 + 20000, trader._order_time_cache["SHFE.ag.2512"].front());
}

TEST(R170TraderRisk, CancelWindowRejectsAtBoundaryAndRecovers)
{
    TraderAdapter trader;
    enableRisk(trader, 2, 2);
    trader._rate_clock = &r170Now;
    const uint64_t t0 = 2000000;

    r170Record(trader, false, "SHFE.ag.2512", t0);
    r170Record(trader, false, "SHFE.ag.2512", t0 + 10000);
    g_r170_now = t0 + 59999;
    EXPECT_FALSE(trader.checkCancelLimits("SHFE.ag.2512"));
    EXPECT_TRUE(trader._exclude_codes.find("SHFE.ag.2512") == trader._exclude_codes.end());
    EXPECT_TRUE(trader.checkOrderLimits("SHFE.ag.2512"));       // 撤单窗口不影响下单窗口

    g_r170_now = t0 + 60000;
    EXPECT_TRUE(trader.checkCancelLimits("SHFE.ag.2512"));
    EXPECT_EQ(1u, trader._cancel_time_cache["SHFE.ag.2512"].size());
    EXPECT_EQ(t0 + 10000, trader._cancel_time_cache["SHFE.ag.2512"].front());
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
