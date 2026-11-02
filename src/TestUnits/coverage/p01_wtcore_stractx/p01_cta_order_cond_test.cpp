/**
 * p01_cta_order_cond_test.cpp
 * 覆盖 src/WtCore/CtaStraBaseCtx.cpp 的「下单前置校验 + 条件单装配」：
 *   - stra_enter_long/short：品种缺失拒绝、canShort 拒绝、立即模式下按当前仓位累加
 *   - stra_exit_long：可平持仓（bOnlyValid）<= 0 时早退；有仓时按 min(curQty, qty) 递减
 *   - stra_exit_short：当前是多仓时早退、canShort 拒绝
 *   - limitprice/stopprice 分支：只登记条件单（CondEntrust），不产生信号
 *   - on_tick 的条件单撮合：WCT_SmallerOrEqual / WCT_LargerOrEqual 判定、触发后条件清单被清空
 *   - stra_set_position：目标仓位等于当前仓位时不登记条件单
 *   - on_tick 中 bEmitStrategy == false 时不回调策略、条件单触发时 sigtype = 2
 * 会真正走到 do_set_position（需要引擎的组合持仓簿）与真实柜台回报的分支见 manifest 跳过清单；
 * 缺 session 用真实 WTSBaseDataMgr rules 自动建商品并验证平仓/待发信号边界；不以未初始化
 * engine 上的历史查询作为产品复现或验收。
 */
#include "p01_cta_env.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include <WTSTools/WTSBaseDataMgr.h>

namespace {

class P01CtaOrderTest : public ::testing::Test
{
protected:
    P01TempEnv          env{ "cta_order" };
    FakeBaseDataMgr     bd;
    FakeHotMgr          hot;
    P01CtaEngine        engine;
    WTSCommodityInfo*   rbComm = NULL;
    std::unique_ptr<P01CtaCtx> ctx;

    static const char* kCode;

    void SetUp() override
    {
        bd.regisSession("TRADING");

        rbComm = p01MakeComm("SHFE", "rb", "TRADING", TM_Both);
        // stra_exit_long 会直接用 commInfo->getSessionInfo()，没有时段就会空指针
        rbComm->setSessionInfo(bd.getSession("TRADING"));
        bd.regis(rbComm, p01MakeCt("SHFE", "rb", "rb2510"));

        // 不可做空：TM_LongT1 => canShort() == false
        WTSCommodityInfo* ofComm = p01MakeComm("SZSE", "of", "TRADING", TM_LongT1);
        ofComm->setSessionInfo(bd.getSession("TRADING"));
        bd.regis(ofComm, p01MakeCt("SZSE", "of", "of159915"));

        engine.attach(&bd, &hot);
        ctx.reset(new P01CtaCtx(&engine, "p01o_stra", 0));
    }

    // 喂一条 tick；bEmitStrategy 固定 false，避免策略回调干扰断言
    void feed(const char* code, double price, bool bEmit = false)
    {
        WTSTickData* tick = p01MakeTick("SHFE", "rb2510", 20250110, 20250110, 93100000, price);
        ASSERT_NE(tick, nullptr);
        ctx->on_tick(code, tick, bEmit);
        tick->release();
    }
};

// 三段式：CodeHelper::extractStdCode 对两段串（SHFE.rb2510）会把整段当作 product，
// 于是 getCommodity("SHFE","rb2510") 取不到夹具里注册的 product "rb"，
// stra_enter_long 在 commInfo==NULL 处早退，仓位读回 0（见 A-04 同根因）。
const char* P01CtaOrderTest::kCode = "SHFE.rb.2510";

//////////////////////////////////////////////////////////////////////////
// 前置校验
//////////////////////////////////////////////////////////////////////////
TEST_F(P01CtaOrderTest, unknown_contract_is_rejected_without_any_state)
{
    ctx->stra_enter_long("SHFE.rb9999", 2, "t1");
    ctx->stra_enter_short("SHFE.rb9999", 2, "t2");
    ctx->stra_exit_long("SHFE.rb9999", 2, "t3");
    ctx->stra_exit_short("SHFE.rb9999", 2, "t4");
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.rb9999"), 0.0);
    EXPECT_EQ(ctx->tick_updates, 0u);
    EXPECT_EQ(ctx->cond_fires, 0u);
}

TEST_F(P01CtaOrderTest, short_is_rejected_on_non_shortable_product)
{
    ctx->stra_enter_short("SZSE.of.159915", 3, "tag");
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SZSE.of.159915"), 0.0);

    ctx->stra_exit_short("SZSE.of.159915", 1, "tag");
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SZSE.of.159915"), 0.0);

    // 开多不受 canShort 限制
    ctx->stra_enter_long("SZSE.of.159915", 1, "tag");
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SZSE.of.159915"), 1.0);
}

// 当前没有仓位时，平多/平空都必须早退
TEST_F(P01CtaOrderTest, exit_on_flat_book_is_noop)
{
    ctx->stra_exit_long(kCode, 2, "t");
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 0.0);

    ctx->stra_exit_short(kCode, 2, "t");
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 0.0);
}

// 空头持仓时平多必须早退（curQty >= 0 判定）
TEST_F(P01CtaOrderTest, exit_long_ignored_when_position_is_short)
{
    ctx->stra_enter_short(kCode, 3, "short");
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), -3.0);

    ctx->stra_exit_long(kCode, 1, "long");
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), -3.0);
}

// 多头持仓时平空必须早退
TEST_F(P01CtaOrderTest, exit_short_ignored_when_position_is_long)
{
    ctx->stra_enter_long(kCode, 3, "long");
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 3.0);

    ctx->stra_exit_short(kCode, 1, "short");
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 3.0);
}

//////////////////////////////////////////////////////////////////////////
// 立即模式（limitprice == stopprice == 0）的仓位累加
//////////////////////////////////////////////////////////////////////////
TEST_F(P01CtaOrderTest, enter_long_accumulates_on_top_of_current_qty)
{
    ctx->stra_enter_long(kCode, 2, "a");
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 2.0);

    ctx->stra_enter_long(kCode, 3, "b");
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 5.0);

    // 已有空头时反手到目标手数
    ctx->stra_set_position(kCode, -4, "c");
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), -4.0);
    ctx->stra_enter_long(kCode, 2, "d");
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 2.0);
}

TEST_F(P01CtaOrderTest, enter_short_accumulates_below_zero)
{
    ctx->stra_enter_short(kCode, 2, "a");
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), -2.0);

    ctx->stra_enter_short(kCode, 1, "b");
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), -3.0);

    // 已有多头时反手到 -qty
    ctx->stra_set_position(kCode, 5, "c");
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 5.0);
    ctx->stra_enter_short(kCode, 3, "d");
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), -3.0);
}

TEST_F(P01CtaOrderTest, exit_long_cuts_at_most_the_available_qty)
{
    ctx->stra_set_position(kCode, 3, "full");
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 3.0);

    // 超出当前持仓的平仓手数被截断到 curQty
    ctx->stra_exit_long(kCode, 10, "part");
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 0.0);

    ctx->stra_set_position(kCode, 5, "again");
    ctx->stra_exit_long(kCode, 2, "two");
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 3.0);
}

TEST_F(P01CtaOrderTest, exit_short_cuts_at_most_the_available_qty)
{
    ctx->stra_set_position(kCode, -4, "short");
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), -4.0);

    ctx->stra_exit_short(kCode, 1, "one");
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), -3.0);

    ctx->stra_exit_short(kCode, 99, "all");
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 0.0);
}

//////////////////////////////////////////////////////////////////////////
// 条件单：登记 -> tick 判定 -> 触发后清空
//////////////////////////////////////////////////////////////////////////
TEST_F(P01CtaOrderTest, limit_price_registers_condition_without_signal)
{
    ctx->stra_enter_long(kCode, 2, "cond", 3400.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 0.0);

    // 没有跌破限价，条件单保持待发
    feed(kCode, 3450.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 0.0);

    // 跌破限价 -> OL 触发，目标仓位 = 0 + 2
    feed(kCode, 3399.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 2.0);

    // 条件单在同一个 bar 内只会触发一条，触发后即被清空：再来一条不该再加仓
    feed(kCode, 3390.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 2.0);
    EXPECT_EQ(ctx->cond_fires, 0u);
}

TEST_F(P01CtaOrderTest, stop_price_registers_reverse_comparison)
{
    // 开多用 stopprice 是「向上突破」，开空用 stopprice 是「向下跌破」
    ctx->stra_enter_long(kCode, 1, "stop", 0.0, 3600.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 0.0);

    feed(kCode, 3599.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 0.0);

    feed(kCode, 3601.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 1.0);
}

TEST_F(P01CtaOrderTest, short_condition_orders_are_registered_symmetrically)
{
    // 开空的 limitprice 语义是「向上挂单成交」，即 WCT_LargerOrEqual
    ctx->stra_enter_short(kCode, 2, "os", 3300.0);
    feed(kCode, 3299.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 0.0);

    feed(kCode, 3350.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), -2.0);

    // 开空的 stopprice 语义相反，是向下跌破
    ctx->stra_enter_short(kCode, 1, "os2", 0.0, 3200.0);
    feed(kCode, 3250.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), -2.0);

    feed(kCode, 3199.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), -3.0);
}

TEST_F(P01CtaOrderTest, set_position_condition_is_skipped_when_target_equals_current)
{
    ctx->stra_set_position(kCode, 0, "same", 3400.0);
    // 目标仓位与当前仓位一致 -> 直接 return，不登记条件单
    feed(kCode, 3399.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 0.0);

    // 目标仓位与当前不一致 -> 登记 SP 条件单（买向 limit 用 SmallerOrEqual）
    ctx->stra_set_position(kCode, 4, "sp", 3400.0);
    feed(kCode, 3398.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 4.0);
}

TEST_F(P01CtaOrderTest, set_position_rejects_missing_commodity_without_side_effects)
{
    std::filesystem::create_directories(env.dir());
    const std::filesystem::path stateFile =
        std::filesystem::path(WtHelper::getStraDataDir()) / "p01o_stra.json";
    const char* marketCode = "SHFE.zz.2510";
    const char* conditionalCode = "DCE.yy.2405";
    ASSERT_EQ(nullptr, engine.get_commodity_info(marketCode));
    ASSERT_EQ(nullptr, engine.get_commodity_info(conditionalCode));
    ASSERT_FALSE(std::filesystem::exists(stateFile));

    ctx->stra_set_position(marketCode, 2.0, "missing-market");
    EXPECT_FALSE(engine.has_tick_subscription(marketCode));
    EXPECT_FALSE(ctx->has_pending_signal(marketCode));
    EXPECT_DOUBLE_EQ(0.0, ctx->stra_get_position(marketCode));
    EXPECT_FALSE(std::filesystem::exists(stateFile));

    ctx->stra_set_position(conditionalCode, 3.0, "missing-condition", 10.0);
    EXPECT_FALSE(engine.has_tick_subscription(conditionalCode));
    EXPECT_FALSE(ctx->has_pending_signal(conditionalCode));
    EXPECT_DOUBLE_EQ(0.0, ctx->stra_get_position(conditionalCode));
    EXPECT_FALSE(ctx->has_condition(conditionalCode));
    EXPECT_FALSE(std::filesystem::exists(stateFile));

    // Known commodities retain both supported order paths.
    ASSERT_NE(nullptr, engine.get_commodity_info(kCode));
    ctx->stra_set_position(kCode, 2.0, "valid-market");
    EXPECT_TRUE(engine.has_tick_subscription(kCode));
    EXPECT_TRUE(ctx->has_pending_signal(kCode));
    EXPECT_DOUBLE_EQ(2.0, ctx->pending_signal_target(kCode));

    const char* validConditionalCode = "SZSE.of.159915";
    ASSERT_NE(nullptr, engine.get_commodity_info(validConditionalCode));
    ctx->stra_set_position(validConditionalCode, 3.0, "valid-condition", 10.0);
    EXPECT_TRUE(engine.has_tick_subscription(validConditionalCode));
    EXPECT_FALSE(ctx->has_pending_signal(validConditionalCode));
    EXPECT_TRUE(ctx->has_condition(validConditionalCode));
}

TEST_F(P01CtaOrderTest, set_position_without_engine_has_no_side_effects)
{
    std::filesystem::create_directories(env.dir());
    const std::filesystem::path stateFile =
        std::filesystem::path(WtHelper::getStraDataDir()) / "p01_null_engine_stra.json";
    P01CtaCtx nullEngineCtx(nullptr, "p01_null_engine_stra", 0);
    ASSERT_FALSE(std::filesystem::exists(stateFile));

    nullEngineCtx.stra_set_position(kCode, 2.0, "no-engine");
    EXPECT_FALSE(nullEngineCtx.has_pending_signal(kCode));
    EXPECT_DOUBLE_EQ(0.0, nullEngineCtx.stra_get_position(kCode));
    EXPECT_FALSE(nullEngineCtx.has_condition(kCode));
    EXPECT_FALSE(std::filesystem::exists(stateFile));

    nullEngineCtx.stra_set_position(kCode, 2.0, "no-engine-condition", 10.0);
    EXPECT_FALSE(nullEngineCtx.has_pending_signal(kCode));
    EXPECT_FALSE(nullEngineCtx.has_condition(kCode));
    EXPECT_FALSE(std::filesystem::exists(stateFile));
}

TEST_F(P01CtaOrderTest, valid_session_tick_consumes_pending_signal)
{
    WTSSessionInfo* session = bd.getSession("TRADING");
    ASSERT_NE(nullptr, session);
    session->addTradingSection(900, 1600);
    ASSERT_TRUE(session->isInTradingTime(931, true));

    ctx->stra_enter_long(kCode, 2.0, "valid-session");
    ASSERT_TRUE(ctx->has_pending_signal(kCode));
    EXPECT_DOUBLE_EQ(0.0, ctx->accounted_position(kCode));

    engine.prime_price(kCode, 3500.0);
    feed(kCode, 3500.0);

    EXPECT_FALSE(ctx->has_pending_signal(kCode));
    EXPECT_DOUBLE_EQ(2.0, ctx->accounted_position(kCode));
}

TEST_F(P01CtaOrderTest, tick_can_be_emitted_to_strategy_after_condition_handling)
{
    ctx->stra_sub_ticks(kCode);
    feed(kCode, 3500.0, true);
    EXPECT_EQ(ctx->tick_updates, 1u);
    EXPECT_DOUBLE_EQ(ctx->stra_get_price(kCode), 3500.0);
}

//////////////////////////////////////////////////////////////////////////
// BUG-P01-001：真实基础数据加载器可接受 rules 自动创建且缺少 ALLDAY 模板的商品。
// 平仓边界不得崩溃或覆盖同代码既有持久化信号；tick 边界保留该 pending signal。
//////////////////////////////////////////////////////////////////////////
TEST_F(P01CtaOrderTest, missing_session_exit_and_tick_preserve_saved_signal)
{
    std::filesystem::create_directories(env.dir());
    const char* stdCode = "DCE.i2405";
    const std::string strategyName = "p01_missing_session_signal";
    const std::filesystem::path contractsFile =
        std::filesystem::path(env.dir()) / "missing_session_contracts.json";
    {
        std::ofstream contracts(contractsFile);
        ASSERT_TRUE(contracts.good());
        contracts << R"({
            "DCE": {
                "i2405": {
                    "name": "P01 auto-created commodity without session",
                    "exchg": "DCE",
                    "rules": {
                        "pricetick": 0.5,
                        "volscale": 100,
                        "covermode": 0,
                        "pricemode": 0,
                        "lotstick": 1,
                        "minlots": 1
                    },
                    "opendate": 20200101,
                    "expiredate": 20300101
                }
            }
        })";
    }

    WTSBaseDataMgr realBaseData;
    ASSERT_TRUE(realBaseData.loadContracts(contractsFile.string().c_str()));
    WTSCommodityInfo* commodity = realBaseData.getCommodity("DCE", "i2405");
    ASSERT_NE(nullptr, commodity);
    ASSERT_EQ(nullptr, commodity->getSessionInfo()); // ALLDAY was not loaded.
    ASSERT_NE(nullptr, realBaseData.getContract("i2405", "DCE", 20250110));

    P01CtaEngine realEngine;
    realEngine.attach(&realBaseData, &hot);
    P01CtaCtx realCtx(&realEngine, strategyName.c_str(), 0);
    realCtx.on_init();

    const std::filesystem::path stateFile =
        std::filesystem::path(WtHelper::getStraDataDir()) / (strategyName + ".json");
    auto readState = [&]() {
        std::ifstream state(stateFile, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(state),
            std::istreambuf_iterator<char>());
    };
    ASSERT_FALSE(std::filesystem::exists(stateFile));

    // A regular entry signal is written first; a missing-session exit attempt must not replace it.
    realCtx.stra_enter_long(stdCode, 2.0, "keep-me");
    ASSERT_TRUE(realCtx.has_pending_signal(stdCode));
    ASSERT_DOUBLE_EQ(2.0, realCtx.pending_signal_target(stdCode));
    ASSERT_EQ("keep-me", realCtx.pending_signal_tag(stdCode));
    const std::string savedSignal = readState();
    ASSERT_FALSE(savedSignal.empty());

    realCtx.stra_exit_long(stdCode, 1.0, "must-not-overwrite");
    EXPECT_TRUE(realCtx.has_pending_signal(stdCode));
    EXPECT_DOUBLE_EQ(2.0, realCtx.pending_signal_target(stdCode));
    EXPECT_EQ("keep-me", realCtx.pending_signal_tag(stdCode));
    EXPECT_EQ(savedSignal, readState());

    WTSTickData* tick = p01MakeTick("DCE", "i2405", 20250110, 20250110, 93100000, 10.0);
    ASSERT_NE(nullptr, tick);
    realCtx.on_tick(stdCode, tick, false);
    tick->release();

    // stra_get_position reports an untouched signal's target; inspect actual
    // accounting separately to prove the missing-session tick did not execute it.
    EXPECT_DOUBLE_EQ(0.0, realCtx.accounted_position(stdCode));
    EXPECT_TRUE(realCtx.has_pending_signal(stdCode));
    EXPECT_DOUBLE_EQ(2.0, realCtx.pending_signal_target(stdCode));
    EXPECT_EQ("keep-me", realCtx.pending_signal_tag(stdCode));
    EXPECT_EQ(savedSignal, readState());

    // The same pending signal remains recoverable after the strategy context restarts.
    P01CtaCtx restored(&realEngine, strategyName.c_str(), 0);
    restored.on_init();
    EXPECT_TRUE(restored.has_pending_signal(stdCode));
    EXPECT_DOUBLE_EQ(2.0, restored.pending_signal_target(stdCode));
    EXPECT_EQ("keep-me", restored.pending_signal_tag(stdCode));
}

} // namespace
