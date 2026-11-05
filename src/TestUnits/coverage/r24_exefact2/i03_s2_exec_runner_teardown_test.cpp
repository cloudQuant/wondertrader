// 迭代03 最终缺口（slice2）I03-NEW-045：WtExecRunner 析构时，无法证明回调排空的失败 parser
// 候选（仍持有 API）按卡9a 保留到进程结束，其余关闭照常完成，不得 std::terminate。
// 子进程内构造本地 runner、按 initParsers 的失败路径放入候选后直接析构：
// 期望正常退出，候选被进程级持有者保留。改前：析构函数 std::terminate（SIGABRT 134）。
// 只用本地 fake：不加载 parser 模块，不连接柜台。
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <boost/core/noncopyable.hpp>
#include "Includes/IParserApi.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/ExecuteDefs.h"
#include "Includes/FasterDefs.h"
#include "Share/StdUtils.hpp"
#include "Share/SpinMutex.hpp"
#include "WtCore/TraderAdapter.h"
#include "WtCore/ParserAdapter.h"
#include "WtCore/WtExecMgr.h"
#include "WTSTools/WTSHotMgr.h"
#include "WTSTools/WTSBaseDataMgr.h"
#include "WtExecMon/WtSimpDataMgr.h"
#define private public
#include "WtExecMon/WtExecRunner.h"
#undef private

using namespace wtp;

namespace i03s2exec {

class FailingParserApi : public IParserApi
{
public:
	bool init(WTSVariant*) override { return false; }
};

int destroy_exec_runner_with_undrained_parser_candidate()
{
	ParserAdapterPtr candidate(new ParserAdapter);
	{
		WtExecRunner runner;
		if (candidate->initExt("i03s2_exec_undrained", new FailingParserApi(), &runner, nullptr, nullptr))
			return 10;
		if (!candidate->hasApi())
			return 11;
		// 与 WtExecRunner::initParsers 失败分支相同：候选留在 _parser_quarantine
		runner._parser_quarantine.push_back(candidate);
	}	// 析构；改前：shutdownRuntime() 因隔离候选返回 false → std::terminate()
	return candidate.use_count() >= 2 ? 0 : 12;
}

int destroy_exec_runner_without_candidates()
{
	{
		WtExecRunner runner;
	}
	return 0;
}

int run_in_child(int (*body)())
{
	const pid_t pid = ::fork();
	if (pid < 0)
		return -1;
	if (pid == 0)
		::_exit(body());
	int status = 0;
	if (::waitpid(pid, &status, 0) != pid)
		return -2;
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -3;
}

} // namespace i03s2exec

TEST(I03S2ExecRunnerTeardown, DestructorRetainsUndrainedParserCandidateInsteadOfAborting)
{
	const int rc = i03s2exec::run_in_child(&i03s2exec::destroy_exec_runner_with_undrained_parser_candidate);
	EXPECT_EQ(0, rc) << "child exit/signal code " << rc
		<< " (134 = SIGABRT: destructor terminated instead of retaining the undrained owner)";
}

TEST(I03S2ExecRunnerTeardown, DestructorWithoutCandidatesCompletes)
{
	EXPECT_EQ(0, i03s2exec::run_in_child(&i03s2exec::destroy_exec_runner_without_candidates));
}
