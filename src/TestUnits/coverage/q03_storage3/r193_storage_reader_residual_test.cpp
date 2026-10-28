// r193 / WtDataStorage 读端残余：loader 路径与后复权实时块裁剪
//
// 覆盖（src/WtDataStorage/WtDataReader.cpp）：
//  - cacheFinalBarsFromLoader 的未知周期 pname 默认值 :948
// 覆盖（src/WtDataStorage/WtRdmDtReader.cpp）：
//  - readKlineSliceByRange 后复权分支 eBar 落在 RT 两根 K 线之间时的回退 :1954-1957
//  - readKlineSliceByCount 后复权分支 RT 缓存增长 oldSize!=0 与 eBar 回退 :2334 / :2361-2364
//  - getRTTickBlock 当日 RT tick 块缺失时返回 NULL :2152
//
// 手法：r52 夹具的 BDMgr/Loader/ReaderSink + q03::Sandbox；Rdm reader 用
// init(NULL, sink)（不起 5 秒轮询线程），实时块用 make_rt_block 写成合法 mmap 文件。
#include "r52_storage_fixture.h"

#include "WtDataStorage/WtDataReader.h"
#include "WtDataStorage/WtRdmDtReader.h"

namespace r193 {

using namespace q03;
using std::string;
using std::vector;

class R193StorageReaderTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r193rd"));
		_root = _sandbox->root_abs();
		_bd.set_cur_tdate(20240110);
		_rdm.init(NULL, &_qsink);
	}

	void TearDown() override
	{
		_rdm.clearCache();
		_sandbox.reset();
	}

	void init_reader(r52::Loader* loader)
	{
		const string cfgfile = _root + "reader.json";
		const string json = fmtutil::format("{{\"path\":\"{}\",\"adjust_flag\":0}}", _root);
		ASSERT_TRUE(write_bytes(cfgfile, json));
		WTSVariant* cfg = WTSCfgLoader::load_from_file(cfgfile.c_str());
		ASSERT_TRUE(cfg != NULL);
		_reader.init(cfg, &_sink, loader);
		cfg->release();
	}

	void write_rt_min1(const char* code, const vector<WTSBarStruct>& bars, uint32_t capacity)
	{
		const string file = fmtutil::format("rt/min1/SSE/{}.dmb", code);
		ASSERT_TRUE(write_bytes(file,
			make_rt_block<WTSBarStruct>(BT_RT_Minute1, 20240110, (uint32_t)bars.size(), capacity, bars)));
	}

	std::unique_ptr<Sandbox>	_sandbox;
	string						_root;
	r52::BDMgr					_bd;
	HotMgr						_hot;
	r52::ReaderSink				_sink{&_bd, &_hot};
	RdmSink						_qsink{&_bd, &_hot};
	WtDataReader				_reader;
	WtRdmDtReader				_rdm;
};

// WtDataReader.cpp:948；扩展 loader 对 KP_Tick 也返回数据，pname 走 default
TEST_F(R193StorageReaderTest, R193LoaderUnknownPeriodUsesDayName)
{
	r52::Loader loader;
	vector<WTSBarStruct> bars;
	bars.push_back(make_bar(20240108, 202401081500, 1));
	loader.add_final("SSE.STK.600030", bars);
	init_reader(&loader);

	WTSKlineSlice* slice = _reader.readKlineSlice("SSE.STK.600030", KP_Tick, 10,
		202401081500ull);
	ASSERT_TRUE(slice != NULL);
	ASSERT_EQ(slice->size(), 1u);
	slice->release();
}

// WtRdmDtReader.cpp:1954-1957；后复权：RT 为 09:30/09:32，查询截止 09:31
TEST_F(R193StorageReaderTest, R193ExrightRangeClipsBarAfterEndTime)
{
	vector<WTSBarStruct> rt;
	rt.push_back(make_bar(20240110, r52::min_bar_time(20240110, 930), 1));
	rt.push_back(make_bar(20240110, r52::min_bar_time(20240110, 932), 2));
	write_rt_min1("600010", rt, 8);

	WTSKlineSlice* slice = _rdm.readKlineSliceByRange("SSE.STK.600010+", KP_Minute1,
		202401100900ull, 202401100931ull);
	ASSERT_TRUE(slice != NULL);
	vector<WTSBarStruct> got = r52::flatten_bars(slice);
	ASSERT_EQ(got.size(), (std::size_t)1);
	EXPECT_EQ(got[0].time % 10000, 930u);
	slice->release();
}

// WtRdmDtReader.cpp:2334 / 2361-2364；后复权按数量读：RT 从 2 根增长到 4 根，
// 且 eBar=09:31 落在 09:30/09:32 之间
TEST_F(R193StorageReaderTest, R193ExrightCountGrowsCacheAndClipsGap)
{
	vector<WTSBarStruct> rt2;
	rt2.push_back(make_bar(20240110, r52::min_bar_time(20240110, 930), 1));
	rt2.push_back(make_bar(20240110, r52::min_bar_time(20240110, 932), 2));
	write_rt_min1("600011", rt2, 8);

	WTSKlineSlice* first = _rdm.readKlineSliceByCount("SSE.STK.600011+", KP_Minute1, 1,
		202401100931ull);
	ASSERT_TRUE(first != NULL);
	vector<WTSBarStruct> got1 = r52::flatten_bars(first);
	ASSERT_EQ(got1.size(), (std::size_t)1);
	EXPECT_EQ(got1[0].time % 10000, 930u);
	first->release();

	// 同容量、同文件长度原地改写：已有 mmap 直接看到 _size=4
	vector<WTSBarStruct> rt4 = rt2;
	rt4.push_back(make_bar(20240110, r52::min_bar_time(20240110, 933), 3));
	rt4.push_back(make_bar(20240110, r52::min_bar_time(20240110, 934), 4));
	write_rt_min1("600011", rt4, 8);

	WTSKlineSlice* second = _rdm.readKlineSliceByCount("SSE.STK.600011+", KP_Minute1, 1,
		202401100931ull);
	ASSERT_TRUE(second != NULL);
	vector<WTSBarStruct> got2 = r52::flatten_bars(second);
	ASSERT_EQ(got2.size(), (std::size_t)1);
	EXPECT_EQ(got2[0].time % 10000, 930u);
	second->release();
}

// WtRdmDtReader.cpp:2152；当日 RT tick 文件不存在，返回空切片而不是崩溃
TEST_F(R193StorageReaderTest, R193MissingRealtimeTickBlockYieldsEmptySlice)
{
	WTSTickSlice* slice = _rdm.readTickSliceByRange("SSE.STK.600040",
		20240110090000000ull, 20240110150000000ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->size(), 0u);
	slice->release();
}

} // namespace r193
