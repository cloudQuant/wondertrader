/*
 * 迭代03 s5 traders-ctp：T3-012 TraderDumper 关闭排空与对象保活（卡9：9a/9b/9c/9d）。
 *   - 重复 release 幂等、重复 run 拒绝（不替换仍可 join 的线程）；
 *   - worker 线程内 release 延后到安全点（不自 join、不自锁）；
 *   - 用户回调内 release 关闭准入，之后的回调不再到达用户；
 *   - run(true) 在 release 后及时返回；release 结果以 lifecycle_state() 与日志可观察。
 * 只用空 manager / 未连接的 TraderAdapter，不加载交易模块、不连接柜台。
 * WT_I03_S5_TRADERS_BASELINE：改前源码红控编译时定义（新增观察接口以哨兵代替）。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

#include "TraderDumper/Dumper.h"
#include "TraderDumper/TraderAdapter.h"

extern wtp::TraderAdapterMgr g_adapterMgr;

using DumperWorkerWaitHook = void (*)();
void setDumperWorkerWaitHookForTest(DumperWorkerWaitHook hook);
void setDumperUseLegacySleepForTest(bool useLegacySleep);

namespace i03s5dumper {

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

static bool waitFor(bool (*pred)(), int maxMs = 3000)
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

static Dumper* g_target = nullptr;
static std::atomic<int> g_hook_calls{0};
static std::atomic<int> g_account_calls{0};
static std::atomic<bool> g_release_returned_on_worker{false};

static void releaseFromWorkerHook()
{
    if (g_hook_calls.fetch_add(1) == 0 && g_target != nullptr)
    {
        g_target->release();
        g_release_returned_on_worker = true;
    }
}

static void releasingAccountCb(const char*, uint32_t, const char*, double, double, double, double, double,
    double, double, double, double, double, bool)
{
    if (g_account_calls.fetch_add(1) == 0 && g_target != nullptr)
        g_target->release();
}

#ifdef WT_I03_S5_TRADERS_BASELINE
static int stateOf(Dumper&) { return -1; }
#else
static int stateOf(Dumper& d) { return (int)d.lifecycle_state(); }
#endif
static const int kDeferred = 2;
static const int kReleased = 3;

class I03S5DumperLifecycle : public ::testing::Test
{
protected:
    std::string dir_;
    char oldCwd_[1024];

    void SetUp() override
    {
        ASSERT_NE(getcwd(oldCwd_, sizeof(oldCwd_)), nullptr);
        char tmpl[] = "/tmp/i03s5_dumper_XXXXXX";
        char* made = mkdtemp(tmpl);
        ASSERT_NE(made, nullptr);
        dir_ = made;
        ASSERT_EQ(chdir(dir_.c_str()), 0);
        g_adapterMgr.release();
        g_target = nullptr;
        g_hook_calls = 0;
        g_account_calls = 0;
        g_release_returned_on_worker = false;
    }
    void TearDown() override
    {
        setDumperWorkerWaitHookForTest(nullptr);
        setDumperUseLegacySleepForTest(false);
        g_target = nullptr;
        g_adapterMgr.release();
        chdir(oldCwd_);
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    std::string emptyConfig()
    {
        const std::string path = dir_ + "/cfg.json";
        std::ofstream(path) << R"({"config":{"refresh_span":1},"basefiles":{},"traders":[]})";
        return path;
    }
};

}// namespace i03s5dumper

using namespace i03s5dumper;

// 重复 run(false) 被拒绝（旧实现替换可 join 线程 → std::terminate）；重复 release 幂等（旧实现二次 join 抛异常）。
TEST_F(I03S5DumperLifecycle, RepeatedRunIsRejectedAndRepeatedReleaseIsIdempotent)
{
    Dumper d;
    ASSERT_TRUE(d.config(emptyConfig().c_str(), true, dir_.c_str()));
    {
        Watchdog dog("repeated run/release", 8000);
        d.run(false);
        d.run(false);
        EXPECT_NO_THROW(d.release());
        EXPECT_NO_THROW(d.release());
    }
    EXPECT_EQ(kReleased, stateOf(d));
}

// 卡9d：worker 线程内（刷新等待前钩子）调用 release 不自 join/自锁，延后到下一次外部调用完成。
TEST_F(I03S5DumperLifecycle, ReleaseOnWorkerThreadIsDeferredThenCompleted)
{
    Dumper d;
    g_target = &d;
    ASSERT_TRUE(d.config(emptyConfig().c_str(), true, dir_.c_str()));
    setDumperWorkerWaitHookForTest(&releaseFromWorkerHook);
    {
        Watchdog dog("release on worker thread", 8000);
        d.run(false);
        ASSERT_TRUE(waitFor([]() { return g_release_returned_on_worker.load(); }, 5000));
        EXPECT_EQ(kDeferred, stateOf(d));
        d.release();
    }
    EXPECT_EQ(kReleased, stateOf(d));
    EXPECT_EQ(1, g_hook_calls.load());
}

// 卡9d/9a：用户回调内 release 只关闭准入；之后的回调不再到达用户；外部 release 完成后状态为已释放。
TEST_F(I03S5DumperLifecycle, ReleaseInsideUserCallbackCutsOffLaterCallbacks)
{
    Dumper d;
    g_target = &d;
    d.register_callbacks(&releasingAccountCb, nullptr, nullptr, nullptr);
    {
        Watchdog dog("release inside user callback", 8000);
        std::thread provider([&d]() {
            d.on_account("ch", 20260105, "CNY", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, false);
            d.on_account("ch", 20260105, "CNY", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, true);
        });
        provider.join();
        EXPECT_EQ(kDeferred, stateOf(d));
        d.release();
    }
    EXPECT_EQ(1, g_account_calls.load());
    EXPECT_EQ(kReleased, stateOf(d));
    d.on_account("ch", 20260105, "CNY", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, true);
    EXPECT_EQ(1, g_account_calls.load());
}

// run(true) 在仍有存活通道时阻塞；release 请求停止后及时返回（旧实现忽略 release，按 1 秒轮询 isAnyAlive 永不返回）。
TEST_F(I03S5DumperLifecycle, RunOnceReturnsPromptlyAfterRelease)
{
    Dumper d;
    ASSERT_TRUE(d.config(emptyConfig().c_str(), true, dir_.c_str()));
    wtp::TraderAdapterPtr blocker(new wtp::TraderAdapter(&g_adapterMgr));   // never connected
    ASSERT_TRUE(g_adapterMgr.addAdapter("i03s5_blocker", blocker));
    std::atomic<bool> returned{false};
    {
        Watchdog dog("run(true) after release", 8000);
        std::thread runner([&]() {
            d.run(true);
            returned = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        EXPECT_FALSE(returned.load());
        EXPECT_TRUE(g_adapterMgr.isAnyAlive());
        const auto start = std::chrono::steady_clock::now();
        d.release();
        runner.join();
        EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count(), 2000);
    }
    EXPECT_TRUE(returned.load());
    EXPECT_EQ(kReleased, stateOf(d));
}

#include <csignal>
#include <type_traits>
#include <sys/wait.h>

// N-3 / 卡9a：全局 g_adapterMgr 不得有进程退出期析构（供应商线程可能仍回调 decAlive/refresh）。
TEST(I03S5DumperExitRetention, AdapterManagerHasNoExitTimeDestructor)
{
    EXPECT_TRUE(std::is_trivially_destructible<wtp::TraderAdapterMgr>::value)
        << "g_adapterMgr would be destroyed during static cleanup while provider threads may still call back";
}

// 子进程中模拟进程退出期对 g_adapterMgr 的析构，随后仍以适配器回调（decAlive）与查表访问管理器：
// 改前适配器表与互斥已被销毁（崩溃或查不到）；改后状态常驻、访问有效。
TEST(I03S5DumperExitRetention, ProviderCallbackAfterExitTimeDestructionStillSeesManagerState)
{
    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0)
    {
        g_adapterMgr.release();
        wtp::TraderAdapterPtr keep(new wtp::TraderAdapter(&g_adapterMgr));   // never connected
        if (!g_adapterMgr.addAdapter("i03s5_exit_keep", keep))
            _exit(2);
        g_adapterMgr.run();
        g_adapterMgr.~TraderAdapterMgr();          // what static cleanup at exit would run
        keep->handleEvent(wtp::WTE_Connect, -1);        // provider callback during cleanup -> decAlive()
        const bool found = g_adapterMgr.getAdapter("i03s5_exit_keep") != nullptr;
        _exit(found && !g_adapterMgr.isAnyAlive() ? 0 : 42);
    }
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    pid_t waited = 0;
    while ((waited = waitpid(child, &status, WNOHANG)) == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (waited == 0)
    {
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        FAIL() << "child did not finish";
    }
    ASSERT_TRUE(WIFEXITED(status)) << "child terminated by signal " << (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    EXPECT_EQ(0, WEXITSTATUS(status));
}
