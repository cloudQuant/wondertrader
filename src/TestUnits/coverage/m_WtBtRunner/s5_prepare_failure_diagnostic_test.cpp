// I03-NEW-019: when HisDataReplayer::prepare() fails, the runner output must
// carry the replay error code and message, not only a generic line.
// The configuration below omits the holiday calendar, so prepare() fails at the
// checked initial trading-date mapping (decision card 1 A+C) with
// WT_REPLAY_ERROR_DATE_MAPPING (5). Positive control: the same configuration
// with the calendar present runs (covered by r14 selTaskDefaults...).
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

#include "Includes/ReplayStatusV1.h"

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

TEST(S5BtRunnerPrepareDiagnostic, PrepareFailureLogsReplayErrorCodeAndMessage)
{
	const std::filesystem::path cwd = std::filesystem::temp_directory_path() /
		("s5-btr-prepare-" + std::to_string(::getpid()));
	std::error_code ec;
	std::filesystem::remove_all(cwd, ec);
	ASSERT_TRUE(std::filesystem::create_directories(cwd, ec)) << ec.message();
	const std::filesystem::path sessions = repository_root() / "dist/common/sessions.json";
	ASSERT_TRUE(std::filesystem::exists(sessions));

	// No "holiday" basefile: the initial trading date cannot be mapped.
	const std::string cfg =
		"{\"replayer\":{\"mode\":\"none\",\"path\":" + json_quote(cwd.string()) + ","
		"\"stime\":\"202401020930\",\"etime\":\"202401021000\",\"tick\":false,"
		"\"basefiles\":{\"session\":" + json_quote(sessions.string()) + "}},"
		"\"env\":{\"mocker\":\"sel\",\"slippage\":0},"
		"\"sel\":{\"name\":\"s5_prepare_diag\",\"module\":" + json_quote(WT_COV_SEL_PLUGIN_PATH) + ","
		"\"strategy\":{\"name\":\"DualThrustSelection\",\"id\":\"s5_prepare_diag\",\"params\":{"
		"\"days\":1,\"k1\":0.5,\"k2\":0.5,\"period\":\"m1\",\"count\":1,\"stock\":false,\"codes\":\"\"}},"
		"\"task\":{\"date\":0,\"time\":2359,\"period\":\"d\"}}}";
	const std::filesystem::path configPath = cwd / "s5-prepare-diag.json";
	ASSERT_TRUE(write_text(configPath, cfg));
	const std::filesystem::path loggerConfigPath = cwd / "logcfg.json";
	ASSERT_TRUE(write_text(loggerConfigPath,
		"{\"root\":{\"level\":\"debug\",\"async\":false,\"sinks\":["
		"{\"type\":\"console_sink\",\"pattern\":\"%v\"}]}}"));

	const ChildResult result = run_runner_in_child(
		{"wt_btrunner", "-l", loggerConfigPath.string(), "-c", configPath.string()}, cwd.string());
	std::filesystem::remove_all(cwd, ec);

	ASSERT_TRUE(result.exited) << "signal=" << result.signal << " output=" << result.output;
	EXPECT_EQ(255, result.code) << result.output;
	EXPECT_NE(std::string::npos, result.output.find("Preparing historical data replayer failed")) << result.output;
	const std::string codeText = "replay error " + std::to_string(WT_REPLAY_ERROR_DATE_MAPPING) + ": ";
	const std::size_t codePos = result.output.find(codeText);
	ASSERT_NE(std::string::npos, codePos) << result.output;
	// The replayer's own (non-empty) message follows the code on the same line.
	const std::size_t msgPos = codePos + codeText.size();
	const std::size_t lineEnd = result.output.find('\n', msgPos);
	const std::string message = result.output.substr(msgPos, lineEnd == std::string::npos ? std::string::npos : lineEnd - msgPos);
	EXPECT_GE(message.size(), 5u) << result.output;
	EXPECT_EQ(std::string::npos, result.output.find("press enter key to exit")) << result.output;
}
