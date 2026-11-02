#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <dlfcn.h>
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/IBaseDataMgr.h"
#include "Includes/IDataWriter.h"

using namespace wtp;
class Base final : public IBaseDataMgr {
public:
 WTSCommodityInfo* getCommodity(const char*) override{return nullptr;}
 WTSCommodityInfo* getCommodity(const char*, const char*) override{return nullptr;}
 WTSContractInfo* getContract(const char*, const char*, uint32_t) override{return nullptr;}
 WTSArray* getContracts(const char*,uint32_t) override{return nullptr;}
 WTSSessionInfo* getSession(const char*) override{return nullptr;}
 WTSSessionInfo* getSessionByCode(const char*,const char*) override{return nullptr;}
 WTSArray* getAllSessions() override{return nullptr;}
 bool isHoliday(const char*,uint32_t,bool) override{return false;}
 uint32_t calcTradingDate(const char*,uint32_t,uint32_t,bool) override{return 0;}
 uint64_t getBoundaryTime(const char*,uint32_t,bool,bool) override{return 0;}
 uint32_t getContractSize(const char*,uint32_t) override{return 0;}
};
class Sink final : public IDataWriterSink {
public:
 Base base; IDataWriter* writer=nullptr; bool reenter=false; int callbacks=0;
 IBaseDataMgr* getBDMgr() override{return &base;}
 bool canSessionReceive(const char*) override{return true;}
 void broadcastTick(WTSTickData*) override{}
 void broadcastOrdQue(WTSOrdQueData* p) override {++callbacks; std::puts("callback queue"); if(reenter){reenter=false;std::puts("nested-enter queue");writer->writeOrderQueue(p);std::puts("nested-return queue");}}
 void broadcastOrdDtl(WTSOrdDtlData* p) override {++callbacks; std::puts("callback detail"); if(reenter){reenter=false;std::puts("nested-enter detail");writer->writeOrderDetail(p);std::puts("nested-return detail");}}
 void broadcastTrans(WTSTransData* p) override {++callbacks; std::puts("callback trans"); if(reenter){reenter=false;std::puts("nested-enter trans");writer->writeTransaction(p);std::puts("nested-return trans");}}
 CodeSet* getSessionComms(const char*) override{return &codes;}
 uint32_t getTradingDate(const char*) override{return 20240108;}
 void outputLog(WTSLogLevel,const char*) override{}
 CodeSet codes;
};
int main(int argc,char** argv) {
 if(argc!=5)return 10;
 setvbuf(stdout,nullptr,_IONBF,0);
 void* module=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL); if(!module){std::puts(dlerror());return 11;}
 auto create=reinterpret_cast<FuncCreateWriter>(dlsym(module,"createWriter"));
 auto destroy=reinterpret_cast<FuncDeleteWriter>(dlsym(module,"deleteWriter"));
 if(!create||!destroy)return 12;
 Sink sink; sink.writer=create();
 WTSVariant* cfg=WTSVariant::createObject(); cfg->append("path",argv[4]);cfg->append("cache","cache.dmb");cfg->append("async",false);cfg->append("groupsize",1000u);
 if(!sink.writer->init(cfg,&sink)) return 13; cfg->release();
 auto comm=WTSCommodityInfo::create("rb","reentry","SHFE","SESSION","CHN");
 comm->setCategory(CC_Future);
 auto ct=WTSContractInfo::create("rb2210","reentry","SHFE","rb");ct->setCommInfo(comm);
 sink.reenter=std::strcmp(argv[3],"reenter")==0;
 std::puts("write-enter"); alarm(3);
 bool ok=false;
 if(!std::strcmp(argv[2],"queue")) {WTSOrdQueStruct raw{};std::strcpy(raw.exchg,"SHFE");std::strcpy(raw.code,"rb2210");raw.trading_date=20240108;auto p=WTSOrdQueData::create(raw);p->setContractInfo(ct);ok=sink.writer->writeOrderQueue(p);p->release();}
 else if(!std::strcmp(argv[2],"detail")) {WTSOrdDtlStruct raw{};std::strcpy(raw.exchg,"SHFE");std::strcpy(raw.code,"rb2210");raw.trading_date=20240108;auto p=WTSOrdDtlData::create(raw);p->setContractInfo(ct);ok=sink.writer->writeOrderDetail(p);p->release();}
 else if(!std::strcmp(argv[2],"trans")) {WTSTransStruct raw{};std::strcpy(raw.exchg,"SHFE");std::strcpy(raw.code,"rb2210");raw.trading_date=20240108;auto p=WTSTransData::create(raw);p->setContractInfo(ct);ok=sink.writer->writeTransaction(p);p->release();}
 else return 14;
 alarm(0);std::printf("write-return %d callbacks=%d\n",ok,sink.callbacks);
 sink.writer->release();destroy(sink.writer);ct->release();comm->release();dlclose(module);return ok&&sink.callbacks==1?0:15;
}
