// Lane R56 - src/WTSTools/WTSDataFactory.cpp 剩余未覆盖面
// 覆盖：tick 路径 KP_Tick/KP_Minute5 分发与未知周期 default、tick 分钟线按小节对齐、
//       tick 五分钟线的 isLastOfSection 与小节对齐、bar 驱动的分钟线小节对齐钳制、
//       小时线/半小时线的 19900000 回退与 24 点跨日、extractMin1/Min5 的小节对齐
//       与未闭合末根剔除、extractHour/Half 的跨日边界。
// 所有对象均在内存中构造；不使用真实行情、不落盘、不依赖挂钟。
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "WTSTools/WTSDataFactory.h"
#include "Share/TimeUtils.hpp"

using namespace wtp;

namespace {

const char* kCode = "SHFE.rb2405";

WTSBarStruct r56_bar(uint32_t date, uint64_t time, double px, double vol)
{
	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	bar.date = date;
	bar.time = time;
	bar.open = px;
	bar.high = px + 1;
	bar.low = px - 1;
	bar.close = px + 0.5;
	bar.vol = vol;
	bar.money = px * vol;
	bar.hold = vol * 10;
	bar.add = vol;
	bar.settle = px;
	return bar;
}

WTSBarStruct r56_minbar(uint32_t date, uint32_t hhmm, double px, double vol)
{
	return r56_bar(date, TimeUtils::timeToMinBar(date, hhmm), px, vol);
}

WTSTickData* r56_tick(double px, double vol, uint32_t actMs, uint32_t actDate = 20240105,
	uint32_t tradeDate = 20240105)
{
	WTSTickStruct ts;
	memset(&ts, 0, sizeof(ts));
	wt_strcpy(ts.exchg, "SHFE");
	wt_strcpy(ts.code, kCode);
	ts.price = px;
	ts.open = px;
	ts.high = px;
	ts.low = px;
	ts.volume = vol;
	ts.turn_over = px * vol;
	ts.open_interest = vol * 10;
	ts.diff_interest = vol;
	ts.trading_date = tradeDate;
	ts.action_date = actDate;
	ts.action_time = actMs;
	return WTSTickData::create(ts);
}

}	// namespace

class R56DataFactory : public ::testing::Test
{
protected:
	void SetUp() override
	{
		m_sInfo = WTSSessionInfo::create("R56ONE", "单小节", 0);
		m_sInfo->addTradingSection(900, 1000);

		m_sInfoTwo = WTSSessionInfo::create("R56TWO", "双小节", 0);
		m_sInfoTwo->addTradingSection(900, 1000);
		m_sInfoTwo->addTradingSection(1030, 1130);
	}

	void TearDown() override
	{
		for (WTSKlineData* k : m_klines)
			k->release();
		m_klines.clear();
		for (WTSTickData* t : m_ticks)
			t->release();
		m_ticks.clear();
		for (WTSKlineSlice* s : m_slices)
			s->release();
		m_slices.clear();
		m_sInfo->release();
		m_sInfoTwo->release();
	}

	WTSKlineData* kline(WTSKlinePeriod period, uint32_t times)
	{
		WTSKlineData* k = WTSKlineData::create(kCode, 0);
		k->setPeriod(period, times);
		m_klines.push_back(k);
		return k;
	}

	WTSTickData* tick(double px, double vol, uint32_t actMs, uint32_t actDate = 20240105,
		uint32_t tradeDate = 20240105)
	{
		WTSTickData* t = r56_tick(px, vol, actMs, actDate, tradeDate);
		m_ticks.push_back(t);
		return t;
	}

	WTSKlineSlice* slice(std::vector<WTSBarStruct>& bars)
	{
		WTSKlineSlice* s = WTSKlineSlice::create(kCode, KP_Minute1, 1, bars.data(), (uint32_t)bars.size());
		m_slices.push_back(s);
		return s;
	}

	WTSSessionInfo*	m_sInfo = NULL;
	WTSSessionInfo*	m_sInfoTwo = NULL;
	std::vector<WTSKlineData*>	m_klines;
	std::vector<WTSTickData*>	m_ticks;
	std::vector<WTSKlineSlice*>	m_slices;
	WTSDataFactory	m_factory;
};

// WTSDataFactory.cpp:23-78,708-753
TEST_F(R56DataFactory, TickDispatcherPeriods)
{
	// KP_Tick：分发到 updateSecData，首次返回新建 bar（调用方持有）
	WTSKlineData* sec = kline(KP_Tick, 30);
	WTSBarStruct* b1 = m_factory.updateKlineData(sec, tick(3500.0, 1.0, 90012500), m_sInfo);
	ASSERT_NE(nullptr, b1);
	EXPECT_EQ(0u, sec->size());	// updateSecData 只回传，不 append
	// 由调用方接管后再来一根同 30 秒桶的 tick，走原地累加分支
	sec->appendBar(*b1);
	delete b1;
	EXPECT_EQ(1u, sec->size());
	WTSBarStruct* merged = m_factory.updateKlineData(sec, tick(3510.0, 2.0, 90020000), m_sInfo);
	EXPECT_EQ(nullptr, merged);
	ASSERT_EQ(1u, sec->size());
	EXPECT_DOUBLE_EQ(3510.0, sec->at(-1)->close);
	EXPECT_DOUBLE_EQ(3510.0, sec->at(-1)->high);
	EXPECT_DOUBLE_EQ(3500.0, sec->at(-1)->low);
	EXPECT_DOUBLE_EQ(3.0, sec->at(-1)->vol);

	// KP_Minute5：分发到 tick 版 updateMin5Data，内部 append
	WTSKlineData* m5 = kline(KP_Minute5, 5);
	WTSBarStruct* b2 = m_factory.updateKlineData(m5, tick(3510.0, 2.0, 90300000), m_sInfo);
	ASSERT_NE(nullptr, b2);
	ASSERT_EQ(1u, m5->size());
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 925), b2->time);

	// 未知周期（小时/半小时在 tick 入口没有对应分支）
	WTSKlineData* hour = kline(KP_Hour, 1);
	EXPECT_EQ(nullptr, m_factory.updateKlineData(hour, tick(3520.0, 1.0, 90400000), m_sInfo));
}

// WTSDataFactory.cpp:191-304
TEST_F(R56DataFactory, TickMin1AlignsToSections)
{
	WTSKlineData* k1 = kline(KP_Minute1, 3);
	WTSBarStruct* b1 = m_factory.updateKlineData(k1, tick(3500.0, 1.0, 91000000), m_sInfo, true);
	ASSERT_NE(nullptr, b1);
	// 09:10 -> (10/3)*3+3 = 12 -> 09:12
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 912), b1->time);
	WTSBarStruct* merged = m_factory.updateKlineData(k1, tick(3505.0, 2.0, 91100000), m_sInfo, true);
	EXPECT_EQ(nullptr, merged);
	ASSERT_EQ(1u, k1->size());
	EXPECT_DOUBLE_EQ(3505.0, k1->at(-1)->high);
	EXPECT_DOUBLE_EQ(3.0, k1->at(-1)->vol);

	// 双小节：10:31 属于第二小节，按小节起点重新对齐 -> 10:33
	WTSKlineData* k2 = kline(KP_Minute1, 3);
	WTSBarStruct* b2 = m_factory.updateKlineData(k2, tick(3530.0, 1.0, 103100000), m_sInfoTwo, true);
	ASSERT_NE(nullptr, b2);
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 1033), b2->time);
}

// WTSDataFactory.cpp:422-509
TEST_F(R56DataFactory, TickMin5LastOfSectionAndAlign)
{
	// 10:00 是单小节最后一分钟：先回退一分钟再分桶，对齐后仍落在 10:00
	WTSKlineData* k1 = kline(KP_Minute5, 20);
	WTSBarStruct* b1 = m_factory.updateKlineData(k1, tick(3500.0, 1.0, 100000000), m_sInfo, true);
	ASSERT_NE(nullptr, b1);
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 1000), b1->time);

	// 09:06 处于第一小节：uBarMin 超过小节末分钟 -> 钳到 10:00
	WTSKlineData* k2 = kline(KP_Minute5, 20);
	WTSBarStruct* b2 = m_factory.updateKlineData(k2, tick(3510.0, 1.0, 90600000), m_sInfo, true);
	ASSERT_NE(nullptr, b2);
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 1000), b2->time);

	// 10:31 处于第二小节：按第二小节起点对齐，超过小节末分钟 -> 钳到 11:30
	WTSKlineData* k3 = kline(KP_Minute5, 20);
	WTSBarStruct* b3 = m_factory.updateKlineData(k3, tick(3520.0, 1.0, 103100000), m_sInfoTwo, true);
	ASSERT_NE(nullptr, b3);
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 1130), b3->time);
}

// WTSDataFactory.cpp:93-189,306-420
TEST_F(R56DataFactory, BarDrivenMinuteAlignClampsToSection)
{
	WTSKlineData* m1 = kline(KP_Minute1, 61);
	WTSBarStruct raw1 = r56_minbar(20240105, 901, 3500.0, 1.0);
	WTSBarStruct* b1 = m_factory.updateKlineData(m1, &raw1, m_sInfo, true);
	ASSERT_NE(nullptr, b1);
	// steplen=61 会越过 10:00，钳到小节末分钟
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 1000), b1->time);

	WTSKlineData* m5 = kline(KP_Minute5, 20);
	WTSBarStruct raw2 = r56_minbar(20240105, 906, 3510.0, 2.0);
	WTSBarStruct* b2 = m_factory.updateKlineData(m5, &raw2, m_sInfoTwo, true);
	ASSERT_NE(nullptr, b2);
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 1000), b2->time);

	WTSBarStruct raw3 = r56_minbar(20240105, 1031, 3520.0, 3.0);
	WTSBarStruct* b3 = m_factory.updateKlineData(m5, &raw3, m_sInfoTwo, true);
	ASSERT_NE(nullptr, b3);
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 1130), b3->time);
	EXPECT_EQ(2u, m5->size());
}

// WTSDataFactory.cpp:511-669
TEST_F(R56DataFactory, HourAndHalfHandleZeroAndMidnight)
{
	WTSKlineData* hour = kline(KP_Hour, 1);
	WTSBarStruct h0 = r56_bar(20240105, 0, 3500.0, 1.0);
	WTSBarStruct* hb0 = m_factory.updateKlineData(hour, &h0, m_sInfo);
	ASSERT_NE(nullptr, hb0);
	// time=0 -> minBarToDate 返回 19900000，回退到 bar 自带交易日
	EXPECT_EQ(20240105u, hb0->date);
	EXPECT_EQ(0u, TimeUtils::minBarToTime(hb0->time));

	WTSBarStruct h1 = r56_minbar(20240105, 2350, 3510.0, 2.0);
	WTSBarStruct* hb1 = m_factory.updateKlineData(hour, &h1, m_sInfo);
	ASSERT_NE(nullptr, hb1);
	// 23:50 向上取整到 24 点 -> 落到下一自然日 00:00
	EXPECT_EQ(TimeUtils::timeToMinBar(20240106, 0), hb1->time);
	ASSERT_EQ(2u, hour->size());

	WTSKlineData* half = kline(KP_Half, 1);
	WTSBarStruct f0 = r56_bar(20240105, 0, 3500.0, 1.0);
	WTSBarStruct* fb0 = m_factory.updateKlineData(half, &f0, m_sInfo);
	ASSERT_NE(nullptr, fb0);
	EXPECT_EQ(20240105u, fb0->date);
	WTSBarStruct f1 = r56_minbar(20240105, 2345, 3520.0, 3.0);
	WTSBarStruct* fb1 = m_factory.updateKlineData(half, &f1, m_sInfo);
	ASSERT_NE(nullptr, fb1);
	EXPECT_EQ(TimeUtils::timeToMinBar(20240106, 0), fb1->time);
	ASSERT_EQ(2u, half->size());

	// 10:15 -> 10:30 的半小时对齐
	WTSBarStruct f2 = r56_minbar(20240105, 1015, 3530.0, 4.0);
	WTSBarStruct* fb2 = m_factory.updateKlineData(half, &f2, m_sInfo);
	ASSERT_NE(nullptr, fb2);
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 1030), fb2->time);
}

// WTSDataFactory.cpp:805-929,931-1051
TEST_F(R56DataFactory, ExtractAlignsAndDropsOpenBar)
{
	std::vector<WTSBarStruct> bars;
	bars.push_back(r56_minbar(20240105, 910, 3500.0, 1.0));
	bars.push_back(r56_minbar(20240105, 959, 3510.0, 2.0));
	bars.push_back(r56_minbar(20240105, 1031, 3520.0, 3.0));
	WTSKlineSlice* s = slice(bars);

	// sInfo 为空时重采样直接失败（extractMin1/extractMin5 的入口守卫）
	EXPECT_EQ(nullptr, m_factory.extractKlineData(s, KP_Minute1, 5, (WTSSessionInfo*)NULL, true, true));
	EXPECT_EQ(nullptr, m_factory.extractKlineData(s, KP_Minute5, 2, (WTSSessionInfo*)NULL, true, true));

	// 一分钟线按 5 分钟小节对齐重采样
	WTSKlineData* m1 = m_factory.extractKlineData(s, KP_Minute1, 5, m_sInfoTwo, true, true);
	ASSERT_NE(nullptr, m1);
	ASSERT_EQ(3u, m1->size());
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 910), m1->at(0)->time);
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 1000), m1->at(1)->time);
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 1035), m1->at(2)->time);
	m1->release();

	std::vector<WTSBarStruct> bars2;
	bars2.push_back(r56_minbar(20240105, 958, 3500.0, 1.0));
	bars2.push_back(r56_minbar(20240105, 959, 3510.0, 2.0));
	bars2.push_back(r56_minbar(20240105, 1031, 3520.0, 3.0));
	WTSKlineSlice* s2 = slice(bars2);

	// 五分钟线：前两根对齐到 10:00，最后一根 10:31 对齐到 10:40；
	// bIncludeOpen=false 时末根未闭合 bar 被剔除
	WTSKlineData* m5 = m_factory.extractKlineData(s2, KP_Minute5, 2, m_sInfoTwo, false, true);
	ASSERT_NE(nullptr, m5);
	ASSERT_EQ(1u, m5->size());
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 1000), m5->at(0)->time);
	m5->release();
}

// WTSDataFactory.cpp:772-803,1053-1243
TEST_F(R56DataFactory, ExtractHourAndHalfCrossDay)
{
	std::vector<WTSBarStruct> bars;
	bars.push_back(r56_bar(20240105, 0, 3500.0, 1.0));
	bars.push_back(r56_minbar(20240105, 2350, 3510.0, 2.0));
	WTSKlineSlice* s = slice(bars);

	// KP_Minute5 + PERIOD_TIMES_HOUR 这一特殊组合分发到 extractHourData
	WTSKlineData* hour = m_factory.extractKlineData(s, KP_Minute5, PERIOD_TIMES_HOUR, m_sInfo, true);
	ASSERT_NE(nullptr, hour);
	ASSERT_EQ(2u, hour->size());
	EXPECT_EQ(0u, TimeUtils::minBarToTime(hour->at(0)->time));
	EXPECT_EQ(20240106u, TimeUtils::minBarToDate(hour->at(1)->time));
	hour->release();

	std::vector<WTSBarStruct> bars2;
	bars2.push_back(r56_bar(20240105, 0, 3500.0, 1.0));
	bars2.push_back(r56_minbar(20240105, 2345, 3510.0, 2.0));
	WTSKlineSlice* s2 = slice(bars2);

	WTSKlineData* half = m_factory.extractKlineData(s2, KP_Minute5, PERIOD_TIMES_HALF, m_sInfo, true);
	ASSERT_NE(nullptr, half);
	ASSERT_EQ(2u, half->size());
	EXPECT_EQ(0u, TimeUtils::minBarToTime(half->at(0)->time));
	EXPECT_EQ(20240106u, TimeUtils::minBarToDate(half->at(1)->time));
	half->release();

	// 同一批基础数据，bIncludeOpen=false：跨日末根被剔除
	WTSKlineData* halfClosed = m_factory.extractKlineData(s2, KP_Minute5, PERIOD_TIMES_HALF, m_sInfo, false);
	ASSERT_NE(nullptr, halfClosed);
	ASSERT_EQ(1u, halfClosed->size());
	EXPECT_EQ(0u, TimeUtils::minBarToTime(halfClosed->at(0)->time));
	halfClosed->release();
}
