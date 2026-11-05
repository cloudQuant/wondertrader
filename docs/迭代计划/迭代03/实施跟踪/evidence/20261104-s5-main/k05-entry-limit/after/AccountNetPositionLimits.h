/*!
 * \file AccountNetPositionLimits.h
 * \brief Account net-position limit per actual contract (decision card 8,
 *        supplementary ruling 2026-11-03; BUG-P02-005).
 *
 * Contract implemented here:
 * - Every actual contract is limited separately: |account net target| of that
 *   contract must not exceed its limit. Quantities of different contracts are
 *   never added together.
 * - All executers trading through the same account share the judgement for a
 *   contract: the account net target is the sum of every registered owner's
 *   physical target for that contract, in trader quantity (after scale).
 * - The check runs in addition to the executer's group limits (both must pass,
 *   so the stricter one wins); either violation rejects the whole batch.
 * - Limits are declared in an executer configuration as
 *   "account_limits": { "<exchg.code.month>" | "<exchg.product>" | "default": max_abs }.
 *   The most specific key matching a contract applies within one declaration;
 *   when several owners of the same account declare a limit for a contract the
 *   smallest one applies (strictest wins, independent of binding order).
 * - A change that does not increase |account net| of a contract is always
 *   accepted, so an account that became over-limit (e.g. after a stricter
 *   declaration joined) can still be reduced.
 *
 * Account identity is read-only: executers pass the AccountTradingGate object
 * shared by every TraderAdapter bound to the same configured account_id (WP1).
 * Without account_id the gate is private to one adapter, so only executers on
 * that adapter share limits. An executer without a trader forms its own
 * account scope. This is a target-level (planned exposure) gate inside one
 * process; positions changed outside these executers, other processes and
 * in-flight order consistency at the adapter are not covered here.
 */
#pragma once

#include "../Includes/WTSVariant.hpp"
#include "../Share/CodeHelper.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace account_net_limits
{
using wtp::WTSVariant;

struct Limits
{
	std::map<std::string, double> by_key;
	bool empty() const { return by_key.empty(); }
};

/** Pure read of "account_limits"; absent means no declaration. */
inline bool parse(WTSVariant* params, Limits& out, std::string& error)
{
	Limits result;
	WTSVariant* node = params == NULL ? NULL : params->get("account_limits");
	if (node != NULL)
	{
		if (node->type() != WTSVariant::VT_Object)
		{
			error = "account_limits must be an object";
			return false;
		}
		for (const std::string& key : node->memberNames())
		{
			WTSVariant* value = node->get(key.c_str());
			double limit = 0.0;
			bool numeric = false;
			if (value != NULL)
			{
				switch (value->type())
				{
				case WTSVariant::VT_Int32:
				case WTSVariant::VT_Uint32:
				case WTSVariant::VT_Int64:
				case WTSVariant::VT_Uint64:
				case WTSVariant::VT_Real:
					limit = value->asDouble();
					numeric = true;
					break;
				default:
					break;
				}
			}
			if (key.empty() || key.find('\0') != std::string::npos || !numeric ||
				!std::isfinite(limit) || limit < 0.0)
			{
				error = "account_limits entry must name a contract, product or default and be a finite non-negative number";
				return false;
			}
			result.by_key[key] = limit;
		}
	}
	out.by_key.swap(result.by_key);
	return true;
}

/** Most specific declared limit for an actual contract; false when none applies. */
inline bool lookup(const Limits& limits, const std::string& code, double& limit)
{
	auto it = limits.by_key.find(code);
	if (it == limits.by_key.end())
	{
		CodeHelper::CodeInfo info = CodeHelper::extractStdCode(code.c_str(), NULL);
		it = limits.by_key.find(info.stdCommID());
		if (it == limits.by_key.end())
			it = limits.by_key.find("default");
	}
	if (it == limits.by_key.end())
		return false;
	limit = it->second;
	return true;
}

typedef std::map<std::string, double> Positions;

/**
 * Process-wide ledger of published owner targets, partitioned by account.
 * All state is guarded by one leaf mutex: callers may hold their own locks,
 * and nothing here calls back into an executer.
 */
class Registry
{
public:
	static Registry& instance()
	{
		static Registry registry;
		return registry;
	}

	/**
	 * Atomically checks and publishes the owner's complete set of physical
	 * targets (trader quantity) for account `account`. `keepalive` pins the
	 * identity object while the owner is registered so the address cannot be
	 * reused by another account. On success the previous publication of this
	 * owner (in any account) is returned through `previous` for rollback.
	 * `force` publishes without checking; it is used only to make already
	 * committed (e.g. restored) targets visible to the other owners.
	 */
	bool apply(const void* account, const std::shared_ptr<const void>& keepalive,
		const void* owner, const std::string& ownerName, const Limits& ownerLimits,
		const Positions& targets, Positions& previous, const void*& previousAccount,
		std::string& error, bool force = false)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		previous.clear();
		previousAccount = NULL;
		auto ownerIt = _owner_account.find(owner);
		if (ownerIt != _owner_account.end())
		{
			previousAccount = ownerIt->second;
			Account& old = _accounts[previousAccount];
			auto entry = old.owners.find(owner);
			if (entry != old.owners.end())
				previous = entry->second.targets;
		}

		Account& acct = _accounts[account];
		const Positions oldOwn = previousAccount == account ? previous : Positions();
		std::set<std::string> codes;
		for (const auto& item : targets) codes.insert(item.first);
		for (const auto& item : oldOwn) codes.insert(item.first);
		for (const std::string& code : codes)
		{
			double limit = 0.0;
			bool limited = lookup(ownerLimits, code, limit);
			double others = 0.0;
			for (const auto& item : acct.owners)
			{
				if (item.first == owner)
					continue;
				double declared = 0.0;
				if (lookup(item.second.limits, code, declared) && (!limited || declared < limit))
				{
					limit = declared;
					limited = true;
				}
				auto pos = item.second.targets.find(code);
				if (pos != item.second.targets.end())
					others += pos->second;
			}
			if (!limited || force)
				continue;
			auto nextIt = targets.find(code);
			auto prevIt = oldOwn.find(code);
			const double nextNet = others + (nextIt == targets.end() ? 0.0 : nextIt->second);
			const double prevNet = others + (prevIt == oldOwn.end() ? 0.0 : prevIt->second);
			const double tolerance = 1e-9 * std::max(1.0, limit);
			if (!std::isfinite(nextNet) ||
				(std::fabs(nextNet) > limit + tolerance && std::fabs(nextNet) > std::fabs(prevNet) + tolerance))
			{
				error = "account net position of " + code + " would be " + fmtNumber(nextNet) +
					", exceeding the account limit " + fmtNumber(limit) + " (owner " + ownerName + ")";
				return false;
			}
		}

		if (previousAccount != NULL && previousAccount != account)
			eraseOwnerLocked(previousAccount, owner);
		Owner& self = acct.owners[owner];
		self.limits = ownerLimits;
		self.targets.clear();
		for (const auto& item : targets)
			if (item.second != 0.0)
				self.targets[item.first] = item.second;
		if (keepalive)
			acct.keepalive = keepalive;
		_owner_account[owner] = account;
		return true;
	}

	/** Restores a publication returned by apply() when the caller could not commit. */
	void restore(const void* account, const std::shared_ptr<const void>& keepalive,
		const void* owner, const Limits& ownerLimits, const Positions& previous,
		const void* previousAccount)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		eraseOwnerLocked(account, owner);
		_owner_account.erase(owner);
		if (previousAccount == NULL)
			return;
		Account& acct = _accounts[previousAccount];
		Owner& self = acct.owners[owner];
		self.limits = ownerLimits;
		self.targets = previous;
		if (previousAccount == account && keepalive)
			acct.keepalive = keepalive;
		_owner_account[owner] = previousAccount;
	}

	/** Removes every publication of an owner (executer destruction). */
	void release(const void* owner)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		auto it = _owner_account.find(owner);
		if (it == _owner_account.end())
			return;
		eraseOwnerLocked(it->second, owner);
		_owner_account.erase(it);
	}

	/**
	 * Decision K05=C (2026-11-04): strictest limit any owner of `account` declared for
	 * `code`, for the adapter's order-entry check. Returns false when none applies.
	 */
	bool limitFor(const void* account, const std::string& code, double& limit) const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		auto acct = _accounts.find(account);
		if (acct == _accounts.end())
			return false;
		bool limited = false;
		for (const auto& item : acct->second.owners)
		{
			double declared = 0.0;
			if (lookup(item.second.limits, code, declared) && (!limited || declared < limit))
			{
				limit = declared;
				limited = true;
			}
		}
		return limited;
	}

	/** Diagnostic/test read of the published account net target of a contract. */
	double accountNet(const void* account, const std::string& code) const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		auto acct = _accounts.find(account);
		if (acct == _accounts.end())
			return 0.0;
		double net = 0.0;
		for (const auto& item : acct->second.owners)
		{
			auto pos = item.second.targets.find(code);
			if (pos != item.second.targets.end())
				net += pos->second;
		}
		return net;
	}

	/** Diagnostic/test read of one owner's published target. */
	double ownerTarget(const void* owner, const std::string& code) const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		auto it = _owner_account.find(owner);
		if (it == _owner_account.end())
			return 0.0;
		auto acct = _accounts.find(it->second);
		if (acct == _accounts.end())
			return 0.0;
		auto self = acct->second.owners.find(owner);
		if (self == acct->second.owners.end())
			return 0.0;
		auto pos = self->second.targets.find(code);
		return pos == self->second.targets.end() ? 0.0 : pos->second;
	}

private:
	struct Owner
	{
		Limits limits;
		Positions targets;
	};
	struct Account
	{
		std::shared_ptr<const void> keepalive;
		std::map<const void*, Owner> owners;
	};

	static std::string fmtNumber(double value)
	{
		char buffer[64];
		std::snprintf(buffer, sizeof(buffer), "%.10g", value);
		return buffer;
	}

	void eraseOwnerLocked(const void* account, const void* owner)
	{
		auto acct = _accounts.find(account);
		if (acct == _accounts.end())
			return;
		acct->second.owners.erase(owner);
		if (acct->second.owners.empty())
			_accounts.erase(acct);
	}

	mutable std::mutex _mutex;
	std::map<const void*, Account> _accounts;
	std::map<const void*, const void*> _owner_account;
};
}
