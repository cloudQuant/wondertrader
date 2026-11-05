/*
 * BUG-R104-002 / I03-NEW-003（迭代03 最终缺口，slice2）——WtRunner 的 parser required 选项。
 *  - K13=B：required 缺省 true，初始化失败使配置失败；required:false 失败只告警并继续；
 *  - K14：默认只报告部分连接命令未提交；required 项的连接命令未提交时回滚（stop+drain）
 *    并使启动失败——traders/engine 不再启动，runSucceeded 为 false。
 * 只用本地 fake：缺失模块、内存 parser API，不连接行情源。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Share/DLLHelper.hpp"
#include "Includes/ILogHandler.h"
#include "WtCore/EventNotifier.h"
#include "WtCore/CtaStrategyMgr.h"
#include "WtCore/HftStrategyMgr.h"
#include "WtCore/SelStrategyMgr.h"
#include "WtCore/WtCtaEngine.h"
#include "WtCore/WtHftEngine.h"
#include "WtCore/WtSelEngine.h"
#include "WtCore/WtExecMgr.h"
#include "WtCore/WtLocalExecuter.h"
#include "WtCore/WtDistExecuter.h"
#include "WtCore/TraderAdapter.h"
#include "WtCore/ParserAdapter.h"
#include "WtCore/WtDtMgr.h"
#include "WtCore/ActionPolicyMgr.h"
#include "WTSTools/WTSHotMgr.h"
#include "WTSTools/WTSLogger.h"
#include "WTSUtils/WTSCfgLoader.h"
#include "WTSTools/WTSBaseDataMgr.h"
#include "WtCore/CtaStraContext.h"
#include "WtCore/HftStraContext.h"
#define private public
#include "WtRunner/WtRunner.h"
#undef private
#include "Includes/WTSVariant.hpp"

namespace i03s2k13wt {

class CommandApi : public IParserApi
{
public:
	bool init(WTSVariant*) override { return true; }
	bool connect() override { ++calls; return result; }
	void registerSpi(IParserSpi*) override {}
	void subscribe(const CodeSet&) override {}
	bool result = false;
	int calls = 0;
};

class CountingEngine final : public WtEngine
{
public:
	void run() override { ++calls; }
	void on_bar(const char*, const char*, uint32_t, WTSBarStruct*) override {}
	int calls = 0;
};

// 与 r28 用例相同：关闭失败只允许是“保留 parser owner”，此时 runner/engine 存活到进程退出
std::vector<std::unique_ptr<WtRunner>>& retained_runners()
{
	static auto* runners = new std::vector<std::unique_ptr<WtRunner>>();
	return *runners;
}
std::vector<std::unique_ptr<CountingEngine>>& retained_engines()
{
	static auto* engines = new std::vector<std::unique_ptr<CountingEngine>>();
	return *engines;
}
void finish(std::unique_ptr<WtRunner>& runner, std::unique_ptr<CountingEngine>& engine)
{
	if (runner->shutdown())
	{
		runner.reset();
		return;
	}
	EXPECT_TRUE(runner->parserOwnersRetained()) << "shutdown failed for a reason other than retained parser owners";
	retained_runners().push_back(std::move(runner));
	retained_engines().push_back(std::move(engine));
}

WTSVariant* missing_module_parsers(const char* id, int required /* -1: key absent */)
{
	WTSVariant* parsers = WTSVariant::createArray();
	WTSVariant* item = WTSVariant::createObject();
	item->append("active", true);
	item->append("id", id);
	item->append("module", "i03s2_no_such_parser_module");
	if (required >= 0)
		item->append("required", required != 0);
	parsers->append(item, false);
	return parsers;
}

} // namespace i03s2k13wt

using namespace i03s2k13wt;

TEST(I03S2K13WtRunnerRequired, RequiredParserFailureFailsConfigurationByDefault)
{
	auto runner = std::make_unique<WtRunner>();
	auto engine = std::make_unique<CountingEngine>();
	runner->_engine = engine.get();
	for (int required : { -1, 1 })
	{
		WTSVariant* parsers = missing_module_parsers("i03s2_required", required);
		EXPECT_FALSE(runner->initParsers(parsers)) << "required=" << required;
		parsers->release();
		EXPECT_EQ(nullptr, runner->_parsers.getAdapter("i03s2_required").get());
	}
	EXPECT_TRUE(runner->_required_parsers.empty());
	finish(runner, engine);
}

TEST(I03S2K13WtRunnerRequired, OptionalParserFailureDegradesAndContinues)
{
	auto runner = std::make_unique<WtRunner>();
	auto engine = std::make_unique<CountingEngine>();
	runner->_engine = engine.get();
	WTSVariant* parsers = missing_module_parsers("i03s2_optional", 0);
	EXPECT_TRUE(runner->initParsers(parsers)) << "an optional parser failure must not fail the runner";
	parsers->release();
	EXPECT_EQ(nullptr, runner->_parsers.getAdapter("i03s2_optional").get()) << "a failed parser is never registered";
	EXPECT_EQ(0u, runner->_required_parsers.count("i03s2_optional"));
	finish(runner, engine);
}

TEST(I03S2K14WtRunnerRollback, RequiredParserWithoutSubmittedCommandRollsTheStartupBack)
{
	auto runner = std::make_unique<WtRunner>();
	auto engine = std::make_unique<CountingEngine>();
	runner->_engine = engine.get();
	auto parser = std::make_shared<ParserAdapter>();
	auto* api = new CommandApi();
	api->result = false;	// 连接命令未提交
	ASSERT_TRUE(parser->initExt("i03s2_req_cmd", api, engine.get(), &runner->_bd_mgr));
	ASSERT_TRUE(runner->_parsers.addAdapter("i03s2_req_cmd", parser));
	runner->_required_parsers.insert("i03s2_req_cmd");	// 等同于经 initParsers 登记的 required 项

	runner->run(true);
	EXPECT_EQ(1, api->calls);
	EXPECT_FALSE(runner->parserCommandsSubmitted());
	EXPECT_FALSE(runner->runSucceeded()) << "a required parser without a submitted command must fail the startup";
	EXPECT_EQ(0, engine->calls) << "rollback happens before the engine is started";
	finish(runner, engine);
}

TEST(I03S2K14WtRunnerRollback, OptionalParserWithoutSubmittedCommandIsOnlyReported)
{
	auto runner = std::make_unique<WtRunner>();
	auto engine = std::make_unique<CountingEngine>();
	runner->_engine = engine.get();
	auto parser = std::make_shared<ParserAdapter>();
	auto* api = new CommandApi();
	api->result = false;
	ASSERT_TRUE(parser->initExt("i03s2_opt_cmd", api, engine.get(), &runner->_bd_mgr));
	ASSERT_TRUE(runner->_parsers.addAdapter("i03s2_opt_cmd", parser));

	runner->run(true);
	EXPECT_FALSE(runner->parserCommandsSubmitted());
	EXPECT_TRUE(runner->runSucceeded()) << "K14 default: a partial submission is reported, not rolled back";
	EXPECT_EQ(1, engine->calls);
	finish(runner, engine);
}
