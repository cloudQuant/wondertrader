// r94 / WtRdmDtReader：按条数读 tick 时，历史段的空块与「早于首条」边界
//
// 覆盖（src/WtDataStorage/WtRdmDtReader.cpp）：
//  - readTickSliceByCount 历史段 `tcnt <= 0 -> break` :2573-2575（header-only 文件）
//  - readTickSliceByCount 历史段 `eIdx == 0 -> break` :2584-2588（etime 早于块内首条）
//
// etime 用当前交易日（RT 文件缺失，直接 break），nowTDate 从 0109 往前回退：
// 0109 是只有 12 字节头的空块（tcnt==0）；0108 的唯一一条 tick 在 16:00，
// 大于该日收盘边界 15:00:59.999，lower_bound 命中首条后 `>=` 回退遇 eIdx==0。
// init(NULL) 不起巡检线程；临时目录由 q03::Sandbox 管理。
#include "r74_rdm_fixture.h"

#include "WtDataStorage/WtRdmDtReader.h"

namespace r94 {

using namespace q03;
using std::string;
using std::vector;

namespace {

const uint32_t kCurTDate = 20240110;

} // namespace

class R94RdmTickCountEdgeTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r94tcnt"));
		_bd.set_cur_tdate(kCurTDate);
		_reader.init(NULL, &_sink);
	}

	void TearDown() override
	{
		_reader.clearCache();
		_sandbox.reset();
	}

	std::unique_ptr<Sandbox>	_sandbox;
	BaseDataMgr			_bd;
	HotMgr				_hot;
	RdmSink				_sink{ &_bd, &_hot };
	WtRdmDtReader			_reader;
};

// WtRdmDtReader.cpp: L2573-2575 / L2584-2588
TEST_F(R94RdmTickCountEdgeTest, R94RdmCountSkipsEmptyAndBeforeFirstHisTick)
{
	// 0109：只有块头的空 tick 文件
	ASSERT_TRUE(write_bytes("his/ticks/SSE/20240109/600000.dsb",
		string(sizeof(HisTickBlock), '\0')));

	// 0108：唯一一条 tick 的 action_time 晚于收盘边界
	vector<WTSTickStruct> ticks;
	ticks.push_back(make_tick(20240108, 20240108, 160000000, 1));
	ASSERT_TRUE(write_bytes("his/ticks/SSE/20240108/600000.dsb",
		make_block(BT_HIS_Ticks, BLOCK_VERSION_RAW_V2, pack_ticks(ticks))));

	WTSTickSlice* s = _reader.readTickSliceByCount("SSE.STK.600000", 2,
		20240110150000000ull);
	ASSERT_TRUE(s != NULL);
	EXPECT_EQ(s->size(), 0u);
	s->release();
}

} // namespace r94
