// I03-NEW-006 (S5): the legacy TimeUtils clock helpers have no failure channel.
// A failed time zone lookup (getTZOffset() sentinel 99) must not be applied as
// a 99-hour offset, and a failed clock read must not publish a plausible date.
// Only Share/TimeUtils.hpp is included so the red build can swap in the
// pre-change header.
#include <gtest/gtest.h>

#include <cstdlib>
#include <ctime>
#include <dlfcn.h>

#include "Share/TimeUtils.hpp"

namespace {
bool failMktime = false;
bool failClock = false;
}

extern "C" time_t mktime(struct tm* value)
{
	if (failMktime)
		return static_cast<time_t>(-1);
	using Fn = time_t (*)(struct tm*);
	static Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "mktime"));
	return real(value);
}

extern "C" int clock_gettime(clockid_t clock, struct timespec* result)
{
	if (failClock && clock == CLOCK_REALTIME)
		return -1;
	using Fn = int (*)(clockid_t, struct timespec*);
	static Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "clock_gettime"));
	return real(clock, result);
}

// Must run first in this executable: the per-translation-unit TZ cache is only
// empty before the first successful lookup.
TEST(I03S5TimeZoneFailure, A_FailedLookupIsNotUsedAsAnHourOffset)
{
	setenv("TZ", "UTC", 1);
	tzset();
	failMktime = true;
	ASSERT_EQ(99, TimeUtils::getTZOffset());
	uint32_t refDate = 0, refTime = 0;
	ASSERT_TRUE(TimeUtils::tryGetDateTime(refDate, refTime)); // localtime_r only
	const uint32_t curDate = TimeUtils::getCurDate();
	uint32_t date = 0, time = 0;
	TimeUtils::getDateTime(date, time);
	const uint32_t curMin = TimeUtils::getCurMin();
	const uint64_t stamp = TimeUtils::getYYYYMMDDhhmmss();
	uint32_t afterDate = 0, afterTime = 0;
	ASSERT_TRUE(TimeUtils::tryGetDateTime(afterDate, afterTime));
	failMktime = false;

	// Allow a midnight rollover between the two reference snapshots.
	EXPECT_TRUE(curDate == refDate || curDate == afterDate) << curDate << " vs " << refDate;
	EXPECT_TRUE(date == refDate || date == afterDate) << date;
	EXPECT_TRUE(stamp / 1000000 == refDate || stamp / 1000000 == afterDate) << stamp;
	EXPECT_LE(refTime / 1000 / 10000, curMin / 10000);
	EXPECT_LE(curMin / 10000, afterTime / 1000 / 10000 + (afterDate != refDate ? 24 : 0));
	// Valid calendar arithmetic needs neither mktime nor the TZ cache.
	EXPECT_EQ(20240228u, TimeUtils::getNextDate(20240227, 1));
	failMktime = true;
	EXPECT_EQ(20240301u, TimeUtils::getNextDate(20240229, 1));
	EXPECT_EQ(19691231u, TimeUtils::getNextDate(19700101, -1));
	failMktime = false;
}

TEST(I03S5TimeZoneFailure, B_ClockFailurePublishesAnInvalidDate)
{
	// Prime the legacy per-thread state with a successful read first.
	ASSERT_GE(TimeUtils::getCurDate(), 20200101u);
	failClock = true;
	const uint32_t curDate = TimeUtils::getCurDate();
	uint32_t date = 1, time = 1;
	TimeUtils::getDateTime(date, time);
	uint32_t checkedDate = 0, checkedTime = 0;
	const bool checked = TimeUtils::tryGetDateTime(checkedDate, checkedTime);
	failClock = false;
	uint32_t weekDay = 0;
	EXPECT_FALSE(checked);
	EXPECT_FALSE(TimeUtils::tryGetWeekDay(curDate, weekDay)) << curDate;
	EXPECT_NE(0u, curDate) << "0 means 'current date' to several callers";
	EXPECT_FALSE(TimeUtils::tryGetWeekDay(date, weekDay)) << date;
	EXPECT_EQ(0u, time);
	EXPECT_GE(TimeUtils::getCurDate(), 20200101u); // recovers on the next read
}
