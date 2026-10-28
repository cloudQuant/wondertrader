/**
 * q01_sel_query_test.cpp —— SelStraBaseCtx 查询透传 / 参数存取 / 日志（裸引擎，不触发下单）
 *
 * 覆盖：id、stra_get_tdate/date/time（非调度态）、stra_get_price（本地命中）、
 *       stra_get_comminfo、stra_get_rawcode、stra_get_sessinfo、stra_get_fund_data(0~3+default)、
 *       stra_log_info/debug/warn/error、stra_save_user_data/stra_load_user_data(命中+缺省)。
 * 引擎仅接上 BaseDataMgr/HotMgr，_data_mgr 保持 NULL；本文件不触碰任何历史切片接口。
 */
#include "q01_sel_env.h"

using namespace q01sel;

class Q01SelQueryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        _env.reset(new Q01TempEnv("query"));
        _bdm.regisSession("TRADING");
        // 期货、可空（TM_Both）
        _comm = makeComm("SHFE", "ag", "TRADING", TM_Both, CC_Future);
        _ct = makeCt("SHFE", "ag", "ag2512");
        _bdm.regis(_comm, _ct);
        // 股票、不可空（TM_Long）
        _stock = makeComm("SSE", "600000", "TRADING", TM_Long, CC_Stock);
        _sct = makeCt("SSE", "600000", "600000");
        _bdm.regis(_stock, _sct);
    }

    void TearDown() override
    {
        _env.reset();
    }

    SelCtx newCtx(const char* name)
    {
        _eng.attach(&_bdm, &_hot);
        return SelCtx(&_eng, name, 0);
    }

    std::unique_ptr<Q01TempEnv> _env;
    BaseDataMgr _bdm;
    HotMgr _hot;
    SelEngine _eng;
    WTSCommodityInfo* _comm = NULL;
    WTSContractInfo* _ct = NULL;
    WTSCommodityInfo* _stock = NULL;
    WTSContractInfo* _sct = NULL;
};

TEST_F(Q01SelQueryTest, id_and_date_time_passthrough)
{
    _eng.attach(&_bdm, &_hot);
    _eng.setNow(20250110, 931, 20250110);
    SelCtx ctx(&_eng, "q_id", 0);

    EXPECT_EQ(ctx.id(), ctx.id());          // 稳定
    EXPECT_GE(ctx.id(), 3000u);             // makeSelCtxId 起点
    // 非调度态：透传引擎日期/时间
    EXPECT_EQ(ctx.stra_get_tdate(), 20250110u);
    EXPECT_EQ(ctx.stra_get_date(), 20250110u);
    EXPECT_EQ(ctx.stra_get_time(), 931u);
}

TEST_F(Q01SelQueryTest, get_price_hits_local_map_after_tick)
{
    _eng.attach(&_bdm, &_hot);
    SelCtx ctx(&_eng, "q_px", 0);

    // 未喂数据前本地表空 -> 回落引擎（本用例不触发，避免 _data_mgr NULL 崩溃），
    // 先喂一笔 tick，使 _price_map 落地
    WTSTickData* tk = makeTick("SHFE", "ag2512", 20250110, 20250110, 935, 5210.0);
    ASSERT_NE(tk, nullptr);
    ctx.on_tick("SHFE.ag.2512", tk, false);   // bEmitStrategy=false，不增加 on_tick_updated
    EXPECT_EQ(ctx.tick_updates, 0u);
    EXPECT_DOUBLE_EQ(ctx.stra_get_price("SHFE.ag.2512"), 5210.0);
    tk->release();
}

TEST_F(Q01SelQueryTest, comminfo_rawcode_sessinfo)
{
    _eng.attach(&_bdm, &_hot);
    SelCtx ctx(&_eng, "q_info", 0);

    EXPECT_EQ(ctx.stra_get_comminfo("SHFE.ag.2512"), _comm);
    EXPECT_EQ(ctx.stra_get_comminfo("DCE.nothing.9999"), nullptr);

    // 未挂主力规则 -> get_rawcode 返回空串
    EXPECT_EQ(ctx.stra_get_rawcode("SHFE.ag.2512"), std::string(""));

    // 时段：品种已注册且会话存在 -> 非空
    WTSSessionInfo* s = ctx.stra_get_sessinfo("SHFE.ag.2512");
    EXPECT_NE(s, nullptr);
    // 未注册品种 -> get_session_info 先取 commodity，为 NULL 直接返回 NULL（安全）
    EXPECT_EQ(ctx.stra_get_sessinfo("CFFIC.nothing.9999"), nullptr);
}

TEST_F(Q01SelQueryTest, fund_data_flags_on_empty_book)
{
    _eng.attach(&_bdm, &_hot);
    SelCtx ctx(&_eng, "q_fund", 0);

    // 空账簿：全部为 0
    EXPECT_DOUBLE_EQ(ctx.stra_get_fund_data(0), 0.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_fund_data(1), 0.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_fund_data(2), 0.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_fund_data(3), 0.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_fund_data(99), 0.0);
}

TEST_F(Q01SelQueryTest, user_data_roundtrip_in_memory)
{
    _eng.attach(&_bdm, &_hot);
    SelCtx ctx(&_eng, "q_ud", 0);

    // 缺省值
    EXPECT_STREQ(ctx.stra_load_user_data("missing", "fallback"), "fallback");
    EXPECT_STREQ(ctx.stra_load_user_data("missing"), "");

    ctx.stra_save_user_data("alpha", "1.5");
    ctx.stra_save_user_data("beta", "hello");
    EXPECT_STREQ(ctx.stra_load_user_data("alpha", "x"), "1.5");
    EXPECT_STREQ(ctx.stra_load_user_data("beta", "x"), "hello");

    // 覆盖写
    ctx.stra_save_user_data("alpha", "2.0");
    EXPECT_STREQ(ctx.stra_load_user_data("alpha", "x"), "2.0");
}

TEST_F(Q01SelQueryTest, log_paths_do_not_throw)
{
    _eng.attach(&_bdm, &_hot);
    SelCtx ctx(&_eng, "q_log", 0);
    // 四个日志级别都应安全返回
    ctx.stra_log_info("info line");
    ctx.stra_log_debug("debug line");
    ctx.stra_log_warn("warn line");
    ctx.stra_log_error("error line");
    SUCCEED();
}

TEST_F(Q01SelQueryTest, position_getters_zero_on_empty_book)
{
    _eng.attach(&_bdm, &_hot);
    SelCtx ctx(&_eng, "q_zero", 0);

    EXPECT_DOUBLE_EQ(ctx.stra_get_position("SHFE.ag.2512"), 0.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position("SHFE.ag.2512", true), 0.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position("SHFE.ag.2512", false, "nope"), 0.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_avgpx("SHFE.ag.2512"), 0.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_profit("SHFE.ag.2512"), 0.0);
    EXPECT_EQ(ctx.stra_get_first_entertime("SHFE.ag.2512"), 0u);
    EXPECT_EQ(ctx.stra_get_last_entertime("SHFE.ag.2512"), 0u);
    EXPECT_EQ(ctx.stra_get_last_exittime("SHFE.ag.2512"), 0u);
    EXPECT_DOUBLE_EQ(ctx.stra_get_last_enterprice("SHFE.ag.2512"), 0.0);
    EXPECT_STREQ(ctx.stra_get_last_entertag("SHFE.ag.2512"), "");
    EXPECT_EQ(ctx.stra_get_detail_entertime("SHFE.ag.2512", "t1"), 0u);
    EXPECT_DOUBLE_EQ(ctx.stra_get_detail_cost("SHFE.ag.2512", "t1"), 0.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_detail_profit("SHFE.ag.2512", "t1", 0), 0.0);
}
