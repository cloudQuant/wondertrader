// r70 / WtDataReader：R52 之后仍缺的日线主力连续边界、复权分钟线、逐笔历史错误分支与无 RT 兜底
//
// 覆盖（src/WtDataStorage/WtDataReader.cpp）：
//  - cacheIntegratedBars 的 KP_DAY 分支 :1110-1123、逐段 lower_bound :1181-1219、装配 :1250-1264
//  - cacheAdjustedStkBars 的分钟分支 :1361、:1377-1381、:1451-1455、因子回放 :1508-1555
//  - readTickSlice 的 his 分支 :330-336（etime==0 用 sink 时钟）、:352-356（HOT 原始代码）、
//    :405-455（建块、`>=` 右边界回退）
//  - readOrdQueSlice / readOrdDtlSlice / readTransSlice 的 his 建块与长度校验 :405-421 / :558-612 /
//    :685-769 / :842-926
//  - readKlineSlice 的 etime==0 归一 :1753-1757 与无 RT 块兜底 :1895-1904；getRTKilneBlock 的
//    KP_Minute5 分支 :2150-2155
//
// 手法：WtDataReader::init(cfg, sink, NULL) 不起线程；数据目录由 Sandbox 切 CWD；
// 没有访问权限宏，也没有依赖墙钟的等待。
#include "r52_storage_fixture.h"

#include "WTSUtils/WTSCfgLoader.h"
#include "WtDataStorage/WtDataReader.h"

namespace r70 {

using namespace q03;
using std::string;
using std::vector;

class R70ReaderResidualTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r70rd"));
		_root = _sandbox->root_abs();
		_bd.set_cur_tdate(20240110);
	}

	void TearDown() override
	{
		_sandbox.reset();
	}

	void init_reader(const char* extra = "")
	{
		const string cfgfile = _root + "reader.json";
		const string json = fmtutil::format("{{\"path\":\"{}\",\"adjust_flag\":0{}}}", _root, extra);
		ASSERT_TRUE(write_bytes(cfgfile, json));
		WTSVariant* cfg = WTSCfgLoader::load_from_file(cfgfile.c_str());
		ASSERT_TRUE(cfg != NULL);
		_reader.init(cfg, &_sink, NULL);
		cfg->release();
	}

	std::unique_ptr<Sandbox>	_sandbox;
	string				_root;
	r52::BDMgr			_bd;
	r52::HotMgr			_hot;
	r52::ReaderSink			_sink{ &_bd, &_hot };
	WtDataReader			_reader;
};

// ------------------------------------------------------------------ 主力连续日线边界
// WtDataReader.cpp: cacheIntegratedBars L969-1285（KP_DAY 的 sBar/eBar :1110-1123、
//   逐段读文件 :1181-1207、右端回退 :1213-1219、装配 :1250-1264）
//   + readKlineSlice L1799-1904 的日期确认与无 RT 块兜底
TEST_F(R70ReaderResidualTest, R70ReaderIntegratedDaySectionsAndNoRtTail)
{
	_hot.set_tag("HOT");
	_hot.set_raw_code("rb2402");
	_hot.add_section("rb2401", 20240102, 20240103, 1.1);
	_hot.add_section("rb2402", 20240104, 20240110, 1.2);

	// 包裹文件 his/day/SHFE.rb_HOT.dsb 故意不建：BUG-R52-001 的遮蔽读取不可达，
	// 本用例只走「无包裹文件 → 按切段装配」这条。
	vector<WTSBarStruct> mon1;
	mon1.push_back(make_bar(20240102, 20240102, 1));
	mon1.push_back(make_bar(20240103, 20240103, 2));
	ASSERT_TRUE(write_bytes("his/day/SHFE/rb2401.dsb",
		make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(mon1))));

	// rb2402 段：0104..0108 + 0112（跳过 0109~0111），使 eBar(0110) 的 lower_bound 命中 0112 后回退
	vector<WTSBarStruct> mon2;
	const uint32_t dates[] = { 20240104, 20240105, 20240106, 20240107, 20240108, 20240112 };
	for (uint32_t i = 0; i < 6; i++)
		mon2.push_back(make_bar(dates[i], dates[i], 11 + i));
	ASSERT_TRUE(write_bytes("his/day/SHFE/rb2402.dsb",
		make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(mon2))));

	init_reader();

	WTSKlineSlice* slice = _reader.readKlineSlice("SHFE.rb.HOT", KP_DAY, 100, 202401101500ull);
	ASSERT_TRUE(slice != NULL);
	vector<WTSBarStruct> got = r52::flatten_bars(slice);
	ASSERT_EQ(got.size(), (std::size_t)7);
	const uint32_t expect_dates[] = { 20240102, 20240103, 20240104, 20240105,
		20240106, 20240107, 20240108 };
	const double expect_opens[] = { 11.0, 12.0, 21.0, 22.0, 23.0, 24.0, 25.0 };
	for (uint32_t i = 0; i < 7; i++)
	{
		EXPECT_EQ(got[i].date, expect_dates[i]);
		EXPECT_DOUBLE_EQ(got[i].open, expect_opens[i]);
	}
	EXPECT_GE(_hot.split_calls(), 1u);
	slice->release();
}

// ------------------------------------------------------------------ 股票复权分钟线
// WtDataReader.cpp: loadStkAdjFactorsFromFile L252-320 + cacheAdjustedStkBars L1287-1562
//   （lastQTime 分钟分支 :1361、sBar 构造 :1377-1381、原始文件 lower_bound :1446-1455、
//    因子回放 :1484-1531、装配 :1538-1557）
TEST_F(R70ReaderResidualTest, R70ReaderAdjustedStockMin1Continuation)
{
	const string adjfile = _root + "adj.json";
	ASSERT_TRUE(write_bytes(adjfile,
		"{\"SSE\":{\"600000\":[{\"date\":20240104,\"factor\":1.1},"
		"{\"date\":20240105,\"factor\":1.2}]}}"));

	// 前复权文件两根：0103 14:58 / 0104 14:58
	vector<WTSBarStruct> adj;
	adj.push_back(make_bar(20240103, TimeUtils::timeToMinBar(20240103, 1458), 1));
	adj.push_back(make_bar(20240104, TimeUtils::timeToMinBar(20240104, 1458), 2));
	ASSERT_TRUE(write_bytes("his/min1/SSE/600000-.dsb",
		make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, pack_bars(adj))));

	// 原始文件从 0105 续接；0104 14:58 那条已被 lastQTime 覆盖
	vector<WTSBarStruct> raw;
	raw.push_back(make_bar(20240104, TimeUtils::timeToMinBar(20240104, 1458), 11));
	raw.push_back(make_bar(20240105, TimeUtils::timeToMinBar(20240105, 930), 12));
	raw.push_back(make_bar(20240105, TimeUtils::timeToMinBar(20240105, 931), 13));
	ASSERT_TRUE(write_bytes("his/min1/SSE/600000.dsb",
		make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, pack_bars(raw))));

	const string extra = fmtutil::format(",\"adjfactor\":\"{}\"", adjfile);
	init_reader(extra.c_str());

	WTSKlineSlice* slice = _reader.readKlineSlice("SSE.STK.600000-", KP_Minute1, 100,
		202401051500ull);
	ASSERT_TRUE(slice != NULL);
	vector<WTSBarStruct> got = r52::flatten_bars(slice);
	ASSERT_EQ(got.size(), (std::size_t)4);
	EXPECT_EQ(got[0].date, 20240103u);
	EXPECT_EQ(got[1].date, 20240104u);
	EXPECT_EQ(got[2].date, 20240105u);
	EXPECT_EQ(got[3].date, 20240105u);
	// 0105 是因子表最后一条，baseFactor=1.2，回放因子为 1.0 → 原始段数值不变
	EXPECT_DOUBLE_EQ(got[2].open, 22.0);
	EXPECT_DOUBLE_EQ(got[3].open, 23.0);
	slice->release();
}

// ------------------------------------------------------------------ 逐笔历史错误分支
// WtDataReader.cpp: readTickSlice L405-455（短文件/header-only）
//   + readOrdQueSlice L533-612（CMP_V2 建块、短文件）
//   + readOrdDtlSlice L690-769（建块、15:00 全量、09:31 回退）
//   + readTransSlice L847-926（尺寸不符）
TEST_F(R70ReaderResidualTest, R70ReaderHisHftErrorBranches)
{
	// tick：0108 只有 8 字节（< HisTickBlock 12）；0109 只有块头（tcnt==0）
	ASSERT_TRUE(write_bytes("his/ticks/SSE/20240108/600000.dsb", string(8, 'X')));
	ASSERT_TRUE(write_bytes("his/ticks/SSE/20240109/600000.dsb",
		make_block(BT_HIS_Ticks, BLOCK_VERSION_RAW_V2, "")));
	// 委托队列：8 字节短文件
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240108/600000.dsb", string(8, 'X')));

	// 委托明细：3 条 CMP_V2
	vector<WTSOrdDtlStruct> dtl;
	for (uint32_t i = 0; i < 3; i++)
		dtl.push_back(make_orddtl(20240107, 20240107, 93000000u + i * 100000u, 11 + i));
	ASSERT_TRUE(write_bytes("his/orders/SSE/20240107/600000.dsb",
		make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, pack_items(dtl))));

	// 逐笔成交：2 条 CMP_V2，尾部多 1 字节 → `_size` 与实际长度不符
	vector<WTSTransStruct> trs;
	for (uint32_t i = 0; i < 2; i++)
		trs.push_back(make_trans(20240106, 20240106, 93000000u + i * 100000u, 21 + i));
	ASSERT_TRUE(write_bytes("his/trans/SSE/20240106/600000.dsb",
		make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, pack_items(trs)) + "X"));

	init_reader();

	const uint32_t err0 = _sink.count_of(LL_ERROR);
	EXPECT_TRUE(_reader.readTickSlice("SSE.STK.600000", 5, 20240108150000000ull) == NULL);
	EXPECT_GT(_sink.count_of(LL_ERROR), err0);
	EXPECT_TRUE(_reader.readTickSlice("SSE.STK.600000", 5, 20240109150000000ull) == NULL);

	const uint32_t err1 = _sink.count_of(LL_ERROR);
	EXPECT_TRUE(_reader.readOrdQueSlice("SSE.STK.600000", 5, 20240108150000000ull) == NULL);
	EXPECT_GT(_sink.count_of(LL_ERROR), err1);

	WTSOrdDtlSlice* sd = _reader.readOrdDtlSlice("SSE.STK.600000", 10, 20240107150000000ull);
	ASSERT_TRUE(sd != NULL);
	ASSERT_EQ(sd->size(), 3u);
	EXPECT_EQ(sd->at(1)->index, dtl[1].index);
	sd->release();

	WTSOrdDtlSlice* sd2 = _reader.readOrdDtlSlice("SSE.STK.600000", 10, 20240107093100000ull);
	ASSERT_TRUE(sd2 != NULL);
	ASSERT_EQ(sd2->size(), 1u);
	EXPECT_EQ(sd2->at(0)->index, dtl[0].index);
	sd2->release();

	const uint32_t err2 = _sink.count_of(LL_ERROR);
	EXPECT_TRUE(_reader.readTransSlice("SSE.STK.600000", 5, 20240106150000000ull) == NULL);
	EXPECT_GT(_sink.count_of(LL_ERROR), err2);
}

// ------------------------------------------------------------------ tick 的 sink 时钟 + HOT 原始代码
// WtDataReader.cpp: readTickSlice L329-343（etime==0 用 sink 的 date/min/secs）、
//   L350-356（future + ruleTag → getCustomRawCode）、L414-455（建块与 `>=` 回退）
TEST_F(R70ReaderResidualTest, R70ReaderTickSinkClockAndHotRawCode)
{
	_hot.set_tag("HOT");
	_hot.set_raw_code("rb2401");

	vector<WTSTickStruct> ticks;
	ticks.push_back(make_tick(20240108, 20240108, 93000000, 1));
	ticks.push_back(make_tick(20240108, 20240108, 103000000, 2));
	ticks.push_back(make_tick(20240108, 20240108, 143000000, 3));
	ASSERT_TRUE(write_bytes("his/ticks/SHFE/20240108/rb2401.dsb",
		make_block(BT_HIS_Ticks, BLOCK_VERSION_RAW_V2, pack_ticks(ticks))));

	init_reader();
	_sink.set_clock(20240108, 1500, 0);

	// etime==0 → 20240108150000000；HOT 代码映射到 rb2401
	WTSTickSlice* s = _reader.readTickSlice("SHFE.rb.HOT", 2, 0);
	ASSERT_TRUE(s != NULL);
	vector<WTSTickStruct> got = r52::flatten_ticks(s);
	ASSERT_EQ(got.size(), (std::size_t)2);
	EXPECT_EQ(got[0].action_time, 103000000u);
	EXPECT_EQ(got[1].action_time, 143000000u);
	EXPECT_DOUBLE_EQ(got[0].price, 32.0);
	s->release();

	// 右边界正好压在 10:30 上：his 分支用的是 `>=` → 边界被退掉，只剩 09:30
	WTSTickSlice* s2 = _reader.readTickSlice("SHFE.rb.HOT", 2, 20240108103000000ull);
	ASSERT_TRUE(s2 != NULL);
	vector<WTSTickStruct> got2 = r52::flatten_ticks(s2);
	ASSERT_EQ(got2.size(), (std::size_t)1);
	EXPECT_EQ(got2[0].action_time, 93000000u);
	EXPECT_DOUBLE_EQ(got2[0].price, 31.0);
	s2->release();
}

// ------------------------------------------------------------------ 无 RT 块的 K 线兜底
// WtDataReader.cpp: readKlineSlice L1751-1757（etime==0 用 sink 时钟）、L1895-1904（RT 缺失、
//   纯 his 尾窗）、getRTKilneBlock L2132-2205 的 KP_DAY 早退与 KP_Minute5 分支
TEST_F(R70ReaderResidualTest, R70ReaderKlineNoRtFallbackAndMin5)
{
	vector<WTSBarStruct> day;
	day.push_back(make_bar(20240108, 0, 1));
	day.push_back(make_bar(20240109, 0, 2));
	day.push_back(make_bar(20240110, 0, 3));
	ASSERT_TRUE(write_bytes("his/day/SSE/600000.dsb",
		make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(day))));

	vector<WTSBarStruct> min5;
	min5.push_back(make_bar(20240108, TimeUtils::timeToMinBar(20240108, 1455), 11));
	min5.push_back(make_bar(20240109, TimeUtils::timeToMinBar(20240109, 1455), 12));
	ASSERT_TRUE(write_bytes("his/min5/SSE/600000.dsb",
		make_block(BT_HIS_Minute5, BLOCK_VERSION_RAW_V2, pack_bars(min5))));

	init_reader();
	_sink.set_clock(20240110, 1500, 0);

	WTSKlineSlice* sd = _reader.readKlineSlice("SSE.STK.600000", KP_DAY, 2, 0);
	ASSERT_TRUE(sd != NULL);
	vector<WTSBarStruct> gd = r52::flatten_bars(sd);
	ASSERT_EQ(gd.size(), (std::size_t)2);
	EXPECT_EQ(gd[0].date, 20240109u);
	EXPECT_EQ(gd[1].date, 20240110u);
	EXPECT_DOUBLE_EQ(gd[1].open, 13.0);
	sd->release();

	// min5：rt/min5/SSE/600000.dmb 不存在 → getRTKilneBlock 的 KP_Minute5 分支返回 NULL
	WTSKlineSlice* sm = _reader.readKlineSlice("SSE.STK.600000", KP_Minute5, 5, 202401101500ull);
	ASSERT_TRUE(sm != NULL);
	vector<WTSBarStruct> gm = r52::flatten_bars(sm);
	ASSERT_EQ(gm.size(), (std::size_t)2);
	EXPECT_EQ(gm[0].time % 10000, 1455u);
	EXPECT_EQ(gm[1].date, 20240109u);
	sm->release();
}

} // namespace r70
