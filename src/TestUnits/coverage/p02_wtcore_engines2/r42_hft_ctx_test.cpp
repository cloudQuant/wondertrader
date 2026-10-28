/**
 * r42_hft_ctx_test.cpp  (lane R42 / WtCore 策略上下文补测)
 *
 * 覆盖目标 src/WtCore/HftStraBaseCtx.cpp：
 *   init_outputs（数据托管开关）/ on_init / save_userdata / load_userdata
 *   on_tick / on_order_queue / on_order_detail / on_transaction / on_bar /
 *   on_order / on_channel_ready / on_channel_lost / on_entrust / on_session_end
 *   stra_sub_* 订阅族 / stra_log_* 日志门面 / 元信息与空仓查询
 *
 * HftStraBaseCtx 本身是可实例化类；涉及 _trader 的下单/持仓接口不在本用例范围内，
 * 因此引擎只需接上基础数据与主力替身，不加载任何交易插件。
 */
#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <string>

#include "r42_core_fakes.h"

#include <WtCore/HftStraBaseCtx.h>

using namespace wtp;

namespace {

static const char* kHftCode = "SHFE.rb.2510";

std::string r42_hft_read(const std::string& path)
{
    std::ifstream ifs(path.c_str());
    if (!ifs.is_open())
        return std::string();
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

class R42HftCtxTest : public ::testing::Test
{
protected:
    r42fakes::R42GenDir      gen;
    r42fakes::R42BaseDataMgr bd;
    r42fakes::R42HotMgr      hot;
    r42fakes::R42HftEngine   engine;

    void SetUp() override
    {
        bd.regisSession("TRADING");
        WTSSessionInfo* sess = bd.session("TRADING");
        ASSERT_NE(sess, nullptr);
        sess->addTradingSection(900, 1500);

        WTSCommodityInfo* rb = r42fakes::makeComm("SHFE", "rb", "TRADING", TM_Both);
        rb->setSessionInfo(sess);
        bd.regis(rb, r42fakes::makeCt("SHFE", "rb", "2510"));

        engine.attach(&bd, &hot);
    }

    std::string out_file(const char* name, const char* file) const
    {
        return std::string(WtHelper::getOutputDir()) + name + "/" + file;
    }

    std::string ud_file(const char* name) const
    {
        return std::string(WtHelper::getStraUsrDatDir()) + "ud_" + name + ".json";
    }
};

// 覆盖 HftStraBaseCtx.cpp L65-141 init_outputs/on_init（数据托管 = true）
TEST_F(R42HftCtxTest, DataAgentInitCreatesOutputFilesIdempotently)
{
    HftStraBaseCtx ctx(&engine, "r42_hft_agent", true, 0);
    ctx.on_init();

    EXPECT_FALSE(r42_hft_read(out_file("r42_hft_agent", "trades.csv")).empty());
    EXPECT_FALSE(r42_hft_read(out_file("r42_hft_agent", "closes.csv")).empty());
    EXPECT_FALSE(r42_hft_read(out_file("r42_hft_agent", "funds.csv")).empty());
    EXPECT_FALSE(r42_hft_read(out_file("r42_hft_agent", "signals.csv")).empty());

    // 第二次初始化走 seek_to_end 分支，仍可正常写日志
    ctx.on_init();
    ctx.on_session_end(20250110);
    const std::string funds = r42_hft_read(out_file("r42_hft_agent", "funds.csv"));
    EXPECT_NE(funds.find("20250110"), std::string::npos);
}

// 覆盖 L65-68 init_outputs 的非托管早退（不应创建策略输出目录）
TEST_F(R42HftCtxTest, NonDataAgentInitSkipsOutputs)
{
    const std::string dir = std::string(WtHelper::getOutputDir()) + "r42_hft_noagent";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    HftStraBaseCtx ctx(&engine, "r42_hft_noagent", false, 0);
    ctx.on_init();

    EXPECT_FALSE(std::filesystem::exists(dir));
}

// 覆盖 L726-783 save_userdata/load_userdata 以及 L536-599 各回调的脏数据落盘
TEST_F(R42HftCtxTest, UserDataFlushedByCallbacksAndReloaded)
{
    HftStraBaseCtx ctx(&engine, "r42_hft_ud", true, 0);
    ctx.on_init();

    ctx.stra_save_user_data("k1", "v1");
    ctx.stra_save_user_data("k2", "v2");
    // on_tick 是第一条落盘路径
    ctx.on_tick(kHftCode, NULL);

    const std::string raw = r42_hft_read(ud_file("r42_hft_ud"));
    EXPECT_NE(raw.find("\"k1\""), std::string::npos);

    // 其余回调在再次置脏后各自触发一次 save_userdata
    ctx.stra_save_user_data("k3", "v3");
    ctx.on_order_queue(kHftCode, NULL);
    ctx.stra_save_user_data("k4", "v4");
    ctx.on_order_detail(kHftCode, NULL);
    ctx.stra_save_user_data("k5", "v5");
    ctx.on_transaction(kHftCode, NULL);
    ctx.stra_save_user_data("k6", "v6");
    ctx.on_bar(kHftCode, "m1", 1, NULL);
    ctx.stra_save_user_data("k7", "v7");
    ctx.on_order(1, kHftCode, true, 1, 0, 3500, true);
    ctx.stra_save_user_data("k8", "v8");
    ctx.on_channel_ready();
    ctx.stra_save_user_data("k9", "v9");
    ctx.on_channel_lost();
    ctx.stra_save_user_data("k10", "v10");
    ctx.on_entrust(1, kHftCode, true, "ok");

    HftStraBaseCtx reloaded(&engine, "r42_hft_ud", true, 0);
    reloaded.on_init();
    EXPECT_STREQ(reloaded.stra_load_user_data("k10"), "v10");
    EXPECT_STREQ(reloaded.stra_load_user_data("missing", "def"), "def");
}

// 覆盖 L485-534 订阅/日志门面、L601-709 元信息与空仓查询、L946-967 on_session_end
TEST_F(R42HftCtxTest, LogsSubscriptionsMetadataAndSessionEnd)
{
    HftStraBaseCtx ctx(&engine, "r42_hft_meta", true, 0);
    ctx.on_init();

    ctx.stra_log_info("r42 hft info");
    ctx.stra_log_debug("r42 hft debug");
    ctx.stra_log_warn("r42 hft warn");
    ctx.stra_log_error("r42 hft error");

    ctx.stra_sub_ticks(kHftCode);
    ctx.stra_sub_ticks(kHftCode);
    ctx.stra_sub_order_details(kHftCode);
    ctx.stra_sub_order_queues(kHftCode);
    ctx.stra_sub_transactions(kHftCode);

    EXPECT_EQ(ctx.stra_get_date(), 20250110u);
    EXPECT_EQ(ctx.stra_get_time(), 931u);
    EXPECT_EQ(ctx.stra_get_secs(), 0u);
    EXPECT_EQ(ctx.stra_get_rawcode(kHftCode), std::string());
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_profit(kHftCode), 0.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_avgpx(kHftCode), 0.0);
    EXPECT_EQ(ctx.id() > 0, true);

    WTSCommodityInfo* comm = ctx.stra_get_comminfo(kHftCode);
    ASSERT_NE(comm, nullptr);
    EXPECT_STREQ(comm->getProduct(), "rb");
    EXPECT_TRUE(ctx.stra_get_comminfo("SHFE.none.1") == NULL);

    // 空仓位下结算：只写资金日志，不产生持仓记录
    ctx.on_session_end(20250110);
    EXPECT_FALSE(r42_hft_read(out_file("r42_hft_meta", "funds.csv")).empty());
}

} // namespace
