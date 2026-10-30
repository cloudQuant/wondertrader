// R1 / src/WtDataStorage/WtDataReader.cpp — K 线「历史文件 + 实时块」读取层
//
// 覆盖（公开入口 init(cfg!=NULL) 无轮询线程，p09 已实测；本文件补 q03 没测的 bars 侧）：
//  - readKlineSlice :1720 非当日尾窗 :1906-1912（hisCnt=count 尾部截取）
//  - cacheHisBarsFromFile :1564 起：his/day|min1|min5/<exchg>/<code>.dsb 尺寸校验、
//    proc_block_data 升版读回（RAW 老结构体 / CMP / RAW_V2 / CMP_V2）
//  - getRTKilneBlock :2131：rt/min1/<exchg>/<code>.dmb 只读映射 + 当日 min1 历史/实时拼接
//  - _bars_cache 命中后不再触盘（删文件仍可读）；文件缺失 → 空切片
//
// 沙箱：Sandbox chdir + init cfg.path=root_abs()，路径绝对、与仓库 dist/ 无关。
#include "../q03_storage3/q03_storage3_fixture.h"

#include "WtDataStorage/WtDataReader.h"

namespace r1 {

using namespace q03;

class R1ReaderBarsTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r1bars"));
		string json = fmtutil::format(R"({{"path":"{}"}})", _sandbox->root_abs());
		ASSERT_TRUE(write_bytes("r1_cfg.json", json));
		WTSVariant* cfg = WTSCfgLoader::load_from_file("r1_cfg.json");
		ASSERT_TRUE(cfg != NULL);
		_reader.init(cfg, &_sink, NULL);
		cfg->release();
		_sink.set_clock(20240110, 1500, 0);
	}

	void TearDown() override
	{
		_sandbox.reset();
	}

	static string his_kline_file(const char* pname, const char* code)
	{
		return fmtutil::format("his/{}/SSE/{}.dsb", pname, code);
	}

	static string rt_kline_file(const char* pname, const char* code)
	{
		return fmtutil::format("rt/{}/SSE/{}.dmb", pname, code);
	}

	// WtDataReader 内部把 min1 bar.time 编码为 (date-19900000)*10000+hhmm
	static uint64_t enc_min1_time(uint32_t date, uint32_t hhmm)
	{
		return (uint64_t)(date - 19900000) * 10000 + hhmm;
	}

	std::unique_ptr<Sandbox>	_sandbox;
	BaseDataMgr	_bd;
	HotMgr		_hot;
	ReaderSink	_sink{ &_bd, &_hot };
	WtDataReader	_reader;
};

TEST_F(R1ReaderBarsTest, day_his_raw_v2_tail_window_by_count)
{
	vector<WTSBarStruct> bars = {
		make_bar(20240108, 20240108, 0),
		make_bar(20240109, 20240109, 1),
		make_bar(20240110, 20240110, 2)
	};
	ASSERT_TRUE(write_bytes(his_kline_file("day", "610001"),
		make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(bars))));

	// etime 晚于预置的 20240108/09/10 数据且不是当日 → 纯 his 尾窗：count=2 取最后两条
	WTSKlineSlice* slice = _reader.readKlineSlice("SSE.STK.610001", KP_DAY, 2, 202401111500ull);
	ASSERT_TRUE(slice != NULL);
	ASSERT_EQ(slice->size(), 2);
	expect_bar_eq(*slice->at(0), bars[1]);
	expect_bar_eq(*slice->at(1), bars[2]);
	// count 超过总量时钳制到全量
	WTSKlineSlice* all = _reader.readKlineSlice("SSE.STK.610001", KP_DAY, 10, 202401111500ull);
	ASSERT_TRUE(all != NULL);
	EXPECT_EQ(all->size(), 3);
	expect_bar_eq(*all->at(0), bars[0]);
	all->release();
	slice->release();
}

TEST_F(R1ReaderBarsTest, day_his_cmp_v2_and_old_struct_upgrade)
{
	vector<WTSBarStruct> bars = { make_bar(20240108, 20240108, 0), make_bar(20240109, 20240109, 1) };
	ASSERT_TRUE(write_bytes(his_kline_file("day", "610002"),
		make_block(BT_HIS_Day, BLOCK_VERSION_CMP_V2, pack_bars(bars))));
	WTSKlineSlice* cmp = _reader.readKlineSlice("SSE.STK.610002", KP_DAY, 2, 202401111500ull);
	ASSERT_TRUE(cmp != NULL);
	ASSERT_EQ(cmp->size(), 2);
	expect_bar_eq(*cmp->at(1), bars[1]);
	cmp->release();

	vector<WTSBarStructOld> oldBars = { make_old_bar(20240108, 20240108, 0), make_old_bar(20240109, 20240109, 1) };
	ASSERT_TRUE(write_bytes(his_kline_file("day", "610003"),
		make_block(BT_HIS_Day, BLOCK_VERSION_RAW, pack_old_bars(oldBars))));
	WTSKlineSlice* up = _reader.readKlineSlice("SSE.STK.610003", KP_DAY, 2, 202401111500ull);
	ASSERT_TRUE(up != NULL);
	ASSERT_EQ(up->size(), 2);
	expect_bar_upgraded(*up->at(1), oldBars[1]);
	up->release();
}

TEST_F(R1ReaderBarsTest, day_his_cutoff_excludes_later_bar)
{
	vector<WTSBarStruct> bars = {
		make_bar(20240108, 20240108, 0),
		make_bar(20240110, 20240110, 1)
	};
	ASSERT_TRUE(write_bytes(his_kline_file("day", "610009"),
		make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(bars))));

	// 202401091500 严格晚于目标日 20240108，早于后续 bar 所在的 20240110。
	// 这个中间日期用例验证截止时间不会把之后的 bar 带入切片。
	WTSKlineSlice* slice = _reader.readKlineSlice("SSE.STK.610009", KP_DAY, 2, 202401091500ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->size(), 1);
	for(int32_t i = 0; i < slice->size(); i++)
		EXPECT_LE(slice->at(i)->date, 20240109u);
	if(slice->size() == 1)
		expect_bar_eq(*slice->at(0), bars[0]);
	slice->release();
}

TEST_F(R1ReaderBarsTest, day_his_cutoff_is_inclusive_and_clamps_to_available_history)
{
	vector<WTSBarStruct> bars = {
		make_bar(20240108, 20240108, 0),
		make_bar(20240109, 20240109, 1),
		make_bar(20240110, 20240110, 2)
	};
	ASSERT_TRUE(write_bytes(his_kline_file("day", "610010"),
		make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(bars))));

	// A cutoff before the first daily bar returns an empty slice.
	WTSKlineSlice* before = _reader.readKlineSlice("SSE.STK.610010", KP_DAY, 5, 202401071500ull);
	ASSERT_TRUE(before != NULL);
	EXPECT_TRUE(before->empty());
	before->release();

	// Daily bars are keyed by trading date; the cutoff date itself is included.
	WTSKlineSlice* exact = _reader.readKlineSlice("SSE.STK.610010", KP_DAY, 5, 202401091500ull);
	ASSERT_TRUE(exact != NULL);
	ASSERT_EQ(exact->size(), 2);
	expect_bar_eq(*exact->at(0), bars[0]);
	expect_bar_eq(*exact->at(1), bars[1]);
	exact->release();
}

TEST_F(R1ReaderBarsTest, day_size_check_failure_logs_error_and_returns_empty)
{
	// 比 HisKlineBlock 头还短 → cacheHisBarsFromFile return false（:1591 LL_ERROR）
	ASSERT_TRUE(write_bytes(his_kline_file("day", "610004"), string(8, 'q')));
	uint32_t err_before = _sink.count_of(LL_ERROR);
	WTSKlineSlice* slice = _reader.readKlineSlice("SSE.STK.610004", KP_DAY, 1, 202401081500ull);
	EXPECT_GT(_sink.count_of(LL_ERROR), err_before);
	if(slice != NULL)
	{
		EXPECT_TRUE(slice->empty());
		slice->release();
	}
}

TEST_F(R1ReaderBarsTest, min5_his_roundtrip_uses_min5_dir)
{
	vector<WTSBarStruct> bars;
	for(uint32_t i = 0; i < 6; i++)
		bars.push_back(make_bar(20240108, enc_min1_time(20240108, 930 + i * 5), i));
	ASSERT_TRUE(write_bytes(his_kline_file("min5", "610005"),
		make_block(BT_HIS_Minute5, BLOCK_VERSION_RAW_V2, pack_bars(bars))));

	WTSKlineSlice* slice = _reader.readKlineSlice("SSE.STK.610005", KP_Minute5, 3, 202401081500ull);
	ASSERT_TRUE(slice != NULL);
	ASSERT_EQ(slice->size(), 3);
	expect_bar_eq(*slice->at(0), bars[3]);
	expect_bar_eq(*slice->at(2), bars[5]);
	slice->release();
}

TEST_F(R1ReaderBarsTest, min1_his_cutoff_uses_encoded_calendar_time)
{
	vector<WTSBarStruct> bars;
	for (uint32_t i = 0; i < 3; i++)
		bars.push_back(make_bar(20240108, enc_min1_time(20240108, 930 + i), i));
	ASSERT_TRUE(write_bytes(his_kline_file("min1", "610011"),
		make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, pack_bars(bars))));

	// The etime minute is inclusive; a larger count is clamped to bars at or
	// before that encoded calendar time.
	WTSKlineSlice* exact = _reader.readKlineSlice("SSE.STK.610011", KP_Minute1, 10, 202401080931ull);
	ASSERT_TRUE(exact != NULL);
	ASSERT_EQ(exact->size(), 2);
	expect_bar_eq(*exact->at(0), bars[0]);
	expect_bar_eq(*exact->at(1), bars[1]);
	exact->release();

	// A cutoff before the first bar returns no history.
	WTSKlineSlice* before = _reader.readKlineSlice("SSE.STK.610011", KP_Minute1, 2, 202401080929ull);
	ASSERT_TRUE(before != NULL);
	EXPECT_TRUE(before->empty());
	before->release();

	// After the final bar, the count limit selects the trailing two bars.
	WTSKlineSlice* after = _reader.readKlineSlice("SSE.STK.610011", KP_Minute1, 2, 202401080933ull);
	ASSERT_TRUE(after != NULL);
	ASSERT_EQ(after->size(), 2);
	expect_bar_eq(*after->at(0), bars[1]);
	expect_bar_eq(*after->at(1), bars[2]);
	after->release();
}

TEST_F(R1ReaderBarsTest, min1_splices_his_tail_with_rt_block_of_today)
{
	// 历史 min1：20240108 两根
	vector<WTSBarStruct> his;
	his.push_back(make_bar(20240108, enc_min1_time(20240108, 1425), 100));
	his.push_back(make_bar(20240108, enc_min1_time(20240108, 1430), 101));
	ASSERT_TRUE(write_bytes(his_kline_file("min1", "610006"),
		make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, pack_bars(his))));

	// 当日 rt min1 块：20240110 两根（930/931），日期=curTDate → bHasToday
	vector<WTSBarStruct> rt;
	rt.push_back(make_bar(20240110, enc_min1_time(20240110, 930), 200));
	rt.push_back(make_bar(20240110, enc_min1_time(20240110, 931), 201));
	ASSERT_TRUE(write_bytes(rt_kline_file("min1", "610006"),
		make_rt_block<WTSBarStruct>(BT_RT_Minute1, 20240110, (uint32_t)rt.size(), 8, rt)));

	WTSKlineSlice* slice = _reader.readKlineSlice("SSE.STK.610006", KP_Minute1, 4, 202401101500ull);
	ASSERT_TRUE(slice != NULL);
	// rt 2 根全部命中 + his 尾 2 根 = 4
	ASSERT_EQ(slice->size(), 4);
	expect_bar_eq(*slice->at(0), his[0]);
	expect_bar_eq(*slice->at(1), his[1]);
	expect_bar_eq(*slice->at(2), rt[0]);
	expect_bar_eq(*slice->at(3), rt[1]);
	slice->release();

	// count=1 只要最后一条 → 必为 rt[1]
	WTSKlineSlice* one = _reader.readKlineSlice("SSE.STK.610006", KP_Minute1, 1, 202401101500ull);
	ASSERT_TRUE(one != NULL);
	ASSERT_EQ(one->size(), 1);
	expect_bar_eq(*one->at(0), rt[1]);
	one->release();
}

TEST_F(R1ReaderBarsTest, bars_cache_hit_survives_file_deletion)
{
	vector<WTSBarStruct> bars = { make_bar(20240108, 20240108, 0), make_bar(20240109, 20240109, 1) };
	string f = his_kline_file("day", "610007");
	ASSERT_TRUE(write_bytes(f, make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(bars))));

	WTSKlineSlice* first = _reader.readKlineSlice("SSE.STK.610007", KP_DAY, 2, 202401111500ull);
	ASSERT_TRUE(first != NULL);
	ASSERT_EQ(first->size(), 2);
	first->release();

	std::error_code ec;
	std::filesystem::remove(f, ec);
	WTSKlineSlice* second = _reader.readKlineSlice("SSE.STK.610007", KP_DAY, 2, 202401111500ull);
	ASSERT_TRUE(second != NULL);
	ASSERT_EQ(second->size(), 2);
	expect_bar_eq(*second->at(0), bars[0]);		// 缓存命中：不再触盘
	second->release();
}

TEST_F(R1ReaderBarsTest, missing_his_file_returns_empty_slice_not_null)
{
	// 非当日、无文件：bHasHisData=false → 走 :1906 else 尾窗（barsList 空 → size 0 切片）
	WTSKlineSlice* slice = _reader.readKlineSlice("SSE.STK.610008", KP_DAY, 5, 202401081500ull);
	if(slice == NULL)
	{
		FAIL() << "WtDataReader::readKlineSlice 对未知代码返回了 NULL（当前实现恒返回切片对象）";
	}
	EXPECT_TRUE(slice->empty());
	slice->release();
}

TEST_F(R1ReaderBarsTest, header_only_his_block_returns_empty_slice)
{
	// 一个合法 RAW_V2 块头、零条 bar：cacheHisBarsFromFile 应保留可读的空缓存。
	ASSERT_TRUE(write_bytes(his_kline_file("day", "610012"),
		make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(vector<WTSBarStruct>{}))));

	WTSKlineSlice* slice = _reader.readKlineSlice("SSE.STK.610012", KP_DAY, 5, 202401081500ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_TRUE(slice->empty());
	slice->release();
}

TEST_F(R1ReaderBarsTest, zero_count_returns_empty_slice_for_nonempty_history)
{
	vector<WTSBarStruct> bars = { make_bar(20240108, 20240108, 0) };
	ASSERT_TRUE(write_bytes(his_kline_file("day", "610013"),
		make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(bars))));

	// count=0 is an empty request even when history is available; it must not
	// form a bar address from an empty [size, size) range.
	WTSKlineSlice* slice = _reader.readKlineSlice("SSE.STK.610013", KP_DAY, 0, 202401111500ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_TRUE(slice->empty());
	slice->release();
}

} // namespace r1
