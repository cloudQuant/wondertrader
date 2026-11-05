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
#include <unistd.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

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
	namespace std_fs = std::filesystem;

	class EmptyFactoryDir
	{
	public:
		EmptyFactoryDir()
		{
			const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
			_path = std_fs::temp_directory_path() / ("r6_factory_dir_" + std::to_string(static_cast<long>(::getpid())) + "_" + std::to_string(nonce));
			std::error_code ec;
			_ok = std_fs::create_directory(_path, ec) && !ec;
		}

		~EmptyFactoryDir()
		{
			if (!_ok)
				return;
			std::error_code ec;
			std_fs::permissions(_path, std_fs::perms::owner_all, std_fs::perm_options::add, ec);
			std_fs::remove_all(_path, ec);
		}

		bool ok() const { return _ok; }
		const std_fs::path& path() const { return _path; }

	private:
		std_fs::path _path;
		bool _ok = false;
	};

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

	class R6ConfigTempDir
	{
	public:
		R6ConfigTempDir()
		{
			const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
			_path = std_fs::temp_directory_path() / ("r6_config_" + std::to_string(static_cast<long>(::getpid())) + "_" + std::to_string(nonce));
			std::error_code ec;
			_ok = std_fs::create_directories(_path, ec) && !ec;
		}

		~R6ConfigTempDir()
		{
			if (_ok)
			{
				std::error_code ec;
				std_fs::remove_all(_path, ec);
			}
		}

		bool ok() const { return _ok; }
		const std_fs::path& path() const { return _path; }

	private:
		std_fs::path _path;
		bool _ok = false;
	};

	bool write_r6_file(const std_fs::path& path, const std::string& content)
	{
		std::ofstream file(path);
		if (!file.is_open())
			return false;
		file << content;
		return file.good();
	}

	WtRtRunner* new_r6_runner(const std::string& genDir)
	{
		WtRtRunner* runner = new WtRtRunner();
		runner->init(kAbsentLogCfg, true, genDir.c_str());
		return runner;
	}

	std::string minimal_r6_cta_config(const std::string& dir)
	{
		return "{\"basefiles\":{},\"env\":{\"name\":\"cta\"},\"data\":{},"
			"\"bspolicy\":\"" + dir + "/actions.json\","
			"\"parsers\":[{\"active\":false,\"id\":\"r6_p_off\"}],"
			"\"traders\":[{\"active\":false,\"id\":\"r6_t_off\"}],"
			"\"executers\":[{\"active\":false,\"id\":\"r6_e_off\"}],"
			"\"strategies\":{\"cta\":[{\"active\":false,\"id\":\"r6_s_off\","
			"\"name\":\"r6_n_off\"}]}}";
	}

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

TEST(R6PorterFactories, cta_empty_directory_is_accepted)
{
	EmptyFactoryDir dir;
	ASSERT_TRUE(dir.ok());
	const std::string path = dir.path().string();
	WtRtRunner r;
	EXPECT_TRUE(r.addCtaFactories(path.c_str()));
}

TEST(R6PorterFactories, sel_empty_directory_is_accepted)
{
	EmptyFactoryDir dir;
	ASSERT_TRUE(dir.ok());
	const std::string path = dir.path().string();
	WtRtRunner r;
	EXPECT_TRUE(r.addSelFactories(path.c_str()));
}

TEST(R6PorterFactories, hft_empty_directory_is_accepted)
{
	EmptyFactoryDir dir;
	ASSERT_TRUE(dir.ok());
	const std::string path = dir.path().string();
	WtRtRunner r;
	EXPECT_TRUE(r.addHftFactories(path.c_str()));
}

TEST(R6PorterFactories, cta_regular_file_is_rejected)
{
	WtRtRunner r;
	bool loaded = true;
	EXPECT_NO_THROW(loaded = r.addCtaFactories(__FILE__));
	EXPECT_FALSE(loaded);
}

TEST(R6PorterFactories, sel_regular_file_is_rejected)
{
	WtRtRunner r;
	bool loaded = true;
	EXPECT_NO_THROW(loaded = r.addSelFactories(__FILE__));
	EXPECT_FALSE(loaded);
}

TEST(R6PorterFactories, hft_regular_file_is_rejected)
{
	WtRtRunner r;
	bool loaded = true;
	EXPECT_NO_THROW(loaded = r.addHftFactories(__FILE__));
	EXPECT_FALSE(loaded);
}

TEST(R6PorterFactories, cta_unreadable_directory_enumeration_failure_returns_false)
{
	EmptyFactoryDir dir;
	ASSERT_TRUE(dir.ok());
	std::error_code ec;
	std_fs::permissions(dir.path(), std_fs::perms::none, std_fs::perm_options::replace, ec);
	ASSERT_FALSE(ec) << ec.message();

	std_fs::directory_iterator probe(dir.path(), ec);
	if (!ec)
		GTEST_SKIP() << "The current user can enumerate a directory with no permission bits";
	const std::string path = dir.path().string();

	WtRtRunner r;
	bool loaded = true;
	EXPECT_NO_THROW(loaded = r.addCtaFactories(path.c_str()));
	EXPECT_FALSE(loaded);
}

TEST(R6PorterFactories, sel_unreadable_directory_enumeration_failure_returns_false)
{
	EmptyFactoryDir dir;
	ASSERT_TRUE(dir.ok());
	std::error_code ec;
	std_fs::permissions(dir.path(), std_fs::perms::none, std_fs::perm_options::replace, ec);
	ASSERT_FALSE(ec) << ec.message();

	std_fs::directory_iterator probe(dir.path(), ec);
	if (!ec)
		GTEST_SKIP() << "The current user can enumerate a directory with no permission bits";
	const std::string path = dir.path().string();

	WtRtRunner r;
	bool loaded = true;
	EXPECT_NO_THROW(loaded = r.addSelFactories(path.c_str()));
	EXPECT_FALSE(loaded);
}

TEST(R6PorterFactories, hft_unreadable_directory_enumeration_failure_returns_false)
{
	EmptyFactoryDir dir;
	ASSERT_TRUE(dir.ok());
	std::error_code ec;
	std_fs::permissions(dir.path(), std_fs::perms::none, std_fs::perm_options::replace, ec);
	ASSERT_FALSE(ec) << ec.message();

	std_fs::directory_iterator probe(dir.path(), ec);
	if (!ec)
		GTEST_SKIP() << "The current user can enumerate a directory with no permission bits";
	const std::string path = dir.path().string();

	WtRtRunner r;
	bool loaded = true;
	EXPECT_NO_THROW(loaded = r.addHftFactories(path.c_str()));
	EXPECT_FALSE(loaded);
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
 * DISABLED_ 组：尚未转为安全回归测试的崩溃 / 挂死 / 进程级副作用
 *
 * 触发条件：
 *  C1 config() 的空配置和必需 basefiles 检查已有启用用例，失败后 run() 安全拒绝。
 *  C2 createExtParser() 在未配置时已有 false 返回守卫；由本地与公开 C API 用例覆盖。
 *  C3 get_raw_stdcode() 未 config 时解引用空 _engine（BUG-Q06-002 同族）——
 *     I03-NEW-128 已修为返回空串并记录诊断，改为启用的合同用例
 *     R6PorterRawCode.get_raw_stdcode_without_engine_fails_closed。
 *  C4 feedAdjFactors() 不判空即调 _feeder_fcts（BUG-Q06-004/R46-001）——
 *     I03-NEW-129 已修为未注册时告警并返回，改为启用的合同用例
 *     R6PorterFeed.feed_adj_factors_without_feeder_fails_closed。
 *  C5 run()（1091~1124）：_engine 为空段错误；非空则起引擎线程并在
 *     `while(!_to_exit) sleep(10ms)` 上挂死。
 *  C6 release()（1141~1155）：`WTSLogger::stop()` 会把进程级 m_bStopped 置位并
 *     spdlog::shutdown()，同二进制里后续用例的日志行为被永久改变 ->
 *     只能在「每用例一进程」的采样口径下跑，本轮按约束留给主控裁定。
 * ---------------------------------------------------------------------- */

TEST(R6PorterConfig, missing_config_file_returns_false_and_run_is_rejected)
{
	WtRtRunner r;
	bool configured = true;
	EXPECT_NO_THROW(configured = r.config(kAbsentCfg, true));
	EXPECT_FALSE(configured);
	EXPECT_NO_THROW(r.run(false));

	R6ConfigTempDir dir;
	ASSERT_TRUE(dir.ok());
	const std_fs::path emptyConfig = dir.path() / "empty.json";
	ASSERT_TRUE(write_r6_file(emptyConfig, ""));
	configured = true;
	EXPECT_NO_THROW(configured = r.config(emptyConfig.string().c_str(), true));
	EXPECT_FALSE(configured);
	EXPECT_NO_THROW(r.run(false));
}

TEST(R6PorterConfig, invalid_basefiles_is_rejected_and_configuration_can_be_retried)
{
	R6ConfigTempDir dir;
	ASSERT_TRUE(dir.ok());
	ASSERT_TRUE(write_r6_file(dir.path() / "actions.json", "{\"default\":{}}"));

	// Keep this runner for process lifetime: successful configuration loads global
	// managers, and this test target intentionally avoids global logger teardown.
	WtRtRunner* runner = new_r6_runner(dir.path().string());
	ASSERT_NE(runner, nullptr);

	EXPECT_FALSE(runner->config("{}", false));
	EXPECT_NO_THROW(runner->run(false));

	EXPECT_FALSE(runner->config("{\"basefiles\":[]}", false));
	EXPECT_NO_THROW(runner->run(false));
	EXPECT_FALSE(runner->config("{\"basefiles\":\"not-an-object\"}", false));
	EXPECT_NO_THROW(runner->run(false));

	const std::string validConfig = minimal_r6_cta_config(dir.path().string());
	ASSERT_TRUE(runner->config(validConfig.c_str(), false))
		<< "failed basefiles validation must release the parsed config and leave retry available";
	EXPECT_NE(runner->getEngine(), nullptr);
}

TEST(R6PorterExtParser, create_ext_parser_without_engine_returns_false)
{
	WtRtRunner r;
	EXPECT_FALSE(r.createExtParser("r6parser"));
}

/*
 * I03-NEW-128（卡 4a）：未 config 时没有引擎，get_raw_stdcode 返回空串（失败值）
 * 并记录诊断；改前解引用空 _engine 崩溃（WtRtRunner.cpp get_raw_stdcode）。
 * 原 DISABLED_ 用例断言的 "IF2108" 是有引擎时的成功路径，不属于本合同。
 */
TEST(R6PorterRawCode, get_raw_stdcode_without_engine_fails_closed)
{
	WtRtRunner r;
	ASSERT_EQ(nullptr, r.getEngine());

	const char* raw = r.get_raw_stdcode("CFFEX.IF.2108");	// C3
	ASSERT_NE(nullptr, raw);
	EXPECT_STREQ("", raw);

	raw = r.get_raw_stdcode(NULL);
	ASSERT_NE(nullptr, raw);
	EXPECT_STREQ("", raw);
	EXPECT_EQ(nullptr, r.getEngine());
}

namespace {
	struct R6FactorProbe
	{
		int			calls = 0;
		void*		obj = nullptr;
		std::string	code;
		uint32_t	count = 0;
		uint32_t	dates[2] = { 0, 0 };
		double		factors[2] = { 0, 0 };
	};
	R6FactorProbe g_r6_factor_probe;
	WtRtRunner* g_r6_feed_runner = nullptr;

	void r6_read_factors(void* obj, const char* stdCode, uint32_t* dates, double* factors, uint32_t count)
	{
		g_r6_factor_probe.calls++;
		g_r6_factor_probe.obj = obj;
		g_r6_factor_probe.code = stdCode == NULL ? "<null>" : stdCode;
		g_r6_factor_probe.count = count;
		for (uint32_t i = 0; i < count && i < 2; i++)
		{
			g_r6_factor_probe.dates[i] = dates[i];
			g_r6_factor_probe.factors[i] = factors[i];
		}
	}

	// The host side of an extended factor loader: it answers a load request
	// by feeding the factors back through the runner, as wtpy does.
	bool PORTER_FLAG r6_load_adj_factors(const char* stdCode)
	{
		uint32_t dates[2] = { 20210104, 20210601 };
		double factors[2] = { 1.0, 1.25 };
		g_r6_feed_runner->feedAdjFactors(stdCode, dates, factors, 2);
		return true;
	}
}

/*
 * I03-NEW-129（BUG-Q06-004/R46-001）：未注册扩展复权因子加载器、或尚未通过
 * loadAdjFactors 装入 feeder 时，feedAdjFactors 告警并返回，不调用空函数指针；
 * 改前直接调用 _feeder_fcts（NULL）崩溃。最后一段为正向对照：装入 feeder 后
 * 加载器回灌的因子原样转交。
 */
TEST(R6PorterFeed, feed_adj_factors_without_feeder_fails_closed)
{
	g_r6_factor_probe = R6FactorProbe();
	WtRtRunner r;
	uint32_t dates[1] = { 20210104 };
	double factors[1] = { 1.0 };

	// No loader registered, no feeder.
	r.feedAdjFactors("SSE.STK.600000", dates, factors, 1);	// C4
	// A loader is registered, but no load request has installed a feeder yet.
	g_r6_feed_runner = &r;
	r.registerExtDataLoader(NULL, NULL, r6_load_adj_factors);
	r.feedAdjFactors("SSE.STK.600000", dates, factors, 1);
	EXPECT_EQ(0, g_r6_factor_probe.calls);

	// Positive control: a load request installs the feeder, and the factors the
	// loader feeds back are forwarded exactly.
	int owner = 0;
	ASSERT_TRUE(r.loadAdjFactors(&owner, "SSE.STK.600000", r6_read_factors));
	g_r6_feed_runner = nullptr;
	EXPECT_EQ(1, g_r6_factor_probe.calls);
	EXPECT_EQ(&owner, g_r6_factor_probe.obj);
	EXPECT_EQ("SSE.STK.600000", g_r6_factor_probe.code);
	EXPECT_EQ(2u, g_r6_factor_probe.count);
	EXPECT_EQ(20210104u, g_r6_factor_probe.dates[0]);
	EXPECT_EQ(20210601u, g_r6_factor_probe.dates[1]);
	EXPECT_DOUBLE_EQ(1.0, g_r6_factor_probe.factors[0]);
	EXPECT_DOUBLE_EQ(1.25, g_r6_factor_probe.factors[1]);
}

TEST(R6PorterRunRelease, DISABLED_run_without_engine_and_release_global_logger_stop)
{
	WtRtRunner r;
	r.run(true);		// C5
	r.release();		// C6
}
