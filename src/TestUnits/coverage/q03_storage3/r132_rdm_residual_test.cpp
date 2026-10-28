// r132 / WtRdmDtReader：K 线区间/条数读取的 RT 右界回退、历史缓存右界回退、
//       逐笔 RT 区间比较器的跨 action_date 分支、当日空 RT 块。
//
// 覆盖（src/WtDataStorage/WtRdmDtReader.cpp）：
//  - readOrdQueSliceByRange RT 比较器跨 action_date :660-665
//  - readOrdDtlSliceByRange RT 比较器跨 action_date :835-840
//  - readTransSliceByRange RT 比较器跨 action_date :1009-1014
//  - readKlineSliceByRange RT 右界回退 :1880-1884
//  - indexBarFromCacheByRange 的历史右界回退 :1760-1766
//  - readKlineSliceByCount RT 右界回退 :2296-2307
//  - readTickSliceByDate 当日 RT 块 _size==0 :349-351
//
// 手法：Sandbox 切到临时目录，WtRdmDtReader::init(NULL, sink) 只注入替身、不起 5s 巡检
// 线程；数据文件都写成相对路径（_base_dir 为空串）。没有访问权限宏，也没有墙钟/线程。
#include "r52_storage_fixture.h"

#include "WtDataStorage/WtRdmDtReader.h"

namespace r132 {

using namespace q03;
using std::string;
using std::vector;

namespace {

uint64_t min_time(uint32_t date, uint32_t hhmm)
{
	return TimeUtils::timeToMinBar(date, hhmm);
}

} // namespace

class R132RdmResidualTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r132rdm"));
		_bd.set_cur_tdate(20240110);
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

// WtRdmDtReader.cpp: readKlineSliceByRange L1880-1884 / readKlineSliceByCount L2296-2307
TEST_F(R132RdmResidualTest, R132RdmKlineRtBackoffByRangeAndCount)
{
	// 历史两根：09:28 / 09:29；RT 三根：09:31 / 09:33 / 09:34
	vector<WTSBarStruct> his;
	his.push_back(make_bar(20240110, min_time(20240110, 928), 1));
	his.push_back(make_bar(20240110, min_time(20240110, 929), 2));
	ASSERT_TRUE(write_bytes("his/min1/SSE/600020.dsb",
		make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, pack_bars(his))));

	vector<WTSBarStruct> rt;
	rt.push_back(make_bar(20240110, min_time(20240110, 931), 11));
	rt.push_back(make_bar(20240110, min_time(20240110, 933), 12));
	rt.push_back(make_bar(20240110, min_time(20240110, 934), 13));
	ASSERT_TRUE(write_bytes("rt/min1/SSE/600020.dmb",
		make_rt_block<WTSBarStruct>(BT_RT_Minute1, 20240110, 3, 8, rt)));

	// 区间 [09:28, 09:32]：RT 右界 09:32 落在 09:31 与 09:33 之间，回退到 09:31
	WTSKlineSlice* byRange = _reader.readKlineSliceByRange("SSE.STK.600020", KP_Minute1,
		202401100928ull, 202401100932ull);
	ASSERT_TRUE(byRange != NULL);
	vector<WTSBarStruct> got = r52::flatten_bars(byRange);
	ASSERT_EQ(got.size(), (std::size_t)3);
	EXPECT_EQ(got[0].time, min_time(20240110, 928));
	EXPECT_EQ(got[1].time, min_time(20240110, 929));
	EXPECT_EQ(got[2].time, min_time(20240110, 931));
	byRange->release();

	// 条数读取：先取 RT 尾部 09:31，再用历史补 1 根 09:29
	WTSKlineSlice* byCount = _reader.readKlineSliceByCount("SSE.STK.600020", KP_Minute1,
		2, 202401100932ull);
	ASSERT_TRUE(byCount != NULL);
	got = r52::flatten_bars(byCount);
	ASSERT_EQ(got.size(), (std::size_t)2);
	EXPECT_EQ(got[0].time, min_time(20240110, 929));
	EXPECT_EQ(got[1].time, min_time(20240110, 931));
	byCount->release();
}

// WtRdmDtReader.cpp: indexBarFromCacheByRange L1756-1766
TEST_F(R132RdmResidualTest, R132RdmKlineRangeEndBetweenHisBars)
{
	vector<WTSBarStruct> his;
	his.push_back(make_bar(20240110, min_time(20240110, 931), 1));
	his.push_back(make_bar(20240110, min_time(20240110, 933), 2));
	his.push_back(make_bar(20240110, min_time(20240110, 935), 3));
	ASSERT_TRUE(write_bytes("his/min1/SSE/600021.dsb",
		make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, pack_bars(his))));

	// etime=09:32 落在 09:31 与 09:33 之间：lower_bound 命中 09:33 后必须回退
	WTSKlineSlice* s = _reader.readKlineSliceByRange("SSE.STK.600021", KP_Minute1,
		202401100930ull, 202401100932ull);
	ASSERT_TRUE(s != NULL);
	const vector<WTSBarStruct> got = r52::flatten_bars(s);
	ASSERT_EQ(got.size(), (std::size_t)1);
	EXPECT_EQ(got[0].time, min_time(20240110, 931));
	s->release();
}

// WtRdmDtReader.cpp: readOrdQueSliceByRange L660-665 / readOrdDtlSliceByRange L835-840 /
//                    readTransSliceByRange L1009-1014
TEST_F(R132RdmResidualTest, R132RdmRtHftComparatorsSeeOlderActionDate)
{
	// RT 记录的 action_date 比查询日期早一天：比较器首轮走 action_date 分支
	vector<WTSOrdQueStruct> que;
	que.push_back(make_ordque(20240110, 20240109, 140000000, 1));
	que.push_back(make_ordque(20240110, 20240109, 140100000, 2));
	ASSERT_TRUE(write_bytes("rt/queue/SSE/600022.dmb",
		make_rt_block<WTSOrdQueStruct>(BT_RT_OrdQueue, 20240110, 2, 8, que)));

	vector<WTSOrdDtlStruct> dtl;
	dtl.push_back(make_orddtl(20240110, 20240109, 140000000, 11));
	dtl.push_back(make_orddtl(20240110, 20240109, 140100000, 12));
	ASSERT_TRUE(write_bytes("rt/orders/SSE/600023.dmb",
		make_rt_block<WTSOrdDtlStruct>(BT_RT_OrdDetail, 20240110, 2, 8, dtl)));

	vector<WTSTransStruct> trs;
	trs.push_back(make_trans(20240110, 20240109, 140000000, 21));
	trs.push_back(make_trans(20240110, 20240109, 140100000, 22));
	ASSERT_TRUE(write_bytes("rt/trans/SSE/600024.dmb",
		make_rt_block<WTSTransStruct>(BT_RT_Trnsctn, 20240110, 2, 8, trs)));

	WTSOrdQueSlice* sq = _reader.readOrdQueSliceByRange("SSE.STK.600022",
		20240109210000000ull, 20240110100000000ull);
	ASSERT_TRUE(sq != NULL);
	EXPECT_EQ(sq->size(), 2u);
	EXPECT_EQ(sq->at(0)->action_date, 20240109u);
	sq->release();

	WTSOrdDtlSlice* sd = _reader.readOrdDtlSliceByRange("SSE.STK.600023",
		20240109210000000ull, 20240110100000000ull);
	ASSERT_TRUE(sd != NULL);
	EXPECT_EQ(sd->size(), 2u);
	EXPECT_EQ(sd->at(0)->action_date, 20240109u);
	sd->release();

	WTSTransSlice* st = _reader.readTransSliceByRange("SSE.STK.600024",
		20240109210000000ull, 20240110100000000ull);
	ASSERT_TRUE(st != NULL);
	EXPECT_EQ(st->size(), 2u);
	EXPECT_EQ(st->at(0)->action_date, 20240109u);
	st->release();
}

// WtRdmDtReader.cpp: readTickSliceByDate L334-358（当日 RT 块 _size==0）
TEST_F(R132RdmResidualTest, R132RdmTickDateEmptyRtBlockReturnsNull)
{
	ASSERT_TRUE(write_bytes("rt/ticks/SSE/600030.dmb",
		make_rt_block<WTSTickStruct>(BT_RT_Ticks, 20240110, 0, 8, vector<WTSTickStruct>())));

	EXPECT_TRUE(_reader.readTickSliceByDate("SSE.STK.600030", 20240110) == NULL);
}

} // namespace r132
