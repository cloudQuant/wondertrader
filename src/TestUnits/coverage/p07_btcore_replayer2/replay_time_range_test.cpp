#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include <WtBtCore/ReplayTimeRange.hpp>

namespace
{
using wtp::replay::ReplayTimeRange;
using wtp::replay::TimeRangeParseError;
using wtp::replay::parseReplayTimeRange;

TEST(ReplayTimeRangeTest, acceptsMinuteAndSecondPrecisionAndPreservesInputs)
{
	ReplayTimeRange range;
	EXPECT_EQ(TimeRangeParseError::None, parseReplayTimeRange("202402291159", "20240229120030", range));
	EXPECT_EQ("202402291159", range.inputBegin);
	EXPECT_EQ("20240229120030", range.inputEnd);
	EXPECT_EQ(12u, range.begin.precisionDigits);
	EXPECT_EQ(14u, range.end.precisionDigits);
	EXPECT_EQ(0, range.begin.second);
	EXPECT_EQ(30, range.end.second);
	EXPECT_EQ(91, range.tickEndExclusiveSeconds - range.tickBeginSeconds);

	EXPECT_EQ(TimeRangeParseError::None, parseReplayTimeRange("20240229115930", "202402291200", range));
	EXPECT_EQ(30, range.begin.second);
	EXPECT_EQ(12u, range.end.precisionDigits);
	EXPECT_EQ(90, range.tickEndExclusiveSeconds - range.tickBeginSeconds);
}

TEST(ReplayTimeRangeTest, numericOverloadAcceptsBothSupportedWidths)
{
	ReplayTimeRange range;
	EXPECT_EQ(TimeRangeParseError::None,
		parseReplayTimeRange(uint64_t{202401020930}, uint64_t{20240102093045}, range));
	EXPECT_EQ("202401020930", range.inputBegin);
	EXPECT_EQ("20240102093045", range.inputEnd);
	EXPECT_EQ(46, range.tickEndExclusiveSeconds - range.tickBeginSeconds);
}

TEST(ReplayTimeRangeTest, twelveDigitEndIncludesTheWholeMinute)
{
	ReplayTimeRange range;
	ASSERT_EQ(TimeRangeParseError::None, parseReplayTimeRange("202401020930", "202401020931", range));
	EXPECT_EQ(120, range.tickEndExclusiveSeconds - range.tickBeginSeconds);
	EXPECT_EQ(2, range.barEndExclusiveMinute - range.barBeginMinute);
}

TEST(ReplayTimeRangeTest, fourteenDigitEndIncludesTheWholeSecond)
{
	ReplayTimeRange range;
	ASSERT_EQ(TimeRangeParseError::None, parseReplayTimeRange("20240102093000", "20240102093059", range));
	EXPECT_EQ(60, range.tickEndExclusiveSeconds - range.tickBeginSeconds);
	EXPECT_EQ(1, range.barEndExclusiveMinute - range.barBeginMinute);
}

TEST(ReplayTimeRangeTest, barsRoundNonzeroStartSecondsUpAndEndSecondsDown)
{
	ReplayTimeRange range;
	ASSERT_EQ(TimeRangeParseError::None, parseReplayTimeRange("20240102093030", "20240102093245", range));
	ReplayTimeRange minuteRange;
	ASSERT_EQ(TimeRangeParseError::None, parseReplayTimeRange("202401020931", "202401020932", minuteRange));
	EXPECT_EQ(minuteRange.barBeginMinute, range.barBeginMinute);
	EXPECT_EQ(minuteRange.barEndExclusiveMinute, range.barEndExclusiveMinute);
	EXPECT_EQ(136, range.tickEndExclusiveSeconds - range.tickBeginSeconds);
}

TEST(ReplayTimeRangeTest, aSecondOnlyIntervalCanContainNoWholeMinute)
{
	ReplayTimeRange range;
	ASSERT_EQ(TimeRangeParseError::None, parseReplayTimeRange("20240102093030", "20240102093045", range));
	EXPECT_FALSE(range.hasBarMinutes());
	EXPECT_EQ(range.barBeginMinute, range.barEndExclusiveMinute);
}

TEST(ReplayTimeRangeTest, minuteBoundaryStartAndEndRetainThatMinute)
{
	ReplayTimeRange range;
	ASSERT_EQ(TimeRangeParseError::None, parseReplayTimeRange("20240102093000", "20240102093000", range));
	EXPECT_TRUE(range.hasBarMinutes());
	EXPECT_EQ(1, range.barEndExclusiveMinute - range.barBeginMinute);
	EXPECT_EQ(1, range.tickEndExclusiveSeconds - range.tickBeginSeconds);
}

TEST(ReplayTimeRangeTest, crossingMidnightUsesContinuousCalendarSeconds)
{
	ReplayTimeRange range;
	ASSERT_EQ(TimeRangeParseError::None, parseReplayTimeRange("202401022359", "202401030001", range));
	EXPECT_EQ(180, range.tickEndExclusiveSeconds - range.tickBeginSeconds);
	EXPECT_EQ(3, range.barEndExclusiveMinute - range.barBeginMinute);
}

TEST(ReplayTimeRangeTest, crossingMonthAndYearBoundaryIsCorrect)
{
	ReplayTimeRange range;
	ASSERT_EQ(TimeRangeParseError::None, parseReplayTimeRange("202312312359", "202401010000", range));
	EXPECT_EQ(120, range.tickEndExclusiveSeconds - range.tickBeginSeconds);
	EXPECT_EQ(2, range.barEndExclusiveMinute - range.barBeginMinute);
}

TEST(ReplayTimeRangeTest, leapDayAndLeapYearBoundariesAreCorrect)
{
	ReplayTimeRange range;
	ASSERT_EQ(TimeRangeParseError::None, parseReplayTimeRange("202402282359", "202402290000", range));
	EXPECT_EQ(120, range.tickEndExclusiveSeconds - range.tickBeginSeconds);

	ASSERT_EQ(TimeRangeParseError::None, parseReplayTimeRange("200002282359", "200002290000", range));
	EXPECT_EQ(2, range.barEndExclusiveMinute - range.barBeginMinute);
}

TEST(ReplayTimeRangeTest, rejectsInvalidLengths)
{
	ReplayTimeRange range;
	for (const std::string& value : {"20240102093", "2024010209300", "202401020930000", ""})
		EXPECT_EQ(TimeRangeParseError::InvalidLength, parseReplayTimeRange(value, "202401020931", range)) << value;
}

TEST(ReplayTimeRangeTest, rejectsNonDigits)
{
	ReplayTimeRange range;
	EXPECT_EQ(TimeRangeParseError::NonDigit, parseReplayTimeRange("2024010A0930", "202401020931", range));
	EXPECT_EQ(TimeRangeParseError::NonDigit, parseReplayTimeRange("202401020930", "20240102093x", range));
}

TEST(ReplayTimeRangeTest, rejectsInvalidGregorianDates)
{
	ReplayTimeRange range;
	const std::vector<std::string> invalidDates = {
		"202302290930", "190002290930", "202404310930", "202401000930",
		"202413010930", "000001010930", "100000000930"
	};
	for (const auto& value : invalidDates)
		EXPECT_EQ(TimeRangeParseError::InvalidDate, parseReplayTimeRange(value, value, range)) << value;

	EXPECT_EQ(TimeRangeParseError::None, parseReplayTimeRange("200002290930", "200002290930", range));
	EXPECT_EQ(TimeRangeParseError::None, parseReplayTimeRange("202402290930", "202402290930", range));
}

TEST(ReplayTimeRangeTest, rejectsInvalidClockFields)
{
	ReplayTimeRange range;
	const std::vector<std::string> invalidTimes = {
		"202401022430", "202401020960", "20240102093060", "20240102240000",
		"20240102096000", "20240102093060"
	};
	for (const auto& value : invalidTimes)
		EXPECT_EQ(TimeRangeParseError::InvalidTime, parseReplayTimeRange(value, value, range)) << value;
}

TEST(ReplayTimeRangeTest, rejectsReversedRangeWithoutChangingOutput)
{
	ReplayTimeRange range;
	ASSERT_EQ(TimeRangeParseError::None, parseReplayTimeRange("202401020930", "202401020931", range));
	const auto previousBegin = range.inputBegin;
	const auto previousEnd = range.inputEnd;
	EXPECT_EQ(TimeRangeParseError::ReversedRange,
		parseReplayTimeRange("20240102093200", "202401020931", range));
	EXPECT_EQ(TimeRangeParseError::ReversedRange,
		parseReplayTimeRange("20240102093201", "202401020931", range));
	EXPECT_EQ(previousBegin, range.inputBegin);
	EXPECT_EQ(previousEnd, range.inputEnd);
}

TEST(ReplayTimeRangeTest, equalInstantWithMixedPrecisionIsValid)
{
	ReplayTimeRange range;
	ASSERT_EQ(TimeRangeParseError::None, parseReplayTimeRange("20240102093000", "202401020930", range));
	EXPECT_EQ(60, range.tickEndExclusiveSeconds - range.tickBeginSeconds);
	EXPECT_EQ(1, range.barEndExclusiveMinute - range.barBeginMinute);
}

TEST(ReplayTimeRangeTest, secondPrecisionStartInsideMinutePrecisionEndIsValid)
{
	ReplayTimeRange range;
	ASSERT_EQ(TimeRangeParseError::None, parseReplayTimeRange("20240102093030", "202401020930", range));
	EXPECT_EQ(30, range.tickEndExclusiveSeconds - range.tickBeginSeconds);
	EXPECT_FALSE(range.hasBarMinutes());
	EXPECT_EQ("20240102093030", range.inputBegin);
	EXPECT_EQ("202401020930", range.inputEnd);
}

TEST(ReplayTimeRangeTest, startAtEndExclusiveIsRejected)
{
	ReplayTimeRange range;
	EXPECT_EQ(TimeRangeParseError::ReversedRange,
		parseReplayTimeRange("20240102093100", "202401020930", range));
	EXPECT_EQ(TimeRangeParseError::ReversedRange,
		parseReplayTimeRange("20240102093059", "20240102093058", range));
}
} // namespace

