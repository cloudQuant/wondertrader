#include "r4_ad_fixture.h"

namespace {
std::string retryTickItem(const WTSTickStruct& tick)
{
	std::string item(sizeof(TickCacheItem), '\0');
	const uint32_t date = tick.trading_date;
	std::memcpy(&item[0], &date, sizeof(date));
	std::memcpy(&item[sizeof(date)], &tick, sizeof(tick));
	return item;
}

std::string retryBarItem(const char* exchange, const char* code, double close)
{
	std::string item(sizeof(BarCacheItem), '\0');
	WTSBarStruct bar;
	bar.date = 20240108;
	bar.time = TimeUtils::timeToMinBar(20240108, 930);
	bar.open = bar.close = close;
	std::memcpy(&item[0], exchange, std::strlen(exchange));
	std::memcpy(&item[16], code, std::strlen(code));
	std::memcpy(&item[48], &bar, sizeof(bar));
	return item;
}

std::string retryReadFile(const std::filesystem::path& path)
{
	std::ifstream input(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void seedRetryRoot(const std::filesystem::path& root, double price, bool malformedM5)
{
	std::filesystem::create_directories(root);
	auto tick = make_tick("RETRY", "rb01", 20240108, 20240108, 93000000, price, 1, 10, 2, 1);
	write_block_file((root / "cache_tick.dmb").string(), BLK_FLAG, BT_RT_Cache, 1, 1, 1,
		retryTickItem(tick));
	const std::string item = retryBarItem("RETRY", "rb01", price);
	write_block_file((root / "cache_m1.dmb").string(), BLK_FLAG, BT_RT_Cache, 1, 1, 1, item);
	std::string m5 = item;
	if (malformedM5)
		std::memset(&m5[16], 'X', 32);
	write_block_file((root / "cache_m5.dmb").string(), BLK_FLAG, BT_RT_Cache, 1, 1, 1, m5);
	write_block_file((root / "cache_d1.dmb").string(), BLK_FLAG, BT_RT_Cache, 1, 1, 1, item);
}

TEST_F(R4WriterADTest, FailedInitThenDifferentRootUsesOnlyNewRoot)
{
	const auto first = _root / "first";
	const auto second = _root / "second";
	seedRetryRoot(first, 101.0, true);
	seedRetryRoot(second, 202.0, false);
	auto* commodity = _bd->add_commodity("RETRY", "rb", ContractCategory::CC_Future);
	auto* contract = _bd->add_contract("RETRY", "rb01", commodity);
	WtDataWriterAD candidate;
	WTSVariant* firstCfg = make_writer_cfg(first.string(), 1000, true, false, true, true);
	WTSVariant* secondCfg = make_writer_cfg(second.string(), 1000, true, false, true, true);
	ASSERT_FALSE(candidate.init(firstCfg, _sink));
	const std::string originalM1 = retryReadFile(first / "cache_m1.dmb");
	ASSERT_TRUE(candidate.init(secondCfg, _sink));
	WTSTickData* current = candidate.getCurTick("rb01", "RETRY");
	ASSERT_NE(current, nullptr);
	EXPECT_DOUBLE_EQ(current->price(), 202.0);
	current->release();
	auto tick = make_tick("RETRY", "rb01", 20240108, 20240108, 93001000, 250.0, 1, 10, 2, 1);
	WTSTickData* update = tick_of(contract, tick);
	ASSERT_TRUE(candidate.writeTick(update, 0));
	update->release();
	EXPECT_EQ(retryReadFile(first / "cache_m1.dmb"), originalM1);
	std::ifstream newM1(second / "cache_m1.dmb", std::ios::binary);
	newM1.seekg(sizeof(RTBarCache) + 48);
	WTSBarStruct bar;
	newM1.read(reinterpret_cast<char*>(&bar), sizeof(bar));
	ASSERT_EQ(newM1.gcount(), static_cast<std::streamsize>(sizeof(bar)));
	EXPECT_DOUBLE_EQ(bar.close, 250.0);
	firstCfg->release();
	secondCfg->release();
	candidate.release();
}

TEST_F(R4WriterADTest, FailedReinitDoesNotDiscardPreviouslyMappedCache)
{
	const auto first = _root / "first";
	const auto second = _root / "second";
	seedRetryRoot(first, 101.0, false);
	seedRetryRoot(second, 202.0, false);
	{
		std::ofstream broken(second / "cache_m5.dmb", std::ios::binary | std::ios::trunc);
		broken << "short";
	}
	auto* commodity = _bd->add_commodity("RETRY", "rb", ContractCategory::CC_Future);
	_bd->add_contract("RETRY", "rb01", commodity);
	WtDataWriterAD candidate;
	WTSVariant* firstCfg = make_writer_cfg(first.string(), 1000, true, true, true, true);
	WTSVariant* secondCfg = make_writer_cfg(second.string(), 1000, true, true, true, true);
	ASSERT_TRUE(candidate.init(firstCfg, _sink));
	ASSERT_FALSE(candidate.init(secondCfg, _sink));
	WTSTickData* current = candidate.getCurTick("rb01", "RETRY");
	ASSERT_NE(current, nullptr);
	EXPECT_DOUBLE_EQ(current->price(), 101.0);
	current->release();
	firstCfg->release();
	secondCfg->release();
	candidate.release();
}

} // namespace
