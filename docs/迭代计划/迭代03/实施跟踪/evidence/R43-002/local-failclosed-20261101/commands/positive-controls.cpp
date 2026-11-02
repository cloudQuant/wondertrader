// Standalone evidence fixture: exercises both historical baseline and final source.
#include "../final/r7_stock_vwap_unit_test.cpp"

TEST(r43_positive_controls, known_canceled_order_retries_once_for_zero_and_nonzero_span)
{
	for (const uint32_t tail : { 0u, 3600u })
	{
		SCOPED_TRACE(tail);
		SVwapFile vf(25.0);
		SFakeCtx ctx;
		ctx.comm = mkSComm("STK");
		ctx.pos = ctx.avail = 1000;
		{
			WtStockVWapExeUnit unit;
			bootS(unit, ctx, kStk, 100, mkSCfg(tail ? 1 : 3, 0, 0, 100, 1000, 0, tail, true, 1000, 1100));
			ASSERT_EQ(unit._fire_span, tail ? 0u : 1200u);
			ASSERT_EQ(ctx.sells.size(), 1u);
			ASSERT_EQ(unit._fired_times, 1u);
			const double initial_qty = ctx.sells[0].qty;
			unit.on_order(7000, kStk, false, 50, 10.50, true);
			ASSERT_EQ(ctx.sells.size(), 1u);
			unit.on_tick(mkSTick(kStk, 100100000, 10.50, 10.49, 10.51));
			ASSERT_EQ(ctx.sells.size(), 2u);
			EXPECT_TRUE(ctx.buys.empty());
			EXPECT_GT(ctx.sells[1].qty, 0.0);
			EXPECT_LE(ctx.sells[1].qty, initial_qty);
			EXPECT_LE(ctx.sells[1].qty, ctx.avail);
			EXPECT_DOUBLE_EQ(std::fmod(ctx.sells[1].qty, 100.0), 0.0);
			EXPECT_EQ(unit._fired_times, 1u);
			EXPECT_FALSE(unit.hasUnverifiedSubmission());
		}
		ctx.comm->release();
	}
}
