/**
 * p11_push_notify_test.cpp — 实时回报推送链（TraderAdapter.cpp:1873-2335）
 *
 * 覆盖 onPushOrder / onPushTrade / onRspEntrust 对订单表、持仓可平量、未完成量、开平统计的实体改写，
 * 以及错乱/重复/迟到回报的防御分支：
 *   - 首笔推送：订单计数 + 平仓预占可平量（先昨后今、下限截断）
 *   - 重复推送：不再计数；撤销时按 (vol-volTraded) 回补可平量，昨仓溢出部分转到今仓
 *   - 未完成量只在「撤销」推送与「委托错误回报」时更新（挂单量由 onRspOrders 全量重算）
 *   - 非本系统的单子：不进订单表、不更新未完成量、不通知订阅者
 *   - 净头寸模式：先平后开，可用量直接等于总量
 *   - 自成交：同一成交号关联不同订单 => 拉黑该合约；同号同单 => 重复推送放过
 *   - 多订阅者分发 on_order / on_trade / on_entrust
 * 通道一律用 FakeTraderApi 替身。
 */
#include "p11_trader_fixture.h"

using namespace p11fix;
using wtp::TraderAdapter;

namespace {

const char* kStdAu = "SHFE.au.2212";
const char* kTag1001 = "otp.p11a.1001";
const char* kTag1002 = "otp.p11a.1002";
const char* kTag1003 = "otp.p11a.1003";

WTSOrderInfo* makePushOrder(WTSContractInfo* ct, WTSDirectionType dir, WTSOffsetType off,
    WTSOrderState st, double vol, double traded, const char* oid, const char* utag,
    WTSOrderFlag flag = WOF_NOR)
{
    WTSOrderInfo* o = WTSOrderInfo::create();
    o->setCode(kStdCode);
    o->setExchange(ct->getExchg());
    o->setContractInfo(ct);
    o->setDirection(dir);
    o->setOffsetType(off);
    o->setOrderState(st);
    o->setOrderFlag(flag);
    o->setPrice(7100.0);
    o->setVolume(vol);
    o->setVolTraded(traded);
    o->setVolLeft(vol - traded);
    o->setOrderID(oid);
    o->setEntrustID("P11E010");
    o->setUserTag(utag);
    o->setOrderDate(20240510);
    o->setOrderTime(93100);
    o->setStateMsg("pushed");
    return o;
}

WTSTradeInfo* makePushTrade(WTSContractInfo* ct, WTSDirectionType dir, WTSOffsetType off,
    double vol, double price, const char* tid, const char* refOrd, const char* utag)
{
    WTSTradeInfo* t = WTSTradeInfo::create(kStdCode, ct->getExchg());
    t->setContractInfo(ct);
    t->setDirection(dir);
    t->setOffsetType(off);
    t->setVolume(vol);
    t->setPrice(price);
    t->setTradeID(tid);
    t->setRefOrder(refOrd);
    t->setUserTag(utag);
    t->setTradeDate(20240510);
    t->setTradeTime(93110);
    t->setAmount(vol * price);
    return t;
}

WTSPositionItem* makePosItem(WTSContractInfo* ct, WTSDirectionType dir,
    double prePos, double preAvail, double newPos, double newAvail)
{
    WTSPositionItem* p = WTSPositionItem::create(ct->getCode(), "CNY", ct->getExchg());
    p->setContractInfo(ct);
    p->setDirection(dir);
    p->setPrePosition(prePos);
    p->setAvailPrePos(preAvail);
    p->setNewPosition(newPos);
    p->setAvailNewPos(newAvail);
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

class P11PushNotify : public P11TraderTest
{
};

TEST_F(P11PushNotify, push_order_first_time_and_repeat)
{
    TraderAdapter trader;
    buildReady(trader);
    trader.onLoginResult(true, "ok", 20240510);
    trader.onRspPosition(makeArray({
        makePosItem(ct_, WDT_LONG, 10, 10, 4, 4),
        makePosItem(ct_, WDT_SHORT, 6, 6, 0, 0) }));
    EXPECT_DOUBLE_EQ(14.0, trader.getPosition(kStdCode, true, 1));

    // 首笔: 平多 8 手 -> 先扣昨仓可用 10-8=2
    trader.onPushOrder(makePushOrder(ct_, WDT_LONG, WOT_CLOSE, WOS_Submitting,
        8, 0, "PO1", kTag1001));
    EXPECT_DOUBLE_EQ(6.0, trader.getPosition(kStdCode, true, 1));     // 2 + 4
    EXPECT_DOUBLE_EQ(-6.0, trader.getPosition(kStdCode, true, 2));    // 空头未动
    ASSERT_EQ(1u, sink_.orders_.size());
    EXPECT_EQ(1001u, sink_.orders_[0].localid);
    EXPECT_STREQ(kStdCode, sink_.orders_[0].code.c_str());
    EXPECT_FALSE(sink_.orders_[0].isBuy);          // 平多 = 卖动作
    EXPECT_DOUBLE_EQ(8.0, sink_.orders_[0].totalQty);
    EXPECT_DOUBLE_EQ(8.0, sink_.orders_[0].leftQty);
    EXPECT_FALSE(sink_.orders_[0].canceled);
    // 挂单推送不动未完成量, 只有撤销回报才回补
    EXPECT_DOUBLE_EQ(0.0, trader.getUndoneQty(kStdCode));

    OrderMap* all = trader.getOrders("");
    ASSERT_NE(nullptr, all);
    EXPECT_EQ(1u, all->size());
    all->release();

    // 完全相同的重复推送: 订单号已在 _orderids 里且状态不是已撤销 -> 只分发, 不再改数量
    trader.onPushOrder(makePushOrder(ct_, WDT_LONG, WOT_CLOSE, WOS_Submitting,
        8, 0, "PO1", kTag1001));
    EXPECT_DOUBLE_EQ(6.0, trader.getPosition(kStdCode, true, 1));
    EXPECT_EQ(2u, sink_.orders_.size());

    // 部分成交: 仍存活, 替换订单表里的同一 localid
    trader.onPushOrder(makePushOrder(ct_, WDT_LONG, WOT_CLOSE, WOS_PartTraded_Queuing,
        8, 3, "PO1", kTag1001));
    EXPECT_DOUBLE_EQ(6.0, trader.getPosition(kStdCode, true, 1));
    EXPECT_EQ(3u, sink_.orders_.size());
    EXPECT_DOUBLE_EQ(5.0, sink_.orders_[2].leftQty);
    EXPECT_FALSE(sink_.orders_[2].canceled);

    // 撤销: 按 8-3=5 回补可平量, 同时回补未完成量
    trader.onPushOrder(makePushOrder(ct_, WDT_LONG, WOT_CLOSE, WOS_Canceled,
        8, 3, "PO1", kTag1001));
    EXPECT_DOUBLE_EQ(11.0, trader.getPosition(kStdCode, true, 1));   // 6 + 5, 未超昨仓总量
    EXPECT_DOUBLE_EQ(5.0, trader.getUndoneQty(kStdCode));            // 卖单撤销 => +5
    ASSERT_EQ(4u, sink_.orders_.size());
    EXPECT_TRUE(sink_.orders_[3].canceled);
    EXPECT_DOUBLE_EQ(5.0, sink_.orders_[3].leftQty);

    // 单子已经不存活 -> 从订单表摘掉
    OrderMap* after = trader.getOrders("");
    ASSERT_NE(nullptr, after);
    EXPECT_EQ(0u, after->size());
    after->release();

    // 迟到的「全部成交」推送: 订单号已知, 既不重复预占也不回补
    trader.onPushOrder(makePushOrder(ct_, WDT_LONG, WOT_CLOSE, WOS_AllTraded,
        8, 8, "PO1", kTag1001));
    EXPECT_DOUBLE_EQ(11.0, trader.getPosition(kStdCode, true, 1));
    EXPECT_DOUBLE_EQ(5.0, trader.getUndoneQty(kStdCode));
    EXPECT_EQ(5u, sink_.orders_.size());
    EXPECT_EQ(1001u, sink_.orders_[4].localid);
}

TEST_F(P11PushNotify, push_order_claims_avail_across_new_and_old)
{
    TraderAdapter trader;
    buildReady(trader);
    trader.onLoginResult(true, "ok", 20240510);
    trader.onRspPosition(makeArray({
        makePosItem(ct_, WDT_LONG, 3, 1, 4, 2),
        makePosItem(ct_, WDT_SHORT, 2, 2, 0, 0) }));

    // 平多 5 手: 昨可用 1 先扣, 再扣今可用 2, 合计截到 0（下限截断分支）
    trader.onPushOrder(makePushOrder(ct_, WDT_LONG, WOT_CLOSE, WOS_Submitting,
        5, 0, "PO2", kTag1002));
    EXPECT_DOUBLE_EQ(0.0, trader.getPosition(kStdCode, true, 1));

    // 同一单撤销 4 手(已成交 1): 昨仓可用封顶 3, 溢出 1 转到今仓
    trader.onPushOrder(makePushOrder(ct_, WDT_LONG, WOT_CLOSE, WOS_Canceled,
        5, 1, "PO2", kTag1002));
    EXPECT_DOUBLE_EQ(4.0, trader.getPosition(kStdCode, true, 1));    // 昨3 + 今1

    // 平今: 只动今仓可用 1 -> 0
    trader.onPushOrder(makePushOrder(ct_, WDT_LONG, WOT_CLOSETODAY, WOS_Submitting,
        2, 0, "PO3", kTag1002));
    EXPECT_DOUBLE_EQ(3.0, trader.getPosition(kStdCode, true, 1));    // 只剩昨仓可用

    // 平今撤销(未成交): 今仓可用 +2
    trader.onPushOrder(makePushOrder(ct_, WDT_LONG, WOT_CLOSETODAY, WOS_Canceled,
        2, 0, "PO3", kTag1002));
    EXPECT_DOUBLE_EQ(5.0, trader.getPosition(kStdCode, true, 1));

    // 平空(买动作): 只动空头可用 2 -> 0
    trader.onPushOrder(makePushOrder(ct_, WDT_SHORT, WOT_CLOSE, WOS_Submitting,
        2, 0, "PO4", kTag1003));
    EXPECT_DOUBLE_EQ(0.0, trader.getPosition(kStdCode, true, 2));
    EXPECT_DOUBLE_EQ(5.0, trader.getPosition(kStdCode, true, 1));    // 多头不受影响

    // 空头平今: 今仓可用本来就是 0, 截断后仍为 0
    trader.onPushOrder(makePushOrder(ct_, WDT_SHORT, WOT_CLOSETODAY, WOS_Submitting,
        3, 0, "PO6", kTag1003));
    EXPECT_DOUBLE_EQ(0.0, trader.getPosition(kStdCode, true, 2));
    EXPECT_EQ(TraderAdapter::AS_POSITION_QRYED, trader.state());
}

TEST_F(P11PushNotify, external_orders_do_not_touch_undone)
{
    TraderAdapter trader;
    buildReady(trader);
    trader.onLoginResult(true, "ok", 20240510);
    trader.onRspPosition(makeArray({ makePosItem(ct_, WDT_LONG, 8, 8, 0, 0) }));

    // 外部单(usertag 不带 otp.p11a. 前缀): 预占可平量但不入订单表、不更新未完成量、不通知订阅者
    trader.onPushOrder(makePushOrder(ct_, WDT_LONG, WOT_CLOSE, WOS_Submitting,
        3, 0, "PX1", ""));
    EXPECT_DOUBLE_EQ(5.0, trader.getPosition(kStdCode, true, 1));
    EXPECT_DOUBLE_EQ(0.0, trader.getUndoneQty(kStdCode));
    EXPECT_EQ(0u, sink_.orders_.size());
    // 本系统没有单进过订单表 => _orders 尚未建立
    EXPECT_EQ(nullptr, trader.getOrders(""));

    // 同一外部单撤销: 可平量回补, 仍未分发
    trader.onPushOrder(makePushOrder(ct_, WDT_LONG, WOT_CLOSE, WOS_Canceled,
        3, 0, "PX1", ""));
    EXPECT_DOUBLE_EQ(8.0, trader.getPosition(kStdCode, true, 1));
    EXPECT_EQ(0u, sink_.orders_.size());

    // 错单(WOEF_Normal) 与撤单在统计里区分开, 但撤销分支仍会回补未完成量
    WTSOrderInfo* wrong = makePushOrder(ct_, WDT_LONG, WOT_OPEN, WOS_Canceled,
        2, 0, "PX2", kTag1001);
    wrong->retain();
    wrong->setError(WOEF_Normal);
    trader.onPushOrder(wrong);
    EXPECT_TRUE(wrong->isError());
    EXPECT_FALSE(wrong->isSelfTrade());
    EXPECT_DOUBLE_EQ(-2.0, trader.getUndoneQty(kStdCode));
    EXPECT_DOUBLE_EQ(0.0, trader.getPosition(kStdCode, false, 2));
    // 实测行为：_orders 还不存在时, 死单会走 else 分支被塞进订单表
    // （与 _orders 已存在时「移除死单」的分支不一致, 见 bugs/p11_wtcore_traderadapter.md）
    OrderMap* after = trader.getOrders("");
    ASSERT_NE(nullptr, after);
    EXPECT_EQ(1u, after->size());
    EXPECT_EQ(wrong, after->get(1001));
    after->release();
}

TEST_F(P11PushNotify, push_trade_updates_position_and_undone)
{
    TraderAdapter trader;
    buildReady(trader);
    trader.onLoginResult(true, "ok", 20240510);
    trader.onRspPosition(makeArray({
        makePosItem(ct_, WDT_LONG, 6, 6, 0, 0),
        makePosItem(ct_, WDT_SHORT, 2, 2, 0, 0) }));

    // 开多 4: 今仓 +4, 非T1品种今仓可用同步 +4
    trader.onPushTrade(makePushTrade(ct_, WDT_LONG, WOT_OPEN, 4, 7100, "PT1", "O1", kTag1001));
    EXPECT_DOUBLE_EQ(10.0, trader.getPosition(kStdCode, false, 1));
    EXPECT_DOUBLE_EQ(10.0, trader.getPosition(kStdCode, true, 1));
    EXPECT_EQ(1u, api_.n_qaccount_);            // 成交回报后固定查一次资金
    ASSERT_EQ(1u, sink_.trades_.size());
    EXPECT_EQ(1001u, sink_.trades_[0].localid);
    EXPECT_TRUE(sink_.trades_[0].isBuy);        // 开多 = 买
    EXPECT_DOUBLE_EQ(4.0, sink_.trades_[0].vol);
    EXPECT_DOUBLE_EQ(7100.0, sink_.trades_[0].price);

    // 平多 3: 先吃昨仓
    trader.onPushTrade(makePushTrade(ct_, WDT_LONG, WOT_CLOSE, 3, 7110, "PT2", "O2", kTag1002));
    EXPECT_DOUBLE_EQ(7.0, trader.getPosition(kStdCode, false, 1));

    // 平今 2: 只减今仓
    trader.onPushTrade(makePushTrade(ct_, WDT_LONG, WOT_CLOSETODAY, 2, 7120, "PT3", "O3", kTag1003));
    EXPECT_DOUBLE_EQ(5.0, trader.getPosition(kStdCode, false, 1));

    // 开空 3
    trader.onPushTrade(makePushTrade(ct_, WDT_SHORT, WOT_OPEN, 3, 7090, "PT4", "O4", kTag1002));
    EXPECT_DOUBLE_EQ(-3.0, trader.getPosition(kStdCode, false, 2));

    // 平空 5: 昨空 2 先出, 余下 3 出今空
    trader.onPushTrade(makePushTrade(ct_, WDT_SHORT, WOT_CLOSE, 5, 7080, "PT5", "O5", kTag1003));
    EXPECT_DOUBLE_EQ(0.0, trader.getPosition(kStdCode, false, 2));

    // 未完成量: -4 +3 +2 +3 -5 = -1
    EXPECT_DOUBLE_EQ(-1.0, trader.getUndoneQty(kStdCode));

    // 外部成交(usertag 为空)不改未完成量, 但仍然分发 (localid=0)
    trader.onPushTrade(makePushTrade(ct_, WDT_LONG, WOT_OPEN, 1, 7000, "PT6", "O6", ""));
    EXPECT_DOUBLE_EQ(-1.0, trader.getUndoneQty(kStdCode));
    ASSERT_EQ(7u, sink_.trades_.size());
    EXPECT_EQ(0u, sink_.trades_[6].localid);
    EXPECT_EQ(2u, api_.n_qaccount_);
}

TEST_F(P11PushNotify, push_trade_net_mode)
{
    TraderAdapter trader;
    buildReady(trader);
    trader.onLoginResult(true, "ok", 20240510);
    trader.onRspPosition(makeArray({
        makePosItem(ct_, WDT_LONG, 5, 5, 0, 0),
        makePosItem(ct_, WDT_SHORT, 0, 0, 0, 0) }));

    // 净头寸卖出 1: 先平昨多 1
    WTSTradeInfo* t1 = makePushTrade(ct_, WDT_LONG, WOT_OPEN, 1, 7100, "PN1", "O1", kTag1001);
    t1->setNetDirection(false);
    trader.onPushTrade(t1);
    EXPECT_DOUBLE_EQ(4.0, trader.getPosition(kStdCode, false, 3));
    ASSERT_EQ(1u, sink_.trades_.size());
    EXPECT_FALSE(sink_.trades_[0].isBuy);

    // 净头寸买入 6: 无空仓可平, 全部开多; 可用量被强制等于总量
    WTSTradeInfo* t2 = makePushTrade(ct_, WDT_LONG, WOT_OPEN, 6, 7100, "PN2", "O2", kTag1002);
    t2->setNetDirection(true);
    trader.onPushTrade(t2);
    EXPECT_DOUBLE_EQ(10.0, trader.getPosition(kStdCode, false, 3));   // 昨4 + 今6
    EXPECT_DOUBLE_EQ(10.0, trader.getPosition(kStdCode, true, 3));    // 净模式下可用=总量
    EXPECT_TRUE(t2->isNet());
    EXPECT_TRUE(t2->isNetBuy());

    // 未完成量: +1 - 6 = -5
    EXPECT_DOUBLE_EQ(-5.0, trader.getUndoneQty(kStdCode));
    EXPECT_EQ(2u, sink_.trades_.size());
    EXPECT_TRUE(sink_.trades_[1].isBuy);
}

TEST_F(P11PushNotify, self_match_via_push_trade)
{
    TraderAdapter trader;
    buildReady(trader);
    trader.onLoginResult(true, "ok", 20240510);

    trader.onPushTrade(makePushTrade(ct_, WDT_LONG, WOT_OPEN, 1, 7100, "PS1", "OA", kTag1001));
    EXPECT_FALSE(trader.isSelfMatched(kStdCode));

    // 重复推送: 同成交号同关联订单 -> 放过
    trader.onPushTrade(makePushTrade(ct_, WDT_LONG, WOT_OPEN, 1, 7100, "PS1", "OA", kTag1001));
    EXPECT_FALSE(trader.isSelfMatched(kStdCode));

    // 同成交号不同关联订单 -> 自成交, 拉黑该合约
    trader.onPushTrade(makePushTrade(ct_, WDT_SHORT, WOT_OPEN, 1, 7100, "PS1", "OB", kTag1002));
    EXPECT_TRUE(trader.isSelfMatched(kStdCode));

    // 其他合约不受影响
    EXPECT_FALSE(trader.isSelfMatched(kStdAu));
    EXPECT_EQ(3u, sink_.trades_.size());
    EXPECT_EQ(3u, api_.n_qaccount_);
}

TEST_F(P11PushNotify, entrust_error_updates_undone_once)
{
    TraderAdapter trader;
    buildReady(trader);
    trader.onLoginResult(true, "ok", 20240510);

    // 用订单查询回报把未完成量做成 +5
    trader.onRspOrders(makeArray({
        makePushOrder(ct_, WDT_LONG, WOT_OPEN, WOS_Submitting, 5, 0, "PE1", kTag1001) }));
    EXPECT_DOUBLE_EQ(5.0, trader.getUndoneQty(kStdCode));

    WTSError* err = WTSError::create(WEC_ORDERINSERT, "reject by counter");
    WTSError* none = WTSError::create(WEC_NONE, "no error");
    WTSEntrust* ent = WTSEntrust::create(kStdCode, 5, 7100.0);
    ent->setContractInfo(ct_);
    ent->setDirection(WDT_LONG);
    ent->setOffsetType(WOT_OPEN);
    ent->setUserTag(kTag1001);

    // 无错误码 -> 什么都不做
    trader.onRspEntrust(ent, none);
    EXPECT_DOUBLE_EQ(5.0, trader.getUndoneQty(kStdCode));
    EXPECT_EQ(0u, sink_.entrusts_.size());

    // 错单回报: 未完成量回落, 并把失败原因发给订阅者
    trader.onRspEntrust(ent, err);
    EXPECT_DOUBLE_EQ(0.0, trader.getUndoneQty(kStdCode));
    ASSERT_EQ(1u, sink_.entrusts_.size());
    EXPECT_EQ(1001u, sink_.entrusts_[0].localid);
    EXPECT_STREQ(kStdCode, sink_.entrusts_[0].code.c_str());
    EXPECT_FALSE(sink_.entrusts_[0].ok);
    EXPECT_STREQ("reject by counter", sink_.entrusts_[0].msg.c_str());

    // 重复的错单回报: 未完成量已为 0, 直接返回, 不再二次扣减也不二次通知
    trader.onRspEntrust(ent, err);
    EXPECT_DOUBLE_EQ(0.0, trader.getUndoneQty(kStdCode));
    EXPECT_EQ(1u, sink_.entrusts_.size());

    // 平仓方向错单: 未完成量正向回落
    trader.onRspOrders(makeArray({
        makePushOrder(ct_, WDT_LONG, WOT_CLOSE, WOS_Submitting, 3, 0, "PE2", kTag1002) }));
    EXPECT_DOUBLE_EQ(-3.0, trader.getUndoneQty(kStdCode));
    WTSEntrust* ent2 = WTSEntrust::create(kStdCode, 3, 7100.0);
    ent2->setContractInfo(ct_);
    ent2->setDirection(WDT_LONG);
    ent2->setOffsetType(WOT_CLOSE);
    ent2->setUserTag(kTag1002);
    trader.onRspEntrust(ent2, err);
    EXPECT_DOUBLE_EQ(0.0, trader.getUndoneQty(kStdCode));
    ASSERT_EQ(2u, sink_.entrusts_.size());
    EXPECT_EQ(1002u, sink_.entrusts_[1].localid);

    // 外部委托(usertag 为空)只回落未完成量, 不通知订阅者
    trader.onRspOrders(makeArray({
        makePushOrder(ct_, WDT_LONG, WOT_OPEN, WOS_Submitting, 2, 0, "PE3", kTag1003) }));
    EXPECT_DOUBLE_EQ(2.0, trader.getUndoneQty(kStdCode));
    WTSEntrust* ent3 = WTSEntrust::create(kStdCode, 2, 7100.0);
    ent3->setContractInfo(ct_);
    ent3->setUserTag("");
    trader.onRspEntrust(ent3, err);
    EXPECT_DOUBLE_EQ(0.0, trader.getUndoneQty(kStdCode));
    EXPECT_EQ(2u, sink_.entrusts_.size());
}

} // namespace
