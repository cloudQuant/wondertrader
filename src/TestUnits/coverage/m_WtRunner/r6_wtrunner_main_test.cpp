/*
 * R6 lane —— src/WtRunner/（可执行体 + WtRunner 类）
 *
 * 形状照抄 m_LoaderRunner/seam_loader_runner_test.cpp：构建期由 coverage/CMakeLists.txt
 * 对本模块目录下的每个 .cpp 追加 -Dmain=wt_coverage_main_wtrunner（小写、实测 LoaderRunner 已跑通），
 * 于是 main.cpp 的入口符号变成 wt_coverage_main_wtrunner，与本文件的 gtest_main 同链一个二进制。
 *
 * 安全口径（本轮硬规则）：
 *  - 只调用能自行终止、不起线程、不阻塞、不 exit() 宿主的路径；
 *  - main.cpp 里 -h/--help 分支：R6 当时以为它在 `WtRunner runner;` 之前 return 0，
 *    R28 实测更正 —— cppcli 在 parse() 内部就 std::exit(0)（Share/cppcli.hpp:1897-1900），
 *    main.cpp:86-87 的 return 0 是死代码，所以本文件的三条 help 用例已转 DISABLED_，
 *    help 分支的覆盖改由 r28_runner_main_test.cpp 用 fork 子进程钉住；
 *  - 当前 main() 会检查 runner.config() 的返回值；配置失败时返回 1，不调用 run()。
 *    缺省/显式缺失配置的入口行为由文件末尾的子进程回归覆盖，避免在 gtest
 *    宿主内触碰 runner 生命周期。
 *  - WtRunner.cpp 侧只走「不依赖 libWtCore 运行期」的成员函数（构造/析构/init/config 早退），
 *    并且必须堆上构造（见下方 BUG-R28-001 说明）。
 *
 * 关联缺陷见 docs/迭代计划/迭代01/bugs/R6_porter_runner.md（BUG-R6-011 起）。
 */
#include <gtest/gtest.h>

#ifndef _WIN32
#include <dlfcn.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cerrno>
#ifndef _WIN32
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#endif
#include <memory>
#include <string>
#include <vector>

#include "WtCore/WtHelper.h"
#include "WtRunner/WtRunner.h"
#include "Share/DLLHelper.hpp"

/* 生产 TU（main.cpp）经 -Dmain=... 改名后的入口；与测试自己的 gtest_main 不冲突 */
int wt_coverage_main_wtrunner(int argc, char* argv[]);

/* 生产 TU（WtRunner.cpp:26）的自由函数，未在 WtRunner.h 里声明，按定义处重新声明 */
const char* getBinDir();

/* 生产 TU（WtRunner.cpp:614）的级别->标签表，handleLogAppend 按 ll-100 取用 */
extern const char* LOG_TAGS[];

namespace {

/* 用受控 argv 调入口；argv[0] 只是程序名，cppcli 不校验它 */
int call_entry(std::vector<const char*> words)
{
	std::vector<char*> argv;
	argv.reserve(words.size());
	for (const char* w : words)
		argv.push_back(const_cast<char*>(w));
	return wt_coverage_main_wtrunner(static_cast<int>(argv.size()), argv.data());
}

#ifndef _WIN32
struct ChildResult
{
	bool exited = false;
	int code = -1;
	int signal = -1;
	bool engine_marker_created = false;
	bool parent_watchdog_timed_out = false;
	std::string output;
};

ChildResult call_entry_in_child(const std::vector<std::string>& words, bool use_success_config = false)
{
	ChildResult result;
	char directoryTemplate[] = "/tmp/wt-r6-011-XXXXXX";
	char* directory = mkdtemp(directoryTemplate);
	if (directory == nullptr)
		return result;

	const std::string outputPath = std::string(directory) + "/child-output.log";
	std::ofstream logConfig(std::string(directory) + "/logcfg.yaml");
	logConfig << "root:\n"
		"    async: false\n"
		"    level: error\n"
		"    sinks:\n"
		"    -   type: console_sink\n"
		"        pattern: '%v'\n";
	logConfig.close();
	if (!logConfig)
	{
		std::filesystem::remove_all(directory);
		return result;
	}
	if (use_success_config)
	{
		Dl_info binaryInfo = {};
		if (dladdr(reinterpret_cast<const void*>(wt_coverage_main_wtrunner), &binaryInfo) == 0 ||
			binaryInfo.dli_fname == nullptr)
		{
			std::filesystem::remove_all(directory);
			return result;
		}
		const std::filesystem::path pluginSource =
			std::filesystem::path(binaryInfo.dli_fname).parent_path().parent_path() /
			DLLHelper::wrap_module("WtDataStorage");
		std::error_code copyError;
		std::filesystem::copy_file(pluginSource,
			std::filesystem::path(directory) / DLLHelper::wrap_module("WtDataStorage"),
			std::filesystem::copy_options::overwrite_existing, copyError);
		if (copyError)
		{
			std::filesystem::remove_all(directory);
			return result;
		}

		std::ofstream sessions(std::string(directory) + "/sessions.json");
		sessions << R"JSON({"SD0930":{"name":"r6","offset":0,"sections":[{"from":930,"to":1500}]}})JSON";
		sessions.close();
		std::ofstream policy(std::string(directory) + "/actpolicy.yaml");
		policy << "default:\n    order: []\n";
		policy.close();
		std::filesystem::create_directory(std::string(directory) + "/data");
		// 迭代03（I03-NEW-008 / 卡13d 费用门禁）：配置了的费用模板必须存在；本例验证 main
		// 到达 run()，改用有效空模板（原 ./absent_fees.json，.before 见 s5-runners 证据）。
		std::ofstream fees(std::string(directory) + "/fees.json");
		fees << "{}";
		fees.close();
		std::ofstream config(std::string(directory) + "/config.yaml");
		config << "basefiles:\n"
			"    session: ./sessions.json\n"
			"env:\n"
			"    name: cta\n"
			"    fees: ./fees.json\n"
			"    filters: ./absent_filters.yaml\n"
			"    poolsize: 0\n"
			"    product:\n"
			"        session: SD0930\n"
			"data:\n"
			"    store:\n"
			"        module: WtDataStorage\n"
			"        path: ./data\n"
			"bspolicy: ./actpolicy.yaml\n";
		config.close();
		if (!sessions || !policy || !config)
		{
			std::filesystem::remove_all(directory);
			return result;
		}
	}

	const pid_t child = fork();
	if (child == 0)
	{
		alarm(5);
		if (chdir(directory) != 0)
			_exit(120);
		FILE* output = std::freopen(outputPath.c_str(), "w", stdout);
		if (output == nullptr || std::freopen(outputPath.c_str(), "a", stderr) == nullptr)
			_exit(121);

		std::vector<char*> argv;
		argv.reserve(words.size());
		for (const std::string& word : words)
			argv.push_back(const_cast<char*>(word.c_str()));
		const int code = wt_coverage_main_wtrunner(static_cast<int>(argv.size()), argv.data());
		fflush(nullptr);
		_exit(code);
	}

	if (child > 0)
	{
		int status = 0;
		pid_t waited = 0;
		if (use_success_config)
		{
			// run(false) is expected to block. The child alarm asks the runner to stop
			// gracefully; this independent deadline still reaps it if signal handlers
			// change or runner shutdown hangs.
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
			for (;;)
			{
				waited = waitpid(child, &status, WNOHANG);
				if (waited == child)
					break;
				if (waited < 0 && errno != EINTR)
					break;
				if (std::chrono::steady_clock::now() >= deadline)
				{
					result.parent_watchdog_timed_out = true;
					(void)kill(child, SIGKILL);
					do
					{
						waited = waitpid(child, &status, 0);
					} while (waited < 0 && errno == EINTR);
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
		}
		else
		{
			waited = waitpid(child, &status, 0);
		}
		if (waited == child)
		{
			result.exited = WIFEXITED(status);
			result.code = result.exited ? WEXITSTATUS(status) : -1;
			result.signal = WIFSIGNALED(status) ? WTERMSIG(status) : -1;
		}
	}
	std::ifstream input(outputPath);
	result.output.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
	if (use_success_config)
	{
		std::error_code ec;
		result.engine_marker_created = std::filesystem::is_regular_file(
			std::string(directory) + "/generated/marker.json", ec) && !ec;
	}
	std::filesystem::remove_all(directory);
	return result;
}
#endif

} // namespace

/* -------------------------------------------------------------------------
 * 一、入口 --help 分支（main.cpp:51~87）
 *
 * R28 更正（见 manifests/R28_runner_mains.md 的诊断一节）：本族的 -h/--help
 * 断言原来写成 EXPECT_EQ(call_entry(...), 0)，实测判决是 **NORUN 而不是 PASS**。
 * 原因是 cppcli 的 help 处理在 src/Share/cppcli.hpp:1897-1900 里
 * `std::cout << buildHelpDoc(); std::exit(0);` —— 它在 opt.parse() 内部就把
 * 整个宿主进程收尾了，main.cpp:86-87 的 `return 0` 是永远走不到的死代码。
 * 所以这三条改成 DISABLED_（help 分支的真实覆盖由 r28_runner_main_test.cpp
 * 用 fork 子进程的方式钉住），下面三条保留作为「为什么不能在本进程里调」的记录。
 * ---------------------------------------------------------------------- */

TEST(r6_wtrunner_entry, DISABLED_short_help_returns_zero_before_touching_runner)
{
	// main.cpp:86 if(hParam->exists()) return 0;  —— 实际到不了，cppcli 已 exit(0)
	EXPECT_EQ(call_entry({"wt_runner", "-h"}), 0);
}

TEST(r6_wtrunner_entry, DISABLED_long_help_returns_zero)
{
	EXPECT_EQ(call_entry({"wt_runner", "--help"}), 0);
}

TEST(r6_wtrunner_entry, DISABLED_help_wins_over_config_and_logcfg_words)
{
	// -h 与 -c/-l 同时给出：parse() 后 hParam->exists() 仍为真，早退先于任何文件访问
	EXPECT_EQ(call_entry({"wt_runner", "-c", "./nonexistent_r6.yaml",
		"-l", "./nonexistent_log_r6.yaml", "-h"}), 0);
}

/* -------------------------------------------------------------------------
 * 二、WtRunner.cpp:26~37 getBinDir()
 * ---------------------------------------------------------------------- */

TEST(r6_wtrunner_binroot, getBinDir_is_cwd_and_stable_across_calls)
{
	const char* first = getBinDir();
	ASSERT_NE(first, nullptr);
	// 第二次调用走 if(basePath.empty()) 的假分支，返回同一块内存
	const char* second = getBinDir();
	EXPECT_EQ(first, second) << "getBinDir() 的 static 缓存每次都重新算路径";
	EXPECT_GT(std::string(first).size(), 1u) << "初始工作目录不该是空串或单字符";
}

/* -------------------------------------------------------------------------
 * 三、WtRunner.cpp:41~65 构造/析构 + init()
 *
 * R28 更正：下面四条原来是 `WtRunner runner;`（测试栈帧上构造），实测一律
 * **SIGKILL(9)** —— 不是原注释说的「空指针段错误」，进程在构造/析构阶段就被打死，
 * profraw 归零，WtRunner.cpp 一行都记不上。同一类型改成堆上 new 出来
 * ctor/dtor 全程正常（实测对照见 manifests/R28_runner_mains.md），
 * 所以这里统一换成 unique_ptr 持有（BUG-R28-001）。
 * ---------------------------------------------------------------------- */

TEST(r6_wtrunner_init, ctor_dtor_roundtrip_routes_declared_levels_only)
{
	std::unique_ptr<WtRunner> runner(new WtRunner());
	// WtRunner.cpp:626 取用式是 LOG_TAGS[ll-100]；Includes/WTSTypes.h:128 LL_ALL=100..LL_NONE=106
	for (uint32_t ll = LL_ALL; ll <= LL_NONE; ll++)
	{
		const uint32_t idx = ll - 100u;
		ASSERT_LT(idx, 7u) << "级别枚举与 LOG_TAGS 长度不同步: ll=" << ll;
		// 未 init 通知器时 _mq_sid==0，notify_log 立刻返回（WtCore/EventNotifier.cpp:116）
		EXPECT_NO_THROW(runner->handleLogAppend(static_cast<WTSLogLevel>(ll), "r6 probe"))
			<< "ll=" << ll;
	}
}

TEST(r6_wtrunner_init, init_with_missing_logcfg_returns_and_stamps_inst_dir)
{
	std::unique_ptr<WtRunner> runner(new WtRunner());
	WtHelper::setInstDir("");
	// WtRunner.cpp:57 WTSLogger::init(不存在的路径) -> 内部告警；:59 setInstDir；:61 warn
	EXPECT_NO_THROW(runner->init("./r6_missing_logcfg.yaml"));
	// :59 的 setInstDir 是无条件执行的，能反证 init() 没在中途崩
	EXPECT_FALSE(WtHelper::getInstDir().empty());
}

/* -------------------------------------------------------------------------
 * 四、WtRunner.cpp:67~74 config() 加载失败早退
 * ---------------------------------------------------------------------- */

TEST(r6_wtrunner_config, missing_file_returns_false)
{
	std::unique_ptr<WtRunner> runner(new WtRunner());
	EXPECT_FALSE(runner->config("./r6_definitely_absent_cfg.yaml"));
}

TEST(r6_wtrunner_config, log_tag_table_matches_the_seven_declared_levels)
{
	// WtRunner.cpp:614-622 与 WtPorter 侧同名表内容一致，但各 TU 各定义一份
	EXPECT_STREQ(LOG_TAGS[0], "all");
	EXPECT_STREQ(LOG_TAGS[1], "debug");
	EXPECT_STREQ(LOG_TAGS[4], "error");
	EXPECT_STREQ(LOG_TAGS[6], "none");
}

TEST(r6_wtrunner_config, config_error_path_leaves_object_reusable)
{
	std::unique_ptr<WtRunner> runner(new WtRunner());
	ASSERT_FALSE(runner->config("./r6_definitely_absent_cfg.yaml"));
	// 第二次加载不同路径仍走同一条早退支，不因为第一次失败而改变行为
	EXPECT_FALSE(runner->config("./r6_another_absent_cfg.yaml"));
}

/* Historical disabled case for the separate required-basefiles configuration path. */
TEST(r6_wtrunner_config, DISABLED_empty_object_config_derefs_null_basefiles)
{
	WtRunner runner;
	EXPECT_FALSE(runner.config("./r6_empty_cfg.yaml"));
}

#ifndef _WIN32
TEST(r6_wtrunner_entry, missing_default_config_exits_nonzero_without_starting_runner)
{
	const ChildResult result = call_entry_in_child({"wt_runner"});
	ASSERT_TRUE(result.exited) << "signal=" << result.signal << " output=" << result.output;
	EXPECT_EQ(result.code, 1) << result.output;
	EXPECT_NE(result.output.find("Loading config file ./config.yaml failed"), std::string::npos) << result.output;
}

TEST(r6_wtrunner_entry, missing_explicit_config_exits_nonzero_without_starting_runner)
{
	const ChildResult result = call_entry_in_child({"wt_runner", "-c", "./missing-config.yaml"});
	ASSERT_TRUE(result.exited) << "signal=" << result.signal << " output=" << result.output;
	EXPECT_EQ(result.code, 1) << result.output;
	EXPECT_NE(result.output.find("Loading config file ./missing-config.yaml failed"), std::string::npos) << result.output;
}

TEST(r6_wtrunner_entry, valid_config_reaches_engine_run_before_main_blocks)
{
	const ChildResult result = call_entry_in_child(
		{"wt_runner", "-l", "./logcfg.yaml", "-c", "./config.yaml"}, true);
	ASSERT_FALSE(result.parent_watchdog_timed_out) << result.output;
	ASSERT_TRUE(result.exited) << "the watchdog should let the synchronous runner stop; signal="
		<< result.signal << " output=" << result.output;
	EXPECT_EQ(result.code, 0) << result.output;
	EXPECT_TRUE(result.engine_marker_created)
		<< "the CTA engine marker is written only after main passes config() and calls run(); output="
		<< result.output;
}
#endif
