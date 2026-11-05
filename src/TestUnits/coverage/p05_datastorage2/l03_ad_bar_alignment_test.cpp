#include "r4_ad_fixture.h"
#include "WtDataStorageAD/LMDBKeys.h"
#include <map>

namespace {

std::string rawBarItem(const char* exchange, const char* code, const WTSBarStruct& bar)
{
	static_assert(sizeof(BarCacheItem) == 48 + sizeof(bar), "AD bar disk ABI changed");
	std::string bytes(sizeof(BarCacheItem), '\0');
	std::memcpy(&bytes[0], exchange, std::strlen(exchange));
	std::memcpy(&bytes[16], code, std::strlen(code));
	std::memcpy(&bytes[48], &bar, sizeof(bar));
	return bytes;
}

WTSBarStruct rawBarAt(const std::string& path, std::size_t index)
{
	WTSBarStruct bar;
	std::ifstream input(path, std::ios::binary);
	input.seekg(sizeof(RTBarCache) + index * sizeof(BarCacheItem) + 48);
	input.read(reinterpret_cast<char*>(&bar), sizeof(bar));
	EXPECT_EQ(input.gcount(), static_cast<std::streamsize>(sizeof(bar)));
	return bar;
}

std::string readAll(const std::filesystem::path& path)
{
	std::ifstream file(path, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

class AlignedBarDumper : public IHisDataDumper
{
public:
	bool dumpHisBars(const char*, const char* period, WTSBarStruct* bars, uint32_t count) override
	{
		if (count) values[period].push_back(*bars);
		return true;
	}
	bool dumpHisTicks(const char*, uint32_t, WTSTickStruct*, uint32_t) override { return true; }
	std::map<std::string, std::vector<WTSBarStruct>> values;
};

TEST_F(R4WriterADTest, L03AlignedBarCachesFirstWriteAndSameMinuteUpdate)
{
	init_writer(make_writer_cfg(_root.string(), 1000, true, false, false, false));
	auto* comm = _bd->add_commodity("ALGN", "rb", ContractCategory::CC_Future);
	auto* ct = _bd->add_contract("ALGN", "rb01", comm);
	auto first = make_tick("ALGN", "rb01", 20240108, 20240108, 93000000, 100, 1, 10, 2, 1);
	auto second = make_tick("ALGN", "rb01", 20240108, 20240108, 93001000, 105, 2, 20, 3, 1, 3, 30);
	write_or_fail(tick_of(ct, first), 0);
	write_or_fail(tick_of(ct, second), 0);
	for (const char* name : {"cache_m1.dmb", "cache_m5.dmb", "cache_d1.dmb"})
	{
		EXPECT_EQ(read_head(path_of(name))._size, 1u);
		const auto bar = rawBarAt(path_of(name), 0);
		EXPECT_DOUBLE_EQ(bar.open, 100);
		EXPECT_DOUBLE_EQ(bar.close, 105);
		EXPECT_DOUBLE_EQ(bar.vol, 3);
	}
}

TEST_F(R4WriterADTest, L03AlignedBarCachesRecoverSecondRecordAndFlushPreviousBars)
{
	WTSBarStruct first, second;
	first.date = second.date = 20240108;
	first.open = first.close = 70;
	second.open = second.close = 80;
	first.time = second.time = TimeUtils::timeToMinBar(20240108, 930);
	const auto barPayload = rawBarItem("ALGN", "rb01", first) + rawBarItem("ALGN", "rb02", second);
	for (const char* name : {"cache_m1.dmb", "cache_m5.dmb", "cache_d1.dmb"})
		write_block_file(path_of(name), BLK_FLAG, BT_RT_Cache, 1, 2, 3,
			barPayload + std::string(sizeof(BarCacheItem), '\0'));
	init_writer(make_writer_cfg(_root.string(), 1000, true, false, false, false));
	auto* comm = _bd->add_commodity("ALGN", "rb", ContractCategory::CC_Future);
	auto* ct = _bd->add_contract("ALGN", "rb02", comm);
	AlignedBarDumper dumper;
	_writer->add_ext_dumper("record", &dumper);
	auto tick = make_tick("ALGN", "rb02", 20240109, 20240109, 93100000, 90, 1, 10, 2, 1);
	write_or_fail(tick_of(ct, tick), 0);
	// 卡3 3c=C（用户裁定 2026-11-04，R4-BUG-13）：版本 1 的旧日线缓存由 writer 打开时原地迁移为
	// time=0 并把头版本提升为 2；其余字段逐字节保留。分钟缓存不迁移，仍为版本 1、逐字节不变。
	WTSBarStruct firstDay = first, secondDay = second;
	firstDay.time = secondDay.time = 0;
	EXPECT_EQ(read_head(path_of("cache_d1.dmb"))._version, (uint16_t)2);
	EXPECT_EQ(read_head(path_of("cache_m1.dmb"))._version, (uint16_t)1);
	EXPECT_EQ(read_head(path_of("cache_m5.dmb"))._version, (uint16_t)1);
	for (const auto& entry : {std::pair<const char*, const char*>{"cache_d1.dmb", "d1"},
		{"cache_m1.dmb", "m1"}, {"cache_m5.dmb", "m5"}})
	{
		const bool isDay = std::strcmp(entry.second, "d1") == 0;
		const WTSBarStruct& firstExpected = isDay ? firstDay : first;
		const WTSBarStruct& secondExpected = isDay ? secondDay : second;
		const auto firstAfter = rawBarAt(path_of(entry.first), 0);
		const auto secondAfter = rawBarAt(path_of(entry.first), 1);
		EXPECT_EQ(std::memcmp(&firstAfter, &firstExpected, sizeof(first)), 0) << entry.second;
		EXPECT_EQ(secondAfter.date, 20240109u);
		if (isDay)
		{
			EXPECT_EQ(secondAfter.time, 0u) << "new day bar must carry time=0";
		}
		EXPECT_DOUBLE_EQ(secondAfter.close, 90);
		ASSERT_EQ(dumper.values[entry.second].size(), 1u);
		EXPECT_EQ(std::memcmp(&dumper.values[entry.second][0], &secondExpected, sizeof(second)), 0) << entry.second;
	}
	_writer->release();
	_writer.reset(); // Close each LMDB before opening it for readback.
	for (const auto& entry : {std::pair<const char*, const char*>{"day", "d1"},
		{"min1", "m1"}, {"min5", "m5"}})
	{
		WtLMDB db(false);
		ASSERT_TRUE(db.open((_root / entry.first / "ALGN").string().c_str(), 8 * 1024 * 1024));
		WtLMDBQuery query(db);
		LMDBBarKey key("ALGN", "rb02", entry.second == std::string("d1") ? second.date : static_cast<uint32_t>(second.time));
		const std::string value = query.get(&key, sizeof(key));
		ASSERT_EQ(value.size(), sizeof(second));
		// 跨日落库的 LMDB day 记录同样遵守 time=0（3c=C）；分钟记录逐字节等于原 bar
		const WTSBarStruct& expected = entry.second == std::string("d1") ? secondDay : second;
		EXPECT_EQ(std::memcmp(value.data(), &expected, sizeof(second)), 0) << entry.second;
	}
}

TEST_F(R4WriterADTest, L03AlignedBarCacheRejectsTruncatedFileWithoutMutation)
{
	WTSBarStruct bar;
	bar.date = 20240108;
	std::string payload = rawBarItem("ALGN", "rb01", bar);
	payload.pop_back();
	const auto path = path_of("cache_m1.dmb");
	write_block_file(path, BLK_FLAG, BT_RT_Cache, 1, 1, 1, payload);
	std::ifstream beforeFile(path, std::ios::binary);
	const std::string before((std::istreambuf_iterator<char>(beforeFile)), std::istreambuf_iterator<char>());
	beforeFile.close();
	WtDataWriterAD candidate;
	WTSVariant* cfg = make_writer_cfg(_root.string(), 1000, true, true, true, true);
	ASSERT_FALSE(candidate.init(cfg, _sink));
	cfg->release();
	std::ifstream afterFile(path, std::ios::binary);
	const std::string after((std::istreambuf_iterator<char>(afterFile)), std::istreambuf_iterator<char>());
	EXPECT_EQ(after, before);
	EXPECT_FALSE(std::filesystem::exists(_root / "cache_tick.dmb"));
}

TEST_F(R4WriterADTest, L03AlignedBarCacheRejectsMalformedIdentityAndRetries)
{
	const char* names[] = {"cache_m1.dmb", "cache_m5.dmb", "cache_d1.dmb"};
	for (const char* malformed : names)
	{
		const auto root = _root / malformed;
		std::filesystem::create_directories(root);
		write_block_file((root / "cache_tick.dmb").string(), BLK_FLAG, BT_RT_Cache, 1, 0, 2,
			std::string(2 * sizeof(TickCacheItem), '\0'));
		WTSBarStruct first, second;
		first.date = second.date = 20240108;
		first.close = 70;
		second.close = 80;
		const auto good = rawBarItem("ALGN", "rb01", first) + rawBarItem("ALGN", "rb02", second);
		std::string bad = good;
		std::memset(&bad[sizeof(BarCacheItem) + 16], 'X', 32);
		for (const char* name : names)
			write_block_file((root / name).string(), BLK_FLAG, BT_RT_Cache, 1, 2, 2,
				std::strcmp(name, malformed) == 0 ? bad : good);
		const auto before = readAll(root / malformed);
		WtDataWriterAD candidate;
		WTSVariant* cfg = make_writer_cfg(root.string(), 1000, true, true, true, true);
		ASSERT_FALSE(candidate.init(cfg, _sink)) << malformed;
		EXPECT_EQ(readAll(root / malformed), before);
		write_block_file((root / malformed).string(), BLK_FLAG, BT_RT_Cache, 1, 2, 2, good);
		ASSERT_TRUE(candidate.init(cfg, _sink)) << malformed;
		cfg->release();
		candidate.release();
	}
}

TEST_F(R4WriterADTest, L03AlignedBarCacheRejectsSizeBeyondCapacity)
{
	WTSBarStruct bar;
	bar.date = 20240108;
	const auto path = path_of("cache_m5.dmb");
	write_block_file(path, BLK_FLAG, BT_RT_Cache, 1, 2, 1, rawBarItem("ALGN", "rb01", bar));
	const auto before = readAll(path);
	WtDataWriterAD candidate;
	WTSVariant* cfg = make_writer_cfg(_root.string(), 1000, true, true, true, true);
	ASSERT_FALSE(candidate.init(cfg, _sink));
	cfg->release();
	EXPECT_EQ(readAll(path), before);
	EXPECT_FALSE(std::filesystem::exists(_root / "cache_tick.dmb"));
}

} // namespace
