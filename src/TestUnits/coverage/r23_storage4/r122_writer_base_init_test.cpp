// R122 / WtDataWriter：已完成正常生命周期初始化后，隔离 tick cache 指针为空，
// 验证同步 writeTick 命中「tick 缓存未初始化」的早退分支并写出 ERROR 日志。
#include "../p10_storage_writer/p10_writer_prefix.h"
#include "r23_writer_fixture.h"

namespace {

class R122WriterBaseInitTest : public r23::R23WriterTest
{
};

TEST_F(R122WriterBaseInitTest, R122WriterTickCacheUninitializedIsLogged)
{
	r23::R23Writer* writer = make_writer();
	ASSERT_NE(writer, nullptr);

	WTSTickStruct ts;
	memset(&ts, 0, sizeof(ts));
	snprintf(ts.exchg, sizeof(ts.exchg), "%s", "SHFE");
	snprintf(ts.code, sizeof(ts.code), "%s", "rb2210");
	ts.trading_date = 20240108;
	ts.action_date = 20240108;
	ts.action_time = 93000000;
	ts.price = 3600.0;
	ts.open = 3600.0;
	ts.high = 3600.0;
	ts.low = 3600.0;
	ts.total_volume = 100.0;
	ts.volume = 1.0;
	ts.total_turnover = 360000.0;
	ts.turn_over = 3600.0;

	WTSTickData* tick = WTSTickData::create(ts);
	ASSERT_NE(tick, nullptr);
	tick->setContractInfo(_bd->getContract("rb2210", "SHFE"));

	// init() starts the periodic check_loop. Stop and join it to rule out any
	// background access while the test temporarily injects a null cache pointer.
	ASSERT_NE(writer->_proc_chk, nullptr);
	ASSERT_TRUE(writer->_proc_chk->joinable());
	writer->_terminated.store(true);
	writer->_check_cond.notify_all();
	writer->_proc_chk->join();
	ASSERT_EQ(writer->_proc_thrd, nullptr);

	// The fixture uses async=false, so writeTick reaches updateCache synchronously.
	RTTickCache* cacheBlock = nullptr;
	{
		SpinLock lock(writer->_lck_tick_cache);
		cacheBlock = writer->_tick_cache_block;
		ASSERT_NE(cacheBlock, nullptr);
		writer->_tick_cache_block = nullptr;
	}

	EXPECT_TRUE(writer->writeTick(tick, 0));
	EXPECT_EQ(_sink->ticks(), 0u);
	EXPECT_EQ(_sink->count_of(LL_ERROR), 1u);
	EXPECT_TRUE(_sink->has_log("Tick cache data not initialized", LL_ERROR));

	{
		SpinLock lock(writer->_lck_tick_cache);
		writer->_tick_cache_block = cacheBlock;
	}
	tick->release();

	// NULL 入参的公开守卫
	EXPECT_FALSE(writer->writeTick(NULL, 0));
	EXPECT_FALSE(writer->writeOrderQueue(NULL));
	EXPECT_FALSE(writer->writeOrderDetail(NULL));
	EXPECT_FALSE(writer->writeTransaction(NULL));

	close_writer(writer);
}

} // namespace
