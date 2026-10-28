/**
 * r81_cta_gap_test.cpp — lane R81 / CtaStraBaseCtx 剩余行段
 *
 * 覆盖 src/WtCore/CtaStraBaseCtx.cpp：
 *   load_data 的可选字段/零手明细；空明细查询守卫；update_dyn_profit 零手与亏损极值
 *   on_tick 条件单五种算法、五种动作、无对应条件早退；on_schedule 第 20 次统计/脏用户数据/条件单落盘
 *   on_session_end 零手仓位跳过；stra_exit_long/short 限价/止损、stra_set_position 止损
 *   do_set_position 同仓早退、无品种早退、T+1 冻结、滑点、平仓/反手与 log_close
 *   stra_get_price/day_price 在无引擎时的零值兜底
 *
 * 只走公有 API 与测试内子类的 protected 包装；不使用 private 宏/布局镜像。
 */
#include "r81_env.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>

using namespace wtp;

namespace {

class R81CtaCtx : public CtaStraBaseCtx
{
public:
    R81CtaCtx(WtCtaEngine* engine, const char* name, int32_t slippage = 0)
        : CtaStraBaseCtx(engine, name, slippage)
    {
    }

    ~R81CtaCtx() override {}

    void on_tick_updated(const char* stdCode, WTSTickData* newTick) override
    {
        (void)newTick;
        ++tick_updates;
        last_tick_code = stdCode;
    }

    void on_bar_close(const char* stdCode, const char* period, WTSBarStruct* newBar) override
    {
        (void)newBar;
        ++bar_closes;
        last_bar_code = stdCode;
        last_bar_period = period;
    }

    void on_calculate(uint32_t curDate, uint32_t curTime) override
    {
        ++calc_times;
        last_calc_date = curDate;
        last_calc_time = curTime;
        if (emit_condition)
            stra_enter_long(cond_code.c_str(), 1.0, "sched", 9999.0);
    }

    void on_condition_triggered(const char* stdCode, double target, double price,
        const char* usertag) override
    {
        (void)price;
        (void)usertag;
        ++cond_fires;
        last_cond_code = stdCode;
        last_cond_target = target;
    }

    // protected 包装（测试内子类，仅驱动被测基类实现）
    void set_main_key(const char* key) { _main_key = key; }
    // 注入一条非法比较算法的条件单，用于覆盖 switch 的 default 分支。
    // 正常 load_data 会对 alg 做 CMP_ALG_NAMES 下标访问，非法值属越界，故只从测试侧注入。
    void inject_condition(const char* stdCode, WTSCompareType alg, char action,
        double target, double qty, const char* tag)
    {
        CondEntrust entrust;
        wt_strcpy(entrust._code, stdCode);
        wt_strcpy(entrust._usertag, tag);
        entrust._field = WCF_NEWPRICE;
        entrust._alg = alg;
        entrust._target = target;
        entrust._qty = qty;
        entrust._action = action;
        _condtions[stdCode].emplace_back(entrust);
    }

    uint32_t tick_updates = 0;
    uint32_t bar_closes = 0;
    uint32_t calc_times = 0;
    uint32_t cond_fires = 0;
    std::string last_tick_code;
    std::string last_bar_code;
    std::string last_bar_period;
    std::string last_cond_code;
    double last_cond_target = 0;
    uint32_t last_calc_date = 0;
    uint32_t last_calc_time = 0;
    bool emit_condition = false;
    std::string cond_code;
};

class R81CtaTest : public ::testing::Test
{
protected:
    r81::R81GenEnv env;
    r81::R81BaseDataMgr bd;
    r81::R81HotMgr hot;
    r81::R81CtaEngine engine;

    void SetUp() override
    {
        bd.regisSession("TRADING");
        WTSSessionInfo* sess = bd.session("TRADING");
        ASSERT_NE(sess, nullptr);
        sess->addTradingSection(900, 1500);

        reg("SHFE", "rb", "rb2510", TM_Both, sess);
        reg("SHFE", "ag", "ag2512", TM_LongT1, sess);
        reg("SHFE", "cu", "cu2510", TM_Both, sess);
        reg("SHFE", "au", "au2212", TM_Both, sess);
        reg("SHFE", "ni", "ni2510", TM_Both, sess);
        reg("SHFE", "zn", "zn2510", TM_Both, sess);
        reg("SHFE", "sn", "sn2510", TM_Both, sess);

        engine.attach(&bd, &hot);
    }

    void reg(const char* exchg, const char* pid, const char* code, TradingMode tm,
        WTSSessionInfo* sess)
    {
        WTSCommodityInfo* comm = r81::makeComm(exchg, pid, "TRADING", tm);
        comm->setSessionInfo(sess);
        bd.regis(comm, r81::makeCt(exchg, pid, code));
    }

    std::unique_ptr<R81CtaCtx> makeCtx(const char* name, int32_t slippage = 0)
    {
        return std::unique_ptr<R81CtaCtx>(new R81CtaCtx(&engine, name, slippage));
    }

    void feed(R81CtaCtx& ctx, const char* stdCode, double price, bool emit = false)
    {
        WTSTickData* tick = r81::makeTick("SHFE", stdCode, 20250110, 20250110, 93100000, price);
        ASSERT_NE(tick, nullptr);
        engine.prime_price(stdCode, price);
        ctx.on_tick(stdCode, tick, emit);
        tick->release();
    }
};

// 覆盖 CtaStraBaseCtx.cpp:359-395（零手明细/缺省 openbarno）、752-793（零手/亏损极值）、
// 1073-1109（零手仓位跳过）、1761-1885（空明细/零手查询守卫）
TEST_F(R81CtaTest, LoadOptionalFieldsAndZeroVolumeQueries)
{
    auto ctx = makeCtx("r81_cta_optional");
    const char* json = R"json({
  "fund": {"total_profit": 0.5, "total_dynprofit": 1.0, "total_fees": 0.1, "tdate": 20250110},
  "positions": [
    {"code": "SHFE.rb.2510", "volume": 2, "closeprofit": 0.5, "dynprofit": 1.0,
     "lastentertime": 11, "lastexittime": 12,
     "details": [
       {"long": true, "price": 3000, "volume": 0, "opentime": 10, "profit": 0,
        "maxprofit": 0, "maxloss": 0, "opentag": "ZERO"},
       {"long": true, "price": 3000, "volume": 2, "opentime": 11, "profit": 1.0,
        "maxprofit": 2.0, "maxloss": -1.0, "opentag": "A"}
     ]},
    {"code": "SHFE.ag.2512", "volume": 0, "closeprofit": 0, "dynprofit": 3.0,
     "lastentertime": 0, "lastexittime": 0, "details": []}
  ],
  "conditions": {"settime": 0, "items": {}}
})json";
    ASSERT_TRUE(r81::write_file(r81::stra_data_file("r81_cta_optional"), json));

    ctx->on_init();

    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.rb.2510"), 2.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position_avgpx("SHFE.rb.2510"), 3000.0);
    // 缺省 maxprice/minprice 取开仓价；opentdate/openbarno 缺省
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit("SHFE.rb.2510", "A", 2), 3000.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit("SHFE.rb.2510", "A", -2), 3000.0);
    EXPECT_EQ(ctx->stra_get_detail_entertime("SHFE.rb.2510", "A"), 11u);

    // 零手仓位：明细为空，全部查询走零值守卫
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.ag.2512"), 0.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position_avgpx("SHFE.ag.2512"), 0.0);
    EXPECT_EQ(ctx->stra_get_first_entertime("SHFE.ag.2512"), 0u);
    EXPECT_STREQ(ctx->stra_get_last_entertag("SHFE.ag.2512"), "");
    EXPECT_EQ(ctx->stra_get_last_entertime("SHFE.ag.2512"), 0u);
    EXPECT_DOUBLE_EQ(ctx->stra_get_last_enterprice("SHFE.ag.2512"), 0.0);

    // volume==0 -> update_dyn_profit 直接清零
    feed(*ctx, "SHFE.ag.2512", 3600.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position_profit("SHFE.ag.2512"), 0.0);

    // 下跌 tick -> max_loss 分支
    feed(*ctx, "SHFE.rb.2510", 2900.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position_profit("SHFE.rb.2510"), -2000.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit("SHFE.rb.2510", "A", -1), -2000.0);

    ctx->on_session_end(20250110);
    const std::string posFile = std::string(WtHelper::getOutputDir())
        + "r81_cta_optional/positions.csv";
    const std::string rows = r81::read_file(posFile);
    EXPECT_EQ(rows.find("SHFE.ag.2512"), std::string::npos);
    EXPECT_NE(rows.find("SHFE.rb.2510"), std::string::npos);
}

// 覆盖 CtaStraBaseCtx.cpp:825-935 条件单算法/动作全分支（含无对应条件早退与 default alg）、
// 1395-1409 append_signal
TEST_F(R81CtaTest, ConditionAlgorithmsActionsAndNoMatchCode)
{
    auto ctx = makeCtx("r81_cta_cond");
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
    ASSERT_TRUE(r81::write_file(r81::stra_data_file("r81_cta_cond"), json));
    ctx->on_init();

    // 条件表非空，但当前 tick 没有对应条件 -> 直接 return
    feed(*ctx, "SHFE.sn.2510", 3500.0);

    // 注入非法 alg：switch 走 default，不匹配、不产生信号
    ctx->inject_condition("SHFE.sn.2510", (WTSCompareType)99, COND_ACTION_SP, 3500.0, 1.0, "badalg");
    feed(*ctx, "SHFE.sn.2510", 3500.0);

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
    EXPECT_DOUBLE_EQ(targets["SHFE.sn.2510"], 0.0);
}

// 覆盖 CtaStraBaseCtx.cpp:937-1016 on_schedule（第 20 次统计、脏用户数据落盘、条件单落盘）
TEST_F(R81CtaTest, ScheduleTwentiethEmitPersistsUserDataAndConditions)
{
    auto ctx = makeCtx("r81_cta_sched");
    ctx->on_init();
    ctx->set_main_key("SHFE.rb.2510#m1");
    ctx->emit_condition = true;
    ctx->cond_code = "SHFE.rb.2510";

    ctx->stra_save_user_data("dirty", "yes");

    WTSBarStruct bar;
    memset(&bar, 0, sizeof(bar));
    bar.close = 3008.0;
    for (int i = 0; i < 20; ++i)
    {
        ctx->on_bar("SHFE.rb.2510", "m", 1, &bar);
        EXPECT_TRUE(ctx->on_schedule(20250110, 931));
    }

    EXPECT_EQ(ctx->calc_times, 20u);
    EXPECT_EQ(ctx->last_calc_date, 20250110u);
    EXPECT_EQ(ctx->last_calc_time, 931u);

    const std::string ud = r81::read_file(r81::user_data_file("r81_cta_sched"));
    EXPECT_NE(ud.find("\"dirty\""), std::string::npos);

    const std::string data = r81::read_file(r81::stra_data_file("r81_cta_sched"));
    EXPECT_NE(data.find("\"conditions\""), std::string::npos);
    EXPECT_NE(data.find("\"lastbarno\""), std::string::npos);
}

// 覆盖 CtaStraBaseCtx.cpp:198-218 log_close、1229-1330 平多/平空条件分支、
// 1352-1393 set_position 止损、1411-1575 do_set_position 全分支
TEST_F(R81CtaTest, ExitConditionsAndPositionAccountingBranches)
{
    auto ctx = makeCtx("r81_cta_exit", 2);
    const char* json = R"json({
  "positions": [
    {"code": "SHFE.rb.2510", "volume": 5, "closeprofit": 0, "dynprofit": 0,
     "lastentertime": 1, "lastexittime": 0,
     "details": [{"long": true, "price": 3000, "maxprice": 3000, "minprice": 3000,
                  "volume": 5, "opentime": 1, "opentdate": 20250110, "profit": 0,
                  "maxprofit": 0, "maxloss": 0, "opentag": "A", "openbarno": 1}]},
    {"code": "SHFE.cu.2510", "volume": 2, "closeprofit": 0, "dynprofit": 0,
     "lastentertime": 1, "lastexittime": 0,
     "details": [{"long": true, "price": 3000, "maxprice": 3000, "minprice": 3000,
                  "volume": 2, "opentime": 1, "opentdate": 20250110, "profit": 0,
                  "maxprofit": 0, "maxloss": 0, "opentag": "B", "openbarno": 1}]},
    {"code": "SHFE.au.2212", "volume": -4, "closeprofit": 0, "dynprofit": 0,
     "lastentertime": 1, "lastexittime": 0,
     "details": [{"long": false, "price": 4000, "maxprice": 4000, "minprice": 4000,
                  "volume": 4, "opentime": 1, "opentdate": 20250110, "profit": 0,
                  "maxprofit": 0, "maxloss": 0, "opentag": "C", "openbarno": 1}]},
    {"code": "SHFE.ag.2512", "volume": 0, "closeprofit": 0, "dynprofit": 0,
     "lastentertime": 0, "lastexittime": 0, "details": []},
    {"code": "SHFE.ni.2510", "volume": 0, "closeprofit": 0, "dynprofit": 0,
     "lastentertime": 0, "lastexittime": 0, "details": []}
  ],
  "conditions": {"settime": 0, "items": {}}
})json";
    ASSERT_TRUE(r81::write_file(r81::stra_data_file("r81_cta_exit"), json));
    ctx->on_init();

    // 限价平多：CL + LargerOrEqual
    ctx->stra_exit_long("SHFE.rb.2510", 2.0, "EL", 3400.0);
    feed(*ctx, "SHFE.rb.2510", 3400.0);
    feed(*ctx, "SHFE.rb.2510", 3400.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.rb.2510"), 3.0);

    // 止损平多：CL + SmallerOrEqual
    ctx->stra_exit_long("SHFE.cu.2510", 1.0, "ELS", 0.0, 3300.0);
    feed(*ctx, "SHFE.cu.2510", 3300.0);
    feed(*ctx, "SHFE.cu.2510", 3300.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.cu.2510"), 1.0);

    // 限价平空：CS + SmallerOrEqual
    ctx->stra_exit_short("SHFE.au.2212", 2.0, "ES", 4000.0);
    feed(*ctx, "SHFE.au.2212", 4000.0);
    feed(*ctx, "SHFE.au.2212", 4000.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.au.2212"), -2.0);

    // 止损平空：CS + LargerOrEqual
    ctx->stra_exit_short("SHFE.au.2212", 1.0, "ESS", 0.0, 4100.0);
    feed(*ctx, "SHFE.au.2212", 4100.0);
    feed(*ctx, "SHFE.au.2212", 4100.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.au.2212"), -1.0);

    // stra_set_position 止损分支：SP + LargerOrEqual
    ctx->stra_set_position("SHFE.ni.2510", 2.0, "SP", 0.0, 3300.0);
    feed(*ctx, "SHFE.ni.2510", 3300.0);
    feed(*ctx, "SHFE.ni.2510", 3300.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.ni.2510"), 2.0);

    // do_set_position：目标==当前早退（经公有 stra_set_position + tick 驱动）
    ctx->stra_set_position("SHFE.ni.2510", 2.0, "same");
    feed(*ctx, "SHFE.ni.2510", 3300.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.ni.2510"), 2.0);

    // T+1 + 滑点：开仓/加仓/全平
    ctx->stra_set_position("SHFE.ag.2512", 3.0, "T1");
    feed(*ctx, "SHFE.ag.2512", 3500.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.ag.2512"), 3.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.ag.2512", true), 0.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position_avgpx("SHFE.ag.2512"), 3502.0);

    ctx->stra_set_position("SHFE.ag.2512", 4.0, "T2");
    feed(*ctx, "SHFE.ag.2512", 3510.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.ag.2512"), 4.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position_avgpx("SHFE.ag.2512"), 3504.5);

    ctx->stra_set_position("SHFE.ag.2512", 0.0, "T3");
    feed(*ctx, "SHFE.ag.2512", 3600.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.ag.2512"), 0.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position_profit("SHFE.ag.2512"), 0.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_fund_data(1), 3740.0);

    // 多头平仓后反手开空（覆盖剩余手数反向建仓）
    ctx->stra_set_position("SHFE.rb.2510", -1.0, "REV");
    feed(*ctx, "SHFE.rb.2510", 3200.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.rb.2510"), -1.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_cost("SHFE.rb.2510", "REV"), 3198.0);

    // 空头全平：覆盖 !_long 的盈亏取反
    ctx->stra_set_position("SHFE.au.2212", 0.0, "CLS");
    feed(*ctx, "SHFE.au.2212", 4100.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.au.2212"), 0.0);

    const std::string out = std::string(WtHelper::getOutputDir()) + "r81_cta_exit/";
    const std::string closes = r81::read_file(out + "closes.csv");
    EXPECT_NE(closes.find("SHFE.rb.2510"), std::string::npos);
    EXPECT_NE(closes.find("SHFE.au.2212"), std::string::npos);
    EXPECT_NE(r81::read_file(out + "trades.csv").find("SHFE.ag.2512"), std::string::npos);
}

// 覆盖 CtaStraBaseCtx.cpp:1332-1350 无引擎时的零值兜底
TEST_F(R81CtaTest, NullEnginePriceAccessorsReturnZero)
{
    R81CtaCtx ctx(NULL, "r81_cta_null", 0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_price("SHFE.rb.2510"), 0.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_day_price("SHFE.rb.2510", 0), 0.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_day_price("SHFE.rb.2510", 3), 0.0);
}

} // namespace
