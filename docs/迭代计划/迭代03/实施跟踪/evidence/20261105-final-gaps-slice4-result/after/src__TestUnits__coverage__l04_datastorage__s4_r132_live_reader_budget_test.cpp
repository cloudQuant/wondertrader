// BUG-R132-001 remainder (iteration 03 final gaps, slice4): the live reader WtDataReader and the
// data-service reader WtRdmDtReader read the K37 decompression budget (uncompress_max_bytes /
// uncompress_max_ratio, next to path) from their own store config in init(), like WtBtDtReader
// (slice2, i03_r132_btreader_budget_test.cpp). Decompression runs inside WtDataStorage, so only
// the reader's own init can apply the budget to this module.
//  - a ratio ceiling below the data's real compression ratio rejects the CMP_V2 history block;
//  - an invalid value is diagnosed (ERROR) and fails closed for compressed blocks, raw blocks
//    still read;
//  - without the keys the default budget (1 GiB / 1000:1) reads the same file (positive control).
// Red control: the test binary relinked with the pre-change reader objects (config ignored).
#include "../q03_storage3/q03_storage3_fixture.h"

#include "WtDataStorage/WtDataReader.h"
#include "WtDataStorage/WtRdmDtReader.h"

namespace s4r132 {

using namespace q03;

// The q03 sinks keep only the last message; these also keep every ERROR line.
template <typename Base>
class S4ErrorRecorder : public Base
{
public:
	using Base::Base;
	void reader_log(WTSLogLevel ll, const char* message) override
	{
		if (ll == LL_ERROR)
			errors.emplace_back(message == NULL ? "" : message);
		Base::reader_log(ll, message);
	}
	bool error_has(const char* part) const
	{
		for (const string& e : errors)
			if (e.find(part) != string::npos)
				return true;
		return false;
	}
	vector<string> errors;
};
typedef S4ErrorRecorder<ReaderSink> S4LiveSink;
typedef S4ErrorRecorder<RdmSink> S4RdmSink;

class S4R132LiveReaderBudget : public ::testing::Test
{
protected:
	void SetUp() override
	{
		WTSCmpHelper::set_uncompress_budget(1ull << 30, 1000);
		_sandbox.reset(new Sandbox("s4r132"));
		_root = _sandbox->root_abs();

		// 2000 day bars, mostly constant fields: CMP_V2 ratio is far above 5, below 1000.
		vector<WTSBarStruct> bars;
		uint32_t date = 20180102;
		for (uint32_t i = 0; i < 2000; i++)
		{
			WTSBarStruct bar = make_bar(date, 0, i % 7);
			bars.push_back(bar);
			date = TimeUtils::getNextDate(date, 1);
		}
		ASSERT_LT(date, 20240110u) << "fixture: every bar lies before the reader's trading date";
		const string raw = pack_bars(bars);
		const string compressed = make_block(BT_HIS_Day, BLOCK_VERSION_CMP_V2, raw);
		_ratio = raw.size() / (compressed.size() - sizeof(BlockHeaderV2));
		ASSERT_TRUE(write_bytes("his/day/SSE/600000.dsb", compressed));
		ASSERT_TRUE(write_bytes("his/day/SSE/600001.dsb", make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, raw)));
		_bd.set_cur_tdate(20240110);
	}

	void TearDown() override
	{
		WTSCmpHelper::set_uncompress_budget(1ull << 30, 1000);
		_sandbox.reset();
	}

	WTSVariant* base_cfg()
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("path", _root.c_str());
		cfg->append("adjust_flag", (uint32_t)0);
		return cfg;
	}

	static std::size_t bar_count(WTSKlineSlice* slice)
	{
		if (slice == NULL)
			return 0;
		std::size_t n = 0;
		for (std::size_t i = 0; i < slice->get_block_counts(); i++)
			n += slice->get_block_size(i);
		slice->release();
		return n;
	}

	// WtDataReader: readKlineSlice of the history day block.
	std::size_t live_read(WtDataReader& reader, const char* code)
	{
		return bar_count(reader.readKlineSlice(code, KP_DAY, 10, 0));
	}

	// WtRdmDtReader: readKlineSliceByRange of the history day block.
	std::size_t rdm_read(WtRdmDtReader& reader, const char* code)
	{
		return bar_count(reader.readKlineSliceByRange(code, KP_DAY, 201801010000ull, 202401090000ull));
	}

	std::unique_ptr<Sandbox>	_sandbox;
	string			_root;
	std::size_t		_ratio = 0;
	BaseDataMgr		_bd;
	HotMgr			_hot;
};

// ------------------------------------------------------------------ WtDataReader

TEST_F(S4R132LiveReaderBudget, LiveReaderDefaultBudgetReadsCompressedHistory)
{
	ASSERT_GT(_ratio, 5u);
	ASSERT_LT(_ratio, 1000u);
	S4LiveSink sink(&_bd, &_hot);
	WtDataReader reader;
	WTSVariant* cfg = base_cfg();
	reader.init(cfg, &sink, NULL);
	cfg->release();
	EXPECT_EQ(10u, live_read(reader, "SSE.STK.600000"));
	EXPECT_EQ(0u, sink.count_of(LL_ERROR));
}

TEST_F(S4R132LiveReaderBudget, LiveReaderConfiguredRatioCeilingRejectsTheCompressedBlock)
{
	S4LiveSink sink(&_bd, &_hot);
	WtDataReader reader;
	WTSVariant* cfg = base_cfg();
	cfg->append("uncompress_max_ratio", (uint32_t)5);
	reader.init(cfg, &sink, NULL);
	cfg->release();
	EXPECT_EQ(5u, WTSCmpHelper::uncompress_max_ratio()) << "store config must reach this module's budget";

	EXPECT_EQ(0u, live_read(reader, "SSE.STK.600000")) << "the CMP_V2 block exceeds the configured ratio";
	EXPECT_GT(sink.count_of(LL_ERROR), 0u) << "the rejected block must be reported";
	EXPECT_EQ(10u, live_read(reader, "SSE.STK.600001")) << "raw blocks are not bound by the ratio";
}

TEST_F(S4R132LiveReaderBudget, LiveReaderInvalidBudgetFailsClosedForCompressedBlocks)
{
	S4LiveSink sink(&_bd, &_hot);
	WtDataReader reader;
	WTSVariant* cfg = base_cfg();
	cfg->append("uncompress_max_ratio", (int32_t)-5);
	reader.init(cfg, &sink, NULL);
	cfg->release();
	ASSERT_GT(sink.count_of(LL_ERROR), 0u) << "an invalid budget must be diagnosed";
	EXPECT_TRUE(sink.error_has("invalid uncompress_max_ratio")) << "the ERROR names the offending key";
	EXPECT_TRUE(WTSCmpHelper::uncompress_refused());

	EXPECT_EQ(0u, live_read(reader, "SSE.STK.600000"))
		<< "an invalid budget must not fall back to an unbounded or default budget";
	EXPECT_EQ(10u, live_read(reader, "SSE.STK.600001")) << "raw blocks need no decompression";
}

// ------------------------------------------------------------------ WtRdmDtReader

TEST_F(S4R132LiveReaderBudget, RdmReaderDefaultBudgetReadsCompressedHistory)
{
	S4RdmSink sink(&_bd, &_hot);
	WtRdmDtReader reader;
	WTSVariant* cfg = base_cfg();
	reader.init(cfg, &sink);
	cfg->release();
	EXPECT_EQ(2000u, rdm_read(reader, "SSE.STK.600000"));
	EXPECT_EQ(0u, sink.count_of(LL_ERROR));
	reader.clearCache();
}

TEST_F(S4R132LiveReaderBudget, RdmReaderConfiguredBytesCeilingRejectsTheCompressedBlock)
{
	S4RdmSink sink(&_bd, &_hot);
	WtRdmDtReader reader;
	// YAML scalars arrive as strings: a digit string is a valid budget.
	WTSVariant* cfg = base_cfg();
	cfg->append("uncompress_max_bytes", "4096");
	reader.init(cfg, &sink);
	cfg->release();
	EXPECT_EQ(4096u, WTSCmpHelper::uncompress_max_bytes()) << "store config must reach this module's budget";

	EXPECT_EQ(0u, rdm_read(reader, "SSE.STK.600000")) << "declared size above the configured 4 KiB ceiling";
	EXPECT_GT(sink.count_of(LL_ERROR), 0u) << "the rejected block must be reported";
	EXPECT_EQ(2000u, rdm_read(reader, "SSE.STK.600001")) << "raw blocks are not bound by the ceiling";
	reader.clearCache();
}

TEST_F(S4R132LiveReaderBudget, RdmReaderInvalidBudgetFailsClosedForCompressedBlocks)
{
	S4RdmSink sink(&_bd, &_hot);
	WtRdmDtReader reader;
	WTSVariant* cfg = base_cfg();
	cfg->append("uncompress_max_bytes", "1e9");
	reader.init(cfg, &sink);
	cfg->release();
	ASSERT_GT(sink.count_of(LL_ERROR), 0u) << "an invalid budget must be diagnosed";
	EXPECT_TRUE(sink.error_has("invalid uncompress_max_bytes")) << "the ERROR names the offending key";
	EXPECT_TRUE(WTSCmpHelper::uncompress_refused());

	EXPECT_EQ(0u, rdm_read(reader, "SSE.STK.600000"))
		<< "an invalid budget must not fall back to an unbounded or default budget";
	EXPECT_EQ(2000u, rdm_read(reader, "SSE.STK.600001")) << "raw blocks need no decompression";
	reader.clearCache();
}

} // namespace s4r132
