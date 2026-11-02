// r212 / WtDataStorage：WtDataWriter 残余分支（同步 async=false 公开入口）
//
// 覆盖（src/WtDataStorage/WtDataWriter.cpp）：
//  - resizeRTBlock 检测物理文件短于旧映射声明容量并 fail closed
//  - pipeToTicks 扩容失败后安全返回，不写入旧映射容量以外
//  - pipeToKlines 同一分钟内的 low==0 与 low!=0 两条低点更新分支：
//    min1 :1164-1167，min5 :1270-1273
//  - procQueue/procOrder/procTrans 的 canSessionReceive 拒绝守卫：441-442/495-496/549-550
//
// 手法：
//  * 扩容失败时将 rt tick 块文件截断为普通零长度文件。resizeRTBlock 应在
//    扩展或映射前拒绝短文件并保留旧映射。
//    专门的 remap-failure 和旧映射保留/重试由 WriterResizeAfterExtendTest 覆盖。
//  * 低点分支先写 low=0 的 tick 建 bar，同分钟再写一笔 low>0 的 tick；
//    下一分钟再重复一次，覆盖 bar 尚未闭合时的两个 else 分支。
//  * 拒绝分支把 sink 的 canSessionReceive 置 false，三类逐笔都应不进块文件。
// 全部走 r23 夹具的公开 API；writer 只 release 不 delete（check_loop 不可 join）。
#include "r23_writer_fixture.h"

namespace r212w {

using namespace r23;
using std::string;

class R212WriterTailTest : public R23WriterTest
{
};

// WtDataWriter.cpp: resizeRTBlock / pipeToTicks
TEST_F(R212WriterTailTest, R212WriterTickResizeRejectsTruncatedFile)
{
	R23Writer* w = make_writer("\"groupsize\":100000");
	ASSERT_TRUE(w != NULL);

	TickSpec spec;
	spec.code = "rb2210";
	spec.exchg = "SHFE";
	spec.trading_date = 20240108;
	spec.action_date = 20240108;
	spec.price = 3600.0;
	spec.open = 3600.0;
	spec.high = 3600.0;
	spec.low = 3600.0;

	// HFT_SIZE_STEP == 2500（WtDataWriter.cpp:60），写满整块但不触发扩容
	for (uint32_t i = 0; i < 2500; i++)
	{
		spec.action_time = 90000000u + i * 1000u;
		spec.total_volume = 1000.0 + i;
		write_tick(w, spec, 0);
	}

	const string tick_file = rt_tick_file("rb2210");
	ASSERT_TRUE(file_exists(tick_file));
	RTDayBlockHeader head = read_block_header<RTDayBlockHeader>(tick_file);
	ASSERT_EQ(head._size, 2500u);
	ASSERT_EQ(head._capacity, 2500u);

	// 截断为普通零长度文件：长度小于旧映射声明的容量，触发短文件保护。
	std::error_code ec;
	ASSERT_TRUE(std::filesystem::remove(tick_file, ec)) << ec.message();
	ASSERT_FALSE(ec) << ec.message();
	{
		std::ofstream truncated(tick_file, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(truncated.is_open());
	}
	ASSERT_TRUE(std::filesystem::is_regular_file(tick_file));
	ASSERT_EQ(std::filesystem::file_size(tick_file), 0u);

	spec.action_time = 90000000u + 2500u * 1000u;
	spec.total_volume = 1000.0 + 2500u;
	write_tick(w, spec, 0);

	EXPECT_TRUE(_sink->has_log("is shorter than its declared capacity", LL_ERROR));
	EXPECT_TRUE(_sink->has_log("could not be expanded", LL_ERROR));
	EXPECT_GE(_sink->count_of(LL_ERROR), 1u);
	EXPECT_GE(_sink->ticks(), 2501u);
}

// WtDataWriter.cpp:1164-1167 / 1270-1273
TEST_F(R212WriterTailTest, R212WriterKlineZeroLowThenNonZeroLowMerge)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	TickSpec spec;
	spec.code = "rb2210";
	spec.exchg = "SHFE";
	spec.trading_date = 20240108;
	spec.action_date = 20240108;

	// 09:30 第一根：price=0 使 isNewLow=false 时 bar.low 也留下 0
	spec.action_time = 93000000u;
	spec.price = 0.0;
	spec.low = 0.0;
	spec.total_volume = 100.0;
	spec.volume = 1.0;
	write_tick(w, spec, 0);

	// 09:30:30 同一分钟：low==0 分支
	spec.action_time = 93030000u;
	spec.price = 11.0;
	spec.low = 5.0;
	spec.total_volume = 101.0;
	write_tick(w, spec, 0);

	// 09:36 新一分钟/新五分钟 bar：低点非 0
	spec.action_time = 93600000u;
	spec.price = 12.0;
	spec.low = 6.0;
	spec.total_volume = 102.0;
	write_tick(w, spec, 0);

	// 09:36:30 同一分钟：low!=0 分支
	spec.action_time = 93630000u;
	spec.price = 13.0;
	spec.low = 7.0;
	spec.total_volume = 103.0;
	write_tick(w, spec, 0);

	RTDayBlockHeader h1 = read_block_header<RTKlineBlock>(rt_min1_file("rb2210"));
	ASSERT_EQ(h1._size, 2u);
	WTSBarStruct bar;
	ASSERT_TRUE((read_block_item<RTKlineBlock, WTSBarStruct>(rt_min1_file("rb2210"), 0, bar)));
	EXPECT_DOUBLE_EQ(bar.low, 11.0);
	EXPECT_DOUBLE_EQ(bar.close, 11.0);
	ASSERT_TRUE((read_block_item<RTKlineBlock, WTSBarStruct>(rt_min1_file("rb2210"), 1, bar)));
	EXPECT_DOUBLE_EQ(bar.low, 12.0);
	EXPECT_DOUBLE_EQ(bar.close, 13.0);

	RTDayBlockHeader h5 = read_block_header<RTKlineBlock>(rt_min5_file("rb2210"));
	ASSERT_EQ(h5._size, 2u);
}

// WtDataWriter.cpp:441-442 / 495-496 / 549-550
TEST_F(R212WriterTailTest, R212WriterSessionRejectionSkipsAllIntradayBlocks)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	_sink->set_can_receive(false);

	TickSpec spec;
	spec.code = "rb2210";
	spec.exchg = "SHFE";
	spec.trading_date = 20240108;
	spec.action_date = 20240108;
	spec.action_time = 93000000u;
	write_tick(w, spec, 0);

	OrdQueSpec q;
	ASSERT_TRUE(w->writeOrderQueue(make_ordque_data(q)));
	ASSERT_TRUE(w->writeOrderDetail(make_orddtl_data(93000000u, 3600.0, 1)));
	ASSERT_TRUE(w->writeTransaction(make_trans_data(93000000u, 3601.0, 2)));

	EXPECT_EQ(_sink->ticks(), 0u);
	EXPECT_EQ(_sink->queues(), 0u);
	EXPECT_EQ(_sink->dtls(), 0u);
	EXPECT_EQ(_sink->trans(), 0u);
	EXPECT_FALSE(file_exists(rt_tick_file("rb2210")));
	EXPECT_FALSE(file_exists(path_of("rt/queue/SHFE/rb2210.dmb")));
	EXPECT_FALSE(file_exists(path_of("rt/orders/SHFE/rb2210.dmb")));
	EXPECT_FALSE(file_exists(path_of("rt/trans/SHFE/rb2210.dmb")));
}

} // namespace r212w
