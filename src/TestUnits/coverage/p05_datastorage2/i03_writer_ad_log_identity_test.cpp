/*
 * Iteration 03 (2026-11-04, package dt-writer)
 *  - I03-NEW-038: WtDataWriterAD log formatting must stay inside its 512-byte
 *    per-thread buffer even when a configured path is long.
 *  - BUG-L04-004 writer sub-items: an invalid storage identity is rejected before any
 *    cache/database/queue side effect with an observable diagnostic; an asynchronously
 *    accepted tick that fails the identity recheck is counted and logged, not dropped
 *    silently (async writeTick=true only means "accepted").
 * Build with -DS5_AD_BASELINE against the pre-fix writer for the red control: the new
 * counter accessor does not exist there, all other assertions are shared.
 */
#include "r4_ad_fixture.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

namespace {

class I03BlockingTickSink : public R4WriterSink
{
public:
	using R4WriterSink::R4WriterSink;
	void broadcastTick(WTSTickData* tick) override
	{
		{
			std::unique_lock<std::mutex> lock(_mutex);
			if (!_entered)
			{
				_entered = true;
				_condition.notify_all();
				_condition.wait(lock, [this] { return _proceed; });
			}
		}
		R4WriterSink::broadcastTick(tick);
	}
	bool waitForEntry()
	{
		std::unique_lock<std::mutex> lock(_mutex);
		return _condition.wait_for(lock, std::chrono::seconds(5), [this] { return _entered; });
	}
	void unblock()
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_proceed = true;
		_condition.notify_all();
	}
private:
	std::mutex _mutex;
	std::condition_variable _condition;
	bool _entered = false;
	bool _proceed = false;
};

// Captures every message with its length; R4WriterSink only keeps counts/last/all.
class I03LengthSink : public R4WriterSink
{
public:
	using R4WriterSink::R4WriterSink;
	void outputLog(WTSLogLevel ll, const char* message) override
	{
		R4WriterSink::outputLog(ll, message);
		if (message != NULL)
			_messages.emplace_back(message);
	}
	std::vector<std::string> _messages;
};

TEST_F(R4WriterADTest, I03LongPathFailureLogStaysInsideBuffer)
{
	if (::geteuid() == 0)
		GTEST_SKIP() << "root bypasses directory permissions";
	// ~620-byte directory; read-only so creating the cache files fails and the
	// failure diagnostics carry the full path.
	std::filesystem::path longDir = _root / std::string(200, 'a') / std::string(200, 'b') / std::string(200, 'c');
	std::filesystem::create_directories(longDir);
	ASSERT_EQ(::chmod(longDir.string().c_str(), 0555), 0);

	I03LengthSink sink(_bd);
	WtDataWriterAD writer;
	WTSVariant* cfg = make_writer_cfg(longDir.string() + "/", 1000, true, true, true, true);
	const bool initialized = writer.init(cfg, &sink);
	cfg->release();
	::chmod(longDir.string().c_str(), 0755);

	EXPECT_FALSE(initialized);
	ASSERT_FALSE(sink._messages.empty());
	bool sawLongDiagnostic = false;
	for (const std::string& message : sink._messages)
	{
		EXPECT_LT(message.size(), 512u) << "log message escaped the 512-byte buffer: " << message.size();
		if (message.size() >= 400 && message.find("...[truncated]") != std::string::npos)
			sawLongDiagnostic = true;
	}
	EXPECT_TRUE(sawLongDiagnostic);
	writer.release();
}

TEST_F(R4WriterADTest, I03InvalidStorageIdentityIsRejectedWithDiagnostic)
{
	init_writer_all_disabled();
	// WTSContractInfo::create already refuses identities that cannot fit the key, so an
	// invalid identity reaches the writer through the tick's fixed-size fields.
	WTSCommodityInfo* comm = _bd->add_commodity("EXI", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXI", "rb2301", comm);
	ASSERT_NE(ct, nullptr);

	// Tick code fills its 32-byte field with no terminator.
	WTSTickStruct s = make_tick("EXI", "rb2301", 20230103, 20230103, 93000000, 3900.0, 10, 100, 5, 1);
	std::memset(s.code, 'r', sizeof(s.code));
	WTSTickData* tick = tick_of(ct, s);
	EXPECT_FALSE(_writer->writeTick(tick, 0));
	tick->release();
	EXPECT_EQ(_sink->count_containing("Rejected tick before any side effect"), 1u);

	// Tick exchange fills its 16-byte field with no terminator.
	s = make_tick("EXI", "rb2301", 20230103, 20230103, 93000000, 3900.0, 10, 100, 5, 1);
	std::memset(s.exchg, 'E', sizeof(s.exchg));
	tick = tick_of(ct, s);
	EXPECT_FALSE(_writer->writeTick(tick, 0));
	tick->release();
	EXPECT_EQ(_sink->count_containing("Rejected tick before any side effect"), 2u);

	// Empty tick code is not a usable identity either.
	s = make_tick("EXI", "", 20230103, 20230103, 93000000, 3900.0, 10, 100, 5, 1);
	tick = tick_of(ct, s);
	EXPECT_FALSE(_writer->writeTick(tick, 0));
	tick->release();
	EXPECT_EQ(_sink->count_containing("Rejected tick before any side effect"), 3u);

	// No side effect reached the cache or the sink; the valid control still works.
	EXPECT_EQ(_sink->bcast_tick(), 0u);
	EXPECT_EQ(read_head(path_of("cache_tick.dmb"))._size, 0u);
	s = make_tick("EXI", "rb2301", 20230103, 20230103, 93000000, 3900.0, 10, 100, 5, 1);
	write_or_fail(tick_of(ct, s), 0);
	EXPECT_EQ(_sink->bcast_tick(), 1u);
	EXPECT_EQ(read_head(path_of("cache_tick.dmb"))._size, 1u);
}

TEST_F(R4WriterADTest, I03AsyncAcceptedTickFailingRecheckIsCountedAndLogged)
{
	I03BlockingTickSink sink(_bd);
	WtDataWriterAD writer;
	WTSVariant* cfg = make_writer_cfg(_root.string(), 1000, true, true, true, true, true);
	ASSERT_TRUE(writer.init(cfg, &sink));
	cfg->release();
	WTSCommodityInfo* comm = _bd->add_commodity("EXA", "rb", ContractCategory::CC_Future);
	WTSContractInfo* firstCt = _bd->add_contract("EXA", "rb01", comm);
	WTSContractInfo* secondCt = _bd->add_contract("EXA", "rb02", comm);

	WTSTickData* first = tick_of(firstCt, make_tick("EXA", "rb01", 20230103, 20230103, 93000000, 100, 1, 1, 1, 1));
	ASSERT_TRUE(writer.writeTick(first, 0));
	first->release();
	if (!sink.waitForEntry())
	{
		sink.unblock();
		writer.release();
		FAIL() << "worker did not reach the broadcast barrier";
	}

	WTSTickData* second = tick_of(secondCt, make_tick("EXA", "rb02", 20230103, 20230103, 93000000, 101, 1, 1, 1, 1));
	const bool accepted = writer.writeTick(second, 0);
	// The worker is blocked in the first task; the mutation happens-before unblock.
	std::memset(second->getTickStruct().code, 'X', sizeof(second->getTickStruct().code));
	second->release();
	sink.unblock();
	writer.release();

	EXPECT_TRUE(accepted) << "async writeTick reports acceptance only";
	EXPECT_EQ(sink.bcast_tick(), 1u);
	EXPECT_EQ(sink.count_containing("Dropped accepted tick"), 1u);
#if !defined(S5_AD_BASELINE)
	EXPECT_EQ(writer.tickRejectedAfterAcceptance(), 1u);
	EXPECT_EQ(writer.tickPersistFailures(), 0u);
#endif
}

} // namespace
