#include <gtest/gtest.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

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

	void write_file(const std::filesystem::path& path, const std::string& content)
	{
		std::ofstream out(path);
		ASSERT_TRUE(out.is_open()) << path.string();
		out << content;
		ASSERT_TRUE(out.good()) << path.string();
	}

	ChildResult run_runner_in_child(const std::filesystem::path& log_config,
		const std::filesystem::path& runner_config)
	{
		ChildResult result;
		int fds[2];
		if (pipe(fds) != 0)
			return result;

		std::vector<std::string> words = {
			"wt_btrunner", "-l", log_config.string(), "-c", runner_config.string()};
		pid_t pid = fork();
		if (pid < 0)
		{
			close(fds[0]);
			close(fds[1]);
			return result;
		}

		if (pid == 0)
		{
			dup2(fds[1], STDOUT_FILENO);
			int devnull = open("/dev/null", O_RDONLY);
			if (devnull >= 0)
			{
				dup2(devnull, STDIN_FILENO);
				close(devnull);
			}
			devnull = open("/dev/null", O_WRONLY);
			if (devnull >= 0)
			{
				dup2(devnull, STDERR_FILENO);
				close(devnull);
			}
			close(fds[0]);
			close(fds[1]);

			std::vector<char*> argv;
			argv.reserve(words.size());
			for (std::string& word : words)
				argv.push_back(word.data());
			const int rc = wt_coverage_main_wtbtrunner(static_cast<int>(argv.size()), argv.data());
			std::cout.flush();
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
		waitpid(pid, &status, 0);
		result.exited = WIFEXITED(status);
		result.code = result.exited ? WEXITSTATUS(status) : -1;
		result.signal = WIFSIGNALED(status) ? WTERMSIG(status) : -1;
		return result;
	}

	class StrategyFactoryInitFailure : public ::testing::TestWithParam<std::string>
	{
	protected:
		void SetUp() override
		{
			m_dir = std::filesystem::temp_directory_path() /
				("wtbt_runner_strategy_fail_" + std::to_string(static_cast<long>(getpid())) + "_" + GetParam());
			std::filesystem::remove_all(m_dir);
			ASSERT_TRUE(std::filesystem::create_directories(m_dir));
		}

		void TearDown() override
		{
			std::filesystem::remove_all(m_dir);
		}

		std::filesystem::path m_dir;
	};
}

TEST_P(StrategyFactoryInitFailure, ReturnsFailureBeforeSinkRegistrationAndReplay)
{
	const std::string mode = GetParam();
	const std::string config_key = mode;
	const std::filesystem::path logger_path = m_dir / "log.json";
	const std::filesystem::path runner_path = m_dir / "runner.json";
	const std::filesystem::path missing_module = m_dir / "missing_strategy_plugin.dylib";

	write_file(logger_path,
		R"({"root":{"level":"error","async":false,"sinks":[{"type":"ostream_sink","pattern":"%v"}]}})");
	std::ostringstream config;
	config << "{"
		<< R"("replayer":{"mode":"csv","path":".","stime":20200101090000,"etime":20200101150000,"basefiles":{}},)"
		<< R"("env":{"mocker":")" << mode << R"(","slippage":0},)"
		<< '"' << config_key << R"(":{"module":")" << missing_module.string()
		<< R"(","strategy":{"name":"missing_strategy","id":"r220_failure"})";
	if (mode == "sel")
		config << R"(,"task":{"date":0,"time":2359,"period":"d"})";
	config << "}}";
	write_file(runner_path, config.str());

	const ChildResult result = run_runner_in_child(logger_path, runner_path);
	ASSERT_TRUE(result.exited) << "signal=" << result.signal << " output=" << result.output;
	EXPECT_EQ(result.code, 101) << result.output;
	EXPECT_NE(result.output.find("Initializing " + std::string(mode == "cta" ? "CTA" : mode == "hft" ? "HFT" : "SEL") +
		" strategy factory failed"), std::string::npos) << result.output;
	EXPECT_EQ(result.output.find("press enter key to exit"), std::string::npos) << result.output;
	if (mode == "cta")
		EXPECT_EQ(result.output.find("---cta ends---"), std::string::npos) << result.output;
}

INSTANTIATE_TEST_SUITE_P(CTA_HFT_SEL, StrategyFactoryInitFailure,
	::testing::Values(std::string("cta"), std::string("hft"), std::string("sel")),
	[](const ::testing::TestParamInfo<std::string>& info) {
		return info.param == "cta" ? "Cta" : (info.param == "hft" ? "Hft" : "Sel");
	});
