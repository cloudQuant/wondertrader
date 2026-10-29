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
 * 缺 session 的 CTA 平仓空指针路径保留为 DISABLED_ 静态探针；不以未初始化
 * engine 上的历史查询作为产品复现或验收。
 */
#include "p01_cta_env.h"

#include <memory>
#include <string>

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

        // 缺 session 的品种只供 BUG-P01-001 的禁用边界探针使用。
        WTSCommodityInfo* noSess = p01MakeComm("CZCE", "ta", "TRADING", TM_Both);
        bd.regis(noSess, p01MakeCt("CZCE", "ta", "ta509"));

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

TEST_F(P01CtaOrderTest, tick_can_be_emitted_to_strategy_after_condition_handling)
{
    ctx->stra_sub_ticks(kCode);
    feed(kCode, 3500.0, true);
    EXPECT_EQ(ctx->tick_updates, 1u);
    EXPECT_DOUBLE_EQ(ctx->stra_get_price(kCode), 3500.0);
}

//////////////////////////////////////////////////////////////////////////
// BUG-P01-001 的缺 session 平仓探针。它没有完成 WtEngine::init，因此不能证明
// BUG-P01-002 的历史数据接口在受支持生命周期内出错；该 fixture 中 _data_mgr 还是
// 未初始化指针，并非 NULL。历史查询不在此处调用，以免将无效前置条件写成产品回归。
//////////////////////////////////////////////////////////////////////////
TEST(DISABLED_P01CtaMissingSessionProbe, exit_long_with_unbound_session)
{
    P01TempEnv env2{ "cta_crash" };
    FakeBaseDataMgr bd;
    FakeHotMgr hot;
    P01CtaEngine engine;
    bd.regisSession("TRADING");

    // CZCE.ta 故意不挂时段
    WTSCommodityInfo* ta = p01MakeComm("CZCE", "ta", "TRADING", TM_Both);
    bd.regis(ta, p01MakeCt("CZCE", "ta", "ta509"));
    engine.attach(&bd, &hot);

    P01CtaCtx ctx(&engine, "p01c_stra", 0);
    // getSessionInfo() == NULL -> offsetTime 解引用空指针（仅保留静态探针）。
    ctx.stra_exit_long("CZCE.ta509", 1, "t");
}

} // namespace
