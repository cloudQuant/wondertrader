#pragma once

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace r7_test
{
class ScopedTempDir
{
public:
	explicit ScopedTempDir(const std::string& prefix)
	{
		std::error_code ec;
		const std::filesystem::path root = std::filesystem::temp_directory_path(ec);
		if (ec)
			return;
		const std::string pattern = (root / (prefix + "_XXXXXX")).string();
		std::vector<char> writable(pattern.begin(), pattern.end());
		writable.push_back('\0');
		char* created = ::mkdtemp(writable.data());
		if (created == nullptr)
			return;
		_path = created;
		_owns = true;
	}

	~ScopedTempDir() { cleanup(); }

	bool ready() const { return _owns; }
	const std::string& path() const { return _path; }
	bool cleanup()
	{
		if (!_owns)
			return true;
		std::error_code ec;
		std::filesystem::remove_all(_path, ec);
		if (!ec)
			_owns = false;
		return !ec;
	}

private:
	std::string _path;
	bool _owns{ false };
};

struct ProbeResult
{
	bool started{ false };
	bool waited{ false };
	bool markerReceived{ false };
	int waitStatus{ 0 };
	int error{ 0 };
	std::string output;
};

using ProbeAction = int(*)(uint32_t, int);
inline const char* marker() { return "BUG-R7-002 before init\n"; }

inline bool write_marker(int fd)
{
	const size_t markerSize = std::strlen(marker());
	size_t markerBytes = 0;
	while (markerBytes < markerSize)
	{
		const ssize_t written = ::write(fd, marker() + markerBytes, markerSize - markerBytes);
		if (written > 0)
			markerBytes += (size_t)written;
		else if (written < 0 && errno == EINTR)
			continue;
		else
			return false;
	}
	return true;
}

inline ProbeResult run_bounded_child(const std::string& tempDir, uint32_t totalTimes,
	ProbeAction action)
{
	ProbeResult result;
	int markerPipe[2];
	if (::pipe(markerPipe) != 0)
	{
		result.error = errno;
		return result;
	}
	const pid_t child = ::fork();
	if (child < 0)
	{
		result.error = errno;
		::close(markerPipe[0]);
		::close(markerPipe[1]);
		return result;
	}
	if (child == 0)
	{
		::close(markerPipe[0]);
		if (::chdir(tempDir.c_str()) != 0)
			::_exit(125);
		// As in the R7-005 probe, the child runs C++ after fork. This assumes the
		// GoogleTest process is single-threaded; a threaded host needs exec isolation.
		::alarm(5);
		const int exitCode = action(totalTimes, markerPipe[1]);
		::close(markerPipe[1]);
		::_exit(exitCode);
	}

	result.started = true;
	::close(markerPipe[1]);
	pid_t waited;
	do
	{
		waited = ::waitpid(child, &result.waitStatus, 0);
	}
	while (waited < 0 && errno == EINTR);
	if (waited == child)
		result.waited = true;
	else
		result.error = errno;

	char buffer[128];
	for (;;)
	{
		const ssize_t count = ::read(markerPipe[0], buffer, sizeof(buffer));
		if (count > 0)
			result.output.append(buffer, (size_t)count);
		else if (count == 0)
			break;
		else if (errno != EINTR)
		{
			if (result.error == 0)
				result.error = errno;
			break;
		}
	}
	::close(markerPipe[0]);
	result.markerReceived = result.output.find(marker()) != std::string::npos;
	return result;
}

inline bool expect_normal_return(const ProbeResult& result)
{
	EXPECT_TRUE(result.started) << "fork/pipe failed with errno " << result.error;
	EXPECT_TRUE(result.markerReceived) << "child did not reach the pre-init marker; output: " << result.output;
	EXPECT_TRUE(result.waited) << "waitpid failed with errno " << result.error;
	if (!result.waited)
		return false;
	if (WIFEXITED(result.waitStatus))
	{
		EXPECT_EQ(WEXITSTATUS(result.waitStatus), 0) << "child output: " << result.output;
		return result.started && result.waited && result.markerReceived
			&& WEXITSTATUS(result.waitStatus) == 0;
	}
	else if (WIFSIGNALED(result.waitStatus))
		ADD_FAILURE() << "child terminated by signal " << WTERMSIG(result.waitStatus)
			<< " (including alarm timeout); output: " << result.output;
	else
		ADD_FAILURE() << "child did not exit normally; status=" << result.waitStatus;
	return false;
}
}
