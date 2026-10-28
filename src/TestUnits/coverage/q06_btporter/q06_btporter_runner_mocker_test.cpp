/*
 * Q06 - WtBtPorter 回测侧分发层与 Exp*Mocker 包装
 *
 * 覆盖 src/WtBtPorter/WtBtRunner.cpp 的
 * registerCtaCallbacks / registerSelCallbacks / registerHftCallbacks（7/6/11 个槽位赋值）、
 * ctx_on_init / ctx_on_session_event / ctx_on_tick / ctx_on_calc / ctx_on_calc_done /
 * ctx_on_bar / ctx_on_cond_triggered（EngineType 路由 + default 分支）、
 * hft_on_order_queue / order_detail / transaction / channel_ready / entrust / order / trade、
 * loadFinalHisBars / loadRawHisBars / loadAllAdjFactors / loadAdjFactors /
 * loadRawHisTicks 的周期与参数映射、feedRawBars / feedRawTicks / feedAdjFactors 的保护分支；
 * WtBtRunner.h 的内联 on_initialize_event / on_session_event / on_schedule_event /
 * on_backtest_end（经 registerEvtCallback 注入探针）；
 * src/WtBtPorter/ExpCtaMocker.cpp（on_init / on_session_begin / on_session_end）、
 * src/WtBtPorter/ExpSelMocker.cpp（on_init / on_session_begin）、
 * src/WtBtPorter/ExpHftMocker.cpp（on_init）的事件转发契约。
 *
 * 只读生产代码；缺陷记录见 docs/迭代计划/迭代01/bugs/q06_porters.md。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "WtBtPorter/PorterDefs.h"
#include "WtBtPorter/WtBtRunner.h"
#include "WtBtPorter/ExpCtaMocker.h"
#include "WtBtPorter/ExpSelMocker.h"
#include "WtBtPorter/ExpHftMocker.h"

#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSStruct.h"

USING_NS_WTP;

extern WtBtRunner& getRunner();

namespace {

	struct BtProbe
	{
		uint32_t	cta_init = 0, hft_init = 0, sel_init = 0;
		uint32_t	cta_calc = 0, sel_calc = 0;
		uint32_t	cta_calc_done = 0, sel_calc_done = 0;
		uint32_t	cta_bar = 0, hft_bar = 0, sel_bar = 0;
		uint32_t	cta_tick = 0, hft_tick = 0, sel_tick = 0;
		uint32_t	sess = 0;
		uint32_t	cond = 0;
		uint32_t	ordque = 0, orddtl = 0, trans = 0;
		uint32_t	chnl = 0, ord = 0, trd = 0, entrust = 0;
		uint32_t	evt = 0;

		WtUInt32	last_handle = 0;
		WtUInt32	last_date = 0;
		WtUInt32	last_time = 0;
		WtUInt32	last_evt_id = 0;
		WtUInt32	last_sess_flag = 0;
		std::string	last_code;
		std::string	last_period;
		std::string	last_tag;
		double		p1 = 0, p2 = 0;

		void clear() { *this = BtProbe(); }
	};

	BtProbe g;

	void cta_init_cb(CtxHandler h) { g.cta_init++; g.last_handle = (WtUInt32)h; }
	void hft_init_cb(CtxHandler h) { g.hft_init++; g.last_handle = (WtUInt32)h; }
	void sel_init_cb(CtxHandler h) { g.sel_init++; g.last_handle = (WtUInt32)h; }

	void cta_tick_cb(CtxHandler h, const char* code, WTSTickStruct* tick)
	{
		g.cta_tick++;
		g.last_handle = (WtUInt32)h;
		g.last_code = code;
		g.p1 = tick->price;
	}

	void hft_tick_cb(CtxHandler /*h*/, const char* code, WTSTickStruct* tick)
	{
		g.hft_tick++;
		g.last_code = code;
		g.p1 = tick->price;
	}

	void sel_tick_cb(CtxHandler /*h*/, const char* code, WTSTickStruct* tick)
	{
		g.sel_tick++;
		g.last_code = code;
		g.p1 = tick->price;
	}

	void cta_calc_cb(CtxHandler h, WtUInt32 d, WtUInt32 t)
	{
		g.cta_calc++;
		g.last_handle = (WtUInt32)h;
		g.last_date = d;
		g.last_time = t;
	}

	void sel_calc_cb(CtxHandler /*h*/, WtUInt32 d, WtUInt32 t)
	{
		g.sel_calc++;
		g.last_date = d;
		g.last_time = t;
	}

	void cta_calc_done_cb(CtxHandler h, WtUInt32 d, WtUInt32 t)
	{
		g.cta_calc_done++;
		g.last_handle = (WtUInt32)h;
		g.last_date = d;
		g.last_time = t;
	}

	void sel_calc_done_cb(CtxHandler /*h*/, WtUInt32 d, WtUInt32 t)
	{
		g.sel_calc_done++;
		g.last_date = d;
		g.last_time = t;
	}

	void cta_bar_cb(CtxHandler h, const char* code, const char* period, WTSBarStruct* bar)
	{
		g.cta_bar++;
		g.last_handle = (WtUInt32)h;
		g.last_code = code;
		g.last_period = period;
		g.p1 = bar == NULL ? -1.0 : bar->close;
	}

	void hft_bar_cb(CtxHandler /*h*/, const char* code, const char* period, WTSBarStruct* /*bar*/)
	{
		g.hft_bar++;
		g.last_code = code;
		g.last_period = period;
	}

	void sel_bar_cb(CtxHandler /*h*/, const char* code, const char* period, WTSBarStruct* /*bar*/)
	{
		g.sel_bar++;
		g.last_code = code;
		g.last_period = period;
	}

	void sess_cb(CtxHandler h, WtUInt32 tdate, bool isBegin)
	{
		g.sess++;
		g.last_handle = (WtUInt32)h;
		g.last_date = tdate;
		g.last_sess_flag = isBegin ? 1 : 0;
	}

	void cond_cb(CtxHandler h, const char* code, double target, double price, const char* tag)
	{
		g.cond++;
		g.last_handle = (WtUInt32)h;
		g.last_code = code;
		g.p1 = target;
		g.p2 = price;
		g.last_tag = tag;
	}

	void chnl_cb(CtxHandler h, const char* /*trader*/, WtUInt32 evtid)
	{
		g.chnl++;
		g.last_handle = (WtUInt32)h;
		g.last_evt_id = evtid;
	}

	void ord_cb(CtxHandler /*h*/, WtUInt32 localid, const char* code, bool isBuy,
		double totalQty, double leftQty, double price, bool isCanceled, const char* tag)
	{
		g.ord++;
		g.last_date = localid;
		g.last_code = code;
		g.p1 = totalQty + leftQty + price;
		g.last_tag = tag;
		(void)isBuy; (void)isCanceled;
	}

	void trd_cb(CtxHandler /*h*/, WtUInt32 localid, const char* code, bool isBuy,
		double vol, double price, const char* /*tag*/)
	{
		g.trd++;
		g.last_date = localid;
		g.last_code = code;
		g.p1 = vol + price;
		(void)isBuy;
	}

	void entrust_cb(CtxHandler /*h*/, WtUInt32 localid, const char* code,
		bool bSuccess, const char* msg, const char* /*tag*/)
	{
		g.entrust++;
		g.last_date = localid;
		g.last_code = code;
		g.p1 = bSuccess ? 1.0 : 0.0;
		g.last_tag = msg;
	}

	void ordque_cb(CtxHandler /*h*/, const char* code, WTSOrdQueStruct* /*q*/) { g.ordque++; g.last_code = code; }
	void orddtl_cb(CtxHandler /*h*/, const char* code, WTSOrdDtlStruct* /*d*/) { g.orddtl++; g.last_code = code; }
	void trans_cb(CtxHandler /*h*/, const char* code, WTSTransStruct* /*t*/) { g.trans++; g.last_code = code; }

	void evt_cb(WtUInt32 evtId, WtUInt32 curDate, WtUInt32 curTime)
	{
		g.evt++;
		g.last_evt_id = evtId;
		g.last_date = curDate;
		g.last_time = curTime;
	}

	std::string g_load_code;
	std::string g_load_period;
	uint32_t g_load_date = 0;
	uint32_t g_fnl = 0, g_raw = 0, g_fct = 0, g_tick = 0;
	uint32_t g_feeder_bars = 0, g_feeder_fcts = 0, g_feeder_ticks = 0;
	bool g_ret = true;

	bool fnl_loader(const char* stdCode, const char* period)
	{
		g_fnl++; g_load_code = stdCode; g_load_period = period;
		return g_ret;
	}
	bool raw_loader(const char* stdCode, const char* period)
	{
		g_raw++; g_load_code = stdCode; g_load_period = period;
		return g_ret;
	}
	bool fct_loader(const char* stdCode)
	{
		g_fct++; g_load_code = stdCode;
		return g_ret;
	}
	bool tick_loader(const char* stdCode, uint32_t uDate)
	{
		g_tick++; g_load_code = stdCode; g_load_date = uDate;
		return g_ret;
	}

	void bars_feeder(void* /*obj*/, WTSBarStruct* /*bars*/, uint32_t count) { g_feeder_bars = count; }
	void fct_feeder(void* /*obj*/, const char* /*code*/, uint32_t* /*d*/, double* /*f*/, uint32_t count) { g_feeder_fcts = count; }
	void tick_feeder(void* /*obj*/, WTSTickStruct* /*t*/, uint32_t count) { g_feeder_ticks = count; }

	class Q06BtRunner : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			g.clear();
			g_fnl = g_raw = g_fct = g_tick = 0;
			g_feeder_bars = g_feeder_fcts = g_feeder_ticks = 0;
			g_load_code.clear();
			g_load_period.clear();
			g_ret = true;

			WtBtRunner& r = getRunner();
			r.registerEvtCallback(evt_cb);
			r.registerCtaCallbacks(cta_init_cb, cta_tick_cb, cta_calc_cb, cta_bar_cb,
				sess_cb, cta_calc_done_cb, cond_cb);
			r.registerSelCallbacks(sel_init_cb, sel_tick_cb, sel_calc_cb, sel_bar_cb,
				sess_cb, sel_calc_done_cb);
			r.registerHftCallbacks(hft_init_cb, hft_tick_cb, hft_bar_cb, chnl_cb, ord_cb, trd_cb,
				entrust_cb, orddtl_cb, ordque_cb, trans_cb, sess_cb);
			r.registerExtDataLoader(fnl_loader, raw_loader, fct_loader, tick_loader, false);
		}

		void TearDown() override
		{
			WtBtRunner& r = getRunner();
			r.registerEvtCallback(NULL);
			r.registerCtaCallbacks(NULL, NULL, NULL, NULL, NULL, NULL, NULL);
			r.registerSelCallbacks(NULL, NULL, NULL, NULL, NULL, NULL);
			r.registerHftCallbacks(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
			r.registerExtDataLoader(NULL, NULL, NULL, NULL, false);
			g.clear();
		}
	};

	WTSTickStruct makeBtTick()
	{
		WTSTickStruct tick;
		memset(&tick, 0, sizeof(tick));
		strncpy(tick.code, "au2406", sizeof(tick.code) - 1);
		strncpy(tick.exchg, "SHFE", sizeof(tick.exchg) - 1);
		tick.price = 456.78;
		return tick;
	}
}

/* 四个内联事件钩子各自把 evtId 与日期时间透传给 _cb_evt */
TEST_F(Q06BtRunner, EngineEventHooksForwardEvtIdAndTime)
{
	WtBtRunner& r = getRunner();
	r.on_initialize_event();
	EXPECT_EQ(g.evt, 1U);
	EXPECT_EQ(g.last_evt_id, EVENT_ENGINE_INIT);

	r.on_session_event(20240601, true);
	EXPECT_EQ(g.last_evt_id, EVENT_SESSION_BEGIN);
	EXPECT_EQ(g.last_date, 20240601U);

	r.on_session_event(20240601, false);
	EXPECT_EQ(g.last_evt_id, EVENT_SESSION_END);

	r.on_schedule_event(20240602, 153000);
	EXPECT_EQ(g.last_evt_id, EVENT_ENGINE_SCHDL);
	EXPECT_EQ(g.last_date, 20240602U);
	EXPECT_EQ(g.last_time, 153000U);

	r.on_backtest_end();
	EXPECT_EQ(g.last_evt_id, EVENT_BACKTEST_END);
	EXPECT_EQ(g.evt, 5U);
}

TEST_F(Q06BtRunner, CtxEventsRouteByEngineType)
{
	WtBtRunner& r = getRunner();
	r.ctx_on_init(11, ET_CTA);
	r.ctx_on_init(12, ET_HFT);
	r.ctx_on_init(13, ET_SEL);
	r.ctx_on_init(14, (EngineType)4242);
	EXPECT_EQ(g.cta_init, 1U);
	EXPECT_EQ(g.hft_init, 1U);
	EXPECT_EQ(g.sel_init, 1U);
	EXPECT_EQ(g.last_handle, 13U);	// default 分支没有触发任何回调

	r.ctx_on_calc(21, 20240601, 93000, ET_CTA);
	r.ctx_on_calc(22, 20240602, 15000, ET_SEL);
	r.ctx_on_calc(23, 20240603, 15000, ET_HFT);
	EXPECT_EQ(g.cta_calc, 1U);
	EXPECT_EQ(g.sel_calc, 1U);

	r.ctx_on_calc_done(31, 20240601, 93100, ET_CTA);
	r.ctx_on_calc_done(32, 20240602, 15100, ET_SEL);
	r.ctx_on_calc_done(33, 20240603, 15100, ET_HFT);
	EXPECT_EQ(g.cta_calc_done, 1U);
	EXPECT_EQ(g.sel_calc_done, 1U);
	EXPECT_EQ(g.last_time, 15100U);

	r.ctx_on_cond_triggered(41, "SHFE.au2406", 460.0, 459.5, "tag", ET_CTA);
	r.ctx_on_cond_triggered(42, "SHFE.au2406", 460.0, 459.5, "tag", ET_SEL);
	EXPECT_EQ(g.cond, 1U);
	EXPECT_DOUBLE_EQ(g.p1, 460.0);
	EXPECT_DOUBLE_EQ(g.p2, 459.5);
	EXPECT_EQ(g.last_tag, "tag");
}

TEST_F(Q06BtRunner, BarAndSessionAndTickDispatch)
{
	WtBtRunner& r = getRunner();
	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	bar.date = 20240601;
	bar.close = 12.34;

	r.ctx_on_bar(51, "SHFE.au2406", "m1", &bar, ET_CTA);
	EXPECT_EQ(g.cta_bar, 1U);
	EXPECT_EQ(g.last_period, "m1");
	EXPECT_DOUBLE_EQ(g.p1, 12.34);

	r.ctx_on_bar(52, "SSE.600000", "d1", NULL, ET_SEL);
	r.ctx_on_bar(53, "SSE.600000", "d1", NULL, ET_HFT);
	EXPECT_EQ(g.sel_bar, 1U);
	EXPECT_EQ(g.hft_bar, 1U);

	r.ctx_on_session_event(61, 20240601, true, ET_CTA);
	r.ctx_on_session_event(62, 20240601, false, ET_HFT);
	r.ctx_on_session_event(63, 20240601, false, ET_SEL);
	EXPECT_EQ(g.sess, 3U);
	EXPECT_EQ(g.last_handle, 63U);
	EXPECT_EQ(g.last_sess_flag, 0U);

	WTSTickStruct raw = makeBtTick();
	WTSTickData* tick = WTSTickData::create(raw);
	ASSERT_NE(tick, nullptr);
	r.ctx_on_tick(71, "SHFE.au2406", tick, ET_CTA);
	r.ctx_on_tick(72, "SHFE.au2406", tick, ET_HFT);
	r.ctx_on_tick(73, "SHFE.au2406", tick, ET_SEL);
	EXPECT_EQ(g.cta_tick, 1U);
	EXPECT_EQ(g.hft_tick, 1U);
	EXPECT_EQ(g.sel_tick, 1U);
	EXPECT_DOUBLE_EQ(g.p1, 456.78);
	tick->release();
}

TEST_F(Q06BtRunner, HftMicroAndTradeEventsPassThrough)
{
	WtBtRunner& r = getRunner();

	WTSOrdQueStruct que;
	memset(&que, 0, sizeof(que));
	WTSOrdQueData* queData = WTSOrdQueData::create(que);
	ASSERT_NE(queData, nullptr);
	r.hft_on_order_queue(81, "SHFE.au2406", queData);
	queData->release();
	EXPECT_EQ(g.ordque, 1U);

	WTSOrdDtlStruct dtl;
	memset(&dtl, 0, sizeof(dtl));
	WTSOrdDtlData* dtlData = WTSOrdDtlData::create(dtl);
	ASSERT_NE(dtlData, nullptr);
	r.hft_on_order_detail(82, "SHFE.au2406", dtlData);
	dtlData->release();
	EXPECT_EQ(g.orddtl, 1U);

	WTSTransStruct trn;
	memset(&trn, 0, sizeof(trn));
	WTSTransData* trnData = WTSTransData::create(trn);
	ASSERT_NE(trnData, nullptr);
	r.hft_on_transaction(83, "SHFE.au2406", trnData);
	trnData->release();
	EXPECT_EQ(g.trans, 1U);

	r.hft_on_channel_ready(84, "CTP");
	EXPECT_EQ(g.chnl, 1U);
	// WtBtRunner.cpp 里传的是字面量 1000（WtBtPorter/PorterDefs.h 未定义 CHNL_EVENT_READY）
	EXPECT_EQ(g.last_evt_id, 1000U);

	r.hft_on_entrust(85, 1001, "SHFE.au2406", true, "accepted", "tag");
	EXPECT_EQ(g.entrust, 1U);
	EXPECT_DOUBLE_EQ(g.p1, 1.0);
	EXPECT_EQ(g.last_tag, "accepted");

	r.hft_on_order(86, 1002, "SHFE.au2406", true, 1.0, 2.0, 3.0, false, "tag");
	EXPECT_EQ(g.ord, 1U);
	EXPECT_DOUBLE_EQ(g.p1, 6.0);

	r.hft_on_trade(87, 1003, "SHFE.au2406", false, 4.0, 5.0, "tag");
	EXPECT_EQ(g.trd, 1U);
	EXPECT_DOUBLE_EQ(g.p1, 9.0);
}

TEST_F(Q06BtRunner, ExtLoaderMapsPeriodAndDateArgs)
{
	WtBtRunner& r = getRunner();
	int obj = 7;

	EXPECT_TRUE(r.loadFinalHisBars(&obj, "SSE.600000", KP_DAY, bars_feeder));
	EXPECT_EQ(g_fnl, 1U);
	EXPECT_EQ(g_load_period, "d1");
	EXPECT_TRUE(r.loadFinalHisBars(&obj, "SSE.600000", KP_Minute1, bars_feeder));
	EXPECT_EQ(g_load_period, "m1");
	EXPECT_TRUE(r.loadFinalHisBars(&obj, "SSE.600000", KP_Minute5, bars_feeder));
	EXPECT_EQ(g_load_period, "m5");

	EXPECT_TRUE(r.loadRawHisBars(&obj, "SSE.600519", KP_Minute5, bars_feeder));
	EXPECT_EQ(g_raw, 1U);
	EXPECT_EQ(g_load_period, "m5");
	EXPECT_EQ(g_load_code, "SSE.600519");

	const WTSKlinePeriod bogus = (WTSKlinePeriod)91;
	EXPECT_FALSE(r.loadFinalHisBars(&obj, "SSE.600000", bogus, bars_feeder));
	EXPECT_FALSE(r.loadRawHisBars(&obj, "SSE.600000", bogus, bars_feeder));
	EXPECT_EQ(g_fnl, 3U);
	EXPECT_EQ(g_raw, 1U);

	g_ret = false;
	EXPECT_FALSE(r.loadFinalHisBars(&obj, "SSE.600000", KP_DAY, bars_feeder));
	g_ret = true;

	EXPECT_TRUE(r.loadAllAdjFactors(&obj, fct_feeder));
	EXPECT_EQ(g_load_code, "");
	EXPECT_TRUE(r.loadAdjFactors(&obj, "SSE.600519", fct_feeder));
	EXPECT_EQ(g_fct, 2U);
	EXPECT_EQ(g_load_code, "SSE.600519");

	EXPECT_TRUE(r.loadRawHisTicks(&obj, "SSE.600000", 20240601, tick_feeder));
	EXPECT_EQ(g_tick, 1U);
	EXPECT_EQ(g_load_date, 20240601U);
}

TEST_F(Q06BtRunner, FeedHelpersUseCachedFeeders)
{
	WtBtRunner& r = getRunner();
	int obj = 7;
	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	WTSTickStruct tick;
	memset(&tick, 0, sizeof(tick));
	uint32_t dates[2] = { 20240601, 20240602 };
	double factors[2] = { 1.0, 1.2 };

	ASSERT_TRUE(r.loadFinalHisBars(&obj, "SSE.600000", KP_DAY, bars_feeder));
	r.feedRawBars(&bar, 2);
	EXPECT_EQ(g_feeder_bars, 2U);

	ASSERT_TRUE(r.loadRawHisTicks(&obj, "SSE.600000", 20240601, tick_feeder));
	r.feedRawTicks(&tick, 3);
	EXPECT_EQ(g_feeder_ticks, 3U);

	ASSERT_TRUE(r.loadAdjFactors(&obj, "SSE.600000", fct_feeder));
	r.feedAdjFactors("SSE.600000", dates, factors, 2);
	EXPECT_EQ(g_feeder_fcts, 2U);
}

TEST(Q06BtRunnerNoLoader, AllLoadAndFeedPathsRefuse)
{
	WtBtRunner& r = getRunner();
	r.registerExtDataLoader(NULL, NULL, NULL, NULL, false);
	r.registerEvtCallback(NULL);

	int obj = 0;
	EXPECT_FALSE(r.loadFinalHisBars(&obj, "SSE.600000", KP_DAY, bars_feeder));
	EXPECT_FALSE(r.loadRawHisBars(&obj, "SSE.600000", KP_DAY, bars_feeder));
	EXPECT_FALSE(r.loadAllAdjFactors(&obj, fct_feeder));
	EXPECT_FALSE(r.loadAdjFactors(&obj, "SSE.600000", fct_feeder));
	EXPECT_FALSE(r.loadRawHisTicks(&obj, "SSE.600000", 20240601, tick_feeder));

	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	WTSTickStruct tick;
	memset(&tick, 0, sizeof(tick));
	uint32_t dates[1] = { 20240601 };
	double factors[1] = { 1.0 };
	r.feedRawBars(&bar, 1);
	r.feedRawTicks(&tick, 1);
	r.feedAdjFactors("SSE.600000", dates, factors, 1);

	// _cb_evt 为 NULL 时四个内联事件钩子应当静默返回
	r.on_initialize_event();
	r.on_session_event(20240601, true);
	r.on_schedule_event(20240601, 93000);
	r.on_backtest_end();
	SUCCEED();
}

/*
 * Exp*Mocker 包装层：构造只写入 id，on_init / 交易日事件必须把
 * _context_id 透传到注册的回调，并额外触发一次引擎级事件。
 */
TEST_F(Q06BtRunner, ExpMockersForwardLifecycleToCallbacks)
{
	WtBtRunner& r = getRunner();
	HisDataReplayer* replayer = &r.replayer();

	ExpCtaMocker cta(replayer, "q06_bt_cta", 0, false, NULL, false);
	EXPECT_GT(cta.id(), 0U);
	cta.on_init();
	EXPECT_EQ(g.cta_init, 1U);
	EXPECT_EQ(g.last_handle, cta.id());
	EXPECT_EQ(g.evt, 1U);			// on_initialize_event
	EXPECT_EQ(g.last_evt_id, EVENT_ENGINE_INIT);

	cta.on_session_begin(20240601);
	EXPECT_EQ(g.sess, 1U);
	EXPECT_EQ(g.last_date, 20240601U);
	EXPECT_EQ(g.last_sess_flag, 1U);
	EXPECT_EQ(g.evt, 2U);

	cta.on_session_end(20240601);
	EXPECT_EQ(g.sess, 2U);
	EXPECT_EQ(g.last_sess_flag, 0U);
	EXPECT_EQ(g.evt, 3U);

	ExpSelMocker sel(replayer, "q06_bt_sel", 0, false);
	sel.on_init();
	EXPECT_EQ(g.sel_init, 1U);
	EXPECT_EQ(g.last_handle, sel.id());
	sel.on_session_begin(20240602);
	EXPECT_EQ(g.sess, 3U);
	EXPECT_EQ(g.last_date, 20240602U);
	EXPECT_EQ(g.evt, 5U);		// init + session_begin 各带一次引擎级事件

	ExpHftMocker hft(replayer, "q06_bt_hft");
	hft.on_init();
	EXPECT_EQ(g.hft_init, 1U);
	EXPECT_EQ(g.last_handle, hft.id());
	EXPECT_EQ(g.evt, 6U);
}

/* 未注册任何回调时，Mocker 的转发生命周期不得触碰空函数指针 */
TEST_F(Q06BtRunner, ExpMockerLifecycleIsSafeWithoutCallbacks)
{
	WtBtRunner& r = getRunner();
	r.registerCtaCallbacks(NULL, NULL, NULL, NULL, NULL, NULL, NULL);
	r.registerSelCallbacks(NULL, NULL, NULL, NULL, NULL, NULL);
	r.registerHftCallbacks(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
	r.registerEvtCallback(NULL);

	HisDataReplayer* replayer = &r.replayer();
	ExpCtaMocker cta(replayer, "q06_bt_cta_nocb", 0, false, NULL, false);
	cta.on_init();
	cta.on_session_begin(20240601);
	cta.on_session_end(20240601);

	ExpSelMocker sel(replayer, "q06_bt_sel_nocb", 0, false);
	sel.on_init();

	ExpHftMocker hft(replayer, "q06_bt_hft_nocb");
	hft.on_init();

	// 未订阅的合约：ExpCtaMocker::on_tick_updated 首段即返回
	EXPECT_EQ(g.cta_init, 0U);
	EXPECT_EQ(g.sess, 0U);
	SUCCEED();
}

/*
 * 跳过目标（详见 manifest）：
 * - WtBtRunner::init/config/run/stop/release：读配置、装载存储、起线程并 join。
 * - ExpCtaMocker::on_tick_updated / on_bar_close / on_calculate / on_calculate_done /
 *   on_bactest_end / on_condition_triggered：依赖已装载的行情与落盘目录。
 * - WtBtRunner::initCtaMocker/initSelMocker/initHftMocker：register_sink/register_task
 *   依赖交易时段表与代码表。
 */
TEST_F(Q06BtRunner, DISABLED_BacktestRunPathsNeedStorage)
{
	EXPECT_TRUE(true);
}
