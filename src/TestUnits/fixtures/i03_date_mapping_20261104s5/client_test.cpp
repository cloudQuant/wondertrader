// BUG-R12-13 / BUG-L03-009 / BUG-L03-010 / BUG-L01-018 (local part), S5 package.
// DateMappingClient (decision card 1 A+C), checked calendar arithmetic and
// trading-section overlap rules (decision card 2: only 0-0 is full day).
#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

#include "fakes.hpp"
#include "Includes/DateMappingClient.hpp"
#include "WTSTools/WTSBaseDataMgr.h"
#include "WTSTools/WTSLogger.h"

using namespace wtp;

namespace {
std::string writeConfig(const std::string& content)
{
	char name[] = "/tmp/wt-i03-s5-date-XXXXXX";
	const int fd = mkstemp(name);
	if (fd < 0) return {};
	close(fd);
	const std::string path = std::string(name) + ".json";
	if (rename(name, path.c_str()) != 0) return {};
	std::ofstream out(path);
	out << content;
	return path;
}

void initLogger()
{
	static bool done = [] {
		WTSLogger::init(R"({"root":{"level":"debug","async":false,"sinks":[]}})", false, nullptr);
		return true;
	}();
	(void)done;
}

struct ClockGuard
{
	ClockGuard() { setenv("TZ", "UTC", 1); tzset(); FakeClock::reset(); }
	~ClockGuard() { FakeClock::reset(); }
};
}

TEST(I03S5DateClient, LegacyRejectsInvalidInputBeforeAnyVirtualCall)
{
	FakeLegacyMgr mgr;
	DateMappingClient client(&mgr);
	EXPECT_EQ(DateMappingStatus::InvalidArgument, client.calcTradingDate(nullptr, 20240227, 900).status);
	EXPECT_EQ(DateMappingStatus::InvalidArgument, client.calcTradingDate("", 20240227, 900).status);
	EXPECT_EQ(DateMappingStatus::InvalidTime, client.calcTradingDate("EX.P", 20240227, 960).status);
	EXPECT_EQ(DateMappingStatus::InvalidTime, client.calcTradingDate("EX.P", 20240227, 2401).status);
	EXPECT_EQ(DateMappingStatus::InvalidDate, client.calcTradingDate("EX.P", 20240230, 900).status);
	EXPECT_EQ(DateMappingStatus::InvalidDate, client.calcTradingDate("EX.P", 20241301, 900).status);
	EXPECT_EQ(DateMappingStatus::OutOfRange, client.calcTradingDate("EX.P", 9991231, 900).status);
	EXPECT_EQ(DateMappingStatus::InvalidDate, client.getBoundaryTime("EX.P", 20230229).status);
	EXPECT_EQ(DateMappingStatus::InvalidArgument, client.getBoundaryTime(nullptr, 20240227).status);
	const auto failed = client.calcTradingDate("EX.P", 20240230, 900);
	EXPECT_EQ(0u, failed.value);
	EXPECT_TRUE(mgr.calcCalls.empty());
	EXPECT_TRUE(mgr.boundaryCalls.empty());
}

TEST(I03S5DateClient, LegacyRequiresCommoditySessionAndCalendarName)
{
	{
		FakeLegacyMgr mgr;
		mgr.hasCommodity = false;
		DateMappingClient client(&mgr);
		EXPECT_EQ(DateMappingStatus::MissingCommodity, client.calcTradingDate("EX.P", 20240227, 900).status);
		EXPECT_EQ(DateMappingStatus::MissingCommodity, client.getBoundaryTime("EX.P", 20240227).status);
		EXPECT_EQ(DateMappingStatus::MissingSession, client.calcTradingDate("NOPE", 20240227, 900, true).status);
		EXPECT_EQ(DateMappingStatus::Ok, client.calcTradingDate("DAY", 20240227, 900, true).status);
		EXPECT_EQ(1u, mgr.calcCalls.size());
	}
	{
		FakeLegacyMgr mgr("CHINA", false);
		DateMappingClient client(&mgr);
		EXPECT_EQ(DateMappingStatus::MissingSession, client.calcTradingDate("EX.P", 20240227, 900).status);
		EXPECT_TRUE(mgr.calcCalls.empty());
	}
	{
		FakeLegacyMgr mgr("CHINA", true, 0);
		DateMappingClient client(&mgr);
		EXPECT_EQ(DateMappingStatus::MissingSession, client.calcTradingDate("EX.P", 20240227, 900).status);
		EXPECT_EQ(DateMappingStatus::MissingSession, client.getBoundaryTime("EX.P", 20240227).status);
		EXPECT_TRUE(mgr.calcCalls.empty());
	}
	{
		FakeLegacyMgr mgr("");
		DateMappingClient client(&mgr);
		EXPECT_EQ(DateMappingStatus::MissingCalendar, client.calcTradingDate("EX.P", 20240227, 900).status);
		EXPECT_TRUE(mgr.calcCalls.empty());
	}
	{
		DateMappingClient client(static_cast<IBaseDataMgr*>(nullptr));
		EXPECT_EQ(DateMappingStatus::Unsupported, client.calcTradingDate("EX.P", 20240227, 900).status);
	}
}

TEST(I03S5DateClient, LegacyRejectsInvalidResultsAndExceptions)
{
	FakeLegacyMgr mgr;
	DateMappingClient client(&mgr);
	const auto ok = client.calcTradingDate("EX.P", 20240227, 900);
	EXPECT_EQ(DateMappingStatus::Ok, ok.status);
	EXPECT_EQ(20240227u, ok.value);
	mgr.overrideCalc = true;
	for (uint32_t bad : {0u, 20240230u, 20241301u, 9991231u})
	{
		mgr.calcValue = bad;
		const auto r = client.calcTradingDate("EX.P", 20240227, 900);
		EXPECT_EQ(DateMappingStatus::InternalError, r.status) << bad;
		EXPECT_EQ(0u, r.value);
	}
	mgr.overrideCalc = false;
	mgr.throwOnCalc = true;
	EXPECT_EQ(DateMappingStatus::InternalError, client.calcTradingDate("EX.P", 20240227, 900).status);

	const auto start = client.getBoundaryTime("EX.P", 20240227, false, true);
	EXPECT_EQ(DateMappingStatus::Ok, start.status);
	EXPECT_EQ(202402270900ULL, start.value);
	mgr.overrideBoundary = true;
	for (uint64_t bad : {0ULL, 202402300900ULL, 202402272460ULL, 202402272401ULL, 20240227090000000ULL})
	{
		mgr.boundaryValue = bad;
		const auto r = client.getBoundaryTime("EX.P", 20240227, false, false);
		EXPECT_EQ(DateMappingStatus::InternalError, r.status) << bad;
		EXPECT_EQ(0u, r.value);
	}
	mgr.boundaryValue = 202402272400ULL; // 2400 is a valid endpoint label
	EXPECT_EQ(DateMappingStatus::Ok, client.getBoundaryTime("EX.P", 20240227, false, false).status);
}

// Path C keeps the legacy meaning of date 0 ("the manager's current date") and
// only requires one successful checked clock snapshot first.
TEST(I03S5DateClient, LegacyDateZeroKeepsManagerCurrentAndClockFailureStops)
{
	ClockGuard guard;
	FakeLegacyMgr mgr;
	mgr.currentDate = 20240105; // simulated "current" differs from the wall clock
	{
		DateMappingClient client(&mgr);
		FakeClock::armed = true;
		FakeClock::seconds = { 1709078399, 1709078401 };
		const auto first = client.calcTradingDate("EX.P", 0, 0);
		const auto second = client.calcTradingDate("EX.P", 0, 0);
		const auto boundary = client.getBoundaryTime("EX.P", 0, false, true);
		FakeClock::armed = false;
		EXPECT_EQ(1, FakeClock::calls);
		ASSERT_EQ(DateMappingStatus::Ok, first.status);
		EXPECT_EQ(20240105u, first.value);
		EXPECT_EQ(first.value, second.value);
		EXPECT_EQ(202401050900ULL, boundary.value);
		ASSERT_EQ(2u, mgr.calcCalls.size());
		EXPECT_EQ(0u, mgr.calcCalls[0].date);
		EXPECT_EQ(0u, mgr.boundaryCalls.at(0).date);
		// A manager answering date 0 with an invalid date is rejected.
		mgr.currentDate = 20240230;
		EXPECT_EQ(DateMappingStatus::InternalError, client.calcTradingDate("EX.P", 0, 0).status);
	}
	mgr.calcCalls.clear();
	mgr.boundaryCalls.clear();
	{
		DateMappingClient client(&mgr);
		FakeClock::reset();
		FakeClock::armed = true;
		FakeClock::fail = true;
		const auto r = client.calcTradingDate("EX.P", 0, 0);
		const auto b = client.getBoundaryTime("EX.P", 0);
		FakeClock::reset();
		EXPECT_EQ(DateMappingStatus::ClockFailure, r.status);
		EXPECT_EQ(DateMappingStatus::ClockFailure, b.status);
		EXPECT_TRUE(mgr.calcCalls.empty());
		EXPECT_TRUE(mgr.boundaryCalls.empty());
		// Explicit dates do not need the clock.
		EXPECT_EQ(DateMappingStatus::Ok, client.calcTradingDate("EX.P", 20240227, 900).status);
	}
}

class I03S5DateClientV1 : public ::testing::Test
{
protected:
	WTSBaseDataMgr mgr;
	void SetUp() override
	{
		initLogger();
		const auto sessions = writeConfig(R"({"POS":{"name":"pos","offset":300,"sections":[{"from":2100,"to":2300},{"from":900,"to":1500}]},"DAY":{"name":"day","offset":0,"sections":[{"from":900,"to":1500}]}})");
		const auto holidays = writeConfig(R"({"CHINA":[20240226]})");
		const auto commodities = writeConfig(R"({"EX":{"P":{"name":"pos","session":"POS","holiday":"CHINA","pricetick":1,"volscale":1},"D":{"name":"day","session":"DAY","holiday":"CHINA","pricetick":1,"volscale":1},"M":{"name":"missing","session":"DAY","holiday":"MISSING","pricetick":1,"volscale":1}}})");
		ASSERT_TRUE(mgr.loadSessions(sessions.c_str()));
		ASSERT_TRUE(mgr.loadHolidays(holidays.c_str()));
		ASSERT_TRUE(mgr.loadCommodities(commodities.c_str()));
		unlink(sessions.c_str()); unlink(holidays.c_str()); unlink(commodities.c_str());
	}
};

TEST_F(I03S5DateClientV1, TablePathMatchesCheckedManagerAndLegacyPathOnValidInput)
{
	const DateMappingV1 table = mgr.getDateMappingV1();
	DateMappingClient typed(&table);
	DateMappingClient legacy(static_cast<IBaseDataMgr*>(&mgr));
	struct Case { const char* id; uint32_t date; uint32_t time; };
	for (const Case& c : { Case{"EX.P", 20240223, 2200}, Case{"EX.P", 20240224, 100}, Case{"EX.P", 20240227, 1000},
		Case{"EX.D", 20240224, 1000}, Case{"EX.D", 20240227, 1000}, Case{"EX.P", 20240225, 2200} })
	{
		const auto expected = mgr.checkedCalcTradingDate(c.id, c.date, c.time, false);
		const auto viaTable = typed.calcTradingDate(c.id, c.date, c.time);
		const auto viaLegacy = legacy.calcTradingDate(c.id, c.date, c.time);
		ASSERT_EQ(DateMappingStatus::Ok, expected.status) << c.id << " " << c.date;
		EXPECT_EQ(expected.status, viaTable.status);
		EXPECT_EQ(expected.value, viaTable.value) << c.id << " " << c.date << " " << c.time;
		EXPECT_EQ(expected.status, viaLegacy.status);
		EXPECT_EQ(expected.value, viaLegacy.value) << c.id << " " << c.date << " " << c.time;
		for (bool start : {true, false})
		{
			const auto eb = mgr.checkedGetBoundaryTime(c.id, c.date, false, start);
			EXPECT_EQ(eb.value, typed.getBoundaryTime(c.id, c.date, false, start).value);
			EXPECT_EQ(eb.value, legacy.getBoundaryTime(c.id, c.date, false, start).value);
		}
	}
	// Holiday 20240226 (Monday) is skipped from the Saturday night.
	EXPECT_EQ(20240227u, typed.calcTradingDate("EX.P", 20240224, 100).value);
}

TEST_F(I03S5DateClientV1, TableDateZeroUsesOneCheckedSnapshot)
{
	ClockGuard guard;
	const DateMappingV1 table = mgr.getDateMappingV1();
	DateMappingClient typed(&table);
	FakeClock::armed = true;
	FakeClock::seconds = { 1709078399, 1709078401 }; // 2024-02-27 23:59:59 (Tue), then 2024-02-28
	const auto first = typed.calcTradingDate("EX.D", 0, 0);
	const auto second = typed.calcTradingDate("EX.D", 0, 0);
	const auto start = typed.getBoundaryTime("EX.D", 0, false, true);
	FakeClock::armed = false;
	EXPECT_EQ(1, FakeClock::calls);
	ASSERT_EQ(DateMappingStatus::Ok, first.status);
	EXPECT_EQ(20240227u, first.value);
	EXPECT_EQ(first.value, second.value);
	EXPECT_EQ(202402270900ULL, start.value);
	FakeClock::reset();
	DateMappingClient failing(&table);
	FakeClock::armed = true;
	FakeClock::fail = true;
	EXPECT_EQ(DateMappingStatus::ClockFailure, failing.calcTradingDate("EX.D", 0, 0).status);
	FakeClock::reset();
}

TEST_F(I03S5DateClientV1, MissingCalendarIsOnlyVisibleThroughTheTable)
{
	const DateMappingV1 table = mgr.getDateMappingV1();
	DateMappingClient typed(&table);
	EXPECT_EQ(DateMappingStatus::MissingCalendar, typed.calcTradingDate("EX.M", 20240227, 1000).status);
	EXPECT_EQ(DateMappingStatus::MissingCalendar, typed.getBoundaryTime("EX.M", 20240227).status);
	// Documented path-C limit: IBaseDataMgr cannot report an unloaded calendar.
	DateMappingClient legacy(static_cast<IBaseDataMgr*>(&mgr));
	EXPECT_EQ(DateMappingStatus::Ok, legacy.calcTradingDate("EX.M", 20240227, 1000).status);
	EXPECT_EQ(DateMappingStatus::MissingCommodity, typed.calcTradingDate("EX.X", 20240227, 1000).status);
	EXPECT_EQ(DateMappingStatus::MissingCommodity, legacy.calcTradingDate("EX.X", 20240227, 1000).status);

	DateMappingV1 broken = table;
	broken.size -= 1;
	DateMappingClient unsupported(&broken);
	EXPECT_EQ(DateMappingStatus::Unsupported, unsupported.calcTradingDate("EX.P", 20240227, 1000).status);
	broken = table;
	broken.version = 2;
	DateMappingClient unsupported2(&broken);
	EXPECT_EQ(DateMappingStatus::Unsupported, unsupported2.getBoundaryTime("EX.P", 20240227).status);
	DateMappingClient none(static_cast<const DateMappingV1*>(nullptr));
	EXPECT_EQ(DateMappingStatus::Unsupported, none.calcTradingDate("EX.P", 20240227, 1000).status);
}

// BUG-L01-018 local matrix and I03-NEW-006: calendar arithmetic does not depend
// on the time zone cache, DST or mktime.
TEST(I03S5CheckedDate, NextDateMatrixAndWeekdayConsistency)
{
	uint32_t r = 7;
	EXPECT_TRUE(TimeUtils::tryGetNextDate(19691231, 1, r)); EXPECT_EQ(19700101u, r);
	EXPECT_TRUE(TimeUtils::tryGetNextDate(19700101, -1, r)); EXPECT_EQ(19691231u, r);
	EXPECT_TRUE(TimeUtils::tryGetNextDate(20240228, 1, r)); EXPECT_EQ(20240229u, r);
	EXPECT_TRUE(TimeUtils::tryGetNextDate(20230228, 1, r)); EXPECT_EQ(20230301u, r);
	EXPECT_TRUE(TimeUtils::tryGetNextDate(21000228, 1, r)); EXPECT_EQ(21000301u, r);
	EXPECT_TRUE(TimeUtils::tryGetNextDate(20000228, 1, r)); EXPECT_EQ(20000229u, r);
	EXPECT_TRUE(TimeUtils::tryGetNextDate(20240101, 30, r)); EXPECT_EQ(20240131u, r);
	EXPECT_TRUE(TimeUtils::tryGetNextDate(20240315, 0, r)); EXPECT_EQ(20240315u, r);
	EXPECT_TRUE(TimeUtils::tryGetNextDate(10000101, 0, r)); EXPECT_EQ(10000101u, r);
	r = 7;
	for (uint32_t bad : {0u, 20240230u, 20241301u, 20230229u, 9991231u, 4294967295u})
	{
		EXPECT_FALSE(TimeUtils::tryGetNextDate(bad, 1, r)) << bad;
		EXPECT_EQ(7u, r);
	}
	EXPECT_FALSE(TimeUtils::tryGetNextDate(99991231, 1, r));
	EXPECT_FALSE(TimeUtils::tryGetNextDate(10000101, -1, r));
	EXPECT_FALSE(TimeUtils::tryGetNextDate(20240101, 5000000, r));
	EXPECT_EQ(7u, r);

	uint32_t date = 18991225, weekDay = 0;
	ASSERT_TRUE(TimeUtils::tryGetWeekDay(date, weekDay));
	for (int i = 0; i < 80000; ++i)
	{
		uint32_t next = 0, nextWeekDay = 0, back = 0;
		ASSERT_TRUE(TimeUtils::tryGetNextDate(date, 1, next)) << date;
		ASSERT_TRUE(TimeUtils::tryGetWeekDay(next, nextWeekDay));
		ASSERT_EQ((weekDay + 1) % 7, nextWeekDay) << next;
		ASSERT_TRUE(TimeUtils::tryGetNextDate(next, -1, back));
		ASSERT_EQ(date, back);
		// Legacy entry agrees for valid dates.
		ASSERT_EQ(next, TimeUtils::getNextDate(date, 1)) << date;
		date = next;
		weekDay = nextWeekDay;
	}
	EXPECT_EQ(19681108u, TimeUtils::getNextDate(19691231, -418));
	EXPECT_EQ(3u, TimeUtils::getWeekDay(19691231));
	EXPECT_EQ(TimeUtils::INVALID_WEEKDAY, TimeUtils::getWeekDay(20240230));
	EXPECT_FALSE(TimeUtils::isWeekends(20240230));
}

// BUG-L03-010: decision card 2 makes 0-0 the only full-day section; mixing it
// with any other section (either order) is rejected without mutation, so the
// full-day aggregate view stays at exactly one day. General interior overlap
// of ordinary sections is not decided (decision-card draft) and not asserted.
TEST(I03S5SessionSections, FullDaySectionCannotBeMixedWithOtherSections)
{
	for (int32_t offset : {0, 300, -300})
	{
		WTSSessionInfo* s = WTSSessionInfo::create("S", "s", offset);
		ASSERT_TRUE(s->tryAddTradingSection(2100, 230));
		ASSERT_TRUE(s->tryAddTradingSection(900, 1015));
		ASSERT_TRUE(s->tryAddTradingSection(1015, 1130)); // touching endpoint is allowed
		const uint32_t mins = s->getTradingMins();
		const auto list = s->getSecMinList();
		EXPECT_FALSE(s->tryAddTradingSection(0, 0));
		s->addTradingSection(0, 0);                          // legacy void entry ignores it
		EXPECT_EQ(3u, s->getSectionCount());
		EXPECT_EQ(mins, s->getTradingMins());
		EXPECT_EQ(list, s->getSecMinList());
		EXPECT_EQ(75u + 75u + 330u, mins);
		EXPECT_TRUE(s->tryAddTradingSection(1330, 1500));
		s->release();

		WTSSessionInfo* all = WTSSessionInfo::create("A", "a", offset);
		ASSERT_TRUE(all->tryAddTradingSection(0, 0));
		EXPECT_FALSE(all->tryAddTradingSection(900, 1000));
		EXPECT_FALSE(all->tryAddTradingSection(2100, 230));
		EXPECT_FALSE(all->tryAddTradingSection(0, 0));
		all->addTradingSection(900, 1000);
		EXPECT_EQ(1u, all->getSectionCount());
		EXPECT_EQ(1440u, all->getTradingMins());
		EXPECT_EQ(1440u * 60u, all->getTradingSeconds());
		ASSERT_EQ(1u, all->getSecMinList().size());
		EXPECT_EQ(1440u, all->getSecMinList().back());
		all->release();
	}
}

TEST(I03S5SessionSections, MixedFullDayConfigurationIsNotPublished)
{
	initLogger();
	WTSBaseDataMgr mgr;
	const auto good = writeConfig(R"({"OK":{"name":"ok","offset":0,"sections":[{"from":900,"to":1130},{"from":1130,"to":1500}]}})");
	ASSERT_TRUE(mgr.loadSessions(good.c_str()));
	const auto bad = writeConfig(R"({"NEW":{"name":"new","offset":0,"sections":[{"from":900,"to":1500}]},"MIX":{"name":"mix","offset":300,"sections":[{"from":900,"to":1500},{"from":0,"to":0}]}})");
	EXPECT_FALSE(mgr.loadSessions(bad.c_str()));
	EXPECT_EQ(nullptr, mgr.getSession("NEW"));
	EXPECT_EQ(nullptr, mgr.getSession("MIX"));
	ASSERT_NE(nullptr, mgr.getSession("OK"));
	EXPECT_EQ(360u, mgr.getSession("OK")->getTradingMins());
	unlink(good.c_str()); unlink(bad.c_str());
}
