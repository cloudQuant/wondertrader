/**
 * r59_trader_buy_test.cpp — TraderAdapter::buy 实体路径（TraderAdapter.cpp:663-954）
 *
 * p11 把 buy() 整段列为「依赖墙钟 + 真实时段表」跳过。本文件用
 *   - WTSSessionInfo::addTradingSection(900,1500) 挂到商品上
 *   - WtHelper::setTime(20240510, 1000) 钉住进程内时间
 *   - ActionPolicyMgr::init 从临时 JSON 装载 open/closetoday/closeyestoday/close 规则
 * 把 buy() 的拆单、限额、平今/平昨/平仓、纯今纯昨保护、剩余量日志全部变成确定性路径。
 * 每笔委托都由 R59FakeTraderApi 记录方向/开平/手数。
 */
#include "r59_trader_fixture.h"

using namespace r59x;
using wtp::ActionPolicyMgr;
using wtp::TraderAdapter;

namespace {

class R59Buy : public R59TraderTest
{
};

// TraderAdapter.cpp:663-762（入口/自成交/时段/未完成量/AT_Open 拆单与市价上限）
TEST_F(R59Buy, open_splits_by_limit_volume_and_handles_market_price)
{
    ActionPolicyMgr pm;
    ASSERT_TRUE(loadPolicy(pm, "r59_buy_open.json",
        R"json({"default":{"order":[{"action":"open"}]}})json"));

    TraderAdapter trader;
    build(trader, &pm);
    api2_.vols_.clear();

    // 限价 7000 -> 单笔上限 getMaxLmtVol()=2000，5000 手拆成 2000+2000+1000
    OrderIDs ids = trader.buy(kStdCode, 7000.0, 5000.0, 0, false);
    ASSERT_EQ(3u, ids.size());
    EXPECT_EQ(3u, api2_.n_insert_);
    ASSERT_EQ(3u, api2_.vols_.size());
    EXPECT_DOUBLE_EQ(2000.0, api2_.vols_[0]);
    EXPECT_DOUBLE_EQ(2000.0, api2_.vols_[1]);
    EXPECT_DOUBLE_EQ(1000.0, api2_.vols_[2]);
    EXPECT_EQ(WDT_LONG, api2_.dirs_[0]);
    EXPECT_EQ(WOT_OPEN, api2_.offsets_[0]);
    EXPECT_EQ(WPT_LIMITPRICE, api2_.last_pxtype_);
    EXPECT_DOUBLE_EQ(5000.0, trader.getUndoneQty(kStdCode));

    // 价格 0 -> 市价单且单笔上限改成 getMaxMktVol()=1000，1500 手拆成 1000+500
    api2_.vols_.clear();
    ids = trader.buy(kStdCode, 0.0, 1500.0, 0, false);
    ASSERT_EQ(2u, ids.size());
    ASSERT_EQ(2u, api2_.vols_.size());
    EXPECT_DOUBLE_EQ(1000.0, api2_.vols_[0]);
    EXPECT_DOUBLE_EQ(500.0, api2_.vols_[1]);
    EXPECT_EQ(WPT_ANYPRICE, api2_.last_pxtype_);

    // 非交易时段（20:00 不在 09:00-15:00）-> 直接拒绝
    WtHelper::setTime(20240510, 2000, 0);
    uint32_t before = api2_.n_insert_;
    EXPECT_TRUE(trader.buy(kStdCode, 7000.0, 1.0, 0, false).empty());
    EXPECT_EQ(before, api2_.n_insert_);
    WtHelper::setTime(20240510, 1000, 0);

    // 数量为 0 的入口直接返回，不产生委托
    before = api2_.n_insert_;
    EXPECT_TRUE(trader.buy(kStdCode, 7000.0, 0.0, 0, false).empty());
    EXPECT_EQ(before, api2_.n_insert_);
}

// TraderAdapter.cpp:701-742（AT_Open 的 limit_l / limit 已达上限 continue）
TEST_F(R59Buy, open_skips_when_long_or_total_limit_reached)
{
    // 多头开仓上限：当日已开 5 手，limit_l=5 -> 规则整条跳过
    ActionPolicyMgr pm;
    ASSERT_TRUE(loadPolicy(pm, "r59_buy_lim_l.json",
        R"json({"default":{"order":[{"action":"open","limit_l":5}]}})json"));
    TraderAdapter t1;
    build(t1, &pm);
    t1.onRspTrades(makeArray({
        makeTrade(ct_, WDT_LONG, WOT_OPEN, 5, 7000, "BOL1", "BO1", "") }));
    EXPECT_TRUE(t1.buy(kStdCode, 7000.0, 1.0, 0, false).empty());
    EXPECT_EQ(0u, api2_.n_insert_);

    // 总开仓上限：多 3 + 空 2 = 5，limit=5 -> 同样跳过
    ActionPolicyMgr pm2;
    ASSERT_TRUE(loadPolicy(pm2, "r59_buy_lim_total.json",
        R"json({"default":{"order":[{"action":"open","limit":5}]}})json"));
    TraderAdapter t2;
    build(t2, &pm2);
    t2.onRspTrades(makeArray({
        makeTrade(ct_, WDT_LONG, WOT_OPEN, 3, 7000, "BOL2", "BO2", ""),
        makeTrade(ct_, WDT_SHORT, WOT_OPEN, 2, 7000, "BOS2", "BO3", "") }));
    uint32_t before = api2_.n_insert_;
    EXPECT_TRUE(t2.buy(kStdCode, 7000.0, 1.0, 0, false).empty());
    EXPECT_EQ(before, api2_.n_insert_);
}

// TraderAdapter.cpp:718-742（AT_Open 的剩余手数 min 截断与 left 递减）
TEST_F(R59Buy, open_caps_quantity_by_remaining_long_and_total_limits)
{
    // limit_l=5、已开多 2 -> 本次最多 3 手
    ActionPolicyMgr pm;
    ASSERT_TRUE(loadPolicy(pm, "r59_buy_cap_l.json",
        R"json({"default":{"order":[{"action":"open","limit_l":5,"limit":100}]}})json"));
    TraderAdapter t1;
    build(t1, &pm);
    t1.onRspTrades(makeArray({
        makeTrade(ct_, WDT_LONG, WOT_OPEN, 2, 7000, "BCL1", "BC1", "") }));
    api2_.vols_.clear();
    OrderIDs ids = t1.buy(kStdCode, 7000.0, 10.0, 0, false);
    ASSERT_EQ(1u, ids.size());
    ASSERT_EQ(1u, api2_.vols_.size());
    EXPECT_DOUBLE_EQ(3.0, api2_.vols_[0]);
    // getUndoneQty 跟踪的是「本次请求量」而非限额截断后的下单量：
    // buy() 在进入限额循环前先 updateUndone(stdCode, qty, true)（TraderAdapter.cpp:688），
    // 截断只影响实际委托（vols_[0]==3）；该表在 onRspOrders 里被整体重建
    // （TraderAdapter.cpp:1611 _undone_qty.clear()）。原断言 3 与产品语义不符。
    EXPECT_DOUBLE_EQ(10.0, t1.getUndoneQty(kStdCode));

    // limit=10、已开 2+1=3 -> 本次最多 7 手
    ActionPolicyMgr pm2;
    ASSERT_TRUE(loadPolicy(pm2, "r59_buy_cap_total.json",
        R"json({"default":{"order":[{"action":"open","limit_l":100,"limit":10}]}})json"));
    TraderAdapter t2;
    build(t2, &pm2);
    t2.onRspTrades(makeArray({
        makeTrade(ct_, WDT_LONG, WOT_OPEN, 2, 7000, "BCT1", "BCT2", ""),
        makeTrade(ct_, WDT_SHORT, WOT_OPEN, 1, 7000, "BCT3", "BCT4", "") }));
    api2_.vols_.clear();
    ids = t2.buy(kStdCode, 7000.0, 10.0, 0, false);
    ASSERT_EQ(1u, ids.size());
    ASSERT_EQ(1u, api2_.vols_.size());
    EXPECT_DOUBLE_EQ(7.0, api2_.vols_[0]);
}

// TraderAdapter.cpp:763-813（AT_CloseToday：CM_CoverToday/CM_OpenCover + pure 保护）
TEST_F(R59Buy, close_today_uses_cover_mode_and_pure_guard)
{
    ActionPolicyMgr pm;
    ASSERT_TRUE(loadPolicy(pm, "r59_buy_ct.json",
        R"json({"default":{"order":[{"action":"closetoday"}]}})json"));

    TraderAdapter trader;
    build(trader, &pm);
    // ag 区分平今：只读可平今仓 4，买平 -> 平空今仓 3 手
    seedPos(trader, { makePos(ct_, WDT_SHORT, 0, 0, 5, 4) });
    api2_.offsets_.clear();
    OrderIDs ids = trader.buy(kStdCode, 7000.0, 3.0, 0, false);
    ASSERT_EQ(1u, ids.size());
    ASSERT_EQ(1u, api2_.offsets_.size());
    EXPECT_EQ(WOT_CLOSETODAY, api2_.offsets_[0]);
    EXPECT_EQ(WDT_SHORT, api2_.dirs_[0]);
    EXPECT_DOUBLE_EQ(3.0, api2_.vols_[0]);

    // au 不区分平今：读 avail_pos(false)=3，买平 -> 普通平仓 3 手
    seedPos(trader, { makePos(ct2_, WDT_SHORT, 2, 2, 1, 1) });
    api2_.offsets_.clear();
    ids = trader.buy(kStdAu, 7000.0, 3.0, 0, false);
    ASSERT_EQ(1u, ids.size());
    EXPECT_EQ(WOT_CLOSE, api2_.offsets_[0]);

    // pure=true 且昨仓不为 0：今仓 1 手可用也不平，规则跳过
    ActionPolicyMgr pmPure;
    ASSERT_TRUE(loadPolicy(pmPure, "r59_buy_ct_pure.json",
        R"json({"default":{"order":[{"action":"closetoday","pure":true}]}})json"));
    TraderAdapter pure;
    build(pure, &pmPure);
    seedPos(pure, { makePos(ct_, WDT_SHORT, 1, 1, 1, 1) });
    uint32_t before = api2_.n_insert_;
    EXPECT_TRUE(pure.buy(kStdCode, 7000.0, 1.0, 0, false).empty());
    EXPECT_EQ(before, api2_.n_insert_);
}

// TraderAdapter.cpp:814-858（AT_CloseYestoday：pure 跳过与 bForceClose 绕过）
TEST_F(R59Buy, close_yestoday_pure_guard_and_force_close)
{
    ActionPolicyMgr pm;
    ASSERT_TRUE(loadPolicy(pm, "r59_buy_cy.json",
        R"json({"default":{"order":[{"action":"closeyestoday"}]}})json"));

    TraderAdapter trader;
    build(trader, &pm);
    seedPos(trader, { makePos(ct_, WDT_SHORT, 5, 3, 0, 0) });
    api2_.offsets_.clear();
    OrderIDs ids = trader.buy(kStdCode, 7000.0, 2.0, 0, false);
    ASSERT_EQ(1u, ids.size());
    EXPECT_EQ(WOT_CLOSE, api2_.offsets_[0]);
    EXPECT_DOUBLE_EQ(2.0, api2_.vols_[0]);

    // pure=true 且今仓不为 0 -> 跳过；bForceClose=true 绕过 pure 检查
    ActionPolicyMgr pmPure;
    ASSERT_TRUE(loadPolicy(pmPure, "r59_buy_cy_pure.json",
        R"json({"default":{"order":[{"action":"closeyestoday","pure":true}]}})json"));
    TraderAdapter pure;
    build(pure, &pmPure);
    seedPos(pure, { makePos(ct_, WDT_SHORT, 3, 3, 1, 1) });
    uint32_t before = api2_.n_insert_;
    EXPECT_TRUE(pure.buy(kStdCode, 7000.0, 1.0, 0, false).empty());
    EXPECT_EQ(before, api2_.n_insert_);

    ids = pure.buy(kStdCode, 7000.0, 1.0, 0, true);
    ASSERT_EQ(1u, ids.size());
    EXPECT_EQ(before + 1, api2_.n_insert_);
}

// TraderAdapter.cpp:859-954（AT_Close：先昨后今拆单、CM_OpenCover 单段平仓、剩余量告警、自成交入口）
TEST_F(R59Buy, close_splits_pre_then_new_and_self_match_blocks_buy)
{
    ActionPolicyMgr pm;
    ASSERT_TRUE(loadPolicy(pm, "r59_buy_close.json",
        R"json({"default":{"order":[{"action":"close"}]}})json"));

    TraderAdapter trader;
    build(trader, &pm);
    // 区分平今：先平昨 2 手，再平今 1 手
    seedPos(trader, { makePos(ct_, WDT_SHORT, 2, 2, 2, 2) });
    api2_.offsets_.clear();
    api2_.vols_.clear();
    OrderIDs ids = trader.buy(kStdCode, 7000.0, 3.0, 0, false);
    ASSERT_EQ(2u, ids.size());
    ASSERT_EQ(2u, api2_.offsets_.size());
    EXPECT_EQ(WOT_CLOSE, api2_.offsets_[0]);
    EXPECT_EQ(WOT_CLOSETODAY, api2_.offsets_[1]);
    EXPECT_DOUBLE_EQ(2.0, api2_.vols_[0]);
    EXPECT_DOUBLE_EQ(1.0, api2_.vols_[1]);

    // 不区分平今：可平 3 手，买平 5 手只发 3 手，剩余 2 手走告警分支
    seedPos(trader, { makePos(ct2_, WDT_SHORT, 1, 1, 2, 2) });
    uint32_t before = api2_.n_insert_;
    ids = trader.buy(kStdAu, 7000.0, 5.0, 0, false);
    ASSERT_EQ(1u, ids.size());
    EXPECT_EQ(before + 1, api2_.n_insert_);
    EXPECT_DOUBLE_EQ(3.0, api2_.last_vol_);

    // 自成交拉黑后，buy 在入口直接返回
    ASSERT_FALSE(trader.checkSelfMatch(kStdCode,
        makeTrade(ct_, WDT_LONG, WOT_OPEN, 1, 7000, "SM1", "SO1", "")));
    ASSERT_TRUE(trader.checkSelfMatch(kStdCode,
        makeTrade(ct_, WDT_LONG, WOT_OPEN, 1, 7000, "SM1", "SO2", "")));
    ASSERT_TRUE(trader.isSelfMatched(kStdCode));
    before = api2_.n_insert_;
    EXPECT_TRUE(trader.buy(kStdCode, 7000.0, 1.0, 0, false).empty());
    EXPECT_EQ(before, api2_.n_insert_);
}

} // namespace
