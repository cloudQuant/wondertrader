#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "WtCore/WtHelper.h"
#include "WtPorter/WtRtRunner.h"

namespace {

class PorterFeeConfigTempDir
{
public:
	PorterFeeConfigTempDir()
	{
		_path = std::filesystem::temp_directory_path() /
			("wt_r240_porter_fee_" + std::to_string(static_cast<long long>(::getpid())));
		std::error_code ec;
		std::filesystem::remove_all(_path, ec);
		std::filesystem::create_directories(_path, ec);
		WtHelper::setGenerateDir((_path.string() + "/").c_str());
	}

	~PorterFeeConfigTempDir()
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

	const std::filesystem::path& path() const { return _path; }

private:
	std::filesystem::path _path;
};

} // namespace

TEST(WtRtRunnerFeeConfig, InvalidKeyRejectsPublicConfigBeforeEngineIsPublished)
{
	PorterFeeConfigTempDir temp;
	const std::string fees = temp.write("fees.json",
		"{\"SHFE.ag\":{\"open\":10,\"close\":10,\"closetoday\":10,\"byvolume\":true},"
		"\"broken\":{\"open\":1}}");
	const std::string policy = temp.write("action.yaml", "default: {}\n");
	const std::string config = "{\"basefiles\":{},\"env\":{\"name\":\"cta\",\"fees\":\"" + fees +
		"\",\"filters\":\"\"},\"data\":{},\"bspolicy\":\"" + policy + "\"}";

	// Porter config failure may leave owners for an explicit control-thread
	// release, so follow the existing runner-config tests and keep this runner
	// alive through process teardown.
	WtRtRunner* runner = new WtRtRunner();
	ASSERT_TRUE(runner->init((temp.path() / "missing_logcfg.yaml").string().c_str(), true,
		temp.path().string().c_str()));
	EXPECT_FALSE(runner->config(config.c_str(), false));
	EXPECT_EQ(nullptr, runner->getEngine());
}
