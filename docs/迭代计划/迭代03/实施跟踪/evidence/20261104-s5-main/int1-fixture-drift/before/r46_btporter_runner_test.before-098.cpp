/*
 * R46 - WtBtRunner 回测引导链补测
 *
 * 目标：src/WtBtPorter/WtBtRunner.cpp
 *   - init() L437-447（WTSLogger::init 守卫、setInstDir/setOutputDir）
 *   - config() L449-462（空内容/坏 JSON 的早退）、L464-471 + L500-507（mode=none）、
 *     L472-479（mode=cta 创建 ExpCtaMocker 并尝试 init_cta_factory）
 *   - initCtaMocker() L266-283、initSelMocker() L299-313（含二次创建替换旧 mocker）
 *   - stop() L550-559（未启动时的早退）
 *
 * 与既有 lane 的分工：q06/r9 只覆盖回调分派、feed 链、config 文件缺失与
 * initEvtNotifier 的拒绝分支；config 的「内容解析成功」段、mocker 创建、stop 早退
 * 在报告里仍未覆盖。q06 的 config 用例还会断言单例 mockers 为空，所以本文件
 * 一律用堆上 new、故意不 delete 的 runner，绝不触碰 getRunner() 单例的 mocker 槽位。
 *
 * 新实例回归会直接读取初始化后的 isAutoTrans()；run() 只在 _inited=false 的
 * 失败守卫用例中调用。其余生命周期行为由各自的 runner lifecycle 用例覆盖。
 * 所有落盘都在系统临时目录的专属子目录，用例末尾 remove_all。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

#include "WtBtPorter/WtBtPorter.h"
#include "WtBtPorter/WtBtRunner.h"

#include "WtBtCore/WtHelper.h"

USING_NS_WTP;

extern WtBtRunner& getRunner();

namespace {
	namespace fs = std::filesystem;

	const char* kOutLeaf = "wt_r46_btporter_out";

	std::string prepare_out_dir()
	{
		std::filesystem::path p = std::filesystem::temp_directory_path() / kOutLeaf;
		std::error_code ec;
		std::filesystem::create_directories(p, ec);
		return p.string();
	}

	void clean_dir(const std::string& dir)
	{
		std::error_code ec;
		std::filesystem::remove_all(dir, ec);
	}

	void write_session_fixture(const std::string& dir)
	{
		std::ofstream(dir + "/sessions.json")
			<< "{\"TRADING\":{\"name\":\"Trading\",\"offset\":0,"
			<< "\"sections\":[{\"from\":0,\"to\":2359}]}}";
		// 卡1：日期映射须有交易日历（CHINA）
		std::ofstream(dir + "/holidays.json") << "{\"CHINA\":[]}";
	}

	std::string replayer_with_session(const std::string& dir)
	{
		return "\"replayer\":{\"mode\":\"none\",\"path\":\"" + dir +
			"\",\"stime\":20240102000000,\"etime\":20240103000000,"
			"\"basefiles\":{\"session\":\"" + dir + "/sessions.json\",\"holiday\":\"" + dir + "/holidays.json\"}}";
	}

	std::string none_mode_config(const std::string& dir)
	{
		return "{" + replayer_with_session(dir) + ",\"env\":{\"mocker\":\"none\"}}";
	}

	fs::path executable_dir()
	{
#ifdef _WIN32
		char path[MAX_PATH] = { 0 };
		const DWORD size = GetModuleFileNameA(NULL, path, MAX_PATH);
		return size == 0 ? fs::current_path() : fs::path(path).parent_path();
#elif defined(__APPLE__)
		uint32_t size = 0;
		_NSGetExecutablePath(NULL, &size);
		std::vector<char> path(size + 1, 0);
		if (_NSGetExecutablePath(path.data(), &size) != 0)
			return fs::current_path();
		return fs::path(path.data()).parent_path();
#else
		char path[4096] = { 0 };
		const ssize_t size = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
		return size <= 0 ? fs::current_path()
			: fs::path(std::string(path, static_cast<size_t>(size))).parent_path();
#endif
	}

	std::string exec_valid_plugin_path()
	{
#ifdef _WIN32
		const char* suffix = ".dll";
#elif defined(__APPLE__)
		const char* suffix = ".dylib";
#else
		const char* suffix = ".so";
#endif
		fs::path dir = executable_dir();
		for (int depth = 0; depth != 8 && !dir.empty(); ++depth)
		{
			const fs::path candidates[] = {
				dir / "exec_v2_plugins" / (std::string("valid") + suffix),
				dir / "coverage" / "exec_v2_plugins" / (std::string("valid") + suffix),
				dir / "TestUnits" / "coverage" / "exec_v2_plugins" / (std::string("valid") + suffix),
				dir / "src" / "TestUnits" / "coverage" / "exec_v2_plugins" / (std::string("valid") + suffix)
			};
			for (const fs::path& candidate : candidates)
				if (fs::exists(candidate))
					return candidate.string();
			dir = dir.parent_path();
		}
		return std::string();
	}

	std::string cta_strategy_plugin_path()
	{
#ifdef _WIN32
		const char* filename = "WtCtaStraFact.dll";
#elif defined(__APPLE__)
		const char* filename = "libWtCtaStraFact.dylib";
#else
		const char* filename = "libWtCtaStraFact.so";
#endif
		fs::path dir = executable_dir();
		for (int depth = 0; depth != 8 && !dir.empty(); ++depth)
		{
			const fs::path candidates[] = {
				dir / filename,
				dir / "bin" / filename
			};
			for (const fs::path& candidate : candidates)
				if (fs::exists(candidate))
					return candidate.string();
			dir = dir.parent_path();
		}
		return std::string();
	}

	std::string tail_char(const std::string& s)
	{
		return s.empty() ? std::string() : s.substr(s.size() - 1);
	}

	/* 堆实例与进程级 C API 单例隔离；此配置 fixture 不主动运行 release/析构路径。 */
	WtBtRunner* new_runner()
	{
		return new WtBtRunner();
	}

} // namespace

/* IBtDataLoader 和 registerExtDataLoader 的默认值均为 true；新对象需有相同的
 * 确定默认值，注册时显式 false/true 可覆盖，省略 bAutoTrans 时使用 true。 */
TEST(R46BtRunner, LoaderAutoTransHasDefinedDefaultAndSetterBehavior)
{
	WtBtRunner* r = new_runner();
	ASSERT_NE(r, nullptr);

	EXPECT_TRUE(r->isAutoTrans());
	r->registerExtDataLoader(nullptr, nullptr, nullptr, nullptr, false);
	EXPECT_FALSE(r->isAutoTrans());
	r->registerExtDataLoader(nullptr, nullptr, nullptr, nullptr, true);
	EXPECT_TRUE(r->isAutoTrans());
	r->registerExtDataLoader(nullptr, nullptr, nullptr, nullptr);
	EXPECT_TRUE(r->isAutoTrans());
}

/* WtBtRunner.cpp: config_v2 accepts the supported no-mocker mode without a
 * mocker-specific section, while still completing replayer initialization. */
TEST(R46BtRunner, ConfigWithNoneMockerBuildsOnlyTheReplayer)
{
	const std::string dir = prepare_out_dir();
	ASSERT_FALSE(dir.empty());

	WtBtRunner* r = new_runner();
	ASSERT_NE(r, nullptr);
	EXPECT_EQ(r->cta_mocker(), nullptr);
	EXPECT_EQ(r->sel_mocker(), nullptr);
	EXPECT_FALSE(r->isAsync());

	const std::string cfg =
		"{\"replayer\":{\"mode\":\"none\",\"basefiles\":{},\"path\":\"" + dir +
		"\",\"stime\":20240102000000,\"etime\":20240103000000},"
		"\"env\":{\"mocker\":\"none\"}}";

	std::string error;
	ASSERT_TRUE(r->config_v2(cfg.c_str(), false, &error)) << error;

	EXPECT_EQ(r->cta_mocker(), nullptr);
	EXPECT_EQ(r->sel_mocker(), nullptr);
	EXPECT_NE(&r->replayer(), (HisDataReplayer*)NULL);

	clean_dir(dir);
}

/* WtBtRunner.cpp:457-462：内容不是合法 JSON 时 load_from_content 返回 NULL，
 * 只打 "Loading config failed"，不建 mockers、不回放。 */
TEST(R46BtRunner, ConfigWithBrokenContentLeavesMockersEmpty)
{
	WtBtRunner* r = new_runner();
	ASSERT_NE(r, nullptr);

	r->config("{ this is not a json config", false);

	EXPECT_EQ(r->cta_mocker(), nullptr);
	EXPECT_EQ(r->sel_mocker(), nullptr);
	EXPECT_FALSE(r->isAsync());
}

/* 生产：WtBtRunner::config 必须在回放器配置初始化失败时早退；
 * 随后的 void run() 只能记录失败并返回，不能调用 prepare/run 或启动策略。
 * 子进程隔离可检测旧路径继续注册策略/进入回放导致的崩溃。 */
TEST(R46BtRunner, InvalidReplayerConfigPreventsStrategyRegistrationAndRun)
{
	ASSERT_EXIT(
		{
			WtBtRunner* r = new WtBtRunner();
			r->config("{\"replayer\":{\"mode\":\"none\"},"
				"\"env\":{\"mocker\":\"cta\"},"
				"\"cta\":{\"name\":\"r46bt_invalid_replayer\",\"slippage\":0}}", false);
			if (r->cta_mocker() != nullptr)
				std::_Exit(2);
			r->run(false, false);
			std::_Exit(0);
		},
		::testing::ExitedWithCode(0), "");
}

/* 生产：有效配置应能完成 prepare；若 replayer 已处于准备态，后续 run 必须在
 * prepare 返回 false 时早退，不应启动第二个 worker。 */
TEST(R46BtRunner, PrepareFailureDoesNotStartAsyncRunAndValidPrepareRemainsUsable)
{
	const std::string dir = prepare_out_dir();
	ASSERT_FALSE(dir.empty());
	{
		std::ofstream(dir + "/sessions.json")
			<< "{\"TRADING\":{\"name\":\"Trading\",\"offset\":0,"
			<< "\"sections\":[{\"from\":0,\"to\":2359}]}}";
		std::ofstream(dir + "/holidays.json") << "{\"CHINA\":[]}";	// 卡1：交易日历
		std::ofstream(dir + "/commodities.json")
			<< "{\"SHFE\":{\"rb\":{\"name\":\"Rebar\",\"session\":\"TRADING\","
			<< "\"pricetick\":1,\"volscale\":10,\"covermode\":0,\"pricemode\":0,"
			<< "\"trademode\":0,\"category\":1}}}";
		std::ofstream(dir + "/contracts.json")
			<< "{\"SHFE\":{\"rb2510\":{\"name\":\"rb2510\",\"product\":\"rb\","
			<< "\"exchg\":\"SHFE\"}}}";
	}

	WtBtRunner* r = new_runner();
	ASSERT_NE(r, nullptr);
	const std::string module = cta_strategy_plugin_path();
	ASSERT_FALSE(module.empty()) << "missing local CTA strategy plugin";

	const std::string cfg = "{\"replayer\":{\"mode\":\"none\",\"path\":\"" + dir
		+ "\",\"stime\":20240102000000,\"etime\":20240103000000,\"tick\":false,"
		+ "\"basefiles\":{\"session\":\"" + dir + "/sessions.json\",\"holiday\":\"" + dir + "/holidays.json\",\"commodity\":\""
		+ dir + "/commodities.json\",\"contract\":\"" + dir + "/contracts.json\"}},"
		+ "\"env\":{\"mocker\":\"cta\"},\"cta\":{\"name\":\"r46_prepare_guard\","
		+ "\"module\":\"" + module + "\",\"slippage\":0,\"strategy\":{"
		+ "\"name\":\"DualThrust\",\"id\":\"r46_prepare_guard\",\"params\":{"
		+ "\"days\":2,\"k1\":0.5,\"k2\":0.5,\"period\":\"d\",\"count\":5,"
		+ "\"code\":\"SHFE.rb2510\",\"stock\":false}}}}";
	std::string error;
	ASSERT_TRUE(r->config_v2(cfg.c_str(), false, &error)) << error;
	ASSERT_NE(r->cta_mocker(), nullptr);

	ASSERT_TRUE(r->replayer().prepare());

	r->run(false, true);
	EXPECT_FALSE(r->isAsync()) << "failed prepare must not launch or mark an async run";
	clean_dir(dir);
}

/* WtBtRunner.cpp:266-283 + 299-313：CTA mocker 二次创建会 delete 旧实例并换新 id；
 * SEL mocker 走 register_task 的 "d" 周期分支。两个 mocker 都不需要行情存储。 */
TEST(R46BtRunner, CtaAndSelMockersCanBeCreatedAndReplaced)
{
	WtBtRunner* r = new_runner();
	ASSERT_NE(r, nullptr);

	const uint32_t ctaId1 = r->initCtaMocker("r46bt_cta_a", 0, false, false, false, false);
	ASSERT_NE(ctaId1, 0u);
	ASSERT_NE(r->cta_mocker(), nullptr);

	const uint32_t ctaId2 = r->initCtaMocker("r46bt_cta_b", 2, false, false, false, false);
	ASSERT_NE(ctaId2, 0u);
	EXPECT_NE(ctaId2, ctaId1) << "重建 mocker 必须换新上下文 id";
	EXPECT_NE(r->cta_mocker(), nullptr);

	const uint32_t selId = r->initSelMocker("r46bt_sel", 20240102, 93000, "d");
	ASSERT_NE(selId, 0u);
	EXPECT_NE(r->sel_mocker(), nullptr);
}

/* WtBtRunner.cpp:550-559：未启动（_running=false 且 _worker 为空）时 stop() 直接返回。
 * 只能用静态零初始化的单例：堆实例的 _worker 是未初始化 shared_ptr（BUG-R46-001）。 */
TEST(R46BtRunner, StopBeforeRunIsANoOpOnTheSingleton)
{
	EXPECT_NO_THROW(getRunner().stop());
	EXPECT_NO_THROW(getRunner().stop());
}

/* WtBtRunner.cpp:437-447：不存在的日志文件由 WTSLogger::init 的 exists 守卫吞掉；
 * 随后 setInstDir(getBinDir()) 与 setOutputDir(outDir) 把全局目录改到临时目录。 */
TEST(R46BtRunner, InitStampsInstanceAndOutputDirectories)
{
	const std::string dir = prepare_out_dir();
	ASSERT_FALSE(dir.empty());

	WtBtRunner* r = new_runner();
	ASSERT_NE(r, nullptr);

	r->init((dir + "/r46_absent_logcfg.yaml").c_str(), true, dir.c_str());

	EXPECT_FALSE(WtHelper::getInstDir().empty());
	EXPECT_EQ(tail_char(WtHelper::getInstDir()), "/");
	EXPECT_EQ(std::string(WtHelper::getOutputDir()), dir + "/");

	clean_dir(dir);
}

/* Missing env must be rejected before the real replayer init loads sessions.
 * Then the runner's run guard safely rejects the unconfigured instance, and a
 * corrected config remains retryable because no replay state was initialized. */
TEST(R46BtRunner, ConfigWithoutEnvRejectsBeforeReplayerInitAndAllowsRetry)
{
	const std::string dir = prepare_out_dir();
	ASSERT_FALSE(dir.empty());
	write_session_fixture(dir);
	const std::string cfg = "{" + replayer_with_session(dir) + "}";
	const std::string validCfg = none_mode_config(dir);

	ASSERT_EXIT(
		{
			WtBtRunner* r = new WtBtRunner();
			std::string error;
			if (r->config_v2(cfg.c_str(), false, &error) ||
				error.find("env must be an object") == std::string::npos)
				std::_Exit(2);
			if (r->replayer().get_session_info("TRADING") != nullptr)
				std::_Exit(3); // proves HisDataReplayer::init did not load the valid file
			r->run(false, false);
			if (r->isAsync())
				std::_Exit(4);
			if (!r->config_v2(validCfg.c_str(), false, &error))
				std::_Exit(5);
			if (r->replayer().get_session_info("TRADING") == nullptr)
				std::_Exit(6);
			std::_Exit(0);
		},
		::testing::ExitedWithCode(0), "");

	clean_dir(dir);
}

TEST(R46BtRunner, MissingReplayerEnvAndMockerStructuresFailClosed)
{
	const std::string dir = prepare_out_dir();
	ASSERT_FALSE(dir.empty());
	write_session_fixture(dir);
	const std::string replay = replayer_with_session(dir);
	const std::vector<std::pair<std::string, std::string>> invalidConfigs = {
		{"{\"env\":{\"mocker\":\"none\"}}", "replayer must be an object"},
		{"{\"replayer\":null,\"env\":{\"mocker\":\"none\"}}", "replayer must be an object"},
		{"{" + replay + "}", "env must be an object"},
		{"{" + replay + ",\"env\":null}", "env must be an object"},
		{"{" + replay + ",\"env\":{}}", "env.mocker must be a string"},
		{"{" + replay + ",\"env\":{\"mocker\":3}}", "env.mocker must be a string"},
		{"{" + replay + ",\"env\":{\"mocker\":\"\"}}", "env.mocker is missing"},
		{"{" + replay + ",\"env\":{\"mocker\":\"unknown\"}}", "unsupported backtest mocker mode"},
		{"{" + replay + ",\"env\":{\"mocker\":\"cta\"},\"cta\":[]}", "mode configuration is missing or invalid"},
		{"{" + replay + ",\"env\":{\"mocker\":\"cta\"},\"cta\":{}}", "cta mode name is missing"}
	};

	ASSERT_EXIT(
		{
			for (size_t i = 0; i < invalidConfigs.size(); ++i)
			{
				WtBtRunner* r = new WtBtRunner();
				std::string error;
				if (r->config_v2(invalidConfigs[i].first.c_str(), false, &error) ||
					error.find(invalidConfigs[i].second) == std::string::npos)
					std::_Exit(static_cast<int>(10 + i));
				if (r->replayer().get_session_info("TRADING") != nullptr)
					std::_Exit(static_cast<int>(30 + i));
				r->run(false, false);
				if (r->isAsync())
					std::_Exit(static_cast<int>(50 + i));
			}
			std::_Exit(0);
		},
		::testing::ExitedWithCode(0), "");

	clean_dir(dir);
}

/* A valid exec configuration is a non-none supported-mode positive control.
 * The fixture is built as a dependency of CoverageTests_btporter. */
TEST(R46BtRunner, ValidExecModePassesPreflightAndCompletesConfiguration)
{
	const std::string dir = prepare_out_dir();
	ASSERT_FALSE(dir.empty());
	const std::string module = exec_valid_plugin_path();
	ASSERT_FALSE(module.empty()) << "missing coverage execution-plugin fixture valid";
	const std::string cfg = "{\"replayer\":{\"mode\":\"none\",\"path\":\"" + dir +
		"\",\"stime\":20240102000000,\"etime\":20240103000000,"
		"\"basefiles\":{}},\"env\":{\"mocker\":\"exec\"},"
		"\"exec\":{\"name\":\"r46_exec\",\"module\":\"" + module +
		"\",\"code\":\"SHFE.rb2510\",\"period\":\"m1\",\"volunit\":0,\"volmode\":0,"
		"\"executer\":{\"name\":\"R46Unit\",\"id\":\"r46_exec\",\"params\":{}},"
		"\"matcher\":{\"cancelrate\":0}}}";

	ASSERT_EXIT(
		{
			WtBtRunner* r = new WtBtRunner();
			std::string error;
			if (!r->config_v2(cfg.c_str(), false, &error))
				std::_Exit(2);
			std::_Exit(0);
		},
		::testing::ExitedWithCode(0), "");

	clean_dir(dir);
}

/* A CTA mode without the required strategy module must fail in its factory
 * initializer; the test observes the existing product contract instead of
 * expecting a mocker slot that is assigned only after successful init. */
TEST(R46BtRunner, CtaModeWithoutStrategyModuleDoesNotCreateMocker)
{
	const std::string dir = prepare_out_dir();
	ASSERT_FALSE(dir.empty());

	WtBtRunner* r = new_runner();
	ASSERT_NE(r, nullptr);

	const std::string cfg =
		"{\"replayer\":{\"mode\":\"none\",\"basefiles\":{},\"path\":\"" + dir +
		"\",\"stime\":20240102000000,\"etime\":20240103000000},"
		"\"env\":{\"mocker\":\"cta\"},"
		"\"cta\":{\"name\":\"r46bt_cta_cfg\",\"slippage\":0}}";

	std::string error;
	EXPECT_FALSE(r->config_v2(cfg.c_str(), false, &error));
	EXPECT_NE(error.find("strategy factory failed"), std::string::npos) << error;

	EXPECT_EQ(r->cta_mocker(), nullptr);
	EXPECT_EQ(r->sel_mocker(), nullptr);
	EXPECT_FALSE(r->isAsync());

	clean_dir(dir);
}
