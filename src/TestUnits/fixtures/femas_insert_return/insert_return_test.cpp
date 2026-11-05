#include "TraderFemas/TraderFemas.h"
#include "TestUnits/coverage/t_TraderFemas/r93_femas_api_stub.h"
#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSTradeDef.hpp"
#include <cstring>
#include <iostream>
#include <string>

using namespace wtp;
class Db : public IBaseDataMgr {
public:
 WTSContractInfo* ct = nullptr;
 WTSCommodityInfo* getCommodity(const char*) override { return nullptr; }
 WTSCommodityInfo* getCommodity(const char*, const char*) override { return nullptr; }
 WTSContractInfo* getContract(const char*, const char*, uint32_t) override { return ct; }
 WTSArray* getContracts(const char*, uint32_t) override { return nullptr; }
 WTSSessionInfo* getSession(const char*) override { return nullptr; }
 WTSSessionInfo* getSessionByCode(const char*, const char*) override { return nullptr; }
 WTSArray* getAllSessions() override { return nullptr; }
 bool isHoliday(const char*, uint32_t, bool) override { return false; }
 uint32_t calcTradingDate(const char*, uint32_t, uint32_t, bool) override { return 0; }
 uint64_t getBoundaryTime(const char*, uint32_t, bool, bool) override { return 0; }
};
class Sink : public ITraderSpi {
public:
 Db db; int replies=0; bool success=false; std::string log;
 IBaseDataMgr* getBaseDataMgr() override { return &db; }
 void handleEvent(WTSTraderEvent, int32_t) override {}
 void onLoginResult(bool, const char*, uint32_t) override {}
 void onRspEntrust(WTSEntrust*, WTSError* error) override { ++replies; success=(error == nullptr); }
 void handleTraderLog(WTSLogLevel, const char* s) override { log = s ? s : ""; }
};
class Probe : public TraderFemas {
public:
 void ready(CUstpFtdcTraderApi* api) { m_pUserAPI=api; m_wrapperState=WS_ALLREADY; m_strBroker="9999"; m_strUser="fake"; m_strSessionID="S1"; }
};
class Api : public R93FemasApiStub {
public:
 Probe* probe=nullptr; bool callback=false; int result=0;
 int ReqOrderInsert(CUstpFtdcInputOrderField* order, int requestID) override {
  ++n_order_insert_; order_req_=*order;
  if (callback) { CUstpFtdcRspInfoField rsp{}; probe->OnRspOrderInsert(order, &rsp, requestID, true); }
  return result;
 }
};
int main(int argc, char** argv) {
 if (argc!=2) return 2;
 WTSCommodityInfo* comm=WTSCommodityInfo::create("rb","rb","SHFE","TRADING","CHINA");
 WTSContractInfo* ct=WTSContractInfo::create("rb2401","rb","SHFE","rb");
 ct->setCommInfo(comm);
 Sink sink; sink.db.ct=ct; Api api; Probe trader; api.probe=&trader;
 trader.registerSpi(&sink); trader.ready(&api);
 WTSEntrust* order=WTSEntrust::create("rb2401",1,3500.0,"SHFE");
 order->setContractInfo(ct); order->setDirection(WDT_LONG); order->setOffsetType(WOT_OPEN);
 order->setPriceType(WPT_LIMITPRICE); order->setOrderFlag(WOF_NOR); order->setUserTag("test.1");
 std::string mode=argv[1];
 if(mode=="negative" || mode=="reentrant") api.result=-3;
 else if(mode=="positive") api.result=7;
 else if(mode!="success") return 2;
 api.callback=(mode=="reentrant");
 int value=trader.orderInsert(order);
 std::cout << "mode=" << mode << " sdk=" << api.result << " wrapper=" << value << " calls=" << api.n_order_insert_ << " replies=" << sink.replies << " success=" << sink.success << " log=" << sink.log << '\n';
 bool ok=api.n_order_insert_==1 && std::strcmp(api.order_req_.UserOrderLocalID,"test.1")==0;
 if(mode=="success") ok=ok && value==0 && sink.replies==0;
 if(mode=="negative" || mode=="positive") ok=ok && value<0 && sink.replies==0 && sink.log.find("Order inserting failed")!=std::string::npos;
 // Deliberately inconsistent fake: record the present behavior, not an oracle
 // for the vendor contract or for whether retry or rollback is safe.
 if(mode=="reentrant") ok=ok && value==0 && sink.replies==1 && sink.success;
 order->release(); ct->release(); comm->release();
 return ok ? 0 : 1;
}
