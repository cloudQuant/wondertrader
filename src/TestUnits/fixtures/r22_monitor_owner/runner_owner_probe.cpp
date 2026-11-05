#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include "Includes/IDataWriter.h"
#include "Share/StdUtils.hpp"
#include "Share/BoostMappingFile.hpp"
#include "Share/DLLHelper.hpp"
#define private public
#include "WtDtCore/DataManager.h"
#undef private
#include "WtDtCore/ParserAdapter.h"
#include "WtDtCore/StateMonitor.h"
#include "WtDtCore/UDPCaster.h"
#include "WtDtCore/IndexWorkerMgr.h"
#include "WtDtCore/ShmCaster.h"
#include "WTSTools/WTSHotMgr.h"
#include "WTSTools/WTSBaseDataMgr.h"
#include <boost/asio.hpp>
#define private public
#include "WtDtPorter/WtDtRunner.h"
#undef private
#include "Includes/WTSSessionInfo.hpp"

struct R154StateMonitorTestAccess {
 static void attach(StateMonitor& monitor, DataManager* manager, WTSSessionInfo* session) {
  monitor._dt_mgr=manager;
  StatePtr info(new StateInfo);
  std::strcpy(info->_session,"TEST"); info->_sInfo=session;
  info->_state=SS_CLOSED; info->_proc_time=0;
  monitor._map["TEST"]=info;
 }
};
struct Shared {
 std::atomic<bool> entered{false}, allowCallback{false}, callbackDone{false};
 std::atomic<bool> releaseCalled{false}, releaseBeforeCallbackDone{false}, allowRelease{false}, destructorStarted{false}, destructorDone{false};
};
class BlockingWriter final : public IDataWriter {
public:
 BlockingWriter(Shared& s, StateMonitor* monitor):s(s),monitor(monitor){}
 void release() override {
  s.releaseBeforeCallbackDone.store(!s.callbackDone.load()); s.releaseCalled.store(true);
  while(!s.allowRelease.load())std::this_thread::yield();
 }
 bool writeTick(WTSTickData*,uint32_t) override {return false;}
 WTSTickData* getCurTick(const char*,const char*) override{return nullptr;}
 bool isSessionProceeded(const char*) override{return false;}
 void transHisData(const char*) override {
  s.entered.store(true);
  while(!s.allowCallback.load())std::this_thread::yield();
  monitor->stop(); // Synchronous callback stop while owner may be joining.
  s.callbackDone.store(true);
 }
 Shared& s;
 StateMonitor* monitor;
};
static void removeWriter(IDataWriter*& writer) {delete writer;writer=nullptr;}
int main(int argc,char** argv) {
 if(argc > 1 && std::strcmp(argv[1],"idle")==0) {
  auto* idle=new WtDtRunner(); delete idle; return 0;
 }
 Shared s; auto* owner=new WtDtRunner();
 auto* session=WTSSessionInfo::create("TEST","test"); session->addTradingSection(0,2400);
 owner->_data_mgr._writer=new BlockingWriter(s,&owner->_state_mon); owner->_data_mgr._remover=removeWriter;
 R154StateMonitorTestAccess::attach(owner->_state_mon,&owner->_data_mgr,session);
 StateMonitor* monitor=&owner->_state_mon;
 monitor->run();
 for(int i=0;i<2000 && !s.entered.load();++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));
 if(!s.entered.load())return 2;
 std::thread killer([&]{s.destructorStarted.store(true);delete owner;s.destructorDone.store(true);});
 while(!s.destructorStarted.load())std::this_thread::yield();
 // A timeout is a probe window, not proof by itself; the writer callback is
 // held at the explicit barrier until after the release observation.
 for(int i=0;i<200 && !s.releaseCalled.load();++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));
 bool early=s.releaseCalled.load();
 s.allowCallback.store(true);
 // Keep DataManager's fake release blocked until the worker is really joined,
 // including in the old implementation; avoid manufacturing a test UAF.
 monitor->stop();
 s.allowRelease.store(true);
 killer.join();session->release();
 if(early || s.releaseBeforeCallbackDone.load() || !s.releaseCalled.load() || !s.destructorDone.load()) {
  std::fprintf(stderr,"bad order: early=%d releaseBeforeDone=%d release=%d destroyed=%d\n",early,s.releaseBeforeCallbackDone.load(),s.releaseCalled.load(),s.destructorDone.load());return 1;
 }
 return 0;
}
