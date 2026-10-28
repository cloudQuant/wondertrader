// r112 / WtDataReaderAD：日线 onMinuteEnd 从 RTBarCache 补最后一根
//
// 覆盖（src/WtDataStorageAD/WtDataReaderAD.cpp）：
//  - get_rt_cache_bar 的日线包装选择 :307-310
//  - onMinuteEnd 日线段 :528-545（update_cache_from_lmdb 无新 bar 后，
//    RT 缓存里 0103 的未闭合日线被追加并回调）
//  - readKlineSlice 的容量不足回源与 etime 归一 :407-426
//
// 与 R100 的分钟用例分属不同 reader：日线 readKlineSlice 用掉第一次 get_k_db，
// onMinuteEnd 的 update 用第二次（move 出的有效句柄），不会再取第三次。
#include "r100_ad_fixture.h"

namespace r112add {

using namespace r100fx;

namespace {
WTSBarStruct day_bar(uint32_t date, double px)
{
	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	bar.date = date;
	bar.open = px;
	bar.high = px + 1;
	bar.low = px - 1;
	bar.close = px + 0.5;
	bar.vol = 10;
	bar.money = px * 10;
	return bar;
}
} // namespace

class R112ReaderADDayTest : public R100ADReaderTest
{
};

// WtDataReaderAD.cpp: L307-310 / L407-426 / L528-545
TEST_F(R112ReaderADDayTest, R112ReaderADDayOnMinuteEndAppendsRtBar)
{
	add_stock("EXDD", "rb2301");

	// LMDB 只有 0102 的已收盘日线
	vector<WTSBarStruct> d1;
	d1.push_back(day_bar(20230102, 4000.0));
	seed_bars("day", "EXDD", "rb2301", d1, true);

	// RTBarCache 里 writer 已把未闭合日线推进到 0103
	vector<BarCacheItem> dc;
	dc.push_back(r4fx::make_bar_item("EXDD", "rb2301", 20230103, 0, 4040.0));
	write_cache("cache_d1.dmb", dc, 1, 4);

	init_reader();

	// 首次读取只回源一次；末条 0102 不早于 etime，不会触发 update
	WTSKlineSlice* dk = _reader->readKlineSlice("EXDD.rb2301", KP_DAY, 2, 20230102100000000ull);
	ASSERT_TRUE(dk != NULL);
	ASSERT_EQ(dk->size(), 1u);
	EXPECT_EQ(dk->get_block_addr(0)[0].date, 20230102u);
	dk->release();
	EXPECT_EQ(_sink->bar_calls(), 0u);

	// 日线段：LMDB 没有 0103，RTBarCache 有 -> 追加并回调
	_reader->onMinuteEnd(20230103, 1000, 20230103);
	EXPECT_EQ(_sink->all_updated(), 1u);
	EXPECT_EQ(_sink->bar_calls(), 1u);
	EXPECT_EQ(_sink->last_period(), KP_DAY);
	EXPECT_EQ(_sink->last_bar().date, 20230103u);
	EXPECT_DOUBLE_EQ(_sink->last_bar().close, 4040.0);

	// 时间闸门：同一 nowTime 再调一次不应重复回调
	_reader->onMinuteEnd(20230103, 1000, 20230103);
	EXPECT_EQ(_sink->bar_calls(), 1u);
}

} // namespace r112add
