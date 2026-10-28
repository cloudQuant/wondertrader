/*
 * R13 - 导出上下文转发层：ExpCtaContext / ExpSelContext / ExpHftContext
 *                     + ExpParser / ExpExecuter
 *
 * 目标（0% 或低覆盖）：
 *   src/WtPorter/ExpHftContext.cpp  missed 61/61
 *   src/WtPorter/ExpCtaContext.cpp  missed 28/33
 *   src/WtPorter/ExpSelContext.cpp  missed 24/29
 *   src/WtPorter/ExpParser.cpp/.h   missed 1/28 + 3/3
 *   src/WtPorter/ExpExecuter.h      missed 2/2
 * 这一层的形状是「先转发给 getRunner() 的 ctx_on_ 与 hft_on_ 系列，再调基类」。
 * 转发段可取样；调基类的段落里凡是要落文件（save_data/save_userdata）、
 * 解引用未绑定 _trader、或读未初始化 _ud_modified 的，一律留白并登记。
 *
 * ExpParser / ExpExecuter 是可直接栈构造的类（IExecCommand 无纯虚，
 * 见 WtCore/IExecCommand.h:62-113），所以不经 create_ext_parser
 * 也能取样 —— 这一步绕开了 BUG-R6-003（createExtParser 解引用空 _engine）。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "WtPorter/PorterDefs.h"
#include "WtPorter/WtPorter.h"
#include "WtPorter/WtRtRunner.h"
#include "WtPorter/ExpParser.h"
#include "WtPorter/ExpExecuter.h"
#include "WtPorter/ExpHftContext.h"

#include "Includes/WTSStruct.h"

USING_NS_WTP;

extern WtRtRunner& getRunner();

namespace {

	const char* kCtaCode = "SHFE.rb.2405";
	const char* kSelCode = "SSE.STK.600000";

	struct FwdRec
	{
		uint32_t	init = 0, tick = 0, calc = 0, bar = 0, sess = 0, cond = 0;
		uint32_t	hft_pos = 0, parser_evt = 0, parser_sub = 0, exec_init = 0, exec_cmd = 0;
		CtxHandler	last_handle = 0;
		uint32_t	last_date = 0, last_time = 0, last_tdate = 0, last_evtid = 0;
		bool		last_isbegin = false, last_subflag = false;
		std::string	last_code, last_period, last_usertag, last_id;
		double		last_target = 0, last_price = 0;
		double		last_prevol = 0, last_preavail = 0, last_newvol = 0, last_newavail = 0;
		bool		last_islong = false;
		void clear() { *this = FwdRec(); }
	};

	FwdRec g;

	void cb_init(CtxHandler c) { g.init++; g.last_handle = c; }
	void cb_tick(CtxHandler c, const char* code, WTSTickStruct*) { g.tick++; g.last_handle = c; g.last_code = code; }
	void cb_calc(CtxHandler c, WtUInt32 d, WtUInt32 t) { g.calc++; g.last_handle = c; g.last_date = d; g.last_time = t; }
	void cb_bar(CtxHandler c, const char* code, const char* period, WTSBarStruct*)
	{ g.bar++; g.last_handle = c; g.last_code = code; g.last_period = period; }
	void cb_sess(CtxHandler c, WtUInt32 tdate, bool isBegin)
	{ g.sess++; g.last_handle = c; g.last_tdate = tdate; g.last_isbegin = isBegin; }
	void cb_cond(CtxHandler c, const char* code, double target, double price, const char* tag)
	{ g.cond++; g.last_handle = c; g.last_code = code; g.last_target = target; g.last_price = price; g.last_usertag = tag; }

	void cb_hft_pos(CtxHandler c, const char* code, bool isLong, double pv, double pa, double nv, double na)
	{
		g.hft_pos++; g.last_handle = c; g.last_code = code; g.last_islong = isLong;
		g.last_prevol = pv; g.last_preavail = pa; g.last_newvol = nv; g.last_newavail = na;
	}

	void cb_parser_evt(WtUInt32 evtId, const char* id) { g.parser_evt++; g.last_evtid = evtId; g.last_id = id; }
	void cb_parser_sub(const char* id, const char* code, bool isSub)
	{ g.parser_sub++; g.last_id = id; g.last_code = code; g.last_subflag = isSub; }
	void cb_exec_init(const char* id) { g.exec_init++; g.last_id = id; }
	void cb_exec_cmd(const char* id, const char* code, double target)
	{ g.exec_cmd++; g.last_id = id; g.last_code = code; g.last_target = target; }

	void wire_all()
	{
		register_cta_callbacks(cb_init, cb_tick, cb_calc, cb_bar, cb_sess, cb_cond);
		register_sel_callbacks(cb_init, cb_tick, cb_calc, cb_bar, cb_sess);
		register_hft_callbacks(cb_init, cb_tick, cb_bar, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
			NULL, cb_hft_pos);
		register_parser_callbacks(cb_parser_evt, cb_parser_sub);
		register_exec_callbacks(cb_exec_init, cb_exec_cmd);
	}

	void unwire_all()
	{
		register_cta_callbacks(NULL, NULL, NULL, NULL, NULL, NULL);
		register_sel_callbacks(NULL, NULL, NULL, NULL, NULL);
		register_hft_callbacks(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
		register_parser_callbacks(NULL, NULL);
		register_exec_callbacks(NULL, NULL);
	}
}

class R13ExpContext : public ::testing::Test
{
protected:
	CtxHandler	_cta = 0;
	CtxHandler	_sel = 0;
	CtxHandler	_hft = 0;

	void SetUp() override
	{
		g.clear();
		wire_all();

		_cta = create_cta_context("r13_exp_cta", 1);
		_sel = create_sel_context("r13_exp_sel", 20240601, 153000, "d", "CHINA", "TRADING", 0);
		_hft = create_hft_context("r13_exp_hft", "R13_TRADER_NOT_EXISTS", false, 0);

		ASSERT_NE(_cta, 0U);
		ASSERT_NE(_sel, 0U);
		ASSERT_NE(_hft, 0U);
		ASSERT_TRUE(getRunner().getCtaContext(_cta) != NULL);
		ASSERT_TRUE(getRunner().getSelContext(_sel) != NULL);
		ASSERT_TRUE(getRunner().getHftContext(_hft) != NULL);
	}

	void TearDown() override
	{
		unwire_all();
		g.clear();
	}
};

/*
 * ExpCtaContext::on_bar_close（ExpCtaContext.cpp:115-119）只做转发：
 * getRunner().ctx_on_bar(_context_id, stdCode, period, newBar, ET_CTA)
 * -> WtRtRunner.cpp:384-394 的 ET_CTA 支 -> _cb_cta_bar。
 * 周期串必须原样透传（这层不做 m1 -> 60 的换算）。
 */
TEST_F(R13ExpContext, cta_on_bar_close_forwards_handle_code_period_verbatim)
{
	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	bar.time = 202406010931ULL;
	bar.close = 3512.0;

	CtaContextPtr ctx = getRunner().getCtaContext(_cta);
	ctx->on_bar_close(kCtaCode, "m1", &bar);

	EXPECT_EQ(g.bar, 1U);
	EXPECT_EQ(g.last_handle, _cta);
	EXPECT_EQ(g.last_code, kCtaCode);
	EXPECT_EQ(g.last_period, "m1");
	// 其余槽位不该被牵连
	EXPECT_EQ(g.calc, 0U);
	EXPECT_EQ(g.cond, 0U);
	EXPECT_EQ(g.hft_pos, 0U);
}

/* 转发层对 NULL bar 也不设守卫（ExpCtaContext 的 bar_close 直接透传），
 * 但 _cb_cta_bar 只是把指针递给导出方，本身不解引用 -> 允许 NULL 通过。 */
TEST_F(R13ExpContext, cta_on_bar_close_passes_null_bar_through_without_deref)
{
	CtaContextPtr ctx = getRunner().getCtaContext(_cta);
	ctx->on_bar_close(kCtaCode, "m5", NULL);
	EXPECT_EQ(g.bar, 1U);
	EXPECT_EQ(g.last_period, "m5");
}

/* ExpCtaContext::on_calculate（:127-131） */
TEST_F(R13ExpContext, cta_on_calculate_forwards_date_and_time_pair)
{
	CtaContextPtr ctx = getRunner().getCtaContext(_cta);
	ctx->on_calculate(20240603, 145500);

	EXPECT_EQ(g.calc, 1U);
	EXPECT_EQ(g.last_handle, _cta);
	EXPECT_EQ(g.last_date, 20240603U);
	EXPECT_EQ(g.last_time, 145500U);
	EXPECT_EQ(g.bar, 0U);
}

/* ExpCtaContext::on_condition_triggered（:141-145）：CTA 独有槽位 */
TEST_F(R13ExpContext, cta_on_condition_triggered_forwards_target_price_and_usertag)
{
	CtaContextPtr ctx = getRunner().getCtaContext(_cta);
	ctx->on_condition_triggered(kCtaCode, 3520.5, 3518.25, "r13-cond-tag");

	EXPECT_EQ(g.cond, 1U);
	EXPECT_EQ(g.last_handle, _cta);
	EXPECT_EQ(g.last_code, kCtaCode);
	// 三个数都是本用例自己写进去的字面量，单次透传没有累加 -> 精确比较
	EXPECT_DOUBLE_EQ(g.last_target, 3520.5);
	EXPECT_DOUBLE_EQ(g.last_price, 3518.25);
	EXPECT_EQ(g.last_usertag, "r13-cond-tag");
}

/*
 * ExpCtaContext::on_tick_updated（:97-106）的「未订阅」支：
 * _tick_subs 为空 -> find 命中失败 -> 直接 return，转发行不执行。
 * （订阅支需要 stra_sub_ticks -> WtEngine::sub_tick 的 _hot_mgr，取不到样，见留白清单）
 */
TEST_F(R13ExpContext, cta_on_tick_updated_is_dropped_when_the_code_was_never_subscribed)
{
	CtaContextPtr ctx = getRunner().getCtaContext(_cta);
	ctx->on_tick_updated(kCtaCode, NULL);

	EXPECT_EQ(g.tick, 0U);
	EXPECT_EQ(g.bar, 0U);
}

/*
 * ExpCtaContext::on_session_begin（:66-73）：先基类（遍历空 _pos_map +
 * _ud_modified 假支；CTA 构造函数确实初始化了它，CtaStraBaseCtx.cpp:67），
 * 再转发 isBegin=true。
 */
TEST_F(R13ExpContext, cta_on_session_begin_forwards_with_isbegin_true)
{
	CtaContextPtr ctx = getRunner().getCtaContext(_cta);
	ctx->on_session_begin(20240604);

	EXPECT_EQ(g.sess, 1U);
	EXPECT_EQ(g.last_handle, _cta);
	EXPECT_EQ(g.last_tdate, 20240604U);
	EXPECT_TRUE(g.last_isbegin);
}

/* ExpSelContext::on_session_begin（ExpSelContext.cpp:62-69）同上，ET_SEL 支 */
TEST_F(R13ExpContext, sel_on_session_begin_forwards_to_the_sel_slot_only)
{
	SelContextPtr ctx = getRunner().getSelContext(_sel);
	ctx->on_session_begin(20240605);

	EXPECT_EQ(g.sess, 1U);
	EXPECT_EQ(g.last_handle, _sel);
	EXPECT_EQ(g.last_tdate, 20240605U);
	EXPECT_TRUE(g.last_isbegin);
	// SEL 与 CTA 用同一个 _cb_sel_* 槽位族，不该串到 CTA 的 bar/calc
	EXPECT_EQ(g.bar, 0U);
	EXPECT_EQ(g.calc, 0U);
}

/* ExpSelContext::on_strategy_schedule（:91-95）-> ctx_on_calc(ET_SEL) */
TEST_F(R13ExpContext, sel_on_strategy_schedule_forwards_as_a_calc_event)
{
	SelContextPtr ctx = getRunner().getSelContext(_sel);
	ctx->on_strategy_schedule(20240605, 151000);

	EXPECT_EQ(g.calc, 1U);
	EXPECT_EQ(g.last_handle, _sel);
	EXPECT_EQ(g.last_date, 20240605U);
	EXPECT_EQ(g.last_time, 151000U);
}

/* ExpSelContext::on_bar_close（:104-108） */
TEST_F(R13ExpContext, sel_on_bar_close_forwards_handle_and_period)
{
	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	bar.time = 202406051500ULL;

	SelContextPtr ctx = getRunner().getSelContext(_sel);
	ctx->on_bar_close(kSelCode, "d1", &bar);

	EXPECT_EQ(g.bar, 1U);
	EXPECT_EQ(g.last_handle, _sel);
	EXPECT_EQ(g.last_code, kSelCode);
	EXPECT_EQ(g.last_period, "d1");
}

/* ExpSelContext::on_tick_updated（:117-125）未订阅支 */
TEST_F(R13ExpContext, sel_on_tick_updated_is_dropped_when_the_code_was_never_subscribed)
{
	SelContextPtr ctx = getRunner().getSelContext(_sel);
	ctx->on_tick_updated(kSelCode, NULL);
	EXPECT_EQ(g.tick, 0U);
}

/*
 * ExpHftContext::on_position（ExpHftContext.cpp:161-165）
 * 是 HFT 里唯一「只转发、不调基类」且不带指针解包的安全入口：
 * 转发 7 个标量 -> WtRtRunner.h:566 的 hft_on_position -> _cb_hft_pos。
 * 期望值就是本用例写进去的 4 个数字。
 */
TEST_F(R13ExpContext, hft_on_position_forwards_all_four_quantity_fields_exactly)
{
	// HftContextPtr 指向 IHftStraCtx，on_position 是 ITrdNotifySink 那一支，
	// 所以按实际类型（create_hft_context 造出来的就是 ExpHftContext）下转。
	ExpHftContext* ctx = static_cast<ExpHftContext*>(getRunner().getHftContext(_hft).get());
	ASSERT_TRUE(ctx != NULL);
	ctx->on_position(kCtaCode, true, 5.0, 3.0, 8.0, 6.0, 20240606);

	EXPECT_EQ(g.hft_pos, 1U);
	EXPECT_EQ(g.last_handle, _hft);
	EXPECT_EQ(g.last_code, kCtaCode);
	EXPECT_TRUE(g.last_islong);
	EXPECT_DOUBLE_EQ(g.last_prevol, 5.0);
	EXPECT_DOUBLE_EQ(g.last_preavail, 3.0);
	EXPECT_DOUBLE_EQ(g.last_newvol, 8.0);
	EXPECT_DOUBLE_EQ(g.last_newavail, 6.0);
}

TEST_F(R13ExpContext, hft_on_position_reports_short_side_with_islong_false)
{
	ExpHftContext* ctx = static_cast<ExpHftContext*>(getRunner().getHftContext(_hft).get());
	ASSERT_TRUE(ctx != NULL);
	ctx->on_position(kCtaCode, false, -2.0, -2.0, -7.0, -7.0, 20240606);

	EXPECT_EQ(g.hft_pos, 1U);
	EXPECT_FALSE(g.last_islong);
	EXPECT_DOUBLE_EQ(g.last_prevol, -2.0);
	EXPECT_DOUBLE_EQ(g.last_newavail, -7.0);
}

/*
 * ExpHftContext::on_bar（ExpHftContext.cpp:29-44）的守卫支：
 * newBar == NULL 时在 :33 就 return，既不求 realPeriod 也不调基类。
 * 非 NULL 路径要进 HftStraBaseCtx::on_bar 的 _ud_modified（构造函数没初始化，
 * HftStraBaseCtx.cpp:41-48 对照 HftStraBaseCtx.h:686）-> 留白，见 BUG-R13-002。
 */
TEST_F(R13ExpContext, hft_on_bar_with_null_bar_returns_before_any_forward)
{
	HftContextPtr ctx = getRunner().getHftContext(_hft);
	ctx->on_bar(kCtaCode, "m1", 1, NULL);

	EXPECT_EQ(g.bar, 0U);
	EXPECT_EQ(g.hft_pos, 0U);
}

/* 微数据三个入口只转发不调基类；本用例不注册对应槽位，验证「静默但不崩」
 * 且转发点确实被执行过（否则解引用发生在 ExpHftContext 里，就是 UB）。 */
TEST_F(R13ExpContext, hft_micro_data_forwards_reach_the_runner_without_a_bound_callback)
{
	register_hft_callbacks(cb_init, cb_tick, cb_bar, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, cb_hft_pos);
	HftContextPtr ctx = getRunner().getHftContext(_hft);

	ctx->on_order_queue(kCtaCode, NULL);
	ctx->on_order_detail(kCtaCode, NULL);
	ctx->on_transaction(kCtaCode, NULL);

	// ordque/orddtl/trans 三个槽位没注册 -> 分发层在 if(_cb_*) 假支返回
	EXPECT_EQ(g.bar, 0U);
	EXPECT_EQ(g.hft_pos, 0U);
	EXPECT_EQ(g.tick, 0U);
}

/*
 * ExpParser（ExpParser.cpp + ExpParser.h 的 9 个内联）：
 * 直接栈构造，绕开 create_ext_parser 的 _engine 解引用（BUG-R6-003）。
 * 每个方法都只把 _id 递给 runner 的 parser_* 转发口。
 */
TEST(R13ExpParser, lifecycle_forwards_its_own_id_to_every_parser_slot)
{
	g.clear();
	register_parser_callbacks(cb_parser_evt, cb_parser_sub);

	ExpParser parser("r13_parser_direct");
	EXPECT_TRUE(parser.isConnected());

	EXPECT_TRUE(parser.init(NULL));
	EXPECT_EQ(g.parser_evt, 1U);
	EXPECT_EQ(g.last_id, "r13_parser_direct");

	EXPECT_TRUE(parser.connect());
	EXPECT_EQ(g.parser_evt, 2U);

	EXPECT_TRUE(parser.disconnect());
	EXPECT_EQ(g.parser_evt, 3U);

	parser.release();
	EXPECT_EQ(g.parser_evt, 4U);
	EXPECT_EQ(g.last_id, "r13_parser_direct");

	unwire_all();
}

TEST(R13ExpParser, subscribe_and_unsubscribe_emit_one_event_per_code)
{
	g.clear();
	register_parser_callbacks(cb_parser_evt, cb_parser_sub);

	ExpParser parser("r13_parser_sub");
	CodeSet codes;
	codes.insert("SHFE.rb.2405");
	codes.insert("DCE.m.2409");
	codes.insert("SSE.STK.600000");

	parser.subscribe(codes);
	EXPECT_EQ(g.parser_sub, 3U);
	EXPECT_TRUE(g.last_subflag);

	parser.unsubscribe(codes);
	// 3 订 + 3 退 = 6；退订的最后一次必须是 isForSub=false
	EXPECT_EQ(g.parser_sub, 6U);
	EXPECT_FALSE(g.last_subflag);
	EXPECT_EQ(g.last_id, "r13_parser_sub");

	unwire_all();
}

/* ExpParser::registerSpi(NULL)：m_sink 赋 NULL 后 if(m_sink) 假支跳过取 BaseDataMgr */
TEST(R13ExpParser, register_spi_with_null_listener_skips_the_basedatamgr_pull)
{
	g.clear();
	unwire_all();

	ExpParser parser("r13_parser_spi");
	parser.registerSpi(NULL);
	// 没有 listener 就没有订阅动作，parser 槽位应保持安静
	EXPECT_EQ(g.parser_evt, 0U);
	EXPECT_EQ(g.parser_sub, 0U);
}

/*
 * ExpExecuter（ExpExecuter.h:36/43/51 的三个转发）：
 * IExecCommand 没有纯虚（IExecCommand.h:62-113），所以可直接栈构造。
 */
TEST(R13ExpExecuter, init_forwards_the_executer_name_to_the_registered_slot)
{
	g.clear();
	register_exec_callbacks(cb_exec_init, cb_exec_cmd);

	ExpExecuter exe("r13_exe_direct");
	EXPECT_STREQ(exe.name(), "r13_exe_direct");

	exe.init();
	EXPECT_EQ(g.exec_init, 1U);
	EXPECT_EQ(g.last_id, "r13_exe_direct");

	unwire_all();
}

TEST(R13ExpExecuter, set_position_emits_one_command_per_map_entry)
{
	g.clear();
	register_exec_callbacks(cb_exec_init, cb_exec_cmd);

	ExpExecuter exe("r13_exe_map");
	wt_hashmap<std::string, double> targets;
	targets["SHFE.rb.2405"] = 3.5;
	targets["DCE.m.2409"] = -1.25;

	exe.set_position(targets);
	EXPECT_EQ(g.exec_cmd, 2U);
	// 单次透传，值就是写进去的字面量
	EXPECT_DOUBLE_EQ(g.last_target, targets[g.last_code]);

	exe.on_position_changed("SSE.STK.600000", 100.0);
	EXPECT_EQ(g.exec_cmd, 3U);
	EXPECT_EQ(g.last_code, "SSE.STK.600000");
	EXPECT_DOUBLE_EQ(g.last_target, 100.0);

	unwire_all();
}

/*
 * 留白（触发条件写清楚，别在启用用例里踩）：
 *  - ExpCtaContext::on_init / ExpSelContext::on_init：基类 on_init 先 init_outputs()
 *    （CtaStraBaseCtx.cpp:77-83 无条件 fs::create_directories）再 load_data()
 *    -> 会在 CWD 落目录/文件，且 load_data 依赖 _data_mgr。
 *  - Exp*Context::on_session_end：基类走 save_data()（CtaStraBaseCtx.cpp:1102、
 *    SelStraBaseCtx.cpp:711）-> 同上。
 *  - ExpHftContext::on_channel_ready / on_channel_lost：_trader->id()（ExpHftContext.cpp:53、:66），
 *    _trader 未绑定且无默认初值 -> 野指针（BUG-R13-001）。
 *  - ExpHftContext::on_tick / on_entrust / on_order / on_trade：都要过基类的 _ud_modified
 *    或 getOrderTag/_trader（BUG-R13-002）。
 */
TEST(R13ExpContextBlanks, DISABLED_init_and_session_end_and_hft_trader_paths_need_output_and_trader_seams)
{
	WtRtRunner& r = getRunner();
	CtaContextPtr cta = r.getCtaContext(0);
	if (cta != NULL)
		cta->on_init();
}
