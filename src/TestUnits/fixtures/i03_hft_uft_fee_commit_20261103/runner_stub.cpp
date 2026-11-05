#include "WtBtPorter/WtBtRunner.h"
// Stub runner observes the real ExpHftMocker virtual callback path; it does
// not exercise the production runner's callback registration or lifecycle.
int expTradeNotifications=0, expOrderNotifications=0;
WtBtRunner::WtBtRunner() : _cb_evt(nullptr) {}
WtBtRunner::~WtBtRunner() {}
WtBtRunner& getRunner() { static WtBtRunner runner; return runner; }
void WtBtRunner::ctx_on_init(uint32_t,EngineType) {}
void WtBtRunner::ctx_on_session_event(uint32_t,uint32_t,bool,EngineType) {}
void WtBtRunner::ctx_on_tick(uint32_t,const char*,WTSTickData*,EngineType) {}
void WtBtRunner::ctx_on_bar(uint32_t,const char*,const char*,WTSBarStruct*,EngineType) {}
void WtBtRunner::hft_on_order_queue(uint32_t,const char*,WTSOrdQueData*) {}
void WtBtRunner::hft_on_order_detail(uint32_t,const char*,WTSOrdDtlData*) {}
void WtBtRunner::hft_on_transaction(uint32_t,const char*,WTSTransData*) {}
void WtBtRunner::hft_on_channel_ready(uint32_t,const char*) {}
void WtBtRunner::hft_on_order(uint32_t,WtUInt32,const char*,bool,double,double,double,bool,const char*) { ++expOrderNotifications; }
void WtBtRunner::hft_on_trade(uint32_t,WtUInt32,const char*,bool,double,double,const char*) { ++expTradeNotifications; }
void WtBtRunner::hft_on_entrust(uint32_t,WtUInt32,const char*,bool,const char*,const char*) {}
bool WtBtRunner::loadFinalHisBars(void*,const char*,WTSKlinePeriod,FuncReadBars) { return false; }
bool WtBtRunner::loadRawHisBars(void*,const char*,WTSKlinePeriod,FuncReadBars) { return false; }
bool WtBtRunner::loadAllAdjFactors(void*,FuncReadFactors) { return false; }
bool WtBtRunner::loadAdjFactors(void*,const char*,FuncReadFactors) { return false; }
bool WtBtRunner::loadRawHisTicks(void*,const char*,uint32_t,FuncReadTicks) { return false; }
