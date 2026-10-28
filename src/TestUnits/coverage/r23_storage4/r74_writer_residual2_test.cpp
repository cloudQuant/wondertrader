// r74 / WtDataWriter：minbar_price_mode 合并分支、空 dumper、无 tick 收盘、缓存读回一致性
//
// 覆盖（src/WtDataStorage/WtDataWriter.cpp）：
//  - pipeToKlines :1178-1182 / :1284-1288（_min_price_mode==1 在“合并已有 bar”分支写 bid/ask）
//  - dump_bars_via_dumper :1729-1801（_dumpers 中的 NULL 实现在 d1/m1/m5 三个循环里 continue）
//  - proc_loop :2413-2701 的“无 tick 合约”路径：getTickBlock/getKlineBlock(false) 缺文件返回 NULL，
//    dump_bars_via_dumper 与 dump_bars_to_file 不产生 bar 回调，逐笔仍正常落地
//  - loadCache :301-309 + updateCache :1446-1454 的缓存槽读回/覆盖（重启 writer 后 getCurTick 一致）
//
// 线程纪律沿用 r23：调过 init 的 writer 只 release 不 delete；走 transHisData 的等 marker.ini
// 后 abandon。临时目录由 r23 夹具管理。
#include "r23_writer_fixture.h"
#include "../q03_storage3/q03_storage3_fixture.h"

namespace r74 {

using namespace r23;
using std::string;
using std::vector;

class R74WriterResidualTest : public R23WriterTest
{
};

// ------------------------------------------------------------------ min_price_mode 合并分支
// WtDataWriter.cpp: pipeToKlines :1152-1188（min1 合并）与 :1258-1294（min5 合并）
TEST_F(R74WriterResidualTest, R74WriterMinPriceModeUpdatesMergedBars)
{
	R23Writer* w = make_writer("\"minbar_price_mode\":1");
	ASSERT_TRUE(w != NULL);

	TickSpec t1;
	t1.price = 3600.0;
	t1.high = 3610.0;
	t1.low = 3580.0;
	t1.total_volume = 1000.0;
	t1.volume = 10.0;
	t1.turn_over = 3600.0;
	write_tick(w, t1, 0);

	TickSpec t2;
	t2.action_time = 93030000;		// 同一分钟 / 同一 5 分钟桶
	t2.price = 3620.0;
	t2.high = 3630.0;
	t2.low = 3610.0;
	t2.total_volume = 1100.0;
	t2.volume = 20.0;
	t2.turn_over = 7200.0;
	write_tick(w, t2, 0);

	WTSBarStruct min1;
	ASSERT_TRUE((read_block_item<RTDayBlockHeader, WTSBarStruct>(rt_min1_file("rb2210"), 0, min1)));
	EXPECT_EQ(min1.time % 10000, 931u);
	EXPECT_DOUBLE_EQ(min1.close, 3620.0);
	EXPECT_DOUBLE_EQ(min1.vol, 30.0);
	// 第二笔重写挂单价（夹具：bid=price-0.5、ask=price+0.5）
	EXPECT_DOUBLE_EQ(min1.bid, 3619.5);
	EXPECT_DOUBLE_EQ(min1.ask, 3620.5);

	WTSBarStruct min5;
	ASSERT_TRUE((read_block_item<RTDayBlockHeader, WTSBarStruct>(rt_min5_file("rb2210"), 0, min5)));
	EXPECT_EQ(min5.time % 10000, 935u);
	EXPECT_DOUBLE_EQ(min5.bid, 3619.5);
	EXPECT_DOUBLE_EQ(min5.ask, 3620.5);
	EXPECT_DOUBLE_EQ(min5.close, 3620.0);
}

// ------------------------------------------------------------------ NULL dumper 三循环
// WtDataWriter.cpp: dump_bars_via_dumper :1729-1734 / :1755-1760 / :1789-1794
TEST_F(R74WriterResidualTest, R74WriterNullDumperIsSkippedInAllBarLoops)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	FakeDumper good;
	w->add_dumper("null", NULL);
	w->add_dumper("good", &good);

	TickSpec t;
	t.total_volume = 1000.0;
	t.total_turnover = 3600000.0;
	write_tick(w, t, 0);

	abandon_writer(w);
	w->transHisData("R74_NULLDUMP");
	ASSERT_TRUE(wait_marker("R74_NULLDUMP"));

	EXPECT_EQ(good.bar_count_of("d1"), 1u);
	EXPECT_EQ(good.bar_count_of("m1"), 1u);
	EXPECT_EQ(good.bar_count_of("m5"), 1u);
	EXPECT_EQ(good.tick_calls(), 1u);
}

// ------------------------------------------------------------------ 无 tick 的收盘作业
// WtDataWriter.cpp: proc_loop :2413-2489（getTickBlock(false)==NULL）与 :2690-2692
//   （dump_bars_via_dumper / dump_bars_to_file 在无 tick、无 K 线块时无输出）
TEST_F(R74WriterResidualTest, R74WriterClosingWithoutTickSkipsBarDumps)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	FakeDumper dumper;
	w->add_dumper("r74", &dumper);

	OrdQueSpec q;
	q.price = 3600.0;
	ASSERT_TRUE(w->writeOrderQueue(make_ordque_data(q)));
	ASSERT_TRUE(w->writeOrderDetail(make_orddtl_data(93000000, 3601.0, 7)));
	ASSERT_TRUE(w->writeTransaction(make_trans_data(93000000, 3602.0, 8)));

	abandon_writer(w);
	w->transHisData("R74_NOTICK");
	ASSERT_TRUE(wait_marker("R74_NOTICK"));

	// 逐笔三类都交给 dumper，bar 三段与 tick 都没有东西
	EXPECT_EQ(dumper.que_calls(), 1u);
	EXPECT_EQ(dumper.dtl_calls(), 1u);
	EXPECT_EQ(dumper.trns_calls(), 1u);
	EXPECT_EQ(dumper.tick_calls(), 0u);
	EXPECT_EQ(dumper.bar_calls(), 0u);

	// 逐笔历史文件仍然落地；tick / K 线历史文件不存在
	EXPECT_TRUE(file_exists(_base + "his/queue/SHFE/20240108/rb2210.dsb"));
	EXPECT_TRUE(file_exists(_base + "his/orders/SHFE/20240108/rb2210.dsb"));
	EXPECT_TRUE(file_exists(_base + "his/trans/SHFE/20240108/rb2210.dsb"));
	EXPECT_FALSE(file_exists(his_tick_file("rb2210", 20240108)));
	EXPECT_FALSE(file_exists(his_min1_file("rb2210")));
	EXPECT_FALSE(file_exists(his_min5_file("rb2210")));
	EXPECT_FALSE(file_exists(his_day_file("rb2210")));
}

// ------------------------------------------------------------------ 缓存读回一致性
// WtDataWriter.cpp: loadCache :301-309（既有 cache.dmb 重建索引）+ updateCache :1446-1454
TEST_F(R74WriterResidualTest, R74WriterReopenSeesCachedTickAndOverwritesSlot)
{
	R23Writer* w1 = make_writer();
	ASSERT_TRUE(w1 != NULL);

	TickSpec t1;
	t1.price = 3611.5;
	t1.total_volume = 1234.0;
	write_tick(w1, t1, 0);
	close_writer(w1);

	TickCacheItem item;
	ASSERT_TRUE((read_block_item<RTBlockHeader, TickCacheItem>(cache_file(), 0, item)));
	EXPECT_DOUBLE_EQ(item._tick.price, 3611.5);
	EXPECT_DOUBLE_EQ(item._tick.total_volume, 1234.0);

	// 重启 writer：从 cache.dmb 重建索引，getCurTick 必须读回同一条
	R23Writer* w2 = make_writer();
	ASSERT_TRUE(w2 != NULL);
	WTSTickData* cached = w2->getCurTick("rb2210", "SHFE");
	ASSERT_TRUE(cached != NULL);
	EXPECT_DOUBLE_EQ(cached->price(), 3611.5);
	cached->release();

	// 同一合约再写一笔：应覆盖 0 号槽位而不是新增
	TickSpec t2;
	t2.action_time = 93100000;
	t2.price = 3612.5;
	t2.total_volume = 1300.0;
	write_tick(w2, t2, 0);
	close_writer(w2);

	ASSERT_TRUE((read_block_item<RTBlockHeader, TickCacheItem>(cache_file(), 0, item)));
	EXPECT_DOUBLE_EQ(item._tick.price, 3612.5);
	EXPECT_DOUBLE_EQ(item._tick.total_volume, 1300.0);
	const RTBlockHeader head = read_block_header<RTBlockHeader>(cache_file());
	EXPECT_EQ(head._size, 1u);
}

} // namespace r74
