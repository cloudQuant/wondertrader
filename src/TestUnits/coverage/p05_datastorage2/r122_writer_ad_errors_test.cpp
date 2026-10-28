// R122 / WtDataWriterAD：LMDB 打开失败的错误日志，以及 tick 缓存未初始化早退。
//
// 覆盖（src/WtDataStorageAD/WtDataWriterAD.cpp）：
//  - get_k_db 打开失败 :1046-1049（日线块转储时）
//  - get_t_db 打开失败 :1066-1069（每笔 tick 落库时）
//  - updateTickCache 的 _tick_cache_block==NULL 早退 :865-868
//
// 失败注入与线程纪律：
//  - 用同名目录顶掉 data.mdb，mdb_env_open 稳定返回失败，不依赖文件权限；
//  - updateTickCache 用例只调用 IDataWriter 基类公开 init 注入 sink，不加载缓存块，
//    并用 r4_ad_fixture 的 new_writer_zeroed 压住 _async_task 残值（R4-BUG-01），
//    因此不会启动任务线程。
#include "r4_ad_fixture.h"

namespace r122adw {

using namespace r4fx;

class R122WriterADErrorsTest : public R4WriterADTest
{
};

// WtDataWriterAD.cpp: L1046-1049 / L1066-1069
TEST_F(R122WriterADErrorsTest, R122WriterADDbOpenFailuresLogged)
{
	WTSCommodityInfo* comm = _bd->add_commodity("EXC", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXC", "rb2301", comm);

	// 只破坏 day 与 ticks 两个库；min1/min5 保持可写，保证流程能走到日线转储
	std::error_code ec;
	std::filesystem::create_directories(_root / "day" / "EXC" / "data.mdb", ec);
	ASSERT_FALSE(ec) << ec.message();
	std::filesystem::create_directories(_root / "ticks" / "EXC" / "rb2301" / "data.mdb", ec);
	ASSERT_FALSE(ec) << ec.message();

	init_writer(make_writer_cfg(_root.string(), 1000, false, false, false, false));

	WTSTickStruct s1 = make_tick("EXC", "rb2301", 20230103, 20230103, 93000000,
		3900.0, 10.0, 39000.0, 500.0, 0.0, 100.0, 390000.0);
	write_or_fail(tick_of(ct, s1), 0);

	// 换交易日：先把 20230103 的日线转储到 day/EXC，触发 get_k_db 打开失败
	WTSTickStruct s2 = make_tick("EXC", "rb2301", 20230104, 20230104, 93000000,
		3910.0, 10.0, 39100.0, 500.0, 0.0, 110.0, 429000.0);
	write_or_fail(tick_of(ct, s2), 0);

	EXPECT_EQ(_sink->bcast_tick(), 2u);
	EXPECT_GE(_sink->count_containing("Opening tick db at"), 1u);
	EXPECT_GE(_sink->count_containing("Opening day db at"), 1u);
}

// WtDataWriterAD.cpp: L862-870
TEST_F(R122WriterADErrorsTest, R122WriterADUpdateTickCacheUninitialized)
{
	WTSCommodityInfo* comm = _bd->add_commodity("EXD", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXD", "rb2301", comm);

	_writer.reset(new_writer_zeroed());
	// 只走 IDataWriter 基类的公开 init：设置 sink，不加载 cache_tick.dmb
	static_cast<IDataWriter*>(_writer.get())->IDataWriter::init(NULL, _sink);

	WTSTickStruct s = make_tick("EXD", "rb2301", 20230103, 20230103, 93000000,
		3800.0, 5.0, 19000.0, 300.0, 0.0, 50.0, 190000.0);
	WTSTickData* tick = tick_of(ct, s);
	ASSERT_TRUE(tick != NULL);
	EXPECT_TRUE(_writer->writeTick(tick, 0));
	tick->release();

	EXPECT_EQ(_sink->bcast_tick(), 0u);
	EXPECT_EQ(_sink->count_containing("Tick cache data not initialized"), 1u);
	EXPECT_EQ(_sink->log_count(LL_ERROR), 1u);
	// 没有缓存块，也不应该创建任何 LMDB 目录
	EXPECT_FALSE(std::filesystem::exists(_root / "ticks"));}

} // namespace r122adw
