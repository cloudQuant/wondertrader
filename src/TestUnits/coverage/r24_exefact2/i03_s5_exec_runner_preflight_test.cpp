/*
 * i03_s5_exec_runner_preflight_test.cpp (迭代03 s5-runners)
 *
 * 关联：BUG-R134-003、BUG-WSN06-001（卡8b=A）。WtExecMon 的 WtExecRunner::
 * initExecuters 在加载工厂、构造或登记任何执行器之前，对整张执行器表做纯预检：
 * 表中任一非法条目、未知 kind、表内重复 id 都使整表失败且不登记任何执行器。
 * 只用本地 fake：不加载 trader/parser 模块、不连接柜台、不下单。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/core/noncopyable.hpp>
#include "Includes/ExecuteDefs.h"
#include "Includes/FasterDefs.h"
#include "Includes/ITraderApi.h"
#include "Share/DLLHelper.hpp"
#include "Share/BoostFile.hpp"
#include "Share/StdUtils.hpp"
#include "Share/SpinMutex.hpp"
#include "WtCore/TraderAdapter.h"
#include "WtCore/ITrdNotifySink.h"
#include "WtCore/IExecCommand.h"
#include "WtCore/ExecUnitInitStatus.h"
#include "WtCore/WtExecuterFactory.h"
#include <Share/threadpool.hpp>
#include "WtCore/WtLocalExecuter.h"
#include "WtCore/WtDiffExecuter.h"
#include "WtCore/WtExecMgr.h"
#include "WtCore/ParserAdapter.h"
#include "WtCore/ActionPolicyMgr.h"
#include "WTSTools/WTSHotMgr.h"
#include "WTSTools/WTSBaseDataMgr.h"
#include "WtExecMon/WtSimpDataMgr.h"
#define private public
#include "WtExecMon/WtExecRunner.h"
#undef private

#include "Includes/WTSVariant.hpp"

using namespace wtp;

namespace
{
WTSVariant* makeExecuter(const char* kind, const char* id, bool validPolicy = true)
{
	WTSVariant* item = WTSVariant::createObject();
	item->append("id", id);
	item->append("active", true);
	item->append("name", kind);
	item->append("scale", 1.0);
	if (validPolicy)
	{
		WTSVariant* policy = WTSVariant::createObject();
		WTSVariant* def = WTSVariant::createObject();
		def->append("name", "WtExeFact.WtMinImpactExeUnit");
		policy->append("default", def, false);
		item->append("policy", policy, false);
	}
	return item;
}

WTSVariant* makeRoot(std::initializer_list<WTSVariant*> items)
{
	WTSVariant* executers = WTSVariant::createArray();
	for (WTSVariant* item : items)
		executers->append(item, false);
	WTSVariant* root = WTSVariant::createObject();
	root->append("executers", executers, false);
	return root;
}
}

TEST(I03S5ExecRunnerPreflight, InvalidEntryInMiddleRegistersNothing)
{
	WtExecRunner runner;
	WTSVariant* cfg = makeRoot({ makeExecuter("local", "i03s5_exec_first"),
		makeExecuter("local", "i03s5_exec_bad", false), makeExecuter("diff", "i03s5_exec_last") });
	EXPECT_FALSE(runner.initExecuters(cfg));
	cfg->release();
	EXPECT_EQ(nullptr, runner._exe_mgr.find_executer("i03s5_exec_first").get());
	EXPECT_EQ(nullptr, runner._exe_mgr.find_executer("i03s5_exec_bad").get());
	EXPECT_EQ(nullptr, runner._exe_mgr.find_executer("i03s5_exec_last").get());
	EXPECT_TRUE(runner.shutdownRuntime());
}

TEST(I03S5ExecRunnerPreflight, UnknownKindIsRejectedInsteadOfBecomingDist)
{
	WtExecRunner runner;
	WTSVariant* cfg = makeRoot({ makeExecuter("local", "i03s5_exec_ok"), makeExecuter("bogus", "i03s5_exec_kind") });
	EXPECT_FALSE(runner.initExecuters(cfg));
	cfg->release();
	EXPECT_EQ(nullptr, runner._exe_mgr.find_executer("i03s5_exec_ok").get());
	EXPECT_EQ(nullptr, runner._exe_mgr.find_executer("i03s5_exec_kind").get());
	EXPECT_TRUE(runner.shutdownRuntime());
}

TEST(I03S5ExecRunnerPreflight, DuplicateIdInTableRegistersNothing)
{
	WtExecRunner runner;
	WTSVariant* cfg = makeRoot({ makeExecuter("local", "i03s5_exec_dup"), makeExecuter("diff", "i03s5_exec_dup") });
	EXPECT_FALSE(runner.initExecuters(cfg));
	cfg->release();
	EXPECT_EQ(nullptr, runner._exe_mgr.find_executer("i03s5_exec_dup").get());
	EXPECT_TRUE(runner.shutdownRuntime());
}

TEST(I03S5ExecRunnerPreflight, ValidTableRegistersEveryExecuter)
{
	WtExecRunner runner;
	WTSVariant* cfg = makeRoot({ makeExecuter("local", "i03s5_exec_local"), makeExecuter("diff", "i03s5_exec_diff") });
	EXPECT_TRUE(runner.initExecuters(cfg));
	cfg->release();
	EXPECT_NE(nullptr, runner._exe_mgr.find_executer("i03s5_exec_local").get());
	EXPECT_NE(nullptr, runner._exe_mgr.find_executer("i03s5_exec_diff").get());
	EXPECT_TRUE(runner.shutdownRuntime());
}
