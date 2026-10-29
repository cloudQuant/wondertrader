// r52 / WtDataReader：主力连续（integrated）与股票复权（adjusted）两条装配大链
//
// 覆盖（src/WtDataStorage/WtDataReader.cpp）：
//  - cacheIntegratedBars   L969-1285   his/day(|min1)/<exchg>/<exchg>.<pid>_<tag><suf>.dsb 探测、
//                                     splitCustomSections 切段、逐段 lower_bound 裁剪、复权因子
//                                     （_adjust_flag 的 1/2/4 位）、hotAy 拼接与逆序装配
//  - cacheAdjustedStkBars  L1287-1562  除权因子文件加载（L252-320）、复权后文件 lastQTime、
//                                     原始文件按因子倒序回放、qfq/hfq 的 baseFactor
//  - cacheFinalBarsFromLoader L930-966 / loadStkAdjFactorsFromLoader L219-250 / getAdjFactors L2349-2391
//  - readKlineSlice         L1720-1917  _raw_code 确认、KP_DAY 的 RT 空分支、尾窗截取
//  - onMinuteEnd            L2210-2311  rt/min1 增量回放（复权分支 + 游标推进 + 早退）
//  - readOrdQueSlice/readOrdDtlSlice/readTransSlice 的 isToday 分支 L499-612 / L656-769 / L813-926
//  - getRTTickBlock / getRTOrdQue/OrdDtl/Trans/KilneBlock 的容量变化重映射分支
//
// 手法：真实 init(cfg, sink, loader) + q03::Sandbox 临时目录；无访问权限宏。
// 已知缺陷（见 bugs/R52.md BUG-R52-001）：cacheIntegratedBars 的 HOT 文件读取变量被内层同名
// 变量遮蔽（L1019-1041）。相关回归按正确的数据返回契约断言，当前产品缺陷应使其失败。
#include "r52_storage_fixture.h"

#include "WTSUtils/WTSCfgLoader.h"
#include "WtDataStorage/WtDataReader.h"

namespace r52 {

class R52ReaderTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new q03::Sandbox("r52rd"));
		_root = _sandbox->root_abs();
	}

	void TearDown() override
	{
		_sandbox.reset();
	}

	void init_reader(uint32_t adjust_flag = 0, const char* extra = "")
	{
		const string cfgfile = _root + "reader.json";
		const string json = fmtutil::format("{{\"path\":\"{}\",\"adjust_flag\":{}{}}}",
			_root, adjust_flag, extra);
		ASSERT_TRUE(q03::write_bytes(cfgfile, json));
		WTSVariant* cfg = WTSCfgLoader::load_from_file(cfgfile.c_str());
		ASSERT_TRUE(cfg != NULL);
		_reader.init(cfg, &_sink, NULL);
		cfg->release();
	}

	std::unique_ptr<q03::Sandbox>	_sandbox;
	string			_root;
	BDMgr			_bd;
	HotMgr			_hot;
	Loader			_loader;
	ReaderSink		_sink{&_bd, &_hot};
	WtDataReader	_reader;
};

// ---------------------------------------------------------------- 主力连续
// WtDataReader.cpp: cacheIntegratedBars 的 HOT 包裹文件读取 L1019-1041 + cacheHisBarsFromFile 的
//                   try/catch L1593-1608 + readKlineSlice 空缓存兜底 L1895-1912
//
TEST_F(R52ReaderTest, R52HotWrappedFileReadReturnsIntegratedBars)
{
	_hot.set_tag("HOT");
	_hot.set_raw_code("rb2402");
	// 覆盖 HOT bars 的切段在最后一根 HOT bar 处结束，避免依赖不存在的分月合约文件。
	_hot.add_section("rb2402", 20240104, 20240106, 1.2);

	vector<WTSBarStruct> hotbars;
	hotbars.push_back(q03::make_bar(20240105, 20240105, 1));
	hotbars.push_back(q03::make_bar(20240106, 20240106, 2));
	// 路径必须与 cacheIntegratedBars L1023 的拼装严格一致：
	//   {his_dir}{pname}/{exchg}.{product}_{ruleTag}{suf}.dsb
	// 原用例多写了一层 <exchg>/ 目录，产品探测不到包裹文件，会落入分段装配路径，
	// 无法覆盖 HOT 包裹文件读取。
	ASSERT_TRUE(q03::write_bytes("his/day/SHFE.rb_HOT.dsb",
		q03::make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, q03::pack_bars(hotbars))));

	init_reader();

	WTSKlineSlice* slice = _reader.readKlineSlice("SHFE.rb.HOT", KP_DAY, 100, 202401101500ull);
	ASSERT_NE(slice, nullptr);
	const vector<WTSBarStruct> got = flatten_bars(slice);
	EXPECT_EQ(got.size(), 2u);
	EXPECT_EQ(slice->size(), 2u);
	if (got.size() == 2)
	{
		EXPECT_EQ(got[0].date, 20240105u);
		EXPECT_DOUBLE_EQ(got[0].close, 11.5);
		EXPECT_EQ(got[1].date, 20240106u);
		EXPECT_DOUBLE_EQ(got[1].close, 12.5);
	}
	slice->release();

	EXPECT_EQ(_sink.count_of(LL_ERROR), 0u);
	EXPECT_EQ(_hot.split_calls(), 1u);

	// 第二次读取命中缓存，返回相同包裹文件 bars，且不重复切段。
	WTSKlineSlice* again = _reader.readKlineSlice("SHFE.rb.HOT", KP_DAY, 2, 202401101500ull);
	ASSERT_NE(again, nullptr);
	const vector<WTSBarStruct> cached = flatten_bars(again);
	EXPECT_EQ(cached.size(), 2u);
	if (cached.size() == 2)
	{
		EXPECT_EQ(cached[0].date, 20240105u);
		EXPECT_DOUBLE_EQ(cached[0].close, 11.5);
		EXPECT_EQ(cached[1].date, 20240106u);
		EXPECT_DOUBLE_EQ(cached[1].close, 12.5);
	}
	EXPECT_EQ(_sink.count_of(LL_ERROR), 0u);
	EXPECT_EQ(_hot.split_calls(), 1u);
	again->release();
}

// WtDataReader.cpp: cacheIntegratedBars L1019-1259（HOT 包裹文件不存在 → 仅按切段装配）
//                   + L1074-1248 复权因子与 adjust_flag + readKlineSlice L1844-1876
//                   + onMinuteEnd L2210-2311
TEST_F(R52ReaderTest, R52HotContinuousMin1AdjustsAndReplaysOnMinuteEnd)
{
	_hot.set_tag("HOT");
	_hot.set_raw_code("rb2402");
	_hot.add_section("rb2402", 20240104, 20240110, 1.2);

	vector<WTSBarStruct> bars;
	bars.push_back(q03::make_bar(20240104, min_bar_time(20240104, 930), 1));
	bars.push_back(q03::make_bar(20240105, min_bar_time(20240105, 930), 2));
	bars.push_back(q03::make_bar(20240106, min_bar_time(20240106, 930), 3));
	bars.push_back(q03::make_bar(20240107, min_bar_time(20240107, 930), 4));
	ASSERT_TRUE(q03::write_bytes("his/min1/SHFE/rb2402.dsb",
		q03::make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, q03::pack_bars(bars))));

	vector<WTSBarStruct> rt;
	for (uint32_t i = 0; i < 3; i++)
		rt.push_back(q03::make_bar(20240110, min_bar_time(20240110, 930 + i), 30 + i));
	ASSERT_TRUE(q03::write_bytes("rt/min1/SHFE/rb2402.dmb",
		q03::make_rt_block<WTSBarStruct>(BT_RT_Minute1, 20240110, 3, 10, rt)));

	init_reader(7);

	// etime=09:31：RT 侧只取 09:30/09:31，历史补 1 条（L1832-1841、L1849-1876）
	WTSKlineSlice* slice = _reader.readKlineSlice("SHFE.rb.HOT+", KP_Minute1, 3, 202401100931ull);
	ASSERT_TRUE(slice != NULL);
	vector<WTSBarStruct> got = flatten_bars(slice);
	ASSERT_EQ(got.size(), (std::size_t)3);
	EXPECT_EQ(got[0].date, 20240107u);
	EXPECT_EQ(got[0].time % 10000, 930u);
	// 历史段：hotSec._factor / baseFactor = 1.2/1.0，OHLC 放大、vol/hold/add 缩小、money 放大
	EXPECT_DOUBLE_EQ(got[0].open, 14.0 * 1.2);
	EXPECT_DOUBLE_EQ(got[0].high, 15.0 * 1.2);
	EXPECT_DOUBLE_EQ(got[0].low, 13.0 * 1.2);
	EXPECT_DOUBLE_EQ(got[0].close, 14.5 * 1.2);
	EXPECT_DOUBLE_EQ(got[0].vol, 2004.0 / 1.2);
	EXPECT_DOUBLE_EQ(got[0].money, 1004.0 * 1.2);
	EXPECT_DOUBLE_EQ(got[0].hold, 3004.0 / 1.2);
	EXPECT_DOUBLE_EQ(got[0].add, 104.0 / 1.2);
	// RT 段：代码只做 OHLC 复权（L1859-1866），量额保持原值
	EXPECT_EQ(got[1].date, 20240110u);
	EXPECT_DOUBLE_EQ(got[1].open, 40.0 * 1.2);
	EXPECT_DOUBLE_EQ(got[1].vol, 2030.0);
	EXPECT_EQ(got[2].time % 10000, 931u);
	EXPECT_DOUBLE_EQ(got[2].open, 41.0 * 1.2);
	slice->release();

	// onMinuteEnd：回放 RT 第 3 根（09:32），后复权走 barsList._bars 副本分支
	_reader.onMinuteEnd(20240110, 1500);
	EXPECT_EQ(_sink.bar_count(), 1u);
	EXPECT_EQ(_sink.last_code(), string("SHFE.rb.HOT+"));
	EXPECT_EQ(_sink.last_period(), KP_Minute1);
	EXPECT_EQ(_sink.last_bar().time % 10000, 932u);
	EXPECT_DOUBLE_EQ(_sink.last_bar().open, 42.0 * 1.2);

	// nowTime <= _last_time 的早退（L2213-2215）
	_reader.onMinuteEnd(20240110, 1400);
	EXPECT_EQ(_sink.bar_count(), 1u);

	// 游标已到最后一根：不再复制，直接从 _bars 尾部取 1 条（L1849 条件为假）
	WTSKlineSlice* last = _reader.readKlineSlice("SHFE.rb.HOT+", KP_Minute1, 1, 202401101500ull);
	ASSERT_TRUE(last != NULL);
	vector<WTSBarStruct> got2 = flatten_bars(last);
	ASSERT_EQ(got2.size(), (std::size_t)1);
	EXPECT_EQ(got2[0].time % 10000, 932u);
	EXPECT_DOUBLE_EQ(got2[0].open, 42.0 * 1.2);
	last->release();
}

// ---------------------------------------------------------------- 股票复权
// WtDataReader.cpp: loadStkAdjFactorsFromFile L252-320 + cacheAdjustedStkBars L1287-1562
//                   + getAdjFactorByDate L2313-2347
TEST_F(R52ReaderTest, R52AdjustedStockQfqUsesFactorFileAndAppliesAllFlagBits)
{
	const string adjfile = _root + "adj.json";
	ASSERT_TRUE(q03::write_bytes(adjfile,
		"{\"SSE\":{\"600000\":[{\"date\":20240103,\"factor\":1.1},"
		"{\"date\":20240105,\"factor\":1.2}]}}"));

	vector<WTSBarStruct> raw;
	for (uint32_t i = 0; i < 7; i++)
		raw.push_back(q03::make_bar(20240102 + i, 20240102 + i, i + 1));
	ASSERT_TRUE(q03::write_bytes("his/day/SSE/600000.dsb",
		q03::make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, q03::pack_bars(raw))));

	const string extra = fmtutil::format(",\"adjfactor\":\"{}\"", adjfile);
	init_reader(7, extra.c_str());

	// 没有 his/day/SSE/600000-.dsb → lastQTime=0，从原始文件头部开始按因子倒序回放
	WTSKlineSlice* slice = _reader.readKlineSlice("SSE.STK.600000-", KP_DAY, 100, 202401081500ull);
	ASSERT_TRUE(slice != NULL);
	vector<WTSBarStruct> got = flatten_bars(slice);
	ASSERT_EQ(got.size(), (std::size_t)7);

	const double f1 = 1.0 / 1.2;			// 20240102 之前
	const double f2 = 1.1 / 1.2;			// 20240103~20240104
	const double f3 = 1.2 / 1.2;			// 20240105 起
	const double factors[] = {f1, f2, f2, f3, f3, f3, f3};
	for (uint32_t i = 0; i < 7; i++)
	{
		const double seq = i + 1.0;
		EXPECT_EQ(got[i].date, 20240102 + i);
		EXPECT_DOUBLE_EQ(got[i].open, (10.0 + seq) * factors[i]);
		EXPECT_DOUBLE_EQ(got[i].high, (11.0 + seq) * factors[i]);
		EXPECT_DOUBLE_EQ(got[i].low, (9.0 + seq) * factors[i]);
		EXPECT_DOUBLE_EQ(got[i].close, (10.5 + seq) * factors[i]);
		EXPECT_DOUBLE_EQ(got[i].vol, (2000.0 + seq) / factors[i]);
		EXPECT_DOUBLE_EQ(got[i].money, (1000.0 + seq) * factors[i]);
		EXPECT_DOUBLE_EQ(got[i].hold, (3000.0 + seq) / factors[i]);
		EXPECT_DOUBLE_EQ(got[i].add, (100.0 + seq) / factors[i]);
	}
	slice->release();

	// adjusted 文件存在的分支（L1330-1366）：lastQTime=0105，原始文件从 0106 续接
	vector<WTSBarStruct> adj3;
	adj3.push_back(q03::make_bar(20240104, 20240104, 50));
	adj3.push_back(q03::make_bar(20240105, 20240105, 51));
	ASSERT_TRUE(q03::write_bytes("his/day/SSE/600003-.dsb",
		q03::make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, q03::pack_bars(adj3))));
	vector<WTSBarStruct> raw3;
	for (uint32_t i = 0; i < 3; i++)
		raw3.push_back(q03::make_bar(20240106 + i, 20240106 + i, 60 + i));
	ASSERT_TRUE(q03::write_bytes("his/day/SSE/600003.dsb",
		q03::make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, q03::pack_bars(raw3))));

	WTSKlineSlice* s3 = _reader.readKlineSlice("SSE.STK.600003-", KP_DAY, 100, 202401081500ull);
	ASSERT_TRUE(s3 != NULL);
	vector<WTSBarStruct> g3 = flatten_bars(s3);
	ASSERT_EQ(g3.size(), (std::size_t)5);
	EXPECT_EQ(g3[0].date, 20240104u);
	EXPECT_EQ(g3[1].date, 20240105u);
	EXPECT_EQ(g3[2].date, 20240106u);
	EXPECT_EQ(g3[2].open, 70.0);
	EXPECT_EQ(g3[4].date, 20240108u);
	s3->release();

	// getAdjFactorByDate：左边界回退、命中、超出尾部、复权后缀、非股票
	EXPECT_DOUBLE_EQ(_reader.getAdjFactorByDate("SSE.STK.600000", 20240102), 1.0);
	EXPECT_DOUBLE_EQ(_reader.getAdjFactorByDate("SSE.STK.600000", 20240104), 1.1);
	EXPECT_DOUBLE_EQ(_reader.getAdjFactorByDate("SSE.STK.600000", 20240106), 1.2);
	EXPECT_DOUBLE_EQ(_reader.getAdjFactorByDate("SSE.STK.600000-", 20240104), 1.1);
	EXPECT_DOUBLE_EQ(_reader.getAdjFactorByDate("SHFE.rb.2401", 20240104), 1.0);

	// 因子文件缺失 / 解析失败两条错误的 init 分支（L254-265）
	q03::ReaderSink sink2(&_bd, &_hot);
	WtDataReader reader2;
	const string cfg2 = _root + "reader_missing.json";
	ASSERT_TRUE(q03::write_bytes(cfg2,
		fmtutil::format("{{\"path\":\"{}\",\"adjust_flag\":0,\"adjfactor\":\"{}nope.json\"}}", _root, _root)));
	WTSVariant* v2 = WTSCfgLoader::load_from_file(cfg2.c_str());
	ASSERT_TRUE(v2 != NULL);
	reader2.init(v2, &sink2, NULL);
	v2->release();
	EXPECT_GE(sink2.count_of(LL_ERROR), 1u);

	q03::ReaderSink sink3(&_bd, &_hot);
	WtDataReader reader3;
	const string badfile = _root + "bad.json";
	ASSERT_TRUE(q03::write_bytes(badfile, "{not-a-json"));
	const string cfg3 = _root + "reader_bad.json";
	ASSERT_TRUE(q03::write_bytes(cfg3,
		fmtutil::format("{{\"path\":\"{}\",\"adjust_flag\":0,\"adjfactor\":\"{}\"}}", _root, badfile)));
	WTSVariant* v3 = WTSCfgLoader::load_from_file(cfg3.c_str());
	ASSERT_TRUE(v3 != NULL);
	reader3.init(v3, &sink3, NULL);
	v3->release();
	EXPECT_GE(sink3.count_of(LL_ERROR), 1u);
}

// ---------------------------------------------------------------- 扩展 loader
// WtDataReader.cpp: loadStkAdjFactorsFromLoader L219-250 + cacheFinalBarsFromLoader L930-966
//                   + cacheHisBarsFromFile 的 loader 分支 L1635-1644 + getAdjFactors L2349-2391
TEST_F(R52ReaderTest, R52LoaderSuppliesFinalRawBarsAndLazyFactors)
{
	vector<WTSBarStruct> fin;
	fin.push_back(q03::make_bar(20240105, 20240105, 1));
	fin.push_back(q03::make_bar(20240108, 20240108, 2));
	fin.push_back(q03::make_bar(20240109, 20240109, 3));
	_loader.add_final("SSE.STK.600000", fin);

	vector<WTSBarStruct> raw1;
	for (uint32_t i = 0; i < 3; i++)
		raw1.push_back(q03::make_bar(20240105 + i, 20240105 + i, 11 + i));
	_loader.add_raw("SSE.STK.600001", raw1);

	vector<WTSBarStruct> raw2;
	const uint32_t raw2_dates[] = {20240102, 20240104, 20240105, 20240108};
	for (uint32_t i = 0; i < 4; i++)
		raw2.push_back(q03::make_bar(raw2_dates[i], raw2_dates[i], 21 + i));
	_loader.add_raw("SSE.STK.600002", raw2);

	// loadAllAdjFactors：只给 600000，600002 走 getAdjFactors 的按需加载
	_loader.add_all_factors("SSE.STK.600000", vector<uint32_t>{20240103}, vector<double>{1.5});
	_loader.add_adj_factors("SSE.STK.600002", vector<uint32_t>{20240103, 20240105},
		vector<double>{1.1, 1.2});

	const string cfgfile = _root + "reader_loader.json";
	ASSERT_TRUE(q03::write_bytes(cfgfile,
		fmtutil::format("{{\"path\":\"{}\",\"adjust_flag\":0}}", _root)));
	WTSVariant* cfg = WTSCfgLoader::load_from_file(cfgfile.c_str());
	ASSERT_TRUE(cfg != NULL);
	_reader.init(cfg, &_sink, &_loader);
	cfg->release();

	// 1) loadFinalHisBars 命中
	WTSKlineSlice* s1 = _reader.readKlineSlice("SSE.STK.600000", KP_DAY, 10, 202401091500ull);
	ASSERT_TRUE(s1 != NULL);
	vector<WTSBarStruct> g1 = flatten_bars(s1);
	ASSERT_EQ(g1.size(), (std::size_t)3);
	EXPECT_EQ(g1[0].date, 20240105u);
	EXPECT_EQ(g1[2].date, 20240109u);
	s1->release();
	EXPECT_DOUBLE_EQ(_reader.getAdjFactorByDate("SSE.STK.600000", 20240104), 1.5);
	EXPECT_DOUBLE_EQ(_reader.getAdjFactorByDate("SSE.STK.600000", 20240102), 1.0);

	// 2) loadFinalHisBars 未命中 → cacheHisBarsFromFile 的 loadRawHisBars 分支
	WTSKlineSlice* s2 = _reader.readKlineSlice("SSE.STK.600001", KP_DAY, 10, 202401071500ull);
	ASSERT_TRUE(s2 != NULL);
	vector<WTSBarStruct> g2 = flatten_bars(s2);
	ASSERT_EQ(g2.size(), (std::size_t)3);
	EXPECT_EQ(g2[0].date, 20240105u);
	EXPECT_EQ(g2[2].date, 20240107u);
	s2->release();

	// 3) 复权股票 + 按需 loader 因子：cacheAdjustedStkBars 的 getAdjFactors 分支
	WTSKlineSlice* s3 = _reader.readKlineSlice("SSE.STK.600002+", KP_DAY, 10, 202401081500ull);
	ASSERT_TRUE(s3 != NULL);
	vector<WTSBarStruct> g3 = flatten_bars(s3);
	ASSERT_EQ(g3.size(), (std::size_t)4);
	EXPECT_DOUBLE_EQ(g3[0].open, 31.0);
	EXPECT_DOUBLE_EQ(g3[1].open, 32.0 * 1.1);
	EXPECT_DOUBLE_EQ(g3[2].open, 33.0 * 1.2);
	EXPECT_DOUBLE_EQ(g3[3].open, 34.0 * 1.2);
	s3->release();
	EXPECT_DOUBLE_EQ(_reader.getAdjFactorByDate("SSE.STK.600002", 20240104), 1.1);
}

// ---------------------------------------------------------------- 逐笔 RT + 重映射
// WtDataReader.cpp: readOrdQueSlice L499-612 / readOrdDtlSlice L656-769 / readTransSlice L813-926
//                   + getRTTickBlock L1940-1966 / getRTOrdDtlBlock L1993-2020 / getRTOrdQueBlock L2047-2074
//                   + getRTTransBlock L2101-2128 / getRTKilneBlock L2176-2205
TEST_F(R52ReaderTest, R52TodayHftBlocksAndCapacityRemap)
{
	vector<WTSTickStruct> ticks;
	ticks.push_back(q03::make_tick(20240110, 20240110, 93000000, 1));
	ticks.push_back(q03::make_tick(20240110, 20240110, 103000000, 2));
	ASSERT_TRUE(q03::write_bytes("rt/ticks/SSE/600000.dmb",
		q03::make_rt_block<WTSTickStruct>(BT_RT_Ticks, 20240110, 2, 8, ticks)));

	vector<WTSOrdQueStruct> ques;
	ques.push_back(q03::make_ordque(20240110, 20240110, 93000000, 1));
	ques.push_back(q03::make_ordque(20240110, 20240110, 103000000, 2));
	const string fque = "rt/queue/SSE/600000.dmb";
	ASSERT_TRUE(q03::write_bytes(fque,
		q03::make_rt_block<WTSOrdQueStruct>(BT_RT_OrdQueue, 20240110, 2, 8, ques)));

	vector<WTSOrdDtlStruct> dtls;
	dtls.push_back(q03::make_orddtl(20240110, 20240110, 93000000, 3));
	dtls.push_back(q03::make_orddtl(20240110, 20240110, 103000000, 4));
	const string fdtl = "rt/orders/SSE/600000.dmb";
	ASSERT_TRUE(q03::write_bytes(fdtl,
		q03::make_rt_block<WTSOrdDtlStruct>(BT_RT_OrdDetail, 20240110, 2, 8, dtls)));

	vector<WTSTransStruct> trs;
	trs.push_back(q03::make_trans(20240110, 20240110, 93000000, 5));
	trs.push_back(q03::make_trans(20240110, 20240110, 103000000, 6));
	const string ftrs = "rt/trans/SSE/600000.dmb";
	ASSERT_TRUE(q03::write_bytes(ftrs,
		q03::make_rt_block<WTSTransStruct>(BT_RT_Trnsctn, 20240110, 2, 8, trs)));

	vector<WTSBarStruct> rtbars;
	rtbars.push_back(q03::make_bar(20240110, min_bar_time(20240110, 930), 1));
	rtbars.push_back(q03::make_bar(20240110, min_bar_time(20240110, 931), 2));
	const string fmin1 = "rt/min1/SSE/600000.dmb";
	ASSERT_TRUE(q03::write_bytes(fmin1,
		q03::make_rt_block<WTSBarStruct>(BT_RT_Minute1, 20240110, 2, 8, rtbars)));

	init_reader();

	// 四条 isToday 逐笔链路（L499-526 / L656-683 / L813-840）
	WTSTickSlice* st = _reader.readTickSlice("SSE.STK.600000", 2, 20240110150000000ull);
	ASSERT_TRUE(st != NULL);
	EXPECT_EQ(flatten_ticks(st).size(), (std::size_t)2);
	st->release();

	WTSOrdQueSlice* sq = _reader.readOrdQueSlice("SSE.STK.600000", 2, 20240110150000000ull);
	ASSERT_TRUE(sq != NULL);
	ASSERT_EQ(sq->size(), 2u);
	EXPECT_DOUBLE_EQ(sq->at(0)->price, ques[0].price);
	EXPECT_DOUBLE_EQ(sq->at(1)->price, ques[1].price);
	sq->release();

	WTSOrdDtlSlice* sd = _reader.readOrdDtlSlice("SSE.STK.600000", 2, 20240110150000000ull);
	ASSERT_TRUE(sd != NULL);
	ASSERT_EQ(sd->size(), 2u);
	EXPECT_EQ(sd->at(1)->index, dtls[1].index);
	sd->release();

	WTSTransSlice* sr = _reader.readTransSlice("SSE.STK.600000", 2, 20240110150000000ull);
	ASSERT_TRUE(sr != NULL);
	ASSERT_EQ(sr->size(), 2u);
	EXPECT_EQ(sr->at(1)->askorder, trs[1].askorder);
	sr->release();

	// 容量原地变大 → 第二次访问走 _last_cap != _capacity 的重新映射分支
	ASSERT_TRUE(patch_rt_capacity("rt/ticks/SSE/600000.dmb", 10));
	ASSERT_TRUE(patch_rt_capacity(fque, 10));
	ASSERT_TRUE(patch_rt_capacity(fdtl, 10));
	ASSERT_TRUE(patch_rt_capacity(ftrs, 10));
	ASSERT_TRUE(patch_rt_capacity(fmin1, 10));

	WTSTickSlice* st2 = _reader.readTickSlice("SSE.STK.600000", 2, 20240110150000000ull);
	ASSERT_TRUE(st2 != NULL);
	EXPECT_EQ(flatten_ticks(st2).size(), (std::size_t)2);
	st2->release();

	WTSOrdQueSlice* sq2 = _reader.readOrdQueSlice("SSE.STK.600000", 2, 20240110150000000ull);
	ASSERT_TRUE(sq2 != NULL);
	EXPECT_EQ(sq2->size(), 2u);
	sq2->release();

	WTSOrdDtlSlice* sd2 = _reader.readOrdDtlSlice("SSE.STK.600000", 2, 20240110150000000ull);
	ASSERT_TRUE(sd2 != NULL);
	EXPECT_EQ(sd2->size(), 2u);
	sd2->release();

	WTSTransSlice* sr2 = _reader.readTransSlice("SSE.STK.600000", 2, 20240110150000000ull);
	ASSERT_TRUE(sr2 != NULL);
	EXPECT_EQ(sr2->size(), 2u);
	sr2->release();

	// 分钟 RT 块：第二次命中重映射，日志带 "expanded to 10"
	WTSKlineSlice* sk1 = _reader.readKlineSlice("SSE.STK.600000", KP_Minute1, 5, 202401101500ull);
	ASSERT_TRUE(sk1 != NULL);
	EXPECT_EQ(flatten_bars(sk1).size(), (std::size_t)2);
	sk1->release();

	WTSKlineSlice* sk2 = _reader.readKlineSlice("SSE.STK.600000", KP_Minute1, 5, 202401101500ull);
	ASSERT_TRUE(sk2 != NULL);
	vector<WTSBarStruct> got = flatten_bars(sk2);
	ASSERT_EQ(got.size(), (std::size_t)2);
	EXPECT_EQ(got[0].time % 10000, 930u);
	sk2->release();
	// 重映射分支在 L2192-2194 打一条 LL_DEBUG（"expanded to ..."），读侧每个周期也各有一条 DEBUG，
	// 这里只确认第二读产生了新的 DEBUG 日志且数据仍可读
	EXPECT_GT(_sink.count_of(LL_DEBUG), 1u);
}

} // namespace r52
