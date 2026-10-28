// R55（第十一波补测）：TimeUtils.hpp 固定日期边界 + StrUtil.hpp 分割边界。
// 只读覆盖，不改生产代码。所有期望值由固定输入推导，不使用当前时间做断言。
// 既有 l01_timeutils / r16_timeutils / ws01_strutil / r16_strutil 已覆盖的接口不再重复。
#include <gtest/gtest.h>

#include "Share/TimeUtils.hpp"
#include "Share/StrUtil.hpp"

#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

// 用本地时区把「墙上时间」编成 epoch；mktime/localtime_r 互逆，断言与机器时区无关。
time_t r55LocalEpoch(int y, int mon, int mday, int h, int min, int sec)
{
	struct tm t;
	std::memset(&t, 0, sizeof(t));
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
// getNextDate 的负偏移与跨年（TimeUtils.hpp:283-297）
TEST(R55_timeutils, get_next_date_negative_and_year_boundaries)
{
	EXPECT_EQ(20231231u, TimeUtils::getNextDate(20240101, -1));
	EXPECT_EQ(20240229u, TimeUtils::getNextDate(20240301, -1));
	EXPECT_EQ(20250101u, TimeUtils::getNextDate(20241231, 1));
	EXPECT_EQ(20230228u, TimeUtils::getNextDate(20230301, -1));
}

// getNextMonth 的整年与多月偏移（TimeUtils.hpp:319-335）
TEST(R55_timeutils, get_next_month_whole_year_and_large_offsets)
{
	EXPECT_EQ(202412u, TimeUtils::getNextMonth(202312u, 12));
	EXPECT_EQ(202601u, TimeUtils::getNextMonth(202401u, 24));
	// -12 个月：uAddYear=-1、uAddMon=0，是负数偏移里唯一正确的整年路径
	EXPECT_EQ(202301u, TimeUtils::getNextMonth(202401u, -12));
	EXPECT_EQ(202501u, TimeUtils::getNextMonth(202412u, 1));
}

// makeTime 的闰日/日末毫秒（TimeUtils.hpp:240-258）
TEST(R55_timeutils, make_time_leap_day_and_end_of_day)
{
	const int64_t base = TimeUtils::makeTime(20240229, 0);
	const int64_t end = TimeUtils::makeTime(20240229, 235959999);
	EXPECT_EQ(86399999LL, end - base);

	const int64_t noon = TimeUtils::makeTime(20240229, 123456789);
	// 12:34:56.789 = (12*3600 + 34*60 + 56)*1000 + 789
	EXPECT_EQ(45296789LL, noon - base);

	const int64_t nextDay = TimeUtils::makeTime(20240301, 0);
	EXPECT_EQ(86400000LL, nextDay - base);
}

// getLocalTime 的 bool 分支（TimeUtils.hpp:136-151）；只校验格式不校验具体钟点
TEST(R55_timeutils, get_local_time_with_and_without_milliseconds_shape)
{
	const std::string withMs = TimeUtils::getLocalTime(true);
	ASSERT_EQ(12u, withMs.size());
	EXPECT_EQ(':', withMs[2]);
	EXPECT_EQ(':', withMs[5]);
	EXPECT_EQ(',', withMs[8]);

	const std::string noMs = TimeUtils::getLocalTime(false);
	ASSERT_EQ(8u, noMs.size());
	EXPECT_EQ(':', noMs[2]);
	EXPECT_EQ(':', noMs[5]);
}

// Time32::fmt 的毫秒补零与自定义格式（TimeUtils.hpp:423-430）
TEST(R55_timeutils, time32_fmt_pads_milliseconds_to_three_digits)
{
	const time_t e = r55LocalEpoch(2021, 3, 5, 9, 30, 15);
	ASSERT_NE((time_t)-1, e);
	TimeUtils::Time32 t(e, 7);
	EXPECT_STREQ("2021.03.05 09:30:15,007", t.fmt("%Y.%m.%d %H:%M:%S", true));
	EXPECT_STREQ("2021", t.fmt("%Y", false));
}

//////////////////////////////////////////////////////////////////////////
// split2 的多字符分隔符 + 前导/尾随空段（StrUtil.hpp:166-201）
TEST(R55_strutil, split2_multichar_leading_and_trailing_delims)
{
	StringVector v = StrUtil::split2("::a::", "::");
	ASSERT_EQ((size_t)3, v.size());
	EXPECT_EQ(std::string(""), v[0]);
	EXPECT_EQ(std::string("a"), v[1]);
	EXPECT_EQ(std::string(""), v[2]);

	// 连续两组分隔符：中间产生一个空段，且每步跳过 2 个字符
	StringVector v2 = StrUtil::split2("a::::b", "::");
	ASSERT_EQ((size_t)3, v2.size());
	EXPECT_EQ(std::string("a"), v2[0]);
	EXPECT_EQ(std::string(""), v2[1]);
	EXPECT_EQ(std::string("b"), v2[2]);
}

// split 出参版本的 maxSplits + 前导分隔符组合（StrUtil.hpp:131-164）
TEST(R55_strutil, split_output_max_splits_with_leading_empty)
{
	StringVector v;
	v.emplace_back("seed");
	StrUtil::split(",a,b", v, ",", 1);
	ASSERT_EQ((size_t)3, v.size());
	EXPECT_EQ(std::string("seed"), v[0]);
	// 前导分隔符先产生空段，随后达到 maxSplits，余串整体入列
	EXPECT_EQ(std::string(""), v[1]);
	EXPECT_EQ(std::string("a,b"), v[2]);
}
