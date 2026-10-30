#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <cstdlib>
#include <string>
#include <vector>
#ifndef _WIN32
#include <climits>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

#include "r19_support.h"

#ifndef _WIN32
extern "C" char **environ;
#endif

using namespace shareblock;

namespace {

#ifndef _WIN32
// Cross-process tests use POSIX and one OS user. Every reader/writer must run
// the same upgraded binary; legacy read-only peers are unsupported. Forking
// while a ShareBlocks lock is held is also outside the supported contract.
constexpr int kChildTimeoutMs = 3000;
constexpr char kExecChildEnv[] = "WT_R19_EXEC_CHILD";
constexpr char kExecModeEnv[] = "WT_R19_EXEC_MODE";
constexpr char kExecTmpdirEnv[] = "WT_R19_EXEC_TMPDIR";
constexpr char kExecDomainEnv[] = "WT_R19_EXEC_DOMAIN";
constexpr char kExecPathEnv[] = "WT_R19_EXEC_PATH";
constexpr char kExecCommandFdEnv[] = "WT_R19_EXEC_COMMAND_FD";
constexpr char kExecEventFdEnv[] = "WT_R19_EXEC_EVENT_FD";

struct PhaseResult
{
	int32_t phase = 0;
	int32_t initialized = 0;
	int32_t refreshed = 0;
	int32_t refreshedAgain = 0;
	int32_t liveKeyCount = 0;
	int32_t firstAddedValue = -1;
	int32_t secondAddedValue = -1;
	int32_t deletedValue = -1;
	int32_t liveValue = -1;
	int32_t sectionCount = 0;
};

struct ScopedFd
{
	explicit ScopedFd(int value = -1) : fd(value) {}
	~ScopedFd() { reset(); }
	ScopedFd(const ScopedFd&) = delete;
	ScopedFd& operator=(const ScopedFd&) = delete;
	int get() const { return fd; }
	int release() { int value = fd; fd = -1; return value; }
	void reset(int value = -1)
	{
		if (fd >= 0)
			::close(fd);
		fd = value;
	}
	int fd;
};

struct ScopedSigpipeIgnore
{
	ScopedSigpipeIgnore()
	{
		struct sigaction action;
		std::memset(&action, 0, sizeof(action));
		action.sa_handler = SIG_IGN;
		sigemptyset(&action.sa_mask);
		valid = (::sigaction(SIGPIPE, &action, &previous) == 0);
	}
	~ScopedSigpipeIgnore()
	{
		if (valid)
			(void)::sigaction(SIGPIPE, &previous, nullptr);
	}
	struct sigaction previous{};
	bool valid = false;
};

struct ScopedPipe
{
	ScopedFd readEnd;
	ScopedFd writeEnd;
	bool open()
	{
		int fds[2] = { -1, -1 };
		if (::pipe(fds) != 0)
			return false;
		readEnd.reset(fds[0]);
		writeEnd.reset(fds[1]);
		for (int fd : fds)
		{
			const int flags = ::fcntl(fd, F_GETFD);
			if (flags < 0 || ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0)
				return false;
		}
		return true;
	}
};

bool wait_child_with_timeout(pid_t child, int& status, int timeoutMs);

struct ChildGuard
{
	explicit ChildGuard(pid_t child = -1) : pid(child) {}
	~ChildGuard()
	{
		if (pid > 0)
		{
			(void)::kill(pid, SIGKILL);
			int ignoredStatus = 0;
			(void)wait_child_with_timeout(pid, ignoredStatus, 1000);
		}
	}
	pid_t pid;
};

bool write_exact(int fd, const void* data, std::size_t size);
bool read_exact_with_timeout(int fd, void* data, std::size_t size, int timeoutMs);
bool spawn_exec_master_child(const std::string& executable, const std::string& mode,
	const std::string& domain,
	const std::string& path, int commandSource, int eventSource,
	const std::string& childTmpdir, pid_t& child);

struct ExecMasterChild
{
	ScopedPipe commandPipe;
	ScopedPipe eventPipe;
	ChildGuard child;

	bool start(const std::string& executable, const std::string& mode,
		const std::string& domain, const std::string& path,
		const std::string& childTmpdir = std::string())
	{
		if (!commandPipe.open() || !eventPipe.open())
			return false;
		pid_t pid = -1;
		if (!spawn_exec_master_child(executable, mode, domain, path,
			commandPipe.readEnd.get(), eventPipe.writeEnd.get(), childTmpdir, pid))
			return false;
		child.pid = pid;
		commandPipe.readEnd.reset();
		eventPipe.writeEnd.reset();
		return true;
	}

	bool send(char value) { return write_exact(commandPipe.writeEnd.get(), &value, sizeof(value)); }
	bool send(const void* data, std::size_t size)
	{
		return write_exact(commandPipe.writeEnd.get(), data, size);
	}
	bool receive(char& value, int timeoutMs = kChildTimeoutMs)
	{
		return read_exact_with_timeout(eventPipe.readEnd.get(), &value, sizeof(value), timeoutMs);
	}
	bool receive(void* data, std::size_t size, int timeoutMs = kChildTimeoutMs)
	{
		return read_exact_with_timeout(eventPipe.readEnd.get(), data, size, timeoutMs);
	}
	bool finish(int& status)
	{
		if (child.pid <= 0)
			return false;
		bool reaped = wait_child_with_timeout(child.pid, status, kChildTimeoutMs);
		if (!reaped)
		{
			(void)::kill(child.pid, SIGKILL);
			reaped = wait_child_with_timeout(child.pid, status, 1000);
		}
		if (reaped)
			child.pid = -1;
		return reaped;
	}
};

bool write_exact(int fd, const void* data, std::size_t size)
{
	const char* bytes = static_cast<const char*>(data);
	std::size_t offset = 0;
	while (offset < size)
	{
		const ssize_t written = ::write(fd, bytes + offset, size - offset);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return false;
		offset += static_cast<std::size_t>(written);
	}
	return true;
}

bool read_exact_with_timeout(int fd, void* data, std::size_t size, int timeoutMs)
{
	char* bytes = static_cast<char*>(data);
	std::size_t offset = 0;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	while (offset < size)
	{
		const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
			deadline - std::chrono::steady_clock::now()).count();
		if (remaining <= 0)
			return false;

		struct pollfd descriptor;
		descriptor.fd = fd;
		descriptor.events = POLLIN;
		descriptor.revents = 0;
		const int ready = ::poll(&descriptor, 1, static_cast<int>(remaining));
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready <= 0)
			return false;

		const ssize_t count = ::read(fd, bytes + offset, size - offset);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return false;
		offset += static_cast<std::size_t>(count);
	}
	return true;
}

bool wait_child_with_timeout(pid_t child, int& status, int timeoutMs)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	for (;;)
	{
		const pid_t result = ::waitpid(child, &status, WNOHANG);
		if (result == child)
			return true;
		if (result < 0 && errno != EINTR)
			return false;
		if (std::chrono::steady_clock::now() >= deadline)
			return false;
		struct pollfd unused{};
		(void)::poll(&unused, 0, 10);
	}
}

std::string current_executable_path()
{
#if defined(__APPLE__)
	uint32_t required = 0;
	(void)::_NSGetExecutablePath(nullptr, &required);
	if (required == 0)
		return {};
	std::vector<char> buffer(required + 1, '\0');
	if (::_NSGetExecutablePath(buffer.data(), &required) != 0)
		return {};
	return std::string(buffer.data());
#elif defined(__linux__)
	char buffer[PATH_MAX + 1] = {};
	const ssize_t length = ::readlink("/proc/self/exe", buffer, PATH_MAX);
	if (length <= 0 || length > PATH_MAX)
		return {};
	buffer[length] = '\0';
	return std::string(buffer);
#else
	return {};
#endif
}

int choose_child_fd(const std::vector<int>& reserved)
{
	struct rlimit limit{};
	if (::getrlimit(RLIMIT_NOFILE, &limit) != 0)
		return -1;
	const rlim_t bound = limit.rlim_cur == RLIM_INFINITY
		? static_cast<rlim_t>(4096) : (std::min)(limit.rlim_cur, static_cast<rlim_t>(4096));
	for (int candidate = 32; static_cast<rlim_t>(candidate) < bound; ++candidate)
	{
		if (std::find(reserved.begin(), reserved.end(), candidate) == reserved.end())
			return candidate;
	}
	return -1;
}

bool spawn_exec_master_child(const std::string& executable, const std::string& mode,
	const std::string& domain,
	const std::string& path, int commandSource, int eventSource,
	const std::string& childTmpdir, pid_t& child)
{
	std::vector<int> reserved{ commandSource, eventSource };
	const int commandTarget = choose_child_fd(reserved);
	if (commandTarget < 0)
		return false;
	reserved.push_back(commandTarget);
	const int eventTarget = choose_child_fd(reserved);
	if (eventTarget < 0)
		return false;

	posix_spawn_file_actions_t actions;
	int error = ::posix_spawn_file_actions_init(&actions);
	if (error != 0)
		return false;
	bool actionsReady = true;
	error = ::posix_spawn_file_actions_adddup2(&actions, commandSource, commandTarget);
	if (error == 0)
		error = ::posix_spawn_file_actions_adddup2(&actions, eventSource, eventTarget);
	if (error != 0)
		actionsReady = false;

	std::vector<std::string> environment;
	if (actionsReady)
	{
		for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry)
		{
			const std::string value(*entry);
			const std::size_t equal = value.find('=');
			const std::string key = value.substr(0, equal);
			if (key == kExecChildEnv || key == kExecModeEnv || key == kExecTmpdirEnv ||
				key == kExecDomainEnv || key == kExecPathEnv
				|| key == kExecCommandFdEnv || key == kExecEventFdEnv ||
				key == "GTEST_TOTAL_SHARDS" || key == "GTEST_SHARD_INDEX" ||
				key == "GTEST_SHARD_STATUS_FILE")
				continue;
			environment.push_back(value);
		}
		environment.push_back(std::string(kExecChildEnv) + "=1");
		environment.push_back(std::string(kExecModeEnv) + "=" + mode);
		if (!childTmpdir.empty())
			environment.push_back(std::string(kExecTmpdirEnv) + "=" + childTmpdir);
		environment.push_back(std::string(kExecDomainEnv) + "=" + domain);
		environment.push_back(std::string(kExecPathEnv) + "=" + path);
		environment.push_back(std::string(kExecCommandFdEnv) + "=" + std::to_string(commandTarget));
		environment.push_back(std::string(kExecEventFdEnv) + "=" + std::to_string(eventTarget));
	}
	std::vector<char*> environmentPointers;
	for (std::string& entry : environment)
		environmentPointers.push_back(const_cast<char*>(entry.c_str()));
	environmentPointers.push_back(nullptr);

	std::string filter = "--gtest_filter=R19ExecChild.protocol";
	char* arguments[] = {
		const_cast<char*>(executable.c_str()),
		const_cast<char*>(filter.c_str()),
		nullptr
	};
	if (actionsReady)
		error = ::posix_spawn(&child, executable.c_str(), &actions, nullptr, arguments,
			environmentPointers.data());
	(void)::posix_spawn_file_actions_destroy(&actions);
	return actionsReady && error == 0;
}

bool parse_fd_environment(const char* name, int& fd)
{
	const char* value = std::getenv(name);
	if (value == nullptr || value[0] == '\0')
		return false;
	char* end = nullptr;
	errno = 0;
	const long parsed = std::strtol(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || parsed < 0 || parsed > INT_MAX)
		return false;
	fd = static_cast<int>(parsed);
	return true;
}

int run_slave_child(int commandFd, int resultFd, const std::string& domain,
	const std::string& path)
{
	PhaseResult result;
	result.phase = 0;
	result.initialized = ShareBlocks::one().init_slave(domain.c_str(), path.c_str()) ? 1 : 0;
	if (result.initialized)
	{
		result.liveKeyCount = static_cast<int32_t>(ShareBlocks::one().get_keys(domain.c_str(), "live").size());
		result.deletedValue = ShareBlocks::one().get_int32(domain.c_str(), "doomed", "gone", -1);
	}
	if (!write_exact(resultFd, &result, sizeof(result)) || !result.initialized)
		return 20;

	for (int expectedPhase = 1; expectedPhase <= 4; ++expectedPhase)
	{
		char command = 0;
		ssize_t count;
		do { count = ::read(commandFd, &command, sizeof(command)); } while (count < 0 && errno == EINTR);
		if (count != 1 || command != static_cast<char>(expectedPhase))
			return 21;

		result = PhaseResult{};
		result.phase = expectedPhase;
		if (expectedPhase == 1)
		{
			result.refreshed = ShareBlocks::one().update_slave(domain.c_str(), false) ? 1 : 0;
			result.liveKeyCount = static_cast<int32_t>(ShareBlocks::one().get_keys(domain.c_str(), "live").size());
			result.firstAddedValue = ShareBlocks::one().get_int32(domain.c_str(), "live", "added1", -1);
			result.refreshedAgain = ShareBlocks::one().update_slave(domain.c_str(), false) ? 1 : 0;
		}
		else if (expectedPhase == 2)
		{
			result.refreshed = ShareBlocks::one().update_slave(domain.c_str(), false) ? 1 : 0;
			result.liveKeyCount = static_cast<int32_t>(ShareBlocks::one().get_keys(domain.c_str(), "live").size());
			result.firstAddedValue = ShareBlocks::one().get_int32(domain.c_str(), "live", "added1", -1);
			result.secondAddedValue = ShareBlocks::one().get_int32(domain.c_str(), "live", "added2", -1);
			result.refreshedAgain = ShareBlocks::one().update_slave(domain.c_str(), false) ? 1 : 0;
		}
		else if (expectedPhase == 3)
		{
			result.refreshed = ShareBlocks::one().update_slave(domain.c_str(), false) ? 1 : 0;
			result.liveKeyCount = static_cast<int32_t>(ShareBlocks::one().get_keys(domain.c_str(), "live").size());
			result.firstAddedValue = ShareBlocks::one().get_int32(domain.c_str(), "live", "added1", -1);
			result.secondAddedValue = ShareBlocks::one().get_int32(domain.c_str(), "live", "added2", -1);
			result.refreshedAgain = ShareBlocks::one().update_slave(domain.c_str(), false) ? 1 : 0;
		}
		else
		{
			result.refreshed = ShareBlocks::one().update_slave(domain.c_str(), false) ? 1 : 0;
			result.liveKeyCount = static_cast<int32_t>(ShareBlocks::one().get_keys(domain.c_str(), "live").size());
			result.deletedValue = ShareBlocks::one().get_int32(domain.c_str(), "doomed", "gone", -1);
			result.liveValue = ShareBlocks::one().get_int32(domain.c_str(), "live", "base", -1);
			result.sectionCount = static_cast<int32_t>(ShareBlocks::one().get_sections(domain.c_str()).size());
			result.refreshedAgain = ShareBlocks::one().update_slave(domain.c_str(), false) ? 1 : 0;
		}
		if (!write_exact(resultFd, &result, sizeof(result)))
			return 22;
	}

	ShareBlocks::one().release_slave(domain.c_str());
	return 0;
}
#endif

} // namespace

// This filtered test is launched through posix_spawn so the child starts from a
// clean executable image instead of inheriting the parent's singleton state.
TEST(R19ExecChild, protocol)
{
#if !defined(_WIN32)
	const char* childMode = std::getenv(kExecChildEnv);
	if (childMode == nullptr || std::strcmp(childMode, "1") != 0)
	{
		GTEST_SKIP() << "internal exec-child protocol test; launched only by its parent test";
	}
	const char* mode = std::getenv(kExecModeEnv);

	const char* domainValue = std::getenv(kExecDomainEnv);
	const char* pathValue = std::getenv(kExecPathEnv);
	int commandFd = -1;
	int eventFd = -1;
	ASSERT_NE(nullptr, domainValue);
	ASSERT_NE(nullptr, pathValue);
	ASSERT_TRUE(parse_fd_environment(kExecCommandFdEnv, commandFd));
	ASSERT_TRUE(parse_fd_environment(kExecEventFdEnv, eventFd));
	ASSERT_NE(nullptr, mode);
	r19::attach_logger();
	const char* childTmpdir = std::getenv(kExecTmpdirEnv);
	if (childTmpdir != nullptr)
		ASSERT_EQ(0, ::setenv("TMPDIR", childTmpdir, 1));

	const std::string domain(domainValue);
	const std::string path(pathValue);
	if (std::strcmp(mode, "slave_read") == 0)
	{
		const char ready = 'R';
		ASSERT_TRUE(write_exact(eventFd, &ready, sizeof(ready)));
		char command = 0;
		ASSERT_TRUE(read_exact_with_timeout(commandFd, &command, sizeof(command), kChildTimeoutMs));
		ASSERT_EQ('G', command);
		const bool initialized = ShareBlocks::one().init_slave(domain.c_str(), path.c_str());
		const int32_t result = initialized
			&& ShareBlocks::one().get_sections(domain.c_str()).size() == 1 ? 1 : 0;
		ASSERT_TRUE(write_exact(eventFd, &result, sizeof(result)));
		if (initialized)
			EXPECT_TRUE(ShareBlocks::one().release_slave(domain.c_str()));
		return;
	}
	if (std::strcmp(mode, "slave_refresh") == 0)
	{
		EXPECT_EQ(0, run_slave_child(commandFd, eventFd, domain, path));
		return;
	}
	ASSERT_STREQ("master_lease", mode);

	const char ready = 'R';
	ASSERT_TRUE(write_exact(eventFd, &ready, sizeof(ready)));
	char command = 0;
	ASSERT_TRUE(read_exact_with_timeout(commandFd, &command, sizeof(command), kChildTimeoutMs));
	ASSERT_EQ('G', command);

	const bool firstInit = ShareBlocks::one().init_master(domain.c_str(), path.c_str());
	char result = firstInit ? '1' : '0';
	ASSERT_TRUE(write_exact(eventFd, &result, sizeof(result)));
	if (firstInit)
	{
		ASSERT_TRUE(read_exact_with_timeout(commandFd, &command, sizeof(command), kChildTimeoutMs));
		ASSERT_EQ('X', command);
		return;
	}

	ASSERT_TRUE(read_exact_with_timeout(commandFd, &command, sizeof(command), kChildTimeoutMs));
	ASSERT_EQ('T', command);
	const bool retryInit = ShareBlocks::one().init_master(domain.c_str(), path.c_str());
	result = retryInit ? '1' : '0';
	ASSERT_TRUE(write_exact(eventFd, &result, sizeof(result)));
	if (retryInit)
	{
		ASSERT_TRUE(read_exact_with_timeout(commandFd, &command, sizeof(command), kChildTimeoutMs));
		ASSERT_EQ('X', command);
	}
#else
	GTEST_SKIP() << "POSIX exec-child protocol is unavailable on Windows";
#endif
}

TEST_F(R19TempDir, r19_clean_exec_slave_observes_commits_deletes_and_refreshes)
{
#if defined(_WIN32)
	GTEST_SKIP() << "POSIX posix_spawn/pipe harness is unavailable on Windows";
#else
	r19::attach_logger();
	ScopedSigpipeIgnore ignoreSigpipe;
	const std::string executable = current_executable_path();
	if (executable.empty())
		GTEST_SKIP() << "clean exec child launch is not available on this POSIX platform";

	const std::string master = r19::domain("process_master");
	const std::string slave = r19::domain("process_slave");
	const std::string path = file("process.blk");
	ASSERT_TRUE(ShareBlocks::one().init_master(master.c_str(), path.c_str()));
	ASSERT_NE(nullptr, ShareBlocks::one().allocate_int32(master.c_str(), "live", "base", 11));
	ASSERT_NE(nullptr, ShareBlocks::one().allocate_int32(master.c_str(), "doomed", "gone", 22));
	ASSERT_TRUE(ShareBlocks::one().commit_section(master.c_str(), "live"));
	ASSERT_TRUE(ShareBlocks::one().commit_section(master.c_str(), "doomed"));

	BoostMappingFile versionMapping;
	ASSERT_TRUE(versionMapping.map(path.c_str()));
	ShmBlock* block = static_cast<ShmBlock*>(versionMapping.addr());
	const uint64_t baseline = (std::numeric_limits<uint64_t>::max)() - 10;
	block->_updatetime = baseline;
	versionMapping.sync();

	// posix_spawn starts the slave with a fresh ShareBlocks singleton. Both sides
	// use the same OS user and the published block layout; Windows IPC is not covered.
	ExecMasterChild child;
	ASSERT_TRUE(child.start(executable, "slave_refresh", slave, path));

	PhaseResult result;
	ASSERT_TRUE(child.receive(&result, sizeof(result)))
		<< "slave initialization did not finish before timeout";
	ASSERT_EQ(0, result.phase);
	ASSERT_EQ(1, result.initialized);
	ASSERT_EQ(1, result.liveKeyCount);
	ASSERT_EQ(22, result.deletedValue);

	// A key allocation alone is not a published index version.
	ASSERT_NE(nullptr, ShareBlocks::one().allocate_int32(master.c_str(), "live", "added1", 33));
	const char pending = 1;
	ASSERT_TRUE(child.send(&pending, sizeof(pending)));
	ASSERT_TRUE(child.receive(&result, sizeof(result)));
	EXPECT_EQ(1, result.phase);
	EXPECT_EQ(0, result.refreshed);
	EXPECT_EQ(1, result.liveKeyCount);
	EXPECT_EQ(-1, result.firstAddedValue);
	EXPECT_EQ(0, result.refreshedAgain);

	ASSERT_TRUE(ShareBlocks::one().commit_section(master.c_str(), "live"));
	EXPECT_EQ(baseline + 1, block->_updatetime);
	const char firstCommit = 2;
	ASSERT_TRUE(child.send(&firstCommit, sizeof(firstCommit)));
	ASSERT_TRUE(child.receive(&result, sizeof(result)));
	EXPECT_EQ(2, result.phase);
	EXPECT_EQ(1, result.refreshed);
	EXPECT_EQ(2, result.liveKeyCount);
	EXPECT_EQ(33, result.firstAddedValue);
	EXPECT_EQ(0, result.refreshedAgain);

	ASSERT_NE(nullptr, ShareBlocks::one().allocate_int32(master.c_str(), "live", "added2", 44));
	ASSERT_TRUE(ShareBlocks::one().commit_section(master.c_str(), "live"));
	EXPECT_EQ(baseline + 2, block->_updatetime);
	const char secondCommit = 3;
	ASSERT_TRUE(child.send(&secondCommit, sizeof(secondCommit)));
	ASSERT_TRUE(child.receive(&result, sizeof(result)));
	EXPECT_EQ(3, result.phase);
	EXPECT_EQ(1, result.refreshed);
	EXPECT_EQ(3, result.liveKeyCount);
	EXPECT_EQ(33, result.firstAddedValue);
	EXPECT_EQ(44, result.secondAddedValue);
	EXPECT_EQ(0, result.refreshedAgain);

	ASSERT_TRUE(ShareBlocks::one().delete_section(master.c_str(), "doomed"));
	EXPECT_EQ(baseline + 3, block->_updatetime);
	const char deletion = 4;
	ASSERT_TRUE(child.send(&deletion, sizeof(deletion)));
	ASSERT_TRUE(child.receive(&result, sizeof(result)));
	EXPECT_EQ(4, result.phase);
	EXPECT_EQ(1, result.refreshed);
	EXPECT_EQ(3, result.liveKeyCount);
	EXPECT_EQ(-1, result.deletedValue);
	EXPECT_EQ(11, result.liveValue);
	EXPECT_EQ(1, result.sectionCount);
	EXPECT_EQ(0, result.refreshedAgain);

	child.commandPipe.writeEnd.reset();
	int status = 0;
	const bool reaped = child.finish(status);
	ASSERT_TRUE(reaped) << "child process did not exit before timeout";
	ASSERT_TRUE(WIFEXITED(status));
	EXPECT_EQ(0, WEXITSTATUS(status));
#endif
}

TEST_F(R19TempDir, r19_existing_short_domain_file_is_never_truncated_or_resized)
{
	const std::string master = r19::domain("short_existing");
	const std::string path = file("short_existing.blk");
	{
		std::ofstream existing(path, std::ios::binary | std::ios::out);
		ASSERT_TRUE(existing.is_open());
		existing.put('x');
	}

	EXPECT_FALSE(ShareBlocks::one().init_master(master.c_str(), path.c_str()));
	std::error_code ec;
	EXPECT_EQ(1u, std::filesystem::file_size(path, ec));
	EXPECT_FALSE(ec);
	std::ifstream existing(path, std::ios::binary | std::ios::in);
	ASSERT_TRUE(existing.is_open());
	EXPECT_EQ('x', existing.get());
}

TEST_F(R19TempDir, r19_shared_operation_locks_accept_explicit_and_empty_path_fallback)
{
	r19::attach_logger();
	const std::string explicitDomain = r19::domain("explicit_lock_path");
	const std::string explicitPath = file("explicit_lock_path.blk");
	ASSERT_TRUE(ShareBlocks::one().init_master(explicitDomain.c_str(), explicitPath.c_str()));
	ASSERT_NE(nullptr, ShareBlocks::one().allocate_int32(explicitDomain.c_str(), "live", "value", 41));
	ASSERT_TRUE(ShareBlocks::one().commit_section(explicitDomain.c_str(), "live"));
	EXPECT_EQ(1u, ShareBlocks::one().get_sections(explicitDomain.c_str()).size());

	// An empty path retains the API's documented fallback to the domain name.
	const std::string fallbackDomain = file("empty_path_fallback.blk");
	ASSERT_TRUE(ShareBlocks::one().init_master(fallbackDomain.c_str(), ""));
	ASSERT_NE(nullptr, ShareBlocks::one().allocate_int32(fallbackDomain.c_str(), "live", "value", 42));
	ASSERT_TRUE(ShareBlocks::one().commit_section(fallbackDomain.c_str(), "live"));
	EXPECT_EQ(1u, ShareBlocks::one().get_sections(fallbackDomain.c_str()).size());

	EXPECT_FALSE(ShareBlocks::one().init_master("", ""));
}

TEST_F(R19TempDir, r19_clean_exec_processes_contend_for_master_lease_and_loser_retries)
{
#if defined(_WIN32)
	GTEST_SKIP() << "POSIX posix_spawn/pipe harness is unavailable on Windows";
#else
	r19::attach_logger();
	ScopedSigpipeIgnore ignoreSigpipe;
	const std::string domain = r19::domain("exec_master_contention");
	const std::string path = file("exec_master_contention.blk");
	const std::string executable = current_executable_path();
	if (executable.empty())
		GTEST_SKIP() << "clean exec child launch is not available on this POSIX platform";

	ExecMasterChild first;
	ExecMasterChild second;
	ASSERT_TRUE(first.start(executable, "master_lease", domain, path));
	ASSERT_TRUE(second.start(executable, "master_lease", domain, path));
	char firstReady = 0;
	char secondReady = 0;
	ASSERT_TRUE(first.receive(firstReady));
	ASSERT_TRUE(second.receive(secondReady));
	ASSERT_EQ('R', firstReady);
	ASSERT_EQ('R', secondReady);
	ASSERT_TRUE(first.send('G'));
	ASSERT_TRUE(second.send('G'));

	char firstResult = 0;
	char secondResult = 0;
	ASSERT_TRUE(first.receive(firstResult));
	ASSERT_TRUE(second.receive(secondResult));
	ASSERT_TRUE(firstResult == '0' || firstResult == '1');
	ASSERT_TRUE(secondResult == '0' || secondResult == '1');
	ASSERT_NE(firstResult, secondResult);
	ASSERT_EQ(1, (firstResult == '1' ? 1 : 0) + (secondResult == '1' ? 1 : 0));

	ExecMasterChild& winner = firstResult == '1' ? first : second;
	ExecMasterChild& loser = firstResult == '0' ? first : second;
	ASSERT_TRUE(winner.send('X'));
	int winnerStatus = 0;
	ASSERT_TRUE(winner.finish(winnerStatus)) << "lease holder did not exit before timeout";
	ASSERT_TRUE(WIFEXITED(winnerStatus));
	ASSERT_EQ(0, WEXITSTATUS(winnerStatus));

	// Retry only after the first master process has exited and released its lease.
	ASSERT_TRUE(loser.send('T'));
	char retryResult = 0;
	ASSERT_TRUE(loser.receive(retryResult));
	EXPECT_EQ('1', retryResult);
	ASSERT_TRUE(loser.send('X'));
	int loserStatus = 0;
	ASSERT_TRUE(loser.finish(loserStatus)) << "retrying child did not exit before timeout";
	EXPECT_TRUE(WIFEXITED(loserStatus));
	EXPECT_EQ(0, WEXITSTATUS(loserStatus));
#endif
}

TEST_F(R19TempDir, r19_shared_operation_lock_child_finishes_after_external_file_lock_release)
{
#if defined(_WIN32)
	GTEST_SKIP() << "POSIX flock/posix_spawn harness is unavailable on Windows";
#else
	r19::attach_logger();
	ScopedSigpipeIgnore ignoreSigpipe;
	const std::string master = r19::domain("operation_lock_master");
	const std::string path = file("operation_lock.blk");
	const std::string executable = current_executable_path();
	if (executable.empty())
		GTEST_SKIP() << "clean exec child launch is not available on this POSIX platform";
	ASSERT_TRUE(ShareBlocks::one().init_master(master.c_str(), path.c_str()));
	ASSERT_NE(nullptr, ShareBlocks::one().allocate_int32(master.c_str(), "live", "value", 17));
	ASSERT_TRUE(ShareBlocks::one().commit_section(master.c_str(), "live"));

	ScopedFd heldFile(::open(path.c_str(), O_RDWR));
	ASSERT_GE(heldFile.get(), 0) << std::strerror(errno);
	ASSERT_EQ(0, ::flock(heldFile.get(), LOCK_EX));
	ExecMasterChild child;
	ASSERT_TRUE(child.start(executable, "slave_read", master, path));

	char ready = 0;
	ASSERT_TRUE(child.receive(ready));
	ASSERT_EQ('R', ready);
	ASSERT_TRUE(child.send('G'));
	ASSERT_EQ(0, ::flock(heldFile.get(), LOCK_UN));
	heldFile.reset();

	int32_t result = 0;
	const bool gotResult = child.receive(&result, sizeof(result));
	child.commandPipe.writeEnd.reset();
	int status = 0;
	const bool reaped = child.finish(status);

	EXPECT_TRUE(gotResult);
	EXPECT_EQ(1, result);
	EXPECT_TRUE(reaped) << "child process did not exit before timeout";
	EXPECT_TRUE(WIFEXITED(status));
	EXPECT_EQ(0, WEXITSTATUS(status));
#endif
}

TEST_F(R19TempDir, r19_shared_master_lease_converges_hardlink_aliases)
{
#if defined(_WIN32)
	GTEST_SKIP() << "POSIX process identity harness is unavailable on Windows";
#else
	r19::attach_logger();
	const std::string master = r19::domain("alias_master");
	const std::string duplicate = r19::domain("alias_duplicate_master");
	const std::string path = file("alias.blk");
	const std::string alias = file("alias_hardlink.blk");
	ASSERT_TRUE(ShareBlocks::one().init_master(master.c_str(), path.c_str()));
	ASSERT_NE(nullptr, ShareBlocks::one().allocate_int32(master.c_str(), "live", "value", 29));
	ASSERT_TRUE(ShareBlocks::one().commit_section(master.c_str(), "live"));

	std::error_code ec;
	std::filesystem::create_hard_link(path, alias, ec);
	if (ec)
		GTEST_SKIP() << "hard links unavailable in the temporary filesystem: " << ec.message();
	EXPECT_FALSE(ShareBlocks::one().init_master(duplicate.c_str(), alias.c_str()));
	const std::vector<std::string> sections = ShareBlocks::one().get_sections(master.c_str());
	ASSERT_EQ(1u, sections.size());
	EXPECT_EQ("live", sections.front());

	// A separate exec process with another TMPDIR must still converge on the
	// mapped file identity for its master lease.
	const std::string executable = current_executable_path();
	if (executable.empty())
		GTEST_SKIP() << "clean exec child launch is not available on this POSIX platform";
	ScopedSigpipeIgnore ignoreSigpipe;
	ExecMasterChild child;
	ASSERT_TRUE(child.start(executable, "master_lease", duplicate, alias, dir()));
	char ready = 0;
	ASSERT_TRUE(child.receive(ready));
	ASSERT_EQ('R', ready);
	ASSERT_TRUE(child.send('G'));
	char childInitialized = 0;
	ASSERT_TRUE(child.receive(childInitialized));
	EXPECT_EQ('0', childInitialized);
	ASSERT_TRUE(child.send('T'));
	char retryResult = 0;
	ASSERT_TRUE(child.receive(retryResult));
	EXPECT_EQ('0', retryResult);
	int status = 0;
	const bool reaped = child.finish(status);
	ASSERT_TRUE(reaped);
	EXPECT_TRUE(WIFEXITED(status));
	EXPECT_EQ(0, WEXITSTATUS(status));
	EXPECT_EQ(1u, ShareBlocks::one().get_sections(master.c_str()).size());
#endif
}

TEST_F(R19TempDir, r19_shared_mapping_lock_path_is_absolute_and_identity_checked)
{
#if defined(_WIN32)
	GTEST_SKIP() << "POSIX rename/CWD harness is unavailable on Windows";
#else
	r19::attach_logger();
	const std::filesystem::path workingDir = std::filesystem::path(dir()) / "working";
	std::error_code ec;
	std::filesystem::create_directories(workingDir, ec);
	ASSERT_FALSE(ec) << ec.message();
	const std::filesystem::path originalCwd = std::filesystem::current_path();
	struct RestoreCwd
	{
		std::filesystem::path path;
		~RestoreCwd() { std::error_code ec; std::filesystem::current_path(path, ec); }
	} restore{ originalCwd };
	ASSERT_NO_THROW(std::filesystem::current_path(workingDir));

	const std::string master = r19::domain("cwd_master");
	const std::string path = "relative.blk";
	const std::filesystem::path absolutePath = workingDir / path;
	const std::filesystem::path movedPath = workingDir / "renamed.blk";
	ASSERT_TRUE(ShareBlocks::one().init_master(master.c_str(), path.c_str()));
	ASSERT_NE(nullptr, ShareBlocks::one().allocate_int32(master.c_str(), "live", "value", 31));
	ASSERT_TRUE(ShareBlocks::one().commit_section(master.c_str(), "live"));

	ASSERT_NO_THROW(std::filesystem::current_path(std::filesystem::temp_directory_path()));
	ASSERT_EQ(1u, ShareBlocks::one().get_sections(master.c_str()).size());
	std::filesystem::rename(absolutePath, movedPath, ec);
	ASSERT_FALSE(ec) << ec.message();
	EXPECT_TRUE(ShareBlocks::one().get_sections(master.c_str()).empty());

	// A replacement at the remembered path must not be mistaken for the mapped
	// inode. The operation lock compares file identity and fails closed.
	{
		std::ofstream replacement(absolutePath, std::ios::binary | std::ios::out);
		ASSERT_TRUE(replacement.is_open());
		replacement.put('x');
	}
	EXPECT_TRUE(ShareBlocks::one().get_sections(master.c_str()).empty());
	std::filesystem::remove(absolutePath, ec);
	ASSERT_FALSE(ec) << ec.message();
	std::filesystem::rename(movedPath, absolutePath, ec);
	ASSERT_FALSE(ec) << ec.message();
	EXPECT_EQ(1u, ShareBlocks::one().get_sections(master.c_str()).size());
#endif
}

