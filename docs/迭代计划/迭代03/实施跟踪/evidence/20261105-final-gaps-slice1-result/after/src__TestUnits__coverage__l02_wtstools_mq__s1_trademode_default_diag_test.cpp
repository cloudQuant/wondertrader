// BUG-L03-002 (iteration 03, slice1 final gaps): decision card 4 follow-up of
// 2026-11-04 - a commodity without "trademode" keeps the documented TM_Both
// loader default, and the omission is reported once per commodities file
// (count plus one example) instead of being silent or repeated per product.
//
// WTSLogger is process-global, so the logging assertions run in a re-executed
// child process (same pattern as R43ReplayerInitBasefilesTest diagnostics).
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "Includes/ILogHandler.h"
#include "Includes/WTSContractInfo.hpp"
#include "WTSTools/WTSBaseDataMgr.h"
#include "WTSTools/WTSLogger.h"
#include "r56_tmp.h"

USING_NS_WTP;

namespace
{
const char* kS1TradeModeChildEnv = "WT_S1_TRADEMODE_DIAG_CHILD";

const char* kS1Sessions = R"({
	"S1DAY": {"name": "day", "offset": 0, "sections": [{"from": 900, "to": 1130}, {"from": 1300, "to": 1500}]}
})";

// rb and hc omit trademode; ag is explicit (TM_Long = 1).
const char* kS1MixedCommodities = R"({
	"SHFE": {
		"rb": {"name": "rb", "session": "S1DAY", "holiday": "CHINA", "pricetick": 1, "volscale": 10},
		"hc": {"name": "hc", "session": "S1DAY", "holiday": "CHINA", "pricetick": 1, "volscale": 10},
		"ag": {"name": "ag", "session": "S1DAY", "holiday": "CHINA", "pricetick": 1, "volscale": 15, "trademode": 1}
	}
})";

const char* kS1ExplicitCommodities = R"({
	"DCE": {
		"i": {"name": "i", "session": "S1DAY", "holiday": "CHINA", "pricetick": 0.5, "volscale": 100, "trademode": 0}
	}
})";

class S1WarnCapture final : public ILogHandler
{
public:
	void handleLogAppend(WTSLogLevel level, const char* message) override
	{
		if (level == LL_WARN && message != nullptr)
			warnings.emplace_back(message);
	}

	std::vector<std::string> matching(const char* needle) const
	{
		std::vector<std::string> out;
		for (const auto& m : warnings)
			if (m.find(needle) != std::string::npos)
				out.push_back(m);
		return out;
	}

	std::vector<std::string> warnings;
};

// Returns 0 when every check holds, otherwise a distinct non-zero code so the
// parent's exit-status assertion identifies the failing check.
int runChild()
{
	S1WarnCapture capture;
	WTSLogger::init(R"({"root":{"level":"info","async":false,"sinks":[]}})", false, &capture);

	const std::filesystem::path dir = r56tmp::unique_dir("s1_trademode");
	const std::string sessions = r56tmp::write_file(dir, "sessions.json", kS1Sessions);
	const std::string mixed = r56tmp::write_file(dir, "commodities_mixed.json", kS1MixedCommodities);
	const std::string explicitOnly = r56tmp::write_file(dir, "commodities_explicit.json", kS1ExplicitCommodities);

	int rc = 0;
	{
		WTSBaseDataMgr mgr;
		if (!mgr.loadSessions(sessions.c_str()))
			rc = 10;
		else if (!mgr.loadCommodities(mixed.c_str()))
			rc = 11;
		else
		{
			WTSCommodityInfo* rb = mgr.getCommodity("SHFE.rb");
			WTSCommodityInfo* hc = mgr.getCommodity("SHFE.hc");
			WTSCommodityInfo* ag = mgr.getCommodity("SHFE.ag");
			const std::vector<std::string> notes = capture.matching("have no trademode");
			if (rb == nullptr || hc == nullptr || ag == nullptr)
				rc = 12;
			else if (rb->getTradingMode() != TM_Both || hc->getTradingMode() != TM_Both || !rb->canShort() || !hc->canShort())
				rc = 13;	// missing trademode keeps the TM_Both default
			else if (ag->getTradingMode() != TM_Long || ag->canShort())
				rc = 14;	// explicit value is still honoured
			else if (notes.size() != 1)
				rc = 15;	// exactly one diagnostic for the file, not one per product
			else if (notes[0].find("2 commodities") == std::string::npos || notes[0].find(mixed) == std::string::npos ||
				(notes[0].find("SHFE.rb") == std::string::npos && notes[0].find("SHFE.hc") == std::string::npos))
				rc = 16;	// count, file and one example are reported
			else if (notes[0].find("SHFE.ag") != std::string::npos)
				rc = 17;	// the explicit product is not reported
			else
			{
				const size_t before = capture.matching("have no trademode").size();
				if (!mgr.loadCommodities(explicitOnly.c_str()))
					rc = 18;
				else if (capture.matching("have no trademode").size() != before)
					rc = 19;	// a file with explicit modes produces no diagnostic
			}
		}
	}

	r56tmp::clean_dir(dir);
	WTSLogger::registerHandler(nullptr);
	return rc;
}
}	// namespace

TEST(S1TradeModeDefault, MissingTradeModeKeepsTmBothAndIsReportedOncePerFile)
{
	if (std::getenv(kS1TradeModeChildEnv) != nullptr)
	{
		std::_Exit(runChild());
	}

	const auto args = ::testing::internal::GetArgvs();
	ASSERT_FALSE(args.empty());
	const std::string executable = std::filesystem::absolute(args[0]).string();
	const pid_t child = ::fork();
	ASSERT_GE(child, 0);
	if (child == 0)
	{
		::alarm(30);
		::setenv(kS1TradeModeChildEnv, "1", 1);
		::execl(executable.c_str(), executable.c_str(),
			"--gtest_filter=S1TradeModeDefault.MissingTradeModeKeepsTmBothAndIsReportedOncePerFile",
			static_cast<char*>(nullptr));
		::_exit(127);
	}
	int status = 0;
	ASSERT_EQ(child, ::waitpid(child, &status, 0));
	ASSERT_TRUE(WIFEXITED(status)) << "child terminated abnormally";
	EXPECT_EQ(0, WEXITSTATUS(status)) << "child check code (see runChild)";
}
