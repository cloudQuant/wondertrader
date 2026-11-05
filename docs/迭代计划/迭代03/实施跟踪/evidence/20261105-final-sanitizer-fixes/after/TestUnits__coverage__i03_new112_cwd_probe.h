/*!
 * I03-NEW-112: WtHelper::getCWD()/get_cwd() used a fixed 255-byte buffer, ignored a
 * getcwd() failure (ERANGE once the working directory path reaches 255 bytes, ENOENT
 * when it was removed) and then read the uninitialized buffer, caching garbage.
 *
 * Every WtHelper copy caches the first result in a function-local static, so each probe
 * runs in a forked child that changes directory before the first call. The child exits
 * 0 only if the returned value matches the contract; the parent asserts on the code.
 */
#pragma once

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace i03_new112
{
	// Build a directory chain under a private temp base whose absolute path is longer
	// than `minLength`, and return it (no trailing slash). Empty string on failure.
	inline std::string makeDeepDir(size_t minLength)
	{
		char tmpl[] = "/tmp/i03new112_XXXXXX";
		if (mkdtemp(tmpl) == NULL)
			return "";
		std::string path = tmpl;
		const std::string segment = "/segment_padding_for_a_long_working_directory_path_0123456789";
		while (path.size() <= minLength)
		{
			path += segment;
			if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST)
				return "";
		}
		return path;
	}

	inline std::string realDirWithSlash(const std::string& path)
	{
		char resolved[PATH_MAX] = { 0 };
		if (realpath(path.c_str(), resolved) == NULL)
			return "";
		std::string out = resolved;
		if (out.empty() || out.back() != '/')
			out += '/';
		return out;
	}

	// Child exit codes: 0 ok, 10 setup failure, 20 wrong value.
	template<typename Getter>
	int runInChild(Getter getter, bool removeDirFirst)
	{
		const std::string deep = makeDeepDir(300);
		if (deep.empty())
			return 10;
		const std::string expected = realDirWithSlash(deep);
		pid_t pid = fork();
		if (pid < 0)
			return 10;
		if (pid == 0)
		{
			if (chdir(deep.c_str()) != 0)
				_exit(10);
			if (removeDirFirst && rmdir(deep.c_str()) != 0)
				_exit(10);
			const std::string got = getter();
			if (removeDirFirst)
				_exit(got == "./" ? 0 : 20);
			_exit(got == expected ? 0 : 20);
		}
		int status = 0;
		const bool reaped = waitpid(pid, &status, 0) == pid;
		// remove the private temp chain ("/tmp/i03new112_XXXXXX/...")
		std::error_code ec;
		std::filesystem::remove_all(deep.substr(0, deep.find('/', 5)), ec);
		if (!reaped || !WIFEXITED(status))
			return 30;
		return WEXITSTATUS(status);
	}
}

#define I03_NEW112_CWD_TESTS(SUITE, GETTER_EXPR)                                         \
	TEST(SUITE, LongWorkingDirectoryIsReturnedInFull)                                    \
	{                                                                                    \
		/* red before the fix: getcwd(buffer,255) failed with ERANGE and garbage was */  \
		/* returned; green: the full absolute path with a trailing slash.            */  \
		EXPECT_EQ(0, i03_new112::runInChild([] { return std::string(GETTER_EXPR); }, false)); \
	}                                                                                    \
	TEST(SUITE, RemovedWorkingDirectoryFallsBackToDot)                                   \
	{                                                                                    \
		/* getcwd fails with ENOENT once the directory is gone: return "./" */           \
		EXPECT_EQ(0, i03_new112::runInChild([] { return std::string(GETTER_EXPR); }, true)); \
	}
