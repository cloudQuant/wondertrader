// r112 / WtDataWriterAD：24:00 收盘时段导致分钟线跨日归位
//
// 覆盖（src/WtDataStorageAD/WtDataWriterAD.cpp）：
//  - updateBarCache 的 1 分钟线 barTime==0 -> getNextDate :691-698
//  - updateBarCache 的 5 分钟线同型分支          :793-800
//
// 手法与主存储 R94 相同：注册 21:00-24:00、offset=1440 的时段，写 23:59 的 tick；
// minuteToTime 在 24:00 收盘时归一为 0，bar 日期必须落到下一交易日。
// 只写一条 tick，不触发 pipeTo*（避免任何 LMDB 环境）。
#include "r4_ad_fixture.h"

#include "Share/TimeUtils.hpp"

namespace r112adm {

using namespace r4fx;

class R112WriterADMidnightTest : public R4WriterADTest
{
protected:
	void SetUp() override
	{
		R4WriterADTest::SetUp();
		// 换成 21:00-24:00、offset=1440 的宿主；R4BaseDataMgr 的析构会释放这些对象
		delete _sink;
		delete _bd;
		_bd = new R4BaseDataMgr("R112_MID", 2100, 2400, 1440);
		_sink = new R4WriterSink(_bd);
	}
};

// WtDataWriterAD.cpp: L691-698 / L793-800
TEST_F(R112WriterADMidnightTest, R112WriterADMidnightMinuteBarsWrapToTradingDate)
{
	WTSCommodityInfo* comm = _bd->add_commodity("EXMID", "cu", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXMID", "cu2401", comm);

	// disabletick=true：不建 tick LMDB；日/1 分/5 分缓存仍然初始化
	WTSVariant* cfg = make_writer_cfg(_root.string(), 1000, true, false, false, false);
	init_writer(cfg);

	// 夜盘 23:59 属于下一交易日 20240109
	WTSTickStruct tick = make_tick("EXMID", "cu2401", 20240109, 20240108, 235900000,
		70000.0, 5.0, 350000.0, 90000.0, 0.0);
	write_or_fail(tick_of(ct, tick), 0);

	BarCacheItem m1;
	ASSERT_TRUE(read_bar_item(path_of("cache_m1.dmb"), "EXMID", "cu2401", m1));
	EXPECT_EQ(m1._bar.date, 20240109u);
	EXPECT_EQ(TimeUtils::minBarToDate(m1._bar.time), 20240109u);
	EXPECT_EQ(TimeUtils::minBarToTime(m1._bar.time), 0u);

	BarCacheItem m5;
	ASSERT_TRUE(read_bar_item(path_of("cache_m5.dmb"), "EXMID", "cu2401", m5));
	EXPECT_EQ(m5._bar.date, 20240109u);
	EXPECT_EQ(TimeUtils::minBarToDate(m5._bar.time), 20240109u);
	EXPECT_EQ(TimeUtils::minBarToTime(m5._bar.time), 0u);

	EXPECT_EQ(_sink->bcast_tick(), 1u);
}

} // namespace r112adm
