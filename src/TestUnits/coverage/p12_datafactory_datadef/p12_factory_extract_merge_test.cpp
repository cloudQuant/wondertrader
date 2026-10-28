// p12 — WTSDataFactory: extract* 重采样 / mergeKlineData / getPrevMinute
// 以及 WTSDataDef 切片装配（多 block 寻址、越界拒绝、对象池配对）的未被覆盖路径
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
const char* kCode = "DCE.i2405";

class FactoryProbe : public WTSDataFactory
{
public:
	using WTSDataFactory::extractMin1Data;
	using WTSDataFactory::extractMin5Data;
	using WTSDataFactory::extractHourData;
	using WTSDataFactory::extractHalfData;
	using WTSDataFactory::extractDayData;
	using WTSDataFactory::getPrevMinute;
};

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

// 生成 hhmm 形态的分钟 bar（date + time 复合编码）
WTSBarStruct mkMinBar(uint32_t date, uint32_t hhmm, double px, double vol)
{
	return mkBar(date, TimeUtils::timeToMinBar(date, hhmm), px, px + 1, px - 1, px + 0.5,
		vol, px * vol, vol * 10, vol, px);
}

WTSTickStruct mkTickStruct(double price, double vol, uint32_t actTimeMs,
	uint32_t actDate = 20240105, uint32_t tradeDate = 20240105)
{
	WTSTickStruct ts;
	memset(&ts, 0, sizeof(ts));
	wt_strcpy(ts.exchg, "DCE");
	wt_strcpy(ts.code, kCode);
	ts.price = price;
	ts.open = price;
	ts.high = price;
	ts.low = price;
	ts.volume = vol;
	ts.turn_over = price * vol;
	ts.trading_date = tradeDate;
	ts.action_date = actDate;
	ts.action_time = actTimeMs;
	return ts;
}

class P12Extract : public ::testing::Test
{
protected:
	void SetUp() override
	{
		m_sInfo = WTSSessionInfo::create("NONE", "无偏移时段", 0);
		m_sInfo->addTradingSection(900, 1000);
	}

	void TearDown() override
	{
		for (auto* k : m_klines)
			k->release();
		m_klines.clear();
		for (auto* s : m_slices)
			s->release();
		m_slices.clear();
		for (auto* t : m_tickSlices)
			t->release();
		m_tickSlices.clear();
		for (auto* v : m_arrays)
			v->release();
		m_arrays.clear();
		m_sInfo->release();
	}

	WTSKlineSlice* newSlice(std::vector<WTSBarStruct>& store)
	{
		WTSKlineSlice* s = WTSKlineSlice::create(kCode, KP_Minute1, 1, store.data(), (uint32_t)store.size());
		m_slices.push_back(s);
		return s;
	}

	WTSTickSlice* newTickSlice(std::vector<WTSTickStruct>& store)
	{
		WTSTickSlice* s = WTSTickSlice::create(kCode, store.data(), (uint32_t)store.size());
		m_tickSlices.push_back(s);
		return s;
	}

	WTSKlineData* keep(WTSKlineData* k)
	{
		if (k != NULL)
			m_klines.push_back(k);
		return k;
	}

	WTSValueArray* keep(WTSValueArray* v)
	{
		if (v != NULL)
			m_arrays.push_back(v);
		return v;
	}

	WTSSessionInfo*	m_sInfo = NULL;
	std::vector<WTSKlineData*>	m_klines;
	std::vector<WTSKlineSlice*>	m_slices;
	std::vector<WTSTickSlice*>	m_tickSlices;
	std::vector<WTSValueArray*>	m_arrays;
	FactoryProbe			m_factory;
};

TEST_F(P12Extract, Minute1ResampleSixBars)
{
	std::vector<WTSBarStruct> bars;
	bars.push_back(mkMinBar(20240105, 901, 10.0, 1));
	bars.push_back(mkMinBar(20240105, 902, 11.0, 2));
	bars.push_back(mkMinBar(20240105, 903, 12.0, 3));
	bars.push_back(mkMinBar(20240105, 904, 13.0, 4));
	bars.push_back(mkMinBar(20240105, 905, 14.0, 5));
	bars.push_back(mkMinBar(20240105, 906, 15.0, 6));

	WTSKlineData* ret = keep(m_factory.extractKlineData(newSlice(bars), KP_Minute1, 3, m_sInfo));
	ASSERT_TRUE(ret != NULL);
	EXPECT_STREQ(kCode, ret->code());
	EXPECT_EQ(KP_Minute1, ret->period());
	EXPECT_EQ(3u, ret->times());
	ASSERT_EQ(2u, ret->size());

	// 第一桶 0901~0903 -> 标记 0903
	const WTSBarStruct* b0 = ret->at(0);
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 903), b0->time);
	EXPECT_DOUBLE_EQ(10.0, b0->open);
	EXPECT_DOUBLE_EQ(13.0, b0->high);	// max(11, 12, 13)
	EXPECT_DOUBLE_EQ(9.0, b0->low);		// min(9, 10, 11)
	EXPECT_DOUBLE_EQ(12.5, b0->close);
	EXPECT_DOUBLE_EQ(6.0, b0->vol);		// 1+2+3
	EXPECT_DOUBLE_EQ(10.0 + 22.0 + 36.0, b0->money);
	EXPECT_DOUBLE_EQ(30.0, (double)b0->hold);	// 取最后一条
	EXPECT_DOUBLE_EQ(6.0, (double)b0->add);		// 1+2+3
	// 目标最后一根与原始最后一根时间一致 -> 保持闭合
	EXPECT_TRUE(ret->isClosed());
}

TEST_F(P12Extract, Minute1TrimsOpenBarWhenExcluded)
{
	std::vector<WTSBarStruct> bars;
	bars.push_back(mkMinBar(20240105, 901, 10.0, 1));
	bars.push_back(mkMinBar(20240105, 902, 11.0, 2));
	bars.push_back(mkMinBar(20240105, 903, 12.0, 3));
	bars.push_back(mkMinBar(20240105, 904, 13.0, 4));
	bars.push_back(mkMinBar(20240105, 905, 14.0, 5));

	// 目标 0906 > 原始 0905 -> bIncludeOpen=true 只置未闭合标志
	WTSKlineSlice* slice = newSlice(bars);
	WTSKlineData* withOpen = keep(m_factory.extractMin1Data(slice, 3, m_sInfo, true, false));
	ASSERT_TRUE(withOpen != NULL);
	EXPECT_EQ(2u, withOpen->size());
	EXPECT_FALSE(withOpen->isClosed());

	WTSKlineData* withoutOpen = keep(m_factory.extractMin1Data(slice, 3, m_sInfo, false, false));
	ASSERT_TRUE(withoutOpen != NULL);
	EXPECT_EQ(1u, withoutOpen->size());
	EXPECT_TRUE(withoutOpen->isClosed());

	// sInfo == NULL 的独立校验
	EXPECT_EQ(NULL, m_factory.extractMin1Data(slice, 3, NULL, true, false));
}

TEST_F(P12Extract, Minute5ResampleAndAligned)
{
	std::vector<WTSBarStruct> bars;
	bars.push_back(mkMinBar(20240105, 906, 10.0, 1));
	bars.push_back(mkMinBar(20240105, 911, 11.0, 2));
	bars.push_back(mkMinBar(20240105, 915, 12.0, 3));

	WTSKlineSlice* slice = newSlice(bars);
	// 0906 -> 1 -> 10 -> 0910；0911 -> 6 -> 10 -> 合并；0915 -> 10 -> 20 -> 0920
	WTSKlineData* ret = keep(m_factory.extractMin5Data(slice, 2, m_sInfo, true, false));
	ASSERT_TRUE(ret != NULL);
	EXPECT_EQ(KP_Minute5, ret->period());
	EXPECT_EQ(2u, ret->times());
	ASSERT_EQ(2u, ret->size());
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 910), ret->at(0)->time);
	EXPECT_EQ(20240105u, ret->at(0)->date);	// 该分支取 curBar.date
	EXPECT_DOUBLE_EQ(3.0, ret->at(0)->vol);
	EXPECT_DOUBLE_EQ(13.0, ret->at(0)->close);
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 920), ret->at(1)->time);
	// 0920 > 原始 0915 -> 未闭合
	EXPECT_FALSE(ret->isClosed());

	// bAlignSec=true：secMins=[60]，桶落在小节内的第一个 10 分钟点
	WTSKlineData* aligned = keep(m_factory.extractMin5Data(slice, 2, m_sInfo, true, true));
	ASSERT_TRUE(aligned != NULL);
	EXPECT_GE(aligned->size(), 1u);

	EXPECT_EQ(NULL, m_factory.extractMin5Data(slice, 2, NULL, true, false));
}

TEST_F(P12Extract, HourAndHalfResample)
{
	std::vector<WTSBarStruct> bars;
	bars.push_back(mkMinBar(20240105, 930, 10.0, 1));
	bars.push_back(mkMinBar(20240105, 1000, 11.0, 2));
	bars.push_back(mkMinBar(20240105, 1030, 12.0, 3));

	WTSKlineSlice* slice = newSlice(bars);
	WTSKlineData* hour = keep(m_factory.extractHourData(slice, m_sInfo, true));
	ASSERT_TRUE(hour != NULL);
	EXPECT_EQ(KP_Hour, hour->period());
	EXPECT_EQ(1u, hour->times());
	ASSERT_EQ(2u, hour->size());
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 1000), hour->at(0)->time);
	EXPECT_DOUBLE_EQ(3.0, hour->at(0)->vol);
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 1100), hour->at(1)->time);
	EXPECT_FALSE(hour->isClosed());	// 1100 > 原始 1030

	WTSKlineData* hourTrim = keep(m_factory.extractHourData(slice, m_sInfo, false));
	ASSERT_TRUE(hourTrim != NULL);
	EXPECT_EQ(1u, hourTrim->size());
	EXPECT_TRUE(hourTrim->isClosed());
	EXPECT_EQ(NULL, m_factory.extractHourData(slice, NULL, true));

	std::vector<WTSBarStruct> halfBars;
	halfBars.push_back(mkMinBar(20240105, 910, 10.0, 1));
	halfBars.push_back(mkMinBar(20240105, 930, 11.0, 2));
	halfBars.push_back(mkMinBar(20240105, 945, 12.0, 3));
	WTSKlineSlice* halfSlice = newSlice(halfBars);
	WTSKlineData* half = keep(m_factory.extractHalfData(halfSlice, m_sInfo, true));
	ASSERT_TRUE(half != NULL);
	EXPECT_EQ(KP_Half, half->period());
	ASSERT_EQ(2u, half->size());
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 930), half->at(0)->time);
	EXPECT_DOUBLE_EQ(3.0, half->at(0)->vol);
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 1000), half->at(1)->time);
	EXPECT_EQ(NULL, m_factory.extractHalfData(halfSlice, NULL, true));
}

TEST_F(P12Extract, DayResampleGroupsByCount)
{
	std::vector<WTSBarStruct> bars;
	bars.push_back(mkBar(20240101, 0, 10, 12, 9, 11, 1, 100, 10, 1));
	bars.push_back(mkBar(20240102, 0, 11, 15, 8, 14, 2, 200, 20, 5));
	bars.push_back(mkBar(20240103, 0, 14, 16, 13, 15, 3, 300, 30, 7));
	bars.push_back(mkBar(20240104, 0, 15, 18, 12, 17, 4, 400, 40, 9));
	bars.push_back(mkBar(20240105, 0, 17, 20, 16, 19, 5, 500, 50, 11));

	WTSKlineData* ret = keep(m_factory.extractKlineData(newSlice(bars), KP_DAY, 2, m_sInfo));
	ASSERT_TRUE(ret != NULL);
	EXPECT_EQ(KP_DAY, ret->period());
	EXPECT_EQ(2u, ret->times());
	ASSERT_EQ(3u, ret->size());
	EXPECT_EQ(20240101u, ret->at(0)->date);
	EXPECT_EQ(0u, ret->at(0)->time);
	EXPECT_DOUBLE_EQ(3.0, ret->at(0)->vol);
	EXPECT_DOUBLE_EQ(300.0, ret->at(0)->money);
	// 前两根（high 12 / 15）合成日线：合并语义是 max(lastBar->high, curBar.high)
	// （src/WTSTools/WTSDataFactory.cpp:160），所以是 15，不是第三根的 16。
	// 原期望值 16.0 是本用例自己算错了输入，产品行为正确 —— 裁定见
	// docs/迭代计划/迭代01/reports/FAIL裁定记录-20261027.md（A-02）。
	EXPECT_DOUBLE_EQ(15.0, ret->at(0)->high);
	EXPECT_DOUBLE_EQ(8.0, ret->at(0)->low);
	EXPECT_DOUBLE_EQ(14.0, ret->at(0)->close);
	// 注意：日线分组用的是赋值而非累加（add 只保留最后一条）
	EXPECT_DOUBLE_EQ(5.0, (double)ret->at(0)->add);
	EXPECT_DOUBLE_EQ(20.0, (double)ret->at(0)->hold);
	EXPECT_EQ(20240103u, ret->at(1)->date);
	EXPECT_DOUBLE_EQ(7.0, ret->at(1)->vol);
	EXPECT_EQ(20240105u, ret->at(2)->date);
	EXPECT_DOUBLE_EQ(5.0, ret->at(2)->vol);
}

TEST_F(P12Extract, TickSliceToSecBars)
{
	std::vector<WTSTickStruct> ticks;
	ticks.push_back(mkTickStruct(3500.0, 1.0, 90012500));
	ticks.push_back(mkTickStruct(3510.0, 2.0, 90020000));
	ticks.push_back(mkTickStruct(3490.0, 4.0, 90045000));

	WTSTickSlice* slice = newTickSlice(ticks);
	// 空切片与 NULL sInfo 的拒绝路径
	WTSTickSlice* empty = WTSTickSlice::create(kCode, NULL, 0);
	EXPECT_EQ(NULL, m_factory.extractKlineData(empty, 30, m_sInfo));
	empty->release();
	EXPECT_EQ(NULL, m_factory.extractKlineData(slice, 30, NULL));

	WTSKlineData* ret = keep(m_factory.extractKlineData(slice, 30, m_sInfo, false, false));
	ASSERT_TRUE(ret != NULL);
	EXPECT_EQ(KP_Tick, ret->period());
	EXPECT_EQ(30u, ret->times());
	EXPECT_FALSE(ret->isUnixTime());
	ASSERT_EQ(2u, ret->size());
	// 12s/20s 同属 30 秒桶，45s 落入 60 秒桶
	EXPECT_EQ((uint64_t)20240105 * 1000000 + 90030, ret->at(0)->time);
	EXPECT_EQ(20240105u, ret->at(0)->date);
	EXPECT_DOUBLE_EQ(3500.0, ret->at(0)->open);
	EXPECT_DOUBLE_EQ(3510.0, ret->at(0)->close);
	EXPECT_DOUBLE_EQ(3510.0, ret->at(0)->high);
	EXPECT_DOUBLE_EQ(3500.0, ret->at(0)->low);	// 同桶合并只取 3500/3510 的较小值
	EXPECT_DOUBLE_EQ(3.0, ret->at(0)->vol);
	EXPECT_EQ((uint64_t)20240105 * 1000000 + 90100, ret->at(1)->time);
	EXPECT_DOUBLE_EQ(3490.0, ret->at(1)->open);

	WTSKlineData* unixRet = keep(m_factory.extractKlineData(slice, 30, m_sInfo, true, false));
	ASSERT_TRUE(unixRet != NULL);
	EXPECT_TRUE(unixRet->isUnixTime());
	ASSERT_EQ(2u, unixRet->size());
	EXPECT_GT(unixRet->at(0)->time, 1000000000u);
	EXPECT_GT(unixRet->at(1)->time, unixRet->at(0)->time);
}

TEST_F(P12Extract, MergeKlineDataPaths)
{
	WTSKlineData* base = keep(WTSKlineData::create(kCode, 0));
	base->setPeriod(KP_Minute1, 1);
	base->appendBar(mkBar(20240105, TimeUtils::timeToMinBar(20240105, 903), 10, 12, 9, 11, 1, 10, 1, 1));
	base->appendBar(mkBar(20240105, TimeUtils::timeToMinBar(20240105, 906), 11, 13, 10, 12, 2, 20, 2, 1));

	WTSKlineData* otherCode = keep(WTSKlineData::create("OTHER", 0));
	otherCode->setPeriod(KP_Minute1, 1);
	EXPECT_FALSE(m_factory.mergeKlineData(NULL, otherCode));
	EXPECT_FALSE(m_factory.mergeKlineData(base, NULL));
	EXPECT_FALSE(m_factory.mergeKlineData(base, otherCode));

	WTSKlineData* otherPeriod = keep(WTSKlineData::create(kCode, 0));
	otherPeriod->setPeriod(KP_Minute5, 1);
	EXPECT_FALSE(m_factory.mergeKlineData(base, otherPeriod));
	WTSKlineData* otherTimes = keep(WTSKlineData::create(kCode, 0));
	otherTimes->setPeriod(KP_Minute1, 3);
	EXPECT_FALSE(m_factory.mergeKlineData(base, otherTimes));

	// 目标为空 -> swap
	WTSKlineData* emptyTarget = keep(WTSKlineData::create(kCode, 0));
	emptyTarget->setPeriod(KP_Minute1, 1);
	ASSERT_TRUE(m_factory.mergeKlineData(emptyTarget, base));
	EXPECT_EQ(2u, emptyTarget->size());
	EXPECT_EQ(0u, base->size());

	// 头部/尾部/重叠：重叠区间的数据被丢弃
	WTSKlineData* filler = keep(WTSKlineData::create(kCode, 0));
	filler->setPeriod(KP_Minute1, 1);
	filler->appendBar(mkBar(20240105, TimeUtils::timeToMinBar(20240105, 903), 1, 1, 1, 1, 1, 1, 1, 1));
	filler->appendBar(mkBar(20240105, TimeUtils::timeToMinBar(20240105, 906), 2, 2, 2, 2, 2, 2, 2, 2));
	WTSKlineData* patch = keep(WTSKlineData::create(kCode, 0));
	patch->setPeriod(KP_Minute1, 1);
	patch->appendBar(mkBar(20240105, TimeUtils::timeToMinBar(20240105, 901), 3, 3, 3, 3, 3, 3, 3, 3));
	patch->appendBar(mkBar(20240105, TimeUtils::timeToMinBar(20240105, 903), 4, 4, 4, 4, 4, 4, 4, 4));
	patch->appendBar(mkBar(20240105, TimeUtils::timeToMinBar(20240105, 909), 5, 5, 5, 5, 5, 5, 5, 5));
	ASSERT_TRUE(m_factory.mergeKlineData(filler, patch));
	ASSERT_EQ(4u, filler->size());
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 901), filler->at(0)->time);
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 903), filler->at(1)->time);
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 906), filler->at(2)->time);
	EXPECT_EQ(TimeUtils::timeToMinBar(20240105, 909), filler->at(3)->time);
	// 被合并方数据未被清空，只是没有回写
	EXPECT_EQ(3u, patch->size());

	// KP_DAY 以 date 作为时间轴
	WTSKlineData* dayBase = keep(WTSKlineData::create(kCode, 0));
	dayBase->setPeriod(KP_DAY, 1);
	dayBase->appendBar(mkBar(20240103, 0, 1, 1, 1, 1, 1, 1, 1, 1));
	WTSKlineData* dayPatch = keep(WTSKlineData::create(kCode, 0));
	dayPatch->setPeriod(KP_DAY, 1);
	dayPatch->appendBar(mkBar(20240101, 0, 2, 2, 2, 2, 2, 2, 2, 2));
	dayPatch->appendBar(mkBar(20240103, 0, 3, 3, 3, 3, 3, 3, 3, 3));
	dayPatch->appendBar(mkBar(20240105, 0, 4, 4, 4, 4, 4, 4, 4, 4));
	ASSERT_TRUE(m_factory.mergeKlineData(dayBase, dayPatch));
	ASSERT_EQ(3u, dayBase->size());
	EXPECT_EQ(20240101u, dayBase->at(0)->date);
	EXPECT_EQ(20240103u, dayBase->at(1)->date);
	EXPECT_EQ(20240105u, dayBase->at(2)->date);
}

TEST_F(P12Extract, PrevMinuteRounding)
{
	EXPECT_EQ(859u, m_factory.getPrevMinute(900));
	EXPECT_EQ(2359u, m_factory.getPrevMinute(0));
	EXPECT_EQ(925u, m_factory.getPrevMinute(930, 5));
	EXPECT_EQ(950u, m_factory.getPrevMinute(1000, 10));
	EXPECT_EQ(904u, m_factory.getPrevMinute(905, 1));
}

TEST_F(P12Extract, SliceAssemblyAcrossBlocks)
{
	std::vector<WTSBarStruct> blk1;
	blk1.push_back(mkMinBar(20240105, 901, 10.0, 1));
	blk1.push_back(mkMinBar(20240105, 902, 11.0, 2));
	blk1.push_back(mkMinBar(20240105, 903, 12.0, 3));
	std::vector<WTSBarStruct> blk2;
	blk2.push_back(mkMinBar(20240105, 904, 13.0, 4));
	blk2.push_back(mkMinBar(20240105, 905, 14.0, 5));

	WTSKlineSlice* slice = newSlice(blk1);
	EXPECT_FALSE(slice->appendBlock(NULL, 5));
	EXPECT_FALSE(slice->appendBlock(blk2.data(), 0));
	EXPECT_TRUE(slice->appendBlock(blk2.data(), 2));
	EXPECT_EQ(5u, (uint32_t)slice->size());
	EXPECT_EQ(2u, (uint32_t)slice->get_block_counts());
	EXPECT_EQ(3u, slice->get_block_size(0));
	EXPECT_EQ(2u, slice->get_block_size(1));
	EXPECT_EQ(NULL, slice->get_block_addr(2));
	EXPECT_EQ(0u, slice->get_block_size(2));
	EXPECT_EQ(blk1.data(), slice->get_block_addr(0));

	// 跨 block 寻址
	EXPECT_EQ(&blk1[0], slice->at(0));
	EXPECT_EQ(&blk2[0], slice->at(3));
	EXPECT_EQ(&blk2[1], slice->at(-1));
	EXPECT_EQ(&blk1[0], slice->at(-6));	// translateIdx 钳到 0
	EXPECT_EQ(NULL, slice->at(5));		// 越界后遍历完 block 返回 NULL

	// 切片字段抽取 + 数值数组
	WTSValueArray* closes = keep(slice->extractData(KFT_CLOSE, 0, -1));
	ASSERT_TRUE(closes != NULL);
	EXPECT_EQ(5u, closes->size());
	EXPECT_DOUBLE_EQ(10.5, closes->at(0));
	EXPECT_DOUBLE_EQ(14.5, closes->at(4));
	EXPECT_DOUBLE_EQ(INVALID_DOUBLE, closes->at(9));

	WTSValueArray* svols = keep(slice->extractData(KFT_SVOLUME, 0, -1));
	ASSERT_TRUE(svols != NULL);
	EXPECT_DOUBLE_EQ(1.0, svols->at(0));		// close > open -> +vol
	EXPECT_DOUBLE_EQ(3.0, svols->at(2));

	WTSValueArray* dates = keep(slice->extractData(KFT_DATE, 1, 2));
	ASSERT_TRUE(dates != NULL);
	EXPECT_EQ(2u, dates->size());
	EXPECT_DOUBLE_EQ(20240105.0, dates->at(0));

	WTSValueArray* times = keep(slice->extractData(KFT_TIME, 0, 0));
	ASSERT_TRUE(times != NULL);
	EXPECT_EQ(1u, times->size());

	// 数值数组自身的越界与无效值语义
	WTSValueArray* arr = keep(WTSValueArray::create());
	arr->resize(3, INVALID_DOUBLE);
	EXPECT_EQ(3u, arr->size());
	EXPECT_DOUBLE_EQ(INVALID_DOUBLE, arr->at(0));
	arr->append(2.0);
	arr->set(0, 1.0);
	arr->set(9, 99.0);	// 越界写入被忽略（idx 为无符号，负数会回绕成大下标同样被拒）
	EXPECT_DOUBLE_EQ(1.0, arr->at(0));
	EXPECT_DOUBLE_EQ(INVALID_DOUBLE, arr->at(2));	// 仍是 resize 的默认值
	EXPECT_DOUBLE_EQ(2.0, arr->at(3));
	EXPECT_DOUBLE_EQ(INVALID_DOUBLE, arr->at(99));	// 越界读返回 INVALID_DOUBLE
	EXPECT_EQ(4u, arr->size());
	EXPECT_FALSE(arr->empty());
	EXPECT_DOUBLE_EQ(2.0, arr->getDataRef()[3]);
	arr->set(3, 5.0);
	EXPECT_DOUBLE_EQ(5.0, arr->getDataRef()[3]);
}

// 越界/崩溃类输入整类收进本用例（默认不执行，仅登记缺陷）：
// 1) WTSValueArray::maxvalue(0, size) / minvalue(0, size)：守卫写成 end > size，
//    允许 end == size，循环 i <= end 会读取 m_vecData[size]（越界读）。
// 2) WTSKlineSlice::maxprice/minprice：at(begin) 越界返回 NULL 后直接解引用（空指针）。
// 3) extractKlineData(WTSTickSlice*, 0, ...)：seconds == 0 -> 整数除零。
// 4) updateMin1Data(tick) 在时段外 tick + 空 klineData 下解引用 NULL（见 p12_factory_update_tick_test.cpp）。
// 详见 docs/迭代计划/迭代01/bugs/p12_datafactory_datadef.md（BUG-P12-001 ~ BUG-P12-004）。
TEST_F(P12Extract, DISABLED_OutOfBoundsClass)
{
	WTSValueArray* arr = keep(WTSValueArray::create());
	arr->append(1.0);
	arr->append(2.0);
	// end == size 时循环访问 m_vecData[2] -> 越界读
	(void)arr->maxvalue(0, 2);

	std::vector<WTSTickStruct> ticks;
	ticks.push_back(mkTickStruct(3500.0, 1.0, 90012500));
	WTSTickSlice* slice = newTickSlice(ticks);
	// seconds == 0 -> 除零崩溃
	(void)m_factory.extractKlineData(slice, 0, m_sInfo, false, false);
}

}	// namespace
