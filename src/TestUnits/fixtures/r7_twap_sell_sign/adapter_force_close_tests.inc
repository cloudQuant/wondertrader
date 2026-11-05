// Isolated integration with real TraderAdapter/ActionPolicyMgr and a fake API.
#include "TestUnits/coverage/p11_wtcore_traderadapter/r59_trader_fixture.h"

class TwapAdapterCtx : public TFakeCtx
{
public:
    wtp::TraderAdapter* trader = nullptr;
    double getPosition(const char* c, bool validOnly = true, int32_t flag = 3) override
    { return trader->getPosition(c, validOnly, flag); }
    double getUndoneQty(const char* c) override { return trader->getUndoneQty(c); }
    OrderIDs buy(const char* c, double p, double q, bool close) override
    { buyForce.push_back(close); lastIds = trader->buy(c, p, q, 0, close); return lastIds; }
    OrderIDs sell(const char* c, double p, double q, bool close) override
    { sellForce.push_back(close); lastIds = trader->sell(c, p, q, 0, close); return lastIds; }
    std::vector<bool> buyForce, sellForce;
    OrderIDs lastIds;
};

class TwapOpenFirstPolicyTest : public R59TraderTest {};

TEST_F(TwapOpenFirstPolicyTest, sell_close_skips_open_first_rule)
{
    using namespace r59x;
    wtp::ActionPolicyMgr pm;
    ASSERT_TRUE(loadPolicy(pm, "twap_sell_open_first.json",
        R"json({"default":{"order":[{"action":"open"},{"action":"close"}]}})json"));
    wtp::TraderAdapter trader; build(trader, &pm);
    seedPos(trader, {makePos(ct_, WDT_LONG, 100, 10, 0, 0)});
    TwapAdapterCtx ctx; ctx.trader = &trader; ctx.comm = comm_;
    {
        WtTWapExeUnit unit; WTSVariant* cfg = mkTCfg(4);
        unit.init(&ctx, kStdCode, cfg); cfg->release(); ASSERT_TRUE(unit.initializationSucceeded());
        unit.on_channel_ready(); unit.set_position(kStdCode, 50.0);
        feedTWapTick(unit, mkTTick(kStdCode, 100000000, 10.5, 10.49, 10.51));
        ASSERT_EQ(ctx.sellForce.size(), 1u); EXPECT_TRUE(ctx.sellForce[0]);
        ASSERT_EQ(api2_.offsets_.size(), 1u);
        EXPECT_NE(api2_.offsets_[0], WOT_OPEN); EXPECT_DOUBLE_EQ(api2_.vols_[0], 10.0);
        ASSERT_EQ(ctx.lastIds.size(), 1u);
        const uint32_t id = ctx.lastIds[0];
        unit.on_order(id, kStdCode, false, 5.0, 10.5, true);
        ASSERT_EQ(ctx.sellForce.size(), 2u); EXPECT_TRUE(ctx.sellForce[1]);
        ASSERT_EQ(api2_.offsets_.size(), 2u);
        EXPECT_NE(api2_.offsets_[1], WOT_OPEN); EXPECT_DOUBLE_EQ(api2_.vols_[1], 5.0);
    }
}

TEST_F(TwapOpenFirstPolicyTest, buy_close_skips_open_first_rule)
{
    using namespace r59x;
    wtp::ActionPolicyMgr pm;
    ASSERT_TRUE(loadPolicy(pm, "twap_buy_open_first.json",
        R"json({"default":{"order":[{"action":"open"},{"action":"close"}]}})json"));
    wtp::TraderAdapter trader; build(trader, &pm);
    seedPos(trader, {makePos(ct_, WDT_SHORT, 100, 10, 0, 0)});
    TwapAdapterCtx ctx; ctx.trader = &trader; ctx.comm = comm_;
    {
        WtTWapExeUnit unit; WTSVariant* cfg = mkTCfg(4);
        unit.init(&ctx, kStdCode, cfg); cfg->release(); ASSERT_TRUE(unit.initializationSucceeded());
        unit.on_channel_ready(); unit.set_position(kStdCode, -50.0);
        feedTWapTick(unit, mkTTick(kStdCode, 100000000, 10.5, 10.49, 10.51));
        ASSERT_EQ(ctx.buyForce.size(), 1u); EXPECT_TRUE(ctx.buyForce[0]);
        ASSERT_EQ(api2_.offsets_.size(), 1u);
        EXPECT_NE(api2_.offsets_[0], WOT_OPEN); EXPECT_DOUBLE_EQ(api2_.vols_[0], 10.0);
        ASSERT_EQ(ctx.lastIds.size(), 1u);
        const uint32_t id = ctx.lastIds[0];
        unit.on_order(id, kStdCode, true, 5.0, 10.5, true);
        ASSERT_EQ(ctx.buyForce.size(), 2u); EXPECT_TRUE(ctx.buyForce[1]);
        ASSERT_EQ(api2_.offsets_.size(), 2u);
        EXPECT_NE(api2_.offsets_[1], WOT_OPEN); EXPECT_DOUBLE_EQ(api2_.vols_[1], 5.0);
    }
}

TEST_F(TwapOpenFirstPolicyTest, force_close_insert_failure_returns_invalid_id_and_holds)
{
    using namespace r59x;
    wtp::ActionPolicyMgr pm;
    ASSERT_TRUE(loadPolicy(pm, "twap_sell_rejected.json",
        R"json({"default":{"order":[{"action":"open"},{"action":"close"}]}})json"));
    wtp::TraderAdapter trader; build(trader, &pm);
    seedPos(trader, {makePos(ct_, WDT_LONG, 100, 10, 0, 0)});
    api2_.insert_ret_ = -1;
    TwapAdapterCtx ctx; ctx.trader = &trader; ctx.comm = comm_;
    {
        WtTWapExeUnit unit; WTSVariant* cfg = mkTCfg(4);
        unit.init(&ctx, kStdCode, cfg); cfg->release(); ASSERT_TRUE(unit.initializationSucceeded());
        unit.on_channel_ready(); unit.set_position(kStdCode, 50.0);
        feedTWapTick(unit, mkTTick(kStdCode, 100000000, 10.5, 10.49, 10.51));
        ASSERT_EQ(ctx.lastIds.size(), 1u);
        EXPECT_EQ(ctx.lastIds[0], (std::numeric_limits<uint32_t>::max)());
        EXPECT_TRUE(unit._submission_unverified.load());
        EXPECT_FALSE(unit._orders_mon.has_order((std::numeric_limits<uint32_t>::max)()));
        EXPECT_EQ(unit._fired_times, 0u); EXPECT_FALSE(unit._this_target_valid);
        const uint32_t attempts = api2_.n_insert_;
        feedTWapTick(unit, mkTTick(kStdCode, 100100000, 10.5, 10.49, 10.51));
        EXPECT_EQ(api2_.n_insert_, attempts);
    }
}
