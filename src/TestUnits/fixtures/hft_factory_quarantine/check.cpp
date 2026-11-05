#include "../../../WtCore/HftStrategyMgr.h"
#include <dlfcn.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <new>
#include <string>
#include <thread>
#include <type_traits>

static_assert(std::is_standard_layout<HftStraWrapper>::value, "legacy wrapper stays standard layout");
static_assert(sizeof(HftStraWrapper)==sizeof(HftStrategy*)+sizeof(IHftStrategyFact*), "legacy wrapper stays two pointers");
static_assert(std::is_constructible<HftStraWrapper,HftStrategy*,IHftStrategyFact*>::value, "legacy constructor remains available");

namespace {
std::atomic<int> failAllocationAfter{-1};
std::atomic<int> allocationsSinceArmed{0};
}
void* operator new(std::size_t size) {
 int remaining=failAllocationAfter.load(std::memory_order_relaxed);
 if (remaining >= 0) allocationsSinceArmed.fetch_add(1, std::memory_order_relaxed);
 if (remaining >= 0 && failAllocationAfter.fetch_sub(1, std::memory_order_relaxed) == 0) {
  failAllocationAfter.store(-1, std::memory_order_relaxed);
  throw std::bad_alloc();
 }
 if (void* p=std::malloc(size ? size : 1)) return p;
 throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {
int failures = 0;
void check(bool b, const char* m) { if (!b) { std::fprintf(stderr, "FAIL %s\n", m); ++failures; } }
using IntFn = int (*)();
using VoidFn = void (*)();
using SetFn = void (*)(void (*)());
HftStrategyMgr* reentrant_manager = nullptr;
std::string reentrant_dir;
bool inner_result = true;
void reenter() { inner_result = reentrant_manager->loadFactories(reentrant_dir.c_str()); }
void failNextAllocation() { allocationsSinceArmed.store(0, std::memory_order_relaxed); failAllocationAfter.store(0, std::memory_order_relaxed); }
void failControlBlockAllocation() { allocationsSinceArmed.store(0, std::memory_order_relaxed); failAllocationAfter.store(1, std::memory_order_relaxed); }
std::string readMarker(const char* path) { std::ifstream in(path); return {std::istreambuf_iterator<char>(in), {}}; }
size_t countText(const std::string& text, const std::string& part) {
 size_t n=0, pos=0; while ((pos=text.find(part,pos))!=std::string::npos) { ++n; pos+=part.size(); } return n;
}
}
int main(int argc, char** argv) {
 if (argc != 4 && argc != 6) return 90;
 std::string mode=argv[1], dir=argv[2], path=argv[3];
 if (mode == "long-invalid-path") {
  const std::string invalid=dir+"/"+std::string(4096,'x');
  HftStrategyMgr manager;
  check(!manager.loadFactories(invalid.c_str()),"overlong invalid path fails");
  const char* logPath=std::getenv("WT_HFT_LOG_MARKER");
  check(logPath!=nullptr,"log marker path available");
  if (logPath) {
   const std::string log=readMarker(logPath);
   check(log.find("...")!=std::string::npos,"invalid path display was bounded");
   check(log.find(invalid)==std::string::npos,"full invalid path did not reach fixed buffer");
   check(log.size()<1024,"invalid path diagnostic remains bounded");
  }
  std::printf("%s: %s\n",mode.c_str(),failures?"FAIL":"PASS"); return failures?1:0;
 }
 // Do not preload the fixture: only the manager's loader reference can keep
 // the code backing the externally-held strategy and its factory available.
 if (mode == "owner-no-preload") {
  const char* markerPath=std::getenv("WT_HFT_MARKER");
  check(markerPath!=nullptr,"marker path available");
  HftStrategyPtr strategy;
  {
   HftStrategyMgr manager;
   check(manager.loadFactories(dir.c_str()),"factory loads without control handle");
   strategy=manager.createStrategy("stable.unit","external");
   check(strategy && strategy->self(),"qualified-name overload creates strategy");
  }
  if (markerPath) {
   const std::string before=readMarker(markerPath);
   check(before.find("factory-delete\n")==std::string::npos,"factory outlives manager");
   check(before.find("unload\n")==std::string::npos,"module outlives manager");
  }
  if (strategy && strategy->self())
   check(std::strcmp(strategy->self()->getName(),"unit")==0,"strategy remains callable");
  strategy.reset();
  if (markerPath) {
   const std::string after=readMarker(markerPath);
   check(countText(after,"strategy-delete\n")==1,"strategy deleted once");
   check(countText(after,"factory-delete\n")==1,"factory deleted once");
   check(after.find("strategy-delete\n")<after.find("factory-delete\n"),"strategy deletion precedes factory deletion");
  }
  std::printf("%s: %s\n",mode.c_str(),failures?"FAIL":"PASS"); return failures?1:0;
 }
 void* control=dlopen(path.c_str(), RTLD_NOW); check(control != nullptr, "preload fixture"); if (!control) return 91;
 if (mode == "missing-creator") {
  const char* markerPath=std::getenv("WT_HFT_MARKER");
  check(markerPath!=nullptr,"marker path available");
  HftStrategyMgr manager;
  check(manager.loadFactories(dir.c_str()),"missing creator remains an ignored module");
  dlclose(control);
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  const std::string first=readMarker(markerPath);
  check(first.find("unload\n")==std::string::npos,"module not unloaded after control ref closes");
  check(manager.loadFactories(dir.c_str()),"repeat scan still ignores missing creator");
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  const std::string second=readMarker(markerPath);
  check(countText(second,"load\n")==1,"module initialized once across repeated scans");
  check(second.find("unload\n")==std::string::npos,"repeat scan did not unload module");
  check(countText(second,"tick\n")>countText(first,"tick\n"),"module background activity remains alive");
  std::printf("%s: %s\n",mode.c_str(),failures?"FAIL":"PASS"); return failures?1:0;
 }
 auto creates=(IntFn)dlsym(control,"fixture_creations"); auto names=(IntFn)dlsym(control,"fixture_names");
 auto deletes=(IntFn)dlsym(control,"fixture_deletes"); auto entered=(IntFn)dlsym(control,"fixture_entered");
 auto factoryDeletes=(IntFn)dlsym(control,"fixture_factory_deletes");
 auto strategyCalls=(IntFn)dlsym(control,"fixture_strategy_calls");
 auto proceed=(VoidFn)dlsym(control,"fixture_proceed"); auto setcb=(SetFn)dlsym(control,"fixture_set_callback");
 auto setCreatedCb=(SetFn)dlsym(control,"fixture_set_created_callback");
 check(creates && names && deletes && factoryDeletes && strategyCalls && entered && proceed && setcb && setCreatedCb, "fixture exports"); if (failures) return 92;
 if (mode == "owner-outlives-manager" || mode == "owner-null-strategy" || mode == "owner-without-remover") {
  HftStrategyPtr first, second;
  std::weak_ptr<HftStraWrapper> weak;
  {
   HftStrategyMgr manager;
   check(manager.loadFactories(dir.c_str()),"factory loads");
   first=manager.createStrategy("stable", mode=="owner-null-strategy" ? "missing" : "unit", "first");
   if (mode=="owner-null-strategy") {
    check(!first && !manager.getStrategy("first"),"null strategy is not published");
    first=manager.createStrategy("stable","unit","first");
    check(first && first->self(),"same ID can be retried after null strategy");
   } else {
    check(first && first->self(),"expected wrapper and strategy");
   }
   if (mode=="owner-outlives-manager") {
    second=manager.createStrategy("stable","unit","second");
    check(second && second->self(),"second wrapper is valid");
    weak=second;
   }
  }
  check(factoryDeletes()==0,"manager destruction retains factory for external wrapper");
  first.reset();
  if (mode=="owner-outlives-manager") {
   check(deletes()==1 && factoryDeletes()==0,"first wrapper releases before final factory owner");
   second.reset();
   check(deletes()==2 && factoryDeletes()==1,"last wrapper is deleted before factory remover");
   check(weak.expired(),"weak wrapper expires without retaining factory");
  } else if (mode=="owner-null-strategy") {
   check(deletes()==1 && factoryDeletes()==1,"retried strategy releases before factory removal");
  } else {
   check(deletes()==1 && factoryDeletes()==0,"missing remover keeps factory while deleting strategy");
  }
  const char* markerPath=std::getenv("WT_HFT_MARKER");
  check(markerPath!=nullptr,"marker path available");
  if (markerPath && mode=="owner-outlives-manager") {
   const std::string marker=readMarker(markerPath);
   check(countText(marker,"strategy-delete\n")==2 && countText(marker,"factory-delete\n")==1,"exact deletion counts");
   check(marker.rfind("strategy-delete\n")<marker.find("factory-delete\n"),"factory remover follows last strategy delete");
  }
  weak.reset();
  dlclose(control);
  std::printf("%s: %s\n",mode.c_str(),failures?"FAIL":"PASS"); return failures?1:0;
 }
 if (mode == "legacy-raw-wrapper") {
  auto creator=(FuncCreateHftStraFact)dlsym(control,"createStrategyFact");
  auto remover=(FuncDeleteHftStraFact)dlsym(control,"deleteStrategyFact");
  check(creator && remover,"legacy factory exports");
  if (!creator || !remover) return 99;
  IHftStrategyFact* legacy=creator();
  { HftStraWrapper wrapper(legacy->createStrategy("unit","legacy"), legacy); check(wrapper.self()!=nullptr,"public raw constructor"); }
  check(deletes()==1 && factoryDeletes()==0,"raw wrapper deletes strategy only");
  remover(legacy);
  check(factoryDeletes()==1,"raw factory remains caller-owned");
  dlclose(control);
  std::printf("%s: %s\n",mode.c_str(),failures?"FAIL":"PASS"); return failures?1:0;
 }
 void* secondLifetimeHandle=nullptr;
 IntFn secondFactoryDeletes=nullptr;
 {
 HftStrategyMgr manager;
 if (mode == "creator-throw" || mode == "name-throw") {
  bool threw=false; try { manager.loadFactories(dir.c_str()); } catch (const std::exception&) { threw=true; }
  check(threw,"first exception propagates"); check(!manager.createStrategy("stable","unit","x"),"failure did not publish");
  check(!manager.loadFactories(dir.c_str()),"retry refuses quarantined module");
  check(creates()==1,"creator called once across retry");
  check(names()==(mode=="name-throw"?1:0),"name calls");
 } else if (mode == "empty" || mode == "null") {
  check(!manager.loadFactories(dir.c_str()),"invalid factory refused"); check(!manager.loadFactories(dir.c_str()),"retry refused");
  check(creates()==1,"invalid creator called once"); check(!manager.createStrategy("stable","unit","x"),"invalid did not publish");
 } else if (mode == "exception-recovery") {
  if (argc != 6) return 93;
  bool threw=false; try { manager.loadFactories(dir.c_str()); } catch (const std::exception&) { threw=true; }
  check(threw,"bad factory throws");
  setenv("WT_HFT_FACTORY_MODE","single-name",1);
  check(manager.loadFactories(argv[4]),"different healthy module loads after failure");
  check(!manager.loadFactories(dir.c_str()),"bad module remains quarantined after healthy load");
  check(creates()==1,"failed creator not repeated");
  auto strategy=manager.createStrategy("stable","unit","x"); check(strategy && strategy->self(),"healthy module creates strategy");
 } else if (mode == "long-name") {
  if (argc != 6) return 96;
  const std::string first=std::string(4096,'x')+"A";
  const std::string second=std::string(4096,'x')+"B";
  setenv("WT_HFT_FACTORY_NAME",first.c_str(),1);
  check(manager.loadFactories(dir.c_str()),"first long factory name loads");
  setenv("WT_HFT_FACTORY_NAME",second.c_str(),1);
  check(manager.loadFactories(argv[4]),"second distinct long factory name loads");
  auto a=manager.createStrategy(first.c_str(),"unit","a");
  auto b=manager.createStrategy(second.c_str(),"unit","b");
  check(a && a->self() && b && b->self(),"full distinct long identities remain usable");
  const char* logPath=std::getenv("WT_HFT_LOG_MARKER");
  check(logPath!=nullptr,"log marker path available");
  if (logPath) {
   const std::string log=readMarker(logPath);
   check(countText(log,std::string(128,'x')+"...")==2,"both log display names are bounded");
   check(log.find(first)==std::string::npos && log.find(second)==std::string::npos,"full long names do not reach logger");
  }
 } else if (mode == "allocation-wrapper-failure" || mode == "allocation-control-failure") {
  check(manager.loadFactories(dir.c_str()),"allocation-failure factory loads");
  setCreatedCb(mode=="allocation-wrapper-failure" ? failNextAllocation : failControlBlockAllocation);
  bool threw=false;
  try { (void)manager.createStrategy("stable","unit","failed"); }
  catch (const std::bad_alloc&) { threw=true; }
  failAllocationAfter.store(-1, std::memory_order_relaxed);
  setCreatedCb(nullptr);
  check(threw,"expected allocation failure propagated");
  const int expectedBoundary=mode=="allocation-wrapper-failure" ? 1 : 2;
  check(allocationsSinceArmed.load(std::memory_order_relaxed)==expectedBoundary,"expected allocation boundary reached");
  check(deletes()==1 && factoryDeletes()==0,"failed wrapper construction deletes strategy exactly once and keeps factory");
  check(!manager.getStrategy("failed"),"failed creation is not published");
  // BUG-I02-NEW-033 (2026-11-05): the ID reservation is rolled back, so the same ID can be retried.
  auto sameId=manager.createStrategy("stable","unit","failed");
  check(sameId && sameId->self(),"same ID can be retried after allocation failure (reservation rolled back)");
  auto retry=manager.createStrategy("stable","unit","retry");
  check(retry && retry->self(),"manager remains usable after allocation failure");
 } else if (mode == "allocation-registry-failure") {
  // BUG-I02-NEW-033 (2026-11-05): createStrategy now reserves the strategy ID in
  // the registry before the external creator runs, so the registry insertion is
  // the first allocation of the call. The old injection (third allocation after
  // the creator) no longer reached the registry; arm before the call instead.
  check(manager.loadFactories(dir.c_str()),"allocation-failure factory loads");
  const int deletesBefore=deletes();
  const int callsBefore=strategyCalls();
  failNextAllocation();
  bool threw=false;
  try { (void)manager.createStrategy("stable","unit","failed"); }
  catch (const std::bad_alloc&) { threw=true; }
  failAllocationAfter.store(-1, std::memory_order_relaxed);
  check(threw,"expected registry allocation failure propagated");
  check(allocationsSinceArmed.load(std::memory_order_relaxed)==1,"registry insertion was the injected allocation");
  check(strategyCalls()==callsBefore,"creator is not called when the ID cannot be reserved");
  check(deletes()==deletesBefore && factoryDeletes()==0,"no strategy to delete and factory kept");
  check(!manager.getStrategy("failed"),"failed creation is not published");
  auto sameId=manager.createStrategy("stable","unit","failed");
  check(sameId && sameId->self(),"same ID can be reserved and created after the registry failure");
  check(strategyCalls()==callsBefore+1 && manager.getStrategy("failed")==sameId,"retry publishes exactly one strategy");
 } else if (mode == "long-path-valid") {
  check(dir.size()>128,"real directory is longer than display bound");
  check(manager.loadFactories(dir.c_str()),"complete real path still loads");
  auto strategy=manager.createStrategy("stable","unit","x");
  check(strategy && strategy->self(),"factory loaded from complete path is usable");
  const char* logPath=std::getenv("WT_HFT_LOG_MARKER");
  check(logPath!=nullptr,"log marker path available");
  if (logPath) {
   const std::string log=readMarker(logPath);
   check(log.find("...")!=std::string::npos,"valid path display was bounded");
   check(log.find(dir)==std::string::npos,"complete path was not sent to fixed buffer");
  }
 } else if (mode == "owner-lifetime") {
  if (argc != 6) return 97;
  check(manager.loadFactories(dir.c_str()),"first owner loads");
  secondLifetimeHandle=dlopen(argv[5],RTLD_NOW);
  check(secondLifetimeHandle!=nullptr,"second module control loads");
  if (!secondLifetimeHandle) return 98;
  secondFactoryDeletes=(IntFn)dlsym(secondLifetimeHandle,"fixture_factory_deletes");
  check(secondFactoryDeletes!=nullptr,"second factory delete counter");
  setenv("WT_HFT_FACTORY_NAME","other",1);
  check(manager.loadFactories(argv[4]),"second distinct owner loads");
  check(factoryDeletes()==0 && secondFactoryDeletes && secondFactoryDeletes()==0,"no factory deleted during publication or growth");
  auto a=manager.createStrategy("stable","unit","a");
  auto b=manager.createStrategy("other","unit","b");
  check(a && a->self() && b && b->self(),"both factories remain callable");
  a.reset(); b.reset();
  check(factoryDeletes()==0 && secondFactoryDeletes && secondFactoryDeletes()==0,"manager retains factories with live strategies");
 } else if (mode == "duplicate-different") {
  if (argc != 6) return 94;
  check(manager.loadFactories(dir.c_str()),"first factory loads");
  void* second=dlopen(argv[5],RTLD_NOW); check(second!=nullptr,"second physical module loads"); if (!second) return 95;
  auto secondCreates=(IntFn)dlsym(second,"fixture_creations"); check(secondCreates!=nullptr,"second counter");
  check(!manager.loadFactories(argv[4]),"different module with duplicate name refused");
  check(!manager.loadFactories(argv[4]),"different duplicate remains locally refused");
  if (secondCreates) check(secondCreates()==1,"second duplicate creator not repeated");
  auto strategy=manager.createStrategy("stable","unit","x"); check(strategy && strategy->self(),"original factory retained");
  HftStrategyMgr other;
  check(other.loadFactories(argv[4]),"other manager can load healthy locally-conflicting module");
  if (secondCreates) check(secondCreates()==2,"other manager creates its own healthy factory");
  auto otherStrategy=other.createStrategy("stable","unit","y");
  check(otherStrategy && otherStrategy->self(),"other manager uses its factory");
  dlclose(second);
 } else if (mode == "duplicate") {
  check(manager.loadFactories(dir.c_str()),"first factory loads");
  check(manager.loadFactories(dir.c_str()),"same manager rescan is idempotent");
  check(manager.loadFactories(dir.c_str()),"repeated rescan remains idempotent");
  check(creates()==1,"same manager rescan did not call creator");
  auto strategy=manager.createStrategy("stable","unit","x"); check(strategy && strategy->self(),"original remains usable");
  strategy.reset(); check(deletes()==0,"manager retains original strategy");
 } else if (mode == "idempotent-cross-manager") {
  check(manager.loadFactories(dir.c_str()),"manager A loads");
  check(manager.loadFactories((dir+"/../plugins").c_str()),"manager A rescans path alias");
  check(creates()==1,"manager A rescan does not create again");
  HftStrategyMgr other;
  check(other.loadFactories(dir.c_str()),"manager B loads same healthy module");
  check(creates()==2,"manager B creates its own factory");
  auto a=manager.createStrategy("stable","unit","a");
  auto b=other.createStrategy("stable","unit","b");
  check(a && a->self() && b && b->self(),"both managers create strategies");
 } else if (mode == "same-manager-concurrent") {
  bool first=false;
  std::thread worker([&] { first=manager.loadFactories(dir.c_str()); });
  for (int i=0;i<5000 && !entered();++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  check(entered()!=0,"first creator entered");
  check(!manager.loadFactories(dir.c_str()),"same manager in-progress rescan safely refuses");
  proceed(); worker.join(); check(first,"first load succeeds");
  check(manager.loadFactories(dir.c_str()),"completed same-manager rescan succeeds");
  check(creates()==1,"same-manager concurrent rescan never creates twice");
 } else if (mode == "concurrent") {
  HftStrategyMgr other; bool first=false;
  std::thread worker([&] { first=manager.loadFactories(dir.c_str()); });
  for (int i=0;i<5000 && !entered();++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  check(entered()!=0,"first creator entered");
  const std::string alias = dir + "/../plugins";
  check(!other.loadFactories(alias.c_str()),"concurrent same handle through path alias refused");
  proceed(); worker.join(); check(first,"first concurrent load succeeds"); check(creates()==1,"concurrent creator called once");
  check(other.loadFactories(dir.c_str()),"healthy module remains loadable after first completes");
  check(creates()==2,"healthy retry creates once");
 } else if (mode == "reentrant") {
  reentrant_manager=&manager; reentrant_dir=dir; setcb(reenter);
  check(manager.loadFactories(dir.c_str()),"outer load succeeds");
  check(!inner_result,"reentrant same handle refused"); check(creates()==1,"reentrant creator called once");
 } else {
  check(manager.loadFactories(dir.c_str()),"healthy loads"); check(names()==(mode=="old-positive"?2:1),"getName call count");
  auto strategy=manager.createStrategy("stable","unit","x"); check(strategy && strategy->self(),"healthy strategy created");
  strategy.reset(); check(deletes()==0,"manager retains healthy strategy");
 }
 }
 if (mode=="owner-lifetime") {
  check(factoryDeletes()==1,"first factory deleted once after manager strategies");
  check(secondFactoryDeletes && secondFactoryDeletes()==1,"second factory deleted once after manager strategies");
  if (secondLifetimeHandle) dlclose(secondLifetimeHandle);
 }
 if (mode=="idempotent-cross-manager") check(deletes()==2,"two managers delete their strategies");
 if (mode=="duplicate" || mode=="duplicate-different" || mode=="exception-recovery" || mode=="single-name" || mode=="old-positive" || mode=="long-name")
  check(deletes()==(mode=="exception-recovery"?0:1),"first module strategy deletion count");
 dlclose(control);
 std::ifstream marker(std::getenv("WT_HFT_MARKER")); std::string line;
 while (std::getline(marker,line)) check(line!="unload","retained module after load");
 std::printf("%s: %s\n",mode.c_str(),failures?"FAIL":"PASS"); return failures?1:0;
}
