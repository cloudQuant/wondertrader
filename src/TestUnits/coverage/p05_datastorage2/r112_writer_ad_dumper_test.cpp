// r112 / WtDataWriterAD：扩展 dumper 失败分支 + 日/分钟线零价合并
//
// 覆盖（src/WtDataStorageAD/WtDataWriterAD.cpp）：
//  - pipeToDayBars 扩展 dumper 返回 false 的日志 :475-479
//  - pipeToM1Bars  扩展 dumper 返回 false 的日志 :508-512（对应零价合并 :743-749）
//  - pipeToM5Bars  扩展 dumper 返回 false 的日志 :541-545
//  - 日线同交易日合并时 open/low 为 0 的补价 :638-644
//  - updateBarCache 的日线/1 分钟线新建与合并主链 :569-758
//
// 写入器由 r4 夹具的 new_writer_zeroed() 构造，_async_task 残值被压成 0（R4-BUG-01）；
// 所有数据只落 temp_directory_path()/r4_wad_*。
#include "r4_ad_fixture.h"

namespace r112adw {

using namespace r4fx;

namespace {

const uint32_t kD1 = 20240103;
const uint32_t kD2 = 20240104;

class R112FailingDumper : public IHisDataDumper
{
public:
	bool dumpHisBars(const char*, const char*, WTSBarStruct*, uint32_t) override
	{
		_bar_calls++;
		return false;
	}

	bool dumpHisTicks(const char*, uint32_t, WTSTickStruct*, uint32_t) override
	{
		_tick_calls++;
		return false;
	}

	uint32_t bar_calls() const { return _bar_calls; }
	uint32_t tick_calls() const { return _tick_calls; }

private:
	uint32_t _bar_calls = 0;
	uint32_t _tick_calls = 0;
};

} // namespace

class R112WriterADDumperTest : public R4WriterADTest
{
};

// WtDataWriterAD.cpp: L475-479 / L508-512 / L541-545 / L638-644
TEST_F(R112WriterADDumperTest, R112WriterADFailingDumperAndZeroPriceMerge)
{
	WTSVariant* cfg = make_writer_cfg(_root.string(), 1000, false, false, false, false);
	init_writer(cfg);

	WTSCommodityInfo* comm = _bd->add_commodity("EXW1", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXW1", "rb2301", comm);

	R112FailingDumper dumper;
	writer()->add_ext_dumper("bad", &dumper);

	// t1：09:30 价格为 0，建立 open/low 均为 0 的日线与分钟线
	WTSTickStruct t1 = make_tick("EXW1", "rb2301", kD1, kD1, 93000000, 0.0, 10.0, 0.0, 100.0, 0.0);
	write_or_fail(tick_of(ct, t1), 0);

	// t2：同一分钟，价格恢复；日线与 m1 都走 open/low==0 的补价分支
	WTSTickStruct t2 = make_tick("EXW1", "rb2301", kD1, kD1, 93030000, 3900.0, 5.0, 500.0, 100.0, 0.0, 15.0, 500.0);
	write_or_fail(tick_of(ct, t2), 0);

	// t3：09:31 闭合 09:30 的 m1 bar -> pipeToM1Bars 失败日志
	WTSTickStruct t3 = make_tick("EXW1", "rb2301", kD1, kD1, 93100000, 3910.0, 5.0, 500.0, 100.0, 0.0, 20.0, 1000.0);
	write_or_fail(tick_of(ct, t3), 0);

	// t4：下一交易日，闭合 d1/m1/m5 三条 bar -> 三个 pipeTo*Bars 的失败日志
	WTSTickStruct t4 = make_tick("EXW1", "rb2301", kD2, kD2, 93000000, 3920.0, 5.0, 500.0, 100.0, 0.0, 25.0, 1500.0);
	write_or_fail(tick_of(ct, t4), 0);

	EXPECT_EQ(_sink->bcast_tick(), 4u);
	// d1 + m1 + m5 各一次；m1 在 t3/t4 各闭合一次
	EXPECT_GE(dumper.bar_calls(), 4u);
	EXPECT_EQ(dumper.tick_calls(), 4u);
	EXPECT_GE(_sink->count_containing("via extended dumper"), 4u);
	EXPECT_GE(_sink->log_count(LL_ERROR), 4u);

	// 日线缓存最后一条应是 0104，且开放价来自 t4
	BarCacheItem item;
	ASSERT_TRUE(read_bar_item(path_of("cache_d1.dmb"), "EXW1", "rb2301", item));
	EXPECT_EQ(item._bar.date, kD2);

	BarCacheItem m1;
	ASSERT_TRUE(read_bar_item(path_of("cache_m1.dmb"), "EXW1", "rb2301", m1));
	EXPECT_EQ(m1._bar.date, kD2);
}

} // namespace r112adw
