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
//    并用 r4_ad_fixture 的正常构造 writer，缺省同步模式来自公开 init 配置合同，
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
	WTSTickStruct s2 = make_tick("EXC", "rb2301", 20230104, 20230104, 93000000,
		3910.0, 10.0, 39100.0, 500.0, 0.0, 110.0, 429000.0);

	// 卡13（13a=A，用户裁定 2026-11-03）：tick 库打不开 = LMDB 提交失败 → 该 tick 不发布
	// 缓存、不广播，同步 writeTick 返回 false，失败计数可观察（旧合同"仍缓存并广播"已废止）。
	for (WTSTickStruct* s : { &s1, &s2 })
	{
		WTSTickData* t = tick_of(ct, *s);
		EXPECT_FALSE(_writer->writeTick(t, 0));
		t->release();
	}
	EXPECT_EQ(_sink->bcast_tick(), 0u);
	EXPECT_EQ(_writer->tickPersistFailures(), 2u);
	WTSTickData* cur = _writer->getCurTick("rb2301", "EXC");
	EXPECT_TRUE(cur == NULL);
	if (cur) cur->release();
	EXPECT_GE(_sink->count_containing("Opening tick db at"), 1u);

	// 日线库打开失败的日志：未发布的 tick 不会推进 K 线，因此改用不落 tick 库的 writer
	// （disabletick=true）走到换交易日的日线转储，触发 get_k_db 打开失败。
	_writer->release();
	init_writer(make_writer_cfg(_root.string(), 1000, true, false, false, false));
	write_or_fail(tick_of(ct, s1), 0);
	// 换交易日：先把 20230103 的日线转储到 day/EXC，触发 get_k_db 打开失败
	write_or_fail(tick_of(ct, s2), 0);

	EXPECT_EQ(_sink->bcast_tick(), 2u);
	EXPECT_GE(_sink->count_containing("Opening day db at"), 1u);
}

// WtDataWriterAD.cpp: L862-870
TEST_F(R122WriterADErrorsTest, R122WriterADUpdateTickCacheUninitialized)
{
	WTSCommodityInfo* comm = _bd->add_commodity("EXD", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXD", "rb2301", comm);

	_writer.reset(new_writer());
	// 只走 IDataWriter 基类的公开 init：设置 sink，不加载 cache_tick.dmb
	static_cast<IDataWriter*>(_writer.get())->IDataWriter::init(NULL, _sink);

	WTSTickStruct s = make_tick("EXD", "rb2301", 20230103, 20230103, 93000000,
		3800.0, 5.0, 19000.0, 300.0, 0.0, 50.0, 190000.0);
	WTSTickData* tick = tick_of(ct, s);
	ASSERT_TRUE(tick != NULL);
	// 卡1（DateMappingV1）：只做基类 init 的写端没有基础数据，无法核验 tick 的会话日期，
	// 在任何副作用前拒绝（改前期望：接受并在更新缓存时记录 “Tick cache data not initialized”）。
	EXPECT_FALSE(_writer->writeTick(tick, 0));
	tick->release();

	EXPECT_EQ(_sink->bcast_tick(), 0u);
	EXPECT_EQ(_sink->count_containing("Cannot verify session date"), 1u);
	EXPECT_EQ(_sink->log_count(LL_ERROR), 1u);
	// 没有缓存块，也不应该创建任何 LMDB 目录
	EXPECT_FALSE(std::filesystem::exists(_root / "ticks"));}

} // namespace r122adw
