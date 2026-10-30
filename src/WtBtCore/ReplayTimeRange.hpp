#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace wtp
{
namespace replay
{

/** Errors reported while decoding a replay time range. */
enum class TimeRangeParseError
{
	None,
	InvalidLength,
	NonDigit,
	InvalidDate,
	InvalidTime,
	ReversedRange
};

/**
 * A timezone-free civil timestamp decoded from YYYYMMDDHHMM or
 * YYYYMMDDHHMMSS input.
 */
struct CivilDateTime
{
	int year {0};
	int month {0};
	int day {0};
	int hour {0};
	int minute {0};
	int second {0};
	unsigned precisionDigits {0};
};

/**
 * A validated range with precision-aware, half-open normalized bounds.
 * Seconds and minutes are counts from 0001-01-01 00:00:00; no timezone or
 * daylight-saving interpretation is applied.
 */
struct ReplayTimeRange
{
	std::string inputBegin;
	std::string inputEnd;
	CivilDateTime begin;
	CivilDateTime end;

	// Tick interval: [tickBeginSeconds, tickEndExclusiveSeconds). A 12-digit
	// end includes its whole minute; a 14-digit end includes its whole second.
	int64_t tickBeginSeconds {0};
	int64_t tickEndExclusiveSeconds {0};

	// Minute-bar/task interval: [barBeginMinute, barEndExclusiveMinute).
	// The end minute is included when the input range intersects that minute.
	// An empty interval is represented by equal bounds.
	int64_t barBeginMinute {0};
	int64_t barEndExclusiveMinute {0};

	bool hasBarMinutes() const { return barBeginMinute < barEndExclusiveMinute; }
};

namespace detail
{
inline bool isLeapYear(int year)
{
	return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

inline int daysInMonth(int year, int month)
{
	static constexpr int kDays[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	if (month == 2 && isLeapYear(year))
		return 29;
	return month >= 1 && month <= 12 ? kDays[month] : 0;
}

inline int parseDigits(std::string_view text, std::size_t offset, std::size_t count)
{
	int value = 0;
	for (std::size_t i = offset; i < offset + count; ++i)
		value = value * 10 + (text[i] - '0');
	return value;
}

// Proleptic Gregorian day number, shifted so 0001-01-01 is day zero.
inline int64_t daysFromCivil(int year, unsigned month, unsigned day)
{
	int adjustedYear = year - (month <= 2 ? 1 : 0);
	const int era = adjustedYear / 400;
	const unsigned yearOfEra = static_cast<unsigned>(adjustedYear - era * 400);
	const int adjustedMonth = static_cast<int>(month) + (month > 2 ? -3 : 9);
	const unsigned dayOfYear = static_cast<unsigned>((153 * adjustedMonth + 2) / 5) + day - 1;
	const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
	return static_cast<int64_t>(era) * 146097 + dayOfEra - 306;
}

struct ParsedEndpoint
{
	CivilDateTime value;
	int64_t seconds {0};
};

inline TimeRangeParseError parseEndpoint(std::string_view text, ParsedEndpoint& output)
{
	if (text.size() != 12 && text.size() != 14)
		return TimeRangeParseError::InvalidLength;
	for (char ch : text)
	{
		if (ch < '0' || ch > '9')
			return TimeRangeParseError::NonDigit;
	}

	CivilDateTime value;
	value.year = parseDigits(text, 0, 4);
	value.month = parseDigits(text, 4, 2);
	value.day = parseDigits(text, 6, 2);
	value.hour = parseDigits(text, 8, 2);
	value.minute = parseDigits(text, 10, 2);
	value.second = text.size() == 14 ? parseDigits(text, 12, 2) : 0;
	value.precisionDigits = static_cast<unsigned>(text.size());

	if (value.year < 1 || value.year > 9999 || value.month < 1 || value.month > 12 ||
		value.day < 1 || value.day > daysInMonth(value.year, value.month))
		return TimeRangeParseError::InvalidDate;
	if (value.hour > 23 || value.minute > 59 || value.second > 59)
		return TimeRangeParseError::InvalidTime;

	const int64_t day = daysFromCivil(value.year,
		static_cast<unsigned>(value.month), static_cast<unsigned>(value.day));
	output.value = value;
	output.seconds = day * 86400 + value.hour * 3600 + value.minute * 60 + value.second;
	return TimeRangeParseError::None;
}
} // namespace detail

/**
 * Parse and normalize a pair of timestamp strings. The output is only changed
 * on success. Each endpoint independently accepts 12 or 14 decimal digits.
 */
inline TimeRangeParseError parseReplayTimeRange(std::string_view beginText,
	std::string_view endText, ReplayTimeRange& output)
{
	detail::ParsedEndpoint begin;
	auto error = detail::parseEndpoint(beginText, begin);
	if (error != TimeRangeParseError::None)
		return error;

	detail::ParsedEndpoint end;
	error = detail::parseEndpoint(endText, end);
	if (error != TimeRangeParseError::None)
		return error;
	const int64_t tickEndExclusive = end.seconds + (end.value.precisionDigits == 12 ? 60 : 1);
	if (begin.seconds >= tickEndExclusive)
		return TimeRangeParseError::ReversedRange;

	ReplayTimeRange parsed;
	parsed.inputBegin.assign(beginText.data(), beginText.size());
	parsed.inputEnd.assign(endText.data(), endText.size());
	parsed.begin = begin.value;
	parsed.end = end.value;
	parsed.tickBeginSeconds = begin.seconds;
	parsed.tickEndExclusiveSeconds = tickEndExclusive;
	parsed.barBeginMinute = (begin.seconds + 59) / 60;
	parsed.barEndExclusiveMinute = end.seconds / 60 + 1;
	output = std::move(parsed);
	return TimeRangeParseError::None;
}

/** Numeric convenience overload for existing YYYYMMDDHHMM[SS] values. */
inline TimeRangeParseError parseReplayTimeRange(uint64_t beginValue,
	uint64_t endValue, ReplayTimeRange& output)
{
	return parseReplayTimeRange(std::to_string(beginValue), std::to_string(endValue), output);
}

} // namespace replay
} // namespace wtp

