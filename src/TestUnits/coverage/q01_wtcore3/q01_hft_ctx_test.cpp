/**
 * q01_hft_ctx_test.cpp —— HftStraBaseCtx 可确定行为（裸 WtHftEngine，_trader=NULL）
 *
 * HftStraBaseCtx 自身已实现 IHftStraCtx 全部纯虚，直接实例化。
 * 只测不触碰 _trader 的历史无关分支：
 *  - id/name/setTrader/stra_get_date/time/secs 透传引擎
 *  - on_init：_data_agent=true 建 4 个 csv / =false 早退
 *  - on_tick/on_order_queue/on_order_detail/on_transaction/on_bar：仅处理 _ud_modified -> save_userdata
 *  - get_inner_code：未映射原样返回
 *  - stra_log_*；stra_save/load_user_data；userdata 落盘 + 第二个 ctx load_userdata 读回
 *  - stra_buy：合约查不到 -> log_error + 返回空 OrderIDs（不 deref _trader）
 */
#include "q01_sel_env.h"

#include <fstream>

#include <Includes/IHftStraCtx.h>
#include <Includes/WTSTradeDef.hpp>
#include <WtCore/HftStraBaseCtx.h>
#include <WtCore/WtHftEngine.h>

using namespace q01sel;

namespace {

/** 裸高频引擎：仅接上基础数据/主力规则，不 init/run/起线程 */
struct HftEngine : public WtHftEngine
{
    HftEngine() {}
    void attach(IBaseDataMgr* bd, IHotMgr* hot)
    {
        _base_data_mgr = bd;
        _hot_mgr = hot;
        set_date_time(20250110, 931, 55);
        set_trading_date(20250110);
    }
};

/** 仅把 protected 的 get_inner_code 提出来做白盒断言（替身派生，不用 private 宏） */
struct HftCtx : public HftStraBaseCtx
{
    HftCtx(WtHftEngine* e, const char* n, bool agent, int32_t slip)
        : HftStraBaseCtx(e, n, agent, slip) {}
    using HftStraBaseCtx::get_inner_code;
};

} // namespace

class Q01HftCtxTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        _env.reset(new Q01TempEnv("hft"));
        _bdm.regisSession("TRADING");
        _comm = makeComm("SHFE", "ag", "TRADING", TM_Both, CC_Future);
        _ct = makeCt("SHFE", "ag", "ag2512");
        _bdm.regis(_comm, _ct);
        _eng.attach(&_bdm, &_hot);
    }

    void TearDown() override { _env.reset(); }

    std::string usrDir()
    {
        std::filesystem::path p(WtHelper::getStraUsrDatDir());
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        return p.string();
    }

    std::unique_ptr<Q01TempEnv> _env;
    BaseDataMgr _bdm;
    HotMgr _hot;
    HftEngine _eng;
    WTSCommodityInfo* _comm = NULL;
    WTSContractInfo* _ct = NULL;
};

TEST_F(Q01HftCtxTest, id_name_and_time_passthrough)
{
    HftStraBaseCtx ctx(&_eng, "h_id", false, 0);
    EXPECT_EQ(ctx.id(), ctx.id());
    EXPECT_GE(ctx.id(), 6000u);              // makeHftCtxId 起点
    EXPECT_STREQ(ctx.name(), "h_id");
    EXPECT_EQ(ctx.stra_get_date(), 20250110u);
    EXPECT_EQ(ctx.stra_get_time(), 0u);      // get_raw_time（attach 未设 rawTime -> 0）
    EXPECT_EQ(ctx.stra_get_secs(), 55u);

    // setTrader 只赋值，不触发任何调用
    ctx.setTrader(NULL);
    SUCCEED();
}

TEST_F(Q01HftCtxTest, init_with_data_agent_creates_files)
{
    {
        HftStraBaseCtx ctx(&_eng, "h_agent", true, 0);
        ctx.on_init();   // init_outputs（agent）+ load_userdata（缺失早退）
    }
    std::filesystem::path out(WtHelper::getOutputDir());
    out /= "h_agent";
    for (const char* f : { "trades.csv", "closes.csv", "funds.csv", "signals.csv" })
        EXPECT_TRUE(std::filesystem::exists(out / f)) << f;
}

TEST_F(Q01HftCtxTest, init_without_data_agent_creates_nothing)
{
    HftStraBaseCtx ctx(&_eng, "h_noagent", false, 0);
    ctx.on_init();   // _data_agent=false -> init_outputs 直接 return
    std::filesystem::path out(WtHelper::getOutputDir());
    out /= "h_noagent";
    EXPECT_FALSE(std::filesystem::exists(out / "trades.csv"));
}

TEST_F(Q01HftCtxTest, data_callbacks_only_flush_userdata)
{
    HftStraBaseCtx ctx(&_eng, "h_cb", false, 0);
    ctx.on_init();
    // 未修改 -> 各回调应安全空转（传 NULL 数据指针亦无解引用）
    ctx.on_tick("SHFE.ag.2512", NULL);
    ctx.on_order_queue("SHFE.ag.2512", NULL);
    ctx.on_order_detail("SHFE.ag.2512", NULL);
    ctx.on_transaction("SHFE.ag.2512", NULL);
    ctx.on_bar("SHFE.ag.2512", "m", 1, NULL);

    // 修改后 -> on_tick 触发 save_userdata 落盘
    ctx.stra_save_user_data("hk", "hv");
    ctx.on_tick("SHFE.ag.2512", NULL);
    EXPECT_TRUE(std::filesystem::exists(usrDir() + "ud_h_cb.json"));
}

TEST_F(Q01HftCtxTest, get_inner_code_unmapped_returns_input)
{
    HftCtx ctx(&_eng, "h_code", false, 0);
    // 未经 stra_buy 规则映射，_code_map 为空 -> 原样返回
    EXPECT_STREQ(ctx.get_inner_code("SHFE.ag.2512"), "SHFE.ag.2512");
}

TEST_F(Q01HftCtxTest, stra_buy_unknown_contract_returns_empty)
{
    HftStraBaseCtx ctx(&_eng, "h_buy", false, 0);
    ctx.on_init();
    // 未注册合约 -> else 分支 getContract==NULL -> log_error 返回空 OrderIDs，不 deref _trader
    OrderIDs ids = ctx.stra_buy("DCE.rr.2509", 100.0, 1.0, "t", 0, false);
    EXPECT_EQ(ids.size(), 0u);
}

TEST_F(Q01HftCtxTest, user_data_roundtrip_across_ctx)
{
    {
        HftStraBaseCtx ctx(&_eng, "h_ud", false, 0);
        ctx.on_init();
        ctx.stra_save_user_data("a", "1");
        ctx.stra_save_user_data("b", "two");
        ctx.on_tick("SHFE.ag.2512", NULL);   // 触发 save_userdata
    }
    HftStraBaseCtx ctx2(&_eng, "h_ud", false, 0);
    ctx2.on_init();   // load_userdata 读回
    EXPECT_STREQ(ctx2.stra_load_user_data("a", ""), "1");
    EXPECT_STREQ(ctx2.stra_load_user_data("b", ""), "two");
    EXPECT_STREQ(ctx2.stra_load_user_data("z", "dflt"), "dflt");
}

TEST_F(Q01HftCtxTest, log_paths_do_not_throw)
{
    HftStraBaseCtx ctx(&_eng, "h_log", false, 0);
    ctx.stra_log_info("i");
    ctx.stra_log_debug("d");
    ctx.stra_log_warn("w");
    ctx.stra_log_error("e");
    SUCCEED();
}
