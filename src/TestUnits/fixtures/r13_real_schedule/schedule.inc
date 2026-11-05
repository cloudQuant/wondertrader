class R13RecordContext : public q01sel::SelCtx {
public:
    using q01sel::SelCtx::SelCtx;
    bool on_schedule(uint32_t, uint32_t, uint32_t) override { ++fired; return true; }
    std::atomic<unsigned> fired{0};
};
TEST(R13RealSchedule, TriggeredDailyTargetsRegisteredContext) {
    q01sel::Q01TempEnv temp("r13daily");
    q01sel::BaseDataMgr bd;
    bd.regisSessionWithTime("TRADING", 900, 1500);
    q01sel::HotMgr hot;
    WtDtMgr dm;
    WtSelEngine engine;
    WTSVariant* cfg = WTSCfgLoader::load_from_content("{\"product\":{\"session\":\"TRADING\"},\"fees\":\"\",\"filters\":\"\"}");
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &bd, &dm, &hot, nullptr);
    cfg->release();
    engine.set_date_time(20250110, 930, 0);
    engine.set_trading_date(20250110);
    auto burn = std::make_shared<R13RecordContext>(&engine, "burn");
    auto target = std::make_shared<R13RecordContext>(&engine, "target");
    ASSERT_NE(burn->id(), target->id());
    engine.addContext(target, 0, 931, TPT_Daily, true, "CHINA", "TRADING");
    engine.on_minute_end(20250110, 930);
    EXPECT_EQ(target->fired.load(), 1u);
    EXPECT_EQ(burn->fired.load(), 0u);
}
TEST(R13RealSchedule, MonthlyOnRequestedCalendarDay) {
    q01sel::Q01TempEnv temp("r13monthly");
    q01sel::BaseDataMgr bd;
    bd.regisSessionWithTime("TRADING", 900, 1500);
    q01sel::HotMgr hot;
    WtDtMgr dm;
    WtSelEngine engine;
    WTSVariant* cfg = WTSCfgLoader::load_from_content("{\"product\":{\"session\":\"TRADING\"},\"fees\":\"\",\"filters\":\"\"}");
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &bd, &dm, &hot, nullptr);
    cfg->release();
    engine.set_date_time(20250110, 930, 0);
    engine.set_trading_date(20250110);
    auto target = std::make_shared<R13RecordContext>(&engine, "monthly");
    engine.addContext(target, 10, 931, TPT_Monthly, true, "CHINA", "TRADING");
    engine.on_minute_end(20250110, 930);
    EXPECT_EQ(target->fired.load(), 1u);
}
