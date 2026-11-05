#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <dlfcn.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include "Includes/IParserApi.h"
#include "Includes/IParserLifecycleV2.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSStruct.h"
#include "WTSTools/WTSBaseDataMgr.h"
#include "WTSTools/WTSLogger.h"
#include "WtCore/ParserAdapter.h"
using namespace wtp;
using namespace std::chrono_literals;
namespace {
struct Module {
 explicit Module(const char* path="./parsers/libParserUDP.dylib") : handle(dlopen(path, RTLD_NOW | RTLD_LOCAL)) {}
 void* handle;
 FuncCreateParser create = handle ? reinterpret_cast<FuncCreateParser>(dlsym(handle,"createParser")) : nullptr;
 WtParserLifecycleV2GetterFn get = handle ? reinterpret_cast<WtParserLifecycleV2GetterFn>(dlsym(handle,WT_PARSER_LIFECYCLE_V2_GETTER_SYMBOL)) : nullptr;
 const WtParserLifecycleApiV2* table = get ? get(WT_PARSER_LIFECYCLE_V2_ABI_MAJOR) : nullptr;
 int live=0;
 ~Module() { if(handle && live==0) dlclose(handle); }
};
struct Config {
 WTSVariant* value = WTSVariant::createObject();
 explicit Config(uint16_t port) { value->append("host","127.0.0.1"); value->append("bport",uint32_t(port)); value->append("sport",uint32_t(9)); value->append("gpsize",uint32_t(1000)); }
 ~Config(){ value->release(); }
};
uint16_t freePort() {
 int fd=socket(AF_INET,SOCK_DGRAM,0); if(fd<0) return 0;
 sockaddr_in address{}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 if(bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))!=0) {close(fd);return 0;}
 socklen_t len=sizeof(address); if(getsockname(fd,reinterpret_cast<sockaddr*>(&address),&len)!=0){close(fd);return 0;}
 uint16_t port=ntohs(address.sin_port); close(fd); return port;
}
bool sendTick(uint16_t port) {
 int fd=socket(AF_INET,SOCK_DGRAM,0); if(fd<0) return false;
 WTSTickStruct tick; std::strcpy(tick.exchg,"EX"); std::strcpy(tick.code,"CODE");
 uint32_t kind=0x200; char packet[sizeof(kind)+sizeof(tick)]; std::memcpy(packet,&kind,sizeof(kind)); std::memcpy(packet+sizeof(kind),&tick,sizeof(tick));
 sockaddr_in address{}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK); address.sin_port=htons(port);
 const auto sent=sendto(fd,packet,sizeof(packet),0,reinterpret_cast<sockaddr*>(&address),sizeof(address)); close(fd); return sent==sizeof(packet);
}
WtParserLifecycleV2Status waitResult(const WtParserLifecycleApiV2* table, IParserApi* api) {
 const auto deadline=std::chrono::steady_clock::now()+2s;
 WtParserLifecycleV2Status status;
 do { status=table->wait_drained(static_cast<void*>(api),0); if(status!=WT_PARSER_LIFECYCLE_V2_PENDING) return status; std::this_thread::sleep_for(2ms); } while(std::chrono::steady_clock::now()<deadline);
 return status;
}
struct ApiOwner {
 Module& module; const WtParserLifecycleApiV2* table; IParserApi* api;
 ApiOwner(Module& value) : module(value), table(value.table), api(value.create ? value.create() : nullptr) { if(api) ++module.live; }
 bool stopAndDestroy() {
  if(!api) return true;
  if(table->request_stop(static_cast<void*>(api)) != WT_PARSER_LIFECYCLE_V2_OK) return false;
  if(waitResult(table,api) != WT_PARSER_LIFECYCLE_V2_DRAINED) return false;
  void* opaque=api; if(table->destroy_api(&opaque)!=WT_PARSER_LIFECYCLE_V2_OK || opaque) return false;
  api=nullptr; --module.live; return true;
 }
 ~ApiOwner() { (void)stopAndDestroy(); }
};
struct Sink : IParserSpi {
 const WtParserLifecycleApiV2* table=nullptr; IParserApi* api=nullptr;
 std::mutex mutex; std::condition_variable cv; bool arrived=false, unblock=false, hold=false, stopInCallback=false;
 std::atomic<int> stopStatus{999}, canWaitStatus{999};
 void handleSymbolList(const WTSArray*) override {}
 void handleQuote(WTSTickData*,uint32_t) override {
  if(stopInCallback) { stopStatus=table->request_stop(static_cast<void*>(api)); canWaitStatus=table->can_wait(static_cast<void*>(api)); }
  std::unique_lock<std::mutex> lock(mutex); arrived=true; cv.notify_all();
  if(hold) cv.wait(lock,[this]{return unblock;});
 }
 void handleParserLog(WTSLogLevel,const char*) override {}
 IBaseDataMgr* getBaseDataMgr() override {return nullptr;}
 bool waitArrival() { std::unique_lock<std::mutex> lock(mutex); return cv.wait_for(lock,2s,[this]{return arrived;}); }
 void resume() {std::lock_guard<std::mutex> lock(mutex); unblock=true;cv.notify_all();}
};
struct BlockingLogSink : IParserSpi {
 const WtParserLifecycleApiV2* table=nullptr; IParserApi* api=nullptr;
 std::mutex mutex; std::condition_variable cv; bool entered=false, resume=false;
 std::atomic<int> canWaitStatus{999};
 void handleSymbolList(const WTSArray*) override {}
 void handleQuote(WTSTickData*,uint32_t) override {}
 void handleParserLog(WTSLogLevel,const char*) override {
  canWaitStatus=table->can_wait(api);
  std::unique_lock<std::mutex> lock(mutex); entered=true; cv.notify_all(); cv.wait(lock,[this]{return resume;});
 }
 IBaseDataMgr* getBaseDataMgr() override {return nullptr;}
};
void logInit(){ static bool init=[] {WTSLogger::init(R"({"root":{"level":"debug","async":false,"sinks":[]}})",false,nullptr);return true;}(); (void)init; }
}
TEST(ParserUdpV2, TableAndPartialInit)
{
 Module module; ASSERT_NE(nullptr,module.handle); ASSERT_NE(nullptr,module.table); ASSERT_NE(nullptr,module.create);
 EXPECT_GE(module.table->struct_size,WT_PARSER_LIFECYCLE_V2_REQUIRED_SIZE);
 EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_OK,module.table->preflight_factory());
 Sink sink; ApiOwner owner(module); ASSERT_NE(nullptr,owner.api); owner.api->registerSpi(&sink);
 EXPECT_FALSE(owner.api->init(nullptr));
 ASSERT_EQ(WT_PARSER_LIFECYCLE_V2_OK,module.table->request_stop(owner.api));
 EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_DRAINED,module.table->wait_drained(owner.api,0));
 EXPECT_TRUE(owner.stopAndDestroy());
}
TEST(ParserUdpV2, FailedConnectStillDrains)
{
 Module module("./fault/parsers/libParserUDP.dylib"); ASSERT_NE(nullptr,module.table);
 auto fail=reinterpret_cast<void(*)()>(dlsym(module.handle,"parserUdpTestFailAfterReceiveArm"));
 ASSERT_NE(nullptr,fail); Config cfg(0); ApiOwner owner(module); ASSERT_NE(nullptr,owner.api);
 ASSERT_TRUE(owner.api->init(cfg.value)); fail(); EXPECT_FALSE(owner.api->connect());
 EXPECT_TRUE(owner.stopAndDestroy());
}
TEST(ParserUdpV2, InjectedThreadCreationFailureHasNoAsyncWork)
{
 Module module("./fault/parsers/libParserUDP.dylib"); ASSERT_NE(nullptr,module.table);
 auto fail = reinterpret_cast<void(*)()>(dlsym(module.handle,"parserUdpTestFailNextWorkerStart"));
 ASSERT_NE(nullptr,fail); ApiOwner owner(module); ASSERT_NE(nullptr,owner.api); Config cfg(0);
 ASSERT_TRUE(owner.api->init(cfg.value)); fail(); EXPECT_FALSE(owner.api->connect());
 EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_OK,module.table->request_stop(owner.api));
 EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_DRAINED,module.table->wait_drained(owner.api,0));
 EXPECT_TRUE(owner.stopAndDestroy());
}
TEST(ParserUdpV2, InjectedTimerFailureReturnsErrorAndRetainsHandle)
{
 Module module("./fault/parsers/libParserUDP.dylib"); ASSERT_NE(nullptr,module.table);
 auto fail=reinterpret_cast<void(*)()>(dlsym(module.handle,"parserUdpTestFailNextStopTimer"));
 auto legacyShutdown=reinterpret_cast<bool(*)(IParserApi*)>(dlsym(module.handle,"parserUdpTestLegacyShutdown"));
 ASSERT_NE(nullptr,fail); ASSERT_NE(nullptr,legacyShutdown); ApiOwner owner(module); Config cfg(0);
 ASSERT_NE(nullptr,owner.api); ASSERT_TRUE(owner.api->init(cfg.value)); fail(); EXPECT_FALSE(owner.api->connect());
 // Join the deliberately failed worker before testing the sticky V2 error;
 // this test-only cleanup proof does not generalize to an uncertain provider.
 ASSERT_TRUE(legacyShutdown(owner.api));
 EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_ERROR,module.table->request_stop(owner.api));
 EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_ERROR,module.table->wait_drained(owner.api,0));
 void* opaque=owner.api;
 EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_ERROR,module.table->destroy_api(&opaque)); EXPECT_EQ(owner.api,opaque);
 delete owner.api; owner.api=nullptr; --module.live;
}
TEST(ParserUdpV2, CallbackStopDefersDrainUntilCallbackAndWorkerExit)
{
 Module module; ASSERT_NE(nullptr,module.table); uint16_t port=freePort(); ASSERT_NE(0,port);
 Sink sink; ApiOwner owner(module); ASSERT_NE(nullptr,owner.api);
 sink.table=module.table; sink.api=owner.api; sink.stopInCallback=true; sink.hold=true;
 owner.api->registerSpi(&sink); Config cfg(port); ASSERT_TRUE(owner.api->init(cfg.value)); EXPECT_FALSE(owner.api->init(cfg.value));
 CodeSet symbols; symbols.insert("EX.CODE"); owner.api->subscribe(symbols); ASSERT_TRUE(owner.api->connect());
 ASSERT_TRUE(sendTick(port)); bool arrived=sink.waitArrival();
 if (!arrived) { sink.resume(); EXPECT_TRUE(owner.stopAndDestroy()); FAIL() << "loopback callback timed out"; }
 EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_OK,sink.stopStatus.load());
 EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK,sink.canWaitStatus.load());
 const auto start=std::chrono::steady_clock::now();
 EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_OK,module.table->request_stop(owner.api));
 EXPECT_LT(std::chrono::steady_clock::now()-start,100ms);
 EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_PENDING,module.table->wait_drained(owner.api,0));
 sink.resume(); EXPECT_TRUE(owner.stopAndDestroy());
}
TEST(ParserUdpV2, ConcurrentHostApiCallbackKeepsPollPending)
{
 Module module; ASSERT_NE(nullptr,module.table); Sink original; BlockingLogSink replacement;
 ApiOwner owner(module); ASSERT_NE(nullptr,owner.api); replacement.table=module.table; replacement.api=owner.api;
 owner.api->registerSpi(&original);
 std::thread host([&]{owner.api->registerSpi(&replacement);});
 bool entered;
 { std::unique_lock<std::mutex> lock(replacement.mutex); entered=replacement.cv.wait_for(lock,2s,[&]{return replacement.entered;}); }
 if (entered) {
  const auto start=std::chrono::steady_clock::now();
  EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_OK,module.table->request_stop(owner.api));
  EXPECT_LT(std::chrono::steady_clock::now()-start,100ms);
  EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK,replacement.canWaitStatus.load());
  EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_PENDING,module.table->wait_drained(owner.api,0));
 }
 {std::lock_guard<std::mutex> lock(replacement.mutex); replacement.resume=true; replacement.cv.notify_all();}
 host.join(); EXPECT_TRUE(entered); EXPECT_TRUE(owner.stopAndDestroy());
}
TEST(ParserUdpV2, MultipleInstancesDrainIndependently)
{
 Module module; ASSERT_NE(nullptr,module.table); uint16_t a=freePort(), b=freePort(); ASSERT_NE(0,a); ASSERT_NE(0,b);
 if(a==b) b=freePort(); ASSERT_NE(a,b);
 Sink first, second; ApiOwner one(module),two(module); ASSERT_NE(nullptr,one.api); ASSERT_NE(nullptr,two.api);
 one.api->registerSpi(&first); two.api->registerSpi(&second); Config c1(a),c2(b);
 ASSERT_TRUE(one.api->init(c1.value)); ASSERT_TRUE(two.api->init(c2.value));
 CodeSet symbols; symbols.insert("EX.CODE"); one.api->subscribe(symbols); two.api->subscribe(symbols);
 ASSERT_TRUE(one.api->connect()); ASSERT_TRUE(two.api->connect());
 ASSERT_TRUE(sendTick(a)); ASSERT_TRUE(sendTick(b)); EXPECT_TRUE(first.waitArrival()); EXPECT_TRUE(second.waitArrival());
 EXPECT_TRUE(one.stopAndDestroy()); EXPECT_TRUE(two.api->isConnected()); EXPECT_TRUE(two.stopAndDestroy());
}
TEST(ParserUdpV2, RealParserAdapterTwoPhase)
{
 logInit(); auto* mgr=new WTSBaseDataMgr; auto* manager=new ParserAdapterMgr;
 auto a=std::make_shared<ParserAdapter>(); auto b=std::make_shared<ParserAdapter>();
 Config cfg1(0), cfg2(0); cfg1.value->append("module","ParserUDP"); cfg2.value->append("module","ParserUDP");
 ASSERT_TRUE(a->init("udp1",cfg1.value,nullptr,mgr)); ASSERT_TRUE(b->init("udp2",cfg2.value,nullptr,mgr));
 ASSERT_TRUE(manager->addAdapter("udp1",a)); ASSERT_TRUE(manager->addAdapter("udp2",b)); ASSERT_TRUE(manager->runWithResult());
 ASSERT_TRUE(manager->requestStopLifecycleAll());
 const auto deadline=std::chrono::steady_clock::now()+2s;
 while(!manager->finishReleaseLifecycleAll() && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(2ms);
 if(manager->hasQuarantinedAdapters()) { FAIL() << "adapter remains quarantined; owner graph intentionally retained"; }
 delete manager; delete mgr;
}
