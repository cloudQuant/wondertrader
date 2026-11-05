// 迭代03 K60（BUG-I02-NEW-059 / I03-NEW-042，用户裁定 2026-11-06）：按时间区间读取统一为闭区间。
//
// 合同：WtRdmDtReader 的 readOrdQueSliceByRange / readOrdDtlSliceByRange / readTransSliceByRange
// 对 [stime, etime] 两端都包含；同一时刻的多条记录全部返回；stime == etime 返回恰好在该时刻的记录；
// 历史（.dsb）与实时（.dmb）两条路径对同一组记录、同一窗口返回相同结果。tick 一直是闭区间，不在本文件内。
//
// 数据：09:30:00.000 一条、09:31:00.000 两条（同一时刻）、09:32:00.000 一条，共 4 条。
#include "r74_rdm_fixture.h"

#include "WtDataStorage/WtRdmDtReader.h"

#include <functional>

namespace i03k60 {

using std::string;
using std::vector;

namespace {

const uint32_t kHisDate = 20240108;	// 非当前交易日 → 历史路径
const uint32_t kRtDate = 20240110;	// 当前交易日 → 实时路径
const uint32_t kTimes[] = { 93000000u, 93100000u, 93100000u, 93200000u };

uint64_t at(uint32_t date, uint32_t hhmmssmmm)
{
	return (uint64_t)date * 1000000000ull + hhmmssmmm;
}

template <typename T, typename Maker>
vector<T> make_items(uint32_t date, Maker maker)
{
	vector<T> ay;
	for (uint32_t i = 0; i < 4; i++)
		ay.push_back(maker(date, date, kTimes[i], 100 + i));
	return ay;
}

string his_file(const char* kind, uint32_t date)
{
	return fmtutil::format("his/{}/SSE/{}/600000.dsb", kind, date);
}

string rt_file(const char* kind)
{
	return fmtutil::format("rt/{}/SSE/600000.dmb", kind);
}

} // namespace

class I03K60HftRangeEndpointTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new q03::Sandbox("i03k60"));
		_bd.set_cur_tdate(kRtDate);
		_reader.reset(new WtRdmDtReader());
		_reader->init(NULL, &_sink);

		const auto que_h = make_items<WTSOrdQueStruct>(kHisDate, q03::make_ordque);
		const auto dtl_h = make_items<WTSOrdDtlStruct>(kHisDate, q03::make_orddtl);
		const auto trs_h = make_items<WTSTransStruct>(kHisDate, q03::make_trans);
		ASSERT_TRUE(q03::write_bytes(his_file("queue", kHisDate),
			q03::make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, q03::pack_items(que_h))));
		ASSERT_TRUE(q03::write_bytes(his_file("orders", kHisDate),
			q03::make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, q03::pack_items(dtl_h))));
		ASSERT_TRUE(q03::write_bytes(his_file("trans", kHisDate),
			q03::make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, q03::pack_items(trs_h))));

		const auto que_r = make_items<WTSOrdQueStruct>(kRtDate, q03::make_ordque);
		const auto dtl_r = make_items<WTSOrdDtlStruct>(kRtDate, q03::make_orddtl);
		const auto trs_r = make_items<WTSTransStruct>(kRtDate, q03::make_trans);
		ASSERT_TRUE(q03::write_bytes(rt_file("queue"),
			q03::make_rt_block<WTSOrdQueStruct>(BT_RT_OrdQueue, kRtDate, 4, 8, que_r)));
		ASSERT_TRUE(q03::write_bytes(rt_file("orders"),
			q03::make_rt_block<WTSOrdDtlStruct>(BT_RT_OrdDetail, kRtDate, 4, 8, dtl_r)));
		ASSERT_TRUE(q03::write_bytes(rt_file("trans"),
			q03::make_rt_block<WTSTransStruct>(BT_RT_Trnsctn, kRtDate, 4, 8, trs_r)));
	}

	void TearDown() override
	{
		_reader->clearCache();
		_reader.reset();
		_sandbox.reset();
	}

	// 返回 [stime, etime] 内三种数据的条数；NULL 计为 0。三种必须一致，否则直接失败。
	uint32_t count(uint32_t date, uint32_t s, uint32_t e)
	{
		uint32_t n[3] = { 0, 0, 0 };
		WTSOrdQueSlice* q = _reader->readOrdQueSliceByRange("SSE.STK.600000", at(date, s), at(date, e));
		if (q != NULL) { n[0] = (uint32_t)q->size(); q->release(); }
		WTSOrdDtlSlice* d = _reader->readOrdDtlSliceByRange("SSE.STK.600000", at(date, s), at(date, e));
		if (d != NULL) { n[1] = (uint32_t)d->size(); d->release(); }
		WTSTransSlice* t = _reader->readTransSliceByRange("SSE.STK.600000", at(date, s), at(date, e));
		if (t != NULL) { n[2] = (uint32_t)t->size(); t->release(); }
		EXPECT_EQ(n[0], n[1]) << "queue vs orders, date " << date << " [" << s << "," << e << "]";
		EXPECT_EQ(n[0], n[2]) << "queue vs trans, date " << date << " [" << s << "," << e << "]";
		return n[0];
	}

	std::unique_ptr<q03::Sandbox>	_sandbox;
	q03::BaseDataMgr		_bd;
	r74::HotMgr			_hot;
	q03::RdmSink			_sink{ &_bd, &_hot };
	std::unique_ptr<WtRdmDtReader>	_reader;
};

TEST_F(I03K60HftRangeEndpointTest, HistoricalRangeIncludesBothEnds)
{
	EXPECT_EQ(4u, count(kHisDate, 90000000u, 150000000u));	// 覆盖全部
	EXPECT_EQ(3u, count(kHisDate, 90000000u, 93100000u));	// 右端命中：09:31 的两条都返回
	EXPECT_EQ(3u, count(kHisDate, 93100000u, 93200000u));	// 两端都命中
	EXPECT_EQ(1u, count(kHisDate, 93000000u, 93000000u));	// 起止相等：恰好在该时刻的一条
	EXPECT_EQ(2u, count(kHisDate, 93100000u, 93100000u));	// 起止相等：同一时刻的两条都返回
	EXPECT_EQ(1u, count(kHisDate, 93150000u, 93200000u));	// 左端不命中、右端命中
	EXPECT_EQ(0u, count(kHisDate, 93030000u, 93030000u));	// 起止相等但该时刻无数据
}

TEST_F(I03K60HftRangeEndpointTest, RealtimeRangeIncludesBothEnds)
{
	EXPECT_EQ(4u, count(kRtDate, 90000000u, 150000000u));
	EXPECT_EQ(3u, count(kRtDate, 90000000u, 93100000u));
	EXPECT_EQ(3u, count(kRtDate, 93100000u, 93200000u));
	EXPECT_EQ(1u, count(kRtDate, 93000000u, 93000000u));
	EXPECT_EQ(2u, count(kRtDate, 93100000u, 93100000u));
	EXPECT_EQ(1u, count(kRtDate, 93150000u, 93200000u));
	EXPECT_EQ(0u, count(kRtDate, 93030000u, 93030000u));
}

TEST_F(I03K60HftRangeEndpointTest, HistoricalAndRealtimeAgreeOnEveryWindow)
{
	const uint32_t points[] = { 90000000u, 93000000u, 93030000u, 93100000u, 93150000u, 93200000u, 150000000u };
	for (uint32_t s : points)
	{
		for (uint32_t e : points)
		{
			if (e < s)
				continue;
			EXPECT_EQ(count(kRtDate, s, e), count(kHisDate, s, e)) << "[" << s << "," << e << "]";
		}
	}
}

} // namespace i03k60
