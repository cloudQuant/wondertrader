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

#include <map>

/*
 * 迭代03 K50（I03-NEW-089，用户裁定 2026-11-06）：WtRunner 的人工合约归属释放请求目录。
 * 未知 trader 的合法请求被账户门禁拒绝并归档为 .rejected；缺字段的请求归档为 .invalid；
 * 已归档的请求不会再被处理。请求解析与门禁语义见 wtcore 侧 i03_k50 用例。
 */
TEST(I03K50WtRunnerClaimInbox, RequestsAreProcessedAndArchived)
{
	namespace fs = std::filesystem;
	const fs::path dir = fs::temp_directory_path() /
		("wt_i03_k50_runner_" + std::to_string(static_cast<long long>(::getpid())));
	std::error_code ec;
	fs::remove_all(dir, ec);
	fs::create_directories(dir, ec);
	{
		std::ofstream out((dir / "a_unknown_trader.json").string(), std::ios::binary);
		out << R"({"trader":"i03_k50_no_such_trader","code":"SHFE.ag.2512","operator":"alice","reason":"closed"})";
	}
	{
		std::ofstream out((dir / "b_missing_reason.json").string(), std::ios::binary);
		out << R"({"trader":"t","code":"SHFE.ag.2512","operator":"alice"})";
	}

	std::unique_ptr<WtRunner> runner(new WtRunner());
	runner->setManualClaimFolder(dir.string());
	EXPECT_EQ(2u, runner->processManualClaimRequests());

	std::map<std::string, std::string> suffix;
	for (const auto& entry : fs::directory_iterator(dir, ec))
	{
		const std::string name = entry.path().filename().string();
		const std::size_t json = name.find(".json");
		if (json != std::string::npos)
			suffix[name.substr(0, json + 5)] = name.substr(name.rfind('.') + 1);
	}
	EXPECT_EQ("rejected", suffix["a_unknown_trader.json"]);
	EXPECT_EQ("invalid", suffix["b_missing_reason.json"]);
	EXPECT_EQ(0u, runner->processManualClaimRequests());
	fs::remove_all(dir, ec);
}
