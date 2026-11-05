/*
 * Durable journal for financial reports that the caller cannot safely parse.
 *
 * This primitive only persists opaque caller-serialized bytes. It does not
 * parse or replay reports, authenticate operators, or make an account ready.
 * An empty pending list or an ACK is never sufficient to mark an account ready;
 * the caller must pause after every startup or uncertain write and complete a
 * fresh external reconciliation. CRCs detect accidental corruption, not
 * malicious same-user or administrator tampering. If the storage device is
 * unavailable or reports a flush error, this class cannot promise persistence.
 * A cleanup marker that reappears after a crash causes RecoveryRequired and is
 * never cleared automatically.
 */
#pragma once

#include "../Includes/WTSMarcos.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

NS_WTP_BEGIN

class UnresolvedReportJournal
{
public:
	enum class Status
	{
		Ok,
		InvalidArgument,
		NotOpen,
		NotFound,
		AlreadyExists,
		Busy,
		StorageFailed,
		Corrupt,
		RecoveryRequired,
		UnsafePath,
		PlatformUnsupported,
		StaleHighWater,
		SequenceExhausted
	};

	struct Report
	{
		uint64_t sequence = 0;
		std::string raw_payload;
		// True only when the complete journal opened without recovery requirements.
		bool committed = true;
		// Structural evidence that PREPARE, COMMIT, and the primary frame match.
		// This can be true while committed is false in a RecoveryRequired view.
		bool guard_commit_matches_primary = false;
	};

	struct Acknowledgement
	{
		uint64_t sequence = 0;
		uint64_t acknowledged_high_water = 0;
		std::string operator_id;
		std::string reason;
		// True only when the complete journal opened without recovery requirements.
		bool committed = true;
		// Structural evidence that PREPARE, COMMIT, and the primary frame match.
		// This can be true while committed is false in a RecoveryRequired view.
		bool guard_commit_matches_primary = false;
	};

	/**
	 * Read-only forensic view retained after a recovery-required open or an
	 * ambiguous failed mutation. Prefix bytes are evidence only; callers must not
	 * use them to make an account ready.
	 */
	struct RecoveryInspection
	{
		std::vector<Report> primary_reports;
		std::vector<Acknowledgement> primary_acknowledgements;
		std::vector<Report> unresolved_prepares;
		std::string primary_valid_prefix;
		std::string guard_valid_prefix;
		std::string in_flight_marker_bytes;
		std::string cleanup_marker_bytes;
		uint64_t primary_bad_offset = 0;
		uint64_t guard_bad_offset = 0;
		std::string primary_issue;
		std::string guard_issue;
		bool has_in_flight_marker = false;
		bool has_cleanup_marker = false;
		// Cleanup durability is uncertain; readiness must remain paused pending
		// external manual reconciliation.
		bool cleanup_uncertain = false;
		bool requires_manual_recovery = false;
	};

	UnresolvedReportJournal();
	~UnresolvedReportJournal();

	UnresolvedReportJournal(const UnresolvedReportJournal&) = delete;
	UnresolvedReportJournal& operator=(const UnresolvedReportJournal&) = delete;

	/**
	 * Explicit first-run bootstrap. The caller must first establish that this is
	 * the intended new account and provide a durable reference to that evidence.
	 * This class records the reference but cannot authenticate or verify it.
	 * The journal directory must already exist; parent directories are never
	 * created by this method.
	 */
	Status createNew(const std::string& stable_path,
		const std::string& bootstrap_evidence_reference,
		std::string* error = nullptr);

	/**
	 * Open an existing journal only. A missing path is never treated as a new
	 * account and is never initialized by this method.
	 * macOS/Linux builds require a dedicated owner-only mode-0700 directory and
	 * owned mode-0600 single-link files without extended ACLs. Files are opened
	 * relative to a pinned directory descriptor with no-follow flags. Other POSIX
	 * systems fail closed when ACL checks are unavailable. Windows currently
	 * returns PlatformUnsupported until native ACL and crash-durability behavior
	 * has been validated on that platform.
	 */
	Status open(const std::string& stable_path, std::string* error = nullptr);
	void close();

	bool isOpen() const;
	bool isHealthy() const;
	std::string lastError() const;

	/** Persist opaque serialized report bytes before returning Ok. */
	Status append(const std::string& raw_payload, uint64_t& sequence);

	/** Return the in-memory unresolved list reconstructed by strict WAL scan. */
	Status pending(std::vector<Report>& reports) const;
	/** Status-bearing query; Ok + zero means no pending reports. */
	Status queryPendingHighWater(uint64_t& high_water) const;
	/** Legacy ambiguous convenience query; zero can mean no data or unavailable. */
	uint64_t pendingHighWater() const;
	Status acknowledgements(std::vector<Acknowledgement>& records) const;
	/** Read-only forensic snapshot after recovery-required open or failed mutation. */
	Status recoveryInspection(RecoveryInspection& inspection) const;

	/**
	 * Persist an internal acknowledgement only when expected_high_water exactly
	 * matches the current pending high-water. operator_id/reason are audit fields;
	 * this module does not authenticate the operator. Callers must separately
	 * authenticate and complete a fresh reconciliation before changing account
	 * readiness. This method never resumes trading by itself. An ambiguous write
	 * failure retains a recoveryInspection() snapshot; cleanup failure returns
	 * RecoveryRequired, and earlier transaction failures return StorageFailed even
	 * though recoveryInspection() reports RecoveryRequired.
	 * The caller remains responsible for pausing the account, authenticating the
	 * operator, and completing fresh reconciliation after startup or uncertain
	 * writes. No account recovery UI or authorization is provided here.
	 */
	Status acknowledge(uint64_t expected_high_water,
		const std::string& operator_id, const std::string& reason);

private:
	enum class RecordType : uint16_t;
	enum class GuardRecordType : uint16_t;
	enum class TestFaultPoint
	{
		None,
		FailMarkerFlush,
		FailMarkerCreateDirectoryFlush,
		FailPrepareFlush,
		FailPrimaryPartialWrite,
		FailPrimaryFlush,
		FailCommitFlush,
		FailCleanupMarkerFlush,
		FailCleanupMarkerCreateDirectoryFlush,
		FailMarkerClearDirectoryFlush,
		FailCleanupMarkerRemove,
		FailComponentProbe,
		FailCleanupProbe
	};

	friend struct UnresolvedReportJournalTestAccess;

	Status openLocked(const std::string& stable_path, bool create_new,
		const std::string& bootstrap_evidence_reference, std::string* error);
	Status closeLocked();
	Status scanLocked(std::string* error);
	Status appendGuardedFrameLocked(RecordType type, const std::string& payload,
		uint64_t& sequence);
	Status appendRawFrameLocked(int fd, const uint8_t magic[8], uint16_t version,
		uint16_t type, const std::string& payload, uint64_t sequence,
		TestFaultPoint flush_fault, std::string& frame, uint64_t& offset);
	Status writeInFlightMarkerLocked(RecordType type, uint64_t primary_sequence,
		const std::string& payload);
	Status writeCleanupMarkerLocked(RecordType type, uint64_t primary_sequence,
		const std::string& payload);
	Status clearInFlightMarkerLocked();
	Status clearCleanupMarkerLocked();
	// Existing opens may pass markers through to scanLocked so it can retain a
	// read-only recovery inspection. Normal operations still require their
	// absence; path/descriptor identity and permission checks always run.
	bool validatePathsLocked(bool require_no_markers = true) const;
	Status storageFailureLocked(const std::string& message);
	void releaseHandlesLocked();

	mutable std::mutex _mutex;
	std::string _path;
	std::string _directory_path;
	std::string _wal_name;
	std::string _guard_name;
	std::string _lock_name;
	std::string _inflight_name;
	std::string _cleanup_name;
	std::string _lock_path;
	std::string _guard_path;
	std::string _inflight_path;
	std::string _cleanup_path;
	std::string _last_error;
	std::vector<Report> _pending;
	std::vector<Acknowledgement> _acknowledgements;
	RecoveryInspection _recovery_inspection;
	uint64_t _last_sequence;
	uint64_t _acknowledged_high_water;
	bool _opened;
	bool _healthy;
	bool _created_new;
	bool _has_recovery_inspection;
	uint64_t _guard_sequence;

	int _fd;
	int _guard_fd;
	int _lock_fd;
	int _directory_fd;
	uint64_t _guard_device;
	uint64_t _guard_inode;
	uint64_t _directory_device;
	uint64_t _directory_inode;
	uint64_t _file_device;
	uint64_t _file_inode;
	uint64_t _lock_device;
	uint64_t _lock_inode;
	uint64_t _inflight_device;
	uint64_t _inflight_inode;
	uint64_t _cleanup_device;
	uint64_t _cleanup_inode;

	TestFaultPoint _test_fault_point;
	size_t _test_write_chunk;
	size_t _test_fail_after_write_bytes;
};

NS_WTP_END
