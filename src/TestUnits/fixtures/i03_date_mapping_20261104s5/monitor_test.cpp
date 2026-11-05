// BUG-R12-13 / I03-NEW-006 (S5): StateMonitor::advanceOnce must not move any
// state on an invalid date/time snapshot (including date 0, which the legacy
// helpers replaced with a new clock reading).
#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <unistd.h>

#include "Includes/WTSVariant.hpp"
#include "WTSTools/WTSBaseDataMgr.h"
#include "WTSTools/WTSLogger.h"
#include "WTSUtils/WTSCfgLoader.h"
#include "WtDtCore/DataManager.h"
#include "WtDtCore/StateMonitor.h"

// StateMonitor only reaches the data manager after a CLOSED state; the tests
// never get there. These stubs keep the executable free of WtDtCore's archive.
void DataManager::transHisData(const char*) { ADD_FAILURE() << "unexpected transHisData"; }
bool DataManager::isSessionProceeded(const char*) { ADD_FAILURE() << "unexpected isSessionProceeded"; return true; }

struct I03MonitorDateTestAccess
{
	static bool init(StateMonitor& monitor, WTSVariant* cfg, WTSBaseDataMgr* bd) { return monitor.initializeConfig(cfg, bd, nullptr); }
	static void advance(StateMonitor& monitor, uint32_t date, uint32_t time) { monitor.advanceOnce(date, time); }
};

namespace {
std::string writeConfig(const std::string& content)
{
	char name[] = "/tmp/wt-i03-s5-monitor-XXXXXX";
	const int fd = mkstemp(name);
	if (fd < 0) return {};
	close(fd);
	const std::string path = std::string(name) + ".json";
	if (rename(name, path.c_str()) != 0) return {};
	std::ofstream out(path);
	out << content;
	return path;
}

class MonitorSnapshot : public ::testing::Test
{
protected:
	WTSBaseDataMgr bd;
	StateMonitor monitor;
	void SetUp() override
	{
		static bool logger = [] { WTSLogger::init(R"({"root":{"level":"debug","async":false,"sinks":[]}})", false, nullptr); return true; }();
		(void)logger;
		const auto sessions = writeConfig(R"({"DAY":{"name":"day","offset":0,"sections":[{"from":900,"to":1500}]}})");
		const auto holidays = writeConfig(R"({"CHINA":[20240226]})");
		const auto commodities = writeConfig(R"({"EX":{"D":{"name":"day","session":"DAY","holiday":"CHINA","pricetick":1,"volscale":1}}})");
		ASSERT_TRUE(bd.loadSessions(sessions.c_str()));
		ASSERT_TRUE(bd.loadHolidays(holidays.c_str()));
		ASSERT_TRUE(bd.loadCommodities(commodities.c_str()));
		unlink(sessions.c_str()); unlink(holidays.c_str()); unlink(commodities.c_str());
		WTSVariant* cfg = WTSVariant::createObject();
		WTSVariant* item = WTSVariant::createObject();
		item->append("inittime", static_cast<uint32_t>(850));
		item->append("closetime", static_cast<uint32_t>(1515));
		item->append("proctime", static_cast<uint32_t>(1600));
		cfg->append("DAY", item, false);
		ASSERT_TRUE(I03MonitorDateTestAccess::init(monitor, cfg, &bd));
		ASSERT_TRUE(monitor.isInState("DAY", SS_ORIGINAL));
	}
};
}

TEST_F(MonitorSnapshot, InvalidSnapshotsDoNotMoveAnyState)
{
	for (const auto& snapshot : { std::make_pair(20240230u, 1000u), std::make_pair(0u, 1000u),
		std::make_pair(20240227u, 960u), std::make_pair(20240227u, 2401u), std::make_pair(9991231u, 1000u) })
	{
		I03MonitorDateTestAccess::advance(monitor, snapshot.first, snapshot.second);
		EXPECT_TRUE(monitor.isInState("DAY", SS_ORIGINAL)) << snapshot.first << "." << snapshot.second;
	}
}

TEST_F(MonitorSnapshot, ValidSnapshotsStillDriveTransitions)
{
	// Tuesday trading time: leaves the original state, not as a holiday.
	I03MonitorDateTestAccess::advance(monitor, 20240227, 1000);
	EXPECT_FALSE(monitor.isInState("DAY", SS_ORIGINAL));
	EXPECT_FALSE(monitor.isInState("DAY", SS_Holiday));
}

TEST_F(MonitorSnapshot, ConfiguredHolidayIsStillAHoliday)
{
	I03MonitorDateTestAccess::advance(monitor, 20240226, 1000);
	EXPECT_TRUE(monitor.isInState("DAY", SS_Holiday));
}
