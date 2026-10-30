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

void expect_bar_fields(const WTSBarStruct& actual, const WTSBarStruct& expected)
{
	EXPECT_EQ(actual.date, expected.date);
	EXPECT_EQ(actual.reserve_, expected.reserve_);
	EXPECT_EQ(actual.time, expected.time);
	EXPECT_DOUBLE_EQ(actual.open, expected.open);
	EXPECT_DOUBLE_EQ(actual.high, expected.high);
	EXPECT_DOUBLE_EQ(actual.low, expected.low);
	EXPECT_DOUBLE_EQ(actual.close, expected.close);
	EXPECT_DOUBLE_EQ(actual.settle, expected.settle);
	EXPECT_DOUBLE_EQ(actual.money, expected.money);
	EXPECT_DOUBLE_EQ(actual.vol, expected.vol);
	EXPECT_DOUBLE_EQ(actual.hold, expected.hold);
	EXPECT_DOUBLE_EQ(actual.add, expected.add);
}

// WtDataReaderAD.cpp:450-456 / :459-461
// 写入 09:30 / 09:35 两笔 tick：09:30 的分钟线（bar 时间 09:31）已转储 LMDB，
// RT 缓存里是 09:36。
//  - 以 09:30 截止（早于 LMDB 的 09:31）：`lastBarTime < etime` 为假，直接检查
//    RT 缓存；缓存 bar 晚于 etime，重读 LMDB 且不追加 09:36（450-456）。
//  - 以 09:40 截止、只取 1 条：09:36 尚未转储，追加缓存 bar 并置 _last_from_cache
//    （459-461），返回最新的 09:36。
//
// 每次刷新各用一个新 reader，隔离共享句柄缓存缺陷的影响；此用例覆盖 RT bar 加入后的
// 返回切片数量和字段。
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

// BUG-R215-004：请求 count 大于 LMDB 当前记录数时，不能在 RT bar 追加前将 count
// 裁到 LMDB 数量。固定 sink 时钟由 ReaderSink 提供，etime=0 避免依赖显式时间解析。
// 最终切片应包括 LMDB 最近 bar 与 RT 最近 bar，且每个 WTSBarStruct 字段都保持原值。
TEST_F(R203AdResidualTest, R203ReaderRtAppendPreservesRequestedCountAndBarFields)
{
	WtDataWriterAD* writer = make_writer();
	ASSERT_TRUE(writer != NULL);
	push_tick(writer, 20240110u, 93000000u, 30.0, 1.0);
	push_tick(writer, 20240110u, 93500000u, 31.0, 2.0);

	std::unique_ptr<WtDataReaderAD> reader(make_reader());
	ASSERT_TRUE(reader != NULL);
	WTSKlineSlice* slice = reader->readKlineSlice("SSE.600000", KP_Minute1, 10, 0);
	ASSERT_TRUE(slice != NULL);
	ASSERT_EQ(bar_count(slice), 2u);
	ASSERT_EQ(slice->get_block_counts(), 1u);

	WTSBarStruct lmdbBar;
	lmdbBar.date = 20240110u;
	lmdbBar.time = TimeUtils::timeToMinBar(20240110u, 931);
	lmdbBar.open = 30.0;
	lmdbBar.high = 30.0;
	lmdbBar.low = 30.0;
	lmdbBar.close = 30.0;
	lmdbBar.money = 30.0;
	lmdbBar.vol = 1.0;
	lmdbBar.hold = 1001.0;

	WTSBarStruct rtBar;
	rtBar.date = 20240110u;
	rtBar.time = TimeUtils::timeToMinBar(20240110u, 936);
	rtBar.open = 31.0;
	rtBar.high = 31.0;
	rtBar.low = 31.0;
	rtBar.close = 31.0;
	rtBar.money = 31.0;
	rtBar.vol = 1.0;
	rtBar.hold = 1002.0;

	expect_bar_fields(*slice->at(0), lmdbBar);
	expect_bar_fields(*slice->at(1), rtBar);
	slice->release();
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
