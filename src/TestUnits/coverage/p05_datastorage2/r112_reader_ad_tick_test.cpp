// r112 / WtDataReaderAD：tick 增量重载、库句柄缓存命中与第三次取库失败
//
// 覆盖（src/WtDataStorageAD/WtDataReaderAD.cpp）：
//  - readTickSlice 的 reload_flag==2 全量 get_lowers :165-182
//  - reload_flag==1 增量 get_range :147-163
//  - get_t_db 的命中缓存 :601-602 与第三次被 std::move 掏空后的返回 :143-145
//  - array_two 足够/不足两条切片路径 :189-205
//
// WtLMDB 同一 (exchg,code) 的第 3 次 get_t_db 会拿到被移走的空句柄（R4-BUG-02），
// 本用例把它作为确定性行为固定下来，不触碰任何写库或线程。
#include "r100_ad_fixture.h"

namespace r112adr {

using namespace r100fx;

namespace {
const uint32_t kDate = 20230103;
}

class R112ReaderADTickTest : public R100ADReaderTest
{
};

// WtDataReaderAD.cpp: L143-145 / L147-182 / L189-205 / L601-602
TEST_F(R112ReaderADTickTest, R112ReaderADTickIncrementalAndHandleMove)
{
	add_stock("EXTK", "rb2301");

	vector<WTSTickStruct> ticks;
	ticks.push_back(r4fx::make_tick("EXTK", "rb2301", kDate, kDate, 93000000, 3900.0, 1, 10, 5, 0, 100, 1000));
	ticks.push_back(r4fx::make_tick("EXTK", "rb2301", kDate, kDate, 93100000, 3910.0, 1, 10, 5, 0, 110, 1100));
	ticks.push_back(r4fx::make_tick("EXTK", "rb2301", kDate, kDate, 93200000, 3920.0, 1, 10, 5, 0, 120, 1200));
	seed_ticks("EXTK", "rb2301", ticks);

	init_reader();

	// 第一次：容量 0<1 -> reload_flag=2 被随后的 _last_req_time==0 覆盖为增量 reload=1，
	// 从 0 读到 21:00，容量 1 只保留最后一笔（array_two 直接切片）
	WTSTickSlice* s1 = _reader->readTickSlice("EXTK.rb2301", 1, 20230103210000000ull);
	ASSERT_TRUE(s1 != NULL);
	ASSERT_EQ(s1->size(), 1u);
	EXPECT_EQ(s1->get_block_addr(0)[0].action_time, 93200000u);
	s1->release();

	// 第二次：容量 1<3 -> reload_flag=2；etime 未前进，不再被增量分支覆盖，
	// 走全量 get_lowers（get_t_db 命中缓存，move 出有效句柄）
	WTSTickSlice* s2 = _reader->readTickSlice("EXTK.rb2301", 3, 20230103210000000ull);
	ASSERT_TRUE(s2 != NULL);
	ASSERT_EQ(s2->size(), 3u);
	EXPECT_EQ(s2->get_block_addr(0)[0].action_time, 93000000u);
	EXPECT_EQ(s2->get_block_addr(0)[2].action_time, 93200000u);
	s2->release();

	// 第三次：get_t_db 的缓存槽已被 move 掏空，返回 NULL
	WTSTickSlice* s3 = _reader->readTickSlice("EXTK.rb2301", 1, 20230103220000000ull);
	EXPECT_TRUE(s3 == NULL);

}

} // namespace r112adr
