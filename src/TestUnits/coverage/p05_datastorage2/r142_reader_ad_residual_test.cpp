// r142 / WtDataStorageAD 读取器：init(NULL) 早退、LMDB 增量补线并回调、
//       RT 缓存索引未命中、按条数扩容重载
//
// 覆盖（src/WtDataStorageAD/WtDataReaderAD.cpp）：
//  - init NULL 配置早退 :23
//  - update_cache_from_lmdb 新 bar 追加并 on_bar :292-297（用例因 BUG-R155-001 禁用）
//  - get_rt_cache_bar 索引未命中返回 NULL :358
//  - readKlineSlice 容量不足回源 :407-413
//  - readTickSlice 扩容后重载 :117-180
//
// RT 缓存只写 disk 文件缺失场景，不触发二次回源。
#include "r100_ad_fixture.h"

#include "WtDataStorageAD/WtDataReaderAD.h"

namespace r142ad {

using std::string;
using std::vector;

class R142ReaderADTest : public r100fx::R100ADReaderTest
{
protected:
	uint64_t mt(uint32_t date, uint32_t hhmm) const
	{
		return TimeUtils::timeToMinBar(date, hhmm);
	}
};

// WtDataReaderAD.cpp: L23（cfg==NULL 时只完成基类与 sink 注入）
TEST_F(R142ReaderADTest, R142ReaderADInitNullReturnsEarly)
{
	WtDataReaderAD reader;
	reader.init(NULL, _sink, NULL);

	// cfg 为空时不应写任何日志，也不应设置 cache 文件名
	EXPECT_EQ(_sink->total_logs(), 0u);
	EXPECT_EQ(reader.getAdjustingFlag(), 0u);
}

// WtDataReaderAD.cpp: L292-297 / L358 / L407-413
//
// 2026-10-28 R155 夹具修正：两段式代码 EXR.rb2301 经 CodeHelper 解析出的品种
// 就是 rb2301，品种必须按同名注册，否则 readKlineSlice 里 commInfo 为空，
// `commInfo->isFuture()` 直接 SIGSEGV（原崩溃根因）。
//
// 修正夹具后确认产品缺陷 BUG-R155-001：容量已满时追加 LMDB bar 会使环形缓冲回绕；
// 继续给旧 back() 引用赋值会覆写逻辑首条。固定 sink 时钟并传 etime=0，确认最近三条
// bar 的数量与全部字段仍对应 09:33、09:34、09:35。
TEST_F(R142ReaderADTest, R142ReaderADKlineIncrementalAppendAndRtMiss)
{
	WTSContractInfo* ct = add_future("EXR", "rb2301", "rb2301");
	ASSERT_TRUE(ct != NULL);

	vector<WTSBarStruct> bars;
	bars.push_back(r4fx::make_bar_item("EXR", "rb2301", 20230103, mt(20230103, 931), 10.0)._bar);
	bars.push_back(r4fx::make_bar_item("EXR", "rb2301", 20230103, mt(20230103, 932), 11.0)._bar);
	bars.push_back(r4fx::make_bar_item("EXR", "rb2301", 20230103, mt(20230103, 933), 12.0)._bar);
	seed_bars("min1", "EXR", "rb2301", bars, false);

	init_reader();

	// 第一次：结束时间包含 09:33，容量 0<3 -> get_lowers 取 09:31/09:32/09:33
	_sink->set_clock(20230103, 933, 0);
	WTSKlineSlice* s1 = _reader->readKlineSlice("EXR.rb2301", KP_Minute1, 3, 0);
	ASSERT_TRUE(s1 != NULL);
	ASSERT_EQ(s1->size(), 3);
	EXPECT_EQ(s1->at(0)->time, mt(20230103, 931));
	EXPECT_EQ(s1->at(2)->time, mt(20230103, 933));
	s1->release();

	// 缓存已满且容量足够，这里往 LMDB 追加两条新 bar
	vector<WTSBarStruct> more;
	more.push_back(r4fx::make_bar_item("EXR", "rb2301", 20230103, mt(20230103, 934), 13.0)._bar);
	more.push_back(r4fx::make_bar_item("EXR", "rb2301", 20230103, mt(20230103, 935), 14.0)._bar);
	seed_bars("min1", "EXR", "rb2301", more, false);

	// 第二次：不再回源取全量，update_cache_from_lmdb 逐条追加并回调 on_bar；
	// 09:35 仍早于 09:36，随后 get_rt_cache_bar 因 cache_m1.dmb 不存在而返回 NULL。
	_sink->set_clock(20230103, 936, 0);
	WTSKlineSlice* s2 = _reader->readKlineSlice("EXR.rb2301", KP_Minute1, 3, 0);
	ASSERT_TRUE(s2 != NULL);
	ASSERT_EQ(s2->size(), 3);
	auto expect_bar = [](const WTSBarStruct& actual, uint64_t time, double price) {
		EXPECT_EQ(actual.date, 20230103u);
		EXPECT_EQ(actual.reserve_, 0u);
		EXPECT_EQ(actual.time, time);
		EXPECT_DOUBLE_EQ(actual.open, price);
		EXPECT_DOUBLE_EQ(actual.high, price);
		EXPECT_DOUBLE_EQ(actual.low, price);
		EXPECT_DOUBLE_EQ(actual.close, price);
		EXPECT_DOUBLE_EQ(actual.settle, 0.0);
		EXPECT_DOUBLE_EQ(actual.money, 0.0);
		EXPECT_DOUBLE_EQ(actual.vol, 0.0);
		EXPECT_DOUBLE_EQ(actual.hold, 0.0);
		EXPECT_DOUBLE_EQ(actual.add, 0.0);
	};
	expect_bar(*s2->at(0), mt(20230103, 933), 12.0);
	expect_bar(*s2->at(1), mt(20230103, 934), 13.0);
	expect_bar(*s2->at(2), mt(20230103, 935), 14.0);
	s2->release();

	EXPECT_EQ(_sink->bar_calls(), 2u);
	expect_bar(_sink->last_bar(), mt(20230103, 935), 14.0);
	EXPECT_EQ(_sink->last_period(), KP_Minute1);
}

// WtDataReaderAD.cpp: L407-413（count 增长触发 rset_capacity + 全量回源）
TEST_F(R142ReaderADTest, R142ReaderADDayKlineCapacityGrowthReloads)
{
	// 同上：两段式 EXS.rb2301 的品种为 rb2301
	WTSContractInfo* ct = add_future("EXS", "rb2301", "rb2301");
	ASSERT_TRUE(ct != NULL);

	vector<WTSBarStruct> bars;
	bars.push_back(r4fx::make_bar_item("EXS", "rb2301", 20230101, 0, 10.0)._bar);
	bars.push_back(r4fx::make_bar_item("EXS", "rb2301", 20230102, 0, 11.0)._bar);
	bars.push_back(r4fx::make_bar_item("EXS", "rb2301", 20230103, 0, 12.0)._bar);
	seed_bars("day", "EXS", "rb2301", bars, true);

	init_reader();

	WTSKlineSlice* s1 = _reader->readKlineSlice("EXS.rb2301", KP_DAY, 1,
		20230103090000000ull);
	ASSERT_TRUE(s1 != NULL);
	ASSERT_EQ(s1->size(), 1);
	EXPECT_EQ(s1->at(0)->date, 20230103u);
	s1->release();

	// count 由 1 增到 3：capacity 不足 -> 清空后第二次 get_k_db 全量重载
	WTSKlineSlice* s2 = _reader->readKlineSlice("EXS.rb2301", KP_DAY, 3,
		20230103090000000ull);
	ASSERT_TRUE(s2 != NULL);
	ASSERT_EQ(s2->size(), 3);
	EXPECT_EQ(s2->at(0)->date, 20230101u);
	EXPECT_EQ(s2->at(2)->date, 20230103u);
	s2->release();
}

// WtDataReaderAD.cpp: readTickSlice L117-180（容量扩容后按条数重载）
TEST_F(R142ReaderADTest, R142ReaderADTickCapacityGrowthReloads)
{
	// 同上：两段式 EXT.rb2301 的品种为 rb2301
	WTSContractInfo* ct = add_future("EXT", "rb2301", "rb2301");
	ASSERT_TRUE(ct != NULL);

	vector<WTSTickStruct> ticks;
	ticks.push_back(r4fx::make_tick("EXT", "rb2301", 20230103, 20230103, 93100000,
		10.0, 1.0, 1000.0, 100.0, 0.0, 10.0, 10000.0));
	ticks.push_back(r4fx::make_tick("EXT", "rb2301", 20230103, 20230103, 93200000,
		11.0, 1.0, 1100.0, 100.0, 0.0, 11.0, 11000.0));
	ticks.push_back(r4fx::make_tick("EXT", "rb2301", 20230103, 20230103, 93300000,
		12.0, 1.0, 1200.0, 100.0, 0.0, 12.0, 12000.0));
	seed_ticks("EXT", "rb2301", ticks);

	init_reader();

	WTSTickSlice* t1 = _reader->readTickSlice("EXT.rb2301", 1, 20230103150000000ull);
	ASSERT_TRUE(t1 != NULL);
	ASSERT_EQ(t1->size(), 1u);
	EXPECT_EQ(t1->at(0)->action_time, 93300000u);
	t1->release();

	// capacity 1 < 3 -> clear + 重新 get_lowers 取最新 3 条
	WTSTickSlice* t2 = _reader->readTickSlice("EXT.rb2301", 3, 20230103150000000ull);
	ASSERT_TRUE(t2 != NULL);
	ASSERT_EQ(t2->size(), 3u);
	EXPECT_EQ(t2->at(0)->action_time, 93100000u);
	EXPECT_EQ(t2->at(2)->action_time, 93300000u);
	t2->release();
}

} // namespace r142ad
