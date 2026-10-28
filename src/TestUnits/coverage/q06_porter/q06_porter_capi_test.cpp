/*
 * Q06 - WtPorter 实盘侧 C 导出层：未初始化/未知句柄的参数校验与错误码
 *
 * 覆盖 src/WtPorter/WtPorter.cpp 的导出入口（get_version / write_log /
 * feed_raw_ticks / cta_* / sel_* / hft_* 的 `ctx == NULL` 保护分支 /
 * cta_get_all_position 与 sel_get_all_position 的终止回调 / parser_push_quote
 * 的「Parser not exists」分支），以及 src/WtPorter/WtRtRunner.cpp 中
 * registerXxx 六个纯赋值函数（经由导出层调用）。
 *
 * 不启动 runner、不连柜台、不开共享内存：全部走「未注册即拒绝」路径。
 * 只读生产代码；缺陷记录见 docs/迭代计划/迭代01/bugs/q06_porters.md。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "WtPorter/PorterDefs.h"
#include "WtPorter/WtPorter.h"
#include "Includes/WTSStruct.h"

USING_NS_WTP;

namespace {

	// 未创建任何上下文时使用的非法句柄（getContext 走 map 未命中分支）
	const CtxHandler kBadHandle = 0x7FFF0001UL;

	/* 回调计数表：导出层是 C 函数指针，只能用进程级变量收集 */
	uint32_t g_pos_cb_calls = 0;
	double  g_pos_cb_value = -1.0;
	std::string g_pos_cb_code;
	bool      g_pos_cb_islast = false;

	void pos_cb(CtxHandler /*cHandle*/, const char* stdCode, double position, bool isLast)
	{
		g_pos_cb_calls++;
		g_pos_cb_code = (stdCode == NULL) ? "<null>" : stdCode;
		g_pos_cb_value = position;
		g_pos_cb_islast = isLast;
	}

	void reset_pos_cb()
	{
		g_pos_cb_calls = 0;
		g_pos_cb_value = -1.0;
		g_pos_cb_code.clear();
		g_pos_cb_islast = false;
	}

	// bars/ticks 回调在未知句柄分支里不会被调用，这里只用于占位传参
	void bars_cb(CtxHandler /*cHandle*/, const char* /*stdCode*/, const char* /*period*/,
		WTSBarStruct* /*bar*/, WtUInt32 /*count*/, bool /*isLast*/)
	{
	}

	void ticks_cb(CtxHandler /*cHandle*/, const char* /*stdCode*/,
		WTSTickStruct* /*tick*/, WtUInt32 /*count*/, bool /*isLast*/)
	{
	}

	void ordque_cb(CtxHandler /*cHandle*/, const char* /*stdCode*/,
		WTSOrdQueStruct* /*item*/, WtUInt32 /*count*/, bool /*isLast*/)
	{
	}

	void orddtl_cb(CtxHandler /*cHandle*/, const char* /*stdCode*/,
		WTSOrdDtlStruct* /*item*/, WtUInt32 /*count*/, bool /*isLast*/)
	{
	}

	void trans_cb(CtxHandler /*cHandle*/, const char* /*stdCode*/,
		WTSTransStruct* /*item*/, WtUInt32 /*count*/, bool /*isLast*/)
	{
	}
}

/*
 * get_version()：PLATFORM_NAME + " " + WT_VERSION + " Build@" + __DATE__ + " " + __TIME__
 * 断言只取结构（前缀/包含关系），不断言墙钟取值。
 */
TEST(Q06PorterCapi, VersionStringHasPlatformPrefix)
{
	const char* ver = get_version();
	ASSERT_STREQ(ver, get_version());	// 第二次走缓存分支，同一指针

	std::string s(ver);
	EXPECT_NE(s.find("UNIX"), std::string::npos) << s;
	EXPECT_NE(s.find("Build@"), std::string::npos) << s;
	EXPECT_GT(s.size(), strlen("UNIX Build@"));
	// 平台名与版本号之间恰好一个空格
	EXPECT_EQ(s.compare(0, 5, "UNIX "), 0) << s;
}

/*
 * write_log() 的两个分支：catName 非空走 log_raw_by_cat，空串走 log_raw。
 * WTSLogger 未 init 时（m_bInited=false）自身有兜底，不会落文件。
 */
TEST(Q06PorterCapi, WriteLogAcceptsNamedAndEmptyCategory)
{
	write_log(LOG_LEVEL_INFO, "q06 default category", "");
	write_log(LOG_LEVEL_DEBUG, "q06 named category", "porter");
	write_log(LOG_LEVEL_WARN, "q06 warn category", "porter");
	write_log(LOG_LEVEL_ERROR, "q06 error category", "");
	SUCCEED();
}

/* feed_raw_ticks 在实盘侧是「未实现」桩：只打错误日志，不得崩溃 */
TEST(Q06PorterCapi, FeedRawTicksIsNotImplementedStub)
{
	WTSTickStruct tick;
	memset(&tick, 0, sizeof(tick));
	feed_raw_ticks(&tick, 1);
	feed_raw_ticks(NULL, 0);
	SUCCEED();
}

/* CTA 族：未知句柄必须被 ctx==NULL 拦住，返回中性值而不是崩溃 */
TEST(Q06PorterCapi, CtaGettersRejectUnknownHandle)
{
	EXPECT_DOUBLE_EQ(cta_get_position(kBadHandle, "SHFE.au2406", true, ""), 0.0);
	EXPECT_DOUBLE_EQ(cta_get_position_profit(kBadHandle, "SHFE.au2406"), 0.0);
	EXPECT_DOUBLE_EQ(cta_get_position_avgpx(kBadHandle, "SHFE.au2406"), 0.0);
	EXPECT_DOUBLE_EQ(cta_get_detail_cost(kBadHandle, "SHFE.au2406", "t1"), 0.0);
	EXPECT_DOUBLE_EQ(cta_get_detail_profit(kBadHandle, "SHFE.au2406", "t1", 0), 0.0);
	EXPECT_EQ(cta_get_detail_entertime(kBadHandle, "SHFE.au2406", "t1"), 0ULL);
	EXPECT_EQ(cta_get_first_entertime(kBadHandle, "SHFE.au2406"), 0ULL);
	EXPECT_EQ(cta_get_last_entertime(kBadHandle, "SHFE.au2406"), 0ULL);
	EXPECT_EQ(cta_get_last_exittime(kBadHandle, "SHFE.au2406"), 0ULL);
	EXPECT_DOUBLE_EQ(cta_get_last_enterprice(kBadHandle, "SHFE.au2406"), 0.0);
	EXPECT_DOUBLE_EQ(cta_get_fund_data(kBadHandle, 0), 0.0);
	EXPECT_EQ(cta_get_bars(kBadHandle, "SHFE.au2406", "m1", 10, false, bars_cb), 0U);
	EXPECT_EQ(cta_get_ticks(kBadHandle, "SHFE.au2406", 10, ticks_cb), 0U);
	EXPECT_FALSE(cta_register_index_line(kBadHandle, "idx", "line", 0));
	EXPECT_FALSE(cta_add_index_baseline(kBadHandle, "idx", "line", 1.0));
	EXPECT_FALSE(cta_set_index_value(kBadHandle, "idx", "line", 1.0));
}

/* cta_get_last_entertag 未命中时直接 return 0（即 NULL），与 cta_load_userdata 返回 defVal 不对称 */
TEST(Q06PorterCapi, CtaStringGettersUnknownHandle)
{
	EXPECT_STREQ(cta_load_userdata(kBadHandle, "k", "fallback"), "fallback");
	// 见缺陷 Q06-001：无上下文时 entertag 返回空指针而不是空串/默认值
	const char* tag = cta_get_last_entertag(kBadHandle, "SHFE.au2406");
	EXPECT_TRUE(tag == NULL || strlen(tag) == 0)
		<< (tag == NULL ? "<null>" : tag);
}

/* CTA 族的 void 入口：未知句柄应静默返回，不产生副作用 */
TEST(Q06PorterCapi, CtaVoidEntriesIgnoreUnknownHandle)
{
	cta_enter_long(kBadHandle, "SHFE.au2406", 2, "q06", 0, 0);
	cta_exit_long(kBadHandle, "SHFE.au2406", 1, "q06", 0, 0);
	cta_enter_short(kBadHandle, "SHFE.au2406", 2, "q06", 0, 0);
	cta_exit_short(kBadHandle, "SHFE.au2406", 1, "q06", 0, 0);
	cta_set_position(kBadHandle, "SHFE.au2406", 3, "q06", 0, 0);
	cta_log_text(kBadHandle, LOG_LEVEL_DEBUG, "q06 debug");
	cta_log_text(kBadHandle, LOG_LEVEL_INFO, "q06 info");
	cta_log_text(kBadHandle, LOG_LEVEL_WARN, "q06 warn");
	cta_log_text(kBadHandle, LOG_LEVEL_ERROR, "q06 error");
	cta_log_text(kBadHandle, 99, "q06 unknown level falls to default");
	cta_save_userdata(kBadHandle, "k", "v");
	cta_sub_ticks(kBadHandle, "SHFE.au2406");
	cta_sub_bar_events(kBadHandle, "SHFE.au2406", "m1");
	cta_set_chart_kline(kBadHandle, "SHFE.au2406", "m1");
	cta_add_chart_mark(kBadHandle, 100.0, "icon", "tag");
	cta_register_index(kBadHandle, "idx", 0);
	SUCCEED();
}

/* SEL 族同样受 ctx==NULL 保护 */
TEST(Q06PorterCapi, SelEntriesRejectUnknownHandle)
{
	EXPECT_DOUBLE_EQ(sel_get_position(kBadHandle, "SSE.600000", true, ""), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_position_profit(kBadHandle, "SSE.600000"), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_position_avgpx(kBadHandle, "SSE.600000"), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_detail_cost(kBadHandle, "SSE.600000", "t1"), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_detail_profit(kBadHandle, "SSE.600000", "t1", 0), 0.0);
	EXPECT_EQ(sel_get_detail_entertime(kBadHandle, "SSE.600000", "t1"), 0ULL);
	EXPECT_EQ(sel_get_first_entertime(kBadHandle, "SSE.600000"), 0ULL);
	EXPECT_EQ(sel_get_last_entertime(kBadHandle, "SSE.600000"), 0ULL);
	EXPECT_EQ(sel_get_last_exittime(kBadHandle, "SSE.600000"), 0ULL);
	EXPECT_DOUBLE_EQ(sel_get_last_enterprice(kBadHandle, "SSE.600000"), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_fund_data(kBadHandle, 1), 0.0);
	EXPECT_EQ(sel_get_bars(kBadHandle, "SSE.600000", "d1", 5, bars_cb), 0U);
	EXPECT_EQ(sel_get_ticks(kBadHandle, "SSE.600000", 5, ticks_cb), 0U);
	EXPECT_STREQ(sel_load_userdata(kBadHandle, "k", "sel_default"), "sel_default");

	sel_set_position(kBadHandle, "SSE.600000", 100, "q06");
	sel_log_text(kBadHandle, LOG_LEVEL_INFO, "q06 sel log");
	sel_save_userdata(kBadHandle, "k", "v");
	sel_sub_ticks(kBadHandle, "SSE.600000");
	SUCCEED();
}

/* HFT 族：数值入口返回 0/false，字符串入口返回空串 */
TEST(Q06PorterCapi, HftEntriesRejectUnknownHandle)
{
	EXPECT_DOUBLE_EQ(hft_get_position(kBadHandle, "SHFE.au2406", true), 0.0);
	EXPECT_DOUBLE_EQ(hft_get_position_profit(kBadHandle, "SHFE.au2406"), 0.0);
	EXPECT_DOUBLE_EQ(hft_get_position_avgpx(kBadHandle, "SHFE.au2406"), 0.0);
	EXPECT_DOUBLE_EQ(hft_get_undone(kBadHandle, "SHFE.au2406"), 0.0);
	EXPECT_FALSE(hft_cancel(kBadHandle, 1234));
	EXPECT_STREQ(hft_cancel_all(kBadHandle, "SHFE.au2406", true), "");
	EXPECT_STREQ(hft_buy(kBadHandle, "SHFE.au2406", 100.0, 1, "q06", 0), "");
	EXPECT_STREQ(hft_sell(kBadHandle, "SHFE.au2406", 100.0, 1, "q06", 0), "");
	EXPECT_STREQ(hft_load_userdata(kBadHandle, "k", "hft_default"), "hft_default");
	EXPECT_EQ(hft_get_bars(kBadHandle, "SHFE.au2406", "m1", 3, bars_cb), 0U);
	EXPECT_EQ(hft_get_ticks(kBadHandle, "SHFE.au2406", 3, ticks_cb), 0U);
	EXPECT_EQ(hft_get_ordque(kBadHandle, "SHFE.au2406", 3, ordque_cb), 0U);
	EXPECT_EQ(hft_get_orddtl(kBadHandle, "SHFE.au2406", 3, orddtl_cb), 0U);
	EXPECT_EQ(hft_get_trans(kBadHandle, "SHFE.au2406", 3, trans_cb), 0U);

	hft_log_text(kBadHandle, LOG_LEVEL_INFO, "q06 hft log");
	hft_sub_ticks(kBadHandle, "SHFE.au2406");
	hft_sub_order_queue(kBadHandle, "SHFE.au2406");
	hft_sub_order_detail(kBadHandle, "SHFE.au2406");
	hft_sub_transaction(kBadHandle, "SHFE.au2406");
	hft_save_userdata(kBadHandle, "k", "v");
	SUCCEED();
}

/*
 * 未知句柄下的 enum_position 入口仍然要给出终止回调（isLast=true），
 * 否则上层 Python/其他语言的枚举循环会永远等不到结束标记。
 */
TEST(Q06PorterCapi, EnumPositionStillEmitsTerminatorForUnknownHandle)
{
	reset_pos_cb();
	cta_get_all_position(kBadHandle, pos_cb);
	EXPECT_EQ(g_pos_cb_calls, 1U);
	EXPECT_EQ(g_pos_cb_code, "");
	EXPECT_DOUBLE_EQ(g_pos_cb_value, 0.0);
	EXPECT_TRUE(g_pos_cb_islast);

	reset_pos_cb();
	sel_get_all_position(kBadHandle, pos_cb);
	EXPECT_EQ(g_pos_cb_calls, 1U);
	EXPECT_TRUE(g_pos_cb_islast);
}

/* parser_push_quote：未 create_ext_parser 的 id 走 "Parser not exists" 告警分支 */
TEST(Q06PorterCapi, ParserPushQuoteIgnoresUnknownParserId)
{
	WTSTickStruct tick;
	memset(&tick, 0, sizeof(tick));
	strncpy(tick.code, "au2406", sizeof(tick.code) - 1);
	strncpy(tick.exchg, "SHFE", sizeof(tick.exchg) - 1);
	tick.price = 100.5;

	parser_push_quote("q06_not_registered", &tick, 0);
	SUCCEED();
}

/*
 * 注册族入口只做赋值（WtRtRunner::registerXxx 六个函数），
 * 传 NULL 也应安全落地；真正的分发在 q06_porter_runner_test.cpp 验证。
 */
TEST(Q06PorterCapi, CallbackRegistrationAcceptsNullAndReplaces)
{
	register_evt_callback(NULL);
	register_cta_callbacks(NULL, NULL, NULL, NULL, NULL);
	register_sel_callbacks(NULL, NULL, NULL, NULL, NULL);
	register_hft_callbacks(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
	register_parser_callbacks(NULL, NULL);
	register_exec_callbacks(NULL, NULL);
	register_ext_data_loader(NULL, NULL, NULL, NULL);

	parser_push_quote("q06_not_registered", NULL, 0);
	SUCCEED();
}

/*
 * 收口：以下入口在未初始化（getEngine() 返回 NULL）时会直接解引用空指针，
 * 属于崩溃类输入，统一关进 DISABLED_ 用例，启用用例里一次都不碰。
 * 见缺陷 Q06-002（无 null 校验的引擎直取入口清单）。
 */
TEST(Q06PorterCapi, DISABLED_UninitializedEngineGettersCrash)
{
	// cta_get_price / cta_get_day_price / cta_get_tdate / cta_get_date / cta_get_time
	// sel_get_price / sel_get_day_price / sel_get_date / sel_get_time / sel_get_tdate
	// hft_get_price / hft_get_date / hft_get_time / hft_get_secs
	// get_raw_stdcode（内部 _engine->get_rawcode）
	// create_ext_parser / create_ext_executer（内部 _engine->get_basedata_mgr()）
	// reg_cta_factories 等四个工厂入口（依赖 DLL 目录扫描，不落在此处）
	// init_porter / config_porter / run_porter / release_porter（起线程、join，禁止进入）
	(void)0;
}
