// L01 (lane Share/WTSUtils): TimeUtils.hpp coverage.
// 全部用例只使用固定日期输入；真实时钟函数只做区间校验，不做精确断言。
// 疑似缺陷见 docs/迭代计划/迭代01/bugs/l01_share_wtsutils.md
#include <gtest/gtest.h>

#include "Share/TimeUtils.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <time.h>

namespace {

uint32_t dateOf(const tm& t)
{
	return static_cast<uint32_t>((t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday);
}

uint32_t timeOf(const tm& t)
{
	return static_cast<uint32_t>(t.tm_hour * 10000 + t.tm_min * 100 + t.tm_sec);
}

tm localFrom(time_t ts)
{
	tm out;
	std::memset(&out, 0, sizeof(out));
	::localtime_r(&ts, &out);
	return out;
}

} // namespace

// ------------------------------------------------------------------ 纯计算 constexpr
TEST(l01_timeutils, get_next_minute)
{
	EXPECT_EQ(TimeUtils::getNextMinute(930), 931u);
	EXPECT_EQ(TimeUtils::getNextMinute(959), 1000u);
	// 跨天回绕
	EXPECT_EQ(TimeUtils::getNextMinute(2359), 0u);
	// 回拨
	EXPECT_EQ(TimeUtils::getNextMinute(0, -1), 2359u);
	EXPECT_EQ(TimeUtils::getNextMinute(1200, -30), 1130u);
	EXPECT_EQ(TimeUtils::getNextMinute(1000, 60), 1100u);
	// 一次偏移超过 24 小时时结果仍然只加/减一天（见 bug 清单）
	EXPECT_EQ(TimeUtils::getNextMinute(1000, 1440), 1000u);
}

TEST(l01_timeutils, get_next_minute_underflow_returns_wrapped_uint)
{
	// BUG-L01-006：mins 的绝对值大于 1440 时只做一次 +1440，totalMins 仍为负，
	// 负值被强转成 uint32_t 返回。这里断言的是“实际行为”，期望值见 DISABLED 用例。
	const uint32_t actual = TimeUtils::getNextMinute(0, -1500);
	EXPECT_EQ(actual, 4294967196u);
}

TEST(l01_timeutils, DISABLED_get_next_minute_should_normalize_full_days)
{
	// 00:00 往前 1500 分钟 == 往前 60 分钟 == 前一天 23:00
	EXPECT_EQ(TimeUtils::getNextMinute(0, -1500), 2300u);
}

TEST(l01_timeutils, get_next_month)
{
	EXPECT_EQ(TimeUtils::getNextMonth(202412u), 202501u);
	EXPECT_EQ(TimeUtils::getNextMonth(202401u, 11), 202412u);
	EXPECT_EQ(TimeUtils::getNextMonth(202406u, 12), 202506u);
	EXPECT_EQ(TimeUtils::getNextMonth(202406u, 11), 202505u);
	EXPECT_EQ(TimeUtils::getNextMonth(202406u, 13), 202507u);
	EXPECT_EQ(TimeUtils::getNextMonth(202401u, 0), 202401u);
}

TEST(l01_timeutils, get_next_month_negative_months_is_off_by_one_year)
{
	// BUG-L01-007：months<0 时 uAddYear = months/12 截断为 0，uAddMon 被 +12 修正，
	// 但年份没有回退，结果比正确值大一年。断言的是当前实际行为。
	EXPECT_EQ(TimeUtils::getNextMonth(202401u, -1), 202412u);
	EXPECT_EQ(TimeUtils::getNextMonth(202406u, -1), 202505u);
}

TEST(l01_timeutils, DISABLED_get_next_month_negative_months)
{
	EXPECT_EQ(TimeUtils::getNextMonth(202401u, -1), 202312u);
	EXPECT_EQ(TimeUtils::getNextMonth(202406u, -1), 202405u);
}

TEST(l01_timeutils, min_bar_helpers)
{
	const uint64_t bar = TimeUtils::timeToMinBar(20200101, 930);
	EXPECT_EQ(TimeUtils::minBarToDate(bar), 20200101u);
	EXPECT_EQ(TimeUtils::minBarToTime(bar), 930u);

	EXPECT_EQ(TimeUtils::minBarToDate(3001010930ull), 20200101u);
	EXPECT_EQ(TimeUtils::minBarToTime(3001010930ull), 930u);

	static_assert(TimeUtils::minBarToDate(TimeUtils::timeToMinBar(20200101, 1459)) == 20200101,
		"min bar roundtrip");
	static_assert(TimeUtils::minBarToTime(TimeUtils::timeToMinBar(20200101, 1459)) == 1459,
		"min bar time roundtrip");
}

TEST(l01_timeutils, time_to_min_bar_overflows_after_2032)
{
	// BUG-L01-008：(uDate-19900000)*10000 在 uint32 里先算再升宽，
	// 2032-10 之后的日期会绕回，minBarToDate 再也还原不出原日期。
	const uint64_t bar = TimeUtils::timeToMinBar(20330101, 930);
	EXPECT_EQ(bar, 6043634ull);
	EXPECT_EQ(TimeUtils::minBarToDate(bar), 19900604u);
}

TEST(l01_timeutils, DISABLED_time_to_min_bar_should_roundtrip_for_2033)
{
	const uint64_t bar = TimeUtils::timeToMinBar(20330101, 930);
	EXPECT_EQ(TimeUtils::minBarToDate(bar), 20330101u);
	EXPECT_EQ(TimeUtils::minBarToTime(bar), 930u);
}

// ------------------------------------------------------------------ makeTime / timeToString
TEST(l01_timeutils, make_time_hour_minute_ms_offsets)
{
	const int64_t base = TimeUtils::makeTime(20240315, 0);
	const int64_t full = TimeUtils::makeTime(20240315, 93000500);
	// 同一天的两个时间点之差与时区无关：09:30:00.500
	EXPECT_EQ(full - base, 34200500);

	const int64_t nextDay = TimeUtils::makeTime(20240316, 0);
	EXPECT_EQ(nextDay - base, 86400000);
}

TEST(l01_timeutils, make_time_to_utc_uses_tz_offset)
{
	const int32_t offset = TimeUtils::getTZOffset();
	EXPECT_GE(offset, -12);
	EXPECT_LE(offset, 14);

	const int64_t local = TimeUtils::makeTime(20240601, 120000000);
	const int64_t utc = TimeUtils::makeTime(20240601, 120000000, true);
	EXPECT_EQ(utc, local - static_cast<int64_t>(offset) * 3600000);
}

TEST(l01_timeutils, time_to_string_roundtrip)
{
	const int64_t ms = TimeUtils::makeTime(20240315, 93000500);
	const std::string s = TimeUtils::timeToString(ms);
	// 有毫秒时格式为 yyyymmddHHMMSS.mmm（4+2+2+2+2+2+1+3 = 18）
	ASSERT_EQ(s.size(), 18u);
	EXPECT_EQ(s[14], '.');
	EXPECT_EQ(s.substr(15), "500");

	const int64_t noMs = TimeUtils::makeTime(20240315, 93000000);
	const std::string s2 = TimeUtils::timeToString(noMs);
	EXPECT_EQ(s2.size(), 14u);

	// 0 直接返回空串
	EXPECT_EQ(TimeUtils::timeToString(0), std::string(""));
	// 负毫秒 => msec < 0 分支
	EXPECT_EQ(TimeUtils::timeToString(-1500), std::string(""));
}

TEST(l01_timeutils, get_next_date)
{
	EXPECT_EQ(TimeUtils::getNextDate(20240315), 20240316u);
	EXPECT_EQ(TimeUtils::getNextDate(20240228), 20240229u);
	EXPECT_EQ(TimeUtils::getNextDate(20230228), 20230301u);
	EXPECT_EQ(TimeUtils::getNextDate(20240101, 30), 20240131u);
}

// ------------------------------------------------------------------ 星期 / 周末
TEST(l01_timeutils, get_week_day)
{
	EXPECT_EQ(TimeUtils::getWeekDay(20240101), 1u);	// 周一
	EXPECT_EQ(TimeUtils::getWeekDay(20240105), 5u);	// 周五
	EXPECT_EQ(TimeUtils::getWeekDay(20240106), 6u);	// 周六
	EXPECT_EQ(TimeUtils::getWeekDay(20240107), 0u);	// 周日
	EXPECT_EQ(TimeUtils::getWeekDay(20200101), 3u);	// 周三
	EXPECT_EQ(TimeUtils::getWeekDay(20241225), 3u);	// 周三
}

TEST(l01_timeutils, is_weekends)
{
	EXPECT_TRUE(TimeUtils::isWeekends(20240106));
	EXPECT_TRUE(TimeUtils::isWeekends(20240107));
	EXPECT_FALSE(TimeUtils::isWeekends(20240101));
	EXPECT_FALSE(TimeUtils::isWeekends(20240105));
	EXPECT_FALSE(TimeUtils::isWeekends(20200101));
}

TEST(l01_timeutils, week_day_and_weekends_are_consistent)
{
	for (uint32_t day = 20240101; day <= 20240131; day += 1)
	{
		const uint32_t w = TimeUtils::getWeekDay(day);
		ASSERT_LT(w, 7u);
		EXPECT_EQ(TimeUtils::isWeekends(day), (w == 0u || w == 6u));
	}
}

// ------------------------------------------------------------------ Time32
TEST(l01_timeutils, time32_from_time_t)
{
	const time_t ts = 1700000000;
	const tm lt = localFrom(ts);

	TimeUtils::Time32 a(ts, 123);
	EXPECT_EQ(a.date(), dateOf(lt));
	EXPECT_EQ(a.time(), timeOf(lt));
	EXPECT_EQ(a.time_ms(), timeOf(lt) * 1000u + 123u);
}

TEST(l01_timeutils, time32_from_milliseconds_and_from_local_time)
{
	const uint64_t ms = 1700000000ull * 1000ull + 456ull;
	TimeUtils::Time32 a(ms);

	TimeUtils::Time32 b;
	b.from_local_time(ms);

	TimeUtils::Time32 c(static_cast<time_t>(ms / 1000), static_cast<uint32_t>(ms % 1000));

	EXPECT_EQ(a.date(), c.date());
	EXPECT_EQ(a.time(), c.time());
	EXPECT_EQ(b.date(), a.date());
	EXPECT_EQ(b.time_ms(), a.time_ms());
	EXPECT_EQ(a.time_ms(), timeOf(localFrom(static_cast<time_t>(ms / 1000))) * 1000u + 456u);
}

TEST(l01_timeutils, time32_default_zero_msec)
{
	const time_t ts = 1700000000;
	TimeUtils::Time32 a(ts);
	// 默认毫秒为 0
	EXPECT_EQ(a.time_ms(), timeOf(localFrom(ts)) * 1000u);
}

TEST(l01_timeutils, time32_fmt)
{
	const time_t ts = 1700000000;
	const tm lt = localFrom(ts);

	char expect[64];
	std::memset(expect, 0, sizeof(expect));
	(void)std::strftime(expect, sizeof(expect), "%Y.%m.%d %H:%M:%S", &lt);

	char dash[32];
	std::memset(dash, 0, sizeof(dash));
	(void)std::strftime(dash, sizeof(dash), "%Y-%m-%d", &lt);

	TimeUtils::Time32 a(ts, 123);
	EXPECT_STREQ(a.fmt(), expect);
	EXPECT_STREQ(a.fmt("%Y-%m-%d"), dash);

	const std::string withMs = std::string(expect) + ",123";
	EXPECT_STREQ(a.fmt("%Y.%m.%d %H:%M:%S", true), withMs.c_str());
}

TEST(l01_timeutils, time32_fmt_returns_shared_static_buffer)
{
	// BUG-L01-009：fmt() 返回函数级 static 缓冲区（非 thread_local），
	// 不同实例共用同一块内存，多线程/同表达式内两次调用会互相覆盖。
	const time_t ts = 1700000000;
	TimeUtils::Time32 a(ts, 1);
	TimeUtils::Time32 b(ts + 86400, 2);

	const char* pa = a.fmt();
	const char* pb = b.fmt();
	EXPECT_EQ(pa, pb);
	EXPECT_STREQ(pa, b.fmt());
}

// ------------------------------------------------------------------ 时钟类接口（区间断言）
TEST(l01_timeutils, wall_clock_helpers_are_sane)
{
	const int64_t now = TimeUtils::getLocalTimeNow();
	EXPECT_GT(now, 1600000000000);
	EXPECT_LT(now, 4000000000000);

	const uint32_t date = TimeUtils::getCurDate();
	EXPECT_GE(date, 20200101u);
	EXPECT_LE(date, 21001231u);

	EXPECT_LT(TimeUtils::getCurMin(), 240000u);
	EXPECT_LT(TimeUtils::getWeekDay(), 7u);

	uint32_t d = 0;
	uint32_t t = 0;
	TimeUtils::getDateTime(d, t);
	// 只校验区间，避免和上面 getCurDate() 之间跨午夜造成偶发不等
	EXPECT_GE(d, 20200101u);
	EXPECT_LE(d, 21001231u);
	EXPECT_LT(t, 240000u);

	const uint64_t ymdhms = TimeUtils::getYYYYMMDDhhmmss();
	EXPECT_GE(ymdhms / 10000000000ull, 2020ull);
	EXPECT_LE(ymdhms / 10000000000ull, 2100ull);

	const std::string s = TimeUtils::getLocalTime();
	EXPECT_GE(s.size(), 10u);
}

TEST(l01_timeutils, ticker_measures_elapsed_time)
{
	TimeUtils::Ticker tick;
	EXPECT_GE(tick.nano_seconds(), 0);
	EXPECT_GE(tick.micro_seconds(), 0);
	EXPECT_GE(tick.milli_seconds(), 0);
	EXPECT_GE(tick.seconds(), 0);
	EXPECT_GE(tick.nano_seconds(), tick.milli_seconds());

	tick.reset();
	EXPECT_EQ(tick.seconds(), 0);
}
