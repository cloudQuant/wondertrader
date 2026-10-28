/**
 * r71_hft_ctx_test.cpp — lane R71 / HftStraBaseCtx 余量补测
 *
 * 覆盖 src/WtCore/HftStraBaseCtx.cpp 中 R42/Q01 未覆盖的下单/查询/切片面：
 *   stra_buy/sell、enter/exit_long/short、cancel 的正常与自定义规则映射
 *   stra_get_position/undone、get_inner_code、stra_get_price
 *   on_trade -> do_set_position 的开仓/平仓/手续费与日志
 *   HftStraContext::on_tick -> update_dyn_profit 的正/负极值
 *   stra_get_bars/ticks/order_detail/order_queue/transaction/last_tick
 *
 * 交易侧用真实 TraderAdapter + 进程内 ITraderApi 替身，不连接任何柜台。
 */
#include "r71_wtcore_env.h"

#include <WtCore/HftStraContext.h>

using namespace wtp;

namespace {

class R71HftCtx : public HftStraContext
{
public:
    R71HftCtx(WtHftEngine* engine, const char* name, bool agent, int32_t slippage)
        : HftStraContext(engine, name, agent, slippage)
    {
    }

    ~R71HftCtx() override {}

    using HftStraBaseCtx::get_inner_code;
};

class R71HftTest : public ::testing::Test
{
protected:
    r71::R71GenEnv env;
    r71::R71TraderEnv traderEnv;
    r71::R71DataMgr dm;
    r71::R71HftEngine engine;
    TraderAdapter trader;

    void SetUp() override
    {
        traderEnv.bd.regisSession("TRADING");
        WTSSessionInfo* sess = traderEnv.bd.session("TRADING");
        ASSERT_NE(sess, nullptr);
        sess->addTradingSection(900, 1500);

        WTSCommodityInfo* ag = r71::makeComm("SHFE", "ag", "TRADING", TM_Both);
        ag->setSessionInfo(sess);
        traderEnv.bd.regis(ag, r71::makeCt("SHFE", "ag", "ag2512"));

        WTSCommodityInfo* au = r71::makeComm("SHFE", "au", "TRADING", TM_Long);
        au->setSessionInfo(sess);
        traderEnv.bd.regis(au, r71::makeCt("SHFE", "au", "au2212"));

        engine.attach(&traderEnv.bd, &traderEnv.hot, &dm);
        ASSERT_TRUE(traderEnv.build(trader, "r71_hft"));
    }

    std::unique_ptr<R71HftCtx> makeCtx(const char* name, int32_t slippage = 0)
    {
        std::unique_ptr<R71HftCtx> ctx(new R71HftCtx(&engine, name, true, slippage));
        ctx->setTrader(&trader);
        ctx->on_init();
        return ctx;
    }
};

// 覆盖 HftStraBaseCtx.cpp:188-259 stra_buy/cancel/正常合约路径、
// L262-322 stra_sell、L325-414 enter/exit、L631-694 查询与价格
TEST_F(R71HftTest, TradingMethodsForwardToTraderAdapter)
{
    auto ctx = makeCtx("r71_hft_trade");
    const uint32_t before = traderEnv.api.n_insert;

    OrderIDs b = ctx->stra_buy("SHFE.ag.2512", 3500.0, 2.0, "b");
    EXPECT_EQ(b.size(), 1u);
    EXPECT_EQ(traderEnv.api.n_insert, before + 1);
    EXPECT_STREQ(traderEnv.api.last_code.c_str(), "ag2512");
    EXPECT_EQ(traderEnv.api.last_dir, WDT_LONG);
    EXPECT_EQ(traderEnv.api.last_off, WOT_OPEN);

    OrderIDs s = ctx->stra_sell("SHFE.ag.2512", 3501.0, 1.0, "s");
    EXPECT_EQ(s.size(), 1u);
    EXPECT_EQ(traderEnv.api.last_dir, WDT_SHORT);

    EXPECT_NE(ctx->stra_enter_long("SHFE.ag.2512", 3500.0, 1.0, "el", 0), 0u);
    EXPECT_NE(ctx->stra_exit_long("SHFE.ag.2512", 3500.0, 1.0, "xl", true, 0), 0u);
    EXPECT_NE(ctx->stra_enter_short("SHFE.ag.2512", 3500.0, 1.0, "es", 0), 0u);
    EXPECT_NE(ctx->stra_exit_short("SHFE.ag.2512", 3500.0, 1.0, "xs", true, 0), 0u);

    EXPECT_FALSE(ctx->stra_cancel(123u));
    EXPECT_TRUE(ctx->stra_cancel("SHFE.ag.2512", true, 0.0).empty());

    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.ag.2512"), 0.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_undone("SHFE.ag.2512"), 0.0);
    EXPECT_STREQ(ctx->get_inner_code("SHFE.ag.2512"), "SHFE.ag.2512");

    engine.prime_price("SHFE.ag.2512", 3510.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_price("SHFE.ag.2512"), 3510.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_price("SHFE.au.2212"), 3502.0); // 引擎兜底
}

// 覆盖 HftStraBaseCtx.cpp:218-238/283-301 自定义规则映射、
// L242-247 合约缺失、L268-276 不可做空且可用不足
TEST_F(R71HftTest, CustomRuleMappingAndTradingGuards)
{
    auto ctx = makeCtx("r71_hft_custom");

    OrderIDs b = ctx->stra_buy("SHFE.ag.HOT", 3500.0, 1.0, "hb");
    EXPECT_EQ(b.size(), 1u);
    EXPECT_STREQ(ctx->get_inner_code("SHFE.ag.2512"), "SHFE.ag.HOT");
    EXPECT_STREQ(traderEnv.api.last_code.c_str(), "ag2512");

    OrderIDs s = ctx->stra_sell("SHFE.ag.HOT", 3500.0, 1.0, "hs");
    EXPECT_EQ(s.size(), 1u);

    EXPECT_NE(ctx->stra_enter_long("SHFE.ag.HOT", 3500.0, 1.0, "hel", 0), 0u);
    EXPECT_NE(ctx->stra_exit_long("SHFE.ag.HOT", 3500.0, 1.0, "hxl", false, 0), 0u);
    EXPECT_NE(ctx->stra_enter_short("SHFE.ag.HOT", 3500.0, 1.0, "hes", 0), 0u);
    EXPECT_NE(ctx->stra_exit_short("SHFE.ag.HOT", 3500.0, 1.0, "hxs", false, 0), 0u);

    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.ag.HOT"), 0.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_undone("SHFE.ag.HOT"), 0.0);

    // 未登记合约 -> getContract 为空，直接返回空单号
    EXPECT_TRUE(ctx->stra_buy("DCE.rr.2509", 100.0, 1.0, "bad").empty());

    // 不可做空品种且可用持仓为 0 -> 拒绝卖出
    EXPECT_TRUE(ctx->stra_sell("SHFE.au.2212", 100.0, 1.0, "noshort").empty());
}

// 覆盖 HftStraBaseCtx.cpp:536-552 on_trade、L785-904 do_set_position、
// L906-939 update_dyn_profit、L946-976 会话结算与日志落盘
TEST_F(R71HftTest, TradeAccountingAndDynProfitExtremes)
{
    auto ctx = makeCtx("r71_hft_acct", 2);

    ctx->on_trade(0, "SHFE.ag.2512", true, 2.0, 3500.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.ag.2512"), 2.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position_avgpx("SHFE.ag.2512"), 3502.0);

    // 多头按 bidprice 估值：正极值
    WTSTickData* up = r71::makeTick("SHFE", "SHFE.ag.2512", 20250110, 20250110, 93100000, 3500.0, 3600.0, 3400.0);
    ASSERT_NE(up, nullptr);
    ctx->on_tick("SHFE.ag.2512", up);
    up->release();
    EXPECT_DOUBLE_EQ(ctx->stra_get_position_profit("SHFE.ag.2512"), 1960.0);

    // 负极值
    WTSTickData* down = r71::makeTick("SHFE", "SHFE.ag.2512", 20250110, 20250110, 93101000, 3400.0, 3400.0, 3600.0);
    ASSERT_NE(down, nullptr);
    ctx->on_tick("SHFE.ag.2512", down);
    down->release();
    EXPECT_DOUBLE_EQ(ctx->stra_get_position_profit("SHFE.ag.2512"), -2040.0);

    // 部分平仓 -> 剩余 1 手；再全平
    ctx->on_trade(1, "SHFE.ag.2512", false, 1.0, 3600.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.ag.2512"), 1.0);
    ctx->on_trade(2, "SHFE.ag.2512", false, 1.0, 3600.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.ag.2512"), 0.0);

    ctx->on_session_end(20250110);
    const std::string out = std::string(WtHelper::getOutputDir()) + "r71_hft_acct/";
    EXPECT_NE(r71::read_file(out + "trades.csv").find("SHFE.ag.2512"), std::string::npos);
    EXPECT_NE(r71::read_file(out + "closes.csv").find("SHFE.ag.2512"), std::string::npos);
    EXPECT_NE(r71::read_file(out + "funds.csv").find("20250110"), std::string::npos);
}

// 覆盖 HftStraBaseCtx.cpp:426-483 历史切片与 L2 订阅入口
TEST_F(R71HftTest, HistoricalSlicesAndL2Subscriptions)
{
    auto ctx = makeCtx("r71_hft_slices");

    WTSKlineSlice* bars = ctx->stra_get_bars("SHFE.ag.2512", "m5", 2);
    ASSERT_NE(bars, nullptr);
    EXPECT_EQ(bars->size(), 3u);
    bars->release();
    EXPECT_EQ(dm.kline_calls, 1u);

    WTSTickSlice* ticks = ctx->stra_get_ticks("SHFE.ag.2512", 2);
    ASSERT_NE(ticks, nullptr);
    EXPECT_EQ(ticks->size(), 3u);
    ticks->release();
    EXPECT_EQ(dm.tick_calls, 1u);

    WTSOrdDtlSlice* dtl = ctx->stra_get_order_detail("SHFE.ag.2512", 1);
    ASSERT_NE(dtl, nullptr);
    dtl->release();
    WTSOrdQueSlice* que = ctx->stra_get_order_queue("SHFE.ag.2512", 1);
    ASSERT_NE(que, nullptr);
    que->release();
    WTSTransSlice* trans = ctx->stra_get_transaction("SHFE.ag.2512", 1);
    ASSERT_NE(trans, nullptr);
    trans->release();
    EXPECT_EQ(dm.dtl_calls, 1u);
    EXPECT_EQ(dm.que_calls, 1u);
    EXPECT_EQ(dm.trans_calls, 1u);

    WTSTickData* last = ctx->stra_get_last_tick("SHFE.ag.2512");
    ASSERT_NE(last, nullptr);
    last->release();

    ctx->stra_sub_ticks("SHFE.ag.2512");
    ctx->stra_sub_order_details("SHFE.ag.2512");
    ctx->stra_sub_order_queues("SHFE.ag.2512");
    ctx->stra_sub_transactions("SHFE.ag.2512");

    WTSCommodityInfo* comm = ctx->stra_get_comminfo("SHFE.ag.2512");
    ASSERT_NE(comm, nullptr);
    EXPECT_STREQ(comm->getProduct(), "ag");
    EXPECT_EQ(ctx->stra_get_rawcode("SHFE.ag.2512"), std::string());
}

} // namespace
