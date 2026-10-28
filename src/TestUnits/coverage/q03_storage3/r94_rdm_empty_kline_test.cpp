// r94 / WtRdmDtReader：复权/普通 K 线文件的空载荷与过短文件守卫
//
// 覆盖（src/WtDataStorage/WtRdmDtReader.cpp）：
//  - cacheAdjustedStkBars  CMP 头 `_size == 0 -> break`          :1399-1401
//  - cacheAdjustedStkBars  RAW 头解出空 buffer -> break           :1411-1412
//  - cacheAdjustedStkBars  原始文件解出空 buffer -> break         :1480-1481
//  - cacheHisBarsFromFile  普通文件解出空 buffer -> return false  :1588-1589
//
// 四个用例各用一个独立合约代码，避免共用 _bars_cache；init(NULL) 不起巡检线程。
#include "r74_rdm_fixture.h"

#include "WtDataStorage/WtRdmDtReader.h"

namespace r94 {

using namespace q03;
using std::string;
using std::vector;

namespace {

const uint32_t kCurTDate = 20240110;

string cmp_header_with_zero_size(uint16_t blkType)
{
	BlockHeaderV2 h;
	std::memset(&h, 0, sizeof(h));
	std::memcpy(h._blk_flag, BLK_FLAG, FLAG_SIZE);
	h._type = blkType;
	h._version = BLOCK_VERSION_CMP;
	h._size = 0;
	return string((const char*)&h, sizeof(h));
}

} // namespace

class R94RdmEmptyKlineTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r94empty"));
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

// WtRdmDtReader.cpp: L1399-1401
TEST_F(R94RdmEmptyKlineTest, R94RdmCmpAdjustedFileWithZeroSizeIsEmpty)
{
	ASSERT_TRUE(write_bytes("his/min1/SSE/600000-.dsb",
		cmp_header_with_zero_size(BT_HIS_Minute1)));

	EXPECT_TRUE(_reader.readKlineSliceByRange("SSE.STK.600000-", KP_Minute1,
		202401010000ull, 202401101500ull) == NULL);
}

// WtRdmDtReader.cpp: L1411-1412
TEST_F(R94RdmEmptyKlineTest, R94RdmRawAdjustedFileWithHeaderOnlyIsEmpty)
{
	ASSERT_TRUE(write_bytes("his/min1/SSE/600001-.dsb",
		make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, "")));

	EXPECT_TRUE(_reader.readKlineSliceByRange("SSE.STK.600001-", KP_Minute1,
		202401010000ull, 202401101500ull) == NULL);
}

// WtRdmDtReader.cpp: L1480-1481
TEST_F(R94RdmEmptyKlineTest, R94RdmRawContinuationFileHeaderOnlyKeepsAdjustedBars)
{
	vector<WTSBarStruct> adj;
	adj.push_back(make_bar(20240105, r52::min_bar_time(20240105, 930), 7));
	ASSERT_TRUE(write_bytes("his/min1/SSE/600002-.dsb",
		make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, pack_bars(adj))));
	ASSERT_TRUE(write_bytes("his/min1/SSE/600002.dsb",
		make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, "")));

	WTSKlineSlice* s = _reader.readKlineSliceByRange("SSE.STK.600002-", KP_Minute1,
		202401010000ull, 202401101500ull);
	ASSERT_TRUE(s != NULL);
	const vector<WTSBarStruct> got = r52::flatten_bars(s);
	ASSERT_EQ(got.size(), (std::size_t)1);
	EXPECT_EQ(got[0].date, 20240105u);
	s->release();
}

// WtRdmDtReader.cpp: L1588-1589
TEST_F(R94RdmEmptyKlineTest, R94RdmPlainFileHeaderOnlyReturnsFalse)
{
	ASSERT_TRUE(write_bytes("his/min1/SSE/600003.dsb",
		make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, "")));

	EXPECT_TRUE(_reader.readKlineSliceByRange("SSE.STK.600003", KP_Minute1,
		202401010000ull, 202401101500ull) == NULL);
}

} // namespace r94
