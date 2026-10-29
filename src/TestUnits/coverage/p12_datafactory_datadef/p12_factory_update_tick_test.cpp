// p12 — WTSDataFactory: tick 驱动的 K 线装配路径（updateKlineData / update*Data）
// 只覆盖确定性、内存内对象；不落盘、不启动任何服务或线程。
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "WTSTools/WTSDataFactory.h"
#include "Share/TimeUtils.hpp"

using namespace wtp;

namespace
{
// WTSDataFactory 的 update*/extract* 均为 protected，这里在派生类里公开转发，
// 不修改任何生产代码。
class FactoryProbe : public WTSDataFactory
{
public:
	using WTSDataFactory::updateMin1Data;
	using WTSDataFactory::updateMin5Data;
	using WTSDataFactory::updateDayData;
	using WTSDataFactory::updateSecData;
	using WTSDataFactory::updateHourData;
	using WTSDataFactory::updateHalfData;
};

const char* kCode = "rb2401";

WTSTickData* makeTick(double price, double vol, double turnover, uint32_t actTime,
	uint32_t actDate = 20240105, uint32_t tradeDate = 20240105, double oi = 0, double diff = 0)
{
	WTSTickStruct ts;
	memset(&ts, 0, sizeof(ts));
	wt_strcpy(ts.exchg, "SHFE");
	wt_strcpy(ts.code, kCode);
	ts.price = price;
	ts.open = price;
	ts.high = price;
	ts.low = price;
	ts.volume = vol;
	ts.turn_over = turnover;
	ts.open_interest = oi;
	ts.diff_interest = diff;
	ts.trading_date = tradeDate;
	ts.action_date = actDate;
	ts.action_time = actTime;
	return WTSTickData::create(ts);
}

class P12TickFactory : public ::testing::Test
{
protected:
	void SetUp() override
	{
		m_sInfo = WTSSessionInfo::create("NONE", "无偏移时段", 0);
		m_sInfo->addTradingSection(900, 1000);
	}

	void TearDown() override
	{
		for (auto* t : m_ticks)
			t->release();
		m_ticks.clear();

		if (m_kline != NULL)
		{
			m_kline->release();
			m_kline = NULL;
		}
		if (m_sInfo != NULL)
		{
			m_sInfo->release();
			m_sInfo = NULL;
		}
	}

	WTSTickData* keep(WTSTickData* t)
	{
		m_ticks.push_back(t);
		return t;
	}

	WTSKlineData* newKline(WTSKlinePeriod period, uint32_t times)
	{
		m_kline = WTSKlineData::create(kCode, 0);
		m_kline->setPeriod(period, times);
		return m_kline;
	}

	WTSSessionInfo*	m_sInfo = NULL;
	WTSKlineData*		m_kline = NULL;
	std::vector<WTSTickData*>	m_ticks;
	FactoryProbe		m_factory;
};

TEST_F(P12TickFactory, ParamValidationOnTickPath)
{
	newKline(KP_Minute1, 1);
	WTSTickData* tick = keep(makeTick(3500.0, 10.0, 35000.0, 90100000));

	// tick == NULL
	EXPECT_EQ(NULL, m_factory.updateKlineData(m_kline, (WTSTickData*)NULL, m_sInfo));
	// klineData == NULL
	EXPECT_EQ(NULL, m_factory.updateKlineData((WTSKlineData*)NULL, tick, m_sInfo));
	// 合约代码不一致
	WTSKlineData* other = WTSKlineData::create("i2405", 0);
	other->setPeriod(KP_Minute1, 1);
	EXPECT_EQ(NULL, m_factory.updateKlineData(other, tick, m_sInfo));
	other->release();
	// sInfo == NULL
	EXPECT_EQ(NULL, m_factory.updateKlineData(m_kline, tick, NULL));
	// 非交易时间（时段表为空的 session，timeToMinutes 直接返回 INVALID）
	WTSSessionInfo* emptySess = WTSSessionInfo::create("EMPTY", "空时段", 0);
	EXPECT_EQ(NULL, m_factory.updateKlineData(m_kline, tick, emptySess));
	emptySess->release();

	// 未支持的周期（分钟线之外的 tick 路径没有分支）
	m_kline->setPeriod(KP_Hour, 1);
	EXPECT_EQ(NULL, m_factory.updateKlineData(m_kline, tick, m_sInfo));
	m_kline->setPeriod(KP_Half, 1);
	EXPECT_EQ(NULL, m_factory.updateKlineData(m_kline, tick, m_sInfo));
	m_kline->setPeriod(KP_DAY, 1);
	WTSBarStruct* day = m_factory.updateKlineData(m_kline, tick, m_sInfo);
	ASSERT_TRUE(day != NULL);
	delete day;
}

TEST_F(P12TickFactory, Minute1Times1CreatesThenUpdatesInPlace)
{
	newKline(KP_Minute1, 1);
	// 09:01 -> 分钟序号 1 -> bar 分钟 2 -> 09:02
	WTSTickData* t1 = keep(makeTick(3500.0, 10.0, 35000.0, 90100000, 20240105, 20240105, 1000.0, 5.0));
	WTSBarStruct* bar = m_factory.updateKlineData(m_kline, t1, m_sInfo);
	ASSERT_TRUE(bar != NULL);
	ASSERT_EQ(1u, m_kline->size());
	EXPECT_EQ(20240105u, bar->date);
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 902), bar->time);
	EXPECT_DOUBLE_EQ(3500.0, bar->open);
	EXPECT_DOUBLE_EQ(3500.0, bar->high);
	EXPECT_DOUBLE_EQ(3500.0, bar->low);
	EXPECT_DOUBLE_EQ(3500.0, bar->close);
	EXPECT_DOUBLE_EQ(10.0, bar->vol);
	EXPECT_DOUBLE_EQ(35000.0, bar->money);
	EXPECT_DOUBLE_EQ(1000.0, (double)bar->hold);
	EXPECT_DOUBLE_EQ(5.0, (double)bar->add);
	EXPECT_TRUE(m_kline->isClosed());

	// 同一分钟桶（09:01 -> 09:02）落在同一条 bar 上，原地累加并返回 NULL
	WTSTickData* t2 = keep(makeTick(3510.0, 7.0, 24570.0, 90130000, 20240105, 20240105, 1008.0, 8.0));
	EXPECT_EQ(NULL, m_factory.updateKlineData(m_kline, t2, m_sInfo));
	ASSERT_EQ(1u, m_kline->size());
	WTSBarStruct* same = m_kline->at(-1);
	EXPECT_DOUBLE_EQ(3510.0, same->close);
	EXPECT_DOUBLE_EQ(3510.0, same->high);
	EXPECT_DOUBLE_EQ(3500.0, same->low);
	EXPECT_DOUBLE_EQ(17.0, same->vol);
	EXPECT_DOUBLE_EQ(59570.0, same->money);
	EXPECT_DOUBLE_EQ(1008.0, (double)same->hold);
	EXPECT_DOUBLE_EQ(13.0, (double)same->add);

	// 下一分钟 -> 新 bar
	WTSTickData* t3 = keep(makeTick(3490.0, 3.0, 10470.0, 90230000));
	WTSBarStruct* nb = m_factory.updateKlineData(m_kline, t3, m_sInfo);
	ASSERT_TRUE(nb != NULL);
	EXPECT_EQ(2u, m_kline->size());
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 903), nb->time);
	EXPECT_DOUBLE_EQ(3490.0, nb->open);
	EXPECT_DOUBLE_EQ(3490.0, nb->low);
}

TEST_F(P12TickFactory, Minute1Times3Bucketing)
{
	newKline(KP_Minute1, 3);
	// 09:01 -> 分钟 1 -> (1/3)*3+3 = 3 -> 09:03
	WTSTickData* t1 = keep(makeTick(3500.0, 1.0, 3500.0, 90100000));
	WTSBarStruct* bar = m_factory.updateKlineData(m_kline, t1, m_sInfo);
	ASSERT_TRUE(bar != NULL);
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 903), bar->time);

	// 09:02 -> 分钟 2 -> (2/3)*3+3 = 3 -> 同一根
	WTSTickData* t2 = keep(makeTick(3505.0, 2.0, 7010.0, 90200000));
	EXPECT_EQ(NULL, m_factory.updateKlineData(m_kline, t2, m_sInfo));
	EXPECT_EQ(1u, m_kline->size());
	EXPECT_DOUBLE_EQ(3505.0, m_kline->at(-1)->high);
	EXPECT_DOUBLE_EQ(3.0, m_kline->at(-1)->vol);

	// 09:03 -> 分钟 3 -> 6 -> 09:06，新开一根
	WTSTickData* t3 = keep(makeTick(3499.0, 4.0, 13996.0, 90300000));
	WTSBarStruct* bar2 = m_factory.updateKlineData(m_kline, t3, m_sInfo);
	ASSERT_TRUE(bar2 != NULL);
	EXPECT_EQ(2u, m_kline->size());
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 906), bar2->time);
}

TEST_F(P12TickFactory, LastMinuteOfSectionGetsRolledBackOneMinute)
{
	newKline(KP_Minute1, 1);
	// 10:00 是小节最后一分钟，isLastOfSection 命中 -> 分钟序号回退 1 -> bar 落在 1000
	WTSTickData* t = keep(makeTick(3600.0, 9.0, 32400.0, 100000000));
	WTSBarStruct* bar = m_factory.updateKlineData(m_kline, t, m_sInfo);
	ASSERT_TRUE(bar != NULL);
	EXPECT_EQ(1u, m_kline->size());
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 1000), bar->time);
}

TEST_F(P12TickFactory, TradingDateRollCreatesNewBarEvenWithSmallerBarTime)
{
	newKline(KP_Minute1, 1);
	WTSTickData* t1 = keep(makeTick(3500.0, 1.0, 3500.0, 90100000, 20240105, 20240105));
	ASSERT_TRUE(m_factory.updateKlineData(m_kline, t1, m_sInfo) != NULL);

	// 交易日 +1、时间反而更小（夜盘/次日回退场景）
	WTSTickData* t2 = keep(makeTick(3520.0, 2.0, 7040.0, 90100000, 20240106, 20240106));
	WTSBarStruct* bar = m_factory.updateKlineData(m_kline, t2, m_sInfo);
	ASSERT_TRUE(bar != NULL);
	EXPECT_EQ(2u, m_kline->size());
	EXPECT_EQ(20240106u, bar->date);

	// 同一交易日内、bar 时间早于已有 bar -> 直接丢弃
	WTSTickData* t3 = keep(makeTick(3530.0, 3.0, 10590.0, 90010000, 20240106, 20240106));
	EXPECT_EQ(NULL, m_factory.updateKlineData(m_kline, t3, m_sInfo));
	EXPECT_EQ(2u, m_kline->size());
}

TEST_F(P12TickFactory, Minute5TickPath)
{
	newKline(KP_Minute5, 1);
	// steplen = 5 * 1 = 5; 09:03 -> 分钟 3 -> (3/5)*5+5 = 5 -> 09:05
	WTSTickData* t1 = keep(makeTick(3500.0, 1.0, 3500.0, 90300000));
	WTSBarStruct* bar = m_factory.updateMin5Data(m_sInfo, m_kline, t1, false);
	ASSERT_TRUE(bar != NULL);
	EXPECT_EQ(1u, m_kline->size());
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 905), bar->time);

	// 09:04 -> 分钟 4 -> 5 -> 同一根（时间相同）-> 原地累加返回 NULL
	WTSTickData* t2 = keep(makeTick(3508.0, 2.0, 7016.0, 90400000));
	WTSTickData* t3 = keep(makeTick(3495.0, 4.0, 13980.0, 90430000));
	EXPECT_EQ(NULL, m_factory.updateMin5Data(m_sInfo, m_kline, t2, false));
	EXPECT_EQ(NULL, m_factory.updateMin5Data(m_sInfo, m_kline, t3, false));
	ASSERT_EQ(1u, m_kline->size());
	WTSBarStruct* same = m_kline->at(-1);
	EXPECT_DOUBLE_EQ(3508.0, same->high);
	EXPECT_DOUBLE_EQ(3495.0, same->low);
	EXPECT_DOUBLE_EQ(3495.0, same->close);
	EXPECT_DOUBLE_EQ(7.0, same->vol);

	// 09:06 -> 分钟 6 -> 10 -> 09:10 新 bar
	WTSTickData* t4 = keep(makeTick(3512.0, 5.0, 17560.0, 90600000));
	WTSBarStruct* bar2 = m_factory.updateMin5Data(m_sInfo, m_kline, t4, true);
	ASSERT_TRUE(bar2 != NULL);
	EXPECT_EQ(2u, m_kline->size());
}

TEST_F(P12TickFactory, DayPathReturnsDetachedBar)
{
	newKline(KP_DAY, 1);
	WTSTickData* t1 = keep(makeTick(3500.0, 10.0, 35000.0, 90100000, 20240105, 20240105, 800.0, 12.0));
	// 新交易日：工厂把新建的 bar 直接返回给调用方（不 append），所有权移交调用者
	WTSBarStruct* day = m_factory.updateDayData(m_sInfo, m_kline, t1);
	ASSERT_TRUE(day != NULL);
	EXPECT_EQ(0u, m_kline->size());
	EXPECT_EQ(20240105u, day->date);
	EXPECT_EQ(0u, day->time);
	EXPECT_DOUBLE_EQ(3500.0, day->open);
	EXPECT_DOUBLE_EQ(3500.0, day->close);
	EXPECT_DOUBLE_EQ(10.0, day->vol);
	EXPECT_DOUBLE_EQ(0.0, day->settle);
	delete day;

	// 由调用方追加以后，同一交易日内原地更新并返回 NULL
	WTSBarStruct seed;
	seed.date = 20240105;
	seed.time = 0;
	seed.open = seed.high = seed.low = seed.close = 3500.0;
	seed.vol = 10.0;
	seed.money = 35000.0;
	seed.add = 12.0;
	m_kline->appendBar(seed);

	WTSTickData* t2 = keep(makeTick(3520.0, 5.0, 17600.0, 90200000, 20240105, 20240105, 810.0, 2.0));
	EXPECT_EQ(NULL, m_factory.updateDayData(m_sInfo, m_kline, t2));
	WTSBarStruct* bar = m_kline->at(-1);
	EXPECT_DOUBLE_EQ(3520.0, bar->high);
	EXPECT_DOUBLE_EQ(3500.0, bar->low);
	EXPECT_DOUBLE_EQ(15.0, bar->vol);
	EXPECT_DOUBLE_EQ(52600.0, bar->money);
	EXPECT_DOUBLE_EQ(810.0, (double)bar->hold);
	EXPECT_DOUBLE_EQ(14.0, (double)bar->add);
}

TEST_F(P12TickFactory, SecPathWithTimesAndUnixTime)
{
	newKline(KP_Tick, 30);
	// 09:00:12.500 -> 秒序号 12 -> 30 -> secondsToTime(30) = 090030
	WTSTickData* t1 = keep(makeTick(3500.0, 1.0, 3500.0, 90012500));
	WTSBarStruct* bar = m_factory.updateSecData(m_sInfo, m_kline, t1);
	ASSERT_TRUE(bar != NULL);
	// 首根是调用方所有的 detached bar；装入序列后由序列持有，供同桶更新路径使用。
	m_kline->appendBar(*bar);
	delete bar;

	// 同一 30 秒桶 -> 原地更新，返回 NULL
	WTSTickData* t2 = keep(makeTick(3510.0, 2.0, 7020.0, 90020000));
	WTSTickData* t3 = keep(makeTick(3490.0, 3.0, 10470.0, 90025000));
	EXPECT_EQ(NULL, m_factory.updateSecData(m_sInfo, m_kline, t2));
	EXPECT_EQ(NULL, m_factory.updateSecData(m_sInfo, m_kline, t3));
	ASSERT_EQ(1u, m_kline->size());
	EXPECT_DOUBLE_EQ(3510.0, m_kline->at(-1)->high);
	EXPECT_DOUBLE_EQ(3490.0, m_kline->at(-1)->low);
	EXPECT_DOUBLE_EQ(3490.0, m_kline->at(-1)->close);
	EXPECT_DOUBLE_EQ(6.0, m_kline->at(-1)->vol);

	// 下一个 30 秒桶 -> 新 bar（工厂仍然返回新建对象，调用方需要自行 delete）
	WTSTickData* t4 = keep(makeTick(3520.0, 1.0, 3520.0, 90031000));
	WTSBarStruct* bar2 = m_factory.updateSecData(m_sInfo, m_kline, t4);
	ASSERT_TRUE(bar2 != NULL);
	delete bar2;
	EXPECT_EQ(1u, m_kline->size());	// updateSecData 不 append，只回传新 bar

	// unix 时间戳模式：时间戳不再是 hhmmss 形态
	m_kline->setUnixTime(true);
	WTSTickData* t5 = keep(makeTick(3530.0, 1.0, 3530.0, 90035000, 20240105, 20240105));
	WTSBarStruct* barUnix = m_factory.updateSecData(m_sInfo, m_kline, t5);
	if (barUnix != NULL)
	{
		EXPECT_GT(barUnix->time, 1000000000u);
		delete barUnix;
	}
}

TEST_F(P12TickFactory, RefCountPairedWithPoolAllocation)
{
	WTSTickData* tick = makeTick(3500.0, 1.0, 3500.0, 90100000);
	ASSERT_TRUE(tick != NULL);
	EXPECT_EQ(1u, tick->retainCount());
	EXPECT_EQ(2u, tick->retain());
	tick->release();
	EXPECT_EQ(1u, tick->retainCount());
	tick->release();

	newKline(KP_Minute1, 1);
	WTSObject* obj = m_kline;
	EXPECT_EQ(1u, obj->retainCount());
	m_kline->retain();
	m_kline->release();
	m_kline->release();
	m_kline = NULL;
}

// 崩溃类输入统一收进本用例（默认不执行）：
// 1) updateMin1Data(tick) 在 timeToMinutes 返回 INVALID_UINT32（时段外 tick）且 klineData 为空时，
//    会走 WTSDataFactory.cpp:206 的 klineData->at(size()-1) == NULL 分支并直接解引用；
//    公开入口 updateKlineData 因为先做了 isInTradingTime 校验而不可达，属于 protected 路径的潜在空指针。
// 2) extractKlineData(WTSTickSlice*, 0, ...) 秒数为 0 会整数除零。
// 见 docs/迭代计划/迭代01/bugs/p12_datafactory_datadef.md（BUG-P12-001 / BUG-P12-002）。
TEST_F(P12TickFactory, DISABLED_CrashClassNotExecuted)
{
	WTSSessionInfo* emptySession = WTSSessionInfo::create("EMPTY", "空时段", 0);
	newKline(KP_Minute1, 1);
	WTSTickData* t = keep(makeTick(3500.0, 1.0, 3500.0, 120000000));
	// 若启用将因空指针解引用崩溃，这里只做符号占位
	(void)m_factory.updateMin1Data(emptySession, m_kline, t, false);
	emptySession->release();
}

}	// namespace
