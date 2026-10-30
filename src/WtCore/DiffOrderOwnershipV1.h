/*
 * Durable ownership journal for Diff executor orders.
 *
 * This is an independent persistence primitive. Callers must persist an order
 * intent before sending the order and must provide a stable user tag / intent
 * id on later trade callbacks. It deliberately never infers ownership from an
 * instrument or account position.
 */
#pragma once

#include "../Includes/WTSMarcos.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

NS_WTP_BEGIN

/**
 * Version 1 append-only WAL for per-Diff order ownership.
 *
 * One canonical WAL path belongs to exactly one stable account id and must be
 * shared by all Diff owners for that account. Every owner-scoped event carries
 * a stable owner id. A process-wide journal instance serializes concurrent Diff
 * writers; the file itself is locked exclusively while open so a second
 * process cannot append to the same WAL. Frame checksums detect accidental
 * corruption and are not cryptographic authenticity proofs.
 */
class DiffOrderOwnershipJournalV1
{
public:
	enum class Status
	{
		Ok,
		Duplicate,
		InvalidArgument,
		NotOpen,
		StorageFailed,
		Busy,
		Corrupt,
		UnknownIntent,
		Conflict,
		LocalOrderIdFloorRequired,
		SequenceExhausted
	};

	struct OwnerCodeKey
	{
		std::string owner_id;
		std::string std_code;

		bool operator<(const OwnerCodeKey& other) const
		{
			return owner_id < other.owner_id ||
				(owner_id == other.owner_id && std_code < other.std_code);
		}
	};

	struct PositionState
	{
		double target = 0.0;
		double diff = 0.0;
		double baseline_account_position = 0.0;
		uint32_t baseline_trading_day = 0;
		uint64_t last_sequence = 0;
		bool has_baseline = false;
	};

	struct OrderIntentState
	{
		std::string intent_id;
		std::string owner_id;
		std::string std_code;
		// Signed account-position effect: buys are positive, sells are negative.
		double signed_quantity = 0.0;
		// Broker cumulative fill is kept separate from fill quantity reconstructed
		// from durable, deduplicated trades; filled_quantity is their maximum.
		double reported_filled_quantity = 0.0;
		double trade_filled_quantity = 0.0;
		double filled_quantity = 0.0;
		double remaining_quantity = 0.0;
		// Raw broker remainder; terminal orders retain this audit value while
		// effective remaining_quantity is normalized to zero.
		double reported_remaining_quantity = 0.0;
		uint32_t status_code = 0;
		uint32_t local_order_id = 0;
		std::string broker_order_id;
		uint64_t last_sequence = 0;
		bool terminal = false;
		bool order_bound = false;
		uint32_t terminal_state = 0;
	};

	struct Snapshot
	{
		std::string account_id;
		uint64_t last_sequence = 0;
		uint32_t local_order_id_high_water = 0;
		bool local_order_id_floor_established = false;
		std::string local_order_id_floor_evidence;
		std::map<OwnerCodeKey, PositionState> positions;
		std::map<std::string, OrderIntentState> intents;
	};

	DiffOrderOwnershipJournalV1();
	~DiffOrderOwnershipJournalV1();

	DiffOrderOwnershipJournalV1(const DiffOrderOwnershipJournalV1&) = delete;
	DiffOrderOwnershipJournalV1& operator=(const DiffOrderOwnershipJournalV1&) = delete;

	/**
	 * Open and replay a WAL. A new file receives a durable account header.
	 * Any truncated, malformed, wrong-account, or checksum-invalid record fails
	 * closed; the WAL is never silently truncated or repaired.
	 */
	Status open(const std::string& wal_path, const std::string& stable_account_id,
		std::string* error = nullptr);
	void close();

	bool isOpen() const;
	bool isHealthy() const;
	std::string accountId() const;
	std::string lastError() const;
	Snapshot snapshot() const;

	/**
	 * Persist this record and flush it before the caller submits any order.
	 * Reusing an intent id never authorizes a second send: an exact repeat returns
	 * Duplicate (and the previously reserved local id), while a changed repeat
	 * returns Conflict. The caller may submit only after this method returns Ok.
	 * A Duplicate after restart is for reconciliation and must not be resent.
	 * The returned reserved id must be placed in the order's provider user tag;
	 * never substitute a process-local generated id before orderInsert.
	 */
	Status prepareOrderIntent(const std::string& owner_id,
		const std::string& intent_id, const std::string& std_code,
		double signed_quantity, uint32_t& reserved_local_order_id);

	/**
	 * Before the first new intent, persist the highest local id ever allocated
	 * by any legacy/provider adapter for this account (not only currently active
	 * ids), plus a non-empty reference to the complete inventory/source proving
	 * that maximum. The journal records the reference but cannot verify an
	 * external source. Do not invent a zero floor: only pass zero when the
	 * referenced complete source proves there were no prior ids. If a complete
	 * maximum cannot be established, do not call this method or submit orders;
	 * prepareOrderIntent then returns LocalOrderIdFloorRequired. This one-way
	 * floor cannot be lowered or changed after any intent exists. UINT32_MAX
	 * remains reserved as the adapter failure sentinel.
	 */
	Status advanceLocalOrderIdHighWater(uint32_t observed_high_water,
		const std::string& complete_source_evidence);

	/** Bind an exchange/local order identity to a previously persisted intent. */
	Status bindOrder(const std::string& intent_id, uint32_t local_order_id,
		const std::string& broker_order_id = std::string());

	/**
	 * Persist a cumulative broker order update without losing owner attribution.
	 * After terminal status, only that same terminal status may be refreshed;
	 * reported fill may increase and raw reported remainder may decrease. The
	 * effective remaining_quantity stays zero throughout terminal updates.
	 */
	Status recordOrderProgress(const std::string& intent_id, uint32_t status_code,
		double reported_filled_quantity, double reported_remaining_quantity,
		bool terminal);

	/**
	 * Apply a trade to the Diff state for the order's recorded owner. The trade
	 * key is account-wide and must be stable across push and query callbacks.
	 * A repeated key with identical contents returns Duplicate; mismatched
	 * contents return Conflict and poison the journal.
	 */
	Status applyTrade(const std::string& trade_key, const std::string& intent_id,
		const std::string& std_code, double signed_diff_delta);

	/** Atomically persist an owner target and its corresponding Diff value. */
	Status setTarget(const std::string& owner_id, const std::string& std_code,
		double target, double diff);

	/**
	 * Atomically persist an account-level observed position alongside this
	 * owner's explicitly supplied target and Diff. The journal does not allocate
	 * shared historical account inventory among owners.
	 */
	Status setBaseline(const std::string& owner_id, const std::string& std_code,
		double account_position, double target, double diff,
		uint32_t trading_day);

	/** Resolve only identities explicitly recorded in this WAL. */
	Status ownerForIntent(const std::string& intent_id, std::string& owner_id) const;
	Status ownerForLocalOrder(uint32_t local_order_id, std::string& owner_id,
		std::string* intent_id = nullptr) const;
	Status checkCancelOwner(uint32_t local_order_id, const std::string& owner_id) const;
	/**
	 * Includes an unbound, nonterminal durable intent. Such an entry is an
	 * ambiguous pre-send/recovery state and must be reconciled before sending
	 * another order; callers can inspect order_bound == false. A Duplicate
	 * prepareOrderIntent result is never permission to submit it again.
	 */
	Status activeOrders(const std::string& owner_id, const std::string& std_code,
		std::vector<OrderIntentState>& orders) const;
	Status undoneQuantity(const std::string& owner_id, const std::string& std_code,
		double& signed_quantity) const;

private:
	enum class RecordType : uint16_t;
	enum class TestFaultPoint
	{
		None,
		BeforeWrite,
		BeforeFlush
	};

	struct TradeState
	{
		std::string intent_id;
		std::string owner_id;
		std::string std_code;
		double signed_delta = 0.0;
	};

	Status appendLocked(RecordType type, const std::string& payload,
		uint64_t& committed_sequence);
	Status replayLocked(std::string* error);
	Status applyRecordLocked(RecordType type, const std::string& payload,
		uint64_t sequence, std::string* error);
	Status storageFailureLocked(const std::string& error);
	Status conflictFailureLocked(const std::string& error);
	void closeLocked();

	mutable std::mutex _mutex;
	std::string _path;
	std::string _account_id;
	uint64_t _last_sequence;
	uint32_t _local_order_id_high_water;
	bool _local_order_id_floor_established;
	std::string _local_order_id_floor_evidence;
	bool _opened;
	bool _healthy;
	bool _has_header;
	int _fd;
	void* _win_handle;
	std::map<OwnerCodeKey, PositionState> _positions;
	std::map<std::string, OrderIntentState> _intents;
	std::map<uint32_t, std::string> _local_order_to_intent;
	std::map<std::string, TradeState> _trades;
	TestFaultPoint _test_fault_point;
	std::string _last_error;

	friend struct DiffOrderOwnershipJournalV1TestAccess;
};

NS_WTP_END
