/**
 * r59_trader_fixture.h — lane R59 公用夹具（在 p11 替身之上补三件事）
 *
 *   1. R59FakeTraderApi: 在 p11fix::FakeTraderApi 之上记录每一笔 orderInsert 的
 *      direction/offset/volume，用于断言 buy/sell 的拆单与开平标志序列。
 *   2. R59TraderTest: 给 ag/au 两张合约挂上固定交易时段 09:00-15:00，并把
 *      WtHelper 的进程内时间钉在 20240510 10:00，使 TraderAdapter::buy/sell
 *      的 isInTradingTime 判断完全确定（不读墙钟）。
 *   3. makeOrder/makeTrade/makePos/loadPolicy/seedPos/makeReady 小工具。
 *
 * 只读复用 p11_trader_fixture.h（FakeTraderApi/FakeBaseDataMgr/RecordingSink），
 * 不改动该文件任何内容。
 */
#pragma once

#include "p11_trader_fixture.h"

#include <WtCore/ActionPolicyMgr.h>
#include <Includes/WTSSessionInfo.hpp>

#include <fstream>

namespace r59x {

inline const char* kStdCode = "SHFE.ag.2512";
inline const char* kStdAu = "SHFE.au.2212";
inline const char* kRawCode = "ag2512";
inline const char* kExchg = "SHFE";

/// 记录每次 orderInsert 的方向/开平/手数，基类只保留最后一笔
class R59FakeTraderApi : public p11fix::FakeTraderApi
{
public:
    std::vector<WTSDirectionType> dirs_;
    std::vector<WTSOffsetType> offsets_;
    std::vector<double> vols_;

    virtual ~R59FakeTraderApi() {}

    virtual int orderInsert(WTSEntrust* entrust) override
    {
        int ret = p11fix::FakeTraderApi::orderInsert(entrust);
        if (ret >= 0)
        {
            dirs_.push_back(entrust->getDirection());
            offsets_.push_back(entrust->getOffsetType());
            vols_.push_back(entrust->getVolume());
        }
        return ret;
    }
};

inline WTSOrderInfo* makeOrder(WTSContractInfo* ct, WTSDirectionType dir, WTSOffsetType off,
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
    o->setEntrustID("R59E001");
    o->setUserTag(utag);
    o->setOrderDate(20240510);
    o->setOrderTime(100000);
    o->setStateMsg("ok");
    return o;
}

inline WTSTradeInfo* makeTrade(WTSContractInfo* ct, WTSDirectionType dir, WTSOffsetType off,
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
    t->setTradeTime(100010);
    t->setAmount(vol * price);
    return t;
}

inline WTSPositionItem* makePos(WTSContractInfo* ct, WTSDirectionType dir,
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
    p->setAvgPrice(7000.0);
    return p;
}

template <typename T>
inline WTSArray* makeArray(std::initializer_list<T*> items)
{
    WTSArray* arr = WTSArray::create();
    for (auto* it : items)
        arr->append(it);
    return arr;
}

inline void seedPos(wtp::TraderAdapter& trader, std::initializer_list<WTSPositionItem*> items)
{
    trader.onRspPosition(makeArray(items));
}

} // namespace r59x

/// 夹具：固定交易时段 + 固定进程内时间 + 策略文件落盘
class R59TraderTest : public P11TraderTest
{
protected:
    r59x::R59FakeTraderApi api2_;

    void SetUp() override
    {
        P11TraderTest::SetUp();

        // 09:00-15:00，buy/sell 里的 isInTradingTime(WtHelper::getTime(), true) 恒为真
        wtp::WTSSessionInfo* sess = wtp::WTSSessionInfo::create("TRADING", "r59_day");
        sess->addTradingSection(900, 1500);
        comm_->setSessionInfo(sess);
        ct_->getCommInfo()->setSessionInfo(sess);
        ct2_->getCommInfo()->setSessionInfo(sess);
        WtHelper::setTime(20240510, 1000, 0);
    }

    /// 写一份动作规则配置并装载到 pm
    bool loadPolicy(wtp::ActionPolicyMgr& pm, const char* fname, const char* json)
    {
        auto path = tmp_ / fname;
        std::ofstream out(path);
        out << json;
        out.close();
        return pm.init(path.string().c_str());
    }

    /// initExt + 订阅 + run + 初始化交易日（不读墙钟）
    void build(wtp::TraderAdapter& trader, wtp::ActionPolicyMgr* pm)
    {
        ASSERT_TRUE(trader.initExt(id_.c_str(), &api2_, &bd_, pm));
        trader.addSink(&sink_);
        ASSERT_TRUE(trader.run());
        trader.onLoginResult(true, "r59", 20240510);
    }

    /// 用空回报数组把状态机推到 AS_ALLREADY（用于 setTrader/isReady）
    void makeReady(wtp::TraderAdapter& trader)
    {
        trader.onRspPosition(static_cast<const WTSArray*>(nullptr));
        trader.onRspOrders(static_cast<const WTSArray*>(nullptr));
        trader.onRspTrades(static_cast<const WTSArray*>(nullptr));
        trader.onRspAccount(static_cast<WTSArray*>(nullptr));
        ASSERT_EQ(wtp::TraderAdapter::AS_ALLREADY, trader.state());
    }
};
