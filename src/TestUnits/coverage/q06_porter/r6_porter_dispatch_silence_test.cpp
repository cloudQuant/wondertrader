/*
 * R6 - WtRtRunner 分发层的「静默 / 覆盖 / 串扰」契约
 *
 * 目标文件：src/WtPorter/WtRtRunner.cpp
 * q06 lane 已经把「回调注册以后能收到事件」这一面钉住了（q06_porter_runner_test.cpp），
 * 本文件不重写它；钉的是那批文件的反面：
 *   1) 一个槽都没注册时，整套 ctx_on_* / hft_on_* / parser_* / executer_* 必须静默返回；
 *   2) registerXxx 是「覆盖」而不是「追加」——再注册一次后只有新回调收得到；
 *   3) 传 NULL 可以把已经注册的槽清空；
 *   4) 三族槽（cta / hft / sel）互不串扰；
 *   5) 未知上下文 id 原样透传，不做存在性过滤。
 * 全部用「同一份探针在注册前/后计数差」做可证伪断言：探针先证明自己能响，再证明不该响时不响。
 *
 * 只引用 WtPorter 一族符号（WtBtPorter 有同名 extern "C" 入口，不能同链）。
 * 缺陷见 docs/迭代计划/迭代01/bugs/R6_porter_runner.md。
 */
#include <gtest/gtest.h>

#include <string>

#include "WtPorter/PorterDefs.h"
#include "WtPorter/WtRtRunner.h"

#include "Includes/WTSStruct.h"

USING_NS_WTP;

namespace {

	struct Probe
	{
		uint32_t	cta_init = 0, cta_tick = 0, cta_calc = 0, cta_bar = 0;
		uint32_t	hft_init = 0, hft_bar = 0, hft_ord = 0, hft_trd = 0, hft_entrust = 0;
		uint32_t	hft_pos = 0, hft_ordque = 0, hft_orddtl = 0, hft_trans = 0, hft_chnl = 0;
		uint32_t	sel_init = 0, sel_tick = 0, sel_calc = 0, sel_bar = 0;
		uint32_t	cond = 0, sess = 0, evt = 0;
		uint32_t	parser_evt = 0, parser_sub = 0, exec_init = 0, exec_cmd = 0;
		WtUInt32	last_id = 0;
		std::string	last_str;
		void clear() { *this = Probe(); }
		uint32_t total() const
		{
			return cta_init + cta_tick + cta_calc + cta_bar
				+ hft_init + hft_bar + hft_ord + hft_trd + hft_entrust
				+ hft_pos + hft_ordque + hft_orddtl + hft_trans + hft_chnl
				+ sel_init + sel_tick + sel_calc + sel_bar
				+ cond + sess + evt + parser_evt + parser_sub + exec_init + exec_cmd;
		}
	};

	Probe g_p;			// 第一组探针
	Probe g_p2;			// 第二组探针：只用来验证「再注册是覆盖不是追加」

	/* ---- 探针 1：cta 全套 ---- */
	void p_cta_init(CtxHandler h) { g_p.cta_init++; g_p.last_id = (WtUInt32)h; }
	void p_cta_tick(CtxHandler h, const char*, WTSTickStruct*) { g_p.cta_tick++; g_p.last_id = (WtUInt32)h; }
	void p_cta_calc(CtxHandler h, WtUInt32, WtUInt32) { g_p.cta_calc++; g_p.last_id = (WtUInt32)h; }
	void p_cta_bar(CtxHandler h, const char* code, const char*, WTSBarStruct*)
	{
		g_p.cta_bar++; g_p.last_id = (WtUInt32)h; g_p.last_str = code;
	}
	void p_sess(CtxHandler h, WtUInt32, bool) { g_p.sess++; g_p.last_id = (WtUInt32)h; }
	void p_cond(CtxHandler, const char*, double, double, const char*) { g_p.cond++; }

	/* ---- 探针 1：hft 全套 ---- */
	void p_hft_init(CtxHandler h) { g_p.hft_init++; g_p.last_id = (WtUInt32)h; }
	void p_hft_bar(CtxHandler, const char*, const char*, WTSBarStruct*) { g_p.hft_bar++; }
	void p_hft_chnl(CtxHandler, const char*, WtUInt32) { g_p.hft_chnl++; }
	void p_hft_ord(CtxHandler, WtUInt32, const char*, bool, double, double, double, bool, const char*) { g_p.hft_ord++; }
	void p_hft_trd(CtxHandler, WtUInt32, const char*, bool, double, double, const char*) { g_p.hft_trd++; }
	void p_hft_entrust(CtxHandler, WtUInt32, const char*, bool, const char*, const char*) { g_p.hft_entrust++; }
	void p_hft_pos(CtxHandler, const char*, bool, double, double, double, double) { g_p.hft_pos++; }
	void p_hft_ordque(CtxHandler, const char*, WTSOrdQueStruct*) { g_p.hft_ordque++; }
	void p_hft_orddtl(CtxHandler, const char*, WTSOrdDtlStruct*) { g_p.hft_orddtl++; }
	void p_hft_trans(CtxHandler, const char*, WTSTransStruct*) { g_p.hft_trans++; }

	/* ---- 探针 1：sel / evt / parser / executer ---- */
	void p_sel_init(CtxHandler h) { g_p.sel_init++; g_p.last_id = (WtUInt32)h; }
	void p_sel_tick(CtxHandler, const char*, WTSTickStruct*) { g_p.sel_tick++; }
	void p_sel_calc(CtxHandler, WtUInt32, WtUInt32) { g_p.sel_calc++; }
	void p_sel_bar(CtxHandler, const char*, const char*, WTSBarStruct*) { g_p.sel_bar++; }
	void p_evt(WtUInt32 evtId, WtUInt32, WtUInt32) { g_p.evt++; g_p.last_str = std::to_string(evtId); }
	void p_parser_evt(WtUInt32, const char*) { g_p.parser_evt++; }
	void p_parser_sub(const char*, const char*, bool) { g_p.parser_sub++; }
	void p_exec_init(const char*) { g_p.exec_init++; }
	void p_exec_cmd(const char*, const char*, double) { g_p.exec_cmd++; }

	/* ---- 探针 2：只有 init，用来验证「覆盖语义」 */
	void p2_cta_init(CtxHandler h) { g_p2.cta_init++; g_p2.last_id = (WtUInt32)h; }

	void reg_all_hft(WtRtRunner& r)
	{
		r.registerHftCallbacks(p_hft_init, p_cta_tick, p_hft_bar, p_hft_chnl, p_hft_ord,
			p_hft_trd, p_hft_entrust, p_hft_orddtl, p_hft_ordque, p_hft_trans,
			p_sess, p_hft_pos);
	}

	/* 一次把六个分发事件全打一遍（cta 族） */
	void fire_all_cta(WtRtRunner& r, uint32_t id)
	{
		r.ctx_on_init(id, ET_CTA);
		r.ctx_on_calc(id, 20210104, 93000, ET_CTA);
		r.ctx_on_bar(id, "IF2108", "d1", NULL, ET_CTA);
		r.ctx_on_session_event(id, 20210104, true, ET_CTA);
		r.ctx_on_cond_triggered(id, "IF2108", 1.0, 2.0, "tag", ET_CTA);
	}

} // namespace

/* =========================================================================
 * 1) 一个回调都没注册：整套分发必须静默
 * ========================================================================= */

TEST(R6PorterSilence, cta_dispatch_is_silent_before_any_registration)
{
	WtRtRunner r;
	g_p.clear();
	// 探针一律没注册（新对象），任何命中都说明槽位被别的实例串进来了
	fire_all_cta(r, 7001);
	EXPECT_EQ(g_p.total(), 0u);
}

TEST(R6PorterSilence, tick_dispatch_with_null_callback_never_touches_the_argument)
{
	WtRtRunner r;
	g_p.clear();
	// ctx_on_tick 里 `newTick->getTickStruct()` 只在命中槽位时才求值（WtRtRunner.cpp:445-447），
	// 未注册回调时传 NULL 必须安全 —— 这一条同时在钉那个守卫的存在
	EXPECT_NO_THROW(r.ctx_on_tick(7002, "IF2108", NULL, ET_CTA));
	EXPECT_NO_THROW(r.ctx_on_tick(7002, "IF2108", NULL, ET_HFT));
	EXPECT_NO_THROW(r.ctx_on_tick(7002, "IF2108", NULL, ET_SEL));
	EXPECT_EQ(g_p.cta_tick, 0u);
}

TEST(R6PorterSilence, hft_micro_data_dispatch_is_silent_and_skips_struct_unwrap)
{
	WtRtRunner r;
	g_p.clear();
	// 未注册 -> 走 if(_cb_xxx) 的假支，连 newOrdQue->getOrdQueStruct() 都不会执行，
	// 所以传 nullptr 是安全的，也正是这一步在证明守卫存在
	r.hft_on_order_queue(7003, "IF2108", NULL);
	r.hft_on_order_detail(7003, "IF2108", NULL);
	r.hft_on_transaction(7003, "IF2108", NULL);
	r.hft_on_channel_ready(7003, "r6trader");
	r.hft_on_channel_lost(7003, "r6trader");
	r.hft_on_entrust(7003, 1, "IF2108", true, "msg", "tag");
	r.hft_on_order(7003, 1, "IF2108", true, 1, 1, 4000, false, "tag");
	r.hft_on_trade(7003, 1, "IF2108", true, 1, 4000, "tag");
	r.hft_on_position(7003, "IF2108", true, 0, 0, 1, 1);
	EXPECT_EQ(g_p.total(), 0u);
}

TEST(R6PorterSilence, parser_and_executer_slots_are_silent_until_registered)
{
	WtRtRunner r;
	g_p.clear();
	r.parser_init("r6p");
	r.parser_connect("r6p");
	r.parser_disconnect("r6p");
	r.parser_release("r6p");
	r.parser_subscribe("r6p", "IF2108");
	r.parser_unsubscribe("r6p", "IF2108");
	r.executer_init("r6e");
	r.executer_set_position("r6e", "IF2108", 2.0);
	EXPECT_EQ(g_p.total(), 0u);

	// 控制组：同一批调用注册后必须逐条命中，证明探针本身是活的
	r.registerParserPorter(p_parser_evt, p_parser_sub);
	r.registerExecuterPorter(p_exec_init, p_exec_cmd);
	r.parser_connect("r6p");
	r.parser_subscribe("r6p", "IF2108");
	r.executer_init("r6e");
	r.executer_set_position("r6e", "IF2108", 2.0);
	EXPECT_EQ(g_p.parser_evt, 1u);
	EXPECT_EQ(g_p.parser_sub, 1u);
	EXPECT_EQ(g_p.exec_init, 1u);
	EXPECT_EQ(g_p.exec_cmd, 1u);
}

/* =========================================================================
 * 2) 三族槽互不串扰
 * ========================================================================= */

TEST(R6PorterSilence, registering_sel_slots_leaves_cta_and_hft_silent)
{
	WtRtRunner r;
	g_p.clear();
	r.registerSelCallbacks(p_sel_init, p_sel_tick, p_sel_calc, p_sel_bar, p_sess);

	r.ctx_on_init(7010, ET_CTA);
	r.ctx_on_calc(7010, 20210104, 93000, ET_CTA);
	r.ctx_on_bar(7010, "IF2108", "d1", NULL, ET_CTA);
	r.ctx_on_init(7010, ET_HFT);
	// sel 槽自己必须响（否则这个「不串扰」是假的绿）
	r.ctx_on_init(7010, ET_SEL);
	r.ctx_on_calc(7010, 20210104, 93000, ET_SEL);

	EXPECT_EQ(g_p.sel_init, 2u);
	EXPECT_EQ(g_p.sel_calc, 1u);
	EXPECT_EQ(g_p.cta_init, 0u);
	EXPECT_EQ(g_p.cta_calc, 0u);
	EXPECT_EQ(g_p.hft_init, 0u);
}

TEST(R6PorterSilence, hft_slots_do_not_fill_cta_calc_slot)
{
	WtRtRunner r;
	g_p.clear();
	reg_all_hft(r);
	r.ctx_on_calc(7011, 20210104, 93000, ET_HFT);	// hft 没有 calc 槽
	r.ctx_on_init(7011, ET_HFT);
	r.hft_on_channel_ready(7011, "r6trader");
	EXPECT_EQ(g_p.hft_init, 1u);
	EXPECT_EQ(g_p.hft_chnl, 1u);
	EXPECT_EQ(g_p.cta_calc, 0u);
	EXPECT_EQ(g_p.sel_calc, 0u);
}

TEST(R6PorterSilence, cond_trigger_slot_only_occupied_by_cta_registration)
{
	WtRtRunner r;
	g_p.clear();
	reg_all_hft(r);
	r.registerSelCallbacks(p_sel_init, p_sel_tick, p_sel_calc, p_sel_bar, p_sess);
	// 既没走 registerCtaCallbacks，_cb_cta_cond_trigger 就永远是 NULL
	r.ctx_on_cond_triggered(7012, "IF2108", 1, 2, "tag", ET_CTA);
	r.ctx_on_cond_triggered(7012, "IF2108", 1, 2, "tag", ET_SEL);
	EXPECT_EQ(g_p.cond, 0u);
}

/* =========================================================================
 * 3) registerXxx 是覆盖而不是追加
 * ========================================================================= */

TEST(R6PorterReregister, second_cta_registration_replaces_the_first)
{
	WtRtRunner r;
	g_p.clear();
	g_p2.clear();

	r.registerCtaCallbacks(p_cta_init, p_cta_tick, p_cta_calc, p_cta_bar, p_sess, p_cond);
	r.ctx_on_init(7020, ET_CTA);
	ASSERT_EQ(g_p.cta_init, 1u) << "第一组探针没响，后面的对照没有意义";

	r.registerCtaCallbacks(p2_cta_init, p_cta_tick, p_cta_calc, p_cta_bar, p_sess, p_cond);
	r.ctx_on_init(7021, ET_CTA);

	EXPECT_EQ(g_p2.cta_init, 1u) << "新回调没接管槽位";
	EXPECT_EQ(g_p.cta_init, 1u) << "旧回调还在收事件：注册语义成了追加";
	EXPECT_EQ(g_p2.last_id, 7021u);
}

TEST(R6PorterReregister, passing_nulls_clears_every_cta_slot)
{
	WtRtRunner r;
	g_p.clear();
	r.registerCtaCallbacks(p_cta_init, p_cta_tick, p_cta_calc, p_cta_bar, p_sess, p_cond);
	fire_all_cta(r, 7022);
	ASSERT_GT(g_p.total(), 0u);

	const uint32_t before = g_p.total();
	r.registerCtaCallbacks(NULL, NULL, NULL, NULL, NULL, NULL);
	fire_all_cta(r, 7023);
	EXPECT_EQ(g_p.total(), before) << "清空以后仍有槽位能收到事件";
}

TEST(R6PorterReregister, passing_nulls_clears_hft_and_parser_and_executer_slots)
{
	WtRtRunner r;
	g_p.clear();
	reg_all_hft(r);
	r.registerParserPorter(p_parser_evt, p_parser_sub);
	r.registerExecuterPorter(p_exec_init, p_exec_cmd);
	r.hft_on_trade(7024, 1, "IF2108", true, 1, 4000, "tag");
	r.parser_disconnect("r6p");
	r.executer_init("r6e");
	ASSERT_EQ(g_p.total(), 3u) << "对照阶段就没打通";

	const uint32_t before = g_p.total();
	r.registerHftCallbacks(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
	r.registerParserPorter(NULL, NULL);
	r.registerExecuterPorter(NULL, NULL);

	r.hft_on_trade(7025, 1, "IF2108", true, 1, 4000, "tag");
	r.parser_disconnect("r6p");
	r.executer_init("r6e");
	EXPECT_EQ(g_p.total(), before);
}

TEST(R6PorterReregister, evt_callback_can_be_revoked)
{
	WtRtRunner r;
	g_p.clear();
	r.registerEvtCallback(p_evt);
	r.on_initialize_event();
	r.on_schedule_event(20210104, 93000);
	r.on_session_event(20210104, false);
	ASSERT_EQ(g_p.evt, 3u) << "三个引擎事件的槽没打通";

	r.registerEvtCallback(NULL);
	r.on_initialize_event();
	r.on_session_event(20210104, true);
	EXPECT_EQ(g_p.evt, 3u) << "撤销后仍能收到事件";
	EXPECT_EQ(g_p.last_str, std::to_string(EVENT_SESSION_END))
		<< "最后一次命中应当停在交易日结束事件上";
}

/* =========================================================================
 * 4) 未知 id 原样透传（分发层不做存在性校验）
 * ========================================================================= */

TEST(R6PorterPassthrough, unknown_context_id_reaches_the_callback_verbatim)
{
	WtRtRunner r;
	g_p.clear();
	r.registerCtaCallbacks(p_cta_init, p_cta_tick, p_cta_calc, p_cta_bar, p_sess, p_cond);

	// 本进程里根本没 createCtaContext 过 654321
	r.ctx_on_init(654321, ET_CTA);
	r.ctx_on_bar(654321, "NONEXIST.R6", "m5", NULL, ET_CTA);
	EXPECT_EQ(g_p.cta_init, 1u);
	EXPECT_EQ(g_p.last_id, 654321u);
	EXPECT_EQ(g_p.cta_bar, 1u);
	EXPECT_EQ(g_p.last_str, "NONEXIST.R6") << "合约码在路由途中被改写";
}

/* =========================================================================
 * 5) DISABLED_：崩溃类
 *
 * D1 parser_* 的 id 参数没有任何校验：ParserAdapterMgr::getAdapter(NULL) 会用它构造
 *    std::string key -> 未定义行为。同理 on_ext_parser_quote(NULL, ...)。
 *    触发条件：任意 parser_/on_ext_parser_quote 入口传 NULL id。
 * D2 ctx_on_tick 注册回调后传 NULL WTSTickData*：`newTick->getTickStruct()` 空指针解引用
 *    （WtRtRunner.cpp:445-447）。触发条件：注册了 tick 回调再传 NULL。
 * 两条都会打死宿主进程，本轮不启用；缺陷登记见 bugs/R6_porter_runner.md（BUG-R6-004/005）。
 * ---------------------------------------------------------------------- */

TEST(R6PorterNullArgs, DISABLED_parser_id_null_builds_string_from_nullptr)
{
	WtRtRunner r;
	r.registerParserPorter(p_parser_evt, p_parser_sub);
	r.parser_connect(NULL);			// D1
	r.on_ext_parser_quote(NULL, NULL, 0);	// D1
	FAIL() << "上面两行只要有一行不崩，就说明守卫加上了，届时应回到启用态";
}

TEST(R6PorterNullArgs, DISABLED_ctx_on_tick_null_tickdata_derefs)
{
	WtRtRunner r;
	r.registerCtaCallbacks(p_cta_init, p_cta_tick, p_cta_calc, p_cta_bar, p_sess, p_cond);
	r.ctx_on_tick(7099, "IF2108", NULL, ET_CTA);		// D2
	FAIL() << "注册了 tick 回调 yet 传 NULL 竟然没崩，守卫已加";
}
