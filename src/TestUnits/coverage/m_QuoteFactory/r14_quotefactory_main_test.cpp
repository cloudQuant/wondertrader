/*
 * R14 lane —— src/QuoteFactory/main.cpp（可执行体，整文件 0.00%）
 *
 * 形状照抄 m_LoaderRunner/seam_loader_runner_test.cpp：
 * coverage/CMakeLists.txt 的 WT_MAIN_MODULES 循环对本模块目录下每个 .cpp 追加
 * -Dmain=wt_coverage_main_quotefactory（全小写，实测大写会在链接期报未定义符号），
 * 于是 main.cpp:264 的入口符号改名后与本文件的 gtest_main 同链一个二进制。
 *
 * 安全口径（本轮硬规则）：
 *  - 只调用能自行终止、不起线程、不阻塞的路径；
 *  - main.cpp 里可启用的分支只有：
 *      B1 :271~276  -h/--help -> hParam->exists() -> return 0（在 WTSLogger::init 之前）
 *      B2 :278~283  日志配置分支（缺文件 -> WTSLogger::init 内部告警后返回）
 *      B3 :299~308  -c 指向不存在的配置 -> StdFile::exists 为假 -> 诊断 + return 1
 *    默认路径和显式路径均在隔离的空临时目录中检查，避免意外加载仓库内配置后
 *    在 :254 g_parsers.run() 起解析线程、:260 g_stateMon.run() 起状态机线程，
 *    并进入末尾 :324 的永久轮询 —— 有效运行配置仍只登记为 DISABLED_；
 *    缺失或错误类型的 basefiles 由入口用例验证受控失败，且不会进入该路径。
 *  - 自由函数 getBinDir()（:50~61）不依赖任何运行期服务，可直接断言。
 *
 * 期望值一律由本用例自己喂进去的 argv / 自己取的工作目录算出，不读挂钟。
 */
#include <gtest/gtest.h>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

/* initialize() 里 :98 用的是 WtDtCore 这份 WtHelper（与 WtUftCore/WtCore 侧同名不同实现） */
#include "WtDtCore/WtHelper.h"

/* 生产 TU（QuoteFactory/main.cpp:264）经 -Dmain=... 改名后的入口 */
int wt_coverage_main_quotefactory(int argc, char* argv[]);

/* 生产 TU（QuoteFactory/main.cpp:50）的自由函数，未在头文件里声明，按定义处重新声明 */
const char* getBinDir();

/* 生产 TU（QuoteFactory/main.cpp:96）的自由函数：配置缺失时在 :104 return，不碰任何 caster */
bool initialize(const std::string& filename);

namespace {

const char* kAbsentDtCfg = "./r14_qf_definitely_absent_dtcfg.yaml";
const char* kConsoleLogCfg = R"YAML(root:
    async: false
    level: error
    sinks:
    -   type: console_sink
        pattern: '%v'
)YAML";

struct ChildResult
{
	bool exited = false;
	int code = -1;
	int signal = -1;
	std::string output;
};

class TempDirectory
{
public:
	TempDirectory()
	{
		const std::string pattern = (std::filesystem::temp_directory_path() / "r14-qf-XXXXXX").string();
		std::vector<char> writable(pattern.begin(), pattern.end());
		writable.push_back('\0');
		char* created = mkdtemp(writable.data());
		if (created != nullptr)
			_path = created;
	}

	~TempDirectory()
	{
		if (!_path.empty())
			std::filesystem::remove_all(_path);
	}

	const std::string& path() const { return _path; }

private:
	std::string _path;
};

bool write_text_file(const std::string& path, const std::string& contents)
{
	std::ofstream output(path);
	output << contents;
	return output.good();
}

ChildResult call_entry_in_child(const std::vector<std::string>& words, const std::string& cwd)
{
	ChildResult result;
	int fds[2];
	if (pipe(fds) != 0)
		return result;

	const pid_t pid = fork();
	if (pid < 0)
	{
		close(fds[0]);
		close(fds[1]);
		return result;
	}

	if (pid == 0)
	{
		close(fds[0]);
		dup2(fds[1], STDOUT_FILENO);
		dup2(fds[1], STDERR_FILENO);
		close(fds[1]);
		if (!cwd.empty() && chdir(cwd.c_str()) != 0)
			_exit(120);
		// If a regression lets a valid config reach main's permanent wait loop,
		// bound the child so the test reports a failure instead of hanging.
		alarm(3);

		std::vector<char*> argv;
		argv.reserve(words.size());
		for (const std::string& word : words)
			argv.push_back(const_cast<char*>(word.c_str()));
		const int rc = wt_coverage_main_quotefactory(static_cast<int>(argv.size()), argv.data());
		fflush(nullptr);
		_exit(rc);
	}

	close(fds[1]);
	char buffer[2048];
	ssize_t count = 0;
	while ((count = read(fds[0], buffer, sizeof(buffer))) > 0)
		result.output.append(buffer, static_cast<size_t>(count));
	close(fds[0]);

	int status = 0;
	if (waitpid(pid, &status, 0) >= 0)
	{
		result.exited = WIFEXITED(status);
		result.code = result.exited ? WEXITSTATUS(status) : -1;
		result.signal = WIFSIGNALED(status) ? WTERMSIG(status) : -1;
	}
	return result;
}

ChildResult call_initialize_in_child(const std::string& config_path)
{
	ChildResult result;
	int fds[2];
	if (pipe(fds) != 0)
		return result;

	const pid_t pid = fork();
	if (pid < 0)
	{
		close(fds[0]);
		close(fds[1]);
		return result;
	}

	if (pid == 0)
	{
		close(fds[0]);
		dup2(fds[1], STDOUT_FILENO);
		close(fds[1]);
		alarm(3);
		const bool initialized = initialize(config_path);
		const char* outcome = initialized ? "initialize=true\n" : "initialize=false\n";
		(void)write(STDOUT_FILENO, outcome, std::strlen(outcome));
		_exit(initialized ? 0 : 1);
	}

	close(fds[1]);
	char buffer[2048];
	ssize_t count = 0;
	while ((count = read(fds[0], buffer, sizeof(buffer))) > 0)
		result.output.append(buffer, static_cast<size_t>(count));
	close(fds[0]);

	int status = 0;
	if (waitpid(pid, &status, 0) >= 0)
	{
		result.exited = WIFEXITED(status);
		result.code = result.exited ? WEXITSTATUS(status) : -1;
		result.signal = WIFSIGNALED(status) ? WTERMSIG(status) : -1;
	}
	return result;
}

/* 用受控 argv 调入口；argv[0] 只是程序名，cppcli 不校验它 */
int call_entry(std::vector<const char*> words)
{
	std::vector<char*> argv;
	argv.reserve(words.size());
	for (const char* w : words)
		argv.push_back(const_cast<char*>(w));
	return wt_coverage_main_quotefactory(static_cast<int>(argv.size()), argv.data());
}

} // namespace

/* -------------------------------------------------------------------------
 * 一、B1 —— help 分支（main.cpp:266~276）
 * ---------------------------------------------------------------------- */

TEST(r14_quote_entry, short_help_returns_zero_before_any_logging_init)
{
	TempDirectory cwd;
	ASSERT_FALSE(cwd.path().empty());
	const ChildResult result = call_entry_in_child({"quote_factory", "-h"}, cwd.path());
	ASSERT_TRUE(result.exited) << "signal=" << result.signal << " output=" << result.output;
	EXPECT_EQ(result.code, 0);
	EXPECT_NE(result.output.find("-c | --config"), std::string::npos) << result.output;
}

TEST(r14_quote_entry, long_help_returns_zero)
{
	TempDirectory cwd;
	ASSERT_FALSE(cwd.path().empty());
	const ChildResult result = call_entry_in_child({"quote_factory", "--help"}, cwd.path());
	ASSERT_TRUE(result.exited) << "signal=" << result.signal << " output=" << result.output;
	EXPECT_EQ(result.code, 0);
	EXPECT_NE(result.output.find("gain help doc"), std::string::npos) << result.output;
}

TEST(r14_quote_entry, help_wins_over_config_and_logcfg_words)
{
	TempDirectory cwd;
	ASSERT_FALSE(cwd.path().empty());
	const std::string absent_config = cwd.path() + "/absent.yaml";
	const std::string absent_log = cwd.path() + "/absent-log.yaml";
	const ChildResult result = call_entry_in_child(
		{"quote_factory", "-c", absent_config, "-l", absent_log, "-h"}, cwd.path());
	ASSERT_TRUE(result.exited) << "signal=" << result.signal << " output=" << result.output;
	EXPECT_EQ(result.code, 0);
	EXPECT_EQ(result.output.find("does not exist"), std::string::npos) << result.output;
}

/* -------------------------------------------------------------------------
 * 二、B2+B3 —— 日志初始化后配置缺失早退（main.cpp:278~308）
 * ---------------------------------------------------------------------- */

TEST(r14_quote_entry, missing_config_path_returns_nonzero_via_explicit_c)
{
	TempDirectory cwd;
	ASSERT_FALSE(cwd.path().empty());
	const std::string absent_config = cwd.path() + "/absent.yaml";
	const ChildResult result = call_entry_in_child({"quote_factory", "-c", absent_config}, cwd.path());
	ASSERT_TRUE(result.exited) << "signal=" << result.signal << " output=" << result.output;
	EXPECT_NE(result.code, 0);
	EXPECT_NE(result.output.find("Configuration file"), std::string::npos) << result.output;
	EXPECT_NE(result.output.find("does not exist"), std::string::npos) << result.output;
	EXPECT_NE(result.output.find(absent_config), std::string::npos) << result.output;
}

TEST(r14_quote_entry, both_c_and_l_given_still_returns_nonzero_when_files_absent)
{
	TempDirectory cwd;
	ASSERT_FALSE(cwd.path().empty());
	const std::string absent_config = cwd.path() + "/absent.yaml";
	const std::string absent_log = cwd.path() + "/absent-log.yaml";
	const ChildResult result = call_entry_in_child(
		{"quote_factory", "-c", absent_config, "-l", absent_log}, cwd.path());
	ASSERT_TRUE(result.exited) << "signal=" << result.signal << " output=" << result.output;
	EXPECT_NE(result.code, 0);
	EXPECT_NE(result.output.find("Configuration file"), std::string::npos) << result.output;
	EXPECT_NE(result.output.find(absent_config), std::string::npos) << result.output;
}

TEST(r14_quote_entry, missing_default_config_returns_nonzero_in_empty_directory)
{
	TempDirectory cwd;
	ASSERT_FALSE(cwd.path().empty());
	const ChildResult result = call_entry_in_child({"quote_factory"}, cwd.path());
	ASSERT_TRUE(result.exited) << "signal=" << result.signal << " output=" << result.output;
	EXPECT_NE(result.code, 0);
	EXPECT_NE(result.output.find("./dtcfg.yaml"), std::string::npos) << result.output;
	EXPECT_NE(result.output.find("does not exist"), std::string::npos) << result.output;
}

TEST(r14_quote_entry, valid_syntax_without_basefiles_fails_before_runner_start)
{
	TempDirectory cwd;
	ASSERT_FALSE(cwd.path().empty());
	const std::string config_path = cwd.path() + "/missing-basefiles.yaml";
	const std::string log_path = cwd.path() + "/console-log.yaml";
	ASSERT_TRUE(write_text_file(config_path, "allday: true\n"));
	ASSERT_TRUE(write_text_file(log_path, kConsoleLogCfg));

	const ChildResult result = call_entry_in_child(
		{"quote_factory", "-c", config_path, "-l", log_path}, cwd.path());
	ASSERT_TRUE(result.exited) << "signal=" << result.signal << " output=" << result.output;
	EXPECT_EQ(result.code, 1) << result.output;
	EXPECT_NE(result.output.find("QuoteFactory config requires basefiles to be an object"), std::string::npos)
		<< result.output;
	EXPECT_EQ(result.output.find("parsers started"), std::string::npos) << result.output;
	EXPECT_EQ(result.output.find("Exit flag is"), std::string::npos) << result.output;
}

TEST(r14_quote_entry, non_object_basefiles_fails_before_runner_start)
{
	TempDirectory cwd;
	ASSERT_FALSE(cwd.path().empty());
	const std::string config_path = cwd.path() + "/array-basefiles.yaml";
	const std::string log_path = cwd.path() + "/console-log.yaml";
	ASSERT_TRUE(write_text_file(config_path, "basefiles: []\nallday: true\n"));
	ASSERT_TRUE(write_text_file(log_path, kConsoleLogCfg));

	const ChildResult result = call_entry_in_child(
		{"quote_factory", "-c", config_path, "-l", log_path}, cwd.path());
	ASSERT_TRUE(result.exited) << "signal=" << result.signal << " output=" << result.output;
	EXPECT_EQ(result.code, 1) << result.output;
	EXPECT_NE(result.output.find("QuoteFactory config requires basefiles to be an object"), std::string::npos)
		<< result.output;
	EXPECT_EQ(result.output.find("parsers started"), std::string::npos) << result.output;
	EXPECT_EQ(result.output.find("Exit flag is"), std::string::npos) << result.output;
}

TEST(r14_quote_initialize, valid_basefiles_object_remains_accepted)
{
	TempDirectory cwd;
	ASSERT_FALSE(cwd.path().empty());
	const std::string config_path = cwd.path() + "/valid-basefiles.yaml";
	ASSERT_TRUE(write_text_file(config_path,
		"basefiles: {}\n"
		"allday: true\n"
		"writer:\n"
		"    module: R14NoSuchWriterModule\n"
		"parsers: []\n"));

	const ChildResult result = call_initialize_in_child(config_path);
	ASSERT_TRUE(result.exited) << "signal=" << result.signal << " output=" << result.output;
	EXPECT_EQ(result.code, 0) << result.output;
	EXPECT_NE(result.output.find("initialize=true"), std::string::npos) << result.output;
}

/* -------------------------------------------------------------------------
 * 三、getBinDir()（main.cpp:50~61）
 * ---------------------------------------------------------------------- */

TEST(r14_quote_binroot, static_cache_returns_same_pointer_twice)
{
	const char* first = getBinDir();
	ASSERT_NE(first, nullptr);
	const char* second = getBinDir();
	// :53 if(basePath.empty()) 第二次走假分支，返回同一块内存
	EXPECT_EQ(first, second) << "getBinDir() 的 static 缓存每次都重算了路径";
}

TEST(r14_quote_binroot, value_is_the_cwd_with_at_most_one_trailing_slash)
{
	char cwd[4096] = {0};
	ASSERT_NE(getcwd(cwd, sizeof(cwd)), nullptr);
	const std::string base(cwd);
	const std::string actual(getBinDir());

	// :55 initial_path -> :57 StrUtil::standardisePath 只补/去尾部分隔符，目录主体不变
	const std::string with_slash = base + "/";
	const bool exact = (actual == base);
	const bool slashed = (actual == with_slash);
	EXPECT_TRUE(exact || slashed) << "getBinDir()=" << actual << " 与 cwd=" << base << " 对不上";
	// 两种形态只可能有一种成立，反证 standardisePath 没有把路径改写（例如去掉 ./ 前缀之外再动刀）
	EXPECT_NE(exact && slashed, true);
}

/* -------------------------------------------------------------------------
 * 四、initialize() 的配置缺失早退（main.cpp:96~105）
 * ---------------------------------------------------------------------- */

TEST(r14_quote_initialize, missing_config_stamps_module_dir_then_returns)
{
	WtHelper::set_module_dir("");
	// :100 load_from_file(不存在的路径) -> WTSCfgLoader.cpp:300 exists() 为假 -> NULL
	// -> :103 记 error -> :104 return，后面的 basefiles/caster/stateMonotor/writer/parsers 全不执行
	bool initialized = true;
	EXPECT_NO_THROW(initialized = initialize(kAbsentDtCfg));
	EXPECT_FALSE(initialized);

	// :98 在早退之前无条件执行，因此可作为「确实进了 initialize()」的可证伪观测点
	const std::string expect(getBinDir());
	EXPECT_EQ(std::string(WtHelper::get_module_dir()), expect)
		<< "initialize() 没有把 module_dir 写成 getBinDir() 的值";

	// 第二次调用走 :53/:25 的假分支（缓存已建），结果必须与第一次一致
	initialize(kAbsentDtCfg);
	EXPECT_EQ(std::string(WtHelper::get_module_dir()), expect);
}

/* -------------------------------------------------------------------------
 * 五、DISABLED_：需要真实服务/线程的分支
 *
 * 触发条件与不启用理由：
 *  D1 main.cpp:304 为假分支的反面（配置存在）-> :310 initialize(filename)，
 *     若包含会启动的 parser，:254 g_parsers.run() / :260 g_stateMon.run() 会起线程，
 *     且 :324 while(!bExit) 每 10ms 轮询永不退出 -> 不适合作为普通用例启用。
 *     缺失/非对象 basefiles 已由上方入口测试验证受控返回；空对象正向对照单独调用
 *     initialize() 并使用空 parser 列表，不进入 main 的永久轮询。
 *  D2 默认路径的缺文件分支已在隔离的空目录中启用验证；只有运行目录存在
 *     可加载配置时才可能进入 D1，因此真实配置/线程路径仍禁用。
 *  D3 :313 install_signal_hooks 会注册 SIGINT/SIGTERM 处理器并可能起监听线程，
 *     违反「禁止起线程」，只能等主控裁定进程外采样。
 * ---------------------------------------------------------------------- */

TEST(r14_quote_entry, DISABLED_existing_config_enters_parsers_run_and_endless_loop)
{
	// 需要能加载真实 parser 的 dtcfg.yaml + shm 目录；可能启动线程并进入永久轮询
	EXPECT_EQ(call_entry({"quote_factory", "-c", "./dtcfg.yaml"}), 0);
}

TEST(r14_quote_entry, DISABLED_bare_argv_with_existing_config_enters_runner)
{
	// 需要真实可加载配置；可能启动解析/状态机线程并进入永久轮询
	EXPECT_EQ(call_entry({"quote_factory"}), 0);
}
