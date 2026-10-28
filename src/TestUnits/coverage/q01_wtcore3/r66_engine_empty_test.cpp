/**
 * r66_engine_empty_test.cpp  (lane R66 / wave13)
 *
 * 只打 R41 没覆盖的“空上下文/空订阅”分支，不重复 init/load_datas 主路径：
 *   src/WtCore/WtCtaEngine.cpp:148-247, 490-637(无订阅), 639-671(无订阅), 704-720
 *   src/WtCore/WtHftEngine.cpp:116-219, 221-285(无/有订阅), 287-374
 *   src/WtCore/WtSelEngine.cpp:59-77, 79-103(无订阅), 105-222(无订阅), 390-425
 */
#include "r66_fixture.h"

#include "WtCore/WtCtaEngine.h"
#include "WtCore/WtSelEngine.h"

namespace {

using namespace r66;

const char* const kAg = "SHFE.ag.2512";

class R66SelHolder
{
public:
    R66SelHolder()
        : engine(new WtSelEngine())
    {
    }

    // WtSelEngine 析构读取未初始化的 _tm_ticker（BUG-R42-001），故意不回收
    ~R66SelHolder() = default;

    WtSelEngine* operator->() const { return engine; }
    WtSelEngine* get() const { return engine; }

private:
    WtSelEngine* engine;
};

//////////////////////////////////////////////////////////////////
// CTA: 空上下文 on_init / session 事件 / notify 空 notifier / getContext 未命中
// src/WtCore/WtCtaEngine.cpp:148-247, 704-720
//////////////////////////////////////////////////////////////////
TEST_F(R66Fixture, R66CtaEngineEmptyInitSessionAndNotify)
{
    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);

    EvtListenerFake listener;
    WtCtaEngine engine;
    engine.set_adapter_mgr(&_adapter_mgr);
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.regEventListener(&listener);

    engine.on_init();                 // 空 _ctx_map，只清缓存 + 通知监听者
    EXPECT_EQ(1u, listener.init);

    engine.on_session_begin();        // 空上下文，只通知监听者并置 _ready
    EXPECT_EQ(1u, listener.session);
    EXPECT_TRUE(listener.last_session_begin);

    engine.on_session_end();          // 首次结算写 funds.csv
    EXPECT_EQ(2u, listener.session);
    EXPECT_FALSE(listener.last_session_begin);

    engine.on_session_end();          // _last_date == _cur_tdate，走不再结算分支
    EXPECT_EQ(3u, listener.session);

    EXPECT_EQ(nullptr, engine.getContext(777).get());

    // 持仓增量入口：空过滤器 -> 建仓位、append_signal、save_datas
    engine.handle_pos_change("r66_cta", kAg, 2.0);

    // IExecuterStub 访问器（含品种不存在时 get_sess_info 返回 NULL）
    EXPECT_NE(nullptr, engine.get_comm_info(kAg));
    EXPECT_NE(nullptr, engine.get_sess_info(kAg));
    EXPECT_EQ(nullptr, engine.get_sess_info("SHFE.zz.9999"));
    (void)engine.get_real_time();

    // _notifier 为空时三个图表/成交通知都只做判空
    engine.notify_chart_marker(1ULL, "stra", 1.0, "icon", "tag");
    engine.notify_chart_index(1ULL, "stra", "idx", "line", 1.0);
    engine.notify_trade("stra", kAg, true, true, 1ULL, 1.0, "tag");
}

//////////////////////////////////////////////////////////////////
// CTA: on_tick 未 ready / ready 但无订阅
// src/WtCore/WtCtaEngine.cpp:490-518
//////////////////////////////////////////////////////////////////
TEST_F(R66Fixture, R66CtaEngineOnTickWithoutSubscriptions)
{
    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.set_adapter_mgr(&_adapter_mgr);
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine.set_date_time(20231010, 931, 0);
    engine.handle_push_quote(NULL);   // _tm_ticker == NULL

    WTSTickData* t1 = makeAgTick(3500.0);
    ASSERT_NE(nullptr, t1);
    engine.on_tick(kAg, t1);          // _ready == false
    t1->release();
    EXPECT_DOUBLE_EQ(3500.0, engine.get_cur_price(kAg));

    engine.on_session_begin();        // _ready = true
    WTSTickData* t2 = makeAgTick(3501.0);
    ASSERT_NE(nullptr, t2);
    engine.on_tick(kAg, t2);          // 无订阅 -> 提前 return
    t2->release();
    EXPECT_DOUBLE_EQ(3501.0, engine.get_cur_price(kAg));
}

//////////////////////////////////////////////////////////////////
// CTA: on_bar 空订阅表（分钟/日线两种日志分支）
// src/WtCore/WtCtaEngine.cpp:639-671
//////////////////////////////////////////////////////////////////
TEST_F(R66Fixture, R66CtaEngineOnBarWithoutSubscription)
{
    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);

    WtCtaEngine engine;
    engine.set_adapter_mgr(&_adapter_mgr);
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);

    WTSBarStruct bar = makeBar(20231010, 931, 3500.0);
    engine.on_bar(kAg, "m", 1, &bar);
    engine.on_bar(kAg, "d", 5, &bar);
    SUCCEED();
}

//////////////////////////////////////////////////////////////////
// HFT: 空订阅的推送处理器 / 空 ticker / 切片访问器 / 空 bar
// src/WtCore/WtHftEngine.cpp:116-189, 287-303, 337-374
//////////////////////////////////////////////////////////////////
TEST_F(R66Fixture, R66HftEngineEmptyPushHandlersAndSlices)
{
    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);

    WtHftEngine engine;
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);

    engine.handle_push_quote(NULL);   // _tm_ticker == NULL
    WTSTickData* quote = makeAgTick(3500.0);
    ASSERT_NE(nullptr, quote);
    engine.handle_push_quote(quote);  // ticker 仍为空，判空后返回
    quote->release();
    EXPECT_EQ(nullptr, engine.get_order_queue_slice(1, kAg, 10));
    EXPECT_EQ(nullptr, engine.get_order_detail_slice(1, kAg, 10));
    EXPECT_EQ(nullptr, engine.get_transaction_slice(1, kAg, 10));

    WTSOrdDtlData* odtl = WTSOrdDtlData::create(kAg);
    WTSOrdQueData* ordque = WTSOrdQueData::create(kAg);
    WTSTransData* trans = WTSTransData::create(kAg);
    ASSERT_NE(nullptr, odtl);
    ASSERT_NE(nullptr, ordque);
    ASSERT_NE(nullptr, trans);
    engine.handle_push_order_detail(odtl);   // 无订阅 -> find end
    engine.handle_push_order_queue(ordque);
    engine.handle_push_transaction(trans);
    odtl->release();
    ordque->release();
    trans->release();

    engine.on_minute_end(20231010, 931);     // HFT 空实现
    WTSBarStruct bar = makeBar(20231010, 931, 3500.0);
    engine.on_bar(kAg, "m", 1, &bar);        // 空订阅表
    SUCCEED();
}

//////////////////////////////////////////////////////////////////
// HFT: tick 原始/前复权/后复权订阅、bar 分发、session 事件
// src/WtCore/WtHftEngine.cpp:221-285, 287-303, 305-335, 347-360
//////////////////////////////////////////////////////////////////
TEST_F(R66Fixture, R66HftEngineDispatchesSubscriptions)
{
    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);

    R66HftEngine engine;
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    EvtListenerFake listener;
    engine.regEventListener(&listener);

    std::shared_ptr<R66HftCtx> c0(new R66HftCtx(&engine, "r66_hft_0"));
    std::shared_ptr<R66HftCtx> c1(new R66HftCtx(&engine, "r66_hft_1"));
    std::shared_ptr<R66HftCtx> c2(new R66HftCtx(&engine, "r66_hft_2"));
    engine.addContext(c0);
    engine.addContext(c1);
    engine.addContext(c2);

    engine.subTickOpt(c0->id(), kAg, 0);
    engine.subTickOpt(c1->id(), kAg, 1);
    engine.subTickOpt(c2->id(), kAg, 2);

    engine.on_session_begin();
    EXPECT_EQ(1u, listener.session);
    EXPECT_TRUE(listener.last_session_begin);
    EXPECT_EQ(1u, c0->session_begin);
    EXPECT_EQ(1u, c1->session_begin);
    EXPECT_EQ(1u, c2->session_begin);

    WTSTickData* tick = makeAgTick(3500.0);
    ASSERT_NE(nullptr, tick);
    engine.on_tick(kAg, tick);
    tick->release();

    EXPECT_EQ(1u, c0->ticks);
    EXPECT_EQ(1u, c1->ticks);
    EXPECT_EQ(1u, c2->ticks);
    EXPECT_EQ(std::string(kAg), c0->last_tick_code);
    EXPECT_EQ(std::string(kAg) + "-", c1->last_tick_code);
    EXPECT_EQ(std::string(kAg) + "+", c2->last_tick_code);
    const std::string hfqCode = std::string(kAg) + "+";
    EXPECT_DOUBLE_EQ(3500.0, engine.get_cur_price(hfqCode.c_str()));

    engine.subBar(c0->id(), kAg, "m", 1);
    WTSBarStruct bar = makeBar(20231010, 931, 3500.0);
    engine.on_bar(kAg, "m", 1, &bar);
    EXPECT_EQ(1u, c0->bars);
    EXPECT_EQ(std::string(kAg) + "-m", c0->last_bar_key);

    EXPECT_EQ(c0.get(), engine.getContext(c0->id()).get());
    EXPECT_EQ(nullptr, engine.getContext(9999).get());

    engine.on_session_end();
    EXPECT_EQ(2u, listener.session);
    EXPECT_FALSE(listener.last_session_begin);
    EXPECT_EQ(1u, c0->session_end);
    EXPECT_EQ(1u, c1->session_end);
    EXPECT_EQ(1u, c2->session_end);
}

//////////////////////////////////////////////////////////////////
// HFT: L2 订阅（含 +/- 后缀剥离）与分发
// src/WtCore/WtHftEngine.cpp:122-219
//////////////////////////////////////////////////////////////////
TEST_F(R66Fixture, R66HftEngineL2SubscriptionsAndDispatch)
{
    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);

    R66HftEngine engine;
    engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);

    std::shared_ptr<R66HftCtx> ctx(new R66HftCtx(&engine, "r66_hft_l2"));
    engine.addContext(ctx);
    const uint32_t sid = ctx->id();

    engine.sub_order_detail(sid, kAg);                       // 无后缀支
    engine.sub_order_queue(sid, (std::string(kAg) + "+").c_str());  // 剥离 '+' 支
    engine.sub_transaction(sid, kAg);

    WTSOrdDtlData* odtl = WTSOrdDtlData::create(kAg);
    WTSOrdQueData* ordque = WTSOrdQueData::create(kAg);
    WTSTransData* trans = WTSTransData::create(kAg);
    ASSERT_NE(nullptr, odtl);
    ASSERT_NE(nullptr, ordque);
    ASSERT_NE(nullptr, trans);

    engine.handle_push_order_detail(odtl);
    engine.handle_push_order_queue(ordque);
    engine.handle_push_transaction(trans);
    EXPECT_EQ(1u, ctx->orddtl);
    EXPECT_EQ(1u, ctx->ordque);
    EXPECT_EQ(1u, ctx->trans);
    EXPECT_EQ(std::string(kAg), ctx->last_detail_code);
    EXPECT_EQ(std::string(kAg), ctx->last_queue_code);
    EXPECT_EQ(std::string(kAg), ctx->last_trans_code);

    // 未订阅代码：三个处理器都只走 find end
    WTSOrdDtlData* other = WTSOrdDtlData::create("SHFE.ag.9999");
    ASSERT_NE(nullptr, other);
    engine.handle_push_order_detail(other);
    other->release();
    EXPECT_EQ(1u, ctx->orddtl);

    odtl->release();
    ordque->release();
    trans->release();
}

//////////////////////////////////////////////////////////////////
// SEL: 空上下文 addContext / session 事件 / 空 ticker 推送
// src/WtCore/WtSelEngine.cpp:59-83, 390-425
//////////////////////////////////////////////////////////////////
TEST_F(R66Fixture, R66SelEngineContextGuardsAndSessionEvents)
{
    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);

    R66SelHolder engine;
    engine->init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);

    EvtListenerFake listener;
    engine->regEventListener(&listener);

    engine->addContext(SelContextPtr(), 20231010, 931, TPT_None);
    EXPECT_EQ(nullptr, engine->getContext(9999).get());

    engine->on_init();
    EXPECT_EQ(1u, listener.init);

    engine->on_session_begin();
    EXPECT_EQ(1u, listener.session);
    EXPECT_TRUE(listener.last_session_begin);

    engine->on_session_end();
    EXPECT_EQ(2u, listener.session);
    EXPECT_FALSE(listener.last_session_begin);

    engine->handle_push_quote(NULL);  // _tm_ticker == NULL

    engine->handle_pos_change("r66_sel", kAg, 3.0);
    EXPECT_NE(nullptr, engine->get_comm_info(kAg));
    EXPECT_NE(nullptr, engine->get_sess_info(kAg));
    EXPECT_EQ(nullptr, engine->get_sess_info("SHFE.zz.9999"));
    (void)engine->get_real_time();
}

//////////////////////////////////////////////////////////////////
// SEL: on_tick 未 ready / ready 但无订阅、空 bar
// src/WtCore/WtSelEngine.cpp:85-222
//////////////////////////////////////////////////////////////////
TEST_F(R66Fixture, R66SelEngineOnTickWithoutSubscriptions)
{
    WTSVariant* cfg = makeCfg();
    ASSERT_NE(nullptr, cfg);

    R66SelHolder engine;
    engine->init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
    engine->set_date_time(20231010, 931, 0);

    WTSTickData* t1 = makeAgTick(3500.0);
    ASSERT_NE(nullptr, t1);
    engine->on_tick(kAg, t1);         // _ready == false
    t1->release();
    EXPECT_DOUBLE_EQ(3500.0, engine->get_cur_price(kAg));

    engine->on_session_begin();
    WTSTickData* t2 = makeAgTick(3501.0);
    ASSERT_NE(nullptr, t2);
    engine->on_tick(kAg, t2);         // 无订阅 -> 直接返回
    t2->release();
    EXPECT_DOUBLE_EQ(3501.0, engine->get_cur_price(kAg));

    WTSBarStruct bar = makeBar(20231010, 931, 3500.0);
    engine->on_bar(kAg, "m", 1, &bar);
    engine->on_bar(kAg, "d", 1, &bar);
    SUCCEED();
}

}  // namespace
