// r112 / WtDataReaderAD：LMDB 增量补线、环形缓冲 array_two 直取、RT 缓存未命中
//
// 覆盖（src/WtDataStorageAD/WtDataReaderAD.cpp）：
//  - readKlineSlice 的容量不足回源 :407-413
//  - 上次请求时间前进 -> update_cache_from_lmdb 追加并回调 :427-466；:290-297
//  - 末条不早于 etime 时不回源
//  - 环形缓冲写满后 array_two 足够，直接切片 :472-480
//  - get_rt_cache_bar 的索引未命中返回 NULL :357-365（缓存文件不存在）
//
// 每个 (exchg,周期) 在该 reader 里只取两次库：第一次 open，第二次 move 出有效句柄；
// 第三次会拿到空句柄（R4-BUG-02），所以本用例不触发第三次。
#include "r100_ad_fixture.h"

namespace r112adk {

using namespace r100fx;

namespace {
const uint32_t kDate = 20230103;

WTSBarStruct bar_at(uint32_t date, uint32_t hhmm, double px)
{
	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	bar.date = date;
	bar.time = (uint64_t)(date - 19900000) * 10000 + hhmm;
	bar.open = px;
	bar.high = px + 1;
	bar.low = px - 1;
	bar.close = px + 0.5;
	bar.vol = 10;
	bar.money = px * 10;
	return bar;
}
} // namespace

class R112ReaderADKlineTest : public R100ADReaderTest
{
};

// WtDataReaderAD.cpp: L290-297 / L407-413 / L427-466 / L472-480
TEST_F(R112ReaderADKlineTest, R112ReaderADKlineUpdateThenArrayTwoTail)
{
	add_stock("EXKU", "rb2301");

	// LMDB 三条 1 分钟线；缓存容量按第一次请求的 count=2 预分配
	vector<WTSBarStruct> bars;
	bars.push_back(bar_at(kDate, 931, 3900.0));
	bars.push_back(bar_at(kDate, 932, 3910.0));
	bars.push_back(bar_at(kDate, 933, 3920.0));
	seed_bars("min1", "EXKU", "rb2301", bars, false);

	init_reader();

	// 第一次：容量 0<2 -> 回源 get_lowers 取 09:31/09:32
	WTSKlineSlice* s1 = _reader->readKlineSlice("EXKU.rb2301", KP_Minute1, 2, 20230103093200000ull);
	ASSERT_TRUE(s1 != NULL);
	ASSERT_EQ(s1->size(), 2u);
	EXPECT_EQ(s1->get_block_addr(0)[0].time, (uint64_t)3301030931ull);
	EXPECT_EQ(s1->get_block_addr(0)[1].time, (uint64_t)3301030932ull);
	s1->release();

	// 第二次：容量 2>=1，截止 09:33 -> update_cache_from_lmdb 追加 09:33 并回调；
	// 环形缓冲 capacity=2 已被第三条写满，array_two 直接给出 1 条
	WTSKlineSlice* s2 = _reader->readKlineSlice("EXKU.rb2301", KP_Minute1, 1, 20230103093300000ull);
	ASSERT_TRUE(s2 != NULL);
	ASSERT_EQ(s2->size(), 1u);
	EXPECT_EQ(s2->get_block_addr(0)[0].time, (uint64_t)3301030933ull);
	s2->release();
	EXPECT_EQ(_sink->bar_calls(), 1u);
	EXPECT_EQ(_sink->last_bar().time, (uint64_t)3301030933ull);

	// 第三个合约：LMDB 有 1 条，但没有 cache_m1.dmb 条目 -> get_rt_cache_bar 未命中
	add_stock("EXKV", "rb2301");
	vector<WTSBarStruct> bars2;
	bars2.push_back(bar_at(kDate, 931, 4900.0));
	seed_bars("min1", "EXKV", "rb2301", bars2, false);

	// 新 reader 避免复用 EXKU 的 get_k_db 状态
	_reader.reset();
	init_reader();

	// 截止 09:35 晚于 LMDB 末条，第一次回源后仍不足 -> 去 RT 缓存取；
	// cache_m1.dmb 不存在 -> wrapper 索引为空，get_rt_cache_bar 返回 NULL
	WTSKlineSlice* s3 = _reader->readKlineSlice("EXKV.rb2301", KP_Minute1, 2, 20230103093500000ull);
	ASSERT_TRUE(s3 != NULL);
	ASSERT_EQ(s3->size(), 1u);
	EXPECT_EQ(s3->get_block_addr(0)[0].time, (uint64_t)3301030931ull);
	s3->release();
}

} // namespace r112adk
