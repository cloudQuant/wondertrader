#include <limits>
TEST_F(R7VwapV2LocalIntegration, FractionalTargetsScaleAndRejectNonFinite)
{
 WTSSessionInfo* session=makeSession("I03_FRACTION",{{900,1500}}); ASSERT_NE(session,nullptr);
 addInstrument("SHFE","rb","rb2510","SHFE.rb.2510","I03_FRACTION",TM_Both,CC_Future,session);
 LocalGroupPositionFact fact;
 std::shared_ptr<CapturingLocalExecuter> local(new CapturingLocalExecuter(&_factory, "i03_local_fraction", &_data_manager));
 _manager.add_executer(std::static_pointer_cast<IExecCommand>(local)); _executors.push_back(local); local->setStub(&_stub);
 std::shared_ptr<WTSVariant> config(WTSCfgLoader::load_from_content(R"({"scale":0.5,"poolsize":0,"policy":{"default":{"name":"LocalGroupPositionFact.LocalGroupPositionUnit"}}})"), [](WTSVariant* p){if(p)p->release();});
 ASSERT_TRUE(config && local->init(config.get()));
 const std::string code="SHFE.rb.2510"; auto probe=std::make_shared<LocalGroupPositionProbe>();
 WtLocalExecuterTestAccess::registerUnit(*local, code, new LocalGroupPositionUnit(probe), &fact);
 wt_hashmap<std::string,double> targets; targets[code]=1.5; local->set_position(targets);
 ASSERT_EQ(probe->snapshot().size(),1u); EXPECT_DOUBLE_EQ(probe->snapshot().back().second,0.75);
 local->on_position_changed(code.c_str(),-2.0); ASSERT_EQ(probe->snapshot().size(),2u); EXPECT_DOUBLE_EQ(probe->snapshot().back().second,-0.25);
 targets[code]=4.0; targets["CFFEX.bad"]=std::numeric_limits<double>::quiet_NaN(); local->set_position(targets);
 EXPECT_EQ(probe->snapshot().size(),2u); EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local,code),-0.5); targets.erase("CFFEX.bad");
 targets[code]=std::numeric_limits<double>::infinity(); local->set_position(targets);
 local->on_position_changed(code.c_str(),std::numeric_limits<double>::quiet_NaN());
 EXPECT_EQ(probe->snapshot().size(),2u); EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local,code),-0.5);
 targets[code]=std::numeric_limits<double>::max(); local->set_position(targets); local->on_position_changed(code.c_str(),std::numeric_limits<double>::max());
 EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local,code),std::numeric_limits<double>::max()); EXPECT_EQ(probe->snapshot().size(),3u);
 WtLocalExecuterTestAccess::unregisterUnits(*local,{code});
}
TEST_F(R7VwapV2LocalIntegration, ExecutionScaleRequiresCompletePositiveNumber)
{
 // Card 8b=A (eager validation): every config carries a valid policy so that each
 // rejection below is attributable to the scale value alone, and the positive cases
 // describe a complete configuration (the executers reject a missing policy).
 const char* configs[]={"{\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}", "{\"scale\":null,\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}", "{\"scale\":true,\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}", "{\"scale\":[],\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}", "{\"scale\":{},\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}", "{\"scale\":0,\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}", "{\"scale\":-1,\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}", "{\"scale\":\"1junk\",\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}", "{\"scale\":\"nan\",\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}", "{\"scale\":\"inf\",\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}"};
 auto local=std::make_shared<CapturingLocalExecuter>(&_factory,"i03_local_scale",&_data_manager); _manager.add_executer(std::static_pointer_cast<IExecCommand>(local)); _executors.push_back(local);
 for (const char* text : configs) {
  std::shared_ptr<WTSVariant> cfg(WTSCfgLoader::load_from_content(text),[](WTSVariant* p){if(p)p->release();}); ASSERT_TRUE(cfg); EXPECT_FALSE(local->init(cfg.get())) << text;
 }
 for(double value : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
  std::shared_ptr<WTSVariant> invalid(WTSCfgLoader::load_from_content("{\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}"),[](WTSVariant* p){if(p)p->release();}); ASSERT_TRUE(invalid->append("scale",value)); EXPECT_FALSE(local->init(invalid.get()));
 }
 std::shared_ptr<WTSVariant> cfg(WTSCfgLoader::load_from_content("{\"scale\":\"1.25\",\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}"),[](WTSVariant* p){if(p)p->release();}); ASSERT_TRUE(cfg); EXPECT_TRUE(local->init(cfg.get()));
 auto tiny=std::make_shared<CapturingLocalExecuter>(&_factory,"i03_local_scale_tiny",&_data_manager); _manager.add_executer(std::static_pointer_cast<IExecCommand>(tiny)); _executors.push_back(tiny);
 std::shared_ptr<WTSVariant> tinycfg(WTSCfgLoader::load_from_content("{\"scale\":\"4.9406564584124654e-324\",\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}"),[](WTSVariant* p){if(p)p->release();}); ASSERT_TRUE(tinycfg); EXPECT_TRUE(tiny->init(tinycfg.get()));
}
TEST_F(R7VwapV2LocalIntegration, FractionalTargetReconcilesAfterInitialization)
{
 WTSSessionInfo* session=makeSession("I03_FRACTION",{{900,1500}}); ASSERT_NE(session,nullptr);
 addInstrument("SHFE","rb","rb2510","SHFE.rb.2510","I03_FRACTION",TM_Both,CC_Future,session);
 LocalGroupPositionFact fact; auto local=std::make_shared<CapturingLocalExecuter>(&_factory,"i03_local_queued",&_data_manager);
 _manager.add_executer(std::static_pointer_cast<IExecCommand>(local)); _executors.push_back(local); local->setStub(&_stub);
 std::shared_ptr<WTSVariant> cfg(WTSCfgLoader::load_from_content(R"({"scale":0.5,"poolsize":0,"policy":{"default":{"name":"LocalGroupPositionFact.LocalGroupPositionUnit"}}})"),[](WTSVariant* p){if(p)p->release();}); ASSERT_TRUE(cfg && local->init(cfg.get()));
 const std::string code="SHFE.rb.2510"; auto probe=std::make_shared<LocalGroupPositionProbe>(); WtLocalExecuterTestAccess::registerUnit(*local,code,new LocalGroupPositionUnit(probe),&fact); WtLocalExecuterTestAccess::makeInitializing(*local,code);
 wt_hashmap<std::string,double> targets; targets[code]=1.5; local->set_position(targets); local->on_position_changed(code.c_str(),-2.0);
 EXPECT_TRUE(probe->snapshot().empty()); WtLocalExecuterTestAccess::finishInitializing(*local,code);
 ASSERT_EQ(probe->snapshot().size(),1u); EXPECT_DOUBLE_EQ(probe->snapshot().back().second,-0.25); EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local,code),-0.5);
 WtLocalExecuterTestAccess::unregisterUnits(*local,{code});
}
