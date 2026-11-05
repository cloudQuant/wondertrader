// 迭代03 卡3 3a=A（BUG-R202-001）追加用例：拼接在 q01_wtcore3/r202_wtcore_public_surface_test.cpp
// 与 r202_nextday/nextday.inc 之后编译（复用 R202Market/R202Reader/R202NextDayEngine）。
// 23:59:60 是非法输入：ticker 拒绝，不交给引擎，不推进引擎时间。

template<class Engine, class Ticker>
void expectLeapSecondRejected()
{
    q01sel::Q01TempEnv tmp("c3_leap");
    R202Market mkt(0, 0);
    R202Reader reader;
    TraderAdapterMgr adapters;
    Engine engine;
    WTSVariant* cfg = mkt.cfg();
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &mkt.bd, &mkt.dm, &mkt.hot, NULL);
    cfg->release();
    if constexpr (std::is_base_of_v<WtCtaEngine, Engine>)
        engine.set_adapter_mgr(&adapters);
    const uint32_t day = TimeUtils::getNextDate(TimeUtils::getCurDate(), 1);
    engine.set_date_time(day, 0, 0);
    engine.set_trading_date(day);
    Ticker ticker(&engine);
    ticker.init(&reader, "TRADING");
    ticker.run();
    auto send = [&](uint32_t time) {
        WTSTickData* tick = q01sel::makeTick("SHFE", "ag2512", day, day, time, 3500.0);
        tick->setContractInfo(mkt.bd.getContract("ag2512", "SHFE"));
        ticker.on_tick(tick);
        tick->release();
    };
    send(235958000);
    const unsigned ticksBefore = engine.ticks.load();
    const uint32_t minBefore = engine.get_min_time();
    send(235960000);	// 23:59:60.000
    const unsigned ticksAfter = engine.ticks.load();
    const uint32_t minAfter = engine.get_min_time();
    const uint32_t secsAfter = engine.get_secs();
    ticker.stop();
    EXPECT_EQ(ticksBefore, ticksAfter) << "23:59:60 must be rejected, not delivered";
    EXPECT_EQ(minBefore, minAfter);
    EXPECT_LT(secsAfter, 60000u);
}

TEST(C3TickerLeapSecond, Cta) { expectLeapSecondRejected<R202NextDayEngine<R202CtaEngine>, WtCtaRtTicker>(); }
TEST(C3TickerLeapSecond, Sel) { expectLeapSecondRejected<R202NextDayEngine<R202SelEngine>, WtSelRtTicker>(); }
TEST(C3TickerLeapSecond, Hft) { expectLeapSecondRejected<R202NextDayEngine<R202HftEngine>, WtHftRtTicker>(); }

// ---------------------------------------------------------------------------------------------
// 卡9 9d=A / R202-001 ticker 合同（2026-11-04 用户裁定）：on_tick 与 worker 串行；回调内同步重入
// 不就地执行，排队并在当前事件返回后的安全点按序恰好投递一次；stop 排空后 join，worker 内 stop 不自 join。
// ---------------------------------------------------------------------------------------------
#include <mutex>
#include <vector>

template<class Base> class C3RecEngine : public Base
{
public:
    void on_tick(const char*, WTSTickData* t) override
    {
        std::function<void(double)> hook;
        {
            std::lock_guard<std::mutex> lk(mtx);
            prices.push_back(t->price());
            hook = onPrice;
        }
        ticks.fetch_add(1);
        if (hook)
            hook(t->price());
    }
    std::vector<double> snapshot()
    {
        std::lock_guard<std::mutex> lk(mtx);
        return prices;
    }
    std::mutex mtx;
    std::vector<double> prices;
    std::function<void(double)> onPrice;
    std::atomic<unsigned> ticks{0};
};

template<class Engine>
void c3InitEngine(Engine& engine, R202Market& mkt, TraderAdapterMgr& adapters, uint32_t day)
{
    WTSVariant* cfg = mkt.cfg();
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &mkt.bd, &mkt.dm, &mkt.hot, NULL);
    cfg->release();
    if constexpr (std::is_base_of_v<WtCtaEngine, Engine>)
        engine.set_adapter_mgr(&adapters);
    engine.set_date_time(day, 0, 0);
    engine.set_trading_date(day);
}

inline WTSTickData* c3Tick(R202Market& mkt, uint32_t date, uint32_t time, double px)
{
    WTSTickData* t = q01sel::makeTick("SHFE", "ag2512", date, date, time, px);
    t->setContractInfo(mkt.bd.getContract("ag2512", "SHFE"));
    return t;
}

template<class Engine>
bool c3WaitPrices(Engine& engine, size_t n, int ms)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (engine.snapshot().size() < n && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    return engine.snapshot().size() >= n;
}

// worker 线程（2400 收盘回调内）同步重入 3 笔新日行情：回调返回时一笔都未执行；
// 之后按 FIFO 恰好各投递一次，引擎时间不被写回 2400，新日 00:01 分钟正常闭合。
template<class Engine, class Ticker>
void expectWorkerReentryQueuedInOrder()
{
    q01sel::Q01TempEnv tmp("c3_reentry_order");
    R202Market mkt(0, 0);
    R202ReentrantReader reader;
    TraderAdapterMgr adapters;
    Engine engine;
    const uint32_t day = TimeUtils::getNextDate(TimeUtils::getCurDate(), 1);
    const uint32_t next = TimeUtils::getNextDate(day, 1);
    c3InitEngine(engine, mkt, adapters, day);
    Ticker ticker(&engine);
    ticker.init(&reader, "TRADING");
    WTSTickData* q1 = c3Tick(mkt, next, 0, 3501.0);
    WTSTickData* q2 = c3Tick(mkt, next, 30000, 3502.0);
    WTSTickData* q3 = c3Tick(mkt, next, 100000, 3503.0);
    std::atomic<int> seenAtReturn{-1};
    reader.callback = [&] {
        ticker.on_tick(q1);
        ticker.on_tick(q2);
        ticker.on_tick(q3);
        seenAtReturn.store((int)engine.snapshot().size());
    };
    ticker.run();
    WTSTickData* last = c3Tick(mkt, day, 235959999, 3500.0);
    ticker.on_tick(last);
    last->release();
    const bool all = c3WaitPrices(engine, 4, 3000);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const std::vector<double> prices = engine.snapshot();
    const uint32_t actualDate = engine.get_date();
    const uint32_t actualTime = engine.get_min_time();
    const uint32_t actualTDate = engine.getTradingDate();
    const uint32_t closes = reader.count.load();
    const uint32_t lastClose = reader.last_time.load();
    ticker.stop();
    q1->release(); q2->release(); q3->release();
    EXPECT_TRUE(all);
    EXPECT_EQ(1, seenAtReturn.load()) << "re-entrant ticks must be queued, not executed inside the callback";
    EXPECT_EQ((std::vector<double>{3500.0, 3501.0, 3502.0, 3503.0}), prices) << "queued ticks delivered in order exactly once";
    EXPECT_EQ(next, actualDate);
    EXPECT_EQ(next, actualTDate);
    EXPECT_NE(2400u, actualTime) << "worker must not write 2400 back after re-entry";
    EXPECT_EQ(2u, actualTime);
    EXPECT_EQ(2u, closes);
    EXPECT_EQ(1u, lastClose);
}

TEST(C3TickerReentry, WorkerQueuedInOrderCta) { expectWorkerReentryQueuedInOrder<C3RecEngine<R202CtaEngine>, WtCtaRtTicker>(); }
TEST(C3TickerReentry, WorkerQueuedInOrderSel) { expectWorkerReentryQueuedInOrder<C3RecEngine<R202SelEngine>, WtSelRtTicker>(); }
TEST(C3TickerReentry, WorkerQueuedInOrderHft) { expectWorkerReentryQueuedInOrder<C3RecEngine<R202HftEngine>, WtHftRtTicker>(); }

// 行情线程（策略 on_tick 回调内）嵌套重入：内层 on_tick 立即返回，外层事件结束后依次投递。
template<class Engine, class Ticker>
void expectStrategyReentryQueuedInOrder()
{
    q01sel::Q01TempEnv tmp("c3_reentry_strategy");
    R202Market mkt(0, 0);
    R202Reader reader;
    TraderAdapterMgr adapters;
    Engine engine;
    const uint32_t day = TimeUtils::getNextDate(TimeUtils::getCurDate(), 1);
    c3InitEngine(engine, mkt, adapters, day);
    Ticker ticker(&engine);
    ticker.init(&reader, "TRADING");
    ticker.run();
    WTSTickData* t1 = c3Tick(mkt, day, 100000000, 3500.0);
    WTSTickData* t2 = c3Tick(mkt, day, 100001000, 3600.0);
    WTSTickData* t3 = c3Tick(mkt, day, 100002000, 3700.0);
    std::vector<int> seenAtInnerReturn;
    engine.onPrice = [&](double px) {
        if (px == 3500.0) { ticker.on_tick(t2); seenAtInnerReturn.push_back((int)engine.snapshot().size()); }
        else if (px == 3600.0) { ticker.on_tick(t3); seenAtInnerReturn.push_back((int)engine.snapshot().size()); }
    };
    ticker.on_tick(t1);	// 10:00:00.000（首 tick）
    const std::vector<double> prices = engine.snapshot();
    engine.onPrice = nullptr;
    ticker.stop();
    t1->release(); t2->release(); t3->release();
    EXPECT_EQ((std::vector<double>{3500.0, 3600.0, 3700.0}), prices);
    EXPECT_EQ((std::vector<int>{1, 2}), seenAtInnerReturn) << "inner on_tick must return before the queued tick is delivered";
}

TEST(C3TickerReentry, StrategyQueuedInOrderCta) { expectStrategyReentryQueuedInOrder<C3RecEngine<R202CtaEngine>, WtCtaRtTicker>(); }
TEST(C3TickerReentry, StrategyQueuedInOrderSel) { expectStrategyReentryQueuedInOrder<C3RecEngine<R202SelEngine>, WtSelRtTicker>(); }
TEST(C3TickerReentry, StrategyQueuedInOrderHft) { expectStrategyReentryQueuedInOrder<C3RecEngine<R202HftEngine>, WtHftRtTicker>(); }

// stop 时有排队事件：worker 仍在 session_end 回调（100ms）内，另一线程 stop 在有界时间内返回，
// 返回前排队事件已恰好投递一次。
template<class Engine, class Ticker>
void expectStopWhileQueuedDrainsAndJoins()
{
    q01sel::Q01TempEnv tmp("c3_stop_queued");
    R202Market mkt(0, 0);
    R202ReentrantReader reader;
    TraderAdapterMgr adapters;
    Engine engine;
    const uint32_t day = TimeUtils::getNextDate(TimeUtils::getCurDate(), 1);
    const uint32_t next = TimeUtils::getNextDate(day, 1);
    c3InitEngine(engine, mkt, adapters, day);
    Ticker ticker(&engine);
    ticker.init(&reader, "TRADING");
    WTSTickData* q1 = c3Tick(mkt, next, 0, 3501.0);
    std::atomic<bool> queued{false};
    reader.callback = [&] { ticker.on_tick(q1); queued.store(true); };
    ticker.run();
    WTSTickData* last = c3Tick(mkt, day, 235959999, 3500.0);
    ticker.on_tick(last);
    last->release();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!queued.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_TRUE(queued.load());
    const auto t0 = std::chrono::steady_clock::now();
    ticker.stop();
    const auto stopMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    const std::vector<double> prices = engine.snapshot();
    q1->release();
    EXPECT_LT(stopMs, 2000) << "stop must drain and join within a bounded time";
    EXPECT_EQ((std::vector<double>{3500.0, 3501.0}), prices) << "queued tick delivered exactly once before stop returns";
}

TEST(C3TickerStop, StopWhileQueuedDrainsAndJoinsCta) { expectStopWhileQueuedDrainsAndJoins<C3RecEngine<R202CtaEngine>, WtCtaRtTicker>(); }
TEST(C3TickerStop, StopWhileQueuedDrainsAndJoinsSel) { expectStopWhileQueuedDrainsAndJoins<C3RecEngine<R202SelEngine>, WtSelRtTicker>(); }
TEST(C3TickerStop, StopWhileQueuedDrainsAndJoinsHft) { expectStopWhileQueuedDrainsAndJoins<C3RecEngine<R202HftEngine>, WtHftRtTicker>(); }

// worker 回调内 stop：只发停止请求、立即返回（不自 join）；随后外部 stop 有界 join。
template<class Engine, class Ticker>
void expectWorkerStopDoesNotSelfJoin()
{
    q01sel::Q01TempEnv tmp("c3_stop_worker");
    R202Market mkt(0, 0);
    R202ReentrantReader reader;
    TraderAdapterMgr adapters;
    Engine engine;
    const uint32_t day = TimeUtils::getNextDate(TimeUtils::getCurDate(), 1);
    c3InitEngine(engine, mkt, adapters, day);
    Ticker ticker(&engine);
    ticker.init(&reader, "TRADING");
    std::atomic<bool> innerReturned{false};
    reader.callback = [&] { ticker.stop(); innerReturned.store(true); };
    ticker.run();
    WTSTickData* last = c3Tick(mkt, day, 235959999, 3500.0);
    ticker.on_tick(last);
    last->release();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!innerReturned.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto t0 = std::chrono::steady_clock::now();
    ticker.stop();
    const auto stopMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_TRUE(innerReturned.load()) << "stop from the worker callback must return without self-join";
    EXPECT_LT(stopMs, 2000);
}

TEST(C3TickerStop, WorkerStopDoesNotSelfJoinCta) { expectWorkerStopDoesNotSelfJoin<C3RecEngine<R202CtaEngine>, WtCtaRtTicker>(); }
TEST(C3TickerStop, WorkerStopDoesNotSelfJoinSel) { expectWorkerStopDoesNotSelfJoin<C3RecEngine<R202SelEngine>, WtSelRtTicker>(); }
TEST(C3TickerStop, WorkerStopDoesNotSelfJoinHft) { expectWorkerStopDoesNotSelfJoin<C3RecEngine<R202HftEngine>, WtHftRtTicker>(); }
