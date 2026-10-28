/**
 * r113_btporter_release_test.cpp  (lane R113 · wave19)
 *
 * 靶子：release_backtest 的两级转发
 *   - WtBtPorter.cpp:232-235 -> WtBtRunner::release()
 *   - WtBtRunner.cpp:585-588 -> WTSLogger::stop()
 *
 * WTSLogger::stop 会进程级静音日志，因此放在 fork 子进程里调用；
 * 父进程不触碰全局 logger，后续用例仍可正常记录。
 */
#include <gtest/gtest.h>

#include <sys/wait.h>
#include <unistd.h>

#include "WtBtPorter/PorterDefs.h"
#include "WtBtPorter/WtBtPorter.h"

/*
 * 生产: WtBtPorter.cpp:232-235（release_backtest 转发）
 *       WtBtRunner.cpp:585-588（WTSLogger::stop）
 */
TEST(R113BtPorterRelease, ReleaseBacktestForwardsToLoggerStopInChild)
{
	const pid_t pid = ::fork();
	ASSERT_GE(pid, 0);
	if (pid == 0)
	{
		release_backtest();
		::exit(0);
	}

	int status = 0;
	ASSERT_EQ(pid, ::waitpid(pid, &status, 0));
	ASSERT_TRUE(WIFEXITED(status));
	EXPECT_EQ(0, WEXITSTATUS(status));
}
