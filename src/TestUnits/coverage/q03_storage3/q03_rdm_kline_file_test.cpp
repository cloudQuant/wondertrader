// q03 / WtRdmDtReader：K 线侧「文件形态」读取链路（his/*.dsb 建库 + 区间裁剪 + RT 块拼接）
//
// 覆盖（src/WtDataStorage/WtRdmDtReader.cpp）：
//  - readKlineSliceByRange :1789  缓存未命中→建库、etime 归一化、RT/his 两支取数、双块拼接
//  - cacheHisBarsFromFile  :1134  「普通合约」分支 :1568-1608（长度校验、四个版本位、尾部残缺）
//  - indexBarFromCacheByRange :1629 经公开入口驱动的左右闭区间裁剪（isDay / 非 isDay 两条比较器）
//  - getRTKilneBlock       :2162  rt/min1/<exchg>/<code>.dmb 只读映射 + 非分钟周期直接 NULL
//
// 同样不使用任何访问权限宏：_base_dir 保持空串 + Sandbox 切 CWD。
#include "q03_storage3_fixture.h"

#include "WtDataStorage/WtRdmDtReader.h"

namespace q03 {

class Q03RdmKlineTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("rdmkline"));
		_reader.init(NULL, &_sink);
	}

	void TearDown() override
	{
		_reader.clearCache();
		_sandbox.reset();
	}

	static string his_bar_file(const char* pname, const char* code = "600000")
	{
		return fmtutil::format("his/{}/SSE/{}.dsb", pname, code);
	}

	void write_his_bars(const char* pname, uint16_t version, const string& payload)
	{
		ASSERT_TRUE(write_bytes(his_bar_file(pname), make_block(BT_HIS_Day, version, payload)));
	}

	void write_his_bars(const char* pname, uint16_t version, const vector<WTSBarStruct>& bars)
	{
		write_his_bars(pname, version, pack_bars(bars));
	}

	void write_rt_bars(const char* subdir, uint32_t date, const vector<WTSBarStruct>& bars, uint32_t capacity = 300)
	{
		string f = fmtutil::format("rt/{}/SSE/600000.dmb", subdir);
		ASSERT_TRUE(write_bytes(f, make_rt_block<WTSBarStruct>(BT_RT_Minute1, date,
			(uint32_t)bars.size(), capacity, bars)));
	}

	// 分钟线 time 编码与生产一致：(date-19900000)*10000 + hhmm
	static uint64_t min_bar_time(uint32_t date, uint32_t hhmm)
	{
		return ((uint64_t)date - 19900000) * 10000 + hhmm;
	}

	static vector<WTSBarStruct> flatten(WTSKlineSlice* slice)
	{
		vector<WTSBarStruct> ret;
		if(slice == NULL)
			return ret;
		for(std::size_t i = 0; i < slice->get_block_counts(); i++)
		{
			WTSBarStruct* addr = slice->get_block_addr(i);
			uint32_t cnt = slice->get_block_size(i);
			if(addr == NULL)
				continue;
			for(uint32_t k = 0; k < cnt; k++)
				ret.push_back(addr[k]);
		}
		return ret;
	}

	// 日线样本：20240102 ~ 20240109 共 8 根
	static vector<WTSBarStruct> day_sample()
	{
		static const uint32_t dates[] = { 20240102, 20240103, 20240104, 20240105,
			20240108, 20240109, 20240110, 20240111 };
		vector<WTSBarStruct> bars;
		for(uint32_t i = 0; i < 8; i++)
		{
			WTSBarStruct bar = make_bar(dates[i], dates[i], i + 1);
			bars.push_back(bar);
		}
		return bars;
	}

	std::unique_ptr<Sandbox>	_sandbox;
	BaseDataMgr	_bd;
	HotMgr		_hot;
	RdmSink		_sink{ &_bd, &_hot };
	WtRdmDtReader	_reader;
};

TEST_F(Q03RdmKlineTest, day_dsb_raw_v2_full_range_roundtrip)
{
	vector<WTSBarStruct> bars = day_sample();
	write_his_bars("day", BLOCK_VERSION_RAW_V2, bars);

	// etime 落在过去（< curTDate 20240110）→ 不碰 RT 块，纯 his
	WTSKlineSlice* slice = _reader.readKlineSliceByRange("SSE.STK.600000", KP_DAY,
		202401010000ull, 202401090000ull);
	ASSERT_TRUE(slice != NULL);
	vector<WTSBarStruct> got = flatten(slice);
	// 20240102..20240109 共 6 根（右端闭区间）
	ASSERT_EQ(got.size(), (std::size_t)6);
	for(std::size_t i = 0; i < got.size(); i++)
		expect_bar_eq(got[i], bars[i]);
	EXPECT_EQ(slice->get_block_counts(), (std::size_t)1);
	slice->release();
}

TEST_F(Q03RdmKlineTest, day_etime_zero_is_normalized_to_203012312359)
{
	vector<WTSBarStruct> bars = day_sample();
	write_his_bars("day", BLOCK_VERSION_RAW_V2, bars);

	// :1807 etime==0 → 203012312359，KP_DAY 的 getRTKilneBlock 直接返回 NULL（:2164）
	WTSKlineSlice* slice = _reader.readKlineSliceByRange("SSE.STK.600000", KP_DAY,
		202401010000ull, 0);
	ASSERT_TRUE(slice != NULL);
	vector<WTSBarStruct> got = flatten(slice);
	ASSERT_EQ(got.size(), bars.size());
	expect_bar_eq(got.back(), bars.back());
	slice->release();
}

TEST_F(Q03RdmKlineTest, day_compressed_v2_roundtrip)
{
	// >100 根时 writer 侧会切到 BlockHeaderV2 + BLOCK_VERSION_CMP_V2，这里按同一形态建库
	vector<WTSBarStruct> bars;
	for(uint32_t i = 0; i < 120; i++)
		bars.push_back(make_bar(20240101 + i, 20240101 + i, i + 1));
	write_his_bars("day", BLOCK_VERSION_CMP_V2, bars);

	string raw = read_bytes(his_bar_file("day"));
	BlockHeaderV2 head;
	memcpy(&head, raw.data(), sizeof(head));
	EXPECT_EQ(head._version, BLOCK_VERSION_CMP_V2);
	EXPECT_EQ(raw.size(), (std::size_t)(sizeof(BlockHeaderV2) + head._size));

	WTSKlineSlice* slice = _reader.readKlineSliceByRange("SSE.STK.600000", KP_DAY,
		202401010000ull, 209912310000ull);
	ASSERT_TRUE(slice != NULL);
	vector<WTSBarStruct> got = flatten(slice);
	ASSERT_EQ(got.size(), bars.size());
	for(std::size_t i = 0; i < got.size(); i++)
		expect_bar_eq(got[i], bars[i]);
	slice->release();
}

TEST_F(Q03RdmKlineTest, day_old_struct_versions_are_upgraded)
{
	vector<WTSBarStructOld> old_bars;
	for(uint32_t i = 0; i < 4; i++)
		old_bars.push_back(make_old_bar(20240102 + i, 20240102 + i, i + 1));

	// BLOCK_VERSION_RAW：老结构体未压缩（WTSBarStructOld → WTSBarStruct 逐字段）
	write_his_bars("day", BLOCK_VERSION_RAW, pack_old_bars(old_bars));
	WTSKlineSlice* s1 = _reader.readKlineSliceByRange("SSE.STK.600000", KP_DAY,
		202401010000ull, 209912310000ull);
	ASSERT_TRUE(s1 != NULL);
	vector<WTSBarStruct> g1 = flatten(s1);
	ASSERT_EQ(g1.size(), old_bars.size());
	for(std::size_t i = 0; i < g1.size(); i++)
		expect_bar_upgraded(g1[i], old_bars[i]);
	s1->release();

	// BLOCK_VERSION_CMP：老结构体 + 压缩 → 解压后再升版
	_reader.clearCache();
	vector<WTSBarStructOld> old_bars2;
	for(uint32_t i = 0; i < 3; i++)
		old_bars2.push_back(make_old_bar(20240105 + i, 20240105 + i, i + 1));
	write_his_bars("day", BLOCK_VERSION_CMP, pack_old_bars(old_bars2));
	WTSKlineSlice* s2 = _reader.readKlineSliceByRange("SSE.STK.600000", KP_DAY,
		202401050000ull, 209912310000ull);
	ASSERT_TRUE(s2 != NULL);
	vector<WTSBarStruct> g2 = flatten(s2);
	ASSERT_EQ(g2.size(), old_bars2.size());
	for(std::size_t i = 0; i < g2.size(); i++)
		expect_bar_upgraded(g2[i], old_bars2[i]);
	s2->release();
}

TEST_F(Q03RdmKlineTest, day_tail_fragment_is_dropped)
{
	vector<WTSBarStruct> bars = day_sample();
	string payload = pack_bars(bars);
	payload.append(sizeof(WTSBarStruct) - 1, 'Q');	// 差 1 字节的残缺尾巴
	write_his_bars("day", BLOCK_VERSION_RAW_V2, payload);

	WTSKlineSlice* slice = _reader.readKlineSliceByRange("SSE.STK.600000", KP_DAY,
		202401010000ull, 209912310000ull);
	ASSERT_TRUE(slice != NULL);
	vector<WTSBarStruct> got = flatten(slice);
	ASSERT_EQ(got.size(), bars.size());
	expect_bar_eq(got.front(), bars.front());
	expect_bar_eq(got.back(), bars.back());
	slice->release();
}

TEST_F(Q03RdmKlineTest, day_size_check_failure_returns_null_and_logs_error)
{
	// 8 字节 < sizeof(HisKlineBlock)(12) → :1582 Sizechecking → cacheHisBarsFromFile 返回 false
	ASSERT_TRUE(write_bytes(his_bar_file("day"), string(8, 'X')));

	WTSKlineSlice* slice = _reader.readKlineSliceByRange("SSE.STK.600000", KP_DAY,
		202401010000ull, 202401090000ull);
	EXPECT_TRUE(slice == NULL);
	EXPECT_GE(_sink.count_of(LL_ERROR), 1u);
}

TEST_F(Q03RdmKlineTest, day_missing_file_caches_empty_entry)
{
	// 文件不存在（:1575 exists==false）→ realCnt==0 → 仍返回 true，空缓存
	WTSKlineSlice* slice = _reader.readKlineSliceByRange("SSE.STK.600000", KP_DAY,
		202401010000ull, 202401090000ull);
	EXPECT_TRUE(slice == NULL);
	EXPECT_EQ(_sink.count_of(LL_ERROR), 0u);
	EXPECT_GE(_sink.count_of(LL_INFO), 1u);

	// 第二次读同一个 key 必须命中缓存（不再产生「cached」日志）
	uint32_t before = _sink.count_of(LL_INFO);
	EXPECT_TRUE(_reader.readKlineSliceByRange("SSE.STK.600000", KP_DAY,
		202401010000ull, 202401090000ull) == NULL);
	EXPECT_EQ(_sink.count_of(LL_INFO), before);
}

TEST_F(Q03RdmKlineTest, day_window_out_of_range_returns_null)
{
	vector<WTSBarStruct> bars = day_sample();
	write_his_bars("day", BLOCK_VERSION_RAW_V2, bars);

	// 左端晚于全部缓存 → indexBarFromCacheByRange 命中空窗口
	EXPECT_TRUE(_reader.readKlineSliceByRange("SSE.STK.600000", KP_DAY,
		202402010000ull, 202402280000ull) == NULL);
	// 右端早于全部缓存
	EXPECT_TRUE(_reader.readKlineSliceByRange("SSE.STK.600000", KP_DAY,
		202401010000ull, 202401020000ull - 1) == NULL);
}

TEST_F(Q03RdmKlineTest, day_range_and_count_handle_first_cached_bar_boundaries)
{
	vector<WTSBarStruct> bars;
	bars.push_back(make_bar(20240105, 20240105, 1));
	write_his_bars("day", BLOCK_VERSION_RAW_V2, bars);

	const uint64_t before_first = 202401040000ull;
	const uint64_t first_bar = 202401050000ull;
	const uint64_t after_first = 202401060000ull;

	// A range wholly before the first cached bar is empty; the first bar itself
	// is included, while a range wholly after this one-bar cache is empty.
	EXPECT_EQ(nullptr, _reader.readKlineSliceByRange("SSE.STK.600000", KP_DAY,
		before_first, before_first));
	WTSKlineSlice* first_range = _reader.readKlineSliceByRange("SSE.STK.600000", KP_DAY,
		first_bar, first_bar);
	ASSERT_NE(nullptr, first_range);
	vector<WTSBarStruct> first_range_bars = flatten(first_range);
	ASSERT_EQ(first_range_bars.size(), (std::size_t)1);
	expect_bar_eq(first_range_bars[0], bars[0]);
	first_range->release();
	EXPECT_EQ(nullptr, _reader.readKlineSliceByRange("SSE.STK.600000", KP_DAY,
		after_first, after_first));

	// Count lookup before the first bar is empty. At and after it, the only
	// available bar is returned with all of its fields intact.
	EXPECT_EQ(nullptr, _reader.readKlineSliceByCount("SSE.STK.600000", KP_DAY, 1,
		before_first));
	WTSKlineSlice* first_count = _reader.readKlineSliceByCount("SSE.STK.600000", KP_DAY, 1,
		first_bar);
	ASSERT_NE(nullptr, first_count);
	vector<WTSBarStruct> first_count_bars = flatten(first_count);
	ASSERT_EQ(first_count_bars.size(), (std::size_t)1);
	expect_bar_eq(first_count_bars[0], bars[0]);
	first_count->release();
	WTSKlineSlice* later_count = _reader.readKlineSliceByCount("SSE.STK.600000", KP_DAY, 2,
		after_first);
	ASSERT_NE(nullptr, later_count);
	vector<WTSBarStruct> later_count_bars = flatten(later_count);
	ASSERT_EQ(later_count_bars.size(), (std::size_t)1);
	expect_bar_eq(later_count_bars[0], bars[0]);
	later_count->release();
}

TEST_F(Q03RdmKlineTest, min1_splices_his_block_with_rt_block)
{
	// his/min1 两根（20240108 全天）+ rt/min1 两根（20240110 当日）
	vector<WTSBarStruct> his;
	his.push_back(make_bar(20240108, min_bar_time(20240108, 930), 1));
	his.push_back(make_bar(20240108, min_bar_time(20240108, 931), 2));
	write_his_bars("min1", BLOCK_VERSION_RAW_V2, his);

	vector<WTSBarStruct> rt;
	rt.push_back(make_bar(20240110, min_bar_time(20240110, 930), 3));
	rt.push_back(make_bar(20240110, min_bar_time(20240110, 931), 4));
	write_rt_bars("min1", 20240110, rt);

	// stime 早于 his 首根 → :1888 「实时K线要全部包含进去」→ 保留 his + 全部 rt，bNeedHisData 仍为 true
	WTSKlineSlice* slice = _reader.readKlineSliceByRange("SSE.STK.600000", KP_Minute1,
		202401080000ull, 202401101500ull);
	ASSERT_TRUE(slice != NULL);
	vector<WTSBarStruct> got = flatten(slice);
	ASSERT_EQ(got.size(), (std::size_t)4);
	EXPECT_EQ(slice->get_block_counts(), (std::size_t)2);
	expect_bar_eq(got[0], his[0]);
	expect_bar_eq(got[1], his[1]);
	expect_bar_eq(got[2], rt[0]);
	expect_bar_eq(got[3], rt[1]);
	slice->release();
}

TEST_F(Q03RdmKlineTest, min1_rt_only_when_start_inside_rt_block)
{
	vector<WTSBarStruct> his;
	his.push_back(make_bar(20240108, min_bar_time(20240108, 930), 1));
	write_his_bars("min1", BLOCK_VERSION_RAW_V2, his);

	vector<WTSBarStruct> rt;
	for(uint32_t i = 0; i < 5; i++)
		rt.push_back(make_bar(20240110, min_bar_time(20240110, 930) + i, i + 2));
	write_rt_bars("min1", 20240110, rt);

	// stime 落在 RT 块内部（930 起第 3 根）→ :1895 左边界查找 + bNeedHisData=false → 只剩 RT 一块
	WTSKlineSlice* slice = _reader.readKlineSliceByRange("SSE.STK.600000", KP_Minute1,
		202401100932ull, 202401101500ull);
	ASSERT_TRUE(slice != NULL);
	vector<WTSBarStruct> got = flatten(slice);
	EXPECT_EQ(slice->get_block_counts(), (std::size_t)1);
	// idx 由右边界定为末根（etime 15:00 晚于全部 RT），左边界裁掉前 2 根
	ASSERT_EQ(got.size(), (std::size_t)3);
	for(std::size_t i = 0; i < got.size(); i++)
		expect_bar_eq(got[i], rt[i + 2]);
	slice->release();
}

TEST_F(Q03RdmKlineTest, min1_right_window_drops_late_bars)
{
	vector<WTSBarStruct> rt;
	for(uint32_t i = 0; i < 6; i++)
		rt.push_back(make_bar(20240110, min_bar_time(20240110, 1000) + i, i + 1));
	write_rt_bars("min1", 20240110, rt);

	vector<WTSBarStruct> his;
	his.push_back(make_bar(20240108, min_bar_time(20240108, 1000), 9));
	write_his_bars("min1", BLOCK_VERSION_RAW_V2, his);

	// etime = 10:02 → :1880 命中「定位比目标大」→ idx 回退一根
	WTSKlineSlice* slice = _reader.readKlineSliceByRange("SSE.STK.600000", KP_Minute1,
		202401101000ull, 202401101002ull);
	ASSERT_TRUE(slice != NULL);
	vector<WTSBarStruct> got = flatten(slice);
	EXPECT_EQ(got.size(), (std::size_t)3);
	for(std::size_t i = 0; i < got.size(); i++)
		expect_bar_eq(got[i], rt[i]);
	slice->release();
}

TEST_F(Q03RdmKlineTest, empty_rt_blocks_fall_back_to_history_for_range_and_count)
{
	struct PeriodCase
	{
		WTSKlinePeriod period;
		const char* subdir;
		uint16_t rt_type;
		uint16_t his_type;
		const char* empty_code;
		const char* history_code;
		const char* single_code;
	};
	const PeriodCase cases[] = {
		{ KP_Minute1, "min1", BT_RT_Minute1, BT_HIS_Minute1, "600010", "600012", "600014" },
		{ KP_Minute5, "min5", BT_RT_Minute5, BT_HIS_Minute5, "600011", "600013", "600015" }
	};
	const uint32_t kCurDate = 20240110;
	const uint64_t kStart = 202401100900ull;
	const uint64_t kEnd = 202401101500ull;
	const uint64_t kHistoryStart = 202401090000ull;

	for (const PeriodCase& test_case : cases)
	{
		const vector<WTSBarStruct> empty;
		const string empty_rt_path = fmtutil::format("rt/{}/SSE/{}.dmb", test_case.subdir, test_case.empty_code);
		ASSERT_TRUE(write_bytes(empty_rt_path, make_rt_block<WTSBarStruct>(
			test_case.rt_type, kCurDate, 0, 8, empty)));

		const string empty_std_code = fmtutil::format("SSE.STK.{}", test_case.empty_code);
		WTSKlineSlice* no_data_range = _reader.readKlineSliceByRange(
			empty_std_code.c_str(), test_case.period, kStart, kEnd);
		EXPECT_TRUE(no_data_range == NULL || no_data_range->size() == 0u);
		if (no_data_range != NULL)
			no_data_range->release();
		WTSKlineSlice* no_data_count = _reader.readKlineSliceByCount(
			empty_std_code.c_str(), test_case.period, 3, kEnd);
		EXPECT_TRUE(no_data_count == NULL || no_data_count->size() == 0u);
		if (no_data_count != NULL)
			no_data_count->release();

		vector<WTSBarStruct> historical;
		historical.push_back(make_bar(20240109, min_bar_time(20240109, 1455), 31));
		const string history_rt_path = fmtutil::format("rt/{}/SSE/{}.dmb", test_case.subdir, test_case.history_code);
		ASSERT_TRUE(write_bytes(history_rt_path, make_rt_block<WTSBarStruct>(
			test_case.rt_type, kCurDate, 0, 8, empty)));
		const string history_path = fmtutil::format("his/{}/SSE/{}.dsb", test_case.subdir, test_case.history_code);
		ASSERT_TRUE(write_bytes(history_path, make_block(
			test_case.his_type, BLOCK_VERSION_RAW_V2, pack_bars(historical))));

		const string history_std_code = fmtutil::format("SSE.STK.{}", test_case.history_code);
		WTSKlineSlice* history_range = _reader.readKlineSliceByRange(
			history_std_code.c_str(), test_case.period, kHistoryStart, kEnd);
		ASSERT_NE(history_range, nullptr);
		vector<WTSBarStruct> range_bars = flatten(history_range);
		ASSERT_EQ(range_bars.size(), (std::size_t)1);
		expect_bar_eq(range_bars[0], historical[0]);
		history_range->release();

		WTSKlineSlice* history_count = _reader.readKlineSliceByCount(
			history_std_code.c_str(), test_case.period, 3, kEnd);
		ASSERT_NE(history_count, nullptr);
		vector<WTSBarStruct> count_bars = flatten(history_count);
		ASSERT_EQ(count_bars.size(), (std::size_t)1);
		expect_bar_eq(count_bars[0], historical[0]);
		history_count->release();

		vector<WTSBarStruct> single_rt;
		single_rt.push_back(make_bar(kCurDate, min_bar_time(kCurDate, 1450), 41));
		const string single_rt_path = fmtutil::format("rt/{}/SSE/{}.dmb", test_case.subdir, test_case.single_code);
		ASSERT_TRUE(write_bytes(single_rt_path, make_rt_block<WTSBarStruct>(
			test_case.rt_type, kCurDate, 1, 8, single_rt)));
		const string single_std_code = fmtutil::format("SSE.STK.{}", test_case.single_code);
		WTSKlineSlice* single_range = _reader.readKlineSliceByRange(
			single_std_code.c_str(), test_case.period, kStart, kEnd);
		ASSERT_NE(single_range, nullptr);
		ASSERT_EQ(single_range->size(), 1u);
		ASSERT_NE(single_range->at(0), nullptr);
		expect_bar_eq(*single_range->at(0), single_rt[0]);
		single_range->release();

		WTSKlineSlice* single_count = _reader.readKlineSliceByCount(
			single_std_code.c_str(), test_case.period, 1, kEnd);
		ASSERT_NE(single_count, nullptr);
		ASSERT_EQ(single_count->size(), 1u);
		ASSERT_NE(single_count->at(0), nullptr);
		expect_bar_eq(*single_count->at(0), single_rt[0]);
		single_count->release();
	}
}

TEST_F(Q03RdmKlineTest, min1_before_first_rt_bar_falls_back_to_history_for_range_and_count)
{
	const char* code = "600016";
	const uint32_t cur_date = 20240110;
	const uint64_t rt_first = 202401100930ull;
	const uint64_t before_first = 202401100929ull;
	const uint64_t after_first = 202401100931ull;
	vector<WTSBarStruct> historical;
	historical.push_back(make_bar(20240109, min_bar_time(20240109, 1455), 51));
	vector<WTSBarStruct> realtime;
	realtime.push_back(make_bar(cur_date, min_bar_time(cur_date, 930), 61));

	ASSERT_TRUE(write_bytes("his/min1/SSE/600016.dsb", make_block(
		BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, pack_bars(historical))));
	ASSERT_TRUE(write_bytes("rt/min1/SSE/600016.dmb", make_rt_block<WTSBarStruct>(
		BT_RT_Minute1, cur_date, 1, 8, realtime)));

	const string std_code = fmtutil::format("SSE.STK.{}", code);

	// A request ending before the first RT bar must retain only eligible history.
	WTSKlineSlice* before_range = _reader.readKlineSliceByRange(
		std_code.c_str(), KP_Minute1, 202401090000ull, before_first);
	ASSERT_NE(nullptr, before_range);
	vector<WTSBarStruct> before_range_bars = flatten(before_range);
	ASSERT_EQ(before_range_bars.size(), (std::size_t)1);
	expect_bar_eq(before_range_bars[0], historical[0]);
	before_range->release();

	// Equal and later right edges include the RT bar without underflowing the index.
	WTSKlineSlice* equal_range = _reader.readKlineSliceByRange(
		std_code.c_str(), KP_Minute1, rt_first, rt_first);
	ASSERT_NE(nullptr, equal_range);
	vector<WTSBarStruct> equal_range_bars = flatten(equal_range);
	ASSERT_EQ(equal_range_bars.size(), (std::size_t)1);
	expect_bar_eq(equal_range_bars[0], realtime[0]);
	equal_range->release();

	WTSKlineSlice* after_range = _reader.readKlineSliceByRange(
		std_code.c_str(), KP_Minute1, rt_first, after_first);
	ASSERT_NE(nullptr, after_range);
	vector<WTSBarStruct> after_range_bars = flatten(after_range);
	ASSERT_EQ(after_range_bars.size(), (std::size_t)1);
	expect_bar_eq(after_range_bars[0], realtime[0]);
	after_range->release();

	// Count lookups before the first RT bar fall back to history; equal/later use RT.
	WTSKlineSlice* before_count = _reader.readKlineSliceByCount(
		std_code.c_str(), KP_Minute1, 2, before_first);
	ASSERT_NE(nullptr, before_count);
	vector<WTSBarStruct> before_count_bars = flatten(before_count);
	ASSERT_EQ(before_count_bars.size(), (std::size_t)1);
	expect_bar_eq(before_count_bars[0], historical[0]);
	before_count->release();

	WTSKlineSlice* equal_count = _reader.readKlineSliceByCount(
		std_code.c_str(), KP_Minute1, 1, rt_first);
	ASSERT_NE(nullptr, equal_count);
	vector<WTSBarStruct> equal_count_bars = flatten(equal_count);
	ASSERT_EQ(equal_count_bars.size(), (std::size_t)1);
	expect_bar_eq(equal_count_bars[0], realtime[0]);
	equal_count->release();

	WTSKlineSlice* after_count = _reader.readKlineSliceByCount(
		std_code.c_str(), KP_Minute1, 1, after_first);
	ASSERT_NE(nullptr, after_count);
	vector<WTSBarStruct> after_count_bars = flatten(after_count);
	ASSERT_EQ(after_count_bars.size(), (std::size_t)1);
	expect_bar_eq(after_count_bars[0], realtime[0]);
	after_count->release();
}

TEST_F(Q03RdmKlineTest, clear_cache_forces_rebuild_from_file)
{
	vector<WTSBarStruct> bars = day_sample();
	write_his_bars("day", BLOCK_VERSION_RAW_V2, bars);

	WTSKlineSlice* s1 = _reader.readKlineSliceByRange("SSE.STK.600000", KP_DAY,
		202401010000ull, 202401090000ull);
	ASSERT_TRUE(s1 != NULL);
	EXPECT_EQ(flatten(s1).size(), (std::size_t)6);
	s1->release();

	// 换文件内容后不清缓存 → 仍旧数据；clearCache 后重建 → 新数据
	vector<WTSBarStruct> two;
	two.push_back(make_bar(20240102, 20240102, 9));
	two.push_back(make_bar(20240103, 20240103, 8));
	write_his_bars("day", BLOCK_VERSION_RAW_V2, two);

	WTSKlineSlice* s2 = _reader.readKlineSliceByRange("SSE.STK.600000", KP_DAY,
		202401010000ull, 202401090000ull);
	ASSERT_TRUE(s2 != NULL);
	EXPECT_EQ(flatten(s2).size(), (std::size_t)6);
	s2->release();

	_reader.clearCache();
	WTSKlineSlice* s3 = _reader.readKlineSliceByRange("SSE.STK.600000", KP_DAY,
		202401010000ull, 202401090000ull);
	ASSERT_TRUE(s3 != NULL);
	vector<WTSBarStruct> g3 = flatten(s3);
	ASSERT_EQ(g3.size(), (std::size_t)2);
	expect_bar_eq(g3[0], two[0]);
	expect_bar_eq(g3[1], two[1]);
	s3->release();
}

} // namespace q03
