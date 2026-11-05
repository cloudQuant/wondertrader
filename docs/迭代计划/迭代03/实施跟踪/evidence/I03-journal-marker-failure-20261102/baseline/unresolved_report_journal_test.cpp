#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <WtCore/UnresolvedReportJournal.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace wtp
{
struct UnresolvedReportJournalTestAccess
{
	using FaultPoint = UnresolvedReportJournal::TestFaultPoint;

	static void shortWrites(UnresolvedReportJournal& journal, size_t chunk)
	{
		std::lock_guard<std::mutex> lock(journal._mutex);
		journal._test_write_chunk = chunk;
	}

	static void failAt(UnresolvedReportJournal& journal, FaultPoint point,
		size_t fail_after_bytes = 0)
	{
		std::lock_guard<std::mutex> lock(journal._mutex);
		journal._test_fault_point = point;
		journal._test_fail_after_write_bytes = fail_after_bytes;
	}

	static void failComponentProbe(UnresolvedReportJournal& journal)
	{
		std::lock_guard<std::mutex> lock(journal._mutex);
		journal._test_fault_point = FaultPoint::FailComponentProbe;
	}

	static void failCleanupProbe(UnresolvedReportJournal& journal)
	{
		std::lock_guard<std::mutex> lock(journal._mutex);
		journal._test_fault_point = FaultPoint::FailCleanupProbe;
	}

	static void exhaustSequence(UnresolvedReportJournal& journal)
	{
		std::lock_guard<std::mutex> lock(journal._mutex);
		journal._last_sequence = std::numeric_limits<uint64_t>::max() - 1;
	}
};
}

namespace
{
namespace fs = std::filesystem;
using Journal = wtp::UnresolvedReportJournal;
using FaultPoint = wtp::UnresolvedReportJournalTestAccess::FaultPoint;

uint64_t processId()
{
#ifdef _WIN32
	return static_cast<uint64_t>(GetCurrentProcessId());
#else
	return static_cast<uint64_t>(getpid());
#endif
}

class TemporaryJournal
{
public:
	TemporaryJournal()
	{
		static std::atomic<uint64_t> serial{ 0 };
		_directory = fs::temp_directory_path() /
			("wondertrader_unresolved_report_" + std::to_string(processId()) + "_" +
			std::to_string(++serial));
		if (!fs::create_directory(_directory))
			throw std::runtime_error("unable to create isolated journal test directory");
#ifndef _WIN32
		fs::permissions(_directory, fs::perms::owner_all, fs::perm_options::replace);
#endif
		_path = _directory / "reports.wal";
	}

	~TemporaryJournal()
	{
		std::error_code ignored;
		fs::remove_all(_directory, ignored);
	}

	std::string string() const { return _path.u8string(); }
	const fs::path& path() const { return _path; }
	const fs::path& directory() const { return _directory; }

private:
	fs::path _directory;
	fs::path _path;
};

Journal::Status createJournal(Journal& journal, const TemporaryJournal& file)
{
	return journal.createNew(file.string(), "test:isolated-fresh-account");
}

#ifdef _WIN32
TEST(UnresolvedReportJournal, WindowsFailsClosedUntilNativeDurabilityIsValidated)
{
	TemporaryJournal file;
	Journal journal;
	EXPECT_EQ(Journal::Status::PlatformUnsupported, journal.open(file.string()));
	EXPECT_EQ(Journal::Status::PlatformUnsupported,
		journal.createNew(file.string(), "test:isolated-fresh-account"));
	EXPECT_FALSE(fs::exists(file.path()));
}
#else

TEST(UnresolvedReportJournal, MissingOpenDoesNotBootstrapAndFreshRecordSurvivesRestart)
{
	TemporaryJournal file;
	Journal missing;
	EXPECT_EQ(Journal::Status::NotFound, missing.open(file.string()));
	EXPECT_FALSE(fs::exists(file.path()));
	uint64_t missingHighWater = 99;
	EXPECT_EQ(Journal::Status::NotOpen, missing.queryPendingHighWater(missingHighWater));
	EXPECT_EQ(0u, missingHighWater);

	Journal journal;
	ASSERT_EQ(Journal::Status::Ok, createJournal(journal, file));
	uint64_t sequence = 0;
	ASSERT_EQ(Journal::Status::Ok, journal.append("serialized-unparseable-report", sequence));
	EXPECT_EQ(2u, sequence); // sequence 1 is the explicit bootstrap evidence frame
	EXPECT_EQ(sequence, journal.pendingHighWater());
	uint64_t safeHighWater = 99;
	EXPECT_EQ(Journal::Status::Ok, journal.queryPendingHighWater(safeHighWater));
	EXPECT_EQ(sequence, safeHighWater);
	journal.close();

	Journal reopened;
	ASSERT_EQ(Journal::Status::Ok, reopened.open(file.string()));
	std::vector<Journal::Report> pending;
	ASSERT_EQ(Journal::Status::Ok, reopened.pending(pending));
	ASSERT_EQ(1u, pending.size());
	EXPECT_EQ(sequence, pending.front().sequence);
	EXPECT_EQ("serialized-unparseable-report", pending.front().raw_payload);
	EXPECT_TRUE(pending.front().committed);
}

TEST(UnresolvedReportJournal, BootstrapCannotBeImplicitlyRepeated)
{
	TemporaryJournal file;
	Journal first;
	ASSERT_EQ(Journal::Status::Ok, createJournal(first, file));
	first.close();
	Journal second;
	EXPECT_EQ(Journal::Status::AlreadyExists, createJournal(second, file));
	EXPECT_EQ(Journal::Status::Ok, second.open(file.string()));
}

TEST(UnresolvedReportJournal, MissingWalWithOtherJournalArtifactsRequiresRecovery)
{
	TemporaryJournal file;
	Journal seed;
	ASSERT_EQ(Journal::Status::Ok, createJournal(seed, file));
	seed.close();
	fs::remove(file.path());

	Journal reopened;
	EXPECT_EQ(Journal::Status::RecoveryRequired, reopened.open(file.string()));
	Journal::RecoveryInspection inspection;
	ASSERT_EQ(Journal::Status::RecoveryRequired, reopened.recoveryInspection(inspection));
	EXPECT_TRUE(inspection.requires_manual_recovery);
	EXPECT_FALSE(inspection.primary_issue.empty());
}

TEST(UnresolvedReportJournal, AckRequiresExactPendingHighWaterAndPersistsAuditFields)
{
	TemporaryJournal file;
	Journal journal;
	ASSERT_EQ(Journal::Status::Ok, createJournal(journal, file));
	uint64_t first = 0, second = 0;
	ASSERT_EQ(Journal::Status::Ok, journal.append("raw-report-1", first));
	ASSERT_EQ(Journal::Status::Ok, journal.append("raw-report-2", second));
	EXPECT_EQ(Journal::Status::StaleHighWater,
		journal.acknowledge(first, "operator-7", "manual reconciliation"));
	EXPECT_EQ(second, journal.pendingHighWater());
	ASSERT_EQ(Journal::Status::Ok,
		journal.acknowledge(second, "operator-7", "matched broker statement ref R-9"));
	EXPECT_EQ(0u, journal.pendingHighWater());
	journal.close();

	Journal reopened;
	ASSERT_EQ(Journal::Status::Ok, reopened.open(file.string()));
	std::vector<Journal::Report> pending;
	ASSERT_EQ(Journal::Status::Ok, reopened.pending(pending));
	EXPECT_TRUE(pending.empty());
	uint64_t safeHighWater = 99;
	EXPECT_EQ(Journal::Status::Ok, reopened.queryPendingHighWater(safeHighWater));
	EXPECT_EQ(0u, safeHighWater);
	std::vector<Journal::Acknowledgement> records;
	ASSERT_EQ(Journal::Status::Ok, reopened.acknowledgements(records));
	ASSERT_EQ(1u, records.size());
	EXPECT_EQ(second, records.front().acknowledged_high_water);
	EXPECT_EQ("operator-7", records.front().operator_id);
	EXPECT_EQ("matched broker statement ref R-9", records.front().reason);
	EXPECT_TRUE(records.front().committed);
}

TEST(UnresolvedReportJournal, SerializedShortWritesCompleteTheFrame)
{
	TemporaryJournal file;
	Journal journal;
	ASSERT_EQ(Journal::Status::Ok, createJournal(journal, file));
	wtp::UnresolvedReportJournalTestAccess::shortWrites(journal, 3);
	uint64_t sequence = 0;
	ASSERT_EQ(Journal::Status::Ok, journal.append("short-write-loop", sequence));
	EXPECT_EQ(2u, sequence);
	journal.close();

	Journal reopened;
	ASSERT_EQ(Journal::Status::Ok, reopened.open(file.string()));
	std::vector<Journal::Report> pending;
	ASSERT_EQ(Journal::Status::Ok, reopened.pending(pending));
	ASSERT_EQ(1u, pending.size());
	EXPECT_EQ("short-write-loop", pending.front().raw_payload);
}

TEST(UnresolvedReportJournal, ConcurrentThreadAppendsReceiveUniqueOrderedSequences)
{
	TemporaryJournal file;
	Journal journal;
	ASSERT_EQ(Journal::Status::Ok, createJournal(journal, file));
	const size_t threadCount = 8;
	std::vector<uint64_t> sequences(threadCount, 0);
	std::vector<Journal::Status> statuses(threadCount, Journal::Status::NotOpen);
	std::vector<std::thread> threads;
	for (size_t i = 0; i < threadCount; ++i)
	{
		threads.emplace_back([&journal, &sequences, &statuses, i]() {
			statuses[i] = journal.append("thread-report-" + std::to_string(i), sequences[i]);
		});
	}
	for (std::thread& thread : threads)
		thread.join();
	for (Journal::Status status : statuses)
		EXPECT_EQ(Journal::Status::Ok, status);
	std::sort(sequences.begin(), sequences.end());
	for (size_t i = 0; i < threadCount; ++i)
		EXPECT_EQ(static_cast<uint64_t>(i + 2), sequences[i]);
}

TEST(UnresolvedReportJournal, LockContentionIsRejectedAcrossProcesses)
{
	TemporaryJournal file;
	Journal owner;
	ASSERT_EQ(Journal::Status::Ok, createJournal(owner, file));
	const pid_t child = fork();
	ASSERT_GE(child, 0);
	if (child == 0)
	{
		Journal contender;
		const Journal::Status status = contender.open(file.string());
		_exit(status == Journal::Status::Busy ? 0 : 1);
	}
	int childStatus = 0;
	ASSERT_EQ(child, waitpid(child, &childStatus, 0));
	ASSERT_TRUE(WIFEXITED(childStatus));
	EXPECT_EQ(0, WEXITSTATUS(childStatus));
}

TEST(UnresolvedReportJournal, TruncatedPrimaryExportsCompletePrefixAndFirstBadOffset)
{
	TemporaryJournal file;
	Journal journal;
	ASSERT_EQ(Journal::Status::Ok, createJournal(journal, file));
	uint64_t sequence = 0;
	ASSERT_EQ(Journal::Status::Ok, journal.append("report-to-truncate", sequence));
	journal.close();
	const uintmax_t originalSize = fs::file_size(file.path());
	ASSERT_GT(originalSize, 1u);
	fs::resize_file(file.path(), originalSize - 1);
	const uintmax_t truncatedSize = fs::file_size(file.path());

	Journal reopened;
	EXPECT_EQ(Journal::Status::RecoveryRequired, reopened.open(file.string()));
	EXPECT_EQ(truncatedSize, fs::file_size(file.path()));
	Journal::RecoveryInspection inspection;
	ASSERT_EQ(Journal::Status::RecoveryRequired, reopened.recoveryInspection(inspection));
	EXPECT_TRUE(inspection.requires_manual_recovery);
	EXPECT_FALSE(inspection.primary_issue.empty());
	EXPECT_EQ(inspection.primary_bad_offset, inspection.primary_valid_prefix.size());
	EXPECT_LT(inspection.primary_valid_prefix.size(), truncatedSize);
	ASSERT_FALSE(inspection.unresolved_prepares.empty());
	EXPECT_EQ("report-to-truncate", inspection.unresolved_prepares.back().raw_payload);
}

TEST(UnresolvedReportJournal, CorruptPrimaryChecksumPreservesBytesAndForensicPrefix)
{
	TemporaryJournal file;
	Journal journal;
	ASSERT_EQ(Journal::Status::Ok, createJournal(journal, file));
	uint64_t sequence = 0;
	ASSERT_EQ(Journal::Status::Ok, journal.append("report-to-corrupt", sequence));
	journal.close();
	{
		std::fstream wal(file.path(), std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(wal.good());
		wal.seekg(-1, std::ios::end);
		char byte = 0;
		wal.read(&byte, 1);
		ASSERT_TRUE(wal.good());
		byte ^= 0x40;
		wal.seekp(-1, std::ios::end);
		wal.write(&byte, 1);
		ASSERT_TRUE(wal.good());
	}
	const std::string corrupted = [&]() {
		std::ifstream input(file.path(), std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(input), {});
	}();

	Journal reopened;
	EXPECT_EQ(Journal::Status::RecoveryRequired, reopened.open(file.string()));
	EXPECT_EQ(corrupted.size(), fs::file_size(file.path()));
	Journal::RecoveryInspection inspection;
	ASSERT_EQ(Journal::Status::RecoveryRequired, reopened.recoveryInspection(inspection));
	EXPECT_EQ(inspection.primary_bad_offset, inspection.primary_valid_prefix.size());
	EXPECT_EQ("frame checksum mismatch", inspection.primary_issue);
	ASSERT_FALSE(inspection.unresolved_prepares.empty());
	EXPECT_EQ("report-to-corrupt", inspection.unresolved_prepares.back().raw_payload);
}

TEST(UnresolvedReportJournal, TruncatedGuardPreservesItsGoodPrefix)
{
	TemporaryJournal file;
	Journal journal;
	ASSERT_EQ(Journal::Status::Ok, createJournal(journal, file));
	uint64_t sequence = 0;
	ASSERT_EQ(Journal::Status::Ok, journal.append("guard-prefix-report", sequence));
	journal.close();
	const fs::path guard = file.path().string() + ".guard";
	const uintmax_t originalSize = fs::file_size(guard);
	fs::resize_file(guard, originalSize - 1);
	const uintmax_t truncatedSize = fs::file_size(guard);

	Journal reopened;
	EXPECT_EQ(Journal::Status::RecoveryRequired, reopened.open(file.string()));
	EXPECT_EQ(truncatedSize, fs::file_size(guard));
	Journal::RecoveryInspection inspection;
	ASSERT_EQ(Journal::Status::RecoveryRequired, reopened.recoveryInspection(inspection));
	EXPECT_EQ(inspection.guard_bad_offset, inspection.guard_valid_prefix.size());
	EXPECT_LT(inspection.guard_valid_prefix.size(), truncatedSize);
}

TEST(UnresolvedReportJournal, InterruptedPrimaryAppendAfterProcessExitExportsForensicPrefix)
{
	TemporaryJournal file;
	Journal seed;
	ASSERT_EQ(Journal::Status::Ok, createJournal(seed, file));
	uint64_t committedSequence = 0;
	ASSERT_EQ(Journal::Status::Ok,
		seed.append("committed-prefix-report", committedSequence));
	seed.close();
	const auto readWal = [&file]() {
		std::ifstream input(file.path(), std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(input), {});
	};
	const std::string validWal = readWal();
	ASSERT_FALSE(validWal.empty());

	const pid_t child = fork();
	ASSERT_GE(child, 0);
	if (child == 0)
	{
		Journal writer;
		if (writer.open(file.string()) != Journal::Status::Ok)
			_exit(10);
		wtp::UnresolvedReportJournalTestAccess::failAt(
			writer, FaultPoint::FailPrimaryPartialWrite, 7);
		uint64_t sequence = 0;
		const Journal::Status status = writer.append("report-with-torn-tail", sequence);
		_exit(status == Journal::Status::StorageFailed && sequence == 0 ? 0 : 1);
	}
	int childStatus = 0;
	ASSERT_EQ(child, waitpid(child, &childStatus, 0));
	ASSERT_TRUE(WIFEXITED(childStatus));
	ASSERT_EQ(0, WEXITSTATUS(childStatus));
	const std::string tornWal = readWal();
	ASSERT_GT(tornWal.size(), validWal.size());
	EXPECT_EQ(validWal, tornWal.substr(0, validWal.size()));

	Journal reopened;
	EXPECT_EQ(Journal::Status::RecoveryRequired, reopened.open(file.string()));
	const std::string afterOpenWal = readWal();
	EXPECT_EQ(tornWal, afterOpenWal);
	Journal::RecoveryInspection inspection;
	ASSERT_EQ(Journal::Status::RecoveryRequired, reopened.recoveryInspection(inspection));
	EXPECT_TRUE(inspection.requires_manual_recovery);
	EXPECT_TRUE(inspection.has_in_flight_marker);
	EXPECT_EQ(validWal, inspection.primary_valid_prefix);
	EXPECT_EQ(validWal.size(), inspection.primary_bad_offset);
	EXPECT_FALSE(inspection.primary_issue.empty());
	ASSERT_FALSE(inspection.unresolved_prepares.empty());
	EXPECT_EQ("report-with-torn-tail", inspection.unresolved_prepares.back().raw_payload);
	std::vector<Journal::Report> pending;
	EXPECT_EQ(Journal::Status::RecoveryRequired, reopened.pending(pending));
}

void expectFaultRequiresRecovery(FaultPoint point, const std::string& report,
	size_t fail_after_bytes = 0)
{
	TemporaryJournal file;
	Journal journal;
	ASSERT_EQ(Journal::Status::Ok, createJournal(journal, file));
	uint64_t priorSequence = 0;
	ASSERT_EQ(Journal::Status::Ok,
		journal.append("prior-report-before-write-failure", priorSequence));
	wtp::UnresolvedReportJournalTestAccess::failAt(journal, point, fail_after_bytes);
	uint64_t sequence = 0;
	const bool cleanup_failure = point == FaultPoint::FailCleanupMarkerFlush ||
		point == FaultPoint::FailCleanupMarkerCreateDirectoryFlush ||
		point == FaultPoint::FailMarkerClearDirectoryFlush ||
		point == FaultPoint::FailCleanupMarkerRemove;
	EXPECT_EQ(cleanup_failure ? Journal::Status::RecoveryRequired : Journal::Status::StorageFailed,
		journal.append(report, sequence));
	EXPECT_FALSE(journal.isHealthy());
	Journal::RecoveryInspection immediate;
	ASSERT_EQ(Journal::Status::RecoveryRequired, journal.recoveryInspection(immediate));
	EXPECT_TRUE(immediate.requires_manual_recovery);
	auto prior = std::find_if(immediate.primary_reports.begin(), immediate.primary_reports.end(),
		[priorSequence](const Journal::Report& candidate) {
			return candidate.sequence == priorSequence;
		});
	ASSERT_NE(immediate.primary_reports.end(), prior);
	EXPECT_EQ("prior-report-before-write-failure", prior->raw_payload);
	journal.close();

	Journal reopened;
	EXPECT_EQ(Journal::Status::RecoveryRequired, reopened.open(file.string()));
	Journal::RecoveryInspection inspection;
	ASSERT_EQ(Journal::Status::RecoveryRequired, reopened.recoveryInspection(inspection));
	EXPECT_TRUE(inspection.requires_manual_recovery);
	EXPECT_TRUE(inspection.has_in_flight_marker || inspection.has_cleanup_marker);
	EXPECT_TRUE(!inspection.in_flight_marker_bytes.empty() ||
		!inspection.cleanup_marker_bytes.empty());
	std::vector<Journal::Report> pending;
	EXPECT_EQ(Journal::Status::RecoveryRequired, reopened.pending(pending));
}

TEST(UnresolvedReportJournal, EveryWriteAndCleanupFailureRequiresRecovery)
{
	expectFaultRequiresRecovery(FaultPoint::FailMarkerFlush, "marker-flush-failure");
	expectFaultRequiresRecovery(FaultPoint::FailMarkerCreateDirectoryFlush,
		"marker-directory-flush-failure");
	expectFaultRequiresRecovery(FaultPoint::FailPrepareFlush, "prepare-flush-failure");
	expectFaultRequiresRecovery(FaultPoint::FailPrimaryPartialWrite,
		"partial-primary-write-failure", 5);
	expectFaultRequiresRecovery(FaultPoint::FailPrimaryFlush, "primary-flush-failure");
	expectFaultRequiresRecovery(FaultPoint::FailCommitFlush, "commit-flush-failure");
	expectFaultRequiresRecovery(FaultPoint::FailCleanupMarkerFlush,
		"cleanup-marker-flush-failure");
	expectFaultRequiresRecovery(FaultPoint::FailCleanupMarkerCreateDirectoryFlush,
		"cleanup-marker-directory-flush-failure");
	expectFaultRequiresRecovery(FaultPoint::FailMarkerClearDirectoryFlush,
		"in-flight-marker-removal-directory-flush-failure");
	expectFaultRequiresRecovery(FaultPoint::FailCleanupMarkerRemove,
		"cleanup-marker-removal-failure");
}

TEST(UnresolvedReportJournal, ComponentProbeErrorFailsClosedWithoutTreatingPathAsMissing)
{
	TemporaryJournal file;
	Journal journal;
	ASSERT_EQ(Journal::Status::Ok, createJournal(journal, file));
	const uintmax_t walSize = fs::file_size(file.path());
	wtp::UnresolvedReportJournalTestAccess::failComponentProbe(journal);
	uint64_t sequence = 0;
	EXPECT_EQ(Journal::Status::StorageFailed,
		journal.append("must-not-write-after-component-probe-error", sequence));
	EXPECT_FALSE(journal.isHealthy());
	uint64_t highWater = 99;
	EXPECT_EQ(Journal::Status::StorageFailed, journal.queryPendingHighWater(highWater));
	EXPECT_EQ(0u, highWater);
	EXPECT_EQ(walSize, fs::file_size(file.path()));
	EXPECT_FALSE(fs::exists(file.path().string() + ".inflight"));
}

TEST(UnresolvedReportJournal, CleanupComponentProbeErrorFailsClosedWithoutTreatingPathAsMissing)
{
	TemporaryJournal file;
	Journal journal;
	ASSERT_EQ(Journal::Status::Ok, createJournal(journal, file));
	const uintmax_t walSize = fs::file_size(file.path());
	wtp::UnresolvedReportJournalTestAccess::failCleanupProbe(journal);
	uint64_t sequence = 0;
	EXPECT_EQ(Journal::Status::StorageFailed,
		journal.append("must-not-write-after-cleanup-probe-error", sequence));
	EXPECT_FALSE(journal.isHealthy());
	uint64_t highWater = 99;
	EXPECT_EQ(Journal::Status::StorageFailed, journal.queryPendingHighWater(highWater));
	EXPECT_EQ(0u, highWater);
	EXPECT_EQ(walSize, fs::file_size(file.path()));
	EXPECT_FALSE(fs::exists(file.path().string() + ".inflight"));
}

TEST(UnresolvedReportJournal, FailedAckCommitImmediatelyExposesPriorReportsAndAckEvidence)
{
	TemporaryJournal file;
	Journal journal;
	ASSERT_EQ(Journal::Status::Ok, createJournal(journal, file));
	uint64_t reportSequence = 0;
	ASSERT_EQ(Journal::Status::Ok,
		journal.append("prior-report-before-ack-failure", reportSequence));
	wtp::UnresolvedReportJournalTestAccess::failAt(journal, FaultPoint::FailCommitFlush);
	EXPECT_EQ(Journal::Status::StorageFailed,
		journal.acknowledge(reportSequence, "operator-11", "reconciliation record R-18"));
	EXPECT_FALSE(journal.isHealthy());
	Journal::RecoveryInspection inspection;
	ASSERT_EQ(Journal::Status::RecoveryRequired, journal.recoveryInspection(inspection));
	auto prior = std::find_if(inspection.primary_reports.begin(), inspection.primary_reports.end(),
		[reportSequence](const Journal::Report& candidate) {
			return candidate.sequence == reportSequence;
		});
	ASSERT_NE(inspection.primary_reports.end(), prior);
	EXPECT_EQ("prior-report-before-ack-failure", prior->raw_payload);
	EXPECT_FALSE(prior->committed);
	EXPECT_TRUE(prior->guard_commit_matches_primary);
	ASSERT_FALSE(inspection.primary_acknowledgements.empty());
	EXPECT_EQ(reportSequence,
		inspection.primary_acknowledgements.back().acknowledged_high_water);
	EXPECT_EQ("operator-11", inspection.primary_acknowledgements.back().operator_id);
	EXPECT_FALSE(inspection.primary_acknowledgements.back().committed);
	EXPECT_TRUE(inspection.primary_acknowledgements.back().guard_commit_matches_primary);
	std::vector<Journal::Report> pending;
	EXPECT_EQ(Journal::Status::RecoveryRequired, journal.pending(pending));
}

TEST(UnresolvedReportJournal, AmbiguousAckCommitRemainsRecoveryRequiredAfterProcessExit)
{
	TemporaryJournal file;
	Journal journal;
	ASSERT_EQ(Journal::Status::Ok, createJournal(journal, file));
	uint64_t reportSequence = 0;
	ASSERT_EQ(Journal::Status::Ok, journal.append("must-remain-unresolved", reportSequence));
	wtp::UnresolvedReportJournalTestAccess::failAt(journal, FaultPoint::FailCommitFlush);

	const pid_t child = fork();
	ASSERT_GE(child, 0);
	if (child == 0)
	{
		const Journal::Status status = journal.acknowledge(reportSequence,
			"operator-7", "fresh reconciliation ref R-12");
		_exit(status == Journal::Status::StorageFailed ? 0 : 1);
	}
	int childStatus = 0;
	ASSERT_EQ(child, waitpid(child, &childStatus, 0));
	ASSERT_TRUE(WIFEXITED(childStatus));
	ASSERT_EQ(0, WEXITSTATUS(childStatus));
	journal.close();

	Journal reopened;
	EXPECT_EQ(Journal::Status::RecoveryRequired, reopened.open(file.string()));
	Journal::RecoveryInspection inspection;
	ASSERT_EQ(Journal::Status::RecoveryRequired, reopened.recoveryInspection(inspection));
	EXPECT_TRUE(inspection.has_in_flight_marker);
	ASSERT_FALSE(inspection.primary_acknowledgements.empty());
	EXPECT_FALSE(inspection.primary_acknowledgements.back().committed);
	EXPECT_TRUE(inspection.primary_acknowledgements.back().guard_commit_matches_primary);
	EXPECT_FALSE(inspection.in_flight_marker_bytes.empty());
	std::vector<Journal::Report> pending;
	EXPECT_EQ(Journal::Status::RecoveryRequired, reopened.pending(pending));
}

TEST(UnresolvedReportJournal, AmbiguousAckPrimaryFlushRemainsRecoveryRequiredAfterProcessExit)
{
	TemporaryJournal file;
	Journal seed;
	ASSERT_EQ(Journal::Status::Ok, createJournal(seed, file));
	uint64_t reportSequence = 0;
	ASSERT_EQ(Journal::Status::Ok, seed.append("must-remain-unresolved", reportSequence));
	seed.close();

	const pid_t child = fork();
	ASSERT_GE(child, 0);
	if (child == 0)
	{
		Journal writer;
		if (writer.open(file.string()) != Journal::Status::Ok)
			_exit(10);
		wtp::UnresolvedReportJournalTestAccess::failAt(
			writer, FaultPoint::FailPrimaryFlush);
		const Journal::Status status = writer.acknowledge(reportSequence,
			"operator-7", "fresh reconciliation ref R-12");
		_exit(status == Journal::Status::StorageFailed ? 0 : 1);
	}
	int childStatus = 0;
	ASSERT_EQ(child, waitpid(child, &childStatus, 0));
	ASSERT_TRUE(WIFEXITED(childStatus));
	ASSERT_EQ(0, WEXITSTATUS(childStatus));

	Journal reopened;
	EXPECT_EQ(Journal::Status::RecoveryRequired, reopened.open(file.string()));
	Journal::RecoveryInspection inspection;
	ASSERT_EQ(Journal::Status::RecoveryRequired, reopened.recoveryInspection(inspection));
	EXPECT_TRUE(inspection.requires_manual_recovery);
	EXPECT_TRUE(inspection.has_in_flight_marker);
	EXPECT_FALSE(inspection.in_flight_marker_bytes.empty());
	ASSERT_FALSE(inspection.primary_reports.empty());
	EXPECT_EQ("must-remain-unresolved", inspection.primary_reports.back().raw_payload);
	EXPECT_FALSE(inspection.primary_reports.back().committed);
	ASSERT_FALSE(inspection.primary_acknowledgements.empty());
	const Journal::Acknowledgement& ack = inspection.primary_acknowledgements.back();
	EXPECT_EQ(reportSequence, ack.acknowledged_high_water);
	EXPECT_EQ("operator-7", ack.operator_id);
	EXPECT_FALSE(ack.committed);
	EXPECT_FALSE(ack.guard_commit_matches_primary);
	std::vector<Journal::Report> pending;
	EXPECT_EQ(Journal::Status::RecoveryRequired, reopened.pending(pending));
	uint64_t safeHighWater = 99;
	EXPECT_EQ(Journal::Status::RecoveryRequired,
		reopened.queryPendingHighWater(safeHighWater));
	EXPECT_EQ(0u, safeHighWater);
}

TEST(UnresolvedReportJournal, MarkerRemovalDirectoryFlushFailurePersistsCleanupUncertainty)
{
	TemporaryJournal file;
	Journal journal;
	ASSERT_EQ(Journal::Status::Ok, createJournal(journal, file));
	uint64_t reportSequence = 0;
	ASSERT_EQ(Journal::Status::Ok, journal.append("report-before-manual-ack", reportSequence));
	wtp::UnresolvedReportJournalTestAccess::failAt(journal,
		FaultPoint::FailMarkerClearDirectoryFlush);
	EXPECT_EQ(Journal::Status::RecoveryRequired,
		journal.acknowledge(reportSequence, "operator-7", "fresh reconciliation ref R-13"));
	EXPECT_FALSE(journal.isHealthy());
	EXPECT_NE(std::string::npos, journal.lastError().find("cleanup uncertainty requires manual recovery"));
	Journal::RecoveryInspection inProcessInspection;
	ASSERT_EQ(Journal::Status::RecoveryRequired,
		journal.recoveryInspection(inProcessInspection));
	EXPECT_TRUE(inProcessInspection.cleanup_uncertain);
	EXPECT_TRUE(inProcessInspection.has_cleanup_marker);
	ASSERT_FALSE(inProcessInspection.primary_reports.empty());
	EXPECT_EQ("report-before-manual-ack",
		inProcessInspection.primary_reports.back().raw_payload);
	EXPECT_TRUE(inProcessInspection.primary_reports.back().guard_commit_matches_primary);
	ASSERT_FALSE(inProcessInspection.primary_acknowledgements.empty());
	EXPECT_TRUE(inProcessInspection.primary_acknowledgements.back().guard_commit_matches_primary);
	EXPECT_FALSE(inProcessInspection.primary_acknowledgements.back().committed);
	journal.close();

	// The cleanup marker was flushed before the in-flight marker removal, so it
	// remains available even when that directory sync fails.
	Journal reopened;
	EXPECT_EQ(Journal::Status::RecoveryRequired, reopened.open(file.string()));
	Journal::RecoveryInspection inspection;
	ASSERT_EQ(Journal::Status::RecoveryRequired, reopened.recoveryInspection(inspection));
	EXPECT_TRUE(inspection.cleanup_uncertain);
	EXPECT_TRUE(inspection.has_cleanup_marker);
	ASSERT_FALSE(inspection.primary_reports.empty());
	EXPECT_EQ("report-before-manual-ack", inspection.primary_reports.back().raw_payload);
	EXPECT_TRUE(inspection.primary_reports.back().guard_commit_matches_primary);
	ASSERT_FALSE(inspection.primary_acknowledgements.empty());
	EXPECT_TRUE(inspection.primary_acknowledgements.back().guard_commit_matches_primary);
	EXPECT_FALSE(inspection.primary_acknowledgements.back().committed);
	EXPECT_FALSE(inspection.cleanup_marker_bytes.empty());
	std::vector<Journal::Report> pending;
	EXPECT_EQ(Journal::Status::RecoveryRequired, reopened.pending(pending));
}

void expectAckCleanupRecovery(FaultPoint point)
{
	TemporaryJournal file;
	Journal journal;
	ASSERT_EQ(Journal::Status::Ok, createJournal(journal, file));
	uint64_t reportSequence = 0;
	ASSERT_EQ(Journal::Status::Ok, journal.append("ack-cleanup-forensic-report", reportSequence));
	wtp::UnresolvedReportJournalTestAccess::failAt(journal, point);
	EXPECT_EQ(Journal::Status::RecoveryRequired,
		journal.acknowledge(reportSequence, "operator-9", "verified statement C-4"));

	Journal::RecoveryInspection immediate;
	ASSERT_EQ(Journal::Status::RecoveryRequired, journal.recoveryInspection(immediate));
	ASSERT_FALSE(immediate.primary_reports.empty());
	EXPECT_EQ("ack-cleanup-forensic-report", immediate.primary_reports.back().raw_payload);
	EXPECT_TRUE(immediate.primary_reports.back().guard_commit_matches_primary);
	ASSERT_FALSE(immediate.primary_acknowledgements.empty());
	EXPECT_TRUE(immediate.primary_acknowledgements.back().guard_commit_matches_primary);
	EXPECT_FALSE(immediate.primary_acknowledgements.back().committed);
	EXPECT_TRUE(immediate.cleanup_uncertain);
	journal.close();

	Journal reopened;
	EXPECT_EQ(Journal::Status::RecoveryRequired, reopened.open(file.string()));
	Journal::RecoveryInspection afterReopen;
	ASSERT_EQ(Journal::Status::RecoveryRequired, reopened.recoveryInspection(afterReopen));
	ASSERT_FALSE(afterReopen.primary_reports.empty());
	EXPECT_EQ("ack-cleanup-forensic-report", afterReopen.primary_reports.back().raw_payload);
	EXPECT_TRUE(afterReopen.primary_reports.back().guard_commit_matches_primary);
	ASSERT_FALSE(afterReopen.primary_acknowledgements.empty());
	EXPECT_TRUE(afterReopen.primary_acknowledgements.back().guard_commit_matches_primary);
	EXPECT_FALSE(afterReopen.primary_acknowledgements.back().committed);
	EXPECT_TRUE(afterReopen.cleanup_uncertain);
	uint64_t safeHighWater = 99;
	EXPECT_EQ(Journal::Status::RecoveryRequired, reopened.queryPendingHighWater(safeHighWater));
	EXPECT_EQ(0u, safeHighWater);
}

TEST(UnresolvedReportJournal, AckCleanupFailuresExposeReportAndMatchedCommitBeforeAndAfterReopen)
{
	expectAckCleanupRecovery(FaultPoint::FailCleanupMarkerFlush);
	expectAckCleanupRecovery(FaultPoint::FailCleanupMarkerCreateDirectoryFlush);
	expectAckCleanupRecovery(FaultPoint::FailMarkerClearDirectoryFlush);
	expectAckCleanupRecovery(FaultPoint::FailCleanupMarkerRemove);
}

TEST(UnresolvedReportJournal, SymlinkAndVisiblePathReplacementFailClosed)
{
	TemporaryJournal symlinkFile;
	Journal symlinkSeed;
	ASSERT_EQ(Journal::Status::Ok, createJournal(symlinkSeed, symlinkFile));
	symlinkSeed.close();
	const fs::path target = symlinkFile.path().string() + ".target";
	const fs::path backup = symlinkFile.path().string() + ".backup";
	fs::rename(symlinkFile.path(), backup);
	{
		std::ofstream output(target, std::ios::binary);
		ASSERT_TRUE(output.good());
		output << "do-not-follow";
	}
	fs::create_symlink(target, symlinkFile.path());
	Journal symlinkJournal;
	EXPECT_EQ(Journal::Status::RecoveryRequired, symlinkJournal.open(symlinkFile.string()));
	std::ifstream targetInput(target, std::ios::binary);
	EXPECT_EQ("do-not-follow", std::string(std::istreambuf_iterator<char>(targetInput), {}));

	TemporaryJournal replacementFile;
	Journal journal;
	ASSERT_EQ(Journal::Status::Ok, createJournal(journal, replacementFile));
	fs::rename(replacementFile.path(), replacementFile.path().string() + ".backup");
	{
		std::ofstream replacement(replacementFile.path(), std::ios::binary);
		ASSERT_TRUE(replacement.good());
		replacement << "replacement";
	}
	uint64_t sequence = 0;
	EXPECT_EQ(Journal::Status::StorageFailed,
		journal.append("must-not-write-replaced-file", sequence));
	EXPECT_FALSE(journal.isHealthy());

	TemporaryJournal lockReplacementFile;
	Journal lockOwner;
	ASSERT_EQ(Journal::Status::Ok, createJournal(lockOwner, lockReplacementFile));
	fs::rename(lockReplacementFile.path().string() + ".lock",
		lockReplacementFile.path().string() + ".lock.backup");
	{
		std::ofstream newLock(lockReplacementFile.path().string() + ".lock", std::ios::binary);
		ASSERT_TRUE(newLock.good());
	}
	Journal contender;
	const Journal::Status contenderStatus = contender.open(lockReplacementFile.string());
	EXPECT_TRUE(contenderStatus == Journal::Status::Busy ||
		contenderStatus == Journal::Status::UnsafePath ||
		contenderStatus == Journal::Status::RecoveryRequired);
	EXPECT_EQ(Journal::Status::StorageFailed,
		lockOwner.append("must-not-write-with-drifted-lock", sequence));
}

TEST(UnresolvedReportJournal, UnsafeDirectoryPermissionsAreRejected)
{
	TemporaryJournal file;
	Journal seed;
	ASSERT_EQ(Journal::Status::Ok, createJournal(seed, file));
	seed.close();
	fs::permissions(file.directory(), fs::perms::owner_all | fs::perms::group_read,
		fs::perm_options::replace);
	Journal reopened;
	EXPECT_EQ(Journal::Status::UnsafePath, reopened.open(file.string()));
}

TEST(UnresolvedReportJournal, ExistingWalGuardAndLockPermissionsAreValidated)
{
	auto expectUnsafeExistingFile = [](const std::string& suffix) {
		TemporaryJournal file;
		Journal seed;
		ASSERT_EQ(Journal::Status::Ok, createJournal(seed, file));
		seed.close();
		const fs::path target = suffix.empty() ? file.path() :
			fs::path(file.path().string() + suffix);
		fs::permissions(target, fs::perms::owner_read | fs::perms::owner_write |
			fs::perms::group_read, fs::perm_options::replace);
		Journal reopened;
		EXPECT_EQ(Journal::Status::UnsafePath, reopened.open(file.string()));
	};
	expectUnsafeExistingFile("");
	expectUnsafeExistingFile(".guard");
	expectUnsafeExistingFile(".lock");
}

TEST(UnresolvedReportJournal, ReplacedContainingDirectoryIsDetectedBeforeMutation)
{
	TemporaryJournal file;
	Journal journal;
	ASSERT_EQ(Journal::Status::Ok, createJournal(journal, file));
	const fs::path moved = file.directory().string() + ".moved";
	fs::rename(file.directory(), moved);
	ASSERT_TRUE(fs::create_directory(file.directory()));
	fs::permissions(file.directory(), fs::perms::owner_all, fs::perm_options::replace);

	const fs::path oldWal = moved / file.path().filename();
	const uintmax_t oldWalSize = fs::file_size(oldWal);
	uint64_t sequence = 0;
	EXPECT_EQ(Journal::Status::StorageFailed,
		journal.append("must-not-write-through-replaced-directory", sequence));
	EXPECT_EQ(oldWalSize, fs::file_size(oldWal));
	EXPECT_FALSE(fs::exists(file.path()));
	std::error_code ignored;
	fs::remove_all(moved, ignored);
}

TEST(UnresolvedReportJournal, PayloadAndAuditFieldLimitsAreRejected)
{
	TemporaryJournal file;
	Journal journal;
	ASSERT_EQ(Journal::Status::Ok, createJournal(journal, file));
	uint64_t sequence = 0;
	EXPECT_EQ(Journal::Status::InvalidArgument,
		journal.append(std::string(1024 * 1024 + 1, 'x'), sequence));
	ASSERT_EQ(Journal::Status::Ok, journal.append("payload", sequence));
	EXPECT_EQ(Journal::Status::InvalidArgument,
		journal.acknowledge(sequence, "", "reason"));
	EXPECT_EQ(Journal::Status::InvalidArgument,
		journal.acknowledge(sequence, "operator", std::string(4097, 'r')));
	EXPECT_EQ(sequence, journal.pendingHighWater());
}

TEST(UnresolvedReportJournal, SequenceExhaustionPoisonsInstanceBeforeWriting)
{
	TemporaryJournal file;
	Journal journal;
	ASSERT_EQ(Journal::Status::Ok, createJournal(journal, file));
	wtp::UnresolvedReportJournalTestAccess::exhaustSequence(journal);
	uint64_t sequence = 0;
	EXPECT_EQ(Journal::Status::SequenceExhausted,
		journal.append("must-not-wrap", sequence));
	EXPECT_EQ(0u, sequence);
	EXPECT_FALSE(journal.isHealthy());
}

#endif // _WIN32
} // namespace
