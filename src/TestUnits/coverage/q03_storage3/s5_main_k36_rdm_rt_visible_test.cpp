// 迭代03 第五会话主线程：决策 K36=A（2026-11-04 用户裁定）——RT HFT 读端统一按卡13 visibleRecords，
// 已发布且落在映射内的最后一条记录可见（不再有“writer 所有的末槽”）。
// 改前：WtRdmDtReader 的委托队列/委托明细/逐笔成交 RT 路径只在 [0, _size-1) 内查找，范围终点等于
// 最后一条记录时间时只返回前 _size-1 条；WtDataReader 已按 visibleRecords 处理，两读端不一致。
#include "r74_rdm_fixture.h"

#include "WtDataStorage/WtRdmDtReader.h"

namespace s5k36 {

using namespace q03;
using std::vector;

namespace {
const uint32_t kCurTDate = 20240110;
} // namespace

class S5MainK36RdmRtVisible : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("s5k36"));
		_bd.set_cur_tdate(kCurTDate);
		_reader.init(NULL, &_sink);

		vector<WTSOrdQueStruct> ques;
		ques.push_back(make_ordque(kCurTDate, kCurTDate, 93000000, 1));
		ques.push_back(make_ordque(kCurTDate, kCurTDate, 93100000, 2));
		ques.push_back(make_ordque(kCurTDate, kCurTDate, 93300000, 3));
		ASSERT_TRUE(write_bytes("rt/queue/SSE/600000.dmb",
			make_rt_block<WTSOrdQueStruct>(BT_RT_OrdQueue, kCurTDate, (uint32_t)ques.size(), 8, ques)));

		vector<WTSOrdDtlStruct> dtls;
		dtls.push_back(make_orddtl(kCurTDate, kCurTDate, 93000000, 3));
		dtls.push_back(make_orddtl(kCurTDate, kCurTDate, 93100000, 4));
		dtls.push_back(make_orddtl(kCurTDate, kCurTDate, 93300000, 5));
		ASSERT_TRUE(write_bytes("rt/orders/SSE/600000.dmb",
			make_rt_block<WTSOrdDtlStruct>(BT_RT_OrdDetail, kCurTDate, (uint32_t)dtls.size(), 8, dtls)));

		vector<WTSTransStruct> trs;
		trs.push_back(make_trans(kCurTDate, kCurTDate, 93000000, 5));
		trs.push_back(make_trans(kCurTDate, kCurTDate, 93100000, 6));
		trs.push_back(make_trans(kCurTDate, kCurTDate, 93300000, 7));
		ASSERT_TRUE(write_bytes("rt/trans/SSE/600000.dmb",
			make_rt_block<WTSTransStruct>(BT_RT_Trnsctn, kCurTDate, (uint32_t)trs.size(), 8, trs)));
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

TEST_F(S5MainK36RdmRtVisible, LastPublishedRecordIsVisibleForAllHftCollections)
{
	const uint64_t from = 20240110093000000ull;
	const uint64_t to = 20240110093300000ull;	// 恰为最后一条已发布记录的时间

	WTSOrdQueSlice* q = _reader.readOrdQueSliceByRange("SSE.STK.600000", from, to);
	ASSERT_TRUE(q != NULL);
	EXPECT_EQ(q->size(), 3u);
	ASSERT_TRUE(q->at(2) != NULL);
	EXPECT_EQ(q->at(2)->action_time, 93300000u);
	q->release();

	WTSOrdDtlSlice* d = _reader.readOrdDtlSliceByRange("SSE.STK.600000", from, to);
	ASSERT_TRUE(d != NULL);
	EXPECT_EQ(d->size(), 3u);
	ASSERT_TRUE(d->at(2) != NULL);
	EXPECT_EQ(d->at(2)->action_time, 93300000u);
	d->release();

	WTSTransSlice* t = _reader.readTransSliceByRange("SSE.STK.600000", from, to);
	ASSERT_TRUE(t != NULL);
	EXPECT_EQ(t->size(), 3u);
	ASSERT_TRUE(t->at(2) != NULL);
	EXPECT_EQ(t->at(2)->action_time, 93300000u);
	t->release();
}

} // namespace s5k36
