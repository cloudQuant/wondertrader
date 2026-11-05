#include "r4_ad_fixture.h"

namespace {

// Build the existing packed disk layout byte-for-byte without constructing its
// potentially unaligned WTSTickStruct member in the test itself.
std::string rawTickItem(uint32_t date, const WTSTickStruct& tick)
{
	static_assert(sizeof(TickCacheItem) == sizeof(date) + sizeof(tick), "AD tick disk ABI changed");
	std::string bytes(sizeof(TickCacheItem), '\0');
	std::memcpy(&bytes[0], &date, sizeof(date));
	std::memcpy(&bytes[sizeof(date)], &tick, sizeof(tick));
	return bytes;
}

WTSTickStruct rawTickAt(const std::string& path, std::size_t index)
{
	WTSTickStruct result;
	std::ifstream input(path, std::ios::binary);
	input.seekg(sizeof(RTTickCache) + index * sizeof(TickCacheItem) + sizeof(uint32_t));
	input.read(reinterpret_cast<char*>(&result), sizeof(result));
	EXPECT_EQ(input.gcount(), static_cast<std::streamsize>(sizeof(result)));
	return result;
}

TEST_F(R4WriterADTest, L03AlignedTickCacheFirstWriteAndRawDiskLayout)
{
	init_writer_all_disabled();
	auto* comm = _bd->add_commodity("ALGN", "rb", ContractCategory::CC_Future);
	auto* ct = _bd->add_contract("ALGN", "rb01", comm);
	auto tick = make_tick("ALGN", "rb01", 20240108, 20240108, 93000000, 100.0, 1, 10, 2, 1);
	write_or_fail(tick_of(ct, tick), 0);
	const auto stored = rawTickAt(path_of("cache_tick.dmb"), 0);
	EXPECT_EQ(std::memcmp(&stored, &tick, sizeof(tick)), 0);
	WTSTickData* fetched = _writer->getCurTick("rb01", "ALGN");
	ASSERT_NE(fetched, nullptr);
	EXPECT_DOUBLE_EQ(fetched->price(), 100.0);
	fetched->release();
}

TEST_F(R4WriterADTest, L03AlignedTickCacheRecoveryAndSecondRecordUpdate)
{
	const auto first = make_tick("ALGN", "rb01", 20240108, 20240108, 93000000, 100.0, 1, 10, 2, 1);
	const auto second = make_tick("ALGN", "rb02", 20240108, 20240108, 93000000, 200.0, 2, 20, 3, 1);
	const auto payload = rawTickItem(20240108, first) + rawTickItem(20240108, second);
	write_block_file(path_of("cache_tick.dmb"), BLK_FLAG, BT_RT_Cache, 1, 2, 4, payload + std::string(2 * sizeof(TickCacheItem), '\0'));
	init_writer_all_disabled();
	auto* comm = _bd->add_commodity("ALGN", "rb", ContractCategory::CC_Future);
	_bd->add_contract("ALGN", "rb01", comm);
	auto* ct = _bd->add_contract("ALGN", "rb02", comm);
	WTSTickData* restored = _writer->getCurTick("rb02", "ALGN");
	ASSERT_NE(restored, nullptr);
	EXPECT_DOUBLE_EQ(restored->price(), 200.0);
	restored->release();
	auto next = make_tick("ALGN", "rb02", 20240108, 20240108, 93100000, 205.0, 3, 30, 4, 1);
	write_or_fail(tick_of(ct, next), 0);
	const auto actualFirst = rawTickAt(path_of("cache_tick.dmb"), 0);
	const auto actualSecond = rawTickAt(path_of("cache_tick.dmb"), 1);
	EXPECT_EQ(std::memcmp(&actualFirst, &first, sizeof(first)), 0);
	EXPECT_EQ(std::memcmp(&actualSecond, &next, sizeof(next)), 0);
}

TEST_F(R4WriterADTest, L03AlignedTickCacheRejectsTruncatedRecordWithoutMutation)
{
	auto tick = make_tick("ALGN", "rb01", 20240108, 20240108, 93000000, 100.0, 1, 10, 2, 1);
	std::string payload = rawTickItem(20240108, tick);
	payload.pop_back();
	write_block_file(path_of("cache_tick.dmb"), BLK_FLAG, BT_RT_Cache, 1, 1, 1, payload);
	std::ifstream beforeFile(path_of("cache_tick.dmb"), std::ios::binary);
	const std::string before((std::istreambuf_iterator<char>(beforeFile)), std::istreambuf_iterator<char>());
	beforeFile.close();
	WtDataWriterAD candidate;
	WTSVariant* cfg = make_writer_cfg(_root.string(), 1000, true, true, true, true);
	ASSERT_FALSE(candidate.init(cfg, _sink));
	cfg->release();
	std::ifstream afterFile(path_of("cache_tick.dmb"), std::ios::binary);
	const std::string after((std::istreambuf_iterator<char>(afterFile)), std::istreambuf_iterator<char>());
	EXPECT_EQ(after, before);
	EXPECT_FALSE(std::filesystem::exists(_root / "cache_m1.dmb"));
}

TEST_F(R4WriterADTest, L03AlignedTickCacheRejectsUnterminatedIdentityAndRetries)
{
	const auto first = make_tick("ALGN", "rb01", 20240108, 20240108, 93000000, 100.0, 1, 10, 2, 1);
	auto second = make_tick("ALGN", "rb02", 20240108, 20240108, 93000000, 200.0, 2, 20, 3, 1);
	std::memset(second.exchg, 'X', sizeof(second.exchg));
	const auto payload = rawTickItem(20240108, first) + rawTickItem(20240108, second);
	const auto path = path_of("cache_tick.dmb");
	write_block_file(path, BLK_FLAG, BT_RT_Cache, 1, 2, 2, payload);
	std::ifstream beforeFile(path, std::ios::binary);
	const std::string before((std::istreambuf_iterator<char>(beforeFile)), std::istreambuf_iterator<char>());
	beforeFile.close();
	WtDataWriterAD candidate;
	WTSVariant* cfg = make_writer_cfg(_root.string(), 1000, true, true, true, true);
	ASSERT_FALSE(candidate.init(cfg, _sink));
	std::ifstream afterFile(path, std::ios::binary);
	const std::string after((std::istreambuf_iterator<char>(afterFile)), std::istreambuf_iterator<char>());
	EXPECT_EQ(after, before);
	EXPECT_FALSE(std::filesystem::exists(_root / "cache_m1.dmb"));
	afterFile.close();

	second = make_tick("ALGN", "rb02", 20240108, 20240108, 93000000, 200.0, 2, 20, 3, 1);
	write_block_file(path, BLK_FLAG, BT_RT_Cache, 1, 2, 2,
		rawTickItem(20240108, first) + rawTickItem(20240108, second));
	ASSERT_TRUE(candidate.init(cfg, _sink));
	cfg->release();
	auto* comm = _bd->add_commodity("ALGN", "rb", ContractCategory::CC_Future);
	_bd->add_contract("ALGN", "rb02", comm);
	WTSTickData* restored = candidate.getCurTick("rb02", "ALGN");
	ASSERT_NE(restored, nullptr);
	EXPECT_DOUBLE_EQ(restored->price(), 200.0);
	restored->release();
	candidate.release();
}

TEST_F(R4WriterADTest, L03AlignedTickCacheRejectsSizeBeyondCapacity)
{
	const auto tick = make_tick("ALGN", "rb01", 20240108, 20240108, 93000000, 100.0, 1, 10, 2, 1);
	write_block_file(path_of("cache_tick.dmb"), BLK_FLAG, BT_RT_Cache, 1, 2, 1,
		rawTickItem(20240108, tick));
	std::ifstream beforeFile(path_of("cache_tick.dmb"), std::ios::binary);
	const std::string before((std::istreambuf_iterator<char>(beforeFile)), std::istreambuf_iterator<char>());
	beforeFile.close();
	WtDataWriterAD candidate;
	WTSVariant* cfg = make_writer_cfg(_root.string(), 1000, true, true, true, true);
	ASSERT_FALSE(candidate.init(cfg, _sink));
	cfg->release();
	std::ifstream afterFile(path_of("cache_tick.dmb"), std::ios::binary);
	const std::string after((std::istreambuf_iterator<char>(afterFile)), std::istreambuf_iterator<char>());
	EXPECT_EQ(after, before);
	EXPECT_FALSE(std::filesystem::exists(_root / "cache_m1.dmb"));
}

} // namespace
