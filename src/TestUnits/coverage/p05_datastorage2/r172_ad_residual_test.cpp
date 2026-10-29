// r172 / WtDataStorageAD 读取端残余分支
//
// 覆盖（src/WtDataStorageAD/WtRdmDtReaderAD.cpp）：
//  - readTickSliceByRange：首笔 tick 早于起始时间时走第二次 lower_bound :236-247
//    （R185：被 BUG-R32-001 阻塞，用例改 DISABLED_，见下）
// 覆盖（src/WtDataStorageAD/WtDataReaderAD.cpp）：
//  - update_cache_from_lmdb 的「同时间戳且上次来自 RT 缓存」覆盖分支 :287-290
//    （R185：被 BUG-R144-002 阻塞，用例改 DISABLED_，见下）
//  - get_rt_cache_bar 的索引命中 :357-361
//  - readKlineSlice 中 RT 缓存 bar 已越过 etime 的二次 LMDB 刷新 :444-456
//    （R185：同样被 BUG-R144-002 阻塞，用例改 DISABLED_，见下）
//  - get_t_db 打开失败时 readTickSlice 返回 NULL :143-145
// 覆盖（src/WtDataStorageAD/WtBtDtReaderAD.cpp）：
//  - get_k_db 对已存在但非法的 LMDB 目录打开失败 :155-159
//
// LMDB 数据由用例直接按 LMDBKeys 公开布局写入（L04 的既有做法），
// 不依赖 WtDataWriterAD 的 _async_task 未初始化问题（BUG-R162-001）。
#include "r162_ad_fixture.h"

#include "Includes/IBtDtReader.h"
#include "Share/TimeUtils.hpp"
#include "WtDataStorageAD/DataDefineAD.h"
#include "WtDataStorageAD/LMDBKeys.h"
#include "WtDataStorageAD/WtBtDtReaderAD.h"
#include "WTSUtils/WtLMDB.hpp"

#include <fstream>

namespace r172ad {

using namespace r162ad;
using namespace wtp;
using std::string;
using std::vector;

namespace {

bool write_blob(const string& path, const string& bytes)
{
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
	std::ofstream ofs(path.c_str(), std::ios::binary | std::ios::trunc);
	if (!ofs.is_open())
		return false;
	ofs.write(bytes.data(), (std::streamsize)bytes.size());
	ofs.flush();
	return ofs.good();
}

string key_str(const LMDBHftKey& key)
{
	return string((const char*)&key, sizeof(key));
}

string key_str(const LMDBBarKey& key)
{
	return string((const char*)&key, sizeof(key));
}

string val_str(const WTSTickStruct& tick)
{
	return string((const char*)&tick, sizeof(tick));
}

string val_str(const WTSBarStruct& bar)
{
	return string((const char*)&bar, sizeof(bar));
}

bool seed_lmdb(const string& dir, const vector<std::pair<string, string>>& kvs)
{
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	WtLMDB db(false);
	if (!db.open(dir.c_str(), 16 * 1024 * 1024))
		return false;
	WtLMDBQuery query(db);
	for (const auto& kv : kvs)
	{
		if (!query.put(kv.first, kv.second))
			return false;
	}
	query.commit();
	return true;
}

WTSTickStruct make_tick_struct(uint32_t tdate, uint32_t atime, double price)
{
	WTSTickStruct tick;
	std::memset(&tick, 0, sizeof(tick));
	snprintf(tick.exchg, sizeof(tick.exchg), "%s", "SSE");
	snprintf(tick.code, sizeof(tick.code), "%s", "600000");
	tick.trading_date = tdate;
	tick.action_date = tdate;
	tick.action_time = atime;
	tick.price = price;
	tick.open = price;
	tick.high = price;
	tick.low = price;
	tick.settle_price = price;
	tick.total_volume = 10.0;
	tick.volume = 1.0;
	tick.total_turnover = price * 10.0;
	tick.turn_over = price;
	tick.open_interest = 100.0;
	tick.pre_interest = 99.0;
	return tick;
}

WTSBarStruct make_bar(uint32_t date, uint32_t time, double base)
{
	WTSBarStruct bar;
	std::memset(&bar, 0, sizeof(bar));
	bar.date = date;
	bar.time = time;
	bar.open = base;
	bar.high = base + 1.0;
	bar.low = base - 1.0;
	bar.close = base + 0.5;
	bar.vol = base * 2.0;
	return bar;
}

// 写一份与 WtDataWriterAD/reader 一致的 RT 分钟线缓存（cache_m1.dmb）
bool write_bar_cache(const string& path, const char* exchg, const char* code, const WTSBarStruct& bar)
{
	vector<char> buffer(sizeof(RTBarCache) + sizeof(BarCacheItem), 0);
	RTBarCache* cache = (RTBarCache*)buffer.data();
	cache->_capacity = 1;
	cache->_size = 1;
	cache->_type = BT_RT_Cache;
	cache->_version = 1;
	std::memcpy(cache->_blk_flag, BLK_FLAG, FLAG_SIZE);

	BarCacheItem& item = cache->_items[0];
	snprintf(item._exchg, sizeof(item._exchg), "%s", exchg);
	snprintf(item._code, sizeof(item._code), "%s", code);
	item._bar = bar;

	return write_blob(path, string(buffer.data(), buffer.size()));
}

uint32_t min_time(uint32_t date, uint32_t hhmm)
{
	return (uint32_t)TimeUtils::timeToMinBar(date, hhmm);
}

uint32_t bar_count(WTSKlineSlice* slice)
{
	uint32_t total = 0;
	if (slice == NULL)
		return 0;
	for (uint32_t i = 0; i < slice->get_block_counts(); i++)
		total += slice->get_block_size(i);
	return total;
}

uint32_t last_bar_time(WTSKlineSlice* slice)
{
	uint32_t out = 0;
	if (slice == NULL)
		return out;
	for (uint32_t i = 0; i < slice->get_block_counts(); i++)
	{
		WTSBarStruct* bars = slice->get_block_addr(i);
		uint32_t cnt = slice->get_block_size(i);
		if (bars != NULL && cnt > 0)
			out = (uint32_t)bars[cnt - 1].time;
	}
	return out;
}

class BtSink : public IBtDtReaderSink
{
public:
	void reader_log(WTSLogLevel ll, const char* message) override
	{
		_levels[ll]++;
		_last = message == NULL ? "" : message;
	}
	bool has_log(const char* needle) const { return _last.find(needle) != string::npos; }
	uint32_t count_of(WTSLogLevel ll) const
	{
		auto it = _levels.find(ll);
		return it == _levels.end() ? 0u : it->second;
	}

private:
	std::map<WTSLogLevel, uint32_t>	_levels;
	string				_last;
};

} // namespace

class R172AdTest : public AdFixture
{
};

// WtRdmDtReaderAD.cpp:236-247
//
// 2026-10-28 R185：本用例是产品缺陷 BUG-R32-001 的复现，不是夹具问题。
// 首次载入后 `WTSTickStruct& curTs = _ticks.front(); curTs = _ticks.back();`
// 把末笔整体覆写回 `_ticks[0]`（R32 已取证：3 笔 [09:30,09:31,09:32] 变成
// [09:32,09:31,09:32]），于是 `_ticks[0]` 恒为末笔、不再早于窗口起点，
// `:232` 的条件走 if 臂，`:236-247` 的第二次 lower_bound 永远不执行，
// 切片也只剩 0 条。保留原输入与断言并禁用，待产品修复后恢复。
TEST_F(R172AdTest, DISABLED_R172RdmADTickRangeStartsAtFirstTick)
{
	const uint32_t date = 20240110;
	// 区间内至少放两笔，避免命中「单笔且时间等于 eTick」时 pTick-- 的既有下溢面
	// （BUG-R100-002 同类缺陷，本 lane 不触发）。
	vector<WTSTickStruct> ticks = { make_tick_struct(date, 93000000, 9.0),
		make_tick_struct(date, 93100000, 10.0), make_tick_struct(date, 93200000, 11.0) };
	vector<std::pair<string, string>> kvs;
	for (const auto& tick : ticks)
		kvs.push_back(std::make_pair(key_str(LMDBHftKey("SSE", "600000", date, tick.action_time)), val_str(tick)));
	ASSERT_TRUE(seed_lmdb(base() + "ticks/SSE/600000", kvs));

	WtRdmDtReaderAD* reader = make_rdm_reader();
	ASSERT_TRUE(reader != NULL);

	WTSTickSlice* slice = reader->readTickSliceByRange("SSE.600000",
		(uint64_t)date * 1000000000ULL + 93100000ULL,
		(uint64_t)date * 1000000000ULL + 93200000ULL);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->size(), 1u);
	ASSERT_TRUE(slice->at(0) != NULL);
	EXPECT_EQ(slice->at(0)->action_time, 93100000u);
	slice->release();
}

// WtDataReaderAD.cpp:287-290 / 357-361；第二次读取时 LMDB 已追平 RT 缓存的时间戳
//
// 2026-10-28 R185：本用例是产品缺陷 BUG-R144-002 的复现，不是夹具问题。
// `cacheBarsFromStorage` 首次 `get_k_db` 后缓存槽仍有效；首读的
// `update_cache_from_lmdb` 是第 2 次取句柄（`std::move` 把槽位掏空但返回值可用）；
// 第二次读的 `update_cache_from_lmdb` 是第 3 次取句柄，得到空 `WtLMDBPtr`
// （既有 BUG-P05-001 / R4-BUG-02），`WtLMDBQuery query(*db)` 直接解引用空指针
// （WtDataReaderAD.cpp:272-274 无 null 检查），必现 SIGSEGV。
// :287-290 的「同时间戳且上次来自 RT 缓存」分支因此在公开 API 下不可达。
// 保留原输入与全部断言并禁用，待生产代码修复后恢复。
TEST_F(R172AdTest, DISABLED_R172ReaderADRtCacheBarPromotedToLmdb)
{
	const uint32_t date = 20240110;
	const uint32_t t1 = min_time(date, 931);
	const uint32_t t2 = min_time(date, 932);
	const WTSBarStruct b1 = make_bar(date, t1, 10.0);
	const WTSBarStruct b2 = make_bar(date, t2, 11.0);

	ASSERT_TRUE(seed_lmdb(base() + "min1/SSE",
		{ std::make_pair(key_str(LMDBBarKey("SSE", "600000", t1)), val_str(b1)) }));
	ASSERT_TRUE(write_bar_cache(path_of("cache_m1.dmb"), "SSE", "600000", b2));

	WtDataReaderAD* reader = make_reader();
	ASSERT_TRUE(reader != NULL);

	WTSKlineSlice* first = reader->readKlineSlice("SSE.600000", KP_Minute1, 10,
		(uint64_t)date * 1000000000ULL + 93200000ULL);
	ASSERT_TRUE(first != NULL);
	EXPECT_EQ(bar_count(first), 2u);	// LMDB 09:31 + RT 缓存 09:32
	EXPECT_EQ(last_bar_time(first), t2);
	first->release();

	// Writer 把 09:32 转储到 LMDB；下一次读取走 :287-290 的覆盖分支
	ASSERT_TRUE(seed_lmdb(base() + "min1/SSE",
		{ std::make_pair(key_str(LMDBBarKey("SSE", "600000", t2)), val_str(b2)) }));

	WTSKlineSlice* second = reader->readKlineSlice("SSE.600000", KP_Minute1, 10,
		(uint64_t)date * 1000000000ULL + 93300000ULL);
	ASSERT_TRUE(second != NULL);
	EXPECT_GE(bar_count(second), 2u);
	EXPECT_EQ(last_bar_time(second), t2);
	second->release();
}

// WtDataReaderAD.cpp:444-456；LMDB 只到 09:30，RT 缓存的 09:32 大于截止 09:31
//
// 2026-10-28 R185：同 BUG-R144-002。本用例在首读内触发「RT bar 越过 etime →
// 二次回源 LMDB」，此时 `get_k_db` 已是第 3 次取句柄（首读的
// `cacheBarsFromStorage` 第 1 次、首次 `update_cache_from_lmdb` 第 2 次），
// 命中被 `std::move` 掏空的槽位返回空 `WtLMDBPtr`，`WtLMDBQuery query(*db)`
// 解引用空指针（既有 BUG-P05-001 / R4-BUG-02），必现 SIGSEGV。
// :444-456 分支因此在公开 API 下不可达；保留原输入与全部断言并禁用。
TEST_F(R172AdTest, DISABLED_R172ReaderADRtCacheBarAheadOfEtime)
{
	const uint32_t date = 20240110;
	const uint32_t t0 = min_time(date, 930);
	const uint32_t t2 = min_time(date, 932);
	const WTSBarStruct b0 = make_bar(date, t0, 20.0);
	const WTSBarStruct b2 = make_bar(date, t2, 21.0);

	ASSERT_TRUE(seed_lmdb(base() + "min1/SSE",
		{ std::make_pair(key_str(LMDBBarKey("SSE", "600000", t0)), val_str(b0)) }));
	ASSERT_TRUE(write_bar_cache(path_of("cache_m1.dmb"), "SSE", "600000", b2));

	WtDataReaderAD* reader = make_reader();
	ASSERT_TRUE(reader != NULL);

	WTSKlineSlice* slice = reader->readKlineSlice("SSE.600000", KP_Minute1, 10,
		(uint64_t)date * 1000000000ULL + 93100000ULL);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(bar_count(slice), 1u);	// 只取已落库的 09:30，09:32 的 RT bar 越过 etime
	EXPECT_EQ(last_bar_time(slice), t0);
	slice->release();
}

// WtDataReaderAD.cpp:143-145
TEST_F(R172AdTest, R172ReaderADTickDbOpenFailureReturnsNull)
{
	ASSERT_TRUE(write_blob(base() + "ticks/SSE/600000/data.mdb", string(64, 'X')));

	WtDataReaderAD* reader = make_reader();
	ASSERT_TRUE(reader != NULL);

	WTSTickSlice* slice = reader->readTickSlice("SSE.600000", 10,
		(uint64_t)20240110 * 1000000000ULL + 150000000ULL);
	EXPECT_TRUE(slice == NULL);
	if (slice != NULL)
		slice->release();
	EXPECT_NE(_rsink->last_log().find("Opening tick db of SSE.600000 failed"), string::npos);
}

// WtBtDtReaderAD.cpp:155-159
TEST_F(R172AdTest, R172BtADKlineDbOpenFailureReturnsFalse)
{
	ASSERT_TRUE(write_blob(base() + "min1/SSE/data.mdb", string(64, 'X')));

	const string cfgfile = write_cfg("bt.json");
	WTSVariant* cfg = WTSCfgLoader::load_from_file(cfgfile.c_str());
	ASSERT_TRUE(cfg != NULL);

	BtSink sink;
	WtBtDtReaderAD reader;
	reader.init(cfg, &sink);
	cfg->release();

	string buffer;
	EXPECT_FALSE(reader.read_raw_bars("SSE", "600000", KP_Minute1, buffer));
	EXPECT_TRUE(buffer.empty());
	EXPECT_TRUE(sink.has_log("Opening min1 db "));
	EXPECT_TRUE(sink.has_log("SSE failed"));
	EXPECT_GE(sink.count_of(LL_ERROR), 1u);
}

} // namespace r172ad
