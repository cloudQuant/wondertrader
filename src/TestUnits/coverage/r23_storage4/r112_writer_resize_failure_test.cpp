// r112 / WtDataWriter：RT tick 块扩容失败的可恢复路径
//
// 覆盖（src/WtDataStorage/WtDataWriter.cpp）：
//  - resizeRTBlock 的 BoostFile 写失败 catch/错误日志/返回 NULL  :329-340
//  - pipeToTicks  扩容返回 NULL 后的 blk==NULL 守卫与 DEBUG 日志 :632-646
//
// 手法：先把 rt/ticks/SHFE/rb2210.dmb 写成满容量（2500 条，初始 HFT_SIZE_STEP），
// 再把该路径替换成同名目录 —— 已有 mmap 仍然有效，但 resizeRTBlock 里的
// BoostFile::open_existing_file 会拿到 EISDIR 并抛异常，从而稳定命中 catch。
// 写入器沿用 r23 纪律：init 后只 release 不 delete（check_loop 不可 join）。
#include "r23_writer_fixture.h"

namespace r112w {

using namespace r23;
using std::string;

class R112WriterResizeFailureTest : public R23WriterTest
{
};

// WtDataWriter.cpp: resizeRTBlock :329-340 / pipeToTicks :632-646
TEST_F(R112WriterResizeFailureTest, R112WriterTickBlockExpansionFailureIsLogged)
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

	// HFT_SIZE_STEP == 2500（WtDataWriter.cpp:60）：写满整块，仍不触发扩容
	for (uint32_t i = 0; i < 2500; i++)
	{
		spec.action_time = 90000000u + i * 1000u;
		spec.total_volume = 1000.0 + i;
		spec.volume = 10.0;
		write_tick(w, spec, 0);
	}

	const string tick_file = rt_tick_file("rb2210");
	ASSERT_TRUE(file_exists(tick_file));
	RTDayBlockHeader head = read_block_header<RTDayBlockHeader>(tick_file);
	ASSERT_EQ(head._size, 2500u);
	ASSERT_EQ(head._capacity, 2500u);

	// 用同名目录替换块文件：mmap 不受影响，但下一次扩容的 open 会失败
	std::error_code ec;
	ASSERT_TRUE(std::filesystem::remove(tick_file, ec)) << ec.message();
	ASSERT_TRUE(std::filesystem::create_directory(tick_file, ec)) << ec.message();

	// 第 2501 条：blk->_size >= blk->_capacity -> resizeRTBlock 打开目录失败
	spec.action_time = 90000000u + 2500u * 1000u;
	spec.total_volume = 1000.0 + 2500u;
	write_tick(w, spec, 0);

	EXPECT_TRUE(_sink->has_log("Exception occured while expanding RT cache file", LL_ERROR));
	EXPECT_GE(_sink->count_of(LL_ERROR), 1u);
	EXPECT_GE(_sink->ticks(), 2501u);
}

} // namespace r112w
