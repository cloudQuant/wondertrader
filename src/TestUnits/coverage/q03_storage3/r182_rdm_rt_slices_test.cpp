// r182 / WtRdmDtReader：RT 分钟线块内的区间/条数裁剪
//
// 覆盖（src/WtDataStorage/WtRdmDtReader.cpp）：
//  - readKlineSliceByRange：起始时间落在 RT 块内部时走第二次 lower_bound :1895-1905
//  - readKlineSliceByCount：按条数从 RT 块尾部截取                    :2296-2313
//
// 全部走公开虚接口；RT 块按 DataDefine.h 的 RTDayBlockHeader 布局落盘，
// init(NULL) 不启动 5 秒巡检线程，临时目录由 q03::Sandbox 清理。
#include "r74_rdm_fixture.h"

#include "WtDataStorage/WtRdmDtReader.h"

namespace r182 {

using namespace q03;
using std::vector;

namespace {

const uint32_t kDate = 20240110;

} // namespace

class R182RdmRtSlicesTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r182rt"));
		_bd.set_cur_tdate(kDate);
		_reader.init(NULL, &_sink);

		vector<WTSBarStruct> bars;
		bars.push_back(make_bar(kDate, r52::min_bar_time(kDate, 930), 1));
		bars.push_back(make_bar(kDate, r52::min_bar_time(kDate, 931), 2));
		bars.push_back(make_bar(kDate, r52::min_bar_time(kDate, 932), 3));
		ASSERT_TRUE(write_bytes("rt/min1/SSE/600000.dmb",
			make_rt_block<WTSBarStruct>(BT_RT_Minute1, kDate, 3, 8, bars)));
	}

	void TearDown() override
	{
		_reader.clearCache();
		_sandbox.reset();
	}

	std::unique_ptr<Sandbox>	_sandbox;
	BaseDataMgr			_bd;
	HotMgr				_hot;
	RdmSink				_sink{&_bd, &_hot};
	WtRdmDtReader			_reader;
};

// WtRdmDtReader.cpp:1888 false 臂、1895-1905；[09:31,09:32] 起点在 RT 块内部
TEST_F(R182RdmRtSlicesTest, R182Min1RangeStartsInsideRtBlock)
{
	WTSKlineSlice* slice = _reader.readKlineSliceByRange("SSE.STK.600000", KP_Minute1,
		(uint64_t)kDate * 10000ull + 931ull, (uint64_t)kDate * 10000ull + 932ull);
	ASSERT_TRUE(slice != NULL);
	ASSERT_EQ(slice->size(), 2u);
	EXPECT_EQ(slice->at(0)->time, r52::min_bar_time(kDate, 931));
	EXPECT_EQ(slice->at(1)->time, r52::min_bar_time(kDate, 932));
	EXPECT_DOUBLE_EQ(slice->at(0)->open, 11.0);	// make_bar(seq=2) -> 10.0+2
	slice->release();
}

// WtRdmDtReader.cpp:2296-2313；按条数从 RT 块尾部取 2 根
TEST_F(R182RdmRtSlicesTest, R182Min1CountTakesTailOfRtBlock)
{
	WTSKlineSlice* slice = _reader.readKlineSliceByCount("SSE.STK.600000", KP_Minute1, 2,
		(uint64_t)kDate * 10000ull + 932ull);
	ASSERT_TRUE(slice != NULL);
	ASSERT_EQ(slice->size(), 2u);
	EXPECT_EQ(slice->at(0)->time, r52::min_bar_time(kDate, 931));
	EXPECT_EQ(slice->at(1)->time, r52::min_bar_time(kDate, 932));
	slice->release();
}

} // namespace r182
