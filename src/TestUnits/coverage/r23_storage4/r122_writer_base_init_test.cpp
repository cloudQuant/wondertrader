// R122 / WtDataWriter：只初始化基类 sink（不进入 init 的 check_loop 线程）后，
// 同步 writeTick 命中「tick 缓存未初始化」的早退分支。
//
// 覆盖（src/WtDataStorage/WtDataWriter.cpp）：
//  - updateCache 的 _tick_cache_block==NULL 早退与 ERROR 日志 :1441-1444
//
// 线程纪律：WtDataWriter::init 无条件启动 check_loop（10s 起睡，不可 join），
// 因此这里用 IDataWriter 基类的公开 init 显式限定调用，只注入 _sink；
// 不启动任何线程，也不触碰 300s 清理/墙钟。_async_proc/_tick_cache_block 在构造函数里
// 未初始化（正常 init 才赋值），故把对象存储清零后再 placement new，保证走到同步分支。
#include "r23_writer_fixture.h"

#include <new>
#include <unistd.h>

namespace {

class R122WriterBaseInitTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		static uint32_t seq = 0;
		seq++;
		_dir = std::filesystem::temp_directory_path()
			/ fmtutil::format("r122_w_{}_{}", (uint32_t)::getpid(), seq);
		std::error_code ec;
		std::filesystem::create_directories(_dir, ec);
		ASSERT_FALSE(ec) << ec.message();

		_backup = std::filesystem::current_path(ec);
		ASSERT_FALSE(ec) << ec.message();
		ASSERT_EQ(::chdir(_dir.string().c_str()), 0);

		_bd.reset(new r23::WriterBDMgr());
		_sink.reset(new r23::WriterSink(_bd.get()));
	}

	void TearDown() override
	{
		std::error_code ec;
		::chdir(_backup.string().c_str());
		_bd.reset();
		_sink.reset();
		std::filesystem::remove_all(_dir, ec);
	}

	std::unique_ptr<r23::WriterBDMgr>	_bd;
	std::unique_ptr<r23::WriterSink>	_sink;
	std::filesystem::path	_dir;
	std::filesystem::path	_backup;
};

// WtDataWriter.cpp: L1438-1445
TEST_F(R122WriterBaseInitTest, R122WriterTickCacheUninitializedIsLogged)
{
	// 把对象存储清零后再 placement new，压住 _async_proc/_tick_cache_block 的
	// 构造函数未初始化残值（正常 init 会赋值，本用例刻意绕开派生 init）
	alignas(WtDataWriter) unsigned char storage[sizeof(WtDataWriter)];
	memset(storage, 0, sizeof(storage));
	WtDataWriter* writer = new (storage) WtDataWriter();
	static_cast<IDataWriter*>(writer)->IDataWriter::init(NULL, _sink.get());

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
	tick->setContractInfo(_bd->getContract("rb2210", "SHFE"));
	ASSERT_TRUE(tick != NULL);

	EXPECT_TRUE(writer->writeTick(tick, 0));
	EXPECT_EQ(_sink->ticks(), 0u);
	EXPECT_EQ(_sink->count_of(LL_ERROR), 1u);
	EXPECT_TRUE(_sink->has_log("Tick cache data not initialized", LL_ERROR));
	tick->release();

	// NULL 入参的公开守卫
	EXPECT_FALSE(writer->writeTick(NULL, 0));
	EXPECT_FALSE(writer->writeOrderQueue(NULL));
	EXPECT_FALSE(writer->writeOrderDetail(NULL));
	EXPECT_FALSE(writer->writeTransaction(NULL));

	writer->release();
	writer->~WtDataWriter();
}

} // namespace
