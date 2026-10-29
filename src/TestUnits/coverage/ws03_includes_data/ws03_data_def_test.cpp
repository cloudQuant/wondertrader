// WS03 Includes 数据模型测试：WTSDataDef.hpp
// 只覆盖公开 API、边界与错误路径；不依赖外部服务，全部为确定性用例。
#include <gtest/gtest.h>

#include <climits>
#include <cstring>

#include "Includes/WTSDataDef.hpp"

using namespace wtp;

namespace
{
WTSBarStruct mkBar(uint32_t date, uint64_t time, double o, double h, double l, double c, double vol)
{
	WTSBarStruct bar;
	bar.date = date;
	bar.time = time;
	bar.open = o;
	bar.high = h;
	bar.low = l;
	bar.close = c;
	bar.vol = vol;
	return bar;
}

void fillTick(WTSTickStruct& ts)
{
	wt_strcpy(ts.exchg, "SHFE");
	wt_strcpy(ts.code, "rb2401");
	ts.price = 3500.5;
	ts.open = 3490.0;
	ts.high = 3510.0;
	ts.low = 3480.0;
	ts.pre_close = 3488.0;
	ts.pre_settle = 3489.0;
	ts.pre_interest = 1000.0;
	ts.upper_limit = 3800.0;
	ts.lower_limit = 3200.0;
	ts.total_volume = 123456.0;
	ts.volume = 1234.0;
	ts.settle_price = 3495.0;
	ts.open_interest = 2000.0;
	ts.diff_interest = 12.0;
	ts.total_turnover = 987654321.0;
	ts.turn_over = 9876543.0;
	ts.trading_date = 20240102;
	ts.action_date = 20240103;
	ts.action_time = 101500500;
	for (int i = 0; i < 10; i++)
	{
		ts.bid_prices[i] = 3499.0 + i;
		ts.ask_prices[i] = 3501.0 + i;
		ts.bid_qty[i] = 10.0 + i;
		ts.ask_qty[i] = 20.0 + i;
	}
}
}

//////////////////////////////////////////////////////////////////////////
// WTSValueArray
//////////////////////////////////////////////////////////////////////////
TEST(ws03_value_array, lifecycle_and_access)
{
	WTSValueArray* va = WTSValueArray::create();
	ASSERT_NE(va, nullptr);

	EXPECT_EQ(va->size(), 0u);
	EXPECT_TRUE(va->empty());
	EXPECT_DOUBLE_EQ(va->at(0), INVALID_DOUBLE);
	EXPECT_EQ(va->translateIdx(-1), -1);

	va->append(10.0);
	va->append(20.0);
	va->append(30.0);
	EXPECT_EQ(va->size(), 3u);
	EXPECT_FALSE(va->empty());
	EXPECT_DOUBLE_EQ(va->at(0), 10.0);
	EXPECT_DOUBLE_EQ(va->at(2), 30.0);
	EXPECT_DOUBLE_EQ(va->at(3), INVALID_DOUBLE);
	EXPECT_EQ(va->translateIdx(-1), 2);
	EXPECT_EQ(va->translateIdx(1), 1);
	ASSERT_EQ(va->getDataRef().size(), 3u);

	// set：区间内生效，越界忽略
	va->set(1, 25.0);
	EXPECT_DOUBLE_EQ(va->at(1), 25.0);
	va->set(3, 99.0);
	EXPECT_EQ(va->size(), 3u);

	// resize：默认值与指定值
	va->resize(5);
	EXPECT_EQ(va->size(), 5u);
	EXPECT_DOUBLE_EQ(va->at(4), INVALID_DOUBLE);
	va->resize(6, 7.5);
	EXPECT_EQ(va->size(), 6u);
	EXPECT_DOUBLE_EQ(va->at(5), 7.5);

	// operator[] 可变/只读
	(*va)[0] = 1.5;
	EXPECT_DOUBLE_EQ((*va)[0], 1.5);
	const WTSValueArray* cva = va;
	EXPECT_DOUBLE_EQ((*cva)[0], 1.5);

	va->release();
}

TEST(ws03_value_array, max_min_range_and_abs)
{
	WTSValueArray* va = WTSValueArray::create();
	va->append(-5.0);
	va->append(INVALID_DOUBLE);
	va->append(3.0);
	va->append(-8.0);
	va->append(2.0);

	EXPECT_DOUBLE_EQ(va->maxvalue(0, 4), 3.0);
	EXPECT_DOUBLE_EQ(va->minvalue(0, 4), -8.0);
	EXPECT_DOUBLE_EQ(va->maxvalue(0, 4, true), 8.0);
	EXPECT_DOUBLE_EQ(va->minvalue(0, 4, true), 2.0);

	// head/tail 反向
	EXPECT_DOUBLE_EQ(va->maxvalue(4, 0), 3.0);
	EXPECT_DOUBLE_EQ(va->minvalue(4, 0), -8.0);

	// 子区间与负下标
	EXPECT_DOUBLE_EQ(va->maxvalue(0, 0), -5.0);
	EXPECT_DOUBLE_EQ(va->minvalue(-3, -1), -8.0);
	EXPECT_DOUBLE_EQ(va->maxvalue(-5, -3), 3.0);

	// 全部为 INVALID 的分支
	EXPECT_DOUBLE_EQ(va->maxvalue(1, 1), INVALID_DOUBLE);
	EXPECT_DOUBLE_EQ(va->minvalue(1, 1), INVALID_DOUBLE);

	// 越界
	EXPECT_DOUBLE_EQ(va->maxvalue(5, 5), INVALID_DOUBLE);
	EXPECT_DOUBLE_EQ(va->minvalue(9, 10), INVALID_DOUBLE);

	va->release();
}

//////////////////////////////////////////////////////////////////////////
// WTSKlineSlice
//////////////////////////////////////////////////////////////////////////
TEST(ws03_kline_slice, blocks_and_access)
{
	WTSBarStruct blk1[3];
	blk1[0] = mkBar(20240102, 900, 10.0, 12.0, 9.0, 11.0, 100.0);
	blk1[1] = mkBar(20240102, 901, 11.0, 13.0, 10.0, 12.0, 200.0);
	blk1[2] = mkBar(20240102, 902, 12.0, 14.0, 11.0, 13.0, 300.0);
	WTSBarStruct blk2[2];
	blk2[0] = mkBar(20240103, 900, 20.0, 22.0, 19.0, 21.0, 400.0);
	blk2[1] = mkBar(20240103, 901, 21.0, 23.0, 20.0, 22.0, 500.0);

	WTSKlineSlice* slice = WTSKlineSlice::create("SHFE.rb2401", KP_Minute1, 5, blk1, 3);
	ASSERT_NE(slice, nullptr);
	EXPECT_STREQ(slice->code(), "SHFE.rb2401");
	EXPECT_EQ(slice->size(), 3);
	EXPECT_FALSE(slice->empty());
	EXPECT_EQ(slice->get_block_counts(), 1u);
	EXPECT_EQ(slice->get_block_addr(0), blk1);
	EXPECT_EQ(slice->get_block_size(0), 3u);
	EXPECT_EQ(slice->get_block_addr(1), nullptr);
	EXPECT_EQ(slice->get_block_size(1), 0u);

	EXPECT_EQ(slice->at(0), blk1);
	EXPECT_EQ(slice->at(2), blk1 + 2);
	EXPECT_EQ(slice->at(-1), blk1 + 2);
	EXPECT_EQ(slice->at(3), nullptr);

	const WTSKlineSlice* cslice = slice;
	EXPECT_EQ(cslice->at(1), blk1 + 1);
	EXPECT_EQ(cslice->at(-2), blk1 + 1);
	EXPECT_EQ(cslice->at(10), nullptr);

	// 追加第二段
	EXPECT_TRUE(slice->appendBlock(blk2, 2));
	EXPECT_FALSE(slice->appendBlock(nullptr, 2));
	EXPECT_FALSE(slice->appendBlock(blk2, 0));
	EXPECT_EQ(slice->size(), 5);
	EXPECT_EQ(slice->get_block_counts(), 2u);
	EXPECT_EQ(slice->get_block_addr(1), blk2);
	EXPECT_EQ(slice->get_block_size(1), 2u);
	EXPECT_EQ(slice->at(3), blk2);
	EXPECT_EQ(slice->at(4), blk2 + 1);
	EXPECT_EQ(slice->at(-1), blk2 + 1);
	EXPECT_EQ(slice->at(5), nullptr);

	EXPECT_DOUBLE_EQ(slice->maxprice(0, 4), 23.0);
	EXPECT_DOUBLE_EQ(slice->minprice(0, 4), 9.0);
	EXPECT_DOUBLE_EQ(slice->maxprice(4, 0), 23.0);
	EXPECT_DOUBLE_EQ(slice->minprice(2, -1), 11.0);

	slice->setCode("DCE.i2405");
	EXPECT_STREQ(slice->code(), "DCE.i2405");
	slice->release();
}

TEST(ws03_kline_slice, empty_slice)
{
	WTSKlineSlice* slice = WTSKlineSlice::create("SHFE.rb2401", KP_DAY, 1);
	ASSERT_NE(slice, nullptr);
	EXPECT_EQ(slice->size(), 0);
	EXPECT_TRUE(slice->empty());
	EXPECT_EQ(slice->get_block_counts(), 0u);
	EXPECT_EQ(slice->get_block_addr(0), nullptr);
	EXPECT_EQ(slice->get_block_size(0), 0u);
	EXPECT_EQ(slice->at(0), nullptr);
	EXPECT_EQ(slice->at(-1), nullptr);
	EXPECT_EQ(slice->extractData(KFT_CLOSE), nullptr);

	// 空切片不能追加空块
	WTSBarStruct bar = mkBar(20240102, 0, 1.0, 1.0, 1.0, 1.0, 0.0);
	EXPECT_FALSE(slice->appendBlock(&bar, 0));
	EXPECT_FALSE(slice->appendBlock(nullptr, 1));
	EXPECT_TRUE(slice->appendBlock(&bar, 1));
	EXPECT_EQ(slice->size(), 1);
	slice->release();
}

TEST(ws03_kline_slice, extract_data_fields)
{
	WTSBarStruct bars[4];
	bars[0] = mkBar(20240102, 900, 10.0, 12.0, 9.0, 11.0, 100.0);
	bars[1] = mkBar(20240102, 901, 11.0, 13.0, 10.0, 12.0, 4000000000.0);	// > INT_MAX
	bars[2] = mkBar(20240102, 902, 12.0, 14.0, 11.0, 10.0, 200.0);			// close < open
	bars[3] = mkBar(20240102, 903, 13.0, 15.0, 12.0, 14.0, 300.0);

	WTSKlineSlice* slice = WTSKlineSlice::create("SHFE.rb2401", KP_Minute1, 1, bars, 4);
	ASSERT_NE(slice, nullptr);

	WTSValueArray* open = slice->extractData(KFT_OPEN, 0, -1);
	ASSERT_NE(open, nullptr);
	ASSERT_EQ(open->size(), 4u);
	EXPECT_DOUBLE_EQ(open->at(0), 10.0);
	EXPECT_DOUBLE_EQ(open->at(3), 13.0);
	open->release();

	WTSValueArray* high = slice->extractData(KFT_HIGH, 1, 2);
	ASSERT_NE(high, nullptr);
	ASSERT_EQ(high->size(), 2u);
	EXPECT_DOUBLE_EQ(high->at(0), 13.0);
	EXPECT_DOUBLE_EQ(high->at(1), 14.0);
	high->release();

	WTSValueArray* low = slice->extractData(KFT_LOW, 0, 0);
	ASSERT_NE(low, nullptr);
	ASSERT_EQ(low->size(), 1u);
	EXPECT_DOUBLE_EQ(low->at(0), 9.0);
	low->release();

	WTSValueArray* close = slice->extractData(KFT_CLOSE, -1, -1);
	ASSERT_NE(close, nullptr);
	ASSERT_EQ(close->size(), 1u);
	EXPECT_DOUBLE_EQ(close->at(0), 14.0);
	close->release();

	WTSValueArray* vol = slice->extractData(KFT_VOLUME);
	ASSERT_NE(vol, nullptr);
	EXPECT_DOUBLE_EQ(vol->at(1), 4000000000.0);
	vol->release();

	// 带方向成交量：超过 INT_MAX 时只保留方向，否则为带符号量
	WTSValueArray* svol = slice->extractData(KFT_SVOLUME);
	ASSERT_NE(svol, nullptr);
	EXPECT_DOUBLE_EQ(svol->at(0), 100.0);	// close > open
	EXPECT_DOUBLE_EQ(svol->at(1), 1.0);		// vol > INT_MAX 且 close > open
	EXPECT_DOUBLE_EQ(svol->at(2), -200.0);	// close < open
	svol->release();

	WTSValueArray* date = slice->extractData(KFT_DATE);
	ASSERT_NE(date, nullptr);
	EXPECT_DOUBLE_EQ(date->at(0), 20240102.0);
	date->release();

	WTSValueArray* tm = slice->extractData(KFT_TIME);
	ASSERT_NE(tm, nullptr);
	EXPECT_DOUBLE_EQ(tm->at(3), 903.0);
	tm->release();

	// 与数据相交的区间被裁剪到合法范围
	WTSValueArray* clamp = slice->extractData(KFT_CLOSE, 2, 20);
	ASSERT_NE(clamp, nullptr);
	EXPECT_EQ(clamp->size(), 2u);
	EXPECT_DOUBLE_EQ(clamp->at(0), 10.0);
	EXPECT_DOUBLE_EQ(clamp->at(1), 14.0);
	clamp->release();

	slice->release();
}

TEST(ws03_kline_slice, extract_data_rejects_range_after_all_bars)
{
	WTSBarStruct bars[2];
	bars[0] = mkBar(20240102, 900, 10.0, 12.0, 9.0, 11.0, 100.0);
	bars[1] = mkBar(20240102, 901, 11.0, 13.0, 10.0, 12.0, 200.0);

	WTSKlineSlice* slice = WTSKlineSlice::create("SHFE.rb2401", KP_Minute1, 1, bars, 2);
	ASSERT_NE(slice, nullptr);

	// WTSKlineSlice::extractData 声明全越界时返回 NULL。当前实现可能返回空数组；
	// 仍要求这里可安全观察该返回值，不解引用非预期的结果。
	WTSValueArray* outside = slice->extractData(KFT_CLOSE, 10, 20);
	EXPECT_EQ(nullptr, outside);
	if (outside != nullptr)
		outside->release();

	slice->release();
}

//////////////////////////////////////////////////////////////////////////
// WTSKlineData
//////////////////////////////////////////////////////////////////////////
TEST(ws03_kline_data, create_period_and_empty)
{
	WTSKlineData* kd = WTSKlineData::create("SHFE.rb2401", 0);
	ASSERT_NE(kd, nullptr);
	EXPECT_EQ(kd->size(), 0u);
	EXPECT_TRUE(kd->IsEmpty());
	EXPECT_STREQ(kd->code(), "SHFE.rb2401");
	EXPECT_EQ(kd->period(), KP_Minute1);
	EXPECT_EQ(kd->times(), 1u);
	EXPECT_FALSE(kd->isUnixTime());
	EXPECT_TRUE(kd->isClosed());
	EXPECT_EQ(kd->extractData(KFT_CLOSE), nullptr);
	EXPECT_EQ(kd->at(0), nullptr);

	kd->setPeriod(KP_Minute5, 3);
	EXPECT_EQ(kd->period(), KP_Minute5);
	EXPECT_EQ(kd->times(), 3u);
	kd->setUnixTime();
	EXPECT_TRUE(kd->isUnixTime());
	kd->setUnixTime(false);
	EXPECT_FALSE(kd->isUnixTime());
	kd->setClosed(false);
	EXPECT_FALSE(kd->isClosed());
	kd->setCode("DCE.i2405");
	EXPECT_STREQ(kd->code(), "DCE.i2405");
	kd->release();
}

TEST(ws03_kline_data, accessors_and_bounds)
{
	WTSKlineData* kd = WTSKlineData::create("SHFE.rb2401", 3);
	ASSERT_NE(kd, nullptr);
	ASSERT_EQ(kd->size(), 3u);
	EXPECT_FALSE(kd->IsEmpty());

	WTSKlineData::WTSBarList& ref = kd->getDataRef();
	ref[0] = mkBar(20240102, 900, 10.0, 12.0, 9.0, 11.0, 100.0);
	ref[1] = mkBar(20240102, 901, 11.0, 13.0, 10.0, 12.0, 200.0);
	ref[2] = mkBar(20240102, 902, 12.0, 14.0, 11.0, 13.0, 300.0);
	ref[0].hold = 1000.0;
	ref[0].add = 10.0;
	ref[0].bid = 9.9;
	ref[0].ask = 10.1;
	ref[0].money = 12345.0;

	EXPECT_DOUBLE_EQ(kd->open(0), 10.0);
	EXPECT_DOUBLE_EQ(kd->high(1), 13.0);
	EXPECT_DOUBLE_EQ(kd->low(2), 11.0);
	EXPECT_DOUBLE_EQ(kd->close(2), 13.0);
	EXPECT_DOUBLE_EQ(kd->volume(1), 200.0);
	EXPECT_DOUBLE_EQ(kd->openinterest(0), 1000.0);
	EXPECT_DOUBLE_EQ(kd->additional(0), 10.0);
	EXPECT_DOUBLE_EQ(kd->bidprice(0), 9.9);
	EXPECT_DOUBLE_EQ(kd->askprice(0), 10.1);
	EXPECT_DOUBLE_EQ(kd->money(0), 12345.0);
	EXPECT_EQ(kd->date(1), 20240102u);
	EXPECT_EQ(kd->time(2), 902u);

	// 负下标与越界
	EXPECT_DOUBLE_EQ(kd->open(-1), 12.0);
	EXPECT_EQ(kd->open(3), INVALID_DOUBLE);
	EXPECT_EQ(kd->high(5), INVALID_DOUBLE);
	EXPECT_EQ(kd->low(9), INVALID_DOUBLE);
	EXPECT_EQ(kd->close(3), INVALID_DOUBLE);
	EXPECT_EQ(kd->volume(4), INVALID_DOUBLE);
	EXPECT_EQ(kd->openinterest(3), static_cast<double>(INVALID_UINT32));
	EXPECT_EQ(kd->additional(3), INVALID_DOUBLE);
	EXPECT_EQ(kd->bidprice(3), static_cast<double>(INVALID_UINT32));
	EXPECT_EQ(kd->askprice(3), INVALID_DOUBLE);
	EXPECT_EQ(kd->money(3), INVALID_DOUBLE);
	EXPECT_EQ(kd->date(3), INVALID_UINT32);
	EXPECT_EQ(kd->time(3), static_cast<uint64_t>(INVALID_UINT32));

	EXPECT_EQ(kd->at(0), &ref[0]);
	EXPECT_EQ(kd->at(-1), &ref[2]);
	EXPECT_EQ(kd->at(3), nullptr);
	EXPECT_EQ(kd->at(99), nullptr);

	EXPECT_DOUBLE_EQ(kd->maxprice(0, 2), 14.0);
	EXPECT_DOUBLE_EQ(kd->minprice(0, 2), 9.0);
	EXPECT_DOUBLE_EQ(kd->maxprice(2, 0), 14.0);
	EXPECT_DOUBLE_EQ(kd->minprice(2, 0), 9.0);
	EXPECT_DOUBLE_EQ(kd->maxprice(-2, -1), 14.0);
	// 越界：begin >= size 或 end > size
	EXPECT_EQ(kd->maxprice(3, 3), INVALID_DOUBLE);
	EXPECT_EQ(kd->minprice(0, 3), INVALID_DOUBLE);
	EXPECT_EQ(kd->maxprice(0, 100), INVALID_DOUBLE);
	EXPECT_EQ(kd->minprice(100, 100), INVALID_DOUBLE);

	kd->release();
}

TEST(ws03_kline_data, append_bar_replaces_same_slot)
{
	WTSKlineData* kd = WTSKlineData::create("SHFE.rb2401", 0);
	ASSERT_NE(kd, nullptr);

	WTSBarStruct b1 = mkBar(20240102, 900, 10.0, 12.0, 9.0, 11.0, 100.0);
	kd->appendBar(b1);
	ASSERT_EQ(kd->size(), 1u);
	EXPECT_DOUBLE_EQ(kd->close(0), 11.0);

	// 相同 date/time：覆盖而不是追加
	WTSBarStruct b1u = mkBar(20240102, 900, 10.0, 13.0, 9.5, 12.5, 150.0);
	kd->appendBar(b1u);
	ASSERT_EQ(kd->size(), 1u);
	EXPECT_DOUBLE_EQ(kd->close(0), 12.5);
	EXPECT_DOUBLE_EQ(kd->volume(0), 150.0);

	// 不同 date/time：追加
	WTSBarStruct b2 = mkBar(20240102, 901, 12.5, 14.0, 12.0, 13.5, 250.0);
	kd->appendBar(b2);
	ASSERT_EQ(kd->size(), 2u);
	EXPECT_DOUBLE_EQ(kd->close(-1), 13.5);

	kd->release();
}

TEST(ws03_kline_data, extract_data_fields_and_invalid_range)
{
	WTSKlineData* kd = WTSKlineData::create("SHFE.rb2401", 4);
	ASSERT_NE(kd, nullptr);
	WTSKlineData::WTSBarList& ref = kd->getDataRef();
	ref[0] = mkBar(20240102, 900, 10.0, 12.0, 9.0, 11.0, 100.0);
	ref[1] = mkBar(20240102, 901, 11.0, 13.0, 10.0, 12.0, 4000000000.0);
	ref[2] = mkBar(20240102, 902, 12.0, 14.0, 11.0, 10.0, 200.0);
	ref[3] = mkBar(20240102, 903, 13.0, 15.0, 12.0, 14.0, 300.0);

	// 非法区间返回 NULL
	EXPECT_EQ(kd->extractData(KFT_CLOSE, 0, 4), nullptr);
	EXPECT_EQ(kd->extractData(KFT_CLOSE, 4, 4), nullptr);

	WTSValueArray* open = kd->extractData(KFT_OPEN, 0, 1);
	ASSERT_NE(open, nullptr);
	ASSERT_EQ(open->size(), 4u);	// 实现按整个容器抽取
	EXPECT_DOUBLE_EQ(open->at(0), 10.0);
	open->release();

	WTSValueArray* high = kd->extractData(KFT_HIGH);
	ASSERT_NE(high, nullptr);
	EXPECT_DOUBLE_EQ(high->at(3), 15.0);
	high->release();

	WTSValueArray* low = kd->extractData(KFT_LOW);
	ASSERT_NE(low, nullptr);
	EXPECT_DOUBLE_EQ(low->at(0), 9.0);
	low->release();

	WTSValueArray* close = kd->extractData(KFT_CLOSE);
	ASSERT_NE(close, nullptr);
	EXPECT_DOUBLE_EQ(close->at(2), 10.0);
	close->release();

	WTSValueArray* vol = kd->extractData(KFT_VOLUME);
	ASSERT_NE(vol, nullptr);
	EXPECT_DOUBLE_EQ(vol->at(1), 4000000000.0);
	vol->release();

	WTSValueArray* svol = kd->extractData(KFT_SVOLUME);
	ASSERT_NE(svol, nullptr);
	EXPECT_DOUBLE_EQ(svol->at(0), 100.0);
	EXPECT_DOUBLE_EQ(svol->at(1), 1.0);
	EXPECT_DOUBLE_EQ(svol->at(2), -200.0);
	svol->release();

	WTSValueArray* date = kd->extractData(KFT_DATE);
	ASSERT_NE(date, nullptr);
	EXPECT_DOUBLE_EQ(date->at(0), 20240102.0);
	date->release();

	WTSValueArray* tm = kd->extractData(KFT_TIME);
	ASSERT_NE(tm, nullptr);
	EXPECT_DOUBLE_EQ(tm->at(3), 903.0);
	tm->release();

	kd->release();
}

//////////////////////////////////////////////////////////////////////////
// WTSTickData / WTSTickRef
//////////////////////////////////////////////////////////////////////////
TEST(ws03_tick_data, create_by_code_and_flags)
{
	WTSTickData* tick = WTSTickData::create("SHFE.rb2401");
	ASSERT_NE(tick, nullptr);
	EXPECT_STREQ(tick->code(), "SHFE.rb2401");
	EXPECT_STREQ(tick->exchg(), "");
	EXPECT_DOUBLE_EQ(tick->price(), 0.0);

	EXPECT_FALSE(tick->isNewHigh());
	EXPECT_FALSE(tick->isNewLow());
	EXPECT_EQ(tick->getLimitFlag(), 0u);
	tick->setLimitFlag(1);
	EXPECT_TRUE(tick->isNewHigh());
	EXPECT_FALSE(tick->isNewLow());
	tick->setLimitFlag(2);
	EXPECT_FALSE(tick->isNewHigh());
	EXPECT_TRUE(tick->isNewLow());
	tick->setLimitFlag(3);
	EXPECT_TRUE(tick->isNewHigh());
	EXPECT_TRUE(tick->isNewLow());

	tick->setCode("DCE.i2405");
	EXPECT_STREQ(tick->code(), "DCE.i2405");
	tick->setCode("X", 1);
	EXPECT_STREQ(tick->code(), "X");

	tick->setContractInfo(nullptr);
	EXPECT_EQ(tick->getContractInfo(), nullptr);
	tick->release();
}

TEST(ws03_tick_data, accessors_and_ten_levels)
{
	WTSTickStruct ts;
	fillTick(ts);
	WTSTickData* tick = WTSTickData::create(ts);
	ASSERT_NE(tick, nullptr);

	EXPECT_STREQ(tick->code(), "rb2401");
	EXPECT_STREQ(tick->exchg(), "SHFE");
	EXPECT_DOUBLE_EQ(tick->price(), 3500.5);
	EXPECT_DOUBLE_EQ(tick->open(), 3490.0);
	EXPECT_DOUBLE_EQ(tick->high(), 3510.0);
	EXPECT_DOUBLE_EQ(tick->low(), 3480.0);
	EXPECT_DOUBLE_EQ(tick->preclose(), 3488.0);
	EXPECT_DOUBLE_EQ(tick->presettle(), 3489.0);
	EXPECT_DOUBLE_EQ(tick->preinterest(), 1000.0);
	EXPECT_DOUBLE_EQ(tick->upperlimit(), 3800.0);
	EXPECT_DOUBLE_EQ(tick->lowerlimit(), 3200.0);
	EXPECT_DOUBLE_EQ(tick->totalvolume(), 123456.0);
	EXPECT_DOUBLE_EQ(tick->volume(), 1234.0);
	EXPECT_DOUBLE_EQ(tick->settlepx(), 3495.0);
	EXPECT_DOUBLE_EQ(tick->openinterest(), 2000.0);
	EXPECT_DOUBLE_EQ(tick->additional(), 12.0);
	EXPECT_DOUBLE_EQ(tick->totalturnover(), 987654321.0);
	EXPECT_DOUBLE_EQ(tick->turnover(), 9876543.0);
	EXPECT_EQ(tick->tradingdate(), 20240102u);
	EXPECT_EQ(tick->actiondate(), 20240103u);
	EXPECT_EQ(tick->actiontime(), 101500500u);

	EXPECT_DOUBLE_EQ(tick->bidprice(0), 3499.0);
	EXPECT_DOUBLE_EQ(tick->bidprice(9), 3508.0);
	EXPECT_DOUBLE_EQ(tick->bidprice(-1), -1.0);
	EXPECT_DOUBLE_EQ(tick->bidprice(10), -1.0);
	EXPECT_DOUBLE_EQ(tick->askprice(0), 3501.0);
	EXPECT_DOUBLE_EQ(tick->askprice(-3), -1.0);
	EXPECT_DOUBLE_EQ(tick->bidqty(2), 12.0);
	EXPECT_DOUBLE_EQ(tick->bidqty(10), -1.0);
	EXPECT_DOUBLE_EQ(tick->askqty(2), 22.0);
	EXPECT_DOUBLE_EQ(tick->askqty(-1), -1.0);

	WTSTickStruct& ref = tick->getTickStruct();
	EXPECT_EQ(&ref, &tick->getTickStruct());
	EXPECT_DOUBLE_EQ(ref.price, 3500.5);

	tick->release();
}

TEST(ws03_tick_ref, reference_semantics)
{
	WTSTickStruct ts;
	fillTick(ts);

	WTSTickRef* ref = WTSTickRef::create(&ts);
	ASSERT_NE(ref, nullptr);
	EXPECT_TRUE(ref->valid());
	EXPECT_TRUE(static_cast<bool>(*ref));
	EXPECT_EQ(ref->getTickStruct(), &ts);

	EXPECT_STREQ(ref->code(), "rb2401");
	EXPECT_STREQ(ref->exchg(), "SHFE");
	EXPECT_DOUBLE_EQ(ref->price(), 3500.5);
	EXPECT_DOUBLE_EQ(ref->open(), 3490.0);
	EXPECT_DOUBLE_EQ(ref->high(), 3510.0);
	EXPECT_DOUBLE_EQ(ref->low(), 3480.0);
	EXPECT_DOUBLE_EQ(ref->preclose(), 3488.0);
	EXPECT_DOUBLE_EQ(ref->presettle(), 3489.0);
	EXPECT_DOUBLE_EQ(ref->preinterest(), 1000.0);
	EXPECT_DOUBLE_EQ(ref->upperlimit(), 3800.0);
	EXPECT_DOUBLE_EQ(ref->lowerlimit(), 3200.0);
	EXPECT_DOUBLE_EQ(ref->totalvolume(), 123456.0);
	EXPECT_DOUBLE_EQ(ref->volume(), 1234.0);
	EXPECT_DOUBLE_EQ(ref->settlepx(), 3495.0);
	EXPECT_DOUBLE_EQ(ref->openinterest(), 2000.0);
	EXPECT_DOUBLE_EQ(ref->additional(), 12.0);
	EXPECT_DOUBLE_EQ(ref->totalturnover(), 987654321.0);
	EXPECT_DOUBLE_EQ(ref->turnover(), 9876543.0);
	EXPECT_EQ(ref->tradingdate(), 20240102u);
	EXPECT_EQ(ref->actiondate(), 20240103u);
	EXPECT_EQ(ref->actiontime(), 101500500u);

	EXPECT_DOUBLE_EQ(ref->bidprice(0), 3499.0);
	EXPECT_DOUBLE_EQ(ref->bidprice(10), -1.0);
	EXPECT_DOUBLE_EQ(ref->askprice(0), 3501.0);
	EXPECT_DOUBLE_EQ(ref->askprice(-1), -1.0);
	EXPECT_DOUBLE_EQ(ref->bidqty(0), 10.0);
	EXPECT_DOUBLE_EQ(ref->bidqty(-1), -1.0);
	EXPECT_DOUBLE_EQ(ref->askqty(0), 20.0);
	EXPECT_DOUBLE_EQ(ref->askqty(10), -1.0);

	ref->setContractInfo(nullptr);
	EXPECT_EQ(ref->getContractInfo(), nullptr);
	ref->release();

	WTSTickRef* empty = WTSTickRef::create(nullptr);
	ASSERT_NE(empty, nullptr);
	EXPECT_FALSE(empty->valid());
	EXPECT_FALSE(static_cast<bool>(*empty));
	EXPECT_EQ(empty->getTickStruct(), nullptr);
	empty->release();
}

//////////////////////////////////////////////////////////////////////////
// WTSOrdQueData / WTSOrdDtlData / WTSTransData
//////////////////////////////////////////////////////////////////////////
TEST(ws03_order_queue_data, create_and_accessors)
{
	WTSOrdQueStruct oq;
	wt_strcpy(oq.exchg, "SSE");
	wt_strcpy(oq.code, "600000");
	oq.trading_date = 20240102;
	oq.action_date = 20240103;
	oq.action_time = 93000000;
	oq.side = BDT_Buy;
	oq.price = 10.5;
	oq.order_items = 3;
	oq.qsize = 2;
	oq.volumes[0] = 100;

	WTSOrdQueData* data = WTSOrdQueData::create(oq);
	ASSERT_NE(data, nullptr);
	EXPECT_STREQ(data->exchg(), "SSE");
	EXPECT_STREQ(data->code(), "600000");
	EXPECT_EQ(data->tradingdate(), 20240102u);
	EXPECT_EQ(data->actiondate(), 20240103u);
	EXPECT_EQ(data->actiontime(), 93000000u);
	EXPECT_EQ(data->getOrdQueStruct().qsize, 2u);
	EXPECT_DOUBLE_EQ(data->getOrdQueStruct().price, 10.5);

	data->setCode("600001");
	EXPECT_STREQ(data->code(), "600001");
	data->setContractInfo(nullptr);
	EXPECT_EQ(data->getContractInfo(), nullptr);
	data->release();

	WTSOrdQueData* byCode = WTSOrdQueData::create("600519");
	ASSERT_NE(byCode, nullptr);
	EXPECT_STREQ(byCode->code(), "600519");
	EXPECT_STREQ(byCode->exchg(), "");
	byCode->release();
}

TEST(ws03_order_detail_data, create_and_accessors)
{
	WTSOrdDtlStruct od;
	wt_strcpy(od.exchg, "SZSE");
	wt_strcpy(od.code, "000001");
	od.trading_date = 20240102;
	od.action_date = 20240103;
	od.action_time = 100000000;
	od.index = 42;
	od.price = 9.8;
	od.volume = 500;
	od.side = BDT_Sell;
	od.otype = ODT_LimitPrice;
	od.orderid = 12345;

	WTSOrdDtlData* data = WTSOrdDtlData::create(od);
	ASSERT_NE(data, nullptr);
	EXPECT_STREQ(data->exchg(), "SZSE");
	EXPECT_STREQ(data->code(), "000001");
	EXPECT_EQ(data->tradingdate(), 20240102u);
	EXPECT_EQ(data->actiondate(), 20240103u);
	EXPECT_EQ(data->actiontime(), 100000000u);
	EXPECT_EQ(data->getOrdDtlStruct().index, 42u);
	EXPECT_EQ(data->getOrdDtlStruct().orderid, 12345);

	data->setCode("000002");
	EXPECT_STREQ(data->code(), "000002");
	data->setContractInfo(nullptr);
	EXPECT_EQ(data->getContractInfo(), nullptr);
	data->release();

	WTSOrdDtlData* byCode = WTSOrdDtlData::create("600000");
	ASSERT_NE(byCode, nullptr);
	EXPECT_STREQ(byCode->code(), "600000");
	byCode->release();
}

TEST(ws03_trans_data, create_and_accessors)
{
	WTSTransStruct ts;
	wt_strcpy(ts.exchg, "SZSE");
	wt_strcpy(ts.code, "000001");
	ts.trading_date = 20240102;
	ts.action_date = 20240103;
	ts.action_time = 100000500;
	ts.index = 77;
	ts.ttype = TT_Match;
	ts.side = BDT_Buy;
	ts.price = 9.9;
	ts.volume = 300;
	ts.askorder = 1;
	ts.bidorder = 2;

	WTSTransData* data = WTSTransData::create(ts);
	ASSERT_NE(data, nullptr);
	EXPECT_STREQ(data->exchg(), "SZSE");
	EXPECT_STREQ(data->code(), "000001");
	EXPECT_EQ(data->tradingdate(), 20240102u);
	EXPECT_EQ(data->actiondate(), 20240103u);
	EXPECT_EQ(data->actiontime(), 100000500u);
	EXPECT_EQ(data->getTransStruct().index, 77u);
	EXPECT_DOUBLE_EQ(data->getTransStruct().price, 9.9);

	data->setCode("000003");
	EXPECT_STREQ(data->code(), "000003");
	data->setContractInfo(nullptr);
	EXPECT_EQ(data->getContractInfo(), nullptr);
	data->release();

	WTSTransData* byCode = WTSTransData::create("600000");
	ASSERT_NE(byCode, nullptr);
	EXPECT_STREQ(byCode->code(), "600000");
	byCode->release();
}

//////////////////////////////////////////////////////////////////////////
// WTSHisTickData
//////////////////////////////////////////////////////////////////////////
TEST(ws03_his_tick_data, create_and_append_with_factor)
{
	WTSTickStruct ts;
	fillTick(ts);

	WTSHisTickData* his = WTSHisTickData::create("SHFE.rb2401", 2, true, 2.0);
	ASSERT_NE(his, nullptr);
	EXPECT_EQ(his->size(), 2u);
	EXPECT_FALSE(his->empty());
	EXPECT_STREQ(his->code(), "SHFE.rb2401");
	EXPECT_TRUE(his->isValidOnly());
	ASSERT_NE(his->at(0), nullptr);
	EXPECT_DOUBLE_EQ(his->at(0)->price, 0.0);
	EXPECT_EQ(his->at(2), nullptr);
	EXPECT_EQ(his->at(100), nullptr);
	ASSERT_EQ(his->getDataRef().size(), 2u);

	his->appendTick(ts);
	ASSERT_EQ(his->size(), 3u);
	EXPECT_DOUBLE_EQ(his->at(2)->price, 7001.0);
	EXPECT_DOUBLE_EQ(his->at(2)->open, 6980.0);
	EXPECT_DOUBLE_EQ(his->at(2)->high, 7020.0);
	EXPECT_DOUBLE_EQ(his->at(2)->low, 6960.0);
	his->release();

	// WTSDataDef.hpp L1320(unsigned int nSize) 与 L1337(bool bValidOnly) 两个 create 重载的
	// 默认参数集合对单字符串实参完全等价, 裸调用会产生二义性(见 bugs/l03_includes.md BUG-L03-001)。
	// 这里用显式 static_cast 锚定到 bool 重载, 语义为"不预分配 tick 数组"。
	WTSHisTickData* empty = WTSHisTickData::create("DCE.i2405", static_cast<bool>(false));
	ASSERT_NE(empty, nullptr);
	EXPECT_EQ(empty->size(), 0u);
	EXPECT_TRUE(empty->empty());
	EXPECT_FALSE(empty->isValidOnly());
	EXPECT_EQ(empty->at(0), nullptr);
	empty->appendTick(ts);
	EXPECT_EQ(empty->size(), 1u);
	EXPECT_DOUBLE_EQ(empty->at(0)->price, 3500.5);
	empty->release();
}

//////////////////////////////////////////////////////////////////////////
// WTSTickSlice
//////////////////////////////////////////////////////////////////////////
TEST(ws03_tick_slice, blocks_access_and_insert)
{
	WTSTickStruct ticks[3];
	fillTick(ticks[0]);
	ticks[1] = ticks[0];
	ticks[1].price = 3501.0;
	ticks[2] = ticks[0];
	ticks[2].price = 3502.0;

	WTSTickSlice* slice = WTSTickSlice::create("SHFE.rb2401", ticks, 3);
	ASSERT_NE(slice, nullptr);
	EXPECT_EQ(slice->size(), 3u);
	EXPECT_FALSE(slice->empty());
	EXPECT_EQ(slice->get_block_counts(), 1u);
	EXPECT_EQ(slice->get_block_addr(0), ticks);
	EXPECT_EQ(slice->get_block_size(0), 3u);
	EXPECT_EQ(slice->get_block_addr(1), nullptr);
	EXPECT_EQ(slice->get_block_size(1), INVALID_UINT32);

	ASSERT_NE(slice->at(0), nullptr);
	EXPECT_DOUBLE_EQ(slice->at(0)->price, 3500.5);
	EXPECT_DOUBLE_EQ(slice->at(2)->price, 3502.0);
	EXPECT_DOUBLE_EQ(slice->at(-1)->price, 3502.0);
	EXPECT_EQ(slice->at(3), nullptr);

	WTSTickStruct extra[2];
	fillTick(extra[0]);
	extra[0].price = 3503.0;
	extra[1] = extra[0];
	extra[1].price = 3504.0;
	EXPECT_TRUE(slice->appendBlock(extra, 2));
	EXPECT_FALSE(slice->appendBlock(nullptr, 2));
	EXPECT_FALSE(slice->appendBlock(extra, 0));
	EXPECT_EQ(slice->size(), 5u);
	EXPECT_EQ(slice->get_block_counts(), 2u);
	EXPECT_DOUBLE_EQ(slice->at(3)->price, 3503.0);
	EXPECT_DOUBLE_EQ(slice->at(4)->price, 3504.0);

	WTSTickStruct first;
	fillTick(first);
	first.price = 3499.0;
	EXPECT_TRUE(slice->insertBlock(0, &first, 1));
	EXPECT_FALSE(slice->insertBlock(0, nullptr, 1));
	EXPECT_FALSE(slice->insertBlock(0, &first, 0));
	EXPECT_EQ(slice->size(), 6u);
	EXPECT_EQ(slice->get_block_counts(), 3u);
	EXPECT_DOUBLE_EQ(slice->at(0)->price, 3499.0);
	EXPECT_DOUBLE_EQ(slice->at(1)->price, 3500.5);
	slice->release();

	// 空块（count == 0）仍建立块，但切片为空
	WTSTickSlice* zero = WTSTickSlice::create("SHFE.rb2401", ticks, 0);
	ASSERT_NE(zero, nullptr);
	EXPECT_EQ(zero->size(), 0u);
	EXPECT_TRUE(zero->empty());
	EXPECT_EQ(zero->get_block_counts(), 1u);
	EXPECT_EQ(zero->get_block_size(0), 0u);
	EXPECT_EQ(zero->at(0), nullptr);
	zero->release();
}

//////////////////////////////////////////////////////////////////////////
// WTSOrdDtlSlice / WTSOrdQueSlice / WTSTransSlice
//////////////////////////////////////////////////////////////////////////
TEST(ws03_order_detail_slice, create_and_at)
{
	WTSOrdDtlStruct items[2];
	memset(items, 0, sizeof(items));
	items[0].price = 1.5;
	items[1].price = 2.5;

	EXPECT_EQ(WTSOrdDtlSlice::create("SSE.600000", nullptr, 2), nullptr);
	EXPECT_EQ(WTSOrdDtlSlice::create("SSE.600000", items, 0), nullptr);

	WTSOrdDtlSlice* slice = WTSOrdDtlSlice::create("SSE.600000", items, 2);
	ASSERT_NE(slice, nullptr);
	EXPECT_EQ(slice->size(), 2u);
	EXPECT_FALSE(slice->empty());
	EXPECT_EQ(slice->at(0), items);
	EXPECT_EQ(slice->at(1), items + 1);
	EXPECT_EQ(slice->at(-1), items + 1);
	EXPECT_EQ(slice->at(-2), items);
	slice->release();
}

TEST(ws03_order_queue_slice, create_and_at)
{
	WTSOrdQueStruct items[2];
	memset(items, 0, sizeof(items));
	items[0].price = 3.5;
	items[1].price = 4.5;

	EXPECT_EQ(WTSOrdQueSlice::create("SSE.600000", nullptr, 2), nullptr);
	EXPECT_EQ(WTSOrdQueSlice::create("SSE.600000", items, 0), nullptr);

	WTSOrdQueSlice* slice = WTSOrdQueSlice::create("SSE.600000", items, 2);
	ASSERT_NE(slice, nullptr);
	EXPECT_EQ(slice->size(), 2u);
	EXPECT_FALSE(slice->empty());
	EXPECT_EQ(slice->at(0), items);
	EXPECT_EQ(slice->at(1), items + 1);
	EXPECT_EQ(slice->at(-1), items + 1);
	EXPECT_EQ(slice->at(-2), items);
	slice->release();
}

TEST(ws03_trans_slice, create_and_at)
{
	WTSTransStruct items[2];
	memset(items, 0, sizeof(items));
	items[0].price = 5.5;
	items[1].price = 6.5;

	EXPECT_EQ(WTSTransSlice::create("SSE.600000", nullptr, 2), nullptr);
	EXPECT_EQ(WTSTransSlice::create("SSE.600000", items, 0), nullptr);

	WTSTransSlice* slice = WTSTransSlice::create("SSE.600000", items, 2);
	ASSERT_NE(slice, nullptr);
	EXPECT_EQ(slice->size(), 2u);
	EXPECT_FALSE(slice->empty());
	EXPECT_EQ(slice->at(0), items);
	EXPECT_EQ(slice->at(1), items + 1);
	EXPECT_EQ(slice->at(-1), items + 1);
	EXPECT_EQ(slice->at(-2), items);
	slice->release();
}
