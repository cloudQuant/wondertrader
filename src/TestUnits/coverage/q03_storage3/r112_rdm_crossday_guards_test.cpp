// r112 / WtRdmDtReader：跨自然日比较器、缺失/短文件守卫、tick 计数与 K 线回退
//
// 覆盖（src/WtDataStorage/WtRdmDtReader.cpp）：
//  - readTickSliceByRange     历史/RT lambda 跨 action_date 比较 :492 / :566
//  - readOrdQueSliceByRange   历史 lambda :750 / :774；RT :662
//  - readOrdDtlSliceByRange   历史 lambda :925 / :948；RT :837
//  - readTransSliceByRange    历史 lambda :1099 / :1122；RT :1011
//  - readOrdDtl/TransSliceByRange 缺失 RT/历史文件 :831/:883、:1005/:1057
//  - readOrdQueSliceByRange   短文件二次访问 :740；RT 缺失 :2048 / 历史块为空 :915/:1089
//  - readTickSliceByCount     RT/历史跨日期 :2451 / :2579
//
// 每个数据类型用不同合约代码，避开 BUG-R74-001（dtl/trans 误查 queue 缓存）。
// 数据全部在 q03::Sandbox 临时目录；init(NULL) 不起 5 秒巡检线程。
#include "r74_rdm_fixture.h"

#include "WtDataStorage/WtRdmDtReader.h"

namespace r112 {

using namespace q03;
using std::string;
using std::vector;

namespace {

const uint32_t kCurTDate = 20240110;

vector<WTSTickStruct> cross_ticks(uint32_t tdate, uint32_t prevDate, uint32_t seq)
{
	vector<WTSTickStruct> ret;
	ret.push_back(make_tick(tdate, prevDate, 210000000, seq));
	ret.push_back(make_tick(tdate, tdate, 93000000, seq + 1));
	ret.push_back(make_tick(tdate, tdate, 103000000, seq + 2));
	return ret;
}

vector<WTSOrdQueStruct> cross_ques(uint32_t tdate, uint32_t prevDate, uint32_t seq)
{
	vector<WTSOrdQueStruct> ret;
	ret.push_back(make_ordque(tdate, prevDate, 210000000, seq));
	ret.push_back(make_ordque(tdate, tdate, 93000000, seq + 1));
	ret.push_back(make_ordque(tdate, tdate, 103000000, seq + 2));
	return ret;
}

vector<WTSOrdDtlStruct> cross_dtls(uint32_t tdate, uint32_t prevDate, uint32_t seq)
{
	vector<WTSOrdDtlStruct> ret;
	ret.push_back(make_orddtl(tdate, prevDate, 210000000, seq));
	ret.push_back(make_orddtl(tdate, tdate, 93000000, seq + 1));
	ret.push_back(make_orddtl(tdate, tdate, 103000000, seq + 2));
	return ret;
}

vector<WTSTransStruct> cross_trans(uint32_t tdate, uint32_t prevDate, uint32_t seq)
{
	vector<WTSTransStruct> ret;
	ret.push_back(make_trans(tdate, prevDate, 210000000, seq));
	ret.push_back(make_trans(tdate, tdate, 93000000, seq + 1));
	ret.push_back(make_trans(tdate, tdate, 103000000, seq + 2));
	return ret;
}

} // namespace

class R112RdmResidualTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r112rdm"));
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

// WtRdmDtReader.cpp: L492 / L566 / L662 / L750 / L774 / L837 / L925 / L948 / L1011 / L1099 / L1122
TEST_F(R112RdmResidualTest, R112RdmHftRangeCrossDateComparators)
{
	const uint32_t kT = kCurTDate;
	const uint32_t kPrev = 20240109;

	// ---- RT 块：前一日夜盘 + 当日两笔，请求截止落在两个 action_date 之间 ----
	ASSERT_TRUE(write_bytes("rt/ticks/SSE/600700.dmb",
		make_rt_block<WTSTickStruct>(BT_RT_Ticks, kT, 3, 8, cross_ticks(kT, kPrev, 1))));
	ASSERT_TRUE(write_bytes("rt/queue/SSE/610700.dmb",
		make_rt_block<WTSOrdQueStruct>(BT_RT_OrdQueue, kT, 3, 8, cross_ques(kT, kPrev, 4))));
	ASSERT_TRUE(write_bytes("rt/orders/SSE/620700.dmb",
		make_rt_block<WTSOrdDtlStruct>(BT_RT_OrdDetail, kT, 3, 8, cross_dtls(kT, kPrev, 7))));
	ASSERT_TRUE(write_bytes("rt/trans/SSE/630700.dmb",
		make_rt_block<WTSTransStruct>(BT_RT_Trnsctn, kT, 3, 8, cross_trans(kT, kPrev, 10))));

	WTSTickSlice* ts = _reader.readTickSliceByRange("SSE.STK.600700",
		20240109200000000ull, 20240110000000000ull);
	ASSERT_TRUE(ts != NULL);
	EXPECT_EQ(r52::flatten_ticks(ts).size(), (std::size_t)1);
	ts->release();

	WTSOrdQueSlice* q = _reader.readOrdQueSliceByRange("SSE.STK.610700",
		20240109200000000ull, 20240110000000000ull);
	ASSERT_TRUE(q != NULL);
	EXPECT_EQ(q->size(), 1u);
	q->release();

	WTSOrdDtlSlice* d = _reader.readOrdDtlSliceByRange("SSE.STK.620700",
		20240109200000000ull, 20240110000000000ull);
	ASSERT_TRUE(d != NULL);
	EXPECT_EQ(d->size(), 1u);
	d->release();

	WTSTransSlice* tr = _reader.readTransSliceByRange("SSE.STK.630700",
		20240109200000000ull, 20240110000000000ull);
	ASSERT_TRUE(tr != NULL);
	EXPECT_EQ(tr->size(), 1u);
	tr->release();

	// 同交易日窗口：第二个 lower_bound 的 sTick 落在夜盘与日盘之间
	q = _reader.readOrdQueSliceByRange("SSE.STK.610700",
		20240110091500000ull, 20240110100000000ull);
	ASSERT_TRUE(q != NULL);
	EXPECT_EQ(q->size(), 1u);
	EXPECT_EQ(q->at(0)->action_time, 93000000u);
	q->release();

	d = _reader.readOrdDtlSliceByRange("SSE.STK.620700",
		20240110091500000ull, 20240110100000000ull);
	ASSERT_TRUE(d != NULL);
	EXPECT_EQ(d->size(), 1u);
	d->release();

	tr = _reader.readTransSliceByRange("SSE.STK.630700",
		20240110091500000ull, 20240110100000000ull);
	ASSERT_TRUE(tr != NULL);
	EXPECT_EQ(tr->size(), 1u);
	tr->release();

	// ---- 历史块：endTDate=0108（非当前交易日），块内同样跨 action_date ----
	const uint32_t kHis = 20240108;
	const uint32_t kHisPrev = 20240107;
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240108/610701.dsb",
		make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, pack_items(cross_ques(kHis, kHisPrev, 20)))));
	ASSERT_TRUE(write_bytes("his/orders/SSE/20240108/620701.dsb",
		make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, pack_items(cross_dtls(kHis, kHisPrev, 23)))));
	ASSERT_TRUE(write_bytes("his/trans/SSE/20240108/630701.dsb",
		make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, pack_items(cross_trans(kHis, kHisPrev, 26)))));
	ASSERT_TRUE(write_bytes("his/ticks/SSE/20240108/600701.dsb",
		make_block(BT_HIS_Ticks, BLOCK_VERSION_CMP_V2, pack_ticks(cross_ticks(kHis, kHisPrev, 29)))));

	q = _reader.readOrdQueSliceByRange("SSE.STK.610701",
		20240107200000000ull, 20240108000000000ull);
	ASSERT_TRUE(q != NULL);
	EXPECT_EQ(q->size(), 1u);
	q->release();

	d = _reader.readOrdDtlSliceByRange("SSE.STK.620701",
		20240107200000000ull, 20240108000000000ull);
	ASSERT_TRUE(d != NULL);
	EXPECT_EQ(d->size(), 1u);
	d->release();

	tr = _reader.readTransSliceByRange("SSE.STK.630701",
		20240107200000000ull, 20240108000000000ull);
	ASSERT_TRUE(tr != NULL);
	EXPECT_EQ(tr->size(), 1u);
	tr->release();

	// 夜盘 tick 文件只在 0108 下；区间从 0107 20:00 开始，nowTDate 会推进到 0108
	ts = _reader.readTickSliceByRange("SSE.STK.600701",
		20240107200000000ull, 20240108000000000ull);
	ASSERT_TRUE(ts != NULL);
	EXPECT_EQ(r52::flatten_ticks(ts).size(), (std::size_t)1);
	ts->release();

	// 同交易日窗口：第二个 lower_bound 跨 action_date（历史逐笔 tick）
	ts = _reader.readTickSliceByRange("SSE.STK.600701",
		20240108091500000ull, 20240108100000000ull);
	ASSERT_TRUE(ts != NULL);
	EXPECT_EQ(r52::flatten_ticks(ts).size(), (std::size_t)1);
	ts->release();

	// 同交易日窗口：历史 queue / detail / trans 的第二个 lower_bound
	q = _reader.readOrdQueSliceByRange("SSE.STK.610701",
		20240108091500000ull, 20240108100000000ull);
	ASSERT_TRUE(q != NULL);
	EXPECT_EQ(q->size(), 1u);
	q->release();

	d = _reader.readOrdDtlSliceByRange("SSE.STK.620701",
		20240108091500000ull, 20240108100000000ull);
	ASSERT_TRUE(d != NULL);
	EXPECT_EQ(d->size(), 1u);
	d->release();

	tr = _reader.readTransSliceByRange("SSE.STK.630701",
		20240108091500000ull, 20240108100000000ull);
	ASSERT_TRUE(tr != NULL);
	EXPECT_EQ(tr->size(), 1u);
	tr->release();
}

// WtRdmDtReader.cpp: L740 / L831 / L883 / L915 / L1005 / L1057 / L1089 / L2048 / L2128
TEST_F(R112RdmResidualTest, R112RdmMissingAndShortFileGuards)
{
	// 短 queue 文件：第一次读清空缓存，第二次命中「块指针为空」
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240107/610703.dsb", string(4, '\0')));
	EXPECT_TRUE(_reader.readOrdQueSliceByRange("SSE.STK.610703",
		20240107090000000ull, 20240107100000000ull) == NULL);
	EXPECT_TRUE(_reader.readOrdQueSliceByRange("SSE.STK.610703",
		20240107090000000ull, 20240107100000000ull) == NULL);

	// 历史 detail / trans 文件缺失
	EXPECT_TRUE(_reader.readOrdDtlSliceByRange("SSE.STK.620703",
		20240106090000000ull, 20240106100000000ull) == NULL);
	EXPECT_TRUE(_reader.readTransSliceByRange("SSE.STK.630703",
		20240106090000000ull, 20240106100000000ull) == NULL);

	// RT detail / trans 文件缺失（getRT*Block 的 bHit==false 与调用方守卫）
	EXPECT_TRUE(_reader.readOrdDtlSliceByRange("SSE.STK.620702",
		20240110090000000ull, 20240110100000000ull) == NULL);
	EXPECT_TRUE(_reader.readTransSliceByRange("SSE.STK.630702",
		20240110090000000ull, 20240110100000000ull) == NULL);

	// 先读 queue，把同一 key 放进 _his_ordque_map；随后 detail/trans 会跳过装载，
	// 直接取各自 map 的默认空块（BUG-R74-001 的确定性表现）
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240105/610705.dsb",
		make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2,
			pack_items(cross_ques(20240105, 20240104, 40)))));
	WTSOrdQueSlice* q = _reader.readOrdQueSliceByRange("SSE.STK.610705",
		20240105090000000ull, 20240105100000000ull);
	ASSERT_TRUE(q != NULL);
	q->release();

	EXPECT_TRUE(_reader.readOrdDtlSliceByRange("SSE.STK.610705",
		20240105090000000ull, 20240105100000000ull) == NULL);
	EXPECT_TRUE(_reader.readTransSliceByRange("SSE.STK.610705",
		20240105090000000ull, 20240105100000000ull) == NULL);
}

// WtRdmDtReader.cpp: L492 / L566 / L2451 / L2579
TEST_F(R112RdmResidualTest, R112RdmTickCountsCrossDate)
{
	const uint32_t kT = kCurTDate;
	const uint32_t kPrev = 20240109;

	ASSERT_TRUE(write_bytes("rt/ticks/SSE/600706.dmb",
		make_rt_block<WTSTickStruct>(BT_RT_Ticks, kT, 3, 8, cross_ticks(kT, kPrev, 50))));

	// RT 区间：截止在夜盘与日盘之间
	WTSTickSlice* ts = _reader.readTickSliceByRange("SSE.STK.600706",
		20240109200000000ull, 20240110000000000ull);
	ASSERT_TRUE(ts != NULL);
	EXPECT_EQ(r52::flatten_ticks(ts).size(), (std::size_t)1);
	ts->release();

	// RT 计数：同一截止时间的 lower_bound 跨 action_date
	ts = _reader.readTickSliceByCount("SSE.STK.600706", 5, 20240110000000000ull);
	ASSERT_TRUE(ts != NULL);
	EXPECT_GT(ts->size(), 0u);
	ts->release();

	// 历史 tick：块内跨 action_date；区间与计数各走一次
	ASSERT_TRUE(write_bytes("his/ticks/SSE/20240109/600707.dsb",
		make_block(BT_HIS_Ticks, BLOCK_VERSION_CMP_V2, pack_ticks(cross_ticks(20240109, 20240108, 60)))));

	ts = _reader.readTickSliceByRange("SSE.STK.600707",
		20240108200000000ull, 20240109000000000ull);
	ASSERT_TRUE(ts != NULL);
	EXPECT_EQ(r52::flatten_ticks(ts).size(), (std::size_t)1);
	ts->release();

	ts = _reader.readTickSliceByCount("SSE.STK.600707", 5, 20240109000000000ull);
	ASSERT_TRUE(ts != NULL);
	EXPECT_GT(ts->size(), 0u);
	ts->release();

}

} // namespace r112
