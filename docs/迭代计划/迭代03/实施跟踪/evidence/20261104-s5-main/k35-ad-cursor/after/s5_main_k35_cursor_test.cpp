// 迭代03 第五会话主线程：决策 K35=C（R21-BUG-07）——WtRdmDtReaderAD K 线缓存的末 bar 游标 + 有界陈旧。
//
//  - 有提交后只读取游标（末 bar 键，含）之后的记录并合并：新 bar 与末 bar 的同键修订立即可见；
//  - 更早 bar 的同键修订在下一次全量校验时可见：请求终点交易日变化、增量次数达到
//    full_check_increments 或距上次全量超过 full_check_seconds；
//  - 合并整体替换缓冲区，已借出的切片保持原内容（卡11）。
// 改前：任何提交都会全量重扫（“Reading back” 每次一行，无增量路径）。
#include "r21_ad_fixture.h"

#include <chrono>
#include <thread>

namespace {

const char* const K35_EXCHG = "XEX";
const char* const K35_CODE = "rb2409";
const char* const K35_STD = "XEX.rb.rb2409";

std::vector<WTSBarStruct> k35_day_bars()
{
	std::vector<WTSBarStruct> bars;
	for (uint32_t i = 0; i < 4; i++)
		bars.push_back(r21_make_bar(20240102 + i, 0, 100.0 + i));	// close = base + 1
	return bars;
}

class K35Reader
{
public:
	K35Reader(const std::filesystem::path& root, R21RdmSink* sink, int increments, int seconds)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("path", root.string().c_str());
		if (increments >= 0)
			cfg->append("full_check_increments", (uint32_t)increments);
		if (seconds >= 0)
			cfg->append("full_check_seconds", (uint32_t)seconds);
		_reader.init(cfg, sink);
		cfg->release();
	}
	WTSKlineSlice* day(uint64_t from, uint64_t to) { return _reader.readKlineSliceByRange(K35_STD, KP_DAY, from, to); }
	WTSKlineSlice* min1(uint64_t from, uint64_t to) { return _reader.readKlineSliceByRange(K35_STD, KP_Minute1, from, to); }

private:
	WtRdmDtReaderAD _reader;
};

double k35_close_of(WTSKlineSlice* slice, uint32_t date)
{
	for (int32_t i = 0; i < (int32_t)slice->get_block_size(0); i++)
		if (slice->get_block_addr(0)[i].date == date)
			return slice->get_block_addr(0)[i].close;
	return -1.0;
}

} // namespace

TEST_F(R21ReaderADTest, K35AppendAndLastBarRevisionAreReadFromTheCursor)
{
	K35Reader reader(_root, sink(), -1, -1);
	put_bar_db("day", K35_EXCHG, K35_CODE, k35_day_bars(), true);
	WTSKlineSlice* first = reader.day(202401020930ull, 202401101500ull);
	ASSERT_TRUE(first != NULL);
	ASSERT_EQ(first->get_block_size(0), 4u);

	// 新 bar + 末 bar（游标）的同键修订
	std::vector<WTSBarStruct> update{ r21_make_bar(20240105, 0, 700.0), r21_make_bar(20240108, 0, 200.0) };
	put_bar_db("day", K35_EXCHG, K35_CODE, update, true);
	WTSKlineSlice* second = reader.day(202401020930ull, 202401101500ull);
	ASSERT_TRUE(second != NULL);
	ASSERT_EQ(second->get_block_size(0), 5u);
	EXPECT_NEAR(k35_close_of(second, 20240105), 701.0, 1e-9) << "a revision of the cursor bar is visible at once";
	EXPECT_NEAR(k35_close_of(second, 20240108), 201.0, 1e-9);
	EXPECT_NEAR(k35_close_of(second, 20240102), 101.0, 1e-9);
	// 卡11：先借出的切片保持原内容
	ASSERT_EQ(first->get_block_size(0), 4u);
	EXPECT_NEAR(k35_close_of(first, 20240105), 104.0, 1e-9);
	first->release();
	second->release();

	EXPECT_EQ(sink()->count_containing("Reading back"), 1u) << "only the first read scans everything";
	EXPECT_EQ(sink()->count_containing("Reading newer"), 1u);
}

TEST_F(R21ReaderADTest, K35OlderRevisionBecomesVisibleAfterTheIncrementBound)
{
	K35Reader reader(_root, sink(), 2, 0);	// 2 次增量后全量校验，不按时间
	put_bar_db("day", K35_EXCHG, K35_CODE, k35_day_bars(), true);
	WTSKlineSlice* s0 = reader.day(202401020930ull, 202401201500ull);
	ASSERT_TRUE(s0 != NULL);
	s0->release();

	put_bar_db("day", K35_EXCHG, K35_CODE, std::vector<WTSBarStruct>{ r21_make_bar(20240102, 0, 500.0) }, true);
	WTSKlineSlice* s1 = reader.day(202401020930ull, 202401201500ull);	// 增量 1
	ASSERT_TRUE(s1 != NULL);
	EXPECT_NEAR(k35_close_of(s1, 20240102), 101.0, 1e-9) << "an older revision stays stale until a full check";
	s1->release();

	put_bar_db("day", K35_EXCHG, K35_CODE, std::vector<WTSBarStruct>{ r21_make_bar(20240108, 0, 200.0) }, true);
	WTSKlineSlice* s2 = reader.day(202401020930ull, 202401201500ull);	// 增量 2
	ASSERT_TRUE(s2 != NULL);
	EXPECT_NEAR(k35_close_of(s2, 20240102), 101.0, 1e-9);
	EXPECT_NEAR(k35_close_of(s2, 20240108), 201.0, 1e-9);
	s2->release();

	put_bar_db("day", K35_EXCHG, K35_CODE, std::vector<WTSBarStruct>{ r21_make_bar(20240109, 0, 300.0) }, true);
	WTSKlineSlice* s3 = reader.day(202401020930ull, 202401201500ull);	// 达到上限 → 全量
	ASSERT_TRUE(s3 != NULL);
	EXPECT_NEAR(k35_close_of(s3, 20240102), 501.0, 1e-9) << "the full check after the bound shows the revision";
	EXPECT_EQ(s3->get_block_size(0), 6u);
	s3->release();
	EXPECT_EQ(sink()->count_containing("Reading back"), 2u);
	EXPECT_EQ(sink()->count_containing("Reading newer"), 2u);
}

TEST_F(R21ReaderADTest, K35OlderRevisionBecomesVisibleAfterTheTimeBound)
{
	K35Reader reader(_root, sink(), 0, 1);	// 不按次数，1 秒后全量校验
	put_bar_db("day", K35_EXCHG, K35_CODE, k35_day_bars(), true);
	WTSKlineSlice* s0 = reader.day(202401020930ull, 202401201500ull);
	ASSERT_TRUE(s0 != NULL);
	s0->release();

	put_bar_db("day", K35_EXCHG, K35_CODE, std::vector<WTSBarStruct>{ r21_make_bar(20240102, 0, 500.0) }, true);
	WTSKlineSlice* s1 = reader.day(202401020930ull, 202401201500ull);
	ASSERT_TRUE(s1 != NULL);
	EXPECT_NEAR(k35_close_of(s1, 20240102), 101.0, 1e-9);
	s1->release();

	std::this_thread::sleep_for(std::chrono::milliseconds(1100));
	put_bar_db("day", K35_EXCHG, K35_CODE, std::vector<WTSBarStruct>{ r21_make_bar(20240108, 0, 200.0) }, true);
	WTSKlineSlice* s2 = reader.day(202401020930ull, 202401201500ull);
	ASSERT_TRUE(s2 != NULL);
	EXPECT_NEAR(k35_close_of(s2, 20240102), 501.0, 1e-9);
	s2->release();
	EXPECT_EQ(sink()->count_containing("Reading back"), 2u);
}

TEST_F(R21ReaderADTest, K35MinuteBarsMergeFromTheCursor)
{
	K35Reader reader(_root, sink(), -1, -1);
	std::vector<WTSBarStruct> bars{
		r21_make_bar(20240102, r21_min_bar_time(20240102, 930), 10.0),
		r21_make_bar(20240102, r21_min_bar_time(20240102, 931), 11.0) };
	put_bar_db("min1", K35_EXCHG, K35_CODE, bars, false);
	WTSKlineSlice* s0 = reader.min1(202401020900ull, 202401021500ull);
	ASSERT_TRUE(s0 != NULL);
	ASSERT_EQ(s0->get_block_size(0), 2u);
	s0->release();

	std::vector<WTSBarStruct> update{
		r21_make_bar(20240102, r21_min_bar_time(20240102, 931), 21.0),
		r21_make_bar(20240102, r21_min_bar_time(20240102, 932), 12.0) };
	put_bar_db("min1", K35_EXCHG, K35_CODE, update, false);
	WTSKlineSlice* s1 = reader.min1(202401020900ull, 202401021500ull);
	ASSERT_TRUE(s1 != NULL);
	ASSERT_EQ(s1->get_block_size(0), 3u);
	EXPECT_EQ(s1->get_block_addr(0)[0].time, r21_min_bar_time(20240102, 930));
	EXPECT_NEAR(s1->get_block_addr(0)[1].close, 22.0, 1e-9);
	EXPECT_EQ(s1->get_block_addr(0)[2].time, r21_min_bar_time(20240102, 932));
	s1->release();
	EXPECT_EQ(sink()->count_containing("Reading newer"), 1u);
}
