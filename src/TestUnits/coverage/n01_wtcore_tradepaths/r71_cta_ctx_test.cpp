/**
 * r71_cta_ctx_test.cpp — lane R71 / CtaStraBaseCtx 余量补测
 *
 * 覆盖 src/WtCore/CtaStraBaseCtx.cpp 中 R42 未覆盖的行段：
 *   load_data 的缺省 maxprice/minprice/openbarno、零手明细、零手仓位
 *   update_dyn_profit 的 volume==0 与 max_loss 分支
 *   on_tick 条件单五种算法 + OL/CL/OS/CS/SP 五种合法动作；未知枚举不覆盖
 *   on_schedule 第 20 次统计、脏用户数据落盘、条件单落盘、非交易时段取消
 *   on_session_end 零手仓位跳过
 *   do_set_position 的 T+1 冻结、滑点、同仓位早退、无品种早退、平仓/反手
 *   stra_exit_long/stra_exit_short 的限价/止损条件单、stra_set_position 止损分支
 *   stra_get_bars/ticks/last_tick（WtDtMgr 派生替身返回确定性切片）
 *   stra_get_price/day_price 的引擎兜底、dump_chart_info 目录创建
 */
#include "r71_wtcore_env.h"

#include <map>

using namespace wtp;

namespace {

class R71CtaCtx : public CtaStraBaseCtx
{
public:
    R71CtaCtx(WtCtaEngine* engine, const char* name, int32_t slippage = 0)
        : CtaStraBaseCtx(engine, name, slippage)
    {
    }

    ~R71CtaCtx() override {}

    void on_tick_updated(const char* stdCode, WTSTickData* newTick) override
    {
        (void)newTick;
        ++tick_updates;
        last_tick_code = stdCode;
    }

    void on_bar_close(const char* stdCode, const char* period, WTSBarStruct* newBar) override
    {
        (void)stdCode;
        (void)newBar;
        ++bar_closes;
        last_bar_period = period;
    }

    void dump_chart() { dump_chart_info(); }

    void on_calculate(uint32_t curDate, uint32_t curTime) override
    {
        (void)curDate;
        (void)curTime;
        ++calc_times;
        if (emit_condition)
            stra_enter_long(cond_code.c_str(), 1.0, "sched", 9999.0);
    }

    void on_condition_triggered(const char* stdCode, double target, double price, const char* usertag) override
    {
        (void)price;
        (void)usertag;
        ++cond_fires;
        last_cond_code = stdCode;
        last_cond_target = target;
    }

    uint32_t tick_updates = 0;
    uint32_t bar_closes = 0;
    uint32_t calc_times = 0;
    uint32_t cond_fires = 0;
    std::string last_tick_code;
    std::string last_bar_period;
    std::string last_cond_code;
    double last_cond_target = 0;
    bool emit_condition = false;
    std::string cond_code;
};

class R71CtaTest : public ::testing::Test
{
protected:
    r71::R71GenEnv env;
    r71::R71BaseDataMgr bd;
    r71::R71HotMgr hot;
    r71::R71DataMgr dm;
    r71::R71CtaEngine engine;

    void SetUp() override
    {
        bd.regisSession("TRADING");
        WTSSessionInfo* sess = bd.session("TRADING");
        ASSERT_NE(sess, nullptr);
        sess->addTradingSection(900, 1500);

        reg("SHFE", "rb", "rb2510", TM_Both, sess);
        reg("SHFE", "ag", "ag2512", TM_LongT1, sess);
        reg("SHFE", "au", "au2212", TM_Both, sess);
        reg("SHFE", "cu", "cu2510", TM_Both, sess);
        reg("SHFE", "ni", "ni2510", TM_Both, sess);
        reg("SHFE", "zn", "zn2510", TM_Both, sess);
        reg("SHFE", "sn", "sn2510", TM_Both, sess);

        engine.attach(&bd, &hot, &dm);
    }

    void reg(const char* exchg, const char* pid, const char* code, TradingMode tm, WTSSessionInfo* sess)
    {
        WTSCommodityInfo* comm = r71::makeComm(exchg, pid, "TRADING", tm);
        comm->setSessionInfo(sess);
        bd.regis(comm, r71::makeCt(exchg, pid, code));
    }

    std::unique_ptr<R71CtaCtx> makeCtx(const char* name, int32_t slippage = 0)
    {
        return std::unique_ptr<R71CtaCtx>(new R71CtaCtx(&engine, name, slippage));
    }

    void feed(CtaStraBaseCtx& ctx, const char* stdCode, double price, bool emit = true)
    {
        WTSTickData* tick = r71::makeTick("SHFE", stdCode, 20250110, 20250110, 93100000, price);
        ASSERT_NE(tick, nullptr);
        ctx.on_tick(stdCode, tick, emit);
        tick->release();
    }

};

// 覆盖 CtaStraBaseCtx.cpp:359-395 load_data 明细缺省字段、L752-793 零手/亏损分支、
// L1073-1109 零手仓位跳过、L1761-1885 空明细查询
TEST_F(R71CtaTest, LoadDataOptionalDefaultsAndZeroVolumeQueries)
{
    auto ctx = makeCtx("r71_cta_optional");
    const char* json = R"json({
  "fund": {"total_profit": 1.5, "total_dynprofit": 2.5, "tdate": 20250110, "total_fees": 0.5},
  "positions": [
    {"code": "SHFE.rb.2510", "volume": 2, "closeprofit": 0.5, "dynprofit": 1.0,
     "lastentertime": 11, "lastexittime": 12,
     "details": [{"long": true, "price": 3000, "volume": 2, "opentime": 11,
                  "profit": 1.0, "maxprofit": 2.0, "maxloss": -1.0, "opentag": "A"}]},
    {"code": "SHFE.ag.2512", "volume": 0, "closeprofit": 0.0, "dynprofit": 3.0,
     "lastentertime": 0, "lastexittime": 0,
     "details": [{"long": true, "price": 3000, "volume": 0, "opentime": 0,
                  "profit": 0.0, "maxprofit": 0.0, "maxloss": 0.0, "opentag": "Z"}]}
  ]
})json";
    ASSERT_TRUE(r71::write_file(r71::stra_data_file("r71_cta_optional"), json));

    ctx->on_init();

    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.rb.2510"), 2.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position_avgpx("SHFE.rb.2510"), 3000.0);
    // maxprice/minprice/openbarno 缺省 -> 取开仓价 / 0
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit("SHFE.rb.2510", "A", 2), 3000.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit("SHFE.rb.2510", "A", -2), 3000.0);
    EXPECT_EQ(ctx->stra_get_detail_entertime("SHFE.rb.2510", "A"), 11u);

    // 零手仓位：明细 volume==0 被跳过，仓位查询全部走空/零分支
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.ag.2512"), 0.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position_avgpx("SHFE.ag.2512"), 0.0);
    EXPECT_EQ(ctx->stra_get_first_entertime("SHFE.ag.2512"), 0u);
    EXPECT_STREQ(ctx->stra_get_last_entertag("SHFE.ag.2512"), "");
    EXPECT_EQ(ctx->stra_get_last_entertime("SHFE.ag.2512"), 0u);
    EXPECT_DOUBLE_EQ(ctx->stra_get_last_enterprice("SHFE.ag.2512"), 0.0);

    // volume==0 -> update_dyn_profit 直接把 dynprofit 置 0
    feed(*ctx, "SHFE.ag.2512", 3600.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position_profit("SHFE.ag.2512"), 0.0);

    // 亏损 tick 覆盖 max_loss 分支
    feed(*ctx, "SHFE.rb.2510", 2900.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position_profit("SHFE.rb.2510"), -2000.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit("SHFE.rb.2510", "A", -1), -2000.0);

    ctx->on_session_end(20250110);
    const std::string posFile = std::string(WtHelper::getOutputDir()) + "r71_cta_optional/positions.csv";
    const std::string rows = r71::read_file(posFile);
    EXPECT_EQ(rows.find("SHFE.ag.2512"), std::string::npos);
    EXPECT_NE(rows.find("SHFE.rb.2510"), std::string::npos);
}

// 覆盖 CtaStraBaseCtx.cpp:825-935 条件单五种算法与五种合法动作；未知枚举不覆盖
TEST_F(R71CtaTest, TickConditionsCoverAllAlgorithmsAndActions)
{
    auto ctx = makeCtx("r71_cta_cond");
    // on_init/load_data 会先用 ACTION_NAMES[action] 写恢复日志；未知 action 在到达 on_tick
    // default 分支前即越界。未知动作需安全测试接缝或独立的非法持久化输入拒绝契约，本用例不注入。
    const char* json = R"json({
  "positions": [
    {"code": "SHFE.rb.2510", "volume": 0, "closeprofit": 0, "dynprofit": 0, "lastentertime": 0, "lastexittime": 0, "details": []},
    {"code": "SHFE.ag.2512", "volume": -1, "closeprofit": 0, "dynprofit": 0, "lastentertime": 0, "lastexittime": 0, "details": []},
    {"code": "SHFE.au.2212", "volume": 5, "closeprofit": 0, "dynprofit": 0, "lastentertime": 0, "lastexittime": 0, "details": []},
    {"code": "SHFE.cu.2510", "volume": 1, "closeprofit": 0, "dynprofit": 0, "lastentertime": 0, "lastexittime": 0, "details": []},
    {"code": "SHFE.ni.2510", "volume": -3, "closeprofit": 0, "dynprofit": 0, "lastentertime": 0, "lastexittime": 0, "details": []},
    {"code": "SHFE.zn.2510", "volume": 0, "closeprofit": 0, "dynprofit": 0, "lastentertime": 0, "lastexittime": 0, "details": []}
  ],
  "conditions": {"settime": 0, "items": {
    "SHFE.rb.2510": [{"usertag":"c1","field":0,"alg":0,"target":3500.0,"qty":2.0,"action":0}],
    "SHFE.ag.2512": [{"usertag":"c2","field":0,"alg":1,"target":3400.0,"qty":3.0,"action":0}],
    "SHFE.au.2212": [{"usertag":"c3","field":0,"alg":3,"target":3300.0,"qty":2.0,"action":1}],
    "SHFE.cu.2510": [{"usertag":"c4","field":0,"alg":2,"target":3600.0,"qty":4.0,"action":2}],
    "SHFE.ni.2510": [{"usertag":"c5","field":0,"alg":4,"target":3700.0,"qty":1.0,"action":3}],
    "SHFE.zn.2510": [{"usertag":"c6","field":0,"alg":0,"target":3500.0,"qty":7.0,"action":4}]
  }}
})json";
    ASSERT_TRUE(r71::write_file(r71::stra_data_file("r71_cta_cond"), json));
    ctx->on_init();

    std::map<std::string, double> targets;
    auto snapshot = [&targets, &ctx]() {
        targets.clear();
        ctx->enum_position([&targets](const char* code, double pos) {
            targets[code] = pos;
        });
    };

    feed(*ctx, "SHFE.rb.2510", 3500.0); // Equal + OL, 0 -> 2
    feed(*ctx, "SHFE.ag.2512", 3401.0); // Larger + OL, -1 -> 3
    feed(*ctx, "SHFE.au.2212", 3300.0); // LargerOrEqual + CL, 5 -> 3
    feed(*ctx, "SHFE.cu.2510", 3599.0); // Smaller + OS, 1 -> -4
    feed(*ctx, "SHFE.ni.2510", 3700.0); // SmallerOrEqual + CS, -3 -> -2
    feed(*ctx, "SHFE.zn.2510", 3500.0); // Equal + SP -> 7

    snapshot();
    EXPECT_DOUBLE_EQ(targets["SHFE.rb.2510"], 2.0);
    EXPECT_DOUBLE_EQ(targets["SHFE.ag.2512"], 3.0);
    EXPECT_DOUBLE_EQ(targets["SHFE.au.2212"], 3.0);
    EXPECT_DOUBLE_EQ(targets["SHFE.cu.2510"], -4.0);
    EXPECT_DOUBLE_EQ(targets["SHFE.ni.2510"], -2.0);
    EXPECT_DOUBLE_EQ(targets["SHFE.zn.2510"], 7.0);
}

// 覆盖 CtaStraBaseCtx.cpp:1411-1575 do_set_position 的平仓/反手/T+1/滑点/早退，
// L198-218 log_trade/log_close 落盘
TEST_F(R71CtaTest, CloseReverseSlippageAndEarlyExitsWriteLogs)
{
    auto ctx = makeCtx("r71_cta_close", 2);
    const char* json = R"json({
  "positions": [
    {"code": "SHFE.rb.2510", "volume": 2, "closeprofit": 0, "dynprofit": 0,
     "lastentertime": 1, "lastexittime": 0,
     "details": [{"long": true, "price": 3000, "maxprice": 3000, "minprice": 3000,
                  "volume": 2, "opentime": 1, "opentdate": 20250110, "profit": 0,
                  "maxprofit": 0, "maxloss": 0, "opentag": "A", "openbarno": 0}]},
    {"code": "SHFE.au.2212", "volume": -3, "closeprofit": 0, "dynprofit": 0,
     "lastentertime": 1, "lastexittime": 0,
     "details": [{"long": false, "price": 4000, "maxprice": 4000, "minprice": 4000,
                  "volume": 3, "opentime": 1, "opentdate": 20250110, "profit": 0,
                  "maxprofit": 0, "maxloss": 0, "opentag": "B", "openbarno": 0}]}
  ]
})json";
    ASSERT_TRUE(r71::write_file(r71::stra_data_file("r71_cta_close"), json));
    ctx->on_init();

    // 多头全平：滑点 +2 * priceTick(1)
    ctx->stra_set_position("SHFE.rb.2510", 0.0, "X");
    feed(*ctx, "SHFE.rb.2510", 3500.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.rb.2510"), 0.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position_profit("SHFE.rb.2510"), 0.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_fund_data(1), 10040.0); // (3502-3000)*2*10

    // 0 -> -1：空仓反手开空
    ctx->stra_set_position("SHFE.rb.2510", -1.0, "Y");
    feed(*ctx, "SHFE.rb.2510", 3400.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.rb.2510"), -1.0);

    // 空头全平
    ctx->stra_set_position("SHFE.au.2212", 0.0, "Z");
    feed(*ctx, "SHFE.au.2212", 3900.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.au.2212"), 0.0);

    // T+1 品种从 0 开多（反手分支的冻结 + 滑点）
    ctx->stra_set_position("SHFE.ag.2512", 3.0, "T");
    feed(*ctx, "SHFE.ag.2512", 3500.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.ag.2512"), 3.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.ag.2512", true), 0.0);

    // 同方向加仓 -> 开仓分支的 T+1 冻结 + 滑点
    ctx->stra_set_position("SHFE.ag.2512", 4.0, "T1B");
    feed(*ctx, "SHFE.ag.2512", 3510.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.ag.2512"), 4.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.ag.2512", true), 0.0);

    // 目标仓位与当前一致 -> do_set_position 早退
    ctx->stra_set_position("SHFE.ag.2512", 4.0, "T2");
    feed(*ctx, "SHFE.ag.2512", 3520.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.ag.2512"), 4.0);

    // 无对应品种 -> do_set_position 早退
    ctx->stra_set_position("SHFE.xx.9999", 1.0, "NO");
    feed(*ctx, "SHFE.xx.9999", 3500.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.xx.9999"), 0.0);

    const std::string out = std::string(WtHelper::getOutputDir()) + "r71_cta_close/";
    EXPECT_NE(r71::read_file(out + "closes.csv").find("SHFE.rb.2510"), std::string::npos);
    EXPECT_NE(r71::read_file(out + "trades.csv").find("SHFE.au.2212"), std::string::npos);
}

// 覆盖 CtaStraBaseCtx.cpp:937-1016 on_schedule 全部分支（20 次统计、脏数据、
// 条件单落盘、非交易时段取消）、L1635-1642 主 K 线首次拉取的 barNo 修正
TEST_F(R71CtaTest, ScheduleEmitsTwentyTimesAndPersistsCondition)
{
    auto ctx = makeCtx("r71_cta_sched");
    ctx->on_init();
    ctx->emit_condition = true;
    ctx->cond_code = "SHFE.rb.2510";

    ASSERT_NE(ctx->stra_get_bars("SHFE.rb.2510", "m", 3, true), nullptr);

    WTSBarStruct bar;
    memset(&bar, 0, sizeof(bar));
    bar.close = 3008.0;

    ctx->stra_save_user_data("dirty", "yes");
    for (int i = 0; i < 20; i++)
    {
        ctx->on_bar("SHFE.rb.2510", "m", 1, &bar);
        EXPECT_TRUE(ctx->on_schedule(20250110, 931));
    }
    EXPECT_EQ(ctx->calc_times, 20u);

    const std::string ud = std::string(WtHelper::getStraUsrDatDir()) + "ud_r71_cta_sched.json";
    EXPECT_NE(r71::read_file(ud).find("\"dirty\""), std::string::npos);
    const std::string data = r71::read_file(r71::stra_data_file("r71_cta_sched"));
    EXPECT_NE(data.find("\"conditions\""), std::string::npos);
    EXPECT_NE(data.find("\"lastbarno\""), std::string::npos);

    // 主 K 线闭合但已过收盘时间 -> on_calculate 不触发
    engine.set_date_time(20250110, 1600, 0);
    ctx->on_bar("SHFE.rb.2510", "m", 1, &bar);
    EXPECT_FALSE(ctx->on_schedule(20250110, 1600));
    EXPECT_EQ(ctx->calc_times, 20u);
}

// 覆盖 CtaStraBaseCtx.cpp:1577-1660 历史切片、L1332-1350 价格兜底、
// L679-750 dump_chart_info 的目录创建分支
TEST_F(R71CtaTest, BarsTicksPricesAndChartDump)
{
    auto ctx = makeCtx("r71_cta_slices");

    // 未 on_init：输出子目录不存在，dump 时创建
    ctx->set_chart_kline("SHFE.rb.2510", "m1");
    ctx->dump_chart();
    const std::string chart = std::string(WtHelper::getOutputDir()) + "r71_cta_slices/rtchart.json";
    EXPECT_NE(r71::read_file(chart).find("SHFE.rb.2510"), std::string::npos);

    WTSKlineSlice* m5 = ctx->stra_get_bars("SHFE.rb.2510", "m5", 2);
    ASSERT_NE(m5, nullptr);
    EXPECT_EQ(m5->size(), 3u);
    m5->release();
    EXPECT_EQ(dm.kline_calls, 1u);

    WTSKlineSlice* main = ctx->stra_get_bars("SHFE.rb.2510", "m", 2, true);
    ASSERT_NE(main, nullptr);
    main->release();
    // 主 K 线已确认，换一个合约再声明主 K 线 -> 拒绝
    EXPECT_TRUE(ctx->stra_get_bars("SHFE.au.2212", "m", 2, true) == NULL);

    WTSTickSlice* ticks = ctx->stra_get_ticks("SHFE.rb.2510", 2);
    ASSERT_NE(ticks, nullptr);
    EXPECT_EQ(ticks->size(), 3u);
    ticks->release();
    EXPECT_EQ(dm.tick_calls, 1u);

    WTSTickData* last = ctx->stra_get_last_tick("SHFE.rb.2510");
    ASSERT_NE(last, nullptr);
    EXPECT_DOUBLE_EQ(last->price(), 3502.0);
    last->release();

    // 上下文价格缓存未命中 -> 引擎兜底（_data_mgr->grab_last_tick）
    EXPECT_DOUBLE_EQ(ctx->stra_get_price("SHFE.ag.2512"), 3502.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_day_price("SHFE.rb.2510", 0), 3480.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_day_price("SHFE.rb.2510", 1), 3520.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_day_price("SHFE.rb.2510", 2), 3470.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_day_price("SHFE.rb.2510", 3), 3502.0);

    WTSCommodityInfo* comm = ctx->stra_get_comminfo("SHFE.rb.2510");
    ASSERT_NE(comm, nullptr);
    EXPECT_STREQ(comm->getProduct(), "rb");
    EXPECT_EQ(ctx->stra_get_rawcode("SHFE.rb.2510"), std::string());
}

// 覆盖 CtaStraBaseCtx.cpp:1229-1330 stra_exit_long/stra_exit_short 的条件单分支、
// L1352-1393 stra_set_position 止损分支与同仓位早退
TEST_F(R71CtaTest, ExitAndSetPositionConditionBranches)
{
    auto ctx = makeCtx("r71_cta_exit");
    const char* json = R"json({
  "positions": [
    {"code": "SHFE.rb.2510", "volume": 5, "closeprofit": 0, "dynprofit": 0, "lastentertime": 0, "lastexittime": 0,
     "details": [{"long": true, "price": 3000, "maxprice": 3000, "minprice": 3000, "volume": 5,
                  "opentime": 1, "opentdate": 20250110, "profit": 0, "maxprofit": 0,
                  "maxloss": 0, "opentag": "A", "openbarno": 0}]},
    {"code": "SHFE.cu.2510", "volume": 2, "closeprofit": 0, "dynprofit": 0, "lastentertime": 0, "lastexittime": 0,
     "details": [{"long": true, "price": 3000, "maxprice": 3000, "minprice": 3000, "volume": 2,
                  "opentime": 1, "opentdate": 20250110, "profit": 0, "maxprofit": 0,
                  "maxloss": 0, "opentag": "B", "openbarno": 0}]},
    {"code": "SHFE.au.2212", "volume": -4, "closeprofit": 0, "dynprofit": 0, "lastentertime": 0, "lastexittime": 0,
     "details": [{"long": false, "price": 4000, "maxprice": 4000, "minprice": 4000, "volume": 4,
                  "opentime": 1, "opentdate": 20250110, "profit": 0, "maxprofit": 0,
                  "maxloss": 0, "opentag": "C", "openbarno": 0}]}
  ]
})json";
    ASSERT_TRUE(r71::write_file(r71::stra_data_file("r71_cta_exit"), json));
    ctx->on_init();

    std::map<std::string, double> targets;
    auto snapshot = [&targets, &ctx]() {
        targets.clear();
        ctx->enum_position([&targets](const char* code, double pos) {
            targets[code] = pos;
        });
    };

    // 限价平多：CL + LargerOrEqual
    ctx->stra_exit_long("SHFE.rb.2510", 2.0, "EL", 3400.0);
    feed(*ctx, "SHFE.rb.2510", 3400.0);
    snapshot();
    EXPECT_DOUBLE_EQ(targets["SHFE.rb.2510"], 3.0);

    // 止损平多：CL + SmallerOrEqual
    ctx->stra_exit_long("SHFE.cu.2510", 1.0, "ELS", 0.0, 3300.0);
    feed(*ctx, "SHFE.cu.2510", 3300.0);
    snapshot();
    EXPECT_DOUBLE_EQ(targets["SHFE.cu.2510"], 1.0);

    // 限价平空：CS + SmallerOrEqual
    ctx->stra_exit_short("SHFE.au.2212", 2.0, "ES", 4000.0);
    feed(*ctx, "SHFE.au.2212", 4000.0);
    snapshot();
    EXPECT_DOUBLE_EQ(targets["SHFE.au.2212"], -2.0);

    // stra_set_position 止损分支：SP + LargerOrEqual
    ctx->stra_set_position("SHFE.zn.2510", 2.0, "SP", 0.0, 3300.0);
    feed(*ctx, "SHFE.zn.2510", 3300.0);
    snapshot();
    EXPECT_DOUBLE_EQ(targets["SHFE.zn.2510"], 2.0);

    // 目标仓位与当前一致 -> 不登记条件单
    ctx->stra_set_position("SHFE.ni.2510", 0.0, "EQ", 0.0, 3300.0);
    feed(*ctx, "SHFE.ni.2510", 3300.0);
    snapshot();
    EXPECT_DOUBLE_EQ(targets["SHFE.ni.2510"], 0.0);
}

} // namespace
