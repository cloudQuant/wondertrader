/**
 * r66_tickers_test.cpp  (lane R66 / wave13)
 *
 * 目标：三个实时触发器的“已 run() + 收到行情”路径。做法是先 stop() 再 run()：
 * _stopped 已为 true，run() 里新建的线程第一轮就退出，不会进入 10s/10ms 睡眠，
 * 随后 stop() 的 join 立即返回；此时 _thrd 仍非空，on_tick 就会走完整的
 * 分钟切片逻辑（这部分 R41 因担心墙钟依赖而未覆盖）。
 *
 * 覆盖：
 *   src/WtCore/WtCtaTicker.cpp:62-169, 171-190, 278-283
 *   src/WtCore/WtHftTicker.cpp:69-155, 157-173, 254-259
 *   src/WtCore/WtSelTicker.cpp:72-158, 160-175, 241-246
 */
#include "r66_fixture.h"

#include "WtCore/WtCtaEngine.h"
#include "WtCore/WtCtaTicker.h"
#include "WtCore/WtHftTicker.h"
#include "WtCore/WtSelEngine.h"
#include "WtCore/WtSelTicker.h"

namespace {

using namespace r66;

const char* const kAg = "SHFE.ag.2512";
const char* const kAgHot = "SHFE.ag.HOT";

/** 让 run() 创建的线程立即退出，同时保留非空 _thrd。 */
template <typename Ticker>
void armThreadedMode(Ticker& ticker)
{
    ticker.stop();
    ticker.run();
    ticker.stop();
}

//////////////////////////////////////////////////////////////////
// CTA: 同一分钟重复行情 + 热合约 + 旧日期行情
// src/WtCore/WtCtaTicker.cpp:62-77, 110-114, 145-169
//////////////////////////////////////////////////////////////////
TEST_F(R66Fixture, R66CtaTickerThreadedSameMinuteDispatch)
{
    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.set_adapter_mgr(&_adapter_mgr);
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 900, 0);

    FakeReader reader;
    WtCtaRtTicker ticker(&engine);
    ticker.init(&reader, "TRADING");
    ASSERT_NO_FATAL_FAILURE(armThreadedMode(ticker));
    ticker.init(&reader, "TRADING");   // run() 会用 data_mgr->reader() 覆盖 _store

    // 第一笔只建立 _cur_pos（WtCtaTicker.cpp:110-114 的 _cur_pos==0 支不调
    // trigger_price），平台时间保持用例预设的 900
    WTSTickData* t1 = makeAgTick(3500.0, 90100000u, 20991231u);
    ASSERT_NE(nullptr, t1);
    ticker.on_tick(t1);
    t1->release();
    EXPECT_EQ(900u, engine.get_min_time());
    EXPECT_EQ(0u, reader.count);

    // 同一分钟再来一笔：走 else 支，触发价格（含热合约循环）并刷新平台时间
    WTSTickData* t2 = makeAgTick(3501.0, 90130000u, 20991231u);
    ASSERT_NE(nullptr, t2);
    ticker.on_tick(t2);
    t2->release();
    EXPECT_DOUBLE_EQ(3501.0, engine.get_cur_price(kAg));
    EXPECT_DOUBLE_EQ(3501.0, engine.get_cur_price(kAgHot));
    EXPECT_EQ(902u, engine.get_min_time());
    EXPECT_EQ(0u, reader.count);

    // 行情日期早于本地日期：直接触发价格，不推进分钟
    WTSTickData* old = makeAgTick(3400.0, 80000000u, 20200101u);
    ASSERT_NE(nullptr, old);
    ticker.on_tick(old);
    old->release();
    EXPECT_DOUBLE_EQ(3400.0, engine.get_cur_price(kAg));
    EXPECT_DOUBLE_EQ(3400.0, engine.get_cur_price(kAgHot));
    EXPECT_EQ(902u, engine.get_min_time());
}

//////////////////////////////////////////////////////////////////
// CTA: 分钟闭合 -> onMinuteEnd + on_schedule；重复分钟后不重复
// src/WtCore/WtCtaTicker.cpp:115-156, 189-190, 280-283
//////////////////////////////////////////////////////////////////
TEST_F(R66Fixture, R66CtaTickerThreadedMinuteCloseSchedulesAndReplays)
{
    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);

    EvtListenerFake listener;
    WtCtaEngine engine;
    engine.set_adapter_mgr(&_adapter_mgr);
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 900, 0);
    engine.regEventListener(&listener);

    FakeReader reader;
    WtCtaRtTicker ticker(&engine);
    ticker.init(&reader, "TRADING");
    ASSERT_NO_FATAL_FAILURE(armThreadedMode(ticker));
    ticker.init(&reader, "TRADING");   // run() 会用 data_mgr->reader() 覆盖 _store
    EXPECT_TRUE(listener.session > 0);            // run() 内的 on_session_begin
    EXPECT_TRUE(listener.last_session_begin);

    WTSTickData* t1 = makeAgTick(3500.0, 90200000u, 20991231u);
    ASSERT_NE(nullptr, t1);
    ticker.on_tick(t1);
    t1->release();
    EXPECT_EQ(900u, engine.get_min_time());
    EXPECT_EQ(0u, reader.count);

    WTSTickData* t2 = makeAgTick(3502.0, 90350000u, 20991231u);
    ASSERT_NE(nullptr, t2);
    ticker.on_tick(t2);
    t2->release();

    EXPECT_EQ(1u, reader.count);
    EXPECT_EQ(20991231u, reader.last_date);
    EXPECT_EQ(903u, reader.last_time);
    EXPECT_EQ(0u, reader.last_end_tdate);
    EXPECT_EQ(1u, listener.sched);
    EXPECT_EQ(20991231u, listener.last_sched_date);
    EXPECT_EQ(903u, listener.last_sched_time);
    EXPECT_EQ(904u, engine.get_min_time());
    EXPECT_DOUBLE_EQ(3502.0, engine.get_cur_price(kAgHot));

    // 分钟未推进：不重复闭合
    WTSTickData* t3 = makeAgTick(3503.0, 90358000u, 20991231u);
    ASSERT_NE(nullptr, t3);
    ticker.on_tick(t3);
    t3->release();
    EXPECT_EQ(1u, reader.count);
    EXPECT_EQ(1u, listener.sched);
    EXPECT_EQ(904u, engine.get_min_time());
}

//////////////////////////////////////////////////////////////////
// CTA: 小节最后一分钟 + 1500 不闭合 + 旧行情短路
// src/WtCore/WtCtaTicker.cpp:95-108, 157-163
//////////////////////////////////////////////////////////////////
TEST_F(R66Fixture, R66CtaTickerSectionEndTickDoesNotCloseEarly)
{
    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);

    EvtListenerFake listener;
    WtCtaEngine engine;
    engine.set_adapter_mgr(&_adapter_mgr);
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 900, 0);
    engine.regEventListener(&listener);

    FakeReader reader;
    WtCtaRtTicker ticker(&engine);
    ticker.init(&reader, "TRADING");
    ASSERT_NO_FATAL_FAILURE(armThreadedMode(ticker));
    ticker.init(&reader, "TRADING");   // run() 会用 data_mgr->reader() 覆盖 _store

    // actiontime 是 HHMMSSmmm，下午 14:59:00 应写作 145900000（9 位）
    WTSTickData* t1 = makeAgTick(3500.0, 145900000u, 20991231u);
    ASSERT_NE(nullptr, t1);
    ticker.on_tick(t1);
    t1->release();
    EXPECT_EQ(900u, engine.get_min_time());

    // 15:00:00 是 isLastOfSection，minutes 计算走 --/++ 支
    WTSTickData* t2 = makeAgTick(3501.0, 150000000u, 20991231u);
    ASSERT_NE(nullptr, t2);
    ticker.on_tick(t2);
    t2->release();
    EXPECT_EQ(1500u, engine.get_min_time());
    EXPECT_EQ(0u, reader.count);
    EXPECT_EQ(0u, listener.sched);

    WTSTickData* old = makeAgTick(3499.0, 90100000u, 20200101u);
    ASSERT_NE(nullptr, old);
    ticker.on_tick(old);
    old->release();
    EXPECT_DOUBLE_EQ(3499.0, engine.get_cur_price(kAgHot));
    EXPECT_EQ(1500u, engine.get_min_time());
}

//////////////////////////////////////////////////////////////////
// HFT: 分钟闭合 -> onMinuteEnd + on_minute_end
// src/WtCore/WtHftTicker.cpp:69-155, 157-173, 254-259
//////////////////////////////////////////////////////////////////
TEST_F(R66Fixture, R66HftTickerThreadedMinuteCloseReplays)
{
    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);

    WtHftEngine engine;
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 900, 0);

    FakeReader reader;
    WtHftRtTicker ticker(&engine);
    ticker.init(&reader, "TRADING");
    ASSERT_NO_FATAL_FAILURE(armThreadedMode(ticker));
    ticker.init(&reader, "TRADING");   // run() 会用 data_mgr->reader() 覆盖 _store

    WTSTickData* t1 = makeAgTick(3500.0, 90100000u, 20991231u);
    ASSERT_NE(nullptr, t1);
    ticker.on_tick(t1);
    t1->release();
    EXPECT_EQ(900u, engine.get_min_time());

    WTSTickData* t2 = makeAgTick(3502.0, 90350000u, 20991231u);
    ASSERT_NE(nullptr, t2);
    ticker.on_tick(t2);
    t2->release();
    EXPECT_EQ(1u, reader.count);
    EXPECT_EQ(20991231u, reader.last_date);
    EXPECT_EQ(902u, reader.last_time);            // 闭合的是已记录的分钟位
    EXPECT_EQ(904u, engine.get_min_time());
    EXPECT_DOUBLE_EQ(3502.0, engine.get_cur_price(kAgHot));

    WTSTickData* t3 = makeAgTick(3503.0, 90358000u, 20991231u);
    ASSERT_NE(nullptr, t3);
    ticker.on_tick(t3);
    t3->release();
    EXPECT_EQ(1u, reader.count);
    EXPECT_EQ(904u, engine.get_min_time());
}

//////////////////////////////////////////////////////////////////
// HFT: 小节末 + 1500 + 旧日期行情
// src/WtCore/WtHftTicker.cpp:77-85, 92-107, 143-149
//////////////////////////////////////////////////////////////////
TEST_F(R66Fixture, R66HftTickerSectionEndAndStaleTick)
{
    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);

    WtHftEngine engine;
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 900, 0);

    FakeReader reader;
    WtHftRtTicker ticker(&engine);
    ticker.init(&reader, "TRADING");
    ASSERT_NO_FATAL_FAILURE(armThreadedMode(ticker));
    ticker.init(&reader, "TRADING");   // run() 会用 data_mgr->reader() 覆盖 _store

    // 14:59:00 / 15:00:00 对应 145900000 / 150000000（HHMMSSmmm）
    WTSTickData* t1 = makeAgTick(3500.0, 145900000u, 20991231u);
    ASSERT_NE(nullptr, t1);
    ticker.on_tick(t1);
    t1->release();
    EXPECT_EQ(900u, engine.get_min_time());

    WTSTickData* t2 = makeAgTick(3501.0, 150000000u, 20991231u);
    ASSERT_NE(nullptr, t2);
    ticker.on_tick(t2);
    t2->release();
    EXPECT_EQ(1500u, engine.get_min_time());
    EXPECT_EQ(0u, reader.count);

    WTSTickData* old = makeAgTick(3498.0, 90000000u, 20200101u);
    ASSERT_NE(nullptr, old);
    ticker.on_tick(old);
    old->release();
    EXPECT_DOUBLE_EQ(3498.0, engine.get_cur_price(kAg));
    EXPECT_DOUBLE_EQ(3498.0, engine.get_cur_price(kAgHot));
    EXPECT_EQ(1500u, engine.get_min_time());
}

//////////////////////////////////////////////////////////////////
// SEL: 分钟闭合 -> onMinuteEnd + on_minute_end
// src/WtCore/WtSelTicker.cpp:72-158, 160-175, 241-246
//////////////////////////////////////////////////////////////////
TEST_F(R66Fixture, R66SelTickerThreadedMinuteCloseReplays)
{
    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);

    // WtSelEngine 析构会读取未初始化的 _tm_ticker（BUG-R42-001），这里堆分配不回收
    WtSelEngine* engine = new WtSelEngine();
    engine->init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine->set_date_time(20231010, 900, 0);

    FakeReader reader;
    WtSelRtTicker ticker(engine);
    ticker.init(&reader, "TRADING");
    ASSERT_NO_FATAL_FAILURE(armThreadedMode(ticker));
    ticker.init(&reader, "TRADING");   // run() 会用 data_mgr->reader() 覆盖 _store

    // 第一笔只记录分钟位（WtSelTicker.cpp:105-109 的 _cur_pos==0 支不触发行情）
    WTSTickData* t1 = makeAgTick(3500.0, 90100000u, 20991231u);
    ASSERT_NE(nullptr, t1);
    ticker.on_tick(t1);
    t1->release();
    EXPECT_EQ(900u, engine->get_min_time());

    WTSTickData* t2 = makeAgTick(3502.0, 90350000u, 20991231u);
    ASSERT_NE(nullptr, t2);
    ticker.on_tick(t2);
    t2->release();
    EXPECT_EQ(1u, reader.count);
    EXPECT_EQ(20991231u, reader.last_date);
    EXPECT_EQ(902u, reader.last_time);
    EXPECT_EQ(904u, engine->get_min_time());
    EXPECT_DOUBLE_EQ(3502.0, engine->get_cur_price(kAgHot));

    WTSTickData* t3 = makeAgTick(3503.0, 90358000u, 20991231u);
    ASSERT_NE(nullptr, t3);
    ticker.on_tick(t3);
    t3->release();
    EXPECT_EQ(1u, reader.count);
    EXPECT_EQ(904u, engine->get_min_time());
}

//////////////////////////////////////////////////////////////////
// SEL: 无热合约的 tick -> trigger_price 跳过热合约循环
// src/WtCore/WtSelTicker.cpp:50-70
//////////////////////////////////////////////////////////////////
TEST_F(R66Fixture, R66SelTickerFlatContractSkipsHotLoop)
{
    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);

    WtSelEngine* engine = new WtSelEngine();
    engine->init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine->set_date_time(20231010, 900, 0);

    FakeReader reader;
    WtSelRtTicker ticker(engine);
    ticker.init(&reader, "TRADING");
    ASSERT_NO_FATAL_FAILURE(armThreadedMode(ticker));
    ticker.init(&reader, "TRADING");   // run() 会用 data_mgr->reader() 覆盖 _store

    // 第一笔只建立 _cur_pos；同一分钟第二笔才会走 trigger_price 的热合约分支
    WTSTickData* tick1 = makeAuTick(480.0, 93100000u, 20991231u);
    ASSERT_NE(nullptr, tick1);
    ticker.on_tick(tick1, 1);
    tick1->release();
    EXPECT_EQ(900u, engine->get_min_time());

    WTSTickData* tick2 = makeAuTick(481.0, 93130000u, 20991231u);
    ASSERT_NE(nullptr, tick2);
    ticker.on_tick(tick2, 1);
    tick2->release();

    EXPECT_DOUBLE_EQ(481.0, engine->get_cur_price("SHFE.au.2212"));
    // 无热合约映射，热合约价格不应被写入
    EXPECT_DOUBLE_EQ(0.0, engine->get_cur_price("SHFE.au.HOT"));
    EXPECT_EQ(932u, engine->get_min_time());
}

//////////////////////////////////////////////////////////////////
// SEL: 小节末 1500 + 旧日期行情
// src/WtCore/WtSelTicker.cpp:92-107, 146-152
//////////////////////////////////////////////////////////////////
TEST_F(R66Fixture, R66SelTickerSectionEndAndStaleTick)
{
    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);

    WtSelEngine* engine = new WtSelEngine();
    engine->init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine->set_date_time(20231010, 900, 0);

    FakeReader reader;
    WtSelRtTicker ticker(engine);
    ticker.init(&reader, "TRADING");
    ASSERT_NO_FATAL_FAILURE(armThreadedMode(ticker));
    ticker.init(&reader, "TRADING");   // run() 会用 data_mgr->reader() 覆盖 _store

    // 14:59:00 / 15:00:00 对应 145900000 / 150000000（HHMMSSmmm）
    WTSTickData* t1 = makeAgTick(3500.0, 145900000u, 20991231u);
    ASSERT_NE(nullptr, t1);
    ticker.on_tick(t1);
    t1->release();
    EXPECT_EQ(900u, engine->get_min_time());

    WTSTickData* t2 = makeAgTick(3501.0, 150000000u, 20991231u);
    ASSERT_NE(nullptr, t2);
    ticker.on_tick(t2);
    t2->release();
    EXPECT_EQ(1500u, engine->get_min_time());
    EXPECT_EQ(0u, reader.count);

    WTSTickData* old = makeAgTick(3497.0, 90100000u, 20200101u);
    ASSERT_NE(nullptr, old);
    ticker.on_tick(old);
    old->release();
    EXPECT_DOUBLE_EQ(3497.0, engine->get_cur_price(kAgHot));
    EXPECT_EQ(1500u, engine->get_min_time());
}

}  // namespace
