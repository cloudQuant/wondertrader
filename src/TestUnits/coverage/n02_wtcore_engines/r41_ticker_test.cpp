/**
 * r41_ticker_test.cpp  (lane R41 / wave10)
 *
 * 覆盖三个实时触发器在"已初始化引擎"下的路径:
 *   src/WtCore/WtCtaTicker.cpp  : init / on_tick -> trigger_price / time_to_mins / is_in_trading
 *   src/WtCore/WtHftTicker.cpp  : init / on_tick -> trigger_price
 *   src/WtCore/WtSelTicker.cpp  : init / on_tick -> trigger_price
 *   src/WtCore/WtHftEngine.cpp  : on_tick / on_session_begin / on_session_end
 *
 * 不调用 run(): 它创建的线程在盘外会 sleep 10s, stop() 的 join 会把用例拖成
 * 墙钟依赖; 因此只驱动 on_tick 的同步分支(thrd==NULL -> trigger_price)。
 */
#include "r41_wtcore_fixture.h"

#include "WtCore/WtCtaEngine.h"
#include "WtCore/WtCtaTicker.h"
#include "WtCore/WtHftEngine.h"
#include "WtCore/WtHftTicker.h"
#include "WtCore/WtSelEngine.h"
#include "WtCore/WtSelTicker.h"

namespace {

using namespace r41;

const char* const kCode = "SHFE.rb.2310";

//////////////////////////////////////////////////////////////////
// CTA ticker: 有效 session + 分钟换算
// src/WtCore/WtCtaTicker.cpp:28-38, 285-299
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41CtaTickerInitWithSession)
{
    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 930, 0);
    engine.set_trading_date(20231010);

    WtCtaRtTicker ticker(&engine);
    ticker.init(NULL, "TRADING");

    // 09:30 落在 900-1500 日盘区间内 -> 第 30 分钟
    EXPECT_EQ(30u, ticker.time_to_mins(930));
    (void)ticker.is_in_trading();

    ticker.stop();
}

//////////////////////////////////////////////////////////////////
// CTA ticker: 未知 session -> _s_info 为空, 换算接口原样返回/返回 false
// src/WtCore/WtCtaTicker.cpp:28-38, 285-299
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41CtaTickerToleratesMissingSession)
{
    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 940, 0);
    engine.set_trading_date(20231010);

    WtCtaRtTicker ticker(&engine);
    ticker.init(NULL, "NO_SUCH_SESSION");

    EXPECT_EQ(940u, ticker.time_to_mins(940));
    EXPECT_FALSE(ticker.is_in_trading());

    // 没有线程时 on_tick 直接走 trigger_price, 与 session 无关
    WTSTickData* tick = makeTick(kCode, 3501.0);
    ticker.on_tick(tick);
    EXPECT_DOUBLE_EQ(3501.0, engine.get_cur_price(kCode));

    tick->release();
    ticker.stop();
}

//////////////////////////////////////////////////////////////////
// CTA ticker: 同步触发 -> 引擎 WtEngine::on_tick
// src/WtCore/WtCtaTicker.cpp:40-68
// src/WtCore/WtCtaEngine.cpp:490-637
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41CtaTickerOnTickFeedsEngine)
{
    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.set_adapter_mgr(&_adapter_mgr);
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 931, 0);
    engine.set_trading_date(20231010);

    WtCtaRtTicker ticker(&engine);
    ticker.init(NULL, "TRADING");
    engine.on_session_begin();

    WTSTickData* tick = makeTick(kCode, 3502.0);
    ticker.on_tick(tick);

    EXPECT_DOUBLE_EQ(3502.0, engine.get_cur_price(kCode));

    WTSTickData* last = engine.get_last_tick(1, kCode);
    ASSERT_NE(nullptr, last);
    EXPECT_DOUBLE_EQ(3502.0, last->price());
    last->release();

    tick->release();
    ticker.stop();
}

//////////////////////////////////////////////////////////////////
// HFT ticker: 同步触发 + HFT 引擎 session 事件
// src/WtCore/WtHftTicker.cpp:42-67
// src/WtCore/WtHftEngine.cpp:223-334
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41HftTickerOnTickFeedsHftEngine)
{
    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);

    WtHftEngine engine;
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 931, 0);
    engine.set_trading_date(20231010);

    WtHftRtTicker ticker(&engine);
    ticker.init(NULL, "TRADING");

    engine.on_session_begin();
    engine.sub_tick(1, kCode);

    WTSTickData* tick = makeTick(kCode, 3503.0);
    ticker.on_tick(tick);

    EXPECT_DOUBLE_EQ(3503.0, engine.get_cur_price(kCode));

    engine.on_session_end();
    ticker.stop();
    tick->release();
}

//////////////////////////////////////////////////////////////////
// SEL ticker: 同步触发 + SEL 引擎 session 事件
// src/WtCore/WtSelTicker.cpp:42-77
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41SelTickerOnTickFeedsSelEngine)
{
    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);

    // WtSelEngine 的 _tm_ticker 未在构造函数初始化，析构的 release() 会读取它
    // （WtSelEngine.h:94 / WtSelEngine.cpp:33-41，BUG-R42-001）。栈上对象在用例结束时
    // 必然析构，之前实测 rc=137；这里改为堆分配且不回收，避免在测试里触发未定义行为。
    WtSelEngine* engine = new WtSelEngine();
    engine->init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine->set_date_time(20231010, 931, 0);
    engine->set_trading_date(20231010);

    WtSelRtTicker ticker(engine);
    ticker.init(NULL, "TRADING");

    engine->on_session_begin();
    engine->sub_tick(1, kCode);

    WTSTickData* tick = makeTick(kCode, 3504.0);
    ticker.on_tick(tick, 0);

    EXPECT_DOUBLE_EQ(3504.0, engine->get_cur_price(kCode));

    engine->on_session_end();
    ticker.stop();
    tick->release();
}

//////////////////////////////////////////////////////////////////
// HFT ticker: 未知 session 不阻断同步触发
// src/WtCore/WtHftTicker.cpp:42-48
//////////////////////////////////////////////////////////////////
TEST_F(R41EngineFixture, R41HftTickerInitWithMissingSession)
{
    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);

    WtHftEngine engine;
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 931, 0);
    engine.set_trading_date(20231010);

    WtHftRtTicker ticker(&engine);
    ticker.init(NULL, "NO_SUCH_SESSION");

    WTSTickData* tick = makeTick(kCode, 3505.0);
    ticker.on_tick(tick);
    EXPECT_DOUBLE_EQ(3505.0, engine.get_cur_price(kCode));

    ticker.stop();
    tick->release();
}

}  // namespace
