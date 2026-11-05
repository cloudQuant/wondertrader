#include "CTPLoader/TraderSpi.h"
#include "t26_ctp_api_stub.hpp"
#include <condition_variable>
#include <mutex>
#include <thread>
#include <chrono>
#include <fstream>
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <map>
#include <set>

CThostFtdcTraderApi* pUserApi = nullptr;
std::string BROKER_ID, INVESTOR_ID, PASSWORD, AUTHCODE, APPID, SAVEPATH;
std::string COMM_FILE="commodities.json", CONT_FILE="contracts.json", FEES_FILE="fees.json";
std::string FRONT_ADDR;
int iRequestID=0;
uint32_t CLASSMASK=1;
bool ONLYINCFG=false, INCREMENTAL=false, QRYFEES=true;
std::map<std::string,std::string> MAP_NAME, MAP_SESSION;
std::set<std::string> SET_FILTERS;
#ifdef WT_T26_FEES_TEST_HOOKS
bool wtT26FeesFailIo(int) { return false; }
#endif

struct CTraderSpiQueueTestAccess {
 static void start(CTraderSpi& s) { s._trading_day=20250102; s.ReqQryInstrument(); }
};
template<size_t N> void set(char (&out)[N], const char* value) {
 std::strncpy(out,value,N-1); out[N-1]='\0';
}
class API : public T26CtpApiStub {
public:
 CTraderSpi* spi; std::string mode, path; std::mutex mutex; std::condition_variable cv;
 int commission=0, margin=0;
 API(CTraderSpi* s,std::string m,std::string p):spi(s),mode(std::move(m)),path(std::move(p)){}
 int ReqQryInstrument(CThostFtdcQryInstrumentField*, int id) override {
  if(mode=="instrument-failure") return -1;
  CThostFtdcInstrumentField f{};
  set(f.InstrumentID,"rb2510"); set(f.ProductID,"rb"); set(f.ExchangeID,"SHFE");
  set(f.InstrumentName,"steel"); set(f.OpenDate,"20170101"); set(f.ExpireDate,"20251231");
  f.ProductClass=THOST_FTDC_PC_Futures; f.VolumeMultiple=10; f.PriceTick=1;
  if(mode=="conflicting-product") {
   spi->OnRspQryInstrument(&f,nullptr,id,false);
   set(f.InstrumentID,"rb2511");
  }
  spi->OnRspQryInstrument(&f,nullptr,id,true);
	  if (mode=="duplicate-instrument") spi->OnRspQryInstrument(&f,nullptr,id,true);
  return 0;
 }
 void sendCommission(int id,bool last,bool data=true, bool error=false) {
  CThostFtdcInstrumentCommissionRateField f{}; set(f.InstrumentID,"rb2510");
  f.OpenRatioByVolume=1; f.CloseRatioByVolume=1; f.CloseTodayRatioByVolume=1;
  CThostFtdcRspInfoField err{}; err.ErrorID=5;
  spi->OnRspQryInstrumentCommissionRate(data?&f:nullptr,error?&err:nullptr,id,last);
 }
 void sendMargin(int id,bool last,bool data=true) {
  CThostFtdcInstrumentMarginRateField f{}; set(f.InstrumentID,mode=="wrong-code"?"au2512":"rb2510");
  if(mode=="wrong-exchange") set(f.ExchangeID,"DCE");
  f.LongMarginRatioByMoney=0.1; f.ShortMarginRatioByMoney=0.1;
  spi->OnRspQryInstrumentMarginRate(data?&f:nullptr,nullptr,id,last);
 }
 bool oldFile() { std::ifstream f(path+"/fees.json"); std::string s; std::getline(f,s); return s=="old-sentinel"; }
 int ReqQryInstrumentCommissionRate(CThostFtdcQryInstrumentCommissionRateField* request,int id) override {
  {std::lock_guard<std::mutex> l(mutex);commission=id;} cv.notify_all();
  if(mode=="conflicting-product") {
   CThostFtdcInstrumentCommissionRateField f{}; set(f.InstrumentID,"rb");
   f.OpenRatioByVolume=std::string(request->InstrumentID)=="rb2511"?2:1;
   f.CloseRatioByVolume=f.OpenRatioByVolume; f.CloseTodayRatioByVolume=f.OpenRatioByVolume;
   spi->OnRspQryInstrumentCommissionRate(&f,nullptr,id,true); return 0;
  }
  if(mode=="sync" || mode=="send-failure" || mode=="sync-contradiction" || mode=="blocked-send" || mode=="blocked-after-terminal") {
   sendCommission(id,true);
   if(!oldFile()) std::_Exit(81);
   if(mode=="sync-contradiction") return -1;
  }
  return 0;
 }
 int ReqQryInstrumentMarginRate(CThostFtdcQryInstrumentMarginRateField* request,int id) override {
  {std::lock_guard<std::mutex> l(mutex);margin=id;} cv.notify_all();
  if(mode=="conflicting-product") {
   CThostFtdcInstrumentMarginRateField f{}; set(f.InstrumentID,request->InstrumentID);
   f.LongMarginRatioByMoney=0.1; f.ShortMarginRatioByMoney=0.1;
   spi->OnRspQryInstrumentMarginRate(&f,nullptr,id,true); return 0;
  }
  if(mode=="send-failure") return -1;
  if(mode=="sync" || mode=="sync-contradiction") { sendMargin(id,true); if(!oldFile()) std::_Exit(82); }
	  if(mode=="blocked-send" || mode=="blocked-after-terminal") {
	   if(mode=="blocked-after-terminal") sendMargin(id,true);
	   std::this_thread::sleep_for(std::chrono::seconds(15));
	   std::ofstream(path+"/send-returned") << "returned";
	  }
  return 0;
 }
};
int main(int argc,char** argv) {
 if(argc!=3) return 90;
 const std::string mode=argv[1]; SAVEPATH=std::string(argv[2])+"/";
 CTraderSpi spi; API api(&spi,mode,argv[2]); pUserApi=&api;
 // K02=B (2026-11-04): the loader no longer exits the process from a callback. The
 // probe observes the published terminal result and ends the process with it
 // (std::_Exit keeps the old semantics without joining a blocked fake worker).
 int code=0;
 auto done=[&]{ return loader_completion::finished(&code); };
 CTraderSpiQueueTestAccess::start(spi);
 if(mode=="sync" || mode=="send-failure" || mode=="sync-contradiction" || mode=="instrument-failure" || mode=="timeout" || mode=="blocked-send" || mode=="blocked-after-terminal" || mode=="conflicting-product") {
  if(loader_completion::wait_for(code,std::chrono::seconds(9))) std::_Exit(code);
  std::_Exit(91);
 }
 { std::unique_lock<std::mutex> l(api.mutex); if(!api.cv.wait_for(l,std::chrono::seconds(8),[&]{return (api.commission && api.margin) || loader_completion::finished();})) std::_Exit(92); }
 if(done()) std::_Exit(code);
 std::this_thread::sleep_for(std::chrono::milliseconds(150));
 if(done()) std::_Exit(code);
 if(!api.oldFile()) std::_Exit(93);
 if(mode=="disconnect") {spi.OnFrontDisconnected(1); if(done()) std::_Exit(code); std::_Exit(94);}
 if(mode=="unknown") api.sendMargin(99999,true);
 if(done()) std::_Exit(code);
 api.sendMargin(api.margin,true);
 if(done()) std::_Exit(code);
 if(!api.oldFile()) std::_Exit(95);
 if(mode=="duplicate") api.sendMargin(api.margin,true);
 if(mode=="late-error") {CThostFtdcRspInfoField err{};err.ErrorID=9;spi.OnRspQryInstrumentMarginRate(nullptr,&err,api.margin,true);}
 if(mode=="late-rsp-error") {CThostFtdcRspInfoField err{};err.ErrorID=9;spi.OnRspError(&err,api.margin,true);}
 if(done()) std::_Exit(code);
 if(mode=="error") api.sendCommission(api.commission,false,false,true);
 if(mode=="rsp-error") {CThostFtdcRspInfoField err{};err.ErrorID=8;spi.OnRspError(&err,api.commission,true);}
 else if(mode=="empty") api.sendCommission(api.commission,true,false);
 else { api.sendCommission(api.commission,false); if(done()) std::_Exit(code); if(!api.oldFile()) std::_Exit(96); api.sendCommission(api.commission,true,false); }
 if(loader_completion::wait_for(code,std::chrono::seconds(5))) std::_Exit(code);
 std::_Exit(97);
}
