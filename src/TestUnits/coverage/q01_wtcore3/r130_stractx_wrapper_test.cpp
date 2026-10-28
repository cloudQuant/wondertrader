/**
 * r130_stractx_wrapper_test.cpp — lane R130 / wave21
 *
 * 目标：src/WtCore/CtaStraContext.cpp（37/40 未覆盖）与
 *       src/WtCore/SelStraContext.cpp（36/36 未覆盖）。
 * 这两个类是策略插件与策略上下文之间的转发壳，生产路径只由 dlopen 的
 * 策略工厂创建，此前没有任何测试直接实例化。
 *
 * 覆盖行段：
 *   Cta : ctor/dtor :13-27；on_bar_close :37-41；on_init :50-58（含 dump_chart_info）；
 *         on_session_begin :67-71；on_session_end :80-86；on_tick_updated :93-102；
 *         on_calculate :110-114；on_condition_triggered :123-127；
 *         以及头文件内联 set_strategy/get_stragety
 *   Sel : ctor/dtor :6-17；on_init :20-25；on_session_begin :28-33；
 *         on_session_end :36-41；on_bar_close :44-47；on_tick_updated :50-58；
 *         on_strategy_schedule :61-64
 *
 * 只传裸引擎（不 init/不 run），不启动任何线程；落盘目录由 r130::TempEnv 指向
 * temp_directory_path()/q01sel_r130ctx_<pid>/ 并在析构时整棵删除。
 */
#include <gtest/gtest.h>

#include <string>

#include "q01_sel_env.h"
#include "r130_env.h"
#include "../p01_wtcore_stractx/p01_cta_env.h"

#include <Includes/CtaStrategyDefs.h>
#include <Includes/SelStrategyDefs.h>
#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>
#include <WtCore/CtaStraContext.h>
#include <WtCore/SelStraContext.h>

using namespace wtp;

namespace {

//////////////////////////////////////////////////////////////////
// 策略替身：接口除 getName/getFactName 外都有默认实现
//////////////////////////////////////////////////////////////////
class R130CtaStra : public CtaStrategy
{
public:
    R130CtaStra() : CtaStrategy("r130cta_stra") {}

    const char* getName() override { return "r130cta_stra"; }
    const char* getFactName() override { return "r130fact"; }

    void on_init(ICtaStraCtx*) override { ++inits; }
    void on_session_begin(ICtaStraCtx*, uint32_t) override { ++begins; }
    void on_session_end(ICtaStraCtx*, uint32_t) override { ++ends; }
    void on_schedule(ICtaStraCtx*, uint32_t, uint32_t) override { ++scheds; }
    void on_tick(ICtaStraCtx*, const char*, WTSTickData*) override { ++ticks; }
    void on_bar(ICtaStraCtx*, const char*, const char*, WTSBarStruct*) override { ++bars; }
    void on_condition_triggered(ICtaStraCtx*, const char*, double, double, const char*) override
    { ++conds; }

    uint32_t inits = 0, begins = 0, ends = 0, scheds = 0, ticks = 0, bars = 0, conds = 0;
};

class R130SelStra : public SelStrategy
{
public:
    R130SelStra() : SelStrategy("r130sel_stra") {}

    const char* getName() override { return "r130sel_stra"; }
    const char* getFactName() override { return "r130fact"; }

    void on_init(ISelStraCtx*) override { ++inits; }
    void on_session_begin(ISelStraCtx*, uint32_t) override { ++begins; }
    void on_session_end(ISelStraCtx*, uint32_t) override { ++ends; }
    void on_schedule(ISelStraCtx*, uint32_t, uint32_t) override { ++scheds; }
    void on_tick(ISelStraCtx*, const char*, WTSTickData*) override { ++ticks; }
    void on_bar(ISelStraCtx*, const char*, const char*, WTSBarStruct*) override { ++bars; }

    uint32_t inits = 0, begins = 0, ends = 0, scheds = 0, ticks = 0, bars = 0;
};

const char* const kCode = "SHFE.ag.2512";

}  // namespace

//////////////////////////////////////////////////////////////////
// CtaStraContext 转发链
// src/WtCore/CtaStraContext.cpp:13-27, 37-41, 50-58, 67-71, 80-86, 93-102, 110-114, 123-127
//////////////////////////////////////////////////////////////////
TEST(R130CtxWrapper, CtaForwardsEveryCallbackToStrategy)
{
    r130::TempEnv env("r130ctx");

    q01sel::BaseDataMgr bd;
    bd.regisSessionWithTime("TRADING", 0, 2359);
    WTSSessionInfo* sess = bd.session("TRADING");
    ASSERT_NE(nullptr, sess);
    WTSCommodityInfo* ag = q01sel::makeComm("SHFE", "ag", "TRADING", TM_Both);
    ag->setSessionInfo(sess);
    bd.regis(ag, q01sel::makeCt("SHFE", "ag", "ag2512"));

    q01sel::HotMgr hot;
    P01CtaEngine engine;
    engine.attach(&bd, &hot);
    // WtCtaEngine::_cfg 只能由 init 赋值，而析构会 _cfg->release()；
    // 原夹具只 attach，析构时读未初始化指针，用例结束即 SIGSEGV。
    WTSVariant* cfg = WTSCfgLoader::load_from_content(
        "{\"product\":{\"session\":\"TRADING\"},\"fees\":\"\",\"filters\":\"\"}");
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &bd, NULL, &hot, NULL);
    cfg->release();

    CtaStraContext ctx(&engine, "r130cta_ctx", 0);
    R130CtaStra stra;
    ctx.set_strategy(&stra);
    EXPECT_EQ(&stra, ctx.get_stragety());

    // on_init：基类建输出文件 + 策略回调 + dump_chart_info
    ctx.on_init();
    EXPECT_EQ(1u, stra.inits);

    ctx.on_session_begin(20250110);
    EXPECT_EQ(1u, stra.begins);

    WTSTickData* tick = q01sel::makeTick("SHFE", "ag2512", 20250110, 20250110, 93100000, 3500.0);
    tick->setContractInfo(bd.getContract("ag2512", "SHFE"));

    // 未订阅：本地 _tick_subs 命中失败，策略不收到 tick
    ctx.on_tick_updated(kCode, tick);
    EXPECT_EQ(0u, stra.ticks);

    // 订阅后转发
    ctx.stra_sub_ticks(kCode);
    ctx.on_tick_updated(kCode, tick);
    EXPECT_EQ(1u, stra.ticks);
    tick->release();

    ctx.on_bar_close(kCode, "min1", NULL);
    EXPECT_EQ(1u, stra.bars);

    ctx.on_calculate(20250110, 931);
    EXPECT_EQ(1u, stra.scheds);

    ctx.on_condition_triggered(kCode, 1.0, 3500.0, "r130tag");
    EXPECT_EQ(1u, stra.conds);

    ctx.on_session_end(20250110);
    EXPECT_EQ(1u, stra.ends);

    // 空策略：转发壳全部短路，基类生命周期仍可执行
    ctx.set_strategy(NULL);
    EXPECT_EQ(NULL, ctx.get_stragety());
    ctx.on_bar_close(kCode, "min1", NULL);
    ctx.on_calculate(20250110, 932);
    ctx.on_condition_triggered(kCode, 2.0, 3501.0, "r130tag2");
    EXPECT_EQ(1u, stra.bars);
    EXPECT_EQ(1u, stra.scheds);
    EXPECT_EQ(1u, stra.conds);

    ctx.on_init();
    EXPECT_EQ(1u, stra.inits);
}

//////////////////////////////////////////////////////////////////
// SelStraContext 转发链
// src/WtCore/SelStraContext.cpp:6-17, 20-25, 28-33, 36-41, 44-47, 50-58, 61-64
//////////////////////////////////////////////////////////////////
TEST(R130CtxWrapper, SelForwardsEveryCallbackToStrategy)
{
    r130::TempEnv env("r130ctx");

    q01sel::BaseDataMgr bd;
    bd.regisSessionWithTime("TRADING", 0, 2359);
    WTSSessionInfo* sess = bd.session("TRADING");
    ASSERT_NE(nullptr, sess);
    WTSCommodityInfo* ag = q01sel::makeComm("SHFE", "ag", "TRADING", TM_Both);
    ag->setSessionInfo(sess);
    bd.regis(ag, q01sel::makeCt("SHFE", "ag", "ag2512"));

    q01sel::HotMgr hot;
    // WtSelEngine 构造未初始化 _tm_ticker（BUG-R42-001），堆分配且不回收以规避析构
    q01sel::SelEngine& engine = *new q01sel::SelEngine();
    engine.attach(&bd, &hot);

    SelStraContext ctx(&engine, "r130sel_ctx", 0);
    R130SelStra stra;
    ctx.set_strategy(&stra);
    EXPECT_EQ(&stra, ctx.get_stragety());

    ctx.on_init();
    EXPECT_EQ(1u, stra.inits);

    ctx.on_session_begin(20250110);
    EXPECT_EQ(1u, stra.begins);

    WTSTickData* tick = q01sel::makeTick("SHFE", "ag2512", 20250110, 20250110, 93100000, 3500.0);
    tick->setContractInfo(bd.getContract("ag2512", "SHFE"));

    ctx.on_tick_updated(kCode, tick);
    EXPECT_EQ(0u, stra.ticks);

    ctx.stra_sub_ticks(kCode);
    ctx.on_tick_updated(kCode, tick);
    EXPECT_EQ(1u, stra.ticks);
    tick->release();

    ctx.on_bar_close(kCode, "min1", NULL);
    EXPECT_EQ(1u, stra.bars);

    ctx.on_strategy_schedule(20250110, 931);
    EXPECT_EQ(1u, stra.scheds);

    ctx.on_session_end(20250110);
    EXPECT_EQ(1u, stra.ends);

    ctx.set_strategy(NULL);
    EXPECT_EQ(NULL, ctx.get_stragety());
    ctx.on_bar_close(kCode, "min1", NULL);
    ctx.on_strategy_schedule(20250110, 932);
    EXPECT_EQ(1u, stra.bars);
    EXPECT_EQ(1u, stra.scheds);

    ctx.on_init();
    EXPECT_EQ(1u, stra.inits);
}
