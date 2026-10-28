// r212 / WtDataStorage 主存储读端残余
//
// 覆盖（src/WtDataStorage/WtRdmDtReader.cpp，init(NULL) 不启 5 秒巡检线程）：
//  - readTickSliceByDate 命中历史 tick 文件后返回切片           :315-330
//  - readTickSliceByDate 历史文件过短 -> 错误日志并继续走 RT 路径 :301-305
//  - readKlineSliceByRange 三个周期的 switch + 历史区间装配      :1826-1829 / 1985-1997
//  - readBarsFromCacheByRange 右端早于首根 -> 返回 0             :1756-1766
//  - readKlineSliceByCount 的 min1/min5 switch 与尾部截取         :2254-2257 / 2376-2392
// 覆盖（src/WtDataStorage/WtDataReader.cpp）：
//  - cacheFinalBarsFromLoader 的 m1/m5/d 三个周期名                :942-949
//
// 数据全部由 q03::Sandbox 的临时 CWD 驱动，块文件按 DataDefine.h 的真实布局拼装；
// 不触碰 dist/、不改生产代码、不使用 private 访问宏。
#include "r74_rdm_fixture.h"

#include "WTSUtils/WTSCfgLoader.h"
#include "WtDataStorage/WtDataReader.h"
#include "WtDataStorage/WtRdmDtReader.h"

namespace r212 {

using namespace q03;
using std::string;
using std::vector;

namespace {

const uint32_t kCurTDate = 20240110;

class CountingLoader : public IHisDataLoader
{
public:
	bool loadFinalHisBars(void* obj, const char* /*stdCode*/, WTSKlinePeriod period,
		FuncReadBars cb) override
	{
		_periods.push_back(period);
		vector<WTSBarStruct> bars;
		if (period == KP_DAY)
		{
			bars.push_back(make_bar(20240109, 0, 1));
			bars.push_back(make_bar(20240110, 0, 2));
		}
		else
		{
			bars.push_back(make_bar(kCurTDate, r52::min_bar_time(kCurTDate, 930), 1));
			bars.push_back(make_bar(kCurTDate, r52::min_bar_time(kCurTDate, 931), 2));
		}
		cb(obj, bars.data(), (uint32_t)bars.size());
		return true;
	}

	bool loadRawHisBars(void*, const char*, WTSKlinePeriod, FuncReadBars) override { return false; }
	bool loadAllAdjFactors(void*, FuncReadFactors) override { return false; }
	bool loadAdjFactors(void*, const char*, FuncReadFactors) override { return false; }

	const vector<WTSKlinePeriod>& periods() const { return _periods; }

private:
	vector<WTSKlinePeriod>	_periods;
};

} // namespace

class R212StorageResidualTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r212"));
		_root = _sandbox->root_abs();
		_bd.set_cur_tdate(kCurTDate);
		_rdm.init(NULL, &_rsink);
	}

	void TearDown() override
	{
		_rdm.clearCache();
		_sandbox.reset();
	}

	void init_std_reader(IHisDataLoader* loader)
	{
		const string cfgfile = _root + "reader.json";
		const string json = fmtutil::format("{{\"path\":\"{}\"}}", _root);
		ASSERT_TRUE(write_bytes(cfgfile, json));
		WTSVariant* cfg = WTSCfgLoader::load_from_file(cfgfile.c_str());
		ASSERT_TRUE(cfg != NULL);
		_std.init(cfg, &_sink, loader);
		cfg->release();
	}

	std::unique_ptr<Sandbox>	_sandbox;
	string				_root;
	BaseDataMgr			_bd;
	HotMgr				_hot;
	ReaderSink			_sink{&_bd, &_hot};
	RdmSink				_rsink{&_bd, &_hot};
	WtRdmDtReader			_rdm;
	WtDataReader			_std;
};

// WtRdmDtReader.cpp:315-330
TEST_F(R212StorageResidualTest, R212RdmHisTickByDateReturnsFileSlice)
{
	vector<WTSTickStruct> ticks;
	ticks.push_back(make_tick(kCurTDate, kCurTDate, 93000000u, 1));
	ticks.push_back(make_tick(kCurTDate, kCurTDate, 93100000u, 2));
	ASSERT_TRUE(write_bytes("his/ticks/SSE/20240110/600000.dsb",
		make_block(BT_HIS_Ticks, BLOCK_VERSION_RAW_V2, pack_ticks(ticks))));

	WTSTickSlice* s = _rdm.readTickSliceByDate("SSE.STK.600000", kCurTDate);
	ASSERT_TRUE(s != NULL);
	EXPECT_EQ(s->size(), 2u);
	EXPECT_DOUBLE_EQ(s->at(0)->price, 31.0);
	EXPECT_EQ(s->at(1)->action_time, 93100000u);
	s->release();

	// 第二次同一 key 直接命中 _his_tick_map，不再读文件
	WTSTickSlice* again = _rdm.readTickSliceByDate("SSE.STK.600000", kCurTDate);
	ASSERT_TRUE(again != NULL);
	EXPECT_EQ(again->size(), 2u);
	again->release();
}

// WtRdmDtReader.cpp:301-305
TEST_F(R212StorageResidualTest, R212RdmHisTickShortFileLogsAndFallsBack)
{
	ASSERT_TRUE(write_bytes("his/ticks/SSE/20240110/600000.dsb", string(8, 'x')));
	WTSTickSlice* s = _rdm.readTickSliceByDate("SSE.STK.600000", kCurTDate);
	EXPECT_TRUE(s == NULL);
	if (s != NULL)
		s->release();
	EXPECT_GE(_rsink.count_of(LL_ERROR), 1u);
	EXPECT_NE(_rsink.last_log().find("Sizechecking of tick data file"), string::npos);
}

// WtRdmDtReader.cpp:1826-1829 / 1985-1997 / 1756-1766
TEST_F(R212StorageResidualTest, R212RdmKlineRangeCoversThreePeriodsAndEmptyRightEdge)
{
	vector<WTSBarStruct> m1;
	for (uint32_t i = 0; i < 3; i++)
		m1.push_back(make_bar(kCurTDate, r52::min_bar_time(kCurTDate, 930 + i), i + 1));
	vector<WTSBarStruct> m5 = m1;
	vector<WTSBarStruct> d1;
	d1.push_back(make_bar(kCurTDate, 0, 11));
	d1.push_back(make_bar(kCurTDate, 0, 12));
	d1.push_back(make_bar(kCurTDate, 0, 13));

	ASSERT_TRUE(write_bytes("his/min1/SSE/600000.dsb",
		make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, pack_bars(m1))));
	ASSERT_TRUE(write_bytes("his/min5/SSE/600000.dsb",
		make_block(BT_HIS_Minute5, BLOCK_VERSION_RAW_V2, pack_bars(m5))));
	ASSERT_TRUE(write_bytes("his/day/SSE/600000.dsb",
		make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(d1))));

	WTSKlineSlice* s1 = _rdm.readKlineSliceByRange("SSE.STK.600000", KP_Minute1,
		202401100930ull, 202401101500ull);
	ASSERT_TRUE(s1 != NULL);
	EXPECT_EQ(r52::flatten_bars(s1).size(), (std::size_t)3);
	s1->release();

	WTSKlineSlice* s5 = _rdm.readKlineSliceByRange("SSE.STK.600000", KP_Minute5,
		202401100930ull, 202401101500ull);
	ASSERT_TRUE(s5 != NULL);
	EXPECT_EQ(r52::flatten_bars(s5).size(), (std::size_t)3);
	s5->release();

	WTSKlineSlice* sd = _rdm.readKlineSliceByRange("SSE.STK.600000", KP_DAY,
		202401100000ull, 202401110000ull);
	ASSERT_TRUE(sd != NULL);
	vector<WTSBarStruct> got = r52::flatten_bars(sd);
	ASSERT_EQ(got.size(), (std::size_t)3);
	EXPECT_EQ(got[2].date, kCurTDate);
	sd->release();

	// 右端落在首根之前：readBarsFromCacheByRange 返回 0，外层返回 NULL
	WTSKlineSlice* empty = _rdm.readKlineSliceByRange("SSE.STK.600000", KP_Minute1,
		202401090900ull, 202401091500ull);
	EXPECT_TRUE(empty == NULL);
	if (empty != NULL)
		empty->release();
}

// WtRdmDtReader.cpp:2254-2257 / 2376-2392
TEST_F(R212StorageResidualTest, R212RdmKlineCountCoversMinutePeriods)
{
	vector<WTSBarStruct> m1;
	for (uint32_t i = 0; i < 3; i++)
		m1.push_back(make_bar(kCurTDate, r52::min_bar_time(kCurTDate, 930 + i), i + 1));
	ASSERT_TRUE(write_bytes("his/min1/SSE/600000.dsb",
		make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, pack_bars(m1))));
	ASSERT_TRUE(write_bytes("his/min5/SSE/600000.dsb",
		make_block(BT_HIS_Minute5, BLOCK_VERSION_RAW_V2, pack_bars(m1))));

	WTSKlineSlice* s1 = _rdm.readKlineSliceByCount("SSE.STK.600000", KP_Minute1,
		2, 202401101500ull);
	ASSERT_TRUE(s1 != NULL);
	vector<WTSBarStruct> b1 = r52::flatten_bars(s1);
	ASSERT_EQ(b1.size(), (std::size_t)2);
	EXPECT_EQ((uint32_t)b1[1].date, kCurTDate);
	s1->release();

	WTSKlineSlice* s5 = _rdm.readKlineSliceByCount("SSE.STK.600000", KP_Minute5,
		2, 202401101500ull);
	ASSERT_TRUE(s5 != NULL);
	EXPECT_EQ(r52::flatten_bars(s5).size(), (std::size_t)2);
	s5->release();
}

// WtDataReader.cpp:942-949
TEST_F(R212StorageResidualTest, R212ReaderLoaderPeriodNamesAndBars)
{
	CountingLoader loader;
	init_std_reader(&loader);

	WTSKlineSlice* m1 = _std.readKlineSlice("SSE.STK.600000", KP_Minute1, 2,
		202401101500ull);
	ASSERT_TRUE(m1 != NULL);
	EXPECT_EQ(r52::flatten_bars(m1).size(), (std::size_t)2);
	m1->release();

	WTSKlineSlice* m5 = _std.readKlineSlice("SSE.STK.600000", KP_Minute5, 2,
		202401101500ull);
	ASSERT_TRUE(m5 != NULL);
	EXPECT_EQ(r52::flatten_bars(m5).size(), (std::size_t)2);
	m5->release();

	WTSKlineSlice* d1 = _std.readKlineSlice("SSE.STK.600000", KP_DAY, 2,
		202401101500ull);
	ASSERT_TRUE(d1 != NULL);
	vector<WTSBarStruct> dbars = r52::flatten_bars(d1);
	ASSERT_EQ(dbars.size(), (std::size_t)2);
	EXPECT_EQ(dbars[1].date, kCurTDate);
	d1->release();

	ASSERT_EQ(loader.periods().size(), (std::size_t)3);
	EXPECT_EQ(loader.periods()[0], KP_Minute1);
	EXPECT_EQ(loader.periods()[1], KP_Minute5);
	EXPECT_EQ(loader.periods()[2], KP_DAY);
	EXPECT_GE(_sink.count_of(LL_INFO), 3u);
}

} // namespace r212
