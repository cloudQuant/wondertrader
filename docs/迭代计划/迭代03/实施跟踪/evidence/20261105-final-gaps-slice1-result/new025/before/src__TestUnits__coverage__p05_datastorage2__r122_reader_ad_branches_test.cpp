// R122 / WtDataReaderAD：循环缓冲区回绕后的 K 线/Tick 切片，以及 LMDB 打开失败回退。
//
// 覆盖（src/WtDataStorageAD/WtDataReaderAD.cpp）：
//  - readKlineSlice/readTickSlice 的 array_one/array_two 单段及跨段切片
//  - get_k_db 打开失败 :584-587；readKlineSlice 空结果 :419-420
//  - get_t_db 打开失败 :608-611；readTickSlice 空句柄返回 :144-145
//
// 失败注入：用同名目录顶掉 data.mdb，mdb_env_open 稳定返回失败，与文件权限无关。
// 说明：本文件的新回归用例通过 tick 增量读取和 RT K 线逐根推进构造实际回绕；
// 文件末尾保留的 DISABLED 历史 oracle 在首次读取前就写满五根，仍不代表回绕复现。
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

WTSTickStruct tick_at(uint32_t date, uint32_t atime, double px)
{
	return r4fx::make_tick("EXT", "rb2301", date, date, atime, px, 1, 10, 5, 0, 100, 1000);
}

uint64_t tick_etime(uint32_t date, uint32_t atime)
{
	return (uint64_t)date * 1000000000ull + atime;
}

} // namespace

class R122ReaderADTest : public R100ADReaderTest
{
};

// WtDataReaderAD.cpp: L419-420 / L477-480
TEST_F(R122ReaderADTest, R122ReaderADKlineZeroCountReturnsEmptySlice)
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

	// 第二次 count=0、etime 不前进：不走增量回源，也不对空段做指针运算。
	WTSKlineSlice* s2 = _reader->readKlineSlice("EXA.rb2301", KP_Minute1, 0, 20230103093300000ull);
	ASSERT_TRUE(s2 != NULL);
	EXPECT_EQ(s2->size(), 0u);
	EXPECT_EQ(s2->get_block_counts(), 1u);
	EXPECT_EQ(s2->get_block_size(0), 0u);
	s2->release();
	EXPECT_EQ(_sink->bar_calls(), 0u);
}

TEST_F(R122ReaderADTest, R122ReaderADTickArrayTwoUsesPhysicalOffsetAfterWrap)
{
	add_stock("EXT", "rb2301");

	vector<WTSTickStruct> ticks;
	for (uint32_t i = 0; i < 5; i++)
		ticks.push_back(tick_at(20230103, 93100000 + i * 100000, 3900.0 + i * 10));
	seed_ticks("EXT", "rb2301", ticks);
	init_reader();

	// 首次请求容量 3、截止 09:33，填满线性环形缓冲。
	WTSTickSlice* first = _reader->readTickSlice("EXT.rb2301", 3, tick_etime(20230103, 93300000));
	ASSERT_NE(nullptr, first);
	ASSERT_EQ(3u, first->size());
	EXPECT_EQ(93100000u, first->at(0)->action_time);
	EXPECT_EQ(93300000u, first->at(2)->action_time);
	first->release();

	WTSTickSlice* zero = _reader->readTickSlice("EXT.rb2301", 0, tick_etime(20230103, 93300000));
	ASSERT_NE(nullptr, zero);
	EXPECT_TRUE(zero->empty());
	EXPECT_EQ(1u, zero->get_block_counts());
	EXPECT_EQ(0u, zero->get_block_size(0));
	zero->release();

	// 推进到 09:35 后，增量追加 09:34/09:35 覆写旧槽位并产生 array_two。
	WTSTickSlice* wrapped_tail = _reader->readTickSlice("EXT.rb2301", 2, tick_etime(20230103, 93500000));
	ASSERT_NE(nullptr, wrapped_tail);
	ASSERT_EQ(2u, wrapped_tail->size());
	ASSERT_EQ(1u, wrapped_tail->get_block_counts());
	EXPECT_EQ(2u, wrapped_tail->get_block_size(0));
	ASSERT_NE(nullptr, wrapped_tail->at(0));
	ASSERT_NE(nullptr, wrapped_tail->at(1));
	EXPECT_EQ(93400000u, wrapped_tail->at(0)->action_time);
	EXPECT_EQ(93500000u, wrapped_tail->at(1)->action_time);
	EXPECT_DOUBLE_EQ(3930.0, wrapped_tail->at(0)->price);
	EXPECT_DOUBLE_EQ(3940.0, wrapped_tail->at(1)->price);
	wrapped_tail->release();

	// count 跨越 array_one/array_two 时，两段都必须按物理连续区间返回。
	WTSTickSlice* wrapped_all = _reader->readTickSlice("EXT.rb2301", 3, tick_etime(20230103, 93500000));
	ASSERT_NE(nullptr, wrapped_all);
	ASSERT_EQ(3u, wrapped_all->size());
	ASSERT_EQ(2u, wrapped_all->get_block_counts());
	EXPECT_EQ(1u, wrapped_all->get_block_size(0));
	EXPECT_EQ(2u, wrapped_all->get_block_size(1));
	EXPECT_EQ(93300000u, wrapped_all->at(0)->action_time);
	EXPECT_EQ(93400000u, wrapped_all->at(1)->action_time);
	EXPECT_EQ(93500000u, wrapped_all->at(2)->action_time);
	wrapped_all->release();

	// 请求超过现有五条数据，扩容重载后应返回全部可用记录。
	WTSTickSlice* oversized = _reader->readTickSlice("EXT.rb2301", 6, tick_etime(20230103, 93500000));
	ASSERT_NE(nullptr, oversized);
	ASSERT_EQ(5u, oversized->size());
	for (uint32_t i = 0; i < 5; i++)
		EXPECT_EQ(93100000u + i * 100000, oversized->at((int32_t)i)->action_time);
	oversized->release();
}

TEST_F(R122ReaderADTest, R122ReaderADKlineWrappedSlicesUsePhysicalOffsets)
{
	add_stock("EXK", "rb2301");

	vector<WTSBarStruct> bars;
	for (uint32_t i = 0; i < 3; i++)
		bars.push_back(bar_at(20230103, 931 + i, 3900.0 + i * 10));
	seed_bars("min1", "EXK", "rb2301", bars, false);
	write_cache("cache_m1.dmb", { r4fx::make_bar_item("EXK", "rb2301", 20230103,
		(uint64_t)(20230103 - 19900000) * 10000 + 934, 3940.0) }, 1, 4);
	init_reader();

	WTSKlineSlice* first = _reader->readKlineSlice("EXK.rb2301", KP_Minute1, 3, 20230103093300000ull);
	ASSERT_NE(nullptr, first);
	ASSERT_EQ(3u, first->size());
	EXPECT_EQ((uint64_t)(20230103 - 19900000) * 10000 + 931, first->at(0)->time);
	EXPECT_EQ((uint64_t)(20230103 - 19900000) * 10000 + 933, first->at(2)->time);
	first->release();

	// onMinuteEnd 从 RT cache 连续追加 09:34、09:35，使容量 3 的环形缓存回绕。
	_reader->onMinuteEnd(20230103, 934);
	ASSERT_TRUE(patch_cache_bar("cache_m1.dmb", 0, 20230103,
		(uint64_t)(20230103 - 19900000) * 10000 + 935));
	_reader->onMinuteEnd(20230103, 935);

	WTSKlineSlice* wrapped_tail = _reader->readKlineSlice("EXK.rb2301", KP_Minute1, 2, 20230103093500000ull);
	ASSERT_NE(nullptr, wrapped_tail);
	ASSERT_EQ(2u, wrapped_tail->size());
	ASSERT_EQ(1u, wrapped_tail->get_block_counts());
	EXPECT_EQ(2u, wrapped_tail->get_block_size(0));
	EXPECT_EQ((uint64_t)(20230103 - 19900000) * 10000 + 934, wrapped_tail->at(0)->time);
	EXPECT_EQ((uint64_t)(20230103 - 19900000) * 10000 + 935, wrapped_tail->at(1)->time);
	EXPECT_DOUBLE_EQ(3940.0, wrapped_tail->at(0)->close);
	EXPECT_DOUBLE_EQ(3940.0, wrapped_tail->at(1)->close);
	wrapped_tail->release();

	WTSKlineSlice* wrapped_all = _reader->readKlineSlice("EXK.rb2301", KP_Minute1, 3, 20230103093500000ull);
	ASSERT_NE(nullptr, wrapped_all);
	ASSERT_EQ(3u, wrapped_all->size());
	ASSERT_EQ(2u, wrapped_all->get_block_counts());
	EXPECT_EQ(1u, wrapped_all->get_block_size(0));
	EXPECT_EQ(2u, wrapped_all->get_block_size(1));
	EXPECT_EQ((uint64_t)(20230103 - 19900000) * 10000 + 933, wrapped_all->at(0)->time);
	EXPECT_EQ((uint64_t)(20230103 - 19900000) * 10000 + 934, wrapped_all->at(1)->time);
	EXPECT_EQ((uint64_t)(20230103 - 19900000) * 10000 + 935, wrapped_all->at(2)->time);
	wrapped_all->release();
}

TEST_F(R122ReaderADTest, R122ReaderADKlineRequestLargerThanStoredCountReturnsAvailableBars)
{
	add_stock("EXL", "rb2301");

	vector<WTSBarStruct> bars;
	for (uint32_t i = 0; i < 2; i++)
		bars.push_back(bar_at(20230103, 931 + i, 3900.0 + i * 10));
	seed_bars("min1", "EXL", "rb2301", bars, false);
	init_reader();

	WTSKlineSlice* slice = _reader->readKlineSlice("EXL.rb2301", KP_Minute1, 3, 20230103093200000ull);
	ASSERT_NE(nullptr, slice);
	ASSERT_EQ(2u, slice->size());
	EXPECT_EQ((uint64_t)(20230103 - 19900000) * 10000 + 931, slice->at(0)->time);
	EXPECT_EQ((uint64_t)(20230103 - 19900000) * 10000 + 932, slice->at(1)->time);
	slice->release();
}

/*
 * 禁用的历史 oracle，不是 BUG-R122-001 的有效复现：这里在首次读取前已写入
 * 09:31–09:35 全部五根。cacheBarsFromStorage 按 count 取末尾数据且不按 etime
 * 截断，所以首读 count=3 实际缓存 09:33–09:35；第二次请求时缓存尾时间已等于
 * 09:35，增量更新和环形缓冲回绕都不会发生。下面保留的 09:34/09:35 期望只表示
 * 目标契约，不能作为该缺陷已被触发的证据。
 *
 * 正确增量前提应先写入 09:31–09:33、首读后再追加 09:34–09:35。实际回绕
 * 已由本文件上方的启用测试通过 RT 缓存逐条追加覆盖；该历史 oracle 保持禁用，
 * 因为它既没有触发增量回源，也没有验证 array_two 物理段的切片起点。
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
	EXPECT_NE(error_log.find("Opening min1 db of EXB failed: "), string::npos);
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
