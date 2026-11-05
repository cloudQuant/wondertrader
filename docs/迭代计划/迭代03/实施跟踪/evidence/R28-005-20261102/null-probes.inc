
TEST(R28NullProbe, JsonNullRouterItemMustNotDisappear)
{
    const std::string json = R"({"router_rules":[{"strategy":"valid","executer":"exec"},null]})";
    WTSVariant* config = WTSCfgLoader::load_from_content(json, false);
    ASSERT_NE(nullptr, config);
    WTSVariant* rules = config->get("router_rules");
    ASSERT_NE(nullptr, rules);
    EXPECT_EQ(2u, rules->size());
    WtExecuterMgr mgr;
    EXPECT_FALSE(mgr.load_router_rules(rules));
    config->release();
}

TEST(R28NullProbe, YamlNullRouterItemMustNotDisappear)
{
    const std::string yaml = "router_rules:\n  - strategy: valid\n    executer: exec\n  - null\n";
    WTSVariant* config = WTSCfgLoader::load_from_content(yaml, true);
    ASSERT_NE(nullptr, config);
    WTSVariant* rules = config->get("router_rules");
    ASSERT_NE(nullptr, rules);
    EXPECT_EQ(2u, rules->size());
    WtExecuterMgr mgr;
    EXPECT_FALSE(mgr.load_router_rules(rules));
    config->release();
}

TEST(R28NullProbe, JsonNullExecuterElementMustNotDisappear)
{
    const std::string json = R"({"router_rules":[{"strategy":"valid","executer":["exec",null]}]})";
    WTSVariant* config = WTSCfgLoader::load_from_content(json, false);
    ASSERT_NE(nullptr, config);
    WTSVariant* rules = config->get("router_rules");
    ASSERT_NE(nullptr, rules);
    ASSERT_NE(nullptr, rules->get(0u));
    ASSERT_NE(nullptr, rules->get(0u)->get("executer"));
    EXPECT_EQ(2u, rules->get(0u)->get("executer")->size());
    WtExecuterMgr mgr;
    EXPECT_FALSE(mgr.load_router_rules(rules));
    config->release();
}

TEST(R28NullProbe, JsonNullOnlyRuleMustNotDisappear)
{
    const std::string json = R"({"router_rules":[null]})";
    WTSVariant* config = WTSCfgLoader::load_from_content(json, false);
    ASSERT_NE(nullptr, config);
    WTSVariant* rules = config->get("router_rules");
    ASSERT_NE(nullptr, rules);
    EXPECT_EQ(1u, rules->size());
    WtExecuterMgr mgr;
    EXPECT_FALSE(mgr.load_router_rules(rules));
    config->release();
}
