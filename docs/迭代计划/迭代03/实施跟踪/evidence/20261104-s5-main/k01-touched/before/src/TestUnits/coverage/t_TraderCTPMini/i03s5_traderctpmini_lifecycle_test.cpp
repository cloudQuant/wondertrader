/*
 * 迭代03 s5 traders-ctp：TraderCTPMini
 *   卡9 / T3-006：有界停止、Req 与 Release 不重叠、回调内释放延后、工厂隔离；void connect 失败以 WTE_Connect 非零可观察。
 *   T3-015：GetTradingDay 空/无效值 fail-closed —— 登录不就绪、不推送合成日期的回报；合法日期与夜盘正控不变。
 *   T2-CTP-08：缺委托 ID 原子分配唯一 OrderRef，合法预分配 ID 保留，同一身份不重复提交。
 * 只用本地 R61CtpMiniApiStub 派生替身；真实 CTPMini SDK 的 GetTradingDay 合同仍为外部条件（X）。
 * WT_I03_S5_TRADERS_BASELINE：改前源码红控编译时定义。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSTradeDef.hpp"

#include "r61_ctpmini_api_stub.h"

extern "C" {
    ITraderApi* createTrader();
    void deleteTrader(ITraderApi*& trader);
}

namespace i03s5mini {

USING_NS_WTP;

class Watchdog
{
public:
    Watchdog(const char* what, int ms) : what_(what)
    {
        thread_ = std::thread([this, ms]() {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!cv_.wait_for(lock, std::chrono::milliseconds(ms), [this]() { return done_; }))
            {
                std::fprintf(stderr, "WATCHDOG_TIMEOUT: %s did not finish within %d ms\n", what_, ms);
                std::fflush(stderr);
                std::_Exit(98);
            }
        });
    }
    ~Watchdog()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            done_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }
private:
    const char* what_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_ = false;
    std::thread thread_;
};

static bool waitFor(const std::function<bool()>& pred, int maxMs = 3000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maxMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

class MiniApi : public R61CtpMiniApiStub
{
public:
    std::atomic<int> releases{0};
    std::atomic<int> inits{0};
    std::atomic<int> active_req{0};
    std::atomic<int> account_queries{0};
    std::atomic<bool> release_overlapped{false};
    std::atomic<bool> block_insert{false};
    std::atomic<bool> block_account{false};
    std::atomic<bool> entered{false};
    std::atomic<bool> unblock{false};
    std::mutex refs_mutex;
    std::vector<std::string> refs;

    void Release() override
    {
        if (active_req.load() != 0)
            release_overlapped = true;
        ++releases;
    }
    void Init(bool) override { ++inits; }
    int ReqOrderInsert(CThostFtdcInputOrderField* req, int) override
    {
        ++active_req;
        entered = true;
        while (block_insert.load() && !unblock.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        {
            std::lock_guard<std::mutex> lock(refs_mutex);
            refs.emplace_back(req->OrderRef);
        }
        --active_req;
        return 0;
    }
    int ReqQryTradingAccount(CThostFtdcQryTradingAccountField*, int) override
    {
        ++active_req;
        ++account_queries;
        entered = true;
        while (block_account.load() && !unblock.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        --active_req;
        return 0;
    }
};

class BaseData : public IBaseDataMgr
{
public:
    WTSContractInfo* ct = nullptr;
    WTSCommodityInfo* getCommodity(const char*) override { return NULL; }
    WTSCommodityInfo* getCommodity(const char*, const char*) override { return NULL; }
    WTSContractInfo* getContract(const char* code, const char* = "", uint32_t = 0) override
    {
        return (ct != nullptr && std::strcmp(code, ct->getCode()) == 0) ? ct : NULL;
    }
    WTSArray* getContracts(const char* = "", uint32_t = 0) override { return NULL; }
    WTSSessionInfo* getSession(const char*) override { return NULL; }
    WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return NULL; }
    WTSArray* getAllSessions() override { return NULL; }
    bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
    uint32_t calcTradingDate(const char*, uint32_t uDate, uint32_t, bool = false) override { return uDate; }
    uint64_t getBoundaryTime(const char*, uint32_t tDate, bool = false, bool = true) override { return tDate; }
};

class Sink : public ITraderSpi
{
public:
    BaseData bd;
    std::mutex mtx;
    std::vector<std::string> logs;
    std::vector<std::pair<int, int32_t>> events;
    std::atomic<int> login_ok{0}, login_fail{0}, pushed_orders{0}, pushed_trades{0};
    std::atomic<uint32_t> login_date{0}, last_order_date{0}, last_trade_date{0};
    std::string last_login_msg;
    std::function<void()> on_order_hook;

    IBaseDataMgr* getBaseDataMgr() override { return &bd; }
    void handleTraderLog(WTSLogLevel, const char* message) override
    {
        std::lock_guard<std::mutex> lock(mtx);
        logs.push_back(message ? message : "");
    }
    void handleEvent(WTSTraderEvent e, int32_t ec) override
    {
        std::lock_guard<std::mutex> lock(mtx);
        events.push_back(std::make_pair((int)e, ec));
    }
    void onLoginResult(bool ok, const char* msg, uint32_t date) override
    {
        if (ok) ++login_ok; else ++login_fail;
        login_date = date;
        std::lock_guard<std::mutex> lock(mtx);
        last_login_msg = msg ? msg : "";
    }
    void onPushOrder(WTSOrderInfo* o) override
    {
        ++pushed_orders;
        last_order_date = o->getOrderDate();
        if (on_order_hook)
        {
            auto hook = std::move(on_order_hook);
            on_order_hook = nullptr;
            hook();
        }
    }
    void onPushTrade(WTSTradeInfo* t) override
    {
        ++pushed_trades;
        last_trade_date = t->getTradeDate();
    }
    bool hasLog(const char* sub)
    {
        std::lock_guard<std::mutex> lock(mtx);
        for (auto& s : logs)
            if (s.find(sub) != std::string::npos)
                return true;
        return false;
    }
    int connectFailures()
    {
        std::lock_guard<std::mutex> lock(mtx);
        int n = 0;
        for (auto& e : events)
            if (e.first == (int)WTE_Connect && e.second != 0)
                ++n;
        return n;
    }
};

class Probe : public TraderCTPMini
{
public:
    void setApi(CThostFtdcTraderApi* api) { m_pUserAPI = api; }
    CThostFtdcTraderApi* api() const { return m_pUserAPI; }
    void setReady(uint32_t front, int32_t session, uint32_t ref)
    {
        m_wrapperState = WS_ALLREADY;
        m_frontID = front;
        m_sessionID = session;
        m_orderRef = ref;
        m_strBroker = "9999";
        m_strUser = "tester";
        m_lDate = 20260105;
    }
    void configure(const std::string& dir)
    {
        m_strFlowDir = dir;
        m_strBroker = "9999";
        m_strUser = "tester";
        m_strFront = "tcp://127.0.0.1:1";
    }
    void setCreator(CTPCreator c) { m_funcCreator = c; }
    WrapperState state() const { return m_wrapperState; }
    uint32_t tradingDate() const { return m_lDate; }
    void setTradingDate(uint32_t d) { m_lDate = d; }
    bool hasWorker() const { return m_thrdWorker != NULL; }
    bool initCaches(const std::string& dir)
    {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return m_eidCache.init((dir + "/eid.sc").c_str(), 20260105, nullptr) &&
            m_oidCache.init((dir + "/oid.sc").c_str(), 20260105, nullptr);
    }
    std::string eidGet(const char* key) { return m_eidCache.get(key); }
#ifdef WT_I03_S5_TRADERS_BASELINE
    int stopState() const { return -1; }
    void setStopBound(uint32_t) {}
    std::string rawDay() const { return "<baseline>"; }
#else
    int stopState() const { return (int)lifecycleState(); }
    void setStopBound(uint32_t ms) { m_stopBoundMs = ms; }
    std::string rawDay() const { return invalidTradingDayRaw(); }
#endif
    void joinWorkerForTeardown()
    {
        m_bStopped = true;
        if (m_thrdWorker != NULL && m_thrdWorker->joinable())
            m_thrdWorker->join();
        m_thrdWorker = NULL;
    }
};

static const int kDeferred = 1;
static const int kReleased = 2;

static CThostFtdcTraderApi* g_api = nullptr;
static std::atomic<int> g_creator_calls{0};
static CThostFtdcTraderApi* miniCreator(const char*)
{
    ++g_creator_calls;
    return g_api;
}
static CThostFtdcTraderApi* nullCreator(const char*)
{
    ++g_creator_calls;
    return nullptr;
}

class I03S5TraderCTPMini : public ::testing::Test
{
protected:
    MiniApi api_;
    Sink sink_;
    Probe trader_;
    std::filesystem::path tmp_;
    WTSCommodityInfo* comm_ = nullptr;
    WTSContractInfo* ct_ = nullptr;
    CThostFtdcRspInfoField ok_;

    void SetUp() override
    {
        std::string tag = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        tmp_ = std::filesystem::temp_directory_path() / ("i03s5_mini_" + tag + "_" + std::to_string((long)::getpid()));
        std::error_code ec;
        std::filesystem::create_directories(tmp_, ec);
        comm_ = WTSCommodityInfo::create("ru", "ru", "SHFE", "TRADING", "CHINA");
        comm_->setVolScale(10);
        ct_ = WTSContractInfo::create("ru2601", "ru2601", "SHFE", "ru");
        ct_->setCommInfo(comm_);
        sink_.bd.ct = ct_;
        trader_.configure(tmp_.string() + "/");
        trader_.registerSpi(&sink_);
        std::memset(&ok_, 0, sizeof(ok_));
        g_api = &api_;
        g_creator_calls = 0;
    }
    void TearDown() override
    {
        api_.unblock = true;
        trader_.joinWorkerForTeardown();
        g_api = nullptr;
        ct_->release();
        comm_->release();
        std::error_code ec;
        std::filesystem::remove_all(tmp_, ec);
    }

    /// 走真实 login → OnRspAuthenticate → OnRspUserLogin，GetTradingDay 取替身值
    void loginWithTradingDay(const char* day)
    {
        api_.trading_day_ = day;
        trader_.setApi(&api_);
        ASSERT_EQ(0, trader_.login("tester", "pwd", "prod"));
        CThostFtdcRspAuthenticateField auth;
        std::memset(&auth, 0, sizeof(auth));
        trader_.OnRspAuthenticate(&auth, &ok_, 1, true);
        CThostFtdcRspUserLoginField lf;
        std::memset(&lf, 0, sizeof(lf));
        lf.FrontID = 3;
        lf.SessionID = 4;
        std::strcpy(lf.MaxOrderRef, "10");
        std::strcpy(lf.LoginTime, "09:00:00");
        trader_.OnRspUserLogin(&lf, &ok_, 2, true);
    }

    void pushOrderAndTrade(const char* time)
    {
        CThostFtdcOrderField o;
        std::memset(&o, 0, sizeof(o));
        std::strcpy(o.InstrumentID, "ru2601");
        std::strcpy(o.ExchangeID, "SHFE");
        std::strcpy(o.InsertTime, time);
        std::strcpy(o.OrderSysID, "S1");
        o.TimeCondition = THOST_FTDC_TC_GFD;
        trader_.OnRtnOrder(&o);
        CThostFtdcTradeField t;
        std::memset(&t, 0, sizeof(t));
        std::strcpy(t.InstrumentID, "ru2601");
        std::strcpy(t.ExchangeID, "SHFE");
        std::strcpy(t.TradeTime, time);
        std::strcpy(t.TradeID, "T1");
        t.Volume = 1;
        trader_.OnRtnTrade(&t);
    }
};

static WTSEntrust* makeOrder(WTSContractInfo* ct)
{
    WTSEntrust* e = WTSEntrust::create("ru2601", 1, 15000.0, "SHFE");
    e->setContractInfo(ct);
    e->setDirection(WDT_LONG);
    e->setOffsetType(WOT_OPEN);
    e->setPriceType(WPT_LIMITPRICE);
    e->setOrderFlag(WOF_NOR);
    return e;
}

}// namespace i03s5mini

using namespace i03s5mini;

// T3-015 正控：合法交易日登录就绪；周一夜盘回推上周五，非周一夜盘回推前一日，日盘原样。
TEST_F(I03S5TraderCTPMini, ValidTradingDayLoginAndNightDatesStayCorrect)
{
    loginWithTradingDay("20260105");   // Monday
    EXPECT_EQ(TraderCTPMini::WS_ALLREADY, trader_.state());
    EXPECT_EQ(1, sink_.login_ok.load());
    EXPECT_EQ(20260105u, sink_.login_date.load());
    pushOrderAndTrade("21:05:00");
    EXPECT_EQ(20260102u, sink_.last_order_date.load());
    EXPECT_EQ(20260102u, sink_.last_trade_date.load());
    pushOrderAndTrade("10:00:00");
    EXPECT_EQ(20260105u, sink_.last_order_date.load());
    EXPECT_EQ(20260105u, sink_.last_trade_date.load());
    trader_.setTradingDate(20260106); // Tuesday
    pushOrderAndTrade("21:05:00");
    EXPECT_EQ(20260105u, sink_.last_order_date.load());
    EXPECT_EQ(20260105u, sink_.last_trade_date.load());
}

// T3-015 反控矩阵：空/非数字/零/短/长/非法日历日期 → 登录 fail-closed，不就绪、不推送合成日期的回报，
// 保留原始串供诊断。
TEST_F(I03S5TraderCTPMini, InvalidTradingDayFailsClosed)
{
    loginWithTradingDay("2026013X");
    EXPECT_EQ(TraderCTPMini::WS_LOGINFAILED, trader_.state());
    EXPECT_EQ(0, sink_.login_ok.load());
    EXPECT_EQ(1, sink_.login_fail.load());
    EXPECT_EQ(0u, trader_.tradingDate());
    EXPECT_EQ("2026013X", trader_.rawDay());
    EXPECT_FALSE(trader_.isConnected());
    pushOrderAndTrade("21:05:00");
    pushOrderAndTrade("10:00:00");
    EXPECT_EQ(0, sink_.pushed_orders.load());
    EXPECT_EQ(0, sink_.pushed_trades.load());
    EXPECT_EQ(-1, trader_.queryAccount());
}

TEST_F(I03S5TraderCTPMini, InvalidTradingDayMatrixIsRejected)
{
    const char* bad[] = { "", "00000000", "2026010", "202601050", "20260230", "20251301", "abcdefgh", "2026-1-5" };
    for (const char* day : bad)
    {
        Probe probe;
        Sink sink;
        sink.bd.ct = ct_;
        probe.configure(tmp_.string() + "/m/");
        probe.registerSpi(&sink);
        MiniApi api;
        api.trading_day_ = day;
        probe.setApi(&api);
        ASSERT_EQ(0, probe.login("tester", "pwd", "prod"));
        CThostFtdcRspAuthenticateField auth;
        std::memset(&auth, 0, sizeof(auth));
        probe.OnRspAuthenticate(&auth, &ok_, 1, true);
        CThostFtdcRspUserLoginField lf;
        std::memset(&lf, 0, sizeof(lf));
        std::strcpy(lf.MaxOrderRef, "1");
        probe.OnRspUserLogin(&lf, &ok_, 2, true);
        EXPECT_EQ(TraderCTPMini::WS_LOGINFAILED, probe.state()) << "[" << day << "]";
        EXPECT_EQ(0, sink.login_ok.load()) << day;
        EXPECT_EQ(0u, probe.tradingDate()) << day;
        probe.release();
    }
    {
        Probe probe;
        Sink sink;
        probe.configure(tmp_.string() + "/null/");
        probe.registerSpi(&sink);
        MiniApi api;
        api.trading_day_ = nullptr;
        probe.setApi(&api);
        ASSERT_EQ(0, probe.login("tester", "pwd", "prod"));
        CThostFtdcRspUserLoginField lf;
        std::memset(&lf, 0, sizeof(lf));
        probe.OnRspUserLogin(&lf, &ok_, 2, true);
        EXPECT_EQ(TraderCTPMini::WS_LOGINFAILED, probe.state());
        EXPECT_EQ(0, sink.login_ok.load());
        probe.release();
    }
    {
        Probe probe;   // leap day positive control
        Sink sink;
        probe.configure(tmp_.string() + "/leap/");
        probe.registerSpi(&sink);
        MiniApi api;
        api.trading_day_ = "20240229";
        probe.setApi(&api);
        ASSERT_EQ(0, probe.login("tester", "pwd", "prod"));
        CThostFtdcRspUserLoginField lf;
        std::memset(&lf, 0, sizeof(lf));
        probe.OnRspUserLogin(&lf, &ok_, 2, true);
        EXPECT_EQ(TraderCTPMini::WS_ALLREADY, probe.state());
        EXPECT_EQ(20240229u, probe.tradingDate());
        probe.release();
    }
}

// T3-006：void connect 的各类失败以 WTE_Connect 非零事件可观察，不创建 worker。
TEST_F(I03S5TraderCTPMini, ConnectFailuresAreObservableEvents)
{
    trader_.setCreator(nullptr);
    trader_.connect();
    EXPECT_EQ(1, sink_.connectFailures());
    EXPECT_FALSE(trader_.hasWorker());
    trader_.setCreator(&nullCreator);
    trader_.connect();
    EXPECT_EQ(2, sink_.connectFailures());
    EXPECT_FALSE(trader_.hasWorker());
    EXPECT_EQ(nullptr, trader_.api());
}

// 卡9：无终帧查询在途时 disconnect 有界返回并 join、Release 一次；之后 connect 被拒绝。
TEST_F(I03S5TraderCTPMini, NoFinalFrameDisconnectIsBoundedAndInstanceIsSingleUse)
{
    trader_.setCreator(&miniCreator);
    trader_.connect();
    ASSERT_TRUE(trader_.hasWorker());
    trader_.setReady(3, 4, 0);
    ASSERT_EQ(0, trader_.queryAccount());
    ASSERT_TRUE(waitFor([this]() { return api_.account_queries.load() == 1; }));
    {
        Watchdog dog("disconnect without final frame", 6000);
        const auto start = std::chrono::steady_clock::now();
        trader_.disconnect();
        EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count(), 2000);
    }
    EXPECT_FALSE(trader_.hasWorker());
    EXPECT_EQ(1, api_.releases.load());
    EXPECT_EQ(kReleased, trader_.stopState());
    trader_.release();
    EXPECT_EQ(1, api_.releases.load());
    trader_.connect();
    EXPECT_EQ(1, g_creator_calls.load());
    EXPECT_EQ(1, sink_.connectFailures());
}

// 并发下单在途时 release 不与 Req 重叠。
TEST_F(I03S5TraderCTPMini, ReleaseNeverOverlapsInFlightOrderInsert)
{
    trader_.setApi(&api_);
    trader_.setReady(3, 4, 0);
    ASSERT_TRUE(trader_.initCaches((tmp_ / "cache").string()));
    api_.block_insert = true;
    WTSEntrust* e = makeOrder(ct_);
    int result = 99;
    std::thread host([&]() { result = trader_.orderInsert(e); });
    ASSERT_TRUE(waitFor([this]() { return api_.entered.load(); }));
    std::thread unblocker([this]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        api_.unblock = true;
    });
    {
        Watchdog dog("release with in-flight insert", 6000);
        trader_.release();
    }
    host.join();
    unblocker.join();
    EXPECT_EQ(0, result);
    EXPECT_EQ(1, api_.releases.load());
    EXPECT_FALSE(api_.release_overlapped.load());
    EXPECT_EQ(kReleased, trader_.stopState());
    e->release();
}

// 卡9d：回调内 release 只关闭准入并延后；回调返回后外部 release 完成。
TEST_F(I03S5TraderCTPMini, ReleaseFromCallbackIsDeferred)
{
    loginWithTradingDay("20260105");
    ASSERT_EQ(TraderCTPMini::WS_ALLREADY, trader_.state());
    int inside = -1, state = -1;
    sink_.on_order_hook = [&]() {
        trader_.release();
        inside = api_.releases.load();
        state = trader_.stopState();
    };
    std::thread sdk([this]() { pushOrderAndTrade("10:00:00"); });
    {
        Watchdog dog("callback re-entry", 6000);
        sdk.join();
    }
    EXPECT_EQ(0, inside);
    EXPECT_EQ(kDeferred, state);
    EXPECT_EQ(0, sink_.pushed_trades.load());     // later callback rejected once stop requested
    {
        Watchdog dog("deferred completion", 6000);
        trader_.release();
    }
    EXPECT_EQ(1, api_.releases.load());
    EXPECT_EQ(kReleased, trader_.stopState());
}

// T2-CTP-08：双无 tag 无 ID 单得到不同 OrderRef；预分配 ID 空 tag 时保留；重复/其他会话拒绝；并发唯一。
TEST_F(I03S5TraderCTPMini, MissingIdentityIsAllocatedUniqueAndPreallocatedIdKept)
{
    trader_.setApi(&api_);
    trader_.setReady(3, 4, 10);
    ASSERT_TRUE(trader_.initCaches((tmp_ / "cache").string()));
    WTSEntrust* a = makeOrder(ct_);
    WTSEntrust* b = makeOrder(ct_);
    ASSERT_EQ(0, trader_.orderInsert(a));
    ASSERT_EQ(0, trader_.orderInsert(b));
    ASSERT_EQ(2u, api_.refs.size());
    EXPECT_EQ("11", api_.refs[0]);
    EXPECT_EQ("12", api_.refs[1]);
    EXPECT_EQ("ctpmini-auto:000003#0000000004#000011", trader_.eidGet("000003#0000000004#000011"));

    char reserved[64] = { 0 };
    ASSERT_TRUE(trader_.makeEntrustID(reserved, sizeof(reserved)));
    WTSEntrust* c = makeOrder(ct_);
    c->setEntrustID(reserved);
    ASSERT_EQ(0, trader_.orderInsert(c));
    EXPECT_EQ("13", api_.refs.back());
    EXPECT_STREQ(reserved, c->getEntrustID());

    const size_t before = api_.refs.size();
    EXPECT_EQ(-1, trader_.orderInsert(a));
    for (const char* bad : { "000003#0000000005#000020", "3#4#0", "3#4#2x", "3#4" })
    {
        WTSEntrust* e = makeOrder(ct_);
        e->setEntrustID(bad);
        EXPECT_EQ(-1, trader_.orderInsert(e)) << bad;
        e->release();
    }
    EXPECT_EQ(before, api_.refs.size());

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([this]() {
            for (int i = 0; i < 50; ++i)
            {
                WTSEntrust* e = makeOrder(ct_);
                trader_.orderInsert(e);
                e->release();
            }
        });
    for (auto& th : threads)
        th.join();
    std::set<std::string> unique(api_.refs.begin(), api_.refs.end());
    EXPECT_EQ(api_.refs.size(), unique.size());
    EXPECT_EQ(before + 200, api_.refs.size());
    a->release(); b->release(); c->release();
}

// 卡9a/9b：创建过 SDK 的实例被工厂 remover 隔离；未创建 SDK 的实例照常删除。
TEST_F(I03S5TraderCTPMini, FactoryRemoverQuarantinesInstanceWithSdk)
{
    Probe* raw = new Probe();
    raw->configure(tmp_.string() + "/q/");
    raw->registerSpi(&sink_);
    raw->setCreator(&miniCreator);
    raw->connect();
    ASSERT_EQ(1, g_creator_calls.load());
    raw->registerSpi(NULL);
    ITraderApi* handle = raw;
    {
        Watchdog dog("deleteTrader", 6000);
        deleteTrader(handle);
    }
    ASSERT_TRUE(handle != NULL);
    EXPECT_EQ(kReleased, raw->stopState());
    EXPECT_EQ(1, api_.releases.load());
    raw->OnFrontDisconnected(0);
    ITraderApi* fresh = createTrader();
    deleteTrader(fresh);
    EXPECT_TRUE(fresh == NULL);
}

// N-1（主线程追加任务，与 TraderCTP 已修子项对齐）：CTP NotTouched('b') 必须报告为 WOS_Nottouched，
// 改前被数值强转为 WOS_Cancelling('b')。Touched('c') 待 D-T2-CTP-07，不在本用例断言。
TEST_F(I03S5TraderCTPMini, NotTouchedIsReportedAsNottouched)
{
    trader_.setReady(3, 4, 0);
    int state = -1;
    sink_.on_order_hook = nullptr;
    CThostFtdcOrderField o;
    std::memset(&o, 0, sizeof(o));
    std::strcpy(o.InstrumentID, "ru2601");
    std::strcpy(o.ExchangeID, "SHFE");
    std::strcpy(o.InsertTime, "10:00:00");
    o.OrderStatus = THOST_FTDC_OST_NotTouched;
    struct StateSink : public Sink { int* out; void onPushOrder(WTSOrderInfo* oi) override { *out = (int)oi->getOrderState(); } } s;
    s.out = &state;
    s.bd.ct = ct_;
    trader_.registerSpi(&s);
    trader_.OnRtnOrder(&o);
    EXPECT_EQ((int)WOS_Nottouched, state);
}
