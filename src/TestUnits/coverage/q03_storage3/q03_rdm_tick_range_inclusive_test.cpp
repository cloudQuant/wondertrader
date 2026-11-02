// BUG-Q03-004: historical and realtime Tick range endpoints are inclusive.
// Cover exact first/middle/last hits, duplicate timestamps, and endpoints just
// outside the available records using the same committed Tick sequence.
#include "q03_storage3_fixture.h"

#include "WtDataStorage/WtRdmDtReader.h"
#include "../r23_storage4/r23_writer_fixture.h"

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
		expect_range(code, afterLast, afterLast, vector<uint32_t>());
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

	// RT _size is the number of actual records. Use the same records as history.
	vector<WTSTickStruct> realtime = committed_ticks();
	ASSERT_TRUE(write_bytes("rt/ticks/SSE/600902.dmb",
		make_rt_block<WTSTickStruct>(BT_RT_Ticks, kTradingDate,
			(uint32_t)realtime.size(), 8, realtime)));

	expect_boundary_matrix("SSE.STK.600901");
	_bd.set_cur_tdate(kTradingDate);
	expect_boundary_matrix("SSE.STK.600902");

	// A single history record and a single RT record exercise exact/before/after
	// bounds without relying on an adjacent record to make the index safe.
	vector<WTSTickStruct> singleHistory;
	singleHistory.push_back(make_tick(kTradingDate, kTradingDate, 93000000, 6));
	ASSERT_TRUE(write_bytes("his/ticks/SSE/20240110/600903.dsb",
		make_block(BT_HIS_Ticks, BLOCK_VERSION_RAW_V2, pack_ticks(singleHistory))));

	vector<WTSTickStruct> singleRealtime = singleHistory;
	singleRealtime[0] = make_tick(kTradingDate, kTradingDate, 93000000, 7);
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

	// This matrix covers finite inclusive endpoints only. The iteration-02
	// adjudication contract for etime=0/latest is exercised separately with a
	// fixed current trading date in ZeroEndTimeReadsLatestCommittedHistoryAndRealtimeTicks.
}

TEST_F(Q03RdmTickRangeInclusiveTest, ZeroEndTimeReadsLatestCommittedHistoryAndRealtimeTicks)
{
	const vector<WTSTickStruct> committed = committed_ticks();
	ASSERT_TRUE(write_bytes("his/ticks/SSE/20240110/600905.dsb",
		make_block(BT_HIS_Ticks, BLOCK_VERSION_RAW_V2, pack_ticks(committed))));

	vector<WTSTickStruct> realtime = committed;
	ASSERT_TRUE(write_bytes("rt/ticks/SSE/600906.dmb",
		make_rt_block<WTSTickStruct>(BT_RT_Ticks, kTradingDate,
			(uint32_t)realtime.size(), 8, realtime)));

	// Keep stime earlier than every fixture record so this case isolates the
	// etime=0 sentinel; same-day left clipping is covered by finite-end cases.
	const uint64_t start = tick_time(TickTime{ kPreviousActionDate, 200000000 });
	const uint32_t expectedDates[] = { kPreviousActionDate, kTradingDate, kTradingDate, kTradingDate };
	const uint32_t expectedTimes[] = { 210000000, 93000000, 93000000, 93100000 };
	auto expect_latest = [&](const char* code) {
		WTSTickSlice* slice = _reader.readTickSliceByRange(code, start, 0);
		ASSERT_NE(nullptr, slice) << code;
		vector<WTSTickStruct> actual = flatten(slice);
		slice->release();
		ASSERT_EQ(actual.size(), committed.size()) << code;
		for (std::size_t i = 0; i < committed.size(); ++i)
		{
			EXPECT_EQ(actual[i].action_date, expectedDates[i]) << code << " index=" << i;
			EXPECT_EQ(actual[i].action_time, expectedTimes[i]) << code << " index=" << i;
			EXPECT_DOUBLE_EQ(actual[i].price, 30.0 + i + 1) << code << " index=" << i;
		}
	};

	// Fix the reader's current trading date through the fake base-data manager:
	// historical latest means latest available before this controlled current date.
	_bd.set_cur_tdate(kHistoryReaderDate);
	expect_latest("SSE.STK.600905");

	// For the controlled current trading date, latest means all actual RT records.
	_bd.set_cur_tdate(kTradingDate);
	expect_latest("SSE.STK.600906");

	// A future left boundary must not leak current records into a future-date
	// range, and a same-day start after the last RT tick must stay empty.
	for (const uint64_t laterStart : {
		tick_time(TickTime{ kTradingDate + 1, 90000000 }),
		tick_time(TickTime{ kTradingDate, 93200000 }) })
	{
		WTSTickSlice* slice = _reader.readTickSliceByRange("SSE.STK.600906", laterStart, 0);
		ASSERT_NE(nullptr, slice);
		vector<WTSTickStruct> actual = flatten(slice);
		slice->release();
		EXPECT_TRUE(actual.empty()) << laterStart;
	}

	// A zero-sized RT block has no actual records.
	vector<WTSTickStruct> emptyRealtime;
	ASSERT_TRUE(write_bytes("rt/ticks/SSE/600907.dmb",
		make_rt_block<WTSTickStruct>(BT_RT_Ticks, kTradingDate, 0, 4, emptyRealtime)));
	WTSTickSlice* emptySlice = _reader.readTickSliceByRange("SSE.STK.600907", start, 0);
	ASSERT_NE(nullptr, emptySlice);
	EXPECT_TRUE(flatten(emptySlice).empty());
	emptySlice->release();

	// A single actual RT record remains visible to latest.
	vector<WTSTickStruct> singleRealtime;
	singleRealtime.push_back(make_tick(kTradingDate, kTradingDate, 93000000, 10));
	ASSERT_TRUE(write_bytes("rt/ticks/SSE/600908.dmb",
		make_rt_block<WTSTickStruct>(BT_RT_Ticks, kTradingDate, 1, 4, singleRealtime)));
	WTSTickSlice* singleSlice = _reader.readTickSliceByRange("SSE.STK.600908", start, 0);
	ASSERT_NE(nullptr, singleSlice);
	vector<WTSTickStruct> singleActual = flatten(singleSlice);
	singleSlice->release();
	ASSERT_EQ(singleActual.size(), (std::size_t)1);
	EXPECT_EQ(singleActual[0].action_time, 93000000u);
	EXPECT_DOUBLE_EQ(singleActual[0].price, 40.0);
}

class WriterReaderSink : public IRdmDtReaderSink
{
public:
	explicit WriterReaderSink(IBaseDataMgr* bd) : _bd(bd) {}
	IBaseDataMgr* get_basedata_mgr() override { return _bd; }
	IHotMgr* get_hot_mgr() override { return &_hot; }
	void reader_log(WTSLogLevel, const char*) override {}

private:
	IBaseDataMgr* _bd;
	q03::HotMgr _hot;
};

class CurrentPathScope
{
public:
	explicit CurrentPathScope(const std::filesystem::path& next)
		: _previous(std::filesystem::current_path())
	{
		std::filesystem::current_path(next);
	}

	~CurrentPathScope()
	{
		std::error_code ec;
		std::filesystem::current_path(_previous, ec);
	}

private:
	std::filesystem::path _previous;
};

class WriterTickRangeE2ETest : public r23::R23WriterTest
{
};

TEST_F(WriterTickRangeE2ETest, PublishedWriterTicksAreVisibleThroughPublicRange)
{
	// Keep writer and reader current-date sources aligned with these fixed test ticks.
	_bd->set_cur_tdate(kTradingDate);
	_sink->set_tdate(kTradingDate);

	r23::R23Writer* writer = make_writer(
		"\"disablemin1\":true,\"disablemin5\":true,\"disableday\":true");
	ASSERT_NE(nullptr, writer);

	r23::TickSpec first;
	first.code = "rb2210";
	first.exchg = "SHFE";
	first.trading_date = kTradingDate;
	first.action_date = kTradingDate;
	first.action_time = 93000000;
	first.price = 3600.0;
	first.total_volume = 1000.0;
	WTSTickData* writtenFirst = write_tick(writer, first, 0);
	ASSERT_NE(nullptr, writtenFirst);

	r23::TickSpec second = first;
	second.action_time = 93100000;
	second.price = 3601.0;
	second.total_volume = 1010.0;

	const uint64_t start = tick_time(TickTime{ kTradingDate, 90000000 });
	const uint64_t firstEnd = tick_time(TickTime{ kTradingDate, first.action_time });
	const uint64_t secondEnd = tick_time(TickTime{ kTradingDate, second.action_time });
	const vector<uint32_t> oneTime{ first.action_time };
	const vector<double> onePrice{ first.price };
	const vector<uint32_t> twoTimes{ first.action_time, second.action_time };
	const vector<double> twoPrices{ first.price, second.price };

	CurrentPathScope cwd(_base);
	WriterReaderSink readerSink(_bd.get());
	WtRdmDtReader reader;
	reader.init(NULL, &readerSink);
	auto expect_range = [&](uint64_t etime, const vector<uint32_t>& expectedTimes,
		const vector<double>& expectedPrices) {
		WTSTickSlice* slice = reader.readTickSliceByRange("SHFE.rb2210", start, etime);
		ASSERT_NE(nullptr, slice);
		vector<WTSTickStruct> actual = flatten(slice);
		slice->release();
		ASSERT_EQ(actual.size(), expectedTimes.size());
		ASSERT_EQ(actual.size(), expectedPrices.size());
		for (std::size_t i = 0; i < actual.size(); ++i)
		{
			EXPECT_EQ(actual[i].action_date, kTradingDate) << "index=" << i;
			EXPECT_EQ(actual[i].action_time, expectedTimes[i]) << "index=" << i;
			EXPECT_DOUBLE_EQ(actual[i].price, expectedPrices[i]) << "index=" << i;
		}
	};

	EXPECT_EQ(r23::read_block_header<RTDayBlockHeader>(rt_tick_file("rb2210"))._size, 1u);
	expect_range(0, oneTime, onePrice);
	expect_range(firstEnd, oneTime, onePrice);

	WTSTickData* writtenSecond = write_tick(writer, second, 0);
	ASSERT_NE(nullptr, writtenSecond);
	EXPECT_EQ(r23::read_block_header<RTDayBlockHeader>(rt_tick_file("rb2210"))._size, 2u);
	expect_range(0, twoTimes, twoPrices);
	expect_range(firstEnd, oneTime, onePrice);
	expect_range(secondEnd, twoTimes, twoPrices);

	reader.clearCache();
}

} // namespace q03range
