/**
 * r59_trader_misc_test.cpp — TraderAdapter 其余可达分支
 *
 * 覆盖 p11 未触及的入口：
 *   initExt 空 API / 通道 init 失败、无 API 时 run() 早退（:209-235, :404-417）;
 *   init() 落地 _cfg 后 handleEvent(WTE_Connect, 0) 用配置里的账号登录（:1405-1417）;
 *   四个回报数组传 NULL 时状态机仍推进并广播 on_channel_ready（:1514-1543,
 *   :1545-1602, :1604-1730, :1739-1816 的空指针分支）;
 *   onPushTrade 无合约信息时直接忽略（:2148-2152）;
 *   TraderAdapterMgr::addAdapter 的 NULL/空名护栏与就绪通道 refresh_funds（:2369-2422）。
 *
 * 另收 1 条 DISABLED_：净头寸模式下卖出量大于多头今仓时，TraderAdapter.cpp:2266
 * 用 left 而不是 maxVol 扣减 l_newvol，持仓被扣成负数（BUG-R59-001），
 * 正确结果应断言 0 多 / 3 空，故该用例默认不执行。
 */
#include "r59_trader_fixture.h"

using namespace r59x;
using wtp::ActionPolicyMgr;
using wtp::TraderAdapter;
using wtp::TraderAdapterMgr;

namespace {

class R59Misc : public R59TraderTest
{
};

// TraderAdapter.cpp:209-235, 404-417（initExt 空指针/api init 失败；无 api 的 run 早退）
TEST_F(R59Misc, init_ext_guards_and_run_without_api)
{
    TraderAdapter none;
    EXPECT_FALSE(none.initExt(id_.c_str(), nullptr, &bd_, nullptr));
    EXPECT_FALSE(none.run());

    p11fix::FakeTraderApi failing;
    failing.init_ret_ = false;
    TraderAdapter bad;
    EXPECT_FALSE(bad.initExt(id_.c_str(), &failing, &bd_, nullptr));
    EXPECT_EQ(1u, failing.n_init_);
    // initExt 在 init 失败前已经把 _trader_api 落位，所以 run() 仍能装配通道
    EXPECT_TRUE(bad.run());
    EXPECT_EQ(1u, failing.n_register_);
}

// TraderAdapter.cpp:99-207, 1405-1417（init 落地 _cfg 后 WTE_Connect 成功用配置账号登录）
TEST_F(R59Misc, connect_event_uses_cfg_credentials)
{
    TraderAdapter trader;
    ASSERT_TRUE(trader.initExt(id_.c_str(), &api2_, &bd_, nullptr));

    // 只带风控之外的最小配置：module 为空 -> init 返回 false，但 _cfg 已落地
    auto path = tmp_ / "r59_connect.json";
    {
        std::ofstream out(path);
        out << R"json({"savedata":false,"user":"r59_user","pass":"r59_pass","module":""})json";
    }
    wtp::WTSVariant* cfg = WTSCfgLoader::load_from_file(path.string().c_str());
    ASSERT_NE(nullptr, cfg);
    EXPECT_FALSE(trader.init(id_.c_str(), cfg, &bd_, nullptr));
    cfg->release();

    // 连接成功事件用 _cfg 的账号登录（不读墙钟、不联网）
    trader.handleEvent(WTE_Connect, 0);
    EXPECT_EQ(1u, api2_.n_login_);
    EXPECT_EQ(TraderAdapter::AS_NOTLOGIN, trader.state());

    // 非 0 事件码只记日志，不登录
    trader.handleEvent(WTE_Connect, -2);
    EXPECT_EQ(1u, api2_.n_login_);
}

// TraderAdapter.cpp:1514-1602, 1604-1730, 1739-1816（四个回报数组为 NULL 时的状态机推进）
TEST_F(R59Misc, null_report_arrays_advance_state_machine)
{
    ActionPolicyMgr pm;
    ASSERT_TRUE(loadPolicy(pm, "r59_misc_none.json",
        R"json({"default":{"order":[{"action":"open"}]}})json"));

    TraderAdapter trader;
    build(trader, &pm);             // 已登录 AS_LOGINED，并触发一次查持仓
    EXPECT_EQ(1u, api2_.n_qpositions_);

    // 四个 NULL 回报：不写任何本地表，但状态机一步步推进到 AS_ALLREADY
    makeReady(trader);

    EXPECT_EQ(1u, api2_.n_qorders_);
    EXPECT_EQ(1u, api2_.n_qtrades_);
    EXPECT_EQ(1u, api2_.n_qaccount_);
    EXPECT_EQ(1u, sink_.n_ready_);
    EXPECT_TRUE(trader.isReady());
    EXPECT_DOUBLE_EQ(0.0, trader.getPosition(kStdCode, false, 3));
    EXPECT_EQ(nullptr, trader.getOrders(kStdCode));
    EXPECT_TRUE(sink_.trades_.empty());
}

// TraderAdapter.cpp:2148-2152（onPushTrade 无合约信息直接返回）
TEST_F(R59Misc, push_trade_without_contract_is_ignored)
{
    ActionPolicyMgr pm;
    ASSERT_TRUE(loadPolicy(pm, "r59_misc_trade.json",
        R"json({"default":{"order":[{"action":"open"}]}})json"));

    TraderAdapter trader;
    build(trader, &pm);

    // 没有合约信息的成交：在写任何表之前返回
    WTSTradeInfo* t = WTSTradeInfo::create(kRawCode, kExchg);
    t->setDirection(WDT_LONG);
    t->setOffsetType(WOT_OPEN);
    t->setVolume(2);
    t->setPrice(7000.0);
    trader.onPushTrade(t);

    EXPECT_TRUE(sink_.trades_.empty());
    EXPECT_DOUBLE_EQ(0.0, trader.getUndoneQty(kStdCode));
    EXPECT_DOUBLE_EQ(0.0, trader.getPosition(kStdCode, false, 3));
}

// TraderAdapter.cpp:2369-2422（addAdapter 的 NULL/空名护栏 + 就绪通道 refresh_funds）
TEST_F(R59Misc, manager_rejects_bad_channels_and_refreshes_ready_funds)
{
    TraderAdapterMgr mgr;
    TraderAdapterPtr nullAdapter;
    EXPECT_FALSE(mgr.addAdapter("r59m", nullAdapter));

    ActionPolicyMgr pm;
    ASSERT_TRUE(loadPolicy(pm, "r59_misc_mgr.json",
        R"json({"default":{"order":[{"action":"open"}]}})json"));
    TraderAdapterPtr ready(new TraderAdapter());
    build(*ready, &pm);
    makeReady(*ready);

    TraderAdapterPtr emptyName(new TraderAdapter());
    EXPECT_FALSE(mgr.addAdapter("", emptyName));

    ASSERT_TRUE(mgr.addAdapter("r59m", ready));
    EXPECT_EQ(1u, mgr.getAdapters().size());

    // 已就绪 -> refresh_funds 真的下发查资金
    uint32_t before = api2_.n_qaccount_;
    mgr.refresh_funds();
    EXPECT_EQ(before + 1, api2_.n_qaccount_);
}

/*
 * DISABLED_：净头寸模式下平仓量大于多头今仓时持仓被扣成负数。
 * 依据 TraderAdapter.cpp:2263-2269：
 *     maxVol = min(left, pItem.l_newvol);
 *     if (decimal::gt(maxVol, 0))
 *     { pItem.l_newvol -= left; left -= maxVol; ... }
 * 应扣 maxVol(2) 却扣了 left(5)，l_newvol = 2-5 = -3；正确的净头寸结果应为
 * 多头 0 手、空头 3 手。详见 bugs/R59.md BUG-R59-001。启用后本用例会失败。
 */
// TraderAdapter.cpp:2246-2305（净头寸先平后开；2266/2290 用 left 扣减今仓，见 BUG-R59-001）
TEST_F(R59Misc, DISABLED_net_mode_sell_more_than_long_new_corrupts_position)
{
    ActionPolicyMgr pm;
    ASSERT_TRUE(loadPolicy(pm, "r59_misc_net.json",
        R"json({"default":{"order":[{"action":"open"}]}})json"));

    TraderAdapter trader;
    build(trader, &pm);
    seedPos(trader, { makePos(ct_, WDT_LONG, 0, 0, 2, 2) });

    WTSTradeInfo* t = makeTrade(ct_, WDT_LONG, WOT_OPEN, 5, 7000, "NET1", "NETO1", "");
    t->setNetDirection(false);      // 净头寸卖出 5
    trader.onPushTrade(t);

    EXPECT_DOUBLE_EQ(0.0, trader.getPosition(kStdCode, false, 1));    // 多头应清零
    EXPECT_DOUBLE_EQ(-3.0, trader.getPosition(kStdCode, false, 2));   // 剩余 3 手转空（flag=2 返回负值）
}

} // namespace
