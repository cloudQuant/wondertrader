#include "../../coverage/t_TraderCTP/t25_traderctp_gap_test.cpp"
#include <set>
#include <limits>

namespace {
class RecordingApi : public StubApi {
public:
 std::mutex mutex;
 std::vector<std::string> refs;
 std::function<void(CThostFtdcInputOrderField*)> inside;
 int result = 0;
 int ReqOrderInsert(CThostFtdcInputOrderField* req, int) override {
  { std::lock_guard<std::mutex> lock(mutex); refs.emplace_back(req->OrderRef); }
  if (inside) inside(req);
  return result;
 }
};
WTSEntrust* order() { return WTSEntrust::create(kCodeAg, 1, 7000.0, kExchgAg); }
}

TEST_F(T25TraderCTPTest, I03IdentityReservedIdAndEmptyTagStayAligned) {
 bringToReady(); char id[64]{}; ASSERT_TRUE(trader_.makeEntrustID(id, sizeof(id)));
 WTSEntrust* e = order(); e->setEntrustID("12345#67#1001");
 ASSERT_EQ(0, trader_.orderInsert(e)); EXPECT_STREQ(id, e->getEntrustID());
 EXPECT_EQ(std::string("ctp-auto:") + id, e->getUserTag()); EXPECT_STREQ("1001", api_.order_.OrderRef);
 EXPECT_EQ(e->getUserTag(), trader_.eidValue(id)); EXPECT_EQ(-1, trader_.orderInsert(e)); EXPECT_EQ(1, api_.n_insert_.load()); e->release();
}
TEST_F(T25TraderCTPTest, I03IdentityMissingIdIsAllocatedAndSuppliedTagPreserved) {
 bringToReady(); WTSEntrust* a = order(); ASSERT_EQ(0, trader_.orderInsert(a));
 EXPECT_STREQ("012345#0000000067#001001", a->getEntrustID()); EXPECT_STREQ("1001", api_.order_.OrderRef);
 WTSEntrust* b = order(); b->setUserTag("otp.owner.77"); ASSERT_EQ(0, trader_.orderInsert(b));
 EXPECT_STREQ("otp.owner.77", b->getUserTag()); EXPECT_STREQ("1002", api_.order_.OrderRef);
 EXPECT_NE(std::string(a->getEntrustID()), std::string(b->getEntrustID())); a->release(); b->release();
}
TEST_F(T25TraderCTPTest, I03IdentityRejectsBadUnknownAndWrongSession) {
 bringToReady(); char reserved[64]{}; ASSERT_TRUE(trader_.makeEntrustID(reserved, sizeof(reserved)));
 for (const char* bad : {"12345#67#1001x", "12345#67#-1", "12345#67#2147483648", "12345#68#1001", "12345#67#1002", "12345#67#0", "#67#1001", "12345#67#1001#4"}) {
  WTSEntrust* e = order(); e->setEntrustID(bad); EXPECT_EQ(-1, trader_.orderInsert(e)) << bad; e->release();
 }
 EXPECT_EQ(0, api_.n_insert_.load()); WTSEntrust* ok = order(); ok->setEntrustID(reserved); EXPECT_EQ(0, trader_.orderInsert(ok)); ok->release();
}
TEST_F(T25TraderCTPTest, I03IdentitySmallBufferDoesNotReserve) {
 bringToReady(); char small[4] = {'x','x','x','x'}; EXPECT_FALSE(trader_.makeEntrustID(small, sizeof(small)));
 EXPECT_EQ('\0', small[0]); EXPECT_EQ('x', small[3]); EXPECT_EQ(1000u, trader_.orderRef());
}
TEST_F(T25TraderCTPTest, I03IdentityExistingCacheKeyIsNeverOverwritten) {
 bringToReady();
 for (const char* stored : {"", "historical"}) {
  char id[64]{}; ASSERT_TRUE(trader_.makeEntrustID(id,sizeof(id))); ASSERT_TRUE(trader_.seedEid(id,stored));
  const uint32_t count=trader_.eidSize(); WTSEntrust* e=order(); e->setEntrustID(id); e->setUserTag("new-tag");
  EXPECT_EQ(-1,trader_.orderInsert(e)); EXPECT_EQ(count,trader_.eidSize()); EXPECT_EQ(stored,trader_.eidValue(id));
  EXPECT_EQ(0,api_.n_insert_.load()); e->release();
 }
}
TEST_F(T25TraderCTPTest, I03IdentityConcurrentAllocationAndDuplicateSubmission) {
 bringToReady(); RecordingApi recorder; trader_.attach(&recorder); constexpr int N=32;
 std::vector<WTSEntrust*> entries; for (int i=0;i<N;++i) entries.push_back(order());
 std::vector<int> result(N, -2); std::vector<std::thread> threads;
 for (int i=0;i<N;++i) threads.emplace_back([&,i]{result[i] = trader_.orderInsert(entries[i]);});
 for (auto& thread : threads) thread.join();
 std::set<std::string> ids, tags, refs; for (int i=0;i<N;++i) { EXPECT_EQ(0,result[i]); ids.insert(entries[i]->getEntrustID()); tags.insert(entries[i]->getUserTag()); }
 for (auto& ref : recorder.refs) refs.insert(ref);
 EXPECT_EQ(static_cast<size_t>(N), ids.size()); EXPECT_EQ(static_cast<size_t>(N), tags.size()); EXPECT_EQ(static_cast<size_t>(N), refs.size());
 for (auto* e : entries) { EXPECT_EQ(-1, trader_.orderInsert(e)); e->release(); }
 EXPECT_EQ(static_cast<size_t>(N), recorder.refs.size()); trader_.attach(&api_);
}
TEST_F(T25TraderCTPTest, I03IdentityFailedSendNeverReusesReference) {
 bringToReady(); api_.ret_=9; WTSEntrust* a=order(); EXPECT_EQ(-1,trader_.orderInsert(a));
 const std::string id=a->getEntrustID(); ASSERT_FALSE(id.empty()); EXPECT_EQ(-1,trader_.orderInsert(a));
 api_.ret_=0; WTSEntrust* b=order(); ASSERT_EQ(0,trader_.orderInsert(b)); EXPECT_NE(id,b->getEntrustID());
 EXPECT_STREQ("1002",api_.order_.OrderRef); a->release(); b->release();
}
TEST_F(T25TraderCTPTest, I03IdentitySyncErrorAndTradeBeforeOrderAreAssociated) {
 bringToReady(); RecordingApi recorder; trader_.attach(&recorder);
 recorder.inside = [&](CThostFtdcInputOrderField* req) {
  CThostFtdcRspInfoField err{}; err.ErrorID=42; strcpy(err.ErrorMsg,"reject"); trader_.OnRspOrderInsert(req,&err,1,true);
  CThostFtdcTradeField trade{}; strcpy(trade.InstrumentID,kCodeAg); strcpy(trade.ExchangeID,kExchgAg);
  strcpy(trade.BrokerID,"9999"); strcpy(trade.InvestorID,"88880001"); strcpy(trade.TradingDay,"20240115");
  strcpy(trade.OrderRef,req->OrderRef); strcpy(trade.OrderSysID,"sys001"); strcpy(trade.TradeDate,"20240115"); strcpy(trade.TradeTime,"09:31:00");
  trade.Volume=1; trade.Price=7000; trader_.OnRtnTrade(&trade);
 };
 WTSEntrust* e=order(); ASSERT_EQ(0,trader_.orderInsert(e)); EXPECT_EQ(1u,sink_.n_rsp_entrust_);
 EXPECT_EQ(e->getEntrustID(),sink_.ent_eid_); EXPECT_EQ(e->getUserTag(),sink_.ent_usertag_);
 EXPECT_EQ(1u,sink_.n_push_trade_); EXPECT_EQ(e->getUserTag(),sink_.trd_usertag_);
 CThostFtdcOrderField ack{}; strcpy(ack.InstrumentID,kCodeAg); strcpy(ack.ExchangeID,kExchgAg); strcpy(ack.OrderRef,"1001");
 strcpy(ack.BrokerID,"9999"); strcpy(ack.InvestorID,"88880001"); strcpy(ack.TradingDay,"20240115");
 strcpy(ack.OrderSysID,"sys001"); strcpy(ack.InsertDate,"20240115"); strcpy(ack.InsertTime,"09:31:00"); ack.FrontID=12345; ack.SessionID=67;
 trader_.OnRtnOrder(&ack); EXPECT_EQ(e->getUserTag(),sink_.ord_usertag_); EXPECT_EQ(e->getEntrustID(),sink_.ord_eid_);
 CThostFtdcTradeField trade{}; strcpy(trade.InstrumentID,kCodeAg); strcpy(trade.ExchangeID,kExchgAg); strcpy(trade.OrderSysID,"sys001");
 strcpy(trade.BrokerID,"9999"); strcpy(trade.InvestorID,"88880001"); strcpy(trade.TradingDay,"20240115");
 strcpy(trade.TradeDate,"20240115"); strcpy(trade.TradeTime,"09:31:00"); trade.Volume=1; trade.Price=7000; trader_.OnRtnTrade(&trade);
 EXPECT_EQ(e->getUserTag(),sink_.trd_usertag_);
 strcpy(trade.OrderRef,"1001"); strcpy(trade.InvestorID,"someone-else"); trader_.OnRtnTrade(&trade);
 EXPECT_TRUE(sink_.trd_usertag_.empty());
 strcpy(trade.InvestorID,"88880001"); trade.TradingDay[0]='\0'; trader_.OnRtnTrade(&trade);
 EXPECT_TRUE(sink_.trd_usertag_.empty()); e->release(); trader_.attach(&api_);
}
TEST_F(T25TraderCTPTest, I03IdentityConflictingOrderSystemKeyDoesNotAttributeTrade) {
 bringToReady(); WTSEntrust* e=order(); ASSERT_EQ(0,trader_.orderInsert(e));
 const char* key="9999#88880001#SHFE#ag2512#20240115#sys001"; ASSERT_TRUE(trader_.seedOid(key,"historical"));
 CThostFtdcOrderField ack{}; strcpy(ack.InstrumentID,kCodeAg); strcpy(ack.ExchangeID,kExchgAg); strcpy(ack.OrderRef,"1001");
 strcpy(ack.BrokerID,"9999"); strcpy(ack.InvestorID,"88880001"); strcpy(ack.TradingDay,"20240115");
 strcpy(ack.OrderSysID,"sys001"); strcpy(ack.InsertDate,"20240115"); strcpy(ack.InsertTime,"09:31:00"); ack.FrontID=12345; ack.SessionID=67;
 trader_.OnRtnOrder(&ack); EXPECT_EQ("historical",trader_.oidValue(key));
 CThostFtdcTradeField trade{}; strcpy(trade.InstrumentID,kCodeAg); strcpy(trade.ExchangeID,kExchgAg); strcpy(trade.OrderSysID,"sys001");
 strcpy(trade.BrokerID,"9999"); strcpy(trade.InvestorID,"88880001"); strcpy(trade.TradingDay,"20240115");
 strcpy(trade.TradeDate,"20240115"); strcpy(trade.TradeTime,"09:31:00"); trade.Volume=1; trade.Price=7000;
 trader_.OnRtnTrade(&trade); EXPECT_TRUE(sink_.trd_usertag_.empty()); e->release();
}
TEST_F(T25TraderCTPTest, I03IdentityLoginCallbackDuringInsertClosesAdmission) {
 bringToReady(); RecordingApi recorder; trader_.attach(&recorder);
 recorder.inside = [&](CThostFtdcInputOrderField*) {
  CThostFtdcRspUserLoginField login{}; login.FrontID=23456; login.SessionID=68; strcpy(login.MaxOrderRef,"5"); strcpy(login.TradingDay,"20240115");
  trader_.OnRspUserLogin(&login,&ok_,999,true);
 };
 WTSEntrust* a=order(); EXPECT_EQ(0,trader_.orderInsert(a)); EXPECT_EQ(TraderCTP::WS_LOGINFAILED,trader_.state());
 WTSEntrust* b=order(); EXPECT_EQ(-1,trader_.orderInsert(b)); EXPECT_EQ(1u,recorder.refs.size()); a->release(); b->release(); trader_.attach(&api_);
}
TEST_F(T25TraderCTPTest, I03IdentitySyncRejectCallbackCanSubmitAnotherOrder) {
 bringToReady(); RecordingApi recorder; trader_.attach(&recorder); WTSEntrust* nested=order();
 struct ReentrantSink : Sink {
  TraderCTP* trader; IBaseDataMgr* manager; WTSEntrust* nested; int nestedResult=-2;
  ReentrantSink(TraderCTP* t, IBaseDataMgr* m, WTSEntrust* n):trader(t),manager(m),nested(n) {}
  IBaseDataMgr* getBaseDataMgr() override { return manager; }
  void onRspEntrust(WTSEntrust*,WTSError*) override { nestedResult=trader->orderInsert(nested); }
 } reentrant(&trader_, &sink_.bd_, nested);
 trader_.registerSpi(&reentrant);
 recorder.inside = [&](CThostFtdcInputOrderField* req) {
  if (std::string(req->OrderRef) == "1001") {
   CThostFtdcRspInfoField err{}; err.ErrorID=42; strcpy(err.ErrorMsg,"reject");
   trader_.OnRspOrderInsert(req,&err,1,true);
  }
 };
 WTSEntrust* outer=order(); EXPECT_EQ(0,trader_.orderInsert(outer)); EXPECT_EQ(0,reentrant.nestedResult);
 ASSERT_EQ(2u,recorder.refs.size()); EXPECT_EQ("1001",recorder.refs[0]); EXPECT_EQ("1002",recorder.refs[1]);
 outer->release(); nested->release(); trader_.registerSpi(&sink_); trader_.attach(&api_);
}

TEST_F(T25TraderCTPTest, I03IdentityReloginKeepsHighWaterAndRejectsOldReservation) {
 bringToReady(); WTSEntrust* sent=order(); ASSERT_EQ(0,trader_.orderInsert(sent)); sent->release();
 char old[64]{}; ASSERT_TRUE(trader_.makeEntrustID(old,sizeof(old)));
 trader_.OnFrontDisconnected(1); ASSERT_EQ(0,trader_.login("88880001","pwd","prod01"));
 CThostFtdcRspAuthenticateField auth{}; trader_.OnRspAuthenticate(&auth,&ok_,api_.last_req_id_,true);
 CThostFtdcRspUserLoginField login{}; login.FrontID=12345; login.SessionID=67; strcpy(login.MaxOrderRef,"2");
 strcpy(login.TradingDay,"20240115"); strcpy(login.BrokerID,"9999"); strcpy(login.UserID,"88880001");
 trader_.OnRspUserLogin(&login,&ok_,api_.last_req_id_,true); ASSERT_EQ(TraderCTP::WS_LOGINED,trader_.state());
 ASSERT_EQ(1u,trader_.drainQueries()); CThostFtdcSettlementInfoConfirmField confirm{}; strcpy(confirm.ConfirmDate,"20240115");
 trader_.OnRspQrySettlementInfoConfirm(&confirm,&ok_,api_.last_req_id_,true); ASSERT_EQ(TraderCTP::WS_ALLREADY,trader_.state());
 WTSEntrust* stale=order(); stale->setEntrustID(old); EXPECT_EQ(-1,trader_.orderInsert(stale)); stale->release();
 char next[64]{}; ASSERT_TRUE(trader_.makeEntrustID(next,sizeof(next))); EXPECT_STREQ("012345#0000000067#001003",next);
 WTSEntrust* fresh=order(); fresh->setEntrustID(next); EXPECT_EQ(0,trader_.orderInsert(fresh)); fresh->release();
}
TEST_F(T25TraderCTPTest, I03IdentityDuplicateReservationOnlyOneConcurrentSend) {
 bringToReady(); RecordingApi recorder; trader_.attach(&recorder);
 char id[64]{}; ASSERT_TRUE(trader_.makeEntrustID(id,sizeof(id))); WTSEntrust* a=order(); WTSEntrust* b=order();
 a->setEntrustID(id); b->setEntrustID(id); int first=-2, second=-2;
 std::thread left([&]{first=trader_.orderInsert(a);}); std::thread right([&]{second=trader_.orderInsert(b);});
 left.join(); right.join(); EXPECT_TRUE((first == 0 && second == -1) || (first == -1 && second == 0));
 EXPECT_EQ(1u,recorder.refs.size()); a->release(); b->release(); trader_.attach(&api_);
}
TEST_F(T25TraderCTPTest, I03IdentityInvalidLoginMetadataClosesAdmission) {
 trader_.attach(&api_); ASSERT_EQ(0,trader_.login("88880001","pwd","prod01"));
 CThostFtdcRspAuthenticateField auth{}; trader_.OnRspAuthenticate(&auth,&ok_,api_.last_req_id_,true);
 CThostFtdcRspUserLoginField login{}; login.FrontID=12345; login.SessionID=67; strcpy(login.MaxOrderRef,"1000x");
 strcpy(login.TradingDay,"20240115"); strcpy(login.BrokerID,"9999"); strcpy(login.UserID,"88880001");
 trader_.OnRspUserLogin(&login,&ok_,api_.last_req_id_,true); EXPECT_EQ(TraderCTP::WS_LOGINFAILED,trader_.state());
 char id[64]{}; EXPECT_FALSE(trader_.makeEntrustID(id,sizeof(id))); WTSEntrust* e=order();
 EXPECT_EQ(-1,trader_.orderInsert(e)); EXPECT_EQ(0,api_.n_insert_.load()); e->release();
}
TEST_F(T25TraderCTPTest, I03IdentityReferenceExhaustionDoesNotWrap) {
 trader_.attach(&api_); ASSERT_EQ(0,trader_.login("88880001","pwd","prod01"));
 CThostFtdcRspAuthenticateField auth{}; trader_.OnRspAuthenticate(&auth,&ok_,api_.last_req_id_,true);
 CThostFtdcRspUserLoginField login{}; login.FrontID=12345; login.SessionID=67; strcpy(login.MaxOrderRef,"2147483646");
 strcpy(login.TradingDay,"20240115"); strcpy(login.BrokerID,"9999"); strcpy(login.UserID,"88880001");
 trader_.OnRspUserLogin(&login,&ok_,api_.last_req_id_,true); ASSERT_EQ(TraderCTP::WS_LOGINED,trader_.state());
 ASSERT_EQ(1u,trader_.drainQueries()); CThostFtdcSettlementInfoConfirmField confirm{}; strcpy(confirm.ConfirmDate,"20240115");
 trader_.OnRspQrySettlementInfoConfirm(&confirm,&ok_,api_.last_req_id_,true); ASSERT_EQ(TraderCTP::WS_ALLREADY,trader_.state());
 char last[64]{}; ASSERT_TRUE(trader_.makeEntrustID(last,sizeof(last))); EXPECT_STREQ("012345#0000000067#2147483647",last);
 char exhausted[64]{}; EXPECT_FALSE(trader_.makeEntrustID(exhausted,sizeof(exhausted)));
 WTSEntrust* allocated=order(); EXPECT_EQ(-1,trader_.orderInsert(allocated)); EXPECT_EQ(0,api_.n_insert_.load()); allocated->release();
 WTSEntrust* final=order(); final->setEntrustID(last); EXPECT_EQ(0,trader_.orderInsert(final)); EXPECT_STREQ("2147483647",api_.order_.OrderRef); final->release();
}
