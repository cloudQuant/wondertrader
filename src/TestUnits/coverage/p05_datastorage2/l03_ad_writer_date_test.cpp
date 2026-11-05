#include "r4_ad_fixture.h"
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace {

// An unrepresentable adjusted action date must be rejected before an async
// task is accepted or any cache, database, broadcast or caller tick changes.
TEST_F(R4WriterADTest, L03InvalidSessionDateRejectedBeforeSynchronousEffects)
{
	_bd->session()->setOffsetMins(1440);
	init_writer(make_writer_cfg(_root.string(), 1000, false, false, false, false));
	WTSCommodityInfo* comm = _bd->add_commodity("L03", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("L03", "rb01", comm);
	WTSTickStruct bad = make_tick("L03", "rb01", 99991231, 99991231, 93000000,
		100, 1, 1, 1, 1, 7, 10);
	WTSTickData* tick = tick_of(ct, bad);
	ASSERT_NE(tick, nullptr);
	EXPECT_FALSE(_writer->writeTick(tick, 1));
	EXPECT_EQ(tick->retainCount(), 1u);
	EXPECT_EQ(std::memcmp(&tick->getTickStruct(), &bad, sizeof(bad)), 0);
	tick->release();
	EXPECT_EQ(_sink->bcast_tick(), 0u);
	EXPECT_EQ(read_head(path_of("cache_tick.dmb"))._size, 0u);
	EXPECT_EQ(read_head(path_of("cache_m1.dmb"))._size, 0u);
	EXPECT_FALSE(std::filesystem::exists(_root / "ticks"));
	WTSTickData* unexpected = _writer->getCurTick("rb01", "L03");
	EXPECT_EQ(unexpected, nullptr);
	if (unexpected) unexpected->release();

	WTSTickStruct good = make_tick("L03", "rb01", 20240109, 20240108, 93000000,
		101, 1, 1, 1, 1, 7, 10);
	write_or_fail(tick_of(ct, good), 0);
	EXPECT_EQ(_sink->bcast_tick(), 1u);
	WTSTickData* stored = _writer->getCurTick("rb01", "L03");
	ASSERT_NE(stored, nullptr);
	EXPECT_DOUBLE_EQ(stored->price(), good.price);
	stored->release();
}

TEST_F(R4WriterADTest, L03InvalidSessionDateRejectedBeforeAsyncAdmission)
{
	_bd->session()->setOffsetMins(1440);
	init_writer_all_disabled(1000, true);
	WTSCommodityInfo* comm = _bd->add_commodity("L03", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("L03", "rb01", comm);
	WTSTickStruct bad = make_tick("L03", "rb01", 99991231, 99991231, 93000000,
		100, 1, 1, 1, 1, 7, 10);
	WTSTickData* tick = tick_of(ct, bad);
	ASSERT_NE(tick, nullptr);
	EXPECT_FALSE(_writer->writeTick(tick, 0));
	EXPECT_EQ(tick->retainCount(), 1u);
	tick->release();
	_writer->release(); // Joins and drains any task the implementation accepted.
	EXPECT_EQ(_sink->bcast_tick(), 0u);
	EXPECT_EQ(read_head(path_of("cache_tick.dmb"))._size, 0u);
	WTSTickData* unexpected = _writer->getCurTick("rb01", "L03");
	EXPECT_EQ(unexpected, nullptr);
	if (unexpected) unexpected->release();
}

class L03MissingSessionManager : public R4BaseDataMgr
{
public:
	WTSSessionInfo* getSessionByCode(const char*, const char*) override { return nullptr; }
};

TEST_F(R4WriterADTest, L03MissingOrInvalidActionDateIsRejected)
{
	init_writer_all_disabled();
	WTSCommodityInfo* comm = _bd->add_commodity("L03", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("L03", "rb01", comm);
	for (uint32_t date : {0u, 20230229u})
	{
		WTSTickStruct data = make_tick("L03", "rb01", 20230301, date, 93000000, 100, 1, 1, 1, 1);
		WTSTickData* tick = tick_of(ct, data);
		EXPECT_FALSE(_writer->writeTick(tick, 0));
		EXPECT_EQ(tick->retainCount(), 1u);
		tick->release();
	}
	EXPECT_EQ(_sink->bcast_tick(), 0u);
	EXPECT_EQ(read_head(path_of("cache_tick.dmb"))._size, 0u);
}

TEST_F(R4WriterADTest, L03MissingSessionIsRejected)
{
	L03MissingSessionManager noSession;
	R4WriterSink sink(&noSession);
	WtDataWriterAD other;
	WTSVariant* cfg = make_writer_cfg((_root / "no-session").string(), 1000, true, true, true, true);
	ASSERT_TRUE(other.init(cfg, &sink));
	cfg->release();
	WTSCommodityInfo* otherComm = noSession.add_commodity("L03", "rb", ContractCategory::CC_Future);
	WTSContractInfo* otherCt = noSession.add_contract("L03", "rb01", otherComm);
	WTSTickStruct data = make_tick("L03", "rb01", 20240109, 20240108, 93000000, 100, 1, 1, 1, 1);
	WTSTickData* tick = tick_of(otherCt, data);
	EXPECT_FALSE(other.writeTick(tick, 0));
	tick->release();
	EXPECT_EQ(sink.bcast_tick(), 0u);
	other.release();
}

class L03BlockingSink : public R4WriterSink
{
public:
	using R4WriterSink::R4WriterSink;
	bool canSessionReceive(const char* sid) override
	{
		std::unique_lock<std::mutex> lock(mutex);
		if (!entered)
		{
			entered = true;
			condition.notify_all();
			condition.wait(lock, [this] { return proceed; });
		}
		lock.unlock();
		return R4WriterSink::canSessionReceive(sid);
	}
	bool waitForEntry()
	{
		std::unique_lock<std::mutex> lock(mutex);
		return condition.wait_for(lock, std::chrono::seconds(5), [this] { return entered; });
	}
	void unblock()
	{
		std::lock_guard<std::mutex> lock(mutex);
		proceed = true;
		condition.notify_all();
	}
private:
	std::mutex mutex;
	std::condition_variable condition;
	bool entered = false;
	bool proceed = false;
};

TEST_F(R4WriterADTest, L03QueuedTickIsRecheckedBeforeProcessing)
{
	_bd->session()->setOffsetMins(1440);
	L03BlockingSink sink(_bd);
	WtDataWriterAD writer;
	WTSVariant* cfg = make_writer_cfg(_root.string(), 1000, true, true, true, true, true);
	ASSERT_TRUE(writer.init(cfg, &sink));
	cfg->release();
	WTSCommodityInfo* comm = _bd->add_commodity("L03", "rb", ContractCategory::CC_Future);
	WTSContractInfo* firstCt = _bd->add_contract("L03", "rb01", comm);
	WTSContractInfo* secondCt = _bd->add_contract("L03", "rb02", comm);
	WTSTickStruct good = make_tick("L03", "rb01", 20240109, 20240108, 93000000, 100, 1, 1, 1, 1);
	WTSTickData* first = tick_of(firstCt, good);
	const bool firstAccepted = writer.writeTick(first, 0);
	first->release();
	if (!firstAccepted)
	{
		writer.release();
		FAIL() << "first control tick was not accepted";
	}
	if (!sink.waitForEntry())
	{
		sink.unblock();
		writer.release();
		FAIL() << "worker did not enter the barrier";
	}
	WTSTickStruct secondData = make_tick("L03", "rb02", 20240109, 20240108, 93000000, 101, 1, 1, 1, 1);
	WTSTickData* second = tick_of(secondCt, secondData);
	const bool accepted = writer.writeTick(second, 0);
	// Worker is blocked in the first task; the mutation happens-before unblock.
	second->getTickStruct().action_date = 99991231;
	second->getTickStruct().trading_date = 99991231;
	second->release();
	sink.unblock();
	writer.release();
	EXPECT_TRUE(accepted);
	EXPECT_EQ(sink.bcast_tick(), 1u);
	WTSTickData* found = writer.getCurTick("rb02", "L03");
	EXPECT_EQ(found, nullptr);
	if (found) found->release();
}

} // namespace
