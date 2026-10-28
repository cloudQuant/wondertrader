/**
 * r113_porter_config_empty_test.cpp  (lane R113 · wave19)
 *
 * 靶子：config_porter 的空串回落支
 *   - WtPorter.cpp:260-266：cfgfile 为空时改用 "config.json" + isFile=true
 *
 * 该入口只写进程单例，因此在 fork 子进程里把 cwd 切到临时目录、写入 config.json，
 * 再调用 config_porter("", true)。子进程用 exit() 让 LLVM profile 正常落盘；
 * 父进程不触碰 WtPorter 单例，避免污染同二进制后续用例。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

#include "WtPorter/PorterDefs.h"
#include "WtPorter/WtPorter.h"
#include "WtPorter/WtRtRunner.h"

USING_NS_WTP;

/* WtPorter.cpp:34-38 的进程单例 */
extern WtRtRunner& getRunner();

namespace
{
	namespace stdfs = std::filesystem;

	void write_text(const std::string& path, const std::string& body)
	{
		std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
		ofs << body;
	}
}

/*
 * 生产: WtPorter.cpp:260-266（strlen(cfgfile)==0 支）
 *       另有 WtRtRunner::config 的完整 cta 引导链作为可达性证明。
 */
TEST(R113PorterConfigEmpty, EmptyConfigPathUsesDefaultConfigJsonInChild)
{
	const stdfs::path dir = stdfs::temp_directory_path()
		/ ("r113_porter_cfgempty_" + std::to_string((long)::getpid()));
	std::error_code ec;
	stdfs::remove_all(dir, ec);
	stdfs::create_directories(dir, ec);

	write_text((dir / "actions.json").string(), "{}");
	write_text((dir / "config.json").string(),
		std::string("{\"basefiles\":{},\"env\":{\"name\":\"cta\"},\"data\":{},"
			"\"bspolicy\":\"") + (dir / "actions.json").string() + "\"}");

	const pid_t pid = ::fork();
	ASSERT_GE(pid, 0);
	if (pid == 0)
	{
		std::error_code cec;
		stdfs::current_path(dir, cec);
		if (cec)
			::exit(2);

		config_porter("", true);

		/* config_porter 无返回值：以单例是否拿到 cta 引擎作为成功判据 */
		if (getRunner().getEngine() == nullptr)
			::exit(3);
		::exit(0);
	}

	int status = 0;
	ASSERT_EQ(pid, ::waitpid(pid, &status, 0));
	ASSERT_TRUE(WIFEXITED(status));
	EXPECT_EQ(0, WEXITSTATUS(status));

	stdfs::remove_all(dir, ec);
}
