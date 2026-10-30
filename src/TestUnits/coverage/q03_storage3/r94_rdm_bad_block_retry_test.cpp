// r94 / WtRdmDtReader: reject corrupt history blocks atomically and retry after repair.
#include "q03_storage3_fixture.h"

#include "WtDataStorage/WtRdmDtReader.h"

#include <memory>

namespace q03 {
namespace {

string bad_cmp_block(uint16_t type, const string& payload)
{
	const string valid = make_block(type, BLOCK_VERSION_CMP_V2, payload);
	BlockHeaderV2 header;
	memcpy(&header, valid.data(), sizeof(header));
	++header._size;

	string corrupt((const char*)&header, sizeof(header));
	corrupt.append(valid.data() + sizeof(header), valid.size() - sizeof(header));
	return corrupt;
}

string legacy_empty_cmp_block(uint16_t type)
{
	BlockHeaderV2 header;
	memset(&header, 0, sizeof(header));
	memcpy(header._blk_flag, BLK_FLAG, FLAG_SIZE);
	header._type = type;
	header._version = BLOCK_VERSION_CMP;
	header._size = 0;
	return string((const char*)&header, sizeof(header));
}

vector<WTSTickStruct> flatten_ticks(WTSTickSlice* slice)
{
	vector<WTSTickStruct> ret;
	if (slice == NULL)
		return ret;
	for (std::size_t i = 0; i < slice->get_block_counts(); ++i)
	{
		WTSTickStruct* block = slice->get_block_addr(i);
		uint32_t count = slice->get_block_size(i);
		if (block != NULL)
			ret.insert(ret.end(), block, block + count);
	}
	return ret;
}

vector<WTSBarStruct> flatten_bars(WTSKlineSlice* slice)
{
	vector<WTSBarStruct> ret;
	if (slice == NULL)
		return ret;
	for (std::size_t i = 0; i < slice->get_block_counts(); ++i)
	{
		WTSBarStruct* block = slice->get_block_addr(i);
		uint32_t count = slice->get_block_size(i);
		if (block != NULL)
			ret.insert(ret.end(), block, block + count);
	}
	return ret;
}

class TwoSectionHotMgr : public HotMgr
{
public:
	const char* getRuleTag(const char*) override { return "HOT"; }
	const char* getCustomRawCode(const char*, const char*, uint32_t) override { return "rb2402"; }
	bool splitCustomSections(const char*, const char*, uint32_t, uint32_t, HotSections& out) override
	{
		out.emplace_back("rb2401", 20240101, 20240103, 1.0);
		out.emplace_back("rb2402", 20240104, 20240110, 1.0);
		return true;
	}
};

class R94RdmBadBlockRetryTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("rdmbad"));
		_bd.set_cur_tdate(20240110);
		_reader.init(NULL, &_sink);
	}

	void TearDown() override
	{
		_reader.clearCache();
		_sandbox.reset();
	}

	static string tick_path(const char* code, uint32_t date)
	{
		return fmtutil::format("his/ticks/SSE/{}/{}.dsb", date, code);
	}

	void write_tick_block(const char* code, uint32_t date, uint16_t version, const vector<WTSTickStruct>& ticks)
	{
		ASSERT_TRUE(write_bytes(tick_path(code, date), make_block(BT_HIS_Ticks, version, pack_ticks(ticks))));
	}

	std::unique_ptr<Sandbox> _sandbox;
	BaseDataMgr _bd;
	HotMgr _hot;
	RdmSink _sink{ &_bd, &_hot };
	WtRdmDtReader _reader;
};

TEST_F(R94RdmBadBlockRetryTest, tick_by_date_rejects_corrupt_block_and_same_reader_retries)
{
	const vector<WTSTickStruct> ticks = {
		make_tick(20240108, 20240108, 93000000, 1),
		make_tick(20240108, 20240108, 103000000, 2)
	};
	ASSERT_TRUE(write_bytes(tick_path("600101", 20240108),
		bad_cmp_block(BT_HIS_Ticks, pack_ticks(ticks))));

	const uint32_t errors_before = _sink.count_of(LL_ERROR);
	EXPECT_EQ(nullptr, _reader.readTickSliceByDate("SSE.STK.600101", 20240108));
	EXPECT_GT(_sink.count_of(LL_ERROR), errors_before);

	write_tick_block("600101", 20240108, BLOCK_VERSION_RAW_V2, ticks);
	WTSTickSlice* retry = _reader.readTickSliceByDate("SSE.STK.600101", 20240108);
	ASSERT_NE(nullptr, retry);
	const vector<WTSTickStruct> got = flatten_ticks(retry);
	ASSERT_EQ(ticks.size(), got.size());
	EXPECT_EQ(ticks[0].action_time, got[0].action_time);
	EXPECT_EQ(ticks[1].action_time, got[1].action_time);
	retry->release();
}

TEST_F(R94RdmBadBlockRetryTest, tick_range_discards_prior_day_slice_and_retries)
{
	const vector<WTSTickStruct> first_day = { make_tick(20240108, 20240108, 93000000, 1) };
	const vector<WTSTickStruct> second_day = { make_tick(20240109, 20240109, 93100000, 2) };
	write_tick_block("600102", 20240108, BLOCK_VERSION_RAW_V2, first_day);
	ASSERT_TRUE(write_bytes(tick_path("600102", 20240109),
		bad_cmp_block(BT_HIS_Ticks, pack_ticks(second_day))));

	const uint32_t errors_before = _sink.count_of(LL_ERROR);
	EXPECT_EQ(nullptr, _reader.readTickSliceByRange("SSE.STK.600102",
		20240108090000000ull, 20240109150000000ull));
	EXPECT_GT(_sink.count_of(LL_ERROR), errors_before);

	write_tick_block("600102", 20240109, BLOCK_VERSION_RAW_V2, second_day);
	WTSTickSlice* retry = _reader.readTickSliceByRange("SSE.STK.600102",
		20240108090000000ull, 20240109150000000ull);
	ASSERT_NE(nullptr, retry);
	EXPECT_EQ((std::size_t)2, flatten_ticks(retry).size());
	retry->release();
}

TEST_F(R94RdmBadBlockRetryTest, tick_count_rejects_corrupt_history_and_retries)
{
	const vector<WTSTickStruct> earlier = { make_tick(20240108, 20240108, 93000000, 1) };
	const vector<WTSTickStruct> latest = { make_tick(20240109, 20240109, 93100000, 2) };
	write_tick_block("600103", 20240108, BLOCK_VERSION_RAW_V2, earlier);
	ASSERT_TRUE(write_bytes(tick_path("600103", 20240109),
		bad_cmp_block(BT_HIS_Ticks, pack_ticks(latest))));

	const uint32_t errors_before = _sink.count_of(LL_ERROR);
	EXPECT_EQ(nullptr, _reader.readTickSliceByCount("SSE.STK.600103", 2, 20240109150000000ull));
	EXPECT_GT(_sink.count_of(LL_ERROR), errors_before);

	write_tick_block("600103", 20240109, BLOCK_VERSION_RAW_V2, latest);
	WTSTickSlice* retry = _reader.readTickSliceByCount("SSE.STK.600103", 2, 20240109150000000ull);
	ASSERT_NE(nullptr, retry);
	EXPECT_EQ((std::size_t)2, flatten_ticks(retry).size());
	retry->release();
}

TEST_F(R94RdmBadBlockRetryTest, absent_tick_history_remains_an_empty_read)
{
	const uint32_t errors_before = _sink.count_of(LL_ERROR);
	EXPECT_EQ(nullptr, _reader.readTickSliceByDate("SSE.STK.600104", 20240108));
	EXPECT_EQ(errors_before, _sink.count_of(LL_ERROR));
}

TEST_F(R94RdmBadBlockRetryTest, plain_bar_range_rejects_corruption_and_same_reader_retries)
{
	const string path = "his/day/SSE/600105.dsb";
	const vector<WTSBarStruct> bars = {
		make_bar(20240105, 20240105, 5),
		make_bar(20240106, 20240106, 6)
	};
	ASSERT_TRUE(write_bytes(path, bad_cmp_block(BT_HIS_Day, pack_bars(bars))));

	const uint32_t errors_before = _sink.count_of(LL_ERROR);
	EXPECT_EQ(nullptr, _reader.readKlineSliceByRange("SSE.STK.600105", KP_DAY,
		202401010000ull, 202401101500ull));
	EXPECT_GT(_sink.count_of(LL_ERROR), errors_before);

	ASSERT_TRUE(write_bytes(path, make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(bars))));
	WTSKlineSlice* retry = _reader.readKlineSliceByRange("SSE.STK.600105", KP_DAY,
		202401010000ull, 202401101500ull);
	ASSERT_NE(nullptr, retry);
	const vector<WTSBarStruct> got = flatten_bars(retry);
	ASSERT_EQ(bars.size(), got.size());
	EXPECT_EQ(bars[0].date, got[0].date);
	EXPECT_DOUBLE_EQ(bars[1].close, got[1].close);
	retry->release();
}

TEST_F(R94RdmBadBlockRetryTest, plain_bar_count_rejects_corruption_and_same_reader_retries)
{
	const string path = "his/day/SSE/600106.dsb";
	const vector<WTSBarStruct> bars = {
		make_bar(20240105, 20240105, 15),
		make_bar(20240106, 20240106, 16)
	};
	ASSERT_TRUE(write_bytes(path, bad_cmp_block(BT_HIS_Day, pack_bars(bars))));

	const uint32_t errors_before = _sink.count_of(LL_ERROR);
	EXPECT_EQ(nullptr, _reader.readKlineSliceByCount("SSE.STK.600106", KP_DAY, 2, 202401101500ull));
	EXPECT_GT(_sink.count_of(LL_ERROR), errors_before);

	ASSERT_TRUE(write_bytes(path, make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(bars))));
	WTSKlineSlice* retry = _reader.readKlineSliceByCount("SSE.STK.600106", KP_DAY, 2, 202401101500ull);
	ASSERT_NE(nullptr, retry);
	EXPECT_EQ((std::size_t)2, flatten_bars(retry).size());
	retry->release();
}

TEST_F(R94RdmBadBlockRetryTest, absent_plain_bar_history_is_not_logged_as_corrupt)
{
	const uint32_t errors_before = _sink.count_of(LL_ERROR);
	EXPECT_EQ(nullptr, _reader.readKlineSliceByRange("SSE.STK.600107", KP_DAY,
		202401010000ull, 202401101500ull));
	EXPECT_EQ(errors_before, _sink.count_of(LL_ERROR));
}

TEST_F(R94RdmBadBlockRetryTest, header_only_plain_bar_block_remains_an_empty_read)
{
	ASSERT_TRUE(write_bytes("his/day/SSE/600109.dsb",
		make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, "")));
	const uint32_t errors_before = _sink.count_of(LL_ERROR);
	EXPECT_EQ(nullptr, _reader.readKlineSliceByRange("SSE.STK.600109", KP_DAY,
		202401010000ull, 202401101500ull));
	EXPECT_EQ(errors_before, _sink.count_of(LL_ERROR));
}

TEST_F(R94RdmBadBlockRetryTest, empty_adjusted_layer_falls_back_to_base_bars_for_range)
{
	const vector<WTSBarStruct> bars = {
		make_bar(20240105, 20240105, 45), make_bar(20240106, 20240106, 46)
	};
	ASSERT_TRUE(write_bytes("his/day/SSE/600110+.dsb",
		make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, "")));
	ASSERT_TRUE(write_bytes("his/day/SSE/600110.dsb",
		make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(bars))));

	const uint32_t errors_before = _sink.count_of(LL_ERROR);
	WTSKlineSlice* slice = _reader.readKlineSliceByRange("SSE.STK.600110+", KP_DAY,
		202401010000ull, 202401101500ull);
	ASSERT_NE(nullptr, slice);
	const vector<WTSBarStruct> got = flatten_bars(slice);
	ASSERT_EQ(bars.size(), got.size());
	EXPECT_EQ(bars[0].date, got[0].date);
	EXPECT_DOUBLE_EQ(bars[1].close, got[1].close);
	EXPECT_EQ(errors_before, _sink.count_of(LL_ERROR));
	slice->release();
}

TEST_F(R94RdmBadBlockRetryTest, empty_adjusted_layer_falls_back_to_base_bars_for_count)
{
	const vector<WTSBarStruct> bars = {
		make_bar(20240105, 20240105, 55), make_bar(20240106, 20240106, 56)
	};
	ASSERT_TRUE(write_bytes("his/day/SSE/600111+.dsb",
		make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, "")));
	ASSERT_TRUE(write_bytes("his/day/SSE/600111.dsb",
		make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(bars))));

	const uint32_t errors_before = _sink.count_of(LL_ERROR);
	WTSKlineSlice* slice = _reader.readKlineSliceByCount("SSE.STK.600111+", KP_DAY, 2,
		202401101500ull);
	ASSERT_NE(nullptr, slice);
	const vector<WTSBarStruct> got = flatten_bars(slice);
	ASSERT_EQ(bars.size(), got.size());
	EXPECT_EQ(bars[0].date, got[0].date);
	EXPECT_DOUBLE_EQ(bars[1].close, got[1].close);
	EXPECT_EQ(errors_before, _sink.count_of(LL_ERROR));
	slice->release();
}

TEST_F(R94RdmBadBlockRetryTest, legacy_empty_cmp_adjusted_layer_falls_back_for_range)
{
	const vector<WTSBarStruct> bars = {
		make_bar(20240105, 20240105, 65), make_bar(20240106, 20240106, 66)
	};
	ASSERT_TRUE(write_bytes("his/day/SSE/600112+.dsb",
		legacy_empty_cmp_block(BT_HIS_Day)));
	ASSERT_TRUE(write_bytes("his/day/SSE/600112.dsb",
		make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(bars))));

	const uint32_t errors_before = _sink.count_of(LL_ERROR);
	WTSKlineSlice* slice = _reader.readKlineSliceByRange("SSE.STK.600112+", KP_DAY,
		202401010000ull, 202401101500ull);
	ASSERT_NE(nullptr, slice);
	const vector<WTSBarStruct> got = flatten_bars(slice);
	ASSERT_EQ(bars.size(), got.size());
	EXPECT_EQ(bars[0].date, got[0].date);
	EXPECT_DOUBLE_EQ(bars[1].close, got[1].close);
	EXPECT_EQ(errors_before, _sink.count_of(LL_ERROR));
	slice->release();
}

TEST_F(R94RdmBadBlockRetryTest, legacy_empty_cmp_adjusted_layer_falls_back_for_count)
{
	const vector<WTSBarStruct> bars = {
		make_bar(20240105, 20240105, 75), make_bar(20240106, 20240106, 76)
	};
	ASSERT_TRUE(write_bytes("his/day/SSE/600113+.dsb",
		legacy_empty_cmp_block(BT_HIS_Day)));
	ASSERT_TRUE(write_bytes("his/day/SSE/600113.dsb",
		make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(bars))));

	const uint32_t errors_before = _sink.count_of(LL_ERROR);
	WTSKlineSlice* slice = _reader.readKlineSliceByCount("SSE.STK.600113+", KP_DAY, 2,
		202401101500ull);
	ASSERT_NE(nullptr, slice);
	const vector<WTSBarStruct> got = flatten_bars(slice);
	ASSERT_EQ(bars.size(), got.size());
	EXPECT_EQ(bars[0].date, got[0].date);
	EXPECT_DOUBLE_EQ(bars[1].close, got[1].close);
	EXPECT_EQ(errors_before, _sink.count_of(LL_ERROR));
	slice->release();
}

TEST_F(R94RdmBadBlockRetryTest, malformed_legacy_empty_cmp_adjusted_layer_is_rejected)
{
	const vector<WTSBarStruct> base_bars = {
		make_bar(20240105, 20240105, 85), make_bar(20240106, 20240106, 86)
	};
	string malformed = legacy_empty_cmp_block(BT_HIS_Day);
	malformed.push_back('x');
	ASSERT_TRUE(write_bytes("his/day/SSE/600114+.dsb", malformed));
	ASSERT_TRUE(write_bytes("his/day/SSE/600114.dsb",
		make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(base_bars))));

	const uint32_t errors_before = _sink.count_of(LL_ERROR);
	EXPECT_EQ(nullptr, _reader.readKlineSliceByRange("SSE.STK.600114+", KP_DAY,
		202401010000ull, 202401101500ull));
	EXPECT_GT(_sink.count_of(LL_ERROR), errors_before);
}

TEST_F(R94RdmBadBlockRetryTest, continuous_bar_failure_frees_prior_sections_and_retries)
{
	TwoSectionHotMgr hot;
	RdmSink sink{ &_bd, &hot };
	WtRdmDtReader reader;
	reader.init(NULL, &sink);

	const vector<WTSBarStruct> recent = {
		make_bar(20240104, 20240104, 24), make_bar(20240105, 20240105, 25)
	};
	const vector<WTSBarStruct> old = {
		make_bar(20240101, 20240101, 21), make_bar(20240102, 20240102, 22)
	};
	ASSERT_TRUE(write_bytes("his/day/SHFE/rb2402.dsb",
		make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(recent))));
	ASSERT_TRUE(write_bytes("his/day/SHFE/rb2401.dsb", bad_cmp_block(BT_HIS_Day, pack_bars(old))));

	const uint32_t errors_before = sink.count_of(LL_ERROR);
	EXPECT_EQ(nullptr, reader.readKlineSliceByRange("SHFE.rb.HOT", KP_DAY,
		202401010000ull, 202401101500ull));
	EXPECT_GT(sink.count_of(LL_ERROR), errors_before);

	ASSERT_TRUE(write_bytes("his/day/SHFE/rb2401.dsb",
		make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(old))));
	WTSKlineSlice* retry = reader.readKlineSliceByRange("SHFE.rb.HOT", KP_DAY,
		202401010000ull, 202401101500ull);
	ASSERT_NE(nullptr, retry);
	EXPECT_EQ((std::size_t)4, flatten_bars(retry).size());
	retry->release();
	reader.clearCache();
}

TEST_F(R94RdmBadBlockRetryTest, adjusted_continuation_failure_releases_hot_buffer_and_retries)
{
	const string adjusted = "his/day/SSE/600108+.dsb";
	const string raw = "his/day/SSE/600108.dsb";
	const vector<WTSBarStruct> adjusted_bars = {
		make_bar(20240105, 20240105, 35), make_bar(20240106, 20240106, 36)
	};
	const vector<WTSBarStruct> raw_bars = {
		make_bar(20240107, 20240107, 37), make_bar(20240108, 20240108, 38)
	};
	ASSERT_TRUE(write_bytes(adjusted,
		make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(adjusted_bars))));
	ASSERT_TRUE(write_bytes(raw, bad_cmp_block(BT_HIS_Day, pack_bars(raw_bars))));

	const uint32_t errors_before = _sink.count_of(LL_ERROR);
	EXPECT_EQ(nullptr, _reader.readKlineSliceByRange("SSE.STK.600108+", KP_DAY,
		202401010000ull, 202401101500ull));
	EXPECT_GT(_sink.count_of(LL_ERROR), errors_before);

	ASSERT_TRUE(write_bytes(raw,
		make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(raw_bars))));
	WTSKlineSlice* retry = _reader.readKlineSliceByRange("SSE.STK.600108+", KP_DAY,
		202401010000ull, 202401101500ull);
	ASSERT_NE(nullptr, retry);
	EXPECT_EQ((std::size_t)4, flatten_bars(retry).size());
	retry->release();
}

} // namespace
} // namespace q03
