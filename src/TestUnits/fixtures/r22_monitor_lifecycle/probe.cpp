#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "Includes/IDataWriter.h"
#include "Share/StdUtils.hpp"
#include "Share/BoostMappingFile.hpp"
#include "Share/DLLHelper.hpp"
#define private public
#include "WtDtCore/DataManager.h"
#undef private
#include "WtDtCore/StateMonitor.h"
#include "Includes/WTSSessionInfo.hpp"

struct R154StateMonitorTestAccess {
 static void attach(StateMonitor& monitor, DataManager* manager, WTSSessionInfo* session, SimpleState state, const char* id = "TEST") {
  monitor._dt_mgr = manager;
  StatePtr info(new StateInfo);
  std::strcpy(info->_session, id);
  info->_sInfo = session;
  info->_state = state;
  info->_proc_time = 0;
  info->_close_time = 2400;
  info->_sections.push_back(StateInfo::Section{0, 2400});
  monitor._map[id] = info;
 }
 static void advance(StateMonitor& monitor) { monitor.advanceOnce(20261102, 900); }
 static bool joinInProgress(StateMonitor& monitor) {
#ifdef R22_OLD_BASELINE
  // The old monitor has no mutex to observe. This checks only stop admission,
  // so the external-join scenario is a weaker historical control.
  return monitor._stopped.load();
#else
  if (!monitor._stopped.load()) return false;
  if (!monitor._thread_mutex.try_lock()) return true;
  monitor._thread_mutex.unlock();
  return false;
#endif
 }
};

struct CallbackWriter final : IDataWriter {
 explicit CallbackWriter(StateMonitor& monitor) : monitor(monitor) {}
 void release() override {}
 bool writeTick(WTSTickData*, uint32_t) override { return false; }
 WTSTickData* getCurTick(const char*,const char*) override { return nullptr; }
 bool isSessionProceeded(const char*) override { return false; }
 void transHisData(const char*) override {
  ++attempts;
  if (throwOnTrans) throw std::runtime_error("injected writer exception");
  // The state must already be visible and this reentrant query must not be
  // blocked by a monitor lock held across the writer callback.
  sawProcessing.store(monitor.isInState("TEST", SS_PROCING));
  entered.store(true);
  while(block && !proceed.load()) std::this_thread::yield();
  monitor.stop();
  called.store(true);
 }
 StateMonitor& monitor;
 bool block = false;
 bool throwOnTrans = false;
 std::atomic<int> attempts{0};
 std::atomic<bool> entered{false}, proceed{false}, sawProcessing{false};
 std::atomic<bool> called{false};
};

int main(int argc,char** argv) {
 if (argc != 2) return 2;
 const std::string mode(argv[1]);
 if(mode == "repeat") {
  StateMonitor monitor; monitor.run(); std::this_thread::sleep_for(std::chrono::milliseconds(20));
  monitor.stop(); monitor.stop(); monitor.run(); monitor.stop(); return 0;
 }
 if(mode == "concurrent") {
  StateMonitor monitor; monitor.run(); std::atomic<bool> go{false};
  std::vector<std::thread> callers;
  for(int i=0;i<8;++i) callers.emplace_back([&]{while(!go.load()){}; monitor.stop();});
  go.store(true); for(auto& t:callers)t.join(); monitor.stop(); return 0;
 }
 if(mode == "runstop") {
  StateMonitor monitor; std::atomic<bool> go{false}; std::vector<std::thread> callers;
  for(int i=0;i<8;++i) callers.emplace_back([&]{while(!go.load()){};monitor.run();});
  callers.emplace_back([&]{while(!go.load()){};monitor.stop();});
  go.store(true); for(auto& t:callers)t.join(); monitor.stop(); return 0;
 }
 if(mode == "multiple") {
  StateMonitor a,b; a.run();b.run();std::this_thread::sleep_for(std::chrono::milliseconds(50));a.stop();b.stop();return 0;
 }
 if(mode == "self" || mode == "self-external-join" || mode == "exception") {
  StateMonitor monitor; DataManager manager; CallbackWriter writer(monitor); manager._writer=&writer;
  writer.block = mode == "self-external-join";
  writer.throwOnTrans = mode == "exception";
  auto* session=WTSSessionInfo::create("TEST","test"); session->addTradingSection(0, 2400);
  R154StateMonitorTestAccess::attach(monitor,&manager,session,SS_CLOSED);
  WTSSessionInfo* active = nullptr;
  if (mode == "exception") {
   active=WTSSessionInfo::create("ACTIVE","active"); active->addTradingSection(0, 2400);
   R154StateMonitorTestAccess::attach(monitor,&manager,active,SS_RECEIVING,"ACTIVE");
   if (!monitor.isInState("ACTIVE", SS_RECEIVING)) return 5;
  }
  monitor.run();
#ifndef R22_OLD_BASELINE
  if (mode == "exception") {
   for(int i=0;i<2000 && !monitor.hasFailed();++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));
   if (!monitor.hasFailed()) return 6;
   monitor.stop();
   SimpleState recorded = SS_ORIGINAL;
   if (!monitor.tryGetRecordedState("TEST", recorded) || recorded != SS_PROCING ||
       monitor.tryGetRecordedState("MISSING", recorded) || monitor.isInState("TEST", SS_PROCING) ||
       monitor.isInState("ACTIVE", SS_RECEIVING) || monitor.isAnyInState(SS_RECEIVING) ||
       monitor.isAllInState(SS_RECEIVING) || monitor.isAnyInState(SS_PROCED) || monitor.isAllInState(SS_PROCED)) return 7;
   monitor.run(); monitor.stop();
   if (writer.attempts.load() != 1) return 8;
   manager._writer=nullptr; session->release(); active->release(); return 0;
  }
#endif
  if (writer.block) {
   for(int i=0;i<2000 && !writer.entered.load();++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));
   if (!writer.entered.load()) return 4;
   std::thread joiner([&] { monitor.stop(); });
   for(int i=0;i<2000 && !R154StateMonitorTestAccess::joinInProgress(monitor);++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
   if (!R154StateMonitorTestAccess::joinInProgress(monitor)) std::terminate();
   writer.proceed.store(true);
   joiner.join();
  }
  for(int i=0;i<2000 && !writer.called.load();++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  if(!writer.called.load() || !writer.sawProcessing.load()) return 3;
  monitor.stop(); manager._writer=nullptr; session->release();return 0;
 }
 if(mode == "queries") {
  for(int i=0;i<100;++i) {
   StateMonitor monitor; auto* session=WTSSessionInfo::create("TEST","test"); session->addTradingSection(0, 2400);
   R154StateMonitorTestAccess::attach(monitor,nullptr,session,SS_INITIALIZED);
   std::atomic<bool> go{false};
   std::thread reader([&]{while(!go.load()){};for(int n=0;n<3000;++n){(void)monitor.isInState("TEST",SS_RECEIVING);(void)monitor.isAnyInState(SS_INITIALIZED);(void)monitor.isAllInState(SS_RECEIVING);}});
   go.store(true); R154StateMonitorTestAccess::advance(monitor);reader.join();session->release();
  }
  return 0;
 }
 return 2;
}
