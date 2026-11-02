/**
 * r3_trader_risklimit.cpp —— lane R3（WtCore 第三波）
 *
 * 覆盖 src/WtCore/TraderAdapter.cpp 的以下行段（行号按当前 dev 版本）：
 *   L237-279  initSaveData：trades.csv / orders.csv / rtdata.json 的落地目录（L245 create_directories）
 *   L303-390  saveData(WTSArray*)：positions(long/short 四段) + funds 段 JSON（含 L305-384）
 *   L428-466  getPosition（全量/可用两种口径）+ enumPosition
 *   L487-520  doEntrust：usertag 组装（_order_pattern + '.' + localid）+ orderInsert 失败返回 UINT_MAX
 *   L544-596  checkCancelLimits：撤单频率窗口（L566-593）
 *   L609-661  checkOrderLimits：下单频率窗口（L631-658）
 *   L663-830  buy()：qty==0 早退 / 非交易时段拒绝 / 规则限额 + 单笔最大委托量拆单
 *   L1244-1291 doCancel + cancel(localid)：撤单频率缓存写入（L1278 SpinLock、L1286 cache）
 *   L1293-1329 cancel(stdCode,isBuy,qty)：按方向与代码筛选活单
 *   L1331-1404 openLong/openShort/closeLong/closeShort：方向·开平·价格类型·orderflag 组装
 *   L1514-1543 onRspAccount -> saveData 带动
 * 交易通道仍用 lane p11 的 FakeTraderApi 替身（不起线程、不联网）。
 * 频率窗口的「过期条目裁剪」支路（L588-591 / L653-656）需要可控时钟：缓存里的时间戳由
 * TimeUtils::getLocalTimeNow() 在 doEntrust/doCancel 内部写入，公开接口无法注入历史时间点，
 * 本轮未打（见 bugs/R3_wtcore.md 的接缝清单）。
 */
#include <cstdlib>
#include <chrono>
#include <cerrno>
#include <functional>
#include <signal.h>
#include <spawn.h>
#include <thread>
#include <sys/wait.h>
#include <vector>

#include <Includes/WTSCollection.hpp>
#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSTradeDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <Share/CodeHelper.hpp>
#include <Share/StdUtils.hpp>

#define private public
#include <WtCore/TraderAdapter.h>
#undef private

#include "../p11_wtcore_traderadapter/p11_trader_fixture.h"

#include <Share/DLLHelper.hpp>
#include <Share/ModuleHelper.hpp>
#include <WtCore/ActionPolicyMgr.h>
#include <Includes/WTSSessionInfo.hpp>
#include <rapidjson/document.h>

#ifndef WT_P11_TRADERMOCKER_PATH
#error "WT_P11_TRADERMOCKER_PATH must point to the built TraderMocker plugin"
#endif

using namespace p11fix;
using wtp::TraderAdapter;

extern char** environ;

namespace {

const char* kStdAu = "SHFE.au.2212";

static std::string r3TmpDir(const char* tag)
{
    std::filesystem::path p = std::filesystem::temp_directory_path()
        / (std::string("r3_trader_") + tag + "_" + std::to_string((long)::getpid()));
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p.string() + "/";
}

static WTSVariant* r3LoadJson(const std::string& dir, const char* fname, const std::string& body)
{
    std::string path = dir + fname;
    std::ofstream ofs(path, std::ios::out | std::ios::trunc);
    ofs << body;
    ofs.flush();
    ofs.close();
    return WTSCfgLoader::load_from_file(path.c_str());
}

/// 活单：不调 setOrderState，与 lane p11 的 makeAliveOrder 同构
WTSOrderInfo* r3Order(WTSContractInfo* ct, WTSDirectionType dir, WTSOffsetType off,
    double vol, double traded, const char* oid, const char* utag)
{
    WTSOrderInfo* o = WTSOrderInfo::create();
    o->setCode(ct->getCode());
    o->setExchange(ct->getExchg());
    o->setContractInfo(ct);
    o->setDirection(dir);
    o->setOffsetType(off);
    o->setOrderFlag(WOF_NOR);
    o->setPrice(7000.0);
    o->setVolume(vol);
    o->setVolTraded(traded);
    o->setVolLeft(vol - traded);
    o->setOrderID(oid);
    o->setEntrustID("P11E001");
    o->setUserTag(utag);
    o->setOrderDate(20250110);
    o->setOrderTime(93000);
    o->setStateMsg("ok");
    return o;
}

WTSOrderInfo* r3DeadOrder(WTSContractInfo* ct, WTSDirectionType dir, WTSOffsetType off,
    double vol, double traded, const char* oid, const char* utag)
{
    WTSOrderInfo* o = r3Order(ct, dir, off, vol, traded, oid, utag);
    o->setOrderState(WOS_Canceled);
    return o;
}

WTSPositionItem* r3Pos(WTSContractInfo* ct, WTSDirectionType dir,
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
WTSArray* r3Array(std::initializer_list<T*> items)
{
    WTSArray* arr = WTSArray::create();
    for (auto* it : items)
        arr->append(it);
    return arr;
}

template <typename T>
WTSArray* r3OwnedArray(std::initializer_list<T*> items)
{
    WTSArray* arr = WTSArray::create();
    for (auto* it : items)
    {
        arr->append(it);
        it->release(); // transfer each create() reference to the array
    }
    return arr;
}

static std::string r3ReadAll(const std::string& path)
{
    std::ifstream ifs(path, std::ios::binary);
    std::stringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

template <typename Fn>
static int r3RunInIsolatedProcess(const char* testName, Fn&& body)
{
    if (std::getenv("WT_R3_ISOLATED_CHILD") != nullptr)
        return body();

    const std::filesystem::path executablePath =
        std::filesystem::path(getBinDir()) / "CoverageTests_wtcore";
    const std::string executable = executablePath.string();
    const std::string filter = std::string("--gtest_filter=") + testName;
    char* args[] = {
        const_cast<char*>(executable.c_str()),
        const_cast<char*>(filter.c_str()),
        nullptr
    };

    std::vector<std::string> environment;
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        const std::string item(*entry);
        if (item.compare(0, sizeof("WT_R3_ISOLATED_CHILD=") - 1,
                "WT_R3_ISOLATED_CHILD=") != 0)
            environment.push_back(item);
    }
    environment.emplace_back("WT_R3_ISOLATED_CHILD=1");
    std::vector<char*> envp;
    envp.reserve(environment.size() + 1);
    for (std::string& item : environment)
        envp.push_back(item.data());
    envp.push_back(nullptr);

    pid_t child = -1;
    const int spawnError = ::posix_spawn(&child, executable.c_str(), nullptr, nullptr, args, envp.data());
    if (spawnError != 0)
        return -1000 - spawnError;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    int status = 0;
    for (;;) {
        const pid_t waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child)
            break;
        if (waited < 0 && errno != EINTR)
            return -1;
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(child, SIGKILL);
            while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            for (uint32_t sequence = 1; sequence <= 16; ++sequence) {
                const uint32_t mockerId = (uint32_t)child * 100u + sequence;
                std::error_code ec;
                std::filesystem::remove_all(
                    std::filesystem::path("mocker_" + std::to_string(mockerId)), ec);
            }
            return -3;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    for (uint32_t sequence = 1; sequence <= 16; ++sequence) {
        const uint32_t mockerId = (uint32_t)child * 100u + sequence;
        std::error_code ec;
        std::filesystem::remove_all(
            std::filesystem::path("mocker_" + std::to_string(mockerId)), ec);
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return -2;
}

struct R3MockerDirCleanup
{
    const std::vector<uint32_t>& ids;

    ~R3MockerDirCleanup()
    {
        std::error_code ec;
        for (uint32_t id : ids)
            std::filesystem::remove_all(std::filesystem::path("mocker_" + std::to_string(id)), ec);
    }
};

/**
 * 在 p11 夹具之上补三件事，让 buy()/sell() 可达：
 *   1) 品种挂上带交易时段的 session（否则 L678-680 空指针）
 *   2) WtHelper::setTime 落在时段内（L680 读的是全局时间）
 *   3) 真实的 ActionPolicyMgr（L701 无判空，必须给非 NULL）
 */
class R3TraderTest : public P11TraderTest
{
protected:
    WTSSessionInfo*  sess_ = NULL;
    ActionPolicyMgr  policy_;
    std::string      dir_;
    std::string      oldInstDir_;
    std::string      moduleInstallDir_;
    std::vector<uint32_t> mockerIds_;
    uint32_t         nextMockerId_ = 1;

    void SetUp() override
    {
        P11TraderTest::SetUp();
        dir_ = r3TmpDir("policy");
        oldInstDir_ = WtHelper::getInstDir();
        moduleInstallDir_ = dir_ + "install/";
        namespace fs = std::filesystem;
        const std::string sourceModule = DLLHelper::wrap_module("TraderMocker");
        const fs::path loaderCwd(WtHelper::getCWD());
        std::error_code cwdEc;
        const fs::path processCwd = fs::current_path(cwdEc);
        ASSERT_FALSE(cwdEc) << "cannot inspect process CWD: " << cwdEc.message();
        const bool sameCwd = fs::equivalent(loaderCwd, processCwd, cwdEc);
        ASSERT_FALSE(cwdEc) << "cannot compare WtHelper CWD with process CWD: " << cwdEc.message();
        ASSERT_TRUE(sameCwd) << "WtHelper CWD cache differs from the process CWD";
        const fs::path cwdModule = loaderCwd / "traders" / sourceModule;
        const fs::file_status cwdModuleStatus = fs::symlink_status(cwdModule);
        ASSERT_TRUE(cwdModuleStatus.type() == fs::file_type::not_found)
            << "CWD already contains a TraderMocker plugin; refusing to load or overwrite it: "
            << cwdModule.string();

        const fs::path sourcePath(WT_P11_TRADERMOCKER_PATH);
        ASSERT_TRUE(sourcePath.is_absolute())
            << "WT_P11_TRADERMOCKER_PATH must be absolute: " << sourcePath.string();
        std::error_code sourceEc;
        const fs::file_status sourceStatus = fs::symlink_status(sourcePath, sourceEc);
        ASSERT_FALSE(sourceEc) << "cannot inspect TraderMocker plugin: " << sourceEc.message();
        ASSERT_TRUE(fs::is_regular_file(sourceStatus))
            << "TraderMocker plugin must be a non-symlink regular file: " << sourcePath.string();

        const std::filesystem::path moduleDir = std::filesystem::path(moduleInstallDir_) / "traders";
        std::error_code moduleEc;
        std::filesystem::create_directories(moduleDir, moduleEc);
        ASSERT_FALSE(moduleEc);
        const fs::path stagedModule = moduleDir / sourceModule;
        const bool copied = fs::copy_file(sourcePath, stagedModule,
            std::filesystem::copy_options::overwrite_existing, moduleEc);
        ASSERT_TRUE(copied) << moduleEc.message();
        ASSERT_FALSE(moduleEc);
        std::error_code stagedEc;
        const fs::file_status stagedStatus = fs::symlink_status(stagedModule, stagedEc);
        ASSERT_FALSE(stagedEc) << "cannot inspect staged TraderMocker plugin: " << stagedEc.message();
        ASSERT_TRUE(fs::is_regular_file(stagedStatus))
            << "staged TraderMocker plugin must be a non-symlink regular file: "
            << stagedModule.string();
        WtHelper::setInstDir(moduleInstallDir_.c_str());
        sess_ = WTSSessionInfo::create("TRADING", "TRADING", 0);
        sess_->addTradingSection(900, 1130);
        comm_->setSessionInfo(sess_);
        WtHelper::setTime(20250110, 931, 0);
    }

    void TearDown() override
    {
        if (sess_)
        {
            sess_->release();
            sess_ = NULL;
        }
        std::error_code ec;
        WtHelper::setInstDir(oldInstDir_.c_str());
        for (uint32_t mockerId : mockerIds_)
            std::filesystem::remove_all(std::filesystem::path("mocker_" + std::to_string(mockerId)), ec);
        mockerIds_.clear();
        std::filesystem::remove_all(dir_, ec);
        P11TraderTest::TearDown();
    }

    /// 使用隔离的 TraderMocker 插件成功初始化，确保风控确实激活。
    bool enableRisk(TraderAdapter& trader, uint32_t orderBoundary, uint32_t cancelBoundary,
        bool start = true)
    {
        std::stringstream ss;
        const uint32_t mockerId = (uint32_t)::getpid() * 100u + nextMockerId_++;
        mockerIds_.push_back(mockerId);
        ss << "{\"module\":\"TraderMocker\",\"savedata\":false,\"mockerid\":" << mockerId
           << ",\"span\":10,\"newpx\":false,\"maxqty\":100,\"minqty\":1,\"udp_port\":0,\"riskmon\":{\"active\":true,\"policy\":{"
           << "\"default\":{\"order_times_boundary\":" << orderBoundary
           << ",\"order_stat_timespan\":60,\"order_total_limits\":100"
           << ",\"cancel_times_boundary\":" << cancelBoundary
           << ",\"cancel_stat_timespan\":60,\"cancel_total_limits\":99}}}}";
        WTSVariant* cfg = r3LoadJson(dir_, "risk.json", ss.str());
        if (cfg == nullptr)
            return false;
        const bool initialized = trader.init(id_.c_str(), cfg, &bd_, NULL);
        cfg->release();
        return initialized && (!start || trader.run());
    }

    void loadPolicy(const std::string& body)
    {
        r3LoadJson(dir_, "policy.json", body);
        ASSERT_TRUE(policy_.init((dir_ + "policy.json").c_str()));
    }

    bool waitForOrder(TraderAdapter& trader, uint32_t localid)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            OrderMap* orders = trader.getOrders("");
            if (orders != nullptr) {
                WTSOrderInfo* order = (WTSOrderInfo*)orders->grab(localid);
                orders->release();
                if (order != nullptr) {
                    order->release();
                    return true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
};

//////////////////////////////////////////////////////////////////////////
// checkOrderLimits 的频率窗口
//////////////////////////////////////////////////////////////////////////
TEST_F(R3TraderTest, order_freq_window_denies_submission_after_boundary)
{
    const int result = r3RunInIsolatedProcess(
        "R3TraderTest.order_freq_window_denies_submission_after_boundary", [this]() {
        R3MockerDirCleanup cleanup{ mockerIds_ };
        ct_->setAltCode(kStdCode);
        TraderAdapter trader;
        if (!enableRisk(trader, 2, 9) || !trader.checkOrderLimits(kStdCode))
            return 10;

        // Each successful API submission is preceded by the public precheck.
        for (int i = 0; i < 2; i++) {
            if (!trader.checkOrderLimits(kStdCode) ||
                trader.openLong(kStdCode, 7000.0, 1, 0) == UINT_MAX)
                return 11;
        }
        const bool thirdAllowed = trader.checkOrderLimits(kStdCode);
        if (thirdAllowed)
            (void)trader.openLong(kStdCode, 7000.0, 1, 0);
        const bool cancelAllowed = trader.checkCancelLimits(kStdCode);
        trader.release();
        return thirdAllowed || !cancelAllowed ? 12 : 0;
    });
    EXPECT_EQ(0, result);
}

TEST_F(R3TraderTest, order_freq_window_precheck_uses_standard_code_after_raw_submission)
{
    const int result = r3RunInIsolatedProcess(
        "R3TraderTest.order_freq_window_precheck_uses_standard_code_after_raw_submission", [this]() {
        R3MockerDirCleanup cleanup{ mockerIds_ };
        // Compare the same standard code and policy using independently fresh
        // adapters. Only the API-facing alias changes between the two runs.
        ct_->setAltCode(kStdCode);
        TraderAdapter alignedTrader;
        if (!enableRisk(alignedTrader, 2, 9))
            return 20;

        uint32_t alignedSubmissions = 0;
        bool alignedDenied = false;
        for (uint32_t attempt = 0; attempt < 8; ++attempt) {
            if (!alignedTrader.checkOrderLimits(kStdCode)) {
                alignedDenied = true;
                break;
            }
            if (alignedTrader.openLong(kStdCode, 7000.0, 1, 0) == UINT_MAX) {
                alignedTrader.release();
                return 21;
            }
            ++alignedSubmissions;
        }
        if (!alignedDenied)
            alignedDenied = !alignedTrader.checkOrderLimits(kStdCode);
        alignedTrader.release();
        if (!alignedDenied || alignedSubmissions == 0 || alignedSubmissions > 8)
            return 22;

        ct_->setAltCode(kRawCode);
        TraderAdapter trader;
        if (!enableRisk(trader, 2, 9))
            return 23;
        uint32_t rawSubmissions = 0;
        while (rawSubmissions < alignedSubmissions) {
            if (!trader.checkOrderLimits(kStdCode) ||
                trader.openLong(kStdCode, 7000.0, 1, 0) == UINT_MAX) {
                trader.release();
                return 24;
            }
            ++rawSubmissions;
        }
        const bool nextAllowed = trader.checkOrderLimits(kStdCode);
        trader.release();
        return rawSubmissions == alignedSubmissions && !nextAllowed ? 0 : 25;
    });
    EXPECT_EQ(0, result);
}

//////////////////////////////////////////////////////////////////////////
// checkCancelLimits 的频率窗口
//////////////////////////////////////////////////////////////////////////
TEST_F(R3TraderTest, cancel_freq_window_denies_cancel_after_boundary)
{
    const int result = r3RunInIsolatedProcess(
        "R3TraderTest.cancel_freq_window_denies_cancel_after_boundary", [this]() {
        R3MockerDirCleanup cleanup{ mockerIds_ };
        ct_->setAltCode(kStdCode);
        TraderAdapter trader;
        if (!enableRisk(trader, 9, 2))
            return 30;

        uint32_t localids[3] = {};
        for (uint32_t i = 0; i < 3; ++i) {
            localids[i] = trader.openLong(kStdCode, 7000.0, 1, 0);
            if (localids[i] == UINT_MAX || !waitForOrder(trader, localids[i])) {
                trader.release();
                return 31;
            }
        }
        for (uint32_t i = 0; i < 2; ++i) {
            if (!trader.checkCancelLimits(kStdCode) || !trader.cancel(localids[i])) {
                trader.release();
                return 32;
            }
        }
        const bool thirdAllowed = trader.checkCancelLimits(kStdCode);
        if (thirdAllowed)
            (void)trader.cancel(localids[2]);
        trader.release();
        return thirdAllowed ? 33 : 0;
    });
    EXPECT_EQ(0, result);
}

TEST_F(R3TraderTest, cancel_localid_frequency_cache_uses_standardized_contract_code)
{
    const int result = r3RunInIsolatedProcess(
        "R3TraderTest.cancel_localid_frequency_cache_uses_standardized_contract_code", [this]() {
        R3MockerDirCleanup cleanup{ mockerIds_ };
        std::vector<uint32_t> localids;
        auto seedOrders = [this, &localids](TraderAdapter& trader, bool standardizedOrderCode) {
            localids.clear();
            for (uint32_t i = 0; i < 8; ++i) {
                const uint32_t localid = trader.openLong(kStdCode, 7000.0, 1, 0);
                if (localid == UINT_MAX || !waitForOrder(trader, localid))
                    return false;
                localids.push_back(localid);
                if (standardizedOrderCode) {
                    OrderMap* stored = trader.getOrders("");
                    if (stored == nullptr)
                        return false;
                    WTSOrderInfo* order = (WTSOrderInfo*)stored->grab(localid);
                    if (order == nullptr) {
                        stored->release();
                        return false;
                    }
                    order->setCode(kStdCode);
                    order->release();
                    stored->release();
                }
            }
            return true;
        };

        // The control and alias run use the same actual standard contract,
        // same policy, and independently fresh adapters.
        ct_->setAltCode(kStdCode);
        TraderAdapter alignedTrader;
        if (!enableRisk(alignedTrader, 9, 2) || !seedOrders(alignedTrader, true))
            return 40;
        uint32_t alignedAccepted = 0;
        bool alignedDenied = false;
        for (uint32_t i = 0; i < 8; ++i) {
            if (!alignedTrader.cancel(localids[i])) {
                alignedDenied = true;
                break;
            }
            ++alignedAccepted;
        }
        alignedTrader.release();
        if (!alignedDenied || alignedAccepted == 0)
            return 41;

        ct_->setAltCode(kRawCode);
        TraderAdapter trader;
        if (!enableRisk(trader, 9, 2) || !seedOrders(trader, false))
            return 42;
        uint32_t rawAccepted = 0;
        for (; rawAccepted < alignedAccepted; ++rawAccepted) {
            if (!trader.cancel(localids[rawAccepted])) {
                trader.release();
                return 43;
            }
        }
        const bool nextAllowed = rawAccepted >= localids.size()
            ? true : trader.cancel(localids[rawAccepted]);
        trader.release();
        return rawAccepted == alignedAccepted && !nextAllowed ? 0 : 44;
    });
    EXPECT_EQ(0, result);
}

TEST_F(R3TraderTest, cancel_localid_rejects_inactive_or_unmapped_order_safely)
{
    const int result = r3RunInIsolatedProcess(
        "R3TraderTest.cancel_localid_rejects_inactive_or_unmapped_order_safely", [this]() {
        R3MockerDirCleanup cleanup{ mockerIds_ };
        TraderAdapter trader;
        if (!enableRisk(trader, 9, 2))
            return 50;

        WTSOrderInfo* inactive = r3Order(ct_, WDT_LONG, WOT_OPEN, 1, 0,
            "INACTIVE", "otp.p11a.3101");
        WTSOrderInfo* unmapped = r3Order(ct_, WDT_LONG, WOT_OPEN, 1, 0,
            "UNMAPPED", "otp.p11a.3102");
        WTSArray* orders = r3Array({ inactive, unmapped });
        trader.onRspOrders(orders);
        orders->release();
        inactive->release();
        unmapped->release();

        OrderMap* seeded = trader.getOrders("");
        if (seeded == nullptr)
            return 51;
        WTSOrderInfo* inactiveStored = (WTSOrderInfo*)seeded->grab(3101);
        WTSOrderInfo* unmappedStored = (WTSOrderInfo*)seeded->grab(3102);
        if (inactiveStored == nullptr || unmappedStored == nullptr) {
            if (inactiveStored != nullptr)
                inactiveStored->release();
            if (unmappedStored != nullptr)
                unmappedStored->release();
            seeded->release();
            return 52;
        }
        inactiveStored->setOrderState(WOS_Canceled);
        inactiveStored->setContractInfo(NULL);
        inactiveStored->release();
        unmappedStored->setContractInfo(NULL);
        unmappedStored->release();
        seeded->release();

        const bool inactiveRejected = !trader.cancel(3101);
        const bool unmappedRejected = !trader.cancel(3102);
        trader.release();
        return inactiveRejected && unmappedRejected ? 0 : 53;
    });
    EXPECT_EQ(0, result);
}

TEST_F(R3TraderTest, cancel_localid_risk_rejection_retries_do_not_grow_cache)
{
    const int result = r3RunInIsolatedProcess(
        "R3TraderTest.cancel_localid_risk_rejection_retries_do_not_grow_cache", [this]() {
        R3MockerDirCleanup cleanup{ mockerIds_ };
        ct_->setAltCode(kStdCode);
        TraderAdapter trader;
        if (!enableRisk(trader, 9, 2))
            return 60;

        const uint32_t acceptedLocalId = trader.openLong(kStdCode, 7000.0, 1, 0);
        const uint32_t deniedLocalId = trader.openLong(kStdCode, 7000.0, 1, 0);
        if (acceptedLocalId == UINT_MAX || deniedLocalId == UINT_MAX ||
            !waitForOrder(trader, acceptedLocalId) || !waitForOrder(trader, deniedLocalId))
            return 61;

        // Establish one successfully submitted cancel and a live second order.
        // Then force the already-established local risk exclusion path so the
        // retry check does not depend on the unresolved window endpoint/clock.
        if (!trader.cancel(acceptedLocalId))
            return 62;
        auto cacheIt = trader._cancel_time_cache.find(kStdCode);
        if (cacheIt == trader._cancel_time_cache.end() || cacheIt->second.size() != 1u)
            return 63;
        const size_t acceptedCacheSize = cacheIt->second.size();

        trader._exclude_codes.insert(kStdCode);
        for (uint32_t retry = 0; retry < 100; ++retry) {
            if (trader.cancel(deniedLocalId))
                return 64;
        }

        cacheIt = trader._cancel_time_cache.find(kStdCode);
        const bool cacheUnchanged = cacheIt != trader._cancel_time_cache.end() &&
            cacheIt->second.size() == acceptedCacheSize;
        trader.release();
        return cacheUnchanged ? 0 : 65;
    });
    EXPECT_EQ(0, result);
}

TEST_F(R3TraderTest, cancel_localid_synchronous_api_failure_is_not_cached)
{
    TraderAdapter trader;
    buildReady(trader);

    WTSOrderInfo* order = r3Order(ct_, WDT_LONG, WOT_OPEN, 1, 0,
        "SYNC_FAIL", ("otp." + id_ + ".3501").c_str());
    WTSArray* orders = r3Array({ order });
    trader.onRspOrders(orders);
    orders->release();
    order->release();

    api_.action_ret_ = -1;
    EXPECT_FALSE(trader.cancel(3501));
    EXPECT_EQ(1u, api_.n_action_);
    EXPECT_EQ(trader._cancel_time_cache.end(), trader._cancel_time_cache.find(kStdCode));

    // A synchronous API acceptance remains recorded as dispatched.
    api_.action_ret_ = 0;
    EXPECT_TRUE(trader.cancel(3501));
    const auto cacheIt = trader._cancel_time_cache.find(kStdCode);
    ASSERT_NE(trader._cancel_time_cache.end(), cacheIt);
    EXPECT_EQ(1u, cacheIt->second.size());
    trader.release();
}

//////////////////////////////////////////////////////////////////////////
// saveData（由 onRspAccount 带动）
//////////////////////////////////////////////////////////////////////////
TEST_F(R3TraderTest, save_data_writes_long_short_positions_and_fund_block)
{
    TraderAdapter trader;
    ASSERT_TRUE(trader.initExt(id_.c_str(), &api_, &bd_, NULL));
    ASSERT_TRUE(trader.run());

    WTSArray* positions = r3OwnedArray({
        r3Pos(ct_, WDT_LONG, 3, 3, 2, 2),
        r3Pos(ct_, WDT_SHORT, 1, 1, 0, 0) });
    trader.onRspPosition(positions);
    positions->release();

    std::string rtfile = std::string(WtHelper::getBaseDir()) + "traders/" + id_ + "/rtdata.json";
    EXPECT_FALSE(std::filesystem::exists(rtfile)) << rtfile;

    // saveData 是 private，由 onRspAccount 的资金分发链带动
    WTSAccountInfo* acct = WTSAccountInfo::create();
    acct->setBalance(123456.0);
    acct->setPreBalance(100000.0);
    WTSArray* accounts = r3OwnedArray({ acct });
    trader.onRspAccount(accounts);
    accounts->release();

    std::string body = r3ReadAll(rtfile);
    rapidjson::Document doc;
    doc.Parse(body.c_str());
    ASSERT_FALSE(doc.HasParseError()) << body;
    ASSERT_TRUE(doc.IsObject());
    ASSERT_TRUE(doc.HasMember("positions") && doc["positions"].IsArray());
    ASSERT_EQ(1u, doc["positions"].Size());
    const auto& position = doc["positions"][0];
    ASSERT_TRUE(position.IsObject());
    ASSERT_TRUE(position.HasMember("code") && position["code"].IsString());
    EXPECT_STREQ(kStdCode, position["code"].GetString());
    ASSERT_TRUE(position.HasMember("long") && position["long"].IsObject());
    ASSERT_TRUE(position["long"].HasMember("newvol") && position["long"]["newvol"].IsNumber());
    ASSERT_TRUE(position["long"].HasMember("prevol") && position["long"]["prevol"].IsNumber());
    EXPECT_DOUBLE_EQ(2.0, position["long"]["newvol"].GetDouble());
    EXPECT_DOUBLE_EQ(3.0, position["long"]["prevol"].GetDouble());
    ASSERT_TRUE(position.HasMember("short") && position["short"].IsObject());
    ASSERT_TRUE(position["short"].HasMember("newvol") && position["short"]["newvol"].IsNumber());
    ASSERT_TRUE(position["short"].HasMember("prevol") && position["short"]["prevol"].IsNumber());
    EXPECT_DOUBLE_EQ(0.0, position["short"]["newvol"].GetDouble());
    EXPECT_DOUBLE_EQ(1.0, position["short"]["prevol"].GetDouble());
    ASSERT_TRUE(doc.HasMember("funds") && doc["funds"].IsObject());
    ASSERT_TRUE(doc["funds"].HasMember("CNY") && doc["funds"]["CNY"].IsObject());
    ASSERT_TRUE(doc["funds"]["CNY"].HasMember("balance") && doc["funds"]["CNY"]["balance"].IsNumber());
    ASSERT_TRUE(doc["funds"]["CNY"].HasMember("prebalance") && doc["funds"]["CNY"]["prebalance"].IsNumber());
    EXPECT_DOUBLE_EQ(123456.0, doc["funds"]["CNY"]["balance"].GetDouble());
    EXPECT_DOUBLE_EQ(100000.0, doc["funds"]["CNY"]["prebalance"].GetDouble());

    // 空资金数组走 size()==0 支路，funds 仍是空对象
    WTSArray* emptyAccounts = WTSArray::create();
    trader.onRspAccount(emptyAccounts);
    emptyAccounts->release();
    std::string body2 = r3ReadAll(rtfile);
    rapidjson::Document emptyFundsDoc;
    emptyFundsDoc.Parse(body2.c_str());
    ASSERT_FALSE(emptyFundsDoc.HasParseError()) << body2;
    ASSERT_TRUE(emptyFundsDoc.HasMember("funds") && emptyFundsDoc["funds"].IsObject());
    EXPECT_EQ(0u, emptyFundsDoc["funds"].MemberCount());
    ASSERT_TRUE(emptyFundsDoc.HasMember("positions") && emptyFundsDoc["positions"].IsArray());
    ASSERT_EQ(1u, emptyFundsDoc["positions"].Size());
    ASSERT_TRUE(emptyFundsDoc["positions"][0].IsObject());
    ASSERT_TRUE(emptyFundsDoc["positions"][0].HasMember("long") && emptyFundsDoc["positions"][0]["long"].IsObject());
    ASSERT_TRUE(emptyFundsDoc["positions"][0]["long"].HasMember("prevol")
        && emptyFundsDoc["positions"][0]["long"]["prevol"].IsNumber());
    EXPECT_DOUBLE_EQ(3.0, emptyFundsDoc["positions"][0]["long"]["prevol"].GetDouble());
    trader.release();
}

//////////////////////////////////////////////////////////////////////////
// 四个下单包装：方向 / 开平 / 价格类型 / flag / usertag
//////////////////////////////////////////////////////////////////////////
TEST_F(R3TraderTest, four_wrappers_stamp_direction_offset_price_and_flag)
{
    TraderAdapter trader;
    buildReady(trader);

    const uint32_t firstLocalId = trader.openLong(kStdCode, 7000.0, 1, 0);
    EXPECT_NE(UINT_MAX, firstLocalId);
    EXPECT_EQ(WDT_LONG, api_.last_dir_);
    EXPECT_EQ(WOT_OPEN, api_.last_offset_);
    EXPECT_EQ(WPT_LIMITPRICE, api_.last_pxtype_);
    EXPECT_EQ((int)WOF_NOR, (int)api_.last_flag_);
    EXPECT_STREQ(kRawCode, api_.last_code_.c_str());
    EXPECT_STREQ(kExchg, api_.last_exchg_.c_str());
    EXPECT_EQ("otp." + id_ + "." + std::to_string(firstLocalId), api_.last_usertag_);
    EXPECT_EQ("P11E001", api_.last_entrust_id_);

    EXPECT_NE(UINT_MAX, trader.openShort(kStdCode, 0.0, 2, 0));
    EXPECT_EQ(WDT_SHORT, api_.last_dir_);
    EXPECT_EQ(WOT_OPEN, api_.last_offset_);
    EXPECT_EQ(WPT_ANYPRICE, api_.last_pxtype_);   // 0 价 => 市价

    EXPECT_NE(UINT_MAX, trader.closeLong(kStdCode, 7100.0, 1, true, 1));
    EXPECT_EQ(WDT_LONG, api_.last_dir_);
    EXPECT_EQ(WOT_CLOSETODAY, api_.last_offset_);
    EXPECT_EQ((int)WOF_NOR + 1, (int)api_.last_flag_);

    EXPECT_NE(UINT_MAX, trader.closeShort(kStdCode, 7100.0, 1, false, 0));
    EXPECT_EQ(WDT_SHORT, api_.last_dir_);
    EXPECT_EQ(WOT_CLOSE, api_.last_offset_);
    EXPECT_EQ(4u, api_.n_insert_);

    // 提交失败 -> UINT_MAX
    api_.insert_ret_ = -1;
    EXPECT_EQ(UINT_MAX, trader.openLong(kStdCode, 7000.0, 1, 0));
    EXPECT_EQ(5u, api_.n_insert_); // FakeTraderApi counts the failed API request too.
    api_.insert_ret_ = 0;

    // 按方向批量撤单：只挑卖方活单
    WTSArray* seededOrders = r3OwnedArray({
        r3Order(ct_, WDT_LONG, WOT_OPEN, 2, 0, "ORD1", ("otp." + id_ + ".2001").c_str()),
        r3Order(ct_, WDT_SHORT, WOT_OPEN, 2, 0, "ORD2", ("otp." + id_ + ".2002").c_str()),
        r3DeadOrder(ct_, WDT_LONG, WOT_OPEN, 2, 2, "ORD3", ("otp." + id_ + ".2003").c_str()) });
    trader.onRspOrders(seededOrders);
    seededOrders->release();
    OrderIDs sold = trader.cancel(kStdCode, false);
    EXPECT_EQ(1u, sold.size());                  // 只有那笔卖开
    trader.release();
}

TEST_F(R3TraderTest, cancel_empty_code_cancels_all_buy_orders_without_crashing)
{
    TraderAdapter trader;
    buildReady(trader);

    WTSArray* seededOrders = r3OwnedArray({
        r3Order(ct_, WDT_LONG, WOT_OPEN, 2, 0, "ORD1", ("otp." + id_ + ".2001").c_str()),
        r3Order(ct2_, WDT_LONG, WOT_OPEN, 2, 0, "ORD2", ("otp." + id_ + ".2002").c_str()) });
    trader.onRspOrders(seededOrders);
    seededOrders->release();

    // extractStdCode safely returns empty CodeInfo for empty input; the wrapper
    // keeps its explicit all-contract behavior for the requested side.
    EXPECT_EXIT(
        {
            OrderIDs cancelled = trader.cancel("", true);
            bool foundAg = false;
            bool foundAu = false;
            for (uint32_t localid : cancelled)
            {
                foundAg = foundAg || localid == 2001u;
                foundAu = foundAu || localid == 2002u;
            }
            if (cancelled.size() != 2u || !foundAg || !foundAu || api_.n_action_ != 2u)
                std::exit(2);
            std::exit(0);
        },
        ::testing::ExitedWithCode(0), "");

    trader.release();
}

TEST_F(R3TraderTest, cancel_standard_code_only_cancels_matching_contract)
{
    TraderAdapter trader;
    buildReady(trader);

    WTSArray* seededOrders = r3OwnedArray({
        r3Order(ct_, WDT_LONG, WOT_OPEN, 2, 0, "ORD1", ("otp." + id_ + ".2001").c_str()),
        r3Order(ct2_, WDT_LONG, WOT_OPEN, 2, 0, "ORD2", ("otp." + id_ + ".2002").c_str()) });
    trader.onRspOrders(seededOrders);
    seededOrders->release();

    OrderIDs cancelled = trader.cancel(kStdCode, true);
    ASSERT_EQ(1u, cancelled.size());
    EXPECT_EQ(2001u, cancelled[0]);
    EXPECT_EQ(1u, api_.n_action_);
    trader.release();
}

TEST_F(R3TraderTest, cancel_empty_code_filters_sell_orders_across_contracts)
{
    TraderAdapter trader;
    buildReady(trader);

    WTSArray* seededOrders = r3OwnedArray({
        r3Order(ct_, WDT_LONG, WOT_OPEN, 2, 0, "ORD1", ("otp." + id_ + ".2001").c_str()),
        r3Order(ct_, WDT_SHORT, WOT_OPEN, 2, 0, "ORD2", ("otp." + id_ + ".2002").c_str()),
        r3Order(ct2_, WDT_SHORT, WOT_OPEN, 3, 0, "ORD3", ("otp." + id_ + ".2003").c_str()) });
    trader.onRspOrders(seededOrders);
    seededOrders->release();

    OrderIDs cancelled = trader.cancel("", false);
    bool foundAgSell = false;
    bool foundAuSell = false;
    bool foundBuy = false;
    for (uint32_t localid : cancelled)
    {
        foundAgSell = foundAgSell || localid == 2002u;
        foundAuSell = foundAuSell || localid == 2003u;
        foundBuy = foundBuy || localid == 2001u;
    }
    EXPECT_EQ(2u, cancelled.size());
    EXPECT_TRUE(foundAgSell);
    EXPECT_TRUE(foundAuSell);
    EXPECT_FALSE(foundBuy);
    EXPECT_EQ(2u, api_.n_action_);
    trader.release();
}

TEST_F(R3TraderTest, cancel_empty_code_stops_after_positive_quantity)
{
    TraderAdapter trader;
    buildReady(trader);

    WTSArray* seededOrders = r3OwnedArray({
        r3Order(ct_, WDT_LONG, WOT_OPEN, 2, 0, "ORD1", ("otp." + id_ + ".2001").c_str()),
        r3Order(ct2_, WDT_LONG, WOT_OPEN, 2, 0, "ORD2", ("otp." + id_ + ".2002").c_str()),
        r3Order(ct_, WDT_LONG, WOT_OPEN, 2, 0, "ORD3", ("otp." + id_ + ".2003").c_str()) });
    trader.onRspOrders(seededOrders);
    seededOrders->release();

    // Each accepted cancel removes two units. Any two orders meet qty=3,
    // regardless of map iteration order; the third order must not be submitted.
    OrderIDs cancelled = trader.cancel("", true, 3.0);
    double acceptedQty = 0.0;
    for (uint32_t localid : cancelled)
    {
        if (localid == 2001u || localid == 2002u || localid == 2003u)
            acceptedQty += 2.0;
        else
            ADD_FAILURE() << "unexpected local ID " << localid;
    }
    EXPECT_EQ(2u, cancelled.size());
    EXPECT_DOUBLE_EQ(4.0, acceptedQty);
    EXPECT_GE(acceptedQty, 3.0);
    EXPECT_EQ(2u, api_.n_action_);
    trader.release();
}

//////////////////////////////////////////////////////////////////////////
// buy()：规则限额 + 单笔最大委托量拆单
//////////////////////////////////////////////////////////////////////////
TEST_F(R3TraderTest, buy_splits_by_rule_cap_and_single_order_max_volume)
{
    loadPolicy(R"JSON({"default":{"order":[
        {"action":"open","limit":3,"limit_l":60,"limit_s":40,"pure":false}
    ]}})JSON");

    ct_->setVolumeLimits(2, 2);   // 单笔最多 2 手
    TraderAdapter trader;
    ASSERT_TRUE(trader.initExt(id_.c_str(), &api_, &bd_, &policy_));
    ASSERT_TRUE(trader.run());

    OrderIDs ids = trader.buy(kStdCode, 7000.0, 5, 0, false);
    // 规则总限额 3 手把 5 手压到 3 手，单笔上限 2 手 => 2 + 1 两笔
    EXPECT_EQ(2u, ids.size());
    EXPECT_EQ(2u, api_.n_insert_);
    EXPECT_DOUBLE_EQ(api_.last_vol_, 1.0);
    EXPECT_EQ(WPT_LIMITPRICE, api_.last_pxtype_);
    EXPECT_EQ(WOT_OPEN, api_.last_offset_);
    EXPECT_DOUBLE_EQ(trader.getUndoneQty(kStdCode), 3.0);

    // qty==0 直接返回空集
    EXPECT_EQ(0u, trader.buy(kStdCode, 7000.0, 0, 0, false).size());
    EXPECT_EQ(2u, api_.n_insert_);

    // 非交易时段（15:00）拒绝，一笔都不下发
    WtHelper::setTime(20250110, 1500, 0);
    EXPECT_EQ(0u, trader.buy(kStdCode, 7000.0, 2, 0, false).size());
    EXPECT_EQ(2u, api_.n_insert_);
    WtHelper::setTime(20250110, 931, 0);

    // 市价单按 maxMktVol 拆分：2 手 => 一笔 2 手
    api_.last_vol_ = 0;
    EXPECT_EQ(1u, trader.buy(kStdCode, 0.0, 2, 0, false).size());
    EXPECT_DOUBLE_EQ(api_.last_vol_, 2.0);
    EXPECT_EQ(WPT_ANYPRICE, api_.last_pxtype_);

    // sell() 走空头开仓支路
    EXPECT_EQ(1u, trader.sell(kStdCode, 0.0, 2, 0, false).size());
    EXPECT_EQ(WDT_SHORT, api_.last_dir_);
    EXPECT_EQ(WOT_OPEN, api_.last_offset_);
    EXPECT_DOUBLE_EQ(api_.last_vol_, 2.0);
    trader.release();
}

//////////////////////////////////////////////////////////////////////////
// 规则限额吃满 => 该动作被跳过，一单不发
//////////////////////////////////////////////////////////////////////////
TEST_F(R3TraderTest, buy_skips_open_rule_when_daily_cap_already_consumed)
{
    loadPolicy(R"JSON({"default":{"order":[
        {"action":"open","limit":2,"limit_l":1,"limit_s":1,"pure":false}
    ]}})JSON");

    TraderAdapter trader;
    ASSERT_TRUE(trader.initExt(id_.c_str(), &api_, &bd_, &policy_));
    ASSERT_TRUE(trader.run());

    // statItem.l_openvol 由开仓成交回报累加（L1780），先喂一笔成交把限额吃满
    WTSTradeInfo* t = WTSTradeInfo::create(kRawCode, kExchg);
    t->setContractInfo(ct_);
    t->setDirection(WDT_LONG);
    t->setOffsetType(WOT_OPEN);
    t->setVolume(1);
    t->setPrice(7000.0);
    t->setTradeID("T1");
    t->setRefOrder("ORD9");
    t->setUserTag(("otp." + id_ + ".9001").c_str());
    t->setTradeDate(20250110);
    t->setTradeTime(93010);
    t->setAmount(7000.0);
    WTSArray* trades = r3OwnedArray({ t });
    trader.onRspTrades(trades);
    trades->release();

    size_t before = api_.n_insert_;
    OrderIDs ids = trader.buy(kStdCode, 7000.0, 3, 0, false);
    EXPECT_EQ(0u, ids.size());                 // 多头开仓限额已满 => continue
    EXPECT_EQ(before, api_.n_insert_);         // 没有任何委托下发
    trader.release();
}

//////////////////////////////////////////////////////////////////////////
// getPosition / enumPosition / getOrders
//////////////////////////////////////////////////////////////////////////
TEST_F(R3TraderTest, position_and_order_tables_read_seeded_state)
{
    TraderAdapter trader;
    ASSERT_TRUE(trader.initExt(id_.c_str(), &api_, &bd_, NULL));
    ASSERT_TRUE(trader.run());

    EXPECT_DOUBLE_EQ(trader.getPosition(kStdCode, false), 0.0);
    EXPECT_DOUBLE_EQ(trader.getPosition(kStdCode, true), 0.0);
    EXPECT_DOUBLE_EQ(trader.getPosition(kStdAu, false), 0.0);

    WTSArray* positions = r3OwnedArray({
        r3Pos(ct_, WDT_LONG, 3, 1, 2, 2),
        r3Pos(ct_, WDT_SHORT, 1, 0, 0, 0) });
    trader.onRspPosition(positions);
    positions->release();

    EXPECT_DOUBLE_EQ(trader.getPosition(kStdCode, false), 4.0);   // long 3+2 less short 1+0
    EXPECT_DOUBLE_EQ(trader.getPosition(kStdCode, true), 3.0);    // 1+2+0+0

    size_t n = 0;
    double longVol = 0, shortVol = 0;
    trader.enumPosition([&n, &longVol, &shortVol](const char* stdCode, bool isLong,
        double prevol, double preavail, double newvol, double newavail) {
        (void)stdCode; (void)preavail; (void)newavail;
        ++n;
        if (isLong)
            longVol += prevol + newvol;
        else
            shortVol += prevol + newvol;
    });
    EXPECT_EQ(2u, n);
    EXPECT_DOUBLE_EQ(longVol, 5.0);
    EXPECT_DOUBLE_EQ(shortVol, 1.0);

    WTSArray* orders = r3OwnedArray({
        r3Order(ct_, WDT_LONG, WOT_OPEN, 2, 0, "ORD1", ("otp." + id_ + ".1001").c_str()),
        r3DeadOrder(ct_, WDT_LONG, WOT_CLOSE, 3, 3, "ORD2", ("otp." + id_ + ".1002").c_str()) });
    trader.onRspOrders(orders);
    orders->release();
    OrderMap* tab = trader.getOrders(kStdCode);
    ASSERT_NE(nullptr, tab);
    EXPECT_EQ(1u, tab->size());      // 文档要求按标准化代码筛选；保持对当前 raw/std 不匹配的可证伪断言。
    tab->release();
    OrderMap* rawTab = trader.getOrders(kRawCode);
    ASSERT_NE(nullptr, rawTab);
    EXPECT_EQ(1u, rawTab->size());   // 确认活单已入表，定位标准化代码筛选差异。
    rawTab->release();
    OrderMap* allTab = trader.getOrders("");
    EXPECT_NE(nullptr, allTab);
    if (allTab)
        allTab->release();
    trader.release();
}

TEST_F(R3TraderTest, get_orders_supports_standard_and_bare_codes_across_exchanges)
{
    TraderAdapter trader;
    buildReady(trader);

    WTSCommodityInfo* dceComm = WTSCommodityInfo::create("ag", "DCE silver", "DCE", "TRADING", "CHINA");
    ASSERT_NE(nullptr, dceComm);
    dceComm->setCategory(CC_Future);
    dceComm->setCoverMode(CM_CoverToday);
    dceComm->setTradingMode(TM_Both);
    WTSContractInfo* dceContract = WTSContractInfo::create(kRawCode, "ag2512", "DCE", "ag");
    ASSERT_NE(nullptr, dceContract);
    dceContract->setAltCode(kRawCode);
    bd_.regis(dceComm, dceContract);

    WTSOrderInfo* shfeOrder = r3Order(ct_, WDT_LONG, WOT_OPEN, 1, 0,
        "SHFE_AG", ("otp." + id_ + ".4101").c_str());
    WTSOrderInfo* dceOrder = r3Order(dceContract, WDT_LONG, WOT_OPEN, 1, 0,
        "DCE_AG", ("otp." + id_ + ".4102").c_str());
    WTSArray* orders = r3OwnedArray({ shfeOrder, dceOrder });
    trader.onRspOrders(orders);
    orders->release();

    // Canonical standard codes retain the exchange dimension.
    OrderMap* byShfe = trader.getOrders(kStdCode);
    ASSERT_NE(nullptr, byShfe);
    ASSERT_EQ(1u, byShfe->size());
    EXPECT_EQ(shfeOrder, byShfe->get(4101));
    EXPECT_EQ(nullptr, byShfe->get(4102));
    byShfe->release();

    OrderMap* byDce = trader.getOrders("DCE.ag.2512");
    ASSERT_NE(nullptr, byDce);
    ASSERT_EQ(1u, byDce->size());
    EXPECT_EQ(nullptr, byDce->get(4101));
    EXPECT_EQ(dceOrder, byDce->get(4102));
    byDce->release();

    // Bare raw-code lookup preserves the existing exact-code behavior: with
    // no exchange qualifier, both matching markets remain visible.
    OrderMap* byRaw = trader.getOrders(kRawCode);
    ASSERT_NE(nullptr, byRaw);
    EXPECT_EQ(2u, byRaw->size());
    EXPECT_EQ(shfeOrder, byRaw->get(4101));
    EXPECT_EQ(dceOrder, byRaw->get(4102));
    byRaw->release();

    OrderMap* all = trader.getOrders("");
    ASSERT_NE(nullptr, all);
    EXPECT_EQ(2u, all->size());
    all->release();

    trader.release();
}

}  // namespace
