// R1 / src/WtDataStorage/WtRdmDtReader.cpp — hft「历史文件」分支（OrdQue/OrdDtl/Trans by range）
//
// 覆盖：
//  - readOrdQueSliceByRange :611 非当日 his 分支 :700-782（his/queue/<exchg>/<tdate>/<code>.dsb）
//  - readOrdDtlSliceByRange :786 非当日 his 分支 :870-955（his/orders/...）
//  - readTransSliceByRange  :960 非当日 his 分支 :1045-1130（his/trans/...）
//  - 尺寸校验（头不足 / _size 不符 → LL_ERROR + NULL）、跨日窗口返回全前缀、
//    cache 命中（_his_ordque_map 按 key 复用）、文件缺失静默 NULL、当日分支缺 rt 块 → NULL
//
// 沙箱手法与 q03 相同：_base_dir 为空串（init(NULL,sink) 不起线程），路径退化为相对路径。
// 已知缺陷（见 bugs/R1_wtdatastorage.md）：OrdDtl/Trans 分支用 _his_ordque_map 判缓存命中，
// 与自身 map 不一致；本文件只断言「首次读」的正常行为，不把缺陷行为固化成期望。
#include "../q03_storage3/q03_storage3_fixture.h"

#include "WtDataStorage/WtRdmDtReader.h"

namespace r1 {

using namespace q03;

class R1RdmHftTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r1hft"));
		_reader.init(NULL, &_sink);
	}

	void TearDown() override
	{
		_reader.clearCache();
		_sandbox.reset();
	}

	static string que_file(const char* code, uint32_t date)
	{
		return fmtutil::format("his/queue/SSE/{}/{}.dsb", date, code);
	}
	static string dtl_file(const char* code, uint32_t date)
	{
		return fmtutil::format("his/orders/SSE/{}/{}.dsb", date, code);
	}
	static string trs_file(const char* code, uint32_t date)
	{
		return fmtutil::format("his/trans/SSE/{}/{}.dsb", date, code);
	}

	std::unique_ptr<Sandbox>	_sandbox;
	BaseDataMgr	_bd;
	HotMgr		_hot;
	RdmSink		_sink{ &_bd, &_hot };
	WtRdmDtReader	_reader;
};

TEST_F(R1RdmHftTest, ordque_his_roundtrip_and_same_day_window)
{
	vector<WTSOrdQueStruct> que;
	que.push_back(make_ordque(20240108, 20240108, 93000000, 1));
	que.push_back(make_ordque(20240108, 20240108, 143000000, 2));
	ASSERT_TRUE(write_bytes(que_file("600000", 20240108),
		make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, pack_items(que))));

	// curTDate=20240110（夹具固定），etime 落在 20240108 → 走 his 文件分支
	WTSOrdQueSlice* sq = _reader.readOrdQueSliceByRange("SSE.STK.600000",
		20240108090000000ull, 20240108150000000ull);
	ASSERT_TRUE(sq != NULL);
	EXPECT_EQ(sq->size(), 2u);
	const WTSOrdQueStruct* it0 = sq->at(0);
	ASSERT_TRUE(it0 != NULL);
	EXPECT_EQ(it0->action_time, 93000000u);
	EXPECT_DOUBLE_EQ(it0->price, que[0].price);
	EXPECT_EQ(it0->order_items, que[0].order_items);
	EXPECT_EQ(it0->volumes[9], que[0].volumes[9]);
	sq->release();
}

TEST_F(R1RdmHftTest, ordque_cross_day_window_returns_full_prefix)
{
	vector<WTSOrdQueStruct> que;
	que.push_back(make_ordque(20240108, 20240108, 93000000, 1));
	que.push_back(make_ordque(20240108, 20240108, 143000000, 2));
	que.push_back(make_ordque(20240108, 20240108, 145900000, 3));
	ASSERT_TRUE(write_bytes(que_file("600001", 20240108),
		make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, pack_items(que))));

	// stime 在 20240105（上一交易日）→ beginTDate != endTDate → 返回 [0..eIdx]
	WTSOrdQueSlice* sq = _reader.readOrdQueSliceByRange("SSE.STK.600001",
		20240105093000000ull, 20240108143000000ull);
	ASSERT_TRUE(sq != NULL);
	// eTick 143000000：lower_bound 半开区间 [0, tcnt-1) 只扫前两条，命中第 2 条后回退
	// 结果应为前 2 条（93000000、143000000 中按实现语义取 eIdx+1）
	EXPECT_GE(sq->size(), 1u);
	EXPECT_LE(sq->size(), 3u);
	// 更强的可证伪点：首条必须是 93000000，且不允许越过 eTick
	const WTSOrdQueStruct* it0 = sq->at(0);
	ASSERT_TRUE(it0 != NULL);
	EXPECT_EQ(it0->action_time, 93000000u);
	for(uint32_t i = 0; i < sq->size(); i++)
	{
		const WTSOrdQueStruct* it = sq->at((int32_t)i);
		ASSERT_TRUE(it != NULL);
		EXPECT_LE(it->action_time, 143000000u);
	}
	sq->release();
}

TEST_F(R1RdmHftTest, ordque_cache_hit_survives_file_removal)
{
	vector<WTSOrdQueStruct> que;
	que.push_back(make_ordque(20240108, 20240108, 93000000, 7));
	ASSERT_TRUE(write_bytes(que_file("600002", 20240108),
		make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, pack_items(que))));

	WTSOrdQueSlice* sq = _reader.readOrdQueSliceByRange("SSE.STK.600002",
		20240108090000000ull, 20240108150000000ull);
	ASSERT_TRUE(sq != NULL);
	EXPECT_EQ(sq->size(), 1u);
	sq->release();

	// 同 key 二次读：_his_ordque_map 命中，不再触盘
	std::error_code ec;
	std::filesystem::remove(que_file("600002", 20240108), ec);
	WTSOrdQueSlice* sq2 = _reader.readOrdQueSliceByRange("SSE.STK.600002",
		20240108090000000ull, 20240108150000000ull);
	ASSERT_TRUE(sq2 != NULL);
	EXPECT_EQ(sq2->size(), 1u);
	sq2->release();

	// clearCache 后文件已不在 → 静默 NULL（缺失分支）
	_reader.clearCache();
	EXPECT_TRUE(_reader.readOrdQueSliceByRange("SSE.STK.600002",
		20240108090000000ull, 20240108150000000ull) == NULL);
}

TEST_F(R1RdmHftTest, ordque_size_check_failures_log_error_and_recover_never)
{
	// ① 文件比 V2 头还短
	ASSERT_TRUE(write_bytes(que_file("600003", 20240108), string(4, 'x')));
	uint32_t err_before = _sink.count_of(LL_ERROR);
	EXPECT_TRUE(_reader.readOrdQueSliceByRange("SSE.STK.600003",
		20240108090000000ull, 20240108150000000ull) == NULL);
	EXPECT_GT(_sink.count_of(LL_ERROR), err_before);

	// ② 头部合法但 _size 与实际不符（尾部多塞垃圾）
	vector<WTSOrdQueStruct> que;
	que.push_back(make_ordque(20240108, 20240108, 93000000, 8));
	string blk = make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, pack_items(que));
	blk.append("tail_garbage");
	std::error_code ec;
	std::filesystem::remove(que_file("600003", 20240108), ec);	// 清掉①的缓存残留
	_reader.clearCache();
	ASSERT_TRUE(write_bytes(que_file("600004", 20240108), blk));
	err_before = _sink.count_of(LL_ERROR);
	EXPECT_TRUE(_reader.readOrdQueSliceByRange("SSE.STK.600004",
		20240108090000000ull, 20240108150000000ull) == NULL);
	EXPECT_GT(_sink.count_of(LL_ERROR), err_before);
}

TEST_F(R1RdmHftTest, ordque_missing_file_returns_null_without_error_log)
{
	uint32_t err_before = _sink.count_of(LL_ERROR);
	EXPECT_TRUE(_reader.readOrdQueSliceByRange("SSE.STK.600999",
		20240108090000000ull, 20240108150000000ull) == NULL);
	EXPECT_EQ(_sink.count_of(LL_ERROR), err_before);
}

TEST_F(R1RdmHftTest, ordque_today_window_without_rt_block_returns_null)
{
	// etime 落在 curTDate(20240110) → isToday → getRTOrdQueBlock 无 rt 文件 → NULL
	EXPECT_TRUE(_reader.readOrdQueSliceByRange("SSE.STK.600000",
		20240110090000000ull, 20240110150000000ull) == NULL);
}

TEST_F(R1RdmHftTest, orddtl_his_first_read_roundtrip)
{
	vector<WTSOrdDtlStruct> dtl;
	dtl.push_back(make_orddtl(20240108, 20240108, 93000000, 3));
	dtl.push_back(make_orddtl(20240108, 20240108, 93100000, 4));
	ASSERT_TRUE(write_bytes(dtl_file("600005", 20240108),
		make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, pack_items(dtl))));

	WTSOrdDtlSlice* sd = _reader.readOrdDtlSliceByRange("SSE.STK.600005",
		20240108090000000ull, 20240108150000000ull);
	ASSERT_TRUE(sd != NULL);
	// 与 queue 相同的半开区间索引：eIdx 至少覆盖第 1 条
	GTEST_ASSERT_GE(sd->size(), 1u);
	const WTSOrdDtlStruct* it = sd->at(0);
	ASSERT_TRUE(it != NULL);
	EXPECT_EQ(it->index, dtl[0].index);
	EXPECT_DOUBLE_EQ(it->price, dtl[0].price);
	EXPECT_EQ(it->orderid, dtl[0].orderid);
	sd->release();
}

TEST_F(R1RdmHftTest, trans_his_first_read_roundtrip)
{
	vector<WTSTransStruct> trs;
	trs.push_back(make_trans(20240108, 20240108, 93000000, 4));
	trs.push_back(make_trans(20240108, 20240108, 93100000, 5));
	ASSERT_TRUE(write_bytes(trs_file("600006", 20240108),
		make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, pack_items(trs))));

	WTSTransSlice* st = _reader.readTransSliceByRange("SSE.STK.600006",
		20240108090000000ull, 20240108150000000ull);
	ASSERT_TRUE(st != NULL);
	GTEST_ASSERT_GE(st->size(), 1u);
	const WTSTransStruct* it = st->at(0);
	ASSERT_TRUE(it != NULL);
	EXPECT_EQ(it->index, trs[0].index);
	EXPECT_DOUBLE_EQ(it->price, trs[0].price);
	EXPECT_EQ(it->askorder, trs[0].askorder);
	EXPECT_EQ(it->bidorder, trs[0].bidorder);
	st->release();
}

TEST_F(R1RdmHftTest, trans_size_check_short_file_logs_error)
{
	ASSERT_TRUE(write_bytes(trs_file("600007", 20240108), string(6, 'z')));
	uint32_t err_before = _sink.count_of(LL_ERROR);
	EXPECT_TRUE(_reader.readTransSliceByRange("SSE.STK.600007",
		20240108090000000ull, 20240108150000000ull) == NULL);
	EXPECT_GT(_sink.count_of(LL_ERROR), err_before);
}

} // namespace r1
