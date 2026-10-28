/**
 * r120_cta_engine_qfq_test.cpp — lane R120 / wave20
 *
 * 目标：src/WtCore/WtCtaEngine.cpp 的前复权（opt==1）订阅分发分支。
 *
 * 覆盖行段：
 *   - on_tick 的 opt==1 池调度 :556-562
 *   - on_tick 的 opt==1 直调 :563-566
 *
 * 接缝说明：WtEngine::sub_tick 里普通代码的 opt 解析取了错误的字符位
 * （:836-838，见 bugs/R120.md BUG-R120-004，R41 已登记），普通 "xxxx-" 永远
 * 落进 opt==2。只有带主力/自定义规则标签的热合约走正确分支（:791-799），
 * 因此这里用本夹具 HotMgrFake 认识的 "SHFE.rb.R41-" 订阅，键为
 * "SHFE.rb.R41"、opt=1；随后喂该键的 tick 触发分发。
 */
#include "r41_wtcore_fixture.h"

#include <WtCore/CtaStraBaseCtx.h>
#include <WtCore/WtCtaEngine.h>

#include <cstring>
#include <memory>
#include <string>

namespace {

using namespace r41;

using namespace wtp;

class R120CtaCtx : public CtaStraBaseCtx
{
public:
    R120CtaCtx(WtCtaEngine* engine, const char* name, int32_t slippage)
        : CtaStraBaseCtx(engine, name, slippage) {}
    ~R120CtaCtx() override {}

    void on_bar_close(const char*, const char*, WTSBarStruct*) override {}
    void on_calculate(uint32_t, uint32_t) override {}

    void on_tick(const char* stdCode, WTSTickData* newTick, bool bEmitStrategy = true) override
    {
        last_code = stdCode;
        ++ticks;
        CtaStraBaseCtx::on_tick(stdCode, newTick, bEmitStrategy);
    }

    std::string last_code;
    uint32_t ticks = 0;
};

typedef std::shared_ptr<R120CtaCtx> R120CtaCtxPtr;

//////////////////////////////////////////////////////////////////
// 池分支：opt==1 的 tick 复制交给线程池执行
// src/WtCore/WtCtaEngine.cpp:552-562
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R120QfqSubscriptionDispatchesThroughPool)
{
    WTSVariant* cfg = makeCfg("", 1);
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.set_adapter_mgr(&_adapter_mgr);
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 931, 0);
    engine.set_trading_date(20231010);

    R120CtaCtxPtr ctx(new R120CtaCtx(&engine, "r120qfq_pool", 0));
    engine.addContext(ctx);
    engine.on_session_begin();

    engine.sub_tick(ctx->id(), "SHFE.rb.R41-");

    WTSTickData* tick = makeTick("rb2310", 10.0);
    ASSERT_NE(nullptr, tick);
    engine.on_tick("SHFE.rb.R41", tick);
    tick->release();

    // on_tick 末尾 _pool->wait()，返回时分发已完成
    EXPECT_EQ(1u, ctx->ticks);
    EXPECT_EQ(std::string("SHFE.rb.R41-"), ctx->last_code);

    engine.release();
}

//////////////////////////////////////////////////////////////////
// 非池分支：opt==1 直接回调
// src/WtCore/WtCtaEngine.cpp:563-566
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R120QfqSubscriptionDispatchesDirectlyWithoutPool)
{
    WTSVariant* cfg = makeCfg("", 0);
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.set_adapter_mgr(&_adapter_mgr);
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 931, 0);
    engine.set_trading_date(20231010);

    R120CtaCtxPtr ctx(new R120CtaCtx(&engine, "r120qfq_direct", 0));
    engine.addContext(ctx);
    engine.on_session_begin();

    engine.sub_tick(ctx->id(), "SHFE.rb.R41-");

    WTSTickData* tick = makeTick("rb2310", 10.5);
    ASSERT_NE(nullptr, tick);
    engine.on_tick("SHFE.rb.R41", tick);
    tick->release();

    EXPECT_EQ(1u, ctx->ticks);
    EXPECT_EQ(std::string("SHFE.rb.R41-"), ctx->last_code);

    engine.release();
}

}  // namespace
