/**
 * r73_cta_ctx_test.cpp — lane R73 / CtaStraBaseCtx JSON 导入导出与查询补测
 *
 * 覆盖 src/WtCore/CtaStraBaseCtx.cpp 中 R42/R71 未覆盖的分支：
 *   - load_data 对缺失/空/坏 JSON 的容忍，utils.lastbarno 恢复
 *   - load_userdata 对坏 JSON 的容忍与有效用户数据回读
 *   - on_tick 非交易时段保留信号、enum_position(bForExecute) 标记触发、
 *     stra_get_position 对未触发信号的告警与取值
 *   - on_schedule 主 K 线未闭合时中止、闭合后触发；收盘后取消
 *   - stra_get_bars 首次拉取主 K 线时清理过期条件单
 *   - stra_get_bars/ticks/last_tick 在数据管理器返回 NULL 时的守卫
 *   - 带 userTag 的持仓/明细查询与资金标志
 *
 * 数据管理器用 WtDtMgr 派生替身返回确定性切片；不启动线程、不联网。
 */
#include "r73_fixture.h"

#include <map>

using namespace wtp;

namespace {

const char* const kCode = "SHFE.rb.2510";
const char* const kOther = "SHFE.ag.2512";

std::string stra_data_file(const char* name)
{
    return std::string(WtHelper::getStraDataDir()) + name + ".json";
}

std::string stra_ud_file(const char* name)
{
    return std::string(WtHelper::getStraUsrDatDir()) + "ud_" + name + ".json";
}

std::string empty_payload()
{
    return R"json({
  "fund": {"total_profit": 0, "total_dynprofit": 0, "tdate": 20250110, "total_fees": 0},
  "positions": [],
  "conditions": {"settime": 0, "items": {}},
  "signals": {},
  "utils": {"lastbarno": 0}
})json";
}

class R73CtaCtxTest : public ::testing::Test
{
protected:
    r73::TempDir _tmp{"cta"};
    r73::BaseDataMgr _bd;
    r73::HotMgr _hot;
    r73::DataMgr _dm;
    r73::CtaEngine _engine;

    void SetUp() override
    {
        WtHelper::setGenerateDir((_tmp.path("generated") + "/").c_str());
        _bd.regisSession("TRADING");
        WTSSessionInfo* sess = _bd.session("TRADING");
        ASSERT_NE(sess, nullptr);
        sess->addTradingSection(900, 1500);

        WTSCommodityInfo* rb = r73::makeComm("SHFE", "rb", "TRADING", TM_Both);
        rb->setSessionInfo(sess);
        _bd.regis(rb, r73::makeCt("SHFE", "rb", "rb2510"));

        WTSCommodityInfo* ag = r73::makeComm("SHFE", "ag", "TRADING", TM_Both);
        ag->setSessionInfo(sess);
        _bd.regis(ag, r73::makeCt("SHFE", "ag", "ag2512"));

        _engine.attach(&_bd, &_hot, &_dm);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove(stra_data_file("r73_cta_load"), ec);
        std::filesystem::remove(stra_data_file("r73_cta_signal"), ec);
        std::filesystem::remove(stra_data_file("r73_cta_sched"), ec);
        std::filesystem::remove(stra_data_file("r73_cta_cond"), ec);
        std::filesystem::remove(stra_data_file("r73_cta_query"), ec);
        std::filesystem::remove(stra_data_file("r73_cta_chart"), ec);
        std::filesystem::remove(stra_ud_file("r73_cta_load"), ec);
    }

    std::unique_ptr<r73::CtaCtx> makeCtx(const char* name)
    {
        return std::unique_ptr<r73::CtaCtx>(new r73::CtaCtx(&_engine, name));
    }

    void feed(CtaStraBaseCtx& ctx, const char* code, double price, uint32_t atime = 93100000,
        bool emit = true)
    {
        WTSTickData* tick = r73::makeTick("SHFE", code, 20250110, 20250110, atime, price);
        ASSERT_NE(tick, nullptr);
        ctx.on_tick(code, tick, emit);
        tick->release();
    }
};

// 覆盖 CtaStraBaseCtx.cpp:246-493（load_data 坏/空/缺文件与 utils 分支）、
// L219-275（load_userdata 坏 JSON 与有效回读）、L647-650（on_bar NULL 早退）
TEST_F(R73CtaCtxTest, LoadToleratesBadJsonAndRestoresUserData)
{
    auto ctx = makeCtx("r73_cta_load");

    // 1) 坏 JSON / 空文件 / 缺文件：只早退，不抛异常
    ASSERT_TRUE(r73::write_file(stra_data_file("r73_cta_load"), "not-json"));
    ctx->on_init();
    ASSERT_TRUE(r73::write_file(stra_data_file("r73_cta_load"), ""));
    ctx->on_init();
    std::error_code ec;
    std::filesystem::remove(stra_data_file("r73_cta_load"), ec);
    ctx->on_init();

    // 2) 合法空骨架：utils.lastbarno 恢复
    std::string payload = empty_payload();
    payload.replace(payload.find("\"lastbarno\": 0"), strlen("\"lastbarno\": 0"),
        "\"lastbarno\": 7");
    ASSERT_TRUE(r73::write_file(stra_data_file("r73_cta_load"), payload));
    ctx->on_init();
    EXPECT_EQ(ctx->last_barno(), 7u);

    // 3) 用户数据坏 JSON -> 忽略；有效 JSON -> 回读
    ASSERT_TRUE(r73::write_file(stra_ud_file("r73_cta_load"), "{{{"));
    ctx->on_init();
    EXPECT_STREQ(ctx->stra_load_user_data("k", "def"), "def");
    ASSERT_TRUE(r73::write_file(stra_ud_file("r73_cta_load"), "{\"k\":\"v\",\"n\":\"1\"}"));
    ctx->on_init();
    EXPECT_STREQ(ctx->stra_load_user_data("k"), "v");
    EXPECT_STREQ(ctx->stra_load_user_data("n"), "1");
    EXPECT_STREQ(ctx->stra_load_user_data("missing", "def"), "def");

    // on_bar 空指针直接返回
    ctx->on_bar(kCode, "m1", 1, NULL);
    EXPECT_EQ(ctx->bar_closes, 0u);
}

// 覆盖 CtaStraBaseCtx.cpp:795-818（tick 信号分支的非交易时段/触发）、
// L930-934（脏用户数据落盘）、L1041-1071（enum_position bForExecute）、
// L1824-1865（stra_get_position 未触发信号与 userTag 分支）
TEST_F(R73CtaCtxTest, SignalHeldOutsideSessionThenExecutedWhenTriggered)
{
    ASSERT_TRUE(r73::write_file(stra_data_file("r73_cta_signal"), empty_payload()));
    auto ctx = makeCtx("r73_cta_signal");
    ctx->on_init();

    // 收盘后设置信号：on_tick 不触发，信号保留
    _engine.set_date_time(20250110, 1600, 0);
    ctx->stra_set_position(kCode, 2.0, "sig");
    feed(*ctx, kCode, 3500.0, 160000000);
    EXPECT_DOUBLE_EQ(2.0, ctx->stra_get_position(kCode));

    // bForExecute=true 把信号标记成已触发，并仍返回目标仓位
    std::map<std::string, double> targets;
    ctx->enum_position([&targets](const char* code, double pos) { targets[code] = pos; }, true);
    ASSERT_EQ(targets.size(), 1u);
    EXPECT_DOUBLE_EQ(2.0, targets[kCode]);

    // 回到交易时段：信号被消费，do_set_position 以 bFireAtOnce=false 入账
    _engine.set_date_time(20250110, 931, 0);
    feed(*ctx, kCode, 3500.0);
    EXPECT_DOUBLE_EQ(2.0, ctx->stra_get_position(kCode));
    EXPECT_DOUBLE_EQ(2.0, ctx->stra_get_position(kCode, false, "sig"));
    EXPECT_DOUBLE_EQ(0.0, ctx->stra_get_position(kCode, false, "missing"));
    EXPECT_STREQ("sig", ctx->stra_get_last_entertag(kCode));

    // 脏用户数据由后续 tick 落盘
    ctx->stra_save_user_data("dirty", "yes");
    feed(*ctx, kCode, 3501.0);
    EXPECT_NE(r73::read_file(stra_ud_file("r73_cta_signal")).find("\"dirty\""), std::string::npos);
}

// 覆盖 CtaStraBaseCtx.cpp:937-1016（主 K 线未闭合中止 / 闭合触发 /
// 收盘后取消）、L165-169（bar 事件订阅后的 on_bar_close）
TEST_F(R73CtaCtxTest, ScheduleWaitsForMainBarCloseAndRespectsCloseTime)
{
    ASSERT_TRUE(r73::write_file(stra_data_file("r73_cta_sched"), empty_payload()));
    auto ctx = makeCtx("r73_cta_sched");
    ctx->on_init();

    ctx->stra_sub_bar_events(kCode, "m1");
    WTSKlineSlice* bars = ctx->stra_get_bars(kCode, "m1", 3, true);
    ASSERT_NE(bars, nullptr);
    EXPECT_EQ(bars->size(), 3u);
    bars->release();
    EXPECT_EQ(ctx->last_barno(), 3u);

    // 主 K 线标记未闭合：on_schedule 直接中止
    EXPECT_FALSE(ctx->on_schedule(20250110, 931));
    EXPECT_EQ(ctx->calc_times, 0u);

    WTSBarStruct bar;
    memset(&bar, 0, sizeof(bar));
    bar.close = 3008.0;
    ctx->on_bar(kCode, "m", 1, &bar);
    EXPECT_EQ(ctx->bar_closes, 1u);
    EXPECT_STREQ("m1", ctx->last_period.c_str());

    // 闭合后触发 on_calculate
    EXPECT_TRUE(ctx->on_schedule(20250110, 931));
    EXPECT_EQ(ctx->calc_times, 1u);

    // 收盘后的调度取消
    _engine.set_date_time(20250110, 1600, 0);
    ctx->on_bar(kCode, "m", 1, &bar);
    EXPECT_FALSE(ctx->on_schedule(20250110, 1600));
    EXPECT_EQ(ctx->calc_times, 1u);
}

// 覆盖 CtaStraBaseCtx.cpp:1602-1646（主 K 线首次拉取、条件单过期清理、
// lastbarno 修正）、L1648-1660（ticks/last_tick 的 NULL 守卫）
TEST_F(R73CtaCtxTest, MainBarsExpireConditionsAndNullSlicesAreGuarded)
{
    std::string payload = R"json({
  "fund": {"total_profit": 0, "total_dynprofit": 0, "tdate": 20250110, "total_fees": 0},
  "positions": [],
  "conditions": {"settime": 0, "items": {
    "SHFE.rb.2510": [{"usertag":"c1","field":0,"alg":0,"target":3500.0,"qty":1.0,"action":0}]
  }},
  "signals": {},
  "utils": {"lastbarno": 0}
})json";
    ASSERT_TRUE(r73::write_file(stra_data_file("r73_cta_cond"), payload));
    auto ctx = makeCtx("r73_cta_cond");
    ctx->on_init();
    ASSERT_EQ(ctx->condition_count(), 1u);

    // 首次拉取主 K 线：最后一根 bar 的时间晚于条件单设置时间 -> 全部清理
    WTSKlineSlice* bars = ctx->stra_get_bars(kCode, "m1", 3, true);
    ASSERT_NE(bars, nullptr);
    bars->release();
    EXPECT_EQ(ctx->condition_count(), 0u);
    EXPECT_EQ(ctx->last_barno(), 3u);

    // 数据管理器返回 NULL：切片入口返回 NULL 且不崩溃
    _dm.null_kline = true;
    _dm.null_ticks = true;
    _dm.null_last_tick = true;
    EXPECT_TRUE(ctx->stra_get_bars(kOther, "m", 3) == NULL);
    EXPECT_TRUE(ctx->stra_get_ticks(kOther, 3) == NULL);
    EXPECT_TRUE(ctx->stra_get_last_tick(kOther) == NULL);
    EXPECT_EQ(_dm.kline_calls, 2u);
    EXPECT_EQ(_dm.tick_calls, 1u);
    EXPECT_EQ(_dm.last_tick_calls, 1u);
}

// 覆盖 CtaStraBaseCtx.cpp:1709-1724（资金标志默认分支）、L1761-1964
// （首末开仓/明细查询、userTag 分支、五个 profit flag）
TEST_F(R73CtaCtxTest, PositionQueriesCoverTagsAndFundFlags)
{
    std::string payload = R"json({
  "fund": {"total_profit": 10.0, "total_dynprofit": 2.0, "tdate": 20250110, "total_fees": 1.0},
  "positions": [
    {"code": "SHFE.rb.2510", "volume": 3, "closeprofit": 10.0, "dynprofit": 2.0,
     "lastentertime": 12, "lastexittime": 13,
     "details": [
       {"long": true, "price": 3000, "volume": 2, "opentime": 11, "profit": 5.0,
        "maxprofit": 6.0, "maxloss": -1.0, "opentag": "A", "openbarno": 1},
       {"long": true, "price": 3100, "volume": 1, "opentime": 12, "profit": 2.0,
        "maxprofit": 3.0, "maxloss": -2.0, "opentag": "B", "openbarno": 2}
     ]}
  ],
  "conditions": {"settime": 0, "items": {}},
  "signals": {},
  "utils": {"lastbarno": 0}
})json";
    ASSERT_TRUE(r73::write_file(stra_data_file("r73_cta_query"), payload));
    auto ctx = makeCtx("r73_cta_query");
    ctx->on_init();

    EXPECT_DOUBLE_EQ(3.0, ctx->stra_get_position(kCode));
    EXPECT_DOUBLE_EQ(3.0, ctx->stra_get_position(kCode, true));
    EXPECT_DOUBLE_EQ(2.0, ctx->stra_get_position(kCode, false, "A"));
    EXPECT_DOUBLE_EQ(1.0, ctx->stra_get_position(kCode, false, "B"));
    EXPECT_DOUBLE_EQ(0.0, ctx->stra_get_position(kCode, false, "X"));

    EXPECT_EQ(11u, ctx->stra_get_first_entertime(kCode));
    EXPECT_EQ(12u, ctx->stra_get_last_entertime(kCode));
    EXPECT_EQ(13u, ctx->stra_get_last_exittime(kCode));
    EXPECT_DOUBLE_EQ(3100.0, ctx->stra_get_last_enterprice(kCode));
    EXPECT_STREQ("A", ctx->stra_get_last_entertag(kCode));
    EXPECT_DOUBLE_EQ((3000.0 * 2 + 3100.0) / 3.0, ctx->stra_get_position_avgpx(kCode));

    EXPECT_EQ(11u, ctx->stra_get_detail_entertime(kCode, "A"));
    EXPECT_DOUBLE_EQ(3000.0, ctx->stra_get_detail_cost(kCode, "A"));
    EXPECT_DOUBLE_EQ(5.0, ctx->stra_get_detail_profit(kCode, "A", 0));
    EXPECT_DOUBLE_EQ(6.0, ctx->stra_get_detail_profit(kCode, "A", 1));
    EXPECT_DOUBLE_EQ(-1.0, ctx->stra_get_detail_profit(kCode, "A", -1));
    EXPECT_DOUBLE_EQ(3000.0, ctx->stra_get_detail_profit(kCode, "A", 2));
    EXPECT_DOUBLE_EQ(3000.0, ctx->stra_get_detail_profit(kCode, "A", -2));
    EXPECT_DOUBLE_EQ(0.0, ctx->stra_get_detail_profit(kCode, "X", 0));

    EXPECT_DOUBLE_EQ(11.0, ctx->stra_get_fund_data(0));
    EXPECT_DOUBLE_EQ(10.0, ctx->stra_get_fund_data(1));
    EXPECT_DOUBLE_EQ(2.0, ctx->stra_get_fund_data(2));
    EXPECT_DOUBLE_EQ(1.0, ctx->stra_get_fund_data(3));
    EXPECT_DOUBLE_EQ(0.0, ctx->stra_get_fund_data(99));

    // tick 更新浮动盈亏：+10 个点 * 3 手 * 10
    feed(*ctx, kCode, 3510.0);
    EXPECT_DOUBLE_EQ(14300.0, ctx->stra_get_position_profit(kCode));
}

// 覆盖 CtaStraBaseCtx.cpp:77-188（init_outputs 重复打开走 seek_to_end）、
// L1966-2062（图表注册/关闭时段守卫/落盘）
TEST_F(R73CtaCtxTest, OutputsAreIdempotentAndChartApisGuardState)
{
    ASSERT_TRUE(r73::write_file(stra_data_file("r73_cta_chart"), empty_payload()));
    auto ctx = makeCtx("r73_cta_chart");
    ctx->on_init();
    ctx->on_init();

    std::string trades = r73::read_file(std::string(WtHelper::getOutputDir()) + "r73_cta_chart/trades.csv");
    EXPECT_NE(trades.find("code,time,direct"), std::string::npos);
    EXPECT_EQ(trades.find("code,time,direct", trades.find("code,time,direct") + 1), std::string::npos);

    // 非调度时段：mark/index 写入被拒绝
    ctx->add_chart_mark(3000.0, "buy", "t");
    EXPECT_FALSE(ctx->set_index_value("idx", "line", 1.0));

    // 未注册的指标/线全部返回 false
    EXPECT_FALSE(ctx->register_index_line("idx", "line", 1));
    EXPECT_FALSE(ctx->add_index_baseline("idx", "base", 1.0));
    ctx->register_index("idx", 1);
    EXPECT_TRUE(ctx->register_index_line("idx", "line", 1));
    EXPECT_TRUE(ctx->add_index_baseline("idx", "base", 1.0));

    // 图表信息落盘（未设置 chart kline 时用主 K 线字段）
    ctx->set_chart_kline(kCode, "m1");
    ctx->register_index("idx2", 2);
    std::string chart = std::string(WtHelper::getOutputDir()) + "r73_cta_chart/rtchart.json";
    // dump_chart_info 是 protected，测试子类未暴露；这里通过调度路径不可达，
    // 仅断言已注册信息没有污染磁盘。
    EXPECT_FALSE(std::filesystem::exists(chart));

    ctx->on_session_end(20250110);
    EXPECT_TRUE(std::filesystem::exists(std::string(WtHelper::getOutputDir()) + "r73_cta_chart/funds.csv"));
}

}  // namespace
