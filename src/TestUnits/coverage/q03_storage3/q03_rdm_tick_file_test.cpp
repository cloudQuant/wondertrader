// q03 / WtRdmDtReader：tick 侧「文件形态」读取链路（无共享内存段、无线程、无墙钟断言）
//
// 覆盖（src/WtDataStorage/WtRdmDtReader.cpp）：
//  - readTickSliceByDate  :239   his/ticks/<exchg>/<date>/<code>.dsb 建块、长度校验、尾部残缺记录
//  - readTickSliceByRange :363   多交易日跨文件拼接 + 左右边界裁剪 + 当日 RT 块接续
//  - getRTTickBlock       :2002  rt/ticks/<exchg>/<code>.dmb 只读映射（与 writer 侧同一文件形态）
//  - proc_block_data 的四个版本位读回闭环（RAW / CMP / RAW_V2 / CMP_V2，含老结构体升级）
//  - readTickSliceByRange 尾段 :608（永远返回切片对象，空结果不返回 NULL）
//
// 白盒手法：**没有** #define private public。_base_dir 保持默认空串（init(cfg!=NULL) 会在 :87 起
// 5 秒轮询线程、析构 :66 join，本波禁止），于是生产代码拼出的 "his/ticks/..." 变成相对路径，
// 由 Sandbox 把 CWD 切到 temp_directory_path()/q03_rdmtk_<pid>_<seq>/data，TearDown 还原并 remove_all。
#include "q03_storage3_fixture.h"

#include <stdexcept>

#include "WtDataStorage/WtRdmDtReader.h"

namespace q03 {

class RuleTagHotMgr : public HotMgr
{
public:
	// 让 CodeHelper::extractStdCode 走进「主力连续」分支（:648 起）
	const char* getRuleTag(const char*) override { return "HOT"; }
	bool splitCustomSections(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
};

class Q03RdmTickTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("rdmtk"));
		_reader.init(NULL, &_sink);
	}

	void TearDown() override
	{
		_reader.clearCache();
		_sandbox.reset();
	}

	static string his_tick_file(uint32_t date, const char* code = "600000")
	{
		return fmtutil::format("his/ticks/SSE/{}/{}.dsb", date, code);
	}

	void write_his_ticks(uint32_t date, uint16_t version, const string& payload)
	{
		ASSERT_TRUE(write_bytes(his_tick_file(date), make_block(BT_HIS_Ticks, version, payload)));
	}

	void write_his_ticks(uint32_t date, uint16_t version, const vector<WTSTickStruct>& ticks)
	{
		write_his_ticks(date, version, pack_ticks(ticks));
	}

	void write_rt_ticks(uint32_t date, const vector<WTSTickStruct>& ticks, uint32_t capacity = 8)
	{
		ASSERT_TRUE(write_bytes("rt/ticks/SSE/600000.dmb",
			make_rt_block<WTSTickStruct>(BT_RT_Ticks, date, (uint32_t)ticks.size(), capacity, ticks)));
	}

	// 把切片里所有块按顺序摊平（create(stdCode,NULL,0) 可能带一个 0 长首块）
	static vector<WTSTickStruct> flatten(WTSTickSlice* slice)
	{
		vector<WTSTickStruct> ret;
		if(slice == NULL)
			return ret;
		for(std::size_t i = 0; i < slice->get_block_counts(); i++)
		{
			WTSTickStruct* addr = slice->get_block_addr(i);
			uint32_t cnt = slice->get_block_size(i);
			if(addr == NULL)
				continue;
			for(uint32_t k = 0; k < cnt; k++)
				ret.push_back(addr[k]);
		}
		return ret;
	}

	static vector<WTSTickStruct> expect_of(const vector<WTSTickStruct>& src, std::size_t from, std::size_t to)
	{
		return vector<WTSTickStruct>(src.begin() + from, src.begin() + to);
	}

	std::unique_ptr<Sandbox>	_sandbox;
	BaseDataMgr	_bd;
	HotMgr		_hot;
	RdmSink		_sink{ &_bd, &_hot };
	WtRdmDtReader	_reader;
};

// ---------------------------------------------------------------- his/ticks/*.dsb
TEST_F(Q03RdmTickTest, his_tick_raw_v2_roundtrip_field_by_field)
{
	vector<WTSTickStruct> ticks;
	ticks.push_back(make_tick(20240108, 20240108, 93000000, 1));
	ticks.push_back(make_tick(20240108, 20240108, 103000000, 2));
	ticks.push_back(make_tick(20240108, 20240108, 145900000, 3));
	write_his_ticks(20240108, BLOCK_VERSION_RAW_V2, ticks);

	WTSTickSlice* slice = _reader.readTickSliceByDate("SSE.STK.600000", 20240108);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->get_block_counts(), (std::size_t)1);
	vector<WTSTickStruct> got = flatten(slice);
	ASSERT_EQ(got.size(), ticks.size());
	for(std::size_t i = 0; i < got.size(); i++)
		expect_tick_eq(got[i], ticks[i]);
	EXPECT_EQ(_sink.count_of(LL_ERROR), 0u);
	slice->release();
}

TEST_F(Q03RdmTickTest, his_tick_compressed_v2_roundtrip)
{
	vector<WTSTickStruct> ticks;
	for(uint32_t i = 0; i < 10; i++)
		ticks.push_back(make_tick(20240108, 20240108, 93000000 + i * 1000, i + 1));
	write_his_ticks(20240108, BLOCK_VERSION_CMP_V2, ticks);

	// 压缩头必须让 proc_block_data 的长度校验通过：size == 20 + _size
	string raw = read_bytes(his_tick_file(20240108));
	BlockHeaderV2 head;
	memcpy(&head, raw.data(), sizeof(head));
	EXPECT_EQ(head._version, BLOCK_VERSION_CMP_V2);
	EXPECT_EQ(raw.size(), (std::size_t)(sizeof(BlockHeaderV2) + head._size));

	WTSTickSlice* slice = _reader.readTickSliceByDate("SSE.STK.600000", 20240108);
	ASSERT_TRUE(slice != NULL);
	vector<WTSTickStruct> got = flatten(slice);
	ASSERT_EQ(got.size(), ticks.size());
	for(std::size_t i = 0; i < got.size(); i++)
		expect_tick_eq(got[i], ticks[i]);
	slice->release();
}

TEST_F(Q03RdmTickTest, his_tick_old_struct_versions_are_upgraded)
{
	// BLOCK_VERSION_RAW：老结构体未压缩 → WTSTickStructOld → WTSTickStruct
	vector<WTSTickStructOld> old1;
	old1.push_back(make_old_tick(20240108, 20240108, 93100000, 1));
	old1.push_back(make_old_tick(20240108, 20240108, 143000000, 2));
	write_his_ticks(20240108, BLOCK_VERSION_RAW, pack_old_ticks(old1));

	// BLOCK_VERSION_CMP：老结构体 + 压缩
	vector<WTSTickStructOld> old2;
	for(uint32_t i = 0; i < 3; i++)
		old2.push_back(make_old_tick(20240109, 20240109, 93200000 + i * 1000, i + 1));
	write_his_ticks(20240109, BLOCK_VERSION_CMP, pack_old_ticks(old2));

	WTSTickSlice* s1 = _reader.readTickSliceByDate("SSE.STK.600000", 20240108);
	ASSERT_TRUE(s1 != NULL);
	vector<WTSTickStruct> g1 = flatten(s1);
	ASSERT_EQ(g1.size(), old1.size());
	for(std::size_t i = 0; i < g1.size(); i++)
		expect_tick_upgraded(g1[i], old1[i]);
	s1->release();

	WTSTickSlice* s2 = _reader.readTickSliceByDate("SSE.STK.600000", 20240109);
	ASSERT_TRUE(s2 != NULL);
	vector<WTSTickStruct> g2 = flatten(s2);
	ASSERT_EQ(g2.size(), old2.size());
	for(std::size_t i = 0; i < g2.size(); i++)
		expect_tick_upgraded(g2[i], old2[i]);
	s2->release();
}

TEST_F(Q03RdmTickTest, his_tick_size_check_rejects_short_file)
{
	// 8 字节 < sizeof(HisTickBlock)(12) → 走 :303 的 Sizechecking 分支，块指针留空 → NULL
	string junk;
	junk.assign(8, 'X');
	ASSERT_TRUE(write_bytes(his_tick_file(20240108), junk));

	WTSTickSlice* slice = _reader.readTickSliceByDate("SSE.STK.600000", 20240108);
	EXPECT_TRUE(slice == NULL);
	EXPECT_GE(_sink.count_of(LL_ERROR), 1u);
}

TEST_F(Q03RdmTickTest, his_tick_tail_fragment_is_dropped)
{
	vector<WTSTickStruct> ticks;
	ticks.push_back(make_tick(20240108, 20240108, 93000000, 1));
	ticks.push_back(make_tick(20240108, 20240108, 145900000, 2));
	string payload = pack_ticks(ticks);
	payload.append(37, 'Z');	// 尾巴不足一条记录，按 :323 的整除规则丢弃
	write_his_ticks(20240108, BLOCK_VERSION_RAW_V2, payload);

	WTSTickSlice* slice = _reader.readTickSliceByDate("SSE.STK.600000", 20240108);
	ASSERT_TRUE(slice != NULL);
	vector<WTSTickStruct> got = flatten(slice);
	ASSERT_EQ(got.size(), (std::size_t)2);
	expect_tick_eq(got[0], ticks[0]);
	expect_tick_eq(got[1], ticks[1]);
	slice->release();
}

TEST_F(Q03RdmTickTest, his_tick_header_only_file_yields_zero_count)
{
	// 正好 12 字节（只有块头）→ 长度校验通过，但 tcnt==0 → :324 break → NULL
	string block_only = make_block(BT_HIS_Ticks, BLOCK_VERSION_RAW_V2, "");
	ASSERT_EQ(block_only.size(), (std::size_t)sizeof(HisTickBlock));
	ASSERT_TRUE(write_bytes(his_tick_file(20240108), block_only));

	EXPECT_TRUE(_reader.readTickSliceByDate("SSE.STK.600000", 20240108) == NULL);
}

// ---------------------------------------------------------------- rt/ticks/*.dmb
TEST_F(Q03RdmTickTest, rt_tick_block_read_back_when_no_his_file)
{
	vector<WTSTickStruct> ticks;
	ticks.push_back(make_tick(20240110, 20240110, 93000000, 1));
	ticks.push_back(make_tick(20240110, 20240110, 103000000, 2));
	ticks.push_back(make_tick(20240110, 20240110, 143000000, 3));
	write_rt_ticks(20240110, ticks);

	// uDate == curTDate → his 未命中后走 :334 的当日实时块分支
	WTSTickSlice* slice = _reader.readTickSliceByDate("SSE.STK.600000", 20240110);
	ASSERT_TRUE(slice != NULL);
	vector<WTSTickStruct> got = flatten(slice);
	ASSERT_EQ(got.size(), ticks.size());
	for(std::size_t i = 0; i < got.size(); i++)
		expect_tick_eq(got[i], ticks[i]);
	slice->release();

	// 同一 (exchg,code) 第二次取数必须命中缓存表，返回同一份数据
	WTSTickSlice* again = _reader.readTickSliceByDate("SSE.STK.600000", 20240110);
	ASSERT_TRUE(again != NULL);
	vector<WTSTickStruct> got2 = flatten(again);
	ASSERT_EQ(got2.size(), ticks.size());
	expect_tick_eq(got2[2], ticks[2]);
	again->release();
}

TEST_F(Q03RdmTickTest, rt_tick_block_missing_and_empty)
{
	// 既没有 his 文件也没有 rt 文件 → getRTTickBlock 返回 NULL → :350 break → NULL
	EXPECT_TRUE(_reader.readTickSliceByDate("SSE.STK.600000", 20240110) == NULL);

	// rt 文件存在但 _size==0 → 同样 break → NULL
	write_rt_ticks(20240110, vector<WTSTickStruct>());
	EXPECT_TRUE(_reader.readTickSliceByDate("SSE.STK.600000", 20240110) == NULL);
}

TEST_F(Q03RdmTickTest, rt_tick_capacity_larger_than_size_is_fully_mapped)
{
	vector<WTSTickStruct> ticks;
	for(uint32_t i = 0; i < 4; i++)
		ticks.push_back(make_tick(20240110, 20240110, 93000000 + i * 100000, i + 1));
	write_rt_ticks(20240110, ticks, 2500);	// writer 侧默认容量

	WTSTickSlice* slice = _reader.readTickSliceByDate("SSE.STK.600000", 20240110);
	ASSERT_TRUE(slice != NULL);
	vector<WTSTickStruct> got = flatten(slice);
	ASSERT_EQ(got.size(), (std::size_t)4);
	for(std::size_t i = 0; i < got.size(); i++)
		expect_tick_eq(got[i], ticks[i]);
	slice->release();
}

// ---------------------------------------------------------------- 跨文件拼接 + 窗口裁剪
TEST_F(Q03RdmTickTest, range_splices_two_his_days_plus_rt_block)
{
	vector<WTSTickStruct> d08;
	d08.push_back(make_tick(20240108, 20240108, 93000000, 1));
	d08.push_back(make_tick(20240108, 20240108, 103000000, 2));
	d08.push_back(make_tick(20240108, 20240108, 145900000, 3));
	write_his_ticks(20240108, BLOCK_VERSION_RAW_V2, d08);

	vector<WTSTickStruct> d09;
	d09.push_back(make_tick(20240109, 20240109, 93100000, 4));
	d09.push_back(make_tick(20240109, 20240109, 140000000, 5));
	write_his_ticks(20240109, BLOCK_VERSION_CMP_V2, d09);

	vector<WTSTickStruct> d10;
	d10.push_back(make_tick(20240110, 20240110, 93200000, 6));
	d10.push_back(make_tick(20240110, 20240110, 143000000, 7));
	write_rt_ticks(20240110, d10);

	WTSTickSlice* slice = _reader.readTickSliceByRange("SSE.STK.600000",
		20240108090000000ull, 20240110150000000ull);
	ASSERT_TRUE(slice != NULL);

	vector<WTSTickStruct> all = d08;
	all.insert(all.end(), d09.begin(), d09.end());
	all.insert(all.end(), d10.begin(), d10.end());

	vector<WTSTickStruct> got = flatten(slice);
	ASSERT_EQ(got.size(), all.size());
	for(std::size_t i = 0; i < got.size(); i++)
		expect_tick_eq(got[i], all[i]);

	// 每个交易日一个块（可能含一个 0 长的占位首块）
	EXPECT_GE(slice->get_block_counts(), (std::size_t)3);
	slice->release();
}

TEST_F(Q03RdmTickTest, range_clips_left_window_on_begin_trading_date)
{
	vector<WTSTickStruct> d08;
	d08.push_back(make_tick(20240108, 20240108, 93000000, 1));	// 早于 stime，应被裁掉
	d08.push_back(make_tick(20240108, 20240108, 103000000, 2));
	d08.push_back(make_tick(20240108, 20240108, 145900000, 3));
	write_his_ticks(20240108, BLOCK_VERSION_RAW_V2, d08);

	vector<WTSTickStruct> d09;
	d09.push_back(make_tick(20240109, 20240109, 93100000, 4));
	d09.push_back(make_tick(20240109, 20240109, 140000000, 5));
	write_his_ticks(20240109, BLOCK_VERSION_RAW_V2, d09);

	vector<WTSTickStruct> d10;
	d10.push_back(make_tick(20240110, 20240110, 93200000, 6));
	d10.push_back(make_tick(20240110, 20240110, 143000000, 7));
	write_rt_ticks(20240110, d10);

	// stime = 20240108 10:30 → 首个交易日从下标 1 开始
	WTSTickSlice* slice = _reader.readTickSliceByRange("SSE.STK.600000",
		20240108103000000ull, 20240110150000000ull);
	ASSERT_TRUE(slice != NULL);
	vector<WTSTickStruct> got = flatten(slice);

	vector<WTSTickStruct> want = expect_of(d08, 1, 3);
	want.insert(want.end(), d09.begin(), d09.end());
	want.insert(want.end(), d10.begin(), d10.end());
	ASSERT_EQ(got.size(), want.size());
	for(std::size_t i = 0; i < got.size(); i++)
		expect_tick_eq(got[i], want[i]);
	slice->release();
}

TEST_F(Q03RdmTickTest, range_clips_right_window_on_end_trading_date)
{
	vector<WTSTickStruct> d10;
	d10.push_back(make_tick(20240110, 20240110, 93000000, 1));
	d10.push_back(make_tick(20240110, 20240110, 103000000, 2));
	d10.push_back(make_tick(20240110, 20240110, 143000000, 3));	// 晚于 etime，应被裁掉
	write_rt_ticks(20240110, d10);

	// 区间只在当日（beginTDate == curTDate），走 :593 的左边界查找 + :553 的右边界 eTick
	WTSTickSlice* slice = _reader.readTickSliceByRange("SSE.STK.600000",
		20240110090000000ull, 20240110110000000ull);
	ASSERT_TRUE(slice != NULL);
	vector<WTSTickStruct> got = flatten(slice);
	ASSERT_EQ(got.size(), (std::size_t)2);
	expect_tick_eq(got[0], d10[0]);
	expect_tick_eq(got[1], d10[1]);
	slice->release();
}

TEST_F(Q03RdmTickTest, range_returns_empty_object_when_no_data_at_all)
{
	// :391 先建空切片，全部分支都不命中时返回的是「空切片」而不是 NULL
	WTSTickSlice* slice = _reader.readTickSliceByRange("SSE.STK.600000",
		20240108090000000ull, 20240108150000000ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_TRUE(flatten(slice).empty());
	slice->release();
}

TEST_F(Q03RdmTickTest, range_skips_missing_middle_day)
{
	vector<WTSTickStruct> d08;
	d08.push_back(make_tick(20240108, 20240108, 93000000, 1));
	write_his_ticks(20240108, BLOCK_VERSION_RAW_V2, d08);

	// 20240109 没有文件 → :443 break，不产生块，也不中断循环
	vector<WTSTickStruct> d10;
	d10.push_back(make_tick(20240110, 20240110, 93000000, 9));
	write_rt_ticks(20240110, d10);

	WTSTickSlice* slice = _reader.readTickSliceByRange("SSE.STK.600000",
		20240108090000000ull, 20240110150000000ull);
	ASSERT_TRUE(slice != NULL);
	vector<WTSTickStruct> got = flatten(slice);
	ASSERT_EQ(got.size(), (std::size_t)2);
	expect_tick_eq(got[0], d08[0]);
	expect_tick_eq(got[1], d10[0]);
	slice->release();
}

TEST_F(Q03RdmTickTest, trading_date_override_routes_to_his_branch)
{
	// 把 20240105 归到交易日 20240108：读文件时用的是交易日而不是自然日
	_bd.set_override(20240105, 20240108);
	vector<WTSTickStruct> d08;
	d08.push_back(make_tick(20240108, 20240105, 93000000, 1));
	write_his_ticks(20240108, BLOCK_VERSION_RAW_V2, d08);

	WTSTickSlice* slice = _reader.readTickSliceByDate("SSE.STK.600000", 20240108);
	ASSERT_TRUE(slice != NULL);
	vector<WTSTickStruct> got = flatten(slice);
	ASSERT_EQ(got.size(), (std::size_t)1);
	expect_tick_eq(got[0], d08[0]);
	slice->release();
}

// ---------------------------------------------------------------- 缺陷复现（不进启用集）
// BUG-Q03-001：cacheHisBarsFromFile 主力连续分支 :1194/:1196 直接 hotAy->at(barcnt - 1)，
// barcnt 是 uint32_t，12~99 字节的合法长度文件（擦掉头之后不足一条 WTSBarStruct）会让
// barcnt == 0 → at(0xFFFFFFFF) 抛 std::out_of_range 逃出库函数；同时 hotAy(:1189 new) 泄漏。
// 崩溃/异常类输入统一收进本 DISABLED_ 用例，启用用例零残留。
TEST_F(Q03RdmTickTest, DISABLED_his_kline_hot_fragment_throws_out_of_range)
{
	RuleTagHotMgr hot;
	RdmSink sink2{ &_bd, &hot };
	WtRdmDtReader reader;
	reader.init(NULL, &sink2);

	// SHFE.rb.HOT → _ruletag="HOT"，文件名 <exchg>.<product>_<ruletag>.dsb（:1170）
	string frag = make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, string(50, 'A'));
	ASSERT_TRUE(write_bytes("his/day/SHFE/SHFE.rb_HOT.dsb", frag));

	EXPECT_THROW(reader.readKlineSliceByRange("SHFE.rb.HOT", KP_DAY, 20240101000000000ull,
		20240110150000000ull), std::out_of_range);
	reader.clearCache();
}

} // namespace q03
