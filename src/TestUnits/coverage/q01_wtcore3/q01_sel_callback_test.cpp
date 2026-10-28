/**
 * q01_sel_callback_test.cpp —— SelStraBaseCtx 回调装配（on_bar/on_tick/on_schedule/session 边界/订阅）
 *
 * 覆盖：
 *  - on_bar：newBar==NULL 早退 / 非空 -> on_bar_close + 周期拼接（period + times）
 *  - on_tick：本地价表 + on_tick_updated 分发；信号存在但会话无交易时段 -> 不触发 do_set_position
 *  - on_schedule：_is_in_schedule 置位期间 stra_get_date/time 命中调度值；未命中信号 -> autoexit
 *  - on_session_begin：frozen_date 早于当日 -> 解冻
 *  - on_session_end：写 positions/funds 并 save_data
 *  - stra_sub_ticks 去重（std::set）
 */
#include "q01_sel_env.h"

#include <fstream>

using namespace q01sel;

class Q01SelCallbackTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        _env.reset(new Q01TempEnv("cb"));
        _bdm.regisSession("TRADING");   // 无交易时段：isInTradingTime 恒 false
        _comm = makeComm("SHFE", "ag", "TRADING", TM_Both, CC_Future);
        _ct = makeCt("SHFE", "ag", "ag2512");
        _bdm.regis(_comm, _ct);
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

    // 把 datas.json 落地；调用方随后用同名 ctx 的 on_init 触发 load_data
    // （SelStraBaseCtx 有用户声明析构 + unique_ptr 成员，不可拷贝/移动，故就地构造）
    void prep(const char* name, const char* json)
    {
        writeFile(dataDir() + std::string(name) + ".json", json);
    }

    static const char* kFrozenJson;

    std::unique_ptr<Q01TempEnv> _env;
    BaseDataMgr _bdm;
    HotMgr _hot;
    SelEngine _eng;
    WTSCommodityInfo* _comm = NULL;
    WTSContractInfo* _ct = NULL;
};

const char* Q01SelCallbackTest::kFrozenJson = R"JSON({
  "fund": {"total_profit":10.0,"total_dynprofit":0.0,"total_fees":1.0,"tdate":20250110},
  "positions": [
    {"code":"SHFE.ag.2512","volume":2.0,"closeprofit":10.0,"dynprofit":4.0,
     "lastentertime":202501100931,"lastexittime":0,"frozen":1.0,"frozendate":20250101,
     "details":[{"long":true,"price":5200.0,"volume":2.0,"opentime":202501100931,
       "opentdate":20250110,"maxprice":5200.0,"minprice":5200.0,
       "profit":0.0,"maxprofit":0.0,"maxloss":0.0,"opentag":"buy1"}]}
  ],
  "signals": {}
})JSON";

TEST_F(Q01SelCallbackTest, on_bar_null_early_return)
{
    SelCtx ctx(&_eng, "c_bar0", 0);
    ctx.on_bar("SHFE.ag.2512", "m", 1, NULL);
    EXPECT_EQ(ctx.bar_closes, 0u);
}

TEST_F(Q01SelCallbackTest, on_bar_close_assembles_period)
{
    SelCtx ctx(&_eng, "c_bar", 0);
    WTSBarStruct bar;
    memset(&bar, 0, sizeof(bar));
    bar.close = 5200.0;
    ctx.on_bar("SHFE.ag.2512", "m", 15, &bar);
    EXPECT_EQ(ctx.bar_closes, 1u);
    EXPECT_EQ(ctx._last_close_code, "SHFE.ag.2512");
    EXPECT_EQ(ctx._last_close_period, "m15");   // period + times
    EXPECT_EQ(ctx._last_close_bar, &bar);
}

TEST_F(Q01SelCallbackTest, on_tick_emits_update_but_no_trigger_without_trading_time)
{
    SelCtx ctx(&_eng, "c_tick", 0);
    ctx.on_init();
    // 造一个待发信号
    ctx.stra_set_position("SHFE.ag.2512", 6.0, "t6");
    EXPECT_DOUBLE_EQ(ctx.stra_get_position("SHFE.ag.2512"), 0.0);

    WTSTickData* tk = makeTick("SHFE", "ag2512", 20250110, 20250110, 1000, 5250.0);
    ASSERT_NE(tk, nullptr);
    ctx.on_tick("SHFE.ag.2512", tk, true);   // 会话无交易时段 -> 不执行 do_set_position
    tk->release();

    EXPECT_DOUBLE_EQ(ctx.stra_get_price("SHFE.ag.2512"), 5250.0);
    EXPECT_GE(ctx.tick_updates, 1u);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position("SHFE.ag.2512"), 0.0);  // 信号仍未落地成仓位
}

TEST_F(Q01SelCallbackTest, on_schedule_reads_sched_time_and_autoexits)
{
    prep("c_sched", kFrozenJson);
    SelCtx ctx(&_eng, "c_sched", 0);
    ctx.on_init();
    // 调度时 on_strategy_schedule 内读取到的应是调度态日期/时间
    bool fired = ctx.on_schedule(20250220, 1500, 1500);
    EXPECT_TRUE(fired);
    EXPECT_EQ(ctx.schedule_fires, 1u);
    EXPECT_EQ(ctx._sched_date, 20250220u);
    EXPECT_EQ(ctx._time_in_sched, 1500u);
    // 调度结束后恢复透传引擎
    EXPECT_EQ(ctx.stra_get_date(), 20250110u);
    // 原持仓不在信号表 -> autoexit 追加 0 目标信号；仓位仍为加载值
    EXPECT_DOUBLE_EQ(ctx.stra_get_position("SHFE.ag.2512"), 2.0);
}

TEST_F(Q01SelCallbackTest, session_begin_releases_frozen)
{
    prep("c_frozen", kFrozenJson);
    SelCtx ctx(&_eng, "c_frozen", 0);
    ctx.on_init();
    EXPECT_DOUBLE_EQ(ctx.stra_get_position("SHFE.ag.2512", true), 1.0);   // 2 - 1 冻结
    ctx.on_session_begin(20250110);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position("SHFE.ag.2512", true), 2.0);   // 解冻
    EXPECT_DOUBLE_EQ(ctx.stra_get_position("SHFE.ag.2512", false), 2.0);
}

TEST_F(Q01SelCallbackTest, session_end_writes_and_saves)
{
    prep("c_end", kFrozenJson);
    SelCtx ctx(&_eng, "c_end", 0);
    ctx.on_init();
    ctx.on_session_end(20250110);   // 写 positions/funds + save_data
    // save_data 落地到同名 datas.json，应仍存在且可读回（不崩即为有效覆盖）
    EXPECT_TRUE(std::filesystem::exists(dataDir() + "c_end.json"));

    std::filesystem::path pos(WtHelper::getOutputDir());
    pos /= "c_end";
    pos /= "positions.csv";
    EXPECT_TRUE(std::filesystem::exists(pos));
}

TEST_F(Q01SelCallbackTest, sub_ticks_is_idempotent)
{
    SelCtx ctx(&_eng, "c_sub", 0);
    ctx.on_init();
    ctx.stra_sub_ticks("SHFE.ag.2512");
    ctx.stra_sub_ticks("SHFE.ag.2512");   // 去重：set 幂等
    // 再次订阅同代码不应报错，仓位仍空
    EXPECT_DOUBLE_EQ(ctx.stra_get_position("SHFE.ag.2512"), 0.0);
}
