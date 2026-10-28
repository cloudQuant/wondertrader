/**
 * r59_trader_sell_test.cpp — TraderAdapter::sell 实体路径（TraderAdapter.cpp:956-1242）
 *
 * 与 r59_trader_buy_test.cpp 同构：固定时段(09:00-15:00)+固定进程内时间(10:00)
 * + ActionPolicyMgr 临时规则，覆盖 sell 的开仓拆单/限手数、平今、平昨、平仓
 * （区分平今与不区分平今两种 cover mode）、pure 保护、bForceClose、剩余量日志
 * 与自成交拉黑入口。
 */
#include "r59_trader_fixture.h"

using namespace r59x;
using wtp::ActionPolicyMgr;
using wtp::TraderAdapter;

namespace {

class R59Sell : public R59TraderTest
{
};

// TraderAdapter.cpp:956-1054（入口/自成交/时段/未完成量/AT_Open 卖开拆单与市价上限）
TEST_F(R59Sell, open_splits_by_limit_volume_and_handles_market_price)
{
    ActionPolicyMgr pm;
    ASSERT_TRUE(loadPolicy(pm, "r59_sell_open.json",
        R"json({"default":{"order":[{"action":"open"}]}})json"));

    TraderAdapter trader;
    build(trader, &pm);
    api2_.vols_.clear();

    // 限价 5000 手 -> 2000+2000+1000，方向为空开
    OrderIDs ids = trader.sell(kStdCode, 7000.0, 5000.0, 0, false);
    ASSERT_EQ(3u, ids.size());
    ASSERT_EQ(3u, api2_.vols_.size());
    EXPECT_DOUBLE_EQ(2000.0, api2_.vols_[0]);
    EXPECT_DOUBLE_EQ(2000.0, api2_.vols_[1]);
    EXPECT_DOUBLE_EQ(1000.0, api2_.vols_[2]);
    EXPECT_EQ(WDT_SHORT, api2_.dirs_[0]);
    EXPECT_EQ(WOT_OPEN, api2_.offsets_[0]);
    EXPECT_EQ(WPT_LIMITPRICE, api2_.last_pxtype_);
    // 卖开未完成量按负方向记
    EXPECT_DOUBLE_EQ(-5000.0, trader.getUndoneQty(kStdCode));

    // 市价 1500 手 -> 1000+500
    api2_.vols_.clear();
    ids = trader.sell(kStdCode, 0.0, 1500.0, 0, false);
    ASSERT_EQ(2u, ids.size());
    ASSERT_EQ(2u, api2_.vols_.size());
    EXPECT_DOUBLE_EQ(1000.0, api2_.vols_[0]);
    EXPECT_DOUBLE_EQ(500.0, api2_.vols_[1]);
    EXPECT_EQ(WPT_ANYPRICE, api2_.last_pxtype_);

    // 非交易时段（20:00 不在 09:00-15:00）-> 直接拒绝
    WtHelper::setTime(20240510, 2000, 0);
    uint32_t before = api2_.n_insert_;
    EXPECT_TRUE(trader.sell(kStdCode, 7000.0, 1.0, 0, false).empty());
    EXPECT_EQ(before, api2_.n_insert_);
    WtHelper::setTime(20240510, 1000, 0);

    before = api2_.n_insert_;
    EXPECT_TRUE(trader.sell(kStdCode, 7000.0, 0.0, 0, false).empty());
    EXPECT_EQ(before, api2_.n_insert_);
}

// TraderAdapter.cpp:1010-1034（AT_Open 的 limit_s / limit 已达上限 continue）
TEST_F(R59Sell, open_skips_when_short_or_total_limit_reached)
{
    ActionPolicyMgr pm;
    ASSERT_TRUE(loadPolicy(pm, "r59_sell_lim_s.json",
        R"json({"default":{"order":[{"action":"open","limit_s":5}]}})json"));
    TraderAdapter t1;
    build(t1, &pm);
    t1.onRspTrades(makeArray({
        makeTrade(ct_, WDT_SHORT, WOT_OPEN, 5, 7000, "SOL1", "SO1", "") }));
    EXPECT_TRUE(t1.sell(kStdCode, 7000.0, 1.0, 0, false).empty());
    EXPECT_EQ(0u, api2_.n_insert_);

    ActionPolicyMgr pm2;
    ASSERT_TRUE(loadPolicy(pm2, "r59_sell_lim_total.json",
        R"json({"default":{"order":[{"action":"open","limit":5}]}})json"));
    TraderAdapter t2;
    build(t2, &pm2);
    t2.onRspTrades(makeArray({
        makeTrade(ct_, WDT_LONG, WOT_OPEN, 2, 7000, "SOL2", "SO2", ""),
        makeTrade(ct_, WDT_SHORT, WOT_OPEN, 3, 7000, "SOS2", "SO3", "") }));
    uint32_t before = api2_.n_insert_;
    EXPECT_TRUE(t2.sell(kStdCode, 7000.0, 1.0, 0, false).empty());
    EXPECT_EQ(before, api2_.n_insert_);
}

// TraderAdapter.cpp:1019-1032（AT_Open 卖开的剩余手数 min 截断）
TEST_F(R59Sell, open_caps_quantity_by_remaining_short_and_total_limits)
{
    ActionPolicyMgr pm;
    ASSERT_TRUE(loadPolicy(pm, "r59_sell_cap_s.json",
        R"json({"default":{"order":[{"action":"open","limit_s":5,"limit":100}]}})json"));
    TraderAdapter t1;
    build(t1, &pm);
    t1.onRspTrades(makeArray({
        makeTrade(ct_, WDT_SHORT, WOT_OPEN, 2, 7000, "SCS1", "SC1", "") }));
    api2_.vols_.clear();
    OrderIDs ids = t1.sell(kStdCode, 7000.0, 10.0, 0, false);
    ASSERT_EQ(1u, ids.size());
    ASSERT_EQ(1u, api2_.vols_.size());
    EXPECT_DOUBLE_EQ(3.0, api2_.vols_[0]);

    ActionPolicyMgr pm2;
    ASSERT_TRUE(loadPolicy(pm2, "r59_sell_cap_total.json",
        R"json({"default":{"order":[{"action":"open","limit_s":100,"limit":10}]}})json"));
    TraderAdapter t2;
    build(t2, &pm2);
    t2.onRspTrades(makeArray({
        makeTrade(ct_, WDT_LONG, WOT_OPEN, 2, 7000, "SCT1", "SCT2", ""),
        makeTrade(ct_, WDT_SHORT, WOT_OPEN, 1, 7000, "SCT3", "SCT4", "") }));
    api2_.vols_.clear();
    ids = t2.sell(kStdCode, 7000.0, 10.0, 0, false);
    ASSERT_EQ(1u, ids.size());
    ASSERT_EQ(1u, api2_.vols_.size());
    EXPECT_DOUBLE_EQ(7.0, api2_.vols_[0]);
}

// TraderAdapter.cpp:1055-1102（AT_CloseToday：CM_CoverToday/CM_OpenCover + pure 保护）
TEST_F(R59Sell, close_today_uses_cover_mode_and_pure_guard)
{
    ActionPolicyMgr pm;
    ASSERT_TRUE(loadPolicy(pm, "r59_sell_ct.json",
        R"json({"default":{"order":[{"action":"closetoday"}]}})json"));

    TraderAdapter trader;
    build(trader, &pm);
    // 区分平今：平多今仓 3 手
    seedPos(trader, { makePos(ct_, WDT_LONG, 0, 0, 5, 4) });
    api2_.offsets_.clear();
    OrderIDs ids = trader.sell(kStdCode, 7000.0, 3.0, 0, false);
    ASSERT_EQ(1u, ids.size());
    EXPECT_EQ(WOT_CLOSETODAY, api2_.offsets_[0]);
    EXPECT_EQ(WDT_LONG, api2_.dirs_[0]);

    // 不区分平今：读 avail_pos(true)=3，卖平 -> 普通平仓
    seedPos(trader, { makePos(ct2_, WDT_LONG, 2, 2, 1, 1) });
    api2_.offsets_.clear();
    ids = trader.sell(kStdAu, 7000.0, 3.0, 0, false);
    ASSERT_EQ(1u, ids.size());
    EXPECT_EQ(WOT_CLOSE, api2_.offsets_[0]);

    // pure=true 且昨仓不为 0 -> 跳过
    ActionPolicyMgr pmPure;
    ASSERT_TRUE(loadPolicy(pmPure, "r59_sell_ct_pure.json",
        R"json({"default":{"order":[{"action":"closetoday","pure":true}]}})json"));
    TraderAdapter pure;
    build(pure, &pmPure);
    seedPos(pure, { makePos(ct_, WDT_LONG, 1, 1, 1, 1) });
    uint32_t before = api2_.n_insert_;
    EXPECT_TRUE(pure.sell(kStdCode, 7000.0, 1.0, 0, false).empty());
    EXPECT_EQ(before, api2_.n_insert_);
}

// TraderAdapter.cpp:1103-1144（AT_CloseYestoday：pure 跳过与 bForceClose 绕过）
TEST_F(R59Sell, close_yestoday_pure_guard_and_force_close)
{
    ActionPolicyMgr pm;
    ASSERT_TRUE(loadPolicy(pm, "r59_sell_cy.json",
        R"json({"default":{"order":[{"action":"closeyestoday"}]}})json"));

    TraderAdapter trader;
    build(trader, &pm);
    seedPos(trader, { makePos(ct_, WDT_LONG, 5, 3, 0, 0) });
    api2_.offsets_.clear();
    OrderIDs ids = trader.sell(kStdCode, 7000.0, 2.0, 0, false);
    ASSERT_EQ(1u, ids.size());
    EXPECT_EQ(WOT_CLOSE, api2_.offsets_[0]);
    EXPECT_DOUBLE_EQ(2.0, api2_.vols_[0]);

    ActionPolicyMgr pmPure;
    ASSERT_TRUE(loadPolicy(pmPure, "r59_sell_cy_pure.json",
        R"json({"default":{"order":[{"action":"closeyestoday","pure":true}]}})json"));
    TraderAdapter pure;
    build(pure, &pmPure);
    seedPos(pure, { makePos(ct_, WDT_LONG, 3, 3, 1, 1) });
    uint32_t before = api2_.n_insert_;
    EXPECT_TRUE(pure.sell(kStdCode, 7000.0, 1.0, 0, false).empty());
    EXPECT_EQ(before, api2_.n_insert_);

    ids = pure.sell(kStdCode, 7000.0, 1.0, 0, true);
    ASSERT_EQ(1u, ids.size());
    EXPECT_EQ(before + 1, api2_.n_insert_);
}

// TraderAdapter.cpp:1145-1242（AT_Close：先昨后今拆单、CM_OpenCover 单段平仓、剩余量告警、自成交入口）
TEST_F(R59Sell, close_splits_pre_then_new_and_self_match_blocks_sell)
{
    ActionPolicyMgr pm;
    ASSERT_TRUE(loadPolicy(pm, "r59_sell_close.json",
        R"json({"default":{"order":[{"action":"close"}]}})json"));

    TraderAdapter trader;
    build(trader, &pm);
    seedPos(trader, { makePos(ct_, WDT_LONG, 2, 2, 2, 2) });
    api2_.offsets_.clear();
    api2_.vols_.clear();
    OrderIDs ids = trader.sell(kStdCode, 7000.0, 3.0, 0, false);
    ASSERT_EQ(2u, ids.size());
    ASSERT_EQ(2u, api2_.offsets_.size());
    EXPECT_EQ(WOT_CLOSE, api2_.offsets_[0]);
    EXPECT_EQ(WOT_CLOSETODAY, api2_.offsets_[1]);
    EXPECT_DOUBLE_EQ(2.0, api2_.vols_[0]);
    EXPECT_DOUBLE_EQ(1.0, api2_.vols_[1]);

    // 不区分平今：可平 3 手，卖平 5 手只发 3 手，剩余 2 手走告警分支
    seedPos(trader, { makePos(ct2_, WDT_LONG, 1, 1, 2, 2) });
    uint32_t before = api2_.n_insert_;
    ids = trader.sell(kStdAu, 7000.0, 5.0, 0, false);
    ASSERT_EQ(1u, ids.size());
    EXPECT_EQ(before + 1, api2_.n_insert_);
    EXPECT_DOUBLE_EQ(3.0, api2_.last_vol_);

    // 自成交拉黑后，sell 在入口直接返回
    ASSERT_FALSE(trader.checkSelfMatch(kStdCode,
        makeTrade(ct_, WDT_LONG, WOT_OPEN, 1, 7000, "SM2", "SO1", "")));
    ASSERT_TRUE(trader.checkSelfMatch(kStdCode,
        makeTrade(ct_, WDT_LONG, WOT_OPEN, 1, 7000, "SM2", "SO2", "")));
    ASSERT_TRUE(trader.isSelfMatched(kStdCode));
    before = api2_.n_insert_;
    EXPECT_TRUE(trader.sell(kStdCode, 7000.0, 1.0, 0, false).empty());
    EXPECT_EQ(before, api2_.n_insert_);
}

} // namespace
