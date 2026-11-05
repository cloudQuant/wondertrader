#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>

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
#include "WTSTools/WTSBaseDataMgr.h"

#define private public
#include "WtRunner/WtRunner.h"
#undef private

#include "Includes/WTSVariant.hpp"
#include "WTSUtils/WTSCfgLoader.h"
#include "WtCore/WtHelper.h"

namespace {

class RiskTempDir
{
public:
	RiskTempDir()
	{
		_path = std::filesystem::temp_directory_path() /
			("wt_i03_k54_runner_" + std::to_string(static_cast<long long>(::getpid())));
		std::error_code ec;
		std::filesystem::remove_all(_path, ec);
		std::filesystem::create_directories(_path, ec);
		WtHelper::setGenerateDir((_path.string() + "/").c_str());
	}
	~RiskTempDir()
	{
		std::error_code ec;
		std::filesystem::remove_all(_path, ec);
	}
private:
	std::filesystem::path _path;
};

// 返回 initEngine 的结果；engineOut 收到 runner 的 _engine 是否为空
bool initEngineWith(const char* riskmonJson, bool& engineSet)
{
	std::string configText = "{\"env\":{\"name\":\"cta\",\"product\":{\"session\":\"TRADING\"},"
		"\"fees\":\"\",\"filters\":\"\"";
	if (riskmonJson != NULL)
		configText += std::string(",\"riskmon\":") + riskmonJson;
	configText += "}}";
	WTSVariant* config = WTSCfgLoader::load_from_content(configText);
	EXPECT_NE(nullptr, config);
	if (config == NULL)
		return false;
	std::unique_ptr<WtRunner> runner(new WtRunner());
	runner->_config = config;
	const bool ok = runner->initEngine();
	engineSet = runner->_engine != nullptr;
	runner->_config->release();
	runner->_config = nullptr;
	return ok;
}

} // namespace

/*
 * 迭代03 K54（BUG-R211-001，用户裁定 2026-11-06）：riskmon.active=true 而风控模块加载失败时，
 * WtRunner::initEngine 返回 false 且不暴露 engine（随后 config 失败，不进入交易）。
 * 未配置 riskmon 或 active=false 时行为不变。
 */
TEST(I03K54WtRunnerRiskmon, ActiveRiskmonLoadFailureRejectsInitEngine)
{
	RiskTempDir temp;
	bool engineSet = true;
	EXPECT_FALSE(initEngineWith("{\"active\":true,\"module\":\"i03_k54_no_such_riskmon\",\"name\":\"x\"}", engineSet));
	EXPECT_FALSE(engineSet);
}

TEST(I03K54WtRunnerRiskmon, InactiveOrAbsentRiskmonKeepsInitEngine)
{
	RiskTempDir temp;
	bool engineSet = false;
	EXPECT_TRUE(initEngineWith(NULL, engineSet));
	EXPECT_TRUE(engineSet);
	engineSet = false;
	EXPECT_TRUE(initEngineWith("{\"active\":false,\"module\":\"i03_k54_no_such_riskmon\",\"name\":\"x\"}", engineSet));
	EXPECT_TRUE(engineSet);
}
