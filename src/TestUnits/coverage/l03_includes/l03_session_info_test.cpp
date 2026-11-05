// L03 Includes  lane 单元测试：WTSSessionInfo 交易时段 / 交易日偏移纯函数
// 目标文件: src/Includes/WTSSessionInfo.hpp
// 全部为确定性用例：不读取真实时钟（唯一涉及 getDateTime 的分支只做量级断言），不依赖共享内存或外部服务。
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "Includes/WTSSessionInfo.hpp"
#include "Includes/WTSMarcos.h"

using namespace wtp;

namespace
{
// 白盘三段：0900-1015 / 1030-1130 / 1330-1500，合计 225 分钟
class L03DaySessionTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		sess_ = WTSSessionInfo::create("L03_DAY", "白盘三段", 0);
		ASSERT_NE(sess_, nullptr);
		sess_->addTradingSection(900, 1015);
		sess_->addTradingSection(1030, 1130);
		sess_->addTradingSection(1330, 1500);
	}

	void TearDown() override
	{
		if (sess_ != nullptr)
		{
			sess_->release();
			sess_ = nullptr;
		}
	}

	WTSSessionInfo* sess_ = nullptr;
};

//////////////////////////////////////////////////////////////////////////
// create / 基本访问器 / 空时段防御分支
//////////////////////////////////////////////////////////////////////////
TEST(l03_session, create_and_id_name)
{
	WTSSessionInfo* s = WTSSessionInfo::create("SID_01", "时段一", 105);
	ASSERT_NE(s, nullptr);
	EXPECT_STREQ(s->id(), "SID_01");
	EXPECT_STREQ(s->name(), "时段一");
	EXPECT_EQ(s->getOffsetMins(), 105);
	EXPECT_EQ(s->getSectionCount(), 0u);
	s->release();

	// 默认 offset = 0
	WTSSessionInfo* d = WTSSessionInfo::create("SID_02", "时段二");
	ASSERT_NE(d, nullptr);
	EXPECT_EQ(d->getOffsetMins(), 0);
	EXPECT_STREQ(d->id(), "SID_02");
	d->release();
}

TEST(l03_session, empty_sections_fallback_branches)
{
	WTSSessionInfo* s = WTSSessionInfo::create("L03_EMPTY", "空时段", 0);
	ASSERT_NE(s, nullptr);

	// 空时段时 open/close 直接返回 0，换算函数返回 INVALID_UINT32
	EXPECT_EQ(s->getOpenTime(), 0u);
	EXPECT_EQ(s->getOpenTime(true), 0u);
	EXPECT_EQ(s->getCloseTime(), 0u);
	EXPECT_EQ(s->getCloseTime(true), 0u);
	EXPECT_EQ(s->timeToMinutes(930), INVALID_UINT32);
	EXPECT_EQ(s->timeToMinutes(930, true), INVALID_UINT32);
	EXPECT_EQ(s->minuteToTime(10), INVALID_UINT32);
	EXPECT_EQ(s->timeToSeconds(93000), INVALID_UINT32);
	EXPECT_EQ(s->secondsToTime(0), INVALID_UINT32);
	EXPECT_FALSE(s->isFirstOfSection(900));
	EXPECT_FALSE(s->isLastOfSection(900));
	EXPECT_FALSE(s->isInTradingTime(930));
	// 集合竞价未配置时恒为 false
	EXPECT_FALSE(s->isInAuctionTime(858));
	// 空集合竞价时 getAuctionStartTime 返回 (uint32_t)-1，即 INVALID_UINT32
	EXPECT_EQ(s->getAuctionStartTime(), INVALID_UINT32);
	EXPECT_EQ(s->getAuctionStartTime(true), INVALID_UINT32);
	EXPECT_EQ(s->getAuctionSections().size(), 0u);
	// count == 0 时被当作全天候时段
	EXPECT_EQ(s->getTradingMins(), 1440u);
	EXPECT_EQ(s->getTradingSeconds(), 1440u * 60u);
	const std::vector<uint32_t>& mins = s->getSecMinList();
	ASSERT_EQ(mins.size(), 1u);
	EXPECT_EQ(mins[0], 1440u);

	s->release();
}

//////////////////////////////////////////////////////////////////////////
// offsetTime：左对齐 / 右对齐分支，0 点与 24 点约定
//////////////////////////////////////////////////////////////////////////
TEST(l03_session, offset_time_zero_offset)
{
	WTSSessionInfo* s = WTSSessionInfo::create("L03_OFF0", "零偏移", 0);
	ASSERT_NE(s, nullptr);

	EXPECT_EQ(s->offsetTime(930, true), 930u);
	EXPECT_EQ(s->offsetTime(930, false), 930u);
	// 0 点：左对齐按 0 点算，右对齐按 24 点算
	EXPECT_EQ(s->offsetTime(0, true), 0u);
	EXPECT_EQ(s->offsetTime(0, false), 2400u);
	// 恰好 1440 分钟：左对齐回绕成 0000，右对齐保留成 2400
	EXPECT_EQ(s->offsetTime(2359, true), 2359u);
	s->release();
}

TEST(l03_session, offset_time_positive_offset_wrap)
{
	// +300 分钟（5 小时）偏移
	WTSSessionInfo* s = WTSSessionInfo::create("L03_OFFP", "正偏移", 300);
	ASSERT_NE(s, nullptr);

	EXPECT_EQ(s->offsetTime(2100, true), 200u);	// 1260+300=1560 -> 120 -> 0200
	EXPECT_EQ(s->offsetTime(2100, false), 200u);
	EXPECT_EQ(s->offsetTime(900, true), 1400u);	// 540+300=840 -> 1400
	// 恰好等于 1440 的临界：左对齐 >=1440 回绕为 0，右对齐 >1440 才回绕 -> 保留 2400
	EXPECT_EQ(s->offsetTime(1900, true), 0u);	// 1140+300=1440 -> 0
	EXPECT_EQ(s->offsetTime(1900, false), 2400u);
	s->release();
}

TEST(l03_session, offset_time_negative_offset_wrap)
{
	// -540 分钟（9 小时）偏移
	WTSSessionInfo* s = WTSSessionInfo::create("L03_OFFN", "负偏移", -540);
	ASSERT_NE(s, nullptr);

	EXPECT_EQ(s->offsetTime(2300, true), 1400u);	// 1380-540=840
	EXPECT_EQ(s->offsetTime(0, true), 1500u);		// 0-540=-540 -> +1440=900 分钟 -> 1500
	s->release();

	// 单独把上面这组临界值算清楚再断言
	WTSSessionInfo* n = WTSSessionInfo::create("L03_OFFN2", "负偏移二", -60);
	ASSERT_NE(n, nullptr);
	// -60：0000 -> -60 -> 左对齐 +1440 = 1380 = 2300
	EXPECT_EQ(n->offsetTime(0, true), 2300u);
	// 右对齐时 <=0 也 +1440，行为与左对齐一致
	EXPECT_EQ(n->offsetTime(0, false), 2300u);
	// 0030 -> 30-60 = -30 -> 1410 -> 2330
	EXPECT_EQ(n->offsetTime(30, false), 2330u);
	// 恰好 curMinute==0 时，右对齐分支 <=0 命中，左对齐分支不命中
	WTSSessionInfo* z = WTSSessionInfo::create("L03_OFFZ", "零和偏移", -540);
	ASSERT_NE(z, nullptr);
	// 0900 -> 540-540 = 0：左对齐保留 0，右对齐 +1440 = 2400
	EXPECT_EQ(z->offsetTime(900, true), 0u);
	EXPECT_EQ(z->offsetTime(900, false), 2400u);
	z->release();
	n->release();
}

TEST(l03_session, original_time_inverse_mapping)
{
	WTSSessionInfo* s = WTSSessionInfo::create("L03_ORIG", "反偏移", 300);
	ASSERT_NE(s, nullptr);
	// offsetTime 与 originalTime 互为逆运算（含回绕）
	EXPECT_EQ(s->originalTime(200), 2100u);	// 120-300=-180 -> 1260
	EXPECT_EQ(s->originalTime(1400), 900u);
	EXPECT_EQ(s->originalTime(400), 2300u);
	s->release();

	WTSSessionInfo* zero = WTSSessionInfo::create("L03_ORIG0", "零偏移反", 0);
	ASSERT_NE(zero, nullptr);
	// offset == 0 走直通分支
	EXPECT_EQ(zero->originalTime(1500), 1500u);
	EXPECT_EQ(zero->originalTime(0), 0u);
	zero->release();
}

TEST(l03_session, offset_arithmetic_keeps_valid_offsets_and_avoids_int32_overflow)
{
	struct OffsetCase { int32_t offset; uint32_t start; uint32_t end; };
	const OffsetCase cases[] = {
		{0, 900, 1000}, {-420, 200, 300}, {500, 1720, 1820}, {-1300, 1120, 1220}
	};
	for (const OffsetCase& tc : cases)
	{
		WTSSessionInfo* s = WTSSessionInfo::create("L03_SAFE", "valid", tc.offset);
		ASSERT_NE(s, nullptr);
		EXPECT_EQ(tc.start, s->offsetTime(900, true));
		EXPECT_EQ(tc.start, s->offsetTime(900, false));
		EXPECT_EQ(tc.end, s->offsetTime(1000, true));
		EXPECT_EQ(tc.end, s->offsetTime(1000, false));
		EXPECT_EQ(900u, s->originalTime(tc.start));
		EXPECT_EQ(1000u, s->originalTime(tc.end));
		s->release();
	}

	// These calls exist solely to expose integer UB under UBSan. Multi-day
	// offset validity and associated date semantics are not established.
	for (const int32_t offset : { (std::numeric_limits<int32_t>::min)(),
		(std::numeric_limits<int32_t>::max)() })
	{
		WTSSessionInfo* s = WTSSessionInfo::create("L03_EXTREME", "extreme", offset);
		ASSERT_NE(s, nullptr);
		volatile uint32_t left = s->offsetTime(900, true);
		volatile uint32_t right = s->offsetTime(1000, false);
		volatile uint32_t original = s->originalTime(900);
		volatile uint32_t date = s->getOffsetDate(20261101, 900);
		(void)left; (void)right; (void)original; (void)date;
		s->release();
	}
}

TEST(l03_session, multi_day_offsets_normalize_endpoints_and_checked_date)
{
	struct Example { int32_t offset; uint32_t time; uint32_t left; uint32_t right; uint32_t date; };
	const Example examples[] = {
		{1441, 0, 1, 1, 20240229},
		{1441, 2359, 0, 2400, 20240301},
		{-1441, 0, 2359, 2359, 20240226},
		{-1441, 900, 859, 859, 20240227},
		{2880, 900, 900, 900, 20240301},
		{-2880, 900, 900, 900, 20240226},
		{2880, 0, 0, 2400, 20240301},
		{-2880, 0, 0, 2400, 20240226},
		{(std::numeric_limits<int32_t>::max)(), 900, 1107, 1107, 61070323},
	};
	for (const auto& e : examples)
	{
		auto* s = WTSSessionInfo::create("L03_MULTI", "multi-day", e.offset);
		ASSERT_NE(s, nullptr);
		uint32_t date = 123;
		EXPECT_EQ(e.left, s->offsetTime(e.time, true));
		EXPECT_EQ(e.right, s->offsetTime(e.time, false));
		ASSERT_TRUE(s->tryGetOffsetDate(20240228, e.time, date));
		EXPECT_EQ(e.date, date);
		EXPECT_EQ(e.date, s->getOffsetDate(20240228, e.time));
		EXPECT_EQ(e.time % 2400, s->originalTime(s->offsetTime(e.time, true)));
		s->release();
	}
}

TEST(l03_session, checked_offset_date_rejects_out_of_range_without_publication)
{
	auto* s = WTSSessionInfo::create("L03_LIMIT", "limit", (std::numeric_limits<int32_t>::min)());
	ASSERT_NE(s, nullptr);
	uint32_t result = 123;
	EXPECT_FALSE(s->tryGetOffsetDate(20240228, 900, result));
	EXPECT_EQ(123u, result);
	s->setOffsetMins(2880);
	for (auto input : {std::pair<uint32_t,uint32_t>{20240230, 900}, {20241301, 900},
		{99991231, 900}, {20240228, 1260}, {20240228, 2401}})
	{
		result = 123;
		EXPECT_FALSE(s->tryGetOffsetDate(input.first, input.second, result));
		EXPECT_EQ(123u, result);
	}
	s->release();
}

TEST(l03_session, offset_setter_rebuilds_sections_after_multiple_days)
{
	auto* s = WTSSessionInfo::create("L03_RESET", "reset", 0);
	ASSERT_NE(s, nullptr);
	s->addTradingSection(900, 1000);
	for (int32_t offset : {2880, -2880, 0})
	{
		s->setOffsetMins(offset);
		ASSERT_EQ(1u, s->getTradingSections().size());
		EXPECT_EQ(900u, s->getTradingSections()[0].first);
		EXPECT_EQ(1000u, s->getTradingSections()[0].second);
		EXPECT_EQ(60u, s->getTradingMins());
	}
	s->release();
}

//////////////////////////////////////////////////////////////////////////
// getOffsetDate：交易日归属
//////////////////////////////////////////////////////////////////////////
TEST(l03_session, offset_date_branches)
{
	WTSSessionInfo* s = WTSSessionInfo::create("L03_DATE", "日期偏移", 300);
	ASSERT_NE(s, nullptr);
	// 2100 + 300 = 1560 >= 1440 -> 下一交易日
	EXPECT_EQ(s->getOffsetDate(20240115, 2100), 20240116u);
	// 跨月/跨年/闰日
	EXPECT_EQ(s->getOffsetDate(20240229, 2100), 20240301u);
	EXPECT_EQ(s->getOffsetDate(20231231, 2100), 20240101u);
	// 未越过 1440 -> 原日期
	EXPECT_EQ(s->getOffsetDate(20240115, 900), 20240115u);
	s->release();

	WTSSessionInfo* n = WTSSessionInfo::create("L03_DATEN", "负日期偏移", -540);
	ASSERT_NE(n, nullptr);
	// 0030 - 540 < 0 -> 上一日
	EXPECT_EQ(n->getOffsetDate(20240115, 30), 20240114u);
	EXPECT_EQ(n->getOffsetDate(20240301, 30), 20240229u);
	EXPECT_EQ(n->getOffsetDate(20240101, 30), 20231231u);
	// 0900 - 540 = 0 -> 恰好不小于 0，仍归属当日
	EXPECT_EQ(n->getOffsetDate(20240115, 900), 20240115u);
	n->release();

	WTSSessionInfo* z = WTSSessionInfo::create("L03_DATE0", "零日偏移", 0);
	ASSERT_NE(z, nullptr);
	EXPECT_EQ(z->getOffsetDate(20241008, 1500), 20241008u);
	// uDate == 0 的分支内部读取当前时间，只做量级断言，保证确定性
	const uint32_t cur = z->getOffsetDate();
	EXPECT_GT(cur, 20200101u);
	EXPECT_LT(cur, 21000101u);
	z->release();
}

//////////////////////////////////////////////////////////////////////////
// addTradingSection：原始值与偏移值同时落表
//////////////////////////////////////////////////////////////////////////
TEST_F(L03DaySessionTest, trading_sections_keep_raw_and_offset)
{
	EXPECT_EQ(sess_->getSectionCount(), 3u);
	const WTSSessionInfo::TradingTimes& secs = sess_->getTradingSections();
	ASSERT_EQ(secs.size(), 3u);
	EXPECT_EQ(secs[0].first_raw, 900u);
	EXPECT_EQ(secs[0].first, 900u);
	EXPECT_EQ(secs[0].second_raw, 1015u);
	EXPECT_EQ(secs[0].second, 1015u);
	EXPECT_EQ(secs[2].first_raw, 1330u);
	EXPECT_EQ(secs[2].second, 1500u);

	EXPECT_EQ(sess_->getOpenTime(), 900u);
	EXPECT_EQ(sess_->getOpenTime(true), 900u);
	EXPECT_EQ(sess_->getCloseTime(), 1500u);
	EXPECT_EQ(sess_->getCloseTime(true), 1500u);
}

TEST(l03_session, night_section_raw_vs_offseted)
{
	WTSSessionInfo* s = WTSSessionInfo::create("L03_NIGHT", "夜盘", 300);
	ASSERT_NE(s, nullptr);
	s->addTradingSection(2100, 2300);
	const WTSSessionInfo::TradingTimes& secs = s->getTradingSections();
	ASSERT_EQ(secs.size(), 1u);
	EXPECT_EQ(secs[0].first_raw, 2100u);
	EXPECT_EQ(secs[0].second_raw, 2300u);
	EXPECT_EQ(secs[0].first, 200u);
	EXPECT_EQ(secs[0].second, 400u);
	// bOffseted 决定取偏移后还是原始时间
	EXPECT_EQ(s->getOpenTime(), 2100u);
	EXPECT_EQ(s->getOpenTime(true), 200u);
	EXPECT_EQ(s->getCloseTime(), 2300u);
	EXPECT_EQ(s->getCloseTime(true), 400u);
	s->release();
}

//////////////////////////////////////////////////////////////////////////
// timeToMinutes
//////////////////////////////////////////////////////////////////////////
TEST_F(L03DaySessionTest, time_to_minutes_inside_sections)
{
	EXPECT_EQ(sess_->timeToMinutes(900), 0u);
	EXPECT_EQ(sess_->timeToMinutes(1000), 60u);
	EXPECT_EQ(sess_->timeToMinutes(1015), 75u);
	// 第二段起点 -> 第一段的 75 分钟
	EXPECT_EQ(sess_->timeToMinutes(1030), 75u);
	EXPECT_EQ(sess_->timeToMinutes(1130), 135u);
	EXPECT_EQ(sess_->timeToMinutes(1330), 135u);
	// 全天最后 225 分钟
	EXPECT_EQ(sess_->timeToMinutes(1500), 225u);
}

TEST_F(L03DaySessionTest, time_to_minutes_break_and_auto_adjust)
{
	// 小节休息 1016-1029：不开启 autoAdjust 时找不到区间
	EXPECT_EQ(sess_->timeToMinutes(1020), INVALID_UINT32);
	// 开启后对齐到下一交易区间，返回已累计分钟数
	EXPECT_EQ(sess_->timeToMinutes(1020, true), 75u);
	// 开盘前 / 收盘后都不在区间内
	EXPECT_EQ(sess_->timeToMinutes(830), INVALID_UINT32);
	EXPECT_EQ(sess_->timeToMinutes(830, true), 0u);
	EXPECT_EQ(sess_->timeToMinutes(1530), INVALID_UINT32);
	EXPECT_EQ(sess_->timeToMinutes(1530, true), INVALID_UINT32);
}

TEST(l03_session, time_to_minutes_offset_nonmonotonic_sections)
{
	WTSSessionInfo* s = WTSSessionInfo::create("L03_NONMONO", "偏移后非单调时段", 300);
	ASSERT_NE(s, nullptr);
	s->addTradingSection(900, 1500);
	s->addTradingSection(2100, 2300);
	s->addTradingSection(0, 200);

	// 保留登记顺序；+300 后 HHMM 顺序为 14:00、02:00、05:00。
	const auto& sections = s->getTradingSections();
	ASSERT_EQ(sections.size(), 3u);
	EXPECT_EQ(sections[0].first, 1400u);
	EXPECT_EQ(sections[1].first, 200u);
	EXPECT_EQ(sections[2].first, 500u);

	// 各段开始、段内和闭合边界都累计声明顺序中先前段的长度。
	EXPECT_EQ(s->timeToMinutes(900), 0u);
	EXPECT_EQ(s->timeToMinutes(1000), 60u);
	EXPECT_EQ(s->timeToMinutes(1500), 360u);
	EXPECT_EQ(s->timeToMinutes(2100), 360u);
	EXPECT_EQ(s->timeToMinutes(2200), 420u);
	EXPECT_EQ(s->timeToMinutes(2300), 480u);
	EXPECT_EQ(s->timeToMinutes(0), 480u);
	EXPECT_EQ(s->timeToMinutes(100), 540u);
	EXPECT_EQ(s->timeToMinutes(200), 600u);

	// 不属于任何连续竞价段的时刻保持无映射；普通午休/开盘前调整由既有合同用例覆盖。
	EXPECT_EQ(s->timeToMinutes(1530), INVALID_UINT32);
	EXPECT_EQ(s->timeToMinutes(2030), INVALID_UINT32);
	EXPECT_EQ(s->timeToMinutes(2330), INVALID_UINT32);

	// 夜盘末段后的空档不作新的自动对齐约定。
	EXPECT_EQ(s->timeToMinutes(201), INVALID_UINT32);

	s->release();
}

TEST_F(L03DaySessionTest, auction_time_short_circuits_to_zero)
{
	sess_->setAuctionTime(855, 859);
	EXPECT_TRUE(sess_->isInAuctionTime(856));
	EXPECT_TRUE(sess_->isInAuctionTime(855));
	// 右边界开区间
	EXPECT_FALSE(sess_->isInAuctionTime(859));
	// 集合竞价时间直接算作 0 分钟
	EXPECT_EQ(sess_->timeToMinutes(858), 0u);
	// timeToSeconds 取 hhmm 部分判断集合竞价
	EXPECT_EQ(sess_->timeToSeconds(85630), 0u);
}

//////////////////////////////////////////////////////////////////////////
// minuteToTime
//////////////////////////////////////////////////////////////////////////
TEST_F(L03DaySessionTest, minute_to_time_forward_and_boundary)
{
	EXPECT_EQ(sess_->minuteToTime(0), 900u);
	EXPECT_EQ(sess_->minuteToTime(1), 901u);
	EXPECT_EQ(sess_->minuteToTime(60), 1000u);
	// 区间末尾：75 落在第一段闭区间右端
	EXPECT_EQ(sess_->minuteToTime(75), 1015u);
	// 76 进入第二段起点 + 1
	EXPECT_EQ(sess_->minuteToTime(76), 1031u);
	EXPECT_EQ(sess_->minuteToTime(135), 1130u); // 精确落在第二段末尾，返回前段收盘时间
	EXPECT_EQ(sess_->minuteToTime(225), 1500u);
	// 超出总时长：落到 getCloseTime() 兜底，而不是 INVALID_UINT32
	EXPECT_EQ(sess_->minuteToTime(226), 1500u);
	EXPECT_EQ(sess_->minuteToTime(4000), 1500u);
}

TEST_F(L03DaySessionTest, minute_to_time_head_first_mode)
{
	// bHeadFirst=true 时区间右端为开区间
	EXPECT_EQ(sess_->minuteToTime(0, true), 900u);
	EXPECT_EQ(sess_->minuteToTime(74, true), 1014u);
	// 75 已经越过第一段，进入第二段头部
	EXPECT_EQ(sess_->minuteToTime(75, true), 1030u);
	EXPECT_EQ(sess_->minuteToTime(224, true), 1459u);
	EXPECT_EQ(sess_->minuteToTime(225, true), 1500u);
}

TEST(l03_session, minute_to_time_24h_uses_2400_convention)
{
	WTSSessionInfo* s = WTSSessionInfo::create("L03_24H", "全天候", 0);
	ASSERT_NE(s, nullptr);
	// 0 点开、0 点收：second 右对齐后为 2400，first 左对齐后为 0000
	s->addTradingSection(0, 0);
	const WTSSessionInfo::TradingTimes& secs = s->getTradingSections();
	ASSERT_EQ(secs.size(), 1u);
	EXPECT_EQ(secs[0].first, 0u);
	EXPECT_EQ(secs[0].second, 2400u);

	EXPECT_EQ(s->getTradingMins(), 1440u);
	EXPECT_EQ(s->getTradingSeconds(), 1440u * 60u);
	EXPECT_EQ(s->minuteToTime(0), 0u);
	EXPECT_EQ(s->minuteToTime(721), 1201u);
	EXPECT_EQ(s->minuteToTime(1439), 2359u);
	// 满 1440 分钟时返回偏移后的收盘时间 2400
	EXPECT_EQ(s->minuteToTime(1440), 2400u);
	// 但 getCloseTime() 默认取原始值，是 0 —— 2400/0 两种约定同时存在
	EXPECT_EQ(s->getCloseTime(), 0u);
	EXPECT_EQ(s->getCloseTime(true), 2400u);
	// 越界分钟数返回原始收盘时间 0
	EXPECT_EQ(s->minuteToTime(1441), 0u);
	s->release();
}

//////////////////////////////////////////////////////////////////////////
// timeToSeconds / secondsToTime
//////////////////////////////////////////////////////////////////////////
TEST_F(L03DaySessionTest, seconds_round_trip)
{
	EXPECT_EQ(sess_->timeToSeconds(90000), 0u);
	EXPECT_EQ(sess_->timeToSeconds(90001), 1u);
	EXPECT_EQ(sess_->timeToSeconds(100000), 3600u);
	// 区间闭右端被减 1，表示最后一秒
	EXPECT_EQ(sess_->timeToSeconds(101500), 4499u);
	EXPECT_EQ(sess_->timeToSeconds(103001), 4501u);
	EXPECT_EQ(sess_->timeToSeconds(113000), 8099u);
	EXPECT_EQ(sess_->timeToSeconds(150000), 13499u);
	// 休息段与收盘后都找不到区间
	EXPECT_EQ(sess_->timeToSeconds(102000), INVALID_UINT32);
	EXPECT_EQ(sess_->timeToSeconds(153000), INVALID_UINT32);

	// 反向映射
	EXPECT_EQ(sess_->secondsToTime(0), 90000u);
	EXPECT_EQ(sess_->secondsToTime(1), 90001u);
	EXPECT_EQ(sess_->secondsToTime(4499), 101459u);
	EXPECT_EQ(sess_->secondsToTime(4500), 101500u);
	EXPECT_EQ(sess_->secondsToTime(4501), 103001u);
	EXPECT_EQ(sess_->secondsToTime(13499), 145959u);
	EXPECT_EQ(sess_->secondsToTime(13500), 150000u);
	// 越界返回 INVALID_UINT32（与 minuteToTime 的兜底策略不一致）
	EXPECT_EQ(sess_->secondsToTime(13501), INVALID_UINT32);
	EXPECT_EQ(sess_->secondsToTime(999999), INVALID_UINT32);
}

//////////////////////////////////////////////////////////////////////////
// getTradingMins / getTradingSeconds / getSecMinList
//////////////////////////////////////////////////////////////////////////
TEST_F(L03DaySessionTest, trading_minutes_and_section_list)
{
	EXPECT_EQ(sess_->getTradingMins(), 225u);
	EXPECT_EQ(sess_->getTradingSeconds(), 225u * 60u);

	const std::vector<uint32_t>& mins = sess_->getSecMinList();
	ASSERT_EQ(mins.size(), 3u);
	EXPECT_EQ(mins[0], 75u);
	EXPECT_EQ(mins[1], 135u);
	EXPECT_EQ(mins[2], 225u);
	// 结果被缓存，二次调用返回同一份
	EXPECT_EQ(sess_->getSecMinList().size(), 3u);
}

TEST(l03_session, all_day_section_aggregate_views_survive_offset_changes)
{
	for (int32_t initialOffset : {0, 300, -300})
	{
		WTSSessionInfo* s = WTSSessionInfo::create("L03_ALL_DAY", "all day", initialOffset);
		ASSERT_NE(s, nullptr);
		s->addTradingSection(0, 0);
		const auto check = [&] {
			EXPECT_EQ(s->getTradingMins(), 1440u);
			EXPECT_EQ(s->getTradingSeconds(), 1440u * 60u);
			ASSERT_EQ(s->getSecMinList().size(), 1u);
			EXPECT_EQ(s->getSecMinList().back(), 1440u);
		};
		check();
		for (int32_t nextOffset : {300, -300, 0})
		{
			s->setOffsetMins(nextOffset);
			check();
		}
		s->release();
	}
}

// BUG-L03-010 (S5, decision card 2): 0-0 is the only full-day section; it
// cannot be mixed with another section in either order, so the full-day
// aggregate views never exceed one day.
TEST(l03_session, full_day_section_is_not_mixed_with_other_sections)
{
	for (int32_t offset : {0, 300, -300})
	{
		WTSSessionInfo* s = WTSSessionInfo::create("L03_MIX", "mixed", offset);
		ASSERT_TRUE(s->tryAddTradingSection(900, 1500));
		EXPECT_FALSE(s->tryAddTradingSection(0, 0));
		s->addTradingSection(0, 0);
		EXPECT_EQ(1u, s->getSectionCount());
		EXPECT_EQ(360u, s->getTradingMins());
		s->release();

		WTSSessionInfo* all = WTSSessionInfo::create("L03_ALL", "all", offset);
		ASSERT_TRUE(all->tryAddTradingSection(0, 0));
		EXPECT_FALSE(all->tryAddTradingSection(900, 1500));
		EXPECT_FALSE(all->tryAddTradingSection(0, 0));
		EXPECT_EQ(1u, all->getSectionCount());
		EXPECT_EQ(1440u, all->getTradingMins());
		ASSERT_EQ(1u, all->getSecMinList().size());
		EXPECT_EQ(1440u, all->getSecMinList().back());
		all->release();
	}
}

TEST(l03_session, non_midnight_zero_length_section_is_rejected)
{
	auto* s = WTSSessionInfo::create("L03_ZERO", "zero width");
	ASSERT_TRUE(s->tryAddTradingSection(900, 1000));
	EXPECT_FALSE(s->tryAddTradingSection(900, 900));
	s->addTradingSection(900, 900);
	ASSERT_EQ(1u, s->getSectionCount());
	EXPECT_EQ(60u, s->getTradingMins());
	s->release();
}

//////////////////////////////////////////////////////////////////////////
// isInTradingTime / isFirstOfSection / isLastOfSection
//////////////////////////////////////////////////////////////////////////
TEST_F(L03DaySessionTest, trading_time_and_section_edges)
{
	EXPECT_TRUE(sess_->isInTradingTime(930));
	EXPECT_TRUE(sess_->isInTradingTime(900));
	EXPECT_TRUE(sess_->isInTradingTime(1500));
	EXPECT_FALSE(sess_->isInTradingTime(1020));
	// 严格模式下区间最后一分钟不算交易时间
	EXPECT_TRUE(sess_->isInTradingTime(1015));
	EXPECT_FALSE(sess_->isInTradingTime(1015, true));
	EXPECT_FALSE(sess_->isInTradingTime(1500, true));

	EXPECT_TRUE(sess_->isFirstOfSection(900));
	EXPECT_TRUE(sess_->isFirstOfSection(1330));
	EXPECT_FALSE(sess_->isFirstOfSection(901));

	EXPECT_TRUE(sess_->isLastOfSection(1015));
	EXPECT_TRUE(sess_->isLastOfSection(1500));
	EXPECT_FALSE(sess_->isLastOfSection(1459));
}

TEST_F(L03DaySessionTest, auction_only_maps_minutes_and_does_not_create_trading_time)
{
	// Auction before the first trading section maps to minute zero, but is not
	// itself a trading interval.
	sess_->setAuctionTime(855, 905);
	EXPECT_TRUE(sess_->isInAuctionTime(856));
	EXPECT_EQ(0u, sess_->timeToMinutes(856));
	EXPECT_FALSE(sess_->isInTradingTime(856));

	// Where auction and a real trading section overlap, trading membership stays true.
	EXPECT_TRUE(sess_->isInAuctionTime(902));
	EXPECT_EQ(0u, sess_->timeToMinutes(902));
	EXPECT_TRUE(sess_->isInTradingTime(902));
}

TEST(l03_session, midnight_section_raw_2400_and_0_alias)
{
	WTSSessionInfo* s = WTSSessionInfo::create("L03_MID", "跨零点", 0);
	ASSERT_NE(s, nullptr);
	s->addTradingSection(2100, 2400);
	// second_raw == 2400 时，isLastOfSection 用 2400%2400=0 做别名比较，
	// 于是 0000 与 2400 被视为同一个时点（见 bugs/l03_includes.md BUG-L03-003）
	EXPECT_TRUE(s->isLastOfSection(2400));
	EXPECT_TRUE(s->isLastOfSection(0));
	EXPECT_FALSE(s->isLastOfSection(2359));
	s->release();
}

//////////////////////////////////////////////////////////////////////////
// 集合竞价多段
//////////////////////////////////////////////////////////////////////////
TEST(l03_session, auction_sections)
{
	WTSSessionInfo* s = WTSSessionInfo::create("L03_AUC", "集合竞价", 0);
	ASSERT_NE(s, nullptr);
	s->addTradingSection(900, 1500);

	s->setAuctionTime(855, 859);
	ASSERT_EQ(s->getAuctionSections().size(), 1u);
	EXPECT_EQ(s->getAuctionStartTime(), 855u);
	EXPECT_EQ(s->getAuctionStartTime(true), 855u);
	EXPECT_EQ(s->getAuctionSections()[0].second_raw, 859u);

	// 再次 setAuctionTime 只更新第 0 段，不追加
	s->setAuctionTime(2055, 2058);
	EXPECT_EQ(s->getAuctionSections().size(), 1u);
	EXPECT_EQ(s->getAuctionStartTime(), 2055u);
	EXPECT_EQ(s->getAuctionSections()[0].first, 2055u);
	EXPECT_EQ(s->getAuctionSections()[0].second, 2058u);
	EXPECT_TRUE(s->isInAuctionTime(2056));
	EXPECT_FALSE(s->isInAuctionTime(2059));

	// addAuctionTime 追加第二段
	s->addAuctionTime(1455, 1459);
	EXPECT_EQ(s->getAuctionSections().size(), 2u);
	EXPECT_TRUE(s->isInAuctionTime(1456));
	EXPECT_TRUE(s->isInAuctionTime(2056));
	s->release();
}

TEST(l03_session, empty_auction_range_with_zero_start_threshold_survives_offsets)
{
	WTSSessionInfo* s = WTSSessionInfo::create("L03_AUC_ZERO", "空竞价区间起点阈值为0", 0);
	ASSERT_NE(s, nullptr);

	s->setAuctionTime(0, 0);
	ASSERT_EQ(s->getAuctionSections().size(), 1u);
	EXPECT_EQ(s->getAuctionStartTime(), 0u);
	EXPECT_EQ(s->getAuctionStartTime(true), 0u);
	EXPECT_EQ(s->getAuctionSections()[0].first_raw, 0u);
	EXPECT_EQ(s->getAuctionSections()[0].second_raw, 0u);
	EXPECT_EQ(s->getAuctionSections()[0].first, 0u);
	EXPECT_EQ(s->getAuctionSections()[0].second, 0u);
	EXPECT_FALSE(s->isInAuctionTime(0));
	EXPECT_FALSE(s->isInAuctionTime(1200));

	// Add preserves the empty range and its configured zero start threshold;
	// set replaces only the first section.
	s->addAuctionTime(0, 0);
	ASSERT_EQ(s->getAuctionSections().size(), 2u);
	EXPECT_EQ(s->getAuctionSections()[1].first, 0u);
	EXPECT_EQ(s->getAuctionSections()[1].second, 0u);
	s->setAuctionTime(855, 859);
	EXPECT_EQ(s->getAuctionSections().size(), 2u);
	EXPECT_EQ(s->getAuctionSections()[0].first, 855u);
	EXPECT_EQ(s->getAuctionSections()[0].second, 859u);
	EXPECT_EQ(s->getAuctionSections()[1].first, 0u);
	EXPECT_EQ(s->getAuctionSections()[1].second, 0u);
	EXPECT_TRUE(s->isInAuctionTime(856));

	// Positive, repeated, and negative remapping keep the empty range inactive.
	s->setOffsetMins(300);
	EXPECT_EQ(s->getAuctionSections()[0].first, 1355u);
	EXPECT_EQ(s->getAuctionSections()[0].second, 1359u);
	EXPECT_EQ(s->getAuctionSections()[1].first, 0u);
	EXPECT_EQ(s->getAuctionSections()[1].second, 0u);
	EXPECT_TRUE(s->isInAuctionTime(856));
	EXPECT_FALSE(s->isInAuctionTime(1200));
	s->setOffsetMins(300);
	EXPECT_EQ(s->getAuctionSections()[1].first, 0u);
	EXPECT_EQ(s->getAuctionSections()[1].second, 0u);
	EXPECT_TRUE(s->isInAuctionTime(856));
	s->setOffsetMins(-300);
	EXPECT_EQ(s->getAuctionSections()[0].first, 355u);
	EXPECT_EQ(s->getAuctionSections()[0].second, 359u);
	EXPECT_EQ(s->getAuctionSections()[1].first, 0u);
	EXPECT_EQ(s->getAuctionSections()[1].second, 0u);
	EXPECT_TRUE(s->isInAuctionTime(856));
	EXPECT_FALSE(s->isInAuctionTime(1200));

	// Setting the first range back to an empty interval preserves its zero threshold.
	s->setAuctionTime(0, 0);
	EXPECT_EQ(s->getAuctionStartTime(), 0u);
	EXPECT_EQ(s->getAuctionStartTime(true), 0u);
	EXPECT_EQ(s->getAuctionSections()[0].first_raw, 0u);
	EXPECT_EQ(s->getAuctionSections()[0].second_raw, 0u);
	EXPECT_EQ(s->getAuctionSections()[0].first, 0u);
	EXPECT_EQ(s->getAuctionSections()[0].second, 0u);
	EXPECT_FALSE(s->isInAuctionTime(0));
	EXPECT_FALSE(s->isInAuctionTime(1200));
	s->setOffsetMins(300);
	EXPECT_EQ(s->getAuctionStartTime(), 0u);
	EXPECT_EQ(s->getAuctionStartTime(true), 0u);
	EXPECT_EQ(s->getAuctionSections()[0].first, 0u);
	EXPECT_EQ(s->getAuctionSections()[0].second, 0u);
	EXPECT_FALSE(s->isInAuctionTime(0));
	EXPECT_FALSE(s->isInAuctionTime(1200));

	s->release();
}

//////////////////////////////////////////////////////////////////////////
// setOffsetMins keeps registered trading section times aligned with the new offset
//////////////////////////////////////////////////////////////////////////
TEST(l03_session, set_offset_mins_keeps_registered_sections_consistent)
{
	WTSSessionInfo* s = WTSSessionInfo::create("L03_STALE", "偏移变更", 0);
	ASSERT_NE(s, nullptr);
	s->addTradingSection(900, 1500);
	// 夜盘以 21:00-23:00、00:00-02:00 两段登记。
	s->addTradingSection(2100, 2300);
	s->addTradingSection(0, 200);
	s->setAuctionTime(855, 859);
	s->addAuctionTime(2101, 2102);

	// 先访问分钟缓存。
	const std::vector<uint32_t> cached_minutes = s->getSecMinList();
	ASSERT_EQ(cached_minutes.size(), 3u);
	EXPECT_EQ(cached_minutes[0], 360u);
	EXPECT_EQ(cached_minutes[1], 480u);
	EXPECT_EQ(cached_minutes[2], 600u);
	const auto& trading_before = s->getTradingSections();
	ASSERT_EQ(trading_before.size(), 3u);
	EXPECT_EQ(trading_before[0].first_raw, 900u);
	EXPECT_EQ(trading_before[0].second_raw, 1500u);
	EXPECT_EQ(trading_before[1].first_raw, 2100u);
	EXPECT_EQ(trading_before[1].second_raw, 2300u);
	EXPECT_EQ(trading_before[2].first_raw, 0u);
	EXPECT_EQ(trading_before[2].second_raw, 200u);

	s->setOffsetMins(300);
	EXPECT_EQ(s->getOffsetMins(), 300);
	EXPECT_EQ(s->getOpenTime(false), 900u);
	EXPECT_EQ(s->getCloseTime(false), 200u);
	EXPECT_EQ(s->getOpenTime(true), 1400u);
	EXPECT_EQ(s->getCloseTime(true), 700u);
	EXPECT_EQ(s->offsetTime(900, true), 1400u);
	EXPECT_EQ(s->offsetTime(1500, true), 2000u);
	const auto& trading_plus = s->getTradingSections();
	EXPECT_EQ(trading_plus[0].first, 1400u);
	EXPECT_EQ(trading_plus[0].second, 2000u);
	EXPECT_EQ(trading_plus[1].first, 200u);
	EXPECT_EQ(trading_plus[1].second, 400u);
	EXPECT_EQ(trading_plus[2].first, 500u);
	EXPECT_EQ(trading_plus[2].second, 700u);
	EXPECT_EQ(trading_plus[0].first_raw, 900u);
	EXPECT_EQ(trading_plus[0].second_raw, 1500u);
	EXPECT_EQ(trading_plus[1].first_raw, 2100u);
	EXPECT_EQ(trading_plus[1].second_raw, 2300u);
	EXPECT_EQ(trading_plus[2].first_raw, 0u);
	EXPECT_EQ(trading_plus[2].second_raw, 200u);
	ASSERT_EQ(s->getSecMinList().size(), 3u);
	EXPECT_EQ(s->getSecMinList()[0], 360u);
	EXPECT_EQ(s->getSecMinList()[1], 480u);
	EXPECT_EQ(s->getSecMinList()[2], 600u);

	const auto& auction_plus = s->getAuctionSections();
	ASSERT_EQ(auction_plus.size(), 2u);
	EXPECT_EQ(s->getAuctionStartTime(true), 1355u);
	EXPECT_EQ(auction_plus[0].first, 1355u);
	EXPECT_EQ(auction_plus[0].second, 1359u);
	EXPECT_EQ(auction_plus[1].first, 201u);
	EXPECT_EQ(auction_plus[1].second, 202u);
	EXPECT_EQ(auction_plus[0].first_raw, 855u);
	EXPECT_EQ(auction_plus[0].second_raw, 859u);
	EXPECT_EQ(auction_plus[1].first_raw, 2101u);
	EXPECT_EQ(auction_plus[1].second_raw, 2102u);

	// 再次设为相同偏移仍从原始值重建；随后改为负偏移，不会累计上次的 +300。
	s->setOffsetMins(300);
	EXPECT_EQ(s->getTradingSections()[0].first, 1400u);
	EXPECT_EQ(s->getAuctionSections()[0].first, 1355u);
	s->setOffsetMins(-300);
	EXPECT_EQ(s->getTradingSections()[0].first, 400u);
	EXPECT_EQ(s->getTradingSections()[0].second, 1000u);
	EXPECT_EQ(s->getTradingSections()[1].first, 1600u);
	EXPECT_EQ(s->getTradingSections()[1].second, 1800u);
	EXPECT_EQ(s->getTradingSections()[2].first, 1900u);
	EXPECT_EQ(s->getTradingSections()[2].second, 2100u);
	EXPECT_EQ(s->getAuctionSections()[0].first, 355u);
	EXPECT_EQ(s->getAuctionSections()[0].second, 359u);
	EXPECT_EQ(s->getAuctionSections()[1].first, 1601u);
	EXPECT_EQ(s->getAuctionSections()[1].second, 1602u);

	// 新增交易和竞价时段继续按当前偏移生成；setAuctionTime 仍只替换第 0 段。
	s->addTradingSection(2300, 2400);
	s->setAuctionTime(2110, 2115);
	s->addAuctionTime(2230, 2235);
	ASSERT_EQ(s->getTradingSections().size(), 4u);
	EXPECT_EQ(s->getTradingSections()[3].first, 1800u);
	EXPECT_EQ(s->getTradingSections()[3].second, 1900u);
	ASSERT_EQ(s->getAuctionSections().size(), 3u);
	EXPECT_EQ(s->getAuctionSections()[0].first, 1610u);
	EXPECT_EQ(s->getAuctionSections()[0].second, 1615u);
	EXPECT_EQ(s->getAuctionSections()[1].first, 1601u);
	EXPECT_EQ(s->getAuctionSections()[2].first, 1730u);

	// 改回正偏移时，后续追加/替换的时段也以各自 raw 值重新映射。
	s->setOffsetMins(300);
	EXPECT_EQ(s->getTradingSections()[3].first, 400u);
	EXPECT_EQ(s->getTradingSections()[3].second, 500u);
	EXPECT_EQ(s->getAuctionSections()[0].first, 210u);
	EXPECT_EQ(s->getAuctionSections()[0].second, 215u);
	EXPECT_EQ(s->getAuctionSections()[2].first, 330u);
	EXPECT_EQ(s->getAuctionSections()[2].second, 335u);

	// 读取分钟缓存后再追加交易时段，缓存必须包含新段的累计分钟数。
	const std::vector<uint32_t>& before_append = s->getSecMinList();
	ASSERT_EQ(before_append.size(), 4u);
	EXPECT_EQ(before_append[3], 660u);
	// 决策 K30（2026-11-04）：与已登记交易时段重叠的追加被拒（旧夹具追加 900-915 与 900-1500 重叠），
	// 时段表与分钟缓存都不变；改为追加不重叠的 1600-1615 验证缓存失效。
	EXPECT_FALSE(s->tryAddTradingSection(900, 915));
	ASSERT_EQ(s->getTradingSections().size(), 4u);
	ASSERT_EQ(s->getSecMinList().size(), 4u);
	s->addTradingSection(1600, 1615);
	s->addAuctionTime(900, 905);
	EXPECT_EQ(s->getTradingSections()[4].first, 2100u);
	EXPECT_EQ(s->getTradingSections()[4].second, 2115u);
	EXPECT_EQ(s->getAuctionSections()[3].first, 1400u);
	EXPECT_EQ(s->getAuctionSections()[3].second, 1405u);
	const std::vector<uint32_t>& after_append = s->getSecMinList();
	ASSERT_EQ(after_append.size(), 5u);
	EXPECT_EQ(after_append[0], 360u);
	EXPECT_EQ(after_append[1], 480u);
	EXPECT_EQ(after_append[2], 600u);
	EXPECT_EQ(after_append[3], 660u);
	EXPECT_EQ(after_append[4], 675u);
	s->release();
}
}  // namespace
