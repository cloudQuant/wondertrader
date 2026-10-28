/**
 * r90_trader_report_test.cpp — lane R90 / wave17
 *
 * 目标：src/WtCore/TraderAdapter.cpp 的公开回报分发、统计分支与通知守卫。
 * 复用 p11/r59 已有的进程内替身（只读），不接柜台、不起线程。
 *
 * 覆盖行段：
 *   - onRspOrders 买/卖 × 错单/普通撤单/自动撤单统计：TraderAdapter.cpp:1645-1693
 *   - onRspTrades 空头平今统计：:1786-1795
 *   - onPushOrder 空合约早退、stateToName 的 UnSmt/Error、空头平仓可平量、二次撤单回补：
 *     :1875-1880, :1860-1870, :2042-2085
 *   - onPushTrade 空头平今与净头寸四个方向：:2225-2235, :2246-2306
 *   - 非活跃 EventNotifier 的 5 个通知守卫 + closeLong/closeShort 市价单：
 *     :1434, :1504, :2145, :2332, :2343, :1372, :1390
 */
#include "../p11_wtcore_traderadapter/r59_trader_fixture.h"

#include <WtCore/EventNotifier.h>

namespace {

using namespace r59x;

WTSOrderInfo* extOrder(WTSContractInfo* ct, WTSDirectionType dir, WTSOffsetType off,
    WTSOrderState st, double vol, double traded, const char* oid, WTSOrderFlag flag,
    bool isError)
{
    WTSOrderInfo* o = makeOrder(ct, dir, off, st, vol, traded, oid,
        (std::string("EXT.") + oid).c_str(), flag);
    if (isError)
        o->setError(WOEF_Normal);
    return o;
}

class R90TraderReport : public R59TraderTest
{
protected:
    wtp::TraderAdapter trader_;

    void buildPlain(wtp::TraderAdapter& trader)
    {
        build(trader, NULL);
    }
};

//////////////////////////////////////////////////////////////////
// onRspOrders 统计全分支 + stateToName 剩余两支 + 空合约早退
// src/WtCore/TraderAdapter.cpp:1645-1693, 1860-1880, 1786-1795
//////////////////////////////////////////////////////////////////
TEST_F(R90TraderReport, OrderReportStatsAndStateNames)
{
    buildPlain(trader_);

    // 买 × 错单 / 普通撤单 / 自动撤单；卖 × 错单 / 普通撤单 / 自动撤单
    trader_.onRspOrders(makeArray({
        extOrder(ct_, WDT_LONG, WOT_OPEN, WOS_Canceled, 5, 2, "BE1", WOF_NOR, true),
        extOrder(ct_, WDT_LONG, WOT_OPEN, WOS_Canceled, 4, 1, "BC1", WOF_NOR, false),
        extOrder(ct_, WDT_LONG, WOT_OPEN, WOS_Canceled, 3, 1, "BA1", WOF_FAK, false),
        extOrder(ct_, WDT_SHORT, WOT_CLOSE, WOS_Canceled, 6, 2, "SE1", WOF_NOR, true),
        extOrder(ct_, WDT_SHORT, WOT_CLOSE, WOS_Canceled, 5, 2, "SC1", WOF_NOR, false),
        extOrder(ct_, WDT_SHORT, WOT_CLOSE, WOS_Canceled, 2, 0, "SA1", WOF_FOK, false),
    }));

    // 外部委托不进内部订单表
    wtp::OrderMap* tab = trader_.getOrders("");
    ASSERT_NE(nullptr, tab);
    EXPECT_EQ(0u, tab->size());
    tab->release();

    // 空头平今成交统计（onRspTrades 的 s_closetvol 分支）
    trader_.onRspTrades(makeArray({
        makeTrade(ct_, WDT_SHORT, WOT_CLOSETODAY, 2, 7000, "TCT", "OCT", "EXT.TCT") }));

    // stateToName：Nottouched -> UnSmt；非法枚举 -> Error
    WTSOrderInfo* nt = makeOrder(ct_, WDT_LONG, WOT_OPEN, WOS_Nottouched, 1, 0,
        "NT1", "EXT.NT1");
    trader_.onPushOrder(nt);
    nt->release();

    WTSOrderInfo* bad = makeOrder(ct_, WDT_LONG, WOT_OPEN,
        (WTSOrderState)0x7fffffff, 1, 0, "BD1", "EXT.BD1");
    trader_.onPushOrder(bad);
    bad->release();

    // 无合约信息的推送在入口早退
    WTSOrderInfo* bare = WTSOrderInfo::create();
    trader_.onPushOrder(bare);
    bare->release();

    EXPECT_EQ(wtp::TraderAdapter::AS_LOGINED, trader_.state());
}

//////////////////////////////////////////////////////////////////
// 空头二次推送补齐可平量 + 空头平今成交 + 净头寸四方向
// src/WtCore/TraderAdapter.cpp:2042-2085, 2225-2235, 2246-2306
//////////////////////////////////////////////////////////////////
TEST_F(R90TraderReport, ShortCloseCancelAndNetTrades)
{
    buildPlain(trader_);
    seedPos(trader_, {
        makePos(ct_, WDT_LONG, 0, 0, 5, 5),
        makePos(ct_, WDT_SHORT, 2, 2, 2, 2) });

    // 首次推送：空头平昨 3 手 -> 先扣昨仓可用，再扣今仓可用
    trader_.onPushOrder(makeOrder(ct_, WDT_SHORT, WOT_CLOSE, WOS_Submitting,
        3, 0, "SC1", "otp.p11a.9001"));
    EXPECT_DOUBLE_EQ(-1.0, trader_.getPosition(kStdCode, true, 2));

    // 二次推送且撤单：回补 3 手，昨仓上限 2，溢出 1 手回到今仓
    trader_.onPushOrder(makeOrder(ct_, WDT_SHORT, WOT_CLOSE, WOS_Canceled,
        3, 0, "SC1", "otp.p11a.9001"));
    EXPECT_DOUBLE_EQ(-4.0, trader_.getPosition(kStdCode, true, 2));

    // 空头平今成交：今仓 2 -> 0，昨仓不变
    trader_.onPushTrade(makeTrade(ct_, WDT_SHORT, WOT_CLOSETODAY, 2, 7000,
        "ST1", "SO1", "otp.p11a.9002"));
    EXPECT_DOUBLE_EQ(-2.0, trader_.getPosition(kStdCode, false, 2));

    // 净头寸卖出：多头今仓 3 - 2 -> 1（不触发反手）
    {
        wtp::TraderAdapter t2;
        buildPlain(t2);
        seedPos(t2, { makePos(ct_, WDT_LONG, 0, 0, 3, 3) });
        WTSTradeInfo* t = makeTrade(ct_, WDT_SHORT, WOT_OPEN, 2, 7000, "N1", "N1", "");
        t->setNetDirection(false);
        t2.onPushTrade(t);
        EXPECT_DOUBLE_EQ(1.0, t2.getPosition(kStdCode, false, 1));
        EXPECT_DOUBLE_EQ(0.0, t2.getPosition(kStdCode, false, 2));
    }

    // 净头寸卖出：无多头 -> 开空 2 手
    {
        wtp::TraderAdapter t3;
        buildPlain(t3);
        WTSTradeInfo* t = makeTrade(ct_, WDT_SHORT, WOT_OPEN, 2, 7000, "N2", "N2", "");
        t->setNetDirection(false);
        t3.onPushTrade(t);
        EXPECT_DOUBLE_EQ(-2.0, t3.getPosition(kStdCode, false, 2));
    }

    // 净头寸买入：先平空昨仓 1 手
    {
        wtp::TraderAdapter t4;
        buildPlain(t4);
        seedPos(t4, { makePos(ct_, WDT_SHORT, 2, 2, 0, 0) });
        WTSTradeInfo* t = makeTrade(ct_, WDT_LONG, WOT_OPEN, 1, 7000, "N3", "N3", "");
        t->setNetDirection(true);
        t4.onPushTrade(t);
        EXPECT_DOUBLE_EQ(-1.0, t4.getPosition(kStdCode, false, 2));
    }

    // 净头寸买入：无空头 -> 开多 2 手
    {
        wtp::TraderAdapter t5;
        buildPlain(t5);
        WTSTradeInfo* t = makeTrade(ct_, WDT_LONG, WOT_OPEN, 2, 7000, "N4", "N4", "");
        t->setNetDirection(true);
        t5.onPushTrade(t);
        EXPECT_DOUBLE_EQ(2.0, t5.getPosition(kStdCode, false, 1));
    }
}

//////////////////////////////////////////////////////////////////
// 非活跃通知器守卫 + closeLong/closeShort 市价 + 错单回报
// src/WtCore/TraderAdapter.cpp:1434, 1504, 2145, 2332, 2343, 1372, 1390
//////////////////////////////////////////////////////////////////
TEST_F(R90TraderReport, InactiveNotifierGuardsAndMarketCloses)
{
    wtp::EventNotifier notifier;  // 未 init -> _mq_sid==0，全部 notify 短路
    wtp::TraderAdapter trader(&notifier);
    ASSERT_TRUE(trader.initExt(id_.c_str(), &api2_, &bd_, NULL));
    trader.addSink(&sink_);
    ASSERT_TRUE(trader.run());

    // 登录失败与通道错误：_notifier 非空但未激活
    trader.onLoginResult(false, "r90 login failed", 0);
    EXPECT_EQ(wtp::TraderAdapter::AS_LOGINFAILED, trader.state());
    WTSError* err = WTSError::create(WEC_ORDERINSERT, "boom");
    trader.onTraderError(err, NULL);
    err->release();

    // 建立未完成量，供错单回报走 updateUndone + notify 分支
    trader.onRspOrders(makeArray({
        makeOrder(ct_, WDT_LONG, WOT_OPEN, WOS_NotTraded_Queuing, 2, 0,
            "G1", "otp.p11a.7001") }));

    WTSEntrust* ent = WTSEntrust::create("ag2512", 1, 7000.0);
    ent->setContractInfo(ct_);
    ent->setDirection(WDT_LONG);
    ent->setOffsetType(WOT_OPEN);
    ent->setUserTag("otp.p11a.7002");
    ent->setEntrustID("R90E1");
    err = WTSError::create(WEC_ORDERINSERT, "rejected");
    trader.onRspEntrust(ent, err);
    err->release();
    ent->release();

    // 委托/成交推送：_notifier 守卫
    trader.onPushOrder(makeOrder(ct_, WDT_LONG, WOT_OPEN, WOS_Submitting,
        1, 0, "G2", "otp.p11a.7003"));
    trader.onPushTrade(makeTrade(ct_, WDT_LONG, WOT_OPEN, 1, 7000,
        "G3", "G2", "otp.p11a.7003"));

    // 平多/平空价格 0 -> WPT_ANYPRICE
    uint32_t before = api2_.n_insert_;
    EXPECT_NE(UINT_MAX, trader.closeLong(kStdCode, 0.0, 1, false, 0));
    EXPECT_EQ(WPT_ANYPRICE, api2_.last_pxtype_);
    EXPECT_NE(UINT_MAX, trader.closeShort(kStdCode, 0.0, 1, false, 0));
    EXPECT_EQ(WPT_ANYPRICE, api2_.last_pxtype_);
    EXPECT_EQ(before + 2, api2_.n_insert_);
}

}  // namespace
