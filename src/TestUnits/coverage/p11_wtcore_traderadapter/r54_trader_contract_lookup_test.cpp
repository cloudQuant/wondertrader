/**
 * BUG-R54-001: WtCore must resolve known trade reports whose provider omits
 * the embedded ContractInfo, using the report's raw code and exchange.
 */
#include "r59_trader_fixture.h"

using namespace r59x;
using wtp::ActionPolicyMgr;
using wtp::TraderAdapter;

namespace {

WTSTradeInfo* makeTradeWithoutContract(WTSDirectionType direction, WTSOffsetType offset,
    double volume, const char* tradeID)
{
    WTSTradeInfo* trade = WTSTradeInfo::create(kRawCode, kExchg);
    trade->setDirection(direction);
    trade->setOffsetType(offset);
    trade->setVolume(volume);
    trade->setPrice(7000.0);
    trade->setTradeID(tradeID);
    trade->setRefOrder("R54-ORDER");
    trade->setTradeDate(20240510);
    trade->setTradeTime(100010);
    trade->setAmount(volume * 7000.0);
    return trade;
}

class R54TraderContractLookup : public R59TraderTest
{
};

TEST_F(R54TraderContractLookup, query_trade_lookup_updates_open_risk_statistics)
{
    ActionPolicyMgr policy;
    ASSERT_TRUE(loadPolicy(policy, "r54_lookup_policy.json",
        R"json({"default":{"order":[{"action":"open","limit_l":2}]}})json"));

    TraderAdapter trader;
    build(trader, &policy);

    WTSTradeInfo* attached = makeTrade(ct_, WDT_LONG, WOT_OPEN, 1.0,
        7000.0, "R54-ATTACHED", "R54-ORDER-1", "");
    WTSTradeInfo* missingInfo = makeTradeWithoutContract(WDT_LONG, WOT_OPEN,
        1.0, "R54-LOOKUP");
    ASSERT_NE(nullptr, attached->getContractInfo());
    ASSERT_EQ(nullptr, missingInfo->getContractInfo());

    WTSArray* reports = makeArray({ attached, missingInfo });
    trader.onRspTrades(reports);
    EXPECT_STREQ(kRawCode, missingInfo->getCode());
    reports->release();
    attached->release();
    missingInfo->release();

    // The attached-contract control and recovered trade fill the same limit.
    EXPECT_TRUE(trader.buy(kStdCode, 7000.0, 1.0, 0, false).empty());
    EXPECT_EQ(0u, api2_.n_insert_);
    trader.release();
}

TEST_F(R54TraderContractLookup, pushed_trade_lookup_updates_position_and_notifies_standard_code)
{
    TraderAdapter trader;
    build(trader, nullptr);

    WTSTradeInfo* missingInfo = makeTradeWithoutContract(WDT_LONG, WOT_OPEN,
        3.0, "R54-PUSH");
    ASSERT_EQ(nullptr, missingInfo->getContractInfo());

    trader.onPushTrade(missingInfo);

    EXPECT_STREQ(kRawCode, missingInfo->getCode());
    EXPECT_DOUBLE_EQ(3.0, trader.getPosition(kStdCode, false, 1));
    ASSERT_EQ(1u, sink_.trades_.size());
    EXPECT_EQ(kStdCode, sink_.trades_[0].code);
    EXPECT_DOUBLE_EQ(3.0, sink_.trades_[0].vol);

    missingInfo->release();
    trader.release();
}

} // namespace
