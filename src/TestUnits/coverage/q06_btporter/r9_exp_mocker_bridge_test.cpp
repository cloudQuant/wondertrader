/*
 * R9 - WtBtPorter 第七波：Exp*Mocker 包装层参数换算 + WtBtRunner.h 内联接缝 + 早退分支
 *
 * 在 q06 的两个文件之上加深（复用同一套「C 导出注册 → runner 分发」思路，夹具自建不冲突）：
 * A) src/WtBtPorter/WtBtPorter.cpp L51-54 / L67-71 / L83-87 / L104-109 / L120-123
 *    五个 register_* C 导出的接线（SetUp 就走 C 导出，本文件用显式用例再钉一次），
 *    以及 L174-185 config_backtest 的早退；
 * B) src/WtBtPorter/WtBtRunner.h L122-125 isAutoTrans / L510-539 五个内联 getter /
 *    L548-596 四个内联事件钩子（q06 已覆盖，这里换一组输入值再证一次期望可推导）；
 * C) src/WtBtPorter/WtBtRunner.cpp L449-462 config 的两个早退、L626-634 initEvtNotifier 拒绝分支；
 * D) src/WtBtPorter/ExpHftMocker.cpp L14-111 全部包装（q06 只打了 on_init）：
 *    on_bar 的周期重算 L16-27、on_channel_ready L30-35、on_entrust L37-42、on_order L69-74、
 *    on_trade L101-106、on_session_begin/end L53-67、on_tick_updated L76-84、
 *    on_ordque/orddtl/trans_updated L86-99、on_bactest_end L108-111；
 * E) src/WtBtPorter/ExpCtaMocker.cpp L113-204、src/WtBtPorter/ExpSelMocker.cpp L86-157。
 *
 * 订阅位 _tick_subs 是 CtaMocker.h:786/1045、SelMocker.h:566/836、HftMocker.h:1216/1258 的
 * protected 成员，用测试本地派生类直接 insert，不使用 #define private public。
 * 不起线程、不跑 run/config 成功路径（见文末 DISABLED_）；缺陷见 docs/迭代计划/迭代01/bugs/R9_wtbtporter.md。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "WtBtPorter/PorterDefs.h"
#include "WtBtPorter/WtBtPorter.h"
#include "WtBtPorter/WtBtRunner.h"
#include "WtBtPorter/ExpCtaMocker.h"
#include "WtBtPorter/ExpSelMocker.h"
#include "WtBtPorter/ExpHftMocker.h"

#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/WTSVariant.hpp"

USING_NS_WTP;

extern WtBtRunner& getRunner();

namespace {

	struct R9Probe
	{
		uint32_t	cta_init = 0, sel_init = 0, hft_init = 0;
		uint32_t	cta_bar = 0, sel_bar = 0, hft_bar = 0;
		uint32_t	cta_tick = 0, sel_tick = 0, hft_tick = 0;
		uint32_t	cta_calc = 0, sel_calc = 0;
		uint32_t	cta_calc_done = 0, sel_calc_done = 0;
		uint32_t	sess = 0, cond = 0;
		uint32_t	chnl = 0, ord = 0, trd = 0, entrust = 0;
		uint32_t	ordque = 0, orddtl = 0, trans = 0;
		uint32_t	evt = 0;

		WtUInt32	last_handle = 0;
		WtUInt32	last_date = 0;
		WtUInt32	last_time = 0;
		WtUInt32	sess_date = 0;
		WtUInt32	sess_flag = 0;
		WtUInt32	evt_id = 0;
		WtUInt32	evt_date = 0;
		WtUInt32	evt_time = 0;
		WtUInt32	evt_chnl = 0;
		bool		last_bar_null = false;
		std::string	last_code;
		std::string	last_period;
		std::string	last_str;
		double		p1 = 0, p2 = 0;

		void clear() { *this = R9Probe(); }
	};

	R9Probe g9;

	void r9_evt_cb(WtUInt32 evtId, WtUInt32 curDate, WtUInt32 curTime)
	{
		g9.evt++;
		g9.evt_id = evtId;
		g9.evt_date = curDate;
		g9.evt_time = curTime;
	}

	void r9_init_cb(CtxHandler h) { g9.last_handle = (WtUInt32)h; }
	void r9_cta_init_cb(CtxHandler h) { g9.cta_init++; r9_init_cb(h); }
	void r9_sel_init_cb(CtxHandler h) { g9.sel_init++; r9_init_cb(h); }
	void r9_hft_init_cb(CtxHandler h) { g9.hft_init++; r9_init_cb(h); }

	void r9_cta_tick_cb(CtxHandler h, const char* code, WTSTickStruct* tick)
	{
		g9.cta_tick++;
		r9_init_cb(h);
		g9.last_code = code;
		g9.p1 = tick->price;
	}

	void r9_sel_tick_cb(CtxHandler h, const char* code, WTSTickStruct* tick)
	{
		g9.sel_tick++;
		r9_init_cb(h);
		g9.last_code = code;
		g9.p1 = tick->price;
	}

	void r9_hft_tick_cb(CtxHandler h, const char* code, WTSTickStruct* tick)
	{
		g9.hft_tick++;
		r9_init_cb(h);
		g9.last_code = code;
		g9.p1 = tick->price;
	}

	void r9_cta_calc_cb(CtxHandler h, WtUInt32 d, WtUInt32 t)
	{
		g9.cta_calc++;
		r9_init_cb(h);
		g9.last_date = d;
		g9.last_time = t;
	}

	void r9_sel_calc_cb(CtxHandler h, WtUInt32 d, WtUInt32 t)
	{
		g9.sel_calc++;
		r9_init_cb(h);
		g9.last_date = d;
		g9.last_time = t;
	}

	void r9_cta_calc_done_cb(CtxHandler h, WtUInt32 d, WtUInt32 t)
	{
		g9.cta_calc_done++;
		r9_init_cb(h);
		g9.last_date = d;
		g9.last_time = t;
	}

	void r9_cta_bar_cb(CtxHandler h, const char* code, const char* period, WTSBarStruct* bar)
	{
		g9.cta_bar++;
		r9_init_cb(h);
		g9.last_code = code;
		g9.last_period = period;
		g9.last_bar_null = (bar == NULL);
		g9.p1 = bar == NULL ? -1.0 : bar->close;
	}

	void r9_sel_bar_cb(CtxHandler h, const char* code, const char* period, WTSBarStruct* bar)
	{
		g9.sel_bar++;
		r9_init_cb(h);
		g9.last_code = code;
		g9.last_period = period;
		g9.p1 = bar == NULL ? -1.0 : bar->close;
	}

	void r9_hft_bar_cb(CtxHandler h, const char* code, const char* period, WTSBarStruct* bar)
	{
		g9.hft_bar++;
		r9_init_cb(h);
		g9.last_code = code;
		g9.last_period = period;
		g9.last_bar_null = (bar == NULL);
		g9.p1 = bar == NULL ? -1.0 : bar->close;
	}

	void r9_sess_cb(CtxHandler h, WtUInt32 tdate, bool isBegin)
	{
		g9.sess++;
		r9_init_cb(h);
		g9.sess_date = tdate;
		g9.sess_flag = isBegin ? 1 : 0;
	}

	void r9_cond_cb(CtxHandler h, const char* code, double target, double price, const char* tag)
	{
		g9.cond++;
		r9_init_cb(h);
		g9.last_code = code;
		g9.p1 = target;
		g9.p2 = price;
		g9.last_str = tag;
	}

	void r9_chnl_cb(CtxHandler h, const char* trader, WtUInt32 evtid)
	{
		g9.chnl++;
		r9_init_cb(h);
		g9.last_str = trader;
		g9.evt_chnl = evtid;
	}

	void r9_ord_cb(CtxHandler h, WtUInt32 localid, const char* code, bool isBuy,
		double totalQty, double leftQty, double price, bool isCanceled, const char* tag)
	{
		g9.ord++;
		r9_init_cb(h);
		g9.last_date = localid;
		g9.last_code = code;
		g9.p1 = totalQty + leftQty + price;
		g9.p2 = (isBuy ? 1.0 : 0.0) + (isCanceled ? 8.0 : 0.0);
		g9.last_str = tag;
	}

	void r9_trd_cb(CtxHandler h, WtUInt32 localid, const char* code, bool isBuy,
		double vol, double price, const char* tag)
	{
		g9.trd++;
		r9_init_cb(h);
		g9.last_date = localid;
		g9.last_code = code;
		g9.p1 = vol + price;
		g9.p2 = isBuy ? 1.0 : 0.0;
		g9.last_str = tag;
	}

	void r9_entrust_cb(CtxHandler h, WtUInt32 localid, const char* code,
		bool bSuccess, const char* msg, const char* tag)
	{
		g9.entrust++;
		r9_init_cb(h);
		g9.last_date = localid;
		g9.last_code = code;
		g9.p1 = bSuccess ? 1.0 : 0.0;
		g9.last_str = msg;
		g9.p2 = (tag == NULL) ? -1.0 : 0.0;
	}

	void r9_ordque_cb(CtxHandler h, const char* code, WTSOrdQueStruct* /*item*/)
	{
		g9.ordque++;
		r9_init_cb(h);
		g9.last_code = code;
	}

	void r9_orddtl_cb(CtxHandler h, const char* code, WTSOrdDtlStruct* /*item*/)
	{
		g9.orddtl++;
		r9_init_cb(h);
		g9.last_code = code;
	}

	void r9_trans_cb(CtxHandler h, const char* code, WTSTransStruct* /*item*/)
	{
		g9.trans++;
		r9_init_cb(h);
		g9.last_code = code;
	}

	bool r9_fnl_loader(const char* /*stdCode*/, const char* /*period*/) { return true; }
	bool r9_raw_loader(const char* /*stdCode*/, const char* /*period*/) { return true; }
	bool r9_fct_loader(const char* /*stdCode*/) { return true; }
	bool r9_tick_loader(const char* /*stdCode*/, uint32_t /*uDate*/) { return true; }
	void r9_bars_feeder(void* /*obj*/, WTSBarStruct* /*bars*/, uint32_t /*count*/) { }
	void r9_fcts_feeder(void* /*obj*/, const char* /*code*/, uint32_t* /*d*/, double* /*f*/, uint32_t /*count*/) { }
	void r9_ticks_feeder(void* /*obj*/, WTSTickStruct* /*t*/, uint32_t /*count*/) { }

	/* 只借用 protected 的订阅表，不触碰任何 private 成员，不重定义访问说明符 */
	class SubbedCta : public ExpCtaMocker
	{
	public:
		SubbedCta(HisDataReplayer* replayer, const char* name)
			: ExpCtaMocker(replayer, name, 0, false, NULL, false) { }
		void sub_tick_locally(const char* stdCode) { _tick_subs.insert(stdCode); }
	};

	class SubbedSel : public ExpSelMocker
	{
	public:
		SubbedSel(HisDataReplayer* replayer, const char* name)
			: ExpSelMocker(replayer, name, 0, false) { }
		void sub_tick_locally(const char* stdCode) { _tick_subs.insert(stdCode); }
	};

	class SubbedHft : public ExpHftMocker
	{
	public:
		SubbedHft(HisDataReplayer* replayer, const char* name)
			: ExpHftMocker(replayer, name) { }
		void sub_tick_locally(const char* stdCode) { _tick_subs.insert(stdCode); }
	};

	WTSTickStruct makeR9Tick(const char* stdCode, double price)
	{
		WTSTickStruct tick;
		memset(&tick, 0, sizeof(tick));
		strncpy(tick.code, stdCode, sizeof(tick.code) - 1);
		strncpy(tick.exchg, "SSE", sizeof(tick.exchg) - 1);
		tick.price = price;
		return tick;
	}

	class R9Wired : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			g9.clear();
			// 通过 WtBtPorter.cpp 的 C 导出接线，而不是直接调 runner
			register_evt_callback(r9_evt_cb);
			register_cta_callbacks(r9_cta_init_cb, r9_cta_tick_cb, r9_cta_calc_cb,
				r9_cta_bar_cb, r9_sess_cb, r9_cta_calc_done_cb, r9_cond_cb);
			register_sel_callbacks(r9_sel_init_cb, r9_sel_tick_cb, r9_sel_calc_cb,
				r9_sel_bar_cb, r9_sess_cb, NULL);
			register_hft_callbacks(r9_hft_init_cb, r9_hft_tick_cb, r9_hft_bar_cb,
				r9_chnl_cb, r9_ord_cb, r9_trd_cb, r9_entrust_cb,
				r9_orddtl_cb, r9_ordque_cb, r9_trans_cb, r9_sess_cb);
			register_ext_data_loader(r9_fnl_loader, r9_raw_loader, r9_fct_loader,
				r9_tick_loader, true);
		}

		void TearDown() override
		{
			register_evt_callback(NULL);
			register_cta_callbacks(NULL, NULL, NULL, NULL, NULL, NULL, NULL);
			register_sel_callbacks(NULL, NULL, NULL, NULL, NULL, NULL);
			register_hft_callbacks(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
			register_ext_data_loader(NULL, NULL, NULL, NULL, false);
			g9.clear();
		}
	};
}

/* 五个 register_* C 导出必须把槽位写进同一个全局 runner（WtBtPorter.cpp:51/67/83/104/120） */
TEST_F(R9Wired, RegisterExportsWireRunnerSlots)
{
	WtBtRunner& r = getRunner();
	g9.clear();

	r.ctx_on_init(77, ET_CTA);
	EXPECT_EQ(g9.cta_init, 1U);
	EXPECT_EQ(g9.last_handle, 77U);

	r.on_initialize_event();
	EXPECT_EQ(g9.evt, 1U);
	EXPECT_EQ(g9.evt_id, (uint32_t)EVENT_ENGINE_INIT);

	EXPECT_TRUE(r.loadFinalHisBars(&r, "SSE.600000", KP_DAY, r9_bars_feeder));
	EXPECT_TRUE(r.loadRawHisTicks(&r, "SSE.600000", 20240601, r9_ticks_feeder));
	EXPECT_TRUE(r.loadAllAdjFactors(&r, r9_fcts_feeder));
	EXPECT_TRUE(r.isAutoTrans());
}

/* register_ext_data_loader 的第 5 个参数逐次覆盖 _loader_auto_trans（WtBtRunner.h:217-224） */
TEST_F(R9Wired, LoaderAutoTransFlagFollowsLastRegistration)
{
	WtBtRunner& r = getRunner();
	EXPECT_TRUE(r.isAutoTrans());

	register_ext_data_loader(r9_fnl_loader, r9_raw_loader, r9_fct_loader, r9_tick_loader, false);
	EXPECT_FALSE(r.isAutoTrans());

	register_ext_data_loader(r9_fnl_loader, r9_raw_loader, r9_fct_loader, r9_tick_loader, true);
	EXPECT_TRUE(r.isAutoTrans());
}

/* 未跑回测前四个 mocker/replayer 内联 getter 的状态（WtBtRunner.h:510-539，构造列表 L82-87） */
TEST_F(R9Wired, MockersAreNullAndAsyncFalseBeforeInit)
{
	WtBtRunner& r = getRunner();
	EXPECT_EQ(r.cta_mocker(), nullptr);
	EXPECT_EQ(r.sel_mocker(), nullptr);
	EXPECT_EQ(r.hft_mocker(), nullptr);
	EXPECT_FALSE(r.isAsync());
}

/* replayer() 返回固定成员的引用，两次取址必须相同（WtBtRunner.h:531，成员 _replayer 在 L639） */
TEST_F(R9Wired, ReplayerAccessorIsStableReference)
{
	WtBtRunner& r = getRunner();
	HisDataReplayer& first = r.replayer();
	HisDataReplayer& second = r.replayer();
	EXPECT_EQ(&first, &second);
	EXPECT_NE(&first, (HisDataReplayer*)NULL);
}

/* config 走 load_from_file 的空文件早退（WtBtRunner.cpp:449-462），两个 mocker 槽位保持为空 */
TEST_F(R9Wired, ConfigBacktestMissingFileLeavesMockersNull)
{
	WtBtRunner& r = getRunner();
	config_backtest("/tmp/wt_r9_no_such_config_9a71.yaml", true);
	EXPECT_EQ(r.cta_mocker(), nullptr);
	EXPECT_EQ(r.sel_mocker(), nullptr);
	EXPECT_EQ(r.hft_mocker(), nullptr);

	config_backtest("/tmp/wt_r9_no_such_config_9a71.json", true);
	EXPECT_EQ(r.cta_mocker(), nullptr);
}

/* initEvtNotifier 的两个拒绝分支：NULL 与非对象变体（WtBtRunner.cpp:626-630） */
TEST_F(R9Wired, InitEvtNotifierRejectsNullAndNonObject)
{
	WtBtRunner& r = getRunner();
	EXPECT_FALSE(r.initEvtNotifier(NULL));

	WTSVariant* notObj = WTSVariant::createArray();
	ASSERT_NE(notObj, nullptr);
	EXPECT_NE(notObj->type(), WTSVariant::VT_Object);
	EXPECT_FALSE(r.initEvtNotifier(notObj));
	notObj->release();
}

/* ExpHftMocker::on_bar 的 newBar==NULL 直接返回，既不转周期也不外发（ExpHftMocker.cpp:16-17） */
TEST_F(R9Wired, HftBarNullGuardSkipsDispatch)
{
	SubbedHft hft(&getRunner().replayer(), "r9_hft_bar_null");
	hft.on_bar("SHFE.au2406", "m1", 1, NULL);
	EXPECT_EQ(g9.hft_bar, 0U);
	EXPECT_EQ(g9.evt, 0U);
}

/*
 * 周期重算：period[0]=='d' 走 "{}{}"（"d1"+3 → "d13"），
 * 否则一律 "m{times}"（"m1"+5 → "m5"，"w1"+2 → "m2"）——ExpHftMocker.cpp:19-23
 */
TEST_F(R9Wired, HftBarPeriodRenamedByTimes)
{
	SubbedHft hft(&getRunner().replayer(), "r9_hft_bar_p");
	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	bar.close = 88.5;

	hft.on_bar("SHFE.au2406", "m1", 5, &bar);
	ASSERT_EQ(g9.hft_bar, 1U);
	EXPECT_EQ(g9.last_period, "m5");
	EXPECT_EQ(g9.last_code, "SHFE.au2406");
	EXPECT_EQ(g9.last_handle, hft.id());
	EXPECT_DOUBLE_EQ(g9.p1, 88.5);

	hft.on_bar("SHFE.au2406", "d1", 3, &bar);
	ASSERT_EQ(g9.hft_bar, 2U);
	EXPECT_EQ(g9.last_period, "d13");

	hft.on_bar("SHFE.au2406", "w1", 2, &bar);
	ASSERT_EQ(g9.hft_bar, 3U);
	EXPECT_EQ(g9.last_period, "m2");
}

/* on_tick_updated 先看订阅表，未订阅不得外发；订阅后把 tick 透传给 ctx_on_tick（ExpHftMocker.cpp:76-84） */
TEST_F(R9Wired, HftTickNeedsSubscription)
{
	SubbedHft hft(&getRunner().replayer(), "r9_hft_tick");
	WTSTickStruct raw = makeR9Tick("600000", 12.75);
	WTSTickData* tick = WTSTickData::create(raw);
	ASSERT_NE(tick, nullptr);

	hft.on_tick_updated("SSE.600000", tick);
	EXPECT_EQ(g9.hft_tick, 0U);

	hft.sub_tick_locally("SSE.600000");
	hft.on_tick_updated("SSE.600000", tick);
	ASSERT_EQ(g9.hft_tick, 1U);
	EXPECT_EQ(g9.last_handle, hft.id());
	EXPECT_DOUBLE_EQ(g9.p1, 12.75);

	tick->release();
}

/* on_channel_ready 外发时 trader 固定是空串，事件号是 runner 里的字面量 1000（ExpHftMocker.cpp:30-35） */
TEST_F(R9Wired, HftChannelReadyDispatch)
{
	SubbedHft hft(&getRunner().replayer(), "r9_hft_chnl");
	hft.on_channel_ready();
	ASSERT_EQ(g9.chnl, 1U);
	EXPECT_EQ(g9.last_handle, hft.id());
	EXPECT_EQ(g9.last_str, "");
	EXPECT_EQ(g9.evt_chnl, 1000U);
}

/* on_entrust/on_order/on_trade 三个回报口必须原样透传实参（ExpHftMocker.cpp:37-42/69-74/101-106） */
TEST_F(R9Wired, HftEntrustOrderTradeArgsPassthrough)
{
	SubbedHft hft(&getRunner().replayer(), "r9_hft_rpt");

	hft.on_entrust(4101, "SHFE.au2406", false, "rejected by mock", "etag");
	ASSERT_EQ(g9.entrust, 1U);
	EXPECT_EQ(g9.last_handle, hft.id());
	EXPECT_EQ(g9.last_date, 4101U);
	EXPECT_DOUBLE_EQ(g9.p1, 0.0);
	EXPECT_EQ(g9.last_str, "rejected by mock");

	g9.clear();
	hft.on_order(4102, "SHFE.au2406", true, 3.0, 1.0, 100.0, false, "otag");
	ASSERT_EQ(g9.ord, 1U);
	EXPECT_EQ(g9.last_date, 4102U);
	EXPECT_DOUBLE_EQ(g9.p1, 104.0);		// 3 + 1 + 100
	EXPECT_DOUBLE_EQ(g9.p2, 1.0);			// isBuy=true, isCanceled=false

	g9.clear();
	hft.on_trade(4103, "SHFE.cu2406", false, 2.0, 70.25, "ttag");
	ASSERT_EQ(g9.trd, 1U);
	EXPECT_EQ(g9.last_date, 4103U);
	EXPECT_EQ(g9.last_code, "SHFE.cu2406");
	EXPECT_DOUBLE_EQ(g9.p1, 72.25);		// 2 + 70.25
	EXPECT_DOUBLE_EQ(g9.p2, 0.0);
}

/* 交易日事件同时打 ctx_on_session_event(ET_HFT) 和全局事件（ExpHftMocker.cpp:53-67） */
TEST_F(R9Wired, HftSessionBeginEndFireCtxAndGlobal)
{
	SubbedHft hft(&getRunner().replayer(), "r9_hft_sess");

	hft.on_session_begin(20240611);
	ASSERT_EQ(g9.sess, 1U);
	EXPECT_EQ(g9.last_handle, hft.id());
	EXPECT_EQ(g9.sess_date, 20240611U);
	EXPECT_EQ(g9.sess_flag, 1U);
	ASSERT_EQ(g9.evt, 1U);
	EXPECT_EQ(g9.evt_id, (uint32_t)EVENT_SESSION_BEGIN);
	EXPECT_EQ(g9.evt_date, 20240611U);

	hft.on_session_end(20240612);
	ASSERT_EQ(g9.sess, 2U);
	EXPECT_EQ(g9.sess_flag, 0U);
	EXPECT_EQ(g9.sess_date, 20240612U);
	ASSERT_EQ(g9.evt, 2U);
	EXPECT_EQ(g9.evt_id, (uint32_t)EVENT_SESSION_END);
	EXPECT_EQ(g9.evt_date, 20240612U);
}

/*
 * 微观结构三个口不看订阅表、也不调父类：任意代码都会外发（ExpHftMocker.cpp:86-99）
 * 与 on_tick_updated 的订阅前置不一致，见缺陷 R9-003。
 */
TEST_F(R9Wired, HftMicroStructIgnoresSubscription)
{
	SubbedHft hft(&getRunner().replayer(), "r9_hft_micro");
	hft.sub_tick_locally("SSE.600000");

	WTSOrdQueStruct que;
	memset(&que, 0, sizeof(que));
	WTSOrdQueData* queData = WTSOrdQueData::create(que);
	ASSERT_NE(queData, nullptr);
	hft.on_ordque_updated("SHFE.rb2410", queData);
	ASSERT_EQ(g9.ordque, 1U);
	EXPECT_EQ(g9.last_handle, hft.id());
	EXPECT_EQ(g9.last_code, "SHFE.rb2410");

	WTSOrdDtlStruct dtl;
	memset(&dtl, 0, sizeof(dtl));
	WTSOrdDtlData* dtlData = WTSOrdDtlData::create(dtl);
	ASSERT_NE(dtlData, nullptr);
	hft.on_orddtl_updated("SHFE.rb2410", dtlData);
	ASSERT_EQ(g9.orddtl, 1U);
	EXPECT_EQ(g9.last_code, "SHFE.rb2410");

	WTSTransStruct trn;
	memset(&trn, 0, sizeof(trn));
	WTSTransData* trnData = WTSTransData::create(trn);
	ASSERT_NE(trnData, nullptr);
	hft.on_trans_updated("SHFE.rb2410", trnData);
	ASSERT_EQ(g9.trans, 1U);
	EXPECT_EQ(g9.last_code, "SHFE.rb2410");

	queData->release();
	dtlData->release();
	trnData->release();
}

/* on_bactest_end 只打全局回测结束事件，不动任何 ctx 槽位（ExpHftMocker.cpp:108-111） */
TEST_F(R9Wired, HftBacktestEndOnlyGlobal)
{
	SubbedHft hft(&getRunner().replayer(), "r9_hft_end");
	hft.on_bactest_end();
	EXPECT_EQ(g9.evt, 1U);
	EXPECT_EQ(g9.evt_id, (uint32_t)EVENT_BACKTEST_END);
	EXPECT_EQ(g9.hft_bar + g9.hft_tick + g9.chnl + g9.ord + g9.trd, 0U);
}

/* on_init 顺序：父类 → ctx_on_init(ET_HFT) → 全局初始化事件（ExpHftMocker.cpp:44-51） */
TEST_F(R9Wired, HftInitFiresCtxInitAndEngineEvent)
{
	SubbedHft hft(&getRunner().replayer(), "r9_hft_init");
	hft.on_init();
	EXPECT_EQ(g9.hft_init, 1U);
	EXPECT_EQ(g9.last_handle, hft.id());
	EXPECT_EQ(g9.evt, 1U);
	EXPECT_EQ(g9.evt_id, (uint32_t)EVENT_ENGINE_INIT);
}

/*
 * ExpCtaMocker::on_bar_close 不做周期换算、也不判空指针，
 * 与 ExpHftMocker::on_bar 的两次行为正好相反（ExpCtaMocker.cpp:133-139）
 */
TEST_F(R9Wired, CtaBarClosePassesPeriodUnchanged)
{
	SubbedCta cta(&getRunner().replayer(), "r9_cta_bar");
	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	bar.date = 20240603;
	bar.close = 15.25;

	cta.on_bar_close("SSE.600519", "m5", &bar);
	ASSERT_EQ(g9.cta_bar, 1U);
	EXPECT_EQ(g9.last_period, "m5");
	EXPECT_EQ(g9.last_code, "SSE.600519");
	EXPECT_EQ(g9.last_handle, cta.id());
	EXPECT_FALSE(g9.last_bar_null);
	EXPECT_DOUBLE_EQ(g9.p1, 15.25);

	cta.on_bar_close("SSE.600519", "d1", NULL);
	ASSERT_EQ(g9.cta_bar, 2U);
	EXPECT_EQ(g9.last_period, "d1");
	EXPECT_TRUE(g9.last_bar_null);
}

/* on_calculate → ctx_on_calc(ET_CTA)；on_calculate_done 额外补一次全局调度事件（ExpCtaMocker.cpp:151-174） */
TEST_F(R9Wired, CtaCalcAndCalcDoneForwardTime)
{
	SubbedCta cta(&getRunner().replayer(), "r9_cta_calc");

	cta.on_calculate(20240603, 93000);
	ASSERT_EQ(g9.cta_calc, 1U);
	EXPECT_EQ(g9.last_handle, cta.id());
	EXPECT_EQ(g9.last_date, 20240603U);
	EXPECT_EQ(g9.last_time, 93000U);
	EXPECT_EQ(g9.evt, 0U);

	cta.on_calculate_done(20240603, 145900);
	ASSERT_EQ(g9.cta_calc_done, 1U);
	EXPECT_EQ(g9.last_date, 20240603U);
	EXPECT_EQ(g9.last_time, 145900U);
	ASSERT_EQ(g9.evt, 1U);
	EXPECT_EQ(g9.evt_id, (uint32_t)EVENT_ENGINE_SCHDL);
	EXPECT_EQ(g9.evt_date, 20240603U);
	EXPECT_EQ(g9.evt_time, 145900U);
}

/* ExpCtaMocker::on_tick_updated 的订阅前置（ExpCtaMocker.cpp:113-121） */
TEST_F(R9Wired, CtaTickNeedsSubscription)
{
	SubbedCta cta(&getRunner().replayer(), "r9_cta_tick");
	WTSTickStruct raw = makeR9Tick("600000", 9.6);
	WTSTickData* tick = WTSTickData::create(raw);
	ASSERT_NE(tick, nullptr);

	cta.on_tick_updated("SSE.600000", tick);
	EXPECT_EQ(g9.cta_tick, 0U);

	cta.sub_tick_locally("SSE.600000");
	cta.on_tick_updated("SSE.600000", tick);
	ASSERT_EQ(g9.cta_tick, 1U);
	EXPECT_EQ(g9.last_handle, cta.id());
	EXPECT_DOUBLE_EQ(g9.p1, 9.6);

	tick->release();
}

/* 条件触发只路由 ET_CTA 一条口；on_bactest_end 只打全局事件（ExpCtaMocker.cpp:184-204） */
TEST_F(R9Wired, CtaCondTriggerAndBacktestEnd)
{
	SubbedCta cta(&getRunner().replayer(), "r9_cta_cond");

	cta.on_condition_triggered("SSE.600000", 12.5, 12.0, "r9tag");
	ASSERT_EQ(g9.cond, 1U);
	EXPECT_EQ(g9.last_handle, cta.id());
	EXPECT_EQ(g9.last_code, "SSE.600000");
	EXPECT_DOUBLE_EQ(g9.p1, 12.5);
	EXPECT_DOUBLE_EQ(g9.p2, 12.0);
	EXPECT_EQ(g9.last_str, "r9tag");

	cta.on_bactest_end();
	EXPECT_EQ(g9.evt, 1U);
	EXPECT_EQ(g9.evt_id, (uint32_t)EVENT_BACKTEST_END);
	EXPECT_EQ(g9.sel_calc, 0U);
}

/* on_bar_close 原样透传周期；on_strategy_schedule 双发 ctx_on_calc(ET_SEL)+全局调度（ExpSelMocker.cpp:122-146） */
TEST_F(R9Wired, SelBarAndScheduleForward)
{
	SubbedSel sel(&getRunner().replayer(), "r9_sel_bar");
	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	bar.close = 7.125;

	sel.on_bar_close("SSE.600519", "d1", &bar);
	ASSERT_EQ(g9.sel_bar, 1U);
	EXPECT_EQ(g9.last_period, "d1");
	EXPECT_EQ(g9.last_handle, sel.id());
	EXPECT_DOUBLE_EQ(g9.p1, 7.125);

	sel.on_strategy_schedule(20240614, 153000);
	ASSERT_EQ(g9.sel_calc, 1U);
	EXPECT_EQ(g9.last_date, 20240614U);
	EXPECT_EQ(g9.last_time, 153000U);
	ASSERT_EQ(g9.evt, 1U);
	EXPECT_EQ(g9.evt_id, (uint32_t)EVENT_ENGINE_SCHDL);
	EXPECT_EQ(g9.evt_date, 20240614U);
	EXPECT_EQ(g9.evt_time, 153000U);
}

/* on_session_end 双发；on_tick_updated 未订阅必须静默（ExpSelMocker.cpp:86-111） */
TEST_F(R9Wired, SelSessionEndAndTickGuard)
{
	SubbedSel sel(&getRunner().replayer(), "r9_sel_sess");

	sel.on_session_end(20240615);
	ASSERT_EQ(g9.sess, 1U);
	EXPECT_EQ(g9.last_handle, sel.id());
	EXPECT_EQ(g9.sess_date, 20240615U);
	EXPECT_EQ(g9.sess_flag, 0U);
	ASSERT_EQ(g9.evt, 1U);
	EXPECT_EQ(g9.evt_id, (uint32_t)EVENT_SESSION_END);

	WTSTickStruct raw = makeR9Tick("600519", 100.5);
	WTSTickData* tick = WTSTickData::create(raw);
	ASSERT_NE(tick, nullptr);
	sel.on_tick_updated("SSE.600519", tick);
	EXPECT_EQ(g9.sel_tick, 0U);

	sel.sub_tick_locally("SSE.600519");
	sel.on_tick_updated("SSE.600519", tick);
	ASSERT_EQ(g9.sel_tick, 1U);
	EXPECT_DOUBLE_EQ(g9.p1, 100.5);
	tick->release();
}

/* on_init/on_session_begin 的双发契约（ExpSelMocker.cpp:52-76）+ 回测结束（L154-157） */
TEST_F(R9Wired, SelInitSessionBeginAndBacktestEnd)
{
	SubbedSel sel(&getRunner().replayer(), "r9_sel_init");

	sel.on_init();
	ASSERT_EQ(g9.sel_init, 1U);
	EXPECT_EQ(g9.last_handle, sel.id());
	EXPECT_EQ(g9.evt, 1U);
	EXPECT_EQ(g9.evt_id, (uint32_t)EVENT_ENGINE_INIT);

	sel.on_session_begin(20240616);
	ASSERT_EQ(g9.sess, 1U);
	EXPECT_EQ(g9.sess_flag, 1U);
	EXPECT_EQ(g9.sess_date, 20240616U);
	ASSERT_EQ(g9.evt, 2U);
	EXPECT_EQ(g9.evt_id, (uint32_t)EVENT_SESSION_BEGIN);

	sel.on_bactest_end();
	ASSERT_EQ(g9.evt, 3U);
	EXPECT_EQ(g9.evt_id, (uint32_t)EVENT_BACKTEST_END);
}

/*
 * 跳过目标（触发条件，逐条见 manifest）：
 * - WtBtRunner::run(bAsync=false)：_replayer.prepare()+run() 会打开行情存储并同步回放整段区间，
 *   未 config 成功时直接进 IO/断言；bAsync=true 还会起线程并需要 stop() join。
 * - init_sel_mocker：内部 register_task 需要交易时段表 trdtpl/session。
 * - init_backtest：WTSLogger::init 会按 outDir 建目录并接管全局日志单例。
 * - WtBtRunner::stop()：_running 为假时也要 join 可能存在的 _worker。
 */
TEST_F(R9Wired, DISABLED_RunConfigSuccessPathsNeedStorageAndThreads)
{
	EXPECT_TRUE(true);
}
