#include <type_traits>
#include <functional>

template<class Base> class R202NextDayEngine : public Base
{
public:
    void on_tick(const char*, WTSTickData*) override { ++ticks; }
    std::atomic<unsigned> ticks{0};
};

template<class Engine, class Ticker>
void expectNextDayAfterFullDayClose()
{
    q01sel::Q01TempEnv tmp("r202_nextday");
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
    const uint32_t next = TimeUtils::getNextDate(day, 1);
    engine.set_date_time(day, 0, 0);
    engine.set_trading_date(day);
    Ticker ticker(&engine);
    ticker.init(&reader, "TRADING");
    ticker.run();
    auto send = [&](uint32_t date, uint32_t tdate, uint32_t time) {
        WTSTickData* tick = q01sel::makeTick("SHFE", "ag2512", date, tdate, time, 3500.0);
        tick->setContractInfo(mkt.bd.getContract("ag2512", "SHFE"));
        ticker.on_tick(tick);
        tick->release();
    };
    send(day, day, 235959999);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((reader.count.load() < 1 || engine.session_ends.load() < 1) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    // Existing fake's on_session_end sleeps 100 ms, so let it return before
    // sending the next-day events; the negative control is not callback reentry.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(1u, reader.count.load());
    EXPECT_EQ(2400u, reader.last_time.load());
    send(next, next, 0);
    send(next, next, 100000);
    const uint32_t seenDate = engine.get_date();
    const uint32_t seenTradingDate = engine.getTradingDate();
    const unsigned ticks = engine.ticks.load();
    const unsigned count = reader.count.load();
    const uint32_t closedDate = reader.last_date.load();
    const uint32_t closedTime = reader.last_time.load();
    ticker.stop();
    EXPECT_EQ(3u, ticks);
    EXPECT_EQ(next, seenDate);
    EXPECT_EQ(next, seenTradingDate);
    EXPECT_EQ(2u, count);
    EXPECT_EQ(next, closedDate);
    EXPECT_EQ(1u, closedTime);
}

TEST(R202NextDay, Cta) { expectNextDayAfterFullDayClose<R202NextDayEngine<R202CtaEngine>, WtCtaRtTicker>(); }
TEST(R202NextDay, Sel) { expectNextDayAfterFullDayClose<R202NextDayEngine<R202SelEngine>, WtSelRtTicker>(); }
TEST(R202NextDay, Hft) { expectNextDayAfterFullDayClose<R202NextDayEngine<R202HftEngine>, WtHftRtTicker>(); }

class R202ReentrantReader : public R202Reader
{
public:
    std::function<void()> callback;
    void onMinuteEnd(uint32_t d, uint32_t t, uint32_t e = 0) override
    {
        R202Reader::onMinuteEnd(d, t, e);
        if (t == 2400 && callback)
            callback();
    }
};

template<class Engine, class Ticker>
void expectMidnightReentryKeepsNewDayTime()
{
    q01sel::Q01TempEnv tmp("r202_reentry");
    R202Market mkt(0, 0);
    R202ReentrantReader reader;
    TraderAdapterMgr adapters;
    Engine engine;
    WTSVariant* cfg = mkt.cfg();
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &mkt.bd, &mkt.dm, &mkt.hot, NULL);
    cfg->release();
    if constexpr (std::is_base_of_v<WtCtaEngine, Engine>)
        engine.set_adapter_mgr(&adapters);
    const uint32_t day = TimeUtils::getNextDate(TimeUtils::getCurDate(), 1);
    const uint32_t next = TimeUtils::getNextDate(day, 1);
    engine.set_date_time(day, 0, 0);
    engine.set_trading_date(day);
    Ticker ticker(&engine);
    ticker.init(&reader, "TRADING");
    WTSTickData* nextTick = q01sel::makeTick("SHFE", "ag2512", next, next, 0, 3501.0);
    nextTick->setContractInfo(mkt.bd.getContract("ag2512", "SHFE"));
    std::atomic<bool> callbackReturned{false};
    reader.callback = [&] { ticker.on_tick(nextTick); callbackReturned.store(true); };
    ticker.run();
    WTSTickData* last = q01sel::makeTick("SHFE", "ag2512", day, day, 235959999, 3500.0);
    last->setContractInfo(mkt.bd.getContract("ag2512", "SHFE"));
    ticker.on_tick(last);
    last->release();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((!callbackReturned.load() || !engine.session_ends.load()) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    // The fake session-end callback sleeps 100 ms after incrementing the count.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto actualDate = engine.get_date();
    const auto actualTime = engine.get_min_time();
    const auto actualTradingDate = engine.getTradingDate();
    ticker.stop();
    nextTick->release();
    EXPECT_TRUE(callbackReturned.load());
    EXPECT_EQ(next, actualDate);
    EXPECT_EQ(1u, actualTime);
    EXPECT_EQ(next, actualTradingDate);
}

TEST(R202Reentry, Cta) { expectMidnightReentryKeepsNewDayTime<R202NextDayEngine<R202CtaEngine>, WtCtaRtTicker>(); }
TEST(R202Reentry, Sel) { expectMidnightReentryKeepsNewDayTime<R202NextDayEngine<R202SelEngine>, WtSelRtTicker>(); }
TEST(R202Reentry, Hft) { expectMidnightReentryKeepsNewDayTime<R202NextDayEngine<R202HftEngine>, WtHftRtTicker>(); }
