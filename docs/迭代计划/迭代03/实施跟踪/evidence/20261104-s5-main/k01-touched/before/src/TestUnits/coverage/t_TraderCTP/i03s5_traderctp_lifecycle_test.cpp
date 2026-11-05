/*
 * 迭代03 s5 traders-ctp：TraderCTP 卡9 生命周期（BUG-T25-005、T2-CTP-02、T2-CTP-03）。
 *
 * 只用本地 StubApi（t25_ctp_api_stub.hpp，不起线程、不开 socket、不碰厂商实现）；
 * connect() 的 creator 被换成测试 creator，CreateFtdcTraderApi 一个字节都不执行。
 *
 * 会在修复前挂死的场景（无终帧 disconnect、worker 卡在 SDK 请求内）由 Watchdog 守护：
 * 超时直接 _Exit(98)，让该用例以非零退出码失败而不是挂住整个进程（红控即据此判定）。
 *
 * WT_I03_S5_TRADERS_BASELINE：用改前产品源码编译红控时定义，新增的生命周期观察接口
 * （lifecycleState/m_stopBoundMs）在改前不存在，以哨兵值代替，使断言失败而非编译失败。
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
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSTradeDef.hpp"
#include "Includes/WTSVariant.hpp"

#include "t25_ctp_api_stub.hpp"

extern "C" {
    ITraderApi* createTrader();
    void deleteTrader(ITraderApi*& trader);
}

namespace i03s5ctp {

USING_NS_WTP;

/// 超时即终止进程：修复前会挂死的路径以退出码 98 失败，而不是挂住整条测试。
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

static long long elapsedMs(std::chrono::steady_clock::time_point start)
{
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

/// 可阻塞的 SDK 替身：记录 Req 在途数，Release 时若有在途 Req 记为重叠。
class LifecycleApi : public t25ctp::StubApi
{
public:
    std::atomic<int>  active_req{0};
    std::atomic<bool> release_overlapped{false};
    std::atomic<bool> block_account{false};
    std::atomic<bool> account_entered{false};
    std::atomic<bool> block_insert{false};
    std::atomic<bool> insert_entered{false};
    std::atomic<bool> unblock{false};
    std::atomic<int>  account_ret{0};

    void Release() override
    {
        if (active_req.load() != 0)
            release_overlapped = true;
        t25ctp::StubApi::Release();
    }
    int ReqQryTradingAccount(CThostFtdcQryTradingAccountField* req, int id) override
    {
        ++active_req;
        account_entered = true;
        while (block_account.load() && !unblock.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const int base = t25ctp::StubApi::ReqQryTradingAccount(req, id);
        --active_req;
        return account_ret.load() != 0 ? account_ret.load() : base;
    }
    int ReqOrderInsert(CThostFtdcInputOrderField* req, int id) override
    {
        ++active_req;
        insert_entered = true;
        while (block_insert.load() && !unblock.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const int r = t25ctp::StubApi::ReqOrderInsert(req, id);
        --active_req;
        return r;
    }
};

class NullBaseData : public IBaseDataMgr
{
public:
    WTSCommodityInfo* getCommodity(const char*) override { return NULL; }
    WTSCommodityInfo* getCommodity(const char*, const char*) override { return NULL; }
    WTSContractInfo* getContract(const char*, const char* = "", uint32_t = 0) override { return NULL; }
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
    NullBaseData bd;
    std::mutex mtx;
    std::vector<std::pair<int, int32_t>> events;
    std::vector<std::string> logs;
    std::atomic<int> n_account{0};
    std::atomic<int> n_login{0};
    std::function<void()> on_account_hook;

    IBaseDataMgr* getBaseDataMgr() override { return &bd; }
    void handleTraderLog(WTSLogLevel, const char* message) override
    {
        std::lock_guard<std::mutex> lock(mtx);
        logs.push_back(message == NULL ? "" : message);
    }
    void handleEvent(WTSTraderEvent e, int32_t ec) override
    {
        std::lock_guard<std::mutex> lock(mtx);
        events.push_back(std::make_pair((int)e, ec));
    }
    void onLoginResult(bool, const char*, uint32_t) override { ++n_login; }
    void onRspAccount(WTSArray*) override
    {
        ++n_account;
        if (on_account_hook)
            on_account_hook();
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

class Probe : public TraderCTP
{
public:
    void setCreator(CTPCreator f) { m_funcCreator = f; }
    void attach(CThostFtdcTraderApi* api) { m_pUserAPI = api; }
    CThostFtdcTraderApi* api() const { return m_pUserAPI; }
    void setState(WrapperState s) { m_wrapperState = s; }
    WrapperState state() const { return m_wrapperState; }
    bool inQuery() const { return m_bInQuery; }
    bool hasWorker() const { return m_thrdWorker != NULL; }
    uint32_t queueDepth()
    {
        StdUniqueLock lock(m_mtxQuery);
        return (uint32_t)m_queQuery.size();
    }
    void configure(const std::string& flowDir)
    {
        m_strBroker = "9999";
        m_strUser = "88880001";
        m_strFlowDir = flowDir;
        m_strFront.push_back("tcp://127.0.0.1:1");
    }
#ifdef WT_I03_S5_TRADERS_BASELINE
    int stopState() const { return -1; }
    void setStopBound(uint32_t) {}
#else
    int stopState() const { return (int)lifecycleState(); }
    void setStopBound(uint32_t ms) { m_stopBoundMs = ms; }
#endif
    /// 不起 worker 时在当前线程执行队首查询
    void runQueuedOnce()
    {
        CommonExecuter handler;
        {
            StdUniqueLock lock(m_mtxQuery);
            if (m_queQuery.empty())
                return;
            handler = m_queQuery.front();
            m_queQuery.pop();
        }
        handler();
    }
    /// 测试收尾安全网：绝不带着可 join 的 worker 析构
    void joinWorkerForTeardown()
    {
        m_bStopped = true;
        if (m_thrdWorker != NULL && m_thrdWorker->joinable())
            m_thrdWorker->join();
        m_thrdWorker = NULL;
    }
};

static const int kReleased = 2;      // wt_vendor_td::StopState::Released
static const int kDeferred = 1;      // wt_vendor_td::StopState::StopDeferred
static const int kUncertain = 3;     // wt_vendor_td::StopState::DrainUncertain

static CThostFtdcTraderApi* g_creator_api = NULL;
static std::atomic<int> g_creator_calls{0};
static CThostFtdcTraderApi* lifecycleCreator(const char*)
{
    ++g_creator_calls;
    return g_creator_api;
}
static CThostFtdcTraderApi* nullCreator(const char*)
{
    ++g_creator_calls;
    return NULL;
}

class I03S5TraderCTPLifecycle : public ::testing::Test
{
protected:
    LifecycleApi api_;
    Sink sink_;
    Probe trader_;
    std::filesystem::path tmp_;
    CThostFtdcRspInfoField ok_;

    void SetUp() override
    {
        std::string tag = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        tmp_ = std::filesystem::temp_directory_path() / ("i03s5_ctp_" + tag + "_" + std::to_string((long)::getpid()));
        std::error_code ec;
        std::filesystem::create_directories(tmp_, ec);
        trader_.configure(tmp_.string() + "/");
        trader_.registerSpi(&sink_);
        std::memset(&ok_, 0, sizeof(ok_));
        g_creator_api = &api_;
        g_creator_calls = 0;
    }
    void TearDown() override
    {
        api_.unblock = true;
        trader_.joinWorkerForTeardown();
        g_creator_api = NULL;
        std::error_code ec;
        std::filesystem::remove_all(tmp_, ec);
    }

    void connectWithStub()
    {
        trader_.setCreator(&lifecycleCreator);
        trader_.connect();
        ASSERT_EQ(1, g_creator_calls.load());
        ASSERT_TRUE(trader_.api() == &api_);
        ASSERT_TRUE(trader_.hasWorker());
    }

    void startAccountQuery()
    {
        trader_.setState(TraderCTP::WS_ALLREADY);
        ASSERT_EQ(0, trader_.queryAccount());
        ASSERT_TRUE(waitFor([this]() { return api_.n_qry_acct_.load() == 1 && trader_.inQuery(); }))
            << "worker did not send the account query";
    }

    void bringToReady()
    {
        trader_.attach(&api_);
        ASSERT_EQ(0, trader_.login("88880001", "pwd", "prod01"));
        CThostFtdcRspAuthenticateField authf;
        std::memset(&authf, 0, sizeof(authf));
        trader_.OnRspAuthenticate(&authf, &ok_, 1, true);
        CThostFtdcRspUserLoginField lf;
        std::memset(&lf, 0, sizeof(lf));
        lf.FrontID = 12345;
        lf.SessionID = 67;
        std::strcpy(lf.BrokerID, "9999");
        std::strcpy(lf.UserID, "88880001");
        std::strcpy(lf.TradingDay, "20240115");
        std::strcpy(lf.MaxOrderRef, "1000");
        trader_.OnRspUserLogin(&lf, &ok_, 2, true);
        ASSERT_EQ(TraderCTP::WS_LOGINED, trader_.state());
        // Execute the queued settlement-confirm query (request id 3) on this thread.
        ASSERT_EQ(1u, trader_.queueDepth());
        runQueuedOnce();
        CThostFtdcSettlementInfoConfirmField conf;
        std::memset(&conf, 0, sizeof(conf));
        std::strcpy(conf.ConfirmDate, "20240115");
        trader_.OnRspQrySettlementInfoConfirm(&conf, &ok_, 3, true);
        ASSERT_EQ(TraderCTP::WS_ALLREADY, trader_.state());
    }

    void runQueuedOnce();
};

void I03S5TraderCTPLifecycle::runQueuedOnce()
{
    trader_.runQueuedOnce();
}

static WTSEntrust* makeEntrust()
{
    WTSEntrust* e = WTSEntrust::create("ag2512", 1, 7000.0, "SHFE");
    e->setDirection(WDT_LONG);
    e->setOffsetType(WOT_OPEN);
    e->setPriceType(WPT_LIMITPRICE);
    e->setOrderFlag(WOF_NOR);
    return e;
}

}// namespace i03s5ctp

using namespace i03s5ctp;

// BUG-T25-005：查询已发出、柜台永不回终帧时，disconnect 必须在 2 秒内返回并 join worker，
// SDK Release 恰好一次；之后迟到的终帧被关闭的回调闸门拒绝，不重复释放。
TEST_F(I03S5TraderCTPLifecycle, NoFinalFrameDisconnectIsBoundedJoinsAndReleasesOnce)
{
    connectWithStub();
    startAccountQuery();
    {
        Watchdog dog("disconnect without final frame", 6000);
        const auto start = std::chrono::steady_clock::now();
        trader_.disconnect();
        EXPECT_LT(elapsedMs(start), 2000);
    }
    EXPECT_FALSE(trader_.hasWorker());
    EXPECT_EQ(1, api_.n_release_.load());
    EXPECT_EQ(NULL, trader_.api());
    EXPECT_EQ(kReleased, trader_.stopState());
    EXPECT_FALSE(trader_.inQuery());

    CThostFtdcTradingAccountField acct;
    std::memset(&acct, 0, sizeof(acct));
    trader_.OnRspQryTradingAccount(&acct, &ok_, 1, true);   // late final frame
    EXPECT_EQ(0, sink_.n_account.load());
    trader_.disconnect();
    trader_.release();
    EXPECT_EQ(1, api_.n_release_.load());
}

// 有终帧：正常查询收尾后 disconnect 有界完成。
TEST_F(I03S5TraderCTPLifecycle, FinalFrameThenDisconnectReleasesOnce)
{
    connectWithStub();
    startAccountQuery();
    CThostFtdcTradingAccountField acct;
    std::memset(&acct, 0, sizeof(acct));
    trader_.OnRspQryTradingAccount(&acct, &ok_, 1, true);
    EXPECT_EQ(1, sink_.n_account.load());
    EXPECT_FALSE(trader_.inQuery());
    {
        Watchdog dog("disconnect after final frame", 6000);
        const auto start = std::chrono::steady_clock::now();
        trader_.disconnect();
        EXPECT_LT(elapsedMs(start), 2000);
    }
    EXPECT_FALSE(trader_.hasWorker());
    EXPECT_EQ(1, api_.n_release_.load());
    EXPECT_EQ(kReleased, trader_.stopState());
}

// 错误终帧后断开；对象仍存活时迟到的错误回调不得再次释放或再次推送。
TEST_F(I03S5TraderCTPLifecycle, ErrorFinalFrameThenLateErrorCallbackIsIgnored)
{
    connectWithStub();
    startAccountQuery();
    CThostFtdcRspInfoField err;
    std::memset(&err, 0, sizeof(err));
    err.ErrorID = 42;
    std::strcpy(err.ErrorMsg, "fake query error");
    trader_.OnRspQryTradingAccount(NULL, &err, 1, true);
    EXPECT_FALSE(trader_.inQuery());
    {
        Watchdog dog("disconnect after error frame", 6000);
        trader_.disconnect();
    }
    EXPECT_EQ(1, api_.n_release_.load());
    trader_.OnRspQryTradingAccount(NULL, &err, 1, true);
    trader_.OnRspError(&err, 9, true);
    EXPECT_EQ(1, api_.n_release_.load());
    EXPECT_FALSE(sink_.hasLog("9 rsp error"));
}

// 查询同步被拒（无终帧）时必须解除查询 gate，后续查询照常发出。
TEST_F(I03S5TraderCTPLifecycle, SynchronousQueryRejectReleasesGateForNextQuery)
{
    api_.account_ret = -3;
    connectWithStub();
    trader_.setState(TraderCTP::WS_ALLREADY);
    ASSERT_EQ(0, trader_.queryAccount());
    ASSERT_EQ(0, trader_.queryPositions());
    EXPECT_TRUE(waitFor([this]() { return api_.n_qry_pos_.load() == 1; }, 4000))
        << "query gate stayed closed after a synchronous rejection";
    EXPECT_TRUE(sink_.hasLog("Sending account query failed: -3"));
    Watchdog dog("disconnect after rejected query", 6000);
    trader_.disconnect();
    EXPECT_EQ(1, api_.n_release_.load());
}

// worker 卡在 SDK 请求内：disconnect 在界限内返回 DrainUncertain，绝不在在途请求下 Release；
// 请求返回后再次 release() 完成停止，Release 恰好一次且从未与请求重叠。
TEST_F(I03S5TraderCTPLifecycle, WorkerBlockedInsideSdkRequestIsUncertainThenRetrySucceeds)
{
    api_.block_account = true;
    connectWithStub();
    trader_.setStopBound(300);
    trader_.setState(TraderCTP::WS_ALLREADY);
    ASSERT_EQ(0, trader_.queryAccount());
    ASSERT_TRUE(waitFor([this]() { return api_.account_entered.load(); }));
    {
        Watchdog dog("disconnect with worker blocked in SDK", 4000);
        const auto start = std::chrono::steady_clock::now();
        trader_.disconnect();
        EXPECT_LT(elapsedMs(start), 1500);
    }
    EXPECT_EQ(kUncertain, trader_.stopState());
    EXPECT_EQ(0, api_.n_release_.load());
    EXPECT_TRUE(trader_.hasWorker());
    EXPECT_TRUE(trader_.api() == &api_);

    api_.unblock = true;
    ASSERT_TRUE(waitFor([this]() { return api_.active_req.load() == 0; }));
    {
        Watchdog dog("retry release after SDK request returned", 6000);
        trader_.release();
    }
    EXPECT_EQ(kReleased, trader_.stopState());
    EXPECT_EQ(1, api_.n_release_.load());
    EXPECT_FALSE(api_.release_overlapped.load());
    EXPECT_FALSE(trader_.hasWorker());
}

// 卡9d：回调内调用 release() 只关闭准入并延后，不就地 Release；回调返回后外部 release 完成。
TEST_F(I03S5TraderCTPLifecycle, ReleaseFromCallbackIsDeferredToSafePoint)
{
    connectWithStub();
    startAccountQuery();
    int releasesInsideCallback = -1;
    int stateInsideCallback = -1;
    sink_.on_account_hook = [&]() {
        trader_.release();
        releasesInsideCallback = api_.n_release_.load();
        stateInsideCallback = trader_.stopState();
    };
    CThostFtdcTradingAccountField acct;
    std::memset(&acct, 0, sizeof(acct));
    std::thread sdkThread([&]() { trader_.OnRspQryTradingAccount(&acct, &ok_, 1, true); });
    {
        Watchdog dog("callback re-entry", 6000);
        sdkThread.join();
    }
    EXPECT_EQ(1, sink_.n_account.load());
    EXPECT_EQ(0, releasesInsideCallback);
    EXPECT_EQ(kDeferred, stateInsideCallback);
    EXPECT_EQ(-1, trader_.queryPositions());        // admission closed immediately
    {
        Watchdog dog("deferred release completion", 6000);
        trader_.release();
    }
    EXPECT_EQ(kReleased, trader_.stopState());
    EXPECT_EQ(1, api_.n_release_.load());
    EXPECT_FALSE(trader_.hasWorker());
}

// T1-CTPOpt-02 同型：并发下单在途时 release 不与 Req 重叠，下单返回后才 Release。
TEST_F(I03S5TraderCTPLifecycle, ReleaseNeverOverlapsInFlightOrderInsert)
{
    bringToReady();
    api_.block_insert = true;
    WTSEntrust* entrust = makeEntrust();
    int insertResult = 99;
    std::thread host([&]() { insertResult = trader_.orderInsert(entrust); });
    ASSERT_TRUE(waitFor([this]() { return api_.insert_entered.load(); }));
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
    EXPECT_EQ(0, insertResult);
    EXPECT_EQ(1, api_.n_release_.load());
    EXPECT_FALSE(api_.release_overlapped.load());
    EXPECT_EQ(kReleased, trader_.stopState());
    EXPECT_EQ(-1, trader_.orderInsert(entrust));     // closed after release
    EXPECT_EQ(1, api_.n_insert_.load());
    entrust->release();
}

// T2-CTP-03 / 卡9c：重复 connect 保留原 API；release 后 connect 被拒绝且以 WTE_Connect 非零可观察。
TEST_F(I03S5TraderCTPLifecycle, RepeatedConnectKeepsApiAndConnectAfterReleaseIsRejected)
{
    connectWithStub();
    trader_.connect();
    EXPECT_EQ(1, g_creator_calls.load());
    EXPECT_EQ(1, api_.n_init_.load());
    EXPECT_EQ(1, api_.n_register_front_.load());
    {
        Watchdog dog("disconnect before reconnect", 6000);
        trader_.disconnect();
    }
    EXPECT_EQ(1, api_.n_release_.load());
    trader_.connect();
    EXPECT_EQ(1, g_creator_calls.load());
    EXPECT_EQ(NULL, trader_.api());
    EXPECT_FALSE(trader_.hasWorker());
    EXPECT_EQ(1, sink_.connectFailures());
}

// T2-CTP-02：void connect 的失败以 WTE_Connect 非零事件与错误日志可观察，不创建 worker。
TEST_F(I03S5TraderCTPLifecycle, ConnectFailuresAreObservableEvents)
{
    trader_.setCreator(NULL);
    trader_.connect();
    EXPECT_EQ(1, sink_.connectFailures());
    EXPECT_FALSE(trader_.hasWorker());

    Probe nullApi;
    Sink sink2;
    nullApi.configure(tmp_.string() + "/null/");
    nullApi.registerSpi(&sink2);
    nullApi.setCreator(&nullCreator);
    nullApi.connect();
    EXPECT_EQ(1, sink2.connectFailures());
    EXPECT_FALSE(nullApi.hasWorker());
    EXPECT_EQ(NULL, nullApi.api());
}

// 卡9a/9b：SDK 对象曾存在的实例不被工厂 remover 删除，而是隔离（指针保留、实例存活），
// 迟到回调被关闭的闸门拒绝；从未创建 SDK 的实例照常删除并置空。
TEST_F(I03S5TraderCTPLifecycle, FactoryRemoverQuarantinesInstanceWithSdkAndDeletesOthers)
{
    Probe* raw = new Probe();
    raw->configure(tmp_.string() + "/quarantine/");
    raw->registerSpi(&sink_);
    raw->setCreator(&lifecycleCreator);
    raw->connect();
    ASSERT_EQ(1, g_creator_calls.load());
    raw->registerSpi(NULL);
    ITraderApi* handle = raw;
    {
        Watchdog dog("deleteTrader with SDK", 6000);
        deleteTrader(handle);
    }
    ASSERT_TRUE(handle != NULL) << "instance with an SDK object was deleted";
    EXPECT_EQ(kReleased, raw->stopState());
    EXPECT_EQ(1, api_.n_release_.load());
    EXPECT_FALSE(raw->hasWorker());
    CThostFtdcTradingAccountField acct;
    std::memset(&acct, 0, sizeof(acct));
    raw->OnRspQryTradingAccount(&acct, &ok_, 1, true);   // retained object: rejected, no crash
    raw->OnFrontDisconnected(0);
    EXPECT_EQ(0, sink_.n_account.load());
    // The retained instance stays reachable through the plugin quarantine registry.

    ITraderApi* fresh = createTrader();
    ASSERT_TRUE(fresh != NULL);
    deleteTrader(fresh);
    EXPECT_TRUE(fresh == NULL);
}

//////////////////////////////////////////////////////////////////////////
// T2-CTP-07（D）：CTP Touched 'c' 在 WTS 无独立表示。以下仅为裁定前的现状与红控：
//   - 已启用：NotTouched 'b' → WOS_Nottouched（既有修复），Unknown 'a' → WOS_Submitting；
//   - DISABLED：Touched 'c' 不应被报告为“未触发”（WOS_Nottouched）；裁定前预期失败，
//     以 --gtest_also_run_disabled_tests 运行作为红控证据，不计通过。
namespace i03s5ctp {
class ContractData : public NullBaseData
{
public:
    WTSContractInfo* ct = nullptr;
    WTSContractInfo* getContract(const char* code, const char* = "", uint32_t = 0) override
    {
        return (ct != nullptr && std::strcmp(code, ct->getCode()) == 0) ? ct : NULL;
    }
};
class OrderSink : public Sink
{
public:
    ContractData cd;
    std::atomic<int> state{-1};
    IBaseDataMgr* getBaseDataMgr() override { return &cd; }
    void onPushOrder(WTSOrderInfo* o) override { state = (int)o->getOrderState(); }
};
static int pushedStateFor(char ctpStatus)
{
    WTSCommodityInfo* comm = WTSCommodityInfo::create("ag", "ag", "SHFE", "TRADING", "CHINA");
    WTSContractInfo* ct = WTSContractInfo::create("ag2512", "ag2512", "SHFE", "ag");
    ct->setCommInfo(comm);
    OrderSink sink;
    sink.cd.ct = ct;
    Probe trader;
    trader.registerSpi(&sink);
    CThostFtdcOrderField o;
    std::memset(&o, 0, sizeof(o));
    std::strcpy(o.InstrumentID, "ag2512");
    std::strcpy(o.ExchangeID, "SHFE");
    std::strcpy(o.InsertDate, "20240115");
    std::strcpy(o.InsertTime, "10:00:00");
    o.OrderStatus = ctpStatus;
    trader.OnRtnOrder(&o);
    const int state = sink.state.load();
    ct->release();
    comm->release();
    return state;
}
}// namespace i03s5ctp

TEST(I03S5TraderCTPOrderState, KnownCtpStatusesKeepWtsMeaning)
{
    EXPECT_EQ((int)WOS_Nottouched, pushedStateFor(THOST_FTDC_OST_NotTouched));
    EXPECT_EQ((int)WOS_Submitting, pushedStateFor(THOST_FTDC_OST_Unknown));
    EXPECT_EQ((int)WOS_NotTraded_Queuing, pushedStateFor(THOST_FTDC_OST_NoTradeQueueing));
    EXPECT_EQ((int)WOS_Canceled, pushedStateFor(THOST_FTDC_OST_Canceled));
}

TEST(I03S5TraderCTPOrderState, DISABLED_T2CTP07_TouchedIsNotReportedAsUntouched_PendingDecision)
{
    EXPECT_NE((int)WOS_Nottouched, pushedStateFor(THOST_FTDC_OST_Touched))
        << "CTP Touched('c') is cast to WOS_Nottouched('c'): a triggered order is reported as not triggered";
}
