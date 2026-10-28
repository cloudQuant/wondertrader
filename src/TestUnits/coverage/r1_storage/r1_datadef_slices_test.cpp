// R1 / src/Includes/WTSDataDef.hpp — KLineSlice / TickSlice / ValueArray / HisTickData 字段计算
//
// 覆盖（无生产改动，纯 header 语义，全部走公开入口）：
//  - WTSValueArray: create/append/at(含负下标)/set/resize/maxvalue/minvalue(含 isAbs 与 INVALID 跳空)
//  - WTSKlineSlice: create/appendBlock/at 跨块寻址/maxprice/minprice/extractData(含 KFT_SVOLUME 两条分支)
//  - WTSKlineData:  setPeriod/isClosed/setUnixTime/maxprice 越界 INVALID_DOUBLE
//  - WTSTickSlice:  appendBlock/insertBlock/at/get_block_size 越界 INVALID_UINT32
//  - WTSHisTickData: appendTick 的复权修正(price/open/high/low * factor)
//  - WTSOrdDtlSlice/WTSOrdQueSlice/WTSTransSlice: create/appendBlock/at 主链路
//
// 复用 q03_storage3 夹具（仅取其 include 集与 make_* 构造器），无 private public、无线程。
#include "../q03_storage3/q03_storage3_fixture.h"

#include "Includes/WTSDataDef.hpp"

namespace r1 {

using namespace q03;

class R1DataDefTest : public ::testing::Test
{
protected:
	void SetUp() override {}
};

// ---------------------------------------------------------------- WTSValueArray
TEST_F(R1DataDefTest, value_array_append_at_negative_index_and_resize)
{
	WTSValueArray* arr = WTSValueArray::create();
	EXPECT_TRUE(arr->empty());
	arr->append(1.5);
	arr->append(2.5);
	arr->append(3.5);
	EXPECT_EQ(arr->size(), 3u);
	EXPECT_DOUBLE_EQ(arr->at(0), 1.5);
	EXPECT_DOUBLE_EQ(arr->at(-1), 3.5);		// translateIdx 负下标回卷
	EXPECT_DOUBLE_EQ(arr->at(-3), 1.5);
	EXPECT_TRUE(arr->at(3) == INVALID_DOUBLE);	// 越界 INVALID_DOUBLE
	arr->resize(5);
	EXPECT_EQ(arr->size(), 5u);
	EXPECT_TRUE(arr->at(3) == INVALID_DOUBLE);
	arr->set(3, 9.0);
	EXPECT_DOUBLE_EQ(arr->at(3), 9.0);
	arr->set(99, 0.0);						// 越界 set 静默忽略
	EXPECT_TRUE(arr->at(99) == INVALID_DOUBLE);
	arr->release();
}

TEST_F(R1DataDefTest, value_array_max_min_with_abs_and_invalid_holes)
{
	WTSValueArray* arr = WTSValueArray::create();
	arr->append(-5.0);
	arr->append(INVALID_DOUBLE);
	arr->append(2.0);
	arr->append(-7.0);
	EXPECT_DOUBLE_EQ(arr->maxvalue(0, 3), 2.0);
	EXPECT_DOUBLE_EQ(arr->minvalue(0, 3), -7.0);
	EXPECT_DOUBLE_EQ(arr->maxvalue(0, 3, true), 7.0);	// isAbs 分支
	EXPECT_DOUBLE_EQ(arr->minvalue(1, 1), INVALID_DOUBLE);	// 区间全是 INVALID
	arr->release();
}

// ---------------------------------------------------------------- WTSKlineSlice
class KlineSliceProbe
{
public:
	static WTSBarStruct make(uint32_t date, uint32_t seq, double close_bias)
	{
		WTSBarStruct bar = make_bar(date, (uint64_t)date * 10000 + seq, seq);
		bar.close += close_bias;
		return bar;
	}

	static vector<WTSBarStruct> three_day_bars()
	{
		vector<WTSBarStruct> bars;
		bars.push_back(make(20240108, 0, 0.0));		// close 10.5
		bars.push_back(make(20240108, 1, 5.0));		// close 15.5
		bars.push_back(make(20240109, 2, -3.0));	// close 7.5
		return bars;
	}
};

TEST_F(R1DataDefTest, kline_slice_multi_block_at_walks_offsets)
{
	vector<WTSBarStruct> blk1 = { KlineSliceProbe::make(20240108, 0, 0.0), KlineSliceProbe::make(20240108, 1, 5.0) };
	vector<WTSBarStruct> blk2 = { KlineSliceProbe::make(20240109, 2, -3.0) };

	WTSKlineSlice* slice = WTSKlineSlice::create("SHFE.rb2401", KP_DAY, 1, blk1.data(), (int32_t)blk1.size());
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->code(), std::string("SHFE.rb2401"));
	EXPECT_EQ(slice->size(), 2);
	EXPECT_TRUE(slice->appendBlock(blk2.data(), (uint32_t)blk2.size()));
	EXPECT_FALSE(slice->appendBlock(NULL, 3));			// NULL 块拒绝
	EXPECT_FALSE(slice->appendBlock(blk2.data(), 0));		// 0 长块拒绝
	EXPECT_EQ(slice->size(), 3);
	EXPECT_EQ(slice->get_block_counts(), (std::size_t)2);
	EXPECT_EQ(slice->get_block_addr(5), (WTSBarStruct*)NULL);
	EXPECT_EQ(slice->get_block_size(5), 0u);

	EXPECT_EQ(slice->at(0), blk1.data());				// 第 0 条落在第 0 块头
	EXPECT_EQ(slice->at(2), blk2.data());				// 跨块：idx 2 → 块1 偏移 0
	EXPECT_EQ(slice->at(-1), blk2.data());				// 负下标 = 最后一条
	EXPECT_EQ(slice->at(9), (WTSBarStruct*)NULL);		// 越界
	slice->release();
}

TEST_F(R1DataDefTest, kline_slice_max_min_price_and_empty_behavior)
{
	vector<WTSBarStruct> bars = KlineSliceProbe::three_day_bars();
	WTSKlineSlice* emptySlice = WTSKlineSlice::create("SHFE.rb2401", KP_Minute1, 1);
	EXPECT_TRUE(emptySlice->empty());
	EXPECT_EQ(emptySlice->extractData(KFT_CLOSE), (WTSValueArray*)NULL);	// 空切片返回 NULL
	WTSValueArray* hole = emptySlice->extractData(KFT_CLOSE);
	EXPECT_TRUE(hole == NULL);
	emptySlice->release();

	WTSKlineSlice* slice = WTSKlineSlice::create("SHFE.rb2401", KP_DAY, 1, bars.data(), (int32_t)bars.size());
	// high: 11.0/12.0/13.0 → max 13；low: 9.0/10.0/11.0 → min 9
	EXPECT_DOUBLE_EQ(slice->maxprice(0, 2), 13.0);
	EXPECT_DOUBLE_EQ(slice->minprice(0, 2), 9.0);
	EXPECT_DOUBLE_EQ(slice->maxprice(2, 0), 13.0);		// head>tail 时内部会交换
	EXPECT_DOUBLE_EQ(slice->minprice(-2, -1), 10.0);	// 后两条里的最小 low
	slice->release();
}

TEST_F(R1DataDefTest, kline_slice_extract_data_all_field_types)
{
	vector<WTSBarStruct> bars = KlineSliceProbe::three_day_bars();
	bars[0].vol = 100;	// 阳线 close>open → +100
	bars[1].vol = 200;	// 阳线 → +200
	bars[2].vol = 300;	// close 7.5 < open 12 → 阴线 → -300
	WTSKlineSlice* slice = WTSKlineSlice::create("SHFE.rb2401", KP_DAY, 1, bars.data(), (int32_t)bars.size());

	WTSValueArray* closes = slice->extractData(KFT_CLOSE);
	ASSERT_TRUE(closes != NULL);
	ASSERT_EQ(closes->size(), 3u);
	EXPECT_DOUBLE_EQ(closes->at(0), 10.5);
	EXPECT_DOUBLE_EQ(closes->at(2), 7.5);
	closes->release();

	WTSValueArray* svol = slice->extractData(KFT_SVOLUME);
	ASSERT_TRUE(svol != NULL);
	EXPECT_DOUBLE_EQ(svol->at(0), 100.0);
	EXPECT_DOUBLE_EQ(svol->at(2), -300.0);		// 阴线取负
	svol->release();

	WTSValueArray* dates = slice->extractData(KFT_DATE, 1, 2);
	ASSERT_TRUE(dates != NULL);
	EXPECT_EQ(dates->size(), 2u);
	EXPECT_DOUBLE_EQ(dates->at(0), 20240108.0);
	EXPECT_DOUBLE_EQ(dates->at(1), 20240109.0);
	dates->release();

	WTSValueArray* times = slice->extractData(KFT_TIME);
	ASSERT_TRUE(times != NULL);
	EXPECT_DOUBLE_EQ(times->at(0), (double)bars[0].time);
	times->release();
	slice->release();
}

TEST_F(R1DataDefTest, kline_slice_extract_svolume_big_volume_clamped_branch)
{
	vector<WTSBarStruct> bars = KlineSliceProbe::three_day_bars();
	bars[0].vol = (double)INT_MAX + 100.0;		// > INT_MAX 分支：符号钳到 ±1
	WTSKlineSlice* slice = WTSKlineSlice::create("SHFE.rb2401", KP_DAY, 1, bars.data(), (int32_t)bars.size());
	WTSValueArray* svol = slice->extractData(KFT_SVOLUME, 0, 0);
	ASSERT_TRUE(svol != NULL);
	EXPECT_DOUBLE_EQ(svol->at(0), 1.0);			// 阳线大 Volume → +1
	svol->release();
	bars[0].close = bars[0].open - 1.0;			// 切成阴线
	WTSValueArray* svol2 = slice->extractData(KFT_SVOLUME, 0, 0);
	EXPECT_DOUBLE_EQ(svol2->at(0), -1.0);
	svol2->release();
	slice->release();
}

// ---------------------------------------------------------------- WTSKlineData
TEST_F(R1DataDefTest, kline_data_flags_period_and_range_bounds)
{
	WTSKlineData* kd = WTSKlineData::create("SSE.STK.600000", 3);
	kd->setPeriod(KP_Minute5, 5);
	EXPECT_EQ(kd->period(), KP_Minute5);
	EXPECT_EQ(kd->times(), 5u);
	EXPECT_TRUE(kd->isClosed());
	kd->setClosed(false);
	EXPECT_FALSE(kd->isClosed());
	EXPECT_FALSE(kd->isUnixTime());
	kd->setUnixTime();
	EXPECT_TRUE(kd->isUnixTime());

	kd->at(0)->high = 12.0;
	kd->at(1)->high = 18.0;
	kd->at(2)->high = 14.0;
	kd->at(2)->low = 8.0;
	EXPECT_DOUBLE_EQ(kd->maxprice(0, 2), 18.0);
	EXPECT_DOUBLE_EQ(kd->minprice(0, 2), 8.0);
	EXPECT_TRUE(kd->maxprice(0, 5) == INVALID_DOUBLE);	// end > size → INVALID
	EXPECT_EQ(kd->code(), std::string("SSE.STK.600000"));
	kd->release();
}

// ---------------------------------------------------------------- WTSTickSlice
TEST_F(R1DataDefTest, tick_slice_append_insert_and_index_walk)
{
	vector<WTSTickStruct> b1;
	b1.push_back(make_tick(20240108, 20240108, 93000000, 1));
	b1.push_back(make_tick(20240108, 20240108, 93100000, 2));
	vector<WTSTickStruct> b2;
	b2.push_back(make_tick(20240108, 20240108, 145900000, 3));

	WTSTickSlice* slice = WTSTickSlice::create("SSE.600000", b1.data(), (uint32_t)b1.size());
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->size(), 2u);
	EXPECT_FALSE(slice->appendBlock(NULL, 1));
	EXPECT_FALSE(slice->appendBlock(b2.data(), 0));
	EXPECT_TRUE(slice->appendBlock(b2.data(), (uint32_t)b2.size()));
	EXPECT_EQ(slice->size(), 3u);
	EXPECT_EQ(slice->at(2), b2.data());
	EXPECT_EQ(slice->at(-1), b2.data());
	EXPECT_EQ(slice->at(0), b1.data());

	vector<WTSTickStruct> b0;
	b0.push_back(make_tick(20240108, 20240108, 92500000, 0));
	EXPECT_TRUE(slice->insertBlock(0, b0.data(), (uint32_t)b0.size()));
	EXPECT_EQ(slice->size(), 4u);
	EXPECT_EQ(slice->at(0), b0.data());					// 插在头部
	EXPECT_EQ(slice->get_block_size(99), INVALID_UINT32);	// 越界块大小返回 INVALID_UINT32
	EXPECT_EQ(slice->get_block_addr(99), (WTSTickStruct*)NULL);
	slice->release();
}

// ---------------------------------------------------------------- WTSHisTickData
TEST_F(R1DataDefTest, his_tick_data_append_tick_applies_adj_factor)
{
	WTSHisTickData* ht = WTSHisTickData::create("SSE.600000", true, 2.0);
	EXPECT_STREQ(ht->code(), "SSE.600000");
	EXPECT_TRUE(ht->isValidOnly());
	EXPECT_TRUE(ht->empty());
	WTSTickStruct tick = make_tick(20240108, 20240108, 93000000, 5);
	ht->appendTick(tick);
	ASSERT_EQ(ht->size(), 1u);
	WTSTickStruct* got = ht->at(0);
	ASSERT_TRUE(got != NULL);
	EXPECT_DOUBLE_EQ(got->price, tick.price * 2.0);		// 复权修正：price/open/high/low * factor
	EXPECT_DOUBLE_EQ(got->open, tick.open * 2.0);
	EXPECT_DOUBLE_EQ(got->high, tick.high * 2.0);
	EXPECT_DOUBLE_EQ(got->low, tick.low * 2.0);
	EXPECT_DOUBLE_EQ(got->total_volume, tick.total_volume);	// 量不参与复权
	EXPECT_EQ(ht->at(5), (WTSTickStruct*)NULL);
	ht->release();

	WTSHisTickData* pre = WTSHisTickData::create("SSE.600000", (unsigned int)4);
	EXPECT_EQ(pre->size(), 4u);
	pre->release();
}

// ---------------------------------------------------------------- hft slices
TEST_F(R1DataDefTest, orddtl_ordque_trans_slice_core_access)
{
	vector<WTSOrdDtlStruct> dtls;
	dtls.push_back(make_orddtl(20240108, 20240108, 93000000, 1));
	dtls.push_back(make_orddtl(20240108, 20240108, 93001000, 2));
	WTSOrdDtlSlice* ds = WTSOrdDtlSlice::create("SSE.600000", dtls.data(), (uint32_t)dtls.size());
	EXPECT_EQ(ds->size(), 2u);
	const WTSOrdDtlStruct* d0 = ds->at(0);
	ASSERT_TRUE(d0 != NULL);
	EXPECT_DOUBLE_EQ(d0->price, dtls[0].price);
	EXPECT_EQ(ds->at(7), (const WTSOrdDtlStruct*)NULL);
	ds->release();

	vector<WTSOrdQueStruct> ques;
	ques.push_back(make_ordque(20240108, 20240108, 93000000, 3));
	WTSOrdQueSlice* qs = WTSOrdQueSlice::create("SSE.600000", ques.data(), 1);
	EXPECT_EQ(qs->size(), 1u);
	EXPECT_DOUBLE_EQ(qs->at(0)->price, ques[0].price);
	qs->release();

	vector<WTSTransStruct> trans;
	trans.push_back(make_trans(20240108, 20240108, 93000000, 4));
	WTSTransSlice* ts = WTSTransSlice::create("SSE.600000", trans.data(), 1);
	EXPECT_EQ(ts->size(), 1u);
	EXPECT_DOUBLE_EQ(ts->at(0)->price, trans[0].price);
	ts->release();
}

} // namespace r1
