// Compile with the production WtEngine TU. The existing R130 fixture supplies
// a real CTA engine and base-data manager; this test does not start a provider.
#include "../../coverage/q01_wtcore3/r130_engine_fee_test.cpp"

TEST(I03FeeEngineGate, RejectsUnsupportedEnvelopeWithoutChangingFees)
{
    r130::TempEnv env("fee_v2_gate");
    q01sel::BaseDataMgr bd;
    q01sel::HotMgr hot;
    prepareEnv(bd, hot);
    const std::string legacy =
        R"({"SHFE.ag":{"open":1.5,"close":1.2,"closetoday":1,"byvolume":true}})";
    const std::string path = writeFeeFile(legacy, "fee_gate.json");
    R130FeeEngine engine;
    engine.attach(&bd, &hot);
    initEngine(engine, bd, hot, path.c_str());
    ASSERT_TRUE(engine.fee_config_valid());
    ASSERT_DOUBLE_EQ(3.0, engine.calc_fee(kAg, 3500.0, 2.0, 0));
    WTSCommodityInfo* commodity = bd.getCommodity("SHFE", "ag");
    ASSERT_NE(nullptr, commodity);

    // Card 13d (I03-NEW-008) made the version-2 envelope a supported format, parsed
    // strictly by wt_fee::Schedule; any other version is still unsupported and the
    // whole file must be rejected while the active snapshot is kept.
    overwriteFeeFile(path,
        R"({"version":3,"contracts":{"SHFE.ag2512":{"open":10,"close":11,"closetoday":12,"byvolume":true}},"products":{}})");
    engine.reloadFees(path.c_str());
    EXPECT_FALSE(engine.fee_config_valid());
    EXPECT_DOUBLE_EQ(3.0, engine.calc_fee(kAg, 3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(3.0, commodity->calcFee(3500.0, 2.0, 0));
    // The CTA run gate must return before touching the absent data manager.
    engine.run();

    overwriteFeeFile(path, legacy);
    engine.reloadFees(path.c_str());
    EXPECT_TRUE(engine.fee_config_valid());
    EXPECT_DOUBLE_EQ(3.0, engine.calc_fee(kAg, 3500.0, 2.0, 0));
}

TEST(I03FeeEngineGate, ConfiguredUnreadableOrInvalidFilesRejectStartup)
{
    r130::TempEnv env("fee_failed_gate");
    q01sel::BaseDataMgr bd;
    q01sel::HotMgr hot;
    prepareEnv(bd, hot);
    const std::string path = writeFeeFile("{}", "fee_bad.json");
    R130FeeEngine engine;
    engine.attach(&bd, &hot);
    initEngine(engine, bd, hot, path.c_str());
    ASSERT_TRUE(engine.fee_config_valid());

    const std::string invalid[] = {"", "{", "[]", "null"};
    for (const auto& content : invalid)
    {
        overwriteFeeFile(path, content);
        engine.reloadFees(path.c_str());
        EXPECT_FALSE(engine.fee_config_valid()) << "input=" << content;
        if (!engine.fee_config_valid())
            engine.run();
    }
    engine.reloadFees((path + ".missing").c_str());
    EXPECT_FALSE(engine.fee_config_valid());
    const std::string wrongExtension = writeFeeFile("{}", "fees.txt");
    engine.reloadFees(wrongExtension.c_str());
    EXPECT_FALSE(engine.fee_config_valid());
    const std::string malformedYaml = writeFeeFile("version: [", "fees.yaml");
    engine.reloadFees(malformedYaml.c_str());
    EXPECT_FALSE(engine.fee_config_valid());

    overwriteFeeFile(path, "{}");
    engine.reloadFees(path.c_str());
    EXPECT_TRUE(engine.fee_config_valid());
}

TEST(I03FeeEngineGate, InvalidFileAtInitIsRejectedByCtaRun)
{
    // Card 13d: an empty version-2 envelope is valid; these remain invalid (syntax
    // error, unsupported version, and a v2 product entry missing required fields).
    const char* invalidFiles[] = {"{", R"({"version":3,"contracts":{},"products":{}})",
        R"({"version":2,"contracts":{},"products":{"SHFE.ag":{"open":1}}})"};
    for (const char* invalid : invalidFiles)
    {
        r130::TempEnv env("fee_init_gate");
        q01sel::BaseDataMgr bd;
        q01sel::HotMgr hot;
        prepareEnv(bd, hot);
        const std::string path = writeFeeFile(invalid, "fee_init.json");
        R130FeeEngine engine;
        engine.attach(&bd, &hot);
        initEngine(engine, bd, hot, path.c_str());
        ASSERT_FALSE(engine.fee_config_valid()) << invalid;
        engine.run(); // returns before attempting to use the absent data manager
    }
}

TEST(I03FeeEngineGate, NullFilenameMeansNoConfiguredTemplate)
{
    r130::TempEnv env("fee_null_gate");
    q01sel::BaseDataMgr bd;
    q01sel::HotMgr hot;
    prepareEnv(bd, hot);
    R130FeeEngine engine;
    engine.attach(&bd, &hot);
    initEngine(engine, bd, hot);
    engine.reloadFees(nullptr);
    EXPECT_TRUE(engine.fee_config_valid());
}
