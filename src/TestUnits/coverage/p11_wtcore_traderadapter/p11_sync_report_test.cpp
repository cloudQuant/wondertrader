/**
 * p11_sync_report_test.cpp — 登录后同步回报链（TraderAdapter.cpp:1426-1816, 2346-2362）
 *
 * 覆盖 n01 跳过的「回报实体分支」中的同步查询部分：
 *   onLoginResult 成功/失败、handleEvent(WTE_Close)、
 *   onRspPosition 写持仓表（覆盖语义）+ on_position 多订阅者分发 + enumPosition、
 *   onRspOrders 建订单表 / 清并重算未完成量 / 买卖·错单·撤单统计 / 外部单与死单不入表、
 *   onRspTrades 开平统计 + checkSelfMatch、
 *   onRspAccount 资金分发 + AS_TRADES_QRYED→AS_ALLREADY + on_channel_ready、
 *   queryFund 就绪后真的去查资金。
 * 交易通道用 p11_trader_fixture.h 里的 FakeTraderApi 替身，不接任何真实柜台。
 *
 * 内存策略：create() 出的对象自带 1 引用，WTSArray::append 再 +1，被测方 add/grab 各自 +1，
 * 用例不回收这些引用（宁可泄漏也不悬垂），保证启用用例零崩溃。
 */
#include "p11_trader_fixture.h"

#include <algorithm>

using namespace p11fix;
using wtp::TraderAdapter;

namespace {

const char* kStdAu = "SHFE.au.2212";
const char* kRawAu = "au2212";
const char* kTag1001 = "otp.p11a.1001";
const char* kTag1002 = "otp.p11a.1002";

WTSOrderInfo* makeOrder(WTSContractInfo* ct, WTSDirectionType dir, WTSOffsetType off,
    WTSOrderState st, double vol, double traded, const char* oid, const char* utag,
    WTSOrderFlag flag = WOF_NOR)
{
    WTSOrderInfo* o = WTSOrderInfo::create();
    o->setCode(ct->getCode());
    o->setExchange(ct->getExchg());
    o->setContractInfo(ct);
    o->setDirection(dir);
    o->setOffsetType(off);
    o->setOrderState(st);
    o->setOrderFlag(flag);
    o->setPrice(7000.0);
    o->setVolume(vol);
    o->setVolTraded(traded);
    o->setVolLeft(vol - traded);
    o->setOrderID(oid);
    o->setEntrustID("P11E001");
    o->setUserTag(utag);
    o->setOrderDate(20240510);
    o->setOrderTime(93000);
    o->setStateMsg("ok");
    return o;
}

WTSTradeInfo* makeTrade(WTSContractInfo* ct, WTSDirectionType dir, WTSOffsetType off,
    double vol, double price, const char* tid, const char* refOrd, const char* utag)
{
    WTSTradeInfo* t = WTSTradeInfo::create(ct->getCode(), ct->getExchg());
    t->setContractInfo(ct);
    t->setDirection(dir);
    t->setOffsetType(off);
    t->setVolume(vol);
    t->setPrice(price);
    t->setTradeID(tid);
    t->setRefOrder(refOrd);
    t->setUserTag(utag);
    t->setTradeDate(20240510);
    t->setTradeTime(93010);
    t->setAmount(vol * price);
    return t;
}

WTSPositionItem* makePos(WTSContractInfo* ct, WTSDirectionType dir,
    double prePos, double preAvail, double newPos, double newAvail)
{
    WTSPositionItem* p = WTSPositionItem::create(ct == NULL ? kRawCode : ct->getCode(),
        "CNY", ct == NULL ? kExchg : ct->getExchg());
    p->setContractInfo(ct);
    p->setDirection(dir);
    p->setPrePosition(prePos);
    p->setAvailPrePos(preAvail);
    p->setNewPosition(newPos);
    p->setAvailNewPos(newAvail);
    p->setInitPosition(prePos + newPos);
    p->setAvgPrice(7000.0);
    return p;
}

template <typename T>
WTSArray* makeArray(std::initializer_list<T*> items)
{
    WTSArray* arr = WTSArray::create();
    for (auto* it : items)
        arr->append(it);
    return arr;
}

typedef p11fix::RecordingSink::PosRec PosRec;
const PosRec* findPos(const std::vector<PosRec>& v, const char* code, bool isLong)
{
    for (auto& r : v)
        if (r.code == code && r.isLong == isLong)
            return &r;
    return NULL;
}

class P11SyncReport : public P11TraderTest
{
};

TEST_F(P11SyncReport, login_result_drives_query_chain)
{
    TraderAdapter trader;
    buildReady(trader);

    EXPECT_EQ(TraderAdapter::AS_LOGINING, trader.state());
    EXPECT_EQ(1u, api_.n_register_);
    EXPECT_EQ(1u, api_.n_connect_);
    EXPECT_EQ(0u, api_.n_qpositions_);

    // 登录失败：状态落到 AS_LOGINFAILED，不触发任何查询
    trader.onLoginResult(false, "bad password", 0);
    EXPECT_EQ(TraderAdapter::AS_LOGINFAILED, trader.state());
    EXPECT_FALSE(trader.isReady());
    EXPECT_EQ(0u, api_.n_qpositions_);
    EXPECT_EQ(0u, sink_.n_ready_);

    // 登录成功：AS_LOGINED + 交易日落地 + 立刻查持仓
    trader.onLoginResult(true, "ok", 20240510);
    EXPECT_EQ(TraderAdapter::AS_LOGINED, trader.state());
    EXPECT_EQ(1u, api_.n_qpositions_);

    trader.onRspPosition(makeArray({ makePos(ct_, WDT_LONG, 3, 3, 2, 2) }));
    EXPECT_EQ(TraderAdapter::AS_POSITION_QRYED, trader.state());
    EXPECT_EQ(1u, api_.n_qorders_);

    trader.onRspOrders(makeArray({
        makeOrder(ct_, WDT_LONG, WOT_OPEN, WOS_NotTraded_Queuing, 5, 0, "O1001", kTag1001) }));
    EXPECT_EQ(TraderAdapter::AS_ORDERS_QRYED, trader.state());
    EXPECT_EQ(1u, api_.n_qtrades_);

    trader.onRspTrades(makeArray({
        makeTrade(ct_, WDT_LONG, WOT_OPEN, 2, 7100, "T1", "O1001", kTag1001) }));
    EXPECT_EQ(TraderAdapter::AS_TRADES_QRYED, trader.state());
    EXPECT_EQ(1u, api_.n_qaccount_);

    // 资金回报 -> 全部就绪 + on_channel_ready
    WTSAccountInfo* acct = WTSAccountInfo::create();
    acct->setCurrency("CNY");
    acct->setPreBalance(1000000.0);
    acct->setBalance(1002500.0);
    acct->setDynProfit(3000.0);
    acct->setAvailable(900000.0);
    acct->setCloseProfit(1500.0);
    acct->setMargin(80000.0);
    acct->setCommission(200.0);
    acct->setDeposit(50000.0);
    acct->setWithdraw(10000.0);
    trader.onRspAccount(makeArray({ acct }));

    EXPECT_TRUE(trader.isReady());
    EXPECT_EQ(TraderAdapter::AS_ALLREADY, trader.state());
    EXPECT_EQ(1u, sink_.n_ready_);
    ASSERT_EQ(1u, sink_.accts_.size());
    EXPECT_STREQ("CNY", sink_.accts_[0].currency.c_str());
    EXPECT_DOUBLE_EQ(1000000.0, sink_.accts_[0].prebal);
    EXPECT_DOUBLE_EQ(1002500.0, sink_.accts_[0].bal);
    EXPECT_DOUBLE_EQ(1005500.0, sink_.accts_[0].dynbal);   // balance + dynprofit
    EXPECT_DOUBLE_EQ(900000.0, sink_.accts_[0].avail);
    EXPECT_DOUBLE_EQ(1500.0, sink_.accts_[0].closeprofit);
    EXPECT_DOUBLE_EQ(3000.0, sink_.accts_[0].dynprofit);
    EXPECT_DOUBLE_EQ(80000.0, sink_.accts_[0].margin);
    EXPECT_DOUBLE_EQ(200.0, sink_.accts_[0].fee);
    EXPECT_DOUBLE_EQ(50000.0, sink_.accts_[0].deposit);
    EXPECT_DOUBLE_EQ(10000.0, sink_.accts_[0].withdraw);

    // 就绪前 queryFund 是空转，就绪后才真的查
    trader.queryFund();
    EXPECT_EQ(2u, api_.n_qaccount_);

    // 断连：所有订阅者收到 on_channel_lost
    trader.handleEvent(WTE_Close, -1);
    EXPECT_EQ(1u, sink_.n_lost_);

    // 已就绪后重复的资金回报仍分发，但不再重复广播通道就绪
    trader.onRspAccount(makeArray({ acct }));
    EXPECT_EQ(TraderAdapter::AS_ALLREADY, trader.state());
    EXPECT_EQ(1u, sink_.n_ready_);
    EXPECT_EQ(2u, sink_.accts_.size());

    trader.release();
    EXPECT_EQ(2u, api_.n_register_);        // release() 里还会 registerSpi(NULL)
    EXPECT_EQ(1u, api_.n_release_);
}

TEST_F(P11SyncReport, position_report_writes_table_and_skips_unknown_contract)
{
    TraderAdapter trader;
    buildReady(trader);
    trader.onLoginResult(true, "ok", 20240510);

    // 空数组回报：不写持仓表，但状态照样推进并触发查订单（回报里「有没有数据」和「状态机」是解耦的）
    WTSArray* empty = WTSArray::create();
    trader.onRspPosition(empty);
    EXPECT_EQ(TraderAdapter::AS_POSITION_QRYED, trader.state());
    EXPECT_EQ(1u, api_.n_qorders_);
    EXPECT_EQ(0u, sink_.poss_.size());

    // 多 / 空 / 合约信息缺失(应被 continue 跳过)
    trader.onRspPosition(makeArray({
        makePos(ct_, WDT_LONG, 3, 3, 2, 2),
        makePos(ct2_, WDT_SHORT, 4, 1, 2, 0),
        makePos(NULL, WDT_LONG, 9, 9, 9, 9) }));

    EXPECT_EQ(TraderAdapter::AS_POSITION_QRYED, trader.state());
    EXPECT_EQ(1u, api_.n_qorders_);      // 已经不是 AS_LOGINED, 不会重复查订单

    EXPECT_DOUBLE_EQ(5.0, trader.getPosition(kStdCode, false, 1));      // 多头 今2+昨3
    EXPECT_DOUBLE_EQ(5.0, trader.getPosition(kStdCode, true, 1));       // 多头可用 2+3
    EXPECT_DOUBLE_EQ(0.0, trader.getPosition(kStdCode, false, 2));      // 空头为 0
    EXPECT_DOUBLE_EQ(5.0, trader.getPosition(kStdCode, false, 3));      // 5 - 0
    EXPECT_DOUBLE_EQ(0.0, trader.getPosition(kStdAu, false, 1));        // 无多头
    EXPECT_DOUBLE_EQ(-1.0, trader.getPosition(kStdAu, true, 3));        // 0 - (0+1)
    EXPECT_DOUBLE_EQ(-6.0, trader.getPosition(kStdAu, false, 3));       // 0 - (2+4)
    EXPECT_DOUBLE_EQ(0.0, trader.getPosition("DCE.m.2109", false, 3));  // 未登记

    // on_position 每个持仓各发多空两条, 合约信息缺失的那条不产生记录
    EXPECT_EQ(4u, sink_.poss_.size());
    const PosRec* pAgLong = findPos(sink_.poss_, kStdCode, true);
    ASSERT_NE(nullptr, pAgLong);
    EXPECT_DOUBLE_EQ(3.0, pAgLong->prevol);
    EXPECT_DOUBLE_EQ(3.0, pAgLong->preavail);
    EXPECT_DOUBLE_EQ(2.0, pAgLong->newvol);
    EXPECT_DOUBLE_EQ(2.0, pAgLong->newavail);
    EXPECT_EQ(20240510u, pAgLong->tdate);

    const PosRec* pAuShort = findPos(sink_.poss_, kStdAu, false);
    ASSERT_NE(nullptr, pAuShort);
    EXPECT_DOUBLE_EQ(4.0, pAuShort->prevol);
    EXPECT_DOUBLE_EQ(1.0, pAuShort->preavail);
    EXPECT_DOUBLE_EQ(2.0, pAuShort->newvol);
    EXPECT_DOUBLE_EQ(0.0, pAuShort->newavail);

    const PosRec* pAgShort = findPos(sink_.poss_, kStdCode, false);
    ASSERT_NE(nullptr, pAgShort);
    EXPECT_DOUBLE_EQ(0.0, pAgShort->newvol);
    EXPECT_EQ(nullptr, findPos(sink_.poss_, "ag2512", true));   // 表里只用标准代码

    uint32_t nLong = 0, nShort = 0;
    trader.enumPosition([&](const char* code, bool isLong, double preVol, double preAvail,
        double newVol, double newAvail) {
            (void)preVol; (void)preAvail; (void)newVol; (void)newAvail;
            if (strcmp(code, kStdCode) != 0 && strcmp(code, kStdAu) != 0)
                ADD_FAILURE() << "unexpected code " << code;
            if (isLong) ++nLong; else ++nShort;
        });
    EXPECT_EQ(1u, nLong);
    EXPECT_EQ(1u, nShort);

    // 第二次回报是覆盖语义而不是累加
    trader.onRspPosition(makeArray({ makePos(ct_, WDT_LONG, 3, 1, 0, 0) }));
    EXPECT_DOUBLE_EQ(1.0, trader.getPosition(kStdCode, true, 1));
    EXPECT_DOUBLE_EQ(3.0, trader.getPosition(kStdCode, false, 1));
}

TEST_F(P11SyncReport, order_report_rebuilds_table_and_undone)
{
    TraderAdapter trader;
    buildReady(trader);
    trader.onLoginResult(true, "ok", 20240510);
    trader.onRspPosition(makeArray({ makePos(ct_, WDT_LONG, 10, 10, 0, 0) }));

    EXPECT_EQ(nullptr, trader.getOrders(kStdCode));    // _orders 尚未建立

    WTSOrderInfo* o1001 = makeOrder(ct_, WDT_LONG, WOT_OPEN, WOS_NotTraded_Queuing, 5, 0, "O1001", kTag1001);
    WTSOrderInfo* o1002 = makeOrder(ct_, WDT_LONG, WOT_CLOSE, WOS_PartTraded_Queuing, 3, 1, "O1002", kTag1002);
    WTSOrderInfo* oDone = makeOrder(ct_, WDT_SHORT, WOT_OPEN, WOS_AllTraded, 2, 2, "O1003", "otp.p11a.1003");
    WTSOrderInfo* oExt = makeOrder(ct_, WDT_LONG, WOT_OPEN, WOS_NotTraded_Queuing, 7, 0, "O1004", "out.1.1004");
    WTSOrderInfo* oWrong = makeOrder(ct_, WDT_SHORT, WOT_OPEN, WOS_Canceled, 4, 0, "O1005", "otp.p11a.1005");
    oWrong->setError(WOEF_Normal);
    WTSOrderInfo* oCancl = makeOrder(ct_, WDT_LONG, WOT_CLOSE, WOS_Canceled, 6, 2, "O1006", "otp.p11a.1006");
    WTSOrderInfo* oAuto = makeOrder(ct_, WDT_SHORT, WOT_CLOSE, WOS_Canceled, 8, 3, "O1007", "otp.p11a.1007", WOF_FAK);
    WTSOrderInfo* oNoCt = makeOrder(ct_, WDT_LONG, WOT_OPEN, WOS_NotTraded_Queuing, 1, 0, "O1008", kTag1001);
    oNoCt->setContractInfo(NULL);

    trader.onRspOrders(makeArray({ o1001, o1002, oDone, oExt, oWrong, oCancl, oAuto, oNoCt,
        static_cast<WTSOrderInfo*>(NULL) }));

    // setCode(cInfo->getCode()): 标准代码被回写成交易所原始代码
    EXPECT_STREQ(kRawCode, o1001->getCode());

    // 未完成量每次全量重算: 只有仍然存活且属于本系统的单参与 -> 买开+5, 卖平-2
    EXPECT_DOUBLE_EQ(3.0, trader.getUndoneQty(kStdCode));
    EXPECT_DOUBLE_EQ(0.0, trader.getUndoneQty(kStdAu));

    OrderMap* all = trader.getOrders("");
    ASSERT_NE(nullptr, all);
    EXPECT_EQ(2u, all->size());
    EXPECT_EQ(o1001, all->get(1001));
    EXPECT_EQ(o1002, all->get(1002));
    EXPECT_EQ(nullptr, all->get(1003));
    EXPECT_EQ(nullptr, all->get(1004));        // 外部单不入表
    EXPECT_GE(o1001->retainCount(), 2u);    // 订单表确实持有该单

    // 表里的 code 已被回写成原始代码, 所以按标准代码过滤取不到
    OrderMap* byStd = trader.getOrders(kStdCode);
    ASSERT_NE(nullptr, byStd);
    EXPECT_EQ(0u, byStd->size());
    byStd->release();

    OrderMap* byRaw = trader.getOrders(kRawCode);
    ASSERT_NE(nullptr, byRaw);
    EXPECT_EQ(2u, byRaw->size());
    byRaw->release();

    OrderMap* none = trader.getOrders(kRawAu);
    ASSERT_NE(nullptr, none);
    EXPECT_EQ(0u, none->size());
    none->release();

    // 第二次回报: 表被复用, 同键旧单被替换释放, 状态不再倒退
    trader.onRspOrders(makeArray({
        makeOrder(ct_, WDT_LONG, WOT_OPEN, WOS_NotTraded_Queuing, 5, 0, "O1001", kTag1001) }));
    EXPECT_EQ(TraderAdapter::AS_ORDERS_QRYED, trader.state());
    EXPECT_DOUBLE_EQ(5.0, trader.getUndoneQty(kStdCode));

    OrderMap* after = trader.getOrders("");
    ASSERT_NE(nullptr, after);
    EXPECT_EQ(1u, after->size());
    after->release();

    EXPECT_EQ(1u, api_.n_qtrades_);
}

TEST_F(P11SyncReport, trade_report_stats_and_self_match_guard)
{
    TraderAdapter trader;
    buildReady(trader);
    trader.onLoginResult(true, "ok", 20240510);

    uint32_t actsBefore = api_.n_action_;
    trader.onRspTrades(makeArray({
        makeTrade(ct_, WDT_LONG, WOT_OPEN, 2, 7100, "T1", "O1001", kTag1001),
        makeTrade(ct_, WDT_LONG, WOT_CLOSE, 1, 7110, "T2", "O1002", kTag1002),
        makeTrade(ct_, WDT_LONG, WOT_CLOSETODAY, 1, 7120, "T3", "O1003", kTag1002),
        makeTrade(ct_, WDT_SHORT, WOT_OPEN, 3, 7090, "T4", "O1006", kTag1002),
        makeTrade(ct_, WDT_SHORT, WOT_CLOSE, 2, 7080, "T5", "O1007", ""),
        makeTrade(ct2_, WDT_LONG, WOT_OPEN, 5, 500, "T6", "O1008", "out.1.1008"),
        // 同一个成交号、不同关联订单 => 判定自成交
        makeTrade(ct_, WDT_LONG, WOT_OPEN, 1, 7100, "T1", "O9999", kTag1001),
        // 同一个成交号、相同关联订单 => 重复推送, 不再判自成交
        makeTrade(ct_, WDT_LONG, WOT_OPEN, 1, 7100, "T4", "O1006", kTag1002) }));

    EXPECT_TRUE(trader.isSelfMatched(kStdCode));
    EXPECT_FALSE(trader.isSelfMatched(kStdAu));
    EXPECT_FALSE(trader.isSelfMatched(kRawCode));
    EXPECT_EQ(TraderAdapter::AS_TRADES_QRYED, trader.state());
    EXPECT_EQ(1u, api_.n_qaccount_);

    // NULL 成交直接返回 false
    EXPECT_FALSE(trader.checkSelfMatch(kStdCode, NULL));

    // 尚未建立订单表时按代码撤单得到空集合, 也不会碰通道
    OrderIDs ids = trader.cancel(kStdCode, true);
    EXPECT_EQ(0u, ids.size());
    EXPECT_EQ(actsBefore, api_.n_action_);
}

} // namespace
