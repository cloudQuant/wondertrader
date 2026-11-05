// Appended to r7_twap_unit_test.cpp in an isolated build. These are red
// controls and must not be linked into the normal suite until the defects are fixed.
TEST(r7_twap_sell_sign, canceled_sell_does_not_submit_a_buy)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	ctx.pos = 100.0;
	{
		WtTWapExeUnit unit;
		bootT(unit, ctx, 20.0, mkTCfg(4));
		feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
		ASSERT_EQ(ctx.sells.size(), 1u);
		EXPECT_DOUBLE_EQ(ctx.sells[0].qty, 20.0);
		unit.on_order(8000, kTCode, false, 20.0, 10.50, true);
		EXPECT_TRUE(ctx.buys.empty());
	}
	ctx.comm->release();
}

TEST(r7_twap_sell_sign, last_slice_never_reverses_a_sell_into_a_buy)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	ctx.pos = 100.0;
	{
		WtTWapExeUnit unit;
		bootT(unit, ctx, 0.0, mkTCfg(1));
		feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
		ASSERT_EQ(ctx.sells.size(), 1u);
		EXPECT_DOUBLE_EQ(ctx.sells[0].qty, 100.0);
		// Model partial fill and channel reconciliation with no remaining order.
		ctx.pos = 80.0;
		unit.on_channel_ready();
		feedTWapTick(unit, mkTTick(kTCode, 100100000, 10.50, 10.49, 10.51));
		EXPECT_TRUE(ctx.buys.empty());
	}
	ctx.comm->release();
}

TEST(r7_twap_sell_sign, late_buy_fill_must_not_be_retried_twice)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	{
		WtTWapExeUnit unit;
		bootT(unit, ctx, 80.0, mkTCfg(4));
		feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
		ASSERT_EQ(ctx.buys.size(), 1u);
		ASSERT_DOUBLE_EQ(ctx.buys[0].qty, 20.0);
		// Ten filled, but the position callback has not yet been reflected.
		unit.on_order(8000, kTCode, true, 10.0, 10.50, true);
		ASSERT_LE(ctx.buys.size(), 2u);
		if (ctx.buys.size() == 2u)
			EXPECT_LE(ctx.buys[1].qty, 10.0);
		ctx.pos = 10.0; // delayed fill now reaches the account view
	}
	ctx.comm->release();
}

TEST(r7_twap_sell_sign, accounted_buy_fill_retries_only_the_remainder)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	{
		WtTWapExeUnit unit;
		bootT(unit, ctx, 80.0, mkTCfg(4));
		feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
		ASSERT_EQ(ctx.buys.size(), 1u);
		ASSERT_DOUBLE_EQ(ctx.buys[0].qty, 20.0);
		ctx.pos = 10.0;
		unit.on_order(8000, kTCode, true, 10.0, 10.50, true);
		ASSERT_EQ(ctx.buys.size(), 2u);
		EXPECT_DOUBLE_EQ(ctx.buys[1].qty, 10.0);
	}
	ctx.comm->release();
}

TEST(r7_twap_sell_sign, sell_slice_does_not_exceed_target_difference)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm(); // minimum quantity and quantity increment are both one
	ctx.pos = 10.0;
	{
		WtTWapExeUnit unit;
		bootT(unit, ctx, 8.0, mkTCfg(4, 0, 0, 3.0));
		feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
		ASSERT_EQ(ctx.sells.size(), 1u);
		EXPECT_LE(ctx.sells[0].qty, 2.0);
	}
	ctx.comm->release();
}
