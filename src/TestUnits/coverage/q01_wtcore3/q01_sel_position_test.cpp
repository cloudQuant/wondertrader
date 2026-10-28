/**
 * q01_sel_position_test.cpp —— SelStraBaseCtx 下单前置校验 + 信号登记 + 崩溃类收口
 *
 * 启用覆盖：
 *  - stra_set_position：品种缺失拒绝 / 不可做空拒绝 / 目标与当前相等早退 / 正常登记信号
 *  - append_signal -> save_data 落盘 + enum_position 合并信号视图
 * 崩溃类整条收进唯一 DISABLED（详见 manifest）：_data_mgr 为 NULL 时
 *  stra_get_day_price / stra_get_bars / stra_get_ticks / stra_get_last_tick 以及
 *  「信号 + 交易时段」驱动的 do_set_position（经 handle_pos_change->append_signal->get_cur_price）
 *  都会解引用空历史数据管理器而段错误。
 */
#include "q01_sel_env.h"

#include <fstream>

using namespace q01sel;

class Q01SelPositionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        _env.reset(new Q01TempEnv("pos"));
        _bdm.regisSession("TRADING");   // 无交易时段
        _comm = makeComm("SHFE", "ag", "TRADING", TM_Both, CC_Future);
        _ct = makeCt("SHFE", "ag", "ag2512");
        _bdm.regis(_comm, _ct);
        _stock = makeComm("SSE", "600000", "TRADING", TM_Long, CC_Stock);
        _sct = makeCt("SSE", "600000", "600000");
        _bdm.regis(_stock, _sct);
        _eng.attach(&_bdm, &_hot);
    }

    void TearDown() override { _env.reset(); }

    std::string dataDir()
    {
        std::filesystem::path p(WtHelper::getStraDataDir());
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        return p.string();
    }

    static void writeFile(const std::string& path, const std::string& content)
    {
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        ofs << content;
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

TEST_F(Q01SelPositionTest, set_position_rejects_unknown_commodity)
{
    SelCtx ctx(&_eng, "x_unknown", 0);
    ctx.on_init();
    ctx.stra_set_position("DCE.rr.2509", 3.0, "u");   // 品种未注册 -> commInfo NULL -> 拒绝

    bool touched = false;
    ctx.enum_position([&](const char*, double) { touched = true; });
    EXPECT_FALSE(touched);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position("DCE.rr.2509"), 0.0);
}

TEST_F(Q01SelPositionTest, set_position_rejects_short_when_cannot_short)
{
    SelCtx ctx(&_eng, "x_noshort", 0);
    ctx.on_init();
    ctx.stra_set_position("SSE.600000.600000", -2.0, "s");   // TM_Long -> canShort() false -> 拒绝

    bool touched = false;
    ctx.enum_position([&](const char*, double) { touched = true; });
    EXPECT_FALSE(touched);
}

TEST_F(Q01SelPositionTest, set_position_records_signal)
{
    SelCtx ctx(&_eng, "x_rec", 0);
    ctx.on_init();
    WTSTickData* tk = makeTick("SHFE", "ag2512", 20250110, 20250110, 1010, 5320.0);
    ctx.on_tick("SHFE.ag.2512", tk, false);
    tk->release();

    ctx.stra_set_position("SHFE.ag.2512", 4.0, "r4");   // 正常登记信号

    std::string code;
    double vol = -1;
    ctx.enum_position([&](const char* c, double v) { code = c; vol = v; });
    EXPECT_EQ(code, "SHFE.ag.2512");
    EXPECT_DOUBLE_EQ(vol, 4.0);
    // 仓位尚未成交（信号待触发），查询仍为 0
    EXPECT_DOUBLE_EQ(ctx.stra_get_position("SHFE.ag.2512"), 0.0);
}

TEST_F(Q01SelPositionTest, set_position_equal_target_is_noop)
{
    static const char* kJson = R"JSON({
      "fund": {"total_profit":0.0,"total_dynprofit":0.0,"total_fees":0.0,"tdate":20250110},
      "positions": [
        {"code":"SHFE.ag.2512","volume":2.0,"closeprofit":0.0,"dynprofit":0.0,
         "lastentertime":202501100931,"lastexittime":0,"frozen":0.0,"frozendate":0,
         "details":[{"long":true,"price":5200.0,"volume":2.0,"opentime":202501100931,
           "opentdate":20250110,"maxprice":5200.0,"minprice":5200.0,
           "profit":0.0,"maxprofit":0.0,"maxloss":0.0,"opentag":"buy1"}]}
      ],
      "signals": {}
    })JSON";
    writeFile(dataDir() + "x_eq.json", kJson);
    SelCtx ctx(&_eng, "x_eq", 0);
    ctx.on_init();

    EXPECT_DOUBLE_EQ(ctx.stra_get_position("SHFE.ag.2512"), 2.0);
    ctx.stra_set_position("SHFE.ag.2512", 2.0, "eq");   // 目标==当前 -> 早退，不产信号

    // 目标不变则不应新增信号；enum_position 只见到既有仓位 2.0
    std::string code;
    double vol = -1;
    ctx.enum_position([&](const char* c, double v) { code = c; vol = v; });
    EXPECT_EQ(code, "SHFE.ag.2512");
    EXPECT_DOUBLE_EQ(vol, 2.0);
}

/**
 * 崩溃类收口：以下入口在裸引擎（_data_mgr=NULL）下都会解引用空指针段错误，
 * 启用态一律不触碰；保留本 DISABLED 用例登记这些真实可达但需完整引擎的分支，
 * 供评审排除。解开会立刻崩溃，符合"整类收进同一个 DISABLED"的纪律。
 */
TEST_F(Q01SelPositionTest, DISABLED_SelCrashClass_null_history_mgr)
{
    SelCtx ctx(&_eng, "x_crash", 0);
    ctx.on_init();
    // 1) 当日价：get_day_price 直接解引用 _data_mgr->grab_last_tick
    volatile double px = ctx.stra_get_day_price("SHFE.ag.2512", 0); (void)px;
    // 2) 分钟K线：get_kline_slice -> _data_mgr
    volatile auto* k = ctx.stra_get_bars("SHFE.ag.2512", "m1", 10); (void)k;
    // 3) tick 切片 / 最新 tick
    volatile auto* ts = ctx.stra_get_ticks("SHFE.ag.2512", 10); (void)ts;
    volatile auto* lt = ctx.stra_get_last_tick("SHFE.ag.2512"); (void)lt;
    // 4) 带交易时段的会话 + 待发信号 -> on_tick 触发 do_set_position -> handle_pos_change
    //    -> WtEngine::append_signal -> get_cur_price -> _data_mgr
    _bdm.regisSessionWithTime("TRADING", 900, 1500);
    _eng.setNow(20250110, 1000, 20250110);
    ctx.stra_set_position("SHFE.ag.2512", 5.0, "fire");
    WTSTickData* tk = makeTick("SHFE", "ag2512", 20250110, 20250110, 1000, 5350.0);
    ctx.on_tick("SHFE.ag.2512", tk, false);
    tk->release();
}
