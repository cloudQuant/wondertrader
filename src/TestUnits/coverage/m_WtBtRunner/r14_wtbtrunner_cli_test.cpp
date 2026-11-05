#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef WT_COV_SEL_PLUGIN_PATH
#error "WT_COV_SEL_PLUGIN_PATH must point to the isolated SEL factory test plugin"
#endif

int wt_coverage_main_wtbtrunner(int argc, char* argv[]);

namespace
{
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
			const std::string pattern = (std::filesystem::temp_directory_path() / "r14-btr-XXXXXX").string();
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

	std::filesystem::path repository_root()
	{
		std::filesystem::path root(__FILE__);
		for (int i = 0; i < 5; ++i)
			root = root.parent_path();
		return root;
	}

	std::string json_quote(const std::string& value)
	{
		std::string result = "\"";
		for (char ch : value)
		{
			if (ch == '\\' || ch == '"')
				result.push_back('\\');
			result.push_back(ch);
		}
		result.push_back('"');
		return result;
	}

	bool write_text(const std::filesystem::path& path, const std::string& content)
	{
		std::ofstream output(path);
		if (!output)
			return false;
		output << content;
		return output.good();
	}

	ChildResult run_runner_in_child(const std::vector<std::string>& words, const std::string& cwd)
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
			alarm(10);
			close(fds[0]);
			dup2(fds[1], STDOUT_FILENO);
			dup2(fds[1], STDERR_FILENO);
			close(fds[1]);
			const int nullfd = open("/dev/null", O_RDONLY);
			if (nullfd >= 0)
			{
				dup2(nullfd, STDIN_FILENO);
				close(nullfd);
			}
			if (chdir(cwd.c_str()) != 0)
				_exit(120);

			std::vector<char*> argv;
			argv.reserve(words.size());
			for (const std::string& word : words)
				argv.push_back(const_cast<char*>(word.c_str()));
			const int rc = wt_coverage_main_wtbtrunner(static_cast<int>(argv.size()), argv.data());
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
}

TEST(r14_wtbtrunner_cli, short_help_exits_zero_in_child)
{
	TempDirectory cwd;
	ASSERT_FALSE(cwd.path().empty());
	const ChildResult result = run_runner_in_child({"wt_btrunner", "-h"}, cwd.path());
	ASSERT_TRUE(result.exited) << "signal=" << result.signal << " output=" << result.output;
	EXPECT_EQ(result.code, 0);
	EXPECT_NE(result.output.find("-c | --config"), std::string::npos) << result.output;
}

TEST(r14_wtbtrunner_cli, help_precedes_explicit_config_check)
{
	TempDirectory cwd;
	ASSERT_FALSE(cwd.path().empty());
	const std::string absent_config = cwd.path() + "/absent.yaml";
	const ChildResult result = run_runner_in_child(
		{"wt_btrunner", "-c", absent_config, "-h"}, cwd.path());
	ASSERT_TRUE(result.exited) << "signal=" << result.signal << " output=" << result.output;
	EXPECT_EQ(result.code, 0);
	EXPECT_EQ(result.output.find("---logger initialized---"), std::string::npos) << result.output;
	EXPECT_EQ(result.output.find("does not exist"), std::string::npos) << result.output;
}

TEST(r14_wtbtrunner_cli, explicit_missing_config_exits_nonzero_with_diagnostic)
{
	TempDirectory cwd;
	ASSERT_FALSE(cwd.path().empty());
	const std::string absent_config = cwd.path() + "/absent.yaml";
	const ChildResult result = run_runner_in_child(
		{"wt_btrunner", "-c", absent_config}, cwd.path());
	ASSERT_TRUE(result.exited) << "signal=" << result.signal << " output=" << result.output;
	EXPECT_NE(result.code, 0);
	EXPECT_NE(result.output.find("---logger initialized---"), std::string::npos) << result.output;
	EXPECT_NE(result.output.find("Configuration file"), std::string::npos) << result.output;
	EXPECT_NE(result.output.find("does not exist"), std::string::npos) << result.output;
	EXPECT_NE(result.output.find(absent_config), std::string::npos) << result.output;
}

TEST(r14_wtbtrunner_cli, missing_default_config_exits_nonzero_in_empty_directory)
{
	TempDirectory cwd;
	ASSERT_FALSE(cwd.path().empty());
	const ChildResult result = run_runner_in_child({"wt_btrunner"}, cwd.path());
	ASSERT_TRUE(result.exited) << "signal=" << result.signal << " output=" << result.output;
	EXPECT_NE(result.code, 0);
	EXPECT_NE(result.output.find("./configbt.yaml"), std::string::npos) << result.output;
	EXPECT_NE(result.output.find("does not exist"), std::string::npos) << result.output;
}

TEST(r14_wtbtrunner_cli, selTaskDefaultsAllowRunWhenTemplatesAreOmitted)
{
	TempDirectory cwd;
	ASSERT_FALSE(cwd.path().empty());
	const std::filesystem::path root = repository_root();
	const std::filesystem::path sessions = root / "dist/common/sessions.json";
	ASSERT_TRUE(std::filesystem::exists(sessions));
	// prepare() 以 checked 日期映射校验起始交易日（卡1 A+C），缺 CHINA 日历时显式失败，
	// 因此配置需提供日历；本用例验证的是 SEL task 模板缺省，而非日历缺省。
	const std::filesystem::path holidays = root / "dist/common/holidays.json";
	ASSERT_TRUE(std::filesystem::exists(holidays));

	const std::string cfg =
		"{\"replayer\":{\"mode\":\"none\",\"path\":" + json_quote(cwd.path()) + ","
		"\"stime\":\"202401020930\",\"etime\":\"202401021000\",\"tick\":false,"
		"\"basefiles\":{\"session\":" + json_quote(sessions.string()) + ","
		"\"holiday\":" + json_quote(holidays.string()) + "}},"
		"\"env\":{\"mocker\":\"sel\",\"slippage\":0},"
		"\"sel\":{\"name\":\"r14_default_task\",\"module\":" + json_quote(WT_COV_SEL_PLUGIN_PATH) + ","
		"\"strategy\":{\"name\":\"DualThrustSelection\",\"id\":\"r14_default_task\",\"params\":{"
		"\"days\":1,\"k1\":0.5,\"k2\":0.5,\"period\":\"m1\",\"count\":1,\"stock\":false,\"codes\":\"\"}},"
		"\"task\":{\"date\":0,\"time\":2359,\"period\":\"d\"}}}";
	const std::string configPath = cwd.path() + "/sel-defaults.json";
	ASSERT_TRUE(write_text(configPath, cfg));
	const std::string loggerConfigPath = cwd.path() + "/logcfg.json";
	ASSERT_TRUE(write_text(loggerConfigPath,
		"{\"root\":{\"level\":\"debug\",\"async\":false,\"sinks\":["
		"{\"type\":\"console_sink\",\"pattern\":\"%v\"}]}}"));

	const ChildResult result = run_runner_in_child(
		{"wt_btrunner", "-l", loggerConfigPath, "-c", configPath}, cwd.path());
	ASSERT_TRUE(result.exited) << "signal=" << result.signal << " output=" << result.output;
	EXPECT_EQ(result.code, 0) << result.output;
	EXPECT_NE(result.output.find("press enter key to exit"), std::string::npos) << result.output;
}

TEST(r14_wtbtrunner_cli, selMissingTaskIsRejectedBeforeFactoryCreation)
{
	TempDirectory cwd;
	ASSERT_FALSE(cwd.path().empty());
	const std::string cfg =
		"{\"replayer\":{\"mode\":\"none\",\"path\":" + json_quote(cwd.path()) + ","
		"\"stime\":\"202401020930\",\"etime\":\"202401021000\",\"basefiles\":{}},"
		"\"env\":{\"mocker\":\"sel\"},"
		"\"sel\":{\"name\":\"r14_missing_task\",\"module\":\"\"}}";
	const std::string configPath = cwd.path() + "/sel-missing-task.json";
	ASSERT_TRUE(write_text(configPath, cfg));
	const std::string loggerConfigPath = cwd.path() + "/logcfg.json";
	ASSERT_TRUE(write_text(loggerConfigPath,
		"{\"root\":{\"level\":\"debug\",\"async\":false,\"sinks\":["
		"{\"type\":\"console_sink\",\"pattern\":\"%v\"}]}}"));

	const ChildResult result = run_runner_in_child(
		{"wt_btrunner", "-l", loggerConfigPath, "-c", configPath}, cwd.path());
	ASSERT_TRUE(result.exited) << "signal=" << result.signal << " output=" << result.output;
	EXPECT_EQ(result.code, 255);
	EXPECT_NE(result.output.find("Invalid SEL task configuration"), std::string::npos) << result.output;
	EXPECT_EQ(result.output.find("Initializing SEL strategy factory failed"), std::string::npos) << result.output;
}
