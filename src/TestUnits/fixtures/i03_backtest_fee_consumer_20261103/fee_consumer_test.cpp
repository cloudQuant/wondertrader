#include <gtest/gtest.h>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "WtBtCore/WtHelper.h"
#define private public
#define protected public
#include "WtBtCore/HisDataReplayer.h"
#include "WtBtCore/CtaMocker.h"
#include "WtBtCore/SelMocker.h"
#undef protected
#undef private

namespace {
const char* code = "SHFE.rb.2405";
const char* code2 = "SHFE.rb.2409";
const char* hot = "SHFE.rb.HOT";
const char* productFees = R"({"version":2,"contracts":{"SHFE.rb2405":{"open":2,"close":4,"closetoday":6,"byvolume":true}},"products":{"SHFE.rb":{"open":3,"close":5,"closetoday":7,"byvolume":true}}})";
const char* zeroFees = R"({"version":2,"contracts":{},"products":{"SHFE.rb":{"open":0,"close":0,"closetoday":0,"byvolume":true}}})";
const char* mixedFees = R"({"version":2,"contracts":{},"products":{"SHFE.rb":{"open":1,"close":2,"closetoday":3,"byvolume":true}}})";
const char* failCloseFees = R"({"version":2,"contracts":{},"products":{"SHFE.rb":{"open":1,"close":2,"closetoday":1e307,"byvolume":true}}})";

template<class Mocker> class FeeConsumer : public ::testing::Test {
public:
 HisDataReplayer rp;
 WTSCommodityInfo* comm = nullptr;
 WTSContractInfo *first = nullptr, *second = nullptr;
 void SetUp() override {
  rp._cur_date = rp._cur_tdate = 20240102; rp._cur_time = 93000; rp._cur_secs = 0;
  comm = WTSCommodityInfo::create("rb", "rb", "SHFE", "TRADING", "CHINA");
  ASSERT_NE(comm, nullptr); comm->setVolScale(10); comm->setPriceTick(1); comm->setTradingMode(TM_Both);
  WTSSessionInfo* session = WTSSessionInfo::create("TRADING", "all day");
  ASSERT_NE(session, nullptr); session->addTradingSection(0, 0); comm->setSessionInfo(session); session->release();
  rp._bd_mgr._trading_days["CHINA"] = TradingDayTpl{};
  rp._bd_mgr._fullpid_map->add(comm->getFullPid(), comm, false);
  first = addContract("rb2405"); second = addContract("rb2409");
  ASSERT_NE(first, nullptr); ASSERT_NE(second, nullptr);
 }
 WTSContractInfo* addContract(const char* raw) {
  auto* ct = WTSContractInfo::create(raw, raw, "SHFE", "rb");
  if (!ct) return nullptr;
  ct->setDates(20200101, 20301231); ct->setCommInfo(comm); ct->setVolScale(10);
  rp._bd_mgr._fullcode_map->add(ct->getFullCode(), ct, false); return ct;
 }
 void schedule(const char* json) { std::string error; ASSERT_TRUE(rp._fee_schedule.loadJson(json, error)) << error; }
 void tick(Mocker& mocker, const char* subscription, WTSContractInfo* contract, double px = 100) {
  WTSTickStruct data{}; wt_strcpy(data.code, contract ? contract->getCode() : "rb2405");
  data.action_date = data.trading_date = rp._cur_tdate; data.action_time = 93000000; data.price = px;
  WTSTickData* tick = WTSTickData::create(data); tick->setContractInfo(contract);
  mocker.handle_tick(subscription, tick, 3); tick->release();
 }
 void enter(Mocker& mocker, const char* sub, WTSContractInfo* contract, double target) {
  tick(mocker, sub, contract); mocker.stra_set_position(sub, target, "fee-test");
  ASSERT_EQ(1u, mocker._sig_map.count(sub)); tick(mocker, sub, contract);
  ASSERT_TRUE(rp.fee_error().empty()) << rp.fee_error();
 }
};
using MockerTypes = ::testing::Types<CtaMocker, SelMocker>;
TYPED_TEST_SUITE(FeeConsumer, MockerTypes);

TYPED_TEST(FeeConsumer, ContractOverrideAndProductFallback) {
 this->schedule(productFees); TypeParam m(&this->rp, "contract-product");
 this->enter(m, code, this->first, 2); EXPECT_DOUBLE_EQ(4, m._fund_info._total_fees);
 this->enter(m, code2, this->second, 1); EXPECT_DOUBLE_EQ(7, m._fund_info._total_fees);
 EXPECT_TRUE(m._sig_map.empty()); EXPECT_DOUBLE_EQ(2, m._pos_map[code]._volume);
}
TYPED_TEST(FeeConsumer, ConfiguredZeroIsValid) {
 this->schedule(zeroFees); TypeParam m(&this->rp, "zero");
 this->enter(m, code, this->first, 1); EXPECT_TRUE(this->rp.fee_error().empty());
 EXPECT_DOUBLE_EQ(0, m._fund_info._total_fees); EXPECT_DOUBLE_EQ(1, m._pos_map[code]._volume);
}
TYPED_TEST(FeeConsumer, MissingFeeKeepsSignalAndNoLedgerOrTrade) {
 this->schedule(R"({"version":2,"contracts":{},"products":{}})"); TypeParam m(&this->rp, "missing");
 this->tick(m, code, this->first); m.stra_set_position(code, 1, "missing"); ASSERT_EQ(1u, m._sig_map.count(code));
 const auto trades = m._trade_logs.str(); const auto closes = m._close_logs.str();
 this->tick(m, code, this->first);
 EXPECT_EQ(1u, m._sig_map.count(code)); EXPECT_EQ(0u, m._pos_map.count(code));
 EXPECT_DOUBLE_EQ(0, m._fund_info._total_fees); EXPECT_DOUBLE_EQ(0, m._fund_info._total_profit);
 EXPECT_EQ(trades, m._trade_logs.str()); EXPECT_EQ(closes, m._close_logs.str()); EXPECT_FALSE(this->rp.fee_error().empty());
}
TYPED_TEST(FeeConsumer, MixedCloseAndReverseFees) {
 this->schedule(mixedFees); TypeParam m(&this->rp, "mixed");
 this->enter(m, code, this->first, 1);
 this->rp._cur_date = this->rp._cur_tdate = 20240103;
 this->enter(m, code, this->first, 3); EXPECT_DOUBLE_EQ(3, m._fund_info._total_fees);
 this->enter(m, code, this->first, -1); EXPECT_TRUE(this->rp.fee_error().empty());
 EXPECT_DOUBLE_EQ(-1, m._pos_map[code]._volume); EXPECT_DOUBLE_EQ(12, m._fund_info._total_fees);
 ASSERT_EQ(1u, m._pos_map[code]._details.size()); EXPECT_FALSE(m._pos_map[code]._details[0]._long);
}
TYPED_TEST(FeeConsumer, LaterSegmentOverflowDoesNotPartiallyCommit) {
 this->schedule(mixedFees); TypeParam m(&this->rp, "overflow");
 this->enter(m, code, this->first, 1); this->rp._cur_date = this->rp._cur_tdate = 20240103;
 this->enter(m, code, this->first, 3); this->schedule(failCloseFees);
 const auto beforeTrades = m._trade_logs.str(); const auto beforeCloses = m._close_logs.str();
 const double fees = m._fund_info._total_fees, profit = m._fund_info._total_profit;
 m.stra_set_position(code, -1, "overflow"); ASSERT_EQ(1u, m._sig_map.count(code)); this->tick(m, code, this->first);
 EXPECT_EQ(1u, m._sig_map.count(code)); EXPECT_DOUBLE_EQ(3, m._pos_map[code]._volume);
 ASSERT_EQ(2u, m._pos_map[code]._details.size()); EXPECT_DOUBLE_EQ(1, m._pos_map[code]._details[0]._volume);
 EXPECT_DOUBLE_EQ(2, m._pos_map[code]._details[1]._volume); EXPECT_DOUBLE_EQ(fees, m._fund_info._total_fees);
 EXPECT_DOUBLE_EQ(profit, m._fund_info._total_profit); EXPECT_EQ(beforeTrades, m._trade_logs.str());
 EXPECT_EQ(beforeCloses, m._close_logs.str()); EXPECT_FALSE(this->rp.fee_error().empty());
}
TYPED_TEST(FeeConsumer, ContinuousFillUsesContractFromSameTick) {
 this->schedule(R"({"version":2,"contracts":{"SHFE.rb2409":{"open":7,"close":0,"closetoday":0,"byvolume":true}},"products":{"SHFE.rb":{"open":2,"close":0,"closetoday":0,"byvolume":true}}})");
 TypeParam m(&this->rp, "hot"); this->enter(m, hot, this->second, 1);
 EXPECT_TRUE(this->rp.fee_error().empty()); EXPECT_DOUBLE_EQ(7, m._fund_info._total_fees);
}
TYPED_TEST(FeeConsumer, ContinuousReplayDispatchUsesEachEventsPhysicalContract) {
 this->schedule(R"({"version":2,"contracts":{"SHFE.rb2405":{"open":2,"close":0,"closetoday":0,"byvolume":true},"SHFE.rb2409":{"open":7,"close":0,"closetoday":0,"byvolume":true}},"products":{}})");
 TypeParam m(&this->rp, "replay-hot"); this->tick(m, hot, this->first); m.stra_set_position(hot, 1, "first");
 struct Sink : IDataSink {
  TypeParam* mocker; unsigned calls = 0;
  explicit Sink(TypeParam* m) : mocker(m) {}
  void handle_tick(const char* code, WTSTickData* tick, uint32_t type) override {
   mocker->handle_tick(code, tick, type);
   if (++calls == 1) mocker->stra_set_position(code, 2, "second");
  }
  void handle_bar_close(const char*, const char*, uint32_t, WTSBarStruct*) override {}
  void handle_schedule(uint32_t, uint32_t) override {}
  void handle_init() override {}
  void handle_session_begin(uint32_t) override {}
  void handle_session_end(uint32_t) override {}
  void handle_replay_done() override {}
 } sink(&m);
 WTSTickStruct a{}, b{}; wt_strcpy(a.code, "rb2405"); wt_strcpy(b.code, "rb2409");
 wt_strcpy(a.exchg, "SHFE"); wt_strcpy(b.exchg, "SHFE");
 a.action_date = b.action_date = a.trading_date = b.trading_date = 20240102;
 a.action_time = 93000000; b.action_time = 93100000; a.price = b.price = 100;
 this->rp._listener = &sink; this->rp._tick_enabled = true; this->rp._running.store(true);
 this->rp._terminated.store(false); this->rp._dispatched_tick_subscriptions.insert(hot);
 auto& cache = this->rp._replay_ticks_cache[hot][20240102]; cache._items = {a, b}; cache._count = 2; cache._cursor = UINT_MAX;
 HisDataReplayer::ReplaySubscriptionSnapshot snapshot; snapshot.ticks.insert(hot); bool did = false;
 const bool replayed = this->rp.replayHftInterval(20240102090000000ULL, 20240102100000000ULL, did, snapshot);
 this->rp._running.store(false);
 EXPECT_TRUE(replayed); EXPECT_TRUE(did); EXPECT_EQ(2u, sink.calls);
 EXPECT_TRUE(this->rp.fee_error().empty()) << this->rp.fee_error();
 EXPECT_DOUBLE_EQ(2, m._pos_map[hot]._volume); EXPECT_DOUBLE_EQ(9, m._fund_info._total_fees);
}
TYPED_TEST(FeeConsumer, ContinuousWithoutTickContractIsRejected) {
 this->schedule(mixedFees); TypeParam m(&this->rp, "hot-missing");
 this->tick(m, hot, nullptr); m.stra_set_position(hot, 1, "missing-contract"); ASSERT_EQ(1u, m._sig_map.count(hot));
 this->tick(m, hot, nullptr); EXPECT_EQ(1u, m._sig_map.count(hot)); EXPECT_EQ(0u, m._pos_map.count(hot));
 EXPECT_FALSE(this->rp.fee_error().empty()); EXPECT_TRUE(m._trade_logs.str().empty());
}
TYPED_TEST(FeeConsumer, PartialScheduleDoesNotBecomeZeroFee) {
 std::string error;
 EXPECT_FALSE(this->rp._fee_schedule.loadJson(R"({"version":2,"contracts":{},"products":{"SHFE.rb":{"open":1,"close":2,"byvolume":true}}})", error));
 TypeParam m(&this->rp, "partial"); this->tick(m, code, this->first); m.stra_set_position(code, 1, "partial");
 ASSERT_EQ(1u, m._sig_map.count(code)); this->tick(m, code, this->first);
 EXPECT_EQ(1u, m._sig_map.count(code)); EXPECT_EQ(0u, m._pos_map.count(code));
 EXPECT_FALSE(this->rp.fee_error().empty()); EXPECT_TRUE(m._trade_logs.str().empty());
}
TYPED_TEST(FeeConsumer, ExistingFundPlusFeeOverflowDoesNotCommit) {
 this->schedule(R"({"version":2,"contracts":{},"products":{"SHFE.rb":{"open":1e306,"close":0,"closetoday":0,"byvolume":true}}})");
 TypeParam m(&this->rp, "fund-overflow"); m._fund_info._total_fees = (std::numeric_limits<double>::max)();
 this->tick(m, code, this->first); m.stra_set_position(code, 1, "overflow");
 ASSERT_EQ(1u, m._sig_map.count(code)); this->tick(m, code, this->first);
 EXPECT_EQ(1u, m._sig_map.count(code)); EXPECT_EQ(0u, m._pos_map.count(code));
 EXPECT_EQ((std::numeric_limits<double>::max)(), m._fund_info._total_fees);
 EXPECT_FALSE(this->rp.fee_error().empty()); EXPECT_TRUE(m._trade_logs.str().empty());
}
TYPED_TEST(FeeConsumer, LargeFiniteZeroFeeReversalStaysFinite) {
 this->schedule(zeroFees); TypeParam m(&this->rp, "large-reversal");
 this->enter(m, code, this->first, 1e200);
 this->enter(m, code, this->first, -1e200);
 ASSERT_TRUE(this->rp.fee_error().empty()) << this->rp.fee_error();
 ASSERT_EQ(1u, m._pos_map[code]._details.size());
 EXPECT_DOUBLE_EQ(-1e200, m._pos_map[code]._volume);
 EXPECT_TRUE(std::isfinite(m._pos_map[code]._details[0]._volume));
 EXPECT_DOUBLE_EQ(1e200, m._pos_map[code]._details[0]._volume);
 EXPECT_DOUBLE_EQ(0, m._fund_info._total_fees);
}

class ConditionCta : public CtaMocker {
public:
 using CtaMocker::CtaMocker;
 int triggered = 0;
 void on_condition_triggered(const char*, double, double, const char*) override { ++triggered; }
};
class ConditionFeeConsumer : public FeeConsumer<ConditionCta> {};
TEST_F(ConditionFeeConsumer, FailedTriggeredSignalDoesNotNotifySuccess) {
 schedule(R"({"version":2,"contracts":{},"products":{}})");
 ConditionCta m(&rp, "condition"); tick(m, code, first);
 m.append_signal(code, 1, "condition", 0, 2); ASSERT_EQ(1u, m._sig_map.count(code));
 tick(m, code, first);
 EXPECT_EQ(0, m.triggered); EXPECT_EQ(1u, m._sig_map.count(code));
 EXPECT_EQ(0u, m._pos_map.count(code)); EXPECT_FALSE(rp.fee_error().empty());
}
} // namespace
