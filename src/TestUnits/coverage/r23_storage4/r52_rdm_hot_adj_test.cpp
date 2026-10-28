// r52 / WtRdmDtReader：主力连续（HOT）与股票复权（adjusted）两条装配大链
//
// 覆盖（src/WtDataStorage/WtRdmDtReader.cpp）：
//  - cacheHisBarsFromFile 的 HOT 分支 L1162-1363：包裹文件 lastHotTime、splitCustomSections 切段、
//    日线/分钟线两套边界（getBoundaryTime）、逐段 lower_bound、HOT+ 的 OHLC 复权、bAllCovered
//  - cacheHisBarsFromFile 的股票复权分支 L1364-1567：CMP 老结构解压升版 L1387-1426、
//    非压缩版去头 L1405-1409、原始文件按 lastQTime 续接 L1443-1560
//  - readKlineSliceByRange L1789-1998 的 ruleTag 确认与 cInfo._exright==2 的 _rt_bars 缓存分支 L1909-1981
//  - readKlineSliceByCount 的 exright==2 分支 L2316-2372
//  - readTickSliceByDate/ByRange 的 hotCode 探测 L255-265 / L403-447 与当日 RT 兜底 L337-346
//
// 手法：init(NULL, sink)（不起 5 秒轮询线程）+ q03::Sandbox 切 CWD；无访问权限宏。
#include "../q03_storage3/r52_storage_fixture.h"

#include "WtDataStorage/WtRdmDtReader.h"

namespace r52 {

using namespace q03;

const uint32_t kCurTDate = 20240110;

class R52RdmTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r52rdm"));
		_bd.set_cur_tdate(kCurTDate);
		_reader.init(NULL, &_sink);
	}

	void TearDown() override
	{
		_reader.clearCache();
		_sandbox.reset();
	}

	std::unique_ptr<Sandbox>	_sandbox;
	BDMgr		_bd;
	HotMgr		_hot;
	RdmSink		_sink{&_bd, &_hot};
	WtRdmDtReader	_reader;
};

// ---------------------------------------------------------------- HOT 日线
// WtRdmDtReader.cpp: cacheHisBarsFromFile L1162-1363 + readKlineSliceByRange L1789-1917 / L1985-1998
TEST_F(R52RdmTest, R52HotContinuousDayBarsUseSectionsAndHotTail)
{
	_hot.set_tag("HOT");
	_hot.set_raw_code("rb2402");
	_hot.add_section("rb2401", 20240102, 20240103, 1.1);
	_hot.add_section("rb2402", 20240104, 20240110, 1.2);

	// HOT 包裹文件覆盖到 20240106：切段必须从 0107 续接，0105/0106 由 hotAy 提供
	vector<WTSBarStruct> hotbars;
	hotbars.push_back(make_bar(20240105, 20240105, 1));
	hotbars.push_back(make_bar(20240106, 20240106, 2));
	ASSERT_TRUE(write_bytes("his/day/SHFE/SHFE.rb_HOT.dsb",
		make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(hotbars))));

	vector<WTSBarStruct> mon2;
	for (uint32_t i = 0; i < 5; i++)
		mon2.push_back(make_bar(20240104 + i, 20240104 + i, 11 + i));
	ASSERT_TRUE(write_bytes("his/day/SHFE/rb2402.dsb",
		make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(mon2))));

	WTSKlineSlice* slice = _reader.readKlineSliceByRange("SHFE.rb.HOT", KP_DAY,
		202401010000ull, 202401101500ull);
	ASSERT_TRUE(slice != NULL);
	vector<WTSBarStruct> got = flatten_bars(slice);
	ASSERT_EQ(got.size(), (std::size_t)4);
	EXPECT_EQ(got[0].date, 20240105u);
	EXPECT_EQ(got[1].date, 20240106u);
	EXPECT_EQ(got[2].date, 20240107u);
	EXPECT_EQ(got[3].date, 20240108u);
	EXPECT_EQ(_hot.split_calls(), 1u);
	slice->release();

	// 第二次命中 _bars_cache（L1796-1805），不再切段
	WTSKlineSlice* again = _reader.readKlineSliceByRange("SHFE.rb.HOT", KP_DAY,
		202401050000ull, 202401081500ull);
	ASSERT_TRUE(again != NULL);
	EXPECT_EQ(flatten_bars(again).size(), (std::size_t)4);
	again->release();
	EXPECT_EQ(_hot.split_calls(), 1u);

	// splitCustomSections 返回 false → cacheHisBarsFromFile 返回 false（L1202-1207）→ 空结果 NULL
	HotMgr hot2;
	hot2.set_tag("HOT");
	hot2.set_raw_code("rb2402");
	RdmSink sink2(&_bd, &hot2);
	WtRdmDtReader reader2;
	reader2.init(NULL, &sink2);
	EXPECT_TRUE(reader2.readKlineSliceByRange("SHFE.rb.HOT", KP_DAY,
		202401010000ull, 202401101500ull) == NULL);
	reader2.clearCache();
}

// ---------------------------------------------------------------- HOT 分钟线 + RT
// WtRdmDtReader.cpp: cacheHisBarsFromFile L1229-1349（分钟边界 + 切段裁剪）
//                   + readKlineSliceByRange L1860-1907（RT 直接拼接的两条左边界分支）
TEST_F(R52RdmTest, R52HotContinuousMin1SplicesRealtimeBars)
{
	_hot.set_tag("HOT");
	_hot.set_raw_code("rb2402");
	_hot.add_section("rb2402", 20240104, 20240110, 1.2);

	vector<WTSBarStruct> hotbars;
	hotbars.push_back(make_bar(20240104, min_bar_time(20240104, 930), 1));
	hotbars.push_back(make_bar(20240105, min_bar_time(20240105, 930), 2));
	ASSERT_TRUE(write_bytes("his/min1/SHFE/SHFE.rb_HOT.dsb",
		make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, pack_bars(hotbars))));

	vector<WTSBarStruct> bars;
	for (uint32_t i = 0; i < 4; i++)
		bars.push_back(make_bar(20240104 + i, min_bar_time(20240104 + i, 930), 11 + i));
	ASSERT_TRUE(write_bytes("his/min1/SHFE/rb2402.dsb",
		make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, pack_bars(bars))));

	vector<WTSBarStruct> rt;
	for (uint32_t i = 0; i < 3; i++)
		rt.push_back(make_bar(kCurTDate, min_bar_time(kCurTDate, 930 + i), 30 + i));
	ASSERT_TRUE(write_bytes("rt/min1/SHFE/rb2402.dmb",
		make_rt_block<WTSBarStruct>(BT_RT_Minute1, kCurTDate, 3, 8, rt)));

	// etime=09:31：RT 定位 idx=1，取前 2 条；历史补 4 条
	WTSKlineSlice* slice = _reader.readKlineSliceByRange("SHFE.rb.HOT", KP_Minute1,
		202401010000ull, 202401100931ull);
	ASSERT_TRUE(slice != NULL);
	vector<WTSBarStruct> got = flatten_bars(slice);
	ASSERT_EQ(got.size(), (std::size_t)6);
	EXPECT_EQ(slice->get_block_counts(), (std::size_t)2);
	// hotAy 原样（hot 文件本身已是连续价）
	EXPECT_DOUBLE_EQ(got[0].open, 11.0);
	EXPECT_DOUBLE_EQ(got[1].open, 12.0);
	// 分月段：非复权合约按原值拼接
	EXPECT_DOUBLE_EQ(got[2].open, 23.0);
	EXPECT_DOUBLE_EQ(got[3].open, 24.0);
	// RT 段直接引用 rt 块（L1886-1892）
	EXPECT_EQ(got[4].date, kCurTDate);
	EXPECT_DOUBLE_EQ(got[4].open, 40.0);
	EXPECT_EQ(got[5].time % 10000, 931u);
	slice->release();

	// stime 正压在首根 RT 上 → bNeedHisData=false，只返回 RT 两条（L1895-1905）
	WTSKlineSlice* rt_only = _reader.readKlineSliceByRange("SHFE.rb.HOT", KP_Minute1,
		202401100930ull, 202401100931ull);
	ASSERT_TRUE(rt_only != NULL);
	vector<WTSBarStruct> got2 = flatten_bars(rt_only);
	ASSERT_EQ(got2.size(), (std::size_t)2);
	EXPECT_EQ(rt_only->get_block_counts(), (std::size_t)1);
	EXPECT_DOUBLE_EQ(got2[0].open, 40.0);
	rt_only->release();
}

// ---------------------------------------------------------------- 股票复权
// WtRdmDtReader.cpp: cacheHisBarsFromFile L1364-1567
TEST_F(R52RdmTest, R52AdjustedStockBarsReadCompressedOldFileAndRawSuffix)
{
	// 1) 前复权：复权后文件是「CMP + 老结构」，原始文件是 RAW_V2
	vector<WTSBarStructOld> oldbars;
	oldbars.push_back(make_old_bar(20240104, 20240104, 1));
	oldbars.push_back(make_old_bar(20240105, 20240105, 2));
	ASSERT_TRUE(write_bytes("his/day/SSE/600000-.dsb",
		make_block(BT_HIS_Day, BLOCK_VERSION_CMP, pack_old_bars(oldbars))));

	vector<WTSBarStruct> raw;
	for (uint32_t i = 0; i < 7; i++)
		raw.push_back(make_bar(20240102 + i, 20240102 + i, 11 + i));
	ASSERT_TRUE(write_bytes("his/day/SSE/600000.dsb",
		make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(raw))));

	WTSKlineSlice* slice = _reader.readKlineSliceByRange("SSE.STK.600000-", KP_DAY,
		202401010000ull, 202401081500ull);
	ASSERT_TRUE(slice != NULL);
	vector<WTSBarStruct> got = flatten_bars(slice);
	ASSERT_EQ(got.size(), (std::size_t)5);
	// 复权后文件的末条 20240105 → 原始文件从 20240106 续接
	expect_bar_upgraded(got[0], oldbars[0]);
	expect_bar_upgraded(got[1], oldbars[1]);
	EXPECT_EQ(got[2].date, 20240106u);
	EXPECT_EQ(got[3].date, 20240107u);
	EXPECT_EQ(got[4].date, 20240108u);
	slice->release();

	// 2) 后复权：复权后文件是 RAW_V2（走 erase header 分支 L1405-1409），原始文件只多一条
	vector<WTSBarStruct> adj1;
	adj1.push_back(make_bar(20240105, 20240105, 31));
	ASSERT_TRUE(write_bytes("his/day/SSE/600001+.dsb",
		make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(adj1))));

	vector<WTSBarStruct> raw1;
	const uint32_t dates[] = {20240102, 20240103, 20240104, 20240106};
	for (uint32_t i = 0; i < 4; i++)
		raw1.push_back(make_bar(dates[i], dates[i], 41 + i));
	ASSERT_TRUE(write_bytes("his/day/SSE/600001.dsb",
		make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(raw1))));

	WTSKlineSlice* s2 = _reader.readKlineSliceByRange("SSE.STK.600001+", KP_DAY,
		202401010000ull, 202401061500ull);
	ASSERT_TRUE(s2 != NULL);
	vector<WTSBarStruct> g2 = flatten_bars(s2);
	ASSERT_EQ(g2.size(), (std::size_t)2);
	EXPECT_EQ(g2[0].date, 20240105u);
	EXPECT_EQ(g2[1].date, 20240106u);
	s2->release();
}

// WtRdmDtReader.cpp: cacheHisBarsFromFile HOT+ 的 OHLC 复权 L1334-1344
//                   + readKlineSliceByCount 的 exright==2 分支 L2316-2372
TEST_F(R52RdmTest, R52HotContinuousByCountExrightUsesRealtimeCacheBranch)
{
	_hot.set_tag("HOT");
	_hot.set_raw_code("rb2402");
	_hot.add_section("rb2402", 20240104, 20240110, 1.2);

	vector<WTSBarStruct> hotbars;
	hotbars.push_back(make_bar(20240104, min_bar_time(20240104, 930), 1));
	hotbars.push_back(make_bar(20240105, min_bar_time(20240105, 930), 2));
	ASSERT_TRUE(write_bytes("his/min1/SHFE/SHFE.rb_HOT+.dsb",
		make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, pack_bars(hotbars))));

	vector<WTSBarStruct> bars;
	for (uint32_t i = 0; i < 4; i++)
		bars.push_back(make_bar(20240104 + i, min_bar_time(20240104 + i, 930), 11 + i));
	ASSERT_TRUE(write_bytes("his/min1/SHFE/rb2402.dsb",
		make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, pack_bars(bars))));

	vector<WTSBarStruct> rt;
	for (uint32_t i = 0; i < 3; i++)
		rt.push_back(make_bar(kCurTDate, min_bar_time(kCurTDate, 930 + i), 30 + i));
	ASSERT_TRUE(write_bytes("rt/min1/SHFE/rb2402.dmb",
		make_rt_block<WTSBarStruct>(BT_RT_Minute1, kCurTDate, 3, 8, rt)));

	WTSKlineSlice* slice = _reader.readKlineSliceByCount("SHFE.rb.HOT+", KP_Minute1, 5,
		202401100931ull);
	ASSERT_TRUE(slice != NULL);
	vector<WTSBarStruct> got = flatten_bars(slice);
	ASSERT_EQ(got.size(), (std::size_t)5);
	// his 尾窗 3 条：hot 0105 原样、分月 0106/0107 OHLC*1.2
	EXPECT_EQ(got[0].date, 20240105u);
	EXPECT_DOUBLE_EQ(got[0].open, 12.0);
	EXPECT_DOUBLE_EQ(got[1].open, 23.0 * 1.2);
	EXPECT_DOUBLE_EQ(got[2].open, 24.0 * 1.2);
	// RT 两条在 _rt_bars 副本上按 barList._factor=1.2 放大
	EXPECT_EQ(got[3].date, kCurTDate);
	EXPECT_DOUBLE_EQ(got[3].open, 40.0 * 1.2);
	EXPECT_EQ(got[4].time % 10000, 931u);
	slice->release();

	// 第二次：_rt_bars 条数未变 → 跳过 memcpy（L2329 为假），仍返回 5 条
	WTSKlineSlice* again = _reader.readKlineSliceByCount("SHFE.rb.HOT+", KP_Minute1, 5,
		202401100931ull);
	ASSERT_TRUE(again != NULL);
	EXPECT_EQ(flatten_bars(again).size(), (std::size_t)5);
	again->release();
}

// ---------------------------------------------------------------- tick HOT 代码
// WtRdmDtReader.cpp: readTickSliceByDate L250-332（hotCode 探测/当日 RT 兜底）
//                   + readTickSliceByRange L398-447
TEST_F(R52RdmTest, R52HotTickCodeProbesCustomRawFileAndRealtimeBlock)
{
	_hot.set_tag("HOT");
	_hot.set_raw_code("rb2401");

	vector<WTSTickStruct> d08;
	d08.push_back(make_tick(20240108, 20240108, 93000000, 1));
	d08.push_back(make_tick(20240108, 20240108, 103000000, 2));
	ASSERT_TRUE(write_bytes("his/ticks/SHFE/20240108/rb_HOT.dsb",
		make_block(BT_HIS_Ticks, BLOCK_VERSION_RAW_V2, pack_ticks(d08))));

	WTSTickSlice* byDate = _reader.readTickSliceByDate("SHFE.rb.HOT", 20240108);
	ASSERT_TRUE(byDate != NULL);
	EXPECT_EQ(flatten_ticks(byDate).size(), (std::size_t)2);
	byDate->release();
	EXPECT_TRUE(_sink.count_of(LL_INFO) >= 1u);

	WTSTickSlice* byRange = _reader.readTickSliceByRange("SHFE.rb.HOT",
		20240108090000000ull, 20240108150000000ull);
	ASSERT_TRUE(byRange != NULL);
	vector<WTSTickStruct> got = flatten_ticks(byRange);
	ASSERT_EQ(got.size(), (std::size_t)2);
	EXPECT_EQ(got[0].action_time, 93000000u);
	EXPECT_EQ(got[1].action_time, 103000000u);
	byRange->release();

	// 当日：his 两个候选名都缺失 → getRTTickBlock(custom raw code) 兜底
	vector<WTSTickStruct> rt;
	for (uint32_t i = 0; i < 3; i++)
		rt.push_back(make_tick(kCurTDate, kCurTDate, 93000000 + i * 1000000, 10 + i));
	ASSERT_TRUE(write_bytes("rt/ticks/SHFE/rb2401.dmb",
		make_rt_block<WTSTickStruct>(BT_RT_Ticks, kCurTDate, 3, 8, rt)));

	WTSTickSlice* today = _reader.readTickSliceByDate("SHFE.rb.HOT", kCurTDate);
	ASSERT_TRUE(today != NULL);
	EXPECT_EQ(flatten_ticks(today).size(), (std::size_t)3);
	today->release();
}

// ---------------------------------------------------------------- 缺陷复现（不进启用集）
// BUG-R52-003：exright==2 首次进 RT 分支时 oldSize==0、idx==0，L1930 的
//   memcpy(..., sizeof(WTSBarStruct) * (newSize - oldSize + 1)) 比 resize 后的
//   vector 多写一条 → 堆越界写。
// BUG-R52-002：尾定位用 lower_bound(begin, end) 全区间，etime 晚于全部 RT 条时
//   idx==size，L1952 `&_rt_bars[idx]` 越界读，L1963-1964 rtCnt=idx+1=size+1 多挂一条。
// ByRange L1922-1965 与 ByCount L2352-2369 同型；本用例走 ByRange 首次调用，两坑同时命中。
TEST_F(R52RdmTest, DISABLED_R52ExrightRealtimeRangeBeyondTailOverrunsVector)
{
	vector<WTSBarStruct> rt;
	for (uint32_t i = 0; i < 2; i++)
		rt.push_back(make_bar(kCurTDate, min_bar_time(kCurTDate, 930 + i), 10 + i));
	ASSERT_TRUE(write_bytes("rt/min1/SSE/600000.dmb",
		make_rt_block<WTSBarStruct>(BT_RT_Minute1, kCurTDate, 2, 8, rt)));

	WTSKlineSlice* slice = _reader.readKlineSliceByRange("SSE.STK.600000+", KP_Minute1,
		202401010000ull, 202401101500ull);
	if (slice != NULL)
		slice->release();
	_reader.clearCache();
}

} // namespace r52
