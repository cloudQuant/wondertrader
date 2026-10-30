/**
 * r160_hft_ticker_test.cpp — lane R160 / wave24
 *
 * Covers the run() thread's automatic minute close and the _time unit contract:
 * _time is HHMMSSmmm, while WTSSessionInfo::isInTradingTime takes HHMM.
 *
 * _thrd/_time/_cur_pos are private; use the existing r105 test technique only
 * around WtHftTicker.h, without changing the production interface.
 */
#include "r160_common.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include <Includes/WTSMarcos.h>
#include <Share/StdUtils.hpp>

#define private public
#include <WtCore/WtHftTicker.h>
#undef private

#include "r73_fixture.h"

namespace {

class R160HftEngine : public r73::HftEngine
{
public:
    void on_init() override
    {
        init_calls.fetch_add(1, std::memory_order_relaxed);
        if (on_init_hook)
            on_init_hook();
    }
    void on_session_begin() override { session_begins.fetch_add(1, std::memory_order_relaxed); }

    void on_session_end() override
    {
        if (on_session_end_hook)
            on_session_end_hook();
        sessions.fetch_add(1, std::memory_order_release);
    }

    std::atomic<uint32_t> init_calls{0};
    std::atomic<uint32_t> session_begins{0};
    std::atomic<uint32_t> sessions{0};
    std::function<void()> on_init_hook;
    std::function<void()> on_session_end_hook;
};

class R160HftTickerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        wtp::WTSSessionInfo* sess = _bd.regisSession("TRADING", 930, 1500);
        ASSERT_NE(sess, nullptr);
        wtp::WTSCommodityInfo* ag = r73::makeComm("SHFE", "ag", "TRADING", wtp::TM_Both);
        ag->setSessionInfo(sess);
        _bd.regis(ag, r73::makeCt("SHFE", "ag", "ag2512"));

        _engine.attach(&_bd, &_hot, NULL, 20250110, 931);
    }

    r73::BaseDataMgr _bd;
    r73::HotMgr _hot;
    R160HftEngine _engine;
};

} // namespace

// A 14:59 auto-close must leave _time in HHMMSSmmm. The in-session pass counter
// provides a deterministic observation that the worker evaluated the next
// loop's session predicate. The engine's initial time is set to the close so a
// bad HHMM value would enter the forced-close branch on that next pass.
TEST_F(R160HftTickerTest, AutoCloseKeepsDaySessionTimeEncodingForNextPass)
{
    wtp::WtHftRtTicker ticker(&_engine);
    ticker.init(NULL, "TRADING");
    ASSERT_NE(ticker._s_info, nullptr);

    _engine.attach(&_bd, &_hot, NULL, 20250110, 1500);
    _engine.on_session_end_hook = [&ticker]() { ticker.stop(); };

    ticker._date = 20250110;
    ticker._time = 145800000; // 14:58:00.000
    ticker._cur_pos = 329;    // Next auto-close is 14:59 in this session.
    ticker._last_emit_pos = 0;
    ticker._next_check_time = 0;

    ticker.run();

    for (int i = 0; i < 400 && ticker._in_session_pass_count.load(std::memory_order_acquire) < 2 &&
            _engine.sessions.load(std::memory_order_acquire) == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    ticker.stop();

    EXPECT_GE(ticker._in_session_pass_count.load(std::memory_order_acquire), 2u);
    EXPECT_EQ(ticker._last_emit_pos.load(), 329u);
    EXPECT_EQ(ticker._time, 145900000u);
    EXPECT_TRUE(ticker._s_info->isInTradingTime(ticker._time / 100000, true));
    EXPECT_EQ(_engine.sessions.load(std::memory_order_acquire), 0u);
}

TEST_F(R160HftTickerTest, ConcurrentRunCallsStartOnlyOneWorker)
{
    wtp::WtHftRtTicker ticker(&_engine);
    ticker.init(NULL, "TRADING");
    ASSERT_NE(ticker._s_info, nullptr);

    ticker._date = 20250110;
    ticker._time = 145800000;
    ticker._cur_pos = 0;
    ticker._last_emit_pos = 0;
    ticker._next_check_time = 0;

    std::atomic<uint32_t> ready{0};
    std::atomic<bool> start{false};
    auto run_concurrently = [&]() {
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        ticker.run();
    };

    std::thread first_runner(run_concurrently);
    std::thread second_runner(run_concurrently);
    for (int i = 0; i < 400 && ready.load(std::memory_order_acquire) < 2; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const bool run_callers_ready = ready.load(std::memory_order_acquire) == 2;
    start.store(true, std::memory_order_release);
    first_runner.join();
    second_runner.join();

    for (int i = 0; i < 400 && ticker._in_session_pass_count.load(std::memory_order_acquire) < 2; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ticker.stop();

    EXPECT_GE(ticker._in_session_pass_count.load(std::memory_order_acquire), 2u);
    EXPECT_TRUE(run_callers_ready);
    EXPECT_EQ(_engine.init_calls.load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(_engine.session_begins.load(std::memory_order_relaxed), 1u);
    EXPECT_TRUE(ticker._lifecycle_state == wtp::WtHftRtTicker::LifecycleState::Joined);
}

TEST_F(R160HftTickerTest, StartupStopSkipsSessionBeginAndWorkerCreation)
{
    wtp::WtHftRtTicker ticker(&_engine);
    ticker.init(NULL, "TRADING");
    ASSERT_NE(ticker._s_info, nullptr);
    _engine.on_init_hook = [&ticker]() { ticker.stop(); };

    ticker.run();
    ticker.stop();

    EXPECT_EQ(_engine.init_calls.load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(_engine.session_begins.load(std::memory_order_relaxed), 0u);
    {
        std::lock_guard<std::mutex> lock(ticker._lifecycle_mtx);
        EXPECT_TRUE(ticker._lifecycle_state == wtp::WtHftRtTicker::LifecycleState::Joined);
        EXPECT_EQ(ticker._thrd, nullptr);
    }
}

TEST_F(R160HftTickerTest, WorkerStopBeforeTargetRunSkipsTargetCallbacks)
{
    std::unique_ptr<wtp::WTSSessionInfo, void (*)(wtp::WTSSessionInfo*)> night(
        wtp::WTSSessionInfo::create("MIDNIGHT", "midnight", 60),
        [](wtp::WTSSessionInfo* session) {
            if (session != nullptr)
                session->release();
        });
    ASSERT_NE(night.get(), nullptr);
    night->addTradingSection(2300, 0);

    wtp::WtHftRtTicker worker(&_engine);
    worker.init(NULL, "TRADING");
    worker._s_info = night.get();

    wtp::WtHftRtTicker target(&_engine);
    target.init(NULL, "TRADING");

    _engine.attach(&_bd, &_hot, NULL, 20250110, 2359);
    _engine.on_session_end_hook = [&]() {
        worker.stop();
        target.stop(); // Cross-ticker worker stop remains nonblocking.
    };

    worker._date = 20250110;
    worker._time = 235900000;
    worker._cur_pos = 60;
    worker._last_emit_pos = 0;
    worker._next_check_time = 0;

    worker.run();
    for (int i = 0; i < 400 && _engine.sessions.load(std::memory_order_acquire) == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    worker.stop();

    // The worker's stop request leaves the target Idle until run() observes
    // _stopped before invoking initialization or session callbacks.
    target.run();
    target.stop();

    EXPECT_EQ(_engine.sessions.load(std::memory_order_acquire), 1u);
    EXPECT_EQ(_engine.init_calls.load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(_engine.session_begins.load(std::memory_order_relaxed), 1u);
    {
        std::lock_guard<std::mutex> lock(target._lifecycle_mtx);
        EXPECT_TRUE(target._lifecycle_state == wtp::WtHftRtTicker::LifecycleState::Joined);
        EXPECT_EQ(target._thrd, nullptr);
    }
}

// The existing automatic-close rule advances the local date when the closed
// minute maps to 00:00. Use a one-hour session offset to keep this midnight
// boundary in a valid session time range and enter the session-end callback.
// The callback coordinates a worker self-stop request with concurrent owner
// stops. This checks local-date rollover, not exchange trading-date semantics.
TEST_F(R160HftTickerTest, MidnightRolloverAndConcurrentWorkerStopAreSafe)
{
    std::unique_ptr<wtp::WTSSessionInfo, void (*)(wtp::WTSSessionInfo*)> night(
        wtp::WTSSessionInfo::create("MIDNIGHT", "midnight", 60),
        [](wtp::WTSSessionInfo* session) {
            if (session != nullptr)
                session->release();
        });
    ASSERT_NE(night.get(), nullptr);
    night->addTradingSection(2300, 0);

    wtp::WtHftRtTicker ticker(&_engine);
    ticker.init(NULL, "TRADING");
    ticker._s_info = night.get();

    _engine.attach(&_bd, &_hot, NULL, 20250110, 2359);
    std::atomic<bool> self_stop_returned{false};
    std::atomic<bool> allow_session_callback_return{false};
    _engine.on_session_end_hook = [&]() {
        ticker.stop(); // Worker-thread stop must only request shutdown.
        self_stop_returned.store(true, std::memory_order_release);
        while (!allow_session_callback_return.load(std::memory_order_acquire))
            std::this_thread::yield();
    };

    ticker._date = 20250110;
    ticker._time = 235900000; // 23:59:00.000
    ticker._cur_pos = 60;     // The next close maps back to 00:00.
    ticker._last_emit_pos = 0;
    ticker._next_check_time = 0;

    ticker.run();

    for (int i = 0; i < 400 && !self_stop_returned.load(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    std::atomic<uint32_t> stop_callers_ready{0};
    std::atomic<bool> start_stop_callers{false};
    std::atomic<uint32_t> stop_callers_done{0};
    auto concurrent_stop = [&]() {
        stop_callers_ready.fetch_add(1, std::memory_order_release);
        while (!start_stop_callers.load(std::memory_order_acquire))
            std::this_thread::yield();
        ticker.stop();
        stop_callers_done.fetch_add(1, std::memory_order_release);
    };
    std::thread first_stopper(concurrent_stop);
    std::thread second_stopper(concurrent_stop);
    for (int i = 0; i < 400 && stop_callers_ready.load(std::memory_order_acquire) < 2; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const bool stop_callers_all_ready = stop_callers_ready.load(std::memory_order_acquire) == 2;
    start_stop_callers.store(true, std::memory_order_release);

    bool observed_join_waiter = false;
    for (int i = 0; i < 400 && !observed_join_waiter; ++i)
    {
        {
            std::lock_guard<std::mutex> lock(ticker._lifecycle_mtx);
            observed_join_waiter = ticker._lifecycle_state == wtp::WtHftRtTicker::LifecycleState::Joining &&
                ticker._join_waiter_count > 0;
        }
        if (!observed_join_waiter)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    allow_session_callback_return.store(true, std::memory_order_release);
    first_stopper.join();
    second_stopper.join();
    ticker.stop();

    EXPECT_TRUE(self_stop_returned.load(std::memory_order_acquire));
    EXPECT_TRUE(stop_callers_all_ready);
    EXPECT_TRUE(observed_join_waiter);
    EXPECT_EQ(stop_callers_done.load(std::memory_order_acquire), 2u);
    EXPECT_EQ(_engine.sessions.load(std::memory_order_acquire), 1u);
    EXPECT_EQ(ticker._date, 20250111u);
    EXPECT_EQ(ticker._time, 0u);
    EXPECT_EQ(ticker._last_emit_pos.load(), 60u);
    EXPECT_EQ(_engine.get_min_time(), 0u);
}
