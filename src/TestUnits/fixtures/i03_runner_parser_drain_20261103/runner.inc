namespace {
class I03RunnerParserProbe
{
public:
 using Reset = void (*)(); using Count = int (*)(int); using Set = void (*)(int,int); using Reentry = void (*)(void (*)());
 I03RunnerParserProbe()
 {
  const std::string path=WtHelper::getCWD()+"parsers/"+DLLHelper::wrap_module("Pvt20","lib");
  module=DLLHelper::load_library(path.c_str());
  if(module){reset=reinterpret_cast<Reset>(DLLHelper::get_symbol(module,"probe_reset")); count=reinterpret_cast<Count>(DLLHelper::get_symbol(module,"probe_count")); set=reinterpret_cast<Set>(DLLHelper::get_symbol(module,"probe_set")); reentry=reinterpret_cast<Reentry>(DLLHelper::get_symbol(module,"probe_reentry"));}
 }
 ~I03RunnerParserProbe(){ if(module) DLLHelper::free_library(module); }
 bool valid() const {return module && reset && count && set && reentry;}
 DllHandle module=nullptr; Reset reset=nullptr; Count count=nullptr; Set set=nullptr; Reentry reentry=nullptr;
};
class I03RunnerTrader : public ITraderApi
{
public:
 bool init(WTSVariant*) override { return true; }
 void release() override { ++releases; }
 int releases=0;
};
struct I03RunnerCleanup
{
 WtRunner* runner; I03RunnerParserProbe* probe;
 ~I03RunnerCleanup(){ if(runner && probe && probe->valid()){probe->set(1,0); probe->set(2,1); probe->set(3,0); probe->reentry(nullptr); (void)runner->shutdown();} }
};
static bool i03AddTrader(WtRunner& runner,I03RunnerTrader& api)
{
 TraderAdapterPtr adapter(new TraderAdapter());
 return adapter->initExt("i03_runner_trader",&api,&runner._bd_mgr,&runner._act_policy) && runner._traders.addAdapter("i03_runner_trader",adapter);
}
static bool i03InitParser(WtRunner& runner,const char* id)
{
 WTSVariant* array=WTSVariant::createArray(); WTSVariant* item=WTSVariant::createObject();
 item->append("active",true); item->append("id",id); item->append("module","Pvt20"); array->append(item,false);
 const bool result=runner.initParsers(array); array->release(); return result;
}
static WtRunner* i03ReentryRunner=nullptr;
static bool i03ReentryResult=true;
static int i03ReentryCalls=0;
static void i03ReenterShutdown(){++i03ReentryCalls; i03ReentryResult=i03ReentryRunner->shutdown();}
}

TEST(I03RunnerParserDrain, EmptyRunnerShutdownIsIdempotent)
{
 auto runner=std::make_unique<WtRunner>(); EXPECT_TRUE(runner->shutdown()); EXPECT_TRUE(runner->shutdown()); EXPECT_TRUE(runner->_runtime_shutdown_complete);
}

TEST(I03RunnerParserDrain, StopsRegisteredAndFailedOwnerBeforeAnyPoll)
{
 I03RunnerParserProbe probe; ASSERT_TRUE(probe.valid()); probe.reset(); I03RunnerTrader api; auto runner=std::make_unique<WtRunner>(); I03RunnerCleanup cleanup{runner.get(),&probe};
 ASSERT_TRUE(i03AddTrader(*runner,api)); EXPECT_TRUE(i03InitParser(*runner,"registered"));
 probe.set(0,0); EXPECT_FALSE(i03InitParser(*runner,"failed")); probe.set(0,1); probe.set(3,2);
 EXPECT_EQ(runner->_parsers._adapters.size(),1u); EXPECT_EQ(runner->_failed_parser_owners.size(),1u);
 EXPECT_TRUE(runner->shutdown()); EXPECT_EQ(probe.count(2),2); EXPECT_EQ(probe.count(4),2); EXPECT_EQ(probe.count(6),0); EXPECT_EQ(probe.count(7),2); EXPECT_EQ(probe.count(5),0); EXPECT_EQ(api.releases,1);
 EXPECT_TRUE(runner->_parsers._adapters.empty()); EXPECT_TRUE(runner->_failed_parser_owners.empty()); EXPECT_TRUE(runner->shutdown()); EXPECT_EQ(api.releases,1); EXPECT_EQ(probe.count(7),2);
}

TEST(I03RunnerParserDrain, PendingKeepsEveryOwnerAndDownstreamUntilRetry)
{
 I03RunnerParserProbe probe; ASSERT_TRUE(probe.valid()); probe.reset(); I03RunnerTrader api; auto runner=std::make_unique<WtRunner>(); I03RunnerCleanup cleanup{runner.get(),&probe};
 ASSERT_TRUE(i03AddTrader(*runner,api)); EXPECT_TRUE(i03InitParser(*runner,"registered")); probe.set(0,0); EXPECT_FALSE(i03InitParser(*runner,"failed")); probe.set(0,1);
 probe.set(3,2); probe.set(1,2); EXPECT_FALSE(runner->shutdown());
 EXPECT_EQ(probe.count(2),2); EXPECT_EQ(probe.count(4),2); EXPECT_EQ(probe.count(6),0); EXPECT_EQ(probe.count(7),0); EXPECT_EQ(api.releases,0); EXPECT_EQ(runner->_parsers._adapters.size(),1u); EXPECT_EQ(runner->_failed_parser_owners.size(),1u); EXPECT_FALSE(runner->_runtime_shutdown_complete);
 EXPECT_TRUE(runner->shutdown()); EXPECT_EQ(probe.count(2),2); EXPECT_EQ(probe.count(4),4); EXPECT_EQ(probe.count(7),2); EXPECT_EQ(api.releases,1); EXPECT_TRUE(runner->shutdown()); EXPECT_EQ(api.releases,1);
}

TEST(I03RunnerParserDrain, CallbackReentryReturnsWithoutWaiting)
{
 I03RunnerParserProbe probe; ASSERT_TRUE(probe.valid()); probe.reset(); auto runner=std::make_unique<WtRunner>(); I03RunnerCleanup cleanup{runner.get(),&probe}; EXPECT_TRUE(i03InitParser(*runner,"reentry"));
 i03ReentryRunner=runner.get(); i03ReentryResult=true; i03ReentryCalls=0; probe.reentry(&i03ReenterShutdown);
 EXPECT_TRUE(runner->shutdown()); EXPECT_EQ(i03ReentryCalls,1); EXPECT_FALSE(i03ReentryResult); EXPECT_EQ(probe.count(7),1); probe.reentry(nullptr); i03ReentryRunner=nullptr;
}

TEST(I03RunnerParserDrain, UnsafeWaitRetainsParserAndDownstreamUntilRetry)
{
 I03RunnerParserProbe probe; ASSERT_TRUE(probe.valid()); probe.reset(); I03RunnerTrader api; auto runner=std::make_unique<WtRunner>(); I03RunnerCleanup cleanup{runner.get(),&probe};
 ASSERT_TRUE(i03AddTrader(*runner,api)); EXPECT_TRUE(i03InitParser(*runner,"wait_unsafe")); probe.set(2,0);
 EXPECT_FALSE(runner->shutdown()); EXPECT_EQ(probe.count(2),1); EXPECT_EQ(probe.count(3),1); EXPECT_EQ(probe.count(4),0); EXPECT_EQ(probe.count(7),0); EXPECT_EQ(api.releases,0); EXPECT_EQ(runner->_parsers._adapters.size(),1u);
 probe.set(2,1); EXPECT_TRUE(runner->shutdown()); EXPECT_EQ(probe.count(2),1); EXPECT_EQ(probe.count(4),1); EXPECT_EQ(probe.count(7),1); EXPECT_EQ(api.releases,1);
}
