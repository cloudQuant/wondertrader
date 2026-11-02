#include "TraderFemas/TraderFemas.h"
#include "TestUnits/coverage/t_TraderFemas/r93_femas_api_stub.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <thread>
#include <string>
#include <functional>

class Api final : public R93FemasApiStub {
public:
 std::mutex mx; std::condition_variable cv; bool entered=false, proceed=false, completed=false;
 std::function<void()> on_init;
 void Init() override { if (on_init) on_init(); }
 int ReqQryInvestorAccount(CUstpFtdcQryInvestorAccountField*, int) override {
  std::unique_lock<std::mutex> l(mx); entered=true; std::fprintf(stderr,"request_entered\n"); std::fflush(stderr); cv.notify_all();
  cv.wait(l,[this]{return proceed;}); completed=true; cv.notify_all(); return 0;
 }
};
static Api* current_api=nullptr;
static CUstpFtdcTraderApi* make_fake(const char*) { return current_api; }
class Probe final : public TraderFemas {
public:
 void setup(const std::string& dir) { m_funcCreator=&make_fake; m_strFlowDir=dir; m_strBroker="broker"; m_strUser="user"; }
 void ready() {m_wrapperState=WS_ALLREADY;}
 bool joinable() const { return m_thrdWorker && m_thrdWorker->joinable(); }
};
int main(int argc,char** argv) {
 if(argc!=3) return 2;
 std::set_terminate([]{std::fprintf(stderr,"terminate_from_joinable_worker\n");std::fflush(stderr);std::_Exit(86);});
 Api fake; current_api=&fake; Probe* obj=new Probe(); obj->setup(argv[2]);
 if(std::string(argv[1])=="inflight") fake.on_init=[&]{obj->ready(); std::fprintf(stderr,"init_query_result=%d\n",obj->queryAccount());};
 obj->connect();
 if(!obj->joinable()) {std::fprintf(stderr,"worker_not_started\n");std::_Exit(3);}
 if(std::string(argv[1])=="worker") {
  std::this_thread::sleep_for(std::chrono::milliseconds(20)); obj->release();
  std::fprintf(stderr,"after_release worker_joinable=%d fake_releases=%d\n",obj->joinable(),fake.n_release_); std::fflush(stderr);
  delete obj; std::fprintf(stderr,"delete_returned\n"); std::_Exit(0);
 }
 if(std::string(argv[1])=="inflight") {
  { std::unique_lock<std::mutex> l(fake.mx); if(!fake.cv.wait_for(l,std::chrono::seconds(3),[&]{return fake.entered;})) {std::fprintf(stderr,"request_not_entered\n");std::_Exit(4);} }
  obj->release();
  { std::lock_guard<std::mutex> l(fake.mx); std::fprintf(stderr,"release_returned request_completed=%d fake_releases=%d\n",fake.completed,fake.n_release_); fake.proceed=true; fake.cv.notify_all(); }
  { std::unique_lock<std::mutex> l(fake.mx); if(!fake.cv.wait_for(l,std::chrono::seconds(3),[&]{return fake.completed;})) std::_Exit(5); }
  std::fprintf(stderr,"request_returned_after_release\n");std::fflush(stderr);std::_Exit(0);
 }
 if(std::string(argv[1])=="idlequery") {
  std::this_thread::sleep_for(std::chrono::milliseconds(100)); obj->ready(); int result=obj->queryAccount();
  std::unique_lock<std::mutex> l(fake.mx); bool entered=fake.cv.wait_for(l,std::chrono::milliseconds(300),[&]{return fake.entered;});
  std::fprintf(stderr,"idle_query_result=%d request_entered=%d\n",result,entered); std::fflush(stderr); std::_Exit(result==0 && !entered ? 0 : 7);
 }
 std::_Exit(6);
}
