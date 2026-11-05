/*
 * R82 - WtPorter / WtRtRunner 剩余行段补测
 *
 * 目标：
 *   src/WtPorter/WtPorter.cpp（missed 280/848）
 *     - config_porter 转发（L260-266）
 *     - CTA/SEL/HFT 真实上下文下的下单、订阅、切片取值与空切片分支
 *       （L420-487、L501-568、L673-687、L743-750、L872-875、L985-992、
 *        L1249-1263、L1275-1282、L1295-1322、L1333-1341、L1354-1381、
 *        L1391-1398、L1409-1412、L1668-1675、L1725-1732、L1797-1866、
 *        L1881-1905、L1920-1944、L1959-1983、L2027-2087、L2100-2211）
 *     - release_porter 导出（L286-289）
 *   src/WtPorter/WtRtRunner.cpp（missed 276/938）
 *     - 扩展 K 线加载器 KP_Minute1 与未知周期（L224、L241-242、L249）
 *     - ctx_on_session_event 的 EngineType default（L436-437）
 *     - basefiles 主力回填的 continue（L572）
 *     - config 各段失败臂：parser 文件缺失、traders/executers 文件坏 JSON 与
 *       非数组、data 段缺失（L615-618、L640-646、
 *       L678-689、L724、L888、L936）
 *     - CTA/HFT 缺 name、缺工厂/未知策略拒绝；CTA/HFT 缺列表仍可配置
 *     - initEngine 缺 env 段（L842）
 *     - initExecuters 空 name->local、缺 trader、diff/arbi 未知 trader（L952-964、
 *       L1000-1003、L1027-1030）
 *     - handleLogAppend（L1137-1138）
 *     - release（L1141-1155）
 *     - on_ext_parser_quote 命中支（L1247-1251）
 *
 * 边界：只用公开 API；无 private 宏、无类布局镜像；不启动行情/交易线程，
 *       不调 run()。真实上下文一律由进程单例 create_*_context 建立，
 *       既有 runner 边界用堆实例规避 BUG-R46-001；R6 quote 诊断用例在独立
 *       子进程内创建局部 runner 并显式 release，隔离全局日志和 C API 状态。
 *       临时文件只写系统临时目录并按 pid+tag 隔离，每个用例清理。
 *       release 用例固定写在文件最后（WTSLogger::stop 是进程级静默，见 BUG-R6-006）。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <new>
#include <string>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "WtCore/WtHelper.h"
#include "WtCore/HftStraContext.h"
#include "WtPorter/PorterDefs.h"
#include "WtPorter/WtPorter.h"
#include "WtPorter/WtRtRunner.h"
#include "../trader_mocker_fixture.h"

USING_NS_WTP;

#ifndef _WIN32
extern char** environ;
#endif

/* WtPorter.cpp:34-38 的进程单例 */
extern WtRtRunner& getRunner();

namespace {

	std::string prepare_dir(const char* tag)
	{
		std::filesystem::path p = std::filesystem::temp_directory_path()
			/ (std::string("r82_porter_") + tag + "_" + std::to_string(::getpid()));
		std::error_code ec;
		std::filesystem::remove_all(p, ec);
		std::filesystem::create_directories(p, ec);
		return p.string();
	}

	void clean_dir(const std::string& dir)
	{
		std::error_code ec;
		std::filesystem::remove_all(dir, ec);
		std::filesystem::remove_all("mocker_82001", ec);
	}

	void write_text(const std::string& path, const std::string& body)
	{
		std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
		ofs << body;
	}

	std::string test_binary_dir()
	{
		std::filesystem::path executable;
#ifdef __APPLE__
		uint32_t size = 0;
		_NSGetExecutablePath(NULL, &size);
		std::vector<char> buffer(size + 1, 0);
		if (_NSGetExecutablePath(buffer.data(), &size) == 0)
			executable = std::filesystem::weakly_canonical(buffer.data());
#else
		char buffer[4096] = { 0 };
		const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
		if (length > 0)
			executable = std::filesystem::path(std::string(buffer, (std::size_t)length));
#endif
		std::filesystem::path dir = executable.parent_path();
		if (dir.filename() == "TestUnits")
			dir = dir.parent_path();
		return dir.string();
	}

	WtRtRunner* new_heap_runner(const std::string& dir)
	{
		/* 堆上构造、永不 delete：见 BUG-R46-001（构造列表漏初始化 _config/_engine）。 */
		WtRtRunner* r = new WtRtRunner();
		r->init((dir + "/r82_absent_logcfg.yaml").c_str(), true, dir.c_str());
		return r;
	}

	const char* kSessions =
		"{\"TRADING\":{\"name\":\"Trading\",\"offset\":0,"
		"\"sections\":[{\"from\":900,\"to\":1500}]}}";

	const char* kCommodities =
		"{\"SHFE\":{\"au\":{\"name\":\"Gold\",\"session\":\"TRADING\","
		"\"pricetick\":0.02,\"volscale\":1000,\"covermode\":0,\"pricemode\":0,"
		"\"trademode\":0,\"category\":1}}}";

	const char* kContracts =
		"{\"SHFE\":{\"au2406\":{\"name\":\"au2406\",\"product\":\"au\",\"exchg\":\"SHFE\"}}}";

	const char* kHotRules =
		"{\"SHFE\":{\"au\":[{\"from\":\"au2406\",\"to\":\"au2406\",\"date\":20240101,"
		"\"oldclose\":100.0,\"newclose\":100.0}]}}";

	const char* kActions = "{\"default\":{}}";

	/* 真实上下文用的完整配置：session/commodity/contract/自定义主力规则 + 指定 env；
	 * HFT 分支用本地 TraderMocker 正常初始化交易通道。 */
	std::string real_cfg(const std::string& dir, const char* envName)
	{
		std::string cfg =
			"{\"basefiles\":{"
			"\"session\":\"" + dir + "/sessions.json\","
			"\"commodity\":\"" + dir + "/commodities.json\","
			"\"contract\":\"" + dir + "/contracts.json\","
			"\"rules\":{\"R82\":\"" + dir + "/hot.json\"}},"
			"\"env\":{\"name\":\"" + envName + "\"},"
			"\"data\":{},"
			"\"bspolicy\":\"" + dir + "/actions.json\"";
		if (std::string(envName) == "hft")
			cfg += ",\"traders\":[{\"active\":true,\"id\":\"r82_trd\","
				"\"module\":\"TraderMocker\",\"mockerid\":82001,\"udp_port\":0}]";
		cfg += "}";
		return cfg;
	}

	/* 每族引擎在新进程里只初始化一次；C API 用例各自可先于其它用例执行。
	 * 注意 WtRtRunner::config 只 init 当前 env 对应的引擎，所以 SEL/HFT 用例
	 * 必须先用对应 env 再配置一次，否则各自引擎的 _base_data_mgr/_hot_mgr 为空。 */
	bool g_cta_configured = false;
	bool g_sel_configured = false;
	bool g_hft_configured = false;
	std::string g_current_env;

	/* 卡9c（迭代03）：runner 配置一次性，重复 config 被拒；切换 env 时以一个全新的
	 * 进程级 runner 对象替换（与 v2 夹具相同的析构+原位重建），等价于每引擎一进程。 */
	void fresh_porter_runner()
	{
		WtRtRunner& runner = getRunner();
		runner.WtRtRunner::~WtRtRunner();
		new (&runner) WtRtRunner();
		g_cta_configured = false;
		g_sel_configured = false;
		g_hft_configured = false;
		g_current_env.clear();
	}

	void configure_porter(const std::string& dir, const char* envName, bool& flag)
	{
		fresh_porter_runner();
		getRunner().init((dir + "/r82_absent_logcfg.yaml").c_str(), true, dir.c_str());
		/* WtPorter.cpp:260-266 的 isFile=false 转发支 */
		config_porter(real_cfg(dir, envName).c_str(), false);
		g_current_env = envName;
		flag = true;
	}

	/* engine 是「当前 env 那一个」的指针；切换 env 必须重新 config，
	 * 否则 create_*_context 会拿到未 init 的兄弟引擎。 */
	bool need_config(bool flag, const char* envName)
	{
		return !flag || g_current_env != envName || getRunner().getEngine() == nullptr;
	}

	void ensure_cta_configured(const std::string& dir)
	{
		if (need_config(g_cta_configured, "cta"))
			configure_porter(dir, "cta", g_cta_configured);
	}

	void ensure_sel_configured(const std::string& dir)
	{
		if (need_config(g_sel_configured, "sel"))
			configure_porter(dir, "sel", g_sel_configured);
	}

	bool ensure_hft_configured(const std::string& dir,
		wtp_test::TraderMockerFixture& trader_mocker)
	{
		if (!need_config(g_hft_configured, "hft"))
			return true;

		fresh_porter_runner();
		getRunner().init((dir + "/r82_absent_logcfg.yaml").c_str(), true, dir.c_str());
		if (!trader_mocker.install())
			return false;

		/* WtPorter.cpp:260-266 的 isFile=false 转发支 */
		config_porter(real_cfg(dir, "hft").c_str(), false);
		g_current_env = "hft";
		g_hft_configured = true;
		return true;
	}

	void write_real_fixtures(const std::string& dir)
	{
		write_text(dir + "/sessions.json", kSessions);
		write_text(dir + "/commodities.json", kCommodities);
		write_text(dir + "/contracts.json", kContracts);
		write_text(dir + "/hot.json", kHotRules);
		write_text(dir + "/actions.json", kActions);
	}

	const char* kMonthCode = "SHFE.au.2406";

	struct BarsRec
	{
		uint32_t	calls = 0;
		void clear() { *this = BarsRec(); }
	};

	BarsRec g_bars;
	BarsRec g_ticks;

	struct PosRec
	{
		uint32_t	calls = 0;
		std::vector<std::pair<std::string, double>> positions;
		std::string	last_code;
		double		last_qty = -1;
		bool		last_islast = false;
		void clear() { *this = PosRec(); }
	};

	PosRec g_pos;

	void bars_cb(CtxHandler /*cHandle*/, const char* /*stdCode*/, const char* /*period*/,
		WTSBarStruct* /*bar*/, WtUInt32 /*count*/, bool /*isLast*/)
	{
		g_bars.calls++;
	}

	void ticks_cb(CtxHandler /*cHandle*/, const char* /*stdCode*/, WTSTickStruct* /*tick*/,
		WtUInt32 /*count*/, bool /*isLast*/)
	{
		g_ticks.calls++;
	}

	void pos_cb(CtxHandler /*cHandle*/, const char* stdCode, double position, bool isLast)
	{
		g_pos.calls++;
		g_pos.last_code = (stdCode == NULL) ? "<null>" : stdCode;
		g_pos.last_qty = position;
		g_pos.last_islast = isLast;
		if (!isLast)
			g_pos.positions.emplace_back(g_pos.last_code, position);
	}

	/* 扩展 K 线加载器的取样记录 */
	struct LoadRec
	{
		uint32_t	calls = 0;
		std::string	code;
		std::string	period;
		void clear() { *this = LoadRec(); }
	};

	LoadRec g_fnl;
	LoadRec g_raw;
	LoadRec g_fct;

	bool fnl_loader(const char* stdCode, const char* period)
	{
		g_fnl.calls++;
		g_fnl.code = (stdCode == NULL) ? "<null>" : stdCode;
		g_fnl.period = (period == NULL) ? "<null>" : period;
		return true;
	}

	bool raw_loader(const char* stdCode, const char* period)
	{
		g_raw.calls++;
		g_raw.code = (stdCode == NULL) ? "<null>" : stdCode;
		g_raw.period = (period == NULL) ? "<null>" : period;
		return true;
	}

	bool fct_loader(const char* stdCode)
	{
		g_fct.calls++;
		g_fct.code = (stdCode == NULL) ? "<null>" : stdCode;
		return true;
	}

	void bar_sink(void* /*obj*/, WTSBarStruct* /*bars*/, uint32_t /*count*/) {}
	void fct_sink(void* /*obj*/, const char* /*code*/, uint32_t* /*dates*/,
		double* /*factors*/, uint32_t /*count*/) {}

	class QuoteProbeRunner : public WtRtRunner
	{
	public:
		void handle_push_quote(WTSTickData* tick) override
		{
			++quote_calls;
			last_code = tick == NULL ? "<null>" : tick->code();
			last_price = tick == NULL ? 0.0 : tick->price();
		}

		uint32_t quote_calls = 0;
		std::string last_code;
		double last_price = 0.0;
	};

	template <typename Fn>
	std::string capture_log_output(Fn&& fn)
	{
		testing::internal::CaptureStdout();
		testing::internal::CaptureStderr();
		fn();
		std::string output = testing::internal::GetCapturedStdout();
		output += testing::internal::GetCapturedStderr();
		return output;
	}

#ifndef _WIN32
	struct ChildTestResult
	{
		bool spawned = false;
		bool timed_out = false;
		bool reaped = false;
		int status = 0;
		pid_t pid = -1;
		std::string output;
	};

	ChildTestResult run_isolated_gtest(const char* filter, const char* marker,
		const std::string& outputPath)
	{
		ChildTestResult result;
		const std::vector<std::string> gtestArgs = testing::internal::GetArgvs();
		if (gtestArgs.empty())
			return result;

		std::vector<std::string> args = {
			gtestArgs.front(), std::string("--gtest_filter=") + filter, "--gtest_color=no"
		};
		std::vector<char*> argv;
		argv.reserve(args.size() + 1);
		for (std::string& arg : args)
			argv.push_back(const_cast<char*>(arg.c_str()));
		argv.push_back(NULL);

		std::vector<std::string> envStorage;
		for (char** item = environ; item != NULL && *item != NULL; ++item)
		{
			const std::string entry(*item);
			if (entry.compare(0, std::strlen(marker), marker) != 0 ||
				entry.size() <= std::strlen(marker) || entry[std::strlen(marker)] != '=')
				envStorage.push_back(entry);
		}
		envStorage.emplace_back(std::string(marker) + "=1");
		std::vector<char*> envp;
		envp.reserve(envStorage.size() + 1);
		for (std::string& entry : envStorage)
			envp.push_back(const_cast<char*>(entry.c_str()));
		envp.push_back(NULL);

		const int outputFd = ::open(outputPath.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
		if (outputFd < 0)
			return result;

		posix_spawn_file_actions_t actions;
		if (posix_spawn_file_actions_init(&actions) != 0)
		{
			::close(outputFd);
			return result;
		}
		int actionResult = posix_spawn_file_actions_adddup2(&actions, outputFd, STDOUT_FILENO);
		if (actionResult == 0)
			actionResult = posix_spawn_file_actions_adddup2(&actions, outputFd, STDERR_FILENO);
		if (actionResult == 0)
			actionResult = posix_spawn_file_actions_addclose(&actions, outputFd);

		pid_t child = -1;
		const int spawnResult = actionResult == 0
			? posix_spawn(&child, args.front().c_str(), &actions, NULL, argv.data(), envp.data())
			: actionResult;
		posix_spawn_file_actions_destroy(&actions);
		::close(outputFd);
		if (spawnResult == 0)
		{
			result.spawned = true;
			result.pid = child;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
			pid_t waited = 0;
			while (std::chrono::steady_clock::now() < deadline)
			{
				waited = ::waitpid(child, &result.status, WNOHANG);
				if (waited == child)
				{
					result.reaped = true;
					break;
				}
				if (waited < 0)
					break;
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			if (waited == 0)
			{
				result.timed_out = true;
				::kill(child, SIGKILL);
				result.reaped = ::waitpid(child, &result.status, 0) == child;
			}
		}

		std::ifstream output(outputPath, std::ios::binary);
		result.output.assign(std::istreambuf_iterator<char>(output), std::istreambuf_iterator<char>());
		return result;
	}
#endif

} // namespace

/* ---------------------------------------------------------------------------
 * WtPorter.cpp:420-487 + 501-568 + 673-687 + 743-750 + 872-875 + 985-992
 * 真实 CTA 上下文：四个开平入口、目标仓位、tick 订阅、K线/tick 空切片、
 * 日价格与持仓枚举的终止符。
 * ------------------------------------------------------------------------- */
TEST(R82PorterCapiReal, CtaEntriesAndSliceMisses)
{
	const std::string dir = prepare_dir("cta");
	write_real_fixtures(dir);
	ensure_cta_configured(dir);
	ASSERT_NE(getRunner().getEngine(), nullptr);

	const CtxHandler h = create_cta_context("r82_cta_real", 0);
	ASSERT_NE(h, 0U);
	ASSERT_TRUE(getRunner().getCtaContext(h) != NULL);

	/* Enter/exit 只改动本地信号表；stra_get_position 优先读信号量，可精确断言。 */
	cta_enter_long(h, kMonthCode, 1, "r82", 0, 0);
	EXPECT_DOUBLE_EQ(cta_get_position(h, kMonthCode, false, ""), 1.0);

	cta_exit_long(h, kMonthCode, 1, "r82", 0, 0);
	EXPECT_DOUBLE_EQ(cta_get_position(h, kMonthCode, false, ""), 0.0);

	cta_enter_short(h, kMonthCode, 1, "r82", 0, 0);
	EXPECT_DOUBLE_EQ(cta_get_position(h, kMonthCode, false, ""), -1.0);

	cta_exit_short(h, kMonthCode, 1, "r82", 0, 0);
	EXPECT_DOUBLE_EQ(cta_get_position(h, kMonthCode, false, ""), 0.0);

	cta_set_position(h, kMonthCode, 2, "r82", 0, 0);
	EXPECT_DOUBLE_EQ(cta_get_position(h, kMonthCode, false, ""), 2.0);
	cta_set_position(h, kMonthCode, 0, "r82", 0, 0);

	/* L985-992：订阅只写本地集合与引擎订阅表，不启动线程。 */
	EXPECT_NO_THROW(cta_sub_ticks(h, kMonthCode));

	/* L501-531 / L544-568 的 kData/tData == NULL 支：data 段为空 -> reader 为空。 */
	g_bars.clear();
	g_ticks.clear();
	EXPECT_EQ(cta_get_bars(h, kMonthCode, "m1", 5, false, bars_cb), 0U);
	EXPECT_EQ(cta_get_ticks(h, kMonthCode, 5, ticks_cb), 0U);
	EXPECT_EQ(g_bars.calls, 0U);
	EXPECT_EQ(g_ticks.calls, 0U);

	/* L872-875：data mgr 的实时 tick 缓存为空 -> 0。 */
	EXPECT_DOUBLE_EQ(cta_get_day_price(kMonthCode, 0), 0.0);

	/* L673-687：零目标持仓仍被枚举，并在末尾发送终止符。 */
	g_pos.clear();
	cta_get_all_position(h, pos_cb);
	ASSERT_EQ(g_pos.calls, 2U);
	ASSERT_EQ(g_pos.positions.size(), 1U);
	EXPECT_EQ(g_pos.positions.front().first, kMonthCode);
	EXPECT_DOUBLE_EQ(g_pos.positions.front().second, 0.0);
	EXPECT_TRUE(g_pos.last_islast);
	EXPECT_EQ(g_pos.last_code, "");

	clean_dir(dir);
}

/* ---------------------------------------------------------------------------
 * WtPorter.cpp:1249-1263 + 1275-1282 + 1295-1322 + 1333-1341 + 1354-1381
 * + 1391-1398 + 1409-1412：真实 SEL 上下文的置仓、订阅、K线/tick 空切片、
 * 空持仓枚举。
 * ------------------------------------------------------------------------- */
TEST(R82PorterCapiReal, SelEntriesAndSliceMisses)
{
	const std::string dir = prepare_dir("sel");
	write_real_fixtures(dir);
	ensure_sel_configured(dir);
	ASSERT_NE(getRunner().getEngine(), nullptr);

	const CtxHandler h = create_sel_context("r82_sel_real", 20240101, 93000, "d",
		"CHINA", "TRADING", 0);
	ASSERT_NE(h, 0U);
	ASSERT_TRUE(getRunner().getSelContext(h) != NULL);

	/* set_position 只追加本地信号，实际持仓读回仍是 0。 */
	sel_set_position(h, kMonthCode, 3, "r82");
	EXPECT_DOUBLE_EQ(sel_get_position(h, kMonthCode, false, ""), 0.0);
	sel_set_position(h, kMonthCode, 0, "r82");

	EXPECT_NO_THROW(sel_sub_ticks(h, kMonthCode));

	g_bars.clear();
	g_ticks.clear();
	EXPECT_EQ(sel_get_bars(h, kMonthCode, "m1", 5, bars_cb), 0U);
	EXPECT_EQ(sel_get_ticks(h, kMonthCode, 5, ticks_cb), 0U);
	EXPECT_EQ(g_bars.calls, 0U);
	EXPECT_EQ(g_ticks.calls, 0U);

	EXPECT_DOUBLE_EQ(sel_get_day_price(kMonthCode, 3), 0.0);

	/* 这里仍有 qty=3 的信号记录：设置为实际持仓 0 是 no-op。 */
	g_pos.clear();
	sel_get_all_position(h, pos_cb);
	ASSERT_EQ(g_pos.calls, 2U);
	ASSERT_EQ(g_pos.positions.size(), 1U);
	EXPECT_EQ(g_pos.positions.front().first, kMonthCode);
	EXPECT_DOUBLE_EQ(g_pos.positions.front().second, 3.0);
	EXPECT_TRUE(g_pos.last_islast);
	EXPECT_EQ(g_pos.last_code, "");

	/* 独立新上下文没有持仓或信号，空表只发一个终止回调。 */
	const CtxHandler empty = create_sel_context("r82_sel_empty", 20240101, 93000, "d",
		"CHINA", "TRADING", 0);
	ASSERT_NE(empty, 0U);
	ASSERT_TRUE(getRunner().getSelContext(empty) != NULL);
	g_pos.clear();
	sel_get_all_position(empty, pos_cb);
	ASSERT_EQ(g_pos.calls, 1U);
	EXPECT_TRUE(g_pos.last_islast);
	EXPECT_EQ(g_pos.last_code, "");

	clean_dir(dir);
}

/* ---------------------------------------------------------------------------
 * WtPorter.cpp:1668-1675 + 1725-1732 + 1797-1866 + 1881-1983 + 2027-2087
 * + 2100-2211：绑定真实 TraderAdapter 的 HFT 上下文。qty=0 让 TraderAdapter
 * 在写 _stat_map 之前返回，从而覆盖 buy/sell 全段而不触碰未初始化的风控表。
 * ------------------------------------------------------------------------- */
TEST(R82PorterCapiReal, HftEntriesAndMicroSliceMisses)
{
	const std::string dir = prepare_dir("hft");
	write_real_fixtures(dir);
	wtp_test::TraderMockerFixture trader_mocker("r82");
	if (!ensure_hft_configured(dir, trader_mocker))
	{
		clean_dir(dir);
		FAIL() << "Could not prepare TraderMocker from product output at "
			<< trader_mocker.product_plugin() << ": " << trader_mocker.error();
		return;
	}
	ASSERT_NE(getRunner().getEngine(), nullptr);

	/* r82_trd 使用成功初始化的本地模拟 API；本测试不启动连接/交易线程。 */
	const CtxHandler h = create_hft_context("r82_hft_real", "r82_trd", false, 0);
	ASSERT_NE(h, 0U);
	ASSERT_TRUE(getRunner().getHftContext(h) != NULL);

	/* 持仓/未完成量都走 TraderAdapter 的 map，空表 -> 0。 */
	EXPECT_DOUBLE_EQ(hft_get_position(h, kMonthCode, true), 0.0);
	EXPECT_DOUBLE_EQ(hft_get_position_profit(h, kMonthCode), 0.0);
	EXPECT_DOUBLE_EQ(hft_get_position_avgpx(h, kMonthCode), 0.0);
	EXPECT_DOUBLE_EQ(hft_get_undone(h, kMonthCode), 0.0);

	/* L2100-2140：空订单表 -> false / 空串。 */
	EXPECT_FALSE(hft_cancel(h, 1));
	EXPECT_STREQ(hft_cancel_all(h, kMonthCode, true), "");

	/* L2157-2211：qty=0 时 TraderAdapter::buy/sell 首行即返回空 OrderIDs。 */
	EXPECT_STREQ(hft_buy(h, kMonthCode, 100.0, 0, "r82", 0), "");
	EXPECT_STREQ(hft_sell(h, kMonthCode, 100.0, 0, "r82", 0), "");

	EXPECT_NO_THROW(hft_sub_ticks(h, kMonthCode));
	EXPECT_NO_THROW(hft_sub_order_queue(h, kMonthCode));
	EXPECT_NO_THROW(hft_sub_order_detail(h, kMonthCode));
	EXPECT_NO_THROW(hft_sub_transaction(h, kMonthCode));

	/* L1797-1983：五种切片的 NULL 支（reader 为空）。 */
	g_bars.clear();
	g_ticks.clear();
	EXPECT_EQ(hft_get_bars(h, kMonthCode, "m1", 5, bars_cb), 0U);
	EXPECT_EQ(hft_get_ticks(h, kMonthCode, 5, ticks_cb), 0U);
	EXPECT_EQ(hft_get_ordque(h, kMonthCode, 5, NULL), 0U);
	EXPECT_EQ(hft_get_orddtl(h, kMonthCode, 5, NULL), 0U);
	EXPECT_EQ(hft_get_trans(h, kMonthCode, 5, NULL), 0U);
	EXPECT_EQ(g_bars.calls, 0U);
	EXPECT_EQ(g_ticks.calls, 0U);

	clean_dir(dir);
}

/* ---------------------------------------------------------------------------
 * WtRtRunner.cpp:1247-1251：先 createExtParser（需要已配置 engine），再推一条
 * 注册合约的合法 tick，走 adapter->handleQuote -> engine->handle_push_quote。
 * ------------------------------------------------------------------------- */
TEST(R82PorterCapiReal, ExtParserQuoteSuccess)
{
	const std::string dir = prepare_dir("quote");
	write_real_fixtures(dir);
	ensure_cta_configured(dir);
	ASSERT_NE(getRunner().getEngine(), nullptr);

	EXPECT_TRUE(create_ext_parser("r82_parser"));

	WTSTickStruct tick;
	memset(&tick, 0, sizeof(tick));
	strncpy(tick.code, "au2406", sizeof(tick.code) - 1);
	strncpy(tick.exchg, "SHFE", sizeof(tick.exchg) - 1);
	tick.trading_date = 20240101;
	tick.action_date = 20240101;
	tick.action_time = 93000;
	tick.price = 100.0;
	tick.open = 100.0;
	tick.high = 100.0;
	tick.low = 100.0;

	/* ParserAdapter 要求 actiondate/tradingdate 非 0，且能查到合约。 */
	EXPECT_NO_THROW(parser_push_quote("r82_parser", &tick, 0));

	/* 未注册 id 的告警支（既有 lane 已覆盖，这里只做对照）。 */
	EXPECT_NO_THROW(parser_push_quote("r82_parser_absent", &tick, 0));

	clean_dir(dir);
}

/*
 * BUG-R6-004: NULL id/tick are rejected with distinct diagnostics at both the
 * runner and C API boundaries. A registered parser with a valid tick reaches
 * the parser stub exactly once; no ticker thread is needed for this dispatch
 * count because the probe overrides the runner's IParserStub callback.
 */
TEST(R6PorterExtParserQuote, null_inputs_are_diagnosed_and_registered_quote_is_dispatched_once)
{
	static const char* const childMarker = "WT_R6_QUOTE_DIAGNOSTIC_CHILD";
	if (std::getenv(childMarker) == NULL)
	{
#ifdef _WIN32
		GTEST_SKIP() << "POSIX subprocess harness is unavailable on Windows";
#else
		const std::string dir = prepare_dir("r6_quote_child");
		const std::string outputPath = dir + "/child.log";
		const ChildTestResult child = run_isolated_gtest(
			"R6PorterExtParserQuote.null_inputs_are_diagnosed_and_registered_quote_is_dispatched_once",
			childMarker, outputPath);
		EXPECT_TRUE(child.spawned) << "failed to spawn isolated quote test process";
		EXPECT_FALSE(child.timed_out) << child.output;
		ASSERT_TRUE(child.spawned);
		ASSERT_FALSE(child.timed_out) << child.output;
		ASSERT_TRUE(child.reaped) << child.output;
		ASSERT_TRUE(WIFEXITED(child.status)) << child.output;
		EXPECT_EQ(0, WEXITSTATUS(child.status)) << child.output;
		if (child.pid > 0)
		{
			std::error_code ec;
			std::filesystem::remove_all(std::filesystem::temp_directory_path() /
				(std::string("r82_porter_r6_quote_nulls_") + std::to_string(child.pid)), ec);
		}
		clean_dir(dir);
#endif
		return;
	}

	const std::string dir = prepare_dir("r6_quote_nulls");
	write_real_fixtures(dir);
	write_text(dir + "/actions.json", "{\"default\":{}}");
	write_text(dir + "/logcfg.json",
		"{\"root\":{\"async\":false,\"level\":\"warn\",\"sinks\":["
		"{\"type\":\"console_sink\",\"pattern\":\"%v\"}]}}");

	// Local configured runner lets the test observe the actual registered
	// ParserAdapter -> IParserStub dispatch without starting an engine thread.
	// This test body runs in a fresh process so its WTSLogger initialization and
	// singleton C API calls cannot affect later tests in the parent process.
	ASSERT_EQ(0, ::chdir(dir.c_str()));
	QuoteProbeRunner runner;
	ASSERT_TRUE(runner.init("logcfg.json", true, dir.c_str()));
	const std::string cfg = real_cfg(dir, "cta");
	ASSERT_TRUE(runner.config(cfg.c_str(), false));
	const char* registeredId = "r6_registered_quote_probe";
	ASSERT_TRUE(runner.createExtParser(registeredId));

	WTSTickStruct tick;
	memset(&tick, 0, sizeof(tick));
	strncpy(tick.code, "au2406", sizeof(tick.code) - 1);
	strncpy(tick.exchg, "SHFE", sizeof(tick.exchg) - 1);
	tick.trading_date = 20240101;
	tick.action_date = 20240101;
	tick.action_time = 93000;
	tick.volume = 1;
	tick.price = 100.0;
	unsigned char before[sizeof(tick)];
	memcpy(before, &tick, sizeof(tick));

	const std::string runnerNullId = capture_log_output([&]() {
		runner.on_ext_parser_quote(NULL, &tick, 0);
	});
	EXPECT_NE(std::string::npos, runnerNullId.find("parser id is null or empty"));
	EXPECT_EQ(0, memcmp(before, &tick, sizeof(tick)));
	EXPECT_EQ(0u, runner.quote_calls);

	const std::string runnerRegisteredNullTick = capture_log_output([&]() {
		runner.on_ext_parser_quote(registeredId, NULL, 0);
	});
	EXPECT_NE(std::string::npos, runnerRegisteredNullTick.find("tick is null"));
	EXPECT_NE(std::string::npos, runnerRegisteredNullTick.find(registeredId));
	EXPECT_EQ(0u, runner.quote_calls);

	const std::string runnerUnknownNullTick = capture_log_output([&]() {
		runner.on_ext_parser_quote("r6_unknown_quote_probe", NULL, 0);
	});
	EXPECT_NE(std::string::npos, runnerUnknownNullTick.find("tick is null"));
	EXPECT_EQ(std::string::npos, runnerUnknownNullTick.find("not exists"))
		<< "unknown-id + null-tick must be diagnosed by the null-tick guard, not treated as a registry miss";
	EXPECT_EQ(0u, runner.quote_calls);
	const std::string runnerUnknownId = capture_log_output([&]() {
		runner.on_ext_parser_quote("r6_unknown_quote_probe", &tick, 0);
	});
	EXPECT_NE(std::string::npos, runnerUnknownId.find("not exists"));
	EXPECT_EQ(0u, runner.quote_calls);

	// The exported C API forwards to the singleton runner. Exercise both public
	// NULL-input calls independently instead of counting only direct runner use.
	const std::string capiNullId = capture_log_output([&]() {
		parser_push_quote(NULL, &tick, 0);
	});
	EXPECT_NE(std::string::npos, capiNullId.find("parser id is null or empty"));
	EXPECT_EQ(0, memcmp(before, &tick, sizeof(tick)));

	const std::string capiNullTick = capture_log_output([&]() {
		parser_push_quote("r6_unknown_capi_quote", NULL, 0);
	});
	EXPECT_NE(std::string::npos, capiNullTick.find("tick is null"));
	EXPECT_EQ(std::string::npos, capiNullTick.find("not exists"));

	EXPECT_NO_THROW(runner.on_ext_parser_quote(registeredId, &tick, 0));
	EXPECT_EQ(1u, runner.quote_calls);
	EXPECT_EQ("SHFE.au.2406", runner.last_code);
	EXPECT_DOUBLE_EQ(100.0, runner.last_price);

	runner.release();
	clean_dir(dir);
}

/* ---------------------------------------------------------------------------
 * WtRtRunner.cpp:203-251 + 436-437 + 1137-1138
 * 扩展数据加载器的周期映射矩阵（含 Minute1 与未知周期）、session 事件的
 * EngineType default、日志回调转发。
 * ------------------------------------------------------------------------- */
TEST(R82RunnerEdges, LoaderPeriodMatrixAndDispatchDefaults)
{
	const std::string dir = prepare_dir("loader");
	ASSERT_FALSE(dir.empty());

	WtRtRunner* r = new_heap_runner(dir);
	ASSERT_NE(r, nullptr);
	r->registerExtDataLoader(fnl_loader, raw_loader, fct_loader, NULL);

	g_fnl.clear();
	EXPECT_TRUE(r->loadFinalHisBars(NULL, "SHFE.au.2406", KP_DAY, bar_sink));
	EXPECT_EQ(g_fnl.period, "d1");
	EXPECT_TRUE(r->loadFinalHisBars(NULL, "SHFE.au.2406", KP_Minute1, bar_sink));
	EXPECT_EQ(g_fnl.period, "m1");
	EXPECT_TRUE(r->loadFinalHisBars(NULL, "SHFE.au.2406", KP_Minute5, bar_sink));
	EXPECT_EQ(g_fnl.period, "m5");
	EXPECT_FALSE(r->loadFinalHisBars(NULL, "SHFE.au.2406", KP_Hour, bar_sink));
	EXPECT_EQ(g_fnl.calls, 3U);

	g_raw.clear();
	EXPECT_TRUE(r->loadRawHisBars(NULL, "SHFE.au.2406", KP_DAY, bar_sink));
	EXPECT_EQ(g_raw.period, "d1");
	EXPECT_TRUE(r->loadRawHisBars(NULL, "SHFE.au.2406", KP_Minute1, bar_sink));
	EXPECT_EQ(g_raw.period, "m1");
	EXPECT_TRUE(r->loadRawHisBars(NULL, "SHFE.au.2406", KP_Minute5, bar_sink));
	EXPECT_EQ(g_raw.period, "m5");
	EXPECT_FALSE(r->loadRawHisBars(NULL, "SHFE.au.2406", KP_Hour, bar_sink));
	EXPECT_EQ(g_raw.calls, 3U);

	/* L253-275：复权因子加载器对空合约与指定合约两路转发。 */
	g_fct.clear();
	EXPECT_TRUE(r->loadAllAdjFactors(NULL, fct_sink));
	EXPECT_EQ(g_fct.code, "");
	EXPECT_TRUE(r->loadAdjFactors(NULL, "SHFE.au.2406", fct_sink));
	EXPECT_EQ(g_fct.code, "SHFE.au.2406");
	EXPECT_EQ(g_fct.calls, 2U);

	/* L436-437：EngineType 取枚举外的值走 default。 */
	EXPECT_NO_THROW(r->ctx_on_session_event(1, 20240101, true, (EngineType)0));

	/* L1137-1138：notifier 未 init（_mq_sid==0）时只做 tag 取值后返回。 */
	EXPECT_NO_THROW(r->handleLogAppend(LL_INFO, "r82 log route"));

	clean_dir(dir);
}

/* ---------------------------------------------------------------------------
 * WtRtRunner.cpp:594-704 + 724 + 888 + 936 + 1068
 * config 各段的失败臂：parser 文件缺失；traders 文件为对象/坏 JSON；
 * executers 文件为对象/坏 JSON；data 段缺失；strategies 缺 cta。
 * 每个 runner 独立构造，互不共享 _config。
 * ------------------------------------------------------------------------- */
TEST(R82RunnerEdges, ConfigFailureArmsAcrossSections)
{
	const std::string dir = prepare_dir("cfgerr");
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/actions.json", kActions);
	write_text(dir + "/bad.json", "{ this is not a json document");
	write_text(dir + "/traders_obj.json", "{\"traders\":{}}");
	write_text(dir + "/executers_obj.json", "{\"executers\":{}}");

	const std::string tail = "\"env\":{\"name\":\"cta\"},\"data\":{},"
		"\"bspolicy\":\"" + dir + "/actions.json\"";

	{
		/* parser 配置文件不存在必须拒绝整个配置。 */
		WtRtRunner* r = new_heap_runner(dir);
		const std::string cfg = "{\"basefiles\":{},\"parsers\":\"" + dir
			+ "/absent_parsers.json\"," + tail + "}";
		EXPECT_FALSE(r->config(cfg.c_str(), false));
	}

	{
		/* L630-647 + L1068：traders 文件内容不是数组，config 必须拒绝。 */
		WtRtRunner* r = new_heap_runner(dir);
		const std::string cfg = "{\"basefiles\":{},\"traders\":\""
			+ dir + "/traders_obj.json\"," + tail + "}";
		EXPECT_FALSE(r->config(cfg.c_str(), false));
	}

	{
		/* L643-646：traders 文件坏 JSON。 */
		WtRtRunner* r = new_heap_runner(dir);
		const std::string cfg = "{\"basefiles\":{},\"traders\":\""
			+ dir + "/bad.json\"," + tail + "}";
		EXPECT_FALSE(r->config(cfg.c_str(), false));
	}

	{
		/* trader 配置路径缺失必须拒绝本轮配置并释放旧交易通道。 */
		WtRtRunner* r = new_heap_runner(dir);
		const std::string cfg = "{\"basefiles\":{},\"traders\":\""
			+ dir + "/missing_traders.json\"," + tail + "}";
		EXPECT_FALSE(r->config(cfg.c_str(), false));
	}

	{
		/* executers 文件存在但 executers 段不是数组。迭代03 卡8b=A（BUG-R134-003/WSN06-001）：
		 * 执行器表在任何副作用之前整表预检，非法表使 config 失败（原断言为 TRUE，.before 见 s5-runners 证据）。 */
		WtRtRunner* r = new_heap_runner(dir);
		const std::string cfg = "{\"basefiles\":{},\"executers\":\""
			+ dir + "/executers_obj.json\"," + tail + "}";
		EXPECT_FALSE(r->config(cfg.c_str(), false));
	}

	{
		/* executers 文件坏 JSON：同上，卡8b=A 下 config 失败（原断言为 TRUE）。 */
		WtRtRunner* r = new_heap_runner(dir);
		const std::string cfg = "{\"basefiles\":{},\"executers\":\""
			+ dir + "/bad.json\"," + tail + "}";
		EXPECT_FALSE(r->config(cfg.c_str(), false));
	}

	{
		/* L884-896：缺 data 段，config 不检查 initDataMgr 返回值（BUG-R69-003）。 */
		WtRtRunner* r = new_heap_runner(dir);
		const std::string cfg = "{\"basefiles\":{},\"env\":{\"name\":\"cta\"},"
			"\"bspolicy\":\"" + dir + "/actions.json\"}";
		EXPECT_TRUE(r->config(cfg.c_str(), false));
	}

	{
		/* L716-724：strategies 是对象但没有 cta 数组。 */
		WtRtRunner* r = new_heap_runner(dir);
		const std::string cfg = "{\"basefiles\":{},\"strategies\":{},"
			+ tail + "}";
		EXPECT_TRUE(r->config(cfg.c_str(), false));
	}

	clean_dir(dir);
}

/* ---------------------------------------------------------------------------
 * WtRtRunner.cpp:791-814 + 838-854 + 884-896
 * HFT 模式：缺 hft 段仍表示无策略；active 无效策略必须使 config 返回 false；
 * env 段缺失时 initEngine 返回 false，config 也应拒绝。
 * ------------------------------------------------------------------------- */
TEST(R82RunnerEdges, HftStrategyListAndEngineWithoutEnv)
{
	const std::string dir = prepare_dir("hftcfg");
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/actions.json", kActions);

	const std::string tail = "\"env\":{\"name\":\"hft\"},\"data\":{},"
		"\"bspolicy\":\"" + dir + "/actions.json\"";

	{
		/* L797-799：strategies 对象缺 hft 数组。 */
		WtRtRunner* r = new_heap_runner(dir);
		const std::string cfg = "{\"basefiles\":{},\"strategies\":{},"
			+ tail + "}";
		EXPECT_TRUE(r->config(cfg.c_str(), false));
		EXPECT_NE(dynamic_cast<WtHftEngine*>(r->getEngine()), nullptr);
	}

	{
		/* active 项 name 无 '.' -> HftStrategyMgr::createStrategy 为空并拒绝配置。 */
		WtRtRunner* r = new_heap_runner(dir);
		const std::string cfg = "{\"basefiles\":{},"
			"\"strategies\":{\"hft\":[{\"active\":true,\"id\":\"r82_hft_off\","
			"\"name\":\"nodot\"}]}," + tail + "}";
		EXPECT_FALSE(r->config(cfg.c_str(), false));
		EXPECT_NE(dynamic_cast<WtHftEngine*>(r->getEngine()), nullptr);
	}

	{
		/* active 项缺 name 必须在创建策略和 context 之前拒绝。 */
		WtRtRunner* r = new_heap_runner(dir);
		const std::string cfg = "{\"basefiles\":{},"
			"\"strategies\":{\"hft\":[{\"active\":true,\"id\":\"r82_hft_missing_name\"}]},"
			+ tail + "}";
		EXPECT_FALSE(r->config(cfg.c_str(), false));
	}

	{
		/* env 缺失 -> initEngine false 并传播为 config 失败。 */
		WtRtRunner* r = new_heap_runner(dir);
		const std::string cfg = "{\"basefiles\":{},\"data\":{},"
			"\"bspolicy\":\"" + dir + "/actions.json\"}";
		EXPECT_FALSE(r->config(cfg.c_str(), false));
	}

	clean_dir(dir);
}

TEST(R82RunnerEdges, HftFactoryNullUnitFailsAndSupportedUnitConfigures)
{
	const std::string dir = prepare_dir("hftfactory");
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/actions.json", kActions);

	const std::filesystem::path factoryPath = std::filesystem::path(test_binary_dir())
#ifdef _WIN32
		/ "WtHftStraFact.dll";
#elif defined(__APPLE__)
		/ "libWtHftStraFact.dylib";
#else
		/ "libWtHftStraFact.so";
#endif
	std::error_code ec;
	if (!std::filesystem::is_regular_file(factoryPath, ec))
	{
		if (ec)
			FAIL() << "Could not inspect HFT fixture factory at " << factoryPath.string()
				<< ": " << ec.message();
		GTEST_SKIP() << "HFT fixture factory is unavailable at " << factoryPath.string();
	}
	const std::filesystem::path isolatedFactoryDir = std::filesystem::path(dir) / "hft_factories";
	std::filesystem::create_directories(isolatedFactoryDir, ec);
	ASSERT_FALSE(ec) << ec.message();
	const std::filesystem::path isolatedFactory = isolatedFactoryDir / factoryPath.filename();
	ASSERT_TRUE(std::filesystem::copy_file(factoryPath, isolatedFactory, ec))
		<< "Could not copy HFT fixture factory from " << factoryPath.string()
		<< " to " << isolatedFactory.string() << ": " << ec.message();
	ec.clear();
	ASSERT_TRUE(std::filesystem::is_regular_file(isolatedFactory, ec))
		<< "Copied HFT fixture factory is not a regular file: " << isolatedFactory.string()
		<< ": " << ec.message();

	const std::string tail = "\"env\":{\"name\":\"hft\"},\"data\":{},"
		"\"bspolicy\":\"" + dir + "/actions.json\","
		"\"traders\":[{\"active\":true,\"id\":\"r82_trd\",\"module\":\"TraderMocker\",\"mockerid\":82001,\"udp_port\":0}]";
	{
		WtRtRunner* r = new_heap_runner(dir);
		wtp_test::TraderMockerFixture traderFixture("r82_hft_factory_missing");
		ASSERT_TRUE(traderFixture.install()) << traderFixture.error();
		ASSERT_TRUE(r->addHftFactories(isolatedFactoryDir.string().c_str()));
		const std::string cfg = "{\"basefiles\":{},\"strategies\":{\"hft\":[{"
			"\"active\":true,\"id\":\"r82_hft_null_unit\","
			"\"name\":\"WtHftStraFact.DoesNotExist\",\"trader\":\"r82_trd\"}]}," + tail + "}";
		EXPECT_FALSE(r->config(cfg.c_str(), false));
	}

	{
		WtRtRunner* r = new_heap_runner(dir);
		wtp_test::TraderMockerFixture traderFixture("r82_hft_factory_valid");
		ASSERT_TRUE(traderFixture.install()) << traderFixture.error();
		ASSERT_TRUE(r->addHftFactories(isolatedFactoryDir.string().c_str()));
		const std::string cfg = "{\"basefiles\":{},\"strategies\":{\"hft\":[{"
			"\"active\":true,\"id\":\"r82_hft_valid\","
			// BUG-WSN06-002（迭代03 卡E）：SimpleHft 参数严格 schema 要求 code 必填，空对象被拒绝
			// 且 init(false) 现传播为配置失败；合法正分支须给出最小合法参数（原为 params:{}）。
			"\"name\":\"WtHftStraFact.SimpleHft\",\"trader\":\"r82_trd\",\"params\":{\"code\":\"SHFE.au.2406\"}}]}," + tail + "}";
		EXPECT_TRUE(r->config(cfg.c_str(), false));
		EXPECT_NE(dynamic_cast<WtHftEngine*>(r->getEngine()), nullptr);
	}

	clean_dir(dir);
}

TEST(R82RunnerEdges, LaterInvalidHftFactoryDoesNotLeaveEarlierContextInstalled)
{
	const std::string dir = prepare_dir("hft_partial_context");
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/actions.json", kActions);

	const std::filesystem::path factoryPath = std::filesystem::path(test_binary_dir())
#ifdef _WIN32
		/ "WtHftStraFact.dll";
#elif defined(__APPLE__)
		/ "libWtHftStraFact.dylib";
#else
		/ "libWtHftStraFact.so";
#endif
	std::error_code ec;
	if (!std::filesystem::is_regular_file(factoryPath, ec))
	{
		if (ec)
			FAIL() << "Could not inspect HFT fixture factory at " << factoryPath.string()
				<< ": " << ec.message();
		GTEST_SKIP() << "HFT fixture factory is unavailable at " << factoryPath.string();
	}
	const std::filesystem::path isolatedFactoryDir = std::filesystem::path(dir) / "hft_factories";
	std::filesystem::create_directories(isolatedFactoryDir, ec);
	ASSERT_FALSE(ec) << ec.message();
	const std::filesystem::path isolatedFactory = isolatedFactoryDir / factoryPath.filename();
	ASSERT_TRUE(std::filesystem::copy_file(factoryPath, isolatedFactory, ec))
		<< "Could not copy HFT fixture factory from " << factoryPath.string()
		<< " to " << isolatedFactory.string() << ": " << ec.message();
	ec.clear();
	ASSERT_TRUE(std::filesystem::is_regular_file(isolatedFactory, ec))
		<< "Copied HFT fixture factory is not a regular file: " << isolatedFactory.string()
		<< ": " << ec.message();

	HftStraContext idProbe(NULL, "r82_hft_id_probe", false, 0);
	const uint32_t wouldBeEarlierContext = idProbe.id() + 1;
	WtRtRunner* r = new_heap_runner(dir);
	wtp_test::TraderMockerFixture traderFixture("r82_hft_partial");
	ASSERT_TRUE(traderFixture.install()) << traderFixture.error();
	ASSERT_TRUE(r->addHftFactories(isolatedFactoryDir.string().c_str()));
	const std::string cfg = "{\"basefiles\":{},\"strategies\":{\"hft\":[{"
		"\"active\":true,\"id\":\"r82_hft_before_invalid\","
		"\"name\":\"WtHftStraFact.SimpleHft\",\"trader\":\"r82_trd\",\"params\":{}},"
		"{\"active\":true,\"id\":\"r82_hft_after_invalid\","
		"\"name\":\"WtHftStraFact.DoesNotExist\",\"trader\":\"r82_trd\"}]},"
		"\"env\":{\"name\":\"hft\"},\"data\":{},"
		"\"bspolicy\":\"" + dir + "/actions.json\","
		"\"traders\":[{\"active\":true,\"id\":\"r82_trd\",\"module\":\"TraderMocker\",\"mockerid\":82001,\"udp_port\":0}]}";
	EXPECT_FALSE(r->config(cfg.c_str(), false));
	EXPECT_EQ(nullptr, r->getHftContext(wouldBeEarlierContext));

	clean_dir(dir);
}

TEST(R82RunnerEdges, ActiveCtaStrategyRequiresNameAndResolvableFactory)
{
	const std::string dir = prepare_dir("ctastrategy");
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/actions.json", kActions);
	const std::string tail = "\"env\":{\"name\":\"cta\"},\"data\":{},"
		"\"bspolicy\":\"" + dir + "/actions.json\"";

	{
		WtRtRunner* r = new_heap_runner(dir);
		const std::string cfg = "{\"basefiles\":{},\"strategies\":{\"cta\":[{"
			"\"active\":true,\"id\":\"r82_cta_missing_name\"}]}," + tail + "}";
		EXPECT_FALSE(r->config(cfg.c_str(), false));
	}

	{
		WtRtRunner* r = new_heap_runner(dir);
		const std::string cfg = "{\"basefiles\":{},\"strategies\":{\"cta\":[{"
			"\"active\":true,\"id\":\"r82_cta_missing_factory\","
			"\"name\":\"MissingFactory.SomeUnit\"}]}," + tail + "}";
		EXPECT_FALSE(r->config(cfg.c_str(), false));
	}

	clean_dir(dir);
}

/* ---------------------------------------------------------------------------
 * WtRtRunner.cpp:933-1052
 * initExecuters 的 kind 分支与 trader 绑定失败支：空 name -> local；
 * local/diff/arbi 指向不存在的 trader；dist 走独立分支。
 * ------------------------------------------------------------------------- */
TEST(R82RunnerEdges, ExecuterKindFallbacksAndMissingTraders)
{
	const std::string dir = prepare_dir("exec");
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/actions.json", kActions);

	WtRtRunner* r = new_heap_runner(dir);
	ASSERT_NE(r, nullptr);

	const std::string cfg = "{\"basefiles\":{},"
		"\"env\":{\"name\":\"cta\"},\"data\":{},"
		"\"bspolicy\":\"" + dir + "/actions.json\","
		"\"executers\":["
		// 卡8b=A（迭代03）：local/diff/arbi 须带 scale 与含 default 的 policy。
		"{\"active\":true,\"id\":\"r82_exec_noname\",\"scale\":1,\"policy\":{\"default\":{\"name\":\"WtExeFact.WtMinImpactExeUnit\"}}},"
		"{\"active\":true,\"id\":\"r82_exec_local_badtrd\",\"name\":\"local\","
		"\"trader\":\"r82_absent_trader\",\"scale\":1,\"policy\":{\"default\":{\"name\":\"WtExeFact.WtMinImpactExeUnit\"}}},"
		"{\"active\":true,\"id\":\"r82_exec_diff_badtrd\",\"name\":\"diff\","
		"\"trader\":\"r82_absent_trader\",\"scale\":1,\"policy\":{\"default\":{\"name\":\"WtExeFact.WtMinImpactExeUnit\"}}},"
		"{\"active\":true,\"id\":\"r82_exec_arbi_badtrd\",\"name\":\"arbi\","
		"\"trader\":\"r82_absent_trader\",\"scale\":1,\"policy\":{\"default\":{\"name\":\"WtExeFact.WtMinImpactExeUnit\"}}},"
		"{\"active\":true,\"id\":\"r82_exec_dist\",\"name\":\"dist\"},"
		"{\"active\":false,\"id\":\"r82_exec_skipped\"}"
		"]}";

	EXPECT_TRUE(r->config(cfg.c_str(), false));
	EXPECT_NE(r->getEngine(), nullptr);

	clean_dir(dir);
}

/* ---------------------------------------------------------------------------
 * WtPorter.cpp:286-289 + WtRtRunner.cpp:1141-1155
 * release_porter 转发到 runner.release：置退出标记、释放 parser/trader、
 * 释放当前引擎，最后 WTSLogger::stop。
 *
 * 本用例必须排在文件最后：WTSLogger::stop 会进程级静默日志（BUG-R6-006）。
 * ------------------------------------------------------------------------- */
TEST(R82RunnerEdges, ReleasePorterStopsConfiguredRunner)
{
	const std::string dir = prepare_dir("release");
	write_real_fixtures(dir);
	ensure_cta_configured(dir);
	ASSERT_NE(getRunner().getEngine(), nullptr);

	EXPECT_NO_THROW(release_porter());

	clean_dir(dir);
}
