/*
 * Q06 - WtRtRunner 实盘侧分发层：回调路由、句柄表、外部数据加载器映射
 *
 * 覆盖 src/WtPorter/WtRtRunner.cpp 的 registerXxx 六个注册函数、
 * ctx_on_init / ctx_on_session_event / ctx_on_tick / ctx_on_calc / ctx_on_bar /
 * ctx_on_cond_triggered（按 EngineType 分派 + default 分支）、
 * hft_on_channel_ready / lost / order / trade / entrust / position /
 * order_queue / order_detail / transaction、
 * parser_init / connect / disconnect / release / subscribe / unsubscribe、
 * executer_init / executer_set_position、
 * loadFinalHisBars / loadRawHisBars / loadAllAdjFactors / loadAdjFactors 的
 * 周期映射（d1/m1/m5 + 不支持周期的 default 分支）与未注册即返回 false、
 * feedRawBars 的两条保护分支、
 * createCtaContext / createSelContext / createHftContext 与
 * getCtaContext / getSelContext / getHftContext 的句柄表增删查。
 *
 * 只读生产代码；缺陷记录见 docs/迭代计划/迭代01/bugs/q06_porters.md。
 * 禁止进入 run()/config()/release()/createExtParser()，见文末 DISABLED_ 用例。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "WtPorter/PorterDefs.h"
#include "WtPorter/WtPorter.h"
#include "WtPorter/WtRtRunner.h"

#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSStruct.h"

USING_NS_WTP;

extern WtRtRunner& getRunner();

namespace {

	struct Recorder
	{
		uint32_t	init_cta = 0, init_hft = 0, init_sel = 0;
		uint32_t	calc_cta = 0, calc_sel = 0;
		uint32_t	bar_cta = 0, bar_hft = 0, bar_sel = 0;
		uint32_t	tick_cta = 0, tick_hft = 0, tick_sel = 0;
		uint32_t	sess_cta = 0, sess_hft = 0, sess_sel = 0;
		uint32_t	cond_cta = 0;
		uint32_t	chnl_ready = 0, chnl_lost = 0;
		uint32_t	ord = 0, trd = 0, entrust = 0, pos = 0;
		uint32_t	ordque = 0, orddtl = 0, trans = 0;
		uint32_t	parser_evt = 0, parser_sub = 0;
		uint32_t	exec_init = 0, exec_cmd = 0;

		WtUInt32		last_evt = 0;
		WtUInt32		last_date = 0;
		WtUInt32		last_time = 0;
		WtUInt32		last_sess_flag = 0;
		std::string		last_code;
		std::string		last_period;
		std::string		last_tag;
		std::string		last_msg;
		std::string		last_id;
		double			d1 = 0, d2 = 0, d3 = 0, d4 = 0;
		bool			b1 = false;
		bool			b2 = false;

		double			last_price_seen = 0;
		uint32_t		last_actime_seen = 0;

		std::string		bar_period;
		void clear() { *this = Recorder(); }
	};

	Recorder g;

	void cta_init_cb(CtxHandler h) { g.init_cta++; g.last_evt = (WtUInt32)h; }
	void hft_init_cb(CtxHandler h) { g.init_hft++; g.last_evt = (WtUInt32)h; }
	void sel_init_cb(CtxHandler h) { g.init_sel++; g.last_evt = (WtUInt32)h; }

	void cta_tick_cb(CtxHandler h, const char* code, WTSTickStruct* tick)
	{
		g.tick_cta++;
		g.last_evt = (WtUInt32)h;
		g.last_code = code;
		g.last_price_seen = tick->price;
		g.last_actime_seen = tick->action_time;
	}

	void hft_tick_cb(CtxHandler /*h*/, const char* code, WTSTickStruct* tick)
	{
		g.tick_hft++;
		g.last_code = code;
		g.last_price_seen = tick->price;
	}

	void sel_tick_cb(CtxHandler /*h*/, const char* code, WTSTickStruct* tick)
	{
		g.tick_sel++;
		g.last_code = code;
		g.last_price_seen = tick->price;
	}

	void cta_calc_cb(CtxHandler h, WtUInt32 d, WtUInt32 t)
	{
		g.calc_cta++;
		g.last_evt = (WtUInt32)h;
		g.last_date = d;
		g.last_time = t;
	}

	void sel_calc_cb(CtxHandler /*h*/, WtUInt32 d, WtUInt32 t)
	{
		g.calc_sel++;
		g.last_date = d;
		g.last_time = t;
	}

	void cta_bar_cb(CtxHandler h, const char* code, const char* period, WTSBarStruct* bar)
	{
		g.bar_cta++;
		g.last_evt = (WtUInt32)h;
		g.last_code = code;
		g.bar_period = period;
		g.d1 = bar == NULL ? -1.0 : bar->close;
	}

	void hft_bar_cb(CtxHandler /*h*/, const char* code, const char* period, WTSBarStruct* /*bar*/)
	{
		g.bar_hft++;
		g.last_code = code;
		g.bar_period = period;
	}

	void sel_bar_cb(CtxHandler /*h*/, const char* code, const char* period, WTSBarStruct* /*bar*/)
	{
		g.bar_sel++;
		g.last_code = code;
		g.bar_period = period;
	}

	void sess_evt_cb(CtxHandler h, WtUInt32 tdate, bool isBegin)
	{
		g.sess_cta++;
		g.last_evt = (WtUInt32)h;
		g.last_date = tdate;
		g.last_sess_flag = isBegin ? 1 : 0;
	}

	void cond_trigger_cb(CtxHandler h, const char* code, double target, double price, const char* tag)
	{
		g.cond_cta++;
		g.last_evt = (WtUInt32)h;
		g.last_code = code;
		g.d1 = target;
		g.d2 = price;
		g.last_tag = tag;
	}

	void chnl_cb(CtxHandler h, const char* trader, WtUInt32 evtid)
	{
		g.last_evt = (WtUInt32)h;
		g.last_id = trader;
		if (evtid == CHNL_EVENT_READY)
			g.chnl_ready++;
		else if (evtid == CHNL_EVENT_LOST)
			g.chnl_lost++;
	}

	void ord_cb(CtxHandler /*h*/, WtUInt32 localid, const char* code, bool isBuy,
		double totalQty, double leftQty, double price, bool isCanceled, const char* tag)
	{
		g.ord++;
		g.last_evt = localid;
		g.last_code = code;
		g.b1 = isBuy;
		g.d1 = totalQty;
		g.d2 = leftQty;
		g.d3 = price;
		g.b2 = isCanceled;
		g.last_tag = tag;
	}

	void trd_cb(CtxHandler /*h*/, WtUInt32 localid, const char* code, bool isBuy,
		double vol, double price, const char* tag)
	{
		g.trd++;
		g.last_evt = localid;
		g.last_code = code;
		g.b1 = isBuy;
		g.d1 = vol;
		g.d2 = price;
		g.last_tag = tag;
	}

	void entrust_cb(CtxHandler /*h*/, WtUInt32 localid, const char* code,
		bool bSuccess, const char* msg, const char* tag)
	{
		g.entrust++;
		g.last_evt = localid;
		g.last_code = code;
		g.b1 = bSuccess;
		g.last_msg = msg;
		g.last_tag = tag;
	}

	void pos_cb(CtxHandler /*h*/, const char* code, bool isLong,
		double prevol, double preavail, double newvol, double newavail)
	{
		g.pos++;
		g.last_code = code;
		g.b1 = isLong;
		g.d1 = prevol;
		g.d2 = preavail;
		g.d3 = newvol;
		g.d4 = newavail;
	}

	void ordque_cb(CtxHandler /*h*/, const char* code, WTSOrdQueStruct* /*ordQue*/)
	{
		g.ordque++;
		g.last_code = code;
	}

	void orddtl_cb(CtxHandler /*h*/, const char* code, WTSOrdDtlStruct* /*ordDtl*/)
	{
		g.orddtl++;
		g.last_code = code;
	}

	void trans_cb(CtxHandler /*h*/, const char* code, WTSTransStruct* /*trans*/)
	{
		g.trans++;
		g.last_code = code;
	}

	void parser_evt_cb(WtUInt32 evtId, const char* id)
	{
		g.parser_evt++;
		g.last_evt = evtId;
		g.last_id = id;
	}

	void parser_sub_cb(const char* id, const char* fullCode, bool isForSub)
	{
		g.parser_sub++;
		g.last_id = id;
		g.last_code = fullCode;
		g.b1 = isForSub;
	}

	void exec_init_cb(const char* id) { g.exec_init++; g.last_id = id; }
	void exec_cmd_cb(const char* id, const char* code, double target)
	{
		g.exec_cmd++;
		g.last_id = id;
		g.last_code = code;
		g.d1 = target;
	}

	/* 外部数据加载器：记录 (stdCode, period) 便于断言周期映射 */
	std::string g_load_code;
	std::string g_load_period;
	uint32_t g_fnl_calls = 0, g_raw_calls = 0, g_fct_calls = 0;
	bool g_load_ret = true;

	bool fnl_bar_loader(const char* stdCode, const char* period)
	{
		g_fnl_calls++;
		g_load_code = stdCode;
		g_load_period = period;
		return g_load_ret;
	}

	bool raw_bar_loader(const char* stdCode, const char* period)
	{
		g_raw_calls++;
		g_load_code = stdCode;
		g_load_period = period;
		return g_load_ret;
	}

	bool adj_fct_loader(const char* stdCode)
	{
		g_fct_calls++;
		g_load_code = stdCode;
		return g_load_ret;
	}

	// FuncReadBars / FuncReadFactors 落地回调（feedRawBars / feedAdjFactors 的目标）
	WTSBarStruct* g_feeder_bar_ptr = NULL;
	uint32_t g_feeder_bar_cnt = 0;
	uint32_t g_feeder_fct_cnt = 0;

	void bars_feeder(void* obj, WTSBarStruct* bars, uint32_t count)
	{
		g_feeder_bar_ptr = obj == NULL ? bars : NULL;
		g_feeder_bar_cnt = count;
	}

	void factors_feeder(void* obj, const char* stdCode, uint32_t* dates, double* factors, uint32_t count)
	{
		(void)obj;
		(void)stdCode;
		(void)dates;
		(void)factors;
		g_feeder_fct_cnt = count;
	}

	class Q06PorterRunner : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			g.clear();
			g_fnl_calls = g_raw_calls = g_fct_calls = 0;
			g_load_code.clear();
			g_load_period.clear();
			g_feeder_bar_ptr = NULL;
			g_feeder_bar_cnt = 0;
			g_feeder_fct_cnt = 0;
			g_load_ret = true;

			WtRtRunner& r = getRunner();
			r.registerCtaCallbacks(cta_init_cb, cta_tick_cb, cta_calc_cb, cta_bar_cb, sess_evt_cb, cond_trigger_cb);
			r.registerSelCallbacks(sel_init_cb, sel_tick_cb, sel_calc_cb, sel_bar_cb, sess_evt_cb);
			r.registerHftCallbacks(hft_init_cb, hft_tick_cb, hft_bar_cb, chnl_cb, ord_cb, trd_cb,
				entrust_cb, orddtl_cb, ordque_cb, trans_cb, sess_evt_cb, pos_cb);
			r.registerParserPorter(parser_evt_cb, parser_sub_cb);
			r.registerExecuterPorter(exec_init_cb, exec_cmd_cb);
			r.registerEvtCallback(NULL);
			r.registerExtDataLoader(fnl_bar_loader, raw_bar_loader, adj_fct_loader, NULL);
		}

		void TearDown() override
		{
			// 全部回调复位为 NULL，保证同一进程后续用例零残留
			WtRtRunner& r = getRunner();
			r.registerCtaCallbacks(NULL, NULL, NULL, NULL, NULL, NULL);
			r.registerSelCallbacks(NULL, NULL, NULL, NULL, NULL);
			r.registerHftCallbacks(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
			r.registerParserPorter(NULL, NULL);
			r.registerExecuterPorter(NULL, NULL);
			r.registerExtDataLoader(NULL, NULL, NULL, NULL);
			g.clear();
		}
	};

	WTSTickStruct makeTick()
	{
		WTSTickStruct tick;
		memset(&tick, 0, sizeof(tick));
		strncpy(tick.code, "au2406", sizeof(tick.code) - 1);
		strncpy(tick.exchg, "SHFE", sizeof(tick.exchg) - 1);
		tick.price = 456.78;
		tick.action_date = 20240601;
		tick.action_time = 93000;
		return tick;
	}

	WTSBarStruct makeBar()
	{
		WTSBarStruct bar;
		memset(&bar, 0, sizeof(bar));
		bar.date = 20240601;
		bar.time = 93000;
		bar.close = 456.78;
		bar.hold = 1000;
		return bar;
	}
}

TEST_F(Q06PorterRunner, InitEventRoutesByEngineType)
{
	WtRtRunner& r = getRunner();
	r.ctx_on_init(11, ET_CTA);
	r.ctx_on_init(12, ET_HFT);
	r.ctx_on_init(13, ET_SEL);
	r.ctx_on_init(14, (EngineType)12345);	// 未定义取值走 default 分支

	EXPECT_EQ(g.init_cta, 1U);
	EXPECT_EQ(g.init_hft, 1U);
	EXPECT_EQ(g.init_sel, 1U);
	EXPECT_EQ(g.last_evt, 13U);	// 最后一次被调用的是 SEL
}

TEST_F(Q06PorterRunner, CalcEventOnlyHasCtaAndSelSlots)
{
	WtRtRunner& r = getRunner();
	r.ctx_on_calc(21, 20240601, 93000, ET_CTA);
	r.ctx_on_calc(22, 20240602, 15000, ET_SEL);
	r.ctx_on_calc(23, 20240603, 15000, ET_HFT);	// HFT 无 calc 槽位
	r.ctx_on_calc(24, 20240603, 15000, (EngineType)7);

	EXPECT_EQ(g.calc_cta, 1U);
	EXPECT_EQ(g.calc_sel, 1U);
	EXPECT_EQ(g.last_date, 20240602U);
	EXPECT_EQ(g.last_time, 15000U);
}

TEST_F(Q06PorterRunner, BarEventCarriesCodePeriodAndBar)
{
	WtRtRunner& r = getRunner();
	WTSBarStruct bar = makeBar();
	r.ctx_on_bar(31, "SHFE.au2406", "m1", &bar, ET_CTA);
	EXPECT_EQ(g.bar_cta, 1U);
	EXPECT_EQ(g.last_code, "SHFE.au2406");
	EXPECT_EQ(g.bar_period, "m1");
	EXPECT_DOUBLE_EQ(g.d1, 456.78);

	r.ctx_on_bar(32, "SSE.600000", "d1", NULL, ET_SEL);
	EXPECT_EQ(g.bar_sel, 1U);
	r.ctx_on_bar(33, "SHFE.au2406", "m5", NULL, ET_HFT);
	EXPECT_EQ(g.bar_hft, 1U);
	r.ctx_on_bar(34, "SHFE.au2406", "m5", NULL, (EngineType)8);
	EXPECT_EQ(g.bar_cta + g.bar_hft + g.bar_sel, 3U);
}

TEST_F(Q06PorterRunner, SessionEventPassesBeginEndFlag)
{
	WtRtRunner& r = getRunner();
	r.ctx_on_session_event(41, 20240601, true, ET_CTA);
	EXPECT_EQ(g.sess_cta, 1U);
	EXPECT_EQ(g.last_date, 20240601U);
	EXPECT_EQ(g.last_sess_flag, 1U);

	r.ctx_on_session_event(41, 20240601, false, ET_CTA);
	EXPECT_EQ(g.sess_cta, 2U);
	EXPECT_EQ(g.last_sess_flag, 0U);

	// SEL/HFT 复用同一个 session 回调槽位（注册时都传 sess_evt_cb）
	r.ctx_on_session_event(42, 20240602, true, ET_SEL);
	r.ctx_on_session_event(43, 20240602, true, ET_HFT);
	EXPECT_EQ(g.sess_cta, 4U);
	EXPECT_EQ(g.last_evt, 43U);
}

TEST_F(Q06PorterRunner, TickEventUnwrapsTickStruct)
{
	WtRtRunner& r = getRunner();
	WTSTickStruct raw = makeTick();
	WTSTickData* tick = WTSTickData::create(raw);
	ASSERT_NE(tick, nullptr);

	r.ctx_on_tick(51, "SHFE.au2406", tick, ET_CTA);
	EXPECT_EQ(g.tick_cta, 1U);
	EXPECT_EQ(g.last_code, "SHFE.au2406");
	EXPECT_DOUBLE_EQ(g.last_price_seen, 456.78);
	EXPECT_EQ(g.last_actime_seen, 93000U);

	r.ctx_on_tick(52, "SHFE.au2406", tick, ET_HFT);
	r.ctx_on_tick(53, "SHFE.au2406", tick, ET_SEL);
	r.ctx_on_tick(54, "SHFE.au2406", tick, (EngineType)99);
	EXPECT_EQ(g.tick_hft, 1U);
	EXPECT_EQ(g.tick_sel, 1U);

	tick->release();
}

TEST_F(Q06PorterRunner, CondTriggerOnlyRoutedForCta)
{
	WtRtRunner& r = getRunner();
	r.ctx_on_cond_triggered(61, "SHFE.au2406", 460.0, 459.5, "q06_tag", ET_CTA);
	EXPECT_EQ(g.cond_cta, 1U);
	EXPECT_DOUBLE_EQ(g.d1, 460.0);
	EXPECT_DOUBLE_EQ(g.d2, 459.5);
	EXPECT_EQ(g.last_tag, "q06_tag");

	r.ctx_on_cond_triggered(62, "SHFE.au2406", 460.0, 459.5, "x", ET_SEL);
	EXPECT_EQ(g.cond_cta, 1U);
}

TEST_F(Q06PorterRunner, HftChannelAndTradeEventsPassThrough)
{
	WtRtRunner& r = getRunner();
	r.hft_on_channel_ready(71, "CTP_Q06");
	r.hft_on_channel_lost(71, "CTP_Q06");
	EXPECT_EQ(g.chnl_ready, 1U);
	EXPECT_EQ(g.chnl_lost, 1U);
	EXPECT_EQ(g.last_id, "CTP_Q06");

	r.hft_on_order(72, 1001, "SHFE.au2406", true, 5.0, 2.0, 456.5, false, "tag1");
	EXPECT_EQ(g.ord, 1U);
	EXPECT_EQ(g.last_evt, 1001U);
	EXPECT_TRUE(g.b1);
	EXPECT_FALSE(g.b2);
	EXPECT_DOUBLE_EQ(g.d1, 5.0);
	EXPECT_DOUBLE_EQ(g.d2, 2.0);
	EXPECT_DOUBLE_EQ(g.d3, 456.5);

	r.hft_on_trade(73, 1002, "SHFE.au2406", false, 3.0, 457.0, "tag2");
	EXPECT_EQ(g.trd, 1U);
	EXPECT_FALSE(g.b1);
	EXPECT_DOUBLE_EQ(g.d1, 3.0);
	EXPECT_DOUBLE_EQ(g.d2, 457.0);

	r.hft_on_entrust(74, 1003, "SHFE.au2406", true, "ok", "tag3");
	EXPECT_EQ(g.entrust, 1U);
	EXPECT_EQ(g.last_msg, "ok");

	r.hft_on_position(75, "SHFE.au2406", true, 1.0, 1.0, 3.0, 3.0);
	EXPECT_EQ(g.pos, 1U);
	EXPECT_DOUBLE_EQ(g.d3, 3.0);
	EXPECT_DOUBLE_EQ(g.d4, 3.0);
}

TEST_F(Q06PorterRunner, HftMicroDataEventsPassThrough)
{
	WtRtRunner& r = getRunner();

	WTSOrdQueStruct que;
	memset(&que, 0, sizeof(que));
	strncpy(que.code, "au2406", sizeof(que.code) - 1);
	WTSOrdQueData* queData = WTSOrdQueData::create(que);
	ASSERT_NE(queData, nullptr);
	r.hft_on_order_queue(81, "SHFE.au2406", queData);
	EXPECT_EQ(g.ordque, 1U);
	queData->release();

	WTSOrdDtlStruct dtl;
	memset(&dtl, 0, sizeof(dtl));
	strncpy(dtl.code, "au2406", sizeof(dtl.code) - 1);
	WTSOrdDtlData* dtlData = WTSOrdDtlData::create(dtl);
	ASSERT_NE(dtlData, nullptr);
	r.hft_on_order_detail(82, "SHFE.au2406", dtlData);
	EXPECT_EQ(g.orddtl, 1U);
	dtlData->release();

	WTSTransStruct trn;
	memset(&trn, 0, sizeof(trn));
	strncpy(trn.code, "au2406", sizeof(trn.code) - 1);
	WTSTransData* trnData = WTSTransData::create(trn);
	ASSERT_NE(trnData, nullptr);
	r.hft_on_transaction(83, "SHFE.au2406", trnData);
	EXPECT_EQ(g.trans, 1U);
	trnData->release();
}

TEST_F(Q06PorterRunner, ParserAndExecuterEventsUseRegisteredIds)
{
	WtRtRunner& r = getRunner();
	r.parser_init("q06p");
	r.parser_connect("q06p");
	r.parser_disconnect("q06p");
	r.parser_release("q06p");
	EXPECT_EQ(g.parser_evt, 4U);
	EXPECT_EQ(g.last_id, "q06p");
	EXPECT_EQ(g.last_evt, EVENT_PARSER_RELEASE);

	r.parser_subscribe("q06p", "SHFE.au2406");
	EXPECT_TRUE(g.b1);
	r.parser_unsubscribe("q06p", "SHFE.cu2406");
	EXPECT_FALSE(g.b1);
	EXPECT_EQ(g.parser_sub, 2U);

	r.executer_init("q06e");
	r.executer_set_position("q06e", "SHFE.au2406", 4.0);
	EXPECT_EQ(g.exec_init, 1U);
	EXPECT_EQ(g.exec_cmd, 1U);
	EXPECT_EQ(g.last_id, "q06e");
	EXPECT_DOUBLE_EQ(g.d1, 4.0);
}

TEST_F(Q06PorterRunner, ExtDataLoaderMapsPeriodToShortCode)
{
	WtRtRunner& r = getRunner();
	int feedObj = 0;

	EXPECT_TRUE(r.loadFinalHisBars(&feedObj, "SSE.600000", KP_DAY, bars_feeder));
	EXPECT_EQ(g_fnl_calls, 1U);
	EXPECT_EQ(g_load_period, "d1");
	EXPECT_EQ(g_load_code, "SSE.600000");

	EXPECT_TRUE(r.loadFinalHisBars(&feedObj, "SSE.600000", KP_Minute1, bars_feeder));
	EXPECT_EQ(g_load_period, "m1");
	EXPECT_TRUE(r.loadFinalHisBars(&feedObj, "SSE.600000", KP_Minute5, bars_feeder));
	EXPECT_EQ(g_load_period, "m5");

	EXPECT_TRUE(r.loadRawHisBars(&feedObj, "SSE.600000", KP_DAY, bars_feeder));
	EXPECT_EQ(g_raw_calls, 1U);
	EXPECT_EQ(g_load_period, "d1");
	EXPECT_TRUE(r.loadRawHisBars(&feedObj, "SSE.600000", KP_Minute5, bars_feeder));
	EXPECT_EQ(g_load_period, "m5");

	// loader 返回值直接透传
	g_load_ret = false;
	EXPECT_FALSE(r.loadFinalHisBars(&feedObj, "SSE.600000", KP_Minute1, bars_feeder));
	g_load_ret = true;

	EXPECT_TRUE(r.loadAllAdjFactors(&feedObj, factors_feeder));
	EXPECT_EQ(g_fct_calls, 1U);
	EXPECT_EQ(g_load_code, "");

	EXPECT_TRUE(r.loadAdjFactors(&feedObj, "SSE.600519", factors_feeder));
	EXPECT_EQ(g_fct_calls, 2U);
	EXPECT_EQ(g_load_code, "SSE.600519");
}

TEST_F(Q06PorterRunner, UnsupportedPeriodHitsDefaultBranch)
{
	WtRtRunner& r = getRunner();
	int feedObj = 0;
	const WTSKlinePeriod bogus = (WTSKlinePeriod)77;

	EXPECT_FALSE(r.loadFinalHisBars(&feedObj, "SSE.600000", bogus, bars_feeder));
	EXPECT_EQ(g_fnl_calls, 0U);
	EXPECT_FALSE(r.loadRawHisBars(&feedObj, "SSE.600000", bogus, bars_feeder));
	EXPECT_EQ(g_raw_calls, 0U);
}

TEST_F(Q06PorterRunner, FeedRawBarsUsesCachedFeeder)
{
	WtRtRunner& r = getRunner();
	int feedObj = 42;
	WTSBarStruct bar = makeBar();

	// loadFinalHisBars 会把 bars_feeder 缓存进 _feeder_bars
	ASSERT_TRUE(r.loadFinalHisBars(&feedObj, "SSE.600000", KP_DAY, bars_feeder));
	r.feedRawBars(&bar, 1);
	EXPECT_EQ(g_feeder_bar_cnt, 1U);
	EXPECT_EQ(g_feeder_bar_ptr, nullptr);	// obj 非空 → 走 obj 分支，bars 不外泄

	r.feedAdjFactors("SSE.600000", NULL, NULL, 7);
	EXPECT_EQ(g_feeder_fct_cnt, 7U);
}

TEST(Q06PorterRunnerNoLoader, LoaderFreePathsReturnFalseOrWarn)
{
	WtRtRunner& r = getRunner();
	r.registerExtDataLoader(NULL, NULL, NULL, NULL);

	int feedObj = 0;
	EXPECT_FALSE(r.loadFinalHisBars(&feedObj, "SSE.600000", KP_DAY, bars_feeder));
	EXPECT_FALSE(r.loadRawHisBars(&feedObj, "SSE.600000", KP_Minute1, bars_feeder));
	EXPECT_FALSE(r.loadAllAdjFactors(&feedObj, factors_feeder));
	EXPECT_FALSE(r.loadAdjFactors(&feedObj, "SSE.600000", factors_feeder));

	// feedRawBars 在未注册 loader 时只打错误日志并返回
	r.feedRawBars(NULL, 0);
	WTSBarStruct bar = makeBar();
	r.feedRawBars(&bar, 1);
	SUCCEED();
}

/* 句柄表：create*Context 只写入 map，不起线程 */
TEST(Q06PorterRunnerCtxTable, CreatedContextsAreFindableAndUnknownIdsMiss)
{
	WtRtRunner& r = getRunner();
	r.registerCtaCallbacks(NULL, NULL, NULL, NULL, NULL, NULL);
	r.registerSelCallbacks(NULL, NULL, NULL, NULL, NULL);
	r.registerHftCallbacks(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);

	const uint32_t ctaId = r.createCtaContext("q06_cta_ctx", 2);
	EXPECT_GT(ctaId, 0U);
	CtaContextPtr cta = r.getCtaContext(ctaId);
	ASSERT_NE(cta, nullptr);
	EXPECT_EQ(cta->id(), ctaId);
	EXPECT_STREQ(cta->name(), "q06_cta_ctx");
	EXPECT_EQ(cta->stra_get_position("SHFE.au2406", false, ""), 0.0);

	const uint32_t selId = r.createSelContext("q06_sel_ctx", 20240601, 153000, "d", 0);
	SelContextPtr sel = r.getSelContext(selId);
	ASSERT_NE(sel, nullptr);
	EXPECT_GT(selId, 0U);
	EXPECT_EQ(sel->name(), std::string("q06_sel_ctx"));

	// 未知周期串走 TPT_None 分支，不应抛异常
	const uint32_t selId2 = r.createSelContext("q06_sel_bogus", 20240601, 153000, "not-a-period", 0);
	EXPECT_NE(r.getSelContext(selId2), nullptr);

	// trader 未注册 → 记录 error 后仍然返回句柄（绑通道失败的分支）
	const uint32_t hftId = r.createHftContext("q06_hft_ctx", "TRADER_NOT_EXISTS", false, 1);
	HftContextPtr hft = r.getHftContext(hftId);
	ASSERT_NE(hft, nullptr);
	EXPECT_EQ(hft->id(), hftId);

	// 未注册的句柄一律查不到
	EXPECT_EQ(r.getCtaContext(ctaId + 999999), nullptr);
	EXPECT_EQ(r.getSelContext(selId + 999999), nullptr);
	EXPECT_EQ(r.getHftContext(hftId + 999999), nullptr);
}

/*
 * 崩溃/挂死类目标，整类收进同一个 DISABLED_，启用用例零接触：
 * - run()/config()/init()/release()：起线程并 join 或等待条件变量（HANG）
 * - createExtParser()/createExtExecuter()：解引用未初始化的 _engine（空指针）
 * - get_raw_stdcode()：同上；对应导出入口 cta_get_price / cta_get_tdate 等同理
 * - ExpCtaContext::on_init() 等事件覆写：依赖已装载的策略与数据管理器
 * - stra_sub_ticks / cta_sub_ticks：进入 WtCtaEngine::get_data_mgr() 的未初始化路径
 * - reg_cta_factories / reg_hft_factories / reg_sel_factories / reg_exe_factories：
 *   依赖真实 .so 目录扫描（DLLHelper），留待独立夹具
 */
TEST(Q06PorterRunner, DISABLED_BlockingAndNullEnginePaths)
{
	WtRtRunner& r = getRunner();
	(void)r.getEngine();
	EXPECT_TRUE(true);
}
