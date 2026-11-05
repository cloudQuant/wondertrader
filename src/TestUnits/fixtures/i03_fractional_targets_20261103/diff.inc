#include <limits>
TEST_F(P02DiffExecuterTest, FractionalTargetsAndInvalidInputsPreserveLedger)
{
 std::error_code ec; fs::remove(exec_data_file("p02_diff_exe"), ec);
 ASSERT_TRUE(init_with(kDiffCfg,"i03_diff_fraction.json"));
 const std::string code="SHFE.ag.2212"; wt_hashmap<std::string,double> targets; targets[code]=1.25;
 _executer.set_position(targets);
 EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::target(_executer,code),2.5);
 EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::diff(_executer,code),2.5);
 _executer.on_position_changed(code.c_str(),-2.0);
 EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::target(_executer,code),-1.5);
 EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::diff(_executer,code),-1.5);
 targets[code]=4.0; targets["CFFEX.bad"]=std::numeric_limits<double>::quiet_NaN(); _executer.set_position(targets);
 EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::target(_executer,code),-1.5); targets.erase("CFFEX.bad");
 targets[code]=std::numeric_limits<double>::infinity(); _executer.set_position(targets);
 _executer.on_position_changed(code.c_str(),std::numeric_limits<double>::quiet_NaN());
 EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::target(_executer,code),-1.5);
 EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::diff(_executer,code),-1.5);
 targets[code]=std::numeric_limits<double>::max(); _executer.set_position(targets);
 EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::target(_executer,code),-1.5);
 _executer.on_position_changed(code.c_str(),0.0000001);
 EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::target(_executer,code),-1.5+0.0000002);
}
TEST_F(P02DiffExecuterTest, ExecutionScaleRequiresCompletePositiveNumber)
{
 // Card 8b=A (eager validation): every config carries a valid policy so that each
 // rejection below is attributable to the scale value alone, and the positive cases
 // describe a complete configuration (the executers reject a missing policy).
 const char* configs[]={"{\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}", "{\"scale\":null,\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}", "{\"scale\":true,\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}", "{\"scale\":[],\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}", "{\"scale\":{},\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}", "{\"scale\":0,\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}", "{\"scale\":-1,\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}", "{\"scale\":\"1junk\",\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}", "{\"scale\":\"nan\",\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}", "{\"scale\":\"inf\",\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}"};
 for(const char* text:configs) { WtDiffExecuter exe(&_factory,"i03_diff_scale",&_reader,&_bd_mgr); std::shared_ptr<WTSVariant> cfg(WTSCfgLoader::load_from_content(text),[](WTSVariant* p){if(p)p->release();}); ASSERT_TRUE(cfg); EXPECT_FALSE(exe.init(cfg.get())) << text; }
 WtDiffExecuter exe(&_factory,"i03_diff_scale_good",&_reader,&_bd_mgr); for(double value : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) { std::shared_ptr<WTSVariant> invalid(WTSCfgLoader::load_from_content("{\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}"),[](WTSVariant* p){if(p)p->release();}); ASSERT_TRUE(invalid->append("scale",value)); EXPECT_FALSE(exe.init(invalid.get())); }
 std::shared_ptr<WTSVariant> cfg(WTSCfgLoader::load_from_content("{\"scale\":\"1.25\",\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}"),[](WTSVariant* p){if(p)p->release();}); ASSERT_TRUE(cfg); EXPECT_TRUE(exe.init(cfg.get()));
 WtDiffExecuter tiny(&_factory,"i03_diff_scale_tiny",&_reader,&_bd_mgr); std::shared_ptr<WTSVariant> tinycfg(WTSCfgLoader::load_from_content("{\"scale\":\"4.9406564584124654e-324\",\"policy\":{\"default\":{\"name\":\"I03ScaleFact.I03ScaleUnit\"}}}"),[](WTSVariant* p){if(p)p->release();}); ASSERT_TRUE(tinycfg); EXPECT_TRUE(tiny.init(tinycfg.get()));
}
namespace {
class I03DiffUnit : public ExecuteUnit {
public:
 explicit I03DiffUnit(std::shared_ptr<std::vector<double>> values): values_(std::move(values)) {}
 const char* getName() override { return "I03DiffUnit"; } const char* getFactName() override { return "I03DiffFact"; }
 void set_position(const char*,double value) override { values_->push_back(value); }
 void on_tick(WTSTickData*) override {} void on_trade(uint32_t,const char*,bool,double,double) override {} void on_order(uint32_t,const char*,bool,double,double,bool) override {} void on_entrust(uint32_t,const char*,bool,const char*) override {} void on_channel_ready() override {} void on_channel_lost() override {}
private: std::shared_ptr<std::vector<double>> values_;
};
class I03DiffFact : public IExecuterFact {
public:
 const char* getName() override { return "I03DiffFact"; } void enumExeUnit(FuncEnumUnitCallback) override {}
 ExecuteUnit* createExeUnit(const char*) override { return nullptr; } ExecuteUnit* createDiffExeUnit(const char*) override { return nullptr; } ExecuteUnit* createArbiExeUnit(const char*) override { return nullptr; } bool deleteExeUnit(ExecuteUnit* unit) override { delete unit; return true; }
};
}
TEST_F(P02DiffExecuterTest, FractionalTargetQueuesAndDispatchesWithoutInvalidUpdates)
{
 std::error_code ec; fs::remove(exec_data_file("p02_diff_exe"),ec); ASSERT_TRUE(init_with(kDiffCfg,"i03_diff_queue.json"));
 I03DiffFact fact; auto calls=std::make_shared<std::vector<double>>(); const std::string code="SHFE.ag.2212";
 P02DiffExecuterTestAccess::registerInitializing(_executer,code,new I03DiffUnit(calls),&fact);
 wt_hashmap<std::string,double> targets; targets[code]=1.25; _executer.set_position(targets); _executer.on_position_changed(code.c_str(),-2.0);
 targets[code]=std::numeric_limits<double>::quiet_NaN(); _executer.set_position(targets); _executer.on_position_changed(code.c_str(),std::numeric_limits<double>::infinity());
 EXPECT_TRUE(calls->empty()); EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::target(_executer,code),-1.5);
 P02DiffExecuterTestAccess::finishInitializing(_executer,code); EXPECT_EQ(calls->size(),2u); if(calls->size()==2) { EXPECT_DOUBLE_EQ((*calls)[0],2.5); EXPECT_DOUBLE_EQ((*calls)[1],-1.5); }
 _executer.on_position_changed(code.c_str(),0.125); EXPECT_EQ(calls->size(),3u); if(calls->size()==3) EXPECT_DOUBLE_EQ(calls->back(),-1.25);
 P02DiffExecuterTestAccess::unregisterUnit(_executer,code);
}
