/*!
 * \file OwnerClaimLease.h
 * \brief I03-NEW-089: an executer or HFT context returns its contract claims when
 *        its own lifetime ends.
 *
 * With owner routing enabled every order placed by an executer or an HFT
 * context claims the contract on the account's AccountTradingGate under the
 * owner id ("exec:<name>" / "hft:<name>"). Claims live until releaseOwner();
 * before this lease nothing called it, so a destroyed executer or context kept
 * the contract blocked for every other owner for the rest of the process.
 *
 * The lease is bound when the owner is bound to a trader adapter and returns
 * the owner's claims
 *   - when the owner is rebound to a trader on another account gate (the old
 *     binding ends), and
 *   - when the owner is destroyed.
 *
 * The gate is held weakly: hosts may destroy a non-resident TraderAdapter (and
 * with it the last reference to its gate and claims) before the engine members
 * that own executers and contexts, so the lease never dereferences the adapter
 * itself. Account-level manual claims ("manual") are not touched; how they are
 * released is not decided (see I03-NEW-089).
 */
#pragma once

#include "AccountTradingGate.h"

#include <memory>
#include <mutex>
#include <string>

NS_WTP_BEGIN

class OwnerClaimLease
{
public:
	OwnerClaimLease() = default;
	OwnerClaimLease(const OwnerClaimLease&) = delete;
	OwnerClaimLease& operator=(const OwnerClaimLease&) = delete;

	~OwnerClaimLease()
	{
		release();
	}

	/** Bind to the gate of the owner's current trader; NULL ends the binding. */
	void bind(const std::shared_ptr<AccountTradingGate>& gate, const std::string& owner)
	{
		std::shared_ptr<AccountTradingGate> previous;
		std::string previousOwner;
		{
			std::lock_guard<std::mutex> lock(_mutex);
			previous = _gate.lock();
			previousOwner = _owner;
			_gate = gate;
			_owner = gate ? owner : std::string();
		}
		if (previous && !previousOwner.empty() && (previous != gate || previousOwner != owner))
			previous->releaseOwner(previousOwner);
	}

	/** Return every claim of the bound owner now; the lease is unbound afterwards. */
	void release()
	{
		std::shared_ptr<AccountTradingGate> gate;
		std::string owner;
		{
			std::lock_guard<std::mutex> lock(_mutex);
			gate = _gate.lock();
			owner.swap(_owner);
			_gate.reset();
		}
		if (gate && !owner.empty())
			gate->releaseOwner(owner);
	}

private:
	std::mutex _mutex;
	std::weak_ptr<AccountTradingGate> _gate;
	std::string _owner;
};

NS_WTP_END
