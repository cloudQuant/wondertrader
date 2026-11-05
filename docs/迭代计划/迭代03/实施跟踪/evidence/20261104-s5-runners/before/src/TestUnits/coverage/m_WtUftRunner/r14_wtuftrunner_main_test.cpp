/*
 * R14 lane —— src/WtUftRunner/（可执行体 + WtUftRunner 类，WtUftRunner.cpp 整文件 0.00%）
 *
 * 形状照抄 m_LoaderRunner/seam_loader_runner_test.cpp 与 m_WtRunner/r6_wtrunner_main_test.cpp：
 * coverage/CMakeLists.txt 的 WT_MAIN_MODULES 循环对本模块目录下每个 .cpp（main.cpp + WtUftRunner.cpp）
 * 追加 -Dmain=wt_coverage_main_wtuftrunner（string(TOLOWER) 后是全小写，写成大写会链接期未定义），
 * 于是 main.cpp:31 的入口符号改名后与本文件的 gtest_main 同链一个二进制。
 *
 * 安全口径（本轮硬规则）：
 *  - main.cpp 的帮助分支在构造 WtUftRunner 前返回；配置失败分支现在验证非零退出，且不进入 run()。
 *  - 有效配置会进入 WtUftRunner.cpp 的引擎、共享内存和等待循环；这些依赖真实环境，仍不启用。
 *  - WtUftRunner.cpp 侧只走「不依赖引擎 run 循环」的成员函数：构造/析构/init/config 的两条早退支。
 *  - 禁止起线程：WtUftRunner 构造里的 install_signal_hooks(WtUftRunner.cpp:40~45) 经实测
 *    （src/WTSUtils/SignalHook.hpp 全文）只做 sigaction/signal 注册，不含 std::thread/pthread_create，
 *    所以构造本身不起线程，只在真收到信号时才执行回调 —— 用例不发信号。
 *
 * 期望值一律由本用例自己喂进去的 argv / 自己写的 yaml / 自己取的工作目录算出，不读挂钟。
 */
#include <gtest/gtest.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>
#include <fstream>

#include "WtUftRunner/WtUftRunner.h"
/* WtUftRunner.cpp:57 用的是 WtUftCore 这份 WtHelper（与 WtDtCore/WtRunner 侧同名不同实现，见缺陷登记） */
#include "WtUftCore/WtHelper.h"
#include "WTSTools/WTSLogger.h"

/* 生产 TU（main.cpp:31）经 -Dmain=... 改名后的入口 */
int wt_coverage_main_wtuftrunner(int argc, char* argv[]);

/* 生产 TU（WtUftRunner.cpp:22）的自由函数，未在 WtUftRunner.h 里声明，按定义处重新声明 */
const char* getBinDir();

/* 生产 TU（WtUftRunner.cpp:375）的级别标签表；handleLogAppend(:385) 是空实现，全文件无人取用 */
extern const char* LOG_TAGS[];

namespace {

const char* kAbsentLogCfg = "./r14_uft_absent_logcfg.yaml";
const char* kAbsentCfg = "./r14_uft_absent_config.yaml";
const char* kMissingBasefilesCfg = "./r14_uft_missing_basefiles.yaml";
const char* kNonObjectBasefilesCfg = "./r14_uft_non_object_basefiles.yaml";
/* 本用例自己写的配置：只放「能让 config() 逐段早退」的键，不放 env/data/share_domain/parsers/traders */
const char* kSelfAuthoredCfg = "./r14_uft_min_config.yaml";

class R14LogCapture : public ILogHandler
{
public:
	std::string messages;

	void handleLogAppend(WTSLogLevel, const char* message) override
	{
		if (message != nullptr)
			messages.append(message).push_back('\n');
	}
};

class R14ScopedLogHandler
{
public:
	explicit R14ScopedLogHandler(ILogHandler* handler) { WTSLogger::registerHandler(handler); }
	~R14ScopedLogHandler() { WTSLogger::registerHandler(nullptr); }

	R14ScopedLogHandler(const R14ScopedLogHandler&) = delete;
	R14ScopedLogHandler& operator=(const R14ScopedLogHandler&) = delete;
};

int call_entry(std::vector<const char*> words)
{
	std::vector<char*> argv;
	argv.reserve(words.size());
	for (const char* w : words)
		argv.push_back(const_cast<char*>(w));
	return wt_coverage_main_wtuftrunner(static_cast<int>(argv.size()), argv.data());
}

void call_help_entry_in_child(std::vector<const char*> words)
{
	// cppcli::Option::parse() writes help to stdout and calls std::exit(0).
	// Redirect it to stderr so the GTest death-test matcher can verify the help text.
	std::cout.rdbuf(std::cerr.rdbuf());
	(void)call_entry(words);
}

/* 写出一个由用例完全掌控的 yaml；.yaml 后缀是 WTSCfgLoader.cpp:324 判定走 yaml 解析的前提 */
void write_min_config()
{
	std::ofstream out(kSelfAuthoredCfg, std::ios::binary | std::ios::trunc);
	out << "basefiles:\n"
		<< "    placeholder: r14\n"
		<< "bspolicy: ./r14_uft_absent_actpolicy.yaml\n";
	out.flush();
	out.close();
}

void write_missing_basefiles_config()
{
	std::ofstream out(kMissingBasefilesCfg, std::ios::binary | std::ios::trunc);
	out << "env: {}\n";
}

void initialize_error_log_capture(R14LogCapture& capture)
{
	WTSLogger::init(R"({"root":{"level":"error","async":false,"sinks":[]}})", false);
	WTSLogger::registerHandler(&capture);
}

} // namespace

/* -------------------------------------------------------------------------
 * 一、入口 --help 分支（main.cpp:39~54）
 * ---------------------------------------------------------------------- */

TEST(r14_uft_entry, short_help_exits_zero_before_constructing_runner)
{
	// Help exits while parsing, before main.cpp reaches the WtUftRunner construction.
	EXPECT_EXIT(call_help_entry_in_child({"wt_uft_runner", "-h"}),
		::testing::ExitedWithCode(0), "options:");
}

TEST(r14_uft_entry, long_help_exits_zero_and_prints_help)
{
	EXPECT_EXIT(call_help_entry_in_child({"wt_uft_runner", "--help"}),
		::testing::ExitedWithCode(0), "options:");
}

TEST(r14_uft_entry, help_wins_over_config_and_logcfg_words)
{
	// -h 与 -c/-l 同时给出时，仍应输出帮助并在进入日志/配置加载前以 0 退出。
	EXPECT_EXIT(call_help_entry_in_child(
		{"wt_uft_runner", "-c", kAbsentCfg, "-l", kAbsentLogCfg, "-h"}),
		::testing::ExitedWithCode(0), "options:");
}

/* -------------------------------------------------------------------------
 * 二、WtUftRunner.cpp:22~33 getBinDir()
 * ---------------------------------------------------------------------- */

TEST(r14_uft_binroot, static_cache_returns_same_pointer_twice)
{
	const char* first = getBinDir();
	ASSERT_NE(first, nullptr);
	// :25 if(basePath.empty()) 第二次走假分支
	EXPECT_EQ(first, getBinDir()) << "getBinDir() 的 static 缓存每次都重算了路径";
}

TEST(r14_uft_binroot, value_is_the_cwd_with_at_most_one_trailing_slash)
{
	char cwd[4096] = {0};
	ASSERT_NE(getcwd(cwd, sizeof(cwd)), nullptr);
	const std::string base(cwd);
	const std::string actual(getBinDir());

	// :27 initial_path -> :29 StrUtil::standardisePath 只规范化尾部分隔符
	const bool exact = (actual == base);
	const bool slashed = (actual == base + "/");
	EXPECT_TRUE(exact || slashed) << "getBinDir()=" << actual << " 与 cwd=" << base << " 对不上";
	EXPECT_FALSE(exact && slashed) << "cwd 本身以 / 结尾时两种形态不可能同时成立";
}

/* -------------------------------------------------------------------------
 * 三、WtUftRunner.cpp:37~58 构造/析构 + init()
 * ---------------------------------------------------------------------- */

TEST(r14_uft_init, ctor_dtor_roundtrip_registers_hooks_without_spawning_threads)
{
	EXPECT_NO_THROW({ WtUftRunner runner; (void)runner; });
	// 第二次构造走同一条 install_signal_hooks，验证可重入（:40~45 只做 sigaction）
	EXPECT_NO_THROW({ WtUftRunner runner; (void)runner; });
}

TEST(r14_uft_init, init_with_missing_logcfg_stamps_inst_dir_with_binroot)
{
	WtUftRunner runner;
	WtHelper::setInstDir("");
	// :55 WTSLogger::init(不存在的路径) 内部告警后返回；:57 setInstDir(getBinDir())
	EXPECT_NO_THROW(runner.init(kAbsentLogCfg));

	// :57 是无条件执行的，因此 getInstDir() 必须与同一 TU 的 getBinDir() 逐字相等
	const std::string stamped = WtHelper::getInstDir();
	const std::string expect(getBinDir());
	EXPECT_EQ(stamped, expect) << "init() 没有把安装目录写成 getBinDir() 的值";
	EXPECT_FALSE(stamped.empty());
}

/* -------------------------------------------------------------------------
 * 四、WtUftRunner.cpp:60~197 config() 的早退分支
 * ---------------------------------------------------------------------- */

TEST(r14_uft_config, missing_file_returns_false)
{
	WtUftRunner runner;
	// :62 load_from_file -> WTSCfgLoader.cpp:300 文件不存在 -> NULL -> :65 记日志 -> :66 return false
	EXPECT_FALSE(runner.config(kAbsentCfg));
}

TEST(r14_uft_config, authored_config_without_basefiles_children_walks_the_early_exits)
{
	write_min_config();
	ASSERT_TRUE(StdFile::exists(kSelfAuthoredCfg));

	WtUftRunner runner;
	// basefiles 存在，但 env/data 配置缺失；配置阶段必须返回失败。
	EXPECT_FALSE(runner.config(kSelfAuthoredCfg));
}

TEST(r14_uft_config, missing_basefiles_is_reported_and_rejected_before_run)
{
	write_missing_basefiles_config();
	ASSERT_TRUE(StdFile::exists(kMissingBasefilesCfg));

	R14LogCapture capture;
	initialize_error_log_capture(capture);
	R14ScopedLogHandler logHandler(&capture);
	WtUftRunner runner;
	EXPECT_FALSE(runner.config(kMissingBasefilesCfg));
	EXPECT_NO_THROW(runner.run());
	EXPECT_NE(std::string::npos, capture.messages.find("UFT basefiles configuration is missing"));
	EXPECT_NE(std::string::npos, capture.messages.find("UFT runner cannot run without a successful"));
}

TEST(r14_uft_config, non_object_basefiles_is_reported_and_rejected)
{
	{
		std::ofstream out(kNonObjectBasefilesCfg, std::ios::binary | std::ios::trunc);
		out << "basefiles: invalid\n"
			<< "env: {}\n";
	}
	ASSERT_TRUE(StdFile::exists(kNonObjectBasefilesCfg));

	R14LogCapture capture;
	initialize_error_log_capture(capture);
	R14ScopedLogHandler logHandler(&capture);
	WtUftRunner runner;
	EXPECT_FALSE(runner.config(kNonObjectBasefilesCfg));
	EXPECT_NE(std::string::npos, capture.messages.find("UFT basefiles configuration must be an object"));
}

TEST(r14_uft_config, object_basefiles_passes_validation_and_reaches_env_check)
{
	write_min_config();
	ASSERT_TRUE(StdFile::exists(kSelfAuthoredCfg));

	R14LogCapture capture;
	initialize_error_log_capture(capture);
	R14ScopedLogHandler logHandler(&capture);
	WtUftRunner runner;
	EXPECT_FALSE(runner.config(kSelfAuthoredCfg));
	EXPECT_NE(std::string::npos, capture.messages.find("UFT environment configuration is missing"));
	EXPECT_EQ(std::string::npos, capture.messages.find("UFT basefiles configuration"));
}

TEST(r14_uft_config, missing_then_authored_configs_use_separate_runner_instances)
{
	WtUftRunner missingRunner;
	// 缺文件失败后，再验证缺少必要 env/data 的配置也应失败。
	EXPECT_FALSE(missingRunner.config(kAbsentCfg));
	write_min_config();
	WtUftRunner authoredRunner;
	EXPECT_FALSE(authoredRunner.config(kSelfAuthoredCfg));
}

/* -------------------------------------------------------------------------
 * 五、WtUftRunner.cpp:375~387 LOG_TAGS 表与空实现 handleLogAppend
 * ---------------------------------------------------------------------- */

TEST(r14_uft_logtags, table_matches_the_seven_declared_levels)
{
	// WtUftRunner.cpp:375~383 的字面量顺序，与 Includes/WTSTypes.h 的 LL_ALL=100..LL_NONE=106 对齐
	EXPECT_STREQ(LOG_TAGS[0], "all");
	EXPECT_STREQ(LOG_TAGS[1], "debug");
	EXPECT_STREQ(LOG_TAGS[2], "info");
	EXPECT_STREQ(LOG_TAGS[3], "warn");
	EXPECT_STREQ(LOG_TAGS[4], "error");
	EXPECT_STREQ(LOG_TAGS[5], "fatal");
	EXPECT_STREQ(LOG_TAGS[6], "none");
}

TEST(r14_uft_loghandler, handleLogAppend_is_a_noop_for_every_declared_level)
{
	WtUftRunner runner;
	// :385~387 空函数体，逐个级别调用都应正常返回
	for (uint32_t ll = LL_ALL; ll <= LL_NONE; ll++)
	{
		EXPECT_NO_THROW(runner.handleLogAppend(static_cast<WTSLogLevel>(ll), "r14 probe"))
			<< "ll=" << ll;
	}
}

TEST(r14_uft_entry, config_failure_returns_nonzero_without_running_engine)
{
	write_min_config();
	EXPECT_NE(0, call_entry({"wt_uft_runner", "-c", kSelfAuthoredCfg, "-l", kAbsentLogCfg}));
}

TEST(r14_uft_entry, missing_basefiles_returns_nonzero_without_running_engine)
{
	write_missing_basefiles_config();
	ASSERT_TRUE(StdFile::exists(kMissingBasefilesCfg));
	EXPECT_EXIT(std::exit(call_entry({"wt_uft_runner", "-c", kMissingBasefilesCfg, "-l", kAbsentLogCfg})),
		::testing::ExitedWithCode(1), "");
}

/* -------------------------------------------------------------------------
 * 六、DISABLED_：需要真实引擎/共享内存/主循环的分支
 *
 * 触发条件与不启用理由：
 *  D1 config() 全量成功分支：
 *     :118~121 ShareManager::initialize/init_domain 需要真实 shm 段；
 *     :302/:327 ParserAdapter/TraderAdapter::init 会 dlopen 厂商库并起接收线程。
 *     可回收行数估计：WtUftRunner.cpp 的 :69~107 剩余 + :109~197 + :200~251 + :253~278
 *     + :280~338 + :340~349 + :351~373 ≈ 210 行（占该文件 285 行的绝大部分）。
 *  D2 入口无参分支（argc==1）：走默认 ./logcfg.yaml 与 ./config.yaml，
 *     是否进入 D1 取决于运行目录内容，期望值无法由本用例输入算出 -> 禁用。
 * 另注：WtUftCore/WtUftEngine.cpp:209~213 的 return NULL 之后约 30 行是已登记的不可达段，本轮不追。
 * ---------------------------------------------------------------------- */

TEST(r14_uft_runner, DISABLED_run_false_loops_until_signal)
{
	// D1：_uft_engine.run() + start_watching(2) + 10ms 轮询死循环
	WtUftRunner runner;
	runner.run(false);
	EXPECT_TRUE(true);
}

TEST(r14_uft_config, DISABLED_full_engine_config_needs_shm_and_vendor_libs)
{
	// D2：env/data/share_domain/parsers/traders 全量配置，需要真实 shm 与柜台
	WtUftRunner runner;
	EXPECT_TRUE(runner.config("./config.yaml"));
}
