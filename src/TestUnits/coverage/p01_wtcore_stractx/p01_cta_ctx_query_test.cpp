/**
 * p01_cta_ctx_query_test.cpp
 * 覆盖 src/WtCore/CtaStraBaseCtx.cpp 的「只读/查询/订阅/日志」接口族：
 *   - id()/stra_get_tdate()/stra_get_date()/stra_get_time() 对引擎的透传
 *   - stra_get_comminfo()/stra_get_rawcode() 的命中与未命中
 *   - 空持仓簿时全部 stra_get_*_entertime/cost/profit/avgpx/position 的零值返回
 *   - stra_get_fund_data 的 4 个 flag 分支 + default 分支
 *   - on_tick 的价格装配（_price_map）与 bEmitStrategy 开关
 *   - on_bar 的 KlineTag 通知/不通知、newBar==NULL 早退
 *   - stra_sub_ticks()/stra_sub_bar_events() 重复调用
 *   - stra_save_user_data/stra_load_user_data（含缺省值）与 4 个日志级别
 * 需要 WtDtMgr 历史切片的 stra_get_bars/stra_get_ticks/stra_get_last_tick 见 manifest 跳过清单。
 */
#include "p01_cta_env.h"

#include <memory>
#include <string>

namespace {

class P01CtaQueryTest : public ::testing::Test
{
protected:
    P01TempEnv          env{ "cta_query" };
    FakeBaseDataMgr     bd;
    FakeHotMgr          hot;
    P01CtaEngine        engine;
    WTSCommodityInfo*   rbComm = NULL;
    WTSContractInfo*    rbCt = NULL;
    std::unique_ptr<P01CtaCtx> ctx;

    void SetUp() override
    {
        bd.regisSession("TRADING");
        rbComm = p01MakeComm("SHFE", "rb", "TRADING", TM_Both);
        rbCt = p01MakeCt("SHFE", "rb", "rb2510");
        bd.regis(rbComm, rbCt);

        // 不可做空（T+1，模拟现货/基金类品种）
        WTSCommodityInfo* ofComm = p01MakeComm("SZSE", "of", "TRADING", TM_LongT1);
        WTSContractInfo* ofCt = p01MakeCt("SZSE", "of", "of159915");
        bd.regis(ofComm, ofCt);

        engine.attach(&bd, &hot);
        ctx.reset(new P01CtaCtx(&engine, "p01q_stra", 0));
    }
};

TEST_F(P01CtaQueryTest, identity_and_clock_passthrough)
{
    EXPECT_GT(ctx->id(), 0u);
    EXPECT_STREQ(ctx->name(), "p01q_stra");

    EXPECT_EQ(ctx->stra_get_tdate(), 20250110u);
    EXPECT_EQ(ctx->stra_get_date(), 20250110u);
    EXPECT_EQ(ctx->stra_get_time(), 931u);

    engine.setNow(20250113, 1459, 20250114);
    EXPECT_EQ(ctx->stra_get_tdate(), 20250114u);
    EXPECT_EQ(ctx->stra_get_date(), 20250113u);
    EXPECT_EQ(ctx->stra_get_time(), 1459u);
}

TEST_F(P01CtaQueryTest, comminfo_and_rawcode_lookup)
{
    EXPECT_EQ(ctx->stra_get_comminfo("SHFE.rb.2510"), rbComm);
    EXPECT_EQ(ctx->stra_get_comminfo("SHFE.rb9999"), nullptr);
    EXPECT_EQ(ctx->stra_get_comminfo("GEX.nothing"), nullptr);

    // 替身里没有主力规则（getRuleTag 返回 ""），所以标准化代码一律是空串
    EXPECT_TRUE(ctx->stra_get_rawcode("SHFE.rb.2510").empty());
    EXPECT_TRUE(ctx->stra_get_rawcode("NOTASTD").empty());
}

TEST_F(P01CtaQueryTest, empty_book_lookups_return_neutral_values)
{
    const char* code = "SHFE.rb.2510";
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(code), 0.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(code, true), 0.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(code, false, "tag"), 0.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position_avgpx(code), 0.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position_profit(code), 0.0);
    EXPECT_EQ(ctx->stra_get_first_entertime(code), 0u);
    EXPECT_EQ(ctx->stra_get_last_entertime(code), 0u);
    EXPECT_EQ(ctx->stra_get_last_exittime(code), 0u);
    EXPECT_DOUBLE_EQ(ctx->stra_get_last_enterprice(code), 0.0);
    EXPECT_STREQ(ctx->stra_get_last_entertag(code), "");
    // userTag 与 "default" 都是未登记的持仓标签
    EXPECT_EQ(ctx->stra_get_detail_entertime(code, "entA"), 0u);
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_cost(code, "entA"), 0.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit(code, "entA", 0), 0.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit(code, "entA", 1), 0.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit(code, "entA", -1), 0.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit(code, "entA", 2), 0.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit(code, "entA", -2), 0.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit(code, "entA", 99), 0.0);

    // 未登记合约也一样只能得到中性值
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("DCE.i2509"), 0.0);
    EXPECT_STREQ(ctx->stra_get_last_entertag("DCE.i2509"), "");
}

// stra_get_fund_data 的 flag switch：0 恒等于 1 - 3 + 2，default 返回 0
TEST_F(P01CtaQueryTest, fund_data_flags_are_consistent)
{
    EXPECT_DOUBLE_EQ(ctx->stra_get_fund_data(0),
        ctx->stra_get_fund_data(1) - ctx->stra_get_fund_data(3) + ctx->stra_get_fund_data(2));
    EXPECT_DOUBLE_EQ(ctx->stra_get_fund_data(9), 0.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_fund_data(-1), 0.0);
}

TEST_F(P01CtaQueryTest, on_tick_fills_price_map_and_respects_emit_flag)
{
    WTSTickData* t1 = p01MakeTick("SHFE", "rb2510", 20250110, 20250110, 93100000, 3501.0);
    ASSERT_NE(t1, nullptr);
    ctx->on_tick("SHFE.rb.2510", t1, false);
    EXPECT_DOUBLE_EQ(ctx->stra_get_price("SHFE.rb.2510"), 3501.0);
    EXPECT_EQ(ctx->tick_updates, 0u);

    WTSTickData* t2 = p01MakeTick("SHFE", "rb2510", 20250110, 20250110, 93200000, 3502.5);
    ASSERT_NE(t2, nullptr);
    ctx->on_tick("SHFE.rb.2510", t2, true);
    EXPECT_DOUBLE_EQ(ctx->stra_get_price("SHFE.rb.2510"), 3502.5);
    EXPECT_EQ(ctx->tick_updates, 1u);
    EXPECT_EQ(ctx->_last_upd_code, "SHFE.rb.2510");

    // 没有任何 tick 到达过的合约：ctx 本地表没有，回落去问引擎
    EXPECT_DOUBLE_EQ(ctx->stra_get_price("DCE.i2509"), 0.0);

    t1->release();
    t2->release();
}

TEST_F(P01CtaQueryTest, on_bar_notifies_only_after_subscribing_bar_events)
{
    WTSBarStruct bar;
    memset(&bar, 0, sizeof(bar));
    bar.date = 20250110;
    bar.close = 3500.0;

    // 未订阅 -> 只登记 tag._closed，不回调策略
    ctx->on_bar("SHFE.rb.2510", "m1", 1, &bar);
    EXPECT_EQ(ctx->bar_closes, 0u);

    ctx->stra_sub_bar_events("SHFE.rb.2510", "m1");
    ctx->on_bar("SHFE.rb.2510", "m1", 1, &bar);
    EXPECT_EQ(ctx->bar_closes, 1u);
    EXPECT_EQ(ctx->_last_close_code, "SHFE.rb.2510");
    EXPECT_EQ(ctx->_last_close_period, "m11");
    EXPECT_EQ(ctx->_last_close_bar, &bar);

    // 重复订阅不会让通知变成两次以上的一一对应关系（每次 close 只回调一次）
    ctx->stra_sub_bar_events("SHFE.rb.2510", "m1");
    ctx->on_bar("SHFE.rb.2510", "m1", 1, &bar);
    EXPECT_EQ(ctx->bar_closes, 2u);

    // 其它周期没有订阅
    ctx->on_bar("SHFE.rb.2510", "m5", 1, &bar);
    EXPECT_EQ(ctx->bar_closes, 2u);

    // newBar == NULL 早退
    ctx->on_bar("SHFE.rb.2510", "m1", 1, NULL);
    EXPECT_EQ(ctx->bar_closes, 2u);
}

TEST_F(P01CtaQueryTest, sub_ticks_is_idempotent)
{
    // 首次 on_init：init_outputs 建目录 + load_data/load_userdata 走「文件不存在」早退分支
    ctx->on_init();
    ctx->stra_sub_ticks("SHFE.rb.2510");
    ctx->stra_sub_ticks("SHFE.rb.2510");
    ctx->stra_sub_ticks("SZSE.of159915");
    EXPECT_EQ(ctx->tick_updates, 0u);
}

TEST_F(P01CtaQueryTest, user_data_store_and_default_value)
{
    EXPECT_STREQ(ctx->stra_load_user_data("nokey", "dflt"), "dflt");
    EXPECT_STREQ(ctx->stra_load_user_data("nokey"), "");

    ctx->stra_save_user_data("k1", "v1");
    EXPECT_STREQ(ctx->stra_load_user_data("k1", "dflt"), "v1");

    ctx->stra_save_user_data("k1", "v2");
    EXPECT_STREQ(ctx->stra_load_user_data("k1"), "v2");
}

// 四个级别都只是转发到 WTSLogger，不应抛出
TEST_F(P01CtaQueryTest, log_levels_are_forwarded)
{
    ctx->stra_log_info("info message");
    ctx->stra_log_debug("debug message");
    ctx->stra_log_warn("warn message");
    ctx->stra_log_error("error message");
    SUCCEED();
}

} // namespace
