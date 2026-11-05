// Contract-faithful position snapshots and force-close controls. The real
// adapter may apply an open-first action rule, so the flag is observed here.
class PositionSnapshotCtx : public TFakeCtx
{
public:
    double longTotal = 0, shortTotal = 0, longAvail = 0, shortAvail = 0;
    bool breakNet = false, unstable = false;
    bool overrideIds = false;
    OrderIDs resultIds;
    WtTWapExeUnit* syncUnit = nullptr;
    int syncKind = 0;
    unsigned int reads = 0;
    std::vector<bool> buyForce, sellForce;
    double getPosition(const char*, bool validOnly = true, int32_t flag = 3) override
    {
        ++reads;
        const double longSide = validOnly ? longAvail : longTotal;
        const double shortSide = validOnly ? shortAvail : shortTotal;
        if (flag == 1) return longSide;
        if (flag == 2) return shortSide;
        return longSide + shortSide + ((breakNet || (unstable && reads > 6)) ? 1.0 : 0.0);
    }
    OrderIDs buy(const char* c, double p, double q, bool force) override
    {
        buyForce.push_back(force);
        if (syncUnit && syncKind == 1) syncUnit->on_order(8000, c, true, q, p, false);
        if (syncUnit && syncKind == 2) syncUnit->on_trade(8000, c, true, q, p);
        if (syncUnit && syncKind == 3) syncUnit->on_entrust(8000, c, true, "accepted");
        if (overrideIds) { buys.push_back({c, p, q}); return resultIds; }
        return TFakeCtx::buy(c, p, q, force);
    }
    OrderIDs sell(const char* c, double p, double q, bool force) override
    {
        sellForce.push_back(force);
        if (syncUnit && syncKind == 1) syncUnit->on_order(8000, c, false, q, p, false);
        if (syncUnit && syncKind == 2) syncUnit->on_trade(8000, c, false, q, p);
        if (syncUnit && syncKind == 3) syncUnit->on_entrust(8000, c, true, "accepted");
        if (overrideIds) { sells.push_back({c, p, q}); return resultIds; }
        return TFakeCtx::sell(c, p, q, force);
    }
};

TEST(r7_twap_position, frozen_stock_reduces_by_available_long_and_forces_close)
{
    PositionSnapshotCtx ctx; ctx.comm = mkTComm(); ctx.longTotal = 100; ctx.longAvail = 10;
    {
        WtTWapExeUnit unit;
        bootT(unit, ctx, 50, mkTCfg(4));
        feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.5, 10.49, 10.51));
        EXPECT_TRUE(ctx.buys.empty()); ASSERT_EQ(ctx.sells.size(), 1u);
        EXPECT_DOUBLE_EQ(ctx.sells[0].qty, 10); ASSERT_EQ(ctx.sellForce.size(), 1u); EXPECT_TRUE(ctx.sellForce[0]);
    }
    ctx.comm->release();
}

TEST(r7_twap_position, no_sellable_stock_stays_pending_then_retries_after_availability)
{
    PositionSnapshotCtx ctx; ctx.comm = mkTComm(); ctx.longTotal = 100;
    {
        WtTWapExeUnit unit;
        bootT(unit, ctx, 50, mkTCfg(4));
        feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.5, 10.49, 10.51));
        EXPECT_TRUE(ctx.buys.empty()); EXPECT_TRUE(ctx.sells.empty());
        ctx.longAvail = 8;
        feedTWapTick(unit, mkTTick(kTCode, 100100000, 10.5, 10.49, 10.51));
        ASSERT_EQ(ctx.sells.size(), 1u); EXPECT_DOUBLE_EQ(ctx.sells[0].qty, 8);
        ASSERT_EQ(ctx.sellForce.size(), 1u); EXPECT_TRUE(ctx.sellForce[0]);
    }
    ctx.comm->release();
}

TEST(r7_twap_position, partially_available_short_buys_only_available_and_forces_close)
{
    PositionSnapshotCtx ctx; ctx.comm = mkTComm(); ctx.shortTotal = -100; ctx.shortAvail = -10;
    {
        WtTWapExeUnit unit;
        bootT(unit, ctx, -50, mkTCfg(4));
        feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.5, 10.49, 10.51));
        EXPECT_TRUE(ctx.sells.empty()); ASSERT_EQ(ctx.buys.size(), 1u);
        EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 10); ASSERT_EQ(ctx.buyForce.size(), 1u); EXPECT_TRUE(ctx.buyForce[0]);
    }
    ctx.comm->release();
}

TEST(r7_twap_position, crossing_zero_closes_then_opens_on_later_slice)
{
    PositionSnapshotCtx ctx; ctx.comm = mkTComm(); ctx.longTotal = 2; ctx.longAvail = 2;
    {
        WtTWapExeUnit unit;
        bootT(unit, ctx, -6, mkTCfg(1, 0, 0, 3));
        feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.5, 10.49, 10.51));
        ASSERT_EQ(ctx.sells.size(), 1u); EXPECT_DOUBLE_EQ(ctx.sells[0].qty, 2); EXPECT_TRUE(ctx.sellForce[0]);
        ctx.longTotal = ctx.longAvail = 0;
        unit.on_order(8000, kTCode, false, 0, 10.5, false);
        feedTWapTick(unit, mkTTick(kTCode, 100100000, 10.5, 10.49, 10.51));
        ASSERT_EQ(ctx.sells.size(), 2u); EXPECT_DOUBLE_EQ(ctx.sells[1].qty, 6); EXPECT_FALSE(ctx.sellForce[1]);
    }
    ctx.comm->release();
}

TEST(r7_twap_position, hedged_position_closes_opposite_gross_side_first)
{
    PositionSnapshotCtx ctx; ctx.comm = mkTComm();
    ctx.longTotal = 20; ctx.longAvail = 20; ctx.shortTotal = -10; ctx.shortAvail = -10;
    {
        WtTWapExeUnit unit;
        bootT(unit, ctx, 5, mkTCfg(4));
        feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.5, 10.49, 10.51));
        ASSERT_EQ(ctx.sells.size(), 1u); EXPECT_TRUE(ctx.sellForce[0]);
        EXPECT_LE(ctx.sells[0].qty, 5);
    }
    ctx.comm->release();
}

TEST(r7_twap_position, retry_uses_total_and_force_close)
{
    PositionSnapshotCtx ctx; ctx.comm = mkTComm(); ctx.longTotal = 100; ctx.longAvail = 10;
    {
        WtTWapExeUnit unit;
        bootT(unit, ctx, 50, mkTCfg(4));
        feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.5, 10.49, 10.51));
        ASSERT_EQ(ctx.sells.size(), 1u);
        unit.on_order(8000, kTCode, false, 5, 10.5, true);
        ASSERT_EQ(ctx.sells.size(), 2u); EXPECT_DOUBLE_EQ(ctx.sells[1].qty, 5); EXPECT_TRUE(ctx.sellForce[1]);
        EXPECT_TRUE(ctx.buys.empty());
    }
    ctx.comm->release();
}

TEST(r7_twap_position, short_retry_uses_total_and_force_close)
{
    PositionSnapshotCtx ctx; ctx.comm = mkTComm(); ctx.shortTotal = -100; ctx.shortAvail = -10;
    {
        WtTWapExeUnit unit;
        bootT(unit, ctx, -50, mkTCfg(4));
        feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.5, 10.49, 10.51));
        ASSERT_EQ(ctx.buys.size(), 1u);
        unit.on_order(8000, kTCode, true, 5, 10.5, true);
        ASSERT_EQ(ctx.buys.size(), 2u); EXPECT_DOUBLE_EQ(ctx.buys[1].qty, 5); EXPECT_TRUE(ctx.buyForce[1]);
        EXPECT_TRUE(ctx.sells.empty());
    }
    ctx.comm->release();
}

TEST(r7_twap_position, inconsistent_snapshot_prevents_cancel_retry)
{
    PositionSnapshotCtx ctx; ctx.comm = mkTComm(); ctx.longTotal = 100; ctx.longAvail = 10;
    {
        WtTWapExeUnit unit;
        bootT(unit, ctx, 50, mkTCfg(4));
        feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.5, 10.49, 10.51));
        ASSERT_EQ(ctx.sells.size(), 1u);
        ctx.longAvail = 101;
        unit.on_order(8000, kTCode, false, 5, 10.5, true);
        EXPECT_EQ(ctx.sells.size(), 1u); EXPECT_TRUE(ctx.buys.empty());
    }
    ctx.comm->release();
}

TEST(r7_twap_position, rejects_nonfinite_out_of_range_inconsistent_or_changing_snapshot)
{
    for (int mode = 0; mode < 5; ++mode)
    {
        PositionSnapshotCtx ctx; ctx.comm = mkTComm();
        ctx.longTotal = 100; ctx.longAvail = 10;
        if (mode == 0) ctx.longTotal = std::numeric_limits<double>::quiet_NaN();
        if (mode == 1) ctx.longAvail = 101;
        if (mode == 2) ctx.shortAvail = 1;
        if (mode == 3) ctx.breakNet = true;
        if (mode == 4) ctx.unstable = true;
        {
            WtTWapExeUnit unit;
            bootT(unit, ctx, 50, mkTCfg(4));
            ctx.reads = 0; // ignore setup-time calls; drift must occur inside checked reads
            feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.5, 10.49, 10.51));
            EXPECT_TRUE(ctx.buys.empty()) << mode;
            EXPECT_TRUE(ctx.sells.empty()) << mode;
        }
        ctx.comm->release();
    }
}

TEST(r7_twap_position, missing_or_invalid_order_ids_hold_automatic_actions_without_fake_completion)
{
    for (int mode = 0; mode < 4; ++mode)
    {
        PositionSnapshotCtx ctx; ctx.comm = mkTComm(); ctx.longTotal = 100; ctx.longAvail = 10;
        ctx.overrideIds = true;
        if (mode == 1) ctx.resultIds = {0};
        if (mode == 2) ctx.resultIds = {8000, (std::numeric_limits<uint32_t>::max)()};
        if (mode == 3) ctx.resultIds = {8000, 8000};
        {
            WtTWapExeUnit unit;
            bootT(unit, ctx, 50, mkTCfg(4));
            feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.5, 10.49, 10.51));
            ASSERT_EQ(ctx.sells.size(), 1u) << mode;
            EXPECT_TRUE(ctx.sellForce[0]); EXPECT_EQ(unit._fired_times, 0u);
            EXPECT_FALSE(unit._this_target_valid);
            EXPECT_TRUE(unit._submission_unverified.load());
            if (mode >= 2) EXPECT_TRUE(unit._orders_mon.has_order(8000));
            unit.set_position(kTCode, 40);
            unit.on_channel_ready();
            feedTWapTick(unit, mkTTick(kTCode, 100100000, 10.5, 10.49, 10.51));
            EXPECT_EQ(ctx.sells.size(), 1u); EXPECT_TRUE(ctx.buys.empty());
        }
        ctx.comm->release();
    }
}

TEST(r7_twap_position, retry_without_ids_holds_future_submissions)
{
    PositionSnapshotCtx ctx; ctx.comm = mkTComm(); ctx.longTotal = 100; ctx.longAvail = 10;
    {
        WtTWapExeUnit unit;
        bootT(unit, ctx, 50, mkTCfg(4));
        feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.5, 10.49, 10.51));
        ASSERT_EQ(ctx.sells.size(), 1u);
        ctx.overrideIds = true;
        unit.on_order(8000, kTCode, false, 5, 10.5, true);
        ASSERT_EQ(ctx.sells.size(), 2u); EXPECT_TRUE(ctx.sellForce[1]);
        EXPECT_TRUE(unit._submission_unverified.load());
        feedTWapTick(unit, mkTTick(kTCode, 100100000, 10.5, 10.49, 10.51));
        EXPECT_EQ(ctx.sells.size(), 2u);
    }
    ctx.comm->release();
}

TEST(r7_twap_position, synchronous_report_before_id_registration_is_held)
{
    for (int kind = 1; kind <= 3; ++kind)
    {
        PositionSnapshotCtx ctx; ctx.comm = mkTComm(); ctx.longTotal = 100; ctx.longAvail = 10;
        {
            WtTWapExeUnit unit;
            ctx.syncUnit = &unit; ctx.syncKind = kind;
            bootT(unit, ctx, 50, mkTCfg(4));
            feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.5, 10.49, 10.51));
            ASSERT_EQ(ctx.sells.size(), 1u) << kind;
            EXPECT_TRUE(unit._orders_mon.has_order(8000));
            EXPECT_TRUE(unit._submission_unverified.load());
            EXPECT_EQ(unit._fired_times, 0u); EXPECT_FALSE(unit._this_target_valid);
            ctx.syncUnit = nullptr;
            unit.on_channel_ready();
            feedTWapTick(unit, mkTTick(kTCode, 100100000, 10.5, 10.49, 10.51));
            EXPECT_EQ(ctx.sells.size(), 1u);
        }
        ctx.comm->release();
    }
}

TEST(r7_twap_position, multiple_valid_order_ids_remain_monitored)
{
    PositionSnapshotCtx ctx; ctx.comm = mkTComm(); ctx.longTotal = 100; ctx.longAvail = 10;
    ctx.overrideIds = true; ctx.resultIds = {8000, 8001};
    {
        WtTWapExeUnit unit;
        bootT(unit, ctx, 50, mkTCfg(4));
        feedTWapTick(unit, mkTTick(kTCode, 100000000, 10.5, 10.49, 10.51));
        EXPECT_FALSE(unit._submission_unverified.load());
        EXPECT_TRUE(unit._orders_mon.has_order(8000)); EXPECT_TRUE(unit._orders_mon.has_order(8001));
        EXPECT_EQ(unit._fired_times, 1u); EXPECT_TRUE(unit._this_target_valid);
    }
    ctx.comm->release();
}
