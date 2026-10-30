#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>

#include <unistd.h>

#include "Includes/ILogHandler.h"
#include "WtUftCore/EventNotifier.h"
#include "WtUftCore/UftStrategyMgr.h"
#include "WtUftCore/WtUftEngine.h"
#include "WtUftCore/ParserAdapter.h"
#include "WtUftCore/WtUftDtMgr.h"
#include "WtUftCore/ActionPolicyMgr.h"
#include "WtUftCore/TraderAdapter.h"
#include "WTSTools/WTSHotMgr.h"
#include "WTSTools/WTSBaseDataMgr.h"

#define private public
#include "WtUftRunner/WtUftRunner.h"
#undef private

namespace {
namespace fs = std::filesystem;

class ScopedTempCwd
{
public:
	explicit ScopedTempCwd(const char* tag)
	{
		std::error_code ec;
		_old = fs::current_path(ec);
		if (ec)
			return;
		_dir = fs::temp_directory_path(ec) /
			(std::string("wt_r78_parser_") + tag + "_" + std::to_string(::getpid()));
		if (ec)
			return;
		fs::remove_all(_dir, ec);
		ec.clear();
		fs::create_directories(_dir, ec);
		if (ec)
			return;
		fs::current_path(_dir, ec);
		_ok = !ec;
	}

	~ScopedTempCwd()
	{
		std::error_code ec;
		if (_ok)
			fs::current_path(_old, ec);
		if (!_dir.empty())
			fs::remove_all(_dir, ec);
	}

	bool ok() const { return _ok; }

private:
	fs::path _old;
	fs::path _dir;
	bool _ok = false;
};

bool writeText(const char* path, const char* contents)
{
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out)
		return false;
	out << contents;
	out.close();
	return !out.fail();
}

}

TEST(R78UftRunnerParserRegistration, failedActiveParserIsNotRegistered)
{
	ScopedTempCwd cwd("failed_adapter");
	ASSERT_TRUE(cwd.ok());
	ASSERT_TRUE(writeText("sessions.yaml",
		"ALLDAY:\n"
		"    name: AllDay\n"
		"    offset: 0\n"
		"    sections:\n"
		"        - from: 0\n"
		"          to: 2400\n"));
	ASSERT_TRUE(writeText("action-policy.json",
		"{\"default\":{\"order\":[{\"action\":\"open\",\"limit\":100}]}}"));
	ASSERT_TRUE(writeText("runner.yaml",
		"basefiles:\n"
		"    session: sessions.yaml\n"
		"bspolicy: action-policy.json\n"
		"env:\n"
		"    product:\n"
		"        session: ALLDAY\n"
		"data:\n"
		"    unused: 1\n"
		"parsers:\n"
		"    - active: true\n"
		"      id: parser_missing\n"
		"      module: no_such_parser_module\n"
		"    - active: false\n"
		"      id: parser_inactive\n"
		"      module: no_such_parser_module\n"));

	WtUftRunner runner;
	EXPECT_FALSE(runner.config("runner.yaml"));
	EXPECT_TRUE(runner._parsers._adapters.empty());
	EXPECT_TRUE(runner._quarantined_parsers.empty());
	EXPECT_FALSE(runner._parser_quarantine_pending.load(std::memory_order_acquire));
}

