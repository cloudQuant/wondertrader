/*!
 * \file r16_timeutils_test.cpp
 * \brief R16 覆盖率用例：src/Share/TimeUtils.hpp 中尚未被 l01_timeutils_test.cpp 覆盖的面
 *        （fasttime / Time32::date|time|time_ms|from_local_time / timeToString 负毫秒分支）
 *
 * 已覆盖（l01_share_wtsutils/l01_timeutils_test.cpp）的接口本轮不再重复：
 * getNextMinute / getNextMonth / timeToMinBar / minBarToDate / minBarToTime /
 * makeTime / timeToString 正常分支 / getNextDate / getWeekDay / isWeekends /
 * Time32(time_t) 的 fmt() / Ticker / getLocalTimeNow 等挂钟入口。
 *
 * 约定：期望值一律由本用例自己喂进去的输入算得（oracle 见各用例注释），
 *       不依赖测试运行时的挂钟与 TZ（mktime/localtime_r 成对使用，与时区无关）。
 */
#include <gtest/gtest.h>
#include <time.h>
#include <stdint.h>
#include <string>
#include <cstring>
#include "Share/TimeUtils.hpp"

namespace {

// 独立 oracle：把 unix 秒 + 时区小时数交给标准库 gmtime_r，再取字段。
// gmtime_r 是 UTC 分解，不读本地时区，因此跨机器结果唯一。
struct TmRef
{
	int year, mon, mday, hour, min, sec;
};

TmRef utcBreak(int64_t secSinceEpoch)
{
	time_t t = (time_t)secSinceEpoch;
	struct tm g;
	memset(&g, 0, sizeof(g));
	gmtime_r(&t, &g);
	TmRef r;
	r.year = g.tm_year;
	r.mon = g.tm_mon;
	r.mday = g.tm_mday;
	r.hour = g.tm_hour;
	r.min = g.tm_min;
	r.sec = g.tm_sec;
	return r;
}

// 用本地时区把一个「墙上时间」编成 unix 秒，作为 Time32 的输入。
// mktime 与 localtime_r 互逆，所以断言与机器时区无关。
time_t localEpoch(int y, int mon, int mday, int h, int min, int sec)
{
	struct tm t;
	memset(&t, 0, sizeof(t));
	t.tm_year = y - 1900;
	t.tm_mon = mon - 1;
	t.tm_mday = mday;
	t.tm_hour = h;
	t.tm_min = min;
	t.tm_sec = sec;
	t.tm_isdst = -1;
	return mktime(&t);
}

}	// namespace

//////////////////////////////////////////////////////////////////////////
// fasttime（TimeUtils.hpp:80-105）
TEST(r16_timeutils, fasttime_epoch_zero_utc)
{
	time_t sec = 0;
	struct tm out;
	memset(&out, 0, sizeof(out));
	int ret = TimeUtils::fasttime(sec, &out, 0);
	EXPECT_EQ(0, ret);
	TmRef e = utcBreak(0);
	// 1970-01-01T00:00:00Z
	EXPECT_EQ(e.year, out.tm_year);
	EXPECT_EQ(e.mon, out.tm_mon);
	EXPECT_EQ(e.mday, out.tm_mday);
	EXPECT_EQ(e.hour, out.tm_hour);
	EXPECT_EQ(e.min, out.tm_min);
	EXPECT_EQ(e.sec, out.tm_sec);
}

TEST(r16_timeutils, fasttime_matches_libc_for_2023_epoch_with_plus8_zone)
{
	// 1700000000 = 2023-11-14T22:13:20Z；+8 小时后是 2023-11-15T06:13:20
	time_t sec = 1700000000;
	struct tm out;
	memset(&out, 0, sizeof(out));
	TimeUtils::fasttime(sec, &out, 8);
	TmRef e = utcBreak((int64_t)sec + 8 * 3600);
	EXPECT_EQ(e.year, out.tm_year);
	EXPECT_EQ(e.mon, out.tm_mon);
	EXPECT_EQ(e.mday, out.tm_mday);
	EXPECT_EQ(6, out.tm_hour);
	EXPECT_EQ(13, out.tm_min);
	EXPECT_EQ(20, out.tm_sec);
	EXPECT_EQ(123, out.tm_year);
	EXPECT_EQ(10, out.tm_mon);
	EXPECT_EQ(15, out.tm_mday);
}

TEST(r16_timeutils, fasttime_handles_minute_and_second_fields)
{
	// 输入 = 2000-02-29T12:34:56Z（闰年日），tz=0
	time_t sec = (time_t)951827696;	// 手工核算：见下方对拍断言
	struct tm out;
	memset(&out, 0, sizeof(out));
	TimeUtils::fasttime(sec, &out, 0);
	TmRef e = utcBreak(sec);
	EXPECT_EQ(e.year, out.tm_year);
	EXPECT_EQ(e.mon, out.tm_mon);
	EXPECT_EQ(e.mday, out.tm_mday);
	EXPECT_EQ(e.hour, out.tm_hour);
	EXPECT_EQ(e.min, out.tm_min);
	EXPECT_EQ(e.sec, out.tm_sec);
}

TEST(r16_timeutils, fasttime_negative_timezone_offset)
{
	// 1700000000 = 2023-11-14T22:13:20Z，tz=-5 → 2023-11-14T17:13:20
	time_t sec = 1700000000;
	struct tm out;
	memset(&out, 0, sizeof(out));
	TimeUtils::fasttime(sec, &out, -5);
	TmRef e = utcBreak((int64_t)sec - 5 * 3600);
	EXPECT_EQ(e.year, out.tm_year);
	EXPECT_EQ(e.mon, out.tm_mon);
	EXPECT_EQ(e.mday, out.tm_mday);
	EXPECT_EQ(17, out.tm_hour);
	EXPECT_EQ(13, out.tm_min);
}

TEST(r16_timeutils, fasttime_does_not_fill_week_or_dst_fields)
{
	// 该实现只填 sec/min/hour/mday/mon/year；tm_wday/tm_yday/tm_isdst 保持调用方给的值。
	// 断言「未被写入」本身就是可证伪的：若实现补了 wday，这里就会失败。
	time_t sec = 1700000000;	// 2023-11-14 是星期二（wday=2）
	struct tm out;
	memset(&out, 0, sizeof(out));
	out.tm_wday = 42;
	out.tm_yday = 43;
	out.tm_isdst = 44;
	TimeUtils::fasttime(sec, &out, 0);
	EXPECT_EQ(42, out.tm_wday);
	EXPECT_EQ(43, out.tm_yday);
	EXPECT_EQ(44, out.tm_isdst);
}

TEST(r16_timeutils, DISABLED_fasttime_pre_epoch_date_is_wrong)
{
	// 触发条件见 bugs/R16_share_tools.md BUG-R16-01。
	// 期望：1969-12-31T00:00:00Z（fasttime 对负 epoch 走了截断除法，实际算不出这个值）
	time_t sec = -86400 * 365;	// 1968-12-31 前后，明显早于 epoch
	struct tm out;
	memset(&out, 0, sizeof(out));
	TimeUtils::fasttime(sec, &out, 0);
	TmRef e = utcBreak(sec);
	EXPECT_EQ(e.year, out.tm_year);
	EXPECT_EQ(e.mon, out.tm_mon);
	EXPECT_EQ(e.mday, out.tm_mday);
}

//////////////////////////////////////////////////////////////////////////
// Time32（TimeUtils.hpp:371-435）
TEST(r16_timeutils, time32_date_and_time_from_local_epoch)
{
	time_t e = localEpoch(2021, 3, 5, 9, 30, 15);
	ASSERT_NE(-1, (long)e);
	TimeUtils::Time32 t(e, 250);
	EXPECT_EQ(20210305u, t.date());
	EXPECT_EQ(93015u, t.time());
	EXPECT_EQ(93015250u, t.time_ms());
}

TEST(r16_timeutils, time32_ctor_from_milliseconds_splits_msec)
{
	time_t e = localEpoch(2021, 3, 5, 9, 30, 15);
	ASSERT_NE(-1, (long)e);
	// 构造参数是「毫秒级时间戳」：秒部分走 localtime_r，余数进 _msec
	TimeUtils::Time32 t((uint64_t)e * 1000 + 750);
	EXPECT_EQ(20210305u, t.date());
	EXPECT_EQ(93015u, t.time());
	EXPECT_EQ(93015750u, t.time_ms());
}

TEST(r16_timeutils, time32_ctor_without_msecs_defaults_to_zero)
{
	time_t e = localEpoch(2020, 1, 2, 3, 4, 5);
	ASSERT_NE(-1, (long)e);
	TimeUtils::Time32 t(e);
	EXPECT_EQ(20200102u, t.date());
	EXPECT_EQ(30405u, t.time());
	EXPECT_EQ(30405000u, t.time_ms());
}

TEST(r16_timeutils, time32_from_local_time_overwrites_previous_value)
{
	TimeUtils::Time32 t;	// 默认构造不初始化 tm（BUG-R16-02），只写不读
	uint64_t e = (uint64_t)localEpoch(2022, 12, 30, 23, 59, 58) * 1000 + 999;
	t.from_local_time(e);
	EXPECT_EQ(20221230u, t.date());
	EXPECT_EQ(235958u, t.time());
	EXPECT_EQ(235958999u, t.time_ms());

	// 再次覆盖：date/time/time_ms 必须跟着变
	t.from_local_time((uint64_t)localEpoch(2015, 6, 1, 0, 0, 1) * 1000 + 1);
	EXPECT_EQ(20150601u, t.date());
	EXPECT_EQ(1u, t.time());
	EXPECT_EQ(1001u, t.time_ms());
}

TEST(r16_timeutils, time32_time_ms_is_time_times_1000_plus_msec)
{
	time_t e = localEpoch(2019, 7, 4, 12, 0, 0);
	ASSERT_NE(-1, (long)e);
	TimeUtils::Time32 t(e, 13);
	// 内部自洽性：time_ms = (h*10000+m*100+s)*1000 + msec
	EXPECT_EQ((uint64_t)t.time() * 1000 + 13, (uint64_t)t.time_ms());
	EXPECT_EQ(12000013u, t.time_ms());
}

//////////////////////////////////////////////////////////////////////////
// timeToString 的负毫秒分支（TimeUtils.hpp:260-281）
TEST(r16_timeutils, time_to_string_returns_empty_for_small_negative_input)
{
	// -500/1000 截断为 0，余数 -500 < 0 → 直接返回空串
	EXPECT_EQ(std::string(""), TimeUtils::timeToString(-500));
}

TEST(r16_timeutils, time_to_string_returns_empty_for_zero)
{
	EXPECT_EQ(std::string(""), TimeUtils::timeToString(0));
}

TEST(r16_timeutils, time_to_string_without_millisec_has_no_dot)
{
	// 用本地时区构造一个毫秒为 0 的时间戳，避免依赖固定 UTC 偏移
	time_t e = localEpoch(2021, 3, 5, 9, 30, 15);
	ASSERT_NE(-1, (long)e);
	std::string s = TimeUtils::timeToString((int64_t)e * 1000);
	EXPECT_EQ(std::string::npos, s.find('.'));
	EXPECT_EQ((size_t)14, s.size());
}

TEST(r16_timeutils, time_to_string_with_millisec_appends_dot_msec)
{
	time_t e = localEpoch(2021, 3, 5, 9, 30, 15);
	ASSERT_NE(-1, (long)e);
	std::string s = TimeUtils::timeToString((int64_t)e * 1000 + 7);
	ASSERT_EQ((size_t)18, s.size());
	EXPECT_EQ(std::string(".007"), s.substr(14));
}

//////////////////////////////////////////////////////////////////////////
// getTZOffset（TimeUtils.hpp:61-78）
TEST(r16_timeutils, tz_offset_is_cached_and_in_whole_hours)
{
	int32_t a = TimeUtils::getTZOffset();
	int32_t b = TimeUtils::getTZOffset();
	EXPECT_EQ(a, b);					// static 缓存
	EXPECT_EQ(0, a % 60);				// 分钟部分被 /60 抹平
	EXPECT_LE(-12 * 60, (int)a);		// 合法时区上界（含半时区时仍在 ±13h 内）
	EXPECT_GE(14 * 60, (int)a);
}
