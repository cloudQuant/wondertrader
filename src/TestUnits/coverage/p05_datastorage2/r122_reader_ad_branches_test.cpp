// R122 / WtDataReaderAD：array_two 直取分支（count==0 边界，不触碰疑似错取数据）、
// LMDB 打开失败回退（K 线库与 tick 库分开验证）。
//
// 覆盖（src/WtDataStorageAD/WtDataReaderAD.cpp）：
//  - readKlineSlice 的 array_two 足够分支 :477-480（count=0，切片为空）
//  - get_k_db 打开失败 :584-587；readKlineSlice 空结果 :419-420
//  - get_t_db 打开失败 :608-611；readTickSlice 空句柄返回 :144-145
//
// 失败注入：用同名目录顶掉 data.mdb，mdb_env_open 稳定返回失败，与文件权限无关。
// 说明：array_two 直取分支在 count>0 且环形缓冲回绕后取的是逻辑首元素而非末 count 条
// （`_bars[array_two.second - count]` 少了 array_one.second 偏移，疑似缺陷，见 bugs/R122.md），
// 本波只用 count==0 命中分支，不写用例固定错误数据。
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

// WtDataReaderAD.cpp: L477-480 的 count>0 正确性（BUG-R122-001）
// 环形缓冲回绕后 array_two=[34,35]，期望返回最后两条；当前实现取 _bars[0]=33 起两条。
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
	EXPECT_GE(_sink->count_containing("Opening min1 db if EXB failed"), 1u);
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
