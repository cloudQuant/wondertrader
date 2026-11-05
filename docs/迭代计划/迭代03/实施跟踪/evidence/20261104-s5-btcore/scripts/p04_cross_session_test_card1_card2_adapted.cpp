// Keep the existing real replayer fixture and its test cases unchanged.
#include "/Users/yunjinqi/Documents/new_projects/wondertrader/src/TestUnits/coverage/p07_btcore_replayer2/p07_tick_loader_test.cpp"
#include <Includes/ILogHandler.h>
#include <WTSTools/WTSLogger.h>

TEST_F(P07TickLoaderTest, pureTickMixedSessionsNoDataDayAndFinalMillisecond)
{
    class Capture final : public ILogHandler {
    public:
        void handleLogAppend(WTSLogLevel level, const char* text) override {
            if (level == LL_ERROR && text) std::cerr << "REPLAYER ERROR: " << text << '\n';
        }
    } capture;
    WTSLogger::init(R"({"root":{"level":"info","async":false,"sinks":[]}})", false, &capture);
    InstDirRestore restoreInstDir;
    ASSERT_TRUE(set_data_storage_inst_dir(_root));
    const auto sessions = _base / "cross_sessions.json";
    const auto commodities = _base / "cross_commodities.json";
    {
        std::ofstream out(sessions);
        ASSERT_TRUE(out.is_open());
        out << R"({"TRADING":{"name":"default","offset":0,"sections":[{"from":0,"to":0}]},"P07_24H":{"name":"continuous","offset":0,"sections":[{"from":0,"to":0}]},"P07_NEG":{"name":"offset-negative","offset":-300,"sections":[{"from":0,"to":200}]}})";
    }
    {
        std::ofstream out(commodities);
        ASSERT_TRUE(out.is_open());
        out << R"({"CRY":{"COIN":{"category":1,"covermode":0,"exchg":"CRY","holiday":"CHINA","name":"continuous","precision":2,"pricemode":1,"pricetick":0.01,"session":"P07_24H","volscale":1}},"ZZZ":{"NEG":{"category":1,"covermode":0,"exchg":"ZZZ","holiday":"CHINA","name":"negative offset","precision":2,"pricemode":1,"pricetick":0.01,"session":"P07_NEG","volscale":1}}})";
    }
    const std::string codeA = "CRY.COIN.BTC", codeB = "ZZZ.NEG.N1";
    auto mk = [](const char* exchg, const char* code, uint32_t day, uint32_t tradeDay,
                 uint32_t hhmm, uint32_t sec, uint32_t ms, double price) {
        WTSTickStruct t{};
        std::strcpy(t.exchg, exchg); std::strcpy(t.code, code);
        t.action_date = day; t.trading_date = tradeDay; t.action_time = p07_action_time(hhmm, sec, ms);
        t.open = t.high = t.low = t.price = price; t.volume = t.total_volume = 1;
        return t;
    };
    ASSERT_TRUE(write_raw_block(_base / "his/ticks/CRY/20200108/BTC.dsb", BT_HIS_Ticks,
        std::vector<WTSTickStruct>{mk("CRY", "BTC", 20200108u, 20200108u, 5, 0, 0, 1),
                                   mk("CRY", "BTC", 20200108u, 20200108u, 2359, 59, 999, 3)}));
    ASSERT_TRUE(write_raw_block(_base / "his/ticks/ZZZ/20200107/N1.dsb", BT_HIS_Ticks,
        std::vector<WTSTickStruct>{mk("ZZZ", "N1", 20200108u, 20200107u, 131, 10, 100, 2)}));
    // No natural-date events on Jan 9; the negative-offset contract's Jan 10
    // tick deliberately resides in the Jan 9 trading-date file.
    ASSERT_TRUE(write_raw_block(_base / "his/ticks/ZZZ/20200109/N1.dsb", BT_HIS_Ticks,
        std::vector<WTSTickStruct>{mk("ZZZ", "N1", 20200110u, 20200109u, 131, 10, 100, 4)}));
    ASSERT_TRUE(write_raw_block(_base / "his/ticks/CRY/20200110/BTC.dsb", BT_HIS_Ticks,
        std::vector<WTSTickStruct>{mk("CRY", "BTC", 20200110u, 20200110u, 2359, 59, 999, 5)}));
    HisDataReplayer replayer;
    WTSVariant* cfg = bin_tick_cfg(_root, _base, 20200108000000ull, 20200110235959ull,
                                   "stk_comms.json", sessions, commodities);
    ASSERT_TRUE(replayer.init(cfg)); cfg->release();
    TickSink sink;
    sink.onInit = [&]() { replayer.sub_tick(1u, codeA.c_str()); replayer.sub_tick(2u, codeB.c_str()); };
    std::vector<uint64_t> clocks;
    std::vector<std::size_t> visible;
    sink.onTickProbe = [&](uint32_t) {
        const uint64_t clock = uint64_t(replayer.get_date()) * 1000000000ull +
            uint64_t(replayer.get_min_time()) * 100000ull + replayer.get_secs();
        clocks.push_back(clock);
        const std::string& code = sink.tickCodes.back();
        WTSTickSlice* slice = replayer.get_tick_slice(code.c_str(), 100u);
        visible.push_back(slice ? slice->size() : 0);
        if (slice) {
            for (uint32_t i = 0; i < slice->size(); ++i) {
                const WTSTickStruct* tick = slice->at(i);
                ASSERT_NE(nullptr, tick);
                EXPECT_LT(uint64_t(tick->action_date) * 1000000000ull + tick->action_time, clock);
            }
            slice->release();
        }
    };
    replayer.register_sink(&sink, "p04_mixed_sessions_empty_day_tail");
    ASSERT_TRUE(replayer.prepare()); replayer.run(false);
    EXPECT_FALSE(HisDataReplayerTestAccess::replay_terminated(replayer));
    EXPECT_EQ(std::vector<double>({1,2,3,4,5}), sink.tickPrices);
    EXPECT_EQ(std::vector<std::string>({codeA,codeB,codeA,codeB,codeA}), sink.tickCodes);
    EXPECT_EQ(std::vector<uint32_t>({20200108u,20200108u,20200108u,20200110u,20200110u}), sink.tickDates);
    EXPECT_EQ(std::vector<uint32_t>({p07_action_time(5),p07_action_time(131,10,100),p07_action_time(2359,59,999),p07_action_time(131,10,100),p07_action_time(2359,59,999)}), sink.tickActionTimes);
    EXPECT_EQ(5u, clocks.size()); EXPECT_TRUE(std::is_sorted(clocks.begin(), clocks.end()));
    for (std::size_t i = 0; i < clocks.size() && i < sink.tickDates.size(); ++i)
        EXPECT_EQ(uint64_t(sink.tickDates[i]) * 1000000000ull + sink.tickActionTimes[i], clocks[i]);
    // A same-day older record must actually be returned by the default cutoff.
    ASSERT_EQ(5u, visible.size()); EXPECT_GE(visible[2], 1u);
    EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TickLoaderTest, pureTickContinuousAndFutureNoDataDayAndFinalMillisecond)
{
    class Capture final : public ILogHandler {
    public:
        void handleLogAppend(WTSLogLevel level, const char* text) override {
            if (level == LL_ERROR && text) std::cerr << "REPLAYER ERROR: " << text << '\n';
        }
    } capture;
    WTSLogger::init(R"({"root":{"level":"info","async":false,"sinks":[]}})", false, &capture);
    InstDirRestore restoreInstDir;
    ASSERT_TRUE(set_data_storage_inst_dir(_root));
    const auto sessions = _base / "cross_sessions.json";
    const auto commodities = _base / "cross_commodities.json";
    {
        std::ofstream out(sessions);
        ASSERT_TRUE(out.is_open());
        out << R"({"TRADING":{"name":"default","offset":0,"sections":[{"from":0,"to":0}]},"P07_24H":{"name":"continuous","offset":0,"sections":[{"from":0,"to":0}]},"P07_NEG":{"name":"night market","offset":300,"sections":[{"from":2100,"to":2300},{"from":900,"to":1015},{"from":1030,"to":1130},{"from":1330,"to":1500}]}})";
    }
    {
        std::ofstream out(commodities);
        ASSERT_TRUE(out.is_open());
        out << R"({"CRY":{"COIN":{"category":1,"covermode":0,"exchg":"CRY","holiday":"CHINA","name":"continuous","precision":2,"pricemode":1,"pricetick":0.01,"session":"P07_24H","volscale":1}},"SHFE":{"rb":{"category":1,"covermode":1,"exchg":"SHFE","holiday":"CHINA","name":"negative offset","precision":2,"pricemode":1,"pricetick":0.01,"session":"P07_NEG","volscale":1}}})";
    }
    const std::string codeA = "CRY.COIN.BTC", codeB = "SHFE.rb.2001";
    auto mk = [](const char* exchg, const char* code, uint32_t day, uint32_t tradeDay,
                 uint32_t hhmm, uint32_t sec, uint32_t ms, double price) {
        WTSTickStruct t{};
        std::strcpy(t.exchg, exchg); std::strcpy(t.code, code);
        t.action_date = day; t.trading_date = tradeDay; t.action_time = p07_action_time(hhmm, sec, ms);
        t.open = t.high = t.low = t.price = price; t.volume = t.total_volume = 1;
        return t;
    };
    ASSERT_TRUE(write_raw_block(_base / "his/ticks/CRY/20200108/BTC.dsb", BT_HIS_Ticks,
        std::vector<WTSTickStruct>{mk("CRY", "BTC", 20200108u, 20200108u, 5, 0, 0, 1),
                                   mk("CRY", "BTC", 20200108u, 20200108u, 2359, 59, 999, 3)}));
    ASSERT_TRUE(write_raw_block(_base / "his/ticks/SHFE/20200109/rb2001.dsb", BT_HIS_Ticks,
        std::vector<WTSTickStruct>{mk("SHFE", "rb2001", 20200108u, 20200109u, 2200, 10, 100, 2)}));
    // No natural-date events on Jan 9; Friday night belongs to Monday's trading-date file.
    ASSERT_TRUE(write_raw_block(_base / "his/ticks/SHFE/20200113/rb2001.dsb", BT_HIS_Ticks,
        std::vector<WTSTickStruct>{mk("SHFE", "rb2001", 20200110u, 20200113u, 2200, 10, 100, 4)}));
    ASSERT_TRUE(write_raw_block(_base / "his/ticks/CRY/20200110/BTC.dsb", BT_HIS_Ticks,
        std::vector<WTSTickStruct>{mk("CRY", "BTC", 20200110u, 20200110u, 2359, 59, 999, 5)}));
    HisDataReplayer replayer;
    WTSVariant* cfg = bin_tick_cfg(_root, _base, 20200108000000ull, 20200110235959ull,
                                   "stk_comms.json", sessions, commodities);
    ASSERT_TRUE(replayer.init(cfg)); cfg->release();
    TickSink sink;
    sink.onInit = [&]() { replayer.sub_tick(1u, codeA.c_str()); replayer.sub_tick(2u, codeB.c_str()); };
    std::vector<uint64_t> clocks;
    std::vector<uint32_t> tradingDates;
    std::vector<std::size_t> visible;
    sink.onTickProbe = [&](uint32_t) {
        const uint64_t clock = uint64_t(replayer.get_date()) * 1000000000ull +
            uint64_t(replayer.get_min_time()) * 100000ull + replayer.get_secs();
        clocks.push_back(clock);
        tradingDates.push_back(replayer.get_trading_date());
        const std::string& code = sink.tickCodes.back();
        WTSTickSlice* slice = replayer.get_tick_slice(code.c_str(), 100u);
        visible.push_back(slice ? slice->size() : 0);
        if (slice) {
            for (uint32_t i = 0; i < slice->size(); ++i) {
                const WTSTickStruct* tick = slice->at(i);
                ASSERT_NE(nullptr, tick);
                EXPECT_LT(uint64_t(tick->action_date) * 1000000000ull + tick->action_time, clock);
            }
            slice->release();
        }
    };
    replayer.register_sink(&sink, "p04_mixed_sessions_empty_day_tail");
    ASSERT_TRUE(replayer.prepare()); replayer.run(false);
    EXPECT_FALSE(HisDataReplayerTestAccess::replay_terminated(replayer));
    EXPECT_EQ(std::vector<double>({1,2,3,4,5}), sink.tickPrices);
    EXPECT_EQ(std::vector<std::string>({codeA,codeB,codeA,codeB,codeA}), sink.tickCodes);
    EXPECT_EQ(std::vector<uint32_t>({20200108u,20200108u,20200108u,20200110u,20200110u}), sink.tickDates);
    EXPECT_EQ(std::vector<uint32_t>({p07_action_time(5),p07_action_time(2200,10,100),p07_action_time(2359,59,999),p07_action_time(2200,10,100),p07_action_time(2359,59,999)}), sink.tickActionTimes);
    EXPECT_EQ(5u, clocks.size()); EXPECT_TRUE(std::is_sorted(clocks.begin(), clocks.end()));
    for (std::size_t i = 0; i < clocks.size() && i < sink.tickDates.size(); ++i)
        EXPECT_EQ(uint64_t(sink.tickDates[i]) * 1000000000ull + sink.tickActionTimes[i], clocks[i]);
    ASSERT_EQ(5u, tradingDates.size()); EXPECT_EQ(20200109u, tradingDates[2]);
    // A same-day older record must actually be returned by the default cutoff.
    ASSERT_EQ(5u, visible.size()); EXPECT_GE(visible[2], 1u);
    EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TickLoaderTest, pureTickContinuousAndFutureDispatchOnlyWithNoDataDay)
{
    class Capture final : public ILogHandler {
    public:
        void handleLogAppend(WTSLogLevel level, const char* text) override {
            if (level == LL_ERROR && text) std::cerr << "REPLAYER ERROR: " << text << '\n';
        }
    } capture;
    WTSLogger::init(R"({"root":{"level":"info","async":false,"sinks":[]}})", false, &capture);
    InstDirRestore restoreInstDir;
    ASSERT_TRUE(set_data_storage_inst_dir(_root));
    const auto sessions = _base / "cross_sessions.json";
    const auto commodities = _base / "cross_commodities.json";
    {
        std::ofstream out(sessions);
        ASSERT_TRUE(out.is_open());
        out << R"({"TRADING":{"name":"default","offset":0,"sections":[{"from":0,"to":0}]},"P07_24H":{"name":"continuous","offset":0,"sections":[{"from":0,"to":0}]},"P07_NEG":{"name":"night market","offset":300,"sections":[{"from":2100,"to":2300},{"from":900,"to":1015},{"from":1030,"to":1130},{"from":1330,"to":1500}]}})";
    }
    {
        std::ofstream out(commodities);
        ASSERT_TRUE(out.is_open());
        out << R"({"CRY":{"COIN":{"category":1,"covermode":0,"exchg":"CRY","holiday":"CHINA","name":"continuous","precision":2,"pricemode":1,"pricetick":0.01,"session":"P07_24H","volscale":1}},"SHFE":{"rb":{"category":1,"covermode":1,"exchg":"SHFE","holiday":"CHINA","name":"negative offset","precision":2,"pricemode":1,"pricetick":0.01,"session":"P07_NEG","volscale":1}}})";
    }
    const std::string codeA = "CRY.COIN.BTC", codeB = "SHFE.rb.2001";
    auto mk = [](const char* exchg, const char* code, uint32_t day, uint32_t tradeDay,
                 uint32_t hhmm, uint32_t sec, uint32_t ms, double price) {
        WTSTickStruct t{};
        std::strcpy(t.exchg, exchg); std::strcpy(t.code, code);
        t.action_date = day; t.trading_date = tradeDay; t.action_time = p07_action_time(hhmm, sec, ms);
        t.open = t.high = t.low = t.price = price; t.volume = t.total_volume = 1;
        return t;
    };
    ASSERT_TRUE(write_raw_block(_base / "his/ticks/CRY/20200108/BTC.dsb", BT_HIS_Ticks,
        std::vector<WTSTickStruct>{mk("CRY", "BTC", 20200108u, 20200108u, 5, 0, 0, 1),
                                   mk("CRY", "BTC", 20200108u, 20200108u, 2359, 59, 999, 3)}));
    ASSERT_TRUE(write_raw_block(_base / "his/ticks/SHFE/20200109/rb2001.dsb", BT_HIS_Ticks,
        std::vector<WTSTickStruct>{mk("SHFE", "rb2001", 20200108u, 20200109u, 2200, 10, 100, 2)}));
    // No natural-date events on Jan 9; Friday night belongs to Monday's trading-date file.
    ASSERT_TRUE(write_raw_block(_base / "his/ticks/SHFE/20200113/rb2001.dsb", BT_HIS_Ticks,
        std::vector<WTSTickStruct>{mk("SHFE", "rb2001", 20200110u, 20200113u, 2200, 10, 100, 4)}));
    ASSERT_TRUE(write_raw_block(_base / "his/ticks/CRY/20200110/BTC.dsb", BT_HIS_Ticks,
        std::vector<WTSTickStruct>{mk("CRY", "BTC", 20200110u, 20200110u, 2359, 59, 999, 5)}));
    HisDataReplayer replayer;
    WTSVariant* cfg = bin_tick_cfg(_root, _base, 20200108000000ull, 20200110235959ull,
                                   "stk_comms.json", sessions, commodities);
    ASSERT_TRUE(replayer.init(cfg)); cfg->release();
    TickSink sink;
    sink.onInit = [&]() { replayer.sub_tick(1u, codeA.c_str()); replayer.sub_tick(2u, codeB.c_str()); };
    std::vector<uint64_t> clocks;
    std::vector<std::size_t> visible;
    sink.onTickProbe = [&](uint32_t) {
        const uint64_t clock = uint64_t(replayer.get_date()) * 1000000000ull +
            uint64_t(replayer.get_min_time()) * 100000ull + replayer.get_secs();
        clocks.push_back(clock);
        const std::string& code = sink.tickCodes.back();
        WTSTickSlice* slice = replayer.get_tick_slice(code.c_str(), 100u);
        visible.push_back(slice ? slice->size() : 0);
        if (slice) {
            for (uint32_t i = 0; i < slice->size(); ++i) {
                const WTSTickStruct* tick = slice->at(i);
                ASSERT_NE(nullptr, tick);
                EXPECT_LT(uint64_t(tick->action_date) * 1000000000ull + tick->action_time, clock);
            }
            slice->release();
        }
    };
    replayer.register_sink(&sink, "p04_mixed_sessions_empty_day_tail");
    ASSERT_TRUE(replayer.prepare()); replayer.run(false);
    EXPECT_FALSE(HisDataReplayerTestAccess::replay_terminated(replayer));
    EXPECT_EQ(std::vector<double>({1,2,3,4,5}), sink.tickPrices);
    EXPECT_EQ(std::vector<std::string>({codeA,codeB,codeA,codeB,codeA}), sink.tickCodes);
    EXPECT_EQ(std::vector<uint32_t>({20200108u,20200108u,20200108u,20200110u,20200110u}), sink.tickDates);
    EXPECT_EQ(std::vector<uint32_t>({p07_action_time(5),p07_action_time(2200,10,100),p07_action_time(2359,59,999),p07_action_time(2200,10,100),p07_action_time(2359,59,999)}), sink.tickActionTimes);
    EXPECT_EQ(5u, clocks.size()); EXPECT_TRUE(std::is_sorted(clocks.begin(), clocks.end()));
    for (std::size_t i = 0; i < clocks.size() && i < sink.tickDates.size(); ++i)
        EXPECT_EQ(uint64_t(sink.tickDates[i]) * 1000000000ull + sink.tickActionTimes[i], clocks[i]);
    // A same-day older record must actually be returned by the default cutoff.
    ASSERT_EQ(5u, visible.size());
    EXPECT_EQ(1u, sink.replayDoneCalls);
}
