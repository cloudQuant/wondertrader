/**
 * r81_hft_gap_test.cpp — lane R81 / HftStraBaseCtx 剩余行段
 *
 * 覆盖 src/WtCore/HftStraBaseCtx.cpp：
 *   stra_cancel/stra_buy/stra_sell 的风控拒绝分支（checkCancelLimits/checkOrderLimits 为 false）
 *   stra_get_position/stra_get_undone 的自定义规则映射分支、卖单合约缺失守卫
 *   on_trade 的脏用户数据落盘、do_set_position 的开仓/平仓/反手与空头盈亏取反
 *   load_userdata 的空文件早退
 *
 * 风控开关通过 TraderAdapter 公有 initExt -> init(riskmon, module 为空) -> run 装配，
 * 不加载任何交易插件、不连接柜台、不起线程。
 */
#include "r81_env.h"

#include <memory>
#include <string>

#include <WtCore/ActionPolicyMgr.h>
#include <WtCore/TraderAdapter.h>

using namespace wtp;

namespace {

WTSVariant* makeRiskParams(uint32_t orderLimit)
{
    WTSVariant* params = WTSVariant::createObject();
    params->append("module", "");

    WTSVariant* riskmon = WTSVariant::createObject();
    riskmon->append("active", true);

    WTSVariant* policyCfg = WTSVariant::createObject();
    WTSVariant* rule = WTSVariant::createObject();
    rule->append("cancel_total_limits", (uint32_t)0);
    rule->append("cancel_times_boundary", (uint32_t)0);
    rule->append("cancel_stat_timespan", (uint32_t)0);
    rule->append("order_total_limits", orderLimit);
    rule->append("order_times_boundary", (uint32_t)0);
    rule->append("order_stat_timespan", (uint32_t)0);
    policyCfg->append("default", rule);

    riskmon->append("policy", policyCfg);
    params->append("riskmon", riskmon);
    return params;
}

WTSOrderInfo* makeOrder(WTSContractInfo* cInfo, const char* orderid)
{
    WTSOrderInfo* o = WTSOrderInfo::create();
    o->setCode(cInfo->getCode());
    o->setExchange(cInfo->getExchg());
    o->setContractInfo(cInfo);
    o->setDirection(WDT_LONG);
    o->setOffsetType(WOT_OPEN);
    o->setVolume(1);
    o->setVolLeft(1);
    o->setOrderState(WOS_AllTraded);
    o->setOrderID(orderid);
    return o;
}

class R81HftTest : public ::testing::Test
{
protected:
    r81::R81GenEnv env;
    r81::R81BaseDataMgr bd;
    r81::R81HotMgr hot;
    r81::R81HftEngine engine;
    WTSContractInfo* agCt = NULL;

    void SetUp() override
    {
        bd.regisSession("TRADING");
        WTSSessionInfo* sess = bd.session("TRADING");
        ASSERT_NE(sess, nullptr);
        sess->addTradingSection(900, 1500);

        WTSCommodityInfo* ag = r81::makeComm("SHFE", "ag", "TRADING", TM_Both);
        ag->setSessionInfo(sess);
        agCt = r81::makeCt("SHFE", "ag", "ag2512");
        bd.regis(ag, agCt);

        WTSCommodityInfo* au = r81::makeComm("SHFE", "au", "TRADING", TM_Long);
        au->setSessionInfo(sess);
        bd.regis(au, r81::makeCt("SHFE", "au", "au2212"));

        engine.attach(&bd, &hot);
    }
};

// 覆盖 HftStraBaseCtx.cpp:188-200 stra_cancel 风控拒绝、211-259 stra_buy 两条拒绝、
// 262-323 stra_sell 两条拒绝
TEST_F(R81HftTest, RiskRejectsCancelBuyAndSellBranches)
{
    std::string policyFile = r81::temp_root() + "r81_policy.json";
    ASSERT_TRUE(r81::write_file(policyFile, R"json({"default":{"order":[{"action":"open"}]}})json"));
    ActionPolicyMgr policy;
    ASSERT_TRUE(policy.init(policyFile.c_str()));

    r81::R81FakeTraderApi api;
    TraderAdapter trader;

    // 先给适配器一个进程内 API，再用 module 为空的配置打开 riskmon；
    // init 在解析完 riskmon 后于空 module 处返回 false，但 _trader_api/_bd_mgr 已就绪。
    ASSERT_TRUE(trader.initExt("r81_hft_risk", &api, &bd, &policy));
    WTSVariant* params = makeRiskParams(1);
    EXPECT_FALSE(trader.init("r81_hft_risk", params, &bd, &policy));
    params->release();
    ASSERT_TRUE(trader.run());

    // 推一笔订单，把 SHFE.ag.2512 的 total_orders 顶到 1（限额 1）
    WTSOrderInfo* order = makeOrder(agCt, "R81-ORDER-1");
    trader.onPushOrder(order);
    order->release();

    r81::R81HftCtx ctx(&engine, "r81_hft_risk", false, 0);
    ctx.setTrader(&trader);
    ctx.on_init();

    // 非规则代码：checkOrderLimits 命中总量超限并拉黑
    EXPECT_TRUE(ctx.stra_buy("SHFE.ag.2512", 3500.0, 1.0, "b1").empty());
    // 自定义规则代码：映射到 realCode 后同样被拒
    EXPECT_TRUE(ctx.stra_buy("SHFE.ag.HOT", 3500.0, 1.0, "b2").empty());
    // 卖单两条分支
    EXPECT_TRUE(ctx.stra_sell("SHFE.ag.2512", 3500.0, 1.0, "s1").empty());
    EXPECT_TRUE(ctx.stra_sell("SHFE.ag.HOT", 3500.0, 1.0, "s2").empty());
    // 已拉黑 -> checkCancelLimits 直接 false
    EXPECT_TRUE(ctx.stra_cancel("SHFE.ag.2512", true, 0.0).empty());
    EXPECT_EQ(api.n_insert, 0u);
}

// 覆盖 HftStraBaseCtx.cpp:631-685 规则/非规则仓量查询、262-310 卖单合约缺失
TEST_F(R81HftTest, RuleCodeQueriesAndMissingContractSell)
{
    TraderAdapter trader; // 未 init/run：getPosition/getUndoneQty 对空表安全
    r81::R81HftCtx ctx(&engine, "r81_hft_query", false, 0);
    ctx.setTrader(&trader);
    ctx.on_init();

    // 规则代码 -> getCustomRawCode 映射为 ag2512 -> SHFE.ag.2512
    EXPECT_DOUBLE_EQ(ctx.stra_get_position("SHFE.ag.HOT"), 0.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_undone("SHFE.ag.HOT"), 0.0);
    EXPECT_STREQ(ctx.get_inner_code("SHFE.ag.2512"), "SHFE.ag.HOT");

    // 非规则代码走 else 直通
    EXPECT_DOUBLE_EQ(ctx.stra_get_position("SHFE.ag.2512"), 0.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_undone("SHFE.ag.2512"), 0.0);

    // 品种存在、合约不存在 -> 卖单在 getContract 处早退（买同路径已有覆盖）
    EXPECT_TRUE(ctx.stra_sell("SHFE.ag.9999", 3500.0, 1.0, "miss").empty());
    EXPECT_TRUE(ctx.stra_buy("SHFE.ag.9999", 3500.0, 1.0, "missb").empty());
}

// 覆盖 HftStraBaseCtx.cpp:536-553 on_trade 脏数据落盘、754-783 空 userdata 早退、
// 785-904 do_set_position 开/平/反手与空头盈亏
TEST_F(R81HftTest, UserDataEmptyFileAndTradeAccounting)
{
    const char* name = "r81_hft_trade";
    ASSERT_TRUE(r81::write_file(r81::user_data_file(name), ""));

    r81::R81HftCtx ctx(&engine, name, false, 0);
    ctx.on_init();
    EXPECT_STREQ(ctx.stra_load_user_data("missing", "def"), "def");

    // 脏标记 -> on_trade 开头落盘
    ctx.stra_save_user_data("k1", "v1");
    ctx.on_trade(1, "SHFE.ag.2512", true, 2.0, 3500.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_avgpx("SHFE.ag.2512"), 3500.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_profit("SHFE.ag.2512"), 0.0);

    // 多头部分平仓、全平
    ctx.on_trade(2, "SHFE.ag.2512", false, 1.0, 3600.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_avgpx("SHFE.ag.2512"), 3500.0);
    ctx.on_trade(3, "SHFE.ag.2512", false, 1.0, 3400.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_avgpx("SHFE.ag.2512"), 0.0);

    // 空仓反手做空，再买入平仓：覆盖 !_long 的盈亏取反
    ctx.on_trade(4, "SHFE.ag.2512", false, 2.0, 3500.0);
    ctx.on_trade(5, "SHFE.ag.2512", true, 1.0, 3400.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_avgpx("SHFE.ag.2512"),
        -3500.0); // 负数均价为既有口径（q02 已登记）
    ctx.on_trade(6, "SHFE.ag.2512", true, 1.0, 3300.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_avgpx("SHFE.ag.2512"), 0.0);

    const std::string ud = r81::read_file(r81::user_data_file(name));
    EXPECT_NE(ud.find("\"k1\""), std::string::npos);

    r81::R81HftCtx reloaded(&engine, name, false, 0);
    reloaded.on_init();
    EXPECT_STREQ(reloaded.stra_load_user_data("k1"), "v1");
}

} // namespace
