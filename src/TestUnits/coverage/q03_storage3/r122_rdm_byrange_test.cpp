// R122 / WtRdmDtReader：逐笔类 ByRange 的跨自然日比较器、缺文件回退，
// 以及 ByCount 空结果。
//
// 覆盖（src/WtDataStorage/WtRdmDtReader.cpp）：
//  - readOrdQueSliceByRange RT 跨日 :662，历史 :750/:774，历史缺失 :740
//  - readOrdDtlSliceByRange RT 跨日 :837，RT 缺失 :831，历史跨日 :925/:948，历史缺失 :883
//  - readTransSliceByRange  RT 跨日 :1011，RT 缺失 :1005，历史跨日 :1099/:1122，历史缺失 :1057
//  - readKlineSliceByCount  count==0 且无数据时的收尾 NULL :2392
//
// 每个类型使用不同合约代码，避开 BUG-R74-001（detail/trans 历史装载误查 queue 缓存）。
// 全部数据落在 q03::Sandbox 临时目录，init(NULL) 不起 5 秒巡检线程。
#include "q03_storage3_fixture.h"

#include "WtDataStorage/WtRdmDtReader.h"

namespace q03 {

class R122RdmByRangeTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r122r"));
		_reader.init(NULL, &_sink);
	}

	void TearDown() override
	{
		fprintf(stderr, "Z1 clearCache\n");
		_reader.clearCache();
		fprintf(stderr, "Z2 sandbox reset\n");
		_sandbox.reset();
		fprintf(stderr, "Z3 teardown done\n");
	}

	std::unique_ptr<Sandbox>	_sandbox;
	BaseDataMgr	_bd;
	HotMgr		_hot;
	RdmSink		_sink{ &_bd, &_hot };
	WtRdmDtReader	_reader;
};

// WtRdmDtReader.cpp: L611-784
TEST_F(R122RdmByRangeTest, R122RdmOrdQueByRangeCrossDateAndMissing)
{
	// RT 块：[前一日, 前一日, 当日 09:30, 当日 10:30]
	vector<WTSOrdQueStruct> rt;
	rt.push_back(make_ordque(20240110, 20240109, 140000000, 41));
	rt.push_back(make_ordque(20240110, 20240109, 143000000, 42));
	rt.push_back(make_ordque(20240110, 20240110, 93000000, 43));
	rt.push_back(make_ordque(20240110, 20240110, 103000000, 44));
	ASSERT_TRUE(write_bytes("rt/queue/SSE/600801.dmb",
		make_rt_block<WTSOrdQueStruct>(BT_RT_OrdQueue, 20240110, (uint32_t)rt.size(), 8, rt)));

	fprintf(stderr, "Q1 rt written\n");
	WTSOrdQueSlice* s1 = _reader.readOrdQueSliceByRange("SSE.STK.600801",
		20240110090000000ull, 20240110110000000ull);
	fprintf(stderr, "Q2 rt read ptr=%p\n", (void*)s1);
	ASSERT_TRUE(s1 != NULL);
	fprintf(stderr, "Q3 rt size=%u\n", s1->size());
	ASSERT_EQ(s1->size(), 2u);
	EXPECT_EQ(s1->at(0)->action_time, 93000000u);
	EXPECT_EQ(s1->at(1)->action_time, 103000000u);
	s1->release();

	// 历史块：同型，endTDate=20240108（非当前交易日）
	vector<WTSOrdQueStruct> his;
	his.push_back(make_ordque(20240108, 20240107, 140000000, 45));
	his.push_back(make_ordque(20240108, 20240107, 143000000, 46));
	his.push_back(make_ordque(20240108, 20240108, 93000000, 47));
	his.push_back(make_ordque(20240108, 20240108, 103000000, 48));
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240108/600802.dsb",
		make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, pack_items(his))));

	fprintf(stderr, "Q4 his written\n");
	WTSOrdQueSlice* s2 = _reader.readOrdQueSliceByRange("SSE.STK.600802",
		20240108090000000ull, 20240108110000000ull);
	fprintf(stderr, "Q5 his read ptr=%p\n", (void*)s2);
	ASSERT_TRUE(s2 != NULL);
	fprintf(stderr, "Q6 his size=%u\n", s2->size());
	ASSERT_EQ(s2->size(), 2u);
	EXPECT_EQ(s2->at(0)->action_time, 93000000u);
	EXPECT_EQ(s2->at(1)->action_time, 103000000u);
	s2->release();

	// 历史文件缺失
	fprintf(stderr, "Q7 missing\n");
	EXPECT_TRUE(_reader.readOrdQueSliceByRange("SSE.STK.600803",
		20240106090000000ull, 20240106100000000ull) == NULL);
	fprintf(stderr, "Q8 done\n");
}

// WtRdmDtReader.cpp: L786-1133
TEST_F(R122RdmByRangeTest, R122RdmDtlTransByRangeCrossDateAndMissing)
{
	// ---- 委托明细 ----
	vector<WTSOrdDtlStruct> rt_dtl;
	rt_dtl.push_back(make_orddtl(20240110, 20240109, 140000000, 51));
	rt_dtl.push_back(make_orddtl(20240110, 20240109, 143000000, 52));
	rt_dtl.push_back(make_orddtl(20240110, 20240110, 93000000, 53));
	rt_dtl.push_back(make_orddtl(20240110, 20240110, 103000000, 54));
	ASSERT_TRUE(write_bytes("rt/orders/SSE/600804.dmb",
		make_rt_block<WTSOrdDtlStruct>(BT_RT_OrdDetail, 20240110, (uint32_t)rt_dtl.size(), 8, rt_dtl)));

	fprintf(stderr, "T1 dtl rt written\n");
	WTSOrdDtlSlice* d1 = _reader.readOrdDtlSliceByRange("SSE.STK.600804",
		20240110090000000ull, 20240110110000000ull);
	fprintf(stderr, "T2 dtl rt read %p\n", (void*)d1);
	ASSERT_TRUE(d1 != NULL);
	fprintf(stderr, "T3 dtl rt size %u\n", d1->size());
	ASSERT_EQ(d1->size(), 2u);
	EXPECT_EQ(d1->at(0)->action_time, 93000000u);
	d1->release();

	vector<WTSOrdDtlStruct> his_dtl;
	his_dtl.push_back(make_orddtl(20240108, 20240107, 140000000, 55));
	his_dtl.push_back(make_orddtl(20240108, 20240107, 143000000, 56));
	his_dtl.push_back(make_orddtl(20240108, 20240108, 93000000, 57));
	his_dtl.push_back(make_orddtl(20240108, 20240108, 103000000, 58));
	ASSERT_TRUE(write_bytes("his/orders/SSE/20240108/600805.dsb",
		make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, pack_items(his_dtl))));

	fprintf(stderr, "T4 dtl his written\n");
	WTSOrdDtlSlice* d2 = _reader.readOrdDtlSliceByRange("SSE.STK.600805",
		20240108090000000ull, 20240108110000000ull);
	fprintf(stderr, "T5 dtl his read %p\n", (void*)d2);
	ASSERT_TRUE(d2 != NULL);
	fprintf(stderr, "T6 dtl his size %u\n", d2->size());
	ASSERT_EQ(d2->size(), 2u);
	EXPECT_EQ(d2->at(0)->action_time, 93000000u);
	d2->release();

	fprintf(stderr, "T7 dtl missing rt\n");
	EXPECT_TRUE(_reader.readOrdDtlSliceByRange("SSE.STK.600806",
		20240110090000000ull, 20240110100000000ull) == NULL);
	fprintf(stderr, "T8 dtl missing his\n");
	EXPECT_TRUE(_reader.readOrdDtlSliceByRange("SSE.STK.600807",
		20240106090000000ull, 20240106100000000ull) == NULL);

	// ---- 逐笔成交 ----
	vector<WTSTransStruct> rt_trs;
	rt_trs.push_back(make_trans(20240110, 20240109, 140000000, 61));
	rt_trs.push_back(make_trans(20240110, 20240109, 143000000, 62));
	rt_trs.push_back(make_trans(20240110, 20240110, 93000000, 63));
	rt_trs.push_back(make_trans(20240110, 20240110, 103000000, 64));
	ASSERT_TRUE(write_bytes("rt/trans/SSE/600808.dmb",
		make_rt_block<WTSTransStruct>(BT_RT_Trnsctn, 20240110, (uint32_t)rt_trs.size(), 8, rt_trs)));

	fprintf(stderr, "T9 trans rt written\n");
	WTSTransSlice* t1 = _reader.readTransSliceByRange("SSE.STK.600808",
		20240110090000000ull, 20240110110000000ull);
	fprintf(stderr, "TA trans rt read %p\n", (void*)t1);
	ASSERT_TRUE(t1 != NULL);
	fprintf(stderr, "TB trans rt size %u\n", t1->size());
	ASSERT_EQ(t1->size(), 2u);
	EXPECT_EQ(t1->at(0)->action_time, 93000000u);
	t1->release();

	vector<WTSTransStruct> his_trs;
	his_trs.push_back(make_trans(20240108, 20240107, 140000000, 65));
	his_trs.push_back(make_trans(20240108, 20240107, 143000000, 66));
	his_trs.push_back(make_trans(20240108, 20240108, 93000000, 67));
	his_trs.push_back(make_trans(20240108, 20240108, 103000000, 68));
	ASSERT_TRUE(write_bytes("his/trans/SSE/20240108/600809.dsb",
		make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, pack_items(his_trs))));

	fprintf(stderr, "TC trans his written\n");
	WTSTransSlice* t2 = _reader.readTransSliceByRange("SSE.STK.600809",
		20240108090000000ull, 20240108110000000ull);
	fprintf(stderr, "TD trans his read %p\n", (void*)t2);
	ASSERT_TRUE(t2 != NULL);
	fprintf(stderr, "TE trans his size %u\n", t2->size());
	ASSERT_EQ(t2->size(), 2u);
	EXPECT_EQ(t2->at(0)->action_time, 93000000u);
	t2->release();

	fprintf(stderr, "TF trans missing rt\n");
	EXPECT_TRUE(_reader.readTransSliceByRange("SSE.STK.600810",
		20240110090000000ull, 20240110100000000ull) == NULL);
	fprintf(stderr, "TG trans missing his\n");
	EXPECT_TRUE(_reader.readTransSliceByRange("SSE.STK.600811",
		20240106090000000ull, 20240106100000000ull) == NULL);
}

// WtRdmDtReader.cpp: L2395-2605 收尾 NULL（count==0 且没有历史/实时数据）
TEST_F(R122RdmByRangeTest, R122RdmKlineByCountEmptyResult)
{
	EXPECT_TRUE(_reader.readKlineSliceByCount("SSE.STK.600812", KP_Minute1, 0,
		20240110100000000ull) == NULL);
	EXPECT_TRUE(_reader.readKlineSliceByCount("SSE.STK.600812", KP_DAY, 0,
		20240110100000000ull) == NULL);
}

} // namespace q03
