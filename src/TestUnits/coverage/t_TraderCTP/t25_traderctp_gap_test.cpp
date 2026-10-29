/*
 * lane R25 — src/TraderCTP/TraderCTP.cpp 留白补写（第二刀）
 *
 * 本文件只打 lane T2 之后**仍然是 0%** 的行段。开写前先用本轮构建的 profraw 逐行量过
 * （xcrun llvm-cov export -format=lcov，基线见 docs/迭代计划/迭代01/manifests/T25_traderctp.md），
 * T2 的 20 条用例跑完后 TraderCTP.cpp 仍有 285 行为 0，本文件按那块留白逐段补。
 *
 * seam（与 lane T2 同源，互不依赖：本文件自带 t25_ctp_api_stub.hpp，不改 T2 的任何文件）：
 *   TraderCTP.h:153 起是 protected ⇒ t25ctp::Probe : public TraderCTP 可读写
 *   m_pUserAPI / m_wrapperState / m_funcCreator / m_queQuery / m_eidCache / m_oidCache / m_ayXxx。
 *   不使用 #define private public。
 *
 * 状态机前置（照 FAIL裁定记录 A-07 的实测结论）：
 *   WS_LOGINED 只由 OnRspUserLogin 置位（TraderCTP.cpp:746），OnRspAuthenticate 非错误分支
 *   只到 WS_LOGINING（doLogin 在里面被调用）。所以 bringToLogin() 必须三步都走：
 *   login() → OnRspAuthenticate → OnRspUserLogin。
 *   缺 OnRspUserLogin 这一刀，queryConfirm()（L1424 要求 WS_LOGINED）根本不会入队，
 *   后面所有「结算确认」分支都是空转 —— lane T2 的 settlement 用例就是这么红的。
 *   WS_ALLREADY 有两条来路：OnRspQrySettlementInfoConfirm 的 ConfirmDate>=交易日（L819-826），
 *   或 OnRspSettlementInfoConfirm 且当时状态是 WS_CONFIRM_QRYED（L849-856）。
 *
 * 真厂商实现：一次都不碰。connect() 用的 m_funcCreator 被换成测试自己的 creator，
 * 返回 t25ctp::StubApi（不起线程、不开 socket）；因此 CreateFtdcTraderApi 不会被调用。
 * 唯一会真起线程的是 connect() 里的 worker（L336），本文件把它跑起来并在 TearDown 里 join；
 * 会挂死的路径（worker 因 m_bInQuery 永不复位而卡住）收进 DISABLED_ 用例。
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSCollection.hpp"
#include "Includes/WTSTradeDef.hpp"
#include "Includes/WTSVariant.hpp"
#include "Share/TimeUtils.hpp"

#include "t25_ctp_api_stub.hpp"

namespace t25ctp {

USING_NS_WTP;

static const char* kExchgAg  = "SHFE";
static const char* kCodeAg   = "ag2512";
static const char* kCodeAg2  = "ag2602";
static const char* kExchgIf  = "CFFEX";
static const char* kCodeIf   = "IF2401";
static const char* kCodeIf2  = "IF2403";
static const char* kExchgSpd = "DCE";
static const char* kCodeSpd  = "SPDa_b";
static const char* kCodeUnk  = "zz9999";

/// 合约表替身：登记 4 个品种（CoverToday / 开平 / 组合），未知合约返回 NULL
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
    }

    virtual WTSCommodityInfo* getCommodity(const char* exchgpid) override
    {
        for (auto* c : _comms)
            if (strcmp(c->getFullPid(), exchgpid) == 0)
                return c;
        return NULL;
    }
    virtual WTSCommodityInfo* getCommodity(const char*, const char*) override { return NULL; }
    virtual WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t uDate = 0) override
    {
        (void)uDate;
        auto it = _ct_key.find(std::string(code) + "|" + exchg);
        return it == _ct_key.end() ? NULL : it->second;
    }
    virtual WTSArray* getContracts(const char* = "", uint32_t = 0) override { return NULL; }
    virtual WTSSessionInfo* getSession(const char*) override { return NULL; }
    virtual WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return NULL; }
    virtual WTSArray* getAllSessions() override { return NULL; }
    virtual bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
    virtual uint32_t calcTradingDate(const char*, uint32_t uDate, uint32_t, bool = false) override
    { return uDate; }
    virtual uint64_t getBoundaryTime(const char*, uint32_t tDate, bool = false, bool = true) override
    { return tDate; }

private:
    std::vector<WTSCommodityInfo*>              _comms;
    std::vector<WTSContractInfo*>               _cts;
    wt_hashmap<std::string, WTSContractInfo*>   _ct_key;
};

struct PosSnap
{
    std::string code_, exchg_, currency_;
    int         dir_ = -1;
    double      pre_ = 0, new_ = 0, total_ = 0, availPre_ = 0, availNew_ = 0;
    double      margin_ = 0, dyn_ = 0, cost_ = 0, avg_ = 0;
};

/// 记录型 sink：把回报对象的值抄成快照（生产侧随后 release()，不能存指针）
class Sink : public ITraderSpi
{
public:
    FakeBaseDataMgr bd_;

    uint32_t    n_log_ = 0;
    std::string last_log_;
    std::vector<std::string> logs_;
    std::vector<std::pair<int, int32_t> > n_events_;
    uint32_t    n_login_result_ = 0;
    bool        last_succ_ = false;
    std::string last_msg_;
    uint32_t    last_tradedate_ = 0xFFFFFFFF;

    uint32_t    n_rsp_entrust_ = 0;
    std::string ent_code_, ent_exchg_, ent_eid_, ent_usertag_, ent_err_;
    int         ent_dir_ = -1, ent_offset_ = -1, ent_pxtype_ = -1, ent_flag_ = -1;
    double      ent_vol_ = 0, ent_price_ = 0;

    uint32_t    n_trader_error_ = 0;
    std::string err_msg_;
    bool        has_action_ = false;
    std::string act_code_, act_exchg_, act_eid_, act_oid_, act_usertag_;

    uint32_t    n_rsp_account_ = 0, account_size_ = 0;
    double      account_balance_ = 0, account_available_ = 0;

    uint32_t    n_rsp_position_ = 0;
    std::vector<PosSnap> pos_;

    uint32_t    n_rsp_orders_ = 0, orders_size_ = 0;
    uint32_t    n_rsp_trades_ = 0, trades_size_ = 0;

    uint32_t    n_settlement_ = 0, settle_date_ = 0;
    std::string settle_content_;

    uint32_t    n_push_order_ = 0;
    std::string ord_code_, ord_exchg_, ord_eid_, ord_usertag_, ord_oid_, ord_statemsg_;
    double      ord_price_ = 0, ord_vol_ = 0, ord_traded_ = 0, ord_left_ = 0;
    int         ord_dir_ = -1, ord_offset_ = -1, ord_pxtype_ = -1, ord_flag_ = -1, ord_state_ = -1;
    bool        capture_ord_flag_ = true;
    bool        ord_is_error_ = false;
    uint32_t    ord_date_ = 0;
    uint64_t    ord_time_ = 0;

    uint32_t    n_push_trade_ = 0;
    std::string trd_code_, trd_id_, trd_ref_, trd_usertag_;
    double      trd_price_ = 0, trd_vol_ = 0, trd_amount_ = 0;
    int         trd_dir_ = -1, trd_offset_ = -1, trd_type_ = -1;
    uint32_t    trd_date_ = 0;
    uint64_t    trd_time_ = 0;

    uint32_t    n_inst_status_ = 0;
    std::string inst_exchg_, inst_code_;
    int         inst_status_ = -1;

    virtual ~Sink() {}

    bool hasLog(const char* sub) const
    {
        for (auto& s : logs_)
            if (s.find(sub) != std::string::npos)
                return true;
        return false;
    }

    const PosSnap* findPos(const std::string& code) const
    {
        for (auto& p : pos_)
            if (p.code_ == code)
                return &p;
        return NULL;
    }

    /// 同一合约的多/空两行会产出两个 item，code 相同，必须再按方向区分
    const PosSnap* findPos(const std::string& code, int dir) const
    {
        for (auto& p : pos_)
            if (p.code_ == code && p.dir_ == dir)
                return &p;
        return NULL;
    }

    virtual IBaseDataMgr* getBaseDataMgr() override { return &bd_; }
    virtual void handleTraderLog(WTSLogLevel, const char* message) override
    {
        ++n_log_;
        last_log_ = message;
        logs_.push_back(message);
    }
    virtual void handleEvent(WTSTraderEvent e, int32_t ec) override
    { n_events_.push_back(std::make_pair((int)e, ec)); }
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
        ent_code_ = entrust->getCode();
        ent_exchg_ = entrust->getExchg();
        ent_eid_ = entrust->getEntrustID();
        ent_usertag_ = entrust->getUserTag();
        ent_dir_ = entrust->getDirection();
        ent_offset_ = entrust->getOffsetType();
        ent_pxtype_ = entrust->getPriceType();
        ent_flag_ = entrust->getOrderFlag();
        ent_vol_ = entrust->getVolume();
        ent_price_ = entrust->getPrice();
        ent_err_ = err == NULL ? "" : err->getMessage();
    }
    virtual void onRspAccount(WTSArray* ayAccounts) override
    {
        ++n_rsp_account_;
        account_size_ = ayAccounts == NULL ? 0 : ayAccounts->size();
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
        pos_.clear();
        if (ayPositions == NULL)
            return;
        WTSArray* ay = (WTSArray*)ayPositions;
        for (uint32_t i = 0; i < ay->size(); i++)
        {
            WTSPositionItem* p = ay->at<WTSPositionItem>(i);
            PosSnap s;
            s.code_ = p->getCode();
            s.exchg_ = p->getExchg();
            s.currency_ = p->getCurrency();
            s.dir_ = p->getDirection();
            s.pre_ = p->getPrePosition();
            s.new_ = p->getNewPosition();
            s.total_ = p->getTotalPosition();
            s.availPre_ = p->getAvailPrePos();
            s.availNew_ = p->getAvailNewPos();
            s.margin_ = p->getMargin();
            s.dyn_ = p->getDynProfit();
            s.cost_ = p->getPositionCost();
            s.avg_ = p->getAvgPrice();
            pos_.push_back(s);
        }
    }
    virtual void onRspOrders(const WTSArray* ayOrders) override
    { ++n_rsp_orders_; orders_size_ = ayOrders == NULL ? 0 : ayOrders->size(); }
    virtual void onRspTrades(const WTSArray* ayTrades) override
    { ++n_rsp_trades_; trades_size_ = ayTrades == NULL ? 0 : ayTrades->size(); }
    virtual void onRspSettlementInfo(uint32_t uDate, const char* content) override
    { ++n_settlement_; settle_date_ = uDate; settle_content_ = content; }
    virtual void onPushOrder(WTSOrderInfo* o) override
    {
        ++n_push_order_;
        ord_code_ = o->getCode();
        ord_exchg_ = o->getExchg();
        ord_eid_ = o->getEntrustID();
        ord_usertag_ = o->getUserTag();
        ord_oid_ = o->getOrderID();
        ord_statemsg_ = o->getStateMsg();
        ord_price_ = o->getPrice();
        ord_vol_ = o->getVolume();
        ord_traded_ = o->getVolTraded();
        ord_left_ = o->getVolLeft();
        ord_dir_ = o->getDirection();
        ord_offset_ = o->getOffsetType();
        ord_pxtype_ = o->getPriceType();
        if (capture_ord_flag_)
            ord_flag_ = o->getOrderFlag();
        ord_state_ = o->getOrderState();
        ord_is_error_ = o->isError();
        ord_date_ = o->getOrderDate();
        ord_time_ = o->getOrderTime();
    }
    virtual void onPushTrade(WTSTradeInfo* t) override
    {
        ++n_push_trade_;
        trd_code_ = t->getCode();
        trd_id_ = t->getTradeID();
        trd_ref_ = t->getRefOrder();
        trd_usertag_ = t->getUserTag();
        trd_price_ = t->getPrice();
        trd_vol_ = t->getVolume();
        trd_amount_ = t->getAmount();
        trd_dir_ = t->getDirection();
        trd_offset_ = t->getOffsetType();
        trd_type_ = t->getTradeType();
        trd_date_ = t->getTradeDate();
        trd_time_ = t->getTradeTime();
    }
    virtual void onTraderError(WTSError* err, void* pData = NULL) override
    {
        ++n_trader_error_;
        err_msg_ = err == NULL ? "" : err->getMessage();
        has_action_ = pData != NULL;
        if (pData != NULL)
        {
            WTSEntrustAction* a = (WTSEntrustAction*)pData;
            act_code_ = a->getCode();
            act_exchg_ = a->getExchg();
            act_eid_ = a->getEntrustID();
            act_oid_ = a->getOrderID();
            act_usertag_ = a->getUserTag();
        }
    }
    virtual void onPushInstrumentStatus(const char* exchg, const char* code, WTSTradeStatus state) override
    {
        ++n_inst_status_;
        inst_exchg_ = exchg;
        inst_code_ = code;
        inst_status_ = (int)state;
    }
};

/// protected 面 seam
class Probe : public TraderCTP
{
public:
    virtual ~Probe() {}

    void            attach(CThostFtdcTraderApi* api) { m_pUserAPI = api; }
    void            detach() { m_pUserAPI = NULL; }
    CThostFtdcTraderApi* api() const { return m_pUserAPI; }
    void            setCreator(CTPCreator f) { m_funcCreator = f; }
    WrapperState    state() const { return m_wrapperState; }
    void            setState(WrapperState s) { m_wrapperState = s; }
    uint32_t        frontId() const { return m_frontID; }
    uint32_t        sessionId() const { return m_sessionID; }
    uint32_t        orderRef() const { return m_orderRef.load(); }
    uint32_t        tradingDate() const { return m_lDate; }
    bool            inQuery() const { return m_bInQuery; }
    void            setInQuery(bool b) { m_bInQuery = b; }
    bool            stopped() const { return m_bStopped; }
    uint32_t        frontCount() const { return (uint32_t)m_strFront.size(); }
    std::string     frontAt(uint32_t i) const { return m_strFront.at(i); }
    std::string     broker() const { return m_strBroker; }
    std::string     user() const { return m_strUser; }
    std::string     flowDir() const { return m_strFlowDir; }
    bool            quickStart() const { return m_bQuickStart; }
    void            setQuick(bool b) { m_bQuickStart = b; }
    ITraderSpi*     sink() const { return m_sink; }

    uint32_t        eidSize() const { return m_eidCache.size(); }
    uint32_t        oidSize() const { return m_oidCache.size(); }
    std::string     eidValue(const char* k) const
    {
        const char* v = m_eidCache.get(k);
        return v == NULL ? std::string("<null>") : std::string(v);
    }
    std::string     oidValue(const char* k) const
    {
        const char* v = m_oidCache.get(k);
        return v == NULL ? std::string("<null>") : std::string(v);
    }

    uint32_t        ordersSize() const { return m_ayOrders == NULL ? 0xFFFF : m_ayOrders->size(); }
    uint32_t        tradesSize() const { return m_ayTrades == NULL ? 0xFFFF : m_ayTrades->size(); }
    uint32_t        posDetailSize() const { return m_ayPosDetail == NULL ? 0xFFFF : m_ayPosDetail->size(); }
    uint32_t        posMapSize() const { return m_mapPosition == NULL ? 0xFFFF : (uint32_t)m_mapPosition->size(); }
    /// m_ayPosDetail 生产里从来没有分配点（只有 L212 置 NULL 与 L296 clear），用例自己造一个把 clear() 走进去
    void            armPosDetail() { if (m_ayPosDetail == NULL) m_ayPosDetail = WTSArray::create(); }

    bool            hasWorker() const { return m_thrdWorker != NULL; }

    uint32_t        queryDepth()
    {
        StdUniqueLock lock(m_mtxQuery);
        return (uint32_t)m_queQuery.size();
    }

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

    /// TearDown 安全网：绝不带着 joinable 的 std::thread 析构（否则 std::terminate）
    void            killWorker()
    {
        m_bStopped = true;
        if (m_thrdWorker != NULL)
        {
            if (m_thrdWorker->joinable())
                m_thrdWorker->join();
            m_thrdWorker = NULL;
        }
    }
};

/* connect() 用的假 creator：厂商 CreateFtdcTraderApi 一个字节都不执行 */
static CThostFtdcTraderApi*   g_api_for_creator = NULL;
static std::atomic<int>       g_n_creator{0};
static std::string            g_creator_path;

static CThostFtdcTraderApi* fakeCreator(const char* path)
{
    ++g_n_creator;
    g_creator_path = path == NULL ? "" : path;
    return g_api_for_creator;
}

}// namespace t25ctp

using namespace t25ctp;

/* 插件 C 入口（TraderCTP.cpp:185-201）：真符号是 extern "C" 的 _createTrader/_deleteTrader，
 * 在函数体里写 extern 声明会被按 C++ 链接找符号（实测 undefined symbols），必须在文件作用域声明。 */
extern "C" {
    ITraderApi* createTrader();
    void deleteTrader(ITraderApi*& trader);
}

/// lane T2 之外那个从未被调用的自由函数（TraderCTP.cpp:40），外部链接，可直连取覆盖率
extern uint32_t strToTime(const char* strTime) noexcept;

class T25TraderCTPTest : public ::testing::Test
{
protected:
    Probe           trader_;
    StubApi         api_;
    Sink            sink_;
    WTSVariant*     cfg_ = NULL;
    std::filesystem::path tmp_;

    void SetUp() override
    {
        namespace fs = std::filesystem;
        std::string tag = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        for (auto& ch : tag)
            if (!(std::isalnum((unsigned char)ch) || ch == '_'))
                ch = '_';
        tmp_ = fs::temp_directory_path() / fs::path("t25_ctp_" + tag + "_" + std::to_string((long)::getpid()));
        std::error_code ec;
        fs::create_directories(tmp_, ec);

        /* ag：上期所，开平昨平今、volScale=1 */
        comm_ag_ = WTSCommodityInfo::create("ag", "沪银", kExchgAg, "TRADING", "CHINA");
        comm_ag_->setCategory(CC_Future);
        comm_ag_->setCoverMode(CM_CoverToday);
        comm_ag_->setVolScale(1);
        ct_ag_ = WTSContractInfo::create(kCodeAg, "白银2512", kExchgAg, "ag");
        ct_ag2_ = WTSContractInfo::create(kCodeAg2, "白银2602", kExchgAg, "ag");
        sink_.bd_.regis(comm_ag_, ct_ag_);
        sink_.bd_.regis(comm_ag_, ct_ag2_);

        /* IF：中金所，开平（非 CoverToday）、volScale=50 */
        comm_if_ = WTSCommodityInfo::create("IF", "沪深300", kExchgIf, "TRADING", "CHINA");
        comm_if_->setCategory(CC_Future);
        comm_if_->setCoverMode(CM_OpenCover);
        comm_if_->setVolScale(50);
        ct_if_ = WTSContractInfo::create(kCodeIf, "股指2401", kExchgIf, "IF");
        ct_if2_ = WTSContractInfo::create(kCodeIf2, "股指2403", kExchgIf, "IF");
        sink_.bd_.regis(comm_if_, ct_if_);
        sink_.bd_.regis(comm_if_, ct_if2_);

        /* SPD：组合品种（CC_Combination），走可用持仓不计算那一支 */
        comm_spd_ = WTSCommodityInfo::create("SPD", "价差组合", kExchgSpd, "TRADING", "CHINA");
        comm_spd_->setCategory(CC_Combination);
        comm_spd_->setCoverMode(CM_CoverToday);
        comm_spd_->setVolScale(1);
        ct_spd_ = WTSContractInfo::create(kCodeSpd, "组合", kExchgSpd, "SPD");
        sink_.bd_.regis(comm_spd_, ct_spd_);

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
        memset(&ok_, 0, sizeof(ok_));
    }

    void TearDown() override
    {
        trader_.killWorker();
        std::error_code ec;
        std::filesystem::remove_all(tmp_, ec);
    }

    /// 认证 + 登录回报：停在 WS_LOGINED（结算确认之前）
    void bringToLogin()
    {
        trader_.attach(&api_);
        ASSERT_EQ(0, trader_.login("88880001", "pwd", "prod01"));
        EXPECT_EQ(TraderCTP::WS_LOGINING, trader_.state());
        CThostFtdcRspAuthenticateField authf;
        memset(&authf, 0, sizeof(authf));
        trader_.OnRspAuthenticate(&authf, &ok_, 1, true);
        EXPECT_EQ(1, api_.n_login_.load());

        CThostFtdcRspUserLoginField lf;
        memset(&lf, 0, sizeof(lf));
        lf.FrontID = 12345;
        lf.SessionID = 67;
        strcpy(lf.BrokerID, "9999");
        strcpy(lf.UserID, "88880001");
        strcpy(lf.TradingDay, "20240115");
        strcpy(lf.LoginTime, "09:30:00");
        strcpy(lf.MaxOrderRef, "1000");
        trader_.OnRspUserLogin(&lf, &ok_, 2, true);
        ASSERT_EQ(TraderCTP::WS_LOGINED, trader_.state());
        EXPECT_EQ(12345u, trader_.frontId());
        EXPECT_EQ(67u, trader_.sessionId());
        EXPECT_EQ(1000u, trader_.orderRef());
        EXPECT_EQ(20240115u, trader_.tradingDate());
    }

    /// 走完 认证→登录→查确认→确认日期==交易日 ⇒ WS_ALLREADY
    void bringToReady()
    {
        bringToLogin();
        EXPECT_EQ(1u, trader_.queryDepth());       // OnRspUserLogin 末尾压入 queryConfirm
        EXPECT_EQ(1u, trader_.drainQueries());
        CThostFtdcSettlementInfoConfirmField conf;
        memset(&conf, 0, sizeof(conf));
        strcpy(conf.ConfirmDate, "20240115");
        trader_.OnRspQrySettlementInfoConfirm(&conf, &ok_, 3, true);
        ASSERT_EQ(TraderCTP::WS_ALLREADY, trader_.state());
        EXPECT_TRUE(trader_.isConnected());
    }

    void fillErr(CThostFtdcRspInfoField& info, int id, const char* msg)
    {
        memset(&info, 0, sizeof(info));
        info.ErrorID = id;
        strcpy(info.ErrorMsg, msg);
    }

    /// worker 线程协作：有限轮询，绝不 sleep 等真柜台
    bool waitUntil(const std::function<bool()>& pred, int max_ms = 6000)
    {
        for (int waited = 0; waited < max_ms; waited += 10)
        {
            if (pred())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return pred();
    }

    CThostFtdcRspInfoField  ok_;

    WTSCommodityInfo*   comm_ag_ = NULL;
    WTSCommodityInfo*   comm_if_ = NULL;
    WTSCommodityInfo*   comm_spd_ = NULL;
    WTSContractInfo*    ct_ag_ = NULL;
    WTSContractInfo*    ct_ag2_ = NULL;
    WTSContractInfo*    ct_if_ = NULL;
    WTSContractInfo*    ct_if2_ = NULL;
    WTSContractInfo*    ct_spd_ = NULL;
};

//////////////////////////////////////////////////////////////////////////
// 1) 插件 C 入口（L185-201，lane T2 明确没建这块）
//
TEST_F(T25TraderCTPTest, plugin_entry_points_build_and_destroy_trader)
{
    ITraderApi* p = createTrader();
    ASSERT_TRUE(p != NULL);
    ASSERT_TRUE(dynamic_cast<TraderCTP*>(p) != NULL);
    // 新建对象既没连柜台也没登记容器
    EXPECT_FALSE(p->isConnected());
    p->release();
    EXPECT_FALSE(p->isConnected());

    deleteTrader(p);
    EXPECT_EQ(NULL, p);                     // deleteTrader 必须把入参置空
    deleteTrader(p);                        // 二次调用（已为 NULL）安全
}

//////////////////////////////////////////////////////////////////////////
// 2) init 的缺省 flowdir 与非法 front 类型（L249-250 与 L229-239 双 false 分支）
//
TEST_F(T25TraderCTPTest, init_falls_back_to_ctptdflow_and_ignores_non_string_front)
{
    Probe probe;
    WTSVariant* cfg = WTSVariant::createObject();
    cfg->append("front", 12345);              // 既不是 string 也不是 array
    cfg->append("broker", "7777");
    cfg->append("quick", true);
    // 故意不给 flowdir / user / pass / appid / authcode

    ASSERT_TRUE(probe.init(cfg));
    EXPECT_EQ(0u, probe.frontCount());        // 非字符串/非数组的 front 不入表
    EXPECT_EQ("7777", probe.broker());
    EXPECT_EQ("", probe.user());
    EXPECT_EQ("CTPTDFlow/", probe.flowDir()); // 缺省回落 + standardisePath 补尾斜杠
    EXPECT_TRUE(probe.quickStart());
}

TEST_F(T25TraderCTPTest, init_keeps_trailing_slash_of_given_flowdir)
{
    Probe probe;
    WTSVariant* cfg = WTSVariant::createObject();
    WTSVariant* arr = WTSVariant::createArray();
    arr->append("tcp://10.0.0.1:1234");
    arr->append("tcp://10.0.0.2:1234");
    cfg->append("front", arr, false);
    cfg->append("broker", "8888");
    cfg->append("flowdir", tmp_.string().c_str());

    ASSERT_TRUE(probe.init(cfg));
    EXPECT_EQ(2u, probe.frontCount());
    EXPECT_EQ("tcp://10.0.0.2:1234", probe.frontAt(1));
    EXPECT_EQ(tmp_.string() + "/", probe.flowDir());
}

//////////////////////////////////////////////////////////////////////////
// 3) strToTime（L40-54）：产品里没有任何调用点，函数体本身是外部链接，直连把这段打死
//
TEST_F(T25TraderCTPTest, str_to_time_strips_colons_and_parses_to_uint)
{
    EXPECT_EQ(93015u, strToTime("09:30:15"));
    EXPECT_EQ(213005u, strToTime("21:30:05"));
    EXPECT_EQ(0u, strToTime(""));
    EXPECT_EQ(120000u, strToTime("120000"));          // 无冒号时等价于原样解析
}

//////////////////////////////////////////////////////////////////////////
// 4) OnRspUserLogin 的错误分支（L789-796）与 RspInfo 为 NULL 的语义
//
TEST_F(T25TraderCTPTest, login_rsp_with_error_marks_login_failed_and_never_queries_confirm)
{
    trader_.attach(&api_);
    ASSERT_EQ(0, trader_.login("88880001", "pwd", "prod01"));
    CThostFtdcRspAuthenticateField authf;
    memset(&authf, 0, sizeof(authf));
    trader_.OnRspAuthenticate(&authf, &ok_, 1, true);

    CThostFtdcRspInfoField err;
    fillErr(err, 40, "CTP:登录失败");
    CThostFtdcRspUserLoginField lf;
    memset(&lf, 0, sizeof(lf));
    lf.FrontID = 12345;
    lf.SessionID = 67;
    strcpy(lf.MaxOrderRef, "1000");
    trader_.OnRspUserLogin(&lf, &err, 2, true);

    EXPECT_EQ(TraderCTP::WS_LOGINFAILED, trader_.state());
    EXPECT_FALSE(trader_.isConnected());
    // 会话参数一个都不许落库
    EXPECT_EQ(0u, trader_.frontId());
    EXPECT_EQ(0u, trader_.sessionId());
    EXPECT_EQ(0u, trader_.tradingDate());
    // 失败分支不许压查询
    EXPECT_EQ(0u, trader_.queryDepth());
    EXPECT_EQ(1u, sink_.n_login_result_);
    EXPECT_FALSE(sink_.last_succ_);
    EXPECT_EQ("CTP:登录失败", sink_.last_msg_);
    EXPECT_EQ(0u, sink_.last_tradedate_);
}

TEST_F(T25TraderCTPTest, login_rsp_with_null_rspinfo_is_taken_as_success)
{
    // IsErrorRspInfo(NULL)==false（L56-59）⇒ 空 RspInfo 走成功分支，不是缺陷而是既有语义
    trader_.attach(&api_);
    ASSERT_EQ(0, trader_.login("88880001", "pwd", "prod01"));
    CThostFtdcRspAuthenticateField authf;
    memset(&authf, 0, sizeof(authf));
    trader_.OnRspAuthenticate(&authf, NULL, 1, true);

    CThostFtdcRspUserLoginField lf;
    memset(&lf, 0, sizeof(lf));
    lf.FrontID = 7;
    lf.SessionID = 8;
    strcpy(lf.MaxOrderRef, "50");
    trader_.OnRspUserLogin(&lf, NULL, 2, true);

    EXPECT_EQ(TraderCTP::WS_LOGINED, trader_.state());
    EXPECT_EQ(7u, trader_.frontId());
    EXPECT_EQ(8u, trader_.sessionId());
    EXPECT_EQ(50u, trader_.orderRef());
    EXPECT_EQ(0u, sink_.n_login_result_);        // 成功分支只压确认查询，绝不报「登录成功」
    EXPECT_EQ(1u, trader_.queryDepth());         // L787 ⇒ 成功分支末尾压入 queryConfirm
}

//////////////////////////////////////////////////////////////////////////
// 5) 各 Req* 同步返回码的日志分支：doLogin L441-444 / orderInsert L543-546 / orderAction L581-584
//
TEST_F(T25TraderCTPTest, do_login_send_failure_is_logged_without_waiting_for_response)
{
    api_.ret_ = 0;                               // 认证请求已受理，登录请求仍等异步认证回报
    trader_.attach(&api_);
    ASSERT_EQ(0, trader_.login("88880001", "pwd", "prod01"));
    api_.ret_ = 5;                               // 只让后续 ReqUserLogin 同步失败
    CThostFtdcRspAuthenticateField authf;
    memset(&authf, 0, sizeof(authf));
    trader_.OnRspAuthenticate(&authf, &ok_, 1, true);

    EXPECT_EQ(1, api_.n_login_.load());
    EXPECT_NE(TraderCTP::WS_LOGINING, trader_.state());
    EXPECT_TRUE(sink_.hasLog("Sending login request failed: 5"));
}

TEST_F(T25TraderCTPTest, order_insert_and_action_send_failures_return_error_and_are_logged)
{
    bringToReady();

    api_.ret_ = 9;
    WTSEntrust* e = WTSEntrust::create(kCodeAg, 2, 7000.0, kExchgAg);
    e->setEntrustID("012345#0000000067#001001");
    e->setUserTag("TAG-INSERT-ERROR");
    EXPECT_LT(trader_.orderInsert(e), 0);
    EXPECT_EQ(1, api_.n_insert_.load());
    EXPECT_TRUE(sink_.hasLog("Order inserting failed: 9"));
    e->release();

    WTSEntrustAction* a = WTSEntrustAction::create(kCodeAg, kExchgAg);
    a->setEntrustID("012345#0000000067#001001");
    a->setActionFlag(WAF_CANCEL);
    EXPECT_LT(trader_.orderAction(a), 0);
    EXPECT_EQ(1, api_.n_action_.load());
    EXPECT_TRUE(sink_.hasLog("Sending cancel request failed: 9"));
    a->release();
}

TEST_F(T25TraderCTPTest, authenticate_sync_failure_returns_error_without_waiting_for_login)
{
    // ReqAuthenticate 同步拒绝时，不应把登录报告为已受理并继续等待异步回报。
    // ret=0 的异步等待路径由 bringToLogin() 覆盖。
    api_.ret_ = 7;
    trader_.attach(&api_);
    EXPECT_NE(0, trader_.login("88880001", "pwd", "prod01"));
    EXPECT_EQ(1, api_.n_auth_.load());
    EXPECT_EQ(0, api_.n_login_.load());
    EXPECT_NE(TraderCTP::WS_LOGINING, trader_.state());
}

//////////////////////////////////////////////////////////////////////////
// 6) orderInsert 的 usertag 分支与包装矩阵
//
TEST_F(T25TraderCTPTest, order_insert_without_usertag_reuses_current_order_ref_and_skips_cache)
{
    bringToReady();
    uint32_t ref0 = trader_.orderRef();           // bringToReady 后 = 1000（MaxOrderRef）
    uint32_t eid_sz_before = trader_.eidSize();

    WTSEntrust* e = WTSEntrust::create(kCodeAg, 1, 7000.0, kExchgAg);
    // 不设 userTag、不设 entrustID ⇒ 走 L486-489，且不能往 eid 缓存里写
    EXPECT_EQ(0, trader_.orderInsert(e));
    EXPECT_EQ(1, api_.n_insert_.load());
    EXPECT_EQ(std::to_string(ref0), std::string(api_.order_.OrderRef));
    EXPECT_EQ(ref0, trader_.orderRef());          // fetch_add(0) 不自增
    EXPECT_EQ(eid_sz_before, trader_.eidSize());
    EXPECT_EQ(THOST_FTDC_OPT_AnyPrice, api_.order_.OrderPriceType);  // WTSEntrust 默认价型 WPT_ANYPRICE
    EXPECT_EQ(THOST_FTDC_TC_GFD, api_.order_.TimeCondition);         // 默认 WOF_NOR
    e->release();
}

TEST_F(T25TraderCTPTest, order_insert_rejects_malformed_entrust_id_when_usertag_set)
{
    bringToReady();
    const uint32_t eid_sz_before = trader_.eidSize();

    WTSEntrust* e = WTSEntrust::create(kCodeAg, 1, 7000.0, kExchgAg);
    e->setEntrustID("no-hash-at-all");            // extractEntrustID 必然返回 false
    e->setUserTag("TAG-BAD");
    // 与 orderAction 的解析失败路径一致，不能发送未能解析 OrderRef 的请求。
    EXPECT_EQ(-1, trader_.orderInsert(e));
    EXPECT_EQ(0, api_.n_insert_.load());
    EXPECT_EQ(eid_sz_before, trader_.eidSize());
    EXPECT_TRUE(trader_.eidValue("no-hash-at-all").empty());
    e->release();
}

TEST_F(T25TraderCTPTest, order_insert_wraps_bestprice_lastprice_and_every_offset)
{
    bringToReady();

    WTSEntrust* e = WTSEntrust::create(kCodeAg, 1, 7000.0, kExchgAg);
    e->setDirection(WDT_LONG);

    e->setPriceType(WPT_BESTPRICE);
    e->setOffsetType(WOT_CLOSETODAY);
    e->setOrderFlag(WOF_NOR);
    EXPECT_EQ(0, trader_.orderInsert(e));
    EXPECT_EQ(THOST_FTDC_OPT_BestPrice, api_.order_.OrderPriceType);
    EXPECT_EQ(THOST_FTDC_OF_CloseToday, api_.order_.CombOffsetFlag[0]);
    EXPECT_EQ(THOST_FTDC_TC_GFD, api_.order_.TimeCondition);
    EXPECT_EQ(THOST_FTDC_VC_AV, api_.order_.VolumeCondition);

    e->setPriceType(WPT_LASTPRICE);               // 落到 default ⇒ LastPrice
    e->setOffsetType(WOT_CLOSEYESTERDAY);         // ⇒ Close
    e->setOrderFlag(WOF_FOK);
    EXPECT_EQ(0, trader_.orderInsert(e));
    EXPECT_EQ(THOST_FTDC_OPT_LastPrice, api_.order_.OrderPriceType);
    EXPECT_EQ(THOST_FTDC_OF_Close, api_.order_.CombOffsetFlag[0]);
    EXPECT_EQ(THOST_FTDC_TC_IOC, api_.order_.TimeCondition);
    EXPECT_EQ(THOST_FTDC_VC_CV, api_.order_.VolumeCondition);

    e->setPriceType(WPT_CTP_ASK1);                // 未识别价型也落到 default
    e->setOffsetType(WOT_FORCECLOSE);
    e->setOrderFlag(WOF_FAK);
    EXPECT_EQ(0, trader_.orderInsert(e));
    EXPECT_EQ(THOST_FTDC_OPT_LastPrice, api_.order_.OrderPriceType);
    EXPECT_EQ(THOST_FTDC_OF_ForceClose, api_.order_.CombOffsetFlag[0]);
    EXPECT_EQ(THOST_FTDC_VC_AV, api_.order_.VolumeCondition);
    EXPECT_EQ(3, api_.n_insert_.load());
    EXPECT_EQ(THOST_FTDC_HF_Speculation, api_.order_.CombHedgeFlag[0]);
    EXPECT_EQ(THOST_FTDC_CC_Immediately, api_.order_.ContingentCondition);
    EXPECT_EQ(THOST_FTDC_FCC_NotForceClose, api_.order_.ForceCloseReason);
    e->release();
}

TEST_F(T25TraderCTPTest, order_insert_direction_flag_is_swapped_for_short_close)
{
    bringToReady();

    WTSEntrust* e = WTSEntrust::create(kCodeAg, 1, 7000.0, kExchgAg);
    e->setPriceType(WPT_LIMITPRICE);
    e->setDirection(WDT_LONG);
    e->setOffsetType(WOT_OPEN);
    EXPECT_EQ(0, trader_.orderInsert(e));
    EXPECT_EQ(THOST_FTDC_D_Buy, api_.order_.Direction);

    e->setDirection(WDT_LONG);
    e->setOffsetType(WOT_CLOSE);
    EXPECT_EQ(0, trader_.orderInsert(e));
    EXPECT_EQ(THOST_FTDC_D_Sell, api_.order_.Direction);

    e->setDirection(WDT_SHORT);
    e->setOffsetType(WOT_OPEN);
    EXPECT_EQ(0, trader_.orderInsert(e));
    EXPECT_EQ(THOST_FTDC_D_Sell, api_.order_.Direction);

    // 空头平仓在 CTP 侧是 Buy —— lane T2 的 cffex 用例就是把这条期望写反了才红
    e->setDirection(WDT_SHORT);
    e->setOffsetType(WOT_CLOSETODAY);
    EXPECT_EQ(0, trader_.orderInsert(e));
    EXPECT_EQ(THOST_FTDC_D_Buy, api_.order_.Direction);
    EXPECT_EQ(4, api_.n_insert_.load());

    // 中金所的市价单必须是五档价，其它交易所是 AnyPrice
    e->setDirection(WDT_LONG);
    e->setOffsetType(WOT_OPEN);
    e->setPriceType(WPT_ANYPRICE);
    EXPECT_EQ(0, trader_.orderInsert(e));
    EXPECT_EQ(THOST_FTDC_OPT_AnyPrice, api_.order_.OrderPriceType);
    WTSEntrust* cf = WTSEntrust::create(kCodeIf, 1, 3500.0, kExchgIf);
    cf->setPriceType(WPT_ANYPRICE);
    EXPECT_EQ(0, trader_.orderInsert(cf));
    EXPECT_EQ(THOST_FTDC_OPT_FiveLevelPrice, api_.order_.OrderPriceType);
    EXPECT_EQ(kCodeIf, std::string(api_.order_.InstrumentID));
    EXPECT_EQ(kExchgIf, std::string(api_.order_.ExchangeID));
    e->release();
    cf->release();
}

TEST_F(T25TraderCTPTest, order_action_rejects_entrust_id_with_only_one_hash)
{
    bringToReady();

    WTSEntrustAction* a = WTSEntrustAction::create(kCodeAg, kExchgAg);
    a->setEntrustID("012345#67");                 // 第二个 '#' 找不到 ⇒ L1385-1387
    a->setActionFlag(WAF_CANCEL);
    EXPECT_EQ(-1, trader_.orderAction(a));
    EXPECT_EQ(0, api_.n_action_.load());

    a->setEntrustID("012345#0000000067#000042");
    EXPECT_EQ(0, trader_.orderAction(a));
    EXPECT_EQ(1, api_.n_action_.load());
    EXPECT_EQ(12345, api_.action_.FrontID);
    EXPECT_EQ(67, api_.action_.SessionID);
    EXPECT_STREQ("42", api_.action_.OrderRef);
    EXPECT_EQ(THOST_FTDC_AF_Delete, api_.action_.ActionFlag);
    a->release();
}

//////////////////////////////////////////////////////////////////////////
// 7) release/disconnect 与 connect()（L281-303 / L305-366 / L368-379）
//
TEST_F(T25TraderCTPTest, disconnect_without_worker_only_queues_release)
{
    trader_.attach(&api_);
    EXPECT_FALSE(trader_.hasWorker());

    trader_.disconnect();                     // m_thrdWorker==NULL ⇒ 只压队列，不 join
    EXPECT_EQ(1u, trader_.queryDepth());
    EXPECT_FALSE(trader_.stopped());
    EXPECT_EQ(0, api_.n_release_.load());

    EXPECT_EQ(1u, trader_.drainQueries());    // 队列项就是 release()
    EXPECT_TRUE(trader_.stopped());
    EXPECT_EQ(1, api_.n_release_.load());
    EXPECT_EQ(NULL, trader_.api());
    EXPECT_FALSE(trader_.hasWorker());
}

TEST_F(T25TraderCTPTest, release_after_ready_clears_api_and_every_container)
{
    bringToReady();

    // 用生产回报路径把三个容器造出来（m_ayPosDetail 生产无分配点，用例自己造）
    CThostFtdcOrderField o;
    memset(&o, 0, sizeof(o));
    strcpy(o.InstrumentID, kCodeAg);
    strcpy(o.ExchangeID, kExchgAg);
    strcpy(o.InsertDate, "20240115");
    strcpy(o.InsertTime, "10:00:00");
    o.TimeCondition = THOST_FTDC_TC_GFD;
    trader_.OnRspQryOrder(&o, &ok_, 1, false);

    CThostFtdcTradeField t;
    memset(&t, 0, sizeof(t));
    strcpy(t.InstrumentID, kCodeAg);
    strcpy(t.ExchangeID, kExchgAg);
    strcpy(t.TradeDate, "20240115");
    strcpy(t.TradeTime, "10:00:00");
    trader_.OnRspQryTrade(&t, &ok_, 1, false);

    CThostFtdcInvestorPositionField pf;
    memset(&pf, 0, sizeof(pf));
    strcpy(pf.InstrumentID, kCodeAg);
    strcpy(pf.ExchangeID, kExchgAg);
    pf.PosiDirection = THOST_FTDC_PD_Long;
    pf.PositionDate = THOST_FTDC_PSD_Today;
    pf.Position = 3;
    trader_.OnRspQryInvestorPosition(&pf, &ok_, 1, false);

    trader_.armPosDetail();

    ASSERT_EQ(1u, trader_.ordersSize());
    ASSERT_EQ(1u, trader_.tradesSize());
    ASSERT_EQ(1u, trader_.posMapSize());
    ASSERT_EQ(0u, trader_.posDetailSize());
    ASSERT_TRUE(trader_.api() == &api_);

    trader_.release();

    EXPECT_TRUE(trader_.stopped());
    EXPECT_EQ(1, api_.n_release_.load());
    EXPECT_EQ(NULL, trader_.api());               // L289 必须清空，否则 release 后还能发请求
    EXPECT_EQ(0u, trader_.ordersSize());          // 容器仍在，只是被 clear()
    EXPECT_EQ(0u, trader_.tradesSize());
    EXPECT_EQ(0u, trader_.posMapSize());
    EXPECT_EQ(0u, trader_.posDetailSize());
}

TEST_F(T25TraderCTPTest, connect_with_stub_creator_wires_spi_fronts_and_worker_sends_query)
{
    g_api_for_creator = &api_;
    g_n_creator = 0;
    g_creator_path.clear();
    trader_.setCreator(&fakeCreator);
    sink_.logs_.clear();
    sink_.n_log_ = 0;

    trader_.connect();

    EXPECT_EQ(1, g_n_creator.load());
    // flowdir + "flows/<broker>/<user>/"
    EXPECT_EQ(tmp_.string() + "/flows/9999/88880001/", g_creator_path);
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(tmp_.string() + "/flows/9999/88880001")));
    EXPECT_TRUE(trader_.api() == &api_);
    EXPECT_EQ(1, api_.n_register_spi_.load());
    EXPECT_TRUE(api_.ptr_last_spi_ == (CThostFtdcTraderSpi*)&trader_);
    EXPECT_EQ(1, api_.n_register_front_.load());
    EXPECT_EQ("tcp://127.0.0.1:1234", api_.str_last_front_);
    EXPECT_EQ(1, api_.n_sub_public_.load());
    EXPECT_EQ(1, api_.n_sub_private_.load());
    EXPECT_EQ((int)THOST_TERT_RESUME, api_.last_sub_public_);       // quick=false
    EXPECT_EQ((int)THOST_TERT_RESUME, api_.last_sub_private_);
    EXPECT_EQ(1, api_.n_init_.load());
    EXPECT_TRUE(sink_.hasLog("registerFront: tcp://127.0.0.1:1234"));
    ASSERT_TRUE(trader_.hasWorker());

    // 第二次 connect：worker 已存在 ⇒ 走 L334 的 false 分支，不再新建线程
    trader_.setQuick(true);
    trader_.connect();
    EXPECT_EQ(2, api_.n_register_front_.load());
    EXPECT_EQ((int)THOST_TERT_QUICK, api_.last_sub_public_);
    EXPECT_EQ((int)THOST_TERT_QUICK, api_.last_sub_private_);
    EXPECT_EQ(2, api_.n_init_.load());
    EXPECT_TRUE(trader_.hasWorker());

    // 队列由 worker 线程消费（生产语义），用例只轮询观察，不同步执行
    trader_.setState(TraderCTP::WS_ALLREADY);
    EXPECT_EQ(0, trader_.queryAccount());
    ASSERT_TRUE(waitUntil([this]() { return api_.n_qry_acct_.load() >= 1; }))
        << "worker 线程没有把查询发出去";
    EXPECT_STREQ("9999", api_.qry_acct_.BrokerID);
    EXPECT_STREQ("88880001", api_.qry_acct_.InvestorID);
    // 关键：worker 把 m_bInQuery 置 true 后自己不会复位，只有回报的 bIsLast 才复位
    EXPECT_TRUE(trader_.inQuery());

    CThostFtdcTradingAccountField acct;
    memset(&acct, 0, sizeof(acct));
    acct.PreBalance = 100000;
    trader_.OnRspQryTradingAccount(&acct, &ok_, 1, true);
    EXPECT_FALSE(trader_.inQuery());

    trader_.disconnect();                     // 压 release + join（throttle 那一支会睡到 1s）
    EXPECT_TRUE(trader_.stopped());
    EXPECT_FALSE(trader_.hasWorker());
    EXPECT_EQ(1, api_.n_release_.load());
    EXPECT_EQ(NULL, trader_.api());
    EXPECT_EQ(0u, trader_.queryDepth());
}

//////////////////////////////////////////////////////////////////////////
// 8) 结算确认状态机：L828-840 / L845-859 / confirm() L1450-1474 / queryConfirm 失败日志
//
TEST_F(T25TraderCTPTest, settlement_confirm_older_than_trading_day_goes_through_confirm)
{
    bringToLogin();
    EXPECT_EQ(1u, trader_.queryDepth());          // bringToLogin 不 drain：确认查询还压在队列里
    EXPECT_EQ(0, api_.n_qry_confirm_.load());
    EXPECT_EQ(1u, trader_.drainQueries());
    EXPECT_EQ(1, api_.n_qry_confirm_.load());
    EXPECT_STREQ("9999", api_.qry_confirm_.BrokerID);
    EXPECT_STREQ("88880001", api_.qry_confirm_.InvestorID);

    // ConfirmDate < 交易日 ⇒ 只到 WS_CONFIRM_QRYED，并当场发确认请求（L828-834 + confirm()）
    CThostFtdcSettlementInfoConfirmField conf;
    memset(&conf, 0, sizeof(conf));
    strcpy(conf.ConfirmDate, "20240112");
    trader_.OnRspQrySettlementInfoConfirm(&conf, &ok_, 3, true);
    EXPECT_EQ(TraderCTP::WS_CONFIRM_QRYED, trader_.state());
    EXPECT_FALSE(trader_.isConnected());
    EXPECT_EQ(0u, sink_.n_login_result_);
    ASSERT_EQ(1, api_.n_confirm_.load());
    EXPECT_STREQ("9999", api_.confirm_.BrokerID);
    EXPECT_STREQ("88880001", api_.confirm_.InvestorID);
    // confirm() 用当天日期 + 本地时间填充（L1463-1464）
    EXPECT_EQ(8u, strlen(api_.confirm_.ConfirmDate));
    EXPECT_EQ(std::to_string(TimeUtils::getCurDate()), std::string(api_.confirm_.ConfirmDate));
    EXPECT_EQ(8u, strlen(api_.confirm_.ConfirmTime));
    EXPECT_TRUE(isdigit(api_.confirm_.ConfirmTime[0]));
    EXPECT_TRUE(isdigit(api_.confirm_.ConfirmTime[1]));
    EXPECT_EQ(':', api_.confirm_.ConfirmTime[2]);

    // 柜台确认回报 ⇒ WS_CONFIRMED→WS_ALLREADY + onLoginResult(true)（L849-857）
    CThostFtdcSettlementInfoConfirmField conf2;
    memset(&conf2, 0, sizeof(conf2));
    trader_.OnRspSettlementInfoConfirm(&conf2, &ok_, 4, true);
    EXPECT_EQ(TraderCTP::WS_ALLREADY, trader_.state());
    EXPECT_TRUE(trader_.isConnected());
    EXPECT_EQ(1u, sink_.n_login_result_);
    EXPECT_TRUE(sink_.last_succ_);
    EXPECT_EQ(20240115u, sink_.last_tradedate_);
    EXPECT_EQ("", sink_.last_msg_);
}

TEST_F(T25TraderCTPTest, settlement_confirm_query_with_null_field_or_error_keeps_or_skips_state)
{
    bringToLogin();

    // bIsLast 才复位 m_bInQuery（L808-812）；先造出「正在查询」的形状
    trader_.setInQuery(true);
    CThostFtdcRspInfoField err;
    fillErr(err, 88, "无结算确认记录");
    trader_.OnRspQrySettlementInfoConfirm(NULL, &err, 4, false);
    EXPECT_TRUE(trader_.inQuery());               // bIsLast=false ⇒ 不复位
    // 带错误的回报：整个 if 都不进，状态原地不动、也不发确认（L814 false 支）
    EXPECT_EQ(TraderCTP::WS_LOGINED, trader_.state());
    EXPECT_EQ(0, api_.n_confirm_.load());
    trader_.setState(TraderCTP::WS_LOGINED);
    trader_.OnRspQrySettlementInfoConfirm(NULL, &err, 4, true);
    EXPECT_FALSE(trader_.inQuery());              // bIsLast=true ⇒ 复位（即便带错误）
    EXPECT_EQ(TraderCTP::WS_LOGINED, trader_.state());
    EXPECT_EQ(0, api_.n_confirm_.load());         // 错误回报不许发确认

    // pSettlementInfoConfirm==NULL 且无错误 ⇒ 也进 WS_CONFIRM_QRYED 并发确认（L836-840）
    trader_.OnRspQrySettlementInfoConfirm(NULL, &ok_, 3, true);
    EXPECT_EQ(TraderCTP::WS_CONFIRM_QRYED, trader_.state());
    EXPECT_EQ(1, api_.n_confirm_.load());
}

TEST_F(T25TraderCTPTest, settlement_confirm_rsp_only_acts_in_confirm_qryed_state)
{
    bringToLogin();
    trader_.setState(TraderCTP::WS_LOGINED);

    CThostFtdcSettlementInfoConfirmField f;
    memset(&f, 0, sizeof(f));

    // 状态不对 ⇒ 什么都不做
    trader_.OnRspSettlementInfoConfirm(&f, &ok_, 1, true);
    EXPECT_EQ(TraderCTP::WS_LOGINED, trader_.state());
    EXPECT_EQ(0u, sink_.n_login_result_);

    // 状态对但带错误 ⇒ 仍不动
    trader_.setState(TraderCTP::WS_CONFIRM_QRYED);
    CThostFtdcRspInfoField err;
    fillErr(err, 90, "确认失败");
    trader_.OnRspSettlementInfoConfirm(&f, &err, 2, true);
    EXPECT_EQ(TraderCTP::WS_CONFIRM_QRYED, trader_.state());
    EXPECT_EQ(0u, sink_.n_login_result_);

    // 状态对但回报对象为 NULL ⇒ 仍不动（L847 的第二个条件）
    trader_.OnRspSettlementInfoConfirm(NULL, &ok_, 3, true);
    EXPECT_EQ(TraderCTP::WS_CONFIRM_QRYED, trader_.state());
    EXPECT_EQ(0u, sink_.n_login_result_);

    // 只有「状态对 + 无错误 + 对象非空」三者齐备才就绪
    trader_.OnRspSettlementInfoConfirm(&f, &ok_, 4, true);
    EXPECT_EQ(TraderCTP::WS_ALLREADY, trader_.state());
    EXPECT_EQ(1u, sink_.n_login_result_);
}

// ITraderSpi exposes a login-result callback but does not define how a
// synchronous settlement-confirm send failure completes login. T25-009 lists
// rollback, retry, or explicit login failure as alternatives; keep this local
// stub scenario disabled until that policy is specified. In particular, do not
// assert the current stuck state or fabricate a later successful CTP callback.
TEST_F(T25TraderCTPTest, DISABLED_confirm_send_failure_outcome_pending_contract)
{
    bringToLogin();
    api_.ret_ = 8;

    CThostFtdcSettlementInfoConfirmField conf;
    memset(&conf, 0, sizeof(conf));
    strcpy(conf.ConfirmDate, "20240101");         // 早于交易日 ⇒ 触发 confirm()
    trader_.OnRspQrySettlementInfoConfirm(&conf, &ok_, 3, true);
}

TEST_F(T25TraderCTPTest, query_confirm_send_failure_is_logged_inside_worker_item)
{
    bringToLogin();                                // 确认查询已入队，尚未执行
    api_.ret_ = 7;
    EXPECT_EQ(1u, trader_.drainQueries());
    EXPECT_EQ(1, api_.n_qry_confirm_.load());
    EXPECT_TRUE(sink_.hasLog("Sending query of settlement data confirming state failed: 7"));
    EXPECT_EQ(0, api_.n_confirm_.load());          // 只有查询失败，不该发确认
}

//////////////////////////////////////////////////////////////////////////
// 9) 持仓换算矩阵（L958-1050）：lane T2 只进了 CoverToday+Today+Long 一支
//
static CThostFtdcInvestorPositionField makePos(const char* code, const char* exchg,
                                               char posiDir, char posDate,
                                               int position, int todayPos,
                                               int longFrozen, int shortFrozen,
                                               double margin, double cost, double profit)
{
    CThostFtdcInvestorPositionField pf;
    memset(&pf, 0, sizeof(pf));
    strcpy(pf.InstrumentID, code);
    strcpy(pf.ExchangeID, exchg);
    pf.PosiDirection = posiDir;
    pf.PositionDate = posDate;
    pf.Position = position;
    pf.TodayPosition = todayPos;
    pf.LongFrozen = longFrozen;
    pf.ShortFrozen = shortFrozen;
    pf.UseMargin = margin;
    pf.PositionCost = cost;
    pf.PositionProfit = profit;
    return pf;
}

TEST_F(T25TraderCTPTest, position_cover_today_short_today_row_subtracts_long_frozen)
{
    bringToReady();
    // ag2512 空头今仓 5，多头冻结 2 ⇒ 可平今 = 5-2 = 3（L995-1001）
    CThostFtdcInvestorPositionField pf = makePos(
        kCodeAg, kExchgAg, THOST_FTDC_PD_Short, THOST_FTDC_PSD_Today,
        5, 5, 2, 0, 800.0, 35000.0, 20.0);
    // 同交易所 ag2602 多头今仓 1、空头冻结 3（多头行减的是 ShortFrozen）⇒ 1-3<0 ⇒ 夹零（L999-1000）
    CThostFtdcInvestorPositionField pf2 = makePos(
        kCodeAg2, kExchgAg, THOST_FTDC_PD_Long, THOST_FTDC_PSD_Today,
        1, 1, 0, 3, 0.0, 0.0, 0.0);
    trader_.OnRspQryInvestorPosition(&pf, &ok_, 1, false);
    trader_.OnRspQryInvestorPosition(&pf2, &ok_, 1, true);

    ASSERT_EQ(2u, sink_.pos_.size());
    const PosSnap* clamped = sink_.findPos(kCodeAg2);
    ASSERT_TRUE(clamped != NULL);
    EXPECT_DOUBLE_EQ(1.0, clamped->total_);
    EXPECT_DOUBLE_EQ(0.0, clamped->availNew_);      // 1-3<0 ⇒ 夹零
    EXPECT_DOUBLE_EQ(0.0, clamped->avg_);
    const PosSnap* p = sink_.findPos(kCodeAg);
    ASSERT_TRUE(p != NULL);
    EXPECT_EQ(kExchgAg, p->exchg_);
    EXPECT_EQ("CNY", p->currency_);
    EXPECT_EQ((int)WDT_SHORT, p->dir_);
    EXPECT_DOUBLE_EQ(5.0, p->new_);
    EXPECT_DOUBLE_EQ(0.0, p->pre_);
    EXPECT_DOUBLE_EQ(5.0, p->total_);
    EXPECT_DOUBLE_EQ(3.0, p->availNew_);
    EXPECT_DOUBLE_EQ(0.0, p->availPre_);           // 今仓那一支不碰可平昨
    EXPECT_DOUBLE_EQ(800.0, p->margin_);
    EXPECT_DOUBLE_EQ(20.0, p->dyn_);
    EXPECT_DOUBLE_EQ(35000.0, p->cost_);
    EXPECT_DOUBLE_EQ(7000.0, p->avg_);             // 35000 / 5 / volScale(1)
    EXPECT_EQ(0xFFFFu, trader_.posMapSize());      // bIsLast ⇒ map 释放并置 NULL（L1070-1074）
}

TEST_F(T25TraderCTPTest, position_cover_today_history_rows_clamp_negative_availability)
{
    bringToReady();
    // ag2512 多头昨仓 4、空头冻结 1 ⇒ 可平昨 3（L1005-1009）
    CThostFtdcInvestorPositionField pf1 = makePos(
        kCodeAg, kExchgAg, THOST_FTDC_PD_Long, THOST_FTDC_PSD_History,
        4, 0, 0, 1, 100.0, 0.0, 0.0);
    // ag2602 空头昨仓 3、多头冻结 5 ⇒ 3-5<0 ⇒ 夹到 0（L1010-1016）
    CThostFtdcInvestorPositionField pf2 = makePos(
        kCodeAg2, kExchgAg, THOST_FTDC_PD_Short, THOST_FTDC_PSD_History,
        3, 0, 5, 0, 0.0, 0.0, 0.0);
    trader_.OnRspQryInvestorPosition(&pf1, &ok_, 1, false);
    trader_.OnRspQryInvestorPosition(&pf2, &ok_, 1, false);
    EXPECT_EQ(0u, sink_.n_rsp_position_);          // 非 bIsLast 一条都不推
    trader_.OnRspQryInvestorPosition(NULL, &ok_, 1, true);

    ASSERT_EQ(1u, sink_.n_rsp_position_);
    ASSERT_EQ(2u, sink_.pos_.size());
    const PosSnap* a = sink_.findPos(kCodeAg);
    ASSERT_TRUE(a != NULL);
    EXPECT_DOUBLE_EQ(4.0, a->pre_);
    EXPECT_DOUBLE_EQ(0.0, a->new_);
    EXPECT_DOUBLE_EQ(4.0, a->total_);
    EXPECT_DOUBLE_EQ(3.0, a->availPre_);
    EXPECT_DOUBLE_EQ(0.0, a->availNew_);
    EXPECT_DOUBLE_EQ(0.0, a->avg_);                // cost=0 ⇒ avg=0
    const PosSnap* b = sink_.findPos(kCodeAg2);
    ASSERT_TRUE(b != NULL);
    EXPECT_DOUBLE_EQ(3.0, b->pre_);
    EXPECT_DOUBLE_EQ(0.0, b->availPre_);           // 负数被夹住
    EXPECT_EQ((int)WDT_SHORT, b->dir_);
}

TEST_F(T25TraderCTPTest, position_direct_cover_splits_new_pre_and_both_freeze_sides)
{
    bringToReady();
    // IF 是 CM_OpenCover ⇒ 走 L1019-1038（今仓=TodayPosition，昨仓=Position-TodayPosition）
    CThostFtdcInvestorPositionField lf = makePos(
        kCodeIf, kExchgIf, THOST_FTDC_PD_Long, THOST_FTDC_PSD_Today,
        10, 4, 0, 1, 20000.0, 350000.0, 100.0);
    CThostFtdcInvestorPositionField sf = makePos(
        kCodeIf, kExchgIf, THOST_FTDC_PD_Short, THOST_FTDC_PSD_Today,
        5, 2, 1, 0, 9000.0, 175000.0, -50.0);
    trader_.OnRspQryInvestorPosition(&lf, &ok_, 1, false);
    trader_.OnRspQryInvestorPosition(&sf, &ok_, 1, false);
    // 未知合约的行必须被忽略（L946），但 bIsLast 仍然推送
    CThostFtdcInvestorPositionField uk = makePos(
        kCodeUnk, kExchgAg, THOST_FTDC_PD_Long, THOST_FTDC_PSD_Today,
        9, 9, 0, 0, 1.0, 1.0, 1.0);
    trader_.OnRspQryInvestorPosition(&uk, &ok_, 1, false);
    // 同品种的 IF2403 空头今仓 1、多头冻结 4 ⇒ 可平今 1-4<0 ⇒ 夹零（L1030-1031）
    CThostFtdcInvestorPositionField cf = makePos(
        kCodeIf2, kExchgIf, THOST_FTDC_PD_Short, THOST_FTDC_PSD_Today,
        1, 1, 4, 0, 0.0, 0.0, 0.0);
    trader_.OnRspQryInvestorPosition(&cf, &ok_, 1, false);
    trader_.OnRspQryInvestorPosition(NULL, &ok_, 1, true);

    ASSERT_EQ(3u, sink_.pos_.size());
    const PosSnap* c2 = sink_.findPos(kCodeIf2, WDT_SHORT);
    ASSERT_TRUE(c2 != NULL);
    EXPECT_DOUBLE_EQ(0.0, c2->availNew_);            // 夹零
    EXPECT_DOUBLE_EQ(-3.0, c2->availPre_);           // 这一支只夹可平今，可平昨原样落负数
    EXPECT_TRUE(sink_.findPos(kCodeUnk) == NULL);
    const PosSnap* l = sink_.findPos(kCodeIf, WDT_LONG);
    ASSERT_TRUE(l != NULL);
    EXPECT_DOUBLE_EQ(4.0, l->new_);
    EXPECT_DOUBLE_EQ(6.0, l->pre_);
    EXPECT_DOUBLE_EQ(10.0, l->total_);
    EXPECT_DOUBLE_EQ(3.0, l->availNew_);           // 4 - ShortFrozen(1)
    EXPECT_DOUBLE_EQ(6.0, l->availPre_);           // 10 - 0 - 1 - 3
    EXPECT_DOUBLE_EQ(700.0, l->avg_);              // 350000 / 10 / volScale(50)
    const PosSnap* s = sink_.findPos(kCodeIf, WDT_SHORT);
    ASSERT_TRUE(s != NULL);
    EXPECT_DOUBLE_EQ(2.0, s->new_);
    EXPECT_DOUBLE_EQ(3.0, s->pre_);
    EXPECT_DOUBLE_EQ(1.0, s->availNew_);           // 2 - LongFrozen(1)
    EXPECT_DOUBLE_EQ(3.0, s->availPre_);           // 5 - 1 - 0 - 1
    EXPECT_DOUBLE_EQ(-50.0, s->dyn_);
    EXPECT_EQ((int)WDT_SHORT, s->dir_);
    EXPECT_EQ(kExchgIf, s->exchg_);
}

TEST_F(T25TraderCTPTest, position_combination_skips_availability_and_zero_margin_clears_it)
{
    bringToReady();
    // SPD 是 CC_Combination ⇒ 跳过可用持仓计算（L1040-1043）
    CThostFtdcInvestorPositionField pf1 = makePos(
        kCodeSpd, kExchgSpd, THOST_FTDC_PD_Long, THOST_FTDC_PSD_Today,
        5, 5, 0, 0, 0.0, 0.0, 0.0);
    // 同 key 再补一条昨仓 -8 ⇒ 总持仓 -3、保证金 0 ⇒ 命中 L1045-1050 的套利合约兜底
    CThostFtdcInvestorPositionField pf2 = makePos(
        kCodeSpd, kExchgSpd, THOST_FTDC_PD_Long, THOST_FTDC_PSD_History,
        -8, 0, 0, 0, 0.0, 0.0, 0.0);
    trader_.OnRspQryInvestorPosition(&pf1, &ok_, 1, false);
    trader_.OnRspQryInvestorPosition(&pf2, &ok_, 1, true);

    ASSERT_EQ(1u, sink_.pos_.size());
    const PosSnap* p = &sink_.pos_[0];
    EXPECT_DOUBLE_EQ(5.0, p->new_);
    EXPECT_DOUBLE_EQ(-8.0, p->pre_);
    EXPECT_DOUBLE_EQ(-3.0, p->total_);
    EXPECT_DOUBLE_EQ(0.0, p->availNew_);
    EXPECT_DOUBLE_EQ(0.0, p->availPre_);
    EXPECT_DOUBLE_EQ(0.0, p->margin_);
}

TEST_F(T25TraderCTPTest, position_zero_total_sets_avg_price_to_zero_and_still_pushes)
{
    bringToReady();
    CThostFtdcInvestorPositionField pf = makePos(
        kCodeAg, kExchgAg, THOST_FTDC_PD_Long, THOST_FTDC_PSD_Today,
        0, 0, 0, 0, 12.5, 0.0, 3.0);
    trader_.OnRspQryInvestorPosition(&pf, &ok_, 1, true);

    ASSERT_EQ(1u, sink_.pos_.size());
    const PosSnap* p = sink_.findPos(kCodeAg);
    ASSERT_TRUE(p != NULL);
    EXPECT_DOUBLE_EQ(0.0, p->total_);
    EXPECT_DOUBLE_EQ(0.0, p->avg_);                // L979-982：总持仓 0 ⇒ 均价强制 0
    EXPECT_DOUBLE_EQ(12.5, p->margin_);            // 保证金与持仓无关，仍然累加
    EXPECT_DOUBLE_EQ(0.0, p->availNew_);
}

TEST_F(T25TraderCTPTest, position_error_last_row_pushs_accumulated_then_map_is_reset)
{
    bringToReady();
    CThostFtdcInvestorPositionField pf = makePos(
        kCodeAg, kExchgAg, THOST_FTDC_PD_Long, THOST_FTDC_PSD_Today,
        2, 2, 0, 0, 10.0, 14000.0, 0.0);
    trader_.OnRspQryInvestorPosition(&pf, &ok_, 1, false);
    ASSERT_EQ(1u, trader_.posMapSize());

    CThostFtdcRspInfoField err;
    fillErr(err, 5, "查询失败");
    // 带错误的 bIsLast：不进累加（L940 false），但仍然推一次并把 map 释放
    trader_.OnRspQryInvestorPosition(NULL, &err, 1, true);
    EXPECT_EQ(1u, sink_.n_rsp_position_);
    EXPECT_EQ(1u, sink_.pos_.size());
    EXPECT_EQ(0xFFFFu, trader_.posMapSize());

    // 第二次 bIsLast：map 已空 ⇒ 推 0 条（L1059 false 支）
    trader_.OnRspQryInvestorPosition(NULL, &ok_, 1, true);
    EXPECT_EQ(2u, sink_.n_rsp_position_);
    EXPECT_EQ(0u, sink_.pos_.size());
}

//////////////////////////////////////////////////////////////////////////
// 10) 委托/成交回报转换的剩支：eid→oid 缓存链、OrderSubmitStatus、MV、价型与开平回映射
//
TEST_F(T25TraderCTPTest, usertag_round_trips_from_eid_cache_into_order_then_trade)
{
    bringToReady();

    char eid[64];
    ASSERT_TRUE(trader_.makeEntrustID(eid, sizeof(eid)));
    EXPECT_STREQ("012345#0000000067#001001", eid);      // L381-399 定宽格式（T2 把它写反过）

    WTSEntrust* e = WTSEntrust::create(kCodeAg, 3, 7000.0, kExchgAg);
    e->setEntrustID(eid);
    e->setUserTag("TAG-25");
    e->setDirection(WDT_LONG);
    e->setOffsetType(WOT_OPEN);
    e->setPriceType(WPT_LIMITPRICE);
    EXPECT_EQ(0, trader_.orderInsert(e));
    EXPECT_STREQ("1001", api_.order_.OrderRef);          // entrustID 尾段回填
    EXPECT_EQ("TAG-25", trader_.eidValue(eid));
    EXPECT_EQ(1u, trader_.eidSize());
    EXPECT_EQ(0u, trader_.oidSize());                    // 此刻还没有订单编号，oid 不该有内容
    e->release();

    CThostFtdcOrderField o;
    memset(&o, 0, sizeof(o));
    strcpy(o.InstrumentID, kCodeAg);
    strcpy(o.ExchangeID, kExchgAg);
    o.FrontID = 12345;
    o.SessionID = 67;
    strcpy(o.OrderRef, "1001");
    strcpy(o.OrderSysID, "800001");
    o.LimitPrice = 7000.0;
    o.VolumeTotalOriginal = 3;
    o.VolumeTraded = 1;
    o.Direction = THOST_FTDC_D_Buy;
    o.CombOffsetFlag[0] = THOST_FTDC_OF_Open;
    o.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
    o.TimeCondition = THOST_FTDC_TC_GFD;
    o.VolumeCondition = THOST_FTDC_VC_AV;
    o.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
    o.OrderSubmitStatus = THOST_FTDC_OSS_Accepted;
    strcpy(o.InsertDate, "20240115");
    strcpy(o.InsertTime, "10:00:00");
    strcpy(o.StatusMsg, "已受理");
    trader_.OnRtnOrder(&o);

    ASSERT_EQ(1u, sink_.n_push_order_);
    EXPECT_EQ(eid, sink_.ord_eid_);
    EXPECT_EQ("TAG-25", sink_.ord_usertag_);             // eid 缓存命中 ⇒ L1250-1252
    EXPECT_FALSE(sink_.ord_is_error_);                   // OrderSubmitStatus < InsertRejected ⇒ 不置错误
    EXPECT_DOUBLE_EQ(2.0, sink_.ord_left_);
    EXPECT_EQ("800001", sink_.ord_oid_);
    EXPECT_EQ("已受理", sink_.ord_statemsg_);
    EXPECT_EQ(20240115u, sink_.ord_date_);
    EXPECT_EQ(1u, trader_.oidSize());                    // 有 OrderSysID ⇒ 写 oid 缓存（L1254-1257）
    EXPECT_EQ("TAG-25", trader_.oidValue("800001"));

    CThostFtdcTradeField t;
    memset(&t, 0, sizeof(t));
    strcpy(t.InstrumentID, kCodeAg);
    strcpy(t.ExchangeID, kExchgAg);
    strcpy(t.TradeID, "T0001");
    strcpy(t.OrderSysID, "800001");
    t.Price = 7000.0;
    t.Volume = 1;
    t.Direction = THOST_FTDC_D_Buy;
    t.OffsetFlag = THOST_FTDC_OF_Open;
    strcpy(t.TradeDate, "20240115");
    strcpy(t.TradeTime, "10:05:00");
    trader_.OnRtnTrade(&t);

    ASSERT_EQ(1u, sink_.n_push_trade_);
    EXPECT_EQ("TAG-25", sink_.trd_usertag_);             // oid 缓存命中 ⇒ L1361-1363
    EXPECT_DOUBLE_EQ(7000.0, sink_.trd_amount_);         // volScale(1)*1*7000
    EXPECT_EQ((int)WDT_LONG, sink_.trd_dir_);
    EXPECT_EQ((int)WOT_OPEN, sink_.trd_offset_);
    EXPECT_EQ("800001", sink_.trd_ref_);
    EXPECT_EQ(20240115u, sink_.trd_date_);
}

TEST_F(T25TraderCTPTest, order_rsp_maps_rejected_submit_status_and_mv_volume_and_reverse_types)
{
    bringToReady();

    CThostFtdcOrderField o;
    memset(&o, 0, sizeof(o));
    strcpy(o.InstrumentID, kCodeAg);
    strcpy(o.ExchangeID, kExchgAg);
    o.FrontID = 1;
    o.SessionID = 2;
    strcpy(o.OrderRef, "7");
    o.LimitPrice = 7100.0;
    o.VolumeTotalOriginal = 4;
    o.VolumeTraded = 0;
    o.Direction = THOST_FTDC_D_Sell;
    o.OrderPriceType = THOST_FTDC_OPT_BestPrice;
    o.TimeCondition = THOST_FTDC_TC_IOC;
    o.VolumeCondition = THOST_FTDC_VC_MV;             // ⇒ FAK（L1211-1212 的第二个条件）
    o.OrderStatus = THOST_FTDC_OST_PartTradedQueueing;
    o.OrderSubmitStatus = THOST_FTDC_OSS_InsertRejected;  // ⇒ setError（L1236-1237）
    strcpy(o.InsertDate, "20240112");
    strcpy(o.InsertTime, "09:00:00");

    o.CombOffsetFlag[0] = THOST_FTDC_OF_CloseToday;
    trader_.OnRtnOrder(&o);
    ASSERT_EQ(1u, sink_.n_push_order_);
    EXPECT_EQ((int)WOF_FAK, sink_.ord_flag_);
    EXPECT_EQ((int)WPT_BESTPRICE, sink_.ord_pxtype_);     // L134-135
    EXPECT_EQ((int)WOT_CLOSETODAY, sink_.ord_offset_);    // L103-104
    EXPECT_EQ((int)WDT_LONG, sink_.ord_dir_);             // Sell + CloseToday ⇒ 多头平仓
    EXPECT_TRUE(sink_.ord_is_error_);
    EXPECT_DOUBLE_EQ(4.0, sink_.ord_left_);
    EXPECT_EQ(20240112u, sink_.ord_date_);                // uDate != m_lDate ⇒ 夜盘修正不触发

    o.CombOffsetFlag[0] = THOST_FTDC_OF_CloseYesterday;
    o.OrderPriceType = THOST_FTDC_OPT_AskPrice1;        // 未识别 ⇒ WPT_LASTPRICE
    o.VolumeCondition = THOST_FTDC_VC_CV;               // ⇒ FOK
    trader_.OnRtnOrder(&o);
    EXPECT_EQ(2u, sink_.n_push_order_);
    EXPECT_EQ((int)WOT_CLOSEYESTERDAY, sink_.ord_offset_);
    EXPECT_EQ((int)WPT_LASTPRICE, sink_.ord_pxtype_);
    EXPECT_EQ((int)WOF_FOK, sink_.ord_flag_);

    o.CombOffsetFlag[0] = THOST_FTDC_OF_ForceClose;
    o.OrderSubmitStatus = THOST_FTDC_OSS_Accepted;
    trader_.OnRtnOrder(&o);
    EXPECT_EQ(3u, sink_.n_push_order_);
    EXPECT_EQ((int)WOT_FORCECLOSE, sink_.ord_offset_);
    EXPECT_FALSE(sink_.ord_is_error_);
    EXPECT_EQ("000001#0000000002#000007", sink_.ord_eid_);   // 用回报里的 front/session/ref
    EXPECT_EQ(sink_.ord_eid_, sink_.ord_usertag_);            // 缓存没命中 ⇒ 回落自身
}

TEST_F(T25TraderCTPTest, entrust_rsp_maps_ioc_cv_to_fok_and_usertag_from_cache)
{
    bringToReady();

    char eid[64];
    ASSERT_TRUE(trader_.makeEntrustID(eid, sizeof(eid)));
    WTSEntrust* e = WTSEntrust::create(kCodeAg, 2, 7200.0, kExchgAg);
    e->setEntrustID(eid);
    e->setUserTag("TAG-E");
    EXPECT_EQ(0, trader_.orderInsert(e));
    e->release();

    CThostFtdcInputOrderField io;
    memset(&io, 0, sizeof(io));
    strcpy(io.InstrumentID, kCodeAg);
    strcpy(io.ExchangeID, kExchgAg);
    strcpy(io.OrderRef, "1001");
    io.Direction = THOST_FTDC_D_Sell;
    io.CombOffsetFlag[0] = THOST_FTDC_OF_Close;
    io.OrderPriceType = THOST_FTDC_OPT_FiveLevelPrice;
    io.TimeCondition = THOST_FTDC_TC_IOC;
    io.VolumeCondition = THOST_FTDC_VC_CV;
    io.LimitPrice = 7200.0;
    io.VolumeTotalOriginal = 2;

    CThostFtdcRspInfoField err;
    fillErr(err, 15, "资金不足");
    trader_.OnErrRtnOrderInsert(&io, &err);
    ASSERT_EQ(1u, sink_.n_rsp_entrust_);
    EXPECT_EQ(kCodeAg, sink_.ent_code_);
    EXPECT_EQ(kExchgAg, sink_.ent_exchg_);
    EXPECT_EQ(eid, sink_.ent_eid_);
    EXPECT_EQ("TAG-E", sink_.ent_usertag_);             // L1296-1298
    EXPECT_EQ((int)WOF_FOK, sink_.ent_flag_);           // L1285-1291
    EXPECT_EQ((int)WPT_ANYPRICE, sink_.ent_pxtype_);    // FiveLevelPrice 归到市价
    EXPECT_EQ((int)WDT_LONG, sink_.ent_dir_);           // Sell + Close ⇒ 多头平仓（L66-69 的反向映射）
    EXPECT_EQ((int)WOT_CLOSE, sink_.ent_offset_);
    EXPECT_DOUBLE_EQ(2.0, sink_.ent_vol_);
    EXPECT_DOUBLE_EQ(7200.0, sink_.ent_price_);
    EXPECT_EQ("资金不足(15)", sink_.ent_err_);          // makeError 的定式（L1317-1321）

    io.VolumeCondition = THOST_FTDC_VC_MV;              // IOC + MV ⇒ FAK（L1287-1288 第二个条件）
    io.TimeCondition = THOST_FTDC_TC_IOC;
    trader_.OnErrRtnOrderInsert(&io, &err);
    EXPECT_EQ(2u, sink_.n_rsp_entrust_);
    EXPECT_EQ((int)WOF_FAK, sink_.ent_flag_);

    io.VolumeCondition = THOST_FTDC_VC_AV;              // IOC + AV ⇒ 同样 FAK
    trader_.OnErrRtnOrderInsert(&io, &err);
    EXPECT_EQ(3u, sink_.n_rsp_entrust_);
    EXPECT_EQ((int)WOF_FAK, sink_.ent_flag_);

    io.TimeCondition = THOST_FTDC_TC_GFD;               // ⇒ NOR
    trader_.OnErrRtnOrderInsert(&io, &err);
    EXPECT_EQ(4u, sink_.n_rsp_entrust_);
    EXPECT_EQ((int)WOF_NOR, sink_.ent_flag_);
}

TEST_F(T25TraderCTPTest, rsp_order_insert_with_unknown_contract_pushes_only_trader_error)
{
    bringToReady();

    CThostFtdcInputOrderField io;
    memset(&io, 0, sizeof(io));
    strcpy(io.InstrumentID, kCodeUnk);
    strcpy(io.ExchangeID, kExchgAg);
    CThostFtdcRspInfoField err;
    fillErr(err, 20, "不存在合约");

    // makeEntrust 返回 NULL ⇒ 只走 L873-879 的 onTraderError，entrust 通道一条不出
    trader_.OnRspOrderInsert(&io, &err, 9, true);
    EXPECT_EQ(0u, sink_.n_rsp_entrust_);
    EXPECT_EQ(1u, sink_.n_trader_error_);
    EXPECT_EQ("不存在合约(20)", sink_.err_msg_);

    // 同一条回报若没有错误码，则两个分支都不进 ⇒ 完全静默
    sink_.n_trader_error_ = 0;
    trader_.OnRspOrderInsert(&io, &ok_, 10, true);
    EXPECT_EQ(0u, sink_.n_trader_error_);
    EXPECT_EQ(0u, sink_.n_rsp_entrust_);

    // OnErrRtnOrderInsert 没有 else-if 兜底：未知合约 + 错误 ⇒ 这条错误被丢掉（L1397-1409）
    trader_.OnErrRtnOrderInsert(&io, &err);
    EXPECT_EQ(0u, sink_.n_rsp_entrust_);
    EXPECT_EQ(0u, sink_.n_trader_error_);
}

TEST_F(T25TraderCTPTest, rsp_order_action_error_carries_action_fields_and_bare_message)
{
    bringToReady();

    char eid[64];
    ASSERT_TRUE(trader_.makeEntrustID(eid, sizeof(eid)));
    WTSEntrust* e = WTSEntrust::create(kCodeAg, 1, 7000.0, kExchgAg);
    e->setEntrustID(eid);
    e->setUserTag("TAG-C");
    EXPECT_EQ(0, trader_.orderInsert(e));
    e->release();

    CThostFtdcInputOrderActionField ia;
    memset(&ia, 0, sizeof(ia));
    strcpy(ia.InstrumentID, kCodeAg);
    strcpy(ia.ExchangeID, kExchgAg);
    strcpy(ia.OrderSysID, "800002");
    ia.FrontID = 12345;
    ia.SessionID = 67;
    strcpy(ia.OrderRef, "1001");

    CThostFtdcRspInfoField err;
    fillErr(err, 30, "撤单被拒");
    trader_.OnRspOrderAction(&ia, &err, 11, true);

    EXPECT_EQ(1u, sink_.n_trader_error_);
    // 与 makeError 不同，这里直接拿 ErrorMsg，不带 "(ErrorID)" 后缀（L886）
    EXPECT_EQ("撤单被拒", sink_.err_msg_);
    EXPECT_TRUE(sink_.has_action_);
    EXPECT_EQ(kCodeAg, sink_.act_code_);
    EXPECT_EQ(kExchgAg, sink_.act_exchg_);
    EXPECT_EQ(eid, sink_.act_eid_);                     // generateEntrustID（L1308）
    EXPECT_EQ("TAG-C", sink_.act_usertag_);             // L1310-1312
    EXPECT_EQ("800002", sink_.act_oid_);

    // 没有错误码时一条回报都不发（L884 false 支）
    trader_.OnRspOrderAction(&ia, &ok_, 12, true);
    EXPECT_EQ(1u, sink_.n_trader_error_);
}

TEST_F(T25TraderCTPTest, trade_with_unknown_contract_is_dropped_by_both_channels)
{
    bringToReady();

    CThostFtdcTradeField t;
    memset(&t, 0, sizeof(t));
    strcpy(t.InstrumentID, kCodeUnk);
    strcpy(t.ExchangeID, kExchgAg);
    strcpy(t.TradeDate, "20240115");
    strcpy(t.TradeTime, "10:00:00");
    t.Price = 7000.0;
    t.Volume = 1;

    trader_.OnRtnTrade(&t);                             // makeTradeInfo ⇒ NULL ⇒ 不推（L1325-1327）
    EXPECT_EQ(0u, sink_.n_push_trade_);
    trader_.OnRspQryTrade(&t, &ok_, 1, true);           // 累加不进，bIsLast 仍回调
    EXPECT_EQ(1u, sink_.n_rsp_trades_);
    EXPECT_EQ(0u, sink_.trades_size_);
}

TEST_F(T25TraderCTPTest, qry_order_and_trade_rows_accumulate_and_clear_after_last)
{
    bringToReady();

    CThostFtdcOrderField o;
    memset(&o, 0, sizeof(o));
    strcpy(o.InstrumentID, kCodeAg);
    strcpy(o.ExchangeID, kExchgAg);
    o.FrontID = 3;
    o.SessionID = 4;
    strcpy(o.OrderRef, "9");
    o.LimitPrice = 7000.0;
    o.VolumeTotalOriginal = 1;
    o.TimeCondition = THOST_FTDC_TC_GFD;
    o.OrderStatus = THOST_FTDC_OST_AllTraded;
    strcpy(o.InsertDate, "20240115");
    strcpy(o.InsertTime, "10:00:00");

    trader_.OnRspQryOrder(&o, &ok_, 1, false);
    EXPECT_EQ(1u, trader_.ordersSize());                // 数组在 bIsLast 之前一直留着
    strcpy(o.OrderRef, "10");
    trader_.OnRspQryOrder(&o, &ok_, 1, false);
    EXPECT_EQ(2u, trader_.ordersSize());

    CThostFtdcRspInfoField err;
    fillErr(err, 2, "查询被拒");
    // 错误行：不追加（L1137 false），bIsLast ⇒ 把已经攒的 2 条推出去并 clear
    trader_.OnRspQryOrder(NULL, &err, 1, true);
    EXPECT_EQ(1u, sink_.n_rsp_orders_);
    EXPECT_EQ(2u, sink_.orders_size_);
    EXPECT_EQ(0u, trader_.ordersSize());                // 数组没销毁，只是清空
    EXPECT_FALSE(trader_.inQuery());

    CThostFtdcTradeField t;
    memset(&t, 0, sizeof(t));
    strcpy(t.InstrumentID, kCodeAg);
    strcpy(t.ExchangeID, kExchgAg);
    strcpy(t.TradeID, "T1");
    strcpy(t.TradeDate, "20240115");
    strcpy(t.TradeTime, "21:10:00");                    // 夜盘 + 等于交易日 ⇒ 回退一个自然日
    t.Price = 7000.0;
    t.Volume = 2;
    t.Direction = THOST_FTDC_D_Sell;
    t.OffsetFlag = THOST_FTDC_OF_Open;
    trader_.OnRspQryTrade(&t, &ok_, 2, false);
    EXPECT_EQ(1u, trader_.tradesSize());
    // bIsLast 那一行也先 append 再推 ⇒ 推出 2 条，随后数组清空
    trader_.OnRspQryTrade(&t, &ok_, 2, true);
    EXPECT_EQ(1u, sink_.n_rsp_trades_);
    EXPECT_EQ(2u, sink_.trades_size_);
    EXPECT_EQ(0u, trader_.tradesSize());
}

TEST_F(T25TraderCTPTest, settlement_info_concatenates_rows_and_pushes_only_when_non_empty)
{
    bringToReady();
    EXPECT_EQ(0u, sink_.n_settlement_);

    CThostFtdcSettlementInfoField s1, s2;
    memset(&s1, 0, sizeof(s1));
    memset(&s2, 0, sizeof(s2));
    strcpy(s1.TradingDay, "20240115");
    strcpy(s1.Content, "line-A");
    strcpy(s2.TradingDay, "20240115");
    strcpy(s2.Content, "line-B");
    trader_.OnRspQrySettlementInfo(&s1, &ok_, 1, false);
    trader_.OnRspQrySettlementInfo(&s2, &ok_, 1, true);
    EXPECT_EQ(1u, sink_.n_settlement_);
    EXPECT_EQ(20240115u, sink_.settle_date_);
    EXPECT_EQ("line-Aline-B", sink_.settle_content_);

    // querySettlement 先清空缓冲（L690）；之后带错误的回报不该拼进内容
    EXPECT_EQ(0, trader_.querySettlement(20240116));
    EXPECT_EQ(1u, trader_.drainQueries());
    EXPECT_STREQ("20240116", api_.qry_settle_.TradingDay);
    CThostFtdcRspInfoField err;
    fillErr(err, 3, "没有结算单");
    trader_.OnRspQrySettlementInfo(&s1, &err, 2, true);
    EXPECT_EQ(1u, sink_.n_settlement_);                 // 缓冲为空 ⇒ 不推
}

TEST_F(T25TraderCTPTest, unsupported_time_condition_keeps_other_order_mappings)
{
    bringToReady();

    // WTSOrderInfo's default constructor does not initialize m_orderFlag, and
    // makeOrderInfo leaves it untouched for GTD. Do not read that indeterminate
    // value through the recording sink; keep checking the fields this callback
    // explicitly maps.
    sink_.capture_ord_flag_ = false;
    CThostFtdcOrderField o;
    memset(&o, 0, sizeof(o));
    strcpy(o.InstrumentID, kCodeAg);
    strcpy(o.ExchangeID, kExchgAg);
    o.FrontID = 1;
    o.SessionID = 2;
    strcpy(o.OrderRef, "30");
    o.LimitPrice = 7000.0;
    o.VolumeTotalOriginal = 1;
    o.Direction = THOST_FTDC_D_Buy;
    o.CombOffsetFlag[0] = THOST_FTDC_OF_Open;
    o.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
    o.TimeCondition = THOST_FTDC_TC_GTD;              // '4'：既不是 GFD 也不是 IOC
    o.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
    strcpy(o.InsertDate, "20240115");
    strcpy(o.InsertTime, "10:00:00");
    trader_.OnRtnOrder(&o);

    ASSERT_EQ(1u, sink_.n_push_order_);
    EXPECT_EQ((int)WPT_LIMITPRICE, sink_.ord_pxtype_);
    EXPECT_EQ((int)WOT_OPEN, sink_.ord_offset_);
    EXPECT_EQ((int)WDT_LONG, sink_.ord_dir_);

    // makeEntrust's non-GFD/IOC default is initialized, but this case asserts
    // only the direction/offset/price mappings shared with the order callback.
    CThostFtdcInputOrderField io;
    memset(&io, 0, sizeof(io));
    strcpy(io.InstrumentID, kCodeAg);
    strcpy(io.ExchangeID, kExchgAg);
    strcpy(io.OrderRef, "31");
    io.TimeCondition = THOST_FTDC_TC_GTD;
    io.VolumeCondition = THOST_FTDC_VC_AV;
    io.LimitPrice = 7000.0;
    io.VolumeTotalOriginal = 1;
    io.Direction = THOST_FTDC_D_Buy;
    io.CombOffsetFlag[0] = THOST_FTDC_OF_Open;
    io.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
    trader_.OnErrRtnOrderInsert(&io, &ok_);
    ASSERT_EQ(1u, sink_.n_rsp_entrust_);
    EXPECT_EQ((int)WPT_LIMITPRICE, sink_.ent_pxtype_);
    EXPECT_EQ((int)WOT_OPEN, sink_.ent_offset_);
    EXPECT_EQ((int)WDT_LONG, sink_.ent_dir_);
}

TEST_F(T25TraderCTPTest, front_and_heartbeat_paths_are_quiet_when_sink_is_unregistered)
{
    // write_log 的 sink==NULL 早退（L32-33）：注册空 sink 后所有日志路径都不能崩
    trader_.registerSpi(NULL);
    EXPECT_EQ(NULL, trader_.sink());

    EXPECT_EQ(-1, trader_.orderInsert(NULL));           // L471-475 会 write_log(NULL,...)
    EXPECT_EQ(TraderCTP::WS_NOTLOGIN, trader_.state());
    trader_.OnFrontConnected();
    trader_.OnFrontDisconnected(108);
    trader_.OnRspUserLogout(NULL, NULL, 1, true);
    trader_.OnHeartBeatWarning(30);
    EXPECT_EQ(0u, sink_.n_log_);                        // 事件确实没往旧 sink 上打

    // 认证失败分支：日志与 onLoginResult 都判空，但状态仍要落
    trader_.attach(&api_);
    CThostFtdcRspAuthenticateField authf;
    memset(&authf, 0, sizeof(authf));
    CThostFtdcRspInfoField err;
    fillErr(err, 11, "AuthCode invalid");
    trader_.OnRspAuthenticate(&authf, &err, 1, true);
    EXPECT_EQ(TraderCTP::WS_LOGINFAILED, trader_.state());
    // 事件与登录结果一条都不许发出去
    EXPECT_EQ(0u, sink_.n_events_.size());
    EXPECT_EQ(0u, sink_.n_login_result_);
    // 重新登记后事件恢复
    trader_.registerSpi(&sink_);
    trader_.OnFrontConnected();
    ASSERT_EQ(1u, sink_.n_events_.size());
    EXPECT_EQ((int)WTE_Connect, sink_.n_events_[0].first);
}

TEST_F(T25TraderCTPTest, front_disconnect_resets_state_and_logout_reports_reason)
{
    bringToLogin();
    ASSERT_EQ(1u, trader_.queryDepth());       // Settlement-confirm query is queued, not answered.
    EXPECT_EQ(0, api_.n_qry_confirm_.load());
    ASSERT_EQ(1u, trader_.drainQueries());
    ASSERT_EQ(1, api_.n_qry_confirm_.load());  // Stub accepted the request; no callback has arrived.
    EXPECT_STREQ("88880001", api_.qry_confirm_.InvestorID);

    trader_.OnFrontDisconnected(200);
    EXPECT_EQ(TraderCTP::WS_NOTLOGIN, trader_.state());
    EXPECT_FALSE(trader_.isConnected());
    ASSERT_EQ(1u, sink_.n_events_.size());
    EXPECT_EQ((int)WTE_Close, sink_.n_events_[0].first);
    EXPECT_EQ(200, sink_.n_events_[0].second);

    // Conditional stub ordering test: deliver the first response for the request
    // accepted above, after disconnect. This does not claim the CTP SDK guarantees
    // that an in-flight callback arrives after a front disconnect.
    CThostFtdcSettlementInfoConfirmField conf;
    memset(&conf, 0, sizeof(conf));
    strcpy(conf.ConfirmDate, "20240115");
    const uint32_t successfulLoginsBeforeLateReply = sink_.n_login_result_;
    const int loginRequestsBeforeLateReply = api_.n_login_.load();
    trader_.OnRspQrySettlementInfoConfirm(&conf, &ok_, 5, true);
    EXPECT_EQ(TraderCTP::WS_NOTLOGIN, trader_.state());
    EXPECT_FALSE(trader_.isConnected());
    EXPECT_EQ(successfulLoginsBeforeLateReply, sink_.n_login_result_);
    EXPECT_EQ(loginRequestsBeforeLateReply, api_.n_login_.load());

    // 登出回报 ⇒ WS_NOTLOGIN + WTE_Logout
    EXPECT_EQ(0, trader_.logout());
    EXPECT_EQ(1, api_.n_logout_.load());
    EXPECT_STREQ("9999", api_.logout_.BrokerID);
    EXPECT_STREQ("88880001", api_.logout_.UserID);
    trader_.OnRspUserLogout(NULL, NULL, 6, true);
    EXPECT_EQ(TraderCTP::WS_NOTLOGIN, trader_.state());
    EXPECT_FALSE(trader_.isConnected());
    // 就绪链本身不发事件，所以这里只有 Close + Logout 两条
    ASSERT_EQ(2u, sink_.n_events_.size());
    EXPECT_EQ((int)WTE_Close, sink_.n_events_[0].first);
    EXPECT_EQ((int)WTE_Logout, sink_.n_events_[1].first);
    EXPECT_EQ(0, sink_.n_events_[1].second);

    // 心跳只写日志，不动状态、不发事件
    sink_.logs_.clear();
    trader_.OnHeartBeatWarning(15);
    EXPECT_EQ(TraderCTP::WS_NOTLOGIN, trader_.state());
    ASSERT_EQ(1u, sink_.logs_.size());
    EXPECT_TRUE(sink_.hasLog("[TraderCTP][9999-88880001] Heartbeating..."));

    // OnRspError 只把「请求号 + 错误码 + 文本」拼成一条日志（L1159-1162）
    sink_.logs_.clear();
    CThostFtdcRspInfoField err;
    fillErr(err, 15, "资金不足");
    trader_.OnRspError(&err, 4211, true);
    ASSERT_EQ(1u, sink_.logs_.size());
    EXPECT_EQ("4211 rsp error: 15 : 资金不足", sink_.last_log_);

    // 合约状态回报原样强转（L1411-1415）
    CThostFtdcInstrumentStatusField is;
    memset(&is, 0, sizeof(is));
    strcpy(is.ExchangeID, kExchgAg);
    strcpy(is.InstrumentID, kCodeAg);
    is.InstrumentStatus = THOST_FTDC_IS_Continous;
    trader_.OnRtnInstrumentStatus(&is);
    EXPECT_EQ(1u, sink_.n_inst_status_);
    EXPECT_EQ(kExchgAg, sink_.inst_exchg_);
    EXPECT_EQ(kCodeAg, sink_.inst_code_);
    EXPECT_EQ((int)TS_Continous, sink_.inst_status_);
}

TEST_F(T25TraderCTPTest, confirm_guard_rejects_when_api_is_gone_or_state_moved_on)
{
    bringToLogin();
    EXPECT_EQ(1u, trader_.drainQueries());

    // 先让状态进 WS_CONFIRM_QRYED，再把柜台指针摘掉：confirm() 的判空必须挡住请求
    CThostFtdcSettlementInfoConfirmField conf;
    memset(&conf, 0, sizeof(conf));
    strcpy(conf.ConfirmDate, "20240101");
    trader_.OnRspQrySettlementInfoConfirm(&conf, &ok_, 3, true);
    ASSERT_EQ(TraderCTP::WS_CONFIRM_QRYED, trader_.state());
    ASSERT_EQ(1, api_.n_confirm_.load());

    trader_.detach();
    // 状态已经是 CONFIRM_QRYED（不是 CONFIRM_QRYED 才发得出去），所以这里靠 L1452 的
    // m_pUserAPI==NULL 挡住；重复进入同一状态也要照样挡住，不能再发一次确认
    CThostFtdcSettlementInfoConfirmField conf2;
    memset(&conf2, 0, sizeof(conf2));
    strcpy(conf2.ConfirmDate, "20240101");
    trader_.OnRspQrySettlementInfoConfirm(&conf2, &ok_, 4, true);
    EXPECT_EQ(TraderCTP::WS_CONFIRM_QRYED, trader_.state());
    EXPECT_EQ(1, api_.n_confirm_.load());              // 一次都没多发
    EXPECT_EQ(0u, sink_.n_login_result_);
}

TEST_F(T25TraderCTPTest, login_warns_and_continues_when_cache_dir_is_not_writable)
{
    namespace fs = std::filesystem;
    // 预先把 flowdir/local/<broker>/ 建成只读目录 ⇒ 两个 kv 缓存都建不出映射文件
    fs::path dir = tmp_ / "local" / "9999";
    std::error_code ec;
    fs::create_directories(dir, ec);
    ASSERT_FALSE(ec);
    fs::permissions(dir, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);

    trader_.attach(&api_);
    ASSERT_EQ(0, trader_.login("88880001", "pwd", "prod01"));
    CThostFtdcRspAuthenticateField authf;
    memset(&authf, 0, sizeof(authf));
    trader_.OnRspAuthenticate(&authf, &ok_, 1, true);
    CThostFtdcRspUserLoginField lf;
    memset(&lf, 0, sizeof(lf));
    lf.FrontID = 12345;
    lf.SessionID = 67;
    strcpy(lf.MaxOrderRef, "1000");
    trader_.OnRspUserLogin(&lf, &ok_, 2, true);

    fs::permissions(dir, fs::perms::all, fs::perm_options::replace);   // 别让 TearDown 删不掉

    // L766-768 / L779-781：两个缓存各自的告警回调都被打了一次
    EXPECT_EQ(TraderCTP::WS_LOGINED, trader_.state());
    EXPECT_EQ(20240115u, trader_.tradingDate());
    EXPECT_LE(2u, std::count(sink_.logs_.begin(), sink_.logs_.end(), std::string("Mapping cache file failed")));
    EXPECT_EQ(1u, trader_.queryDepth());                // 失败被吞掉，登录链照旧往下走
    EXPECT_EQ(0u, trader_.eidSize());                   // 缓存没建起来 ⇒ 容量 0
    EXPECT_EQ(0u, trader_.oidSize());
    EXPECT_EQ("", trader_.eidValue("012345#0000000067#001001"));   // get() 只查索引 ⇒ 安全回落空串
}

//////////////////////////////////////////////////////////////////////////
// 11) 会崩/会挂的输入：收进 DISABLED_，理由写在 manifest里

//
/*
 * OnRspQrySettlementInfo 的 bIsLast 分支（L1093-1096）同时无条件解引用 m_sink 与
 * pSettlementInfo：只要「前面攒到过内容」且「最后一行是 NULL」就是空指针解引用。
 * 启用会让整条用例变 CRASH，故留在 DISABLED_（见 BUG-T25-003）。
 */
TEST_F(T25TraderCTPTest, DISABLED_settlement_info_last_row_without_payload_crashes)
{
    bringToReady();
    CThostFtdcSettlementInfoField s1;
    memset(&s1, 0, sizeof(s1));
    strcpy(s1.TradingDay, "20240115");
    strcpy(s1.Content, "line-A");
    trader_.OnRspQrySettlementInfo(&s1, &ok_, 1, false);
    // 末行为 NULL（柜台以「无更多数据 + 错误」收尾时就是这个形状）⇒ L1095 解引用 NULL
    trader_.OnRspQrySettlementInfo(NULL, &ok_, 1, true);
}

/*
 * OnRspError（L1161）无条件 m_sink->handleTraderLog：同一文件里 write_log 判空（L32-33）、
 * OnFrontConnected/OnRspUserLogout/OnRtnInstrumentStatus 也都判空，唯独这里不判。
 * 实测把 m_sink 显式置 NULL（registerSpi(NULL)）后调用，进程 SIGSEGV（rc=139），
 * 启用会让整条用例变 CRASH，故留 DISABLED_（见 BUG-T25-003）。
 */
TEST_F(T25TraderCTPTest, DISABLED_rsp_error_with_null_sink_crashes)
{
    bringToReady();
    trader_.registerSpi(NULL);                  // 明确为 NULL，不靠未初始化内存的偶然
    CThostFtdcRspInfoField err;
    fillErr(err, 1, "boom");
    trader_.OnRspError(&err, 42, true);         // L1161 ⇒ 解引用 NULL
    ADD_FAILURE() << "走到这里说明没有崩，本条 DISABLED 的理由就不成立了";
}

/*
 * 承接上一条：缓存 init 失败只是「记一条日志 + 返回 false」，产品不接这个返回值
 * （TraderCTP.cpp:766、779）。init 失败时 _cache._block 仍是 NULL，而 WtKVCache::put
 * （WtKVCache.hpp:211-232）在索引未命中时直接读 `_cache._block->_size` ⇒ 空指针解引用。
 * 也就是说：只要 flowdir 不可写，第一笔带 usertag 的委托就会把进程打死。
 * 实测 SIGSEGV（rc=139），所以留 DISABLED_（见 BUG-T25-007）。
 */
TEST_F(T25TraderCTPTest, DISABLED_order_insert_after_failed_cache_init_crashes)
{
    namespace fs = std::filesystem;
    fs::path dir = tmp_ / "local" / "9999";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::permissions(dir, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);

    bringToLogin();                         // 内部不断言缓存告警，只把状态推到 LOGINED
    fs::permissions(dir, fs::perms::all, fs::perm_options::replace);
    trader_.setState(TraderCTP::WS_ALLREADY);

    char eid[64];
    ASSERT_TRUE(trader_.makeEntrustID(eid, sizeof(eid)));
    WTSEntrust* e = WTSEntrust::create(kCodeAg, 1, 7000.0, kExchgAg);
    e->setEntrustID(eid);
    e->setUserTag("TAG-DEAD");
    EXPECT_EQ(0, trader_.orderInsert(e));   // L500 m_eidCache.put ⇒ 解引用 NULL block
    e->release();
}

/*
 * connect() 的 worker（L336-364）把 m_bInQuery 置 true 后，只有某个 OnRsp* 的 bIsLast
 * 才会复位（L810/L903/L936/L1084/L1103/L1133）；查询发出去而回报永不到达时（柜台断流、
 * 回报丢包），worker 就在 L339 的 `m_bInQuery` 上无限自旋，此时 disconnect() 的
 * m_thrdWorker->join()（L376）永不返回 ⇒ 整条用例挂死，只能 DISABLED（见 BUG-T25-005）。
 * 复位之后同一序列又能正常收尾，这正是上面 connect_with_stub_creator 用例能跑绿的原因。
 */
TEST_F(T25TraderCTPTest, DISABLED_disconnect_hangs_when_no_response_clears_in_query)
{
    g_api_for_creator = &api_;
    trader_.setCreator(&fakeCreator);
    trader_.connect();
    trader_.setState(TraderCTP::WS_ALLREADY);
    EXPECT_EQ(0, trader_.queryAccount());
    ASSERT_TRUE(waitUntil([this]() { return api_.n_qry_acct_.load() >= 1; }));
    ASSERT_TRUE(trader_.inQuery());                   // 没有任何回报会把它复位
    trader_.disconnect();                             // join() 挂死，永远走不到这里
    EXPECT_TRUE(trader_.stopped());
}

/*
 * TraderCTP() 构造表（L203-217）没有列 m_sink / m_bdMgr / m_frontID / m_sessionID /
 * m_orderRef / m_lDate / m_lastQryTime 之外的那几个指针成员，其中 m_sink、m_bdMgr
 * 是**裸指针且无类内初值** ⇒ 未登记 sink 直接调用任何带日志的路径就是解不确定指针。
 * 崩不崩取决于堆上残留内容，所以这条既不能启用、也不能当「稳定复现」的用例，
 * 只作为 BUG-T25-001 的复现脚手架保留（见 bugs 文件）。
 */
TEST_F(T25TraderCTPTest, DISABLED_plugin_created_trader_has_indeterminate_sink)
{
    // 先把刚被归还的堆块涂脏，提高命中非空垃圾值的概率
    std::vector<void*> dirty;
    for (int i = 0; i < 64; i++)
    {
        void* p = ::operator new(512);
        memset(p, 0x5a, 512);
        dirty.push_back(p);
    }
    for (auto p : dirty)
        ::operator delete(p);

    ITraderApi* p = createTrader();
    TraderCTP* t = dynamic_cast<TraderCTP*>(p);
    ASSERT_TRUE(t != NULL);
    WTSEntrust* e = WTSEntrust::create(kCodeAg, 1, 7000.0, kExchgAg);
    EXPECT_EQ(-1, t->orderInsert(e));                 // L473 用未初始化的 m_sink 打日志
    e->release();
    deleteTrader(p);
}


