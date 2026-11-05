// 迭代03 最终缺口（slice2）：WtRtRunner 析构路径的两项运行时用例。
//
//  - I03-NEW-045：卡9a 下无法证明回调排空的失败 parser 候选（仍持有 API）是合同结果，
//    WtRtRunner 析构时应把它保留到进程结束并完成其余关闭，而不是 std::terminate。
//    子进程内构造本地 runner、注入候选后直接析构：期望正常退出且候选被进程级持有者保留。
//  - I03-NEW-046：porter 单例在进程退出期析构时，宿主（Python）解释器可能已 finalize，
//    析构期间不得再回调宿主；显式 release_porter() 仍按原样投递 EVENT_PARSER_RELEASE。
//    以显式调用全局 runner 析构函数模拟进程退出期析构（ExpParser::release 经 getRunner()
//    回到同一对象），再原地重建。
// 只用本地 fake：不加载 parser/trader 模块，不连接柜台。
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <string>
#include <system_error>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "Includes/IParserApi.h"
#include "Includes/WTSVariant.hpp"
#include "WtCore/ParserAdapter.h"
#include "WtPorter/WtPorter.h"
#include "WtPorter/PorterDefs.h"

#define private public
#include "../../../WtPorter/WtRtRunner.h"
#undef private

extern WtRtRunner& getRunner();

USING_NS_WTP;

namespace i03s2rt {

class FailingParserApi : public IParserApi
{
public:
	bool init(WTSVariant*) override { return false; }
};

std::atomic<int> g_release_events{ 0 };
std::atomic<int> g_other_events{ 0 };

void PORTER_FLAG count_parser_events(WtUInt32 evtId, const char*)
{
	if (evtId == EVENT_PARSER_RELEASE)
		g_release_events.fetch_add(1);
	else
		g_other_events.fetch_add(1);
}

void rebuild_global_runner()
{
	WtRtRunner& runner = getRunner();
	runner.WtRtRunner::~WtRtRunner();
	new (&runner) WtRtRunner();
}

// I03-NEW-045：子进程内运行，返回子进程退出码（信号终止返回 128+信号）
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

int destroy_runner_with_undrained_parser_candidate()
{
	ParserAdapterPtr candidate(new ParserAdapter);
	{
		WtRtRunner runner;
		runner._engine = &runner._cta_engine;
		// FailingParserApi 的 init 失败，但适配器已持有 API：无排空证明（卡9a）
		if (candidate->initExt("i03s2_undrained_parser", new FailingParserApi(), &runner, nullptr, nullptr))
			return 10;
		if (!candidate->hasApi())
			return 11;
		runner._quarantined_parser_owners.emplace_back(candidate);
	}	// 析构；改前：std::terminate()（SIGABRT）
	// 候选必须被进程级持有者保留，而不是随 runner 释放
	return candidate.use_count() >= 2 ? 0 : 12;
}

class I03S2PorterHostCallbacks : public ::testing::Test
{
protected:
	void SetUp() override
	{
		rebuild_global_runner();
		g_release_events.store(0);
		g_other_events.store(0);
		_dir = std::filesystem::temp_directory_path() / ("i03_s2_host_cb_" + std::to_string(static_cast<long>(::getpid())));
		std::error_code ec;
		std::filesystem::remove_all(_dir, ec);
		std::filesystem::create_directories(_dir, ec);
		ASSERT_FALSE(static_cast<bool>(ec));
		std::ofstream actions(_dir / "actions.json", std::ios::binary | std::ios::trunc);
		actions << "{\"default\":{}}";
		ASSERT_TRUE(actions.good());
		std::ofstream sessions(_dir / "sessions.yaml", std::ios::binary | std::ios::trunc);
		sessions << "ALLDAY:\n"
			"    name: AllDay\n"
			"    offset: 0\n"
			"    sections:\n"
			"        - from: 0\n"
			"          to: 0\n";
		ASSERT_TRUE(sessions.good());
	}

	void TearDown() override
	{
		register_parser_callbacks(NULL, NULL);
		rebuild_global_runner();
		std::error_code ec;
		std::filesystem::remove_all(_dir, ec);
	}

	bool configure_with_ext_parser(const char* parserId)
	{
		const std::string absentLog = (_dir / "no_log_config.yaml").string();
		getRunner().init(absentLog.c_str(), true, _dir.string().c_str());
		const std::string basefiles = (_dir / "sessions.yaml").generic_string();
		const std::string actionFile = (_dir / "actions.json").generic_string();
		const std::string cfg = "{\"basefiles\":{\"session\":\"" + basefiles + "\"},"
			"\"env\":{\"name\":\"cta\",\"product\":{\"session\":\"ALLDAY\"}},"
			"\"data\":{},\"bspolicy\":\"" + actionFile + "\","
			"\"parsers\":[],\"traders\":[],\"executers\":[],"
			"\"strategies\":{\"cta\":[{\"active\":false,\"id\":\"i03s2_off\",\"name\":\"i03s2_off\"}]}}";
		if (!getRunner().config(cfg.c_str(), false))
			return false;
		register_parser_callbacks(&count_parser_events, NULL);
		return create_ext_parser(parserId);
	}

	std::filesystem::path _dir;
};

} // namespace i03s2rt

using namespace i03s2rt;

TEST(I03S2RtRunnerTeardown, DestructorRetainsUndrainedParserCandidateInsteadOfAborting)
{
	const int rc = run_in_child(&destroy_runner_with_undrained_parser_candidate);
	EXPECT_EQ(0, rc) << "child exit/signal code " << rc
		<< " (134 = SIGABRT: destructor terminated instead of retaining the undrained owner)";
}

TEST_F(I03S2PorterHostCallbacks, ExplicitReleaseStillNotifiesTheHostOnce)
{
	ASSERT_TRUE(configure_with_ext_parser("i03s2_host_cb_release"));
	getRunner().release();
	EXPECT_EQ(1, g_release_events.load()) << "explicit release_porter keeps the orderly host notification";
}

TEST_F(I03S2PorterHostCallbacks, ProcessExitDestructionDoesNotCallBackIntoTheHost)
{
	ASSERT_TRUE(configure_with_ext_parser("i03s2_host_cb_exit"));
	const int othersBefore = g_other_events.load();
	// 进程退出期：porter 单例析构；ExpParser::release 经 getRunner() 回到正在析构的同一对象
	WtRtRunner& runner = getRunner();
	runner.WtRtRunner::~WtRtRunner();
	EXPECT_EQ(0, g_release_events.load()) << "no host callback once process-exit teardown began";
	EXPECT_EQ(othersBefore, g_other_events.load());
	new (&runner) WtRtRunner();
}
