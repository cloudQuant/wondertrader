#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include "Includes/IDataWriter.h"
#include "Share/StdUtils.hpp"
#include "Share/BoostMappingFile.hpp"
#include "Share/DLLHelper.hpp"
#define private public
#include "WtDtCore/DataManager.h"
#undef private
#include "WtDtCore/StateMonitor.h"
#include "Includes/WTSSessionInfo.hpp"

extern StateMonitor g_stateMon;
extern DataManager g_dataMgr;
struct R154StateMonitorTestAccess {
 static void attach(StateMonitor& mon,DataManager* manager,WTSSessionInfo* session) {
  mon._dt_mgr=manager;StatePtr info(new StateInfo);std::strcpy(info->_session,"TEST");
  info->_sInfo=session;info->_state=SS_CLOSED;info->_proc_time=0;mon._map["TEST"]=info;
 }
};
struct Shared {std::atomic<bool> entered{false},allow{false},callbackDone{false},exiting{false};};
class BlockingWriter final:public IDataWriter {
public:
 explicit BlockingWriter(Shared& s):s(s){}
 void release() override {
  if(!s.callbackDone.load()) {std::fprintf(stderr,"EARLY_RELEASE\n");std::fflush(stderr);std::_Exit(9);}
  std::fprintf(stderr,"RELEASE_AFTER_CALLBACK\n");
 }
 bool writeTick(WTSTickData*,uint32_t) override{return false;}
 WTSTickData* getCurTick(const char*,const char*) override{return nullptr;}
 bool isSessionProceeded(const char*) override{return false;}
 void transHisData(const char*) override {s.entered.store(true);while(!s.allow.load())std::this_thread::yield();g_stateMon.stop();s.callbackDone.store(true);}
 Shared& s;
};
static void removeWriter(IDataWriter*& writer){delete writer;writer=nullptr;}
int main(int argc,char** argv) {
 if(argc > 1 && std::strcmp(argv[1],"idle")==0)std::exit(0);
 Shared s;auto* session=WTSSessionInfo::create("TEST","test");session->addTradingSection(0,2400);
 g_dataMgr._writer=new BlockingWriter(s);g_dataMgr._remover=removeWriter;
 R154StateMonitorTestAccess::attach(g_stateMon,&g_dataMgr,session);g_stateMon.run();
 for(int i=0;i<2000&&!s.entered.load();++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));
 if(!s.entered.load())return 2;
 std::thread exitThread([&]{s.exiting.store(true);std::exit(0);});
 while(!s.exiting.load())std::this_thread::yield();
 std::this_thread::sleep_for(std::chrono::milliseconds(200));s.allow.store(true);
 // The exiting thread executes static destructors and terminates the process.
 // Neither a detach nor early destruction of the live worker is used here.
 for(;;)std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
