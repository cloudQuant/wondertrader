// Manual known-red probes. Fake dumper retention is deliberately stronger than
// the undocumented callback contract and does not establish plugin behavior.
#include "r4_ad_fixture.h"
#include <thread>

namespace {
class FailureDumper : public IHisDataDumper
{
public:
	bool dumpHisBars(const char*, const char*, WTSBarStruct* bar, uint32_t count) override
	{
		++attempts;
		if (attempts == 1) return false;
		if (count) delivered.push_back(*bar);
		return true;
	}
	bool dumpHisTicks(const char*, uint32_t, WTSTickStruct*, uint32_t) override { return true; }
	int attempts = 0;
	std::vector<WTSBarStruct> delivered;
};

class RetainingDumper : public IHisDataDumper
{
public:
	bool dumpHisBars(const char*, const char*, WTSBarStruct* bars, uint32_t count) override
	{
		if (count) retained = bars;
		return true;
	}
	bool dumpHisTicks(const char*, uint32_t, WTSTickStruct*, uint32_t) override { return true; }
	WTSBarStruct* retained = nullptr;
};

TEST_F(R4WriterADTest, ManualDumperFailureDoesNotLosePreviousBar)
{
	init_writer(make_writer_cfg(_root.string(), 1000, true, false, true, true));
	auto* commodity = _bd->add_commodity("FAIL", "rb", ContractCategory::CC_Future);
	auto* contract = _bd->add_contract("FAIL", "rb01", commodity);
	FailureDumper dumper;
	_writer->add_ext_dumper("failing", &dumper);
	for (unsigned minute : {930u, 931u, 932u})
	{
		auto tick = make_tick("FAIL", "rb01", 20240108, 20240108, minute * 100000, 100.0 + minute - 930, 1, 10, 2, 1);
		write_or_fail(tick_of(contract, tick), 0);
	}
	ASSERT_GE(dumper.attempts, 2);
	ASSERT_FALSE(dumper.delivered.empty());
	EXPECT_DOUBLE_EQ(dumper.delivered.front().close, 100.0)
		<< "the bar rejected by the dumper must not disappear after the next bar";
}

TEST_F(R4WriterADTest, ManualDumperRetainsBorrowedBarAfterReturn)
{
	init_writer(make_writer_cfg(_root.string(), 1000, true, false, true, true));
	auto* commodity = _bd->add_commodity("RETAIN", "rb", ContractCategory::CC_Future);
	auto* contract = _bd->add_contract("RETAIN", "rb01", commodity);
	RetainingDumper dumper;
	_writer->add_ext_dumper("retaining", &dumper);
	for (unsigned minute : {930u, 931u})
	{
		auto tick = make_tick("RETAIN", "rb01", 20240108, 20240108, minute * 100000, 100.0 + minute - 930, 1, 10, 2, 1);
		write_or_fail(tick_of(contract, tick), 0);
	}
	ASSERT_NE(dumper.retained, nullptr);
	double retainedPrice = 0;
	std::thread reader([&] { retainedPrice = dumper.retained->close; });
	reader.join();
	EXPECT_DOUBLE_EQ(retainedPrice, 100.0);
}

TEST_F(R4WriterADTest, ManualBarDirectoryFailureReportsAndCanRetry)
{
	init_writer(make_writer_cfg(_root.string(), 1000, true, false, true, true));
	auto* commodity = _bd->add_commodity("BADPATH", "rb", ContractCategory::CC_Future);
	auto* contract = _bd->add_contract("BADPATH", "rb01", commodity);
	FailureDumper dumper;
	_writer->add_ext_dumper("failing", &dumper);
	auto first = make_tick("BADPATH", "rb01", 20240108, 20240108, 93000000, 100, 1, 10, 2, 1);
	write_or_fail(tick_of(contract, first), 0);
	std::filesystem::create_directories(_root / "min1");
	{ std::ofstream blocker(_root / "min1" / "BADPATH"); blocker << "file"; }
	auto next = make_tick("BADPATH", "rb01", 20240108, 20240108, 93100000, 101, 1, 10, 2, 1);
	WTSTickData* item = tick_of(contract, next);
	bool accepted = true;
	bool threw = false;
	try { accepted = _writer->writeTick(item, 0); }
	catch (...) { threw = true; }
	item->release();
	EXPECT_FALSE(threw);
	EXPECT_FALSE(accepted);
	WTSTickData* old = _writer->getCurTick("rb01", "BADPATH");
	ASSERT_NE(old, nullptr);
	EXPECT_DOUBLE_EQ(old->price(), 100.0);
	old->release();
	std::filesystem::remove(_root / "min1" / "BADPATH");
	WTSTickData* retry = tick_of(contract, next);
	EXPECT_TRUE(_writer->writeTick(retry, 0));
	retry->release();
	EXPECT_GE(dumper.attempts, 1) << "the same tick must retry the failed bar output";
}
} // namespace
