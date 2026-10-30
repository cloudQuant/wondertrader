// Historical IDataReader count/etime boundary regression for queue/detail/trans.
#include "r74_rdm_fixture.h"

#include "WTSUtils/WTSCfgLoader.h"
#include "WtDataStorage/WtDataReader.h"

namespace r216 {

using std::string;
using std::vector;

class ReaderHistoricalCountBoundsTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new q03::Sandbox("r216count"));
		_root = _sandbox->root_abs();
		_bd.set_cur_tdate(20240110);
	}

	void init_reader()
	{
		const string cfgfile = _root + "reader.json";
		const string json = fmtutil::format("{{\"path\":\"{}\",\"adjust_flag\":0}}", _root);
		ASSERT_TRUE(q03::write_bytes(cfgfile, json));
		WTSVariant* cfg = WTSCfgLoader::load_from_file(cfgfile.c_str());
		ASSERT_NE(cfg, nullptr);
		_reader.init(cfg, &_sink, nullptr);
		cfg->release();
	}

	template<typename T>
	bool write_series(const char* folder, const char* code, uint16_t type, const vector<T>& values)
	{
		const string path = fmtutil::format("his/{}/{}/{}/{}.dsb", folder, "SSE", "20240108", code);
		return q03::write_bytes(path,
			q03::make_block(type, BLOCK_VERSION_CMP_V2, q03::pack_items(values)));
	}

	template<typename T>
	bool write_series_on_date(const char* folder, const char* date, const char* code,
		uint16_t type, const vector<T>& values)
	{
		const string path = fmtutil::format("his/{}/{}/{}/{}.dsb", folder, "SSE", date, code);
		return q03::write_bytes(path,
			q03::make_block(type, BLOCK_VERSION_CMP_V2, q03::pack_items(values)));
	}

	std::unique_ptr<q03::Sandbox> _sandbox;
	string _root;
	r52::BDMgr _bd;
	r52::HotMgr _hot;
	r52::ReaderSink _sink{&_bd, &_hot};
	WtDataReader _reader;
};

TEST_F(ReaderHistoricalCountBoundsTest, SingleRecordBeforeAndAtFirstReturnNullForAllTypes)
{
	const WTSOrdQueStruct queue = q03::make_ordque(20240108, 20240108, 93000000, 11);
	const WTSOrdDtlStruct detail = q03::make_orddtl(20240108, 20240108, 93000000, 21);
	const WTSTransStruct trans = q03::make_trans(20240108, 20240108, 93000000, 31);
	ASSERT_TRUE(write_series("queue", "610201", BT_HIS_OrdQueue, vector<WTSOrdQueStruct>{queue}));
	ASSERT_TRUE(write_series("orders", "620201", BT_HIS_OrdDetail, vector<WTSOrdDtlStruct>{detail}));
	ASSERT_TRUE(write_series("trans", "630201", BT_HIS_Trnsctn, vector<WTSTransStruct>{trans}));
	init_reader();

	const uint64_t before = 20240108090000000ull;
	const uint64_t exact = 20240108093000000ull;
	const uint64_t after = 20240108093100000ull;
	EXPECT_EQ(_reader.readOrdQueSlice("SSE.STK.610201", 5, before), nullptr);
	EXPECT_EQ(_reader.readOrdQueSlice("SSE.STK.610201", 5, exact), nullptr);
	EXPECT_EQ(_reader.readOrdDtlSlice("SSE.STK.620201", 5, before), nullptr);
	EXPECT_EQ(_reader.readOrdDtlSlice("SSE.STK.620201", 5, exact), nullptr);
	EXPECT_EQ(_reader.readTransSlice("SSE.STK.630201", 5, before), nullptr);
	EXPECT_EQ(_reader.readTransSlice("SSE.STK.630201", 5, exact), nullptr);

	WTSOrdQueSlice* q = _reader.readOrdQueSlice("SSE.STK.610201", 5, after);
	ASSERT_NE(q, nullptr);
	ASSERT_EQ(q->size(), 1u);
	EXPECT_DOUBLE_EQ(q->at(0)->price, queue.price);
	q->release();

	WTSOrdDtlSlice* d = _reader.readOrdDtlSlice("SSE.STK.620201", 5, after);
	ASSERT_NE(d, nullptr);
	ASSERT_EQ(d->size(), 1u);
	EXPECT_EQ(d->at(0)->index, detail.index);
	d->release();

	WTSTransSlice* t = _reader.readTransSlice("SSE.STK.630201", 5, after);
	ASSERT_NE(t, nullptr);
	ASSERT_EQ(t->size(), 1u);
	EXPECT_EQ(t->at(0)->index, trans.index);
	t->release();
}

TEST_F(ReaderHistoricalCountBoundsTest, MultiRecordCrossDateMiddleAndTailQueriesStayBounded)
{
	const char* const queueCode = "610202";
	const char* const detailCode = "620202";
	const char* const transCode = "630202";
	const vector<WTSOrdQueStruct> queues{
		q03::make_ordque(20240109, 20240108, 210000000, 41),
		q03::make_ordque(20240109, 20240109, 93000000, 42),
		q03::make_ordque(20240109, 20240109, 103000000, 43)};
	const vector<WTSOrdDtlStruct> details{
		q03::make_orddtl(20240109, 20240108, 210000000, 51),
		q03::make_orddtl(20240109, 20240109, 93000000, 52),
		q03::make_orddtl(20240109, 20240109, 103000000, 53)};
	const vector<WTSTransStruct> transactions{
		q03::make_trans(20240109, 20240108, 210000000, 61),
		q03::make_trans(20240109, 20240109, 93000000, 62),
		q03::make_trans(20240109, 20240109, 103000000, 63)};

	for (const char* date : {"20240108", "20240109"})
	{
		ASSERT_TRUE(write_series_on_date("queue", date, queueCode, BT_HIS_OrdQueue, queues));
		ASSERT_TRUE(write_series_on_date("orders", date, detailCode, BT_HIS_OrdDetail, details));
		ASSERT_TRUE(write_series_on_date("trans", date, transCode, BT_HIS_Trnsctn, transactions));
	}
	const vector<WTSOrdQueStruct> priorDateQueues{
		q03::make_ordque(20240109, 20240108, 200000000, 71),
		q03::make_ordque(20240109, 20240108, 210000000, 72)};
	const vector<WTSOrdDtlStruct> priorDateDetails{
		q03::make_orddtl(20240109, 20240108, 200000000, 81),
		q03::make_orddtl(20240109, 20240108, 210000000, 82)};
	const vector<WTSTransStruct> priorDateTransactions{
		q03::make_trans(20240109, 20240108, 200000000, 91),
		q03::make_trans(20240109, 20240108, 210000000, 92)};
	ASSERT_TRUE(write_series_on_date("queue", "20240109", "610203", BT_HIS_OrdQueue, priorDateQueues));
	ASSERT_TRUE(write_series_on_date("orders", "20240109", "620203", BT_HIS_OrdDetail, priorDateDetails));
	ASSERT_TRUE(write_series_on_date("trans", "20240109", "630203", BT_HIS_Trnsctn, priorDateTransactions));
	init_reader();

	auto check = [&](uint64_t endTime, std::size_t expected)
	{
		WTSOrdQueSlice* q = _reader.readOrdQueSlice("SSE.STK.610202", 5, endTime);
		ASSERT_NE(q, nullptr);
		ASSERT_EQ(q->size(), expected);
		if (expected > 0)
			EXPECT_EQ(q->at(expected - 1)->action_time,
				queues[expected - 1].action_time);
		q->release();

		WTSOrdDtlSlice* d = _reader.readOrdDtlSlice("SSE.STK.620202", 5, endTime);
		ASSERT_NE(d, nullptr);
		ASSERT_EQ(d->size(), expected);
		if (expected > 0)
			EXPECT_EQ(d->at(expected - 1)->index, details[expected - 1].index);
		d->release();

		WTSTransSlice* t = _reader.readTransSlice("SSE.STK.630202", 5, endTime);
		ASSERT_NE(t, nullptr);
		ASSERT_EQ(t->size(), expected);
		if (expected > 0)
			EXPECT_EQ(t->at(expected - 1)->index, transactions[expected - 1].index);
		t->release();
	};

	check(20240109090000000ull, 1);  // Previous action date is before the exclusive end.
	check(20240109100000000ull, 2);  // Between the second and third records.
	check(20240109110000000ull, 3);  // After the final record returns the full slice.

	// The previous day's final action time is later than this day's cutoff time,
	// but it is still earlier in lexicographic (action_date, action_time) order.
	WTSOrdQueSlice* q = _reader.readOrdQueSlice("SSE.STK.610203", 5, 20240109080000000ull);
	ASSERT_NE(q, nullptr);
	ASSERT_EQ(q->size(), 2u);
	EXPECT_EQ(q->at(1)->action_time, priorDateQueues[1].action_time);
	q->release();

	WTSOrdDtlSlice* d = _reader.readOrdDtlSlice("SSE.STK.620203", 5, 20240109080000000ull);
	ASSERT_NE(d, nullptr);
	ASSERT_EQ(d->size(), 2u);
	EXPECT_EQ(d->at(1)->index, priorDateDetails[1].index);
	d->release();

	WTSTransSlice* t = _reader.readTransSlice("SSE.STK.630203", 5, 20240109080000000ull);
	ASSERT_NE(t, nullptr);
	ASSERT_EQ(t->size(), 2u);
	EXPECT_EQ(t->at(1)->index, priorDateTransactions[1].index);
	t->release();
}

} // namespace r216
