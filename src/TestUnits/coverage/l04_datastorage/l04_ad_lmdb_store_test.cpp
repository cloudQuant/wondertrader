// L04 / WtDataStorageAD：AD 缓存块布局 + LMDB 键区间读写 round-trip
// 覆盖 DataDefineAD.h 的 RTTickCache/RTBarCache 二进制布局（临时目录落盘校验）
// 以及 WtLMDB/WtLMDBQuery + LMDBBarKey/LMDBHftKey 的区间语义（等价于
// WtDataReaderAD::cacheBarsFromStorage / update_cache_from_lmdb 的取数逻辑）。
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Includes/WTSStruct.h"
#include "Share/TimeUtils.hpp"
#include "WtDataStorageAD/DataDefineAD.h"
#include "WtDataStorageAD/LMDBKeys.h"
#include "WTSUtils/WtLMDB.hpp"

using namespace wtp;

namespace {

class L04ADStoreTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		static uint32_t seq = 0;
		_root = std::filesystem::temp_directory_path()
			/ ("l04_ad_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
		std::filesystem::remove_all(_root);
		std::filesystem::create_directories(_root);
	}

	void TearDown() override
	{
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
	}

	static std::string key_str(const LMDBBarKey& key)
	{
		return std::string((const char*)&key, sizeof(LMDBBarKey));
	}

	static std::string key_str(const LMDBHftKey& key)
	{
		return std::string((const char*)&key, sizeof(LMDBHftKey));
	}

	static std::string bar_str(const WTSBarStruct& bar)
	{
		return std::string((const char*)&bar, sizeof(WTSBarStruct));
	}

	// 与 WtDataWriterAD 一致：分钟线用 timeToMinBar，日线用 date 作为键尾
	static uint32_t min_bar_time(uint32_t date, uint32_t hhmm)
	{
		return (uint32_t)TimeUtils::timeToMinBar(date, hhmm);
	}

	static WTSBarStruct make_bar(uint32_t date, uint64_t time, double base)
	{
		WTSBarStruct bar;
		bar.date = date;
		bar.time = time;
		bar.open = base;
		bar.high = base + 1.0;
		bar.low = base - 1.0;
		bar.close = base + 0.5;
		bar.vol = base * 2.0;
		return bar;
	}

	// 写入 20 根分钟线（同一合约），返回期望的时间序列
	std::vector<uint32_t> seed_bars(const std::string& db_path, const char* exchg, const char* code)
	{
		std::vector<uint32_t> times;
		WtLMDB db(false);
		if (!db.open(db_path.c_str(), 8 * 1024 * 1024))
			return times;

		WtLMDBQuery query(db);
		for (uint32_t i = 0; i < 20; i++)
		{
			const uint32_t bt = min_bar_time(20240102, 931 + i);
			WTSBarStruct bar = make_bar(20240102, bt, 100.0 + i);
		 EXPECT_TRUE(query.put(key_str(LMDBBarKey(exchg, code, bt)), bar_str(bar)));
			times.push_back(bt);
		}
		query.commit();
		return times;
	}

	std::filesystem::path _root;
};

void write_blob(const std::filesystem::path& path, const std::string& content)
{
	std::filesystem::create_directories(path.parent_path());
	std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
	ofs.write(content.data(), (std::streamsize)content.size());
	ofs.flush();
}

std::string read_blob(const std::filesystem::path& path)
{
	std::ifstream ifs(path, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

}

//////////////////////////////////////////////////////////////////////////
// DataDefineAD.h 布局
//////////////////////////////////////////////////////////////////////////

TEST(L04ADLayout, HeaderSizesMatchStorageModule)
{
	EXPECT_EQ(sizeof(BlockHeader), (std::size_t)(FLAG_SIZE + 4));
	EXPECT_EQ(BLOCK_HEADER_SIZE, sizeof(BlockHeader));
	EXPECT_EQ(sizeof(RTBlockHeader), sizeof(BlockHeader) + 2 * sizeof(uint32_t));
	EXPECT_EQ(BT_RT_Cache, 4);
	EXPECT_EQ(BLOCK_VERSION_RAW, 1);
	EXPECT_EQ(sizeof(TickCacheItem), sizeof(uint32_t) + sizeof(WTSTickStruct));
	EXPECT_EQ(sizeof(BarCacheItem), (std::size_t)(16 + 32 + sizeof(WTSBarStruct)));
}

TEST(L04ADLayout, FlexibleArrayOffsets)
{
	std::vector<char> buf(sizeof(RTTickCache) + 3 * sizeof(TickCacheItem), 0);
	RTTickCache* cache = (RTTickCache*)buf.data();
	EXPECT_EQ((const char*)&cache->_items[0] - buf.data(), (std::ptrdiff_t)sizeof(RTBlockHeader));
	for (uint32_t i = 0; i < 3; i++)
		EXPECT_EQ((const char*)&cache->_items[i] - buf.data(),
			(std::ptrdiff_t)(sizeof(RTBlockHeader) + i * sizeof(TickCacheItem)));

	std::vector<char> bbuf(sizeof(RTBarCache) + 2 * sizeof(BarCacheItem), 0);
	RTBarCache* bars = (RTBarCache*)bbuf.data();
	EXPECT_EQ((const char*)&bars->_items[0] - bbuf.data(), (std::ptrdiff_t)sizeof(RTBlockHeader));
	EXPECT_EQ((const char*)&bars->_items[1]._bar.open - (const char*)&bars->_items[1]._bar,
		(std::ptrdiff_t)(16 + 32));
}

TEST_F(L04ADStoreTest, BarCacheFileRoundTrip)
{
	const uint32_t cnt = 5;
	std::string content(sizeof(RTBlockHeader) + cnt * sizeof(BarCacheItem), 0);
	RTBarCache* cache = (RTBarCache*)content.data();
	memcpy(cache->_blk_flag, BLK_FLAG, FLAG_SIZE);
	cache->_type = BT_RT_Cache;
	cache->_version = BLOCK_VERSION_RAW;
	cache->_size = cnt;
	cache->_capacity = 10000;
	for (uint32_t i = 0; i < cnt; i++)
	{
		BarCacheItem& item = cache->_items[i];
		wt_strcpy(item._exchg, "SHFE");
		wt_strcpy(item._code, "rb2401");
		item._bar = make_bar(20240102, min_bar_time(20240102, 931 + i), 3000.0 + i);
	}

	const std::filesystem::path file = _root / "barcache.bin";
	write_blob(file, content);
	const std::string back = read_blob(file);
	ASSERT_EQ(back.size(), content.size());

	const RTBarCache* rc = (const RTBarCache*)back.data();
	EXPECT_EQ(memcmp(rc->_blk_flag, BLK_FLAG, FLAG_SIZE), 0);
	EXPECT_EQ(rc->_size, cnt);
	EXPECT_EQ(rc->_capacity, 10000u);
	for (uint32_t i = 0; i < cnt; i++)
	{
		EXPECT_STREQ(rc->_items[i]._exchg, "SHFE");
		EXPECT_STREQ(rc->_items[i]._code, "rb2401");
		EXPECT_DOUBLE_EQ(rc->_items[i]._bar.open, 3000.0 + i);
		EXPECT_EQ(rc->_items[i]._bar.time, (uint64_t)min_bar_time(20240102, 931 + i));
	}
}

TEST_F(L04ADStoreTest, TickCacheFileRoundTrip)
{
	const uint32_t cnt = 3;
	std::string content(sizeof(RTBlockHeader) + cnt * sizeof(TickCacheItem), 0);
	RTTickCache* cache = (RTTickCache*)content.data();
	memcpy(cache->_blk_flag, BLK_FLAG, FLAG_SIZE);
	cache->_type = BT_RT_Cache;
	cache->_version = BLOCK_VERSION_RAW;
	cache->_size = cnt;
	cache->_capacity = 5000;
	for (uint32_t i = 0; i < cnt; i++)
	{
		cache->_items[i]._date = 20240102 + i;
		WTSTickStruct& t = cache->_items[i]._tick;
		t = WTSTickStruct();
		wt_strcpy(t.code, "600000");
		t.price = 8.0 + i;
		t.action_time = 150000000u;
	}

	const std::filesystem::path file = _root / "tickcache.bin";
	write_blob(file, content);
	const std::string back = read_blob(file);
	const RTTickCache* rc = (const RTTickCache*)back.data();
	for (uint32_t i = 0; i < cnt; i++)
	{
		EXPECT_EQ(rc->_items[i]._date, 20240102u + i);
		EXPECT_STREQ(rc->_items[i]._tick.code, "600000");
		EXPECT_DOUBLE_EQ(rc->_items[i]._tick.price, 8.0 + i);
	}
}

//////////////////////////////////////////////////////////////////////////
// LMDB 键区间读写（对应 WtDataReaderAD / WtRdmDtReaderAD 的取数路径）
//////////////////////////////////////////////////////////////////////////

TEST_F(L04ADStoreTest, BarsCommittedAndReadableBack)
{
	const std::string db_path = (_root / "kline").string();
	std::vector<uint32_t> times = seed_bars(db_path, "SHFE", "rb2401");
	ASSERT_EQ(times.size(), 20u);

	WtLMDB db(true);
	ASSERT_TRUE(db.open(db_path.c_str(), 8 * 1024 * 1024));
	WtLMDBQuery query(db);

	LMDBBarKey lKey("SHFE", "rb2401", 0);
	LMDBBarKey rKey("SHFE", "rb2401", 0xffffffff);
	std::vector<WTSBarStruct> got;
	int cnt = query.get_range(key_str(lKey), key_str(rKey),
		[&got](const ValueArray& /*ayKeys*/, const ValueArray& ayVals) {
			for (const std::string& item : ayVals)
			{
				ASSERT_EQ(item.size(), sizeof(WTSBarStruct));
				got.push_back(*(const WTSBarStruct*)item.data());
			}
		});
	EXPECT_EQ(cnt, 20);
	ASSERT_EQ(got.size(), 20u);
	for (std::size_t i = 0; i < got.size(); i++)
	{
		// get_range 逐条回调按字典序返回，即时间升序
		EXPECT_EQ(got[i].time, (uint64_t)times[i]) << "index " << i;
		EXPECT_DOUBLE_EQ(got[i].close, 100.5 + (double)i);
	}
}

TEST_F(L04ADStoreTest, GetLowersReturnsTailSliceWithLeftBound)
{
	const std::string db_path = (_root / "kline").string();
	ASSERT_EQ(seed_bars(db_path, "SHFE", "rb2401").size(), 20u);

	WtLMDB db(true);
	ASSERT_TRUE(db.open(db_path.c_str(), 8 * 1024 * 1024));
	WtLMDBQuery query(db);

	LMDBBarKey lKey("SHFE", "rb2401", 0);
	LMDBBarKey rKey("SHFE", "rb2401", 0xffffffff);
	std::vector<uint32_t> keys;
	std::vector<WTSBarStruct> bars;
	int cnt = query.get_lowers(key_str(lKey), key_str(rKey), 5,
		[&keys, &bars](const ValueArray& ayKeys, const ValueArray& ayVals) {
			for (std::size_t i = 0; i < ayVals.size(); i++)
			{
				keys.push_back(reverseEndian(*(const uint32_t*)((const char*)ayKeys[i].data()
					+ MAX_EXCHANGE_LENGTH + MAX_INSTRUMENT_LENGTH)));
				bars.push_back(*(const WTSBarStruct*)ayVals[i].data());
			}
		});
	EXPECT_EQ(cnt, 5);
	ASSERT_EQ(bars.size(), 5u);
	// 逆序查找后已 reverse，仍是升序
	for (std::size_t i = 1; i < keys.size(); i++)
		EXPECT_LT(keys[i - 1], keys[i]);
	EXPECT_EQ(keys.back(), min_bar_time(20240102, 950));
	EXPECT_DOUBLE_EQ(bars.front().open, 115.0);
	EXPECT_DOUBLE_EQ(bars.back().open, 119.0);
}

TEST_F(L04ADStoreTest, GetUppersSkipsBarEqualToLastBarTime)
{
	const std::string db_path = (_root / "kline").string();
	std::vector<uint32_t> times = seed_bars(db_path, "SHFE", "rb2401");
	ASSERT_EQ(times.size(), 20u);

	WtLMDB db(true);
	ASSERT_TRUE(db.open(db_path.c_str(), 8 * 1024 * 1024));
	WtLMDBQuery query(db);

	// update_cache_from_lmdb：从 lastBarTime 往后取，等于 lastBarTime 的那根要覆盖而不是追加
	const uint32_t lastBarTime = times[10];
	LMDBBarKey lKey("SHFE", "rb2401", lastBarTime);
	LMDBBarKey rKey("SHFE", "rb2401", 0xFFFFFFFF);
	std::vector<uint64_t> pushed;
	int cnt = query.get_uppers(key_str(lKey), key_str(rKey), 9999,
		[&pushed, lastBarTime](const ValueArray& ayKeys, const ValueArray& ayVals) {
			for (std::size_t idx = 0; idx < ayVals.size(); idx++)
			{
				const WTSBarStruct* bar = (const WTSBarStruct*)ayVals[idx].data();
				(void)ayKeys;
				if (bar->time == lastBarTime)
					continue;
				pushed.push_back(bar->time);
			}
		});
	EXPECT_EQ(cnt, 10);
	ASSERT_EQ(pushed.size(), 9u);
	for (std::size_t i = 0; i < pushed.size(); i++)
		EXPECT_EQ(pushed[i], (uint64_t)times[11 + i]);
}

TEST_F(L04ADStoreTest, CodesInSameDbAreIsolatedByKeyPrefix)
{
	const std::string db_path = (_root / "multi").string();
	{
		WtLMDB db(false);
		ASSERT_TRUE(db.open(db_path.c_str(), 8 * 1024 * 1024));
		WtLMDBQuery query(db);
		for (uint32_t i = 0; i < 6; i++)
		{
			const uint32_t bt = min_bar_time(20240102, 931 + i);
			WTSBarStruct bar = make_bar(20240102, bt, 10.0 + i);
			EXPECT_TRUE(query.put(key_str(LMDBBarKey("SHFE", "rb2401", bt)), bar_str(bar)));
			bar = make_bar(20240102, bt, 200.0 + i);
			EXPECT_TRUE(query.put(key_str(LMDBBarKey("SHFE", "rb2402", bt)), bar_str(bar)));
			bar = make_bar(20240102, bt, 700.0 + i);
			EXPECT_TRUE(query.put(key_str(LMDBBarKey("DCE", "i2401", bt)), bar_str(bar)));
		}
		query.commit();
	}

	WtLMDB db(true);
	ASSERT_TRUE(db.open(db_path.c_str(), 8 * 1024 * 1024));
	WtLMDBQuery query(db);
	std::vector<double> opens;
	LMDBBarKey lKey("SHFE", "rb2402", 0);
	LMDBBarKey rKey("SHFE", "rb2402", 0xffffffff);
	int cnt = query.get_range(key_str(lKey), key_str(rKey),
		[&opens](const ValueArray& /*ayKeys*/, const ValueArray& ayVals) {
			for (const std::string& item : ayVals)
				opens.push_back(((const WTSBarStruct*)item.data())->open);
		});
	EXPECT_EQ(cnt, 6);
	ASSERT_EQ(opens.size(), 6u);
	EXPECT_DOUBLE_EQ(opens.front(), 200.0);
	EXPECT_DOUBLE_EQ(opens.back(), 205.0);
}

TEST_F(L04ADStoreTest, TickRangeByDateUsesHftKeyOrdering)
{
	const std::string db_path = (_root / "ticks").string();
	const char* exchg = "CFFEX";
	const char* code = "IF2401";
	{
		WtLMDB db(false);
		ASSERT_TRUE(db.open(db_path.c_str(), 8 * 1024 * 1024));
		WtLMDBQuery query(db);
		const uint32_t dates[] = { 20240102u, 20240103u };
		for (uint32_t date : dates)
		{
			for (uint32_t i = 0; i < 10; i++)
			{
				WTSTickStruct tick;
				tick = WTSTickStruct();
				wt_strcpy(tick.exchg, exchg);
				wt_strcpy(tick.code, code);
				tick.trading_date = date;
				tick.action_time = 93000000u + i;
				tick.price = (double)(date % 100) + i * 0.5;
				LMDBHftKey key(exchg, code, date, 93000000u + i);
				EXPECT_TRUE(query.put(key_str(key),
					std::string((const char*)&tick, sizeof(tick))));
			}
		}
		query.commit();
	}

	WtLMDB db(true);
	ASSERT_TRUE(db.open(db_path.c_str(), 8 * 1024 * 1024));
	WtLMDBQuery query(db);

	std::vector<uint32_t> times;
	LMDBHftKey lKey(exchg, code, 20240103u, 0u);
	LMDBHftKey rKey(exchg, code, 20240103u, 240000000u);
	int cnt = query.get_range(key_str(lKey), key_str(rKey),
		[&times](const ValueArray& /*ayKeys*/, const ValueArray& ayVals) {
			for (std::size_t i = 0; i < ayVals.size(); i++)
			{
				const WTSTickStruct& tick = *(const WTSTickStruct*)ayVals[i].data();
				times.push_back(tick.action_time);
			}
		});
	EXPECT_EQ(cnt, 10);
	ASSERT_EQ(times.size(), 10u);
	for (std::size_t i = 0; i < times.size(); i++)
		EXPECT_EQ(times[i], 93000000u + (uint32_t)i);

	// 单条精确命中
	std::string one = query.get(key_str(LMDBHftKey(exchg, code, 20240102u, 93000005u)));
	ASSERT_EQ(one.size(), sizeof(WTSTickStruct));
	EXPECT_DOUBLE_EQ(((const WTSTickStruct*)one.data())->price, 2.5 + 5 * 0.5);
}

TEST_F(L04ADStoreTest, BarTimeEncodingMatchesReaderExpectation)
{
	// 与 WtDataReaderAD 的 (uDate-19900000)*10000+uTime 保持一致
	const uint32_t date = 20240102u;
	const uint32_t hhmm = 1459u;
	const uint32_t bt = min_bar_time(date, hhmm);
	EXPECT_EQ(bt, (20240102u - 19900000u) * 10000u + 1459u);
	EXPECT_EQ(TimeUtils::minBarToDate(bt), date);
	EXPECT_EQ(TimeUtils::minBarToTime(bt), hhmm);
	EXPECT_EQ(reverseEndian(reverseEndian(bt)), bt);
}
