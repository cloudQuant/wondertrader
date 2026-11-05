# K05 multi-adapter in-flight + atomic reservation (BUG-P02-005/BUG-R202-003),
# K03 host side (I03-NEW-005), checked clock for local order ids (I03-NEW-006).
ROOT='/Users/yunjinqi/Documents/new_projects/wondertrader/src/'
def edit_crlf(rel, pairs):
    p=ROOT+rel; raw=open(p,'rb').read(); bom=raw.startswith(b'\xef\xbb\xbf')
    s=raw.decode('utf-8-sig'); assert '\n' not in s.replace('\r\n','')
    s=s.replace('\r\n','\n')
    for old,new in pairs:
        assert s.count(old)==1,(rel,old[:90]); s=s.replace(old,new)
    out=s.replace('\n','\r\n').encode('utf-8')
    open(p,'wb').write((b'\xef\xbb\xbf' if bom else b'')+out)
def edit_bytes(rel, pairs):
    p=ROOT+rel; b=open(p,'rb').read()
    for old,new in pairs:
        old=old.encode(); new=new.encode()
        assert b.count(old)==1,(rel,old[:90]); b=b.replace(old,new)
    open(p,'wb').write(b)

# ---------------- AccountTradingGate.h (LF) ----------------
edit_bytes('WtCore/AccountTradingGate.h', [
 ('#include <atomic>\n#include <chrono>\n#include <cstdint>\n', '#include <atomic>\n#include <chrono>\n#include <cmath>\n#include <cstdint>\n'),
 ('#include <string>\n#include <vector>\n', '#include <string>\n#include <unordered_map>\n#include <vector>\n'),
 ('''	void removeParticipant(const void* owner)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_participants.erase(owner);
	}
''','''	void removeParticipant(const void* owner)
	{
		{
			std::lock_guard<std::mutex> lock(_mutex);
			_participants.erase(owner);
		}
		clearInflight(owner);
	}

	// Decision K05=C (BUG-P02-005/BUG-R202-003, slice1 final gaps): account-wide
	// in-flight quantity and atomic entry reservations. Every adapter bound to this
	// account reports the signed change of its undone quantity per contract; the
	// entry re-check of the account net limit reads the sum over all participants
	// plus the reservations of entries that passed the check and have not reached
	// their ledger yet, and reserves its own quantity in the same locked step.
	// _inflight_mutex is a leaf lock: it is never held while calling out of the gate.
	struct EntryCheck
	{
		bool allowed = false;
		double current = 0;		///< position + account in-flight + pending reservations
		double projected = 0;	///< current + this entry
		uint64_t token = 0;		///< reservation to release once the order is in the ledger
	};

	void adjustInflight(const void* participant, const std::string& code, double signedDelta)
	{
		if (signedDelta == 0)
			return;
		std::lock_guard<std::mutex> lock(_inflight_mutex);
		auto& byParticipant = _inflight[code];
		double& value = byParticipant[participant];
		value += signedDelta;
		if (std::fabs(value) < 1e-12)
		{
			byParticipant.erase(participant);
			if (byParticipant.empty())
				_inflight.erase(code);
		}
	}

	void clearInflight(const void* participant)
	{
		std::lock_guard<std::mutex> lock(_inflight_mutex);
		for (auto it = _inflight.begin(); it != _inflight.end();)
		{
			it->second.erase(participant);
			if (it->second.empty())
				it = _inflight.erase(it);
			else
				++it;
		}
	}

	double inflightTotal(const std::string& code) const
	{
		std::lock_guard<std::mutex> lock(_inflight_mutex);
		return inflightLocked(code);
	}

	EntryCheck reserveEntry(const std::string& code, double position, double signedQty, double limit, double tolerance)
	{
		EntryCheck check;
		std::lock_guard<std::mutex> lock(_inflight_mutex);
		check.current = position + inflightLocked(code);
		check.projected = check.current + signedQty;
		if (std::fabs(check.projected) > limit + tolerance && std::fabs(check.projected) > std::fabs(check.current) + tolerance)
			return check;
		check.allowed = true;
		check.token = ++_entry_sequence;
		_pending_entries[check.token] = std::make_pair(code, signedQty);
		return check;
	}

	void releaseEntry(uint64_t token)
	{
		if (token == 0)
			return;
		std::lock_guard<std::mutex> lock(_inflight_mutex);
		_pending_entries.erase(token);
	}
'''),
 ('''	const std::string _key;
	mutable std::mutex _mutex;
''','''	double inflightLocked(const std::string& code) const
	{
		double total = 0;
		auto it = _inflight.find(code);
		if (it != _inflight.end())
			for (const auto& item : it->second)
				total += item.second;
		for (const auto& pending : _pending_entries)
			if (pending.second.first == code)
				total += pending.second.second;
		return total;
	}

	const std::string _key;
	mutable std::mutex _mutex;
	mutable std::mutex _inflight_mutex;	///< leaf lock of the K05 in-flight accounting
	std::unordered_map<std::string, std::unordered_map<const void*, double>> _inflight;
	std::unordered_map<uint64_t, std::pair<std::string, double>> _pending_entries;
	uint64_t _entry_sequence = 0;
'''),
])

# ---------------- TraderAdapter.h (mixed endings: match exact bytes) ----------------
p=ROOT+'WtCore/TraderAdapter.h'; b=open(p,'rb').read()
old=b'\t\tdouble trade_filled = 0;\t///< sum of distinct trade reports\r\n'
if b.count(old)!=1:
    old=b'\t\tdouble trade_filled = 0;\t///< sum of distinct trade reports\n'
assert b.count(old)==1
eol=b'\r\n' if old.endswith(b'\r\n') else b'\n'
new=old+(b'\t\t// Decision K03 (I03-NEW-005): closed after a synchronous provider failure.'+eol+
         b'\t\tbool not_sent = false;'+eol+
         b'\t\tbool not_sent_evidence = false;\t///< a later live/traded report was seen (account paused once)'+eol)
b=b.replace(old,new)
old2=b'\tbool ledgerApplyOrderLocked(uint32_t localid, const std::string& stdCode, bool isBuy,'
assert b.count(old2)==1
i=b.index(old2); j=b.rfind(b'\n',0,i)+1
line_eol=b'\r\n' if b[i-2:i]==b'\r\n' or b[j-2:j]==b'\r\n' else b'\n'
ins=(b'\t// Decision K03 (I03-NEW-005): live or traded evidence for an order recorded as NOT_SENT'+line_eol+
     b'\t// pauses the account (the submission outcome is unknown).'+line_eol+
     b'\tvoid checkNotSentEvidence(uint32_t localid, const char* what);'+line_eol)
b=b[:j]+ins+b[j:]
open(p,'wb').write(b)

# ---------------- TraderAdapter.cpp (CRLF) ----------------
edit_crlf('WtCore/TraderAdapter.cpp', [
 ('''uint32_t makeLocalOrderID()
{
	static std::atomic<uint32_t> _auto_order_id{ 0 };
	// Local ID 0 is reserved as the no-order sentinel; keep initialization state separate.
	static std::once_flag _auto_order_id_once;
	std::call_once(_auto_order_id_once, []() {
		uint32_t curYear = TimeUtils::getCurDate() / 10000 * 10000 + 101;
		uint32_t seed = (uint32_t)((TimeUtils::getLocalTimeNow() - TimeUtils::makeTime(curYear, 0)) / 1000 * 50);
		if (seed == 0)
			seed = 1;
		_auto_order_id.store(seed, std::memory_order_relaxed);
	});

	return _auto_order_id.fetch_add(1, std::memory_order_relaxed);
}
''','''// I03-NEW-006 (slice1 final gaps): the process-wide id seed comes from the checked
// clock snapshot of the first entry that has one. Until a seed exists no id is
// allocated (0 is the no-order sentinel), so a failed clock never freezes a seed
// derived from failed values and a later entry retries. The sequence skips 0 on wrap.
static uint32_t makeLocalOrderID(uint32_t curDate, uint64_t epochMillis)
{
	static std::atomic<uint32_t> _auto_order_id{ 0 };
	static std::atomic<bool> _seeded{ false };
	static std::mutex _seed_mutex;
	if (!_seeded.load(std::memory_order_acquire))
	{
		std::lock_guard<std::mutex> lock(_seed_mutex);
		if (!_seeded.load(std::memory_order_relaxed))
		{
			const int64_t yearStart = TimeUtils::makeTime((long)(curDate / 10000 * 10000 + 101), 0);
			if (yearStart <= 0 || (int64_t)epochMillis < yearStart)
				return 0;
			uint32_t seed = (uint32_t)(((int64_t)epochMillis - yearStart) / 1000 * 50);
			if (seed == 0)
				seed = 1;
			_auto_order_id.store(seed, std::memory_order_relaxed);
			_seeded.store(true, std::memory_order_release);
		}
	}

	uint32_t id = _auto_order_id.fetch_add(1, std::memory_order_relaxed);
	if (id == 0)
		id = _auto_order_id.fetch_add(1, std::memory_order_relaxed);
	return id;
}
'''),
 ('''	{
		double accountLimit = 0.0;
		if (account_net_limits::Registry::instance().limitFor(_account_gate.get(), riskCode, accountLimit))
		{
			const double current = getPosition(riskCode.c_str(), false, 3) + getUndoneQty(riskCode.c_str());
			const double projected = current + (isBuy ? qty : -qty);
			const double tolerance = 1e-9 * (std::max)(1.0, accountLimit);
			if (std::fabs(projected) > accountLimit + tolerance && std::fabs(projected) > std::fabs(current) + tolerance)
			{
				WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR,
					"[{}] entrust rejected: {} net position {} plus in-flight would become {}, exceeding the account limit {}",
					_id.c_str(), riskCode.c_str(), getPosition(riskCode.c_str(), false, 3), projected, accountLimit);
				return UINT_MAX;
			}
		}
	}
''','''	// Slice1 final gaps: "in flight" is account-wide (every adapter bound to this
	// account_id reports its undone quantity to the shared gate), and the check
	// reserves this entry's quantity atomically, so concurrent entries of one or
	// several adapters cannot all pass. The reservation is released once the order
	// is counted in the ledger, or on any early return.
	struct AccountEntryReservation
	{
		std::shared_ptr<AccountTradingGate> gate;
		uint64_t token = 0;
		void release()
		{
			if (gate && token != 0)
				gate->releaseEntry(token);
			token = 0;
		}
		~AccountEntryReservation() { release(); }
	} accountEntry{ _account_gate };
	{
		double accountLimit = 0.0;
		if (account_net_limits::Registry::instance().limitFor(_account_gate.get(), riskCode, accountLimit))
		{
			const double position = getPosition(riskCode.c_str(), false, 3);
			const double tolerance = 1e-9 * (std::max)(1.0, accountLimit);
			const AccountTradingGate::EntryCheck check = accountEntry.gate->reserveEntry(riskCode, position,
				isBuy ? qty : -qty, accountLimit, tolerance);
			if (!check.allowed)
			{
				WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR,
					"[{}] entrust rejected: {} net position {} plus account in-flight would become {}, exceeding the account limit {}",
					_id.c_str(), riskCode.c_str(), position, check.projected, accountLimit);
				return UINT_MAX;
			}
			accountEntry.token = check.token;
		}
	}
'''),
 ('''	else
		localid = makeLocalOrderID();
''','''	else
	{
		// I03-NEW-006 (slice1 final gaps): without an identity journal the local id
		// needs one checked clock snapshot; a clock or time-zone failure rejects the
		// entry before anything is reserved or sent (fail-closed), and a later entry
		// retries once the clock is available.
		uint32_t clockDate = 0;
		uint32_t clockTime = 0;
		uint64_t clockMillis = 0;
		if (!_order_clock(clockDate, clockTime, clockMillis) || (localid = makeLocalOrderID(clockDate, clockMillis)) == 0)
		{
			WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR,
				"[{}] entrust of {} rejected: the local clock or time zone is unavailable, no order id allocated",
				_id.c_str(), riskCode.c_str());
			return UINT_MAX;
		}
	}
'''),
 ('''		entry.volume = qty;
		OrderLedgerEntry& stored = _ledger[localid] = entry;
		ledgerRecomputeLocked(stored);
	}
''','''		entry.volume = qty;
		OrderLedgerEntry& stored = _ledger[localid] = entry;
		ledgerRecomputeLocked(stored);
	}
	// K05: the order is now counted in the account in-flight sum.
	accountEntry.release();
'''),
 ('''			if (it != _ledger.end() && it->second.intent_id == intentId && it->second.state != OrderLedgerState::Terminal)
				ledgerTombstoneLocked(localid, it->second, OrderLedgerState::Terminal);
		}
		journalTerminal(localid, intentId, IDENTITY_STATUS_NOT_SENT, 0);
	};
''','''			if (it != _ledger.end() && it->second.intent_id == intentId && it->second.state != OrderLedgerState::Terminal)
			{
				it->second.not_sent = true;	// K03: later live/traded evidence pauses the account
				ledgerTombstoneLocked(localid, it->second, OrderLedgerState::Terminal);
			}
		}
		journalTerminal(localid, intentId, IDENTITY_STATUS_NOT_SENT, 0);
	};
'''),
 ('''		const bool canceled = orderInfo->getOrderState() == WOS_Canceled;
		noteInflightReport(localid, canceled ? INFLIGHT_REJECTED : INFLIGHT_ACCEPTED);
''','''		const bool canceled = orderInfo->getOrderState() == WOS_Canceled;
		noteInflightReport(localid, canceled ? INFLIGHT_REJECTED : INFLIGHT_ACCEPTED);
		if (orderInfo->isAlive() || orderInfo->getVolTraded() > 0)
			checkNotSentEvidence(localid, "live order report");
'''),
 ('''	if (taggedId == UINT32_MAX)
	{
		quarantineTrade(tradeRecord, "malformed local order tag");
		return;
	}
	tradeRecord->setCode(cInfo->getCode());
''','''	if (taggedId == UINT32_MAX)
	{
		quarantineTrade(tradeRecord, "malformed local order tag");
		return;
	}
	if (taggedId != 0)
		checkNotSentEvidence(taggedId, "trade");
	tradeRecord->setCode(cInfo->getCode());
'''),
 ('''	double& undone = _undone_qty[stdCode];
	double oldQty = undone;
	undone += qty;
''','''	double& undone = _undone_qty[stdCode];
	double oldQty = undone;
	undone += qty;
	// K05 (slice1 final gaps): the account gate sums the in-flight quantity of all
	// adapters bound to the account.
	if (_account_gate)
		_account_gate->adjustInflight(this, stdCode, qty);
'''),
 ('''		if (previous)
		{
			previous->removeParticipant(this);
			if (previous->paused())
				gate->pause(_id + ":carried", "pause carried from previous binding " + previous->key());
		}
		_account_gate = gate;
	}
''','''		if (previous)
		{
			previous->removeParticipant(this);
			if (previous->paused())
				gate->pause(_id + ":carried", "pause carried from previous binding " + previous->key());
		}
		{
			// K05: carry this adapter's in-flight quantity to the account gate.
			std::lock_guard<std::mutex> lock(_ledger_mutex);
			if (previous)
				previous->clearInflight(this);
			for (const auto& item : _undone_qty)
				gate->adjustInflight(this, item.first, item.second);
			_account_gate = gate;
		}
	}
'''),
 ('''bool TraderAdapter::isUnresolvedReportPaused() const noexcept
''','''void TraderAdapter::checkNotSentEvidence(uint32_t localid, const char* what)
{
	bool pause = false;
	{
		std::lock_guard<std::mutex> lock(_ledger_mutex);
		auto it = _ledger.find(localid);
		if (it != _ledger.end() && it->second.not_sent && !it->second.not_sent_evidence)
		{
			it->second.not_sent_evidence = true;
			pause = true;
		}
	}
	if (pause)
		pauseAccount("identity", fmt::format("order {} was closed as not sent after a synchronous provider failure, "
			"but a {} was received for it; the submission outcome is unknown", localid, what ? what : "report"));
}

bool TraderAdapter::isUnresolvedReportPaused() const noexcept
'''),
])
print('applied trader patch')
