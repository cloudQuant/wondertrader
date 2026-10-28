/**
 * r73_trader_report_test.cpp — lane R73 / TraderAdapter 查询与回报分发补测
 *
 * 复用 p11 的进程内 ITraderApi 替身（只读 include，不修改该目录），
 * 覆盖 src/WtCore/TraderAdapter.cpp 中 R59/P11 未覆盖的按合约类别转换分支：
 *   - onRspPosition / onRspOrders / onRspTrades / onPushOrder / onPushTrade
 *     的期货期权、股票与普通（else）代码转换
 *   - cancel(localid) / cancel(stdCode, isBuy, qty) 的 doCancel 类别分支
 *   - 回报数组里的 NULL 合约项、外部单、死单守卫
 *   - queryFund 未就绪早退与 TraderAdapterMgr 空查询
 *
 * 不连接任何真实柜台，不启动线程。
 */
#include "../p11_wtcore_traderadapter/p11_trader_fixture.h"

#include <Share/CodeHelper.hpp>

using namespace p11fix;
using wtp::TraderAdapter;

namespace {

const char* const kOptRaw = "IO2013-C-4000";
const char* const kFlat = "IF";

WTSPositionItem* makePos(WTSContractInfo* ct, WTSDirectionType dir,
    double prePos, double preAvail, double newPos, double newAvail)
{
    WTSPositionItem* p = WTSPositionItem::create(ct->getCode(), "CNY", ct->getExchg());
    p->setContractInfo(ct);
    p->setDirection(dir);
    p->setPrePosition(prePos);
    p->setAvailPrePos(preAvail);
    p->setNewPosition(newPos);
    p->setAvailNewPos(newAvail);
    p->setInitPosition(prePos + newPos);
    p->setAvgPrice(100.0);
    return p;
}

WTSOrderInfo* makeOrder(WTSContractInfo* ct, WTSDirectionType dir, WTSOffsetType off,
    WTSOrderState st, double vol, double traded, const char* oid, const char* utag)
{
    WTSOrderInfo* o = WTSOrderInfo::create();
    o->setCode(ct == NULL ? kFlat : ct->getCode());
    o->setExchange(ct == NULL ? "CFFEX" : ct->getExchg());
    o->setContractInfo(ct);
    o->setDirection(dir);
    o->setOffsetType(off);
    o->setOrderState(st);
    o->setOrderFlag(WOF_NOR);
    o->setPrice(100.0);
    o->setVolume(vol);
    o->setVolTraded(traded);
    o->setVolLeft(vol - traded);
    o->setOrderID(oid);
    o->setEntrustID("R73E001");
    o->setUserTag(utag);
    o->setOrderDate(20250110);
    o->setOrderTime(93100);
    o->setStateMsg("ok");
    return o;
}

WTSTradeInfo* makeTrade(WTSContractInfo* ct, WTSDirectionType dir, WTSOffsetType off,
    double vol, double price, const char* tid, const char* refOrd, const char* utag)
{
    WTSTradeInfo* t = WTSTradeInfo::create(ct == NULL ? kFlat : ct->getCode(),
        ct == NULL ? "CFFEX" : ct->getExchg());
    t->setContractInfo(ct);
    t->setDirection(dir);
    t->setOffsetType(off);
    t->setVolume(vol);
    t->setPrice(price);
    t->setTradeID(tid);
    t->setRefOrder(refOrd);
    t->setUserTag(utag);
    t->setTradeDate(20250110);
    t->setTradeTime(93101);
    t->setAmount(vol * price);
    return t;
}

WTSAccountInfo* makeAccount(const char* cny, double balance)
{
    WTSAccountInfo* a = WTSAccountInfo::create();
    a->setCurrency(cny);
    a->setPreBalance(balance);
    a->setBalance(balance);
    a->setAvailable(balance);
    a->setDynProfit(1.0);
    a->setCloseProfit(2.0);
    a->setMargin(3.0);
    a->setCommission(4.0);
    a->setDeposit(5.0);
    a->setWithdraw(6.0);
    return a;
}

template <typename T>
WTSArray* makeArray(std::initializer_list<T*> items)
{
    WTSArray* arr = WTSArray::create();
    for (auto* it : items)
        arr->append(it);
    return arr;
}

WTSPositionItem* makePosNoContract(WTSDirectionType dir)
{
    WTSPositionItem* p = WTSPositionItem::create("NONE", "CNY", "CFFEX");
    p->setContractInfo(NULL);
    p->setDirection(dir);
    p->setNewPosition(1);
    return p;
}

class R73TraderReportTest : public P11TraderTest
{
protected:
    void TearDown() override
    {
        std::error_code ec;
        for (const char* id : { "r73_opt", "r73_flat", "r73_cancel", "r73_pre", "r73_tol" })
        {
            std::filesystem::path dir(WtHelper::getBaseDir());
            dir /= "traders";
            dir /= id;
            std::filesystem::remove_all(dir, ec);
        }
        P11TraderTest::TearDown();
    }

    WTSCommodityInfo* makeProduct(const char* pid, const char* exchg,
        wtp::ContractCategory cat)
    {
        WTSCommodityInfo* comm = WTSCommodityInfo::create(pid, pid, exchg, "TRADING", "CHINA");
        comm->setCategory(cat);
        comm->setCoverMode(wtp::CM_OpenCover);
        comm->setTradingMode(wtp::TM_Both);
        comm->setVolScale(1);
        comm->setPriceTick(1.0);
        return comm;
    }
};

// 覆盖 TraderAdapter.cpp:1545-1602（期权持仓转换）、L1604-1730（期权订单
// 转换与 undone）、L1739-1816（期权成交转换）、L1873-2146（期权推送单）、
// L2148-2335（期权推送成交）
TEST_F(R73TraderReportTest, OptionReportsAndPushes)
{
    WTSCommodityInfo* comm = makeProduct("IO", "CFFEX", wtp::CC_FutOption);
    WTSContractInfo* ct = WTSContractInfo::create(kOptRaw, kOptRaw, "CFFEX", "IO");
    ct->setAltCode(kOptRaw);
    bd_.regis(comm, ct);

    const std::string optStd = CodeHelper::rawFutOptCodeToStdCode(kOptRaw, "CFFEX");

    TraderAdapter trader;
    ASSERT_TRUE(trader.initExt("r73_opt", &api_, &bd_, NULL));
    trader.addSink(&sink_);
    ASSERT_TRUE(trader.run());
    trader.onLoginResult(true, "ok", 20250110);
    EXPECT_EQ(TraderAdapter::AS_LOGINED, trader.state());

    WTSArray* pos = makeArray<WTSPositionItem>({
        makePos(ct, WDT_LONG, 2, 2, 1, 1),
        makePos(ct, WDT_SHORT, 1, 1, 0, 0),
        makePosNoContract(WDT_LONG)
    });
    trader.onRspPosition(pos);
    EXPECT_EQ(TraderAdapter::AS_POSITION_QRYED, trader.state());
    EXPECT_EQ(2u, sink_.poss_.size());
    EXPECT_DOUBLE_EQ(3.0, trader.getPosition(optStd.c_str(), false, 1));
    EXPECT_DOUBLE_EQ(-1.0, trader.getPosition(optStd.c_str(), false, 2));

    WTSArray* orders = makeArray<WTSOrderInfo>({
        makeOrder(ct, WDT_LONG, WOT_OPEN, WOS_NotTraded_Queuing, 2, 0, "OPT1", "otp.r73_opt.1001"),
        makeOrder(NULL, WDT_LONG, WOT_OPEN, WOS_NotTraded_Queuing, 1, 0, "NOC", "otp.r73_opt.1002"),
        makeOrder(ct, WDT_SHORT, WOT_OPEN, WOS_NotTraded_Queuing, 1, 0, "EXT1", "external")
    });
    trader.onRspOrders(orders);
    EXPECT_EQ(TraderAdapter::AS_ORDERS_QRYED, trader.state());
    EXPECT_EQ(1u, api_.n_qtrades_);
    EXPECT_DOUBLE_EQ(2.0, trader.getUndoneQty(optStd.c_str()));
    OrderMap* om = trader.getOrders(optStd.c_str());
    ASSERT_NE(om, nullptr);
    EXPECT_EQ(1u, om->size());
    om->release();

    WTSArray* trades = makeArray<WTSTradeInfo>({
        makeTrade(ct, WDT_LONG, WOT_OPEN, 1, 100.0, "OPT1", "OPT1", "otp.r73_opt.1001")
    });
    trader.onRspTrades(trades);
    EXPECT_EQ(TraderAdapter::AS_TRADES_QRYED, trader.state());
    EXPECT_EQ(1u, api_.n_qaccount_);

    WTSArray* acct = makeArray<WTSAccountInfo>({ makeAccount("CNY", 1000.0) });
    trader.onRspAccount(acct);
    EXPECT_EQ(TraderAdapter::AS_ALLREADY, trader.state());
    EXPECT_EQ(1u, sink_.accts_.size());
    EXPECT_EQ(1u, sink_.n_ready_);

    trader.onPushOrder(makeOrder(ct, WDT_LONG, WOT_OPEN, WOS_NotTraded_Queuing,
        1, 0, "OPT2", "otp.r73_opt.1003"));
    trader.onPushTrade(makeTrade(ct, WDT_LONG, WOT_OPEN, 1, 100.0,
        "OPT2", "OPT2", "otp.r73_opt.1003"));
    EXPECT_EQ(1u, sink_.trades_.size());
    EXPECT_EQ(1u, sink_.orders_.size());
}

// 覆盖 TraderAdapter.cpp:1557-1563（else 平码）、L1753-1762（股票/else 成交）、
// L1884-1891（else 推送单）、L2156-2165（股票/else 推送成交）
TEST_F(R73TraderReportTest, StockAndFlatCodeConversions)
{
    WTSCommodityInfo* stock = makeProduct("600000", "SSE", wtp::CC_Stock);
    WTSContractInfo* sct = WTSContractInfo::create("600000", "600000", "SSE", "600000");
    sct->setAltCode("600000");
    bd_.regis(stock, sct);

    WTSCommodityInfo* index = makeProduct("IF", "CFFEX", wtp::CC_UserIndex);
    WTSContractInfo* ict = WTSContractInfo::create("IF", "IF", "CFFEX", "IF");
    ict->setAltCode("IF");
    bd_.regis(index, ict);

    TraderAdapter trader;
    ASSERT_TRUE(trader.initExt("r73_flat", &api_, &bd_, NULL));
    trader.addSink(&sink_);
    ASSERT_TRUE(trader.run());
    trader.onLoginResult(true, "ok", 20250110);

    // 600000 不是分月代码 -> onRspPosition 走 rawFlatCodeToStdCode
    const std::string stockFlat = CodeHelper::rawFlatCodeToStdCode("600000", "SSE", "600000");
    const std::string indexStd = CodeHelper::rawFlatCodeToStdCode("IF", "CFFEX", "IF");

    WTSArray* pos = makeArray<WTSPositionItem>({
        makePos(sct, WDT_LONG, 1, 1, 1, 1),
        makePos(ict, WDT_LONG, 2, 2, 0, 0)
    });
    trader.onRspPosition(pos);
    EXPECT_DOUBLE_EQ(2.0, trader.getPosition(stockFlat.c_str(), false, 1));
    EXPECT_DOUBLE_EQ(2.0, trader.getPosition(indexStd.c_str(), false, 1));

    WTSArray* trades = makeArray<WTSTradeInfo>({
        makeTrade(sct, WDT_LONG, WOT_OPEN, 1, 10.0, "ST1", "ST1", "otp.r73_flat.2001"),
        makeTrade(ict, WDT_LONG, WOT_OPEN, 2, 20.0, "IX1", "IX1", "otp.r73_flat.2002")
    });
    trader.onRspTrades(trades);

    trader.onPushOrder(makeOrder(ict, WDT_LONG, WOT_OPEN, WOS_NotTraded_Queuing,
        1, 0, "IX2", "otp.r73_flat.2003"));
    trader.onPushTrade(makeTrade(sct, WDT_LONG, WOT_OPEN, 1, 10.0,
        "ST2", "ST2", "otp.r73_flat.2004"));
    trader.onPushTrade(makeTrade(ict, WDT_LONG, WOT_OPEN, 1, 20.0,
        "IX3", "IX3", "otp.r73_flat.2005"));
    EXPECT_EQ(1u, sink_.orders_.size());
    EXPECT_EQ(2u, sink_.trades_.size());
}

// 覆盖 TraderAdapter.cpp:1244-1329（doCancel 的期权/平码转换、
// cancel(localid)/cancel(code, dir, qty) 与未命中订单）
TEST_F(R73TraderReportTest, CancelAcrossContractCategories)
{
    WTSCommodityInfo* comm = makeProduct("IO", "CFFEX", wtp::CC_FutOption);
    WTSContractInfo* oct = WTSContractInfo::create(kOptRaw, kOptRaw, "CFFEX", "IO");
    oct->setAltCode(kOptRaw);
    bd_.regis(comm, oct);

    WTSCommodityInfo* index = makeProduct("IF", "CFFEX", wtp::CC_UserIndex);
    WTSContractInfo* ict = WTSContractInfo::create(kFlat, kFlat, "CFFEX", "IF");
    ict->setAltCode(kFlat);
    bd_.regis(index, ict);

    TraderAdapter trader;
    ASSERT_TRUE(trader.initExt("r73_cancel", &api_, &bd_, NULL));
    ASSERT_TRUE(trader.run());
    trader.onLoginResult(true, "ok", 20250110);

    WTSArray* orders = makeArray<WTSOrderInfo>({
        makeOrder(oct, WDT_LONG, WOT_OPEN, WOS_NotTraded_Queuing, 1, 0, "OC1", "otp.r73_cancel.3001"),
        makeOrder(ict, WDT_LONG, WOT_OPEN, WOS_NotTraded_Queuing, 1, 0, "IC1", "otp.r73_cancel.3002")
    });
    trader.onRspOrders(orders);
    ASSERT_EQ(2u, trader.getOrders("")->size());

    EXPECT_TRUE(trader.cancel(3001u));
    EXPECT_TRUE(trader.cancel(3002u));
    EXPECT_EQ(2u, api_.n_action_);
    EXPECT_FALSE(trader.cancel(9999u));

    // 批量撤单：平码 IF 与订单 code 匹配，方向为买开
    OrderIDs ids = trader.cancel("CFFEX.IF", true, 0.0);
    EXPECT_FALSE(ids.empty());

    // 方向不匹配 -> 空
    EXPECT_TRUE(trader.cancel("CFFEX.IF", false, 0.0).empty());
}

// 覆盖 TraderAdapter.cpp:1547-1555（持仓 NULL 合约跳过）、L1613-1636
// （订单 NULL 合约/外部单/死单）、L1743-1752（成交 NULL 合约跳过）、
// L2356-2362（queryFund 未就绪早退）、L2386-2422（Mgr 空查询）
TEST_F(R73TraderReportTest, ReportTolerantItemsAndPreReadyQueries)
{
    TraderAdapter pre;
    ASSERT_TRUE(pre.initExt("r73_pre", &api_, &bd_, NULL));
    const uint32_t q0 = api_.n_qaccount_;
    pre.queryFund();
    EXPECT_EQ(q0, api_.n_qaccount_);

    TraderAdapter trader;
    ASSERT_TRUE(trader.initExt("r73_tol", &api_, &bd_, NULL));
    ASSERT_TRUE(trader.run());
    trader.onLoginResult(true, "ok", 20250110);

    WTSArray* pos = makeArray<WTSPositionItem>({
        makePosNoContract(WDT_LONG),
        makePos(ct_, WDT_LONG, 1, 1, 1, 1)
    });
    trader.onRspPosition(pos);
    EXPECT_EQ(2u, sink_.poss_.size()); // 两条方向都发，NULL 合约项不参与

    WTSArray* orders = makeArray<WTSOrderInfo>({
        makeOrder(NULL, WDT_LONG, WOT_OPEN, WOS_NotTraded_Queuing, 1, 0, "N1", "x"),
        makeOrder(ct_, WDT_LONG, WOT_OPEN, WOS_AllTraded, 1, 1, "D1", "otp.r73_tol.4001"),
        makeOrder(ct_, WDT_SHORT, WOT_OPEN, WOS_NotTraded_Queuing, 1, 0, "E1", "external")
    });
    trader.onRspOrders(orders);
    EXPECT_EQ(0u, trader.getOrders("")->size());

    WTSArray* trades = makeArray<WTSTradeInfo>({
        makeTrade(NULL, WDT_LONG, WOT_OPEN, 1, 10.0, "NT", "NT", "x"),
        makeTrade(ct_, WDT_LONG, WOT_OPEN, 1, 10.0, "T1", "T1", "otp.r73_tol.4002")
    });
    trader.onRspTrades(trades);

    trader.enumPosition([this](const char* code, bool isLong, double, double, double, double) {
        sink_.poss_.push_back({ code, isLong, 0, 0, 0, 0, 0 });
    });
    EXPECT_GE(sink_.poss_.size(), 3u);

    WTSVariant* dummy = WTSCfgLoader::load_from_content("{}");
    ASSERT_NE(dummy, nullptr);
    TraderAdapterMgr mgr;
    EXPECT_TRUE(mgr.getAdapter("missing") == NULL);
    mgr.refresh_funds();
    mgr.release();
    dummy->release();
}

}  // namespace
