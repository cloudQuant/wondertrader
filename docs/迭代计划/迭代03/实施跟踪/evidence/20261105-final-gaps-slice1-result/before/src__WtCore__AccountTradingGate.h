/*!
 * \file AccountTradingGate.h
 * \brief Account-level trading admission gate (decision cards 5/6, 2026-11-04).
 *
 * One gate instance is shared by every TraderAdapter bound to the same
 * configured account key in this process, and therefore by every executer
 * that trades through any of those adapters. Any pause source (unresolved
 * report, identity recovery failure, persistence failure, ...) closes the
 * gate for the whole account. The gate is reopened only by an explicit
 * resume request (operator, reason and the observed pause generation) that
 * is first recorded in a durable, append-only audit log. There is no
 * timed/automatic resume.
 *
 * Limits: the account key is a configuration value; it is not a verified
 * physical broker account identity. Other processes or external clients
 * trading the same account are outside this gate. The audit log uses
 * write+fsync on POSIX; Windows durability is unverified.
 */
#pragma once

#include "../Includes/WTSMarcos.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

NS_WTP_BEGIN

class AccountTradingGate
{
public:
	struct PauseRecord
	{
		uint64_t generation = 0;
		std::string source;
		std::string reason;
		uint64_t wall_ms = 0;
	};

	struct ResumeRequest
	{
		std::string operator_id;
		std::string reason;
		// Generation the operator reviewed; a pause recorded after that review
		// makes the request stale and keeps the account paused.
		uint64_t expected_generation = 0;
		// Free-text reference to the reconciliation evidence (ticket, file ...).
		std::string reconciliation_reference;
	};

	enum class ResumeStatus
	{
		Ok,
		NotPaused,
		InvalidRequest,
		StaleGeneration,
		AuditUnavailable,
		AuditWriteFailed,
		ParticipantRejected
	};

	// A participant validates/acknowledges its own state while the gate mutex
	// is held. It must not call back into the gate. Returning false keeps the
	// whole account paused; diagnostic explains why.
	typedef std::function<bool(const ResumeRequest&, std::string& diagnostic)> Participant;

	explicit AccountTradingGate(const std::string& key) : _key(key) {}
	~AccountTradingGate()
	{
#ifndef _WIN32
		if (_audit_fd >= 0)
			::close(_audit_fd);
#endif
	}
	AccountTradingGate(const AccountTradingGate&) = delete;
	AccountTradingGate& operator=(const AccountTradingGate&) = delete;

	/** Process-wide registry: adapters with the same key share one gate. */
	static std::shared_ptr<AccountTradingGate> acquireShared(const std::string& key)
	{
		Registry& reg = registry();
		std::lock_guard<std::mutex> lock(reg.mutex);
		auto it = reg.gates.find(key);
		if (it != reg.gates.end())
		{
			std::shared_ptr<AccountTradingGate> existing = it->second.lock();
			if (existing)
				return existing;
		}
		std::shared_ptr<AccountTradingGate> gate = std::make_shared<AccountTradingGate>(key);
		reg.gates[key] = gate;
		return gate;
	}

	const std::string& key() const noexcept { return _key; }

	/** Lock-free admission check used before every order/cancel entrance. */
	bool paused() const noexcept
	{
		return _pending_pauses.load(std::memory_order_acquire) != 0 ||
			_paused.load(std::memory_order_acquire);
	}

	uint64_t generation() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _generation;
	}

	/**
	 * Close the gate. Admission is blocked before this call takes the mutex,
	 * so a concurrent resume can never reopen over a pause in progress.
	 * Returns the new pause generation.
	 */
	uint64_t pause(const std::string& source, const std::string& reason)
	{
		_pending_pauses.fetch_add(1, std::memory_order_acq_rel);
		uint64_t gen = 0;
		try
		{
			std::lock_guard<std::mutex> lock(_mutex);
			_paused.store(true, std::memory_order_release);
			gen = ++_generation;
			PauseRecord record;
			record.generation = gen;
			record.source = source;
			record.reason = reason;
			record.wall_ms = nowMs();
			_active_pauses.push_back(record);
			// Best effort audit; a failed write never reopens the gate.
			std::string ignored;
			appendAuditLocked(auditLine("pause", gen, source, reason, std::string(), std::string()), ignored);
		}
		catch (...)
		{
			_paused.store(true, std::memory_order_release);
		}
		_pending_pauses.fetch_sub(1, std::memory_order_acq_rel);
		return gen;
	}

	std::vector<PauseRecord> activePauses() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _active_pauses;
	}

	std::vector<std::string> auditRecords() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _audit_memory;
	}

	std::string lastDiagnostic() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _diagnostic;
	}

	/**
	 * Configure the durable audit log (append-only, created 0600). A second
	 * adapter on the same account must name the same path.
	 */
	bool configureAudit(const std::string& path, std::string& error)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (path.empty())
		{
			error = "account audit log path is empty";
			return false;
		}
		if (!_audit_path.empty())
		{
			if (_audit_path == path)
				return true;
			error = "account audit log path conflicts with " + _audit_path;
			return false;
		}
#ifdef _WIN32
		error = "account audit log is unsupported on this platform";
		return false;
#else
		int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
		if (fd < 0)
		{
			error = std::string("account audit log open failed: ") + std::strerror(errno);
			return false;
		}
		_audit_fd = fd;
		_audit_path = path;
		// Record earlier in-memory pauses so the durable log is complete.
		for (const PauseRecord& record : _active_pauses)
		{
			std::string ignored;
			appendAuditLocked(auditLine("pause", record.generation, record.source, record.reason,
				std::string(), std::string()), ignored);
		}
		return true;
#endif
	}

	bool auditConfigured() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _audit_fd >= 0;
	}

	void addParticipant(const void* owner, const Participant& participant)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_participants[owner] = participant;
	}

	void removeParticipant(const void* owner)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_participants.erase(owner);
	}

	/**
	 * Explicit manual resume. The request is durably audited before any
	 * participant acknowledges, and the gate opens only after the "resumed"
	 * record is durable. Any failure keeps the account paused.
	 */
	ResumeStatus resume(const ResumeRequest& request, std::string* diagnostic = nullptr)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		auto finish = [&](ResumeStatus status, const std::string& text) {
			_diagnostic = text;
			if (diagnostic)
				*diagnostic = text;
			return status;
		};
		if (!_paused.load(std::memory_order_acquire))
			return finish(ResumeStatus::NotPaused, "account is not paused");
		if (request.operator_id.empty() || request.reason.empty())
			return finish(ResumeStatus::InvalidRequest, "operator_id and reason are required");
		if (_audit_fd < 0)
			return finish(ResumeStatus::AuditUnavailable, "account audit log is not configured");
		if (request.expected_generation != _generation ||
			_pending_pauses.load(std::memory_order_acquire) != 0)
		{
			std::string ignored;
			appendAuditLocked(auditLine("resume_rejected", _generation, request.operator_id,
				"stale generation", request.reason, request.reconciliation_reference), ignored);
			return finish(ResumeStatus::StaleGeneration, "pause generation changed since review");
		}
		std::string error;
		if (!appendAuditLocked(auditLine("resume_requested", _generation, request.operator_id,
			std::string(), request.reason, request.reconciliation_reference), error))
			return finish(ResumeStatus::AuditWriteFailed, error);
		for (auto& item : _participants)
		{
			std::string text;
			bool ok = false;
			try { ok = item.second(request, text); }
			catch (...) { ok = false; text = "participant exception"; }
			if (!ok)
			{
				std::string ignored;
				appendAuditLocked(auditLine("resume_rejected", _generation, request.operator_id,
					text, request.reason, request.reconciliation_reference), ignored);
				return finish(ResumeStatus::ParticipantRejected, text);
			}
		}
		if (_pending_pauses.load(std::memory_order_acquire) != 0)
			return finish(ResumeStatus::StaleGeneration, "pause arrived during resume");
		if (!appendAuditLocked(auditLine("resumed", _generation, request.operator_id,
			std::string(), request.reason, request.reconciliation_reference), error))
			return finish(ResumeStatus::AuditWriteFailed, error);
		_active_pauses.clear();
		_paused.store(false, std::memory_order_release);
		return finish(ResumeStatus::Ok, "resumed by " + request.operator_id);
	}

	/**
	 * Account-scoped shared resource (e.g. the order identity WAL, which must
	 * have exactly one writer per account). factory runs once, under the gate
	 * mutex; it must not call back into the gate.
	 */
	std::shared_ptr<void> attachment(const std::string& name, const std::function<std::shared_ptr<void>()>& factory)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		auto it = _attachments.find(name);
		if (it != _attachments.end())
			return it->second;
		std::shared_ptr<void> value = factory();
		_attachments[name] = value;
		return value;
	}

	/**
	 * Card 5b/5c contract ownership on this account. An exclusive (non-shared)
	 * owner must be the only owner of the contract; shared owners may coexist
	 * with each other but never with another owner's exclusive claim. Claims
	 * live until releaseOwner(). Returns false and a diagnostic on conflict.
	 */
	bool claimContract(const std::string& code, const std::string& owner, bool shared, std::string& diagnostic)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		ContractClaims& claims = _claims[code];
		auto self = claims.owners.find(owner);
		for (const auto& item : claims.owners)
		{
			if (item.first == owner)
				continue;
			if (!shared || !item.second)
			{
				diagnostic = "contract " + code + " on account " + _key + " is " +
					(item.second ? "shared-owned" : "exclusively owned") + " by " + item.first +
					"; " + owner + " requested " + (shared ? "shared" : "exclusive") + " ownership";
				return false;
			}
		}
		if (self != claims.owners.end() && self->second != shared)
		{
			diagnostic = "owner " + owner + " cannot change its ownership mode on " + code;
			return false;
		}
		claims.owners[owner] = shared;
		return true;
	}

	void releaseOwner(const std::string& owner)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		for (auto it = _claims.begin(); it != _claims.end();)
		{
			it->second.owners.erase(owner);
			if (it->second.owners.empty())
				it = _claims.erase(it);
			else
				++it;
		}
	}

	/** Test hook: make subsequent audit writes fail (simulated storage error). */
	void failAuditWritesForTest(bool fail)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_fail_audit_for_test = fail;
	}

private:
	struct Registry
	{
		std::mutex mutex;
		std::map<std::string, std::weak_ptr<AccountTradingGate>> gates;
	};
	static Registry& registry()
	{
		static Registry instance;
		return instance;
	}

	static uint64_t nowMs()
	{
		return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
	}

	static void escape(std::string& out, const std::string& value)
	{
		static const char* hex = "0123456789abcdef";
		for (unsigned char ch : value)
		{
			if (ch == '"' || ch == '\\') { out.push_back('\\'); out.push_back((char)ch); }
			else if (ch < 0x20) { out += "\\u00"; out.push_back(hex[ch >> 4]); out.push_back(hex[ch & 15]); }
			else out.push_back((char)ch);
		}
	}

	std::string auditLine(const char* event, uint64_t generation, const std::string& actor,
		const std::string& detail, const std::string& reason, const std::string& reference)
	{
		std::string line = "{\"schema\":1,\"seq\":";
		line += std::to_string(++_audit_sequence);
		line += ",\"wall_ms\":" + std::to_string(nowMs());
		line += ",\"account\":\""; escape(line, _key);
		line += "\",\"event\":\""; line += event;
		line += "\",\"generation\":" + std::to_string(generation);
		line += ",\"actor\":\""; escape(line, actor);
		line += "\",\"detail\":\""; escape(line, detail);
		line += "\",\"reason\":\""; escape(line, reason);
		line += "\",\"reference\":\""; escape(line, reference);
		line += "\"}\n";
		return line;
	}

	bool appendAuditLocked(const std::string& line, std::string& error)
	{
		_audit_memory.push_back(line);
		if (_audit_fd < 0)
		{
			error = "account audit log is not configured";
			return false;
		}
		if (_fail_audit_for_test)
		{
			error = "account audit log write failed (injected)";
			return false;
		}
#ifdef _WIN32
		error = "account audit log is unsupported on this platform";
		return false;
#else
		size_t written = 0;
		while (written < line.size())
		{
			ssize_t n = ::write(_audit_fd, line.data() + written, line.size() - written);
			if (n < 0)
			{
				if (errno == EINTR)
					continue;
				error = std::string("account audit log write failed: ") + std::strerror(errno);
				return false;
			}
			written += (size_t)n;
		}
		if (::fsync(_audit_fd) != 0)
		{
			error = std::string("account audit log fsync failed: ") + std::strerror(errno);
			return false;
		}
		return true;
#endif
	}

	const std::string _key;
	mutable std::mutex _mutex;
	std::atomic<bool> _paused{false};
	std::atomic<uint32_t> _pending_pauses{0};
	uint64_t _generation = 0;
	uint64_t _audit_sequence = 0;
	std::vector<PauseRecord> _active_pauses;
	std::vector<std::string> _audit_memory;
	std::map<const void*, Participant> _participants;
	std::map<std::string, std::shared_ptr<void>> _attachments;
	struct ContractClaims
	{
		std::map<std::string, bool> owners;	///< owner -> shared
	};
	std::map<std::string, ContractClaims> _claims;
	std::string _audit_path;
	std::string _diagnostic;
	int _audit_fd = -1;
	bool _fail_audit_for_test = false;
};

NS_WTP_END
