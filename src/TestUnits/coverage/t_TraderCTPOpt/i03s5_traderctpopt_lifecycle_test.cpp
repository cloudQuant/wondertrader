/*
 * 迭代03 s5 traders-ctp：TraderCTPOpt 卡9 生命周期与身份分配
 *   T1-CTPOpt-02：在途 API lease —— Req* 与 Release 不重叠，同步回调重入不死锁，回调内释放延后。
 *   T1-CTPOpt-01：析构在闸门排空后归还查询数组容器。
 *   T2-CTP-08：缺委托 ID 原子分配唯一 OrderRef，合法预分配 ID 保留，同一身份不重复提交。
 *
 * 只用本地 R61CtpOptApiStub 派生替身，不加载厂商库、不起 socket。
 * WT_I03_S5_TRADERS_BASELINE：用改前产品源码编译红控时定义（新增观察接口以哨兵替代）。
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
#include "Includes/WTSVariant.hpp"

#include "r61_ctpopt_api_stub.h"

extern "C" {
    ITraderApi* createTrader();
    void deleteTrader(ITraderApi*& trader);
}

namespace i03s5opt {

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

/// 原子计数 + 可阻塞的 CTPOpt SDK 替身
class OptApi : public R61CtpOptApiStub
{
public:
    std::atomic<int> active_req{0};
    std::atomic<int> releases{0};
    std::atomic<int> inits{0};
    std::atomic<int> actions{0};
    std::atomic<int> inserts{0};
    std::atomic<int> exec_inserts{0};
    std::atomic<int> account_queries{0};
    std::atomic<bool> release_overlapped{false};
    std::atomic<bool> block_action{false};
    std::atomic<bool> block_account{false};
    std::atomic<bool> entered{false};
    std::atomic<bool> unblock{false};
    CThostFtdcTraderSpi* spi = nullptr;
    std::function<void()> inside_action;
    std::mutex refs_mutex;
    std::vector<std::string> refs;
    CThostFtdcInputOrderActionField last_action;

    void Release() override
    {
        if (active_req.load() != 0)
            release_overlapped = true;
        ++releases;
    }
    void Init() override { ++inits; }
    void RegisterSpi(CThostFtdcTraderSpi* p) override { spi = p; }
    int ReqOrderAction(CThostFtdcInputOrderActionField* req, int) override
    {
        ++active_req;
        ++actions;
        if (req) last_action = *req;
        entered = true;
        while (block_action.load() && !unblock.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (inside_action)
        {
            auto hook = std::move(inside_action);
            inside_action = nullptr;   // a moved-from std::function is unspecified
            hook();
        }
        --active_req;
        return 0;
    }
    int ReqOrderInsert(CThostFtdcInputOrderField* req, int) override
    {
        ++inserts;
        std::lock_guard<std::mutex> lock(refs_mutex);
        refs.emplace_back(req->OrderRef);
        return 0;
    }
    int ReqExecOrderInsert(CThostFtdcInputExecOrderField* req, int) override
    {
        ++exec_inserts;
        std::lock_guard<std::mutex> lock(refs_mutex);
        refs.emplace_back(req->ExecOrderRef);
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
    std::vector<std::string> logs;
    std::vector<std::pair<int, int32_t>> events;
    std::atomic<int> trader_errors{0};
    std::function<void()> on_error_hook;

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
    void onLoginResult(bool, const char*, uint32_t) override {}
    void onTraderError(WTSError*, void* = NULL) override
    {
        ++trader_errors;
        if (on_error_hook)
        {
            auto hook = std::move(on_error_hook);
            on_error_hook = nullptr;
            hook();
        }
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

class Probe : public TraderCTPOpt
{
public:
    void setApi(CThostFtdcTraderApi* api) { m_pUserAPI = api; }
    CThostFtdcTraderApi* api() const { return m_pUserAPI; }
    void setReady(uint32_t front, uint32_t session, uint32_t ref)
    {
        m_wrapperState = WS_ALLREADY;
        m_frontID = front;
        m_sessionID = session;
        m_orderRef = ref;
        m_strBroker = "9999";
        m_strUser = "tester";
    }
    void setCreator(CTPCreator c) { m_funcCreator = c; }
    void setFlowDir(const std::string& d) { m_strFlowDir = d; m_strBroker = "9999"; m_strUser = "tester"; m_strFront = "tcp://127.0.0.1:1"; }
    void loadIni(const std::string& path) { m_iniHelper.load(path.c_str()); }
    std::string iniString(const char* sec, const char* key) { return m_iniHelper.readString(sec, key); }
    bool hasWorker() const { return m_thrdWorker != NULL; }
    bool inQuery() const { return m_bInQuery; }
    WTSArray* execOrders() const { return m_ayExecOrds; }
#ifdef WT_I03_S5_TRADERS_BASELINE
    int stopState() const { return -1; }
    void setStopBound(uint32_t) {}
#else
    int stopState() const { return (int)lifecycleState(); }
    void setStopBound(uint32_t ms) { m_stopBoundMs = ms; }
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
static const int kUncertain = 3;

static CThostFtdcTraderApi* g_api = nullptr;
static std::atomic<int> g_creator_calls{0};
static CThostFtdcTraderApi* optCreator(const char*)
{
    ++g_creator_calls;
    return g_api;
}

static WTSEntrustAction* makeCancel()
{
    WTSEntrustAction* act = WTSEntrustAction::create("rb2610", "CFFEX");
    act->setEntrustID("000005#0000000006#000042");
    act->setActionFlag(WAF_CANCEL);
    act->setOrderID("SYS42");
    return act;
}

static WTSEntrust* makeOrder(WTSContractInfo* ct)
{
    WTSEntrust* e = WTSEntrust::create("rb2610", 1, 3500.0, "CFFEX");
    e->setContractInfo(ct);
    e->setDirection(WDT_LONG);
    e->setOffsetType(WOT_OPEN);
    e->setPriceType(WPT_LIMITPRICE);
    e->setOrderFlag(WOF_NOR);
    return e;
}

class I03S5TraderCTPOptLifecycle : public ::testing::Test
{
protected:
    OptApi api_;
    Sink sink_;
    Probe trader_;
    std::filesystem::path tmp_;
    WTSCommodityInfo* comm_ = nullptr;
    WTSContractInfo* ct_ = nullptr;

    void SetUp() override
    {
        std::string tag = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        tmp_ = std::filesystem::temp_directory_path() / ("i03s5_opt_" + tag + "_" + std::to_string((long)::getpid()));
        std::error_code ec;
        std::filesystem::create_directories(tmp_, ec);
        trader_.registerSpi(&sink_);
        trader_.loadIni((tmp_ / "entrust.ini").string());
        comm_ = WTSCommodityInfo::create("rb", "rb", "CFFEX", "TRADING", "CHINA");
        ct_ = WTSContractInfo::create("rb2610", "rb2610", "CFFEX", "rb");
        ct_->setCommInfo(comm_);
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
};

}// namespace i03s5opt

using namespace i03s5opt;

// T1-CTPOpt-02 验收：安全 stub 并发触发一次 adapter cancel 与 teardown：无空指针、Release 与 Req 不重叠、停机完成；
// 正常 ready 状态撤单字段映射不变。
TEST_F(I03S5TraderCTPOptLifecycle, ConcurrentCancelAndReleaseNeverOverlap)
{
    trader_.setApi(&api_);
    trader_.setReady(5, 6, 0);
    api_.block_action = true;
    WTSEntrustAction* act = makeCancel();
    int cancelResult = 99;
    std::thread adapter([&]() { cancelResult = trader_.orderAction(act); });
    ASSERT_TRUE(waitFor([this]() { return api_.entered.load(); }));
    std::thread unblocker([this]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        api_.unblock = true;
    });
    {
        Watchdog dog("release during in-flight cancel", 6000);
        trader_.release();
    }
    adapter.join();
    unblocker.join();
    EXPECT_EQ(0, cancelResult);
    EXPECT_EQ(5, api_.last_action.FrontID);
    EXPECT_EQ(6, api_.last_action.SessionID);
    EXPECT_STREQ("42", api_.last_action.OrderRef);
    EXPECT_STREQ("SYS42", api_.last_action.OrderSysID);
    EXPECT_EQ(1, api_.releases.load());
    EXPECT_FALSE(api_.release_overlapped.load());
    EXPECT_EQ(kReleased, trader_.stopState());
    EXPECT_EQ(-1, trader_.orderAction(act));     // teardown complete: no new Req*
    EXPECT_EQ(1, api_.actions.load());
    act->release();
}

// 同步回调重入：Req 内同步回报错误 → 宿主回调里再撤单（不得死锁）并请求释放（延后到安全点）。
TEST_F(I03S5TraderCTPOptLifecycle, SynchronousCallbackReentryDefersReleaseWithoutDeadlock)
{
    trader_.setApi(&api_);
    trader_.setReady(5, 6, 0);
    WTSEntrustAction* act = makeCancel();
    int nested = 99, releasesInside = -1, stateInside = -1;
    sink_.on_error_hook = [&]() {
        nested = trader_.orderAction(act);
        trader_.release();
        releasesInside = api_.releases.load();
        stateInside = trader_.stopState();
    };
    api_.inside_action = [&]() {
        CThostFtdcRspInfoField err;
        std::memset(&err, 0, sizeof(err));
        err.ErrorID = 26;
        std::strcpy(err.ErrorMsg, "sync reject");
        CThostFtdcInputOrderActionField f;
        std::memset(&f, 0, sizeof(f));
        std::strcpy(f.InstrumentID, "rb2610");
        std::strcpy(f.ExchangeID, "CFFEX");
        api_.spi = &trader_;
        trader_.OnRspOrderAction(&f, &err, 7, true);
    };
    {
        Watchdog dog("synchronous callback re-entry", 6000);
        EXPECT_EQ(0, trader_.orderAction(act));
    }
    EXPECT_EQ(1, sink_.trader_errors.load());
    EXPECT_EQ(0, nested);                 // nested cancel admitted before the stop request
    EXPECT_EQ(0, releasesInside);         // no Release() inside the SDK call
    EXPECT_EQ(kDeferred, stateInside);
    EXPECT_FALSE(api_.release_overlapped.load());
    {
        Watchdog dog("deferred release completion", 6000);
        trader_.release();
    }
    EXPECT_EQ(1, api_.releases.load());
    EXPECT_EQ(kReleased, trader_.stopState());
    EXPECT_EQ(-1, trader_.orderAction(act));
    act->release();
}

// worker 卡在 SDK 查询请求内：disconnect 有界返回 DrainUncertain 且不 Release；请求返回后重试完成。
TEST_F(I03S5TraderCTPOptLifecycle, WorkerBlockedInsideQueryIsUncertainThenRetrySucceeds)
{
    trader_.setFlowDir(tmp_.string() + "/");
    trader_.setCreator(&optCreator);
    trader_.connect();
    ASSERT_EQ(1, g_creator_calls.load());
    ASSERT_TRUE(trader_.hasWorker());
    trader_.setStopBound(300);
    trader_.setReady(5, 6, 0);
    api_.block_account = true;
    ASSERT_EQ(0, trader_.queryAccount());
    ASSERT_TRUE(waitFor([this]() { return api_.entered.load(); }));
    {
        Watchdog dog("disconnect with worker blocked in SDK", 4000);
        const auto start = std::chrono::steady_clock::now();
        trader_.disconnect();
        EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count(), 1500);
    }
    EXPECT_EQ(kUncertain, trader_.stopState());
    EXPECT_EQ(0, api_.releases.load());
    api_.unblock = true;
    ASSERT_TRUE(waitFor([this]() { return api_.active_req.load() == 0; }));
    {
        Watchdog dog("retry release", 6000);
        trader_.release();
    }
    EXPECT_EQ(kReleased, trader_.stopState());
    EXPECT_EQ(1, api_.releases.load());
    EXPECT_FALSE(api_.release_overlapped.load());
    EXPECT_FALSE(trader_.hasWorker());
}

// 卡9c/T2-CTP-03 同型：重复 connect 保留原 API；释放后 connect 被拒绝并以 WTE_Connect 非零可观察。
TEST_F(I03S5TraderCTPOptLifecycle, RepeatedConnectKeepsApiAndConnectAfterReleaseIsRejected)
{
    trader_.setFlowDir(tmp_.string() + "/");
    trader_.setCreator(&optCreator);
    trader_.connect();
    trader_.connect();
    EXPECT_EQ(1, g_creator_calls.load());
    EXPECT_EQ(1, api_.inits.load());
    {
        Watchdog dog("disconnect", 6000);
        trader_.disconnect();
    }
    trader_.connect();
    EXPECT_EQ(1, g_creator_calls.load());
    EXPECT_EQ(1, sink_.connectFailures());
    EXPECT_EQ(1, api_.releases.load());
}

// T1-CTPOpt-01：析构在闸门关闭并排空后归还行权查询数组（以引用计数观察）。
TEST_F(I03S5TraderCTPOptLifecycle, DestructorReturnsQueryArrayOwnership)
{
    Probe* probe = new Probe();
    probe->registerSpi(&sink_);
    CThostFtdcExecOrderField first;
    std::memset(&first, 0, sizeof(first));
    std::snprintf(first.InstrumentID, sizeof(first.InstrumentID), "%s", "rb2610");
    std::snprintf(first.ExchangeID, sizeof(first.ExchangeID), "%s", "CFFEX");
    std::snprintf(first.ExecOrderRef, sizeof(first.ExecOrderRef), "%s", "41");
    first.Volume = 1;
    probe->OnRspQryExecOrder(&first, nullptr, 1, false);
    WTSArray* arr = probe->execOrders();
    ASSERT_NE(nullptr, arr);
    arr->retain();
    EXPECT_EQ(2u, arr->retainCount());
    delete probe;
    EXPECT_EQ(1u, arr->retainCount());
    arr->release();
}

// T2-CTP-08 / 卡6d：两张无 tag 无 ID 单得到不同 OrderRef；makeEntrustID 预分配 ID 在空 tag 时保留且不再分配；
// 重复提交、其他会话 ID、非法格式拒绝；并发分配唯一。
TEST_F(I03S5TraderCTPOptLifecycle, MissingIdentityIsAllocatedUniqueAndPreallocatedIdKept)
{
    trader_.setApi(&api_);
    trader_.setReady(5, 6, 100);
    WTSEntrust* a = makeOrder(ct_);
    WTSEntrust* b = makeOrder(ct_);
    ASSERT_EQ(0, trader_.orderInsert(a));
    ASSERT_EQ(0, trader_.orderInsert(b));
    ASSERT_EQ(2u, api_.refs.size());
    EXPECT_NE(api_.refs[0], api_.refs[1]);
    EXPECT_EQ("101", api_.refs[0]);
    EXPECT_EQ("102", api_.refs[1]);
    EXPECT_STREQ("000005#0000000006#000101", a->getEntrustID());
    EXPECT_STREQ("ctpopt-auto:000005#0000000006#000101", a->getUserTag());
    EXPECT_EQ("ctpopt-auto:000005#0000000006#000101", trader_.iniString("entrusts", "000005#0000000006#000101"));

    char reserved[64] = { 0 };
    ASSERT_TRUE(trader_.makeEntrustID(reserved, sizeof(reserved)));
    WTSEntrust* c = makeOrder(ct_);
    c->setEntrustID(reserved);          // empty tag, legal preallocated id
    ASSERT_EQ(0, trader_.orderInsert(c));
    EXPECT_EQ("103", api_.refs.back());
    EXPECT_STREQ(reserved, c->getEntrustID());
    EXPECT_EQ(std::string("ctpopt-auto:") + reserved, c->getUserTag());

    WTSEntrust* tagged = makeOrder(ct_);
    tagged->setEntrustID("000005#0000000006#000500");
    tagged->setUserTag("otp.owner.1");
    ASSERT_EQ(0, trader_.orderInsert(tagged));
    EXPECT_EQ("500", api_.refs.back());
    EXPECT_STREQ("otp.owner.1", tagged->getUserTag());
    WTSEntrust* after = makeOrder(ct_);
    ASSERT_EQ(0, trader_.orderInsert(after));
    EXPECT_EQ("501", api_.refs.back());   // allocation stays above the kept reference

    const size_t before = api_.refs.size();
    EXPECT_EQ(-1, trader_.orderInsert(a));               // duplicate submission
    EXPECT_EQ(-1, trader_.orderInsert(tagged));
    for (const char* bad : { "000005#0000000007#000600", "000004#0000000006#000600", "5#6#0", "5#6#x1", "5#6", "5#6#7#8", "#6#600" })
    {
        WTSEntrust* e = makeOrder(ct_);
        e->setEntrustID(bad);
        EXPECT_EQ(-1, trader_.orderInsert(e)) << bad;
        e->release();
    }
    EXPECT_EQ(before, api_.refs.size());

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
    {
        threads.emplace_back([this]() {
            for (int i = 0; i < 50; ++i)
            {
                WTSEntrust* e = makeOrder(ct_);
                trader_.orderInsert(e);
                e->release();
            }
        });
    }
    for (auto& th : threads)
        th.join();
    std::set<std::string> unique(api_.refs.begin(), api_.refs.end());
    EXPECT_EQ(api_.refs.size(), unique.size());
    EXPECT_EQ(before + 200, api_.refs.size());
    a->release(); b->release(); c->release(); tagged->release(); after->release();
}

// 卡9a/9b：创建过 SDK 的实例被工厂 remover 隔离（保留指针、实例存活），迟到回调被拒绝。
TEST_F(I03S5TraderCTPOptLifecycle, FactoryRemoverQuarantinesInstanceWithSdk)
{
    Probe* raw = new Probe();
    raw->registerSpi(&sink_);
    raw->setFlowDir(tmp_.string() + "/q/");
    raw->setCreator(&optCreator);
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
    CThostFtdcRspInfoField err;
    std::memset(&err, 0, sizeof(err));
    err.ErrorID = 1;
    raw->OnRspError(&err, 1, true);
    ITraderApi* fresh = createTrader();
    deleteTrader(fresh);
    EXPECT_TRUE(fresh == NULL);
}

// N-1（主线程追加任务，与 TraderCTP 已修子项对齐）：CTP NotTouched('b') 必须报告为 WOS_Nottouched，
// 改前被数值强转为 WOS_Cancelling('b')。Touched('c') 待 D-T2-CTP-07。
namespace i03s5opt {
class ContractData : public NullBaseData
{
public:
    WTSContractInfo* ct = nullptr;
    WTSContractInfo* getContract(const char* code, const char* = "", uint32_t = 0) override
    {
        return (ct != nullptr && std::strcmp(code, ct->getCode()) == 0) ? ct : NULL;
    }
};
class StateSink : public Sink
{
public:
    ContractData cd;
    int state = -1;
    IBaseDataMgr* getBaseDataMgr() override { return &cd; }
    void onPushOrder(WTSOrderInfo* o) override { state = (int)o->getOrderState(); }
};
}
TEST_F(I03S5TraderCTPOptLifecycle, NotTouchedIsReportedAsNottouched)
{
    i03s5opt::StateSink s;
    s.cd.ct = ct_;
    trader_.registerSpi(&s);
    CThostFtdcOrderField o;
    std::memset(&o, 0, sizeof(o));
    std::strcpy(o.InstrumentID, "rb2610");
    std::strcpy(o.ExchangeID, "CFFEX");
    std::strcpy(o.InsertTime, "10:00:00");
    o.OrderStatus = THOST_FTDC_OST_NotTouched;
    trader_.OnRtnOrder(&o);
    EXPECT_EQ((int)WOS_Nottouched, s.state);
    // K01=B（2026-11-04）：Touched('c') 映射为 WOS_Submitting，不再被强转为 WOS_Nottouched。
    o.OrderStatus = THOST_FTDC_OST_Touched;
    trader_.OnRtnOrder(&o);
    EXPECT_EQ((int)WOS_Submitting, s.state);
}

/* ------------------------------------------------------------------ *
 * I03-NEW-133（与 TraderCTP 的 I03-NEW-131 同一合同，卡14c）：结算单查询末帧
 * 不得解引用 NULL 载荷；末帧结束本次查询，一次查询只给一次结果：
 *   - 已缓存内容按最近一行的 TradingDay 推送；
 *   - 有未完成请求而无内容（无载荷或空内容末帧）时推送请求日的空结果；
 *   - 错误末帧且无缓存不推送。
 * ------------------------------------------------------------------ */
namespace i03s5opt {

class SettleSink : public Sink
{
public:
    uint32_t n = 0;
    uint32_t date = 0;
    std::string content;
    void onRspSettlementInfo(uint32_t uDate, const char* c) override
    {
        ++n;
        date = uDate;
        content = (c == NULL) ? "<null>" : c;
    }
};

static CThostFtdcSettlementInfoField settleRow(const char* day, const char* content)
{
    CThostFtdcSettlementInfoField f;
    std::memset(&f, 0, sizeof(f));
    std::strncpy(f.TradingDay, day, sizeof(f.TradingDay) - 1);
    std::strncpy(f.Content, content, sizeof(f.Content) - 1);
    return f;
}

/// 局部 sink 先于 fixture 的 trader_ 析构：结束时（含 ASSERT 早退）恢复 fixture sink 并摘掉 stub API。
struct SettleScope
{
    Probe& trader;
    Sink& fixtureSink;
    ~SettleScope() { trader.setApi(NULL); trader.registerSpi(&fixtureSink); }
};

}// namespace i03s5opt

TEST_F(I03S5TraderCTPOptLifecycle, S3New133NullLastRowEndsQueryWithCachedContent)
{
    SettleSink s;
    SettleScope scope{ trader_, sink_ };
    trader_.registerSpi(&s);
    trader_.setApi(&api_);
    trader_.setReady(5, 6, 0);
    CThostFtdcRspInfoField ok;
    std::memset(&ok, 0, sizeof(ok));

    ASSERT_EQ(0, trader_.querySettlement(20240116));
    CThostFtdcSettlementInfoField r1 = settleRow("20240115", "line-A");
    trader_.OnRspQrySettlementInfo(&r1, &ok, 1, false);
    EXPECT_EQ(0u, s.n);
    trader_.OnRspQrySettlementInfo(NULL, &ok, 1, true);
    EXPECT_FALSE(trader_.inQuery());
    ASSERT_EQ(1u, s.n);
    EXPECT_EQ(20240115u, s.date);
    EXPECT_EQ("line-A", s.content);

    trader_.OnRspQrySettlementInfo(NULL, &ok, 1, true);   // 迟到的重复末帧
    EXPECT_EQ(1u, s.n);

    ASSERT_EQ(0, trader_.querySettlement(20240117));
    CThostFtdcSettlementInfoField r2 = settleRow("20240117", "line-B");
    trader_.OnRspQrySettlementInfo(&r2, &ok, 2, true);
    EXPECT_EQ(2u, s.n);
    EXPECT_EQ(20240117u, s.date);
    EXPECT_EQ("line-B", s.content);
}

TEST_F(I03S5TraderCTPOptLifecycle, S3New133EmptyResponseReportsEmptyResultOnce)
{
    SettleSink s;
    SettleScope scope{ trader_, sink_ };
    trader_.registerSpi(&s);
    trader_.setApi(&api_);
    trader_.setReady(5, 6, 0);
    CThostFtdcRspInfoField ok;
    std::memset(&ok, 0, sizeof(ok));

    ASSERT_EQ(0, trader_.querySettlement(20240116));
    trader_.OnRspQrySettlementInfo(NULL, &ok, 1, true);
    ASSERT_EQ(1u, s.n);
    EXPECT_EQ(20240116u, s.date);
    EXPECT_EQ("", s.content);
    trader_.OnRspQrySettlementInfo(NULL, NULL, 1, true);
    EXPECT_EQ(1u, s.n);

    ASSERT_EQ(0, trader_.querySettlement(20240117));
    CThostFtdcSettlementInfoField blank = settleRow("", "");
    trader_.OnRspQrySettlementInfo(&blank, &ok, 2, true);  // 空内容载荷末帧
    ASSERT_EQ(2u, s.n);
    EXPECT_EQ(20240117u, s.date);
    EXPECT_EQ("", s.content);
}

TEST_F(I03S5TraderCTPOptLifecycle, S3New133ErrorLastFrameWithoutPayloadReportsNothing)
{
    SettleSink s;
    SettleScope scope{ trader_, sink_ };
    trader_.registerSpi(&s);
    trader_.setApi(&api_);
    trader_.setReady(5, 6, 0);
    CThostFtdcRspInfoField err;
    std::memset(&err, 0, sizeof(err));
    err.ErrorID = 3;

    ASSERT_EQ(0, trader_.querySettlement(20240116));
    trader_.OnRspQrySettlementInfo(NULL, &err, 1, true);
    EXPECT_FALSE(trader_.inQuery());
    EXPECT_EQ(0u, s.n);
}

/* ------------------------------------------------------------------ *
 * I03-NEW-135（与 TraderCTP 的 I03-NEW-134 同一合同，卡14c）：资金查询的非错误末帧
 * 不带载荷时不得解引用；结束查询并推送非 NULL 的空资金数组，不伪造资金字段。
 * 错误末帧仍不推送；非末帧的 NULL 载荷不推送。
 * ------------------------------------------------------------------ */
namespace i03s5opt {

class AccountSink : public Sink
{
public:
    uint32_t n = 0;
    bool last_null = false;
    uint32_t last_size = 0;
    void onRspAccount(WTSArray* ay) override
    {
        ++n;
        last_null = (ay == NULL);
        last_size = (ay == NULL) ? 0 : ay->size();
    }
};

}// namespace i03s5opt

TEST_F(I03S5TraderCTPOptLifecycle, S3New135NullAccountLastFrameReportsEmptyAccountList)
{
    AccountSink s;
    SettleScope scope{ trader_, sink_ };
    trader_.registerSpi(&s);
    trader_.setApi(&api_);
    trader_.setReady(5, 6, 0);
    CThostFtdcRspInfoField ok;
    std::memset(&ok, 0, sizeof(ok));

    trader_.OnRspQryTradingAccount(NULL, &ok, 1, true);
    EXPECT_FALSE(trader_.inQuery());
    ASSERT_EQ(1u, s.n);
    EXPECT_FALSE(s.last_null);
    EXPECT_EQ(0u, s.last_size);

    trader_.OnRspQryTradingAccount(NULL, NULL, 2, true);
    EXPECT_EQ(2u, s.n);
    EXPECT_FALSE(s.last_null);
    EXPECT_EQ(0u, s.last_size);

    CThostFtdcRspInfoField err;
    std::memset(&err, 0, sizeof(err));
    err.ErrorID = 42;
    trader_.OnRspQryTradingAccount(NULL, &err, 3, true);
    EXPECT_EQ(2u, s.n);                                // 错误末帧不推送

    trader_.OnRspQryTradingAccount(NULL, &ok, 4, false);
    EXPECT_EQ(2u, s.n);                                // 非末帧不推送

    // 对照：带载荷的末帧照常推送一条资金
    CThostFtdcTradingAccountField acct;
    std::memset(&acct, 0, sizeof(acct));
    acct.PreBalance = 1000.0;
    acct.Available = 800.0;
    trader_.OnRspQryTradingAccount(&acct, &ok, 5, true);
    EXPECT_EQ(3u, s.n);
    EXPECT_EQ(1u, s.last_size);
}
