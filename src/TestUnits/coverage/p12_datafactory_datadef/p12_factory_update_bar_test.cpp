// p12 — WTSDataFactory: 基础周期 K 线（bar）驱动的重采样路径
// updateMin1Data / updateMin5Data / updateHourData / updateHalfData（bar 入参版）
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "WTSTools/WTSDataFactory.h"
#include "Share/TimeUtils.hpp"

using namespace wtp;

namespace
{
const char* kBarCode = "SHFE.rb2405";

WTSBarStruct mkBar(uint32_t date, uint64_t time, double o, double h, double l, double c,
	double vol, double money, double hold, double add, double settle = 0)
{
	WTSBarStruct bar;
	bar.date = date;
	bar.time = time;
	bar.open = o;
	bar.high = h;
	bar.low = l;
	bar.close = c;
	bar.vol = vol;
	bar.money = money;
	bar.hold = hold;
	bar.add = add;
	bar.settle = settle;
	return bar;
}

class P12BarFactory : public ::testing::Test
{
protected:
	void SetUp() override
	{
		m_sInfo = WTSSessionInfo::create("NONE", "无偏移时段", 0);
		m_sInfo->addTradingSection(900, 1000);

		m_sInfoTwo = WTSSessionInfo::create("NONE2", "两小节", 0);
		m_sInfoTwo->addTradingSection(900, 1000);
		m_sInfoTwo->addTradingSection(1030, 1130);

		m_sInfoShort = WTSSessionInfo::create("SHORT", "短小节", 0);
		m_sInfoShort->addTradingSection(900, 910);
		m_sInfoShort->addTradingSection(930, 940);

		m_sInfoNight = WTSSessionInfo::create("NIGHT", "夜盘", 0);
		m_sInfoNight->addTradingSection(2100, 2359);
	}

	void TearDown() override
	{
		for (auto* k : m_extraKlines)
			k->release();
		m_extraKlines.clear();

		if (m_kline != NULL)
		{
			m_kline->release();
			m_kline = NULL;
		}
		m_sInfo->release();
		m_sInfoTwo->release();
		m_sInfoShort->release();
		m_sInfoNight->release();
	}

	WTSKlineData* newKline(WTSKlinePeriod period, uint32_t times)
	{
		m_kline = WTSKlineData::create(kBarCode, 0);
		m_kline->setPeriod(period, times);
		return m_kline;
	}

	WTSKlineData* extraKline(WTSKlinePeriod period, uint32_t times)
	{
		WTSKlineData* k = WTSKlineData::create(kBarCode, 0);
		m_extraKlines.push_back(k);
		k->setPeriod(period, times);
		return k;
	}

	WTSSessionInfo*	m_sInfo = NULL;
	WTSSessionInfo*	m_sInfoTwo = NULL;
	WTSSessionInfo*	m_sInfoShort = NULL;
	WTSSessionInfo*	m_sInfoNight = NULL;
	WTSKlineData*		m_kline = NULL;
	std::vector<WTSKlineData*>	m_extraKlines;
	WTSDataFactory		m_factory;
};

TEST_F(P12BarFactory, BarPathParamValidation)
{
	newKline(KP_Minute1, 3);
	WTSBarStruct bar = mkBar(20240105, TimeUtils::timeToMinBar(20240105, 901), 10, 12, 9, 11, 5, 50, 1, 1);

	EXPECT_EQ(NULL, m_factory.updateKlineData((WTSKlineData*)NULL, &bar, m_sInfo));
	EXPECT_EQ(NULL, m_factory.updateKlineData(m_kline, (WTSBarStruct*)NULL, m_sInfo));
	EXPECT_EQ(NULL, m_factory.updateKlineData(m_kline, &bar, NULL));

	// 未支持的周期走 default 分支
	WTSKlineData* dayK = extraKline(KP_DAY, 1);
	EXPECT_EQ(NULL, m_factory.updateKlineData(dayK, &bar, m_sInfo));
	dayK->setPeriod(KP_Tick, 10);
	EXPECT_EQ(NULL, m_factory.updateKlineData(dayK, &bar, m_sInfo));
}

TEST_F(P12BarFactory, Minute1Times1JustAppends)
{
	newKline(KP_Minute1, 1);
	WTSBarStruct b1 = mkBar(20240105, TimeUtils::timeToMinBar(20240105, 901), 10, 12, 9, 11, 5, 50, 100, 2, 11);
	WTSBarStruct* r1 = m_factory.updateKlineData(m_kline, &b1, m_sInfo);
	ASSERT_TRUE(r1 != NULL);
	EXPECT_EQ(1u, m_kline->size());
	EXPECT_EQ(901u, TimeUtils::minBarToTime(r1->time));
	EXPECT_DOUBLE_EQ(11.0, r1->close);
	EXPECT_DOUBLE_EQ(11.0, r1->settle);
	EXPECT_TRUE(m_kline->isClosed());

	// 相同 date+time 再更新一次 -> appendBar 覆盖同一槽位，条数不变
	WTSBarStruct b2 = mkBar(20240105, TimeUtils::timeToMinBar(20240105, 901), 10, 15, 9, 14, 8, 80, 105, 5, 14);
	WTSBarStruct* r2 = m_factory.updateKlineData(m_kline, &b2, m_sInfo);
	ASSERT_TRUE(r2 != NULL);
	EXPECT_EQ(1u, m_kline->size());
	EXPECT_DOUBLE_EQ(15.0, r2->high);
	EXPECT_DOUBLE_EQ(14.0, r2->close);
	EXPECT_DOUBLE_EQ(8.0, r2->vol);
}

TEST_F(P12BarFactory, Minute1Times3BucketAndMerge)
{
	newKline(KP_Minute1, 3);
	// 0901 -> (1-1)/3*3+3 = 3 -> 0903，目标时间大于原始时间 -> 未闭合
	WTSBarStruct b1 = mkBar(20240105, TimeUtils::timeToMinBar(20240105, 901), 10, 12, 9, 11, 5, 50, 100, 2);
	WTSBarStruct* r1 = m_factory.updateKlineData(m_kline, &b1, m_sInfo);
	ASSERT_TRUE(r1 != NULL);
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 903), r1->time);
	EXPECT_EQ(20240105u, r1->date);
	EXPECT_FALSE(m_kline->isClosed());

	// 0902 -> (2-1)/3*3+3 = 3 -> 同一根，合并后返回 NULL；903 > 902 仍是未闭合
	WTSBarStruct b2 = mkBar(20240105, TimeUtils::timeToMinBar(20240105, 902), 11, 18, 8, 16, 7, 70, 120, 20);
	EXPECT_EQ(NULL, m_factory.updateKlineData(m_kline, &b2, m_sInfo));
	ASSERT_EQ(1u, m_kline->size());
	WTSBarStruct* same = m_kline->at(-1);
	EXPECT_DOUBLE_EQ(18.0, same->high);
	EXPECT_DOUBLE_EQ(8.0, same->low);
	EXPECT_DOUBLE_EQ(16.0, same->close);
	EXPECT_DOUBLE_EQ(12.0, same->vol);
	EXPECT_DOUBLE_EQ(120.0, (double)same->hold);
	EXPECT_DOUBLE_EQ(22.0, (double)same->add);
	EXPECT_FALSE(m_kline->isClosed());

	// 0904 -> (4-1)/3*3+3 = 6 -> 0906 新根
	WTSBarStruct b3 = mkBar(20240105, TimeUtils::timeToMinBar(20240105, 904), 16, 20, 15, 19, 3, 30, 130, 5);
	WTSBarStruct* r3 = m_factory.updateKlineData(m_kline, &b3, m_sInfo);
	ASSERT_TRUE(r3 != NULL);
	EXPECT_EQ(2u, m_kline->size());
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 906), r3->time);
}

TEST_F(P12BarFactory, Minute1RawTimeBarUsesTradingDateSentinel)
{
	newKline(KP_Minute1, 3);
	// time 字段只写 HHMM -> minBarToDate 返回 19900000，需要回落到 bar 自带交易日
	WTSBarStruct bar = mkBar(20240105, 901, 10, 12, 9, 11, 5, 50, 100, 2);
	WTSBarStruct* r = m_factory.updateKlineData(m_kline, &bar, m_sInfo);
	ASSERT_TRUE(r != NULL);
	EXPECT_EQ(20240105u, r->date);
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 903), r->time);
}

TEST_F(P12BarFactory, Minute1SessionCloseStaysOnTradingDate)
{
	newKline(KP_Minute1, 3);
	// 合法的 session 收盘分钟是 timeToMinutes 的闭区间边界；3 分钟桶仍落在收盘点。
	WTSBarStruct bar = mkBar(20240105, TimeUtils::timeToMinBar(20240105, 1000), 10, 12, 9, 11, 5, 50, 100, 2);
	WTSBarStruct* r = m_factory.updateKlineData(m_kline, &bar, m_sInfo);
	ASSERT_TRUE(r != NULL);
	EXPECT_EQ(20240105u, r->date);
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 1000), r->time);
}

TEST_F(P12BarFactory, Minute1AlignedToSection)
{
	newKline(KP_Minute1, 3);
	// 1035 -> 全局分钟 65 -> 第二小节 -> 60 + (4/3)*3+3 = 66 -> 1036
	WTSBarStruct bar = mkBar(20240105, TimeUtils::timeToMinBar(20240105, 1035), 10, 12, 9, 11, 5, 50, 100, 2);
	WTSBarStruct* r = m_factory.updateKlineData(m_kline, &bar, m_sInfoTwo, true);
	ASSERT_TRUE(r != NULL);
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 1036), r->time);

	// secIdx == 0 分支：0905 -> 分钟 4 -> (4/3)*3+3 = 6 -> 0906
	WTSKlineData* k2 = extraKline(KP_Minute1, 3);
	WTSBarStruct b2 = mkBar(20240105, TimeUtils::timeToMinBar(20240105, 905), 10, 12, 9, 11, 5, 50, 100, 2);
	WTSBarStruct* r2 = m_factory.updateKlineData(k2, &b2, m_sInfoTwo, true);
	ASSERT_TRUE(r2 != NULL);
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 906), r2->time);

	// 小节上边界钳制：短时段 0900-0910 + 0930-0940（secMins=[10,20]），steplen=8 -> 16 钳到 10
	WTSKlineData* k3 = extraKline(KP_Minute1, 8);
	WTSBarStruct b3 = mkBar(20240105, TimeUtils::timeToMinBar(20240105, 909), 10, 12, 9, 11, 5, 50, 100, 2);
	WTSBarStruct* r3 = m_factory.updateKlineData(k3, &b3, m_sInfoShort, true);
	ASSERT_TRUE(r3 != NULL);
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 910), r3->time);
}

TEST_F(P12BarFactory, Minute5Times1And2)
{
	newKline(KP_Minute5, 1);
	WTSBarStruct b1 = mkBar(20240105, TimeUtils::timeToMinBar(20240105, 905), 10, 12, 9, 11, 5, 50, 100, 2);
	WTSBarStruct* r1 = m_factory.updateKlineData(m_kline, &b1, m_sInfo);
	ASSERT_TRUE(r1 != NULL);
	EXPECT_EQ(1u, m_kline->size());
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 905), r1->time);

	WTSKlineData* k2 = extraKline(KP_Minute5, 2);	// steplen = 10
	// 0905 -> 5-5=0 -> 10 -> 0910
	WTSBarStruct b2 = mkBar(20240105, TimeUtils::timeToMinBar(20240105, 905), 10, 12, 9, 11, 5, 50, 100, 2);
	WTSBarStruct* r2 = m_factory.updateKlineData(k2, &b2, m_sInfo);
	ASSERT_TRUE(r2 != NULL);
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 910), r2->time);
	EXPECT_EQ(20240105u, r2->date);

	// 09:10 是有效的五分钟基础 bar，归入 09:10 桶并与前一根合并。
	WTSBarStruct b3 = mkBar(20240105, TimeUtils::timeToMinBar(20240105, 910), 11, 20, 8, 19, 7, 70, 120, 30);
	EXPECT_EQ(NULL, m_factory.updateKlineData(k2, &b3, m_sInfo));
	ASSERT_EQ(1u, k2->size());
	EXPECT_DOUBLE_EQ(20.0, k2->at(-1)->high);
	EXPECT_DOUBLE_EQ(8.0, k2->at(-1)->low);
	EXPECT_DOUBLE_EQ(19.0, k2->at(-1)->close);
	EXPECT_DOUBLE_EQ(12.0, k2->at(-1)->vol);

	// 0915 -> 10 -> 20 -> 0920 新根
	WTSBarStruct b4 = mkBar(20240105, TimeUtils::timeToMinBar(20240105, 915), 19, 21, 18, 20, 3, 30, 130, 4);
	WTSBarStruct* r4 = m_factory.updateKlineData(k2, &b4, m_sInfo);
	ASSERT_TRUE(r4 != NULL);
	EXPECT_EQ(2u, k2->size());
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 920), r4->time);
}

TEST_F(P12BarFactory, Minute5AlignedAndSentinel)
{
	newKline(KP_Minute5, 2);
	// 小节内对齐：secMins=[60]，0905 -> uMinute=0 -> secIdx=0 -> (0/10)*10+10=10 -> 0910
	WTSBarStruct b1 = mkBar(20240105, TimeUtils::timeToMinBar(20240105, 905), 10, 12, 9, 11, 5, 50, 100, 2);
	WTSBarStruct* r1 = m_factory.updateKlineData(m_kline, &b1, m_sInfo, true);
	ASSERT_TRUE(r1 != NULL);
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 910), r1->time);

	WTSKlineData* k2 = extraKline(KP_Minute5, 2);
	// 日期哨兵：time 只带 HHMM；有效五分钟基础 bar 回落 curBar.date。
	WTSBarStruct b2 = mkBar(20240105, 905, 10, 13, 9, 12, 5, 50, 100, 2);
	WTSBarStruct* r2 = m_factory.updateKlineData(k2, &b2, m_sInfo);
	ASSERT_TRUE(r2 != NULL);
	EXPECT_EQ(20240105u, r2->date);
	// 0910 -> 5 -> 10 -> 0910，同一根合并
	WTSBarStruct b3 = mkBar(20240105, 910, 12, 30, 11, 28, 9, 90, 150, 60);
	EXPECT_EQ(NULL, m_factory.updateKlineData(k2, &b3, m_sInfo));
	ASSERT_EQ(1u, k2->size());
	EXPECT_DOUBLE_EQ(30.0, k2->at(-1)->high);
	EXPECT_DOUBLE_EQ(9.0, k2->at(-1)->low);
	EXPECT_DOUBLE_EQ(14.0, k2->at(-1)->vol);
}

TEST_F(P12BarFactory, HourResamplePaths)
{
	newKline(KP_Hour, 1);
	// 0930 -> 小时向上取整 1000
	WTSBarStruct b1 = mkBar(20240105, TimeUtils::timeToMinBar(20240105, 930), 10, 12, 9, 11, 5, 50, 100, 2);
	WTSBarStruct* r1 = m_factory.updateKlineData(m_kline, &b1, m_sInfo);
	ASSERT_TRUE(r1 != NULL);
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 1000), r1->time);
	EXPECT_EQ(20240105u, r1->date);
	EXPECT_FALSE(m_kline->isClosed());

	// 1000 -> 整点 -> 同一小时，合并并返回 NULL
	WTSBarStruct b2 = mkBar(20240105, TimeUtils::timeToMinBar(20240105, 1000), 11, 16, 10, 15, 6, 60, 110, 3);
	EXPECT_EQ(NULL, m_factory.updateKlineData(m_kline, &b2, m_sInfo));
	ASSERT_EQ(1u, m_kline->size());
	EXPECT_DOUBLE_EQ(16.0, m_kline->at(-1)->high);
	EXPECT_DOUBLE_EQ(9.0, m_kline->at(-1)->low);
	EXPECT_DOUBLE_EQ(15.0, m_kline->at(-1)->close);
	EXPECT_DOUBLE_EQ(11.0, m_kline->at(-1)->vol);
	EXPECT_DOUBLE_EQ(110.0, (double)m_kline->at(-1)->hold);
	// 合并后 lastBar->time == curBar.time -> 重新置为闭合
	EXPECT_TRUE(m_kline->isClosed());

	// 夜盘自然时间跨午夜；输出 date 仍是 bar 的交易日，time 走到次日 00:00。
	WTSKlineData* k2 = extraKline(KP_Hour, 1);
	WTSBarStruct b3 = mkBar(20240108, TimeUtils::timeToMinBar(20240107, 2330), 10, 12, 9, 11, 5, 50, 100, 2);
	WTSBarStruct* r2 = m_factory.updateKlineData(k2, &b3, m_sInfoNight);
	ASSERT_TRUE(r2 != NULL);
	EXPECT_EQ(20240108u, r2->date);
	EXPECT_EQ(TimeUtils::timeToMinBar(20240108, 0), r2->time);
}

TEST_F(P12BarFactory, HalfResamplePaths)
{
	newKline(KP_Half, 1);
	// 0910 -> 半小时向上取整 0930
	WTSBarStruct b1 = mkBar(20240105, TimeUtils::timeToMinBar(20240105, 910), 10, 12, 9, 11, 5, 50, 100, 2);
	WTSBarStruct* r1 = m_factory.updateKlineData(m_kline, &b1, m_sInfo);
	ASSERT_TRUE(r1 != NULL);
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 930), r1->time);

	// 0930 -> 整点归 0 分 -> 0930 同一根，合并
	WTSBarStruct b2 = mkBar(20240105, TimeUtils::timeToMinBar(20240105, 930), 11, 14, 10, 13, 6, 60, 120, 4);
	EXPECT_EQ(NULL, m_factory.updateKlineData(m_kline, &b2, m_sInfo));
	ASSERT_EQ(1u, m_kline->size());
	EXPECT_DOUBLE_EQ(14.0, m_kline->at(-1)->high);
	EXPECT_DOUBLE_EQ(11.0, m_kline->at(-1)->vol);
	EXPECT_TRUE(m_kline->isClosed());

	// 0945 -> 超过 30 分 -> 1000
	WTSBarStruct b3 = mkBar(20240105, TimeUtils::timeToMinBar(20240105, 945), 13, 15, 12, 14, 7, 70, 130, 5);
	WTSBarStruct* r3 = m_factory.updateKlineData(m_kline, &b3, m_sInfo);
	ASSERT_TRUE(r3 != NULL);
	EXPECT_EQ(2u, m_kline->size());
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 1000), r3->time);

	// 自然日 20240107 夜盘跨午夜；date 保留对应的交易日 20240108。
	WTSKlineData* k2 = extraKline(KP_Half, 1);
	WTSBarStruct b4 = mkBar(20240108, TimeUtils::timeToMinBar(20240107, 2345), 10, 12, 9, 11, 5, 50, 100, 2);
	WTSBarStruct* r4 = m_factory.updateKlineData(k2, &b4, m_sInfoNight);
	ASSERT_TRUE(r4 != NULL);
	EXPECT_EQ(20240108u, r4->date);
	EXPECT_EQ(TimeUtils::timeToMinBar(20240108, 0), r4->time);
}

TEST_F(P12BarFactory, ExtractDispatchParameterGuards)
{
	std::vector<WTSBarStruct> bars;
	bars.push_back(mkBar(20240105, TimeUtils::timeToMinBar(20240105, 901), 10, 12, 9, 11, 5, 50, 100, 2));
	WTSKlineSlice* slice = WTSKlineSlice::create(kBarCode, KP_Minute1, 1, bars.data(), (uint32_t)bars.size());
	WTSKlineSlice* emptySlice = WTSKlineSlice::create(kBarCode, KP_Minute1, 1, NULL, 0);

	// 空切片
	EXPECT_EQ(NULL, m_factory.extractKlineData(emptySlice, KP_Minute1, 2, m_sInfo));
	// times <= 1 或 period == KP_Tick：无需转换
	EXPECT_EQ(NULL, m_factory.extractKlineData(slice, KP_Minute1, 1, m_sInfo));
	EXPECT_EQ(NULL, m_factory.extractKlineData(slice, KP_Tick, 5, m_sInfo));
	// sInfo == NULL 的三条子路径
	EXPECT_EQ(NULL, m_factory.extractKlineData(slice, KP_Minute1, 2, NULL));
	EXPECT_EQ(NULL, m_factory.extractKlineData(slice, KP_Minute5, PERIOD_TIMES_HOUR, NULL));
	EXPECT_EQ(NULL, m_factory.extractKlineData(slice, KP_Minute5, PERIOD_TIMES_HALF, NULL));

	emptySlice->release();
	slice->release();
}

}	// namespace
