#include "../../../WtCore/SelStrategyMgr.h"
#include <dlfcn.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <system_error>
#include <iterator>
#include <string>
#include <thread>
#include <type_traits>

static_assert(std::is_standard_layout<SelStraWrapper>::value, "legacy wrapper stays standard layout");
static_assert(sizeof(SelStraWrapper)==sizeof(SelStrategy*)+sizeof(ISelStrategyFact*), "legacy wrapper stays two pointers");

namespace {
int failures = 0;
void check(bool b, const char* m) { if (!b) { std::fprintf(stderr, "FAIL %s\n", m); ++failures; } }
using IntFn = int (*)();
using VoidFn = void (*)();
using SetFn = void (*)(void (*)());
SelStrategyMgr* reentrant_manager = nullptr;
std::string reentrant_dir;
bool inner_result = true;
void reenter() { inner_result = reentrant_manager->loadFactories(reentrant_dir.c_str()); }
std::string readMarker(const char* path) { std::ifstream in(path); return {std::istreambuf_iterator<char>(in), {}}; }
size_t countText(const std::string& text, const std::string& part) {
 size_t n=0, pos=0; while ((pos=text.find(part,pos))!=std::string::npos) { ++n; pos+=part.size(); } return n;
}
}
int main(int argc, char** argv) {
 if (argc != 4 && argc != 6) return 90;
 std::string mode=argv[1], dir=argv[2], path=argv[3];
 if (mode == "long-directory") {
  const std::string longPath=dir+"/"+std::string(4096,'x');
  std::error_code expected;
  (void)std::filesystem::is_directory(std::filesystem::path(longPath), expected);
  check(static_cast<bool>(expected),"full path produces an error");
  SelStrategyMgr manager;
  check(!manager.loadFactories(longPath.c_str()),"inaccessible long path rejected");
  const char* logPath=std::getenv("WT_SEL_LOG_MARKER");
  const std::string log=logPath ? readMarker(logPath) : std::string();
  check(log.find(longPath.substr(0,128)+"...")!=std::string::npos,"path diagnostic bounded");
  check(log.find(longPath)==std::string::npos,"full path absent from log");
  check(log.find(expected.message())!=std::string::npos,"filesystem used full path's error");
  std::printf("%s: %s\n",mode.c_str(),failures?"FAIL":"PASS"); return failures?1:0;
 }
 if (mode == "owner-no-preload" || mode == "owner-final-removal") {
  const char* markerPath=std::getenv("WT_SEL_MARKER");
  check(markerPath!=nullptr,"marker path available");
  SelStrategyPtr first, second, nullStrategy;
  std::weak_ptr<SelStraWrapper> weak;
  {
   SelStrategyMgr manager;
   check(manager.loadFactories(dir.c_str()),"factory loads without control handle");
   nullStrategy=manager.createStrategy("stable","missing","null");
   check(!nullStrategy && !manager.getStrategy("null"),"null strategy is not published");
   first=manager.createStrategy("stable.unit","null");
   check(first && first->self(),"same ID can be retried after null strategy");
   second=manager.createStrategy("stable","unit","second");
   check(first && first->self() && second && second->self(),"both creation overloads work");
   weak=second;
  }
  if (markerPath) {
   const auto before=readMarker(markerPath);
   check(before.find("factory-delete\n")==std::string::npos,"factory outlives manager");
   check(before.find("unload\n")==std::string::npos,"module outlives manager");
  }
  if (first && first->self()) check(std::strcmp(first->self()->getName(),"unit")==0,"strategy still callable");
  first.reset(); second.reset();
  if (markerPath) check(countText(readMarker(markerPath),"factory-delete\n")==0,"existing factory remains retained");
  nullStrategy.reset();
  if (markerPath) {
   const auto after=readMarker(markerPath);
   check(countText(after,"strategy-delete\n")==2,"strategies deleted exactly once");
   if (mode == "owner-final-removal") {
    check(countText(after,"factory-delete\n")==1,"factory deleted exactly once before weak pointer release");
    check(after.rfind("strategy-delete\n")<after.find("factory-delete\n"),"factory deletion follows strategy deletion");
   } else {
    check(countText(after,"factory-delete\n")==0,"existing factory remains retained after strategy release");
   }
  }
  check(weak.expired(),"weak pointer expires");
  weak.reset();
  std::printf("%s: %s\n",mode.c_str(),failures?"FAIL":"PASS"); return failures?1:0;
 }
 void* control=dlopen(path.c_str(), RTLD_NOW); check(control != nullptr, "preload fixture"); if (!control) return 91;
 if (mode == "missing-remover") {
  const char* markerPath=std::getenv("WT_SEL_MARKER");
  check(markerPath != nullptr,"marker path available");
  auto creates=(IntFn)dlsym(control,"fixture_creations");
  check(creates != nullptr,"creator counter available");
  SelStrategyMgr manager;
  // Card 4b=A: a versioned SEL table without destroy_factory is invalid and
  // refused (formerly an ignored module with a missing v1 remover).
  check(!manager.loadFactories(dir.c_str()),"table without destroy_factory refused");
  check(creates && creates()==0,"creator not called without remover");
  dlclose(control);
  check(readMarker(markerPath).find("unload\n")==std::string::npos,"missing remover module remains resident");
  check(!manager.loadFactories(dir.c_str()),"repeated invalid table still refused");
  check(readMarker(markerPath).find("unload\n")==std::string::npos,"repeat did not unload missing remover module");
  std::printf("%s: %s\n",mode.c_str(),failures?"FAIL":"PASS"); return failures?1:0;
 }
 if (mode == "missing-creator") {
  const char* markerPath=std::getenv("WT_SEL_MARKER");
  check(markerPath!=nullptr,"marker path available");
  SelStrategyMgr manager;
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
 auto proceed=(VoidFn)dlsym(control,"fixture_proceed"); auto setcb=(SetFn)dlsym(control,"fixture_set_callback");
 check(creates && names && deletes && entered && proceed && setcb, "fixture exports"); if (failures) return 92;
 {
 SelStrategyMgr manager;
 if (mode == "creator-throw" || mode == "name-throw") {
  bool threw=false; try { manager.loadFactories(dir.c_str()); } catch (const std::exception&) { threw=true; }
  check(threw,"first exception propagates"); check(!manager.createStrategy("stable","unit","x"),"failure did not publish");
  check(!manager.loadFactories(dir.c_str()),"retry refuses quarantined module");
  check(creates()==1,"creator called once across retry");
  check(names()==(mode=="name-throw"?1:0),"name calls");
 } else if (mode == "empty" || mode == "null") {
  check(manager.loadFactories(dir.c_str()),"invalid factory remains skipped"); check(manager.loadFactories(dir.c_str()),"invalid rescan remains skipped");
  check(creates()==1,"invalid creator called once"); check(!manager.createStrategy("stable","unit","x"),"invalid did not publish");
  const char* logPath=std::getenv("WT_SEL_LOG_MARKER");
  check(logPath && readMarker(logPath).find("previously skipped and retained")!=std::string::npos,"skipped quarantine is observable");
 } else if (mode == "exception-recovery") {
  if (argc != 6) return 93;
  bool threw=false; try { manager.loadFactories(dir.c_str()); } catch (const std::exception&) { threw=true; }
  check(threw,"bad factory throws");
  setenv("WT_SEL_FACTORY_MODE","single-name",1);
  check(manager.loadFactories(argv[4]),"different healthy module loads after failure");
  check(!manager.loadFactories(dir.c_str()),"bad module remains quarantined after healthy load");
  check(creates()==1,"failed creator not repeated");
  auto strategy=manager.createStrategy("stable","unit","x"); check(strategy && strategy->self(),"healthy module creates strategy");
 } else if (mode == "long-name") {
  if (argc != 6) return 96;
  const std::string first=std::string(4096,'x')+"A";
  const std::string second=std::string(4096,'x')+"B";
  setenv("WT_SEL_FACTORY_NAME",first.c_str(),1);
  check(manager.loadFactories(dir.c_str()),"first long factory name loads");
  setenv("WT_SEL_FACTORY_NAME",second.c_str(),1);
  check(manager.loadFactories(argv[4]),"second distinct long factory name loads");
  auto a=manager.createStrategy(first.c_str(),"unit","a");
  auto b=manager.createStrategy(second.c_str(),"unit","b");
  check(a && a->self() && b && b->self(),"full distinct long identities remain usable");
  const char* logPath=std::getenv("WT_SEL_LOG_MARKER");
  check(logPath!=nullptr,"log marker path available");
  if (logPath) {
   const std::string log=readMarker(logPath);
   check(countText(log,std::string(128,'x')+"...")==2,"both log display names are bounded");
   check(log.find(first)==std::string::npos && log.find(second)==std::string::npos,"full long names do not reach logger");
  }
 } else if (mode == "duplicate-different") {
  if (argc != 6) return 94;
  check(manager.loadFactories(dir.c_str()),"first factory loads");
  void* second=dlopen(argv[5],RTLD_NOW); check(second!=nullptr,"second physical module loads"); if (!second) return 95;
  auto secondCreates=(IntFn)dlsym(second,"fixture_creations"); check(secondCreates!=nullptr,"second counter");
  check(manager.loadFactories(argv[4]),"different module with duplicate name skipped");
  check(manager.loadFactories(argv[4]),"different duplicate remains locally skipped");
  if (secondCreates) check(secondCreates()==1,"second duplicate creator not repeated");
  auto strategy=manager.createStrategy("stable","unit","x"); check(strategy && strategy->self(),"original factory retained");
  SelStrategyMgr other;
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
  SelStrategyMgr other;
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
  SelStrategyMgr other; bool first=false;
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
  check(manager.loadFactories(dir.c_str()),"healthy loads"); check(names()==1,"getName call count");
  auto strategy=manager.createStrategy("stable","unit","x"); check(strategy && strategy->self(),"healthy strategy created");
  strategy.reset(); check(deletes()==0,"manager retains healthy strategy");
  if (mode=="long-valid-path") {
   const char* logPath=std::getenv("WT_SEL_LOG_MARKER");
   const std::string log=logPath ? readMarker(logPath) : std::string();
   check(dir.size()>128 && log.find(dir.substr(0,128)+"...")!=std::string::npos,"long valid path diagnostic is bounded");
   check(log.find(dir)==std::string::npos,"long valid path not printed in full");
  }
 }
 }
 if (mode=="idempotent-cross-manager") check(deletes()==2,"two managers delete their strategies");
 if (mode=="duplicate" || mode=="duplicate-different" || mode=="exception-recovery" || mode=="single-name" || mode=="old-positive" || mode=="long-name")
  check(deletes()==(mode=="exception-recovery"?0:1),"first module strategy deletion count");
 dlclose(control);
 std::ifstream marker(std::getenv("WT_SEL_MARKER")); std::string line;
 while (std::getline(marker,line))
  if (mode!="old-positive") check(line!="unload","module remains loaded after manager destruction");
 std::printf("%s: %s\n",mode.c_str(),failures?"FAIL":"PASS"); return failures?1:0;
}
