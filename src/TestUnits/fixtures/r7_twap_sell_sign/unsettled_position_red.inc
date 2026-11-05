// Intentionally failing safety probe: no confirmed fill/position barrier exists.
TEST(r7_twap_unsettled_red, later_tick_can_submit_while_prior_fill_is_not_in_position)
{
    TFakeCtx ctx; ctx.comm = mkTComm();
    {
        WtTWapExeUnit unit;
        bootT(unit, ctx, 80.0, mkTCfg(4));
        feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
        ASSERT_EQ(ctx.buys.size(), 1u);
        // Partial fill not yet reflected in the account view. The terminal
        // reports ten unfilled; its retry is submitted but the fake account
        // still reports no pending quantity and no updated position.
        unit.on_order(8000, kTCode, true, 10.0, 10.5, true);
        ASSERT_EQ(ctx.buys.size(), 2u);
        feedTWapTick(unit, mkTTick(kTCode, 100100000, 10.50, 10.49, 10.51));
        EXPECT_LE(ctx.buys.size(), 2u);
    }
    ctx.comm->release();
}

TEST(r7_twap_unsettled_red, two_canceled_orders_need_both_remaining_quantities)
{
    TFakeCtx ctx; ctx.comm = mkTComm();
    {
        WtTWapExeUnit unit;
        bootT(unit, ctx, 80.0, mkTCfg(4));
        feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
        ASSERT_EQ(ctx.buys.size(), 1u);
        // Model one 20-unit slice split by the adapter into two local IDs:
        // first order filled 4 and has 8 unfilled; second filled 4 and has 4
        // unfilled. Both callbacks are independently observed as canceled.
        const uint32_t second = 8001;
        unit._orders_mon.push_order(&second, 1, ctx.curTime);
        unit._cancel_cnt = 2;
        ctx.pos = 8.0;
        unit.on_order(8000, kTCode, true, 8.0, 10.5, true);
        ASSERT_EQ(ctx.buys.size(), 1u);
        unit.on_order(8001, kTCode, true, 4.0, 10.5, true);
        ASSERT_EQ(ctx.buys.size(), 2u);
        EXPECT_DOUBLE_EQ(ctx.buys[1].qty, 12.0);
    }
    ctx.comm->release();
}
