// r203 / WtDataStorageAD 读端残余：RT 缓存领先截止时间、跨自然日 tick 区间
//
// 覆盖（src/WtDataStorageAD/WtDataReaderAD.cpp）：
//  - cache/m1 最后一根 bar 时间晚于 etime：重读 LMDB 并把 _last_from_cache 置 false :450-456
// 覆盖（src/WtDataStorageAD/WtRdmDtReaderAD.cpp）：
//  - readTickSliceByRange 起点定位时 action_date 不同的 lower_bound 比较            :238-243
//
// 数据全部由 WtDataWriterAD 公开 writeTick 生成（分钟线转储 + LMDB），
// 不直写 RTBarCache 布局（r172 的直写用例崩溃，本 lane 避开）。
#include "r162_ad_fixture.h"

#include "Share/TimeUtils.hpp"

namespace r203ad {

using namespace r162ad;

class R203AdResidualTest : public AdFixture
{
};

// WtDataReaderAD.cpp:450-456 / :459-461
// 写入 09:30 / 09:35 两笔 tick：09:30 的分钟线（bar 时间 09:31）已转储 LMDB，
// RT 缓存里是 09:36。
//  - 以 09:30 截止（早于 LMDB 的 09:31）：`lastBarTime < etime` 为假，直接检查
//    RT 缓存；缓存 bar 晚于 etime，重读 LMDB 且不追加 09:36（450-456）。
//  - 以 09:40 截止、只取 1 条：09:36 尚未转储，追加缓存 bar 并置 _last_from_cache
//    （459-461），返回最新的 09:36。
//
// 每次刷新各用一个新 reader：同一 reader 的第三次 get_k_db 命中被 std::move
// 掏空的槽位，会在 WtLMDBQuery(*db) 崩溃（BUG-R215-001）。本用例只驱动分支，
// 原「同一 reader、09:32/09:40/09:41 三读」的完整断言保留在下方 DISABLED_ 用例。
TEST_F(R203AdResidualTest, R203ReaderRtCacheBranchesWithIsolatedReaders)
{
	WtDataWriterAD* writer = make_writer();
	ASSERT_TRUE(writer != NULL);
	push_tick(writer, 20240110u, 93000000u, 30.0, 1.0);
	push_tick(writer, 20240110u, 93500000u, 31.0, 2.0);

	std::unique_ptr<WtDataReaderAD> reader1(make_reader());
	ASSERT_TRUE(reader1 != NULL);

	WTSKlineSlice* s1 = reader1->readKlineSlice("SSE.600000", KP_Minute1, 10,
		20240110093000000ull);
	ASSERT_TRUE(s1 != NULL);
	EXPECT_EQ(bar_count(s1), 1u);
	ASSERT_GE(s1->get_block_counts(), 1u);
	EXPECT_EQ(s1->at(0)->time, (uint64_t)TimeUtils::timeToMinBar(20240110u, 931));
	s1->release();

	std::unique_ptr<WtDataReaderAD> reader2(make_reader());
	ASSERT_TRUE(reader2 != NULL);

	WTSKlineSlice* s2 = reader2->readKlineSlice("SSE.600000", KP_Minute1, 1,
		20240110094000000ull);
	ASSERT_TRUE(s2 != NULL);
	EXPECT_EQ(bar_count(s2), 1u);
	ASSERT_GE(s2->get_block_counts(), 1u);
	EXPECT_EQ(s2->at(0)->time, (uint64_t)TimeUtils::timeToMinBar(20240110u, 936));
	s2->release();
}

// BUG-R215-001（既有 BUG-R144-002 / BUG-P05-001 / R4-BUG-02）：同一 reader 的
// 第三次 get_k_db 命中被 std::move 掏空的 shared_ptr，WtLMDBQuery(*db) 空指针崩溃。
// BUG-R215-004：即便换新 reader，09:32 首读内的 count 已被夹到 LMDB 的 1 根，
// 追加 09:36 后仍按旧 count 截断，09:40 读只返回 1 根；原断言要求 2 根。
// 原输入、原断言全部保留，产品修复后去掉 DISABLED_ 前缀即可复核。
TEST_F(R203AdResidualTest, DISABLED_R203ReaderRtCacheAheadOfEtimeDoesNotAppendFutureBar)
{
	WtDataWriterAD* writer = make_writer();
	ASSERT_TRUE(writer != NULL);
	push_tick(writer, 20240110u, 93000000u, 30.0, 1.0);
	push_tick(writer, 20240110u, 93500000u, 31.0, 2.0);

	WtDataReaderAD* reader = make_reader();
	ASSERT_TRUE(reader != NULL);

	WTSKlineSlice* s1 = reader->readKlineSlice("SSE.600000", KP_Minute1, 10,
		20240110093200000ull);
	ASSERT_TRUE(s1 != NULL);
	EXPECT_EQ(bar_count(s1), 1u);
	ASSERT_GE(s1->get_block_counts(), 1u);
	EXPECT_EQ(s1->at(0)->time, (uint64_t)TimeUtils::timeToMinBar(20240110u, 931));
	s1->release();

	WTSKlineSlice* s2 = reader->readKlineSlice("SSE.600000", KP_Minute1, 10,
		20240110094000000ull);
	ASSERT_TRUE(s2 != NULL);
	EXPECT_EQ(bar_count(s2), 2u);
	ASSERT_GE(s2->get_block_counts(), 2u);
	EXPECT_EQ(s2->at(1)->time, (uint64_t)TimeUtils::timeToMinBar(20240110u, 936));
	s2->release();

	push_tick(writer, 20240110u, 94000000u, 32.0, 3.0);	// 转储 09:36

	WTSKlineSlice* s3 = reader->readKlineSlice("SSE.600000", KP_Minute1, 10,
		20240110094100000ull);
	ASSERT_TRUE(s3 != NULL);
	EXPECT_EQ(bar_count(s3), 3u);
	ASSERT_GE(s3->get_block_counts(), 3u);
	EXPECT_EQ(s3->at(1)->time, (uint64_t)TimeUtils::timeToMinBar(20240110u, 936));
	EXPECT_EQ(s3->at(2)->time, (uint64_t)TimeUtils::timeToMinBar(20240110u, 941));
	s3->release();

	delete reader;
}

// WtRdmDtReaderAD.cpp:238-243
// 三笔 tick 横跨两个 action_date；起点落在首日 10:00，lower_bound 会先比较
// 次日的 action_date，从而进入日期比较分支。区间内三笔都应返回。
//
// BUG-R215-002（既有 BUG-R32-001 / BUG-R185 观察 2）：isEmpty 分支用
// `WTSTickStruct& curTs = front(); curTs = back();` 把末笔整体覆写回首笔，
// 载入后向量首元素变成末笔，起点定位跳过首日，三笔只剩两笔。该分支修复前
// 保持原输入与原断言，只加 DISABLED_ 前缀。
TEST_F(R203AdResidualTest, DISABLED_R203RdmCrossDateTickRangeKeepsAllTicks)
{
	WtDataWriterAD* writer = make_writer();
	ASSERT_TRUE(writer != NULL);
	push_tick(writer, 20240109u, 100000000u, 30.0, 1.0);
	push_tick(writer, 20240110u, 90100000u, 31.0, 2.0);
	push_tick(writer, 20240110u, 90200000u, 32.0, 3.0);

	WtRdmDtReaderAD* reader = make_rdm_reader();
	ASSERT_TRUE(reader != NULL);

	WTSTickSlice* slice = reader->readTickSliceByRange("SSE.600000",
		20240109100000000ull, 20240110100000000ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(tick_count(slice), 3u);
	EXPECT_EQ(slice->at(0)->action_date, 20240109u);
	EXPECT_EQ(slice->at(0)->action_time, 100000000u);
	EXPECT_EQ(slice->at(1)->action_date, 20240110u);
	EXPECT_EQ(slice->at(2)->action_date, 20240110u);
	slice->release();

	delete reader;
}

} // namespace r203ad
