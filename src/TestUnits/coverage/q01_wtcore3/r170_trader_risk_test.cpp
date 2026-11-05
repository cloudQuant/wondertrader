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
#include <mutex>
#include <string>
#include <vector>

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
#include <WtCore/ITrdNotifySink.h>
#include "../p11_wtcore_traderadapter/p11_trader_fixture.h"

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

// 迭代03 s5（卡8c=A，BUG-R202-003）：风控开启时解析不到合约的代码一律拒绝，
// 窗口用例因此登记所用合约；未知代码另行断言被拒。
struct R170KnownContracts
{
    p11fix::FakeBaseDataMgr bd;
    WTSCommodityInfo* ag = WTSCommodityInfo::create("ag", "silver", "SHFE", "TRADING", "CHINA");
    WTSCommodityInfo* au = WTSCommodityInfo::create("au", "gold", "SHFE", "TRADING", "CHINA");
    WTSContractInfo* ag2512 = WTSContractInfo::create("ag2512", "ag2512", "SHFE", "ag");
    WTSContractInfo* au2512 = WTSContractInfo::create("au2512", "au2512", "SHFE", "au");
    R170KnownContracts() { bd.regis(ag, ag2512); bd.regis(au, au2512); }
    ~R170KnownContracts() { ag2512->release(); au2512->release(); ag->release(); au->release(); }
};

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
    R170KnownContracts known;
    TraderAdapter trader;
    enableRisk(trader, 2, 2);
    trader._bd_mgr = &known.bd;
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
    // 卡8c=A：未知合约在风控开启时被拒（查询与下单/撤单门禁一致）。
    EXPECT_FALSE(trader.isTradeEnabled("SHFE.xx.9999"));
    EXPECT_FALSE(trader.checkOrderLimits("SHFE.xx.9999"));
    EXPECT_FALSE(trader.checkCancelLimits("SHFE.xx.9999"));
}

TEST(R170TraderRisk, CancelWindowRejectsAtBoundaryAndRecovers)
{
    R170KnownContracts known;
    TraderAdapter trader;
    enableRisk(trader, 2, 2);
    trader._bd_mgr = &known.bd;
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

    // 迭代03 s5 更新（理由见 evidence/20261104-s5-wp1-ledger/README.md）：
    // - 平今空可平量 0→2 的原意保留，直接断言 s_newavail；getPosition 自 2021
    //   (8cd46907) 起按净头寸约定把空头记为负值，故 flag=2 的有效持仓为 -2。
    // - 未完成量按 R3-008 逐单账本（卡5/卡7 已裁定并验收）：该订单首次存活回报
    //   预占 3，撤单终态恰好释放一次，结果为 0，不再沿用“撤单时 +2”的旧聚合启发式。
    EXPECT_DOUBLE_EQ(2.0, trader._positions["SHFE.ag.2512"].s_newavail);
    EXPECT_DOUBLE_EQ(-2.0, trader.getPosition("SHFE.ag.2512", true, 2));
    EXPECT_DOUBLE_EQ(0.0, trader.getUndoneQty("SHFE.ag.2512"));
    EXPECT_EQ(TraderAdapter::OrderLedgerState::Terminal, trader.orderLedger(77).state);

    ct->release();
    comm->release();
}

//////////////////////////////////////////////////////////////////
// 期权错误回报：stdCode 必须走 rawFutOptCodeToStdCode 分支；
// 迭代03 s5 更新：原用例的委托不带本地 tag、直接预置 _undone_qty=5 并期望错误
// 回报扣到 4。T2-CTP-08（卡6d）起无本地 tag 的拒单不消耗本适配器未完成量，
// R3-008 逐单账本（卡5）起未完成量只由逐单账本释放。现改为本适配器真实发出的
// 期权订单（逐单账本预占 1，另有 4 手其他未完成），拒单按期权标准码通知并
// 只释放该单的 1 手：5 -> 4。
// src/WtCore/TraderAdapter.cpp onRspEntrust 期权分支
//////////////////////////////////////////////////////////////////
namespace {
class R170EntrustSink : public ITrdNotifySink
{
public:
    std::vector<std::string> codes;
    std::vector<uint32_t> ids;
    void on_trade(uint32_t, const char*, bool, double, double) override {}
    void on_order(uint32_t, const char*, bool, double, double, double, bool) override {}
    void on_channel_ready() override {}
    void on_channel_lost() override {}
    void on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char*) override
    {
        if (!bSuccess)
        {
            ids.push_back(localid);
            codes.push_back(stdCode ? stdCode : "");
        }
    }
};
}  // namespace

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
    ent->setUserTag("otp.r170opt.88");

    WTSError* err = WTSError::create(WEC_ORDERINSERT, "r170 rejected");
    ASSERT_NE(nullptr, err);

    const std::string optCode = CodeHelper::rawFutOptCodeToStdCode("ag2512C3200", "SHFE");
    ASSERT_FALSE(optCode.empty());

    TraderAdapter trader;
    trader._id = "r170opt";
    trader._order_pattern = "otp.r170opt";
    R170EntrustSink sink;
    trader.addSink(&sink);
    {
        std::lock_guard<std::mutex> lock(trader._ledger_mutex);
        trader._undone_qty[optCode] = 4.0;	// other pending orders of the same option
        TraderAdapter::OrderLedgerEntry entry;
        entry.std_code = optCode;
        entry.intent_id = "r170:opt:88";
        entry.sign = 1.0;
        entry.verified = true;
        entry.state = TraderAdapter::OrderLedgerState::Live;
        TraderAdapter::OrderLedgerEntry& stored = trader._ledger[88] = entry;
        trader.ledgerSetAccountedLocked(stored, 1.0);
    }
    ASSERT_DOUBLE_EQ(5.0, trader.getUndoneQty(optCode.c_str()));

    trader.onRspEntrust(ent, err);

    EXPECT_DOUBLE_EQ(4.0, trader.getUndoneQty(optCode.c_str()));
    ASSERT_EQ(1u, sink.codes.size());
    EXPECT_EQ(optCode, sink.codes[0]) << "rejection is notified with the option standard code";
    EXPECT_EQ(88u, sink.ids[0]);

    trader.onRspEntrust(ent, err);	// duplicate rejection releases nothing more
    EXPECT_DOUBLE_EQ(4.0, trader.getUndoneQty(optCode.c_str()));
    EXPECT_EQ(1u, sink.codes.size());

    // Untagged (external) rejection never consumes this adapter's quantity.
    WTSEntrust* external = WTSEntrust::create("ag2512C3200", 1, 3500.0, "SHFE");
    external->setContractInfo(ct);
    external->setDirection(WDT_LONG);
    external->setOffsetType(WOT_OPEN);
    trader.onRspEntrust(external, err);
    EXPECT_DOUBLE_EQ(4.0, trader.getUndoneQty(optCode.c_str()));
    external->release();

    trader.removeSink(&sink);
    err->release();
    ent->release();
    ct->release();
    comm->release();
}
