/*
 * R6 - WtRtRunner 实盘运行器「生命周期 + 扩展层」补充覆盖
 *
 * 目标文件：src/WtPorter/WtRtRunner.cpp（分派口径 missed=621/938）
 * q06 lane 已收口的面（registerXxx / ctx_on_* / hft_on_* / parser_init /
 * executer_init / load*Bars / feedRawBars / create*Context / get*Context）
 * 本文件不重写；这里补的是它没碰的那几段：
 *   - init()（107~127）
 *   - addCtaFactories/addSelFactories/addExeFactories/addHftFactories（1168~1186）
 *   - handleLogAppend + LOG_TAGS（1126~1139）
 *   - on_ext_parser_quote 的「Parser not exists」告警支（1243~1256）
 *   - createExtExecuter（305~312）
 * 崩溃/挂死/全局副作用类目标整类 DISABLED_，触发条件见文件末注释与 bugs/R6_porter_runner.md。
 *
 * 只引用 WtPorter 一族符号；不牵 WtBtPorter（两边各有一套同名 extern "C" 入口，
 * 同链必 duplicate，见 CMakeLists 注释与 BUG-L00-003）。
 * 只读生产代码；缺陷见 docs/迭代计划/迭代01/bugs/R6_porter_runner.md。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "WtCore/WtHelper.h"
#include "WtPorter/PorterDefs.h"
#include "WtPorter/WtPorter.h"
#include "WtPorter/WtRtRunner.h"

#include "Includes/WTSStruct.h"

USING_NS_WTP;

extern WtRtRunner& getRunner();

/* WtRtRunner.cpp:1126 的级别->标签表（全局、外部链接；handleLogAppend 按 ll-100 取用） */
extern const char* LOG_TAGS[];

namespace {

	/* 与 q06 的 Recorder 同形状、不同用途：这里只统计「扩展 parser/executer」通道 */
	struct ExtRec
	{
		uint32_t	evt = 0;
		uint32_t	sub = 0;
		uint32_t	init = 0;
		uint32_t	cmd = 0;
		std::string	last_id, last_code, last_msg;
		double		last_target = 0;
		WtUInt32	last_evt = 0;
		bool		last_sub = false;
		void clear() { *this = ExtRec(); }
	};

	ExtRec g_ext;

	void ext_exec_init(const char* id)
	{
		g_ext.init++;
		g_ext.last_id = (id == NULL) ? "<null>" : id;
	}

	void ext_exec_cmd(const char* id, const char* code, double target)
	{
		g_ext.cmd++;
		g_ext.last_id = (id == NULL) ? "<null>" : id;
		g_ext.last_code = (code == NULL) ? "<null>" : code;
		g_ext.last_target = target;
	}

	/* 绝对不存在的目录/文件：让 loadFactories 走守卫支，不触碰任何插件 */
	const char* kAbsentDir = "./r6_absent_factory_dir_zzz/";
	const char* kAbsentCfg = "./r6_absent_dtcfg_zzz.yaml";
	const char* kAbsentLogCfg = "./r6_absent_logcfg_zzz.yaml";

	std::string tail_slash(const std::string& s)
	{
		return s.empty() ? s : s.substr(s.size() - 1);
	}

} // namespace

/* -------------------------------------------------------------------------
 * init()：107~127
 * isFile=true 且文件不存在时，WTSLogger::init 在 StdFile::exists 守卫处直接返回
 * （WTSTools/WTSLogger.cpp:209），不起日志线程；剩下的只有 setInstDir/setGenerateDir。
 * ---------------------------------------------------------------------- */

TEST(R6PorterLifecycle, init_with_absent_logcfg_file_returns_true_and_stamps_inst_dir)
{
	WtHelper::setInstDir("");			// 清掉上一个用例可能留下的值，保证断言可证伪
	WtRtRunner r;
	EXPECT_TRUE(r.init(kAbsentLogCfg, true, ""));

	const std::string& inst = WtHelper::getInstDir();
	EXPECT_FALSE(inst.empty()) << "init() 没把安装目录写进 WtHelper";
	EXPECT_EQ(tail_slash(inst), "/") << "setInstDir 前应经 standardisePath 补尾斜杠";
}

TEST(R6PorterLifecycle, init_inline_content_that_fails_to_parse_also_returns_true)
{
	// isFile=false -> WTSLogger::init(logCfg,false,this)；load_from_content 解析失败即 return
	WtHelper::setInstDir("");
	WtRtRunner r;
	EXPECT_TRUE(r.init("{ this is not a valid log config", false, ""));
	EXPECT_FALSE(WtHelper::getInstDir().empty());
}

TEST(R6PorterLifecycle, init_with_gen_dir_normalizes_and_keeps_inst_dir_identical)
{
	WtRtRunner r;
	ASSERT_TRUE(r.init(kAbsentLogCfg, true, "./"));
	const std::string first = WtHelper::getInstDir();
	ASSERT_FALSE(first.empty());

	// 第二次 init 只重设同一批目录，不应把安装目录改短/改空
	WtRtRunner r2;
	ASSERT_TRUE(r2.init(kAbsentLogCfg, true, "/tmp"));
	EXPECT_EQ(WtHelper::getInstDir(), first);
}

/* -------------------------------------------------------------------------
 * addXxxFactories：1168~1186（四个转发函数 + 各自的 loadFactories 守卫支）
 * ---------------------------------------------------------------------- */

TEST(R6PorterFactories, cta_factory_absent_dir_returns_false)
{
	WtRtRunner r;
	EXPECT_FALSE(r.addCtaFactories(kAbsentDir));
}

TEST(R6PorterFactories, sel_factory_absent_dir_returns_false)
{
	WtRtRunner r;
	EXPECT_FALSE(r.addSelFactories(kAbsentDir));
}

TEST(R6PorterFactories, hft_factory_absent_dir_returns_false)
{
	WtRtRunner r;
	EXPECT_FALSE(r.addHftFactories(kAbsentDir));
}

TEST(R6PorterFactories, executer_factory_absent_dir_returns_false)
{
	WtRtRunner r;
	EXPECT_FALSE(r.addExeFactories(kAbsentDir));
}

TEST(R6PorterFactories, factory_path_pointing_at_a_file_does_not_return_cleanly)
{
	// StdFile::exists() 用的是 std::filesystem::exists（目录/文件都为真），
	// 守卫过了以后 boost 的 directory_iterator 对「文件」抛 filesystem_error。
	// 断言的是「不会安静地返回 false」这件事本身：返回值与异常二选一，异常优先。
	WtRtRunner r;
	bool threw = false;
	bool ret = false;
	try
	{
		ret = r.addCtaFactories(__FILE__);
	}
	catch (const std::exception&)
	{
		threw = true;
	}
	// 两条正当出路：抛异常（现状）或返回 false（将来加了类型守卫）；
	// 唯独「报告加载成功」不能接受 —— 那个路径上一份插件都没枚举到。
	EXPECT_TRUE(threw || !ret) << "把文件当目录传给 loadFactories 却返回了 true";
}

/* -------------------------------------------------------------------------
 * handleLogAppend + LOG_TAGS：1126~1139
 * ---------------------------------------------------------------------- */

TEST(R6PorterLogRoute, level_to_tag_table_is_the_documented_seven_entries)
{
	// Includes/WTSTypes.h:128 LL_ALL=100 .. LL_NONE=106，取用式是 LOG_TAGS[ll-100]
	EXPECT_STREQ(LOG_TAGS[0], "all");
	EXPECT_STREQ(LOG_TAGS[3], "warn");
	EXPECT_STREQ(LOG_TAGS[6], "none");
}

TEST(R6PorterLogRoute, handleLogAppend_accepts_every_declared_level)
{
	WtRtRunner r;
	const WTSLogLevel levels[] = { LL_ALL, LL_DEBUG, LL_INFO, LL_WARN, LL_ERROR, LL_FATAL, LL_NONE };
	for (WTSLogLevel ll : levels)
	{
		const size_t idx = (size_t)ll - 100u;
		ASSERT_LT(idx, 7u) << "级别枚举与 LOG_TAGS 长度不同步: ll=" << (uint32_t)ll;
		// 未 init 通知器时 _mq_sid==0，notify_log 立刻返回（EventNotifier.cpp:116）
		EXPECT_NO_THROW(r.handleLogAppend(ll, "r6 probe")) << "ll=" << (uint32_t)ll;
		// 空消息同样只走一次取表，不能因为 msg 长度为 0 就改道
		EXPECT_NO_THROW(r.handleLogAppend(ll, "")) << "ll=" << (uint32_t)ll;
	}
}

/* -------------------------------------------------------------------------
 * on_ext_parser_quote：1243~1256（未注册 parser 的告警支）
 * ---------------------------------------------------------------------- */

TEST(R6PorterExtParserQuote, unknown_parser_c_api_leaves_the_supplied_tick_untouched)
{
	WTSTickStruct tick;
	memset(&tick, 0, sizeof(tick));
	tick.action_time = 93000;
	tick.price = 1234.5;
	wt_strcpy(tick.code, "IF2108");
	wt_strcpy(tick.exchg, "SHFE");

	// Use the public C API path exercised by Q06PorterCapi.ParserPushQuoteIgnoresUnknownParserId;
	// do not call the internal method on a directly constructed, uninitialized WtRtRunner.
	// WTSTickStruct has no reference-count field, so preserve and compare its full byte image.
	unsigned char before[sizeof(tick)];
	memcpy(before, &tick, sizeof(tick));

	EXPECT_NO_THROW(parser_push_quote("r6_absent_parser", &tick, 0));

	EXPECT_EQ(0, memcmp(before, &tick, sizeof(tick)))
		<< "unknown parser C API must leave the borrowed tick unchanged";
	EXPECT_EQ(93000u, tick.action_time);
	EXPECT_DOUBLE_EQ(1234.5, tick.price);
	EXPECT_STREQ("IF2108", tick.code);
	EXPECT_STREQ("SHFE", tick.exchg);
}

/* -------------------------------------------------------------------------
 * createExtExecuter：305~312
 * ---------------------------------------------------------------------- */

TEST(R6PorterExtExecuter, create_ext_executer_returns_true_without_engine)
{
	// 走单例：ExpExecuter::init() 内部硬绑 getRunner()（见 ExpExecuter.cpp:27）
	WtRtRunner& r = getRunner();
	EXPECT_EQ(r.getEngine(), nullptr) << "本目标从不跑 config()，引擎必须仍是空的";
	EXPECT_TRUE(r.createExtExecuter("r6exe_a"));
}

TEST(R6PorterExtExecuter, executer_callbacks_fire_on_the_runner_that_registered_them)
{
	// executer_init / executer_set_position 的「已注册」支：回调记在谁身上就谁响
	WtRtRunner local;
	g_ext.clear();
	local.registerExecuterPorter(ext_exec_init, ext_exec_cmd);

	local.executer_init("r6exe_b");
	EXPECT_EQ(g_ext.init, 1u);
	EXPECT_EQ(g_ext.last_id, "r6exe_b");

	local.executer_set_position("r6exe_b", "IF2108", 3.5);
	EXPECT_EQ(g_ext.cmd, 1u);
	EXPECT_EQ(g_ext.last_code, "IF2108");
	EXPECT_DOUBLE_EQ(g_ext.last_target, 3.5);
}

/* -------------------------------------------------------------------------
 * DISABLED_ 组：崩溃 / 挂死 / 进程级副作用
 *
 * 触发条件：
 *  C1 config()（489~494）对 _config 与 basefiles 都没有 NULL 守卫：
 *     不存在的文件 -> load_from_file 返回 NULL -> 第 494 行 _config->get 段错误；
 *     合法但缺 "basefiles" 的内容 -> cfgBF 为 NULL -> 第 495 行 cfgBF->get 段错误。
 *     两条都会打死 gtest 宿主，catch(...) 抓不到 SIGSEGV。
 *  C2 createExtParser()（295~303）无条件 `_engine->get_basedata_mgr()`，
 *     未 config 过时 _engine==NULL -> 段错误。
 *  C3 get_raw_stdcode()（361~366）同理解引用 _engine —— 同族缺陷已由
 *     q06 侧 BUG-Q06-002 登记，这里只保留用例形状。
 *  C4 feedAdjFactors()（277~280）不判空即调 _feeder_fcts（构造列表里没有它）
 *     -> 跳飞指针；同族缺陷 = BUG-Q06-004。
 *  C5 run()（1091~1124）：_engine 为空段错误；非空则起引擎线程并在
 *     `while(!_to_exit) sleep(10ms)` 上挂死。
 *  C6 release()（1141~1155）：`WTSLogger::stop()` 会把进程级 m_bStopped 置位并
 *     spdlog::shutdown()，同二进制里后续用例的日志行为被永久改变 ->
 *     只能在「每用例一进程」的采样口径下跑，本轮按约束留给主控裁定。
 * ---------------------------------------------------------------------- */

TEST(R6PorterConfig, DISABLED_missing_config_file_derefs_null_config)
{
	WtRtRunner r;
	EXPECT_FALSE(r.config(kAbsentCfg, true));		// C1
}

TEST(R6PorterConfig, DISABLED_valid_content_without_basefiles_derefs_null_section)
{
	WtRtRunner r;
	EXPECT_FALSE(r.config("{}", false));			// C1 第二支
}

TEST(R6PorterExtParser, DISABLED_create_ext_parser_without_engine_crashes)
{
	WtRtRunner r;
	EXPECT_TRUE(r.createExtParser("r6parser"));		// C2
}

TEST(R6PorterRawCode, DISABLED_get_raw_stdcode_without_engine_crashes)
{
	WtRtRunner r;
	EXPECT_STREQ(r.get_raw_stdcode("IF2108.CFFEX"), "IF2108");	// C3
}

TEST(R6PorterFeed, DISABLED_feed_adj_factors_without_feeder_jumps_to_garbage)
{
	WtRtRunner r;
	uint32_t dates[1] = { 20210104 };
	double factors[1] = { 1.0 };
	r.feedAdjFactors("IF2108", dates, factors, 1);	// C4
}

TEST(R6PorterRunRelease, DISABLED_run_without_engine_and_release_global_logger_stop)
{
	WtRtRunner r;
	r.run(true);		// C5
	r.release();		// C6
}
