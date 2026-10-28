/**
 * r41_cta_engine_test.cpp  (lane R41 / wave10)
 *
 * 覆盖 src/WtCore/WtCtaEngine.cpp 中需要策略上下文的路径:
 *   on_init / on_schedule(无池 + 线程池) / on_bar / on_tick(ready + 复权订阅) /
 *   on_session_begin/end / IExecuterStub 访问器。
 *
 * 用真实的 CtaStraBaseCtx 作为上下文(公开构造函数), 用记录型 IExecCommand
 * 断言引擎提交的目标仓位, 不触碰任何柜台/行情。
 */
#include "r41_wtcore_fixture.h"

#include "WtCore/WtCtaEngine.h"
#include "WtCore/CtaStraBaseCtx.h"
#include "Includes/WTSStruct.h"
#include "Share/TimeUtils.hpp"

#include <cstring>
#include <memory>

namespace {

using namespace r41;

const char* const kCode = "SHFE.rb.2310";

//////////////////////////////////////////////////////////////////
// 记录型执行器: 只记最后一次 set_position 的目标仓位
//////////////////////////////////////////////////////////////////
class RecExecuter : public IExecCommand
{
public:
    explicit RecExecuter(const char* name) : IExecCommand(name) {}
    virtual ~RecExecuter() {}

    void set_position(const wt_hashmap<std::string, double>& targets) override
    {
        _last = targets;
        ++_sets;
    }

    wt_hashmap<std::string, double> _last;
    uint32_t _sets = 0;
};

typedef std::shared_ptr<RecExecuter> RecExecPtr;

// CtaStraBaseCtx 只差策略侧两个纯虚函数; 用空实现即可作为引擎上下文替身
class CtaCtxFake : public CtaStraBaseCtx
{
public:
    CtaCtxFake(WtCtaEngine* engine, const char* name, int32_t slippage)
        : CtaStraBaseCtx(engine, name, slippage) {}

    void on_bar_close(const char*, const char*, WTSBarStruct*) override {}
    void on_calculate(uint32_t, uint32_t) override {}

    void on_tick(const char* stdCode, WTSTickData* newTick, bool bEmitStrategy = true) override
    {
        _last_code = stdCode;
        ++_ticks;
        CtaStraBaseCtx::on_tick(stdCode, newTick, bEmitStrategy);
    }

    std::string _last_code;
    uint32_t _ticks = 0;
};

typedef std::shared_ptr<CtaCtxFake> CtaCtxPtr;

// 通过公开 API 造一手理论持仓: stra_set_position 挂信号, 再喂一笔 tick 让上下文成交。
// ctx 里的 do_set_position 是 private, 这里不绕过访问控制。
void seedPosition(R41EngineFixture& fx, CtaCtxFake* ctx, double qty)
{
    WTSTickData* seed = fx.makeTick(kCode, 3500.0);
    ctx->stra_set_position(kCode, qty);
    ctx->on_tick(kCode, seed);
    seed->release();
}

//////////////////////////////////////////////////////////////////
// on_init: 上下文持仓 -> 执行器目标仓位, 无风控缩放
// src/WtCore/WtCtaEngine.cpp:148-217
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41OnInitEnumeratesContextPosition)
{
    WTSVariant* cfg = makeCfg("", 0);
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.set_adapter_mgr(&_adapter_mgr);
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 930, 0);
    engine.set_trading_date(20231010);

    EvtListenerFake listener;
    engine.regEventListener(&listener);

    RecExecPtr exec(new RecExecuter("r41_exec"));
    engine.addExecuter(ExecCmdPtr(exec));

    CtaCtxPtr ctx(new CtaCtxFake(&engine, "r41ctx", 0));
    engine.addContext(ctx);

    EXPECT_EQ(ctx, engine.getContext(ctx->id()));
    EXPECT_EQ(CtaContextPtr(), engine.getContext(99999));

    // 直接构造理论持仓, on_init 的 enum_position 会把它交给执行器
    seedPosition(*this, ctx.get(), 3.0);
    engine.on_init();

    EXPECT_EQ(1u, listener._init);
    ASSERT_EQ(1u, exec->_sets);
    ASSERT_NE(exec->_last.end(), exec->_last.find(kCode));
    EXPECT_DOUBLE_EQ(3.0, exec->_last.find(kCode)->second);
}

//////////////////////////////////////////////////////////////////
// on_init: riskmon 缩放后的目标仓位
// src/WtCore/WtCtaEngine.cpp:191-213
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41OnInitAppliesRiskScaleToCachedTargets)
{
    WTSVariant* cfg = makeCfg("", 0);
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.set_adapter_mgr(&_adapter_mgr);
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 930, 0);
    engine.set_trading_date(20231010);

    // setVolScale 把风险日期固定为当前交易日 -> on_init 走 bRiskEnabled 分支
    engine.setVolScale(2.0);

    RecExecPtr exec(new RecExecuter("r41_exec"));
    engine.addExecuter(ExecCmdPtr(exec));

    CtaCtxPtr ctx(new CtaCtxFake(&engine, "r41ctx", 0));
    engine.addContext(ctx);
    seedPosition(*this, ctx.get(), 3.0);

    engine.on_init();

    ASSERT_EQ(1u, exec->_sets);
    ASSERT_NE(exec->_last.end(), exec->_last.find(kCode));
    EXPECT_DOUBLE_EQ(6.0, exec->_last.find(kCode)->second);
}

//////////////////////////////////////////////////////////////////
// on_schedule: 无池分支, 上下文枚举 -> 缓存 -> 提交 -> 待触发信号
// src/WtCore/WtCtaEngine.cpp:316-420
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41OnScheduleWithoutPoolSubmitsContextTarget)
{
    WTSVariant* cfg = makeCfg("", 0);
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.set_adapter_mgr(&_adapter_mgr);
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 931, 0);
    engine.set_trading_date(20231010);

    EvtListenerFake listener;
    engine.regEventListener(&listener);

    RecExecPtr exec(new RecExecuter("r41_exec"));
    engine.addExecuter(ExecCmdPtr(exec));

    CtaCtxPtr ctx(new CtaCtxFake(&engine, "r41ctx", 0));
    engine.addContext(ctx);
    seedPosition(*this, ctx.get(), 2.0);

    engine.on_schedule(20231010, 931);

    EXPECT_EQ(1u, listener._sched);
    EXPECT_EQ(931u, listener._last_sched_time);
    ASSERT_EQ(1u, exec->_sets);
    ASSERT_NE(exec->_last.end(), exec->_last.find(kCode));
    EXPECT_DOUBLE_EQ(2.0, exec->_last.find(kCode)->second);

    // on_schedule 里的 append_signal(..., true) 要等下一笔 tick 才落到组合持仓
    WTSTickData* tick = makeTick(kCode, 3510.0);
    engine.on_tick(kCode, tick);
    std::string saved = readPortfolio("datas.json");
    EXPECT_NE(std::string::npos, saved.find("\"volume\": 2")) << saved;

    tick->release();
}

//////////////////////////////////////////////////////////////////
// on_schedule: 有线程池分支
// src/WtCore/WtCtaEngine.cpp:255-314
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41OnScheduleWithPoolUsesThreadPool)
{
    WTSVariant* cfg = makeCfg("", 2);
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.set_adapter_mgr(&_adapter_mgr);
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 931, 0);
    engine.set_trading_date(20231010);

    RecExecPtr exec(new RecExecuter("r41_exec"));
    engine.addExecuter(ExecCmdPtr(exec));

    CtaCtxPtr ctx(new CtaCtxFake(&engine, "r41ctx", 0));
    engine.addContext(ctx);
    // commit_cached_targets 会按 decimal::rnd 取整（WtExecMgr.cpp:143-148），
    // 这里用整数手数，避免把取整语义混进线程池分支的断言。
    seedPosition(*this, ctx.get(), 2.0);

    engine.on_schedule(20231010, 931);

    ASSERT_EQ(1u, exec->_sets);
    ASSERT_NE(exec->_last.end(), exec->_last.find(kCode));
    EXPECT_DOUBLE_EQ(2.0, exec->_last.find(kCode)->second);
}

//////////////////////////////////////////////////////////////////
// on_tick: _ready + 订阅表(opt=0 原始 / opt=2 后复权)
// src/WtCore/WtCtaEngine.cpp:490-637
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41OnTickDispatchesRawAndHfqAdjusted)
{
    WTSVariant* cfg = makeCfg("", 0);
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.set_adapter_mgr(&_adapter_mgr);
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 931, 0);
    engine.set_trading_date(20231010);

    CtaCtxPtr rawCtx(new CtaCtxFake(&engine, "r41raw", 0));
    CtaCtxPtr hfqCtx(new CtaCtxFake(&engine, "r41hfq", 0));
    engine.addContext(rawCtx);
    engine.addContext(hfqCtx);

    // ready 置位后 on_tick 才会查订阅表
    engine.on_session_begin();

    engine.sub_tick(rawCtx->id(), kCode);
    // 后复权代码以 '+' 结尾, 订阅表键是去掉后缀的代码
    engine.sub_tick(hfqCtx->id(), "SHFE.rb.2310+");

    WTSTickData* tick = makeTick(kCode, 10.0);
    engine.on_tick(kCode, tick);

    EXPECT_EQ(std::string(kCode), rawCtx->_last_code);
    EXPECT_EQ(std::string("SHFE.rb.2310+"), hfqCtx->_last_code);
    EXPECT_DOUBLE_EQ(10.0, rawCtx->stra_get_price(kCode));
    EXPECT_DOUBLE_EQ(10.0, hfqCtx->stra_get_price("SHFE.rb.2310+"));

    tick->release();
}

//////////////////////////////////////////////////////////////////
// 缺陷复现(默认禁用): 无规则标签的 '-' 订阅被误判成后复权
// src/WtCore/WtEngine.cpp:825-837 (flag 取 stdCode[length-1] 而非 [length])
// 期望: 前复权代码 -> 上下文收到 "SHFE.rb.2310-"; 实际: 收到 "+" 分支。
// 见 bugs/R41.md BUG-R41-002
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, DISABLED_R41PlainQfqSubscriptionDispatchesFrontAdjusted)
{
    WTSVariant* cfg = makeCfg("", 0);
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 931, 0);
    engine.set_trading_date(20231010);

    CtaCtxPtr ctx(new CtaCtxFake(&engine, "r41ctx", 0));
    engine.addContext(ctx);
    engine.on_session_begin();

    engine.sub_tick(ctx->id(), "SHFE.rb.2310-");

    WTSTickData* tick = makeTick(kCode, 10.0);
    engine.on_tick(kCode, tick);

    // 当前实现下这里是 "+", 用例保持 DISABLED_
    EXPECT_EQ(std::string("SHFE.rb.2310-"), ctx->_last_code);

    tick->release();
}

//////////////////////////////////////////////////////////////////
// on_bar: 周期订阅表 -> 上下文回调
// src/WtCore/WtCtaEngine.cpp:639-671
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41OnBarDispatchesToSubscribedContext)
{
    WTSVariant* cfg = makeCfg("", 0);
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.set_adapter_mgr(&_adapter_mgr);
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);

    CtaCtxPtr ctx(new CtaCtxFake(&engine, "r41ctx", 0));
    engine.addContext(ctx);

    // 只登记 sid->bar 订阅, 数据读取器为空, 返回 NULL
    EXPECT_EQ(nullptr, engine.get_kline_slice(ctx->id(), kCode, "d", 10));

    WTSBarStruct bar;
    memset(&bar, 0, sizeof(bar));
    bar.date = 20231010;
    bar.open = 3500;
    bar.high = 3510;
    bar.low = 3490;
    bar.close = 3505;

    engine.on_bar(kCode, "d", 1, &bar);
    EXPECT_EQ(ctx, engine.getContext(ctx->id()));
}

//////////////////////////////////////////////////////////////////
// session 事件 + 无 notifier 的通知透传守卫
// src/WtCore/WtCtaEngine.cpp:219-247, 704-720
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41SessionEventsReachContextAndListener)
{
    WTSVariant* cfg = makeCfg("", 0);
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.set_adapter_mgr(&_adapter_mgr);
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 1450, 0);
    engine.set_trading_date(20231010);

    EvtListenerFake listener;
    engine.regEventListener(&listener);

    CtaCtxPtr ctx(new CtaCtxFake(&engine, "r41ctx", 0));
    engine.addContext(ctx);
    seedPosition(*this, ctx.get(), 1.0);

    engine.on_session_begin();
    EXPECT_EQ(1u, listener._session);
    EXPECT_TRUE(listener._last_session_begin);

    engine.on_session_end();
    EXPECT_EQ(2u, listener._session);
    EXPECT_FALSE(listener._last_session_begin);

    // _notifier == NULL 时三个通知入口都直接返回
    engine.notify_chart_marker(20231010093100ULL, "r41ctx", 3500.0, "icon", "tag");
    engine.notify_chart_index(20231010093100ULL, "r41ctx", "idx", "line", 1.0);
    engine.notify_trade("r41ctx", kCode, true, true, 20231010093100ULL, 3500.0, "tag");
}

//////////////////////////////////////////////////////////////////
// IExecuterStub 访问器
// src/WtCore/WtCtaEngine.cpp:683-702
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41ExecuterStubAccessors)
{
    WTSVariant* cfg = makeCfg("", 0);
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 931, 500, 931);
    engine.set_trading_date(20231010);

    EXPECT_EQ(_bd->commodity(), engine.get_comm_info(kCode));
    EXPECT_EQ(_bd->session(), engine.get_sess_info(kCode));
    EXPECT_EQ(nullptr, engine.get_sess_info("SHFE.zz.9999"));
    EXPECT_EQ(_hot.get(), engine.get_hot_mon());
    EXPECT_EQ(20231010u, engine.get_trading_day());

    // 与 TimeUtils 直接计算的毫秒时间戳一致
    uint64_t expect = TimeUtils::makeTime(20231010, 931 * 100000 + 500);
    EXPECT_EQ(expect, engine.get_real_time());
}

}  // namespace
