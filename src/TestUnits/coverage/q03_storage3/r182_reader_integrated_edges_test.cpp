// r182 / WtDataReader：主力连续（cacheIntegratedBars）剩余边界
//
// 覆盖（src/WtDataStorage/WtDataReader.cpp）：
//  - 无 HOT 包裹文件时 splitCustomSections 返回 false           :1063-1065
//  - split 成功但切段为空                                        :1068-1069
//  - 切段合约文件不存在 -> continue                              :1162-1163
//  - 切段文件小于 HisKlineBlock -> 记错误并返回 false            :1167-1171
//  - 切段左边界晚于文件全部K线 -> lower_bound 后 continue        :1196-1199
//  - 切段右边界早于文件第一根K线 -> eIdx==0 break                :1214-1215
//  - 正常切段：sIdx/eIdx 区间装配（行为基线）                    :1183-1224
//
// 手法：q03::Sandbox + 真实 init(cfg, sink, NULL)（loader 为空，走文件分支）；
// r74::HotMgr 提供 HOT 规则标签与可控切段。HOT 包裹文件一律不创建，
// 绕开 BUG-R52-001（内层 filename 遮蔽导致包裹文件读取必然抛异常）的死路径。
#include "r74_rdm_fixture.h"

#include "WTSUtils/WTSCfgLoader.h"
#include "WtDataStorage/WtDataReader.h"

namespace r182 {

using namespace q03;
using std::string;
using std::vector;

class R182ReaderIntegratedTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r182int"));
		_root = _sandbox->root_abs();
		_hot.set_enabled(true);
	}

	void TearDown() override
	{
		_sandbox.reset();
	}

	void init_reader()
	{
		const string cfgfile = _root + "reader.json";
		const string json = fmtutil::format("{{\"path\":\"{}\",\"adjust_flag\":0}}", _root);
		ASSERT_TRUE(write_bytes(cfgfile, json));
		WTSVariant* cfg = WTSCfgLoader::load_from_file(cfgfile.c_str());
		ASSERT_TRUE(cfg != NULL);
		_reader.init(cfg, &_sink, NULL);
		cfg->release();
	}

	void write_section_bars(const char* code, const vector<WTSBarStruct>& bars)
	{
		const string file = fmtutil::format("his/min1/SHFE/{}.dsb", code);
		ASSERT_TRUE(write_bytes(file,
			make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, pack_bars(bars))));
	}

	std::unique_ptr<Sandbox>	_sandbox;
	string				_root;
	r52::BDMgr			_bd;
	r74::HotMgr			_hot;
	ReaderSink			_sink{&_bd, &_hot};
	WtDataReader			_reader;
};

// WtDataReader.cpp:1063-1065；无包裹文件，切段接口直接返回 false
TEST_F(R182ReaderIntegratedTest, R182NoHotFileSplitRejected)
{
	_hot.set_split_ret(false);
	init_reader();

	WTSKlineSlice* slice = _reader.readKlineSlice("SHFE.rb.HOT", KP_Minute1, 100, 202401101500ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->size(), 0u);
	EXPECT_EQ(_hot.split_calls(), 1u);
	slice->release();
}

// WtDataReader.cpp:1068-1069；split 成功但没有切段
TEST_F(R182ReaderIntegratedTest, R182HotSplitReturnsEmptySections)
{
	_hot.set_split_empty(true);
	init_reader();

	WTSKlineSlice* slice = _reader.readKlineSlice("SHFE.rb.HOT", KP_Minute1, 100, 202401101500ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->size(), 0u);
	EXPECT_EQ(_hot.split_calls(), 1u);
	slice->release();
}

// WtDataReader.cpp:1162-1163；切段合约的历史文件不存在
TEST_F(R182ReaderIntegratedTest, R182SectionFileMissingIsSkipped)
{
	_hot.add_section("rb2401", 20240102, 20240103, 1.1);
	init_reader();

	WTSKlineSlice* slice = _reader.readKlineSlice("SHFE.rb.HOT", KP_Minute1, 100, 202401101500ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->size(), 0u);
	EXPECT_EQ(_hot.split_calls(), 1u);
	slice->release();
}

// WtDataReader.cpp:1167-1171；切段文件存在但小于 HisKlineBlock
TEST_F(R182ReaderIntegratedTest, R182ShortSectionFileLogsAndFails)
{
	_hot.add_section("rb2401", 20240102, 20240103, 1.1);
	ASSERT_TRUE(write_bytes("his/min1/SHFE/rb2401.dsb", string(8, 'x')));
	init_reader();

	WTSKlineSlice* slice = _reader.readKlineSlice("SHFE.rb.HOT", KP_Minute1, 100, 202401101500ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->size(), 0u);
	EXPECT_GE(_sink.count_of(LL_ERROR), 1u);
	slice->release();
}

// WtDataReader.cpp:1196-1199；切段左边界晚于文件内全部K线 -> lower_bound 落在末根后 continue
TEST_F(R182ReaderIntegratedTest, R182SectionWindowAfterAllBarsIsSkipped)
{
	_hot.add_section("rb2401", 20240201, 20240201, 1.0);
	vector<WTSBarStruct> bars;
	bars.push_back(make_bar(20240102, r52::min_bar_time(20240102, 930), 1));
	bars.push_back(make_bar(20240102, r52::min_bar_time(20240102, 931), 2));
	write_section_bars("rb2401", bars);
	init_reader();

	WTSKlineSlice* slice = _reader.readKlineSlice("SHFE.rb.HOT", KP_Minute1, 100, 202401101500ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->size(), 0u);
	slice->release();
}

// WtDataReader.cpp:1214-1215；切段右边界早于文件第一根K线 -> eIdx==0 break
TEST_F(R182ReaderIntegratedTest, R182SectionWindowBeforeFirstBarBreaks)
{
	_hot.add_section("rb2401", 20240101, 20240101, 1.0);
	vector<WTSBarStruct> bars;
	bars.push_back(make_bar(20240102, r52::min_bar_time(20240102, 930), 1));
	bars.push_back(make_bar(20240102, r52::min_bar_time(20240102, 931), 2));
	write_section_bars("rb2401", bars);
	init_reader();

	WTSKlineSlice* slice = _reader.readKlineSlice("SHFE.rb.HOT", KP_Minute1, 100, 202401101500ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->size(), 0u);
	slice->release();
}

// WtDataReader.cpp:1183-1224；正常切段，验证边界裁剪与装配
TEST_F(R182ReaderIntegratedTest, R182SectionWindowSelectsBothBars)
{
	_hot.add_section("rb2401", 20240102, 20240102, 1.0);
	vector<WTSBarStruct> bars;
	bars.push_back(make_bar(20240102, r52::min_bar_time(20240102, 930), 1));
	bars.push_back(make_bar(20240102, r52::min_bar_time(20240102, 931), 2));
	write_section_bars("rb2401", bars);
	init_reader();

	WTSKlineSlice* slice = _reader.readKlineSlice("SHFE.rb.HOT", KP_Minute1, 100,
		(uint64_t)20240110 * 10000ull + 1500ull);
	ASSERT_TRUE(slice != NULL);
	ASSERT_EQ(slice->size(), 2u);
	EXPECT_EQ(slice->at(0)->time, r52::min_bar_time(20240102, 930));
	EXPECT_EQ(slice->at(1)->time, r52::min_bar_time(20240102, 931));
	slice->release();
}

} // namespace r182
