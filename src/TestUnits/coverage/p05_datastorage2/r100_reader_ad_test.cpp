// r100 / WtDataReaderAD：onMinuteEnd 的三周期缓存补线、HOT tick/K 线、
// tick 全量重载、K 线 array_two 直取与 RT 缓存新增合约
//
// 覆盖（src/WtDataStorageAD/WtDataReaderAD.cpp）：
//  - readTickSlice 的 future/ruleTag -> getCustomRawCode :105-115
//  - readTickSlice 的 reload_flag==2 全量 get_lowers       :165-182
//  - get_rt_cache_bar 的 day/min5 包装、_last_size 增量索引 :305-361
//  - readKlineSlice 的 ruleTag 原始代码、array_two 直取 :393-403 / :472-490
//  - onMinuteEnd 的分钟段 :503-527 与日线段 :528-545
//
// 只走公开 API；LMDB 和 RTBarCache 都在 temp_directory_path()/r100_rad_* 下。
#include "r100_ad_fixture.h"

namespace r100fx {

namespace {

const uint32_t kDate = 20230103;

WTSBarStruct bar_at(uint32_t date, uint64_t time, double px)
{
	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	bar.date = date;
	bar.time = time;
	bar.open = px;
	bar.high = px + 1;
	bar.low = px - 1;
	bar.close = px + 0.5;
	bar.vol = 10;
	bar.money = px * 10;
	return bar;
}

uint64_t min_bar(uint32_t date, uint32_t hhmm)
{
	return (uint64_t)(date - 19900000) * 10000 + hhmm;
}

WTSTickStruct tick_at(const char* exchg, const char* code, uint32_t date, uint32_t atime, double px)
{
	WTSTickStruct tick = r4fx::make_tick(exchg, code, date, date, atime, px, 1, 10, 5, 0, 100, 1000);
	return tick;
}

} // namespace

// WtDataReaderAD.cpp: L305-361 / L496-527
TEST_F(R100ADReaderTest, R100ReaderADOnMinuteEndMinutes)
{
	add_stock("EXM1", "rb2301");

	// 分钟线：LMDB 里只有 09:31/09:32，RT 缓存已推进到 09:40
	vector<WTSBarStruct> m1;
	m1.push_back(bar_at(kDate, min_bar(kDate, 931), 3900.0));
	m1.push_back(bar_at(kDate, min_bar(kDate, 932), 3910.0));
	seed_bars("min1", "EXM1", "rb2301", m1, false);

	// 5 分钟线：LMDB 与 RT 缓存都停在 09:45，用来触发 _last_from_cache 的覆盖分支
	vector<WTSBarStruct> m5;
	m5.push_back(bar_at(kDate, min_bar(kDate, 930), 3900.0));
	m5.push_back(bar_at(kDate, min_bar(kDate, 935), 3910.0));
	m5.push_back(bar_at(kDate, min_bar(kDate, 945), 3945.0));
	seed_bars("min5", "EXM1", "rb2301", m5, false);

	vector<BarCacheItem> m1c;
	m1c.push_back(r4fx::make_bar_item("EXM1", "rb2301", kDate, min_bar(kDate, 940), 3940.0));
	write_cache("cache_m1.dmb", m1c, 1, 4);

	vector<BarCacheItem> m5c;
	m5c.push_back(r4fx::make_bar_item("EXM1", "rb2301", kDate, min_bar(kDate, 945), 3945.0));
	write_cache("cache_m5.dmb", m5c, 1, 4);

	init_reader();

	// min1：count=2, etime=09:31 -> get_lowers 取最后 2 根（09:31/09:32），
	// 末条不早于 etime，因此本次读不再访问 LMDB（避免第三次 get_k_db 的空句柄缺陷）。
	WTSKlineSlice* s1 = _reader->readKlineSlice("EXM1.rb2301", KP_Minute1, 2, 20230103093100000ull);
	ASSERT_TRUE(s1 != NULL);
	EXPECT_EQ(s1->size(), 2u);
	s1->release();
	EXPECT_EQ(_sink->bar_calls(), 0u);

	// min5：count=3, etime=09:30 -> 只做一次 get_k_db，供随后的 onMinuteEnd 使用第二次
	WTSKlineSlice* s5 = _reader->readKlineSlice("EXM1.rb2301", KP_Minute5, 3, 20230103093000000ull);
	ASSERT_TRUE(s5 != NULL);
	EXPECT_EQ(s5->size(), 3u);
	s5->release();

	// onMinuteEnd 处理 min1 时缓存 09:40 追加并回调；min5 只做等值检查。
	_reader->onMinuteEnd(kDate, 1000, 0);
	EXPECT_EQ(_sink->all_updated(), 1u);
	EXPECT_EQ(_sink->last_update_time(), 1000u);
	EXPECT_EQ(_sink->bar_calls(), 1u);
}

// WtDataReaderAD.cpp: L105-115 / L165-182 / L305-361 / L393-403 / L472-490 / L528-545
TEST_F(R100ADReaderTest, R100ReaderADHotReloadDayAndRtCacheGrowth)
{
	// ---- HOT tick：future + ruleTag -> 自定义主力代码 ----
	add_future("EXF", "rb", "rb2401");
	_hot->set_tag("HOT");
	_hot->set_raw("rb2401");
	vector<WTSTickStruct> hot_ticks;
	hot_ticks.push_back(tick_at("EXF", "rb", kDate, 93000000, 3900.0));
	hot_ticks.push_back(tick_at("EXF", "rb", kDate, 93100000, 3910.0));
	hot_ticks.push_back(tick_at("EXF", "rb", kDate, 93200000, 3920.0));
	seed_ticks("EXF", "rb", hot_ticks);

	// HOT K 线：DB 路径按 cInfo._code=rb，原始代码仍经 getCustomRawCode
	vector<WTSBarStruct> hot_bars;
	hot_bars.push_back(bar_at(kDate, min_bar(kDate, 931), 3900.0));
	hot_bars.push_back(bar_at(kDate, min_bar(kDate, 932), 3910.0));
	seed_bars("min1", "EXF", "rb", hot_bars, false);

	init_reader();

	WTSTickSlice* ht = _reader->readTickSlice("EXF.rb.HOT", 2, 20230103100000000ull);
	ASSERT_TRUE(ht != NULL);
	EXPECT_EQ(ht->size(), 2u);
	ht->release();

	WTSKlineSlice* hk = _reader->readKlineSlice("EXF.rb.HOT", KP_Minute1, 2, 20230103100000000ull);
	ASSERT_TRUE(hk != NULL);
	EXPECT_EQ(hk->size(), 2u);
	hk->release();

	// ---- reload_flag==2：容量不足且请求时间更早，走全量 get_lowers ----
	add_stock("EXR2", "rb2301");
	vector<WTSTickStruct> ticks;
	ticks.push_back(tick_at("EXR2", "rb2301", kDate, 93000000, 3900.0));
	ticks.push_back(tick_at("EXR2", "rb2301", kDate, 93100000, 3910.0));
	ticks.push_back(tick_at("EXR2", "rb2301", kDate, 93200000, 3920.0));
	seed_ticks("EXR2", "rb2301", ticks);

	WTSTickSlice* t1 = _reader->readTickSlice("EXR2.rb2301", 2, 20230103150000000ull);
	ASSERT_TRUE(t1 != NULL);
	EXPECT_EQ(t1->size(), 2u);
	t1->release();

	WTSTickSlice* t2 = _reader->readTickSlice("EXR2.rb2301", 4, 20230103090000000ull);
	ASSERT_TRUE(t2 != NULL);
	EXPECT_EQ(t2->size(), 0u); // 区间早于全部记录，全量查询 0 命中
	t2->release();

	// ---- array_two 足够时直接切片：capacity=count=1，补 1 根缓存 bar 后
	//      环形缓冲的 array_two 正好覆盖 1 条 ----
	add_stock("EXK3", "rb2301");
	vector<WTSBarStruct> kbars;
	kbars.push_back(bar_at(kDate, min_bar(kDate, 931), 3900.0));
	seed_bars("min5", "EXK3", "rb2301", kbars, false);
	vector<BarCacheItem> kc;
	kc.push_back(r4fx::make_bar_item("EXK3", "rb2301", kDate, min_bar(kDate, 940), 3940.0));
	write_cache("cache_m5.dmb", kc, 1, 4);

	WTSKlineSlice* k2 = _reader->readKlineSlice("EXK3.rb2301", KP_Minute5, 1, 20230103100000000ull);
	ASSERT_TRUE(k2 != NULL);
	ASSERT_EQ(k2->size(), 1u);
	EXPECT_EQ(k2->get_block_addr(0)[0].time, min_bar(kDate, 940));
	k2->release();

	// ---- RT 缓存新增合约：_last_size != _size 时补建索引 ----
	add_stock("EXN4", "rb2301");
	add_stock("EXNA", "rb2302");
	vector<WTSBarStruct> n1;
	n1.push_back(bar_at(kDate, min_bar(kDate, 931), 3900.0));
	seed_bars("min1", "EXN4", "rb2301", n1, false);
	vector<WTSBarStruct> n2;
	n2.push_back(bar_at(kDate, min_bar(kDate, 931), 4900.0));
	seed_bars("min1", "EXNA", "rb2302", n2, false);

	vector<BarCacheItem> nc;
	nc.push_back(r4fx::make_bar_item("EXN4", "rb2301", kDate, min_bar(kDate, 940), 3940.0));
	write_cache("cache_m1.dmb", nc, 1, 4);

	WTSKlineSlice* nk1 = _reader->readKlineSlice("EXN4.rb2301", KP_Minute1, 2, 20230103100000000ull);
	ASSERT_TRUE(nk1 != NULL);
	EXPECT_EQ(nk1->size(), 2u);
	nk1->release();

	// 模拟 writer 在同一个 cache_m1.dmb 里登记 rb2302 并推进它的未闭合 bar
	ASSERT_TRUE(patch_cache_size("cache_m1.dmb", 2));
	ASSERT_TRUE(patch_cache_item_code("cache_m1.dmb", 1, "EXNA", "rb2302"));
	ASSERT_TRUE(patch_cache_bar("cache_m1.dmb", 1, kDate, min_bar(kDate, 945)));

	WTSKlineSlice* nk2 = _reader->readKlineSlice("EXNA.rb2302", KP_Minute1, 2, 20230103100000000ull);
	ASSERT_TRUE(nk2 != NULL);
	ASSERT_EQ(nk2->size(), 2u);
	EXPECT_EQ(nk2->get_block_addr(0)[1].time, min_bar(kDate, 945));
	nk2->release();

	// ---- 日线：读时末条不早于 etime，只取一次库；cache 改到 uDate 后由
	//      onMinuteEnd 的日线段懒加载 RTBarCache、追加并回调 ----
	add_stock("EXD5", "rb2301");
	vector<WTSBarStruct> dbars;
	dbars.push_back(bar_at(20230102, 0, 4000.0));
	seed_bars("day", "EXD5", "rb2301", dbars, true);

	vector<BarCacheItem> dc;
	dc.push_back(r4fx::make_bar_item("EXD5", "rb2301", 20230104, 0, 4040.0));
	write_cache("cache_d1.dmb", dc, 1, 4);

	_reader.reset();
	init_reader();

	WTSKlineSlice* dk = _reader->readKlineSlice("EXD5.rb2301", KP_DAY, 2, 20230102100000000ull);
	ASSERT_TRUE(dk != NULL);
	EXPECT_EQ(dk->size(), 1u);
	dk->release();

	// 只做一次 get_k_db 后，把 RT 缓存推进到 uDate，onMinuteEnd 用第二次句柄补线
	ASSERT_TRUE(patch_cache_bar("cache_d1.dmb", 0, 20230103, 0));
	_reader->onMinuteEnd(20230103, 1000, 20230103);
	EXPECT_EQ(_sink->all_updated(), 1u);
	EXPECT_EQ(_sink->bar_calls(), 1u);
	EXPECT_EQ(_sink->last_bar().date, 20230103u);
}

} // namespace r100fx
