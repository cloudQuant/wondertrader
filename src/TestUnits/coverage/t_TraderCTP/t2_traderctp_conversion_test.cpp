/*
 * lane T2 — src/TraderCTP/TraderCTP.cpp 覆盖率用例（收发族）
 *
 * 形状（与 t_TraderMocker/seam_tradermocker_test.cpp 同源，但把 seam 换成 protected 派生）：
 *   TraderCTP.h:153 起是 protected 段，所以派生类 CtpProbe 可以合法读写
 *   m_pUserAPI / m_wrapperState / m_queQuery / m_frontID ... 无需 #define private public。
 *   m_pUserAPI 指向 t2_ctp_api_stub.hpp 里的机械空实现（不起线程、不连柜台），
 *   于是 login/orderInsert/orderAction/OnRsp* 这些「必须有 API 指针」的分支能真的走进去。
 *   查询类接口把 handler 压进 m_queQuery（生产里由 worker 线程执行），
 *   用例用 drain_queries() 在**调用线程**上直接执行，因此没有线程、没有 sleep。
 *
 * 厂商回调结构体全部手工构造后直接喂给 OnRsp / OnRtn 系列函数，不触碰任何真实回报链路。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include <unistd.h>

#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSCollection.hpp"
#include "Includes/WTSTradeDef.hpp"
#include "Includes/WTSVariant.hpp"
#include "Share/TimeUtils.hpp"

#include "t2_ctp_api_stub.hpp"

namespace t2ctp {

USING_NS_WTP;

static const char* kExchg = "SHFE";
static const char* kRawCode = "ag2512";

/// 只登记 SHFE/ag2512 一张合约表，其余查询返回 NULL（用于「未知合约」分支）
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
            if (strcmp(c->getFullPid(), exchgpid) == 0)
                return c;
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
    std::vector<WTSCommodityInfo*>    _comms;
    std::vector<WTSContractInfo*>     _cts;
    wt_hashmap<std::string, WTSContractInfo*>   _ct_key;
    wt_hashmap<std::string, WTSCommodityInfo*>  _comm_key;
};

/// 记录型 sink：只抄字段，不持有指针（生产侧回报对象随后 release()）
class RecordingSink : public ITraderSpi
{
public:
    FakeBaseDataMgr           bd_;

    uint32_t                  n_log_ = 0;
    std::string               last_log_;
    std::vector<std::pair<int, int32_t> > n_events_;      ///< (WTSTraderEvent, ec)
    int                       n_login_result_ = 0;
    bool                      last_succ_ = false;
    std::string               last_msg_;
    uint32_t                  last_tradedate_ = 0;

    uint32_t                  n_rsp_entrust_ = 0;
    std::string               entrust_code_, entrust_usertag_, entrust_err_;
    uint32_t                  n_rsp_account_ = 0;
    uint32_t                  account_size_ = 0;
    double                    account_balance_ = 0, account_available_ = 0;
    uint32_t                  n_rsp_position_ = 0, position_size_ = 0;
    double                    pos_avg_ = 0, pos_total_ = 0, pos_avail_new_ = 0;
    uint32_t                  n_rsp_orders_ = 0, orders_size_ = 0;
    uint32_t                  n_rsp_trades_ = 0, trades_size_ = 0;
    uint32_t                  n_settlement_ = 0;
    uint32_t                  settle_date_ = 0;
    std::string               settle_content_;
    uint32_t                  n_push_order_ = 0;
    WTSOrderInfo*             last_order_ = NULL;
    std::string               push_order_id_, push_order_usertag_;
    uint32_t                  push_order_date_ = 0;
    uint32_t                  n_push_trade_ = 0;
    std::string               push_trade_id_, push_trade_usertag_;
    double                    push_amount_ = 0, push_price_ = 0, push_vol_ = 0;
    uint32_t                  push_trade_date_ = 0;
    uint32_t                  n_trader_error_ = 0;
    std::string               trader_error_;
    uint32_t                  n_inst_status_ = 0;
    std::string               inst_exchg_, inst_code_;
    int                       inst_status_ = -1;

    virtual ~RecordingSink() {}

    virtual IBaseDataMgr* getBaseDataMgr() override { return &bd_; }
    virtual void handleTraderLog(WTSLogLevel ll, const char* message) override
    {
        (void)ll;
        ++n_log_;
        last_log_ = message;
    }
    virtual void handleEvent(WTSTraderEvent e, int32_t ec) override { n_events_.push_back(std::make_pair((int)e, ec)); }
    virtual void onLoginResult(bool bSucc, const char* msg, uint32_t tradingdate) override
    {
        ++n_login_result_;
        last_succ_ = bSucc;
        last_msg_ = msg;
        last_tradedate_ = tradingdate;
    }
    virtual void onRspEntrust(WTSEntrust* entrust, WTSError* err) override
    {
        ++n_rsp_entrust_;
        entrust_code_ = entrust->getCode();
        entrust_usertag_ = entrust->getUserTag();
        entrust_err_ = err->getMessage();
    }
    virtual void onRspAccount(WTSArray* ayAccounts) override
    {
        ++n_rsp_account_;
        account_size_ = ayAccounts->size();
        if (account_size_ > 0)
        {
            WTSAccountInfo* a = ayAccounts->at<WTSAccountInfo>(0);
            account_balance_ = a->getBalance();
            account_available_ = a->getAvailable();
        }
    }
    virtual void onRspPosition(const WTSArray* ayPositions) override
    {
        ++n_rsp_position_;
        position_size_ = ayPositions->size();
        if (position_size_ > 0)
        {
            WTSPositionItem* p = ((WTSArray*)ayPositions)->at<WTSPositionItem>(0);
            pos_avg_ = p->getAvgPrice();
            pos_total_ = p->getTotalPosition();
            pos_avail_new_ = p->getAvailNewPos();
        }
    }
    virtual void onRspOrders(const WTSArray* ayOrders) override
    { ++n_rsp_orders_; orders_size_ = ayOrders == NULL ? 0 : ayOrders->size(); }
    virtual void onRspTrades(const WTSArray* ayTrades) override
    { ++n_rsp_trades_; trades_size_ = ayTrades == NULL ? 0 : ayTrades->size(); }
    virtual void onRspSettlementInfo(uint32_t uDate, const char* content) override
    { ++n_settlement_; settle_date_ = uDate; settle_content_ = content; }
    virtual void onPushOrder(WTSOrderInfo* orderInfo) override
    {
        ++n_push_order_;
        push_order_id_ = orderInfo->getOrderID();
        push_order_usertag_ = orderInfo->getUserTag();
        push_order_date_ = orderInfo->getOrderDate();
        last_snap_.assign(orderInfo->getEntrustID());
        snap_code_ = orderInfo->getCode();
        snap_price_ = orderInfo->getPrice();
        snap_vol_ = orderInfo->getVolume();
        snap_traded_ = orderInfo->getVolTraded();
        snap_left_ = orderInfo->getVolLeft();
        snap_dir_ = orderInfo->getDirection();
        snap_offset_ = orderInfo->getOffsetType();
        snap_pxtype_ = orderInfo->getPriceType();
        snap_flag_ = orderInfo->getOrderFlag();
        snap_state_ = orderInfo->getOrderState();
    }
    virtual void onPushTrade(WTSTradeInfo* tradeRecord) override
    {
        ++n_push_trade_;
        push_trade_id_ = tradeRecord->getTradeID();
        push_trade_usertag_ = tradeRecord->getUserTag();
        push_trade_date_ = tradeRecord->getTradeDate();
        push_amount_ = tradeRecord->getAmount();
        push_price_ = tradeRecord->getPrice();
        push_vol_ = tradeRecord->getVolume();
        trade_dir_ = tradeRecord->getDirection();
        trade_offset_ = tradeRecord->getOffsetType();
        trade_ref_ = tradeRecord->getRefOrder();
    }
    virtual void onTraderError(WTSError* err, void* pData = NULL) override
    {
        (void)pData;
        ++n_trader_error_;
        trader_error_ = err->getMessage();
    }
    virtual void onPushInstrumentStatus(const char* exchg, const char* code, WTSTradeStatus state) override
    {
        ++n_inst_status_;
        inst_exchg_ = exchg;
        inst_code_ = code;
        inst_status_ = (int)state;
    }

    /* 最近一次 onPushOrder 抄下来的快照（orderInfo 随后被 release，不能存指针） */
    std::string last_snap_, snap_code_;
    double snap_price_ = 0, snap_vol_ = 0, snap_traded_ = 0, snap_left_ = 0;
    WTSDirectionType snap_dir_ = WDT_LONG;
    WTSOffsetType snap_offset_ = WOT_OPEN;
    WTSPriceType snap_pxtype_ = WPT_LIMITPRICE;
    WTSOrderFlag snap_flag_ = WOF_NOR;
    WTSOrderState snap_state_ = WOS_AllTraded;
    WTSDirectionType trade_dir_ = WDT_LONG;
    WTSOffsetType trade_offset_ = WOT_OPEN;
    std::string trade_ref_;
};

/// 柜台 API 替身：抓请求结构 + 可编程返回码
class FakeCtpApi : public CtpApiStub
{
public:
    CThostFtdcReqAuthenticateField        auth_;
    CThostFtdcReqUserLoginField           login_;
    CThostFtdcUserLogoutField             logout_;
    CThostFtdcInputOrderField             order_;
    CThostFtdcInputOrderActionField       action_;
    CThostFtdcSettlementInfoConfirmField  confirm_;
    CThostFtdcQrySettlementInfoConfirmField qry_confirm_;
    CThostFtdcQryTradingAccountField      qry_acct_;
    CThostFtdcQryInvestorPositionField    qry_pos_;
    CThostFtdcQryOrderField               qry_order_;
    CThostFtdcQryTradeField               qry_trade_;
    CThostFtdcQrySettlementInfoField      qry_settle_;

    int   n_auth_ = 0, n_login_ = 0, n_logout_ = 0, n_insert_ = 0, n_action_ = 0;
    int   n_confirm_ = 0, n_qry_confirm_ = 0, n_qry_acct_ = 0, n_qry_pos_ = 0;
    int   n_qry_order_ = 0, n_qry_trade_ = 0, n_qry_settle_ = 0;
    int   last_req_id_ = 0;
    char  trading_day_[16] = "20240115";

    virtual ~FakeCtpApi() {}

    virtual const char* GetTradingDay() override { return trading_day_; }

    virtual int ReqAuthenticate(CThostFtdcReqAuthenticateField* p, int id) override
    { ++n_auth_; last_req_id_ = id; if (p) memcpy(&auth_, p, sizeof(auth_)); return stub_ret_; }
    virtual int ReqUserLogin(CThostFtdcReqUserLoginField* p, int id) override
    { ++n_login_; last_req_id_ = id; if (p) memcpy(&login_, p, sizeof(login_)); return stub_ret_; }
    virtual int ReqUserLogout(CThostFtdcUserLogoutField* p, int id) override
    { ++n_logout_; last_req_id_ = id; if (p) memcpy(&logout_, p, sizeof(logout_)); return stub_ret_; }
    virtual int ReqOrderInsert(CThostFtdcInputOrderField* p, int id) override
    { ++n_insert_; last_req_id_ = id; if (p) memcpy(&order_, p, sizeof(order_)); return stub_ret_; }
    virtual int ReqOrderAction(CThostFtdcInputOrderActionField* p, int id) override
    { ++n_action_; last_req_id_ = id; if (p) memcpy(&action_, p, sizeof(action_)); return stub_ret_; }
    virtual int ReqSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField* p, int id) override
    { ++n_confirm_; last_req_id_ = id; if (p) memcpy(&confirm_, p, sizeof(confirm_)); return stub_ret_; }
    virtual int ReqQrySettlementInfoConfirm(CThostFtdcQrySettlementInfoConfirmField* p, int id) override
    { ++n_qry_confirm_; last_req_id_ = id; if (p) memcpy(&qry_confirm_, p, sizeof(qry_confirm_)); return stub_ret_; }
    virtual int ReqQryTradingAccount(CThostFtdcQryTradingAccountField* p, int id) override
    { ++n_qry_acct_; last_req_id_ = id; if (p) memcpy(&qry_acct_, p, sizeof(qry_acct_)); return stub_ret_; }
    virtual int ReqQryInvestorPosition(CThostFtdcQryInvestorPositionField* p, int id) override
    { ++n_qry_pos_; last_req_id_ = id; if (p) memcpy(&qry_pos_, p, sizeof(qry_pos_)); return stub_ret_; }
    virtual int ReqQryOrder(CThostFtdcQryOrderField* p, int id) override
    { ++n_qry_order_; last_req_id_ = id; if (p) memcpy(&qry_order_, p, sizeof(qry_order_)); return stub_ret_; }
    virtual int ReqQryTrade(CThostFtdcQryTradeField* p, int id) override
    { ++n_qry_trade_; last_req_id_ = id; if (p) memcpy(&qry_trade_, p, sizeof(qry_trade_)); return stub_ret_; }
    virtual int ReqQrySettlementInfo(CThostFtdcQrySettlementInfoField* p, int id) override
    { ++n_qry_settle_; last_req_id_ = id; if (p) memcpy(&qry_settle_, p, sizeof(qry_settle_)); return stub_ret_; }
};

/// protected 面 seam（TraderCTP.h:153 起为 protected，不碰 private）
class Probe : public TraderCTP
{
public:
    virtual ~Probe() {}

    void            attach(CThostFtdcTraderApi* api) { m_pUserAPI = api; }
    void            detach() { m_pUserAPI = NULL; }
    WrapperState    state() const { return m_wrapperState; }
    void            setState(WrapperState s) { m_wrapperState = s; }
    uint32_t        frontId() const { return m_frontID; }
    uint32_t        sessionId() const { return m_sessionID; }
    uint32_t        orderRef() const { return m_orderRef.load(); }
    uint32_t        tradingDate() const { return m_lDate; }
    bool            inQuery() const { return m_bInQuery; }
    bool            stopped() const { return m_bStopped; }
    uint32_t        frontCount() const { return (uint32_t)m_strFront.size(); }
    std::string     frontAt(uint32_t i) const { return m_strFront.at(i); }
    std::string     broker() const { return m_strBroker; }
    std::string     user() const { return m_strUser; }
    std::string     flowDir() const { return m_strFlowDir; }
    std::string     prodInfo() const { return m_strProdInfo; }
    std::string     settleInfo() const { return m_strSettleInfo; }
    bool            quickStart() const { return m_bQuickStart; }

    uint32_t        queryDepth()
    {
        StdUniqueLock lock(m_mtxQuery);
        return (uint32_t)m_queQuery.size();
    }

    /// 生产里由 connect() 起的 worker 线程执行队列，用例在调用线程上直接跑掉
    uint32_t        drainQueries()
    {
        uint32_t n = 0;
        for (;;)
        {
            CommonExecuter handler;
            {
                StdUniqueLock lock(m_mtxQuery);
                if (m_queQuery.empty())
                    break;
                handler = m_queQuery.front();
                m_queQuery.pop();
            }
            handler();
            ++n;
        }
        return n;
    }
};

}// namespace t2ctp

using namespace t2ctp;

class T2TraderCTPTest : public ::testing::Test
{
protected:
    Probe           trader_;
    FakeCtpApi      api_;
    RecordingSink   sink_;
    WTSVariant*     cfg_ = NULL;
    std::filesystem::path tmp_;

    void SetUp() override
    {
        namespace fs = std::filesystem;
        std::string tag = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        for (auto& ch : tag)
            if (!(std::isalnum((unsigned char)ch) || ch == '_'))
                ch = '_';
        tmp_ = fs::temp_directory_path() / fs::path("t2_ctp_" + tag + "_" + std::to_string((long)::getpid()));
        std::error_code ec;
        fs::create_directories(tmp_, ec);

        comm_ = WTSCommodityInfo::create("ag", "沪银", kExchg, "TRADING", "CHINA");
        comm_->setCategory(CC_Future);
        comm_->setCoverMode(CM_CoverToday);
        comm_->setVolScale(1);
        ct_ = WTSContractInfo::create(kRawCode, "白银2512", kExchg, "ag");
        sink_.bd_.regis(comm_, ct_);

        cfg_ = WTSVariant::createObject();
        cfg_->append("front", "tcp://127.0.0.1:1234");
        cfg_->append("broker", "9999");
        cfg_->append("user", "88880001");
        cfg_->append("pass", "pwd");
        cfg_->append("appid", "wtp_test");
        cfg_->append("authcode", "authcode123");
        cfg_->append("flowdir", tmp_.string().c_str());

        ASSERT_TRUE(trader_.init(cfg_));
        trader_.registerSpi(&sink_);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(tmp_, ec);
    }

    /// 走完 认证→登录→查确认→(确认)→就绪 全链，返回后就绪
    void bringToReady(const char* confirm_date = "20240115")
    {
        trader_.attach(&api_);
        CThostFtdcRspInfoField ok;
        memset(&ok, 0, sizeof(ok));

        ASSERT_EQ(0, trader_.login("88880001", "pwd", "prod01"));
        CThostFtdcRspAuthenticateField authf;
        memset(&authf, 0, sizeof(authf));
        trader_.OnRspAuthenticate(&authf, &ok, 1, true);
        // 主控修正（原为 T2 遗留）：OnRspAuthenticate 非错误分支只把状态推到 WS_LOGINING，
        // WS_LOGINED 必须由 OnRspUserLogin 置位（TraderCTP.cpp:742-749）。
        // 原来这里缺这一步，导致 bringToReady 之后的整串用例在 :412 的 ASSERT 上失败。
        CThostFtdcRspUserLoginField loginf;
        memset(&loginf, 0, sizeof(loginf));
        loginf.FrontID = 1;
        loginf.SessionID = 123456;
        strcpy(loginf.BrokerID, "8888");
        strcpy(loginf.UserID, "88880001");
        strcpy(loginf.TradingDay, "20240115");
        strcpy(loginf.LoginTime, "09:30:00");
        strcpy(loginf.SystemName, "ctp");
        trader_.OnRspUserLogin(&loginf, &ok, 2, true);
        ASSERT_EQ(TraderCTP::WS_LOGINED, trader_.state());

        CThostFtdcSettlementInfoConfirmField conf;
        memset(&conf, 0, sizeof(conf));
        strcpy(conf.ConfirmDate, confirm_date);
        trader_.OnRspQrySettlementInfoConfirm(&conf, &ok, 2, true);
    }

    WTSCommodityInfo* comm_ = NULL;
    WTSContractInfo* ct_ = NULL;
};

TEST_F(T2TraderCTPTest, init_reads_broker_user_and_defaults)
{
    EXPECT_EQ("9999", trader_.broker());
    EXPECT_EQ("88880001", trader_.user());
    EXPECT_EQ(1u, trader_.frontCount());
    EXPECT_EQ("tcp://127.0.0.1:1234", trader_.frontAt(0));
    // 未给 quick => 默认非 fast-start
    EXPECT_FALSE(trader_.quickStart());
    // flowdir 被 standardisePath 规范化，必须仍指向用例的临时目录
    EXPECT_NE(std::string::npos, trader_.flowDir().find("t2_ctp_"));
    EXPECT_EQ('/', trader_.flowDir()[trader_.flowDir().size() - 1]);
}

TEST_F(T2TraderCTPTest, init_accepts_front_array_and_quick_flag)
{
    WTSVariant* cfg = WTSVariant::createObject();
    WTSVariant* arr = WTSVariant::createArray();
    arr->append("tcp://10.0.0.1:1234");
    arr->append("tcp://10.0.0.2:1234");
    cfg->append("front", arr, false);
    cfg->append("broker", "7777");
    cfg->append("flowdir", tmp_.string().c_str());
    cfg->append("quick", true);

    Probe probe;
    ASSERT_TRUE(probe.init(cfg));
    EXPECT_EQ(2u, probe.frontCount());
    EXPECT_EQ("tcp://10.0.0.2:1234", probe.frontAt(1));
    EXPECT_EQ("7777", probe.broker());
    EXPECT_TRUE(probe.quickStart());
    // 缺省 flowdir 必须回落到 "CTPTDFlow"
    EXPECT_NE(std::string::npos, probe.flowDir().find("CTPTDFlow"));
}

TEST_F(T2TraderCTPTest, release_without_api_only_marks_stopped)
{
    EXPECT_FALSE(trader_.stopped());
    EXPECT_NO_THROW(trader_.release());
    EXPECT_TRUE(trader_.stopped());
    EXPECT_FALSE(trader_.isConnected());
}

TEST_F(T2TraderCTPTest, login_logout_and_queries_rejected_without_api)
{
    trader_.detach();
    EXPECT_EQ(-1, trader_.login("u2", "p2", "prod2"));
    // 拒绝前已经把参数吃进内存
    EXPECT_EQ("u2", trader_.user());
    EXPECT_EQ("prod2", trader_.prodInfo());
    EXPECT_EQ(-1, trader_.logout());
    EXPECT_EQ(TraderCTP::WS_NOTLOGIN, trader_.state());
    EXPECT_FALSE(trader_.isConnected());
    EXPECT_EQ(0u, trader_.queryDepth());
}

TEST_F(T2TraderCTPTest, order_and_query_rejected_when_state_not_ready)
{
    trader_.attach(&api_);
    trader_.setState(TraderCTP::WS_LOGINED);   // 已登录但未确认结算 => 仍不可交易

    WTSEntrust* e = WTSEntrust::create(kRawCode, 1, 7000.0, kExchg);
    EXPECT_EQ(-1, trader_.orderInsert(e));
    WTSEntrustAction* a = WTSEntrustAction::create(kRawCode, kExchg);
    a->setEntrustID("012345#0000000067#000001");
    EXPECT_EQ(-1, trader_.orderAction(a));

    EXPECT_EQ(-1, trader_.queryAccount());
    EXPECT_EQ(-1, trader_.queryPositions());
    EXPECT_EQ(-1, trader_.queryOrders());
    EXPECT_EQ(-1, trader_.queryTrades());
    EXPECT_EQ(-1, trader_.querySettlement(20240115));
    EXPECT_EQ(0u, trader_.queryDepth());
    EXPECT_EQ(0, api_.n_insert_ + api_.n_action_);

    e->release();
    a->release();
}

TEST_F(T2TraderCTPTest, authenticate_failure_marks_login_failed_and_reports)
{
    trader_.attach(&api_);
    ASSERT_EQ(0, trader_.login("88880001", "pwd", "prod01"));
    EXPECT_EQ(TraderCTP::WS_LOGINING, trader_.state());
    EXPECT_EQ(1, api_.n_auth_);
    EXPECT_EQ("9999", std::string(api_.auth_.BrokerID));
    EXPECT_EQ("wtp_test", std::string(api_.auth_.AppID));
    EXPECT_EQ("authcode123", std::string(api_.auth_.AuthCode));

    CThostFtdcRspInfoField err;
    memset(&err, 0, sizeof(err));
    err.ErrorID = 1101;
    strcpy(err.ErrorMsg, "AuthCode invalid");
    CThostFtdcRspAuthenticateField f;
    memset(&f, 0, sizeof(f));
    trader_.OnRspAuthenticate(&f, &err, 3, true);

    EXPECT_EQ(TraderCTP::WS_LOGINFAILED, trader_.state());
    EXPECT_EQ(0, api_.n_login_);           // 认证失败不允许再发登录请求
    EXPECT_EQ(1, sink_.n_login_result_);
    EXPECT_FALSE(sink_.last_succ_);
    EXPECT_EQ("AuthCode invalid", sink_.last_msg_);
}

TEST_F(T2TraderCTPTest, login_rsp_carries_credentials_and_session_params)
{
    bringToReady("20240115");

    EXPECT_EQ(1, api_.n_login_);
    EXPECT_EQ("9999", std::string(api_.login_.BrokerID));
    EXPECT_EQ("88880001", std::string(api_.login_.UserID));
    EXPECT_EQ("pwd", std::string(api_.login_.Password));
    EXPECT_EQ("prod01", std::string(api_.login_.UserProductInfo));
    // 就绪链路：queryConfirm 入队 -> drain 后真的发出 ReqQrySettlementInfoConfirm
    EXPECT_TRUE(trader_.isConnected());
    EXPECT_EQ(1u, trader_.tradingDate() % 100000000u);
    EXPECT_EQ(20240115u, trader_.tradingDate());
    EXPECT_EQ(12345u, trader_.frontId());
    EXPECT_EQ(67u, trader_.sessionId());
    EXPECT_EQ(1000u, trader_.orderRef());
    EXPECT_EQ(1, sink_.n_login_result_);
    EXPECT_TRUE(sink_.last_succ_);
    EXPECT_EQ(20240115u, sink_.last_tradedate_);
}

TEST_F(T2TraderCTPTest, settlement_unconfirmed_goes_through_confirm)
{
    trader_.attach(&api_);
    CThostFtdcRspInfoField ok;
    memset(&ok, 0, sizeof(ok));
    ASSERT_EQ(0, trader_.login("88880001", "pwd", "prod01"));
    CThostFtdcRspAuthenticateField authf;
    memset(&authf, 0, sizeof(authf));
    trader_.OnRspAuthenticate(&authf, &ok, 1, true);

    // 登录响应后应有一个待执行的查询
    EXPECT_EQ(1u, trader_.queryDepth());
    EXPECT_EQ(1u, trader_.drainQueries());
    EXPECT_EQ(1, api_.n_qry_confirm_);
    EXPECT_EQ("9999", std::string(api_.qry_confirm_.BrokerID));
    EXPECT_EQ("88880001", std::string(api_.qry_confirm_.InvestorID));

    // 确认日期 < 交易日 => 只到 CONFIRM_QRYED，并立刻发起确认请求
    CThostFtdcSettlementInfoConfirmField conf;
    memset(&conf, 0, sizeof(conf));
    strcpy(conf.ConfirmDate, "20240112");
    trader_.OnRspQrySettlementInfoConfirm(&conf, &ok, 2, true);
    EXPECT_EQ(TraderCTP::WS_CONFIRM_QRYED != trader_.state(), false);
    EXPECT_FALSE(trader_.isConnected());
    EXPECT_EQ(1, api_.n_confirm_);
    EXPECT_EQ(8u, strlen(api_.confirm_.ConfirmDate));

    // 柜台确认响应回来后进入就绪
    CThostFtdcSettlementInfoConfirmField conf2;
    memset(&conf2, 0, sizeof(conf2));
    trader_.OnRspSettlementInfoConfirm(&conf2, &ok, 3, true);
    EXPECT_EQ(TraderCTP::WS_ALLREADY, trader_.state());
    EXPECT_TRUE(trader_.isConnected());
    EXPECT_TRUE(sink_.last_succ_);
}

TEST_F(T2TraderCTPTest, make_entrust_id_is_fixed_width_and_increments_ref)
{
    bringToReady();

    char buf[64];
    EXPECT_FALSE(trader_.makeEntrustID(NULL, 64));
    EXPECT_FALSE(trader_.makeEntrustID(buf, 0));

    memset(buf, 0, sizeof(buf));
    ASSERT_TRUE(trader_.makeEntrustID(buf, sizeof(buf)));
    EXPECT_STREQ("012345#0000000067#001001", buf);

    memset(buf, 0, sizeof(buf));
    ASSERT_TRUE(trader_.makeEntrustID(buf, sizeof(buf)));
    EXPECT_STREQ("012345#0000000067#001002", buf);
    EXPECT_EQ(1002u, trader_.orderRef());
}

TEST_F(T2TraderCTPTest, order_insert_maps_types_and_ref_for_shfe)
{
    bringToReady();

    char eid[64];
    ASSERT_TRUE(trader_.makeEntrustID(eid, sizeof(eid)));

    WTSEntrust* e = WTSEntrust::create(kRawCode, 3, 7001.5, kExchg);
    e->setEntrustID(eid);
    e->setUserTag("TAG-A");
    e->setDirection(WDT_LONG);
    e->setOffsetType(WOT_OPEN);
    e->setPriceType(WPT_LIMITPRICE);
    e->setOrderFlag(WOF_FAK);

    EXPECT_EQ(0, trader_.orderInsert(e));
    EXPECT_EQ(1, api_.n_insert_);
    EXPECT_EQ(kRawCode, std::string(api_.order_.InstrumentID));
    EXPECT_EQ(kExchg, std::string(api_.order_.ExchangeID));
    EXPECT_EQ("9999", std::string(api_.order_.BrokerID));
    EXPECT_EQ("88880001", std::string(api_.order_.InvestorID));
    // entrustID 尾段 001001 必须回填成 CTP OrderRef
    EXPECT_STREQ("1001", api_.order_.OrderRef);
    EXPECT_EQ(THOST_FTDC_D_Buy, api_.order_.Direction);
    EXPECT_EQ(THOST_FTDC_OF_Open, api_.order_.CombOffsetFlag[0]);
    EXPECT_EQ(THOST_FTDC_OPT_LimitPrice, api_.order_.OrderPriceType);
    EXPECT_EQ(THOST_FTDC_TC_IOC, api_.order_.TimeCondition);
    EXPECT_EQ(THOST_FTDC_VC_AV, api_.order_.VolumeCondition);
    EXPECT_DOUBLE_EQ(7001.5, api_.order_.LimitPrice);
    EXPECT_EQ(3, api_.order_.VolumeTotalOriginal);
    EXPECT_EQ(THOST_FTDC_HF_Speculation, api_.order_.CombHedgeFlag[0]);
    EXPECT_EQ(0, api_.order_.IsAutoSuspend);

    // 带 usertag 的委托必须进 eid 缓存：同 front/session/ref 的订单回报应反查回 "TAG-A"
    CThostFtdcOrderField o;
    memset(&o, 0, sizeof(o));
    strcpy(o.InstrumentID, kRawCode);
    strcpy(o.ExchangeID, kExchg);
    o.FrontID = 12345;
    o.SessionID = 67;
    strcpy(o.OrderRef, "1001");
    o.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
    strcpy(o.InsertDate, "20240115");
    strcpy(o.InsertTime, "10:00:00");
    trader_.OnRtnOrder(&o);
    ASSERT_EQ(1u, sink_.n_push_order_);
    EXPECT_EQ("TAG-A", sink_.push_order_usertag_);

    e->release();
}

TEST_F(T2TraderCTPTest, order_insert_cffex_anyprice_and_close_offsets)
{
    bringToReady();

    WTSEntrust* e = WTSEntrust::create("IF2401", 2, 3500.0, "CFFEX");
    e->setUserTag("TAG-B");
    e->setDirection(WDT_SHORT);
    e->setOffsetType(WOT_CLOSETODAY);
    e->setPriceType(WPT_ANYPRICE);
    e->setOrderFlag(WOF_FOK);
    EXPECT_EQ(0, trader_.orderInsert(e));
    // 中金所的市价单要转成五档即时成交剩余撤销
    EXPECT_EQ(THOST_FTDC_OPT_FiveLevelPrice, api_.order_.OrderPriceType);
    EXPECT_EQ(THOST_FTDC_D_Sell, api_.order_.Direction);
    EXPECT_EQ(THOST_FTDC_OF_CloseToday, api_.order_.CombOffsetFlag[0]);
    EXPECT_EQ(THOST_FTDC_TC_IOC, api_.order_.TimeCondition);
    EXPECT_EQ(THOST_FTDC_VC_CV, api_.order_.VolumeCondition);

    e->setOffsetType(WOT_CLOSEYESTERDAY);
    e->setOrderFlag(WOF_NOR);
    EXPECT_EQ(0, trader_.orderInsert(e));
    // 昨仓在 CTP 侧与平今合并成 Close（本用例把这个既有行为钉死）
    EXPECT_EQ(THOST_FTDC_OF_Close, api_.order_.CombOffsetFlag[0]);
    EXPECT_EQ(THOST_FTDC_TC_GFD, api_.order_.TimeCondition);
    EXPECT_EQ(THOST_FTDC_VC_AV, api_.order_.VolumeCondition);
    EXPECT_EQ(2, api_.n_insert_);

    e->setDirection(WDT_SHORT);
    e->setOffsetType(WOT_FORCECLOSE);
    EXPECT_EQ(0, trader_.orderInsert(e));
    EXPECT_EQ(THOST_FTDC_OF_ForceClose, api_.order_.CombOffsetFlag[0]);

    e->release();
}

TEST_F(T2TraderCTPTest, order_action_rejects_bad_entrust_id_and_builds_cancel)
{
    bringToReady();

    WTSEntrustAction* bad = WTSEntrustAction::create(kRawCode, kExchg);
    bad->setEntrustID("012345-0000000067-000001");     // 没有 '#' 分隔
    bad->setActionFlag(WAF_CANCEL);
    EXPECT_EQ(-1, trader_.orderAction(bad));
    EXPECT_EQ(0, api_.n_action_);
    bad->release();

    WTSEntrustAction* a = WTSEntrustAction::create(kRawCode, kExchg);
    a->setEntrustID("012345#0000000067#000042");
    a->setOrderID("800042");
    a->setActionFlag(WAF_CANCEL);
    EXPECT_EQ(0, trader_.orderAction(a));
    EXPECT_EQ(1, api_.n_action_);
    EXPECT_EQ(12345, api_.action_.FrontID);
    EXPECT_EQ(67, api_.action_.SessionID);
    EXPECT_STREQ("42", api_.action_.OrderRef);
    EXPECT_EQ(THOST_FTDC_AF_Delete, api_.action_.ActionFlag);
    EXPECT_STREQ("800042", api_.action_.OrderSysID);
    EXPECT_EQ(kRawCode, std::string(api_.action_.InstrumentID));

    a->setActionFlag(WAF_MODIFY);
    EXPECT_EQ(0, trader_.orderAction(a));
    EXPECT_EQ(THOST_FTDC_AF_Modify, api_.action_.ActionFlag);
    a->release();
}

TEST_F(T2TraderCTPTest, queries_enqueue_then_send_on_drain)
{
    bringToReady();

    EXPECT_EQ(0, trader_.queryAccount());
    EXPECT_EQ(0, trader_.queryPositions());
    EXPECT_EQ(0, trader_.queryOrders());
    EXPECT_EQ(0, trader_.queryTrades());
    EXPECT_EQ(0, trader_.querySettlement(20240115));
    EXPECT_EQ(5u, trader_.queryDepth());
    EXPECT_EQ(5u, trader_.drainQueries());

    EXPECT_EQ(1, api_.n_qry_acct_);
    EXPECT_EQ(1, api_.n_qry_pos_);
    EXPECT_EQ(1, api_.n_qry_order_);
    EXPECT_EQ(1, api_.n_qry_trade_);
    EXPECT_EQ(1, api_.n_qry_settle_);
    EXPECT_EQ("9999", std::string(api_.qry_acct_.BrokerID));
    EXPECT_EQ("88880001", std::string(api_.qry_pos_.InvestorID));
    EXPECT_STREQ("20240115", api_.qry_settle_.TradingDay);
    EXPECT_EQ(0u, trader_.queryDepth());
    // genRequestID 单调递增
    EXPECT_GT(api_.last_req_id_, 1);
}

TEST_F(T2TraderCTPTest, rtn_order_conversion_maps_state_and_night_date)
{
    bringToReady();

    CThostFtdcOrderField o;
    memset(&o, 0, sizeof(o));
    strcpy(o.InstrumentID, kRawCode);
    strcpy(o.ExchangeID, kExchg);
    o.FrontID = 12345;
    o.SessionID = 67;
    strcpy(o.OrderRef, "1001");
    strcpy(o.OrderSysID, "800001");
    o.LimitPrice = 7002.0;
    o.VolumeTotalOriginal = 5;
    o.VolumeTraded = 2;
    o.Direction = THOST_FTDC_D_Buy;
    o.CombOffsetFlag[0] = THOST_FTDC_OF_Close;
    o.OrderPriceType = THOST_FTDC_OPT_AnyPrice;
    o.TimeCondition = THOST_FTDC_TC_IOC;
    o.VolumeCondition = THOST_FTDC_VC_CV;
    o.OrderStatus = THOST_FTDC_OST_PartTradedNotQueueing;
    o.OrderSubmitStatus = THOST_FTDC_OSS_Accepted;
    strcpy(o.InsertDate, "20240115");
    strcpy(o.InsertTime, "10:20:30");
    strcpy(o.StatusMsg, "部分成交");

    CThostFtdcRspInfoField ok;
    memset(&ok, 0, sizeof(ok));
    // 未登记合约 => makeOrderInfo 返回 NULL，不应推送
    CThostFtdcOrderField o2 = o;
    strcpy(o2.InstrumentID, "zz9999");
    trader_.OnRtnOrder(&o2);
    EXPECT_EQ(0u, sink_.n_push_order_);
    EXPECT_EQ(0, ok.ErrorID);

    trader_.OnRtnOrder(&o);
    ASSERT_EQ(1u, sink_.n_push_order_);
    EXPECT_EQ(kRawCode, sink_.snap_code_);
    EXPECT_DOUBLE_EQ(7002.0, sink_.snap_price_);
    EXPECT_DOUBLE_EQ(5.0, sink_.snap_vol_);
    EXPECT_DOUBLE_EQ(2.0, sink_.snap_traded_);
    EXPECT_DOUBLE_EQ(3.0, sink_.snap_left_);          // 原始总量 - 已成交
    EXPECT_EQ(WDT_SHORT, sink_.snap_dir_);            // Buy + Close => 空头平仓
    EXPECT_EQ(WOT_CLOSE, sink_.snap_offset_);
    EXPECT_EQ(WPT_ANYPRICE, sink_.snap_pxtype_);      // AnyPrice 归一
    EXPECT_EQ(WOF_FOK, sink_.snap_flag_);             // IOC + CV => FOK
    EXPECT_EQ(WOS_Canceled, sink_.snap_state_);       // 部分成交不再排队 => 已撤销
    EXPECT_EQ("012345#0000000067#001001", sink_.last_snap_);
    EXPECT_EQ(20240115u, sink_.push_order_date_);
    EXPECT_EQ("800001", sink_.push_order_id_);
    // eid 缓存里没有这笔委托 => userTag 回落成 entrustID 本身
    EXPECT_EQ("012345#0000000067#001001", sink_.push_order_usertag_);

    // 夜盘（21:30）且成交日==交易日 => 日期回退到自然日
    CThostFtdcOrderField night = o;
    strcpy(night.InsertTime, "21:30:05");
    strcpy(night.OrderRef, "1002");
    night.OrderStatus = THOST_FTDC_OST_Unknown;
    night.TimeCondition = THOST_FTDC_TC_GFD;
    trader_.OnRtnOrder(&night);
    ASSERT_EQ(2u, sink_.n_push_order_);
    EXPECT_EQ(20240114u, sink_.push_order_date_);
    EXPECT_EQ(WOS_Submitting, sink_.snap_state_);     // Unknown => 提交中
    EXPECT_EQ(WOF_NOR, sink_.snap_flag_);
    EXPECT_EQ("012345#0000000067#001002", sink_.push_order_usertag_);
}

TEST_F(T2TraderCTPTest, trade_conversion_amount_and_usertag_chain)
{
    bringToReady();

    // 先用订单回报把 orderid->usertag 写进 oid 缓存
    CThostFtdcOrderField o;
    memset(&o, 0, sizeof(o));
    strcpy(o.InstrumentID, kRawCode);
    strcpy(o.ExchangeID, kExchg);
    o.FrontID = 12345;
    o.SessionID = 67;
    strcpy(o.OrderRef, "1001");
    strcpy(o.OrderSysID, "800009");
    o.LimitPrice = 7000.0;
    o.VolumeTotalOriginal = 4;
    o.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
    strcpy(o.InsertDate, "20240115");
    strcpy(o.InsertTime, "09:15:00");
    trader_.OnRtnOrder(&o);
    ASSERT_EQ(1u, sink_.n_push_order_);

    CThostFtdcTradeField t;
    memset(&t, 0, sizeof(t));
    strcpy(t.InstrumentID, kRawCode);
    strcpy(t.ExchangeID, kExchg);
    strcpy(t.TradeID, "T0001");
    strcpy(t.OrderSysID, "800009");
    t.Price = 7000.0;
    t.Volume = 2;
    t.Direction = THOST_FTDC_D_Sell;
    t.OffsetFlag = THOST_FTDC_OF_Open;
    strcpy(t.TradeDate, "20240115");
    strcpy(t.TradeTime, "09:16:30");
    trader_.OnRtnTrade(&t);
    ASSERT_EQ(1u, sink_.n_push_trade_);
    EXPECT_EQ("T0001", sink_.push_trade_id_);
    EXPECT_DOUBLE_EQ(2.0, sink_.push_vol_);
    EXPECT_DOUBLE_EQ(14000.0, sink_.push_amount_);    // volScale(1)*2*7000
    EXPECT_EQ(WDT_SHORT, sink_.trade_dir_);           // Sell+Open => 空头开仓
    EXPECT_EQ(WOT_OPEN, sink_.trade_offset_);
    EXPECT_EQ("800009", sink_.trade_ref_);
    EXPECT_EQ(20240115u, sink_.push_trade_date_);
    EXPECT_EQ("800009", sink_.push_trade_usertag_);   // 未经 orderInsert 登记时回落自身
    (void)o;

    // 夜盘成交日期修正
    strcpy(t.TradeTime, "21:05:00");
    trader_.OnRtnTrade(&t);
    EXPECT_EQ(2u, sink_.n_push_trade_);
    EXPECT_EQ(20240114u, sink_.push_trade_date_);
}

TEST_F(T2TraderCTPTest, rsp_order_insert_and_action_error_paths)
{
    bringToReady();

    CThostFtdcInputOrderField io;
    memset(&io, 0, sizeof(io));
    strcpy(io.InstrumentID, kRawCode);
    strcpy(io.ExchangeID, kExchg);
    strcpy(io.OrderRef, "1001");
    io.Direction = THOST_FTDC_D_Buy;
    io.CombOffsetFlag[0] = THOST_FTDC_OF_Open;
    io.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
    io.TimeCondition = THOST_FTDC_TC_GFD;
    io.VolumeCondition = THOST_FTDC_VC_AV;
    io.LimitPrice = 7100.0;
    io.VolumeTotalOriginal = 1;

    CThostFtdcRspInfoField err;
    memset(&err, 0, sizeof(err));
    err.ErrorID = 15;
    strcpy(err.ErrorMsg, "资金不足");

    trader_.OnRspOrderInsert(&io, &err, 9, true);
    EXPECT_EQ(1u, sink_.n_rsp_entrust_);
    EXPECT_EQ(kRawCode, sink_.entrust_code_);
    EXPECT_EQ("资金不足(15)", sink_.entrust_err_);

    // OnErrRtnOrderInsert 同样走 entrust 通道
    trader_.OnErrRtnOrderInsert(&io, &err);
    EXPECT_EQ(2u, sink_.n_rsp_entrust_);

    // 平仓方向反查：Buy + Close => 多头平仓
    io.CombOffsetFlag[0] = THOST_FTDC_OF_CloseYesterday;
    trader_.OnErrRtnOrderInsert(&io, &err);
    EXPECT_EQ(3u, sink_.n_rsp_entrust_);

    CThostFtdcInputOrderActionField ia;
    memset(&ia, 0, sizeof(ia));
    strcpy(ia.InstrumentID, kRawCode);
    strcpy(ia.ExchangeID, kExchg);
    strcpy(ia.OrderSysID, "800010");
    strcpy(ia.OrderRef, "1001");
    ia.FrontID = 12345;
    ia.SessionID = 67;
    trader_.OnRspOrderAction(&ia, &err, 10, true);
    EXPECT_EQ(1u, sink_.n_trader_error_);
    EXPECT_EQ("资金不足(15)", sink_.trader_error_);

    // 无错误码时不应推送错误
    CThostFtdcRspInfoField ok;
    memset(&ok, 0, sizeof(ok));
    trader_.OnRspOrderAction(&ia, &ok, 11, true);
    EXPECT_EQ(1u, sink_.n_trader_error_);
}

TEST_F(T2TraderCTPTest, rsp_account_position_and_settlement)
{
    bringToReady();

    CThostFtdcTradingAccountField acct;
    memset(&acct, 0, sizeof(acct));
    strcpy(acct.AccountID, "88880001");
    acct.PreBalance = 100000.0;
    acct.CloseProfit = 500.0;
    acct.Commission = 100.0;
    acct.Deposit = 10000.0;
    acct.Withdraw = 2000.0;
    acct.Available = 90000.0;
    acct.CurrMargin = 8000.0;
    acct.PositionProfit = 300.0;
    strcpy(acct.CurrencyID, "CNY");
    CThostFtdcRspInfoField ok;
    memset(&ok, 0, sizeof(ok));

    trader_.OnRspQryTradingAccount(&acct, &ok, 12, false);
    EXPECT_EQ(0u, sink_.n_rsp_account_);          // 非 bIsLast 不推送
    trader_.OnRspQryTradingAccount(&acct, &ok, 12, true);
    ASSERT_EQ(1u, sink_.n_rsp_account_);
    EXPECT_EQ(1u, sink_.account_size_);
    EXPECT_DOUBLE_EQ(103400.0, sink_.account_balance_);   // 100000+500-100+10000-2000
    EXPECT_DOUBLE_EQ(90000.0, sink_.account_available_);

    CThostFtdcInvestorPositionField pos;
    memset(&pos, 0, sizeof(pos));
    strcpy(pos.InstrumentID, kRawCode);
    strcpy(pos.ExchangeID, kExchg);
    pos.PosiDirection = THOST_FTDC_PD_Long;
    pos.PositionDate = THOST_FTDC_PSD_Today;
    pos.Position = 6;
    pos.TodayPosition = 6;
    pos.UseMargin = 6000.0;
    pos.PositionProfit = 200.0;
    pos.PositionCost = 42000.0;
    pos.LongFrozen = 2;
    trader_.OnRspQryInvestorPosition(&pos, &ok, 13, false);
    // 同 key 再来一笔 => 累加保证金
    pos.UseMargin = 1000.0;
    trader_.OnRspQryInvestorPosition(&pos, &ok, 13, false);
    trader_.OnRspQryInvestorPosition(&pos, &ok, 13, true);
    ASSERT_EQ(1u, sink_.n_rsp_position_);
    EXPECT_EQ(1u, sink_.position_size_);
    EXPECT_DOUBLE_EQ(6.0, sink_.pos_total_);
    EXPECT_DOUBLE_EQ(7000.0, sink_.pos_avg_);     // (42000+42000)/6/1
    EXPECT_DOUBLE_EQ(4.0, sink_.pos_avail_new_);  // Position 6 - LongFrozen 2
    EXPECT_EQ(0u, trader_.queryDepth());

    // 未知合约 => 不进持仓表，但 bIsLast 仍推送空数组
    CThostFtdcInvestorPositionField pos2;
    memset(&pos2, 0, sizeof(pos2));
    strcpy(pos2.InstrumentID, "zz9999");
    strcpy(pos2.ExchangeID, kExchg);
    trader_.OnRspQryInvestorPosition(&pos2, &ok, 14, true);
    EXPECT_EQ(2u, sink_.n_rsp_position_);
    EXPECT_EQ(0u, sink_.position_size_);

    CThostFtdcSettlementInfoField settle;
    memset(&settle, 0, sizeof(settle));
    strcpy(settle.TradingDay, "20240115");
    strcpy(settle.Content, "第一条结算内容;");
    trader_.OnRspQrySettlementInfo(&settle, &ok, 15, false);
    EXPECT_EQ(0u, sink_.n_settlement_);
    strcpy(settle.Content, "第二条结算内容");
    trader_.OnRspQrySettlementInfo(&settle, &ok, 15, true);
    EXPECT_EQ(1u, sink_.n_settlement_);
    EXPECT_EQ(20240115u, sink_.settle_date_);
    EXPECT_EQ("第一条结算内容;第二条结算内容", sink_.settle_content_);
}

TEST_F(T2TraderCTPTest, rsp_qry_order_and_trade_arrays)
{
    bringToReady();

    CThostFtdcRspInfoField ok;
    memset(&ok, 0, sizeof(ok));

    CThostFtdcOrderField o;
    memset(&o, 0, sizeof(o));
    strcpy(o.InstrumentID, kRawCode);
    strcpy(o.ExchangeID, kExchg);
    o.FrontID = 12345;
    o.SessionID = 67;
    strcpy(o.OrderRef, "1001");
    o.LimitPrice = 7000.0;
    o.VolumeTotalOriginal = 1;
    o.OrderStatus = THOST_FTDC_OST_AllTraded;
    strcpy(o.InsertDate, "20240115");
    strcpy(o.InsertTime, "10:00:00");
    trader_.OnRspQryOrder(&o, &ok, 16, false);
    EXPECT_EQ(0u, sink_.n_rsp_orders_);
    trader_.OnRspQryOrder(&o, &ok, 16, true);
    ASSERT_EQ(1u, sink_.n_rsp_orders_);
    EXPECT_EQ(1u, sink_.orders_size_);
    EXPECT_TRUE(trader_.isConnected());

    CThostFtdcTradeField t;
    memset(&t, 0, sizeof(t));
    strcpy(t.InstrumentID, kRawCode);
    strcpy(t.ExchangeID, kExchg);
    strcpy(t.TradeID, "T0002");
    t.Price = 7000.0;
    t.Volume = 1;
    t.Direction = THOST_FTDC_D_Buy;
    t.OffsetFlag = THOST_FTDC_OF_Open;
    strcpy(t.TradeDate, "20240115");
    strcpy(t.TradeTime, "10:00:01");
    trader_.OnRspQryTrade(&t, &ok, 17, false);
    trader_.OnRspQryTrade(&t, &ok, 17, true);
    ASSERT_EQ(1u, sink_.n_rsp_trades_);
    EXPECT_EQ(2u, sink_.trades_size_);

    // 错误回报：只清队列不追加
    CThostFtdcRspInfoField err;
    memset(&err, 0, sizeof(err));
    err.ErrorID = 3000;
    strcpy(err.ErrorMsg, "无有效数据");
    trader_.OnRspQryOrder(&o, &err, 18, true);
    EXPECT_EQ(2u, sink_.n_rsp_orders_);
    EXPECT_EQ(0u, sink_.orders_size_);
}

TEST_F(T2TraderCTPTest, front_events_and_logout_and_status_pushes)
{
    CThostFtdcRspInfoField ok;
    memset(&ok, 0, sizeof(ok));

    trader_.OnFrontConnected();
    trader_.OnHeartBeatWarning(30);
    ASSERT_EQ(1u, sink_.n_events_.size());
    EXPECT_EQ((int)WTE_Connect, sink_.n_events_[0].first);
    EXPECT_EQ(0, sink_.n_events_[0].second);
    EXPECT_GT(sink_.n_log_, 0u);

    CThostFtdcRspInfoField err;
    memset(&err, 0, sizeof(err));
    err.ErrorID = 404;
    strcpy(err.ErrorMsg, "not supported");
    trader_.OnRspError(&err, 21, true);
    EXPECT_EQ("21 rsp error: 404 : not supported", sink_.last_log_);

    CThostFtdcInstrumentStatusField st;
    memset(&st, 0, sizeof(st));
    strcpy(st.ExchangeID, kExchg);
    strcpy(st.InstrumentID, kRawCode);
    st.InstrumentStatus = THOST_FTDC_IS_Closed;
    trader_.OnRtnInstrumentStatus(&st);
    EXPECT_EQ(1u, sink_.n_inst_status_);
    EXPECT_EQ(kExchg, sink_.inst_exchg_);
    EXPECT_EQ(kRawCode, sink_.inst_code_);
    EXPECT_EQ((int)THOST_FTDC_IS_Closed, sink_.inst_status_);

    // 就绪后被踢：状态归零并推 WTE_Close
    trader_.setState(TraderCTP::WS_ALLREADY);
    trader_.OnFrontDisconnected(0x2002);
    EXPECT_EQ(TraderCTP::WS_NOTLOGIN, trader_.state());
    EXPECT_FALSE(trader_.isConnected());
    ASSERT_EQ(2u, sink_.n_events_.size());
    EXPECT_EQ((int)WTE_Close, sink_.n_events_[1].first);
    EXPECT_EQ(0x2002, sink_.n_events_[1].second);

    CThostFtdcUserLogoutField lu;
    memset(&lu, 0, sizeof(lu));
    trader_.OnRspUserLogout(&lu, &ok, 22, true);
    ASSERT_EQ(3u, sink_.n_events_.size());
    EXPECT_EQ((int)WTE_Logout, sink_.n_events_[2].first);
    EXPECT_EQ(TraderCTP::WS_NOTLOGIN, trader_.state());

    // 有 API 时的 logout 走请求发送；返回码非 0 只写日志不改变状态
    trader_.attach(&api_);
    EXPECT_EQ(0, trader_.logout());
    EXPECT_EQ(1, api_.n_logout_);
    EXPECT_EQ("9999", std::string(api_.logout_.BrokerID));
    EXPECT_EQ("88880001", std::string(api_.logout_.UserID));
    api_.stub_ret_ = -2;
    EXPECT_EQ(0, trader_.logout());
    EXPECT_EQ(2, api_.n_logout_);
}

TEST_F(T2TraderCTPTest, DISABLED_connect_starts_worker_thread_and_needs_real_front)
{
    // connect() 会 fs::create_directories + 真实 CreateFtdcTraderApi + RegisterFront + Init，
    // 并 new 一个 StdThread 常驻轮询 m_queQuery（无 sleep 上限、无超时出口）。
    // 门禁规则禁止起线程/连柜台，整条路径改名 DISABLED_，留给主控裁定（见 bugs 文件）。
    trader_.attach(&api_);
    EXPECT_NO_THROW(trader_.disconnect());
}
