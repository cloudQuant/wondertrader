// Additional bounded-quantity controls, appended only in the isolated fixture.
TEST(r7_twap_bounded, opening_minimum_above_remaining_does_not_cross_target)
{
    TFakeCtx ctx; ctx.comm = mkTComm();
    {
        WtTWapExeUnit unit;
        bootT(unit, ctx, 2.0, mkTCfg(4, 0, 0, 3.0));
        feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
        EXPECT_TRUE(ctx.buys.empty()); EXPECT_TRUE(ctx.sells.empty());
        EXPECT_TRUE(ctx.hasLog("remains outstanding"));
    }
    ctx.comm->release();
}

TEST(r7_twap_bounded, fractional_target_is_not_exceeded)
{
    TFakeCtx ctx; ctx.comm = mkTComm();
    ctx.comm->setLotsTick(0.25); ctx.comm->setMinLots(0.5);
    {
        WtTWapExeUnit unit;
        bootT(unit, ctx, 1.1, mkTCfg(1, 0, 0, 0.0));
        feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
        ASSERT_EQ(ctx.buys.size(), 1u);
        EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 1.0);
        EXPECT_LE(ctx.buys[0].qty, 1.1);
    }
    ctx.comm->release();
}

TEST(r7_twap_bounded, crossing_zero_closes_before_opening_opposite_position)
{
    TFakeCtx ctx; ctx.comm = mkTComm(); ctx.pos = 2.0;
    {
        WtTWapExeUnit unit;
        bootT(unit, ctx, -6.0, mkTCfg(1, 0, 0, 3.0));
        feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
        ASSERT_EQ(ctx.sells.size(), 1u);
        EXPECT_DOUBLE_EQ(ctx.sells[0].qty, 2.0);
        EXPECT_TRUE(ctx.buys.empty());
    }
    ctx.comm->release();
}

TEST(r7_twap_bounded, cancel_retry_never_reverses_when_position_moves_past_slice_target)
{
    TFakeCtx ctx; ctx.comm = mkTComm(); ctx.pos = 100.0;
    {
        WtTWapExeUnit unit;
        bootT(unit, ctx, 20.0, mkTCfg(4));
        feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
        ASSERT_EQ(ctx.sells.size(), 1u);
        ctx.pos = 70.0;
        unit.on_order(8000, kTCode, false, 20.0, 10.5, true);
        EXPECT_TRUE(ctx.buys.empty()); EXPECT_EQ(ctx.sells.size(), 1u);
    }
    ctx.comm->release();
}

TEST(r7_twap_bounded, subminimum_opening_cancel_remainder_is_not_enlarged)
{
    TFakeCtx ctx; ctx.comm = mkTComm();
    {
        WtTWapExeUnit unit;
        bootT(unit, ctx, 80.0, mkTCfg(4, 0, 0, 3.0));
        feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
        ASSERT_EQ(ctx.buys.size(), 1u);
        unit.on_order(8000, kTCode, true, 2.0, 10.5, true);
        EXPECT_TRUE(ctx.sells.empty()); EXPECT_EQ(ctx.buys.size(), 1u);
    }
    ctx.comm->release();
}

TEST(r7_twap_bounded, canceled_long_close_below_open_minimum_keeps_sell_direction)
{
    TFakeCtx ctx; ctx.comm = mkTComm(); ctx.pos = 100.0;
    {
        WtTWapExeUnit unit;
        bootT(unit, ctx, 20.0, mkTCfg(4, 0, 0, 3.0));
        feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
        ASSERT_EQ(ctx.sells.size(), 1u);
        unit.on_order(8000, kTCode, false, 2.0, 10.5, true);
        ASSERT_EQ(ctx.sells.size(), 2u);
        EXPECT_DOUBLE_EQ(ctx.sells[1].qty, 2.0);
        EXPECT_TRUE(ctx.buys.empty());
    }
    ctx.comm->release();
}

TEST(r7_twap_bounded, canceled_short_close_below_open_minimum_keeps_buy_direction)
{
    TFakeCtx ctx; ctx.comm = mkTComm(); ctx.pos = -100.0;
    {
        WtTWapExeUnit unit;
        bootT(unit, ctx, -20.0, mkTCfg(4, 0, 0, 3.0));
        feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
        ASSERT_EQ(ctx.buys.size(), 1u);
        unit.on_order(8000, kTCode, true, 2.0, 10.5, true);
        ASSERT_EQ(ctx.buys.size(), 2u);
        EXPECT_DOUBLE_EQ(ctx.buys[1].qty, 2.0);
        EXPECT_TRUE(ctx.sells.empty());
    }
    ctx.comm->release();
}

