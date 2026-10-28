// Lane R12 - src/Includes/WTSSessionInfo.hpp 时段计算
// 覆盖：offsetTime/originalTime 偏移与 0 点对齐、getOffsetDate 跨日、
//       timeToMinutes/minuteToTime 双向映射（多时段、autoAdjust、bHeadFirst）、
//       timeToSeconds/secondsToTime、集合竞价多段与更新分支、
//       isInTradingTime/isFirst/isLast、统计面（分钟数/秒数/小节列表）、空时段
// 说明：全部时间/日期为显式入参，不读挂钟（create 后立即调用带日期入参的重载）。
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "Includes/WTSSessionInfo.hpp"

USING_NS_WTP;

namespace {

// 上期所日盘：9:30-10:15 / 10:30-11:30 / 13:30-15:00，集合竞价 9:00-9:30
WTSSessionInfo* makeDaySession()
{
	WTSSessionInfo* info = WTSSessionInfo::create("SHFE_DAY", "日盘", 0);
	info->addTradingSection(930, 1015);
	info->addTradingSection(1030, 1130);
	info->addTradingSection(1330, 1500);
	info->setAuctionTime(900, 930);
	return info;
}

}	// namespace

TEST(r12_session, create_id_and_offset_accessor)
{
	WTSSessionInfo* info = WTSSessionInfo::create("DCE_NIGHT", "夜盘", -1260);
	ASSERT_NE(info, nullptr);
	EXPECT_STREQ(info->id(), "DCE_NIGHT");
	EXPECT_STREQ(info->name(), "夜盘");
	EXPECT_EQ(info->getOffsetMins(), -1260);
	info->setOffsetMins(-1200);
	EXPECT_EQ(info->getOffsetMins(), -1200);
	// 未添加任何时段
	EXPECT_EQ(info->getSectionCount(), 0u);
	EXPECT_TRUE(info->getTradingSections().empty());
	EXPECT_TRUE(info->getAuctionSections().empty());
	info->release();
}

TEST(r12_session, offset_time_zero_point_alignment)
{
	WTSSessionInfo* info = WTSSessionInfo::create("S", "S", 0);
	// 无偏移时原样返回
	EXPECT_EQ(info->offsetTime(930, true), 930u);
	EXPECT_EQ(info->offsetTime(930, false), 930u);
	// 0 点：左对齐算 0 点，右对齐算 24 点（WTSSessionInfo.hpp:507-527）
	EXPECT_EQ(info->offsetTime(0, true), 0u);
	EXPECT_EQ(info->offsetTime(0, false), 2400u);
	info->release();

	// 负偏移把夜盘时间轴前移
	WTSSessionInfo* night = WTSSessionInfo::create("N", "N", -1260);
	EXPECT_EQ(night->offsetTime(2100, true), 0u);
	EXPECT_EQ(night->offsetTime(2130, true), 30u);
	EXPECT_EQ(night->offsetTime(2300, false), 200u);
	night->release();

	// 正偏移越过后零点回绕
	WTSSessionInfo* plus = WTSSessionInfo::create("P", "P", 60);
	EXPECT_EQ(plus->offsetTime(2330, true), 30u);
	EXPECT_EQ(plus->offsetTime(2330, false), 30u);
	plus->release();
}

TEST(r12_session, original_time_is_inverse_of_offset)
{
	WTSSessionInfo* info = WTSSessionInfo::create("S", "S", -1260);
	// 偏移分钟数非 0 时按 -offset 反算
	EXPECT_EQ(info->originalTime(30), 2130u);
	EXPECT_EQ(info->originalTime(0), 2100u);
	EXPECT_EQ(info->originalTime(1400), 2320u);
	info->release();

	WTSSessionInfo* zero = WTSSessionInfo::create("S", "S", 0);
	// offset==0 直接原样返回
	EXPECT_EQ(zero->originalTime(930), 930u);
	zero->release();
}

TEST(r12_session, get_offset_date_three_branches)
{
	WTSSessionInfo* info = WTSSessionInfo::create("S", "S", 0);
	// 落在当日 → 原日期
	EXPECT_EQ(info->getOffsetDate(20240506, 930), 20240506u);
	info->release();

	// 偏移后 >=1440 → 次日
	WTSSessionInfo* plus = WTSSessionInfo::create("S", "S", 60);
	EXPECT_EQ(plus->getOffsetDate(20240506, 2330), 20240507u);
	plus->release();

	// 偏移后 <0 → 上一日
	WTSSessionInfo* minus = WTSSessionInfo::create("S", "S", -600);
	EXPECT_EQ(minus->getOffsetDate(20240506, 900), 20240505u);
	// 夜盘 00:30 在交易日 20240506 的框架下应回到 20240506
	WTSSessionInfo* night = WTSSessionInfo::create("S", "S", -1260);
	EXPECT_EQ(night->getOffsetDate(20240507, 30), 20240506u);
	EXPECT_EQ(night->getOffsetDate(20240506, 2100), 20240506u);
	night->release();
	minus->release();
}

TEST(r12_session, trading_sections_are_stored_with_raw_and_offset)
{
	WTSSessionInfo* info = makeDaySession();
	EXPECT_EQ(info->getSectionCount(), 3u);
	const WTSSessionInfo::TradingTimes& secs = info->getTradingSections();
	ASSERT_EQ(secs.size(), (std::size_t)3);
	EXPECT_EQ(secs[0].first_raw, 930u);
	EXPECT_EQ(secs[0].first, 930u);
	EXPECT_EQ(secs[0].second_raw, 1015u);
	EXPECT_EQ(secs[2].second, 1500u);
	// 原始/偏移两种口径都能取到开收盘
	EXPECT_EQ(info->getOpenTime(), 930u);
	EXPECT_EQ(info->getOpenTime(true), 930u);
	EXPECT_EQ(info->getCloseTime(), 1500u);
	EXPECT_EQ(info->getCloseTime(true), 1500u);
	EXPECT_EQ(info->getAuctionStartTime(), 900u);
	EXPECT_EQ(info->getAuctionStartTime(true), 900u);
	info->release();

	// 空时段的开收盘返回 0，无集合竞价返回 (uint32_t)-1
	WTSSessionInfo* emptyS = WTSSessionInfo::create("E", "E", 0);
	EXPECT_EQ(emptyS->getOpenTime(), 0u);
	EXPECT_EQ(emptyS->getCloseTime(), 0u);
	EXPECT_EQ(emptyS->getAuctionStartTime(), (uint32_t)-1);
	emptyS->release();

	// 0 点收盘：原始值 0，偏移值按 24 点
	WTSSessionInfo* midnight = WTSSessionInfo::create("M", "M", 0);
	midnight->addTradingSection(2100, 0);
	EXPECT_EQ(midnight->getOpenTime(), 2100u);
	EXPECT_EQ(midnight->getCloseTime(), 0u);
	EXPECT_EQ(midnight->getCloseTime(true), 2400u);
	// second_raw%2400==0，所以 0 点也算本时段最后一分钟
	EXPECT_TRUE(midnight->isLastOfSection(0));
	midnight->release();
}

TEST(r12_session, time_to_minutes_multi_section)
{
	WTSSessionInfo* info = makeDaySession();
	// 集合竞价内固定返回 0
	EXPECT_EQ(info->timeToMinutes(915), 0u);
	// 各时段起点
	EXPECT_EQ(info->timeToMinutes(930), 0u);
	// 9:30→10:00 = 30 分钟
	EXPECT_EQ(info->timeToMinutes(1000), 30u);
	// 第一段全长 45 分钟
	EXPECT_EQ(info->timeToMinutes(1015), 45u);
	// 第二段起点 = 45
	EXPECT_EQ(info->timeToMinutes(1030), 45u);
	// 第三段 14:00 = 45+60+30 = 135
	EXPECT_EQ(info->timeToMinutes(1400), 135u);
	// 收盘 = 45+60+90 = 195
	EXPECT_EQ(info->timeToMinutes(1500), 195u);
	// 收盘之后找不到 → 非法值
	EXPECT_EQ(info->timeToMinutes(1530), INVALID_UINT32);
	info->release();
}

TEST(r12_session, time_to_minutes_auto_adjust_and_empty)
{
	WTSSessionInfo* info = WTSSessionInfo::create("S", "S", 0);
	info->addTradingSection(930, 1130);
	// 开盘前：不自动对齐找不到，自动对齐则贴到本段起点（偏移 0）
	EXPECT_EQ(info->timeToMinutes(900, false), INVALID_UINT32);
	EXPECT_EQ(info->timeToMinutes(900, true), 0u);
	EXPECT_EQ(info->timeToMinutes(1030), 60u);
	info->release();

	WTSSessionInfo* emptyS = WTSSessionInfo::create("E", "E", 0);
	EXPECT_EQ(emptyS->timeToMinutes(930), INVALID_UINT32);
	emptyS->release();
}

TEST(r12_session, minute_to_time_round_trip_both_directions)
{
	WTSSessionInfo* info = makeDaySession();
	EXPECT_EQ(info->minuteToTime(0), 930u);
	// 段边界：45 分钟正好是第一段的收盘时刻
	EXPECT_EQ(info->minuteToTime(45), 1015u);
	// 46 分钟落到第二段起点后的第一分钟
	EXPECT_EQ(info->minuteToTime(46), 1031u);
	// 全天最后一分钟
	EXPECT_EQ(info->minuteToTime(195), 1500u);
	// 超出全天长度回落为收盘时间
	EXPECT_EQ(info->minuteToTime(200), 1500u);
	// bHeadFirst=true 时，段边界归到下一段起点
	EXPECT_EQ(info->minuteToTime(0, true), 930u);
	EXPECT_EQ(info->minuteToTime(45, true), 1030u);
	// 与 timeToMinutes 互逆
	for (uint32_t m = 0; m <= 195; m += 15)
		EXPECT_EQ(info->timeToMinutes(info->minuteToTime(m)), m);
	info->release();

	WTSSessionInfo* emptyS = WTSSessionInfo::create("E", "E", 0);
	EXPECT_EQ(emptyS->minuteToTime(10), INVALID_UINT32);
	emptyS->release();
}

TEST(r12_session, night_session_minute_mapping_with_offset)
{
	WTSSessionInfo* info = WTSSessionInfo::create("N", "N", -1260);
	info->addTradingSection(2100, 2300);
	// 偏移后时段轴是 0000-0200
	const WTSSessionInfo::TradingTimes& secs = info->getTradingSections();
	EXPECT_EQ(secs[0].first, 0u);
	EXPECT_EQ(secs[0].second, 200u);
	// 21:00 → 第 0 分钟，21:30 → 第 30 分钟，23:00 → 第 120 分钟
	EXPECT_EQ(info->timeToMinutes(2100), 0u);
	EXPECT_EQ(info->timeToMinutes(2130), 30u);
	EXPECT_EQ(info->timeToMinutes(2300), 120u);
	// 反向映射要还原成原始时间（originalTime 反算偏移）
	EXPECT_EQ(info->minuteToTime(0), 2100u);
	EXPECT_EQ(info->minuteToTime(30), 2130u);
	EXPECT_EQ(info->getTradingMins(), 200u);
	info->release();
}

TEST(r12_session, seconds_mapping)
{
	WTSSessionInfo* info = makeDaySession();
	// hhmmss → 交易秒
	EXPECT_EQ(info->timeToSeconds(93000), 0u);
	// 10:00:30 = 9:30 起 30 分 30 秒 = 1830 秒
	EXPECT_EQ(info->timeToSeconds(100030), 1830u);
	// 集合竞价时间视为 0 秒
	EXPECT_EQ(info->timeToSeconds(91500), 0u);
	// 收盘之后找不到
	EXPECT_EQ(info->timeToSeconds(153000), INVALID_UINT32);

	// 秒 → hhmmss
	EXPECT_EQ(info->secondsToTime(0), 93000u);
	EXPECT_EQ(info->secondsToTime(1830), 100030u);
	// 第一段末秒（2700 秒）落在 10:15:00
	EXPECT_EQ(info->secondsToTime(2700), 101500u);
	// 全天 195 分钟 = 11700 秒 → 15:00:00
	EXPECT_EQ(info->secondsToTime(11700), 150000u);
	// 超出全天
	EXPECT_EQ(info->secondsToTime(12000), INVALID_UINT32);
	info->release();

	WTSSessionInfo* emptyS = WTSSessionInfo::create("E", "E", 0);
	EXPECT_EQ(emptyS->timeToSeconds(93000), INVALID_UINT32);
	EXPECT_EQ(emptyS->secondsToTime(0), INVALID_UINT32);
	emptyS->release();
}

TEST(r12_session, trading_duration_and_seclist)
{
	WTSSessionInfo* info = makeDaySession();
	// 45+60+90 = 195 分钟
	EXPECT_EQ(info->getTradingMins(), 195u);
	EXPECT_EQ(info->getTradingSeconds(), 195u * 60u);
	const std::vector<uint32_t>& mins = info->getSecMinList();
	ASSERT_EQ(mins.size(), (std::size_t)3);
	EXPECT_EQ(mins[0], 45u);
	EXPECT_EQ(mins[1], 105u);
	EXPECT_EQ(mins[2], 195u);
	// 第二次调用命中缓存分支，长度不变
	EXPECT_EQ(info->getSecMinList().size(), (std::size_t)3);
	info->release();

	// 无时段时按全天候 1440 分钟处理
	WTSSessionInfo* emptyS = WTSSessionInfo::create("E", "E", 0);
	EXPECT_EQ(emptyS->getTradingMins(), 1440u);
	EXPECT_EQ(emptyS->getTradingSeconds(), 1440u * 60u);
	ASSERT_EQ(emptyS->getSecMinList().size(), (std::size_t)1);
	EXPECT_EQ(emptyS->getSecMinList()[0], 1440u);
	emptyS->release();
}

TEST(r12_session, in_trading_time_and_section_boundary_flags)
{
	WTSSessionInfo* info = makeDaySession();
	EXPECT_TRUE(info->isInTradingTime(1000));
	EXPECT_TRUE(info->isInTradingTime(1459));
	// 收盘分钟本身仍算交易时间
	EXPECT_TRUE(info->isInTradingTime(1500));
	// 严格模式下每段最后一分钟不算
	EXPECT_FALSE(info->isInTradingTime(1500, true));
	EXPECT_FALSE(info->isInTradingTime(1015, true));
	// 非交易时间
	EXPECT_FALSE(info->isInTradingTime(1530));
	EXPECT_FALSE(info->isInTradingTime(915));

	EXPECT_TRUE(info->isLastOfSection(1015));
	EXPECT_TRUE(info->isLastOfSection(1500));
	EXPECT_FALSE(info->isLastOfSection(1030));
	EXPECT_TRUE(info->isFirstOfSection(930));
	EXPECT_TRUE(info->isFirstOfSection(1330));
	EXPECT_FALSE(info->isFirstOfSection(1000));
	info->release();
}

TEST(r12_session, auction_multi_section_and_update)
{
	WTSSessionInfo* info = WTSSessionInfo::create("A", "A", 0);
	// 无集合竞价时 isInAuctionTime 恒 false
	EXPECT_FALSE(info->isInAuctionTime(900));
	// 首次 setAuctionTime 走 emplace 分支
	info->setAuctionTime(900, 930);
	EXPECT_EQ(info->getAuctionSections().size(), (std::size_t)1);
	EXPECT_TRUE(info->isInAuctionTime(915));
	// 上边界开区间，不算竞价
	EXPECT_FALSE(info->isInAuctionTime(930));
	// 第二次 setAuctionTime 走更新分支，不增加段数
	info->setAuctionTime(850, 859);
	EXPECT_EQ(info->getAuctionSections().size(), (std::size_t)1);
	EXPECT_EQ(info->getAuctionStartTime(), 850u);
	EXPECT_TRUE(info->isInAuctionTime(855));
	// 追加第二段（夜盘竞价）
	info->addAuctionTime(2055, 2100);
	EXPECT_EQ(info->getAuctionSections().size(), (std::size_t)2);
	EXPECT_TRUE(info->isInAuctionTime(2058));
	EXPECT_FALSE(info->isInAuctionTime(2100));
	// 0-0 的畸形竞价段被跳过
	info->addAuctionTime(0, 0);
	EXPECT_FALSE(info->isInAuctionTime(0));
	info->release();
}
