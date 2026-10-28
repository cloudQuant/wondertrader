// Lane R53 - src/Includes/WTSDataDef.hpp（补 r12/ws03 未覆盖的 const 与边界分支）
// 覆盖：WTSValueArray const operator[]、WTSKlineSlice const at/extract、
//       WTSKlineData 其余字段抽取与 KFT_SVOLUME 溢出分支、HFT 切片拒绝/负索引、
//       WTSTickSlice 零长度块、WTSTickRef 合约绑定。
// 说明：所有切片都指向栈/堆上的显式数组，不读取共享内存。
#include <gtest/gtest.h>

#include <climits>
#include <cstdint>
#include <cstring>
#include <vector>

#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSMarcos.h"
#include "Includes/WTSStruct.h"

using namespace wtp;

namespace
{
WTSBarStruct r53_make_bar(uint32_t date, uint32_t time, double o, double h, double l,
	double c, double vol)
{
	WTSBarStruct bar;
	bar.date = date;
	bar.time = time;
	bar.open = o;
	bar.high = h;
	bar.low = l;
	bar.close = c;
	bar.settle = (h + l) / 2.0;
	bar.money = vol * c;
	bar.vol = vol;
	bar.hold = vol * 10.0;
	bar.add = 1.0;
	return bar;
}

WTSTickStruct r53_make_tick(const char* code, double price)
{
	WTSTickStruct tick;
	wt_strcpy(tick.code, code);
	wt_strcpy(tick.exchg, "SHFE");
	tick.price = price;
	tick.open = price - 1.0;
	tick.high = price + 1.0;
	tick.low = price - 1.0;
	return tick;
}
}  // namespace

// WTSDataDef.hpp:66-84、183-191：const operator[] 与 at 的负数翻译
TEST(r53_value_array, const_index_and_bounds)
{
	WTSValueArray* arr = WTSValueArray::create();
	ASSERT_NE(arr, nullptr);
	arr->append(1.0);
	arr->append(2.0);
	arr->append(3.0);

	const WTSValueArray* carr = arr;
	EXPECT_EQ(carr->size(), 3u);
	EXPECT_FALSE(carr->empty());
	EXPECT_DOUBLE_EQ(carr->operator[](0), 1.0);
	EXPECT_DOUBLE_EQ(carr->operator[](2), 3.0);
	EXPECT_DOUBLE_EQ(carr->at(1), 2.0);
	EXPECT_DOUBLE_EQ(carr->at((uint32_t)-1), 3.0);
	EXPECT_TRUE(carr->at(3) == INVALID_DOUBLE);

	arr->operator[](1) = 20.0;
	EXPECT_DOUBLE_EQ(carr->at(1), 20.0);
	arr->release();
}

// WTSDataDef.hpp:280-317、384-434：const 切片 at 与 extractData 的区间夹取
TEST(r53_kline_slice, const_at_and_extract_ranges)
{
	std::vector<WTSBarStruct> bars;
	bars.push_back(r53_make_bar(20240506, 931, 10.0, 12.0, 9.0, 11.0, 100.0));
	bars.push_back(r53_make_bar(20240506, 932, 11.0, 13.0, 10.0, 12.0, 200.0));
	bars.push_back(r53_make_bar(20240506, 933, 12.0, 14.0, 11.0, 13.0, 300.0));

	WTSKlineSlice* slice = WTSKlineSlice::create("SHFE.rb2405", KP_Minute1, 1,
		bars.data(), static_cast<int32_t>(bars.size()));
	ASSERT_NE(slice, nullptr);
	const WTSKlineSlice* cslice = slice;
	ASSERT_NE(cslice->at(0), nullptr);
	EXPECT_DOUBLE_EQ(cslice->at(0)->open, 10.0);
	EXPECT_DOUBLE_EQ(cslice->at(-1)->close, 13.0);
	EXPECT_EQ(cslice->at(99), nullptr);
	EXPECT_EQ(cslice->size(), 3);
	EXPECT_FALSE(cslice->empty());
	EXPECT_EQ(cslice->code()[0], 'S');

	WTSValueArray* va = cslice->extractData(KFT_HIGH, 1, 2);
	ASSERT_NE(va, nullptr);
	EXPECT_EQ(va->size(), 2u);
	EXPECT_DOUBLE_EQ(va->at(0), 13.0);
	EXPECT_DOUBLE_EQ(va->at(1), 14.0);
	va->release();

	// 负区间从尾部翻译后仍按 min/max 归正
	WTSValueArray* tail = cslice->extractData(KFT_CLOSE, -2, -1);
	ASSERT_NE(tail, nullptr);
	EXPECT_EQ(tail->size(), 2u);
	EXPECT_DOUBLE_EQ(tail->at(0), 12.0);
	EXPECT_DOUBLE_EQ(tail->at(1), 13.0);
	tail->release();
	slice->release();
}

// WTSDataDef.hpp:746-796：WTSKlineData extractData 其余字段与 SVOLUME 溢出分支
TEST(r53_kline_data, extract_remaining_fields_and_volume_overflow)
{
	std::vector<WTSBarStruct> bars;
	bars.push_back(r53_make_bar(20240506, 931, 10.0, 12.0, 9.0, 11.0, (double)INT_MAX + 1.0));
	bars.push_back(r53_make_bar(20240506, 932, 10.0, 11.0, 8.0, 9.0, 100.0));
	bars.push_back(r53_make_bar(20240506, 933, 10.0, 10.5, 9.5, 10.0, (double)INT_MAX + 2.0));

	WTSKlineData* kdata = WTSKlineData::create("SHFE.rb2405", 0);
	ASSERT_NE(kdata, nullptr);
	for (const WTSBarStruct& bar : bars)
		kdata->appendBar(bar);
	ASSERT_EQ(kdata->size(), 3u);

	// vol > INT_MAX 时只保留方向（+1/-1），不参与数值转换
	WTSValueArray* sv = kdata->extractData(KFT_SVOLUME);
	ASSERT_NE(sv, nullptr);
	EXPECT_EQ(sv->size(), 3u);
	EXPECT_DOUBLE_EQ(sv->at(0), 1.0);
	EXPECT_DOUBLE_EQ(sv->at(1), -100.0);
	EXPECT_DOUBLE_EQ(sv->at(2), -1.0);	// close == open 视为负向
	sv->release();

	WTSValueArray* dates = kdata->extractData(KFT_DATE);
	ASSERT_NE(dates, nullptr);
	EXPECT_EQ(dates->size(), 3u);
	EXPECT_DOUBLE_EQ(dates->at(0), 20240506.0);
	dates->release();

	WTSValueArray* times = kdata->extractData(KFT_TIME, 1, 1);
	ASSERT_NE(times, nullptr);
	EXPECT_EQ(times->size(), 3u);	// 该实现抽取全量，head/tail 只做越界校验
	EXPECT_DOUBLE_EQ(times->at(1), 932.0);
	times->release();

	// end 等于 size 时守卫拒绝
	EXPECT_EQ(kdata->extractData(KFT_OPEN, 0, 3), nullptr);
	EXPECT_EQ(kdata->extractData(KFT_OPEN, 5, 6), nullptr);
	kdata->release();
}

// WTSDataDef.hpp:1518-1645：委托队列/逐笔委托/逐笔成交切片的拒绝与负索引
TEST(r53_detail_slices, reject_and_negative_index)
{
	WTSOrdQueStruct ques[2];
	ques[0].price = 10.5;
	ques[1].price = 10.6;
	EXPECT_EQ(WTSOrdQueSlice::create("SSE.600000", nullptr, 2), nullptr);
	EXPECT_EQ(WTSOrdQueSlice::create("SSE.600000", ques, 0), nullptr);
	WTSOrdQueSlice* qs = WTSOrdQueSlice::create("SSE.600000", ques, 2);
	ASSERT_NE(qs, nullptr);
	EXPECT_EQ(qs->size(), 2u);
	EXPECT_FALSE(qs->empty());
	EXPECT_DOUBLE_EQ(qs->at(1)->price, 10.6);
	EXPECT_DOUBLE_EQ(qs->at(-1)->price, 10.6);
	EXPECT_DOUBLE_EQ(qs->at(-5)->price, 10.5);	// translateIdx 夹到 0
	qs->release();

	WTSOrdDtlStruct dtls[2];
	dtls[0].price = 20.0;
	dtls[1].price = 20.5;
	EXPECT_EQ(WTSOrdDtlSlice::create("SZSE.000001", dtls, 0), nullptr);
	WTSOrdDtlSlice* ds = WTSOrdDtlSlice::create("SZSE.000001", dtls, 2);
	ASSERT_NE(ds, nullptr);
	EXPECT_FALSE(ds->empty());
	EXPECT_DOUBLE_EQ(ds->at(-1)->price, 20.5);
	EXPECT_DOUBLE_EQ(ds->at(-9)->price, 20.0);
	ds->release();

	WTSTransStruct trans[2];
	trans[0].price = 30.0;
	trans[1].price = 30.5;
	EXPECT_EQ(WTSTransSlice::create("SZSE.000001", nullptr, 2), nullptr);
	WTSTransSlice* ts = WTSTransSlice::create("SZSE.000001", trans, 2);
	ASSERT_NE(ts, nullptr);
	EXPECT_FALSE(ts->empty());
	EXPECT_DOUBLE_EQ(ts->at(0)->price, 30.0);
	EXPECT_DOUBLE_EQ(ts->at(-1)->price, 30.5);
	ts->release();
}

// WTSDataDef.hpp:1411-1489：create(ticks, 0) 的零长度块与后续 appendBlock
TEST(r53_tick_slice, zero_count_block_then_append)
{
	WTSTickStruct ticks[2];
	ticks[0] = r53_make_tick("rb2405", 3500.0);
	ticks[1] = r53_make_tick("rb2405", 3501.0);

	WTSTickSlice* slice = WTSTickSlice::create("SHFE.rb2405", ticks, 0);
	ASSERT_NE(slice, nullptr);
	EXPECT_TRUE(slice->empty());
	EXPECT_EQ(slice->size(), 0u);
	EXPECT_EQ(slice->at(0), nullptr);
	EXPECT_EQ(slice->get_block_counts(), (std::size_t)1);
	EXPECT_EQ(slice->get_block_size(0), 0u);
	EXPECT_EQ(slice->get_block_addr(0), ticks);

	EXPECT_TRUE(slice->appendBlock(ticks, 2));
	EXPECT_EQ(slice->size(), 2u);
	EXPECT_FALSE(slice->empty());
	ASSERT_NE(slice->at(0), nullptr);
	EXPECT_DOUBLE_EQ(slice->at(0)->price, 3500.0);
	EXPECT_DOUBLE_EQ(slice->at(-1)->price, 3501.0);
	EXPECT_EQ(slice->at(2), nullptr);
	slice->release();
}

// WTSDataDef.hpp:1050-1186：WTSTickRef 的合约绑定与解绑
TEST(r53_tick_ref, contract_binding_and_invalid_reference)
{
	WTSTickStruct tick = r53_make_tick("rb2405", 3500.0);
	WTSContractInfo* comm = WTSContractInfo::create("rb2405", "螺纹钢2405", "SHFE", "rb");
	ASSERT_NE(comm, nullptr);

	WTSTickRef* ref = WTSTickRef::create(&tick, comm);
	ASSERT_NE(ref, nullptr);
	EXPECT_TRUE(ref->valid());
	EXPECT_TRUE(static_cast<bool>(*ref));
	EXPECT_EQ(ref->getContractInfo(), comm);
	EXPECT_EQ(ref->getTickStruct(), &tick);
	EXPECT_STREQ(ref->code(), "rb2405");
	EXPECT_DOUBLE_EQ(ref->price(), 3500.0);
	ref->setContractInfo(nullptr);
	EXPECT_EQ(ref->getContractInfo(), nullptr);
	ref->release();

	WTSTickRef* empty = WTSTickRef::create(nullptr);
	ASSERT_NE(empty, nullptr);
	EXPECT_FALSE(empty->valid());
	EXPECT_FALSE(static_cast<bool>(*empty));
	EXPECT_EQ(empty->getTickStruct(), nullptr);
	EXPECT_EQ(empty->getContractInfo(), nullptr);
	empty->release();
	comm->release();
}
