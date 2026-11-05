// Iteration 03 / card 13: reader visibility rule of the RT publish protocol, exercised on the
// real WtDataReader. The state below is what an independent reader observes when it mapped
// a block before another process extended it and then read the newly published header:
// the header announces capacity/size beyond the reader's own mapping.
#include "q03_storage3_fixture.h"

#include "WTSUtils/WTSCfgLoader.h"
#include "WtDataStorage/WtDataReader.h"

#include <fstream>

namespace q03 {

class MwReaderVisibility : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("mwrv"));
		_root = _sandbox->root_abs();
		string cfgfile = _root + "reader.json";
		string json = fmtutil::format("{{\"path\":\"{}\",\"adjust_flag\":0}}", _root);
		ASSERT_TRUE(write_bytes(cfgfile, json));
		WTSVariant* cfg = WTSCfgLoader::load_from_file(cfgfile.c_str());
		ASSERT_TRUE(cfg != NULL);
		_reader.init(cfg, &_sink, NULL);
		cfg->release();
	}
	void TearDown() override { _sandbox.reset(); }

	static vector<WTSTickStruct> flatten(WTSTickSlice* slice)
	{
		vector<WTSTickStruct> ret;
		if (slice == NULL)
			return ret;
		for (std::size_t i = 0; i < slice->get_block_counts(); i++)
		{
			WTSTickStruct* addr = slice->get_block_addr(i);
			for (uint32_t k = 0; addr != NULL && k < slice->get_block_size(i); k++)
				ret.push_back(addr[k]);
		}
		return ret;
	}

	std::unique_ptr<Sandbox>	_sandbox;
	string		_root;
	BaseDataMgr	_bd;
	HotMgr		_hot;
	ReaderSink	_sink{ &_bd, &_hot };
	WtDataReader	_reader;
};

TEST_F(MwReaderVisibility, ClampsToOwnMappingThenRemapsWhenPublishedCapacityGrows)
{
	const uint32_t physical = 48, published = 100, capacity = 128;
	vector<WTSTickStruct> all;
	for (uint32_t i = 0; i < published; ++i)
		all.push_back(make_tick(20240110, 20240110, 90000000 + i * 1000, i + 1));

	// Header already announces size=100 / capacity=128, but the file (and thus a mapping
	// taken now) only covers 48 records.
	vector<WTSTickStruct> head(all.begin(), all.begin() + physical);
	string bytes = make_rt_block<WTSTickStruct>(BT_RT_Ticks, 20240110, published, capacity, head);
	bytes.resize(sizeof(RTDayBlockHeader) + physical * sizeof(WTSTickStruct));
	const string rel = "rt/ticks/SSE/600000.dmb";
	ASSERT_TRUE(write_bytes(rel, bytes));

	WTSTickSlice* s1 = _reader.readTickSlice("SSE.STK.600000", 5, 20240110150000000ull);
	ASSERT_TRUE(s1 != NULL);
	vector<WTSTickStruct> got = flatten(s1);
	ASSERT_EQ(got.size(), 5u);
	expect_tick_eq(got.back(), all[physical - 1]);	// never beyond the own mapping
	s1->release();

	// The other process finishes its extension: the file now covers the full capacity.
	string full = make_rt_block<WTSTickStruct>(BT_RT_Ticks, 20240110, published, capacity, all);
	{
		std::fstream f(_root + rel, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE((bool)f);
		f.seekp(0, std::ios::end);
		f.write(full.data() + bytes.size(), (std::streamsize)(full.size() - bytes.size()));
	}

	// Header capacity is unchanged (128) but exceeds the reader's mapping: it must remap.
	WTSTickSlice* s2 = _reader.readTickSlice("SSE.STK.600000", 5, 20240110150000000ull);
	ASSERT_TRUE(s2 != NULL);
	got = flatten(s2);
	ASSERT_EQ(got.size(), 5u);
	expect_tick_eq(got.back(), all[published - 1]);
	s2->release();
}

TEST_F(MwReaderVisibility, EmptyPublishedBlockReturnsNoSlice)
{
	vector<WTSTickStruct> none;
	ASSERT_TRUE(write_bytes("rt/ticks/SSE/600000.dmb", make_rt_block<WTSTickStruct>(BT_RT_Ticks, 20240110, 0, 8, none)));
	EXPECT_TRUE(_reader.readTickSlice("SSE.STK.600000", 5, 20240110150000000ull) == NULL);
}

}	// namespace q03
