/**
 * p11_push_persist_test.cpp — 实时推送与落盘链（TraderAdapter.cpp:237-390, 1873-2335）
 *
 * 覆盖只有「回报真的到了」才会走到的实体分支：
 *   initExt -> initSaveData 建 traders/<id>/{trades.csv,orders.csv,rtdata.json} 并写表头
 *   onPushTrade -> logTrade 追加成交行（含 formatAction 的 OL/OS/CL/CS 标记）
 *   onPushOrder -> 订单结束时 logOrder 追加订单行（canceled 标记 + 柜台 stateMsg）
 *   onRspAccount -> saveData 写 rtdata.json（持仓快照 + 资金快照）
 *   第二个适配器打开同一份 csv 时走 seek_to_end 分支, 表头不重复
 * 所有文件都在被测代码自己的落盘根目录下, 用例结束由夹具删除。
 */
#include "p11_trader_fixture.h"

#include <fstream>
#include <sstream>

using namespace p11fix;
using wtp::TraderAdapter;

namespace {

const char* kTag2001 = "otp.p11p.2001";
const char* kTag2002 = "otp.p11p.2002";

std::string readAll(const std::filesystem::path& p)
{
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

WTSOrderInfo* mkOrder(WTSContractInfo* ct, WTSDirectionType dir, WTSOffsetType off,
    WTSOrderState st, double vol, double traded, const char* oid, const char* utag)
{
    WTSOrderInfo* o = WTSOrderInfo::create();
    o->setCode(ct->getCode());
    o->setExchange(ct->getExchg());
    o->setContractInfo(ct);
    o->setDirection(dir);
    o->setOffsetType(off);
    o->setOrderState(st);
    o->setOrderFlag(WOF_NOR);
    o->setPrice(7100.0);
    o->setVolume(vol);
    o->setVolTraded(traded);
    o->setVolLeft(vol - traded);
    o->setOrderID(oid);
    o->setEntrustID("P11E400");
    o->setUserTag(utag);
    o->setOrderDate(20240510);
    o->setOrderTime(93200);
    o->setStateMsg("canceled by hand");
    return o;
}

WTSTradeInfo* mkTrade(WTSContractInfo* ct, WTSDirectionType dir, WTSOffsetType off,
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
    t->setTradeTime(93210);
    t->setAmount(vol * price);
    return t;
}

WTSPositionItem* mkPos(WTSContractInfo* ct, WTSDirectionType dir,
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

class P11PushPersist : public P11TraderTest
{
protected:
    std::filesystem::path chnl_dir_;

    void SetUp() override
    {
        P11TraderTest::SetUp();
        id_ = "p11p";                                     // usertag 前缀 otp.p11p.
        std::string base = WtHelper::getBaseDir();
        std::filesystem::path p(base);
        p /= "traders";
        p /= id_;
        chnl_dir_ = p;
        // 先删干净, 保证走「新文件写表头」分支, 也让断言与历史运行无关
        std::error_code ec;
        std::filesystem::remove_all(chnl_dir_, ec);
    }
};

TEST_F(P11PushPersist, init_ext_creates_csv_headers)
{
    TraderAdapter trader;
    buildReady(trader);

    const std::filesystem::path tradesCsv = chnl_dir_ / "trades.csv";
    const std::filesystem::path ordersCsv = chnl_dir_ / "orders.csv";
    ASSERT_TRUE(std::filesystem::exists(tradesCsv)) << tradesCsv.string();
    ASSERT_TRUE(std::filesystem::exists(ordersCsv)) << ordersCsv.string();
    EXPECT_EQ("localid,date,time,code,action,volume,price,tradeid,orderid\n",
        readAll(tradesCsv));
    EXPECT_EQ("localid,date,inserttime,code,action,volume,traded,price,orderid,canceled,remark\n",
        readAll(ordersCsv));
}

TEST_F(P11PushPersist, trade_and_order_reports_are_appended)
{
    TraderAdapter trader;
    buildReady(trader);
    trader.onLoginResult(true, "ok", 20240510);
    trader.onRspPosition(makeArray({ mkPos(ct_, WDT_LONG, 10, 10, 0, 0) }));

    const std::filesystem::path tradesCsv = chnl_dir_ / "trades.csv";
    const std::filesystem::path ordersCsv = chnl_dir_ / "orders.csv";

    // 开多成交 3 手 -> OL；代码列是标准代码
    trader.onPushTrade(mkTrade(ct_, WDT_LONG, WOT_OPEN, 3, 7100.0, "TT1", "OX1", kTag2001));
    EXPECT_NE(std::string::npos,
        readAll(tradesCsv).find("2001,20240510,93210,SHFE.ag.2512,OL,3,7100,TT1,OX1"));

    // 平多成交 2 手 -> CL
    trader.onPushTrade(mkTrade(ct_, WDT_LONG, WOT_CLOSE, 2, 7200.0, "TT3", "OX5", kTag2001));
    EXPECT_NE(std::string::npos,
        readAll(tradesCsv).find("2001,20240510,93210,SHFE.ag.2512,CL,2,7200,TT3,OX5"));

    // 开空成交 1 手 -> OS
    trader.onPushTrade(mkTrade(ct_, WDT_SHORT, WOT_OPEN, 1, 7000.0, "TT4", "OX6", kTag2002));
    EXPECT_NE(std::string::npos,
        readAll(tradesCsv).find("2002,20240510,93210,SHFE.ag.2512,OS,1,7000,TT4,OX6"));

    // 挂单不写 orders.csv, 结束(撤销)的单才写
    trader.onPushOrder(mkOrder(ct_, WDT_SHORT, WOT_CLOSE, WOS_Submitting, 5, 0, "OX3", kTag2002));
    EXPECT_EQ("localid,date,inserttime,code,action,volume,traded,price,orderid,canceled,remark\n",
        readAll(ordersCsv));

    trader.onPushOrder(mkOrder(ct_, WDT_LONG, WOT_CLOSE, WOS_Canceled, 4, 1, "OX2", kTag2001));
    EXPECT_NE(std::string::npos,
        readAll(ordersCsv).find("2001,20240510,93200,SHFE.ag.2512,CL,4,1,7100,OX2,TRUE,canceled by hand"));

    // 外部成交(usertag 为空)也记入 trades.csv, localid 记 0
    trader.onPushTrade(mkTrade(ct_, WDT_SHORT, WOT_CLOSE, 1, 7050.0, "TT2", "OX4", ""));
    EXPECT_NE(std::string::npos,
        readAll(tradesCsv).find("0,20240510,93210,SHFE.ag.2512,CS,1,7050,TT2,OX4"));

    // 行数核对: 表头 1 行 + 成交 4 行
    std::string trd = readAll(tradesCsv);
    EXPECT_EQ(5u, std::count(trd.begin(), trd.end(), '\n'));
}

TEST_F(P11PushPersist, save_data_writes_position_and_fund_snapshot)
{
    TraderAdapter trader;
    buildReady(trader);
    trader.onLoginResult(true, "ok", 20240510);
    trader.onRspPosition(makeArray({
        mkPos(ct_, WDT_LONG, 3, 3, 2, 2),
        mkPos(ct2_, WDT_SHORT, 4, 1, 2, 0) }));

    const std::filesystem::path rtJson = chnl_dir_ / "rtdata.json";
    // 还没收到资金回报前, saveData 不会被调用
    EXPECT_FALSE(std::filesystem::exists(rtJson));

    WTSAccountInfo* acct = WTSAccountInfo::create();
    acct->setCurrency("CNY");
    acct->setPreBalance(1000.0);
    acct->setBalance(1234.5);
    acct->setDynProfit(20.0);
    acct->setCloseProfit(10.0);
    acct->setMargin(50.0);
    acct->setCommission(2.0);
    acct->setAvailable(900.0);
    acct->setDeposit(0.0);
    acct->setWithdraw(0.0);
    trader.onRspAccount(makeArray({ acct }));

    ASSERT_TRUE(std::filesystem::exists(rtJson)) << rtJson.string();
    std::string json = readAll(rtJson);
    EXPECT_NE(std::string::npos, json.find("\"positions\""));
    EXPECT_NE(std::string::npos, json.find("SHFE.ag.2512"));
    EXPECT_NE(std::string::npos, json.find("SHFE.au.2212"));
    EXPECT_NE(std::string::npos, json.find("\"long\""));
    EXPECT_NE(std::string::npos, json.find("\"short\""));
    EXPECT_NE(std::string::npos, json.find("\"prevol\""));
    EXPECT_NE(std::string::npos, json.find("\"newavail\""));
    EXPECT_NE(std::string::npos, json.find("\"funds\""));
    EXPECT_NE(std::string::npos, json.find("\"CNY\""));
    EXPECT_NE(std::string::npos, json.find("\"prebalance\""));
    EXPECT_NE(std::string::npos, json.find("1234.5"));

    // 空资金数组: saveData 照样落盘, 但不会向订阅者分发任何资金
    trader.onRspAccount(WTSArray::create());
    EXPECT_EQ(1u, sink_.accts_.size());
    EXPECT_FALSE(trader.isReady());      // 状态机还停在 AS_LOGINED
}

TEST_F(P11PushPersist, reopened_csv_keeps_header_and_appends)
{
    {
        TraderAdapter first;
        buildReady(first);
        first.onPushTrade(mkTrade(ct_, WDT_LONG, WOT_CLOSE, 1, 7000.0, "TR1", "OR1", kTag2001));
        first.release();
    }
    const std::filesystem::path tradesCsv = chnl_dir_ / "trades.csv";
    std::string afterFirst = readAll(tradesCsv);
    EXPECT_EQ(2u, std::count(afterFirst.begin(), afterFirst.end(), '\n'));
    EXPECT_NE(std::string::npos,
        afterFirst.find("2001,20240510,93210,SHFE.ag.2512,CL,1,7000,TR1,OR1"));

    // 第二个适配器打开同一份文件: 表头不重复, 新记录追加在后面
    {
        TraderAdapter second;
        ASSERT_TRUE(second.initExt(id_.c_str(), &api_, &bd_, NULL));
        ASSERT_TRUE(second.run());
        second.addSink(&sink_);
        second.onPushTrade(mkTrade(ct_, WDT_SHORT, WOT_OPEN, 2, 7000.0, "TR2", "OR2", kTag2002));
        second.release();
    }
    std::string afterSecond = readAll(tradesCsv);
    EXPECT_EQ(3u, std::count(afterSecond.begin(), afterSecond.end(), '\n'));
    EXPECT_EQ(1u, std::count(afterSecond.begin(), afterSecond.end(), 'l'));   // "localid" 只出现一次
    EXPECT_NE(std::string::npos,
        afterSecond.find("2002,20240510,93210,SHFE.ag.2512,OS,2,7000,TR2,OR2"));
}

TEST_F(P11PushPersist, channel_events_do_not_change_state)
{
    TraderAdapter trader;
    buildReady(trader);
    trader.onLoginResult(true, "ok", 20240510);
    const auto stateBefore = trader.state();

    // 连接失败(非 0 事件码)只写日志, 不改状态、不通知订阅者
    trader.handleEvent(WTE_Connect, -7);
    EXPECT_EQ(stateBefore, trader.state());
    EXPECT_EQ(0u, sink_.n_lost_);
    EXPECT_EQ(0u, sink_.n_ready_);
    EXPECT_EQ(0u, api_.n_login_);

    // 有错误码的错误回报只打日志（_notifier 为空）
    WTSError* err = WTSError::create(WEC_ORDERCANCEL, "cancel rejected");
    err->retain();
    trader.onTraderError(err);
    EXPECT_EQ(stateBefore, trader.state());
    EXPECT_STREQ("cancel rejected", err->getMessage());

    // 空错误回报也不崩
    trader.onTraderError(NULL);
    EXPECT_EQ(stateBefore, trader.state());

    // 通道日志与登出是空/转发动作
    trader.handleTraderLog(LL_ERROR, "raw channel log");
    trader.onLogout();
    EXPECT_EQ(stateBefore, trader.state());

    // 未就绪时 queryFund 不发查询
    trader.queryFund();
    EXPECT_EQ(0u, api_.n_qaccount_);
}

} // namespace
