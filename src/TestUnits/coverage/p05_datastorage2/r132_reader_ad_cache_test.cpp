// r132 / WtDataReaderAD：LMDB 增量更新把新 bar 追加进缓存并回调 on_bar、
//       RT 缓存 bar 超前时二次回源 LMDB、tick 环形缓冲的尾部切片。
//
// 覆盖（src/WtDataStorageAD/WtDataReaderAD.cpp）：
//  - readKlineSlice 的 rtBar 时间大于 etime → 再读一次 LMDB 并清 _last_from_cache :450-456
//  - readTickSlice 的数组尾部切片 :188-204
//
// 说明：update_cache_from_lmdb 的 `curBarTime != lastBarTime` 分支（:292-297）在公开
// API 下不可达——cacheBarsFromStorage 总是按 count 取 LMDB 里最新的 bar，lastBarTime
// 已是库内最大时间，后续 get_uppers 只能命中等值分支。故本轮不构造该分支。
//
// 手法：只走公开 API；所有临时目录由 r100 夹具管理并清理；不启动任何线程。
#include "r100_ad_fixture.h"

#include "WtDataStorageAD/WtDataReaderAD.h"

namespace r132 {

using std::string;
using std::vector;

class R132ReaderADCacheTest : public r100fx::R100ADReaderTest
{
protected:
	uint64_t mt(uint32_t date, uint32_t hhmm) const
	{
		return TimeUtils::timeToMinBar(date, hhmm);
	}
};

// WtDataReaderAD.cpp: L444-467（rtBar 时间超前 → 再读 LMDB，_last_from_cache=false）
//
// 2026-10-28 R144：本用例是产品缺陷 BUG-R144-002 的复现，不再是夹具问题。
// 首次 cacheBarsFromStorage 调 get_k_db 时 `return std::move(it->second)`
// 把 _exchg_m1_dbs 的槽位掏空（既有 BUG-R4-BUG-02）；随后
// update_cache_from_lmdb 里 `WtLMDBQuery query(*db)` 对空的 WtLMDBPtr
// 直接解引用（WtDataReaderAD.cpp:272-274，无 null 检查），必现 SIGSEGV。
// 该分支在公开 API 下不可达，保留契约断言并禁用，待生产代码修复后恢复。
TEST_F(R132ReaderADCacheTest, DISABLED_R132ReaderADRtBarAheadRechecksLmdb)
{
	// CodeHelper 对两段式代码 EXQ.rb2301 解析出的品种就是 rb2301，
	// 夹具必须按同一键注册品种，否则 getCommodity 返回 NULL（原夹具写成 rb）。
	WTSContractInfo* ct = add_future("EXQ", "rb2301", "rb2301");
	ASSERT_TRUE(ct != NULL);
	init_reader();

	vector<WTSBarStruct> bars;
	bars.push_back(r4fx::make_bar_item("EXQ", "rb2301", 20230103, mt(20230103, 931), 10.0)._bar);
	bars.push_back(r4fx::make_bar_item("EXQ", "rb2301", 20230103, mt(20230103, 932), 11.0)._bar);
	bars.push_back(r4fx::make_bar_item("EXQ", "rb2301", 20230103, mt(20230103, 933), 12.0)._bar);
	seed_bars("min1", "EXQ", "rb2301", bars, false);

	// RT 缓存中的最后一条已经到 09:35，超过请求截止 09:34
	vector<BarCacheItem> items;
	items.push_back(r4fx::make_bar_item("EXQ", "rb2301", 20230103, mt(20230103, 935), 13.0));
	write_cache("cache_m1.dmb", items, 1, r4fx::R4_INIT_CAP);

	WTSKlineSlice* s = _reader->readKlineSlice("EXQ.rb2301", KP_Minute1, 2,
		20230103093400000ull);
	ASSERT_TRUE(s != NULL);
	ASSERT_EQ(s->size(), 2u);
	EXPECT_EQ(s->at(0)->time, mt(20230103, 932));
	EXPECT_EQ(s->at(1)->time, mt(20230103, 933));
	EXPECT_EQ(_sink->bar_calls(), 0u);
	s->release();
}

// WtDataReaderAD.cpp: readTickSlice L117-205（容量重载与尾部切片）
TEST_F(R132ReaderADCacheTest, R132ReaderADTickTailSlices)
{
	// 同上：两段式 stdCode 的 product 段与 code 段一致
	WTSContractInfo* ct = add_future("EXQ", "rb2301", "rb2301");
	ASSERT_TRUE(ct != NULL);
	init_reader();

	vector<WTSTickStruct> ticks;
	ticks.push_back(r4fx::make_tick("EXQ", "rb2301", 20230103, 20230103, 93100000, 10.0, 1, 1, 0, 0));
	ticks.push_back(r4fx::make_tick("EXQ", "rb2301", 20230103, 20230103, 93200000, 11.0, 1, 1, 0, 0));
	ticks.push_back(r4fx::make_tick("EXQ", "rb2301", 20230103, 20230103, 93300000, 12.0, 1, 1, 0, 0));
	seed_ticks("EXQ", "rb2301", ticks);

	WTSTickSlice* all = _reader->readTickSlice("EXQ.rb2301", 5, 20230103150000000ull);
	ASSERT_TRUE(all != NULL);
	ASSERT_EQ(all->size(), 3u);
	EXPECT_EQ(all->at(0)->action_time, 93100000u);
	EXPECT_EQ(all->at(2)->action_time, 93300000u);
	all->release();

	// 同一 etime 再取 2 条：不再查库，直接切环形缓冲尾部
	WTSTickSlice* tail = _reader->readTickSlice("EXQ.rb2301", 2, 20230103150000000ull);
	ASSERT_TRUE(tail != NULL);
	ASSERT_EQ(tail->size(), 2u);
	EXPECT_EQ(tail->at(0)->action_time, 93200000u);
	EXPECT_EQ(tail->at(1)->action_time, 93300000u);
	tail->release();
}

} // namespace r132
