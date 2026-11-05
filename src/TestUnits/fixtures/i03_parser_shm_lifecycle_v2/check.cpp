#include <gtest/gtest.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>
#include "ParserShm/ParserShm.h"
#include "Includes/IParserLifecycleV2.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSCollection.hpp"
#include "Includes/IBaseDataMgr.h"
#include "WTSTools/WTSBaseDataMgr.h"
#include "WTSTools/WTSLogger.h"
#include "WtCore/ParserAdapter.h"
using namespace wtp;
using namespace std::chrono_literals;
namespace {
struct Module {
 explicit Module(const char* path="./parsers/libParserShm.dylib") : handle(dlopen(path, RTLD_NOW | RTLD_LOCAL)) {}
 void* handle;
 FuncCreateParser create = handle ? reinterpret_cast<FuncCreateParser>(dlsym(handle,"createParser")) : nullptr;
 WtParserLifecycleV2GetterFn get = handle ? reinterpret_cast<WtParserLifecycleV2GetterFn>(dlsym(handle,WT_PARSER_LIFECYCLE_V2_GETTER_SYMBOL)) : nullptr;
 const WtParserLifecycleApiV2* table = get ? get(WT_PARSER_LIFECYCLE_V2_ABI_MAJOR) : nullptr;
 int live=0;
 ~Module(){if(handle && live==0) dlclose(handle);}
};
struct ShmFile {
 char dir[64]="/tmp/wt-shm-v2-XXXXXX";
 std::string path; int fd=-1; void* map=MAP_FAILED; ParserShm::CastQueue* queue=nullptr; bool ownDir=false;
 explicit ShmFile(bool create=true, bool shortFile=false) {
  if(!mkdtemp(dir)) return; ownDir=true; path=std::string(dir)+"/queue.bin"; if(!create) return;
  fd=open(path.c_str(),O_RDWR|O_CREAT|O_EXCL,0600); if(fd<0) return;
  if(shortFile){(void)ftruncate(fd,8);return;}
  if(ftruncate(fd,sizeof(ParserShm::CastQueue))!=0) return;
  map=mmap(nullptr,sizeof(ParserShm::CastQueue),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
  if(map==MAP_FAILED) return;
  queue=new(map) ParserShm::CastQueue(); queue->_pid=1; queue->_readable=0;
  for(size_t i=0;i<128;i++) {
   auto& item=queue->_items[i]; item._type=0;
   new (&item._tick) WTSTickStruct(); std::strcpy(item._tick.exchg,"EX"); std::strcpy(item._tick.code,"CODE");
   item._tick.trading_date=20261103;item._tick.action_date=20261103;
  }
 }
 ~ShmFile(){ if(map!=MAP_FAILED) munmap(map,sizeof(ParserShm::CastQueue)); if(fd>=0) close(fd); if(!path.empty()) unlink(path.c_str()); if(ownDir) rmdir(dir); }
 bool publish(uint64_t index) {
  const off_t offset=offsetof(ParserShm::CastQueue,_readable);
  return pwrite(fd,&index,sizeof(index),offset)==sizeof(index);
 }
};
struct Cfg {
 WTSVariant* p=WTSVariant::createObject();
 explicit Cfg(const std::string& path) {p->append("path",path.c_str());p->append("checkspan",uint32_t(1000));}
 ~Cfg(){p->release();}
};
struct MockBase : IBaseDataMgr {
 WTSContractInfo* ct=WTSContractInfo::create("CODE","code","EX","P");
 WTSCommodityInfo* commodity=WTSCommodityInfo::create("P","product","EX","TRADING","CHINA");
 std::atomic<int> lookups{0};
 MockBase(){ct->setTotalIndex(0);ct->setAltCode("CODE");ct->setCommInfo(commodity);}
 ~MockBase(){ct->release();commodity->release();}
 WTSCommodityInfo* getCommodity(const char*) override{return nullptr;}
 WTSCommodityInfo* getCommodity(const char*,const char*) override{return nullptr;}
 WTSContractInfo* getContract(const char* code,const char* exchange="",uint32_t=0) override {++lookups;return !std::strcmp(code,"CODE")&&!std::strcmp(exchange,"EX")?ct:nullptr;}
 WTSArray* getContracts(const char* ="",uint32_t=0) override{auto* result=WTSArray::create();result->append(ct,true);return result;}
 WTSSessionInfo* getSession(const char*) override{return nullptr;}
 WTSSessionInfo* getSessionByCode(const char*,const char* ="") override{return nullptr;}
 WTSArray* getAllSessions() override{return nullptr;}
 bool isHoliday(const char*,uint32_t,bool=false) override{return false;}
 uint32_t calcTradingDate(const char*,uint32_t,uint32_t,bool=false) override{return 0;}
 uint64_t getBoundaryTime(const char*,uint32_t,bool=false,bool=true) override{return 0;}
 uint32_t getGlobalSize() override{return 1;}
};
WtParserLifecycleV2Status waitResult(const WtParserLifecycleApiV2* table,IParserApi* api) {
 auto deadline=std::chrono::steady_clock::now()+2s; WtParserLifecycleV2Status status;
 do {status=table->wait_drained(api,0);if(status!=WT_PARSER_LIFECYCLE_V2_PENDING)return status;std::this_thread::sleep_for(2ms);} while(std::chrono::steady_clock::now()<deadline);
 return status;
}
struct Owner {
 Module& module; const WtParserLifecycleApiV2* table; IParserApi* api;
 explicit Owner(Module& m):module(m),table(m.table),api(m.create?m.create():nullptr){if(api)++module.live;}
 bool close(){if(!api)return true;if(table->request_stop(api)!=WT_PARSER_LIFECYCLE_V2_OK)return false;
  if(waitResult(table,api)!=WT_PARSER_LIFECYCLE_V2_DRAINED)return false;
  void* p=api;if(table->destroy_api(&p)!=WT_PARSER_LIFECYCLE_V2_OK||p)return false;
  api=nullptr;--module.live;return true;}
 ~Owner(){(void)close();}
};
struct Sink : IParserSpi {
 IBaseDataMgr* mgr=nullptr; const WtParserLifecycleApiV2* table=nullptr; IParserApi* api=nullptr;
 std::mutex mutex;std::condition_variable cv;bool connected=false,quoted=false,hold=false,resume=false,stopOnQuote=false,stopOnConnect=false,legacyReleaseOnConnect=false;
 std::atomic<int> stopStatus{999},canWaitStatus{999};
 void stop(){stopStatus=table->request_stop(api);canWaitStatus=table->can_wait(api);}
 void handleEvent(WTSParserEvent event,int32_t) override {
  if(event!=WPE_Connect)return; if(stopOnConnect)stop(); if(legacyReleaseOnConnect)api->release();
  std::unique_lock<std::mutex> lock(mutex);connected=true;cv.notify_all();if(hold && (stopOnConnect || legacyReleaseOnConnect))cv.wait(lock,[this]{return resume;});
 }
 void handleSymbolList(const WTSArray*) override {}
 void handleQuote(WTSTickData*,uint32_t) override {
  if(stopOnQuote)stop(); std::unique_lock<std::mutex> lock(mutex);quoted=true;cv.notify_all();if(hold && stopOnQuote)cv.wait(lock,[this]{return resume;});
 }
 void handleParserLog(WTSLogLevel,const char*) override{}
 IBaseDataMgr* getBaseDataMgr() override{return mgr;}
 bool waitConnected(){std::unique_lock<std::mutex> lock(mutex);return cv.wait_for(lock,2s,[this]{return connected;});}
 bool waitQuote(){std::unique_lock<std::mutex> lock(mutex);return cv.wait_for(lock,5ms,[this]{return quoted;});}
 void resumeCallback(){std::lock_guard<std::mutex> lock(mutex);resume=true;cv.notify_all();}
};
struct BlockingLogSink : IParserSpi {
 IBaseDataMgr* mgr=nullptr; const WtParserLifecycleApiV2* table=nullptr; IParserApi* api=nullptr;
 std::mutex mutex;std::condition_variable cv;bool entered=false,resume=false;
 std::atomic<int> canWait{999};
 void handleSymbolList(const WTSArray*) override{}
 void handleQuote(WTSTickData*,uint32_t) override{}
 void handleParserLog(WTSLogLevel,const char*) override {
  canWait=table->can_wait(api);std::unique_lock<std::mutex> lock(mutex);entered=true;cv.notify_all();cv.wait(lock,[this]{return resume;});
 }
 IBaseDataMgr* getBaseDataMgr() override{return mgr;}
};
struct AdapterStub : IParserStub {
 ParserAdapter* adapter=nullptr;std::mutex mutex;std::condition_variable cv;bool quoted=false;
 std::atomic<bool> stopped{false};
 void handle_push_quote(WTSTickData*) override {stopped=adapter->requestStopLifecycle();std::lock_guard<std::mutex> lock(mutex);quoted=true;cv.notify_all();}
 bool waitQuote(){std::unique_lock<std::mutex> lock(mutex);return cv.wait_for(lock,5ms,[this]{return quoted;});}
};
void logInit(){static bool once=[] {WTSLogger::init(R"({"root":{"level":"debug","async":false,"sinks":[]}})",false,nullptr);return true;}();(void)once;}
}
TEST(ParserShmV2, PartialInitAndRepeatedInit)
{
 Module m;ASSERT_NE(nullptr,m.table);EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_OK,m.table->preflight_factory());
 Owner o(m);ASSERT_NE(nullptr,o.api);EXPECT_FALSE(o.api->init(nullptr));
 ShmFile f(false);ASSERT_FALSE(f.path.empty());Cfg cfg(f.path);ASSERT_TRUE(o.api->init(cfg.p));EXPECT_FALSE(o.api->init(cfg.p));
 EXPECT_TRUE(o.close());
}
TEST(ParserShmV2, MissingFileStopIsPromptAndDrains)
{
 Module m;ASSERT_NE(nullptr,m.table);ShmFile f(false);ASSERT_FALSE(f.path.empty());MockBase bd;Sink sink;sink.mgr=&bd;Owner o(m);
 ASSERT_NE(nullptr,o.api);o.api->registerSpi(&sink);Cfg cfg(f.path);ASSERT_TRUE(o.api->init(cfg.p));ASSERT_TRUE(o.api->connect());
 auto start=std::chrono::steady_clock::now();EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_OK,m.table->request_stop(o.api));
 EXPECT_LT(std::chrono::steady_clock::now()-start,100ms);EXPECT_TRUE(o.close());
}
TEST(ParserShmV2, RealMappedFileCallbackStopAndPending)
{
 Module m;ASSERT_NE(nullptr,m.table);ShmFile f;ASSERT_NE(nullptr,f.queue);MockBase bd;Sink sink;sink.mgr=&bd;Owner o(m);ASSERT_NE(nullptr,o.api);
 sink.api=o.api;sink.table=m.table;sink.stopOnConnect=true;sink.hold=true;o.api->registerSpi(&sink);Cfg cfg(f.path);ASSERT_TRUE(o.api->init(cfg.p));ASSERT_TRUE(o.api->connect());
 bool entered=sink.waitConnected();
 if(entered) {EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_OK,sink.stopStatus.load());EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK,sink.canWaitStatus.load());
  EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_PENDING,m.table->wait_drained(o.api,0));}
 sink.resumeCallback();EXPECT_TRUE(entered);EXPECT_TRUE(o.close());
}
TEST(ParserShmV2, LegacyReleaseFromCallbackDefersJoin)
{
 Module m;ASSERT_NE(nullptr,m.table);ShmFile f;ASSERT_NE(nullptr,f.queue);MockBase bd;Sink sink;sink.mgr=&bd;Owner o(m);ASSERT_NE(nullptr,o.api);
 sink.api=o.api;sink.table=m.table;sink.legacyReleaseOnConnect=true;sink.hold=true;o.api->registerSpi(&sink);Cfg cfg(f.path);ASSERT_TRUE(o.api->init(cfg.p));ASSERT_TRUE(o.api->connect());
 bool entered=sink.waitConnected();
 if(entered){EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_OK,m.table->request_stop(o.api));EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_PENDING,m.table->wait_drained(o.api,0));}
 sink.resumeCallback();EXPECT_TRUE(entered);EXPECT_TRUE(o.close());
}
TEST(ParserShmV2, RealMappedTickAndStopDuringQuote)
{
 Module m;ASSERT_NE(nullptr,m.table);ShmFile f;ASSERT_NE(nullptr,f.queue);MockBase bd;Sink sink;sink.mgr=&bd;Owner o(m);ASSERT_NE(nullptr,o.api);
 sink.api=o.api;sink.table=m.table;sink.stopOnQuote=true;sink.hold=true;o.api->registerSpi(&sink);Cfg cfg(f.path);ASSERT_TRUE(o.api->init(cfg.p));
 CodeSet symbols;symbols.insert("EX.CODE");o.api->subscribe(symbols);ASSERT_TRUE(o.api->connect());ASSERT_TRUE(sink.waitConnected());
 bool quoted=false;
 for(uint64_t index=1;index<128 && !quoted;index++){ASSERT_TRUE(f.publish(index));quoted=sink.waitQuote();}
 if(quoted){EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_OK,sink.stopStatus.load());EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK,sink.canWaitStatus.load());
  EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_PENDING,m.table->wait_drained(o.api,0));}
 sink.resumeCallback();EXPECT_TRUE(quoted) << "lookups=" << bd.lookups.load() << " readable=" << f.queue->_readable << " connected=" << o.api->isConnected();EXPECT_TRUE(o.close());
}
TEST(ParserShmV2, HostCallbackBlocksDrainButNotStopRequest)
{
 Module m;ASSERT_NE(nullptr,m.table);MockBase bd;Sink first;first.mgr=&bd;BlockingLogSink second;second.mgr=&bd;Owner o(m);ASSERT_NE(nullptr,o.api);
 second.table=m.table;second.api=o.api;o.api->registerSpi(&first);
 std::thread host([&]{o.api->registerSpi(&second);});bool entered;
 {std::unique_lock<std::mutex> lock(second.mutex);entered=second.cv.wait_for(lock,2s,[&]{return second.entered;});}
 if(entered){auto start=std::chrono::steady_clock::now();EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_OK,m.table->request_stop(o.api));
  EXPECT_LT(std::chrono::steady_clock::now()-start,100ms);EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK,second.canWait.load());
  EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_PENDING,m.table->wait_drained(o.api,0));}
 {std::lock_guard<std::mutex> lock(second.mutex);second.resume=true;second.cv.notify_all();}
 host.join();EXPECT_TRUE(entered);EXPECT_TRUE(o.close());
}
TEST(ParserShmV2, MultipleMappedInstancesStopIndependently)
{
 Module m;ASSERT_NE(nullptr,m.table);ShmFile f1,f2;ASSERT_NE(nullptr,f1.queue);ASSERT_NE(nullptr,f2.queue);MockBase bd;Sink s1,s2;s1.mgr=&bd;s2.mgr=&bd;
 Owner one(m),two(m);ASSERT_NE(nullptr,one.api);ASSERT_NE(nullptr,two.api);one.api->registerSpi(&s1);two.api->registerSpi(&s2);Cfg c1(f1.path),c2(f2.path);
 ASSERT_TRUE(one.api->init(c1.p));ASSERT_TRUE(two.api->init(c2.p));ASSERT_TRUE(one.api->connect());ASSERT_TRUE(two.api->connect());
 EXPECT_TRUE(s1.waitConnected());EXPECT_TRUE(s2.waitConnected());EXPECT_TRUE(one.close());EXPECT_TRUE(two.api->isConnected());EXPECT_TRUE(two.close());
}
TEST(ParserShmV2, WorkerCreationFailureDrains)
{
 Module m("./fault/parsers/libParserShm.dylib");ASSERT_NE(nullptr,m.table);
 auto fail=reinterpret_cast<void(*)()>(dlsym(m.handle,"parserShmTestFailNextWorkerStart"));ASSERT_NE(nullptr,fail);
 ShmFile f(false);MockBase bd;Sink sink;sink.mgr=&bd;Owner o(m);ASSERT_NE(nullptr,o.api);o.api->registerSpi(&sink);Cfg cfg(f.path);ASSERT_TRUE(o.api->init(cfg.p));
 fail();EXPECT_FALSE(o.api->connect());EXPECT_TRUE(o.close());
}
TEST(ParserShmV2, InvalidMappingErrorRetainsHandle)
{
 Module m("./fault/parsers/libParserShm.dylib");ASSERT_NE(nullptr,m.table);
 auto failed=reinterpret_cast<bool(*)(IParserApi*)>(dlsym(m.handle,"parserShmTestWorkerFailed"));
 auto shutdown=reinterpret_cast<bool(*)(IParserApi*)>(dlsym(m.handle,"parserShmTestLegacyShutdown"));ASSERT_NE(nullptr,failed);ASSERT_NE(nullptr,shutdown);
 ShmFile f(true,true);ASSERT_GE(f.fd,0);MockBase bd;Sink sink;sink.mgr=&bd;Owner o(m);ASSERT_NE(nullptr,o.api);o.api->registerSpi(&sink);Cfg cfg(f.path);
 ASSERT_TRUE(o.api->init(cfg.p));ASSERT_TRUE(o.api->connect());auto deadline=std::chrono::steady_clock::now()+2s;
 while(!failed(o.api)&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(2ms);
 ASSERT_TRUE(failed(o.api));ASSERT_TRUE(shutdown(o.api)); // test-only independent join proof
 EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_OK,m.table->request_stop(o.api));EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_ERROR,m.table->wait_drained(o.api,0));
 void* handle=o.api;EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_ERROR,m.table->destroy_api(&handle));EXPECT_EQ(o.api,handle);
 delete o.api;o.api=nullptr;--m.live;
}
TEST(ParserShmV2, ActualParserAdapterTwoPhase)
{
 logInit();ShmFile f;ASSERT_NE(nullptr,f.queue);auto* bd=new MockBase;auto* manager=new ParserAdapterMgr;auto* stub=new AdapterStub;
 auto adapter=std::make_shared<ParserAdapter>();stub->adapter=adapter.get();Cfg cfg(f.path);cfg.p->append("module","ParserShm");
 ASSERT_TRUE(adapter->init("shm",cfg.p,stub,bd));ASSERT_TRUE(manager->addAdapter("shm",adapter));ASSERT_TRUE(manager->runWithResult());
 bool quoted=false;for(uint64_t index=1;index<128 && !quoted;index++){ASSERT_TRUE(f.publish(index));quoted=stub->waitQuote();}
 EXPECT_TRUE(quoted);if(quoted) EXPECT_TRUE(stub->stopped.load());ASSERT_TRUE(manager->requestStopLifecycleAll());
 auto deadline=std::chrono::steady_clock::now()+2s;while(!manager->finishReleaseLifecycleAll()&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(2ms);
 if(manager->hasQuarantinedAdapters()){FAIL()<<"adapter/manager intentionally retained";}
 delete manager;delete bd;delete stub;
}
