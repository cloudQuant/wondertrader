/**
 * r42_sel_ctx_test.cpp  (lane R42 / WtCore 策略上下文补测)
 *
 * 覆盖目标 src/WtCore/SelStraBaseCtx.cpp：
 *   init_outputs / save_data / load_data / save_userdata / load_userdata
 *   on_init / on_tick / on_schedule / on_session_begin / on_session_end / enum_position
 *   stra_set_position（含缺失品种、不可做空、目标相同、T+1 冻结四道守卫）
 *   stra_get_* 查询族 与 stra_log_* 日志门面
 *
 * 引擎用 R42SelEngine（仅接上 bd/hot 与时钟），组合执行器为空，
 * 因此 handle_pos_change 只走空路由，不依赖任何柜台。
 */
#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <string>

#include "r42_core_fakes.h"

#include <WtCore/SelStraBaseCtx.h>

using namespace wtp;

namespace {

static const char* kSelCode = "SHFE.rb.2510";
static const char* kSelT1Code = "SHFE.ag.2512";
static const char* kSelOfCode = "SZSE.of.159915";

std::string r42_sel_read(const std::string& path)
{
    std::ifstream ifs(path.c_str());
    if (!ifs.is_open())
        return std::string();
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

bool r42_sel_write(const std::string& path, const std::string& content)
{
    std::filesystem::path p(path);
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream ofs(path.c_str(), std::ios::out | std::ios::trunc);
    if (!ofs.is_open())
        return false;
    ofs << content;
    ofs.close();
    return true;
}

class R42SelCtx : public SelStraBaseCtx
{
public:
    R42SelCtx(WtSelEngine* engine, const char* name, int32_t slippage = 0)
        : SelStraBaseCtx(engine, name, slippage) {}

    virtual ~R42SelCtx() {}

    void on_bar_close(const char* stdCode, const char* period, WTSBarStruct* newBar) override
    {
        (void)newBar;
        (void)period;
        ++bar_closes;
        last_bar_code = stdCode;
    }

    void on_tick_updated(const char* stdCode, WTSTickData* newTick) override
    {
        (void)newTick;
        ++tick_updates;
        last_tick_code = stdCode;
    }

    void on_strategy_schedule(uint32_t curDate, uint32_t curTime) override
    {
        ++schedules;
        last_sched = std::make_pair(curDate, curTime);
    }

    uint32_t bar_closes = 0;
    uint32_t tick_updates = 0;
    uint32_t schedules = 0;
    std::string last_bar_code;
    std::string last_tick_code;
    std::pair<uint32_t, uint32_t> last_sched { 0, 0 };
};

class R42SelCtxTest : public ::testing::Test
{
protected:
    r42fakes::R42GenDir    gen;
    r42fakes::R42BaseDataMgr bd;
    r42fakes::R42HotMgr    hot;
    // 注意：WtSelEngine 的 _tm_ticker 在构造时未初始化，析构里的 release() 会读它，
    // 这里故意不 delete（进程级测试），避免触发未初始化指针读。见 bugs/R42.md BUG-R42-001。
    r42fakes::R42SelEngine* engine = nullptr;
    std::unique_ptr<R42SelCtx> ctx;

    void SetUp() override
    {
        bd.regisSession("TRADING");
        WTSSessionInfo* sess = bd.session("TRADING");
        ASSERT_NE(sess, nullptr);
        sess->addTradingSection(900, 1500);

        WTSCommodityInfo* rb = r42fakes::makeComm("SHFE", "rb", "TRADING", TM_Both);
        rb->setSessionInfo(sess);
        bd.regis(rb, r42fakes::makeCt("SHFE", "rb", "rb2510"));

        WTSCommodityInfo* ag = r42fakes::makeComm("SHFE", "ag", "TRADING", TM_LongT1);
        ag->setSessionInfo(sess);
        bd.regis(ag, r42fakes::makeCt("SHFE", "ag", "ag2512"));

        WTSCommodityInfo* of = r42fakes::makeComm("SZSE", "of", "TRADING", TM_LongT1);
        of->setSessionInfo(sess);
        bd.regis(of, r42fakes::makeCt("SZSE", "of", "of159915"));

        engine = new r42fakes::R42SelEngine();
        engine->attach(&bd, &hot);
        ctx.reset(new R42SelCtx(engine, "r42_sel_ctx", 0));
    }

    std::string data_file(const char* name) const
    {
        return std::string(WtHelper::getStraDataDir()) + name + ".json";
    }

    std::string ud_file(const char* name) const
    {
        return std::string(WtHelper::getStraUsrDatDir()) + "ud_" + name + ".json";
    }

    void feed(const char* code, double price)
    {
        WTSTickData* tick = r42fakes::makeTick("SHFE", "rb2510", 20250110, 20250110, 93100000, price);
        ASSERT_NE(tick, nullptr);
        // SelStraBaseCtx::do_set_position 无条件把变化量回提交给引擎，
        // 引擎 append_signal -> get_cur_price 需要价格缓存；本替身没有 _data_mgr。
        engine->prime_price(code, price);
        ctx->on_tick(code, tick, false);
        tick->release();
    }

    // stra_set_position 只写信号，下一条 tick 才把信号落成持仓
    void apply_target(const char* code, double qty, const char* tag, double price)
    {
        ctx->stra_set_position(code, qty, tag);
        feed(code, price);
    }
};

// 覆盖 SelStraBaseCtx.cpp L59-140 init_outputs、L198-377 load_data/load_userdata、
// L1081-1204 查询族
TEST_F(R42SelCtxTest, OnInitRestoresPersistedPositionsAndSignals)
{
    const std::string json = R"json({
  "fund": {"total_profit": 12.5, "total_dynprofit": 3.5, "total_fees": 1.25, "tdate": 20250110},
  "positions": [
    {"code": "SHFE.rb.2510", "volume": 2, "closeprofit": 0.5, "dynprofit": 3.5,
     "lastentertime": 100, "lastexittime": 50, "frozen": 1, "frozendate": 20250109,
     "details": [
       {"long": true, "price": 3000, "maxprice": 3100, "minprice": 2900, "volume": 2,
        "opentime": 100, "opentdate": 20250109, "profit": 3.5, "maxprofit": 4,
        "maxloss": -1, "opentag": "A", "openbarno": 3}
     ]},
    {"code": "SHFE.xx.9999", "volume": 4, "closeprofit": 9, "dynprofit": 9,
     "lastentertime": 1, "lastexittime": 2, "details": []}
  ],
  "signals": {
    "SHFE.rb.2510": {"usertag": "sig", "volume": 6, "sigprice": 3300.0, "gentime": 99},
    "SHFE.xx.9999": {"usertag": "old", "volume": 1, "sigprice": 1.0, "gentime": 1}
  }
})json";
    ASSERT_TRUE(r42_sel_write(data_file("r42_sel_ctx"), json));
    ASSERT_TRUE(r42_sel_write(ud_file("r42_sel_ctx"), R"json({"k1":"v1"})json"));

    ctx->on_init();

    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kSelCode), 2.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kSelCode, true), 1.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.xx.9999"), 0.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position_avgpx(kSelCode), 3000.0);

    // 资金：tdate 与引擎一致 -> 手续费也被恢复；仓位汇总覆盖 profit/dynprofit
    EXPECT_DOUBLE_EQ(ctx->stra_get_fund_data(1), 9.5);
    EXPECT_DOUBLE_EQ(ctx->stra_get_fund_data(2), 3.5);
    EXPECT_DOUBLE_EQ(ctx->stra_get_fund_data(3), 1.25);

    EXPECT_EQ(ctx->stra_get_first_entertime(kSelCode), 100u);
    EXPECT_EQ(ctx->stra_get_last_entertime(kSelCode), 100u);
    EXPECT_EQ(ctx->stra_get_last_exittime(kSelCode), 50u);
    EXPECT_DOUBLE_EQ(ctx->stra_get_last_enterprice(kSelCode), 3000.0);
    EXPECT_STREQ(ctx->stra_get_last_entertag(kSelCode), "A");
    EXPECT_EQ(ctx->stra_get_detail_entertime(kSelCode, "A"), 100u);
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_cost(kSelCode, "A"), 3000.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit(kSelCode, "A", 1), 4.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit(kSelCode, "A", -1), -1.0);
    EXPECT_STREQ(ctx->stra_load_user_data("k1"), "v1");
    EXPECT_STREQ(ctx->stra_load_user_data("nope", "def"), "def");

    // 有未触发信号时 enum_position 以信号目标为准
    double target = -1;
    ctx->enum_position([&](const char* code, double qty) {
        if (std::string(code) == kSelCode)
            target = qty;
    });
    EXPECT_DOUBLE_EQ(target, 6.0);

    // 再初始化一次：走输出文件已存在分支
    ctx->on_init();
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kSelCode), 2.0);
}

// 覆盖 L379-481 save_data、L682-716 on_session_end、L734-784 stra_set_position/append_signal
TEST_F(R42SelCtxTest, SaveDataRoundTripAndSetPositionGuards)
{
    ctx->on_init();

    // 四道守卫：缺失品种 / 不可做空 + 负目标 / 目标等于当前
    ctx->stra_set_position("SHFE.none.1", 3, "x");
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.none.1"), 0.0);
    ctx->stra_set_position(kSelOfCode, -1, "x");
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kSelOfCode), 0.0);
    ctx->stra_set_position(kSelOfCode, 1, "x");
    feed(kSelOfCode, 2.0);
    ctx->stra_set_position(kSelOfCode, 1, "same");
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kSelOfCode), 1.0);

    apply_target(kSelCode, 3, "A", 3500.0);
    ASSERT_DOUBLE_EQ(ctx->stra_get_position(kSelCode), 3.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position_avgpx(kSelCode), 3500.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_last_enterprice(kSelCode), 3500.0);

    ctx->on_session_end(20250110);

    R42SelCtx reloaded(engine, "r42_sel_ctx", 0);
    reloaded.on_init();
    EXPECT_DOUBLE_EQ(reloaded.stra_get_position(kSelCode), 3.0);
    EXPECT_DOUBLE_EQ(reloaded.stra_get_position_avgpx(kSelCode), 3500.0);
    EXPECT_STREQ(reloaded.stra_get_last_entertag(kSelCode), "A");
    EXPECT_DOUBLE_EQ(reloaded.stra_get_detail_cost(kSelCode, "A"), 3500.0);

    const std::string out_dir = std::string(WtHelper::getOutputDir()) + "r42_sel_ctx/";
    EXPECT_FALSE(r42_sel_read(out_dir + "funds.csv").empty());
    EXPECT_FALSE(r42_sel_read(out_dir + "positions.csv").empty());
    EXPECT_FALSE(r42_sel_read(out_dir + "trades.csv").empty());
}

// 覆盖 L588-634 on_schedule、L659-680 enum_position
TEST_F(R42SelCtxTest, ScheduleEmitsAutoExitAndPeriodicStats)
{
    ctx->on_init();
    apply_target(kSelCode, 2, "A", 3500.0);
    ASSERT_DOUBLE_EQ(ctx->stra_get_position(kSelCode), 2.0);

    EXPECT_TRUE(ctx->on_schedule(20250110, 931, 931));
    EXPECT_EQ(ctx->schedules, 1u);
    EXPECT_EQ(ctx->last_sched.first, 20250110u);
    EXPECT_EQ(ctx->last_sched.second, 931u);

    // 持仓代码没有对应信号 -> 自动追加 autoexit 目标 0
    double target = -1;
    ctx->enum_position([&](const char* code, double qty) {
        if (std::string(code) == kSelCode)
            target = qty;
    });
    EXPECT_DOUBLE_EQ(target, 0.0);

    // 第 20 次调度触发统计日志分支
    for (int i = 0; i < 19; ++i)
        EXPECT_TRUE(ctx->on_schedule(20250110, 932 + i % 2, 932));
    EXPECT_EQ(ctx->schedules, 20u);
    EXPECT_FALSE(r42_sel_read(std::string(WtHelper::getStraDataDir()) + "r42_sel_ctx.json").empty());
}

// 覆盖 L512-586 update_dyn_profit/on_tick、L636-657 on_session_begin、L170-227 用户数据
TEST_F(R42SelCtxTest, TickAccountingAndSessionBeginReleasesFrozen)
{
    ctx->on_init();

    // T+1 品种：开仓后冻结，可用为 0
    apply_target(kSelT1Code, 3, "T1", 5000.0);
    ASSERT_DOUBLE_EQ(ctx->stra_get_position(kSelT1Code), 3.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kSelT1Code, true), 0.0);

    feed(kSelT1Code, 5010.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_price(kSelT1Code), 5010.0);

    ctx->stra_save_user_data("flag", "on");
    ctx->on_session_begin(20250111);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kSelT1Code, true), 3.0);
    EXPECT_NE(r42_sel_read(ud_file("r42_sel_ctx")).find("\"flag\""), std::string::npos);

    R42SelCtx reloaded(engine, "r42_sel_ctx", 0);
    reloaded.on_init();
    EXPECT_STREQ(reloaded.stra_load_user_data("flag"), "on");
}

// 覆盖 L722-732/L991-1027 元信息、L1046-1064 日志、L1176-1273 明细查询
TEST_F(R42SelCtxTest, MetadataLogsAndDetailQueries)
{
    ctx->on_init();
    apply_target(kSelCode, 2, "A", 3500.0);
    feed(kSelCode, 3700.0);

    // (3700-3500)*2*10 = 4000
    EXPECT_DOUBLE_EQ(ctx->stra_get_position_profit(kSelCode), 4000.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit(kSelCode, "A", 0), 4000.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit(kSelCode, "A", 2), 3700.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit(kSelCode, "A", -2), 3500.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit(kSelCode, "A", 7), 0.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_cost(kSelCode, "missing"), 0.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kSelCode, false, "A"), 2.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kSelCode, false, "none"), 0.0);
    EXPECT_EQ(ctx->stra_get_first_entertime("SHFE.zz.1"), 0u);
    EXPECT_STREQ(ctx->stra_get_last_entertag("SHFE.zz.1"), "");

    ctx->stra_log_info("r42 sel info");
    ctx->stra_log_debug("r42 sel debug");
    ctx->stra_log_warn("r42 sel warn");
    ctx->stra_log_error("r42 sel error");

    ctx->stra_sub_ticks(kSelCode);
    EXPECT_DOUBLE_EQ(ctx->stra_get_price(kSelCode), 3700.0);

    WTSCommodityInfo* comm = ctx->stra_get_comminfo(kSelCode);
    ASSERT_NE(comm, nullptr);
    EXPECT_STREQ(comm->getProduct(), "rb");
    EXPECT_NE(ctx->stra_get_sessinfo(kSelCode), nullptr);
    EXPECT_EQ(ctx->stra_get_rawcode(kSelCode), std::string());
    EXPECT_EQ(ctx->stra_get_tdate(), 20250110u);
    EXPECT_EQ(ctx->stra_get_date(), 20250110u);
    EXPECT_EQ(ctx->stra_get_time(), 931u);
    EXPECT_DOUBLE_EQ(ctx->stra_get_fund_data(9), 0.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_fund_data(0),
        ctx->stra_get_fund_data(1) - ctx->stra_get_fund_data(3) + ctx->stra_get_fund_data(2));
}

// 缺陷占位：WtSelEngine 的 _tm_ticker 构造时未初始化，析构 -> release() 会读它。
// 见 bugs/R42.md BUG-R42-001；默认不执行，避免未初始化指针读。
TEST(R42SelEngineLifecycle, DISABLED_DestroyWithoutInitReadsUninitializedTicker)
{
    r42fakes::R42SelEngine* engine = new r42fakes::R42SelEngine();
    delete engine;
}

} // namespace
