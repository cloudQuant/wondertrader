// BUG-L03-002 consumer: CTA/SEL backtest slippage uses the commodity price tick
// as unit and (ratio mode) as divisor. A zero tick must be rejected before any
// ledger change instead of producing a NaN fill or silently dropping slippage.
// Real HisDataReplayer + CtaMocker/SelMocker; same harness style as
// fixtures/i03_backtest_fee_consumer_20261103.
#include <gtest/gtest.h>
#include <cmath>
#include <string>
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
const char* zeroFees = R"({"version":2,"contracts":{},"products":{"SHFE.rb":{"open":0,"close":0,"closetoday":0,"byvolume":true}}})";

template<class Mocker> class TickDivisor : public ::testing::Test {
public:
 HisDataReplayer rp;
 WTSCommodityInfo* comm = nullptr;
 WTSContractInfo* ct = nullptr;
 void build(double priceTick) {
  rp._cur_date = rp._cur_tdate = 20240102; rp._cur_time = 93000; rp._cur_secs = 0;
  comm = WTSCommodityInfo::create("rb", "rb", "SHFE", "TRADING", "CHINA");
  ASSERT_NE(comm, nullptr); comm->setVolScale(10); comm->setPriceTick(priceTick); comm->setTradingMode(TM_Both);
  WTSSessionInfo* session = WTSSessionInfo::create("TRADING", "all day");
  ASSERT_NE(session, nullptr); session->addTradingSection(0, 0); comm->setSessionInfo(session); session->release();
  rp._bd_mgr._trading_days["CHINA"] = TradingDayTpl{};
  rp._bd_mgr._fullpid_map->add(comm->getFullPid(), comm, false);
  ct = WTSContractInfo::create("rb2405", "rb2405", "SHFE", "rb");
  ASSERT_NE(ct, nullptr); ct->setDates(20200101, 20301231); ct->setCommInfo(comm); ct->setVolScale(10);
  rp._bd_mgr._fullcode_map->add(ct->getFullCode(), ct, false);
  std::string error; ASSERT_TRUE(rp._fee_schedule.loadJson(zeroFees, error)) << error;
 }
 void tick(Mocker& m, double px = 100) {
  WTSTickStruct data{}; wt_strcpy(data.code, "rb2405");
  data.action_date = data.trading_date = rp._cur_tdate; data.action_time = 93000000; data.price = px;
  WTSTickData* t = WTSTickData::create(data); t->setContractInfo(ct);
  m.handle_tick(code, t, 3); t->release();
 }
 // Returns after the fill attempt on the tick following the signal.
 void attempt(Mocker& m, double slippage, bool ratio) {
  m._slippage = slippage; m._ratio_slippage = ratio;
  tick(m); m.stra_set_position(code, 1, "tick-divisor");
  ASSERT_EQ(1u, m._sig_map.count(code)); tick(m);
 }
 void expectRejected(Mocker& m) {
  EXPECT_EQ(1u, m._sig_map.count(code));   // signal kept, not consumed
  EXPECT_EQ(0u, m._pos_map.count(code));   // no position committed
  EXPECT_TRUE(m._trade_logs.str().empty());
  EXPECT_DOUBLE_EQ(0, m._fund_info._total_fees);
  EXPECT_FALSE(rp.fee_error().empty());
 }
};
using MockerTypes = ::testing::Types<CtaMocker, SelMocker>;
TYPED_TEST_SUITE(TickDivisor, MockerTypes);

TYPED_TEST(TickDivisor, ZeroTickWithRatioSlippageIsRejected) {
 this->build(0.0); TypeParam m(&this->rp, "ratio-zero");
 this->attempt(m, 5, true); this->expectRejected(m);
}
TYPED_TEST(TickDivisor, ZeroTickWithFixedSlippageIsRejected) {
 this->build(0.0); TypeParam m(&this->rp, "fixed-zero");
 this->attempt(m, 2, false); this->expectRejected(m);
}
TYPED_TEST(TickDivisor, ValidTickAppliesSlippage) {
 this->build(1.0); TypeParam m(&this->rp, "fixed-ok");
 this->attempt(m, 2, false);
 EXPECT_TRUE(this->rp.fee_error().empty()) << this->rp.fee_error();
 EXPECT_TRUE(m._sig_map.empty());
 ASSERT_EQ(1u, m._pos_map.count(code)); EXPECT_DOUBLE_EQ(1, m._pos_map[code]._volume);
 ASSERT_EQ(1u, m._pos_map[code]._details.size());
 EXPECT_DOUBLE_EQ(102, m._pos_map[code]._details[0]._price); // 100 + 2 ticks of 1
}
TYPED_TEST(TickDivisor, ZeroTickWithoutSlippageStillFills) {
 this->build(0.0); TypeParam m(&this->rp, "no-slippage");
 this->attempt(m, 0, false);
 EXPECT_TRUE(this->rp.fee_error().empty()) << this->rp.fee_error();
 ASSERT_EQ(1u, m._pos_map.count(code)); EXPECT_DOUBLE_EQ(1, m._pos_map[code]._volume);
}
} // namespace
