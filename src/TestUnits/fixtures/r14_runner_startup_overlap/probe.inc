
// Compiled only into the isolated test copy of the existing r95 fixture.
namespace r14_startup_overlap {
std::mutex gateMutex;
std::condition_variable cv;
int arrivals = 0;
bool blocked = false;
bool release = false;
std::atomic<WtUftRunner*> callbackTarget{nullptr};
std::atomic<int> callbackResult{-1};
std::atomic<bool> throwHook{false};
}

extern "C" void r14_startup_overlap_hook()
{
    using namespace r14_startup_overlap;
    if (WtUftRunner* target = callbackTarget.exchange(nullptr))
        callbackResult.store(target->shutdown() ? 1 : 0);
    if (throwHook.exchange(false))
        throw std::runtime_error("r14 injected startup failure");
    std::unique_lock<std::mutex> lock(gateMutex);
    ++arrivals;
    cv.notify_all();
    if (blocked) cv.wait(lock, [] { return release; });
}

TEST(r14_startup_overlap, startup_thread_shutdown_is_pending_and_control_retry_succeeds)
{
    using namespace r14_startup_overlap;
    ScopedTempCwd cwd("r14_startup_callback");
    ASSERT_TRUE(cwd.ok());
    ASSERT_TRUE(prepare_common());
    ASSERT_TRUE(write_text("cfg.yaml", kConfigRun));
    WtUftRunner runner;
    ASSERT_TRUE(runner.config("cfg.yaml"));
    { std::lock_guard<std::mutex> lock(gateMutex); arrivals=0; blocked=false; release=true; }
    callbackResult.store(-1);
    callbackTarget.store(&runner);
    runner.run(true);
    EXPECT_EQ(callbackResult.load(),0) << "startup-thread shutdown did not remain pending";
    EXPECT_TRUE(runner.shutdown()) << "control-thread shutdown retry failed";
    EXPECT_TRUE(runner._shutdown_complete.load(std::memory_order_acquire));
    std::signal(SIGINT,SIG_DFL);
}

TEST(r14_startup_overlap, sequential_run_is_rejected_before_engine_startup)
{
    using namespace r14_startup_overlap;
    ScopedTempCwd cwd("r14_startup_sequential");
    ASSERT_TRUE(cwd.ok());
    ASSERT_TRUE(prepare_common());
    ASSERT_TRUE(write_text("cfg.yaml", kConfigRun));
    WtUftRunner runner;
    ASSERT_TRUE(runner.config("cfg.yaml"));
    { std::lock_guard<std::mutex> lock(gateMutex); arrivals=0; blocked=false; release=true; }
    runner.run(true);
    runner.run(true);
    EXPECT_EQ(arrivals,1);
    EXPECT_TRUE(runner.shutdown());
    std::signal(SIGINT,SIG_DFL);
}

TEST(r14_startup_overlap, failed_configuration_closes_instance_and_shutdown_retry_is_safe)
{
    WtUftRunner runner;
    EXPECT_FALSE(runner.config("missing-r14-startup-config.yaml"));
    runner.run(true);
    EXPECT_FALSE(runner.config("missing-r14-startup-config.yaml"));
    EXPECT_TRUE(runner.shutdown());
    EXPECT_TRUE(runner.shutdown());
    std::signal(SIGINT,SIG_DFL);
}

TEST(r14_startup_overlap, startup_exception_releases_lease_for_shutdown_retry)
{
    using namespace r14_startup_overlap;
    ScopedTempCwd cwd("r14_startup_exception");
    ASSERT_TRUE(cwd.ok());
    ASSERT_TRUE(prepare_common());
    ASSERT_TRUE(write_text("cfg.yaml", kConfigRun));
    WtUftRunner runner;
    ASSERT_TRUE(runner.config("cfg.yaml"));
    { std::lock_guard<std::mutex> lock(gateMutex); arrivals=0; blocked=false; release=true; }
    throwHook.store(true);
    runner.run(true);
    runner.run(true);
    EXPECT_EQ(arrivals,0) << "failed startup was retried without an explicit restart contract";
    EXPECT_TRUE(runner.shutdown());
    std::signal(SIGINT,SIG_DFL);
}

TEST(r14_startup_overlap, pending_shutdown_is_completed_after_startup_exception)
{
    using namespace r14_startup_overlap;
    ScopedTempCwd cwd("r14_startup_exception_pending");
    ASSERT_TRUE(cwd.ok());
    ASSERT_TRUE(prepare_common());
    ASSERT_TRUE(write_text("cfg.yaml", kConfigRun));
    WtUftRunner runner;
    ASSERT_TRUE(runner.config("cfg.yaml"));
    { std::lock_guard<std::mutex> lock(gateMutex); arrivals=0; blocked=false; release=true; }
    callbackResult.store(-1);
    callbackTarget.store(&runner);
    throwHook.store(true);
    runner.run(true);
    EXPECT_EQ(callbackResult.load(),0);
    EXPECT_TRUE(runner._shutdown_complete.load(std::memory_order_acquire));
    EXPECT_TRUE(runner.shutdown());
    std::signal(SIGINT,SIG_DFL);
}

TEST(r14_startup_overlap, shutdown_during_engine_startup_is_pending)
{
    using namespace r14_startup_overlap;
    ScopedTempCwd cwd("r14_startup_shutdown");
    ASSERT_TRUE(cwd.ok());
    ASSERT_TRUE(prepare_common());
    ASSERT_TRUE(write_text("cfg.yaml", kConfigRun));
    WtUftRunner runner;
    ASSERT_TRUE(runner.config("cfg.yaml"));
    { std::lock_guard<std::mutex> lock(gateMutex); arrivals=0; release=false; blocked=true; }
    std::thread starter([&] { runner.run(true); });
    bool reached;
    { std::unique_lock<std::mutex> lock(gateMutex); reached=cv.wait_for(lock,std::chrono::seconds(5),[]{return arrivals>0;}); }
    bool stopped = reached && runner.shutdown();
    { std::lock_guard<std::mutex> lock(gateMutex); release=true; }
    cv.notify_all();
    starter.join();
    EXPECT_TRUE(reached);
    EXPECT_FALSE(stopped) << "shutdown completed while run was still inside startup";
    EXPECT_TRUE(runner.shutdown());
    std::signal(SIGINT,SIG_DFL);
}

TEST(r14_startup_overlap, concurrent_run_is_rejected_before_engine_startup)
{
    using namespace r14_startup_overlap;
    ScopedTempCwd cwd("r14_startup_repeat");
    ASSERT_TRUE(cwd.ok());
    ASSERT_TRUE(prepare_common());
    ASSERT_TRUE(write_text("cfg.yaml", kConfigRun));
    WtUftRunner runner;
    ASSERT_TRUE(runner.config("cfg.yaml"));
    { std::lock_guard<std::mutex> lock(gateMutex); arrivals=0; release=false; blocked=true; }
    std::thread first([&] { runner.run(true); });
    bool reached;
    { std::unique_lock<std::mutex> lock(gateMutex); reached=cv.wait_for(lock,std::chrono::seconds(5),[]{return arrivals>0;}); }
    std::thread second([&] { runner.run(true); });
    int observed;
    { std::unique_lock<std::mutex> lock(gateMutex); cv.wait_for(lock,std::chrono::milliseconds(500),[]{return arrivals>1;}); observed=arrivals; release=true; }
    cv.notify_all();
    first.join(); second.join();
    EXPECT_TRUE(reached);
    EXPECT_EQ(observed,1) << "two run invocations entered the startup phase";
    EXPECT_TRUE(runner.shutdown());
    std::signal(SIGINT,SIG_DFL);
}
