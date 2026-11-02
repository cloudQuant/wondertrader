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

class FeeConfigTempDir
{
public:
	FeeConfigTempDir()
	{
		_path = std::filesystem::temp_directory_path() /
			("wt_r240_fee_runner_" + std::to_string(static_cast<long long>(::getpid())));
		std::error_code ec;
		std::filesystem::remove_all(_path, ec);
		std::filesystem::create_directories(_path, ec);
		WtHelper::setGenerateDir((_path.string() + "/").c_str());
	}

	~FeeConfigTempDir()
	{
		std::error_code ec;
		std::filesystem::remove_all(_path, ec);
	}

	std::string write(const std::string& name, const std::string& content) const
	{
		const auto file = _path / name;
		std::ofstream out(file.string(), std::ios::binary | std::ios::trunc);
		out << content;
		return file.string();
	}

private:
	std::filesystem::path _path;
};

} // namespace

TEST(WtRunnerFeeConfig, InvalidKeyFailsInitEngineBeforeRunnerCanStart)
{
	FeeConfigTempDir temp;
	const std::string fees = temp.write("fees.json",
		"{\"SHFE.ag\":{\"open\":10,\"close\":10,\"closetoday\":10,\"byvolume\":true},"
		"\"broken\":{\"open\":1}}");
	const std::string configText = "{\"env\":{\"name\":\"cta\",\"fees\":\"" + fees +
		"\",\"filters\":\"\"}}";
	WTSVariant* config = WTSCfgLoader::load_from_content(configText);
	ASSERT_NE(nullptr, config);

	std::unique_ptr<WtRunner> runner(new WtRunner());
	runner->_config = config;
	EXPECT_FALSE(runner->initEngine());
	EXPECT_EQ(nullptr, runner->_engine);
	EXPECT_FALSE(runner->_cta_engine.fee_config_valid());

	runner->_config->release();
	runner->_config = nullptr;
}
