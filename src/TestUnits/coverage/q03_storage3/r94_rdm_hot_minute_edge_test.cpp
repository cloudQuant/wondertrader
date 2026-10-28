// r94 / WtRdmDtReader：主力连续分钟线的后复权基础因子与空分段文件边界
//
// 覆盖（src/WtDataStorage/WtRdmDtReader.cpp）：
//  - cacheHisBarsFromFile HOT 分支 cInfo->_exright==1 的 baseFactor :1215-1217
//  - 切段右界 <= 主力末条时间 -> break                              :1248-1249
//  - 分月文件解压后为空 -> break                                    :1283-1284
//
// 用 21:00-15:00 的 r52::BDMgr（分钟边界语义正确）与 r74::HotMgr（能识别 HOT 规则）；
// init(NULL) 不起巡检线程；每个子场景先 clearCache 隔离 _bars_cache。
#include "r74_rdm_fixture.h"

#include "WtDataStorage/WtRdmDtReader.h"

namespace r94 {

using namespace q03;
using std::string;
using std::vector;

namespace {

const uint32_t kCurTDate = 20240110;

} // namespace

class R94RdmHotMinuteEdgeTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r94hotmin"));
		_bd.set_cur_tdate(kCurTDate);
		_hot.set_enabled(true);
		_hot.set_raw("rb2402");
		_reader.init(NULL, &_sink);
	}

	void TearDown() override
	{
		_reader.clearCache();
		_sandbox.reset();
	}

	std::unique_ptr<Sandbox>	_sandbox;
	r52::BDMgr			_bd;
	r74::HotMgr			_hot;
	RdmSink				_sink{ &_bd, &_hot };
	WtRdmDtReader			_reader;
};

// WtRdmDtReader.cpp: L1215-1217 / L1248-1249
TEST_F(R94RdmHotMinuteEdgeTest, R94RdmHotQfqStopsWhenSectionEndsBeforeHotTail)
{
	vector<WTSBarStruct> hot;
	hot.push_back(make_bar(20240105, r52::min_bar_time(20240105, 930), 1));
	hot.push_back(make_bar(20240106, r52::min_bar_time(20240106, 930), 2));
	ASSERT_TRUE(write_bytes("his/min1/SHFE/SHFE.rb_HOT-.dsb",
		make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, pack_bars(hot))));

	_hot.clear_sections();
	_hot.add_section("rb2403", 20231201, 20240102, 1.1);

	WTSKlineSlice* s = _reader.readKlineSliceByRange("SHFE.rb.HOT-", KP_Minute1,
		202312010000ull, 202401101500ull);
	ASSERT_TRUE(s != NULL);
	const vector<WTSBarStruct> got = r52::flatten_bars(s);
	ASSERT_EQ(got.size(), (std::size_t)2);
	EXPECT_EQ(got[0].date, 20240105u);
	EXPECT_EQ(got[1].date, 20240106u);
	s->release();
}

// WtRdmDtReader.cpp: L1283-1284
TEST_F(R94RdmHotMinuteEdgeTest, R94RdmHotEmptySegmentFileBreaksAssembly)
{
	_hot.clear_sections();
	_hot.add_section("rb2404", 20240104, 20240110, 1.0);
	ASSERT_TRUE(write_bytes("his/min1/SHFE/rb2404.dsb",
		make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, "")));

	EXPECT_TRUE(_reader.readKlineSliceByRange("SHFE.rb.HOT", KP_Minute1,
		202401010000ull, 202401101500ull) == NULL);
}

} // namespace r94
