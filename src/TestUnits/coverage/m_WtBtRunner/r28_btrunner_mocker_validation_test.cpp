#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

int wt_coverage_main_wtbtrunner(int argc, char* argv[]);

namespace
{
	namespace fs = std::filesystem;

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
			const std::string pattern = (fs::temp_directory_path() / "r28-btr-mode-XXXXXX").string();
			std::vector<char> writable(pattern.begin(), pattern.end());
			writable.push_back('\0');
			char* created = mkdtemp(writable.data());
			if (created)
				_path = created;
		}

		~TempDirectory()
		{
			if (!_path.empty())
				fs::remove_all(_path);
		}

		const fs::path& path() const { return _path; }

	private:
		fs::path _path;
	};

	bool write_text(const fs::path& path, const std::string& content)
	{
		std::ofstream output(path);
		if (!output)
			return false;
		output << content;
		return output.good();
	}

	ChildResult run_runner_in_child(const fs::path& loggerPath, const fs::path& configPath)
	{
		ChildResult result;
		int fds[2];
		if (pipe(fds) != 0)
			return result;

		std::vector<std::string> words = {
			"wt_btrunner", "-l", loggerPath.string(), "-c", configPath.string()};
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
			dup2(fds[1], STDOUT_FILENO);
			const int nullInput = open("/dev/null", O_RDONLY);
			if (nullInput >= 0)
			{
				dup2(nullInput, STDIN_FILENO);
				close(nullInput);
			}
			const int nullError = open("/dev/null", O_WRONLY);
			if (nullError >= 0)
			{
				dup2(nullError, STDERR_FILENO);
				close(nullError);
			}
			close(fds[0]);
			close(fds[1]);

			std::vector<char*> argv;
			argv.reserve(words.size());
			for (std::string& word : words)
				argv.push_back(word.data());
			const int rc = wt_coverage_main_wtbtrunner(static_cast<int>(argv.size()), argv.data());
			std::fflush(stdout);
			_exit(rc == -1 ? 101 : (rc == 0 ? 0 : 102));
		}

		close(fds[1]);
		char buffer[4096];
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

	std::string config_with_mocker(const std::string& mode)
	{
		return "{\"env\":{\"mocker\":\"" + mode + "\"}}";
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

	std::string config_with_missing_strategy_factory(const fs::path& directory, const std::string& mode)
	{
		std::ostringstream config;
		config << R"JSON({"replayer":{"mode":"csv","path":".","stime":20200101090000,"etime":20200101150000,"basefiles":{}},)JSON"
			<< "\"env\":{\"mocker\":" << json_quote(mode) << ",\"slippage\":0},"
			<< json_quote(mode) << ":{\"module\":"
			<< json_quote((directory / "missing_strategy_plugin.dylib").string())
			<< R"JSON(,"strategy":{"name":"missing_strategy","id":"r28_failure"})JSON";
		if (mode == "sel")
			config << R"JSON(,"task":{"date":0,"time":2359,"period":"d"})JSON";
		config << "}}";
		return config.str();
	}

	const char* factory_failure_message(const std::string& mode)
	{
		if (mode == "cta") return "Initializing CTA strategy factory failed";
		if (mode == "hft") return "Initializing HFT strategy factory failed";
		return "Initializing SEL strategy factory failed";
	}

	ChildResult invoke_with_config(const fs::path& directory, const std::string& config)
	{
		const fs::path configPath = directory / "runner.json";
		const fs::path loggerPath = directory / "logger.json";
		if (!write_text(configPath, config) ||
			!write_text(loggerPath,
				R"JSON({"root":{"level":"error","async":false,"sinks":[{"type":"ostream_sink","pattern":"%v"}]}})JSON"))
			return ChildResult();
		return run_runner_in_child(loggerPath, configPath);
	}

	class SupportedMockerMode : public ::testing::TestWithParam<std::string>
	{
	protected:
		void SetUp() override { ASSERT_FALSE(_directory.path().empty()); }

		TempDirectory _directory;
	};
}

TEST_P(SupportedMockerMode, PassesModePreflightBeforeReplayerInitialization)
{
	const ChildResult result = invoke_with_config(_directory.path(), config_with_mocker(GetParam()));
	ASSERT_TRUE(result.exited) << "signal=" << result.signal << " output=" << result.output;
	EXPECT_EQ(101, result.code) << result.output;
	EXPECT_NE(result.output.find("Initializing historical data replayer failed"), std::string::npos)
		<< result.output;
	EXPECT_EQ(result.output.find("Unsupported backtest mocker mode"), std::string::npos)
		<< result.output;
	EXPECT_EQ(result.output.find("Preparing historical data replayer failed"), std::string::npos)
		<< result.output;
}

INSTANTIATE_TEST_SUITE_P(AllSupportedModes, SupportedMockerMode,
	::testing::Values("cta", "hft", "sel", "exec", "uft"));

class R28StrategyFactoryInitFailure : public ::testing::TestWithParam<std::string>
{
protected:
	void SetUp() override { ASSERT_FALSE(_directory.path().empty()); }

	TempDirectory _directory;
};

TEST_P(R28StrategyFactoryInitFailure, FailsBeforePrepareOrReplay)
{
	const std::string mode = GetParam();
	const ChildResult result = invoke_with_config(
		_directory.path(), config_with_missing_strategy_factory(_directory.path(), mode));
	ASSERT_TRUE(result.exited) << "signal=" << result.signal << " output=" << result.output;
	EXPECT_EQ(101, result.code) << result.output;
	EXPECT_NE(result.output.find("---cfg initialized---"), std::string::npos) << result.output;
	EXPECT_NE(result.output.find(factory_failure_message(mode)), std::string::npos) << result.output;
	EXPECT_EQ(result.output.find("Initializing historical data replayer failed"), std::string::npos)
		<< result.output;
	EXPECT_EQ(result.output.find("Preparing historical data replayer failed"), std::string::npos)
		<< result.output;
	EXPECT_EQ(result.output.find("press enter key to exit"), std::string::npos) << result.output;
	if (mode == "cta")
		EXPECT_EQ(result.output.find("---cta ends---"), std::string::npos) << result.output;
}

INSTANTIATE_TEST_SUITE_P(CTAHftSel, R28StrategyFactoryInitFailure,
	::testing::Values("cta", "hft", "sel"));

TEST(R28BtRunnerMockerValidation, UnknownModeRejectedBeforeReplayerInitialization)
{
	TempDirectory directory;
	ASSERT_FALSE(directory.path().empty());
	const ChildResult result = invoke_with_config(directory.path(), config_with_mocker("r28_unknown"));
	ASSERT_TRUE(result.exited) << "signal=" << result.signal << " output=" << result.output;
	EXPECT_EQ(101, result.code) << result.output;
	EXPECT_NE(result.output.find("Unsupported backtest mocker mode: r28_unknown"), std::string::npos)
		<< result.output;
	EXPECT_EQ(result.output.find("Initializing historical data replayer failed"), std::string::npos)
		<< result.output;
	EXPECT_EQ(result.output.find("Preparing historical data replayer failed"), std::string::npos)
		<< result.output;
}

TEST(R28BtRunnerMockerValidation, EmptyModeRejectedBeforeReplayerInitialization)
{
	TempDirectory directory;
	ASSERT_FALSE(directory.path().empty());
	const ChildResult result = invoke_with_config(directory.path(), config_with_mocker(""));
	ASSERT_TRUE(result.exited) << "signal=" << result.signal << " output=" << result.output;
	EXPECT_EQ(101, result.code) << result.output;
	EXPECT_NE(result.output.find("Backtest configuration requires env.mocker"), std::string::npos)
		<< result.output;
	EXPECT_EQ(result.output.find("Initializing historical data replayer failed"), std::string::npos)
		<< result.output;
}

TEST(R28BtRunnerMockerValidation, MissingModeRejectedBeforeReplayerInitialization)
{
	TempDirectory directory;
	ASSERT_FALSE(directory.path().empty());
	const ChildResult result = invoke_with_config(directory.path(), "{\"env\":{}}" );
	ASSERT_TRUE(result.exited) << "signal=" << result.signal << " output=" << result.output;
	EXPECT_EQ(101, result.code) << result.output;
	EXPECT_NE(result.output.find("Backtest configuration requires env.mocker"), std::string::npos)
		<< result.output;
	EXPECT_EQ(result.output.find("Initializing historical data replayer failed"), std::string::npos)
		<< result.output;
}
