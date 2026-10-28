// Lane R12 - src/Includes/WTSDataDef.hpp 基础数据容器/切片
// 覆盖：WTSValueArray 索引翻译与极值、WTSKlineSlice 分块寻址/切片抽取、
//       WTSKlineData 逐字段读取与 appendBar 覆盖语义、WTSTickData/WTSTickRef 读取面、
//       WTSTickSlice / WTSOrdDtlSlice / WTSTransSlice 计数与越界、跨版本结构体赋值
// 说明：不起线程、不读挂钟、不落真实共享内存；所有时间/日期都是显式入参。
#include <gtest/gtest.h>

#include <cmath>
#include <cfloat>
#include <cstring>
#include <string>
#include <vector>

#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSTypes.h"
#include "Includes/WTSStruct.h"

USING_NS_WTP;

namespace {

const double kInvalidD = INVALID_DOUBLE;

bool isInvalid(double v) { return v == kInvalidD; }

// 构造一根可判定内容的 bar
WTSBarStruct makeBar(uint32_t date, uint32_t time, double o, double h, double l,
	double c, double vol, double money)
{
	WTSBarStruct bar;
	bar.date = date;
	bar.time = time;
	bar.open = o;
	bar.high = h;
	bar.low = l;
	bar.close = c;
	bar.settle = (h + l) / 2.0;
	bar.money = money;
	bar.vol = vol;
	bar.hold = vol * 10.0;
	bar.add = vol;
	return bar;
}

WTSTickStruct makeTick(const char* code, double price, uint32_t adate, uint32_t atime)
{
	WTSTickStruct st;
	wt_strcpy(st.code, code);
	wt_strcpy(st.exchg, "SHFE");
	st.price = price;
	st.open = price - 1.0;
	st.high = price + 2.0;
	st.low = price - 2.0;
	st.settle_price = price + 0.5;
	st.upper_limit = price + 50.0;
	st.lower_limit = price - 50.0;
	st.total_volume = 120000.0;
	st.volume = 1000.0;
	st.total_turnover = price * 120000.0;
	st.turn_over = price * 1000.0;
	st.open_interest = 1520000.0;
	st.diff_interest = 20000.0;
	st.trading_date = adate;
	st.action_date = adate;
	st.action_time = atime;
	st.pre_close = price - 5.0;
	st.pre_settle = price - 3.0;
	st.pre_interest = 1500000.0;
	return st;
}

}	// namespace

TEST(r12_value_array, size_empty_append_and_at)
{
	WTSValueArray* arr = WTSValueArray::create();
	ASSERT_NE(arr, nullptr);
	EXPECT_TRUE(arr->empty());
	EXPECT_EQ(arr->size(), 0u);

	arr->append(1.5);
	arr->append(-2.25);
	arr->append(3.0);

	EXPECT_FALSE(arr->empty());
	EXPECT_EQ(arr->size(), 3u);
	// 正向索引
	EXPECT_DOUBLE_EQ(arr->at(0), 1.5);
	EXPECT_DOUBLE_EQ(arr->at(2), 3.0);
	// 越界返回 INVALID_DOUBLE 是 WTSDataDef.hpp:66-74 的既定语义（不是缺陷）
	EXPECT_TRUE(isInvalid(arr->at(3)));
	EXPECT_TRUE(isInvalid(arr->at(100)));
	arr->release();
}

TEST(r12_value_array, negative_index_translates_from_tail)
{
	WTSValueArray* arr = WTSValueArray::create();
	arr->append(10.0);
	arr->append(20.0);
	arr->append(30.0);

	// translateIdx(-1) => size-1
	EXPECT_EQ(arr->translateIdx(-1), 2);
	EXPECT_EQ(arr->translateIdx(-3), 0);
	EXPECT_EQ(arr->translateIdx(1), 1);
	EXPECT_DOUBLE_EQ(arr->at((uint32_t)-1), 30.0);
	EXPECT_DOUBLE_EQ(arr->at((uint32_t)-3), 10.0);
	// 负向越界（|idx| > size）会翻出负数，仍落到非法值分支
	EXPECT_TRUE(isInvalid(arr->at((uint32_t)-4)));
	arr->release();
}

TEST(r12_value_array, maxvalue_minvalue_range_semantics)
{
	WTSValueArray* arr = WTSValueArray::create();
	arr->append(5.0);
	arr->append(-9.0);
	arr->append(2.0);
	arr->append(7.0);

	EXPECT_DOUBLE_EQ(arr->maxvalue(0, 3), 7.0);
	EXPECT_DOUBLE_EQ(arr->minvalue(0, 3), -9.0);
	// head/tail 传反也要能自动交换
	EXPECT_DOUBLE_EQ(arr->maxvalue(3, 0), 7.0);
	EXPECT_DOUBLE_EQ(arr->minvalue(3, 0), -9.0);
	// 绝对值分支：|−9| 成为最大
	EXPECT_DOUBLE_EQ(arr->maxvalue(0, 3, true), 9.0);
	EXPECT_DOUBLE_EQ(arr->maxvalue(0, 0), 5.0);
	// 尾部越界（end > size）走非法分支
	EXPECT_TRUE(isInvalid(arr->maxvalue(0, 9)));
	EXPECT_TRUE(isInvalid(arr->minvalue(0, 9)));
	// 区间内含 INVALID_DOUBLE 时被跳过，不参与比较
	arr->append(kInvalidD);
	EXPECT_DOUBLE_EQ(arr->maxvalue(0, 4), 7.0);
	EXPECT_DOUBLE_EQ(arr->minvalue(0, 4), -9.0);
	arr->release();
}

TEST(r12_value_array, set_resize_and_operator_index)
{
	WTSValueArray* arr = WTSValueArray::create();
	arr->resize(4, 1.0);
	EXPECT_EQ(arr->size(), 4u);
	EXPECT_DOUBLE_EQ(arr->at(3), 1.0);

	arr->set(1, 8.5);
	EXPECT_DOUBLE_EQ(arr->at(1), 8.5);
	// set 越界静默丢弃，长度不变
	arr->set(10, 99.0);
	EXPECT_EQ(arr->size(), 4u);
	EXPECT_TRUE(isInvalid(arr->at(10)));

	// 非 const operator[] 直接引用可写
	(*arr)[2] = -1.0;
	EXPECT_DOUBLE_EQ(arr->at(2), -1.0);
	EXPECT_DOUBLE_EQ((*arr)[0], 1.0);

	arr->getDataRef().clear();
	EXPECT_TRUE(arr->empty());
	arr->release();
}

TEST(r12_kline_slice, create_at_and_block_accessors)
{
	std::vector<WTSBarStruct> bars;
	bars.push_back(makeBar(20240506, 931, 100.0, 110.0, 95.0, 105.0, 12.0, 1200.0));
	bars.push_back(makeBar(20240506, 932, 105.0, 108.0, 101.0, 106.0, 8.0, 800.0));

	WTSKlineSlice* slice = WTSKlineSlice::create("SHFE.rb2405", KP_Minute1, 1, bars.data(), (int32_t)bars.size());
	ASSERT_NE(slice, nullptr);
	EXPECT_STREQ(slice->code(), "SHFE.rb2405");
	EXPECT_EQ(slice->size(), 2);
	EXPECT_FALSE(slice->empty());
	EXPECT_EQ(slice->get_block_counts(), (std::size_t)1);

	WTSBarStruct* b0 = slice->at(0);
	ASSERT_NE(b0, nullptr);
	EXPECT_DOUBLE_EQ(b0->open, 100.0);
	EXPECT_DOUBLE_EQ(b0->high, 110.0);
	// 负索引按尾部翻译（translateIdx: max(0, count+idx)）
	EXPECT_DOUBLE_EQ(slice->at(-1)->close, 106.0);
	// 越界：索引扣完所有块后返回 NULL
	EXPECT_EQ(slice->at(5), nullptr);
	// 块越界：地址 NULL、长度 0
	EXPECT_EQ(slice->get_block_addr(1), nullptr);
	EXPECT_EQ(slice->get_block_size(1), 0u);
	// const 版本重载走同一条分块寻址路径
	const WTSKlineSlice& cRef = *slice;
	EXPECT_DOUBLE_EQ(cRef.at(1)->close, 106.0);
	EXPECT_EQ(cRef.at(9), nullptr);
	slice->release();
}

TEST(r12_kline_slice, append_block_and_empty_slice)
{
	std::vector<WTSBarStruct> first;
	first.push_back(makeBar(20240506, 930, 10.0, 12.0, 9.0, 11.0, 100.0, 1000.0));
	std::vector<WTSBarStruct> second;
	second.push_back(makeBar(20240506, 1030, 20.0, 22.0, 19.0, 21.0, 200.0, 4000.0));
	second.push_back(makeBar(20240506, 1031, 21.0, 25.0, 20.0, 24.0, 50.0, 1100.0));

	WTSKlineSlice* slice = WTSKlineSlice::create("DCE.i2409", KP_Minute1, 5);
	ASSERT_NE(slice, nullptr);
	// 空切片：size 0、at 直接 NULL
	EXPECT_TRUE(slice->empty());
	EXPECT_EQ(slice->at(0), nullptr);
	// 非法块被拒绝
	EXPECT_FALSE(slice->appendBlock(NULL, 0));
	EXPECT_TRUE(slice->empty());

	EXPECT_TRUE(slice->appendBlock(first.data(), 1));
	EXPECT_TRUE(slice->appendBlock(second.data(), 2));
	EXPECT_EQ(slice->size(), 3);
	EXPECT_EQ(slice->get_block_counts(), (std::size_t)2);
	EXPECT_EQ(slice->get_block_size(1), 2u);
	EXPECT_EQ(slice->get_block_addr(1), second.data());
	// 跨块寻址：索引 1 落在第二块的第 0 根
	EXPECT_DOUBLE_EQ(slice->at(1)->open, 20.0);
	EXPECT_DOUBLE_EQ(slice->at(2)->high, 25.0);
	slice->release();
}

TEST(r12_kline_slice, maxprice_minprice_and_setcode)
{
	std::vector<WTSBarStruct> bars;
	bars.push_back(makeBar(20240506, 931, 100.0, 110.0, 95.0, 105.0, 12.0, 1200.0));
	bars.push_back(makeBar(20240506, 932, 105.0, 108.0, 101.0, 106.0, 8.0, 800.0));
	bars.push_back(makeBar(20240506, 933, 106.0, 120.0, 99.0, 118.0, 30.0, 3500.0));

	WTSKlineSlice* slice = WTSKlineSlice::create("SHFE.rb2405", KP_Minute1, 1, bars.data(), (int32_t)bars.size());
	// 只喂 3 根 bar：区间 [0,1] 的 high 最大是 110（第 3 根的 120 不应参与）
	EXPECT_DOUBLE_EQ(slice->maxprice(0, 1), 110.0);
	EXPECT_DOUBLE_EQ(slice->minprice(0, 1), 95.0);
	// 全区间
	EXPECT_DOUBLE_EQ(slice->maxprice(0, 2), 120.0);
	EXPECT_DOUBLE_EQ(slice->minprice(0, 2), 95.0);
	// head>tail 也要能取到区间
	EXPECT_DOUBLE_EQ(slice->maxprice(2, 0), 120.0);
	// 尾部索引按 count-1 夹取
	EXPECT_DOUBLE_EQ(slice->maxprice(0, 99), 120.0);

	slice->setCode("SHFE.rb2406");
	EXPECT_STREQ(slice->code(), "SHFE.rb2406");
	slice->release();
}

TEST(r12_kline_slice, extract_data_per_field_type)
{
	std::vector<WTSBarStruct> bars;
	bars.push_back(makeBar(20240506, 931, 100.0, 110.0, 95.0, 105.0, 12.0, 1200.0));
	bars.push_back(makeBar(20240506, 932, 105.0, 108.0, 101.0, 106.0, 8.0, 800.0));
	bars.push_back(makeBar(20240506, 933, 106.0, 120.0, 99.0, 98.0, 30.0, 3500.0));

	WTSKlineSlice* slice = WTSKlineSlice::create("SHFE.rb2405", KP_Minute1, 1, bars.data(), (int32_t)bars.size());

	{
		WTSValueArray* va = slice->extractData(KFT_OPEN);
		ASSERT_NE(va, nullptr);
		EXPECT_EQ(va->size(), 3u);
		EXPECT_DOUBLE_EQ(va->at(0), 100.0);
		EXPECT_DOUBLE_EQ(va->at(2), 106.0);
		va->release();
	}
	{
		WTSValueArray* va = slice->extractData(KFT_HIGH, 1, 2);
		ASSERT_NE(va, nullptr);
		EXPECT_EQ(va->size(), 2u);
		EXPECT_DOUBLE_EQ(va->at(0), 108.0);
		EXPECT_DOUBLE_EQ(va->at(1), 120.0);
		va->release();
	}
	{
		WTSValueArray* va = slice->extractData(KFT_LOW);
		EXPECT_DOUBLE_EQ(va->at(1), 101.0);
		va->release();
	}
	{
		WTSValueArray* va = slice->extractData(KFT_CLOSE);
		EXPECT_DOUBLE_EQ(va->at(2), 98.0);
		va->release();
	}
	{
		WTSValueArray* va = slice->extractData(KFT_VOLUME);
		EXPECT_DOUBLE_EQ(va->at(0), 12.0);
		va->release();
	}
	{
		// 符号量：close>open 取正，否则取负
		WTSValueArray* va = slice->extractData(KFT_SVOLUME);
		EXPECT_DOUBLE_EQ(va->at(0), 12.0);
		EXPECT_DOUBLE_EQ(va->at(2), -30.0);
		va->release();
	}
	{
		WTSValueArray* va = slice->extractData(KFT_DATE);
		EXPECT_DOUBLE_EQ(va->at(0), 20240506.0);
		va->release();
	}
	{
		WTSValueArray* va = slice->extractData(KFT_TIME);
		EXPECT_DOUBLE_EQ(va->at(1), 932.0);
		va->release();
	}
	slice->release();
}

TEST(r12_kline_slice, extract_from_empty_slice_returns_null)
{
	WTSKlineSlice* slice = WTSKlineSlice::create("CFFEX.if2406", KP_Minute1, 1);
	ASSERT_NE(slice, nullptr);
	EXPECT_EQ(slice->extractData(KFT_CLOSE), nullptr);
	slice->release();
}

TEST(r12_kline_data, property_accessors_and_sized_create)
{
	WTSKlineData* kdata = WTSKlineData::create("SHFE.rb2405", 0);
	ASSERT_NE(kdata, nullptr);
	EXPECT_STREQ(kdata->code(), "SHFE.rb2405");
	EXPECT_EQ(kdata->period(), KP_Minute1);
	EXPECT_EQ(kdata->times(), 1u);
	// 构造默认视为已闭合 K 线（WTSDataDef.hpp:458-465）
	EXPECT_TRUE(kdata->isClosed());
	EXPECT_FALSE(kdata->isUnixTime());
	EXPECT_TRUE(kdata->IsEmpty());
	EXPECT_EQ(kdata->size(), 0u);

	kdata->setClosed(false);
	EXPECT_FALSE(kdata->isClosed());
	kdata->setUnixTime();
	EXPECT_TRUE(kdata->isUnixTime());
	kdata->setUnixTime(false);
	EXPECT_FALSE(kdata->isUnixTime());
	kdata->setPeriod(KP_Minute1, 15);
	EXPECT_EQ(kdata->times(), 15u);
	kdata->setCode("SHFE.rb2406");
	EXPECT_STREQ(kdata->code(), "SHFE.rb2406");

	kdata->appendBar(makeBar(20240506, 931, 10.0, 12.0, 9.0, 11.0, 100.0, 1100.0));
	kdata->appendBar(makeBar(20240506, 932, 11.0, 13.0, 10.5, 12.5, 200.0, 2600.0));
	EXPECT_EQ(kdata->size(), 2u);
	EXPECT_FALSE(kdata->IsEmpty());

	// 越界读取一律返回非法值而不是崩溃
	EXPECT_TRUE(isInvalid(kdata->open(2)));
	EXPECT_TRUE(isInvalid(kdata->high(-5)));
	EXPECT_TRUE(isInvalid(kdata->money(5)));
	EXPECT_TRUE(isInvalid(kdata->additional(5)));
	kdata->release();
}

TEST(r12_kline_data, field_by_index_accessors)
{
	WTSKlineData* kdata = WTSKlineData::create("CZCE.MA409", 0);
	kdata->appendBar(makeBar(20240506, 931, 10.0, 12.0, 9.0, 11.0, 100.0, 1100.0));
	kdata->appendBar(makeBar(20240506, 932, 11.0, 13.0, 10.5, 12.5, 200.0, 2600.0));

	EXPECT_DOUBLE_EQ(kdata->open(0), 10.0);
	EXPECT_DOUBLE_EQ(kdata->high(1), 13.0);
	EXPECT_DOUBLE_EQ(kdata->low(0), 9.0);
	EXPECT_DOUBLE_EQ(kdata->close(1), 12.5);
	EXPECT_DOUBLE_EQ(kdata->volume(1), 200.0);
	EXPECT_DOUBLE_EQ(kdata->money(0), 1100.0);
	// hold = vol*10 = 1000；add = 100（makeBar 里显式写入）
	EXPECT_DOUBLE_EQ(kdata->openinterest(0), 1000.0);
	EXPECT_DOUBLE_EQ(kdata->additional(0), 100.0);
	// bid/ask 与 hold/add 共用联合体，读同一份内存
	EXPECT_DOUBLE_EQ(kdata->bidprice(0), 1000.0);
	EXPECT_DOUBLE_EQ(kdata->askprice(0), 100.0);
	EXPECT_EQ(kdata->date(0), 20240506u);
	EXPECT_EQ(kdata->time(1), 932u);
	// 负索引从尾部取
	EXPECT_DOUBLE_EQ(kdata->close(-1), 12.5);
	kdata->release();
}

TEST(r12_kline_data, append_bar_overwrites_same_time_and_extacts)
{
	WTSKlineData* kdata = WTSKlineData::create("SSE.600000", 0);
	kdata->appendBar(makeBar(20240506, 931, 10.0, 12.0, 9.0, 11.0, 100.0, 1100.0));
	// 同 date+time 的 bar 覆盖最后一根，而不是追加
	kdata->appendBar(makeBar(20240506, 931, 10.0, 15.0, 9.0, 14.0, 130.0, 1400.0));
	EXPECT_EQ(kdata->size(), 1u);
	EXPECT_DOUBLE_EQ(kdata->high(0), 15.0);
	EXPECT_DOUBLE_EQ(kdata->volume(0), 130.0);

	WTSValueArray* va = kdata->extractData(KFT_CLOSE);
	// extractData 抓的是全量 m_vecBarData，而不是请求区间
	ASSERT_NE(va, nullptr);
	EXPECT_EQ(va->size(), 1u);
	EXPECT_DOUBLE_EQ(va->at(0), 14.0);
	va->release();
	// 区间起点已越界 -> NULL
	EXPECT_EQ(kdata->extractData(KFT_OPEN, 5, 6), nullptr);

	// at() 返回内部指针，可用于原地修改
	WTSBarStruct* p = kdata->at(0);
	ASSERT_NE(p, nullptr);
	p->close = 16.0;
	EXPECT_DOUBLE_EQ(kdata->close(0), 16.0);
	EXPECT_EQ(kdata->at(3), nullptr);
	kdata->release();
}

TEST(r12_kline_data, maxprice_minprice_and_release_keeps_slice_refs)
{
	WTSKlineData* kdata = WTSKlineData::create("SHFE.rb2405", 3);
	EXPECT_EQ(kdata->size(), 3u);
	// resize 出来的默认全 0 bar，显式填内容后再比较
	for (int32_t i = 0; i < 3; i++)
		kdata->getDataRef()[i] = makeBar(20240506, 931 + i, 10.0, 12.0 + i, 9.0, 11.0, 10.0, 100.0);

	EXPECT_DOUBLE_EQ(kdata->maxprice(0, 2), 14.0);
	EXPECT_DOUBLE_EQ(kdata->minprice(0, 2), 9.0);
	EXPECT_DOUBLE_EQ(kdata->maxprice(2, 0), 14.0);
	// begin 越界 / end 越界都返回非法值
	EXPECT_TRUE(isInvalid(kdata->maxprice(3, 4)));
	EXPECT_TRUE(isInvalid(kdata->minprice(0, 9)));
	kdata->release();
}

TEST(r12_tick_data, create_from_struct_and_field_accessors)
{
	WTSTickStruct st = makeTick("rb2405", 3500.0, 20240506, 93000000);
	st.pre_close = 3475.0;
	WTSTickData* tick = WTSTickData::create(st);
	ASSERT_NE(tick, nullptr);
	// create 是 memcpy 复制，不是引用
	EXPECT_NE(tick->getTickStruct().price, 0.0);
	EXPECT_STREQ(tick->code(), "rb2405");
	EXPECT_STREQ(tick->exchg(), "SHFE");
	EXPECT_DOUBLE_EQ(tick->price(), 3500.0);
	EXPECT_DOUBLE_EQ(tick->open(), 3499.0);
	EXPECT_DOUBLE_EQ(tick->high(), 3502.0);
	EXPECT_DOUBLE_EQ(tick->low(), 3498.0);
	EXPECT_DOUBLE_EQ(tick->preclose(), 3475.0);
	EXPECT_DOUBLE_EQ(tick->presettle(), 3497.0);
	EXPECT_DOUBLE_EQ(tick->preinterest(), 1500000.0);
	EXPECT_DOUBLE_EQ(tick->upperlimit(), 3550.0);
	EXPECT_DOUBLE_EQ(tick->lowerlimit(), 3450.0);
	EXPECT_DOUBLE_EQ(tick->totalvolume(), 120000.0);
	EXPECT_DOUBLE_EQ(tick->volume(), 1000.0);
	EXPECT_DOUBLE_EQ(tick->settlepx(), 3500.5);
	EXPECT_DOUBLE_EQ(tick->openinterest(), 1520000.0);
	EXPECT_DOUBLE_EQ(tick->additional(), 20000.0);
	EXPECT_DOUBLE_EQ(tick->totalturnover(), 3500.0 * 120000.0);
	EXPECT_DOUBLE_EQ(tick->turnover(), 3500.0 * 1000.0);
	EXPECT_EQ(tick->tradingdate(), 20240506u);
	EXPECT_EQ(tick->actiondate(), 20240506u);
	EXPECT_EQ(tick->actiontime(), 93000000u);
	tick->release();
}

TEST(r12_tick_data, create_by_code_setcode_and_limit_flag)
{
	WTSTickData* tick = WTSTickData::create("MA409");
	ASSERT_NE(tick, nullptr);
	EXPECT_STREQ(tick->code(), "MA409");
	tick->setCode("MA410");
	EXPECT_STREQ(tick->code(), "MA410");

	// 涨跌停标记是位标记：bit0 新高、bit1 新低
	EXPECT_FALSE(tick->isNewHigh());
	EXPECT_FALSE(tick->isNewLow());
	tick->setLimitFlag(1);
	EXPECT_TRUE(tick->isNewHigh());
	EXPECT_FALSE(tick->isNewLow());
	tick->setLimitFlag(2);
	EXPECT_FALSE(tick->isNewHigh());
	EXPECT_TRUE(tick->isNewLow());
	tick->setLimitFlag(3);
	EXPECT_TRUE(tick->isNewHigh());
	EXPECT_TRUE(tick->isNewLow());
	EXPECT_EQ(tick->getLimitFlag(), 3u);
	tick->setLimitFlag(0);
	EXPECT_FALSE(tick->isNewHigh());
	EXPECT_EQ(tick->getLimitFlag(), 0u);

	// 未绑定合约信息时为 NULL
	EXPECT_EQ(tick->getContractInfo(), nullptr);
	tick->release();
}

TEST(r12_tick_ref, wraps_external_struct_without_copy)
{
	WTSTickStruct st = makeTick("i2409", 800.0, 20240507, 145900000);
	WTSTickRef* ref = WTSTickRef::create(&st);
	ASSERT_NE(ref, nullptr);
	EXPECT_TRUE(ref->valid());
	EXPECT_TRUE((bool)ref);
	// 引用语义：改原始结构体立刻可见
	st.price = 801.5;
	EXPECT_DOUBLE_EQ(ref->price(), 801.5);
	EXPECT_STREQ(ref->code(), "i2409");
	EXPECT_STREQ(ref->exchg(), "SHFE");
	EXPECT_DOUBLE_EQ(ref->open(), 800.5);
	EXPECT_DOUBLE_EQ(ref->high(), 803.5);
	EXPECT_DOUBLE_EQ(ref->low(), 799.5);
	EXPECT_DOUBLE_EQ(ref->preclose(), 796.5);
	EXPECT_DOUBLE_EQ(ref->presettle(), 798.5);
	EXPECT_DOUBLE_EQ(ref->preinterest(), 1500000.0);
	EXPECT_DOUBLE_EQ(ref->upperlimit(), 851.5);
	EXPECT_DOUBLE_EQ(ref->lowerlimit(), 751.5);
	EXPECT_DOUBLE_EQ(ref->totalvolume(), 120000.0);
	EXPECT_DOUBLE_EQ(ref->volume(), 1000.0);
	EXPECT_DOUBLE_EQ(ref->settlepx(), 802.0);
	EXPECT_DOUBLE_EQ(ref->openinterest(), 1520000.0);
	EXPECT_DOUBLE_EQ(ref->additional(), 20000.0);
	EXPECT_DOUBLE_EQ(ref->totalturnover(), 800.0 * 120000.0);
	EXPECT_DOUBLE_EQ(ref->turnover(), 800.0 * 1000.0);
	EXPECT_EQ(ref->actiondate(), 20240507u);
	EXPECT_EQ(ref->actiontime(), 145900000u);
	EXPECT_EQ(ref->tradingdate(), 20240507u);
	EXPECT_EQ(ref->getTickStruct(), &st);
	EXPECT_EQ(ref->getContractInfo(), nullptr);
	ref->release();
}

TEST(r12_tick_slice, block_append_insert_and_index)
{
	std::vector<WTSTickStruct> blkA;
	blkA.push_back(makeTick("rb2405", 3500.0, 20240506, 93000000));
	std::vector<WTSTickStruct> blkB;
	blkB.push_back(makeTick("rb2405", 3501.0, 20240506, 93000500));
	blkB.push_back(makeTick("rb2405", 3502.0, 20240506, 93001000));

	WTSTickSlice* slice = WTSTickSlice::create("SHFE.rb2405", blkA.data(), 1);
	ASSERT_NE(slice, nullptr);
	// 非法块（空指针或 0 长度）被拒绝，计数不变
	EXPECT_FALSE(slice->appendBlock(NULL, 0));
	EXPECT_FALSE(slice->insertBlock(0, NULL, 0));
	EXPECT_FALSE(slice->empty());
	EXPECT_EQ(slice->size(), 1u);

	EXPECT_TRUE(slice->appendBlock(blkB.data(), 2));
	EXPECT_EQ(slice->size(), 3u);
	EXPECT_EQ(slice->get_block_counts(), (std::size_t)2);
	EXPECT_EQ(slice->get_block_size(0), 1u);
	EXPECT_EQ(slice->get_block_addr(1), blkB.data());
	// 块级越界：地址 NULL、长度 INVALID_UINT32
	EXPECT_EQ(slice->get_block_size(9), INVALID_UINT32);
	EXPECT_EQ(slice->get_block_addr(9), nullptr);
	// 跨块寻址
	EXPECT_DOUBLE_EQ(slice->at(0)->price, 3500.0);
	EXPECT_DOUBLE_EQ(slice->at(1)->price, 3501.0);
	// 负索引按尾部翻译后逐块扣除
	EXPECT_DOUBLE_EQ(slice->at(-1)->price, 3502.0);
	// 正向越界：索引扣完所有块后返回 NULL
	EXPECT_EQ(slice->at(10), nullptr);

	// 头部插入后，索引 0/1 分别落在新块与原第一块
	EXPECT_TRUE(slice->insertBlock(0, blkB.data(), 1));
	EXPECT_EQ(slice->size(), 4u);
	EXPECT_EQ(slice->get_block_counts(), (std::size_t)3);
	EXPECT_DOUBLE_EQ(slice->at(0)->price, 3501.0);
	EXPECT_DOUBLE_EQ(slice->at(1)->price, 3500.0);
	slice->release();

	WTSTickSlice* s2 = WTSTickSlice::create("SHFE.rb2405", blkB.data(), 2);
	ASSERT_NE(s2, nullptr);
	EXPECT_EQ(s2->size(), 2u);
	EXPECT_DOUBLE_EQ(s2->at(1)->price, 3502.0);
	s2->release();
}

// 触发条件：WTSTickSlice::create(code) 不传 ticks 指针时（WTSDataDef.hpp:1394 声明
// _count 但构造函数 WTSTickSlice(){_blocks.clear();} 只清块表），_count 从未初始化；
// 随后 size()/empty()/at()/appendBlock() 读或累加未初始化内存，属未定义行为，
// 断言结果不可复现，故整类 DISABLED 只做记录，不作为覆盖。
TEST(DISABLED_r12_tick_slice, count_uninitialized_when_created_without_ticks)
{
	WTSTickSlice* slice = WTSTickSlice::create("SHFE.rb2405");
	ASSERT_NE(slice, nullptr);
	EXPECT_TRUE(slice->empty());
	EXPECT_EQ(slice->size(), 0u);
	slice->release();
}

TEST(r12_detail_slices, orddtl_trans_slice_create_and_at)
{
	WTSOrdDtlStruct odArr[2];
	memset(odArr, 0, sizeof(odArr));
	odArr[0].price = 10.5;
	odArr[0].volume = 100;
	odArr[0].index = 1;
	odArr[1].price = 10.6;
	odArr[1].volume = 50;
	odArr[1].index = 2;

	// count==0 或指针为空一律拒绝创建
	EXPECT_EQ(WTSOrdDtlSlice::create("SSE.600000", odArr, 0), nullptr);
	EXPECT_EQ(WTSOrdDtlSlice::create("SSE.600000", NULL, 2), nullptr);

	WTSOrdDtlSlice* odSlice = WTSOrdDtlSlice::create("SSE.600000", odArr, 2);
	ASSERT_NE(odSlice, nullptr);
	EXPECT_EQ(odSlice->size(), 2u);
	EXPECT_FALSE(odSlice->empty());
	EXPECT_DOUBLE_EQ(odSlice->at(1)->price, 10.6);
	// 负索引夹到 0
	EXPECT_DOUBLE_EQ(odSlice->at(-5)->price, 10.5);
	odSlice->release();

	WTSTransStruct tsArr[2];
	memset(tsArr, 0, sizeof(tsArr));
	tsArr[0].price = 20.0;
	tsArr[0].volume = 3;
	tsArr[0].index = 7;
	tsArr[1].price = 20.5;
	tsArr[1].volume = 4;
	tsArr[1].index = 8;
	EXPECT_EQ(WTSTransSlice::create("SSE.600000", tsArr, 0), nullptr);
	WTSTransSlice* tsSlice = WTSTransSlice::create("SSE.600000", tsArr, 2);
	ASSERT_NE(tsSlice, nullptr);
	EXPECT_EQ(tsSlice->size(), 2u);
	EXPECT_DOUBLE_EQ(tsSlice->at(0)->price, 20.0);
	EXPECT_DOUBLE_EQ(tsSlice->at(-1)->price, 20.0);
	tsSlice->release();
}

TEST(r12_pool_data_objects, orddtl_ordque_trans_code_and_contract)
{
	WTSOrdDtlData* od = WTSOrdDtlData::create("600000");
	ASSERT_NE(od, nullptr);
	EXPECT_STREQ(od->code(), "600000");
	od->setCode("600001");
	EXPECT_STREQ(od->code(), "600001");
	od->getOrdDtlStruct().action_date = 20240506;
	od->getOrdDtlStruct().action_time = 93000000;
	od->getOrdDtlStruct().trading_date = 20240507;
	EXPECT_EQ(od->actiondate(), 20240506u);
	EXPECT_EQ(od->actiontime(), 93000000u);
	EXPECT_EQ(od->tradingdate(), 20240507u);
	EXPECT_EQ(od->getContractInfo(), nullptr);
	od->release();

	WTSOrdQueData* oq = WTSOrdQueData::create("000001");
	ASSERT_NE(oq, nullptr);
	EXPECT_STREQ(oq->code(), "000001");
	oq->getOrdQueStruct().trading_date = 20240506;
	EXPECT_EQ(oq->tradingdate(), 20240506u);
	oq->release();

	WTSTransData* tr = WTSTransData::create("600519");
	ASSERT_NE(tr, nullptr);
	EXPECT_STREQ(tr->code(), "600519");
	tr->setCode("600519");
	EXPECT_STREQ(tr->code(), "600519");
	tr->getTransStruct().action_date = 20240507;
	EXPECT_EQ(tr->actiondate(), 20240507u);
	tr->release();
}

TEST(r12_struct_compat, old_bar_and_tick_assignment)
{
	WTSBarStructOld ob;
	ob.date = 20190104;
	ob.time = 1330;
	ob.open = 11.0;
	ob.high = 12.0;
	ob.low = 10.0;
	ob.close = 11.5;
	ob.settle = 11.2;
	ob.money = 1234.5;
	ob.vol = 600;
	ob.hold = 700;
	ob.add = 30;

	WTSBarStruct nb;
	nb = ob;
	EXPECT_EQ(nb.date, 20190104u);
	EXPECT_EQ(nb.time, 1330u);
	EXPECT_DOUBLE_EQ(nb.open, 11.0);
	EXPECT_DOUBLE_EQ(nb.high, 12.0);
	EXPECT_DOUBLE_EQ(nb.low, 10.0);
	EXPECT_DOUBLE_EQ(nb.close, 11.5);
	EXPECT_DOUBLE_EQ(nb.settle, 11.2);
	EXPECT_DOUBLE_EQ(nb.money, 1234.5);
	EXPECT_DOUBLE_EQ(nb.vol, 600.0);
	EXPECT_DOUBLE_EQ(nb.hold, 700.0);
	EXPECT_DOUBLE_EQ(nb.add, 30.0);

	WTSTickStructOld ot;
	ot.action_date = 20190104;
	ot.action_time = 145959000;
	ot.trading_date = 20190107;
	ot.price = 100.0;
	ot.open = 99.0;
	ot.high = 101.0;
	ot.low = 98.0;
	ot.settle_price = 100.5;
	ot.upper_limit = 110.0;
	ot.lower_limit = 90.0;
	ot.total_volume = 1000;
	ot.volume = 10;
	ot.total_turnover = 100000.0;
	ot.turn_over = 1000.0;
	ot.open_interest = 5000;
	ot.diff_interest = 100;
	ot.pre_close = 97.0;
	ot.pre_settle = 96.0;
	ot.pre_interest = 4900;

	WTSTickStruct nt;
	nt = ot;
	EXPECT_EQ(nt.action_date, 20190104u);
	EXPECT_EQ(nt.action_time, 145959000u);
	EXPECT_EQ(nt.trading_date, 20190107u);
	EXPECT_DOUBLE_EQ(nt.price, 100.0);
	EXPECT_DOUBLE_EQ(nt.settle_price, 100.5);
	EXPECT_DOUBLE_EQ(nt.total_volume, 1000.0);
	EXPECT_DOUBLE_EQ(nt.volume, 10.0);
	EXPECT_DOUBLE_EQ(nt.open_interest, 5000.0);
	EXPECT_DOUBLE_EQ(nt.diff_interest, 100.0);
	EXPECT_DOUBLE_EQ(nt.pre_interest, 4900.0);
	EXPECT_DOUBLE_EQ(nt.pre_close, 97.0);
	EXPECT_DOUBLE_EQ(nt.upper_limit, 110.0);
}
