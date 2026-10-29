/**
 * r41_engine_base_test.cpp  (lane R41 / wave10)
 *
 * 覆盖 src/WtCore/WtEngine.cpp 与 src/WtCore/WtCtaEngine.cpp 中不依赖策略上下文的
 * 主体成功路径: init/load_datas/save_datas/on_tick/append_signal/do_set_position/
 * calc_fee/get_kline_slice/on_session_end/on_schedule 的空上下文分支。
 *
 * 用到的替身见 r41_wtcore_fixture.h: 基础数据、主力规则、事件监听。
 * 临时文件全部落在系统临时目录, 由 TempDir / 夹具清理。
 */
#include "r41_wtcore_fixture.h"

#include "WtCore/WtCtaEngine.h"

#include <cstring>
#include <memory>

namespace {

// 本 lane 的 generate 根目录(必须在任何用例前设置)
::testing::Environment* const g_r41_env =
    ::testing::AddGlobalTestEnvironment(new R41GenerateDirEnv);

using namespace r41;
namespace stdfs = std::filesystem;

const char* const kCode = "SHFE.rb.2310";

// 预置的 portfolio 状态: fund + 两条 position(其中一条 0 仓位) + riskmon
const char* const kDatasJson =
    "{\n"
    "  \"fund\": {\n"
    "    \"predynbal\": 120000.0, \"balance\": 123456.78, \"prebalance\": 120000.0,\n"
    "    \"profit\": 100.0, \"dynprofit\": 22.0, \"fees\": 3.0,\n"
    "    \"max_dyn_bal\": 130000.0, \"max_time\": 93000000,\n"
    "    \"min_dyn_bal\": 110000.0, \"min_time\": 93100000,\n"
    "    \"last_date\": 20231009,\n"
    "    \"maxmd\": { \"date\": 20231009, \"dyn_balance\": 130000.0 },\n"
    "    \"minmd\": { \"date\": 20231009, \"dyn_balance\": 110000.0 },\n"
    "    \"update_time\": 0\n"
    "  },\n"
    "  \"positions\": [\n"
    "    { \"code\": \"SHFE.rb.2310\", \"volume\": 2, \"closeprofit\": 5.0, \"dynprofit\": 6.0,\n"
    "      \"details\": [ { \"long\": true, \"price\": 3500.0, \"volume\": 2,\n"
    "                      \"opentime\": 202310090930, \"profit\": 6.0 },\n"
    "                    { \"long\": false, \"price\": 1.0, \"volume\": 0,\n"
    "                      \"opentime\": 202310090931, \"profit\": 0.0 } ] },\n"
    "    { \"code\": \"SHFE.rb.2401\", \"volume\": 0, \"closeprofit\": 0.0, \"dynprofit\": 9.0,\n"
    "      \"details\": [] }\n"
    "  ],\n"
    "  \"riskmon\": { \"scale\": 2.0, \"date\": 20231010 }\n"
    "}\n";

//////////////////////////////////////////////////////////////////
// WtEngine::init / init_outputs / load_fees
// src/WtCore/WtEngine.cpp:285-316, 843-877, 1187-1219
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41InitWithoutRiskMonCreatesOutputFiles)
{
    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.set_adapter_mgr(&_adapter_mgr);
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);

    // init_outputs 在 portfolio 目录下创建两个 CSV 并写入表头
    EXPECT_TRUE(stdfs::exists(portfolioFile("trades.csv")));
    EXPECT_TRUE(stdfs::exists(portfolioFile("closes.csv")));
    EXPECT_NE(std::string::npos, readPortfolio("trades.csv").find("code,time,direct"));
    EXPECT_NE(std::string::npos, readPortfolio("closes.csv").find("openprice"));
}

TEST_F(R41EngineFixture, R41InitLoadsFeeTemplatesAndToleratesMissingFile)
{
    // 存在的模板文件 -> commInfo 上的费率/保证金被写入 (src/WtCore/WtEngine.cpp:843-875)
    {
        WTSVariant* cfg = makeCfg(writeFees());
        ASSERT_NE(nullptr, cfg);

        WtCtaEngine engine;
        engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);

        WTSCommodityInfo* comm = _bd->commodity();
        ASSERT_NE(nullptr, comm);
        EXPECT_DOUBLE_EQ(0.12, comm->getMarginRate());
        // byvolume: 开仓 2.0/手 * 5 手
        EXPECT_DOUBLE_EQ(10.0, comm->calcFee(3500.0, 5, 0));
    }

    // 不存在的模板文件 -> 只记录错误, 不阻断 init (src/WtCore/WtEngine.cpp:848-852)
    {
        WTSVariant* cfg = makeCfg(_tmp.path("no_such_fees.json"));
        ASSERT_NE(nullptr, cfg);

        WtCtaEngine engine;
        engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
        EXPECT_TRUE(stdfs::exists(portfolioFile("trades.csv")));
    }
}

//////////////////////////////////////////////////////////////////
// load_datas + save_datas + riskmon 缩放
// src/WtCore/WtEngine.cpp:473-590, 367-471
// src/WtCore/WtCtaEngine.cpp:429-488
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41LoadsPersistedPortfolioAndAppliesRiskScale)
{
    writePortfolio("datas.json", kDatasJson);

    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.set_adapter_mgr(&_adapter_mgr);
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 931, 0);
    engine.set_trading_date(20231010);

    WTSPortFundInfo* fund = engine.getFundInfo();
    ASSERT_NE(nullptr, fund);
    EXPECT_DOUBLE_EQ(123456.78, fund->balance());
    EXPECT_DOUBLE_EQ(120000.0, fund->predynbalance());
    EXPECT_EQ(20231009u, fund->last_settle_date());
    // 0 仓位的持仓条目不累加浮盈, 只保留第一条的 6.0
    EXPECT_DOUBLE_EQ(6.0, fund->dynprofit());

    // 先喂一笔 tick 填 _price_map, 否则 append_signal 会转成待触发信号
    WTSTickData* tick = makeTick(kCode, 3500.0);
    engine.on_tick(kCode, tick);

    // riskmon.scale=2.0 且 date 等于当前交易日 -> diff 3 先放大成 6, 目标 2+6=8
    engine.handle_pos_change("r41_stra", kCode, 3.0);

    std::string saved = readPortfolio("datas.json");
    EXPECT_NE(std::string::npos, saved.find("\"code\": \"SHFE.rb.2310\""));
    EXPECT_NE(std::string::npos, saved.find("\"volume\": 8")) << saved;
    EXPECT_NE(std::string::npos, saved.find("\"scale\": 2.0")) << saved;

    tick->release();
}

//////////////////////////////////////////////////////////////////
// 持仓生命周期: 开 -> 反手 -> 归零
// src/WtCore/WtEngine.cpp:956-1084, 1221-1237
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41PositionLifecycleOpenReverseAndFlatten)
{
    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.set_adapter_mgr(&_adapter_mgr);
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 931, 0);
    engine.set_trading_date(20231010);

    WTSTickData* tick = makeTick(kCode, 3500.0);
    engine.on_tick(kCode, tick);

    // 0 -> 5: 同向加仓
    engine.handle_pos_change("r41_stra", kCode, 5.0);
    EXPECT_NE(std::string::npos, readPortfolio("trades.csv").find("OPEN"));

    // 5 -> -3: 先平多 5, 再反手开空 3
    engine.handle_pos_change("r41_stra", kCode, -8.0);
    // closes.csv 的列是 code,direct,opentime,...（WtEngine.cpp:1230-1236），没有字面量 "CLOSE"；
    // 这里用数据行的 "SHFE.rb.2310,LONG," 证明平仓记录已落盘。
    EXPECT_NE(std::string::npos, readPortfolio("closes.csv").find("SHFE.rb.2310,LONG,"));
    EXPECT_NE(std::string::npos, readPortfolio("trades.csv").find("CLOSE"));

    // -3 -> 0: 平掉剩余空头
    engine.handle_pos_change("r41_stra", kCode, 3.0);

    std::string saved = readPortfolio("datas.json");
    EXPECT_NE(std::string::npos, saved.find("\"volume\": 0")) << saved;

    tick->release();
}

//////////////////////////////////////////////////////////////////
// 待触发信号: 没有现价时挂起, 下一笔 tick 在交易时段内触发
// src/WtCore/WtEngine.cpp:120-147, 918-954
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41StandbySignalFiresOnNextTick)
{
    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.set_adapter_mgr(&_adapter_mgr);
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 930, 0);
    engine.set_trading_date(20231010);

    // 没有 tick -> get_cur_price 返回 0 -> 只登记信号
    engine.handle_pos_change("r41_stra", kCode, 5.0);
    EXPECT_EQ(std::string::npos, readPortfolio("trades.csv").find("OPEN"));

    WTSTickData* tick = makeTick(kCode, 3507.0);
    engine.on_tick(kCode, tick);

    // 信号在 tick 价格上成交, 并写入 trades.csv
    EXPECT_NE(std::string::npos, readPortfolio("trades.csv").find("OPEN"));
    EXPECT_DOUBLE_EQ(3507.0, engine.get_cur_price(kCode));

    tick->release();
}

//////////////////////////////////////////////////////////////////
// 行情/价格/周期切片访问面
// src/WtCore/WtEngine.cpp:104-115, 239-245, 592-777
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41PriceSessionAndKlineAccessors)
{
    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);

    WTSTickData* tick = makeTick(kCode, 10.0);
    engine.on_tick(kCode, tick);

    // 未复权 / 前复权 / 后复权
    EXPECT_DOUBLE_EQ(10.0, engine.get_cur_price(kCode));
    EXPECT_DOUBLE_EQ(10.0, engine.get_cur_price("SHFE.rb.2310-"));
    EXPECT_DOUBLE_EQ(10.0, engine.get_cur_price("SHFE.rb.2310+"));   // 因子=1.0
    EXPECT_DOUBLE_EQ(0.0, engine.get_cur_price("SHFE.rb.9999"));

    EXPECT_DOUBLE_EQ(9.0, engine.get_day_price(kCode, 0));
    EXPECT_DOUBLE_EQ(11.0, engine.get_day_price(kCode, 1));
    EXPECT_DOUBLE_EQ(8.0, engine.get_day_price(kCode, 2));
    EXPECT_DOUBLE_EQ(10.0, engine.get_day_price(kCode, 3));
    EXPECT_DOUBLE_EQ(0.0, engine.get_day_price(kCode, 4));
    EXPECT_DOUBLE_EQ(0.0, engine.get_day_price("SHFE.rb.9999", 3));
    // 前复权去掉 '-' 后读同一份缓存; 后复权按因子 1.0 还原
    EXPECT_DOUBLE_EQ(10.0, engine.get_day_price("SHFE.rb.2310-", 3));
    EXPECT_DOUBLE_EQ(10.0, engine.get_day_price("SHFE.rb.2310+", 3));

    WTSTickData* last = engine.get_last_tick(1, kCode);
    ASSERT_NE(nullptr, last);
    EXPECT_DOUBLE_EQ(10.0, last->price());
    last->release();

    // reader 未初始化 -> slice 一律 NULL, 但四条周期分支都要走到
    EXPECT_EQ(nullptr, engine.get_tick_slice(1, kCode, 10));
    EXPECT_EQ(nullptr, engine.get_kline_slice(1, kCode, "m", 10, 5));
    EXPECT_EQ(nullptr, engine.get_kline_slice(1, kCode, "m", 10, 7));
    EXPECT_EQ(nullptr, engine.get_kline_slice(1, kCode, "h", 10));
    EXPECT_EQ(nullptr, engine.get_kline_slice(1, kCode, "f", 10));
    EXPECT_EQ(nullptr, engine.get_kline_slice(1, kCode, "d", 10));

    EXPECT_EQ(0u, engine.get_adjusting_flag());
    EXPECT_EQ(std::string(""), engine.get_rawcode(kCode));
    EXPECT_EQ(std::string("SHFE.rb.2310"), engine.get_rawcode("SHFE.rb.R41"));
    // 自定义规则因子: 品种非股票, 规则标签命中 -> 直接乘因子
    EXPECT_DOUBLE_EQ(2.0, engine.get_exright_factor("SHFE.rb.R41"));

    EXPECT_EQ(_bd->session(), engine.get_session_info("TRADING"));
    EXPECT_EQ(_bd->session(), engine.get_session_info(kCode, true));
    EXPECT_EQ(nullptr, engine.get_session_info("MISSING"));
    EXPECT_EQ(_bd->commodity(), engine.get_commodity_info(kCode));
    EXPECT_EQ(_bd->contract(), engine.get_contract_info(kCode));

    // 带规则标签的订阅: sub_tick 第一分支 + getCustomRawCode
    engine.sub_tick(1, "SHFE.rb.R41-");

    // 带规则标签的行情 + 后复权: 价格乘以规则因子 2.0
    WTSTickData* ruleTick = makeTick("SHFE.rb.R41", 10.0);
    engine.on_tick("SHFE.rb.R41", ruleTick);
    EXPECT_DOUBLE_EQ(20.0, engine.get_cur_price("SHFE.rb.R41+"));
    ruleTick->release();

    engine.writeRiskLog("r41 risk message");

    // handle_push_quote: 原始代码 + 热合约映射各推送一遍
    // 注意 WtCtaEngine 重写了该虚函数（WtCtaEngine.cpp:423-427），没有 _tm_ticker 时
    // 派生实现直接返回；本用例要验证的是 WtEngine 基类的热合约扇出，因此显式限定基类。
    engine.WtEngine::handle_push_quote(tick);
    EXPECT_DOUBLE_EQ(10.0, engine.get_cur_price("SHFE.rb.HOT"));

    tick->release();
}

//////////////////////////////////////////////////////////////////
// on_session_begin/end 与资金结算
// src/WtCore/WtEngine.cpp:318-360
// src/WtCore/WtCtaEngine.cpp:219-247
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41SessionBeginEndSettlesFundAndNotifiesListener)
{
    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.set_adapter_mgr(&_adapter_mgr);
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);

    EvtListenerFake listener;
    engine.regEventListener(&listener);

    engine.set_date_time(20231010, 1450, 0);
    engine.set_trading_date(20231010);

    engine.on_session_begin();
    EXPECT_EQ(1u, listener._session);
    EXPECT_TRUE(listener._last_session_begin);
    EXPECT_EQ(20231010u, listener._last_session_date);

    WTSPortFundInfo* fund = engine.getFundInfo();
    ASSERT_NE(nullptr, fund);

    engine.on_session_end();
    EXPECT_EQ(2u, listener._session);
    EXPECT_FALSE(listener._last_session_begin);

    std::string funds = readPortfolio("funds.csv");
    EXPECT_NE(std::string::npos, funds.find("date,predynbalance"));
    EXPECT_NE(std::string::npos, funds.find("20231010")) << funds;
    EXPECT_EQ(20231010u, fund->last_settle_date());
}

//////////////////////////////////////////////////////////////////
// riskmon 配置分支: active=false / 模块加载失败
// src/WtCore/WtEngine.cpp:304-315, 1143-1162
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41RiskMonConfigBranchesDoNotBreakInit)
{
    {
        WTSVariant* cfg = makeCfg("", 0, ",\n  \"riskmon\": { \"active\": false }");
        ASSERT_NE(nullptr, cfg);

        WtCtaEngine engine;
        engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
        EXPECT_TRUE(stdfs::exists(portfolioFile("trades.csv")));
    }
    {
        WTSVariant* cfg = makeCfg("", 0,
                                  ",\n  \"riskmon\": { \"active\": true,"
                                  " \"module\": \"r41_missing_riskmon\", \"name\": \"r41\" }");
        ASSERT_NE(nullptr, cfg);

        WtCtaEngine engine;
        engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
        EXPECT_TRUE(stdfs::exists(portfolioFile("trades.csv")));
    }
}

//////////////////////////////////////////////////////////////////
// calc_fee: 按已加载的模板计算 by-volume 开仓费 + 无模板时返回 0
// src/WtCore/WtEngine.cpp:843-916
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41CalcFeeUsesLoadedTemplateAndDefaultsWithoutOne)
{
    {
        WTSVariant* cfg = makeCfg(writeFees());
        ASSERT_NE(nullptr, cfg);

        WtCtaEngine engine;
        engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);

        // 配置的 open=2.0、byvolume=true，5 手的引擎手续费应为 10.0。
        EXPECT_DOUBLE_EQ(10.0, _bd->commodity()->calcFee(3500.0, 5, 0));
        EXPECT_DOUBLE_EQ(10.0, engine.calc_fee(kCode, 3500.0, 5, 0));
    }

    {
        WTSVariant* cfg = makeCfg();
        ASSERT_NE(nullptr, cfg);

        WtCtaEngine engine;
        engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
        EXPECT_DOUBLE_EQ(0.0, engine.calc_fee(kCode, 3500.0, 5, 0));
    }
}

//////////////////////////////////////////////////////////////////
// on_schedule: 无线程池上下文/有线程池的空上下文分支 + 组合仓位归零
// src/WtCore/WtCtaEngine.cpp:249-420
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41OnScheduleWithPoolCommitsLeftoverPositionToZero)
{
    WTSVariant* cfg = makeCfg("", 2);
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.set_adapter_mgr(&_adapter_mgr);
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 931, 0);
    engine.set_trading_date(20231010);

    EvtListenerFake listener;
    engine.regEventListener(&listener);

    WTSTickData* tick = makeTick(kCode, 3500.0);
    engine.on_tick(kCode, tick);
    engine.handle_pos_change("r41_stra", kCode, 4.0);

    // 组合里有持仓但没有任何策略目标 -> 自动 append_signal(0) 并给执行器缓存 0
    engine.on_schedule(20231010, 931);

    EXPECT_EQ(1u, listener._sched);
    EXPECT_EQ(20231010u, listener._last_sched_date);
    EXPECT_EQ(931u, listener._last_sched_time);
    EXPECT_TRUE(stdfs::exists(portfolioFile("datas.json")));

    tick->release();
}

}  // namespace
