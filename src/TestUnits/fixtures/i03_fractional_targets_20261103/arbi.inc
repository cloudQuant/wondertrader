#include <limits>
TEST(WtArbiExecuterSerialization, FractionalTargetsScaleQueueAndRejectNonFinite)
{
 ArbiProbeFact fact; auto probe=std::make_shared<ArbiProbe>();
 std::shared_ptr<WTSCommodityInfo> commodity(r71::makeComm("SHFE","rb","I03",TM_Both),[](WTSCommodityInfo* p){if(p)p->release();}); r71::R71ExecStub stub; stub.comm_=commodity.get();
 WtArbiExecuter executer(NULL,"i03_arbi_fraction",NULL); executer.setStub(&stub);
 std::shared_ptr<WTSVariant> cfg(WTSCfgLoader::load_from_content(R"({"scale":0.5,"poolsize":0,"policy":{"default":{"name":"WtExeFact.WtMinImpactExeUnit"}}})"), [](WTSVariant* p){if(p)p->release();});
 ASSERT_TRUE(cfg && executer.init(cfg.get())); const std::string code="SHFE.rb.2510";
 wtp::WtArbiExecuterTestAccess::registerUnit(executer,code,probe,&fact,false);
 wtp::WtArbiExecuterTestAccess::makeInitializing(executer,code);
 wt_hashmap<std::string,double> targets; targets[code]=1.5; executer.set_position(targets);
 wtp::WtArbiExecuterTestAccess::drainPending(executer,code);
 ASSERT_EQ(probe->positionSnapshot().size(),1u); EXPECT_DOUBLE_EQ(probe->positionSnapshot().back(),0.75);
 executer.on_position_changed(code.c_str(),-2.0); ASSERT_EQ(probe->positionSnapshot().size(),2u); EXPECT_DOUBLE_EQ(probe->positionSnapshot().back(),-0.25);
 targets[code]=4.0; targets["CFFEX.bad"]=std::numeric_limits<double>::quiet_NaN(); executer.set_position(targets);
 EXPECT_EQ(probe->positionSnapshot().size(),2u); EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer,code),-0.5); targets.erase("CFFEX.bad");
 targets[code]=std::numeric_limits<double>::infinity(); executer.set_position(targets); executer.on_position_changed(code.c_str(),std::numeric_limits<double>::quiet_NaN());
 EXPECT_EQ(probe->positionSnapshot().size(),2u); EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer,code),-0.5);
 targets[code]=std::numeric_limits<double>::max(); executer.set_position(targets); executer.on_position_changed(code.c_str(),std::numeric_limits<double>::max());
 EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer,code),std::numeric_limits<double>::max()); EXPECT_EQ(probe->positionSnapshot().size(),3u);
}
TEST(WtArbiExecuterSerialization, ExecutionScaleRequiresCompletePositiveNumber)
{
 // Card 8b=A (eager validation): every config carries a valid policy so that each
 // rejection below is attributable to the scale value alone, and the positive cases
 // describe a complete configuration (the executers reject a missing policy).
 const char* configs[]={"{\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}", "{\"scale\":null,\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}", "{\"scale\":true,\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}", "{\"scale\":[],\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}", "{\"scale\":{},\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}", "{\"scale\":0,\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}", "{\"scale\":-1,\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}", "{\"scale\":\"1junk\",\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}", "{\"scale\":\"nan\",\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}", "{\"scale\":\"inf\",\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}"};
 for(const char* text:configs) { WtArbiExecuter exe(NULL,"i03_arbi_scale",NULL); std::shared_ptr<WTSVariant> cfg(WTSCfgLoader::load_from_content(text),[](WTSVariant* p){if(p)p->release();}); ASSERT_TRUE(cfg); EXPECT_FALSE(exe.init(cfg.get())) << text; }
 WtArbiExecuter exe(NULL,"i03_arbi_scale_good",NULL); for(double value : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) { std::shared_ptr<WTSVariant> invalid(WTSCfgLoader::load_from_content("{\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}"),[](WTSVariant* p){if(p)p->release();}); ASSERT_TRUE(invalid->append("scale",value)); EXPECT_FALSE(exe.init(invalid.get())); }
 std::shared_ptr<WTSVariant> cfg(WTSCfgLoader::load_from_content("{\"scale\":\"1.25\",\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}"),[](WTSVariant* p){if(p)p->release();}); ASSERT_TRUE(cfg); EXPECT_TRUE(exe.init(cfg.get()));
 WtArbiExecuter tiny(NULL,"i03_arbi_scale_tiny",NULL); std::shared_ptr<WTSVariant> tinycfg(WTSCfgLoader::load_from_content("{\"scale\":\"4.9406564584124654e-324\",\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}"),[](WTSVariant* p){if(p)p->release();}); ASSERT_TRUE(tinycfg); EXPECT_TRUE(tiny.init(tinycfg.get()));
}
