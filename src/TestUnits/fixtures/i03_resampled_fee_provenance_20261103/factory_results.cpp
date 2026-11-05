#include <cstdio>
#include <vector>
#include "WTSTools/WTSDataFactory.h"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSSessionInfo.hpp"
static WTSBarStruct sample(uint32_t date, uint32_t time, double px) {
    WTSBarStruct b{}; b.date=date; b.time=(uint64_t(date)-19900000)*10000+time;
    b.open=px; b.high=px+3; b.low=px-2; b.close=px+1; b.settle=px+2;
    b.vol=px*10; b.money=px*100; b.add=px; b.hold=px+20; return b;
}
static void run(const char* name,std::vector<WTSBarStruct> bars,WTSKlinePeriod p,uint32_t mult,WTSSessionInfo* session,bool open,bool align) {
    auto* s=WTSKlineSlice::create("SHFE.rb.HOT",p,1,bars.data(),bars.size()); WTSDataFactory factory;
    auto* out=factory.extractKlineData(s,p,mult,session,open,align);
    if(!out) { std::printf("%s null\n",name); s->release(); return; }
    std::printf("%s %u %d\n",name,out->size(),int(out->isClosed()));
    for(uint32_t i=0;i<out->size();++i) { const auto& b=*out->at(i);
        std::printf("%u %llu %.8f %.8f %.8f %.8f %.8f %.8f %.8f %.8f %.8f\n",b.date,(unsigned long long)b.time,b.open,b.high,b.low,b.close,b.settle,b.vol,b.money,b.add,b.hold);
    }
    out->release(); s->release();
}
int main() {
    auto* day=WTSSessionInfo::create("day","day"); day->addTradingSection(900,903); day->addTradingSection(1000,1004);
    std::vector<WTSBarStruct> mins={sample(20240102,901,1),sample(20240102,902,2),sample(20240102,903,3),sample(20240102,1001,4),sample(20240102,1002,5),sample(20240102,1003,6)};
    run("m1-align-open",mins,KP_Minute1,2,day,true,true); run("m1-align-closed",mins,KP_Minute1,2,day,false,true);
    auto* market=WTSSessionInfo::create("market","market"); market->addTradingSection(900,1130); market->addTradingSection(1300,1500);
    std::vector<WTSBarStruct> mins5={sample(20240102,905,1),sample(20240102,910,2),sample(20240102,915,3),sample(20240102,930,4),sample(20240102,935,5)};
    run("m5",mins5,KP_Minute5,2,market,true,false); run("m5-align",mins5,KP_Minute5,2,market,true,true);
    run("hour-open",mins5,KP_Minute5,PERIOD_TIMES_HOUR,market,true,false); run("hour-closed",mins5,KP_Minute5,PERIOD_TIMES_HOUR,market,false,false);
    run("half",mins5,KP_Minute5,PERIOD_TIMES_HALF,market,true,false);
    auto* night=WTSSessionInfo::create("night","night",-1260); night->addTradingSection(2100,230);
    std::vector<WTSBarStruct> nights={sample(20240102,2355,1),sample(20240103,0,2),sample(20240103,5,3)}; for(auto& b:nights) b.date=20240103;
    run("night-hour",nights,KP_Minute5,PERIOD_TIMES_HOUR,night,true,false); run("night-half",nights,KP_Minute5,PERIOD_TIMES_HALF,night,true,false);
    std::vector<WTSBarStruct> days={sample(20240102,0,1),sample(20240103,0,2),sample(20240104,0,3)};
    run("day-open",days,KP_DAY,2,nullptr,true,false); run("day-closed",days,KP_DAY,2,nullptr,false,false);
    night->release(); market->release(); day->release();
}
