/*
 * R35 - CTA 订阅/图表/指标导出族 + ExpParser::registerSpi 命中支
 *
 * 目标：src/WtPorter/WtPorter.cpp
 *   cta_sub_bar_events L1002-1009、cta_set_chart_kline L1019-1026、
 *   cta_add_chart_mark L1037-1044、cta_register_index L1054-1061、
 *   cta_register_index_line L1072-1079、cta_add_index_baseline L1090-1097、
 *   cta_set_index_value L1108-1115
 *   src/WtPorter/ExpParser.cpp registerSpi 非空 listener 支 L98-106
 *
 * 分工：R13 在留白清单第四节里明确把「图表/指标族 6 个 ≈45 行」列为
 * 「未逐行核过，本轮按不确证不启用」，把 ExpParser 的 registerSpi(非空) 支
 * 列为「需要一个 IParserSpi 实现，本轮未造」。本文件把这两块逐行核完后收下：
 *  - set_chart_kline/register_index/register_index_line/add_index_baseline
 *    只动 _chart_code/_chart_indice 这类实例 map（CtaStraBaseCtx.cpp:1966-2027），
 *    指针解引用为零；
 *  - add_chart_mark/set_index_value 都有 `!_is_in_schedule` 早退守卫
 *    （CtaStraBaseCtx.cpp:1974-1977/2029-2032），非调度期不会碰 _engine；
 *    `_is_in_schedule` 由 on_calculate 置位（CtaStraBaseCtx.cpp:939），构造即 false（:66），
 *    所以「返回值 false」本身就是守卫被走到的证明。
 *
 * 断言面：bool 入口的 true/false 次序（未注册->false，注册后->true）、
 * 非调度期两个写入口恒 false、void 入口「六个派发槽计数恒 0」，
 * registerSpi 用 sink 自己的计数钉「getBaseDataMgr 恰好被拉取 1 次 / NULL 时 0 次」。
 * 不栈构造 WtRtRunner；ExpParser 走 R13 验证过的栈构造（绕开 createExtParser 的空 _engine）。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "WtPorter/ExpParser.h"
#include "WtPorter/PorterDefs.h"
#include "WtPorter/WtPorter.h"
#include "WtPorter/WtRtRunner.h"

#include "Includes/IParserApi.h"
#include "Includes/WTSStruct.h"
#include "WTSTools/WTSBaseDataMgr.h"

USING_NS_WTP;

extern WtRtRunner& getRunner();

namespace {

	const char* kMonthCode = "SHFE.rb.2510";

	/* ---- 六族派发槽探针：void 入口必须一个都不敲 ---- */
	uint32_t g_cta_init = 0, g_cta_tick = 0, g_cta_calc = 0, g_cta_bar = 0, g_cta_sess = 0, g_cta_cond = 0;

	void cb_cta_init(CtxHandler) { g_cta_init++; }
	void cb_cta_tick(CtxHandler, const char*, WTSTickStruct*) { g_cta_tick++; }
	void cb_cta_calc(CtxHandler, WtUInt32, WtUInt32) { g_cta_calc++; }
	void cb_cta_bar(CtxHandler, const char*, const char*, WTSBarStruct*) { g_cta_bar++; }
	void cb_sess(CtxHandler, WtUInt32, bool) { g_cta_sess++; }
	void cb_cond(CtxHandler, const char*, double, double, const char*) { g_cta_cond++; }

	uint32_t all_cb_total()
	{
		return g_cta_init + g_cta_tick + g_cta_calc + g_cta_bar + g_cta_sess + g_cta_cond;
	}

	void reset_cb()
	{
		g_cta_init = g_cta_tick = g_cta_calc = g_cta_bar = g_cta_sess = g_cta_cond = 0;
	}

	/* ---- ExpParser::registerSpi 的假 sink：唯一被 ExpParser.cpp 触碰的是 getBaseDataMgr ---- */
	WTSBaseDataMgr g_bdm;

	struct SinkRec
	{
		uint32_t bdm_calls = 0;
		uint32_t quote_calls = 0;
		uint32_t symbol_calls = 0;
		uint32_t log_calls = 0;
		void clear() { *this = SinkRec(); }
	};
	SinkRec g_sink;

	class R35ParserSink : public IParserSpi
	{
	public:
		void handleSymbolList(const WTSArray*) override { g_sink.symbol_calls++; }
		void handleQuote(WTSTickData*, uint32_t) override { g_sink.quote_calls++; }
		void handleParserLog(WTSLogLevel, const char*) override { g_sink.log_calls++; }
		IBaseDataMgr* getBaseDataMgr() override { g_sink.bdm_calls++; return &g_bdm; }
	};

	/* parser 生命周期事件计数（register_parser_callbacks 的 evt 槽） */
	uint32_t g_parser_evt = 0, g_parser_sub = 0;
	std::string g_parser_last_id;

	void cb_parser_evt(WtUInt32, const char* id)
	{
		g_parser_evt++;
		g_parser_last_id = (id == NULL) ? "<null>" : id;
	}
	void cb_parser_sub(const char*, const char*, bool) { g_parser_sub++; }

} // namespace

class R35PorterChart : public ::testing::Test
{
protected:
	CtxHandler _cta = 0;

	void SetUp() override
	{
		reset_cb();
		register_cta_callbacks(cb_cta_init, cb_cta_tick, cb_cta_calc, cb_cta_bar, cb_sess, cb_cond);

		_cta = create_cta_context("r35_cta_chart", 0);
		ASSERT_NE(_cta, 0U);
		ASSERT_TRUE(getRunner().getCtaContext(_cta) != NULL);
	}

	void TearDown() override
	{
		register_cta_callbacks(NULL, NULL, NULL, NULL, NULL, NULL);
		reset_cb();
	}
};

/*
 * cta_sub_bar_events（WtPorter.cpp:1002-1009 -> CtaStraBaseCtx.cpp:1675-1680）：
 * 只往实例的 _kline_tags["SHFE.rb.2510#m1"] 写 _notify=true，不订阅 tick（那是
 * stra_sub_ticks 的活，它带 _engine->sub_tick，属 R13 已登记的崩族，不在本条）、
 * 也不派发任何已注册槽。重复两次仍必须全静默。
 */
TEST_F(R35PorterChart, SubBarEventsDispatchesNothing)
{
	cta_sub_bar_events(_cta, kMonthCode, "m1");
	cta_sub_bar_events(_cta, kMonthCode, "m1");
	cta_sub_bar_events(_cta, "SSE.STK.600000", "d1");

	EXPECT_EQ(all_cb_total(), 0u) << "订阅入口只应写 _kline_tags，不得敲任何已注册回调";
	EXPECT_TRUE(getRunner().getCtaContext(_cta) != NULL) << "订阅不得改变上下文表";
}

/*
 * 指标注册的先后次序必须决定两个 bool 入口的返回值
 * （CtaStraBaseCtx.cpp:1998-2012：_chart_indice find-miss 记 error 返回 false，
 * 注册后落线返回 true）。未注册->false 与注册后->true 两头都钉住，
 * 任何「注册被忽略」或「find 判定写反」都会红。
 */
TEST_F(R35PorterChart, IndexLineFollowsRegistrationOrder)
{
	EXPECT_FALSE(cta_register_index_line(_cta, "r35_ma", "close", 0))
		<< "指标未注册前不得成功（CtaStraBaseCtx.cpp:2000-2004）";

	cta_register_index(_cta, "r35_ma", 1);

	EXPECT_TRUE(cta_register_index_line(_cta, "r35_ma", "close", 0));
	EXPECT_TRUE(cta_register_index_line(_cta, "r35_ma", "open", 1)) << "同指标可叠加多条线";
	EXPECT_FALSE(cta_register_index_line(_cta, "r35_never_registered", "close", 0));

	EXPECT_EQ(all_cb_total(), 0u) << "注册族只动 _chart_indice，不派发事件";
}

/* add_index_baseline 与 register_index_line 同守卫：未知指标 false，已注册指标 true。 */
TEST_F(R35PorterChart, IndexBaselineFollowsRegistrationOrder)
{
	EXPECT_FALSE(cta_add_index_baseline(_cta, "r35_bl", "axis", 0.0));

	cta_register_index(_cta, "r35_bl", 0);
	EXPECT_TRUE(cta_add_index_baseline(_cta, "r35_bl", "axis", 0.0));
	EXPECT_TRUE(cta_add_index_baseline(_cta, "r35_bl", "zero", 3520.5));
	EXPECT_FALSE(cta_add_index_baseline(_cta, "r35_other", "axis", 1.0));
}

/*
 * set_index_value / add_chart_mark 在非调度期必须被 `!_is_in_schedule` 守卫拦下
 * （CtaStraBaseCtx.cpp:2029-2032 / :1974-1977）：前者精确返回 false，
 * 后者是 void，用「六个派发槽计数仍为 0 + 后续注册查询返回 true」证明守卫早退、
 * 状态完好，而不是撞进 `_engine->notify_chart_marker`（未 init 引擎，撞进即崩，
 * 守卫若被删掉本条会以 SIGSEGV 红）。`_is_in_schedule` 的置位点 grep 实测只有
 * CtaStraBaseCtx.cpp:939（on_calculate 内），构造列表 :66 置 false。
 */
TEST_F(R35PorterChart, ChartWritesRefusedOutsideScheduleWithoutTouchingEngine)
{
	cta_register_index(_cta, "r35_ma", 1);
	ASSERT_TRUE(cta_register_index_line(_cta, "r35_ma", "close", 0));

	// 线已注册，但非调度期仍然 false —— false 只能来自守卫而不是 find-miss
	EXPECT_FALSE(cta_set_index_value(_cta, "r35_ma", "close", 3521.25));
	EXPECT_FALSE(cta_set_index_value(_cta, "r35_unknown", "close", 1.0));

	cta_add_chart_mark(_cta, 3521.25, "r35_icon", "r35_tag");
	EXPECT_EQ(all_cb_total(), 0u);

	// 上下文状态完好：同一指标再查询/加线依旧 true
	EXPECT_TRUE(cta_register_index_line(_cta, "r35_ma", "close", 2));
}

/*
 * set_chart_kline 只写 _chart_code/_chart_period 两个 string
 * （CtaStraBaseCtx.cpp:1966-1970），不派发、不建文件；用「派发 0 次 + 句柄仍可查 +
 * 后续指标注册次序不受影响」钉住它没有暗通道。
 */
TEST_F(R35PorterChart, SetChartKlineIsPureLocalState)
{
	cta_set_chart_kline(_cta, kMonthCode, "m1");
	cta_set_chart_kline(_cta, "SSE.STK.600000", "d1");

	EXPECT_EQ(all_cb_total(), 0u);
	ASSERT_TRUE(getRunner().getCtaContext(_cta) != NULL);

	cta_register_index(_cta, "r35_after_chart", 0);
	EXPECT_TRUE(cta_register_index_line(_cta, "r35_after_chart", "l", 0));
}

/*
 * ExpParser::registerSpi 的非空支（ExpParser.cpp:104-105）：
 * 命中 listener 时**恰好拉取一次** getBaseDataMgr；传 NULL 时整段跳过（R13 已取过
 * 该支，这里用 sink 计数做对照组）。栈构造 ExpParser，绕开 createExtParser 的空
 * _engine（BUG-R6-003）。registerSpi 之后 parser 的四个生命周期入口仍各自只发
 * 1 次事件、id 恒为自身，且没有任何行情/合约列表被路由进 sink。
 */
TEST(R35PorterExpParserSpi, RegisterSpiWithLiveSinkPullsBaseDataMgrOnce)
{
	g_sink.clear();
	g_parser_evt = g_parser_sub = 0;
	getRunner().registerParserPorter(cb_parser_evt, cb_parser_sub);

	{
		R35ParserSink sink;
		ExpParser parser("r35_spi_parser");

		parser.registerSpi(&sink);
		EXPECT_EQ(g_sink.bdm_calls, 1u) << "ExpParser.cpp:104-105：m_sink 非空才 getBaseDataMgr()";

		parser.registerSpi(NULL);          // 假支：不得再触碰任何 sink
		parser.init(NULL);
		parser.connect();
		CodeSet subs;
		subs.insert("SHFE.rb.2510");
		subs.insert("SSE.STK.600000");
		parser.subscribe(subs);
		CodeSet unsubs;
		unsubs.insert("SHFE.rb.2510");
		parser.unsubscribe(unsubs);
		parser.disconnect();
		parser.release();

		EXPECT_EQ(g_sink.bdm_calls, 1u) << "NULL listener 分支不得拉取 BaseDataMgr";
		EXPECT_EQ(g_sink.quote_calls, 0u);
		EXPECT_EQ(g_sink.symbol_calls, 0u);
		EXPECT_EQ(g_sink.log_calls, 0u);

		// evt 槽：init/connect/disconnect/release 各 1 次（ExpParser.cpp:26-65）
		EXPECT_EQ(g_parser_evt, 4u);
		// sub 槽：2 订 + 1 退（ExpParser.cpp:72-91）
		EXPECT_EQ(g_parser_sub, 3u);
		EXPECT_EQ(g_parser_last_id, "r35_spi_parser");

		EXPECT_TRUE(parser.isConnected());
	}

	getRunner().registerParserPorter(NULL, NULL);
	g_sink.clear();
}
