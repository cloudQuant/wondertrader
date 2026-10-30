// BUG-Q03-004: historical and realtime Tick range endpoints are inclusive.
// Cover exact first/middle/last hits, duplicate timestamps, and endpoints just
// outside the available records using the same committed Tick sequence.
#include "q03_storage3_fixture.h"

#include "WtDataStorage/WtRdmDtReader.h"

namespace q03range {

using q03::BaseDataMgr;
using q03::HotMgr;
using q03::RdmSink;
using q03::Sandbox;
using q03::make_block;
using q03::make_rt_block;
using q03::make_tick;
using q03::pack_ticks;
using q03::write_bytes;
using std::vector;

namespace {

const uint32_t kTradingDate = 20240110;
const uint32_t kPreviousActionDate = 20240109;
const uint32_t kHistoryReaderDate = 20240112;

struct TickTime
{
	uint32_t date;
	uint32_t time;
};

uint64_t tick_time(const TickTime& value)
{
	return (uint64_t)value.date * 1000000000ull + value.time;
}

vector<WTSTickStruct> committed_ticks()
{
	vector<WTSTickStruct> ticks;
	ticks.push_back(make_tick(kTradingDate, kPreviousActionDate, 210000000, 1));
	ticks.push_back(make_tick(kTradingDate, kTradingDate, 93000000, 2));
	ticks.push_back(make_tick(kTradingDate, kTradingDate, 93000000, 3));
	ticks.push_back(make_tick(kTradingDate, kTradingDate, 93100000, 4));
	return ticks;
}

vector<WTSTickStruct> flatten(WTSTickSlice* slice)
{
	vector<WTSTickStruct> result;
	if (slice == NULL)
		return result;

	for (std::size_t block = 0; block < slice->get_block_counts(); ++block)
	{
		WTSTickStruct* items = slice->get_block_addr(block);
		for (uint32_t i = 0; items != NULL && i < slice->get_block_size(block); ++i)
			result.push_back(items[i]);
	}
	return result;
}

} // namespace

class TradingDateBaseDataMgr : public BaseDataMgr
{
public:
	uint32_t calcTradingDate(const char* stdPID, uint32_t date, uint32_t time, bool isHoliday) override
	{
		// Model an overnight action date belonging to the same trading-date block.
		if (date == kPreviousActionDate)
			return kTradingDate;
		return BaseDataMgr::calcTradingDate(stdPID, date, time, isHoliday);
	}
};

class Q03RdmTickRangeInclusiveTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("q03tickrange"));
		_bd.set_cur_tdate(kHistoryReaderDate);
		_reader.init(NULL, &_sink);
	}

	void TearDown() override
	{
		_reader.clearCache();
		_sandbox.reset();
	}

	void expect_range(const char* code, const TickTime& start, const TickTime& end,
		const vector<uint32_t>& expectedSequences)
	{
		WTSTickSlice* slice = _reader.readTickSliceByRange(code,
			tick_time(start), tick_time(end));
		ASSERT_TRUE(slice != NULL) << code << " [" << start.date << "/" << start.time
			<< ", " << end.date << "/" << end.time << "]";

		vector<WTSTickStruct> actual = flatten(slice);
		slice->release();
		ASSERT_EQ(actual.size(), expectedSequences.size()) << code << " ["
			<< start.date << "/" << start.time << ", " << end.date << "/" << end.time << "]";

		const uint32_t expectedDates[] = { kPreviousActionDate, kTradingDate, kTradingDate, kTradingDate };
		const uint32_t expectedTimes[] = { 210000000, 93000000, 93000000, 93100000 };
		for (std::size_t i = 0; i < expectedSequences.size(); ++i)
		{
			const uint32_t seq = expectedSequences[i];
			ASSERT_GE(seq, 1u);
			ASSERT_LE(seq, 4u);
			EXPECT_EQ(actual[i].action_date, expectedDates[seq - 1]);
			EXPECT_EQ(actual[i].action_time, expectedTimes[seq - 1]);
			EXPECT_DOUBLE_EQ(actual[i].price, 30.0 + seq);
		}
	}

	void expect_boundary_matrix(const char* code)
	{
		const TickTime beforeFirst = { kPreviousActionDate, 205900000 };
		const TickTime first = { kPreviousActionDate, 210000000 };
		const TickTime middle = { kTradingDate, 93000000 };
		const TickTime last = { kTradingDate, 93100000 };
		const TickTime afterLast = { kTradingDate, 93200000 };
		const TickTime later = { kTradingDate, 93300000 };

		// The block contains an overnight action date and a day-session action
		// date, but all records remain in one trading-date file/block.
		expect_range(code, beforeFirst, beforeFirst, vector<uint32_t>());
		// Exact first record.
		expect_range(code, first, first, vector<uint32_t>{ 1 });
		// Exact middle timestamp contains both records sharing that timestamp.
		expect_range(code, first, middle, vector<uint32_t>{ 1, 2, 3 });
		// Exact final committed record.
		expect_range(code, first, last, vector<uint32_t>{ 1, 2, 3, 4 });
		// End after the final committed record still returns the whole interval.
		expect_range(code, first, afterLast, vector<uint32_t>{ 1, 2, 3, 4 });
		// Start after the last record must produce an empty range without underflow.
		expect_range(code, afterLast, later, vector<uint32_t>());
	}

	void expect_single_record_boundaries(const char* code, uint32_t date, uint32_t sequence)
	{
		const TickTime before = { date, 92900000 };
		const TickTime first = { date, 93000000 };
		const TickTime after = { date, 93100000 };
		const TickTime later = { date, 93200000 };

		auto expect_one_or_empty = [&](const TickTime& start, const TickTime& end, bool expectedPresent) {
			WTSTickSlice* slice = _reader.readTickSliceByRange(code, tick_time(start), tick_time(end));
			ASSERT_TRUE(slice != NULL) << code;
			vector<WTSTickStruct> actual = flatten(slice);
			slice->release();
			ASSERT_EQ(actual.size(), expectedPresent ? 1u : 0u) << code;
			if (expectedPresent)
			{
				EXPECT_EQ(actual[0].action_date, date);
				EXPECT_EQ(actual[0].action_time, 93000000u);
				EXPECT_DOUBLE_EQ(actual[0].price, 30.0 + sequence);
			}
		};

		// One published Tick: before it, exact equality, after it, and a start
		// beyond it. The same expected record count applies to history and RT.
		expect_one_or_empty(before, before, false);
		expect_one_or_empty(first, first, true);
		expect_one_or_empty(before, after, true);
		expect_one_or_empty(after, later, false);
	}

	std::unique_ptr<Sandbox> _sandbox;
	TradingDateBaseDataMgr _bd;
	HotMgr _hot;
	RdmSink _sink{ &_bd, &_hot };
	WtRdmDtReader _reader;
};

TEST_F(Q03RdmTickRangeInclusiveTest, HistoricalAndRealtimeKeepInclusiveExactEndpoints)
{
	vector<WTSTickStruct> historical = committed_ticks();
	ASSERT_TRUE(write_bytes("his/ticks/SSE/20240110/600901.dsb",
		make_block(BT_HIS_Ticks, BLOCK_VERSION_RAW_V2, pack_ticks(historical))));

	// RT _size includes the final writer-owned slot. The first four records are
	// the same committed observations as history; the open slot follows etime.
	vector<WTSTickStruct> realtime = committed_ticks();
	realtime.push_back(make_tick(kTradingDate, kTradingDate, 93300000, 5));
	ASSERT_TRUE(write_bytes("rt/ticks/SSE/600902.dmb",
		make_rt_block<WTSTickStruct>(BT_RT_Ticks, kTradingDate,
			(uint32_t)realtime.size(), 8, realtime)));

	expect_boundary_matrix("SSE.STK.600901");
	_bd.set_cur_tdate(kTradingDate);
	expect_boundary_matrix("SSE.STK.600902");

	// A single history record and a single committed RT record (plus the
	// writer-owned RT slot) exercise exact/before/after bounds without relying
	// on an adjacent record to make the index arithmetic safe.
	vector<WTSTickStruct> singleHistory;
	singleHistory.push_back(make_tick(kTradingDate, kTradingDate, 93000000, 6));
	ASSERT_TRUE(write_bytes("his/ticks/SSE/20240110/600903.dsb",
		make_block(BT_HIS_Ticks, BLOCK_VERSION_RAW_V2, pack_ticks(singleHistory))));

	vector<WTSTickStruct> singleRealtime = singleHistory;
	singleRealtime[0] = make_tick(kTradingDate, kTradingDate, 93000000, 7);
	singleRealtime.push_back(make_tick(kTradingDate, kTradingDate, 93100000, 8));
	ASSERT_TRUE(write_bytes("rt/ticks/SSE/600904.dmb",
		make_rt_block<WTSTickStruct>(BT_RT_Ticks, kTradingDate,
			(uint32_t)singleRealtime.size(), 4, singleRealtime)));

	_bd.set_cur_tdate(kHistoryReaderDate);
	expect_single_record_boundaries("SSE.STK.600903", kTradingDate, 6);
	_bd.set_cur_tdate(kTradingDate);
	expect_single_record_boundaries("SSE.STK.600904", kTradingDate, 7);

	const TickTime sameDayStart = { kTradingDate, 93100000 };
	const TickTime sameDayEnd = { kTradingDate, 93000000 };
	EXPECT_TRUE(_reader.readTickSliceByRange("SSE.STK.600902",
		tick_time(sameDayStart), tick_time(sameDayEnd)) == NULL);

	const TickTime crossDayStart = { kTradingDate, 93000000 };
	const TickTime crossDayEnd = { kPreviousActionDate, 210000000 };
	EXPECT_TRUE(_reader.readTickSliceByRange("SSE.STK.600902",
		tick_time(crossDayStart), tick_time(crossDayEnd)) == NULL);

	// etime=0 remains outside this test's oracle: IRdmDtReader declares it only
	// as a default argument, without defining whether it means latest, now, or
	// an empty cutoff. The current Rdm range path decodes zero as timestamp zero
	// (previous observed result: non-NULL empty slice); do not promote that
	// observation to a public contract without an API decision.
}

} // namespace q03range
