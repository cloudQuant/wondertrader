namespace {
std::shared_ptr<WTSVariant> i03GroupCfg(const char* json)
{
 return std::shared_ptr<WTSVariant>(WTSCfgLoader::load_from_content(json),[](WTSVariant* p){if(p)p->release();});
}
}
TEST(I03ExecutionGroups, SignedFractionalAndExplicitGroupReconstructNet)
{
 auto cfg=i03GroupCfg(R"({"groups":[{"name":"G","items":{"A":1.5,"B":-2}}]})"); ASSERT_TRUE(cfg);
 execution_groups::Groups groups; std::string error; ASSERT_TRUE(execution_groups::parse(cfg.get(),groups,error)) << error;
 std::map<std::string,double> input{{"A",3.75},{"B",-5.0}}; execution_groups::Positions out,net;
 ASSERT_TRUE(execution_groups::calculate(groups,input,1.0,out,net,error)) << error;
 EXPECT_DOUBLE_EQ(out["G"],2.5); EXPECT_DOUBLE_EQ(out["A"],0.0); EXPECT_DOUBLE_EQ(out["B"],0.0); EXPECT_DOUBLE_EQ(net["A"],3.75); EXPECT_DOUBLE_EQ(net["B"],-5.0);
 input["A"]=-3.75; input["B"]=5.0; ASSERT_TRUE(execution_groups::calculate(groups,input,1.0,out,net,error)); EXPECT_DOUBLE_EQ(out["G"],-2.5); EXPECT_DOUBLE_EQ(net["A"],-3.75); EXPECT_DOUBLE_EQ(net["B"],5.0);
 input={{"G",2.0},{"A",-1.0},{"B",3.0}}; ASSERT_TRUE(execution_groups::calculate(groups,input,1.0,out,net,error)); EXPECT_DOUBLE_EQ(out["G"],2.0); EXPECT_DOUBLE_EQ(out["A"],-1.0); EXPECT_DOUBLE_EQ(out["B"],3.0); EXPECT_DOUBLE_EQ(net["A"],2.0); EXPECT_DOUBLE_EQ(net["B"],-1.0);
}
TEST(I03ExecutionGroups, OverlapUsesDeclaredOrderAndExplicitObjectOrder)
{
 const char* forward=R"({"groups":[{"name":"G1","items":{"A":1,"B":1}},{"name":"G2","items":{"A":1,"C":1}}]})";
 const char* reverse=R"({"groups":[{"name":"G2","items":{"A":1,"C":1}},{"name":"G1","items":{"A":1,"B":1}}]})";
 std::map<std::string,double> input{{"A",3.0},{"B",2.0},{"C",3.0}}; execution_groups::Groups groups; execution_groups::Positions out,net; std::string error;
 auto cfg=i03GroupCfg(forward); ASSERT_TRUE(cfg && execution_groups::parse(cfg.get(),groups,error)); ASSERT_TRUE(execution_groups::calculate(groups,input,1.0,out,net,error)); EXPECT_DOUBLE_EQ(out["G1"],2.0); EXPECT_DOUBLE_EQ(out["G2"],1.0); EXPECT_DOUBLE_EQ(out["C"],2.0);
 cfg=i03GroupCfg(reverse); ASSERT_TRUE(cfg && execution_groups::parse(cfg.get(),groups,error)); ASSERT_TRUE(execution_groups::calculate(groups,input,1.0,out,net,error)); EXPECT_EQ(out.count("G1"),0u); EXPECT_DOUBLE_EQ(out["G2"],3.0); EXPECT_DOUBLE_EQ(out["B"],2.0);
 cfg=i03GroupCfg(R"({"groups":{"G1":{"A":1,"B":1},"G2":{"A":1,"C":1}}})"); error.clear(); EXPECT_FALSE(execution_groups::parse(cfg.get(),groups,error));
 cfg=i03GroupCfg(R"({"groups":{"G1":{"A":1,"B":1},"G2":{"A":1,"C":1}},"group_order":["G2","G1"]})"); error.clear(); ASSERT_TRUE(execution_groups::parse(cfg.get(),groups,error)); ASSERT_TRUE(execution_groups::calculate(groups,input,1.0,out,net,error)); EXPECT_DOUBLE_EQ(out["G2"],3.0); EXPECT_EQ(out.count("G1"),0u);
}
TEST(I03ExecutionGroups, LimitUsesScaledGroupAndZeroIsValid)
{
 auto cfg=i03GroupCfg(R"({"groups":{"G":{"A":1}},"group_limits":{"G":2}})"); execution_groups::Groups groups; execution_groups::Positions out,net; std::string error; ASSERT_TRUE(cfg && execution_groups::parse(cfg.get(),groups,error));
 std::map<std::string,double> input{{"G",4.0}}; EXPECT_TRUE(execution_groups::calculate(groups,input,0.5,out,net,error)); input["G"]=4.5; EXPECT_FALSE(execution_groups::calculate(groups,input,0.5,out,net,error));
 cfg=i03GroupCfg(R"({"groups":{"G":{"A":1}},"group_limits":{"G":0}})"); error.clear(); ASSERT_TRUE(execution_groups::parse(cfg.get(),groups,error)); input["G"]=0.0; EXPECT_TRUE(execution_groups::calculate(groups,input,1.0,out,net,error)); input["G"]=0.5; EXPECT_FALSE(execution_groups::calculate(groups,input,1.0,out,net,error));
}
TEST(I03ExecutionGroups, RejectsInvalidConfigurationBeforePublishing)
{
 const char* configs[]={R"({"groups":[{"name":"G","items":{"A":1}},{"name":"bad","items":{"A":0}}]})",R"({"groups":[{"name":"G","items":{}}]})",R"({"groups":[{"name":"G","items":{"G":1}}]})",R"({"groups":[{"name":"G","items":{"A":1}},{"name":"H","items":{"G":1}}]})",R"({"groups":[{"name":"G","items":{"A":1}},{"name":"G","items":{"B":1}}]})",R"({"groups":{"G":{"A":1}},"group_order":["G","G"]})",R"({"groups":{"G":{"A":1}},"group_limits":{"G":-1}})",R"({"groups":{"12345678901234567890123456789012":{"A":1}}})"};
 for(const char* text:configs){auto cfg=i03GroupCfg(text); ASSERT_TRUE(cfg) << text; execution_groups::Groups result(1); result[0].name="unchanged"; std::string error; EXPECT_FALSE(execution_groups::parse(cfg.get(),result,error)) << text; EXPECT_EQ(result.size(),1u); EXPECT_EQ(result[0].name,"unchanged");}
}
TEST(I03ExecutionGroups, OverflowRejectsWithoutPublishingOutputs)
{
 auto cfg=i03GroupCfg(R"({"groups":{"G":{"A":2}}})"); execution_groups::Groups groups; execution_groups::Positions out{{"unchanged",7.0}},net{{"unchanged",8.0}}; std::string error; ASSERT_TRUE(cfg && execution_groups::parse(cfg.get(),groups,error));
 std::map<std::string,double> input{{"G",std::numeric_limits<double>::max()}}; EXPECT_FALSE(execution_groups::calculate(groups,input,1.0,out,net,error)); EXPECT_DOUBLE_EQ(out["unchanged"],7.0); EXPECT_DOUBLE_EQ(net["unchanged"],8.0);
}
TEST(I03ExecutionGroups, MismatchedDirectionsAndMissingLegDoNotInventGroups)
{
 auto cfg=i03GroupCfg(R"({"groups":[{"name":"G","items":{"A":1,"B":-1}}]})"); execution_groups::Groups groups; execution_groups::Positions out,net; std::string error; ASSERT_TRUE(cfg && execution_groups::parse(cfg.get(),groups,error));
 std::map<std::string,double> input{{"A",3.0},{"B",2.0}}; ASSERT_TRUE(execution_groups::calculate(groups,input,1.0,out,net,error)); EXPECT_EQ(out.count("G"),0u); EXPECT_DOUBLE_EQ(out["A"],3.0); EXPECT_DOUBLE_EQ(out["B"],2.0);
 input.erase("B"); ASSERT_TRUE(execution_groups::calculate(groups,input,1.0,out,net,error)); EXPECT_EQ(out.count("G"),0u); EXPECT_DOUBLE_EQ(out["A"],3.0);
}
