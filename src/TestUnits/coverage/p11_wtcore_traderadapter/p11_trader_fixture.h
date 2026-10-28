/**
 * p11_trader_fixture.h — lane P11 公用测试替身
 *
 * 为 src/WtCore/TraderAdapter.cpp 的「交易回报处理链」用例提供:
 *   - FakeTraderApi   : ITraderApi 的进程内替身(不接任何真实柜台), 记录被调次数与最近一次委托内容
 *   - RecordingSink   : ITrdNotifySink 订阅者, 记录 on_trade/on_order/on_position/on_entrust/on_account/on_channel_*
 *   - FakeBaseDataMgr : 只登记 SHFE.ag.2512 一张合约表
 *   - P11TraderTest   : gtest 夹具, 落盘只写 temp_directory_path()/p11_*，析构清理
 *
 * 注意(BUG-L00-004): TraderAdapter.h 里 TraderAdapterMgr 用了 boost::noncopyable 却没 include,
 * 所以本文件必须先显式包含 <boost/core/noncopyable.hpp> 再包含 TraderAdapter.h。
 */
#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include <unistd.h>

#include <boost/core/noncopyable.hpp>

#include <WtCore/ITrdNotifySink.h>
#include <WtCore/TraderAdapter.h>
#include <WtCore/WtHelper.h>
#include <Includes/IBaseDataMgr.h>
#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSTradeDef.hpp>
#include <Includes/WTSRiskDef.hpp>
#include <Includes/WTSCollection.hpp>
#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>

USING_NS_WTP;

namespace p11fix {

/// 交易通道替身：只做计数与最后一次委托内容的快照，不起线程、不联网
class FakeTraderApi : public ITraderApi
{
public:
    bool init_ret_ = true;
    int insert_ret_ = 0;          ///< <0 时模拟 orderInsert 提交失败
    int action_ret_ = 0;          ///< <0 时模拟 orderAction 提交失败

    uint32_t n_init_ = 0;
    uint32_t n_release_ = 0;
    uint32_t n_register_ = 0;
    uint32_t n_connect_ = 0;
    uint32_t n_disconnect_ = 0;
    uint32_t n_login_ = 0;
    uint32_t n_logout_ = 0;
    uint32_t n_make_eid_ = 0;
    uint32_t n_insert_ = 0;
    uint32_t n_action_ = 0;
    uint32_t n_qaccount_ = 0;
    uint32_t n_qpositions_ = 0;
    uint32_t n_qorders_ = 0;
    uint32_t n_qtrades_ = 0;

    std::string last_code_;       ///< orderInsert 收到的（已被改写为交易所原始代码的）合约代码
    std::string last_exchg_;
    std::string last_usertag_;
    std::string last_entrust_id_;
    std::string last_action_code_;
    double last_price_ = 0;
    double last_vol_ = 0;
    WTSDirectionType last_dir_ = WDT_LONG;
    WTSOffsetType last_offset_ = WOT_OPEN;
    WTSPriceType last_pxtype_ = WPT_ANYPRICE;
    WTSOrderFlag last_flag_ = WOF_NOR;

    virtual ~FakeTraderApi() {}

    virtual bool init(WTSVariant* params) override { (void)params; ++n_init_; return init_ret_; }
    virtual void release() override { ++n_release_; }
    virtual void registerSpi(ITraderSpi* listener) override { (void)listener; ++n_register_; }
    virtual void connect() override { ++n_connect_; }
    virtual void disconnect() override { ++n_disconnect_; }
    virtual bool isConnected() override { return true; }

    virtual bool makeEntrustID(char* buffer, int length) override
    {
        ++n_make_eid_;
        snprintf(buffer, (size_t)length, "P11E%03u", n_make_eid_);
        return true;
    }

    virtual int login(const char* user, const char* pass, const char* productInfo) override
    {
        (void)user; (void)pass; (void)productInfo;
        ++n_login_;
        return 0;
    }

    virtual int logout() override { ++n_logout_; return 0; }

    virtual int orderInsert(WTSEntrust* entrust) override
    {
        ++n_insert_;
        if (insert_ret_ < 0)
            return insert_ret_;

        last_code_ = entrust->getCode();
        last_exchg_ = entrust->getExchg();
        last_usertag_ = entrust->getUserTag();
        last_entrust_id_ = entrust->getEntrustID();
        last_price_ = entrust->getPrice();
        last_vol_ = entrust->getVolume();
        last_dir_ = entrust->getDirection();
        last_offset_ = entrust->getOffsetType();
        last_pxtype_ = entrust->getPriceType();
        last_flag_ = entrust->getOrderFlag();
        return 0;
    }

    virtual int orderAction(WTSEntrustAction* action) override
    {
        ++n_action_;
        if (action_ret_ < 0)
            return action_ret_;
        last_action_code_ = action->getCode();
        return 0;
    }

    virtual int queryAccount() override { ++n_qaccount_; return 0; }
    virtual int queryPositions() override { ++n_qpositions_; return 0; }
    virtual int queryOrders() override { ++n_qorders_; return 0; }
    virtual int queryTrades() override { ++n_qtrades_; return 0; }
    virtual int querySettlement(uint32_t uDate) override { (void)uDate; return 0; }
};

/// 回报订阅者：把 ITrdNotifySink 的每一次回调原样存下来
class RecordingSink : public ITrdNotifySink
{
public:
    struct TradeRec { uint32_t localid; std::string code; bool isBuy; double vol; double price; };
    struct OrderRec { uint32_t localid; std::string code; bool isBuy; double totalQty; double leftQty; double price; bool canceled; };
    struct PosRec { std::string code; bool isLong; double prevol; double preavail; double newvol; double newavail; uint32_t tdate; };
    struct EntrustRec { uint32_t localid; std::string code; bool ok; std::string msg; };
    struct AcctRec { std::string currency; double prebal; double bal; double dynbal; double avail; double closeprofit; double dynprofit; double margin; double fee; double deposit; double withdraw; };

    uint32_t n_ready_ = 0;
    uint32_t n_lost_ = 0;
    std::vector<TradeRec> trades_;
    std::vector<OrderRec> orders_;
    std::vector<PosRec> poss_;
    std::vector<EntrustRec> entrusts_;
    std::vector<AcctRec> accts_;

    virtual ~RecordingSink() {}

    virtual void on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price) override
    {
        trades_.push_back({ localid, stdCode, isBuy, vol, price });
    }

    virtual void on_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty,
        double leftQty, double price, bool isCanceled = false) override
    {
        orders_.push_back({ localid, stdCode, isBuy, totalQty, leftQty, price, isCanceled });
    }

    virtual void on_position(const char* stdCode, bool isLong, double prevol, double preavail,
        double newvol, double newavail, uint32_t tradingday) override
    {
        poss_.push_back({ stdCode, isLong, prevol, preavail, newvol, newavail, tradingday });
    }

    virtual void on_channel_ready() override { ++n_ready_; }
    virtual void on_channel_lost() override { ++n_lost_; }

    virtual void on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message) override
    {
        entrusts_.push_back({ localid, stdCode, bSuccess, message });
    }

    virtual void on_account(const char* currency, double prebalance, double balance, double dynbalance,
        double avaliable, double closeprofit, double dynprofit, double margin, double fee,
        double deposit, double withdraw) override
    {
        accts_.push_back({ currency, prebalance, balance, dynbalance, avaliable, closeprofit,
            dynprofit, margin, fee, deposit, withdraw });
    }
};

/// 最小基础数据替身：只登记一张分月合约
class FakeBaseDataMgr : public IBaseDataMgr
{
public:
    virtual ~FakeBaseDataMgr() {}

    void regis(WTSCommodityInfo* comm, WTSContractInfo* ct)
    {
        ct->setCommInfo(comm);
        _comms.push_back(comm);
        _cts.push_back(ct);
        _ct_key[std::string(ct->getCode()) + "|" + ct->getExchg()] = ct;
        _comm_key[std::string(comm->getProduct()) + "|" + comm->getExchg()] = comm;
    }

    virtual WTSCommodityInfo* getCommodity(const char* exchgpid) override
    {
        for (auto* c : _comms)
        {
            if (strcmp(c->getFullPid(), exchgpid) == 0)
                return c;
        }
        return NULL;
    }

    virtual WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override
    {
        auto it = _comm_key.find(std::string(pid) + "|" + exchg);
        return it == _comm_key.end() ? NULL : it->second;
    }

    virtual WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t uDate = 0) override
    {
        (void)uDate;
        auto it = _ct_key.find(std::string(code) + "|" + exchg);
        return it == _ct_key.end() ? NULL : it->second;
    }

    virtual WTSArray* getContracts(const char* exchg = "", uint32_t uDate = 0) override
    { (void)exchg; (void)uDate; return NULL; }

    virtual WTSSessionInfo* getSession(const char* sid) override { (void)sid; return NULL; }
    virtual WTSSessionInfo* getSessionByCode(const char* code, const char* exchg = "") override
    { (void)code; (void)exchg; return NULL; }
    virtual WTSArray* getAllSessions() override { return NULL; }
    virtual bool isHoliday(const char* pid, uint32_t uDate, bool isTpl = false) override
    { (void)pid; (void)uDate; (void)isTpl; return false; }
    virtual uint32_t calcTradingDate(const char* stdPID, uint32_t uDate, uint32_t uTime, bool isSession = false) override
    { (void)stdPID; (void)uTime; (void)isSession; return uDate; }
    virtual uint64_t getBoundaryTime(const char* stdPID, uint32_t tDate, bool isSession = false, bool isStart = true) override
    { (void)stdPID; (void)isSession; (void)isStart; return tDate; }

private:
    std::vector<WTSCommodityInfo*> _comms;
    std::vector<WTSContractInfo*> _cts;
    wt_hashmap<std::string, WTSContractInfo*> _ct_key;
    wt_hashmap<std::string, WTSCommodityInfo*> _comm_key;
};

/// 标准化合约: SHFE.ag.2512 <-> 原始分月代码 ag2512（CodeHelper::rawMonthCodeToStdCode 的产物）
static const char* kStdCode = "SHFE.ag.2512";
static const char* kRawCode = "ag2512";
static const char* kExchg = "SHFE";

} // namespace p11fix

/// 夹具：临时目录 + 替身装配 + 落盘清理
class P11TraderTest : public ::testing::Test
{
protected:
    p11fix::FakeTraderApi api_;
    p11fix::FakeBaseDataMgr bd_;
    p11fix::RecordingSink sink_;
    wtp::WTSCommodityInfo* comm_ = NULL;
    wtp::WTSContractInfo* ct_ = NULL;
    wtp::WTSContractInfo* ct2_ = NULL;
    std::filesystem::path tmp_;
    std::string id_ = "p11a";

    void SetUp() override
    {
        namespace fs = std::filesystem;
        std::string tag = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        for (auto& ch : tag)
            if (!(std::isalnum((unsigned char)ch) || ch == '_'))
                ch = '_';
        tmp_ = fs::temp_directory_path() / fs::path("p11_" + tag + "_" + std::to_string((long)::getpid()));
        std::error_code ec;
        fs::create_directories(tmp_, ec);

        comm_ = wtp::WTSCommodityInfo::create("ag", "沪银", p11fix::kExchg, "TRADING", "CHINA");
        comm_->setCategory(wtp::CC_Future);
        comm_->setCoverMode(wtp::CM_CoverToday);
        comm_->setTradingMode(wtp::TM_Both);       // 非T1，开仓即可用
        comm_->setVolScale(1);
        comm_->setPriceTick(1.0);

        ct_ = wtp::WTSContractInfo::create(p11fix::kRawCode, "ag2512", p11fix::kExchg, "ag");
        ct_->setAltCode(p11fix::kRawCode);
        ct_->setVolumeLimits(1000, 2000);
        bd_.regis(comm_, ct_);

        // 第二个品种，用于验证「未登记合约 / 多合约」分支
        wtp::WTSCommodityInfo* comm2 = wtp::WTSCommodityInfo::create("au", "沪金", p11fix::kExchg, "TRADING", "CHINA");
        comm2->setCategory(wtp::CC_Future);
        comm2->setCoverMode(wtp::CM_OpenCover);
        comm2->setTradingMode(wtp::TM_Long);
        ct2_ = wtp::WTSContractInfo::create("au2212", "au2212", p11fix::kExchg, "au");
        ct2_->setAltCode("au2212");
        ct2_->setVolumeLimits(0, 0);
        bd_.regis(comm2, ct2_);
    }

    void TearDown() override
    {
        // TraderAdapter::initExt 会把 trades.csv/orders.csv/rtdata.json 写在 getBaseDir()/traders/<id> 下，
        // 这里按同一规则清掉，避免在用例进程工作目录里留残留
        std::error_code ec;
        std::string base = WtHelper::getBaseDir();
        std::filesystem::path gen(base);
        gen /= "traders";
        gen /= id_;
        std::filesystem::remove_all(gen, ec);
        std::filesystem::remove_all(tmp_, ec);
    }

    /// initExt 装配出的通道：_trader_api 就绪、_save_data=true、state=AS_NOTLOGIN
    void buildReady(wtp::TraderAdapter& trader)
    {
        ASSERT_TRUE(trader.initExt(id_.c_str(), &api_, &bd_, NULL));
        trader.addSink(&sink_);
        ASSERT_TRUE(trader.run());
        ASSERT_NE(nullptr, trader.getBaseDataMgr());
    }
};
