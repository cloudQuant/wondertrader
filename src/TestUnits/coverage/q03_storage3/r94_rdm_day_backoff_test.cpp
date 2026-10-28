// r94 / WtRdmDtReader：日线缓存按区间/按条数读取时，右界落在两根之间的回退
//
// 覆盖（src/WtDataStorage/WtRdmDtReader.cpp）：
//  - indexBarFromCacheByRange  `eit->date/time > eBar -> eit--` :1662-1671（日线 isDay 分支）
//  - indexBarFromCacheByCount  同型回退                        :1711-1720
//
// 文件只有 0105/0110 两根日线，etime 取 0108；lower_bound 命中 0110 后必须回退一根，
// 若直接返回 0110 则结果会越过右界。init(NULL) 不起巡检线程。
#include "r74_rdm_fixture.h"

#include "WtDataStorage/WtRdmDtReader.h"

namespace r94 {

using namespace q03;
using std::string;
using std::vector;

namespace {

const uint32_t kCurTDate = 20240110;

} // namespace

class R94RdmDayBackoffTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r94day"));
		_bd.set_cur_tdate(kCurTDate);
		_reader.init(NULL, &_sink);

		vector<WTSBarStruct> bars;
		bars.push_back(make_bar(20240105, 20240105, 1));
		bars.push_back(make_bar(20240110, 20240110, 2));
		ASSERT_TRUE(write_bytes("his/day/SSE/600010.dsb",
			make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(bars))));
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

// WtRdmDtReader.cpp: L1662-1671 / L1711-1720
TEST_F(R94RdmDayBackoffTest, R94RdmDayRangeAndCountBackOffBeforeRightBoundary)
{
	WTSKlineSlice* range = _reader.readKlineSliceByRange("SSE.STK.600010", KP_DAY,
		202401011500ull, 202401081500ull);
	ASSERT_TRUE(range != NULL);
	vector<WTSBarStruct> rgot = r52::flatten_bars(range);
	ASSERT_EQ(rgot.size(), (std::size_t)1);
	EXPECT_EQ(rgot[0].date, 20240105u);
	range->release();

	WTSKlineSlice* count = _reader.readKlineSliceByCount("SSE.STK.600010", KP_DAY, 1,
		202401081500ull);
	ASSERT_TRUE(count != NULL);
	vector<WTSBarStruct> cgot = r52::flatten_bars(count);
	ASSERT_EQ(cgot.size(), (std::size_t)1);
	EXPECT_EQ(cgot[0].date, 20240105u);
	count->release();
}

// BUG-R94-001（待审核）：右界早于缓存首根时，indexBarFromCacheByRange / ByCount
// 的 `eit->date > eBar.date -> eit--`（:1666-1669 / :1715-1718）没有 begin 守卫；
// eit 已指向 begin 时自减越界，eIdx 回绕成 SIZE_MAX，随后 lower_bound 的区间
// first > last，返回的 count 也会溢出。同型的 RT 段 :1880-1884 / :1953-1957 /
// :2303-2307 / :2360-2364 也有 `pBar--` 无 idx==0 守卫。
TEST_F(R94RdmDayBackoffTest, DISABLED_R94RdmRangeBeforeFirstBarUnderflowsCacheCursor)
{
	WTSKlineSlice* s = _reader.readKlineSliceByRange("SSE.STK.600010", KP_DAY,
		202312010000ull, 202401011500ull);
	if (s != NULL)
		s->release();
}

} // namespace r94
