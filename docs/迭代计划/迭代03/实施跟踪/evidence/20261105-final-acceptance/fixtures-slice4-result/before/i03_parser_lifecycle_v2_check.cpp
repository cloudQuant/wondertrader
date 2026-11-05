#include <gtest/gtest.h>
#include <dlfcn.h>
#include <string>
#include <memory>
#include "WtCore/ParserAdapter.h"
#include "WTSTools/WTSBaseDataMgr.h"
#include "WTSTools/WTSLogger.h"
#include "Includes/WTSVariant.hpp"
using namespace wtp;
namespace {
struct Probe {
 void* module;
 void (*reset)(); int (*count)(int); void (*set)(int,int); void (*reentry)(void(*)());
 explicit Probe(int index) {
   std::string path = "./parsers/libPvt2" + std::to_string(index) + ".dylib";
   module = dlopen(path.c_str(), RTLD_NOW);
   if (!module) { reset=nullptr; count=nullptr; set=nullptr; reentry=nullptr; return; }
   reset = reinterpret_cast<void(*)()>(dlsym(module,"probe_reset"));
   count = reinterpret_cast<int(*)(int)>(dlsym(module,"probe_count"));
   set = reinterpret_cast<void(*)(int,int)>(dlsym(module,"probe_set"));
   reentry = reinterpret_cast<void(*)(void(*)())>(dlsym(module,"probe_reentry"));
 }
 ~Probe() { if (module) dlclose(module); }
};
struct Cfg { WTSVariant* p; explicit Cfg(int mode) : p(WTSVariant::createObject()) { auto name="Pvt2"+std::to_string(mode); p->append("module",name.c_str()); } ~Cfg(){p->release();} };
ParserAdapter* reentryAdapter = nullptr;
bool reentryResult = true;
void reenter() { reentryResult = reentryAdapter->requestStopLifecycle(); }
class ExternalParser : public IParserApi {
public:
 explicit ExternalParser(int& releases) : releases_(releases) {}
 bool init(WTSVariant*) override { return true; }
 void release() override { ++releases_; }
private: int& releases_;
};
class ParserV2 : public ::testing::Test {
protected:
 WTSBaseDataMgr mgr;
 void SetUp() override { static bool init=[] { WTSLogger::init(R"({"root":{"level":"debug","async":false,"sinks":[]}})",false,nullptr); return true; }(); (void)init; }
};
}
TEST_F(ParserV2, MissingAndInvalidTablesFailBeforeCreate)
{
 for (int mode : {1,2,3,4,5,6,7,9,10,11,12}) {
   Probe probe(mode); ASSERT_NE(nullptr, probe.module) << mode; probe.reset();
   Cfg cfg(mode); ParserAdapter adapter;
   EXPECT_FALSE(adapter.init("invalid",cfg.p,nullptr,&mgr));
   EXPECT_EQ(0,probe.count(0)) << mode;
   EXPECT_FALSE(adapter.hasApi());
   EXPECT_TRUE(adapter.requestStopLifecycle()); EXPECT_TRUE(adapter.finishReleaseLifecycle());
 }
}
TEST_F(ParserV2, ManagerRetainsPendingAndRetries)
{
 Probe p(0); ASSERT_NE(nullptr,p.module); p.reset(); p.set(1,1);
 Cfg cfg(0); ParserAdapterMgr manager; auto a=std::make_shared<ParserAdapter>();
 ASSERT_TRUE(a->init("pending",cfg.p,nullptr,&mgr)); ASSERT_TRUE(manager.addAdapter("pending",a));
 manager.release(); EXPECT_TRUE(manager.hasQuarantinedAdapters()); EXPECT_TRUE(a->hasApi());
 EXPECT_EQ(0,p.count(7)); EXPECT_EQ(2u,cfg.p->retainCount());
 manager.release(); EXPECT_FALSE(manager.hasQuarantinedAdapters()); EXPECT_EQ(1,p.count(7));
 EXPECT_EQ(1,p.count(2)); EXPECT_EQ(1u,cfg.p->retainCount());
}
TEST_F(ParserV2, StopAllBeforeAnyPollAndDestroy)
{
 Probe p(0); ASSERT_NE(nullptr,p.module); p.reset(); p.set(3,2);
 Cfg cfg(0); ParserAdapterMgr manager;
 auto a=std::make_shared<ParserAdapter>(); auto b=std::make_shared<ParserAdapter>();
 ASSERT_TRUE(a->init("a",cfg.p,nullptr,&mgr)); ASSERT_TRUE(b->init("b",cfg.p,nullptr,&mgr));
 ASSERT_TRUE(manager.addAdapter("a",a)); ASSERT_TRUE(manager.addAdapter("b",b));
 manager.release();
 EXPECT_EQ(2,p.count(2)); EXPECT_EQ(2,p.count(4)); EXPECT_EQ(0,p.count(6)); EXPECT_EQ(2,p.count(7));
 EXPECT_EQ(0,p.count(5)); EXPECT_FALSE(manager.hasQuarantinedAdapters());
}
TEST_F(ParserV2, PendingAndCanWaitRefusalAreRetryable)
{
 Probe p(0); ASSERT_NE(nullptr,p.module); p.reset(); p.set(1,1); p.set(2,0);
 Cfg cfg(0); ParserAdapter adapter;
 ASSERT_TRUE(adapter.init("a",cfg.p,nullptr,&mgr));
 EXPECT_EQ(2u,cfg.p->retainCount());
 EXPECT_FALSE(adapter.finishReleaseLifecycle());
 ASSERT_TRUE(adapter.requestStopLifecycle()); ASSERT_TRUE(adapter.requestStopLifecycle()); EXPECT_EQ(1,p.count(2));
 EXPECT_FALSE(adapter.finishReleaseLifecycle()); EXPECT_EQ(0,p.count(4)); EXPECT_EQ(0,p.count(7));
 p.set(2,1); EXPECT_FALSE(adapter.finishReleaseLifecycle()); EXPECT_TRUE(adapter.hasApi()); EXPECT_EQ(0,p.count(7));
 EXPECT_EQ(2u,cfg.p->retainCount());
 EXPECT_TRUE(adapter.finishReleaseLifecycle()); EXPECT_FALSE(adapter.hasApi()); EXPECT_EQ(1,p.count(7)); EXPECT_EQ(1u,cfg.p->retainCount());
}
TEST_F(ParserV2, FailedInitRetainsAndDrainsBeforeReinit)
{
 Probe p(0); ASSERT_NE(nullptr,p.module); p.reset(); p.set(0,0);
 Cfg cfg(0); ParserAdapter adapter;
 EXPECT_FALSE(adapter.init("bad",cfg.p,nullptr,&mgr)); EXPECT_TRUE(adapter.hasApi());
 EXPECT_FALSE(adapter.init("again",cfg.p,nullptr,&mgr)); EXPECT_EQ(1,p.count(0));
 ASSERT_TRUE(adapter.requestStopLifecycle()); ASSERT_TRUE(adapter.finishReleaseLifecycle()); EXPECT_EQ(1,p.count(7));
 p.set(0,1); ASSERT_TRUE(adapter.init("again",cfg.p,nullptr,&mgr));
 ASSERT_TRUE(adapter.requestStopLifecycle()); EXPECT_TRUE(adapter.finishReleaseLifecycle());
}
TEST_F(ParserV2, SynchronousStopReentryDoesNotBlock)
{
 Probe p(0); ASSERT_NE(nullptr,p.module); p.reset(); Cfg cfg(0); ParserAdapter adapter;
 ASSERT_TRUE(adapter.init("a",cfg.p,nullptr,&mgr)); reentryAdapter=&adapter; reentryResult=true; p.reentry(reenter);
 ASSERT_TRUE(adapter.requestStopLifecycle()); EXPECT_FALSE(reentryResult);
 p.reentry(nullptr); EXPECT_TRUE(adapter.finishReleaseLifecycle()); reentryAdapter=nullptr;
}
TEST_F(ParserV2, AmbiguousDestroyIsStickyAndPreservesOwner)
{
 Probe p(0); ASSERT_NE(nullptr,p.module); p.reset(); p.set(4,1); Cfg cfg(0);
 auto* adapter = new ParserAdapter; // Quarantined: a non-null destroy success has uncertain ownership.
 ASSERT_TRUE(adapter->init("sticky",cfg.p,nullptr,&mgr)); ASSERT_TRUE(adapter->requestStopLifecycle());
 EXPECT_FALSE(adapter->finishReleaseLifecycle()); EXPECT_TRUE(adapter->hasApi());
 EXPECT_FALSE(adapter->finishReleaseLifecycle()); EXPECT_EQ(1,p.count(7)); EXPECT_FALSE(adapter->init("again",cfg.p,nullptr,&mgr));
}
TEST_F(ParserV2, ExternalWithoutV2IsNotDestroyed)
{
 static int releases=0; auto* external = new ExternalParser(releases);
 auto* adapter = new ParserAdapter; // External module and owner must remain quarantined.
 ASSERT_TRUE(adapter->initExt("ext",external,nullptr,&mgr));
 EXPECT_FALSE(adapter->requestStopLifecycle()); EXPECT_FALSE(adapter->finishReleaseLifecycle());
 EXPECT_TRUE(adapter->hasApi()); EXPECT_EQ(0,releases);
}

TEST_F(ParserV2, FactoryExceptionRetainsUncertainOwner)
{
 Probe p(8); ASSERT_NE(nullptr,p.module); p.reset(); Cfg cfg(8);
 auto* adapter = new ParserAdapter; // Factory ownership is uncertain and is retained.
 EXPECT_FALSE(adapter->init("throw",cfg.p,nullptr,&mgr));
 EXPECT_EQ(1,p.count(0)); EXPECT_TRUE(adapter->hasApi()); EXPECT_EQ(2u,cfg.p->retainCount());
 EXPECT_FALSE(adapter->requestStopLifecycle()); EXPECT_FALSE(adapter->finishReleaseLifecycle());
 EXPECT_FALSE(adapter->init("again",cfg.p,nullptr,&mgr)); EXPECT_EQ(1,p.count(0));
}
