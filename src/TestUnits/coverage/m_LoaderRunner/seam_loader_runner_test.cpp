/*
 * 通路探针（非行为断言）：验证「带 main() 的可执行体」能在不改产品的前提下进入覆盖率测试——
 * 生产 TU 以 -Dmain=<模块>_coverage_main 编译，测试按受控 argv 调用它。
 * 选 --help 分支：LoaderRunner.cpp 该分支会 return 0，能走完 cppcli 参数定义与 parse 又不会阻塞。
 */
#include <gtest/gtest.h>

#include <cstdio>
#include <iostream>
#include <string>

#ifndef _WIN32
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#endif

int wt_coverage_main_loaderrunner(int argc, char* argv[]);

#ifndef _WIN32
namespace {

struct LoaderHelpChildResult
{
	bool exited = false;
	int code = -1;
	int signal = -1;
	std::string output;
};

LoaderHelpChildResult call_loader_help_in_child()
{
	LoaderHelpChildResult result;
	int fds[2];
	if (pipe(fds) != 0)
		return result;

	std::cout.flush();
	std::cerr.flush();
	std::fflush(nullptr);
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
		alarm(5);
		if (dup2(fds[1], STDOUT_FILENO) < 0 || dup2(fds[1], STDERR_FILENO) < 0)
			_exit(121);
		close(fds[1]);

		char program[] = "loader_runner";
		char help[] = "-h";
		char* argv[] = { program, help, nullptr };
		const int rc = wt_coverage_main_loaderrunner(2, argv);
		std::cout.flush();
		std::cerr.flush();
		std::fflush(nullptr);
		_exit(rc & 0xff);
	}

	close(fds[1]);
	char buffer[1024];
	for (;;)
	{
		const ssize_t count = read(fds[0], buffer, sizeof(buffer));
		if (count > 0)
			result.output.append(buffer, static_cast<size_t>(count));
		else if (count < 0 && errno == EINTR)
			continue;
		else
			break;
	}
	close(fds[0]);

	int status = 0;
	pid_t waited = 0;
	do
	{
		waited = waitpid(pid, &status, 0);
	} while (waited < 0 && errno == EINTR);
	if (waited == pid)
	{
		result.exited = WIFEXITED(status);
		result.code = result.exited ? WEXITSTATUS(status) : -1;
		result.signal = WIFSIGNALED(status) ? WTERMSIG(status) : -1;
	}
	return result;
}

} // namespace
#endif

TEST(seam_loader_runner, help_path_returns_without_blocking)
{
#ifdef _WIN32
	GTEST_SKIP() << "this renamed-main probe uses POSIX process isolation";
#else
	const LoaderHelpChildResult result = call_loader_help_in_child();
	ASSERT_TRUE(result.exited) << "signal=" << result.signal << " output=" << result.output;
	EXPECT_EQ(result.code, 0) << result.output;
	EXPECT_NE(result.output.find("-m | --module"), std::string::npos) << result.output;
	EXPECT_NE(result.output.find("-c | --config"), std::string::npos) << result.output;
	EXPECT_NE(result.output.find("-h | --help"), std::string::npos) << result.output;
	EXPECT_NE(result.output.find("gain help doc"), std::string::npos) << result.output;
#endif
}
