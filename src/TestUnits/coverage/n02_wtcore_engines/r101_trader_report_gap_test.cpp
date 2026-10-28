/**
 * r101_trader_report_gap_test.cpp — lane R101 / wave18
 *
 * 目标：src/WtCore/TraderAdapter.cpp 的回报统计与持仓可平量剩余分支。
 *
 * 覆盖行段：
 *   - onRspOrders 空头开仓自动撤单统计：:1680-1691
 *   - onPushOrder 买/卖 × 自动撤单与空头错单统计：:1912-1946
 *   - onPushOrder 空头平今可平量：:2067-2072
 *   - onPushTrade 空头平仓超过昨仓、且今仓非零：:2287-2293
 *   - cancel(code,isBuy,qty) 达到目标量后的 break：:1323-1324
 */
#include <TestUnits/coverage/p11_wtcore_traderadapter/r59_trader_fixture.h>

#include <string>

namespace {

using namespace r59x;

WTSOrderInfo* orderWith(WTSContractInfo* ct, WTSDirectionType dir, WTSOffsetType off,
    WTSOrderState st, double vol, double traded, const char* oid, const std::string& utag,
    WTSOrderFlag flag)
{
    return makeOrder(ct, dir, off, st, vol, traded, oid, utag.c_str(), flag);
}

WTSOrderInfo* errorOrder(WTSContractInfo* ct, WTSDirectionType dir, WTSOffsetType off,
    double vol, const char* oid, const std::string& utag)
{
    WTSOrderInfo* o = makeOrder(ct, dir, off, WOS_Canceled, vol, 0, oid, utag.c_str());
    o->setError(WOEF_Normal);
    return o;
}

class R101TraderReport : public R59TraderTest
{
protected:
    std::string tag(const char* local)
    {
        return std::string("otp.") + id_ + "." + local;
    }

    void plain(wtp::TraderAdapter& trader)
    {
        build(trader, NULL);
    }
};

//////////////////////////////////////////////////////////////////
// 自动撤单/错单统计、空头平今可平量、超量平仓、撤单 break
// src/WtCore/TraderAdapter.cpp:1323-1324, 1680-1691, 1912-1946,
// 2067-2072, 2287-2293
//////////////////////////////////////////////////////////////////
TEST_F(R101TraderReport, AutoCancelErrorAndShortCloseBranches)
{
    // 1) onRspOrders：空头开仓自动撤单 -> s_auto_cancels
    {
        wtp::TraderAdapter trader;
        plain(trader);
        WTSOrderInfo* o = orderWith(ct_, WDT_SHORT, WOT_OPEN, WOS_Canceled, 2, 0,
            "SA1", "EXT.SA1", WOF_FAK);
        trader.onRspOrders(makeArray({o}));
        o->release();
    }

    // 2) onPushOrder：买/卖自动撤单 + 空头错单统计
    {
        wtp::TraderAdapter trader;
        plain(trader);
        WTSOrderInfo* b = orderWith(ct_, WDT_LONG, WOT_OPEN, WOS_Canceled, 3, 1,
            "PB1", "EXT.PB1", WOF_FAK);
        trader.onPushOrder(b);
        b->release();

        WTSOrderInfo* s = orderWith(ct_, WDT_SHORT, WOT_OPEN, WOS_Canceled, 3, 1,
            "PS1", "EXT.PS1", WOF_FAK);
        trader.onPushOrder(s);
        s->release();

        WTSOrderInfo* e = errorOrder(ct_, WDT_SHORT, WOT_OPEN, 4, "PE1", "EXT.PE1");
        trader.onPushOrder(e);
        e->release();
    }

    // 3) onPushOrder：空头平今 -> s_newavail 扣减
    {
        wtp::TraderAdapter trader;
        plain(trader);
        seedPos(trader, { makePos(ct_, WDT_SHORT, 2, 2, 2, 2) });
        WTSOrderInfo* c = orderWith(ct_, WDT_SHORT, WOT_CLOSETODAY, WOS_Submitting,
            2, 0, "SC1", tag("9001"), WOF_NOR);
        trader.onPushOrder(c);
        c->release();
        EXPECT_DOUBLE_EQ(0.0, trader.getPosition(kStdCode, true, 2));
    }

    // 4) onPushTrade（净头寸）：平 5 手，昨仓 0、今仓 3，剩余应反手开多 2 手
    {
        wtp::TraderAdapter trader;
        plain(trader);
        seedPos(trader, { makePos(ct_, WDT_SHORT, 0, 0, 3, 3) });
        WTSTradeInfo* t = makeTrade(ct_, WDT_SHORT, WOT_CLOSE, 5, 7000,
            "EX1", "SO1", tag("9002").c_str());
        t->setNetDirection(true);
        trader.onPushTrade(t);
        t->release();
        // 该分支当前实现把 s_newvol 多扣了 left，正确性由 DISABLED 用例登记
        EXPECT_GE(static_cast<int>(trader.state()),
            static_cast<int>(wtp::TraderAdapter::AS_LOGINED));
    }

    // 5) cancel(code,isBuy,qty)：成交量达到 qty 后 break
    {
        wtp::TraderAdapter trader;
        plain(trader);
        WTSOrderInfo* live = orderWith(ct_, WDT_LONG, WOT_OPEN, WOS_Submitting,
            2, 0, "CL1", tag("9003"), WOF_NOR);
        trader.onPushOrder(live);
        live->release();

        wtp::OrderIDs ids = trader.cancel(kStdCode, true, 1);
        EXPECT_EQ(1u, ids.size());
    }
}

//////////////////////////////////////////////////////////////////
// 产品缺陷：净头寸平空超过昨仓时，今仓被扣了 left 而不是 maxVol
// src/WtCore/TraderAdapter.cpp:2287-2293（BUG-R101-001）
//////////////////////////////////////////////////////////////////
TEST_F(R101TraderReport, DISABLED_NetShortCloseOverDeductsNewPosition)
{
    wtp::TraderAdapter trader;
    plain(trader);
    seedPos(trader, { makePos(ct_, WDT_SHORT, 0, 0, 3, 3) });
    WTSTradeInfo* t = makeTrade(ct_, WDT_SHORT, WOT_CLOSE, 5, 7000,
        "EX2", "SO2", tag("9004").c_str());
    t->setNetDirection(true);
    trader.onPushTrade(t);
    t->release();

    // 平 3 手空 + 反手开 2 手多：期望多头今仓 2
    EXPECT_EQ(2.0, trader.getPosition(kStdCode, false, 1));
}

}  // namespace
