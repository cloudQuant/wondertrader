// R122 / WtDataReaderAD：array_two 直取分支（count==0 边界，不触碰疑似错取数据）、
// LMDB 打开失败回退（K 线库与 tick 库分开验证）。
//
// 覆盖（src/WtDataStorageAD/WtDataReaderAD.cpp）：
//  - readKlineSlice 的 array_two 足够分支 :477-480（count=0，切片为空）
//  - get_k_db 打开失败 :584-587；readKlineSlice 空结果 :419-420
//  - get_t_db 打开失败 :608-611；readTickSlice 空句柄返回 :144-145
//
// 失败注入：用同名目录顶掉 data.mdb，mdb_env_open 稳定返回失败，与文件权限无关。
// 说明：count>0 的 array_two 直取分支疑有偏移问题（见 bugs/R122.md）。本文件末尾的
// DISABLED 历史 oracle 没有建立增量回绕，且当前直接视图存在越界风险，不能作为复现。
// 在安全写入/切片接缝与 R155 引用别名问题隔离前，不启用该路径断言。
#include "r100_ad_fixture.h"

namespace r122adr {

using namespace r100fx;

namespace {

WTSBarStruct bar_at(uint32_t date, uint32_t hhmm, double px)
{
	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	bar.date = date;
	bar.time = (uint64_t)(date - 19900000) * 10000 + hhmm;
	bar.open = px;
	bar.high = px + 1;
	bar.low = px - 1;
	bar.close = px + 0.5;
	bar.vol = 10;
	bar.money = px * 10;
	return bar;
}

} // namespace

class R122ReaderADTest : public R100ADReaderTest
{
};

// WtDataReaderAD.cpp: L419-420 / L477-480
TEST_F(R122ReaderADTest, R122ReaderADKlineZeroCountHitsDirectArrayTwo)
{
	add_stock("EXA", "rb2301");

	vector<WTSBarStruct> bars;
	for(uint32_t i = 0; i < 3; i++)
		bars.push_back(bar_at(20230103, 931 + i, 3900.0 + i * 10));
	seed_bars("min1", "EXA", "rb2301", bars, false);
	init_reader();

	// 第一次填满 capacity=3 的环形缓冲（线性布局，array_two 为空）
	WTSKlineSlice* s1 = _reader->readKlineSlice("EXA.rb2301", KP_Minute1, 3, 20230103093300000ull);
	ASSERT_TRUE(s1 != NULL);
	ASSERT_EQ(s1->size(), 3u);
	s1->release();

	// 第二次 count=0、etime 不前进：不走增量回源，直接命中 cnt_2 >= count 分支
	WTSKlineSlice* s2 = _reader->readKlineSlice("EXA.rb2301", KP_Minute1, 0, 20230103093300000ull);
	ASSERT_TRUE(s2 != NULL);
	EXPECT_EQ(s2->size(), 0u);
	s2->release();
	EXPECT_EQ(_sink->bar_calls(), 0u);
}

/*
 * 禁用的历史 oracle，不是 BUG-R122-001 的有效复现：这里在首次读取前已写入
 * 09:31–09:35 全部五根。cacheBarsFromStorage 按 count 取末尾数据且不按 etime
 * 截断，所以首读 count=3 实际缓存 09:33–09:35；第二次请求时缓存尾时间已等于
 * 09:35，增量更新和环形缓冲回绕都不会发生。下面保留的 09:34/09:35 期望只表示
 * 目标契约，不能作为该缺陷已被触发的证据。
 *
 * 正确增量前提应先写入 09:31–09:33、首读后再追加 09:34–09:35。现有 fixture
 * 没有可安全复用 reader 已打开 LMDB 环境的写入接缝；同时 cap=3 的直接
 * array_two 路径会把从逻辑 _bars[0] 开始的 count=2 连续视图跨过物理缓冲区末尾，
 * 且 update_cache_from_lmdb 持有的 lastBar 引用会跨越 push_back（BUG-R155）。
 * 原字段断言仅作为目标 oracle 保留；在隔离这两个问题并能安全读取切片前，
 * 不得取消 DISABLED 或运行本用例。
 */
TEST_F(R122ReaderADTest, DISABLED_R122ReaderADKlineArrayTwoReturnsWrongTail)
{
	add_stock("EXQ", "rb2301");

	vector<WTSBarStruct> bars;
	for(uint32_t i = 0; i < 5; i++)
		bars.push_back(bar_at(20230103, 931 + i, 4000.0 + i * 10));
	seed_bars("min1", "EXQ", "rb2301", bars, false);
	init_reader();

	WTSKlineSlice* s1 = _reader->readKlineSlice("EXQ.rb2301", KP_Minute1, 3, 20230103093300000ull);
	ASSERT_TRUE(s1 != NULL);
	ASSERT_EQ(s1->size(), 3u);
	s1->release();

	WTSKlineSlice* s2 = _reader->readKlineSlice("EXQ.rb2301", KP_Minute1, 2, 20230103093500000ull);
	ASSERT_TRUE(s2 != NULL);
	ASSERT_EQ(s2->size(), 2u);
	EXPECT_EQ(s2->get_block_addr(0)[0].time, (uint64_t)3301030934ull);
	EXPECT_EQ(s2->get_block_addr(0)[1].time, (uint64_t)3301030935ull);
	s2->release();
}

// 首次读 LMDB 的 cutoff 回归：只检验实现当前解析的 17 位 YYYYMMDDHHMMSSmmm 输入。
// IDataReader.h 对 K 线 etime 的公开说明是 12 位 YYYYMMDDHHMM；本用例不代表 12 位调用契约。
TEST_F(R122ReaderADTest, R122ReaderADFirstKlineReadHonorsEtime)
{
	add_stock("EXD", "rb2301");

	vector<WTSBarStruct> bars;
	for (uint32_t i = 0; i < 5; i++)
		bars.push_back(bar_at(20230103, 931 + i, 3900.0 + i * 10));
	seed_bars("min1", "EXD", "rb2301", bars, false);
	init_reader();

	const uint64_t cutoff = 20230103093300000ull;
	const uint64_t cutoff_bar_time = (uint64_t)(20230103 - 19900000) * 10000 + 933;
	WTSKlineSlice* slice = _reader->readKlineSlice("EXD.rb2301", KP_Minute1, 3, cutoff);
	ASSERT_NE(nullptr, slice);
	EXPECT_EQ(3u, slice->size());
	const uint32_t inspect_count = std::min(3u, (uint32_t)slice->size());
	const uint32_t expected_hhmm[] = { 931, 932, 933 };
	const double expected_close[] = { 3900.5, 3910.5, 3920.5 };
	for (uint32_t i = 0; i < inspect_count; i++)
	{
		const WTSBarStruct* bar = slice->at((int32_t)i);
		EXPECT_NE(nullptr, bar) << "index=" << i;
		if (bar != nullptr)
		{
			EXPECT_LE(bar->time, cutoff_bar_time) << "index=" << i;
			EXPECT_EQ((uint64_t)(20230103 - 19900000) * 10000 + expected_hhmm[i], bar->time)
				<< "index=" << i;
			EXPECT_DOUBLE_EQ(expected_close[i], bar->close) << "index=" << i;
		}
	}
	slice->release();
}

TEST_F(R122ReaderADTest, R122ReaderADFirstKlineReadAtLastBarIsPositiveControl)
{
	add_stock("EXE", "rb2301");

	vector<WTSBarStruct> bars;
	for (uint32_t i = 0; i < 5; i++)
		bars.push_back(bar_at(20230103, 931 + i, 3900.0 + i * 10));
	seed_bars("min1", "EXE", "rb2301", bars, false);
	init_reader();

	WTSKlineSlice* slice = _reader->readKlineSlice("EXE.rb2301", KP_Minute1, 3, 20230103093500000ull);
	ASSERT_NE(nullptr, slice);
	EXPECT_EQ(3u, slice->size());
	const uint32_t expected_hhmm[] = { 933, 934, 935 };
	const double expected_close[] = { 3920.5, 3930.5, 3940.5 };
	const uint32_t inspect_count = std::min(3u, (uint32_t)slice->size());
	for (uint32_t i = 0; i < inspect_count; i++)
	{
		const WTSBarStruct* bar = slice->at((int32_t)i);
		EXPECT_NE(nullptr, bar) << "index=" << i;
		if (bar != nullptr)
		{
			EXPECT_EQ((uint64_t)(20230103 - 19900000) * 10000 + expected_hhmm[i], bar->time)
				<< "index=" << i;
			EXPECT_DOUBLE_EQ(expected_close[i], bar->close) << "index=" << i;
		}
	}
	slice->release();
}

// WtDataReaderAD.cpp: L419-420 / L584-587
TEST_F(R122ReaderADTest, R122ReaderADKlineDbOpenFailure)
{
	add_stock("EXB", "rb2301");

	std::error_code ec;
	std::filesystem::create_directories(_root / "min1" / "EXB" / "data.mdb", ec);
	ASSERT_FALSE(ec) << ec.message();

	init_reader();

	EXPECT_TRUE(_reader->readKlineSlice("EXB.rb2301", KP_Minute1, 5,
		20230103100000000ull) == NULL);
	const string error_log = _sink->last_log(LL_ERROR);
	EXPECT_NE(error_log.find("Opening min1 db "), string::npos);
	EXPECT_NE(error_log.find("EXB failed"), string::npos);
}

// WtDataReaderAD.cpp: L143-145 / L608-611
TEST_F(R122ReaderADTest, R122ReaderADTickDbOpenFailure)
{
	add_stock("EXC", "rb2301");

	std::error_code ec;
	std::filesystem::create_directories(_root / "ticks" / "EXC" / "rb2301" / "data.mdb", ec);
	ASSERT_FALSE(ec) << ec.message();

	init_reader();

	EXPECT_TRUE(_reader->readTickSlice("EXC.rb2301", 5,
		20230103100000000ull) == NULL);
	EXPECT_GE(_sink->count_containing("Opening tick db of EXC.rb2301 failed"), 1u);
}

} // namespace r122adr
