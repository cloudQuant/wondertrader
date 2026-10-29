// r112 / WtDataReaderAD：tick 增量读取及重复读取的数据正确性
//
// 覆盖（src/WtDataStorageAD/WtDataReaderAD.cpp）：
//  - readTickSlice 的 reload_flag==1 增量读取与 reload_flag==2 全量读取 :147-182
//  - array_two 切片路径 :189-205
#include "r100_ad_fixture.h"

namespace r112adr {

using namespace r100fx;

namespace {
const uint32_t kDate = 20230103;

vector<uint32_t> tick_action_times(WTSTickSlice* slice)
{
	vector<uint32_t> times;
	if (slice == NULL)
		return times;

	for (std::size_t block_idx = 0; block_idx < slice->get_block_counts(); ++block_idx)
	{
		WTSTickStruct* block = slice->get_block_addr(block_idx);
		if (block == NULL)
			continue;

		const uint32_t block_size = slice->get_block_size(block_idx);
		for (uint32_t tick_idx = 0; tick_idx < block_size; ++tick_idx)
			times.push_back(block[tick_idx].action_time);
	}
	return times;
}

string tick_slice_layout(WTSTickSlice* slice)
{
	if (slice == NULL)
		return "null slice";

	string layout = "blocks=" + std::to_string(slice->get_block_counts()) + ", sizes=[";
	for (std::size_t block_idx = 0; block_idx < slice->get_block_counts(); ++block_idx)
	{
		if (block_idx > 0)
			layout += ", ";
		layout += std::to_string(slice->get_block_size(block_idx));
	}
	layout += "]";
	return layout;
}
}

class R112ReaderADTickTest : public R100ADReaderTest
{
};

// WtDataReaderAD.cpp: L147-182 / L189-205
TEST_F(R112ReaderADTickTest, R112ReaderADTickIncrementalAndRepeatedReadPreservesLatestData)
{
	add_stock("EXTK", "rb2301");

	vector<WTSTickStruct> ticks;
	ticks.push_back(r4fx::make_tick("EXTK", "rb2301", kDate, kDate, 93000000, 3900.0, 1, 10, 5, 0, 100, 1000));
	ticks.push_back(r4fx::make_tick("EXTK", "rb2301", kDate, kDate, 93100000, 3910.0, 1, 10, 5, 0, 110, 1100));
	ticks.push_back(r4fx::make_tick("EXTK", "rb2301", kDate, kDate, 93200000, 3920.0, 1, 10, 5, 0, 120, 1200));
	seed_ticks("EXTK", "rb2301", ticks);

	init_reader();

	// 首次请求取截至 09:32:30 的最新一笔；fixture 中三笔 tick 都早于该时间。
	WTSTickSlice* s1 = _reader->readTickSlice("EXTK.rb2301", 1, 20230103093230000ull);
	ASSERT_TRUE(s1 != NULL);
	EXPECT_EQ(s1->size(), 1u);
	EXPECT_EQ(tick_action_times(s1), (vector<uint32_t>{93200000u}));
	s1->release();

	// 扩大请求容量后全量加载，返回 fixture 中的三笔 tick。
	WTSTickSlice* s2 = _reader->readTickSlice("EXTK.rb2301", 3, 20230103093230000ull);
	ASSERT_TRUE(s2 != NULL);
	const string s2_layout = tick_slice_layout(s2);
	{
		SCOPED_TRACE("s2 " + s2_layout);
		EXPECT_EQ(s2->size(), 3u);
		EXPECT_EQ(tick_action_times(s2), (vector<uint32_t>{93000000u, 93100000u, 93200000u}));
	}
	s2->release();

	// 第三次请求时间晚于已有 tick；没有新增 fixture 数据，仍应返回缓存中的最新 tick。
	// 数据库句柄是否复用不属于此读取结果断言。
	WTSTickSlice* s3 = _reader->readTickSlice("EXTK.rb2301", 1, 20230103093300000ull);
	ASSERT_TRUE(s3 != NULL);
	{
		SCOPED_TRACE("s2 " + s2_layout + "; s3 " + tick_slice_layout(s3));
		EXPECT_EQ(s3->size(), 1u);
		EXPECT_EQ(tick_action_times(s3), (vector<uint32_t>{93200000u}));
	}
	s3->release();
}

} // namespace r112adr
