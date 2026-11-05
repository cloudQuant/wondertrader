#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <unistd.h>
#include "WtBtCore/HftMocker.h"
#include "WtBtCore/UftMocker.h"
#include "WtBtPorter/ExpHftMocker.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "WTSTools/WTSLogger.h"
extern int expTradeNotifications, expOrderNotifications;

struct HisDataReplayerTestAccess {
 static WTSBaseDataMgr& manager(HisDataReplayer& r) { return r._bd_mgr; }
 static void tick(HisDataReplayer& r, const char* sub, const char* raw, double price, double qty) {
  r._cur_tdate=20240223; r._cur_date=20240223; r._tick_enabled=true; r._running.store(true);
  auto& s=r._last_dispatched_ticks[sub]; s.tradingDate=20240223; s.adjustedSource=false;
  s.physicalCode=raw; s.exchange="SHFE"; s.tick=WTSTickStruct{};
  std::strncpy(s.tick.code,raw,sizeof(s.tick.code)-1); std::strncpy(s.tick.exchg,"SHFE",sizeof(s.tick.exchg)-1);
  s.tick.trading_date=20240223; s.tick.action_date=20240223; s.tick.price=price;
  s.tick.ask_prices[0]=s.tick.bid_prices[0]=price; s.tick.ask_qty[0]=s.tick.bid_qty[0]=qty;
  r._dispatched_tick_subscriptions.insert(sub);
 }
 static void stop(HisDataReplayer& r) { r._running.store(false); }
};
struct HftMockerTestAccess {
 static void order(HftMocker& m, uint32_t id, const char* code, bool buy, double qty) {
  auto p=std::make_shared<HftMocker::OrderInfo>(); p->_localid=id; p->_isBuy=buy; p->_left=p->_total=qty;
  p->_proced_after_placed=true; std::strncpy(p->_code,code,sizeof(p->_code)-1); m._orders[id]=p;
 }
 static bool run(HftMocker& m,uint32_t id) { return m.procOrder(id); }
 static double left(HftMocker& m,uint32_t id) { return m._orders.at(id)->_left; }
 static double fees(HftMocker& m) { return m._fund_info._total_fees; }
 static void setFees(HftMocker& m,double f) { m._fund_info._total_fees=f; }
 static double pos(HftMocker& m,const char* code) { auto i=m._pos_map.find(code); return i==m._pos_map.end()?0:i->second._volume; }
 static std::string trades(HftMocker& m) { return m._trade_logs.str(); }
 static void yesterday(HftMocker& m,const char* code) { for(auto& d : m._pos_map.at(code)._details) d._opentdate=20240222; }
 static void changeFund(HftMocker& m) { m._fund_info._total_fees += 1; }
 static void changeLeft(HftMocker& m,uint32_t id,double left) { m._orders.at(id)->_left=left; }
};
struct UftMockerTestAccess {
 static void order(UftMocker& m,uint32_t id,const char* code,bool isLong,uint32_t offset,double qty) {
  auto p=std::make_shared<UftMocker::OrderInfo>(); p->_localid=id; p->_isLong=isLong; p->_offset=offset;
  p->_isBuy=(offset==0)==isLong; p->_left=p->_total=qty; p->_proced_after_placed=true;
  std::strncpy(p->_code,code,sizeof(p->_code)-1); m._orders[id]=p;
 }
 static bool run(UftMocker& m,uint32_t id) { return m.procOrder(id); }
 static double left(UftMocker& m,uint32_t id) { return m._orders.at(id)->_left; }
 static double fees(UftMocker& m) { return m._fund_info._total_fees; }
 static void setFees(UftMocker& m,double f) { m._fund_info._total_fees=f; }
 static double pos(UftMocker& m,const char* code) { auto i=m._pos_map.find(code); return i==m._pos_map.end()?0:i->second._long.volume(); }
 static std::string trades(UftMocker& m) { return m._trade_logs.str(); }
 static void yesterday(UftMocker& m,const char* code) { auto& p=m._pos_map.at(code)._long; for(auto& d:p._details) d._opentdate=20240222; p._prevol=p._newvol; p._newvol=0; }
 static void changeFund(UftMocker& m) { m._fund_info._total_fees += 1; }
 static void changeLeft(UftMocker& m,uint32_t id,double left) { m._orders.at(id)->_left=left; }
};
class HftProbe : public HftMocker {
public:
 using HftMocker::HftMocker; int calls=0, orderCalls=0; bool skip=false, reenter=false, throwBefore=false, throwAfter=false, throwOrder=false; std::function<void()> before, after, orderAfter;
 void on_trade(uint32_t id,const char* code,bool buy,double qty,double price,const char* tag) override {
  ++calls; if(throwBefore) throw std::runtime_error("before"); if(skip) return; if(before) before(); HftMocker::on_trade(id,code,buy,qty,price,tag);
  if(throwAfter) throw std::runtime_error("after");
  if(reenter) HftMocker::on_trade(id,code,buy,qty,price,tag);
  if(after) after();
 }
 void on_order(uint32_t,const char*,bool,double,double,double,bool,const char*) override { ++orderCalls; if(throwOrder) throw std::runtime_error("order"); if(orderAfter) orderAfter(); }
};
class UftProbe : public UftMocker {
public:
 using UftMocker::UftMocker; int calls=0, orderCalls=0; bool skip=false, reenter=false, throwBefore=false, throwAfter=false, throwOrder=false; std::function<void()> before, after, orderAfter;
 void on_trade(uint32_t id,const char* code,bool dir,uint32_t off,double qty,double price) override {
  ++calls; if(throwBefore) throw std::runtime_error("before"); if(skip) return; if(before) before(); UftMocker::on_trade(id,code,dir,off,qty,price);
  if(throwAfter) throw std::runtime_error("after");
  if(reenter) UftMocker::on_trade(id,code,dir,off,qty,price);
  if(after) after();
 }
 void on_order(uint32_t,const char*,bool,uint32_t,double,double,double,bool) override { ++orderCalls; if(throwOrder) throw std::runtime_error("order"); if(orderAfter) orderAfter(); }
};
class FeeCommit : public ::testing::Test {
protected:
 HisDataReplayer r; std::filesystem::path dir;
 std::string write(const char* name,const std::string& text) { auto p=dir/name; std::ofstream(p)<<text; return p.string(); }
 void SetUp() override {
  static bool init=[] { WTSLogger::init(R"({"root":{"level":"debug","async":false,"sinks":[]}})",false,nullptr); return true; }(); (void)init;
  char pattern[]="/tmp/wt-fee-commit-XXXXXX"; auto* p=mkdtemp(pattern); ASSERT_NE(p,nullptr); dir=p;
  auto sessions=write("sessions.json",R"({"ALL":{"name":"all","sections":[{"from":0,"to":0}]}})");
  auto holidays=write("holidays.json",R"({"CHINA":[]})");
  auto comms=write("comms.json",R"({"SHFE":{"rb":{"name":"steel","session":"ALL","holiday":"CHINA","pricetick":1,"volscale":10}}})");
  auto contracts=write("contracts.json",R"({"SHFE":{"rb2405":{"name":"May","exchg":"SHFE","product":"rb","opendate":20200101,"expiredate":20300101},"rb2410":{"name":"Oct","exchg":"SHFE","product":"rb","opendate":20200101,"expiredate":20300101,"volscale":20}}})");
  auto& m=HisDataReplayerTestAccess::manager(r); ASSERT_TRUE(m.loadSessions(sessions.c_str())); ASSERT_TRUE(m.loadHolidays(holidays.c_str())); ASSERT_TRUE(m.loadCommodities(comms.c_str())); ASSERT_TRUE(m.loadContracts(contracts.c_str()));
 }
 void TearDown() override { HisDataReplayerTestAccess::stop(r); std::error_code ec; std::filesystem::remove_all(dir,ec); }
 void rates(const std::string& data) { ASSERT_TRUE(r.loadFeesChecked(write("fees.json",data).c_str())); }
 void tick(const char* sub="SHFE.rb.HOT",const char* raw="rb2410",double px=100,double qty=10) { HisDataReplayerTestAccess::tick(r,sub,raw,px,qty); }
 const char* code="SHFE.rb.HOT";
};
static const char* normal=R"({"version":2,"contracts":{"SHFE.rb2405":{"open":0,"close":0,"closetoday":0,"byvolume":true},"SHFE.rb2410":{"open":2,"close":3,"closetoday":4,"byvolume":true}},"products":{}})";
TEST_F(FeeCommit,HftOpenCloseTodayReverseAndPhysicalZero) {
 rates(normal); tick(); HftProbe m(&r,"h");
 HftMockerTestAccess::order(m,1,code,true,2); EXPECT_TRUE(HftMockerTestAccess::run(m,1)); EXPECT_EQ(HftMockerTestAccess::pos(m,code),2); EXPECT_EQ(HftMockerTestAccess::fees(m),4);
 HftMockerTestAccess::order(m,2,code,false,1); EXPECT_TRUE(HftMockerTestAccess::run(m,2)); EXPECT_EQ(HftMockerTestAccess::pos(m,code),1); EXPECT_EQ(HftMockerTestAccess::fees(m),8);
 HftMockerTestAccess::order(m,3,code,false,2); EXPECT_TRUE(HftMockerTestAccess::run(m,3)); EXPECT_EQ(HftMockerTestAccess::pos(m,code),-1); EXPECT_EQ(HftMockerTestAccess::fees(m),14);
 tick(code,"rb2405"); HftMockerTestAccess::order(m,4,code,true,1); EXPECT_TRUE(HftMockerTestAccess::run(m,4)); EXPECT_EQ(HftMockerTestAccess::fees(m),14);
}
TEST_F(FeeCommit,UftOpenCloseTodayAndPhysicalZero) {
 rates(normal); tick(); UftProbe m(&r,"u");
 UftMockerTestAccess::order(m,1,code,true,0,2); EXPECT_TRUE(UftMockerTestAccess::run(m,1)); EXPECT_EQ(UftMockerTestAccess::pos(m,code),2); EXPECT_EQ(UftMockerTestAccess::fees(m),4);
 UftMockerTestAccess::order(m,2,code,true,2,1); EXPECT_TRUE(UftMockerTestAccess::run(m,2)); EXPECT_EQ(UftMockerTestAccess::pos(m,code),1); EXPECT_EQ(UftMockerTestAccess::fees(m),8);
 tick(code,"rb2405"); UftMockerTestAccess::order(m,3,code,true,1,1); EXPECT_TRUE(UftMockerTestAccess::run(m,3)); EXPECT_EQ(UftMockerTestAccess::fees(m),8);
}
TEST_F(FeeCommit,MissingFeesDoNotCommitOrNotify) {
 rates(R"({"version":2,"contracts":{},"products":{}})"); tick(); HftProbe h(&r,"h");
 HftMockerTestAccess::order(h,1,code,true,2); EXPECT_FALSE(HftMockerTestAccess::run(h,1)); EXPECT_EQ(HftMockerTestAccess::left(h,1),2); EXPECT_EQ(HftMockerTestAccess::pos(h,code),0); EXPECT_EQ(HftMockerTestAccess::fees(h),0); EXPECT_TRUE(HftMockerTestAccess::trades(h).empty()); EXPECT_EQ(h.calls,0); EXPECT_EQ(h.orderCalls,0); EXPECT_FALSE(r.fee_error().empty());
}
TEST_F(FeeCommit,UftOverflowAndMissingDoNotCommitOrNotify) {
 rates(R"({"version":2,"contracts":{"SHFE.rb2410":{"open":1e308,"close":1,"closetoday":1,"byvolume":false}},"products":{}})"); tick(); UftProbe u(&r,"u");
 UftMockerTestAccess::order(u,1,code,true,0,2); EXPECT_FALSE(UftMockerTestAccess::run(u,1)); EXPECT_EQ(UftMockerTestAccess::left(u,1),2); EXPECT_EQ(UftMockerTestAccess::pos(u,code),0); EXPECT_EQ(UftMockerTestAccess::fees(u),0); EXPECT_TRUE(UftMockerTestAccess::trades(u).empty()); EXPECT_EQ(u.calls,0); EXPECT_EQ(u.orderCalls,0); EXPECT_FALSE(r.fee_error().empty());
}
TEST_F(FeeCommit,HftOverrideWithoutCommitIsRejected) {
 rates(normal); tick(); HftProbe h(&r,"h"); h.skip=true;
 HftMockerTestAccess::order(h,1,code,true,2); EXPECT_FALSE(HftMockerTestAccess::run(h,1)); EXPECT_EQ(HftMockerTestAccess::left(h,1),2); EXPECT_EQ(HftMockerTestAccess::pos(h,code),0); EXPECT_EQ(h.orderCalls,0); EXPECT_FALSE(r.fee_error().empty());
}
TEST_F(FeeCommit,UftOverrideWithoutCommitIsRejected) {
 rates(normal); tick(); UftProbe u(&r,"u"); u.skip=true;
 UftMockerTestAccess::order(u,1,code,true,0,2); EXPECT_FALSE(UftMockerTestAccess::run(u,1)); EXPECT_EQ(UftMockerTestAccess::left(u,1),2); EXPECT_EQ(UftMockerTestAccess::pos(u,code),0); EXPECT_EQ(u.orderCalls,0); EXPECT_FALSE(r.fee_error().empty());
}
TEST_F(FeeCommit,SameMockerReentryStopsAfterCommittedFill) {
 rates(normal); tick(); HftProbe h(&r,"h"); h.reenter=true;
 HftMockerTestAccess::order(h,1,code,true,1); EXPECT_FALSE(HftMockerTestAccess::run(h,1)); EXPECT_EQ(HftMockerTestAccess::pos(h,code),1); EXPECT_EQ(HftMockerTestAccess::left(h,1),0); EXPECT_EQ(h.orderCalls,0); EXPECT_FALSE(r.fee_error().empty());
}
TEST_F(FeeCommit,CloseYesterdayUsesCloseRate) {
 rates(normal); tick(); HftProbe h(&r,"h"); UftProbe u(&r,"u");
 HftMockerTestAccess::order(h,1,code,true,1); ASSERT_TRUE(HftMockerTestAccess::run(h,1)); HftMockerTestAccess::yesterday(h,code);
 UftMockerTestAccess::order(u,1,code,true,0,1); ASSERT_TRUE(UftMockerTestAccess::run(u,1)); UftMockerTestAccess::yesterday(u,code);
 HftMockerTestAccess::order(h,2,code,false,1); EXPECT_TRUE(HftMockerTestAccess::run(h,2)); EXPECT_EQ(HftMockerTestAccess::fees(h),5);
 UftMockerTestAccess::order(u,2,code,true,1,1); EXPECT_TRUE(UftMockerTestAccess::run(u,2)); EXPECT_EQ(UftMockerTestAccess::fees(u),5);
}
TEST_F(FeeCommit,LaterSplitFeeOverflowPreventsEntireRoundForBothMockers) {
 rates(R"({"version":2,"contracts":{"SHFE.rb2410":{"open":1e300,"close":1,"closetoday":1,"byvolume":true}},"products":{}})"); tick(code,"rb2410",100,200);
 HftProbe h(&r,"h"); HftMockerTestAccess::setFees(h,std::numeric_limits<double>::max()-1.5e302);
 HftMockerTestAccess::order(h,1,code,true,200); EXPECT_FALSE(HftMockerTestAccess::run(h,1)); EXPECT_EQ(HftMockerTestAccess::left(h,1),200); EXPECT_EQ(HftMockerTestAccess::pos(h,code),0); EXPECT_TRUE(HftMockerTestAccess::trades(h).empty()); EXPECT_EQ(h.calls,0);
 HisDataReplayerTestAccess::stop(r); rates(R"({"version":2,"contracts":{"SHFE.rb2410":{"open":1e300,"close":1,"closetoday":1,"byvolume":true}},"products":{}})"); tick(code,"rb2410",100,200);
 UftProbe u(&r,"u"); UftMockerTestAccess::setFees(u,std::numeric_limits<double>::max()-1.5e302);
 UftMockerTestAccess::order(u,1,code,true,0,200); EXPECT_FALSE(UftMockerTestAccess::run(u,1)); EXPECT_EQ(UftMockerTestAccess::left(u,1),200); EXPECT_EQ(UftMockerTestAccess::pos(u,code),0); EXPECT_TRUE(UftMockerTestAccess::trades(u).empty()); EXPECT_EQ(u.calls,0);
}
TEST_F(FeeCommit,SplitPlanKeepsOriginalTickContractDespiteCallbackTickChange) {
 rates(normal); tick(code,"rb2410",100,200); HftProbe h(&r,"h");
 h.after=[&] { tick(code,"rb2405",100,200); };
 HftMockerTestAccess::order(h,1,code,true,200); EXPECT_TRUE(HftMockerTestAccess::run(h,1)); EXPECT_GT(h.calls,1); EXPECT_EQ(HftMockerTestAccess::fees(h),400); EXPECT_EQ(HftMockerTestAccess::pos(h,code),200);
}
TEST_F(FeeCommit,UftReentryStopsAfterCommittedFill) {
 rates(normal); tick(); UftProbe u(&r,"u"); u.reenter=true;
 UftMockerTestAccess::order(u,1,code,true,0,1); EXPECT_FALSE(UftMockerTestAccess::run(u,1)); EXPECT_EQ(UftMockerTestAccess::pos(u,code),1); EXPECT_EQ(UftMockerTestAccess::left(u,1),0); EXPECT_EQ(u.orderCalls,0); EXPECT_FALSE(r.fee_error().empty());
}
TEST_F(FeeCommit,CallbackExceptionsRespectHftReceipt) {
 for (int mode=0; mode<3; ++mode) {
  SCOPED_TRACE(mode); HisDataReplayerTestAccess::stop(r); rates(normal); tick(code,"rb2410",100,200);
  HftProbe h(&r,"h"); h.throwBefore=mode==0; h.throwAfter=mode==1; h.throwOrder=mode==2;
  HftMockerTestAccess::order(h,1,code,true,200); EXPECT_FALSE(HftMockerTestAccess::run(h,1));
  double position=HftMockerTestAccess::pos(h,code); EXPECT_EQ(HftMockerTestAccess::left(h,1),200-position);
  EXPECT_EQ(h.calls,1); EXPECT_EQ(h.orderCalls,mode==2 ? 1 : 0); EXPECT_FALSE(r.fee_error().empty());
  if(mode==0) { EXPECT_EQ(position,0); EXPECT_TRUE(HftMockerTestAccess::trades(h).empty()); } else EXPECT_GT(position,0);
 }
}
TEST_F(FeeCommit,CallbackExceptionsRespectUftReceipt) {
 for (int mode=0; mode<3; ++mode) {
  SCOPED_TRACE(mode); HisDataReplayerTestAccess::stop(r); rates(normal); tick(code,"rb2410",100,200);
  UftProbe u(&r,"u"); u.throwBefore=mode==0; u.throwAfter=mode==1; u.throwOrder=mode==2;
  UftMockerTestAccess::order(u,1,code,true,0,200); EXPECT_FALSE(UftMockerTestAccess::run(u,1));
  double position=UftMockerTestAccess::pos(u,code); EXPECT_EQ(UftMockerTestAccess::left(u,1),200-position);
  EXPECT_EQ(u.calls,1); EXPECT_EQ(u.orderCalls,mode==2 ? 1 : 0); EXPECT_FALSE(r.fee_error().empty());
  if(mode==0) { EXPECT_EQ(position,0); EXPECT_TRUE(UftMockerTestAccess::trades(u).empty()); } else EXPECT_GT(position,0);
 }
}
TEST_F(FeeCommit,ExpHftNotifiesOnlyCommittedTrade) {
 rates(normal); tick(); expTradeNotifications=expOrderNotifications=0; ExpHftMocker h(&r,"exp");
 HftMockerTestAccess::order(h,1,code,true,1); EXPECT_TRUE(HftMockerTestAccess::run(h,1));
 EXPECT_EQ(expTradeNotifications,1); EXPECT_EQ(expOrderNotifications,1); EXPECT_EQ(HftMockerTestAccess::pos(h,code),1);
 // A direct continuous-code call lacks an event contract and is rejected by
 // the real ExpHftMocker override without emitting an external notification.
 h.on_trade(2,code,true,1,100,""); EXPECT_EQ(expTradeNotifications,1);
 EXPECT_EQ(HftMockerTestAccess::pos(h,code),1); EXPECT_FALSE(r.fee_error().empty());
}
TEST_F(FeeCommit,UftMissingAndHftOverflowDoNotCommit) {
 rates(R"({"version":2,"contracts":{},"products":{}})"); tick(); UftProbe u(&r,"u");
 UftMockerTestAccess::order(u,1,code,true,0,2); EXPECT_FALSE(UftMockerTestAccess::run(u,1));
 EXPECT_EQ(UftMockerTestAccess::left(u,1),2); EXPECT_EQ(UftMockerTestAccess::pos(u,code),0); EXPECT_EQ(UftMockerTestAccess::fees(u),0); EXPECT_TRUE(UftMockerTestAccess::trades(u).empty()); EXPECT_EQ(u.calls,0); EXPECT_EQ(u.orderCalls,0);
 HisDataReplayerTestAccess::stop(r); rates(R"({"version":2,"contracts":{"SHFE.rb2410":{"open":1e308,"close":1,"closetoday":1,"byvolume":false}},"products":{}})"); tick(); HftProbe h(&r,"h");
 HftMockerTestAccess::order(h,1,code,true,2); EXPECT_FALSE(HftMockerTestAccess::run(h,1));
 EXPECT_EQ(HftMockerTestAccess::left(h,1),2); EXPECT_EQ(HftMockerTestAccess::pos(h,code),0); EXPECT_EQ(HftMockerTestAccess::fees(h),0); EXPECT_TRUE(HftMockerTestAccess::trades(h).empty()); EXPECT_EQ(h.calls,0); EXPECT_EQ(h.orderCalls,0);
}
TEST_F(FeeCommit,PrecommitStateChangeInvalidatesPlan) {
 rates(normal); tick(); HftProbe h(&r,"h"); h.before=[&] { HftMockerTestAccess::changeFund(h); };
 HftMockerTestAccess::order(h,1,code,true,2); EXPECT_FALSE(HftMockerTestAccess::run(h,1)); EXPECT_EQ(HftMockerTestAccess::left(h,1),2); EXPECT_EQ(HftMockerTestAccess::pos(h,code),0); EXPECT_TRUE(HftMockerTestAccess::trades(h).empty()); EXPECT_FALSE(r.fee_error().empty());
 HisDataReplayerTestAccess::stop(r); rates(normal); tick(); UftProbe u(&r,"u"); u.before=[&] { UftMockerTestAccess::changeFund(u); };
 UftMockerTestAccess::order(u,1,code,true,0,2); EXPECT_FALSE(UftMockerTestAccess::run(u,1)); EXPECT_EQ(UftMockerTestAccess::left(u,1),2); EXPECT_EQ(UftMockerTestAccess::pos(u,code),0); EXPECT_TRUE(UftMockerTestAccess::trades(u).empty()); EXPECT_FALSE(r.fee_error().empty());
}
TEST_F(FeeCommit,OtherMockerNestedDirectTradeKeepsOuterReceipt) {
 rates(normal); tick(); HftProbe h(&r,"h"); UftProbe u(&r,"u");
 h.after=[&] { u.on_trade(77,"SHFE.rb.2405",true,0,1,100); };
 HftMockerTestAccess::order(h,1,code,true,1); EXPECT_TRUE(HftMockerTestAccess::run(h,1));
 EXPECT_EQ(HftMockerTestAccess::pos(h,code),1); EXPECT_EQ(UftMockerTestAccess::pos(u,"SHFE.rb.2405"),1); EXPECT_TRUE(r.fee_error().empty());
}
TEST_F(FeeCommit,ZeroLiquidityDoesNotCreateTradeOrFeeError) {
 rates(normal); tick(code,"rb2410",100,0); HftProbe h(&r,"h"); UftProbe u(&r,"u");
 HftMockerTestAccess::order(h,1,code,true,1); UftMockerTestAccess::order(u,1,code,true,0,1);
 EXPECT_FALSE(HftMockerTestAccess::run(h,1)); EXPECT_FALSE(UftMockerTestAccess::run(u,1));
 EXPECT_EQ(HftMockerTestAccess::left(h,1),1); EXPECT_EQ(UftMockerTestAccess::left(u,1),1);
 EXPECT_EQ(h.calls,0); EXPECT_EQ(u.calls,0); EXPECT_TRUE(r.fee_error().empty());
}
TEST_F(FeeCommit,MissingEventPhysicalIdentityFailsClosed) {
 rates(normal); tick(code,"",100,10); HftProbe h(&r,"h");
 HftMockerTestAccess::order(h,1,code,true,1); EXPECT_FALSE(HftMockerTestAccess::run(h,1));
 EXPECT_EQ(HftMockerTestAccess::left(h,1),1); EXPECT_EQ(h.calls,0); EXPECT_FALSE(r.fee_error().empty());
 HisDataReplayerTestAccess::stop(r); rates(normal); tick(code,"",100,10); UftProbe u(&r,"u");
 UftMockerTestAccess::order(u,1,code,true,0,1); EXPECT_FALSE(UftMockerTestAccess::run(u,1));
 EXPECT_EQ(UftMockerTestAccess::left(u,1),1); EXPECT_EQ(u.calls,0); EXPECT_FALSE(r.fee_error().empty());
}
TEST_F(FeeCommit,PostcommitStateChangeStopsRemainingSplit) {
 rates(normal); tick(code,"rb2410",100,200); HftProbe h(&r,"h"); h.after=[&] { HftMockerTestAccess::changeFund(h); };
 HftMockerTestAccess::order(h,1,code,true,200); EXPECT_FALSE(HftMockerTestAccess::run(h,1)); EXPECT_EQ(h.calls,1);
 EXPECT_GT(HftMockerTestAccess::pos(h,code),0); EXPECT_EQ(HftMockerTestAccess::left(h,1),200-HftMockerTestAccess::pos(h,code)); EXPECT_FALSE(r.fee_error().empty());
 HisDataReplayerTestAccess::stop(r); rates(normal); tick(code,"rb2410",100,200); UftProbe u(&r,"u"); u.after=[&] { UftMockerTestAccess::changeFund(u); };
 UftMockerTestAccess::order(u,1,code,true,0,200); EXPECT_FALSE(UftMockerTestAccess::run(u,1)); EXPECT_EQ(u.calls,1);
 EXPECT_GT(UftMockerTestAccess::pos(u,code),0); EXPECT_EQ(UftMockerTestAccess::left(u,1),200-UftMockerTestAccess::pos(u,code)); EXPECT_FALSE(r.fee_error().empty());
}
TEST_F(FeeCommit,ChangedOrderRemainingInvalidatesReceipt) {
 rates(normal); tick(); HftProbe h(&r,"h"); h.before=[&] { HftMockerTestAccess::changeLeft(h,1,0); };
 HftMockerTestAccess::order(h,1,code,true,1); EXPECT_FALSE(HftMockerTestAccess::run(h,1));
 EXPECT_EQ(HftMockerTestAccess::left(h,1),0); EXPECT_EQ(HftMockerTestAccess::pos(h,code),0); EXPECT_TRUE(HftMockerTestAccess::trades(h).empty()); EXPECT_FALSE(r.fee_error().empty());
 HisDataReplayerTestAccess::stop(r); rates(normal); tick(); UftProbe u(&r,"u"); u.after=[&] { UftMockerTestAccess::changeLeft(u,1,0); };
 UftMockerTestAccess::order(u,1,code,true,0,1); EXPECT_FALSE(UftMockerTestAccess::run(u,1));
 EXPECT_EQ(UftMockerTestAccess::left(u,1),0); EXPECT_EQ(UftMockerTestAccess::pos(u,code),1); EXPECT_EQ(u.orderCalls,0); EXPECT_FALSE(r.fee_error().empty());
 HisDataReplayerTestAccess::stop(r); rates(normal); tick(); HftProbe h2(&r,"h2"); h2.after=[&] { HftMockerTestAccess::changeLeft(h2,1,0); };
 HftMockerTestAccess::order(h2,1,code,true,1); EXPECT_FALSE(HftMockerTestAccess::run(h2,1));
 EXPECT_EQ(HftMockerTestAccess::left(h2,1),0); EXPECT_EQ(HftMockerTestAccess::pos(h2,code),1); EXPECT_EQ(h2.orderCalls,0); EXPECT_FALSE(r.fee_error().empty());
 HisDataReplayerTestAccess::stop(r); rates(normal); tick(); UftProbe u2(&r,"u2"); u2.before=[&] { UftMockerTestAccess::changeLeft(u2,1,0); };
 UftMockerTestAccess::order(u2,1,code,true,0,1); EXPECT_FALSE(UftMockerTestAccess::run(u2,1));
 EXPECT_EQ(UftMockerTestAccess::left(u2,1),0); EXPECT_EQ(UftMockerTestAccess::pos(u2,code),0); EXPECT_TRUE(UftMockerTestAccess::trades(u2).empty()); EXPECT_FALSE(r.fee_error().empty());
}
TEST_F(FeeCommit,OrderCallbackChangeStopsRemainingSplits) {
 rates(normal); tick(code,"rb2410",100,200); HftProbe h(&r,"h"); h.orderAfter=[&] { HftMockerTestAccess::changeLeft(h,1,0); };
 HftMockerTestAccess::order(h,1,code,true,200); EXPECT_FALSE(HftMockerTestAccess::run(h,1));
 EXPECT_EQ(h.calls,1); EXPECT_EQ(h.orderCalls,1); EXPECT_EQ(HftMockerTestAccess::left(h,1),0); EXPECT_FALSE(r.fee_error().empty());
 HisDataReplayerTestAccess::stop(r); rates(normal); tick(code,"rb2410",100,200); UftProbe u(&r,"u"); u.orderAfter=[&] { UftMockerTestAccess::changeLeft(u,1,0); };
 UftMockerTestAccess::order(u,1,code,true,0,200); EXPECT_FALSE(UftMockerTestAccess::run(u,1));
 EXPECT_EQ(u.calls,1); EXPECT_EQ(u.orderCalls,1); EXPECT_EQ(UftMockerTestAccess::left(u,1),0); EXPECT_FALSE(r.fee_error().empty());
}
TEST_F(FeeCommit,NonFiniteRemainingQuantityFailsWithoutCommit) {
 rates(normal); tick(); HftProbe h(&r,"h"); HftMockerTestAccess::order(h,1,code,true,std::numeric_limits<double>::quiet_NaN());
 EXPECT_FALSE(HftMockerTestAccess::run(h,1)); EXPECT_EQ(h.calls,0); EXPECT_TRUE(HftMockerTestAccess::trades(h).empty()); EXPECT_FALSE(r.fee_error().empty());
 HisDataReplayerTestAccess::stop(r); rates(normal); tick(); UftProbe u(&r,"u"); UftMockerTestAccess::order(u,1,code,true,0,std::numeric_limits<double>::quiet_NaN());
 EXPECT_FALSE(UftMockerTestAccess::run(u,1)); EXPECT_EQ(u.calls,0); EXPECT_TRUE(UftMockerTestAccess::trades(u).empty()); EXPECT_FALSE(r.fee_error().empty());
}
