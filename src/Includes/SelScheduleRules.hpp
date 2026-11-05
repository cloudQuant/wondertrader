/*!
 * \file SelScheduleRules.hpp
 * \brief SEL timed-task schedule contract (decision K27=A, 2026-11-04).
 *
 * Every engine executes SEL tasks with HHMM times, so the entry points accept
 * only values that can actually fire:
 *   - time:  HHMM (hour 0..23, minute 0..59) for d/w/m/y;
 *            for the "min" period it is the interval in minutes (1..1440);
 *   - date:  ignored for d and min; weekday 0..6 for w (TimeUtils::getWeekDay);
 *            day of month 1..31 for m; MMDD for y.
 * Aliases such as d1/w1/m1 are not accepted (K27 alias option a).
 * Period codes match TaskPeriodType in WtCore/WtBtCore: 4=min, 8=d, 9=w, 10=m, 11=y.
 */
#pragma once
#include <cstdint>

namespace wt_sel_schedule
{
	enum : uint32_t { kMinute = 4, kDaily = 8, kWeekly = 9, kMonthly = 10, kYearly = 11 };

	inline bool isValidHHMM(uint32_t t)
	{
		return t / 100 <= 23 && t % 100 <= 59;
	}

	/// Returns NULL when the schedule is valid, otherwise a short reason.
	inline const char* invalidReason(uint32_t period, uint32_t date, uint32_t time)
	{
		switch (period)
		{
		case kMinute:
			return (time >= 1 && time <= 1440) ? nullptr : "min period requires an interval of 1..1440 minutes";
		case kDaily:
			return isValidHHMM(time) ? nullptr : "time must be HHMM (00:00..23:59)";
		case kWeekly:
			if (!isValidHHMM(time)) return "time must be HHMM (00:00..23:59)";
			return date <= 6 ? nullptr : "weekly date must be a weekday 0..6";
		case kMonthly:
			if (!isValidHHMM(time)) return "time must be HHMM (00:00..23:59)";
			return (date >= 1 && date <= 31) ? nullptr : "monthly date must be a day 1..31";
		case kYearly:
			if (!isValidHHMM(time)) return "time must be HHMM (00:00..23:59)";
			return (date / 100 >= 1 && date / 100 <= 12 && date % 100 >= 1 && date % 100 <= 31)
				? nullptr : "yearly date must be MMDD";
		default:
			return "unknown period";
		}
	}
}
