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

#include <map>
#include <string>

namespace {

using namespace r66;

const char* const kAg = "SHFE.ag.2512";
const char* const kAgHot = "SHFE.ag.HOT";

/**
 * 迭代03 卡3 3a（2026-11-04 用户裁定）：所有首 tick（冷启动首 tick 与新交易日首 tick）都推进平台时间并交给策略。
 * 原断言“第一笔只建立 _cur_pos、不调 trigger_price、平台时间保持 900”已按新合同改为：首 tick 恰好投递一次、
 * 平台日期/时间推进到该 tick 所在分钟、且分钟位置已建立（同分钟后续 tick 不闭合，下一分钟 tick 闭合该分钟）。
 * 计数引擎只统计 on_tick 调用次数并转发给真实引擎，不改变引擎行为。
 */
template <typename Base>
class R66CountingEngine : public Base
{
public:
    void on_tick(const char* stdCode, WTSTickData* curTick) override
    {
        ++delivered[stdCode == NULL ? std::string() : std::string(stdCode)];
        Base::on_tick(stdCode, curTick);
    }
    uint32_t count(const char* code) const
    {
        auto it = delivered.find(code);
        return it == delivered.end() ? 0u : it->second;
    }
    std::map<std::string, uint32_t> delivered;
};

/**
 * I03-NEW-022：HFT ticker 自迭代02（e03cbe1d）起只能启动一次，stop() 之后的 run() 直接结束、不创建 worker，
 * 因此 HFT 用例不再用 armThreadedMode，而是“新建 ticker + 单次 run()”进入 worker 模式，用例结束前 stop()。
 * worker 与 on_tick 经卡9 9d=A 的 strand 串行；用例行情的 _next_check_time 距下一分钟 ≥10s，
 * 用例期间 worker 不会按墙钟自动闭合分钟，断言只反映 on_tick 的数据驱动路径。
 */
template <typename Ticker>
void startOnce(Ticker& ticker)
{
    ticker.run();
}

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

    R66CountingEngine<WtCtaEngine> engine;
    engine.set_adapter_mgr(&_adapter_mgr);
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 900, 0);

    FakeReader reader;
    WtCtaRtTicker ticker(&engine);
    ticker.init(&reader, "TRADING");
    ASSERT_NO_FATAL_FAILURE(armThreadedMode(ticker));
    ticker.init(&reader, "TRADING");   // run() 会用 data_mgr->reader() 覆盖 _store

    // 卡3 3a：首 tick 建立 _cur_pos，同时推进平台时间并恰好投递一次（含热合约），不闭合分钟
    WTSTickData* t1 = makeAgTick(3500.0, 90100000u, 20991231u);
    ASSERT_NE(nullptr, t1);
    ticker.on_tick(t1);
    t1->release();
    EXPECT_EQ(1u, engine.count(kAg));
    EXPECT_EQ(1u, engine.count(kAgHot));
    EXPECT_DOUBLE_EQ(3500.0, engine.get_cur_price(kAg));
    EXPECT_DOUBLE_EQ(3500.0, engine.get_cur_price(kAgHot));
    EXPECT_EQ(20991231u, engine.get_date());
    EXPECT_EQ(902u, engine.get_min_time());       // 09:01:00 落在 09:02 标签的分钟
    EXPECT_EQ(0u, engine.get_secs());
    EXPECT_EQ(0u, reader.count);

    // 同一分钟再来一笔：走 else 支，触发价格（含热合约循环）并刷新平台时间
    WTSTickData* t2 = makeAgTick(3501.0, 90130000u, 20991231u);
    ASSERT_NE(nullptr, t2);
    ticker.on_tick(t2);
    t2->release();
    EXPECT_DOUBLE_EQ(3501.0, engine.get_cur_price(kAg));
    EXPECT_DOUBLE_EQ(3501.0, engine.get_cur_price(kAgHot));
    EXPECT_EQ(2u, engine.count(kAg));
    EXPECT_EQ(902u, engine.get_min_time());
    EXPECT_EQ(0u, reader.count);                  // 首 tick 已建立同一分钟位，不闭合

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
    R66CountingEngine<WtCtaEngine> engine;
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

    // 卡3 3a：首 tick 推进平台时间到 09:03 标签分钟、恰好投递一次，不闭合、不调度
    WTSTickData* t1 = makeAgTick(3500.0, 90200000u, 20991231u);
    ASSERT_NE(nullptr, t1);
    ticker.on_tick(t1);
    t1->release();
    EXPECT_EQ(1u, engine.count(kAg));
    EXPECT_DOUBLE_EQ(3500.0, engine.get_cur_price(kAg));
    EXPECT_EQ(20991231u, engine.get_date());
    EXPECT_EQ(903u, engine.get_min_time());
    EXPECT_EQ(0u, reader.count);
    EXPECT_EQ(0u, listener.sched);

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
    EXPECT_EQ(903u, listener.last_sched_time);    // 闭合的正是首 tick 建立的分钟位
    EXPECT_EQ(904u, engine.get_min_time());
    EXPECT_DOUBLE_EQ(3502.0, engine.get_cur_price(kAgHot));
    EXPECT_EQ(2u, engine.count(kAg));

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
    R66CountingEngine<WtCtaEngine> engine;
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
    // 卡3 3a：首 tick 推进平台时间到 15:00 标签分钟、恰好投递一次，不闭合
    WTSTickData* t1 = makeAgTick(3500.0, 145900000u, 20991231u);
    ASSERT_NE(nullptr, t1);
    ticker.on_tick(t1);
    t1->release();
    EXPECT_EQ(1u, engine.count(kAg));
    EXPECT_DOUBLE_EQ(3500.0, engine.get_cur_price(kAg));
    EXPECT_EQ(20991231u, engine.get_date());
    EXPECT_EQ(1500u, engine.get_min_time());
    EXPECT_EQ(0u, reader.count);

    // 15:00:00 是 isLastOfSection，minutes 计算走 --/++ 支
    WTSTickData* t2 = makeAgTick(3501.0, 150000000u, 20991231u);
    ASSERT_NE(nullptr, t2);
    ticker.on_tick(t2);
    t2->release();
    EXPECT_EQ(1500u, engine.get_min_time());
    EXPECT_EQ(0u, reader.count);                  // 15:00 与首 tick 同一分钟位，不提前闭合
    EXPECT_EQ(0u, listener.sched);
    EXPECT_EQ(2u, engine.count(kAg));

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

    R66CountingEngine<WtHftEngine> engine;
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 900, 0);

    FakeReader reader;
    WtHftRtTicker ticker(&engine);
    ticker.init(&reader, "TRADING");
    ASSERT_NO_FATAL_FAILURE(startOnce(ticker));   // I03-NEW-022：只启动一次合同
    // I03-NEW-115（测试侧 TSan）：HFT run() 不覆盖 _store，worker 运行期间不再重复 init()
    // （重复 init 会与 worker_pass 并发写 _store/_s_info/_date/_time）。_store 仍是上面传入的 reader。

    // 卡3 3a：首 tick 建立分钟位，同时推进平台时间并恰好投递一次（含热合约），不闭合
    WTSTickData* t1 = makeAgTick(3500.0, 90100000u, 20991231u);
    ASSERT_NE(nullptr, t1);
    ticker.on_tick(t1);
    t1->release();
    EXPECT_EQ(1u, engine.count(kAg));
    EXPECT_EQ(1u, engine.count(kAgHot));
    EXPECT_DOUBLE_EQ(3500.0, engine.get_cur_price(kAg));
    EXPECT_EQ(20991231u, engine.get_date());
    EXPECT_EQ(902u, engine.get_min_time());
    EXPECT_EQ(0u, reader.count);

    WTSTickData* t2 = makeAgTick(3502.0, 90350000u, 20991231u);
    ASSERT_NE(nullptr, t2);
    ticker.on_tick(t2);
    t2->release();
    EXPECT_EQ(1u, reader.count);
    EXPECT_EQ(20991231u, reader.last_date);
    EXPECT_EQ(902u, reader.last_time);            // 闭合的是已记录的分钟位
    EXPECT_EQ(904u, engine.get_min_time());
    EXPECT_DOUBLE_EQ(3502.0, engine.get_cur_price(kAgHot));
    EXPECT_EQ(2u, engine.count(kAg));

    WTSTickData* t3 = makeAgTick(3503.0, 90358000u, 20991231u);
    ASSERT_NE(nullptr, t3);
    ticker.on_tick(t3);
    t3->release();
    EXPECT_EQ(1u, reader.count);
    EXPECT_EQ(904u, engine.get_min_time());
    EXPECT_EQ(3u, engine.count(kAg));
    ticker.stop();
}

//////////////////////////////////////////////////////////////////
// HFT: 小节末 + 1500 + 旧日期行情
// src/WtCore/WtHftTicker.cpp:77-85, 92-107, 143-149
//////////////////////////////////////////////////////////////////
TEST_F(R66Fixture, R66HftTickerSectionEndAndStaleTick)
{
    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);

    R66CountingEngine<WtHftEngine> engine;
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 900, 0);

    FakeReader reader;
    WtHftRtTicker ticker(&engine);
    ticker.init(&reader, "TRADING");
    ASSERT_NO_FATAL_FAILURE(startOnce(ticker));   // I03-NEW-022：只启动一次合同
    // I03-NEW-115（测试侧 TSan）：HFT run() 不覆盖 _store，worker 运行期间不再重复 init()
    // （重复 init 会与 worker_pass 并发写 _store/_s_info/_date/_time）。_store 仍是上面传入的 reader。

    // 14:59:00 / 15:00:00 对应 145900000 / 150000000（HHMMSSmmm）
    // 卡3 3a：首 tick 推进平台时间到 15:00 标签分钟、恰好投递一次，不闭合
    WTSTickData* t1 = makeAgTick(3500.0, 145900000u, 20991231u);
    ASSERT_NE(nullptr, t1);
    ticker.on_tick(t1);
    t1->release();
    EXPECT_EQ(1u, engine.count(kAg));
    EXPECT_DOUBLE_EQ(3500.0, engine.get_cur_price(kAg));
    EXPECT_EQ(20991231u, engine.get_date());
    EXPECT_EQ(1500u, engine.get_min_time());
    EXPECT_EQ(0u, reader.count);

    WTSTickData* t2 = makeAgTick(3501.0, 150000000u, 20991231u);
    ASSERT_NE(nullptr, t2);
    ticker.on_tick(t2);
    t2->release();
    EXPECT_EQ(1500u, engine.get_min_time());
    EXPECT_EQ(0u, reader.count);                  // 15:00 与首 tick 同一分钟位，不提前闭合
    EXPECT_EQ(2u, engine.count(kAg));

    WTSTickData* old = makeAgTick(3498.0, 90000000u, 20200101u);
    ASSERT_NE(nullptr, old);
    ticker.on_tick(old);
    old->release();
    EXPECT_DOUBLE_EQ(3498.0, engine.get_cur_price(kAg));
    EXPECT_DOUBLE_EQ(3498.0, engine.get_cur_price(kAgHot));
    EXPECT_EQ(1500u, engine.get_min_time());      // 旧日期行情只投递价格，不推进时间
    EXPECT_EQ(3u, engine.count(kAg));
    ticker.stop();
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
    R66CountingEngine<WtSelEngine>* engine = new R66CountingEngine<WtSelEngine>();
    engine->init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine->set_date_time(20231010, 900, 0);

    FakeReader reader;
    WtSelRtTicker ticker(engine);
    ticker.init(&reader, "TRADING");
    ASSERT_NO_FATAL_FAILURE(armThreadedMode(ticker));
    ticker.init(&reader, "TRADING");   // run() 会用 data_mgr->reader() 覆盖 _store

    // 卡3 3a：首 tick 记录分钟位，同时推进平台时间并恰好投递一次（含热合约），不闭合
    WTSTickData* t1 = makeAgTick(3500.0, 90100000u, 20991231u);
    ASSERT_NE(nullptr, t1);
    ticker.on_tick(t1);
    t1->release();
    EXPECT_EQ(1u, engine->count(kAg));
    EXPECT_EQ(1u, engine->count(kAgHot));
    EXPECT_DOUBLE_EQ(3500.0, engine->get_cur_price(kAg));
    EXPECT_EQ(20991231u, engine->get_date());
    EXPECT_EQ(902u, engine->get_min_time());
    EXPECT_EQ(0u, reader.count);

    WTSTickData* t2 = makeAgTick(3502.0, 90350000u, 20991231u);
    ASSERT_NE(nullptr, t2);
    ticker.on_tick(t2);
    t2->release();
    EXPECT_EQ(1u, reader.count);
    EXPECT_EQ(20991231u, reader.last_date);
    EXPECT_EQ(902u, reader.last_time);            // 闭合的正是首 tick 建立的分钟位
    EXPECT_EQ(904u, engine->get_min_time());
    EXPECT_DOUBLE_EQ(3502.0, engine->get_cur_price(kAgHot));
    EXPECT_EQ(2u, engine->count(kAg));

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

    R66CountingEngine<WtSelEngine>* engine = new R66CountingEngine<WtSelEngine>();
    engine->init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine->set_date_time(20231010, 900, 0);

    FakeReader reader;
    WtSelRtTicker ticker(engine);
    ticker.init(&reader, "TRADING");
    ASSERT_NO_FATAL_FAILURE(armThreadedMode(ticker));
    ticker.init(&reader, "TRADING");   // run() 会用 data_mgr->reader() 覆盖 _store

    // 卡3 3a：首 tick 同样走 trigger_price：恰好投递一次、推进平台时间；无热合约映射时不写热合约
    WTSTickData* tick1 = makeAuTick(480.0, 93100000u, 20991231u);
    ASSERT_NE(nullptr, tick1);
    ticker.on_tick(tick1, 1);
    tick1->release();
    EXPECT_EQ(1u, engine->count("SHFE.au.2212"));
    EXPECT_EQ(0u, engine->count("SHFE.au.HOT"));
    EXPECT_DOUBLE_EQ(480.0, engine->get_cur_price("SHFE.au.2212"));
    EXPECT_DOUBLE_EQ(0.0, engine->get_cur_price("SHFE.au.HOT"));
    EXPECT_EQ(20991231u, engine->get_date());
    EXPECT_EQ(932u, engine->get_min_time());
    EXPECT_EQ(0u, reader.count);

    WTSTickData* tick2 = makeAuTick(481.0, 93130000u, 20991231u);
    ASSERT_NE(nullptr, tick2);
    ticker.on_tick(tick2, 1);
    tick2->release();

    EXPECT_DOUBLE_EQ(481.0, engine->get_cur_price("SHFE.au.2212"));
    // 无热合约映射，热合约价格不应被写入
    EXPECT_DOUBLE_EQ(0.0, engine->get_cur_price("SHFE.au.HOT"));
    EXPECT_EQ(932u, engine->get_min_time());
    EXPECT_EQ(2u, engine->count("SHFE.au.2212"));
    EXPECT_EQ(0u, engine->count("SHFE.au.HOT"));
    EXPECT_EQ(0u, reader.count);
}

//////////////////////////////////////////////////////////////////
// SEL: 小节末 1500 + 旧日期行情
// src/WtCore/WtSelTicker.cpp:92-107, 146-152
//////////////////////////////////////////////////////////////////
TEST_F(R66Fixture, R66SelTickerSectionEndAndStaleTick)
{
    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);

    R66CountingEngine<WtSelEngine>* engine = new R66CountingEngine<WtSelEngine>();
    engine->init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine->set_date_time(20231010, 900, 0);

    FakeReader reader;
    WtSelRtTicker ticker(engine);
    ticker.init(&reader, "TRADING");
    ASSERT_NO_FATAL_FAILURE(armThreadedMode(ticker));
    ticker.init(&reader, "TRADING");   // run() 会用 data_mgr->reader() 覆盖 _store

    // 14:59:00 / 15:00:00 对应 145900000 / 150000000（HHMMSSmmm）
    // 卡3 3a：首 tick 推进平台时间到 15:00 标签分钟、恰好投递一次，不闭合
    WTSTickData* t1 = makeAgTick(3500.0, 145900000u, 20991231u);
    ASSERT_NE(nullptr, t1);
    ticker.on_tick(t1);
    t1->release();
    EXPECT_EQ(1u, engine->count(kAg));
    EXPECT_DOUBLE_EQ(3500.0, engine->get_cur_price(kAg));
    EXPECT_EQ(20991231u, engine->get_date());
    EXPECT_EQ(1500u, engine->get_min_time());
    EXPECT_EQ(0u, reader.count);

    WTSTickData* t2 = makeAgTick(3501.0, 150000000u, 20991231u);
    ASSERT_NE(nullptr, t2);
    ticker.on_tick(t2);
    t2->release();
    EXPECT_EQ(1500u, engine->get_min_time());
    EXPECT_EQ(0u, reader.count);                  // 15:00 与首 tick 同一分钟位，不提前闭合
    EXPECT_EQ(2u, engine->count(kAg));

    WTSTickData* old = makeAgTick(3497.0, 90100000u, 20200101u);
    ASSERT_NE(nullptr, old);
    ticker.on_tick(old);
    old->release();
    EXPECT_DOUBLE_EQ(3497.0, engine->get_cur_price(kAgHot));
    EXPECT_EQ(1500u, engine->get_min_time());
}

}  // namespace
