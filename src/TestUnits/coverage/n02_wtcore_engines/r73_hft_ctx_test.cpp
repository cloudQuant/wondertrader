/**
 * r73_hft_ctx_test.cpp — lane R73 / HftStraBaseCtx 账户与回调分支补测
 *
 * 覆盖 src/WtCore/HftStraBaseCtx.cpp 中 R42/R71 未覆盖的分支：
 *   - do_set_position 的 price==0 回退、同手数早退、部分平仓/全平/反手
 *     （含 maxprofit/maxloss 等比缩放路径）
 *   - update_dyn_profit 的多头 bid / 空头 ask / 零手分支
 *   - 非数据托管模式下的日志与资金结算早退
 *   - 坏用户数据容忍与 9 个回调的脏数据落盘
 *   - 历史切片在数据管理器返回 NULL 时的守卫
 */
#include "r73_fixture.h"

using namespace wtp;

namespace {

const char* const kCode = "SHFE.rb.2510";

std::string hft_ud_file(const char* name)
{
    return std::string(WtHelper::getStraUsrDatDir()) + "ud_" + name + ".json";
}

std::string hft_out_file(const char* name, const char* file)
{
    return std::string(WtHelper::getOutputDir()) + name + "/" + file;
}

class R73HftCtxTest : public ::testing::Test
{
protected:
    r73::TempDir _tmp{"hft"};
    r73::BaseDataMgr _bd;
    r73::HotMgr _hot;
    r73::DataMgr _dm;
    r73::HftEngine _engine;

    void SetUp() override
    {
        WtHelper::setGenerateDir((_tmp.path("generated") + "/").c_str());
        WtHelper::setTime(20250110, 1000, 0);

        _bd.regisSession("TRADING");
        WTSSessionInfo* sess = _bd.session("TRADING");
        ASSERT_NE(sess, nullptr);
        sess->addTradingSection(900, 1500);

        WTSCommodityInfo* rb = r73::makeComm("SHFE", "rb", "TRADING", TM_Both);
        rb->setSessionInfo(sess);
        _bd.regis(rb, r73::makeCt("SHFE", "rb", "rb2510"));

        _engine.attach(&_bd, &_hot, &_dm);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove(hft_ud_file("r73_hft_acct"), ec);
        std::filesystem::remove(hft_ud_file("r73_hft_data"), ec);
    }

    std::unique_ptr<r73::HftCtx> makeCtx(const char* name, bool agent = false, int32_t slippage = 0)
    {
        return std::unique_ptr<r73::HftCtx>(new r73::HftCtx(&_engine, name, agent, slippage));
    }

    void feed(r73::HftCtx& ctx, double price, double bid, double ask, uint32_t atime = 93100000)
    {
        WTSTickData* tick = r73::makeTick("SHFE", kCode, 20250110, 20250110, atime,
            price, bid, ask);
        ASSERT_NE(tick, nullptr);
        ctx.update_dyn_profit(kCode, tick);
        tick->release();
    }
};

// 覆盖 HftStraBaseCtx.cpp:785-904（do_set_position 的 price=0、同手数早退、
// 开仓/部分平仓/全平/反手以及 maxprofit/maxloss 缩放）
TEST_F(R73HftCtxTest, PositionLifecycleCoversCloseAndReverse)
{
    auto ctx = makeCtx("r73_hft_acct", false, 0);

    // price=0 -> 使用内部价格缓存（未喂价时为 0）；2 手多单
    ctx->do_set_position(kCode, 2.0, 0.0, "L1");
    EXPECT_DOUBLE_EQ(0.0, ctx->stra_get_position_avgpx(kCode));

    // 同方向加仓 2 手 @100，均价 (0*2 + 100*2)/4
    ctx->do_set_position(kCode, 4.0, 100.0, "L2");
    EXPECT_DOUBLE_EQ(50.0, ctx->stra_get_position_avgpx(kCode));

    // 手数相同 -> 早退，均价保持
    ctx->do_set_position(kCode, 4.0, 200.0, "L3");
    EXPECT_DOUBLE_EQ(50.0, ctx->stra_get_position_avgpx(kCode));

    // 部分平仓 3 手 @110：先平 L1 的 2 手，再平 L2 的 1 手，剩 1 手 @100
    ctx->do_set_position(kCode, 1.0, 110.0, "C1");
    EXPECT_DOUBLE_EQ(100.0, ctx->stra_get_position_avgpx(kCode));

    // 全平至 0
    ctx->do_set_position(kCode, 0.0, 90.0, "C2");
    EXPECT_DOUBLE_EQ(0.0, ctx->stra_get_position_avgpx(kCode));
    EXPECT_DOUBLE_EQ(0.0, ctx->stra_get_position_profit(kCode));

    // 0 -> -2 反手开空 @120
    ctx->do_set_position(kCode, -2.0, 120.0, "S1");
    EXPECT_DOUBLE_EQ(-120.0, ctx->stra_get_position_avgpx(kCode));
}

// 覆盖 HftStraBaseCtx.cpp:906-939（多头 bid / 空头 ask / 零手动态盈亏）、
// L946-966（数据托管下的 session_end 资金落盘）
TEST_F(R73HftCtxTest, DynProfitUsesBidAskAndWritesFunds)
{
    auto ctx = makeCtx("r73_hft_acct", true, 0);
    ctx->on_init();

    ctx->do_set_position(kCode, 2.0, 100.0, "L");
    feed(*ctx, 105.0, 110.0, 90.0);
    EXPECT_DOUBLE_EQ(200.0, ctx->stra_get_position_profit(kCode));

    // 反手到 -2：多头平仓后开空，空头估值用 ask
    ctx->do_set_position(kCode, -2.0, 100.0, "S");
    feed(*ctx, 105.0, 90.0, 110.0, 93101000);
    EXPECT_DOUBLE_EQ(200.0, ctx->stra_get_position_profit(kCode));

    // 清零后 dynprofit 归零
    ctx->do_set_position(kCode, 0.0, 100.0, "F");
    feed(*ctx, 100.0, 100.0, 100.0, 93102000);
    EXPECT_DOUBLE_EQ(0.0, ctx->stra_get_position_profit(kCode));

    ctx->on_session_end(20250110);
    EXPECT_NE(r73::read_file(hft_out_file("r73_hft_acct", "funds.csv")).find("20250110"),
        std::string::npos);
}

// 覆盖 HftStraBaseCtx.cpp:426-483（5 个历史切片入口的 NULL 守卫）、
// L687-694（价格缓存未命中时引擎兜底）
TEST_F(R73HftCtxTest, NullSlicesAreGuarded)
{
    auto ctx = makeCtx("r73_hft_data", false);
    _dm.null_kline = true;
    _dm.null_ticks = true;
    _dm.null_last_tick = true;
    _dm.null_l2 = true;

    EXPECT_TRUE(ctx->stra_get_bars(kCode, "m5", 3) == NULL);
    EXPECT_TRUE(ctx->stra_get_ticks(kCode, 3) == NULL);
    EXPECT_TRUE(ctx->stra_get_order_detail(kCode, 3) == NULL);
    EXPECT_TRUE(ctx->stra_get_order_queue(kCode, 3) == NULL);
    EXPECT_TRUE(ctx->stra_get_transaction(kCode, 3) == NULL);
    EXPECT_TRUE(ctx->stra_get_last_tick(kCode) == NULL);
    EXPECT_EQ(_dm.kline_calls, 1u);
    EXPECT_EQ(_dm.tick_calls, 1u);
    EXPECT_EQ(_dm.dtl_calls, 1u);
    EXPECT_EQ(_dm.que_calls, 1u);
    EXPECT_EQ(_dm.trans_calls, 1u);
    EXPECT_EQ(_dm.last_tick_calls, 1u);

    EXPECT_DOUBLE_EQ(0.0, ctx->stra_get_price(kCode));
}

// 覆盖 HftStraBaseCtx.cpp:136-186（9 个回调的脏数据落盘 + 非托管
// init_outputs 早退）、L536-567（非托管 on_trade 跳过信号日志）、
// L711-783（坏用户数据容忍）
TEST_F(R73HftCtxTest, DirtyUserDataAndNonAgentCallbacks)
{
    const std::string ud = hft_ud_file("r73_hft_data");
    ASSERT_TRUE(r73::write_file(ud, "{{{"));
    auto ctx = makeCtx("r73_hft_data", false);
    ctx->on_init();
    EXPECT_STREQ(ctx->stra_load_user_data("k", "def"), "def");

    ASSERT_TRUE(r73::write_file(ud, "{\"k\":\"v\"}"));
    ctx->on_init();
    EXPECT_STREQ(ctx->stra_load_user_data("k"), "v");

    auto dirty = [&ctx]() { ctx->stra_save_user_data("k", "v2"); };
    dirty();
    ctx->on_tick(NULL, NULL);
    dirty();
    ctx->on_order_queue(NULL, NULL);
    dirty();
    ctx->on_order_detail(NULL, NULL);
    dirty();
    ctx->on_transaction(NULL, NULL);
    dirty();
    ctx->on_bar(kCode, "m1", 1, NULL);
    dirty();
    ctx->on_order(1, kCode, true, 1, 0, 100.0, true);
    dirty();
    ctx->on_channel_ready();
    dirty();
    ctx->on_channel_lost();
    dirty();
    ctx->on_entrust(1, kCode, true, "ok");
    EXPECT_STREQ(ctx->stra_load_user_data("k"), "v2");

    // 非数据托管：不创建 logs，on_trade 的内部持仓仍更新，但不写 signals.csv
    ctx->on_trade(0, kCode, true, 1.0, 100.0);
    EXPECT_DOUBLE_EQ(100.0, ctx->stra_get_position_avgpx(kCode));
    EXPECT_FALSE(std::filesystem::exists(hft_out_file("r73_hft_data", "signals.csv")));

    // 非托管 session_end 不写 funds.csv
    ctx->on_session_end(20250110);
    EXPECT_FALSE(std::filesystem::exists(hft_out_file("r73_hft_data", "funds.csv")));
}

/*
 * DISABLED：stra_sell 在合约不存在时先解引用 commInfo。
 * getCommodity("DCE","rr") 返回 NULL 后直接调用 canShort()，公开 API 必崩。
 * 见 BUG-R73-002。
 */
TEST_F(R73HftCtxTest, DISABLED_SellUnknownProductDereferencesNullCommodity)
{
    auto ctx = makeCtx("r73_hft_bad", false);
    EXPECT_TRUE(ctx->stra_sell("DCE.rr.2509", 100.0, 1.0, "x").empty());
}

}  // namespace
