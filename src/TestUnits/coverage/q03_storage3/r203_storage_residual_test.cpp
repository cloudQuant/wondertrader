// r203 / 主存储读端残余：跨类型缓存守卫、后复权文件因子、反向切段防御分支
//
// 覆盖（src/WtDataStorage/WtDataReader.cpp）：
//  - readOrdDtlSlice 在 _his_ordque_map 已存在 key 时跳过装载，_his_orddtl_map 为空返回 NULL :741-743
//  - readTransSlice 同型守卫（误查 _his_ordque_map）                                        :898-900
//  - cacheIntegratedBars 反向切段（左边界晚于右边界）后 eIdx < sIdx -> continue             :1211-1222
//
// 覆盖（src/WtDataStorage/WtRdmDtReader.cpp）：
//  - readOrdDtlSliceByRange / readTransSliceByRange 同型缓存守卫                            :913-915 / :1087-1089
//  - cacheHisBarsFromFile 后复权文件：barList._factor 取最后一档因子                        :1506-1513
//  - cacheHisBarsFromFile 反向切段后 eIdx < sIdx -> continue                                :1329-1330
//
// 全部走公开 API；临时目录由 q03::Sandbox 提供，文件均为合法 RAW_V2 块。
#include "r74_rdm_fixture.h"

#include "WTSUtils/WTSCfgLoader.h"
#include "WtDataStorage/WtDataReader.h"
#include "WtDataStorage/WtRdmDtReader.h"

namespace r203 {

using namespace q03;
using std::string;
using std::vector;

class R203StorageResidualTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r203"));
		_root = _sandbox->root_abs();
	}

	void TearDown() override
	{
		_sandbox.reset();
	}

	void init_reader()
	{
		const string cfgfile = _root + "reader.json";
		const string json = fmtutil::format("{{\"path\":\"{}\"}}", _root);
		ASSERT_TRUE(write_bytes(cfgfile, json));
		WTSVariant* cfg = WTSCfgLoader::load_from_file(cfgfile.c_str());
		ASSERT_TRUE(cfg != NULL);
		_reader.init(cfg, &_sink, NULL);
		cfg->release();
	}

	void write_section_bars(const char* exchg, const char* code, const vector<WTSBarStruct>& bars)
	{
		const string file = fmtutil::format("his/min1/{}/{}.dsb", exchg, code);
		ASSERT_TRUE(write_bytes(file,
			make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, pack_bars(bars))));
	}

	std::unique_ptr<Sandbox>	_sandbox;
	string				_root;
	r52::BDMgr			_bd;
	r74::HotMgr			_hot;
	ReaderSink			_sink{&_bd, &_hot};
	RdmSink				_rsink{&_bd, &_hot};
	WtDataReader			_reader;
};

// WtDataReader.cpp:741-743 / 898-900
// 短队列文件让 readOrdQueSlice 在 _his_ordque_map 里留下 key；随后 dtl/trans
// 因误用该 map 判命中而跳过文件装载，各自 map 为空，返回 NULL。
TEST_F(R203StorageResidualTest, R203ReaderQueueCacheEntryLeavesDtlAndTransNull)
{
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240109/600000.dsb", string(8, 'x')));
	init_reader();

	const uint64_t etime = 20240109150000000ull;

	WTSOrdQueSlice* que = _reader.readOrdQueSlice("SSE.600000", 5, etime);
	EXPECT_TRUE(que == NULL);
	if (que != NULL)
		que->release();
	EXPECT_GE(_sink.count_of(LL_ERROR), 1u);

	WTSOrdDtlSlice* dtl = _reader.readOrdDtlSlice("SSE.600000", 5, etime);
	EXPECT_TRUE(dtl == NULL);
	if (dtl != NULL)
		dtl->release();

	WTSTransSlice* trans = _reader.readTransSlice("SSE.600000", 5, etime);
	EXPECT_TRUE(trans == NULL);
	if (trans != NULL)
		trans->release();
}

// WtRdmDtReader.cpp:913-915 / 1087-1089
// 同型守卫：短队列文件留下 key 后，逐笔委托/成交入口不再读文件，空块返回 NULL。
TEST_F(R203StorageResidualTest, R203RdmQueueCacheEntryLeavesDtlAndTransNull)
{
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240109/600000.dsb", string(8, 'x')));

	WtRdmDtReader reader;
	reader.init(NULL, &_rsink);

	const uint64_t stime = 20240109090000000ull;
	const uint64_t etime = 20240109150000000ull;

	WTSOrdQueSlice* que = reader.readOrdQueSliceByRange("SSE.600000", stime, etime);
	EXPECT_TRUE(que == NULL);
	if (que != NULL)
		que->release();

	WTSOrdDtlSlice* dtl = reader.readOrdDtlSliceByRange("SSE.600000", stime, etime);
	EXPECT_TRUE(dtl == NULL);
	if (dtl != NULL)
		dtl->release();

	WTSTransSlice* trans = reader.readTransSliceByRange("SSE.600000", stime, etime);
	EXPECT_TRUE(trans == NULL);
	if (trans != NULL)
		trans->release();
}

// WtRdmDtReader.cpp:1506-1513
// 后复权（+）文件读取：adjfactor 配置提供 20240101=2.0，文件两根 bar 的 OHLC 应翻倍；
// 同时命中 _exright == 2 时 barList._factor 取最后因子的分支。
TEST_F(R203StorageResidualTest, R203RdmPostAdjustedFileScalesBars)
{
	const string adjfile = _root + "adjfactor.json";
	ASSERT_TRUE(write_bytes(adjfile,
		"{\"SSE\":{\"STK.600000\":[{\"date\":20240101,\"factor\":2.0}]}}"));

	const string cfgfile = _root + "rdm.json";
	const string json = fmtutil::format(
		"{{\"path\":\"{}\",\"adjfactor\":\"{}\"}}", _root, adjfile);
	ASSERT_TRUE(write_bytes(cfgfile, json));
	WTSVariant* cfg = WTSCfgLoader::load_from_file(cfgfile.c_str());
	ASSERT_TRUE(cfg != NULL);

	WtRdmDtReader reader;
	reader.init(cfg, &_rsink);
	cfg->release();

	vector<WTSBarStruct> bars;
	bars.push_back(make_bar(20240102, r52::min_bar_time(20240102, 930), 1));
	bars.push_back(make_bar(20240102, r52::min_bar_time(20240102, 931), 2));
	ASSERT_TRUE(write_bytes("his/min1/SSE/600000.dsb",
		make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, pack_bars(bars))));

	WTSKlineSlice* slice = reader.readKlineSliceByCount("SSE.STK.600000+",
		KP_Minute1, 2, 202401101500ull);
	ASSERT_TRUE(slice != NULL);
	vector<WTSBarStruct> got = r52::flatten_bars(slice);
	slice->release();
	ASSERT_EQ(got.size(), 2u);
	EXPECT_DOUBLE_EQ(got[0].open, 22.0);	// 11.0 * 2
	EXPECT_DOUBLE_EQ(got[0].high, 24.0);	// 12.0 * 2
	EXPECT_DOUBLE_EQ(got[0].low, 20.0);	// 10.0 * 2
	EXPECT_DOUBLE_EQ(got[0].close, 23.0);	// 11.5 * 2
	EXPECT_DOUBLE_EQ(got[1].close, 25.0);	// 12.5 * 2
}

// WtDataReader.cpp:1211-1222
// 反向切段（sdate 晚于 edate）：sIdx 落在第二根，eBar 定位后回退一格使 eIdx < sIdx，
// 走 continue 跳过该段，最终返回非 NULL 空切片。
TEST_F(R203StorageResidualTest, R203ReaderReversedSectionSkipsWindow)
{
	_hot.set_enabled(true);
	_hot.add_section("rb2401", 20240103, 20240102, 1.0);

	vector<WTSBarStruct> bars;
	bars.push_back(make_bar(20240102, r52::min_bar_time(20240102, 930), 1));
	bars.push_back(make_bar(20240103, r52::min_bar_time(20240103, 930), 2));
	write_section_bars("SHFE", "rb2401", bars);
	init_reader();

	WTSKlineSlice* slice = _reader.readKlineSlice("SHFE.rb.HOT", KP_Minute1, 100,
		202401101500ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->size(), 0u);
	slice->release();
}

// WtRdmDtReader.cpp:1329-1330
// 同一反向切段喂给 cacheHisBarsFromFile：eIdx 回退到 sIdx 之前，走 continue；
// 历史缓存为空，区间读取返回 NULL（ByRange 的 hisCnt 初值为 0，不会带空指针造切片）。
TEST_F(R203StorageResidualTest, R203RdmReversedSectionSkipsWindow)
{
	_hot.set_enabled(true);
	_hot.add_section("rb2401", 20240103, 20240102, 1.0);

	vector<WTSBarStruct> bars;
	bars.push_back(make_bar(20240102, r52::min_bar_time(20240102, 930), 1));
	bars.push_back(make_bar(20240103, r52::min_bar_time(20240103, 930), 2));
	write_section_bars("SHFE", "rb2401", bars);

	WtRdmDtReader reader;
	reader.init(NULL, &_rsink);

	WTSKlineSlice* slice = reader.readKlineSliceByRange("SHFE.rb.HOT", KP_Minute1,
		(uint64_t)20240102 * 10000ull + 900ull, (uint64_t)20240103 * 10000ull + 1500ull);
	EXPECT_TRUE(slice == NULL);
	if (slice != NULL)
		slice->release();
}

} // namespace r203
