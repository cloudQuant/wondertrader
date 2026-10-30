#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include <WtCore/DiffOrderOwnershipV1.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace wtp
{
struct DiffOrderOwnershipJournalV1TestAccess
{
	static void failBeforeWrite(DiffOrderOwnershipJournalV1& journal)
	{
		std::lock_guard<std::mutex> lock(journal._mutex);
		journal._test_fault_point = DiffOrderOwnershipJournalV1::TestFaultPoint::BeforeWrite;
	}

	static void failBeforeFlush(DiffOrderOwnershipJournalV1& journal)
	{
		std::lock_guard<std::mutex> lock(journal._mutex);
		journal._test_fault_point = DiffOrderOwnershipJournalV1::TestFaultPoint::BeforeFlush;
	}
};
}

namespace
{
namespace fs = std::filesystem;
using Journal = wtp::DiffOrderOwnershipJournalV1;

uint64_t processId()
{
#ifdef _WIN32
	return static_cast<uint64_t>(GetCurrentProcessId());
#else
	return static_cast<uint64_t>(getpid());
#endif
}

class TemporaryWal
{
public:
	TemporaryWal()
	{
		static std::atomic<uint64_t> serial{ 0 };
		_path = fs::temp_directory_path() /
			("wondertrader_diff_ownership_" + std::to_string(processId()) + "_" +
			std::to_string(++serial) + ".wal");
	}
	~TemporaryWal()
	{
		std::error_code ignored;
		fs::remove(_path, ignored);
	}
	const std::string string() const { return _path.u8string(); }
	const fs::path& path() const { return _path; }

private:
	fs::path _path;
};

TEST(DiffOrderOwnershipJournalV1, PersistsOwnerLedgerAndActiveOrdersAcrossRestart)
{
	TemporaryWal wal;
	Journal journal;
	ASSERT_EQ(Journal::Status::Ok, journal.open(wal.string(), "account-A"));
	ASSERT_EQ(Journal::Status::Ok, journal.advanceLocalOrderIdHighWater(0,
		"unit-test:isolated-empty-history"));
	ASSERT_EQ(Journal::Status::Ok, journal.setTarget("diff-A", "EX.TEST", 10.0, 6.0));
	ASSERT_EQ(Journal::Status::Ok, journal.setBaseline("diff-A", "EX.TEST", 4.0,
		10.0, 6.0, 20261030));
	uint32_t localIdA = 0;
	ASSERT_EQ(Journal::Status::Ok, journal.prepareOrderIntent("diff-A", "intent-A-1",
		"EX.TEST", 4.0, localIdA));
	EXPECT_EQ(1u, localIdA);
	ASSERT_EQ(Journal::Status::Ok, journal.bindOrder("intent-A-1", localIdA, "broker-A-1"));
	ASSERT_EQ(Journal::Status::Ok, journal.recordOrderProgress("intent-A-1", 1, 0.0, 4.0, false));
	uint32_t localIdB = 0;
	ASSERT_EQ(Journal::Status::Ok, journal.prepareOrderIntent("diff-B", "intent-B-1",
		"EX.TEST", -2.0, localIdB));
	EXPECT_EQ(2u, localIdB);
	ASSERT_EQ(Journal::Status::Ok, journal.bindOrder("intent-B-1", localIdB, "broker-B-1"));
	ASSERT_EQ(Journal::Status::Ok, journal.applyTrade("trade-A-1", "intent-A-1",
		"EX.TEST", -1.0));
	ASSERT_EQ(Journal::Status::Duplicate, journal.applyTrade("trade-A-1", "intent-A-1",
		"EX.TEST", -1.0));

	std::string owner;
	std::string intentId;
	ASSERT_EQ(Journal::Status::Ok, journal.ownerForLocalOrder(localIdA, owner, &intentId));
	EXPECT_EQ("diff-A", owner);
	EXPECT_EQ("intent-A-1", intentId);
	EXPECT_EQ(Journal::Status::Ok, journal.checkCancelOwner(localIdA, "diff-A"));
	EXPECT_EQ(Journal::Status::Conflict, journal.checkCancelOwner(localIdA, "diff-B"));
	EXPECT_EQ(Journal::Status::Ok, journal.checkCancelOwner(localIdB, "diff-B"));

	std::vector<Journal::OrderIntentState> active;
	ASSERT_EQ(Journal::Status::Ok, journal.activeOrders("diff-A", "EX.TEST", active));
	ASSERT_EQ(1u, active.size());
	EXPECT_EQ(localIdA, active.front().local_order_id);
	EXPECT_DOUBLE_EQ(1.0, active.front().filled_quantity);
	EXPECT_DOUBLE_EQ(3.0, active.front().remaining_quantity);
	double undone = 0.0;
	ASSERT_EQ(Journal::Status::Ok, journal.undoneQuantity("diff-A", "EX.TEST", undone));
	EXPECT_DOUBLE_EQ(3.0, undone);
	ASSERT_EQ(Journal::Status::Ok, journal.undoneQuantity("diff-B", "EX.TEST", undone));
	EXPECT_DOUBLE_EQ(-2.0, undone);
	ASSERT_EQ(Journal::Status::Ok, journal.recordOrderProgress("intent-B-1", 4, 0.0, 2.0, true));
	EXPECT_EQ(Journal::Status::Duplicate,
		journal.recordOrderProgress("intent-B-1", 4, 0.0, 2.0, true));
	EXPECT_DOUBLE_EQ(2.0, journal.snapshot().intents.at("intent-B-1").reported_remaining_quantity);
	EXPECT_DOUBLE_EQ(0.0, journal.snapshot().intents.at("intent-B-1").remaining_quantity);
	EXPECT_EQ(Journal::Status::Conflict, journal.checkCancelOwner(localIdB, "diff-B"));
	ASSERT_EQ(Journal::Status::Ok, journal.activeOrders("diff-B", "EX.TEST", active));
	EXPECT_TRUE(active.empty());

	const uint64_t committedSequence = journal.snapshot().last_sequence;
	journal.close();

	Journal recovered;
	ASSERT_EQ(Journal::Status::Ok, recovered.open(wal.string(), "account-A"));
	const Journal::Snapshot state = recovered.snapshot();
	EXPECT_EQ(committedSequence, state.last_sequence);
	EXPECT_EQ("account-A", state.account_id);
	const Journal::OwnerCodeKey key{ "diff-A", "EX.TEST" };
	ASSERT_NE(state.positions.end(), state.positions.find(key));
	EXPECT_TRUE(state.positions.at(key).has_baseline);
	EXPECT_DOUBLE_EQ(10.0, state.positions.at(key).target);
	EXPECT_DOUBLE_EQ(4.0, state.positions.at(key).baseline_account_position);
	EXPECT_DOUBLE_EQ(5.0, state.positions.at(key).diff);
	ASSERT_NE(state.intents.end(), state.intents.find("intent-A-1"));
	EXPECT_DOUBLE_EQ(1.0, state.intents.at("intent-A-1").filled_quantity);
	EXPECT_DOUBLE_EQ(3.0, state.intents.at("intent-A-1").remaining_quantity);
	EXPECT_TRUE(state.intents.at("intent-B-1").terminal);
	EXPECT_DOUBLE_EQ(2.0, state.intents.at("intent-B-1").reported_remaining_quantity);
	EXPECT_DOUBLE_EQ(0.0, state.intents.at("intent-B-1").remaining_quantity);
	EXPECT_EQ(Journal::Status::Duplicate,
		recovered.recordOrderProgress("intent-B-1", 4, 0.0, 2.0, true));
}

TEST(DiffOrderOwnershipJournalV1, TargetBaselineAndTradeReplayInOneSequence)
{
	TemporaryWal wal;
	Journal journal;
	ASSERT_EQ(Journal::Status::Ok, journal.open(wal.string(), "account-B"));
	ASSERT_EQ(Journal::Status::Ok, journal.advanceLocalOrderIdHighWater(0,
		"unit-test:isolated-empty-history"));
	ASSERT_EQ(Journal::Status::Ok, journal.setBaseline("owner", "EX.X", 3.0, 7.0, 4.0, 20261030));
	uint32_t localId = 0;
	ASSERT_EQ(Journal::Status::Ok,
		journal.prepareOrderIntent("owner", "buy-1", "EX.X", 2.0, localId));
	EXPECT_EQ(1u, localId);
	ASSERT_EQ(Journal::Status::Ok, journal.applyTrade("day1-trade-9", "buy-1", "EX.X", -1.0));
	ASSERT_EQ(Journal::Status::Ok, journal.setTarget("owner", "EX.X", 8.0, 2.0));
	const uint64_t sequence = journal.snapshot().last_sequence;
	journal.close();

	Journal recovered;
	ASSERT_EQ(Journal::Status::Ok, recovered.open(wal.string(), "account-B"));
	const auto snapshot = recovered.snapshot();
	const Journal::PositionState& position = snapshot.positions.at({ "owner", "EX.X" });
	EXPECT_EQ(sequence, snapshot.last_sequence);
	EXPECT_DOUBLE_EQ(3.0, position.baseline_account_position);
	EXPECT_DOUBLE_EQ(8.0, position.target);
	EXPECT_DOUBLE_EQ(2.0, position.diff);
	EXPECT_EQ(Journal::Status::Duplicate,
		recovered.applyTrade("day1-trade-9", "buy-1", "EX.X", -1.0));
	EXPECT_DOUBLE_EQ(2.0, recovered.snapshot().positions.at({ "owner", "EX.X" }).diff);
}

TEST(DiffOrderOwnershipJournalV1, LocalOrderHighWaterAndUnresolvedIntentSurviveRestart)
{
	TemporaryWal wal;
	Journal journal;
	ASSERT_EQ(Journal::Status::Ok, journal.open(wal.string(), "account-localid"));
	ASSERT_EQ(Journal::Status::Ok, journal.advanceLocalOrderIdHighWater(300,
		"unit-test:complete-localid-inventory-through-300"));
	uint32_t localId = 0;
	ASSERT_EQ(Journal::Status::Ok,
		journal.prepareOrderIntent("owner", "intent-1", "EX.X", 1.0, localId));
	EXPECT_EQ(301u, localId);
	const uint64_t sequence = journal.snapshot().last_sequence;
	uint32_t repeatedId = 0;
	EXPECT_EQ(Journal::Status::Duplicate,
		journal.prepareOrderIntent("owner", "intent-1", "EX.X", 1.0, repeatedId));
	EXPECT_EQ(localId, repeatedId);
	EXPECT_EQ(sequence, journal.snapshot().last_sequence);
	journal.close();

	Journal recovered;
	ASSERT_EQ(Journal::Status::Ok, recovered.open(wal.string(), "account-localid"));
	EXPECT_EQ(301u, recovered.snapshot().local_order_id_high_water);
	EXPECT_TRUE(recovered.snapshot().local_order_id_floor_established);
	EXPECT_EQ("unit-test:complete-localid-inventory-through-300",
		recovered.snapshot().local_order_id_floor_evidence);
	repeatedId = 0;
	EXPECT_EQ(Journal::Status::Duplicate,
		recovered.prepareOrderIntent("owner", "intent-1", "EX.X", 1.0, repeatedId));
	EXPECT_EQ(301u, repeatedId);
	uint32_t nextId = 0;
	ASSERT_EQ(Journal::Status::Ok,
		recovered.prepareOrderIntent("owner", "intent-2", "EX.X", 1.0, nextId));
	EXPECT_EQ(302u, nextId);
}

TEST(DiffOrderOwnershipJournalV1, RequiresDurableLegacyLocalIdFloorBeforeReservingOrder)
{
	TemporaryWal wal;
	Journal journal;
	ASSERT_EQ(Journal::Status::Ok, journal.open(wal.string(), "account-floor-required"));
	uint32_t localId = 99;
	const uint64_t headerSequence = journal.snapshot().last_sequence;
	EXPECT_EQ(Journal::Status::LocalOrderIdFloorRequired,
		journal.prepareOrderIntent("owner", "intent-before-floor", "EX.X", 1.0, localId));
	EXPECT_EQ(0u, localId);
	EXPECT_EQ(headerSequence, journal.snapshot().last_sequence);
	EXPECT_TRUE(journal.snapshot().intents.empty());
	EXPECT_EQ(Journal::Status::InvalidArgument,
		journal.advanceLocalOrderIdHighWater(0, std::string()));

	ASSERT_EQ(Journal::Status::Ok, journal.advanceLocalOrderIdHighWater(0,
		"unit-test:isolated-empty-history"));
	EXPECT_TRUE(journal.snapshot().local_order_id_floor_established);
	EXPECT_EQ("unit-test:isolated-empty-history",
		journal.snapshot().local_order_id_floor_evidence);
	ASSERT_EQ(Journal::Status::Ok,
		journal.prepareOrderIntent("owner", "intent-after-floor", "EX.X", 1.0, localId));
	EXPECT_EQ(1u, localId);
	journal.close();

	Journal recovered;
	ASSERT_EQ(Journal::Status::Ok, recovered.open(wal.string(), "account-floor-required"));
	EXPECT_TRUE(recovered.snapshot().local_order_id_floor_established);
	EXPECT_EQ("unit-test:isolated-empty-history",
		recovered.snapshot().local_order_id_floor_evidence);
	EXPECT_EQ(1u, recovered.snapshot().local_order_id_high_water);
}

TEST(DiffOrderOwnershipJournalV1, RepeatedRejectAndPartialCancelProgressStayIdempotentAfterReplay)
{
	TemporaryWal wal;
	Journal journal;
	ASSERT_EQ(Journal::Status::Ok, journal.open(wal.string(), "account-terminal"));
	ASSERT_EQ(Journal::Status::Ok, journal.advanceLocalOrderIdHighWater(0,
		"unit-test:isolated-empty-history"));
	uint32_t cancelId = 0;
	ASSERT_EQ(Journal::Status::Ok,
		journal.prepareOrderIntent("owner", "partial-cancel", "EX.X", 5.0, cancelId));
	ASSERT_EQ(Journal::Status::Ok, journal.bindOrder("partial-cancel", cancelId, "broker-cancel"));
	ASSERT_EQ(Journal::Status::Ok,
		journal.recordOrderProgress("partial-cancel", 1, 2.0, 3.0, false));
	ASSERT_EQ(Journal::Status::Ok,
		journal.recordOrderProgress("partial-cancel", 4, 2.0, 3.0, true));
	EXPECT_EQ(Journal::Status::Duplicate,
		journal.recordOrderProgress("partial-cancel", 4, 2.0, 3.0, true));
	EXPECT_DOUBLE_EQ(3.0,
		journal.snapshot().intents.at("partial-cancel").reported_remaining_quantity);
	EXPECT_DOUBLE_EQ(0.0,
		journal.snapshot().intents.at("partial-cancel").remaining_quantity);

	uint32_t rejectedId = 0;
	ASSERT_EQ(Journal::Status::Ok,
		journal.prepareOrderIntent("owner", "rejected", "EX.X", -2.0, rejectedId));
	ASSERT_EQ(Journal::Status::Ok,
		journal.recordOrderProgress("rejected", 5, 0.0, 2.0, true));
	EXPECT_EQ(Journal::Status::Duplicate,
		journal.recordOrderProgress("rejected", 5, 0.0, 2.0, true));
	journal.close();

	Journal recovered;
	ASSERT_EQ(Journal::Status::Ok, recovered.open(wal.string(), "account-terminal"));
	EXPECT_DOUBLE_EQ(3.0,
		recovered.snapshot().intents.at("partial-cancel").reported_remaining_quantity);
	EXPECT_DOUBLE_EQ(0.0,
		recovered.snapshot().intents.at("partial-cancel").remaining_quantity);
	EXPECT_EQ(Journal::Status::Duplicate,
		recovered.recordOrderProgress("partial-cancel", 4, 2.0, 3.0, true));
	EXPECT_EQ(Journal::Status::Duplicate,
		recovered.recordOrderProgress("rejected", 5, 0.0, 2.0, true));
	EXPECT_TRUE(recovered.isHealthy());
}

TEST(DiffOrderOwnershipJournalV1, LateTradeAdvancesSameTerminalProgressOnceAcrossReplay)
{
	TemporaryWal wal;
	Journal journal;
	ASSERT_EQ(Journal::Status::Ok, journal.open(wal.string(), "account-late-trade"));
	ASSERT_EQ(Journal::Status::Ok, journal.advanceLocalOrderIdHighWater(0,
		"unit-test:isolated-empty-history"));
	uint32_t localId = 0;
	ASSERT_EQ(Journal::Status::Ok,
		journal.prepareOrderIntent("owner", "late-fill", "EX.X", 5.0, localId));
	ASSERT_EQ(Journal::Status::Ok, journal.bindOrder("late-fill", localId, "broker-late"));
	ASSERT_EQ(Journal::Status::Ok,
		journal.recordOrderProgress("late-fill", 1, 2.0, 3.0, false));
	ASSERT_EQ(Journal::Status::Ok,
		journal.recordOrderProgress("late-fill", 4, 2.0, 3.0, true));
	ASSERT_EQ(Journal::Status::Ok,
		journal.applyTrade("late-trade-1", "late-fill", "EX.X", -1.0));
	EXPECT_DOUBLE_EQ(-1.0, journal.snapshot().positions.at({ "owner", "EX.X" }).diff);
	EXPECT_DOUBLE_EQ(2.0, journal.snapshot().intents.at("late-fill").filled_quantity);
	EXPECT_DOUBLE_EQ(1.0, journal.snapshot().intents.at("late-fill").trade_filled_quantity);
	EXPECT_DOUBLE_EQ(0.0, journal.snapshot().intents.at("late-fill").remaining_quantity);
	EXPECT_DOUBLE_EQ(3.0,
		journal.snapshot().intents.at("late-fill").reported_remaining_quantity);

	ASSERT_EQ(Journal::Status::Ok,
		journal.recordOrderProgress("late-fill", 4, 3.0, 2.0, true));
	const uint64_t updatedSequence = journal.snapshot().last_sequence;
	const Journal::OrderIntentState updated = journal.snapshot().intents.at("late-fill");
	EXPECT_DOUBLE_EQ(3.0, updated.filled_quantity);
	EXPECT_DOUBLE_EQ(0.0, updated.remaining_quantity);
	EXPECT_DOUBLE_EQ(2.0, updated.reported_remaining_quantity);
	EXPECT_EQ(Journal::Status::Duplicate,
		journal.recordOrderProgress("late-fill", 4, 3.0, 2.0, true));
	EXPECT_EQ(Journal::Status::Duplicate,
		journal.applyTrade("late-trade-1", "late-fill", "EX.X", -1.0));
	EXPECT_EQ(updatedSequence, journal.snapshot().last_sequence);
	EXPECT_DOUBLE_EQ(-1.0, journal.snapshot().positions.at({ "owner", "EX.X" }).diff);
	journal.close();

	Journal recovered;
	ASSERT_EQ(Journal::Status::Ok, recovered.open(wal.string(), "account-late-trade"));
	EXPECT_TRUE(recovered.isHealthy());
	const Journal::OrderIntentState replayed = recovered.snapshot().intents.at("late-fill");
	EXPECT_TRUE(replayed.terminal);
	EXPECT_EQ(4u, replayed.terminal_state);
	EXPECT_DOUBLE_EQ(3.0, replayed.filled_quantity);
	EXPECT_DOUBLE_EQ(1.0, replayed.trade_filled_quantity);
	EXPECT_DOUBLE_EQ(0.0, replayed.remaining_quantity);
	EXPECT_DOUBLE_EQ(2.0, replayed.reported_remaining_quantity);
	EXPECT_EQ(Journal::Status::Duplicate,
		recovered.recordOrderProgress("late-fill", 4, 3.0, 2.0, true));
	EXPECT_EQ(Journal::Status::Duplicate,
		recovered.applyTrade("late-trade-1", "late-fill", "EX.X", -1.0));
	EXPECT_DOUBLE_EQ(-1.0, recovered.snapshot().positions.at({ "owner", "EX.X" }).diff);
}

TEST(DiffOrderOwnershipJournalV1, UInt32LocalOrderIdExhaustionFailsWithoutWrapping)
{
	TemporaryWal wal;
	Journal journal;
	ASSERT_EQ(Journal::Status::Ok, journal.open(wal.string(), "account-exhaust"));
	ASSERT_EQ(Journal::Status::Ok, journal.advanceLocalOrderIdHighWater(
		std::numeric_limits<uint32_t>::max() - 1,
		"unit-test:complete-localid-inventory-through-max-minus-one"));
	uint32_t localId = 99;
	EXPECT_EQ(Journal::Status::SequenceExhausted,
		journal.prepareOrderIntent("owner", "last-intent", "EX.X", 1.0, localId));
	EXPECT_EQ(0u, localId);
	EXPECT_EQ(Journal::Status::InvalidArgument,
		journal.advanceLocalOrderIdHighWater(std::numeric_limits<uint32_t>::max(),
			"unit-test:invalid-sentinel"));
	journal.close();

	Journal recovered;
	ASSERT_EQ(Journal::Status::Ok, recovered.open(wal.string(), "account-exhaust"));
	EXPECT_EQ(std::numeric_limits<uint32_t>::max() - 1,
		recovered.snapshot().local_order_id_high_water);
	EXPECT_EQ(Journal::Status::SequenceExhausted,
		recovered.prepareOrderIntent("owner", "last-intent", "EX.X", 1.0, localId));
	EXPECT_EQ(0u, localId);
}

TEST(DiffOrderOwnershipJournalV1, RejectsUnknownOrdersAndConflictingTradeKeysFailClosed)
{
	TemporaryWal wal;
	Journal journal;
	ASSERT_EQ(Journal::Status::Ok, journal.open(wal.string(), "account-C"));
	ASSERT_EQ(Journal::Status::Ok, journal.advanceLocalOrderIdHighWater(0,
		"unit-test:isolated-empty-history"));
	ASSERT_EQ(Journal::Status::UnknownIntent,
		journal.applyTrade("legacy-trade", "not-recorded", "EX.X", -1.0));
	EXPECT_TRUE(journal.isHealthy());
	uint32_t localId = 0;
	ASSERT_EQ(Journal::Status::Ok,
		journal.prepareOrderIntent("owner", "intent", "EX.X", 2.0, localId));
	ASSERT_EQ(Journal::Status::Ok, journal.applyTrade("same-key", "intent", "EX.X", -1.0));
	EXPECT_EQ(Journal::Status::Conflict,
		journal.applyTrade("same-key", "intent", "EX.X", -0.5));
	EXPECT_FALSE(journal.isHealthy());
	EXPECT_EQ(Journal::Status::StorageFailed,
		journal.setTarget("owner", "EX.X", 2.0, 1.0));
}

TEST(DiffOrderOwnershipJournalV1, TruncatedAndChecksumDamagedWalFailClosed)
{
	TemporaryWal empty;
	{
		std::ofstream stream(empty.path(), std::ios::binary);
		ASSERT_TRUE(stream.is_open());
	}
	Journal rejectsEmpty;
	std::string emptyError;
	EXPECT_EQ(Journal::Status::Corrupt,
		rejectsEmpty.open(empty.string(), "account-D0", &emptyError));
	EXPECT_NE(std::string::npos, emptyError.find("empty"));

	TemporaryWal truncated;
	{
		Journal writer;
		ASSERT_EQ(Journal::Status::Ok, writer.open(truncated.string(), "account-D"));
		ASSERT_EQ(Journal::Status::Ok, writer.setTarget("owner", "EX.X", 1.0, 0.0));
	}
	{
		std::ofstream stream(truncated.path(), std::ios::binary | std::ios::app);
		ASSERT_TRUE(stream.is_open());
		stream.write("bad", 3);
	}
	Journal rejectsTruncated;
	std::string error;
	EXPECT_EQ(Journal::Status::Corrupt,
		rejectsTruncated.open(truncated.string(), "account-D", &error));
	EXPECT_NE(std::string::npos, error.find("truncated"));
	EXPECT_FALSE(rejectsTruncated.isOpen());

	TemporaryWal damaged;
	{
		Journal writer;
		ASSERT_EQ(Journal::Status::Ok, writer.open(damaged.string(), "account-E"));
		ASSERT_EQ(Journal::Status::Ok, writer.setTarget("owner", "EX.X", 1.0, 0.0));
	}
	{
		std::fstream stream(damaged.path(), std::ios::binary | std::ios::in | std::ios::out);
		ASSERT_TRUE(stream.is_open());
		stream.seekg(-1, std::ios::end);
		char byte = 0;
		stream.read(&byte, 1);
		ASSERT_TRUE(stream.good());
		byte ^= 0x01;
		stream.seekp(-1, std::ios::end);
		stream.write(&byte, 1);
	}
	Journal rejectsDamaged;
	error.clear();
	EXPECT_EQ(Journal::Status::Corrupt,
		rejectsDamaged.open(damaged.string(), "account-E", &error));
	EXPECT_NE(std::string::npos, error.find("checksum"));
}

TEST(DiffOrderOwnershipJournalV1, FailingIntentWriteBlocksFurtherMutations)
{
	TemporaryWal wal;
	Journal journal;
	ASSERT_EQ(Journal::Status::Ok, journal.open(wal.string(), "account-F"));
	ASSERT_EQ(Journal::Status::Ok, journal.advanceLocalOrderIdHighWater(0,
		"unit-test:isolated-empty-history"));
	wtp::DiffOrderOwnershipJournalV1TestAccess::failBeforeWrite(journal);
	uint32_t localId = 99;
	EXPECT_EQ(Journal::Status::StorageFailed,
		journal.prepareOrderIntent("owner", "intent", "EX.X", 1.0, localId));
	EXPECT_EQ(0u, localId);
	EXPECT_FALSE(journal.isHealthy());
	EXPECT_EQ(Journal::Status::StorageFailed,
		journal.prepareOrderIntent("owner", "intent-2", "EX.X", 1.0, localId));
	const Journal::Snapshot state = journal.snapshot();
	EXPECT_EQ(state.intents.end(), state.intents.find("intent"));
}

TEST(DiffOrderOwnershipJournalV1, FlushFailureReturnsBeforeSendCommitAndBlocksJournal)
{
	TemporaryWal wal;
	Journal journal;
	ASSERT_EQ(Journal::Status::Ok, journal.open(wal.string(), "account-G"));
	ASSERT_EQ(Journal::Status::Ok, journal.advanceLocalOrderIdHighWater(0,
		"unit-test:isolated-empty-history"));
	wtp::DiffOrderOwnershipJournalV1TestAccess::failBeforeFlush(journal);
	uint32_t localId = 99;
	EXPECT_EQ(Journal::Status::StorageFailed,
		journal.prepareOrderIntent("owner", "intent", "EX.X", 1.0, localId));
	EXPECT_EQ(0u, localId);
	EXPECT_FALSE(journal.isHealthy());
	EXPECT_EQ(Journal::Status::StorageFailed,
		journal.bindOrder("intent", 1, "broker-123"));
}

TEST(DiffOrderOwnershipJournalV1, ExclusiveOpenAndConcurrentIntentsAreSerialized)
{
	TemporaryWal wal;
	Journal journal;
	ASSERT_EQ(Journal::Status::Ok, journal.open(wal.string(), "account-H"));
	ASSERT_EQ(Journal::Status::Ok, journal.advanceLocalOrderIdHighWater(0,
		"unit-test:isolated-empty-history"));
	Journal secondWriter;
	EXPECT_EQ(Journal::Status::Busy, secondWriter.open(wal.string(), "account-H"));

	std::atomic<unsigned> succeeded{ 0 };
	std::vector<std::thread> writers;
	for (unsigned i = 0; i < 8; ++i)
	{
		writers.emplace_back([&journal, &succeeded, i]() {
			uint32_t localId = 0;
			const Journal::Status result = journal.prepareOrderIntent("owner-" + std::to_string(i),
				"intent-" + std::to_string(i), "EX.X", 1.0, localId);
			if (result == Journal::Status::Ok)
				++succeeded;
		});
	}
	for (auto& writer : writers)
		writer.join();
	EXPECT_EQ(8u, succeeded.load());
	EXPECT_EQ(8u, journal.snapshot().intents.size());
	EXPECT_EQ(9u, journal.snapshot().last_sequence);
}
}
