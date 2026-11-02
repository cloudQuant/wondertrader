#include <WtBtCore/MatchEngine.h>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <cstring>
#include <string>
#include <iostream>
uint32_t makeLocalOrderID() { static uint32_t id = 0; return ++id; }
class Sink final : public IMatchSink {
 public:
 void handle_trade(uint32_t,const char*,bool,double,double,double,uint64_t) override {}
 void handle_order(uint32_t,const char*,bool,double,double,bool,uint64_t) override {}
 void handle_entrust(uint32_t,const char*,bool,const char*,uint64_t) override {}
};
int main(int argc,char**argv) {
 if(argc != 3) return 10;
 const std::string side(argv[1]), book(argv[2]);
 WTSTickStruct ts{};
 std::strcpy(ts.exchg,"SSE"); std::strcpy(ts.code,"STK.610007");
 ts.trading_date=20240102; ts.action_date=20240102; ts.action_time=93000000; ts.price=10.0; ts.total_volume=100; ts.volume=100;
 if(book=="bid" || book=="both") { ts.bid_prices[0]=9.9; ts.bid_qty[0]=4; }
 if(book=="ask" || book=="both") { ts.ask_prices[0]=10.1; ts.ask_qty[0]=6; }
 Sink sink; MatchEngine engine; WTSVariant* cfg=WTSVariant::createObject(); cfg->append("cancelrate",0.0); engine.regisSink(&sink); engine.init(cfg); cfg->release();
 WTSTickData* tick=WTSTickData::create(ts); engine.handle_tick("SSE.STK.610007",tick); tick->release();
 OrderIDs ids = side=="buy" ? engine.buy("SSE.STK.610007",10.0,3,20240102093000000ULL) : engine.sell("SSE.STK.610007",10.0,3,20240102093000000ULL);
 std::cout << side << " " << book << " ids=" << ids.size() << '\n';
 return ids.size()==1 ? 0 : 1;
}
