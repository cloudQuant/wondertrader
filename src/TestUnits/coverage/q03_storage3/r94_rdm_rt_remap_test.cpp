// r94 / WtRdmDtReader：RT 块容量被写侧扩大后，读侧五类块都要重新映射
//
// 覆盖（src/WtDataStorage/WtRdmDtReader.cpp）：
//  - getRTTickBlock    remap 分支 :2024-2036
//  - getRTOrdDtlBlock  remap 分支 :2064-2076
//  - getRTOrdQueBlock  remap 分支 :2104-2116
//  - getRTTransBlock   remap 分支 :2144-2156
//  - getRTKilneBlock   remap 分支 :2201-2213
//
// 手法：先按 capacity=8 的 RT 块读一遍（建立 _last_cap），用 r52::patch_rt_capacity
// 原地把头部 capacity 改成 12，再读一遍触发 `_last_cap != _capacity` 的重映射。
// init(NULL) 不起巡检线程；全部路径在 q03::Sandbox 临时目录内。
#include "r74_rdm_fixture.h"

#include "WtDataStorage/WtRdmDtReader.h"

namespace r94 {

using namespace q03;
using std::string;
using std::vector;

namespace {

const uint32_t kCurTDate = 20240110;

} // namespace

class R94RdmRtRemapTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r94remap"));
		_bd.set_cur_tdate(kCurTDate);
		_reader.init(NULL, &_sink);

		vector<WTSTickStruct> ticks;
		ticks.push_back(make_tick(kCurTDate, kCurTDate, 93000000, 1));
		ticks.push_back(make_tick(kCurTDate, kCurTDate, 93100000, 2));
		ASSERT_TRUE(write_bytes("rt/ticks/SSE/600000.dmb",
			make_rt_block<WTSTickStruct>(BT_RT_Ticks, kCurTDate, 2, 8, ticks)));

		vector<WTSOrdQueStruct> ques;
		ques.push_back(make_ordque(kCurTDate, kCurTDate, 93000000, 1));
		ques.push_back(make_ordque(kCurTDate, kCurTDate, 93100000, 2));
		ASSERT_TRUE(write_bytes("rt/queue/SSE/600000.dmb",
			make_rt_block<WTSOrdQueStruct>(BT_RT_OrdQueue, kCurTDate, 2, 8, ques)));

		vector<WTSOrdDtlStruct> dtls;
		dtls.push_back(make_orddtl(kCurTDate, kCurTDate, 93000000, 3));
		dtls.push_back(make_orddtl(kCurTDate, kCurTDate, 93100000, 4));
		ASSERT_TRUE(write_bytes("rt/orders/SSE/600000.dmb",
			make_rt_block<WTSOrdDtlStruct>(BT_RT_OrdDetail, kCurTDate, 2, 8, dtls)));

		vector<WTSTransStruct> trs;
		trs.push_back(make_trans(kCurTDate, kCurTDate, 93000000, 5));
		trs.push_back(make_trans(kCurTDate, kCurTDate, 93100000, 6));
		ASSERT_TRUE(write_bytes("rt/trans/SSE/600000.dmb",
			make_rt_block<WTSTransStruct>(BT_RT_Trnsctn, kCurTDate, 2, 8, trs)));

		vector<WTSBarStruct> bars;
		bars.push_back(make_bar(kCurTDate, r52::min_bar_time(kCurTDate, 930), 1));
		bars.push_back(make_bar(kCurTDate, r52::min_bar_time(kCurTDate, 931), 2));
		ASSERT_TRUE(write_bytes("rt/min1/SSE/600000.dmb",
			make_rt_block<WTSBarStruct>(BT_RT_Minute1, kCurTDate, 2, 8, bars)));
	}

	void TearDown() override
	{
		_reader.clearCache();
		_sandbox.reset();
	}

	void read_all()
	{
		WTSTickSlice* ts = _reader.readTickSliceByDate("SSE.STK.600000", kCurTDate);
		ASSERT_TRUE(ts != NULL);
		EXPECT_EQ(r52::flatten_ticks(ts).size(), (std::size_t)2);
		ts->release();

		WTSOrdQueSlice* q = _reader.readOrdQueSliceByRange("SSE.STK.600000",
			20240110093000000ull, 20240110093200000ull);
		ASSERT_TRUE(q != NULL);
		EXPECT_EQ(q->size(), 2u);
		q->release();

		WTSOrdDtlSlice* d = _reader.readOrdDtlSliceByRange("SSE.STK.600000",
			20240110093000000ull, 20240110093200000ull);
		ASSERT_TRUE(d != NULL);
		EXPECT_EQ(d->size(), 2u);
		d->release();

		WTSTransSlice* t = _reader.readTransSliceByRange("SSE.STK.600000",
			20240110093000000ull, 20240110093200000ull);
		ASSERT_TRUE(t != NULL);
		EXPECT_EQ(t->size(), 2u);
		t->release();

		WTSKlineSlice* k = _reader.readKlineSliceByRange("SSE.STK.600000", KP_Minute1,
			202401100930ull, 202401101500ull);
		ASSERT_TRUE(k != NULL);
		EXPECT_EQ(r52::flatten_bars(k).size(), (std::size_t)2);
		k->release();
	}

	std::unique_ptr<Sandbox>	_sandbox;
	BaseDataMgr			_bd;
	HotMgr				_hot;
	RdmSink				_sink{ &_bd, &_hot };
	WtRdmDtReader			_reader;
};

// WtRdmDtReader.cpp: L2024-2036 / L2064-2076 / L2104-2116 / L2144-2156 / L2201-2213
TEST_F(R94RdmRtRemapTest, R94RdmAllRtBlocksRemapAfterCapacityGrowth)
{
	read_all();

	ASSERT_TRUE(r52::patch_rt_capacity("rt/ticks/SSE/600000.dmb", 12));
	ASSERT_TRUE(r52::patch_rt_capacity("rt/queue/SSE/600000.dmb", 12));
	ASSERT_TRUE(r52::patch_rt_capacity("rt/orders/SSE/600000.dmb", 12));
	ASSERT_TRUE(r52::patch_rt_capacity("rt/trans/SSE/600000.dmb", 12));
	ASSERT_TRUE(r52::patch_rt_capacity("rt/min1/SSE/600000.dmb", 12));

	read_all();
}

} // namespace r94
