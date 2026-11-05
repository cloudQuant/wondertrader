/*!
 * \file TraderAdapter.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "EventNotifier.h"
#include "WtLocalExecuter.h"
#include "TraderAdapter.h"
#include "UnresolvedReportJournal.h"
#include "DiffOrderOwnershipV1.h"
#include "ActionPolicyMgr.h"
#include "WtHelper.h"
#include "ITrdNotifySink.h"
#include "../Includes/RiskMonDefs.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <random>
#include <limits>
#include <mutex>

#include "../WTSTools/WTSLogger.h"

#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSTradeDef.hpp"
#include "../Includes/WTSRiskDef.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/WTSVersion.h"

#include "../Share/decimal.h"
#include "../Share/TimeUtils.hpp"
#include "../Share/CodeHelper.hpp"
#include "../Share/Converter.hpp"

#include <exception>
#include <iomanip>
#include <locale>
#include <sstream>
#ifdef WT_OWNER_ROUTING_TEST_HOOKS
// Test-only barrier after route lookup, before the per-registration lease.
extern void wtOwnerRoutingTestAfterLookup();
#endif

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/writer.h>

#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;

namespace rj = rapidjson;

namespace
{
struct SinkCallbackFrame
{
	SinkCallbackFrame(const TraderAdapter* value, SinkCallbackFrame* previousFrame)
		: adapter(value), previous(previousFrame) {}

	const TraderAdapter* adapter;
	SinkCallbackFrame* previous;
};

thread_local SinkCallbackFrame* g_sink_callback_frame = NULL;
// Order-entry context of the current thread (owner and pre-send observer).
thread_local TraderAdapter::OwnerScope::Frame* g_owner_scope = NULL;
thread_local TraderAdapter::EntrustObserverScope::Frame* g_entrust_observer = NULL;

// A sink callback starts a fresh order-entry context: an entry made from a
// callback (e.g. an HFT strategy reacting to an executer's report) must never
// inherit the owner or observer of the entry whose report is being delivered.
struct EntryContextIsolation
{
	TraderAdapter::OwnerScope::Frame* owner;
	TraderAdapter::EntrustObserverScope::Frame* observer;
	EntryContextIsolation() : owner(g_owner_scope), observer(g_entrust_observer)
	{
		g_owner_scope = NULL;
		g_entrust_observer = NULL;
	}
	~EntryContextIsolation()
	{
		g_owner_scope = owner;
		g_entrust_observer = observer;
	}
};

uint64_t allocateOwnerRegistryInstanceId()
{
	static std::atomic<uint64_t> next_id(1);
	uint64_t candidate = next_id.load(std::memory_order_relaxed);
	while (candidate != 0)
	{
		const uint64_t next = candidate + 1;
		if (next_id.compare_exchange_weak(candidate, next,
			std::memory_order_relaxed, std::memory_order_relaxed))
			return candidate;
	}
	return 0;
}

bool canFormatOrderPattern(const char* id)
{
	if (id == NULL)
		return false;

	constexpr std::size_t userTagSize = sizeof(((WTSEntrust*)NULL)->m_strUserTag);
	constexpr std::size_t localIdDigits = std::numeric_limits<uint32_t>::digits10 + 1;
	constexpr std::size_t fixedTagBytes = (sizeof("otp.") - 1) + 1 + localIdDigits + 1;
	static_assert(fixedTagBytes < userTagSize, "WTSEntrust user tag cannot hold the order pattern");

	const std::size_t maxIdBytes = userTagSize - fixedTagBytes;
	for (std::size_t i = 0; i <= maxIdBytes; ++i)
	{
		if (id[i] == '\0')
			return true;
	}
	return false;
}
}

uint32_t makeLocalOrderID()
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

inline const char* formatAction(WTSDirectionType dType, WTSOffsetType oType)
{
	if(dType == WDT_LONG)
	{
		if (oType == WOT_OPEN)
			return "OL";
		else if (oType == WOT_CLOSE)
			return "CL";
		else
			return "CNL";
	}
	else
	{
		if (oType == WOT_OPEN)
			return "OS";
		else if (oType == WOT_CLOSE)
			return "CS";
		else
			return "CNS";
	}
}

TraderAdapter::TraderAdapter(EventNotifier* caster /* = NULL */)
	: _cfg(NULL)
	, _id("")
	, _trader_api(NULL)
	, _remover(NULL)
	, _dll_handle(NULL)
	, _owns_trader_api(false)
	, _spi_registered(false)
	, _api_released(false)
	, _state(AS_NOTLOGIN)
	, _notifier(caster)
	, _bd_mgr(NULL)
	, _policy_mgr(NULL)
	, _orders(NULL)
	, _ignore_sefmatch(false)
, _stat_map(NULL)
	, _risk_mon_enabled(false)
	, _save_data(false)
	{
	_owner_registry_instance_id = allocateOwnerRegistryInstanceId();
	// Without an account_id the gate is private to this adapter instance.
	_account_gate = std::make_shared<AccountTradingGate>("adapter-instance");
	_account_gate->addParticipant(this, [this](const AccountTradingGate::ResumeRequest& request, std::string& diagnostic) {
		return identityResumeParticipant(request, diagnostic);
	});
	std::random_device rd;
	const uint64_t wall = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	_boot_nonce = fmt::format("{:x}-{:08x}{:08x}", wall, rd(), rd());
}


TraderAdapter::~TraderAdapter()
{
	requestStopSinkNotifications();
	if (!waitSinkNotificationsStopped())
		std::terminate(); // Destroying the adapter from its own sink callback is unsupported.
	if (_account_gate)
		_account_gate->removeParticipant(this);

	// initExt() receives an externally owned API. Only clean up plugin-owned APIs
	// here; external owners keep their existing lifetime contract.
	if (_owns_trader_api)
		release();

	// NEW-052: no SPI callback may run inside or enter this adapter from now
	// on, so the adapter-owned order map can be released with it. A callback
	// that the provider delivers after this object is gone is outside the
	// host's control (ITraderApi offers no quiescence proof).
	if (closeSpiIngress(true))
	{
		OrderMap* orders = NULL;
		{
			SpinLock lock(_mtx_orders);
			orders = _orders;
			_orders = NULL;
		}
		if (orders)
			orders->release();
	}

	if (_cfg)
		_cfg->release();

	if (_stat_map)
		_stat_map->release();
}

thread_local const TraderAdapter::SpiIngress* TraderAdapter::s_spi_ingress = NULL;

TraderAdapter::SpiIngress::SpiIngress(TraderAdapter* adapter)
	: _adapter(adapter), _entered(false), _previous(NULL)
{
	// Count first, then check: closeSpiIngress() sets the flag before waiting
	// for the count, so an admitted callback is always waited for.
	_adapter->_spi_inflight.fetch_add(1, std::memory_order_seq_cst);
	if (_adapter->_spi_closed.load(std::memory_order_seq_cst))
	{
		// Decrement under the mutex: a waiter in closeSpiIngress(true) can only
		// observe zero after this thread is done with the adapter's mutex.
		std::lock_guard<std::mutex> lock(_adapter->_spi_mutex);
		if (_adapter->_spi_inflight.fetch_sub(1, std::memory_order_seq_cst) == 1)
			_adapter->_spi_idle.notify_all();
		return;
	}
	_entered = true;
	_previous = s_spi_ingress;
	s_spi_ingress = this;
}

TraderAdapter::SpiIngress::~SpiIngress()
{
	if (!_entered)
		return;
	s_spi_ingress = _previous;
	std::lock_guard<std::mutex> lock(_adapter->_spi_mutex);
	if (_adapter->_spi_inflight.fetch_sub(1, std::memory_order_seq_cst) == 1)
		_adapter->_spi_idle.notify_all();
}

bool TraderAdapter::closeSpiIngress(bool wait)
{
	_spi_closed.store(true, std::memory_order_seq_cst);
	if (!wait)
		return false;
	for (const SpiIngress* frame = s_spi_ingress; frame != NULL; frame = frame->_previous)
	{
		if (frame->_adapter == this)
			return false;	// cannot wait for the callback this thread is running
	}
	std::unique_lock<std::mutex> lock(_spi_mutex);
	_spi_idle.wait(lock, [this]() { return _spi_inflight.load(std::memory_order_seq_cst) == 0; });
	return true;
}

void TraderAdapter::reopenSpiIngress()
{
	_spi_closed.store(false, std::memory_order_seq_cst);
}

void TraderAdapter::addSink(ITrdNotifySink* sink)
{
	if (sink == NULL)
		return;

	bool shouldDrain = false;
	{
		std::lock_guard<std::mutex> lock(_sink_mutex);
		if (_sink_notifications_stopping || _sink_registrations.find(sink) != _sink_registrations.end())
			return;

		// A completed unsubscribe can leave a barrier for an in-flight callback.
		// Do not create a second registration until removeSink() has completed it.
		auto retired = _retired_sink_registrations.find(sink);
		if (retired != _retired_sink_registrations.end())
			return;

		SinkRegistrationPtr registration(new SinkRegistration(sink));
		_sinks.insert(sink);
		_sink_registrations[sink] = registration;
		// NEW-008: a declared owner is registered in the same critical section,
		// so READY (and any trading it triggers) never precedes owner routing.
		auto declared = _declared_owner_sinks.find(sink);
		if (declared != _declared_owner_sinks.end() && _owner_routing_enabled)
		{
			OwnerSinkRegistrationToken ownerToken;
			if (registerOwnerSinkLocked(declared->second, sink, ownerToken))
				_declared_owner_tokens[declared->second] = ownerToken;
		}
		if (_state == AS_ALLREADY)
		{
			std::vector<SinkRegistrationPtr> recipients(1, registration);
			shouldDrain = queueChannelNotificationLocked(true, recipients);
		}
	}

	if (shouldDrain)
		drainChannelNotifications();
}

bool TraderAdapter::configureOwnerRoutingCapabilities(const OwnerRoutingCapabilities& capabilities)
{
	std::lock_guard<std::mutex> lock(_sink_mutex);
	if (_sink_notifications_stopping || !_owner_sink_registrations.empty() ||
		!_owner_sink_classified.empty() || _sink_broadcasts_in_flight != 0)
		return false;

	_owner_routing_enabled = _owner_registry_instance_id != 0 && capabilities.complete();
	return _owner_routing_enabled;
}

bool TraderAdapter::registerOwnerSink(const std::string& stable_owner_id, ITrdNotifySink* sink,
	OwnerSinkRegistrationToken& token)
{
	if (token.valid() || stable_owner_id.empty() || sink == NULL)
		return false;

	std::lock_guard<std::mutex> lock(_sink_mutex);
	return registerOwnerSinkLocked(stable_owner_id, sink, token);
}

// Caller holds _sink_mutex.
bool TraderAdapter::registerOwnerSinkLocked(const std::string& stable_owner_id, ITrdNotifySink* sink,
	OwnerSinkRegistrationToken& token)
{
	if (token.valid() || stable_owner_id.empty() || sink == NULL)
		return false;
	std::string map_owner_id(stable_owner_id);
	std::string registration_owner_id(stable_owner_id);
	std::string token_owner_id(stable_owner_id);
	if (_sink_notifications_stopping || !_owner_routing_enabled ||
		_owner_registry_instance_id == 0 || _owner_sink_registrations.find(stable_owner_id) != _owner_sink_registrations.end() ||
		_owner_sink_classified.find(sink) != _owner_sink_classified.end() || _next_owner_sink_generation == 0)
		return false;

	auto sinkIt = _sink_registrations.find(sink);
	if (sinkIt == _sink_registrations.end())
		return false;

	const SinkRegistrationPtr& registration = sinkIt->second;
	std::unique_lock<std::mutex> registrationLock(registration->mutex);
	if (!registration->active || registration->owner_registered)
		return false;

	const uint64_t generation = _next_owner_sink_generation;
	OwnerSinkRegistration ownerRegistration;
	ownerRegistration.sink_registration = registration;
	ownerRegistration.generation = generation;
	auto insertedOwner = _owner_sink_registrations.end();
	try
	{
		auto insertion = _owner_sink_registrations.emplace(std::move(map_owner_id), ownerRegistration);
		if (!insertion.second)
			return false;
		insertedOwner = insertion.first;
		if (!_owner_sink_classified.insert(sink).second)
		{
			_owner_sink_registrations.erase(insertedOwner);
			return false;
		}
	}
	catch (...)
	{
		if (insertedOwner != _owner_sink_registrations.end())
			_owner_sink_registrations.erase(insertedOwner);
		_owner_sink_classified.erase(sink);
		throw;
	}

	registration->owner_registered = true;
	registration->owner_id.swap(registration_owner_id);
	registration->owner_generation = generation;
	++_next_owner_sink_generation;

	token._owner_id.swap(token_owner_id);
	token._adapter_instance_id = _owner_registry_instance_id;
	token._generation = generation;
	return true;
}

bool TraderAdapter::unregisterOwnerSink(const OwnerSinkRegistrationToken& token)
{
	if (!token.valid() || token._adapter_instance_id != _owner_registry_instance_id)
		return false;

	SinkRegistrationPtr registration;
	const bool reentrant = isInSinkCallback();
	{
		std::lock_guard<std::mutex> lock(_sink_mutex);
		auto ownerIt = _owner_sink_registrations.find(token._owner_id);
		if (ownerIt == _owner_sink_registrations.end() || ownerIt->second.generation != token._generation)
			return false;

		registration = ownerIt->second.sink_registration;
		std::lock_guard<std::mutex> registrationLock(registration->mutex);
		if (!registration->owner_registered || registration->owner_id != token._owner_id ||
			registration->owner_generation != token._generation)
			return false;
		registration->owner_registered = false;
		_owner_sink_registrations.erase(ownerIt);
	}

	if (reentrant)
	{
		std::lock_guard<std::mutex> registrationLock(registration->mutex);
		return registration->owner_callbacks_in_flight == 0;
	}

	std::unique_lock<std::mutex> registrationLock(registration->mutex);
	registration->idle.wait(registrationLock, [&registration]() {
		return registration->owner_callbacks_in_flight == 0;
	});
	return true;
}

bool TraderAdapter::removeSink(ITrdNotifySink* sink)
{
	if (sink == NULL)
		return true;

	SinkRegistrationPtr registration;
	const bool reentrant = isInSinkCallback();
	{
		std::lock_guard<std::mutex> lock(_sink_mutex);
		auto active = _sink_registrations.find(sink);
		if (active != _sink_registrations.end())
		{
			registration = active->second;
			_sinks.erase(sink);
			_sink_registrations.erase(active);
			_retired_sink_registrations[sink] = registration;
			_owner_sink_classified.erase(sink);
		}
		else
		{
			auto retired = _retired_sink_registrations.find(sink);
			if (retired == _retired_sink_registrations.end())
				return true;
			registration = retired->second;
		}

		std::lock_guard<std::mutex> registrationLock(registration->mutex);
		// NEW-008: a removed sink's owner declaration ends with it. Otherwise a
		// later sink allocated at the same address would silently inherit the
		// owner, and a re-created owner would find a stale token and never be
		// registered (and so receive other owners' events as a plain sink).
		auto declared = _declared_owner_sinks.find(sink);
		if (declared != _declared_owner_sinks.end())
		{
			auto token = _declared_owner_tokens.find(declared->second);
			if (token != _declared_owner_tokens.end() &&
				token->second._generation == registration->owner_generation)
				_declared_owner_tokens.erase(token);
			_declared_owner_sinks.erase(declared);
		}

		if (registration->owner_registered)
		{
			auto ownerIt = _owner_sink_registrations.find(registration->owner_id);
			if (ownerIt != _owner_sink_registrations.end() &&
				ownerIt->second.generation == registration->owner_generation &&
				ownerIt->second.sink_registration == registration)
				_owner_sink_registrations.erase(ownerIt);
			registration->owner_registered = false;
		}
		registration->active = false;
		if (!reentrant)
			registration->removal_waiter = true;
	}

	if (reentrant)
	{
		{
			std::lock_guard<std::mutex> registrationLock(registration->mutex);
			if (registration->in_flight != 0)
				return false;
		}

		// A sink removed reentrantly may not have an active callback that can
		// clean up its tombstone. Clear an idle tombstone here so the same sink
		// can be subscribed again from the callback without waiting or deadlocking.
		std::lock_guard<std::mutex> lock(_sink_mutex);
		auto retired = _retired_sink_registrations.find(sink);
		if (retired != _retired_sink_registrations.end() && retired->second == registration)
		{
			std::lock_guard<std::mutex> registrationLock(registration->mutex);
			if (registration->in_flight != 0)
				return false;
			if (!_sink_notifications_stopping && !registration->removal_waiter)
				_retired_sink_registrations.erase(retired);
		}
		return true;
	}

	{
		std::unique_lock<std::mutex> registrationLock(registration->mutex);
		registration->idle.wait(registrationLock, [&registration]() {
			return registration->in_flight == 0;
		});
		registration->removal_waiter = false;
	}

	{
		std::lock_guard<std::mutex> lock(_sink_mutex);
		auto retired = _retired_sink_registrations.find(sink);
		if (!_sink_notifications_stopping && retired != _retired_sink_registrations.end() && retired->second == registration)
			_retired_sink_registrations.erase(retired);
	}
	return true;
}

void TraderAdapter::requestStopSinkNotifications()
{
	std::lock_guard<std::mutex> lock(_sink_mutex);
	_sink_notifications_stopping = true;
	_state = AS_NOTLOGIN;
	_channel_notifications.clear();

	for (auto it = _sink_registrations.begin(); it != _sink_registrations.end(); ++it)
	{
		std::lock_guard<std::mutex> registrationLock(it->second->mutex);
		it->second->active = false;
	}
	for (auto it = _retired_sink_registrations.begin(); it != _retired_sink_registrations.end(); ++it)
	{
		std::lock_guard<std::mutex> registrationLock(it->second->mutex);
		it->second->active = false;
	}
}

bool TraderAdapter::waitSinkNotificationsStopped()
{
	if (isInSinkCallback())
		return false;

	std::unique_lock<std::mutex> lock(_sink_mutex);
	if (!_sink_notifications_stopping)
		return false;
	_sink_notifications_idle.wait(lock, [this]() { return sinkCallbacksDrainedLocked(); });
	_sinks.clear();
	_sink_registrations.clear();
	_retired_sink_registrations.clear();
	_owner_sink_registrations.clear();
	_owner_sink_classified.clear();
	return true;
}

void TraderAdapter::setState(AdapterState state)
{
	std::lock_guard<std::mutex> lock(_sink_mutex);
	_state = state;
}

bool TraderAdapter::transitionState(AdapterState expected, AdapterState next)
{
	std::lock_guard<std::mutex> lock(_sink_mutex);
	if (_state != expected)
		return false;
	_state = next;
	return true;
}

void TraderAdapter::forEachSink(const std::function<void(ITrdNotifySink*)>& callback)
{
	std::vector<SinkRegistrationPtr> snapshot;
	{
		std::lock_guard<std::mutex> lock(_sink_mutex);
		if (_sink_notifications_stopping)
			return;
		snapshot.reserve(_sink_registrations.size());
		for (auto it = _sink_registrations.begin(); it != _sink_registrations.end(); ++it)
			snapshot.push_back(it->second);
		if (snapshot.empty())
			return;
		++_sink_broadcasts_in_flight;
	}

	try
	{
		for (const SinkRegistrationPtr& registration : snapshot)
			invokeSink(registration, callback);
	}
	catch (...)
	{
		finishSinkBroadcast();
		throw;
	}
	finishSinkBroadcast();
}

void TraderAdapter::forEachNonOwnerSink(const std::function<void(ITrdNotifySink*)>& callback)
{
	std::vector<SinkRegistrationPtr> snapshot;
	{
		std::lock_guard<std::mutex> lock(_sink_mutex);
		if (_sink_notifications_stopping)
			return;
		for (auto it = _sink_registrations.begin(); it != _sink_registrations.end(); ++it)
		{
			if (_owner_sink_classified.find(it->first) == _owner_sink_classified.end())
				snapshot.push_back(it->second);
		}
		if (snapshot.empty())
			return;
		++_sink_broadcasts_in_flight;
	}
	try
	{
		for (const SinkRegistrationPtr& registration : snapshot)
			invokeSink(registration, callback);
	}
	catch (...)
	{
		finishSinkBroadcast();
		throw;
	}
	finishSinkBroadcast();
}

void TraderAdapter::dispatchOrderEvent(const std::string& owner, const std::function<void(ITrdNotifySink*)>& callback)
{
	if (!isOwnerRoutingEnabled())
	{
		forEachSink(callback);
		return;
	}
	// NEW-008: with owner routing an owner sink only ever receives events of
	// its own orders. Unowned events (direct adapter orders, external or
	// unresolvable orders) reach non-owner observers only and are never
	// broadcast into an owner's ledger.
	if (owner.empty())
	{
		forEachNonOwnerSink(callback);
		return;
	}
	if (!forEachSinkForOwner(owner, callback))
		WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN,
			"[{}] owner {} has no registered sink; its order event reaches non-owner observers only", _id.c_str(), owner.c_str());
	forEachNonOwnerSink(callback);
}

bool TraderAdapter::isOwnerRoutingEnabled() const
{
	std::lock_guard<std::mutex> lock(_sink_mutex);
	return _owner_routing_enabled;
}

bool TraderAdapter::declareOwnerSink(ITrdNotifySink* sink, const std::string& owner)
{
	if (sink == NULL || owner.empty())
		return false;
	std::lock_guard<std::mutex> lock(_sink_mutex);
	_declared_owner_sinks[sink] = owner;
	if (!_owner_routing_enabled)
		return false;	// routing disabled: the owner runs in non-shared mode
	if (_declared_owner_tokens.find(owner) != _declared_owner_tokens.end())
		return true;
	if (_sink_registrations.find(sink) == _sink_registrations.end())
		return true;	// registered atomically by addSink()
	OwnerSinkRegistrationToken token;
	if (!registerOwnerSinkLocked(owner, sink, token))
		return false;
	_declared_owner_tokens[owner] = token;
	return true;
}

void TraderAdapter::releaseOwner(const std::string& owner)
{
	OwnerSinkRegistrationToken token;
	bool hasToken = false;
	{
		std::lock_guard<std::mutex> lock(_sink_mutex);
		auto it = _declared_owner_tokens.find(owner);
		if (it != _declared_owner_tokens.end())
		{
			token = it->second;
			hasToken = true;
			_declared_owner_tokens.erase(it);
		}
		for (auto sit = _declared_owner_sinks.begin(); sit != _declared_owner_sinks.end();)
		{
			if (sit->second == owner)
				sit = _declared_owner_sinks.erase(sit);
			else
				++sit;
		}
	}
	if (hasToken)
		unregisterOwnerSink(token);
	_account_gate->releaseOwner(owner);
}

bool TraderAdapter::forEachSinkForOwner(const std::string& owner_id,
	const std::function<void(ITrdNotifySink*)>& callback)
{
	SinkRegistrationPtr registration;
	uint64_t generation = 0;
	{
		std::lock_guard<std::mutex> lock(_sink_mutex);
		if (_sink_notifications_stopping || !_owner_routing_enabled)
			return false;

		auto ownerIt = _owner_sink_registrations.find(owner_id);
		if (ownerIt == _owner_sink_registrations.end())
			return false;

		registration = ownerIt->second.sink_registration;
		generation = ownerIt->second.generation;
		++_sink_broadcasts_in_flight;
	}

	bool invoked = false;
	try
	{
#ifdef WT_OWNER_ROUTING_TEST_HOOKS
		wtOwnerRoutingTestAfterLookup();
#endif
		invoked = invokeOwnerSink(registration, owner_id, generation, callback);
	}
	catch (...)
	{
		finishSinkBroadcast();
		throw;
	}
	finishSinkBroadcast();
	return invoked;
}

void TraderAdapter::finishSinkBroadcast()
{
	std::lock_guard<std::mutex> lock(_sink_mutex);
	if (_sink_broadcasts_in_flight > 0)
		--_sink_broadcasts_in_flight;
	_sink_notifications_idle.notify_all();
}

void TraderAdapter::invokeSink(const SinkRegistrationPtr& registration,
	const std::function<void(ITrdNotifySink*)>& callback)
{
	{
		std::lock_guard<std::mutex> lock(registration->mutex);
		if (!registration->active)
			return;
		++registration->in_flight;
	}

	SinkCallbackFrame callbackFrame(this, g_sink_callback_frame);
	g_sink_callback_frame = &callbackFrame;
	try
	{
		EntryContextIsolation isolation;
		callback(registration->sink);
	}
	catch (...)
	{
		g_sink_callback_frame = callbackFrame.previous;
		finishSinkCallback(registration);
		throw;
	}
	g_sink_callback_frame = callbackFrame.previous;
	finishSinkCallback(registration);
}

bool TraderAdapter::invokeOwnerSink(const SinkRegistrationPtr& registration,
	const std::string& owner_id, uint64_t generation,
	const std::function<void(ITrdNotifySink*)>& callback)
{
	{
		std::lock_guard<std::mutex> lock(registration->mutex);
		if (!registration->active || !registration->owner_registered ||
			registration->owner_id != owner_id || registration->owner_generation != generation)
			return false;
		++registration->in_flight;
		++registration->owner_callbacks_in_flight;
	}

	SinkCallbackFrame callbackFrame(this, g_sink_callback_frame);
	g_sink_callback_frame = &callbackFrame;
	try
	{
		EntryContextIsolation isolation;
		callback(registration->sink);
	}
	catch (...)
	{
		g_sink_callback_frame = callbackFrame.previous;
		finishSinkCallback(registration, true);
		throw;
	}
	g_sink_callback_frame = callbackFrame.previous;
	finishSinkCallback(registration, true);
	return true;
}

void TraderAdapter::finishSinkCallback(const SinkRegistrationPtr& registration, bool owner_callback)
{
	// Keep the in-flight count positive until all adapter-owned state is done
	// being accessed. This lets shutdown wait for the complete callback wrapper.
	std::lock_guard<std::mutex> lock(_sink_mutex);
	std::lock_guard<std::mutex> registrationLock(registration->mutex);
	if (registration->in_flight > 0)
		--registration->in_flight;
	if (owner_callback && registration->owner_callbacks_in_flight > 0)
		--registration->owner_callbacks_in_flight;
	if (owner_callback && registration->owner_callbacks_in_flight == 0)
		registration->idle.notify_all();
	if (registration->in_flight == 0)
	{
		auto retired = _retired_sink_registrations.find(registration->sink);
		if (!_sink_notifications_stopping &&
			retired != _retired_sink_registrations.end() && retired->second == registration &&
			!registration->removal_waiter)
			_retired_sink_registrations.erase(retired);
		registration->idle.notify_all();
	}
	_sink_notifications_idle.notify_all();
}

bool TraderAdapter::queueChannelNotificationLocked(bool ready,
	const std::vector<SinkRegistrationPtr>& recipients)
{
	if (_sink_notifications_stopping || recipients.empty())
		return false;

	ChannelNotification notification;
	notification.version = ++_channel_event_version;
	notification.ready = ready;
	notification.recipients = recipients;
	_channel_notifications.push_back(std::move(notification));
	if (_channel_drain_active)
		return false;
	_channel_drain_active = true;
	return true;
}

void TraderAdapter::drainChannelNotifications()
{
	for (;;)
	{
		ChannelNotification notification;
		{
			std::lock_guard<std::mutex> lock(_sink_mutex);
			if (_channel_notifications.empty())
			{
				_channel_drain_active = false;
				_sink_notifications_idle.notify_all();
				return;
			}
			notification = std::move(_channel_notifications.front());
			_channel_notifications.pop_front();
		}

		for (const SinkRegistrationPtr& registration : notification.recipients)
		{
			try
			{
				const bool ready = notification.ready;
				invokeSink(registration, [ready](ITrdNotifySink* sink) {
					if (ready)
						sink->on_channel_ready();
					else
						sink->on_channel_lost();
				});
			}
			catch (...)
			{
				// A third-party sink must not strand later READY/LOST notifications.
			}
		}
	}
}

bool TraderAdapter::isInSinkCallback() const
{
	for (SinkCallbackFrame* frame = g_sink_callback_frame; frame != NULL; frame = frame->previous)
	{
		if (frame->adapter == this)
			return true;
	}
	return false;
}

bool TraderAdapter::isInAnySinkCallback()
{
	return g_sink_callback_frame != NULL;
}

bool TraderAdapter::sinkCallbacksDrainedLocked() const
{
	if (_sink_broadcasts_in_flight != 0 || _channel_drain_active)
		return false;

	for (auto it = _sink_registrations.begin(); it != _sink_registrations.end(); ++it)
	{
		std::lock_guard<std::mutex> registrationLock(it->second->mutex);
		if (it->second->in_flight != 0)
			return false;
	}
	for (auto it = _retired_sink_registrations.begin(); it != _retired_sink_registrations.end(); ++it)
	{
		std::lock_guard<std::mutex> registrationLock(it->second->mutex);
		if (it->second->in_flight != 0)
			return false;
	}
	return true;
}

bool TraderAdapter::init(const char* id, WTSVariant* params, IBaseDataMgr* bdMgr, ActionPolicyMgr* policyMgr)
{
	if (!canFormatOrderPattern(id))
		return false;

	if (params == NULL)
		return false;

	if (_cfg != NULL)
		return false;

	const std::string moduleName = params->getString("module");
	if (moduleName.empty())
		return false;

	// 风控已经启用时，policy 是必需配置。先验证再提交适配器状态，
	// 避免配置错误留下半初始化对象或继续装载交易模块。
	WTSVariant* cfgRisk = params->get("riskmon");
	bool riskMonEnabled = cfgRisk && cfgRisk->getBoolean("active");
	WTSVariant* cfgPolicy = NULL;
	if (riskMonEnabled)
	{
		cfgPolicy = cfgRisk->get("policy");
		if (cfgPolicy == NULL || cfgPolicy->type() != WTSVariant::VT_Object)
		{
			WTSLogger::error("[{}] Active risk control requires a policy object", id);
			return false;
		}

		WTSVariant* defaultRule = cfgPolicy->get("default");
		if (defaultRule == NULL || defaultRule->type() != WTSVariant::VT_Object)
		{
			WTSLogger::error("[{}] Active risk control requires a default policy rule", id);
			return false;
		}
	}

	_policy_mgr = policyMgr;
	_bd_mgr = bdMgr;
	_id = id;

	_order_pattern = fmt::format("otp.{}", id);

	_cfg = params;
	_cfg->retain();
	{
		// Card 6a: adapters configured with the same account_id share one gate.
		WTSVariant* accountCfg = _cfg->get("account_id");
		WTSVariant* auditCfg = _cfg->get("account_audit_log");
		if ((accountCfg != NULL && accountCfg->type() != WTSVariant::VT_String) ||
			(auditCfg != NULL && auditCfg->type() != WTSVariant::VT_String))
			pauseAccount("account_config", "account_id/account_audit_log must be strings");
		else if (accountCfg != NULL || auditCfg != NULL)
			bindAccount(_cfg->getString("account_id"), _cfg->getString("account_audit_log"));
	}
	WTSVariant* journalConfig = _cfg->get("unresolved_report_journal");
	if (journalConfig != NULL && (journalConfig->type() != WTSVariant::VT_String ||
		_cfg->getString("unresolved_report_journal").empty()))
	{
		pauseAccount("unresolved_journal", "unresolved_report_journal must be a nonempty string");
		std::lock_guard<std::mutex> lock(_unresolved_report_mutex);
		_unresolved_journal.reset();
		_unresolved_diagnostic = "unresolved_report_journal must be a nonempty string";
	}
	else
		configureUnresolvedJournal(_cfg->getString("unresolved_report_journal"));

	WTSVariant* identityConfig = _cfg->get("order_identity_journal");
	struct OwnerRoutingConfigure
	{
		TraderAdapter* self;
		~OwnerRoutingConfigure()
		{
			// Card 5c: shared owners need complete routing capabilities; the
			// provider capabilities are operator-declared and not verifiable here.
			WTSVariant* routing = self->_cfg ? self->_cfg->get("owner_routing") : NULL;
			if (routing == NULL || routing->type() != WTSVariant::VT_Object || !routing->getBoolean("enabled"))
				return;
			OwnerRoutingCapabilities caps;
			caps.durable_journal = self->_identity_journal != NULL;
			caps.local_order_id_floor = self->_identity_journal != NULL &&
				self->_identity_journal->snapshot().local_order_id_floor_established;
			caps.provider_user_tag_round_trip = routing->getBoolean("provider_user_tag_round_trip");
			caps.stable_trade_id_across_push_and_query = routing->getBoolean("provider_stable_trade_id");
			if (!self->configureOwnerRoutingCapabilities(caps))
				WTSLogger::log_dyn("trader", self->_id.c_str(), LL_WARN,
					"[{}] owner routing requested but capabilities are incomplete; owners stay in non-shared mode", self->_id.c_str());
		}
	} ownerRoutingConfigure{ this };
	if (identityConfig != NULL)
	{
		WTSVariant* floorCfg = _cfg->get("local_order_id_floor");
		if (identityConfig->type() != WTSVariant::VT_String || _cfg->getString("order_identity_journal").empty())
			pauseAccount("identity_journal", "order_identity_journal must be a nonempty string");
		else if (floorCfg != NULL && floorCfg->type() != WTSVariant::VT_Uint32 && floorCfg->type() != WTSVariant::VT_Int32 &&
			floorCfg->type() != WTSVariant::VT_Uint64 && floorCfg->type() != WTSVariant::VT_Int64)
			pauseAccount("identity_journal", "local_order_id_floor must be an integer");
		else
			configureIdentityJournal(_cfg->getString("order_identity_journal"),
				floorCfg ? floorCfg->asUInt32() : 0, _cfg->getString("local_order_id_floor_evidence"), floorCfg != NULL);
	}

	_ignore_sefmatch = _cfg->getBoolean("ignore_selfmatch");
	_save_data = _cfg->getBoolean("savedata");
	if (_save_data)
		initSaveData();

	//这里解析流量风控参数
	if (cfgRisk)
	{
		if (riskMonEnabled)
		{
			_risk_mon_enabled = true;

			auto keys = cfgPolicy->memberNames();
			for (auto it = keys.begin(); it != keys.end(); it++)
			{
				const char* product = (*it).c_str();
				WTSVariant*	vProdItem = cfgPolicy->get(product);
				RiskParams& rParam = _risk_params_map[product];
				rParam._cancel_total_limits = vProdItem->getUInt32("cancel_total_limits");
				rParam._cancel_times_boundary = vProdItem->getUInt32("cancel_times_boundary");
				rParam._cancel_stat_timespan = vProdItem->getUInt32("cancel_stat_timespan");

				rParam._order_total_limits = vProdItem->getUInt32("order_total_limits");
				rParam._order_times_boundary = vProdItem->getUInt32("order_times_boundary");
				rParam._order_stat_timespan = vProdItem->getUInt32("order_stat_timespan");

				WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO, "[{}] Risk control rule {} of trading channel loaded", _id.c_str(), product);
			}

			auto it = _risk_params_map.find("default");
			if (it == _risk_params_map.end())
			{
				WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN, "[{}] Some instruments may not be monitored due to no default risk control rule of trading channel", _id.c_str());
			}
		}
		else
		{
			WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN, "[{}] Risk control rule of trading channel not activated", _id.c_str());
		}
	}
	else
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN, "[{}] No risk control rule setup of trading channel", _id.c_str());
	}

	auto failInit = [this]() {
		release();
		_id.clear();
		_order_pattern.clear();
		_bd_mgr = NULL;
		_policy_mgr = NULL;
		_risk_params_map.clear();
		_risk_mon_enabled = false;
		_ignore_sefmatch = false;
		_save_data = false;
		_trades_log.reset();
		_orders_log.reset();
		_rt_data_file.clear();
		return false;
	};

	std::string module = DLLHelper::wrap_module(moduleName.c_str(), "lib");

	//先看工作目录下是否有交易模块
	std::string dllpath = WtHelper::getModulePath(module.c_str(), "traders", true);
	//如果没有,则再看模块目录,即dll同目录下
	if (!StdFile::exists(dllpath.c_str()))
		dllpath = WtHelper::getModulePath(module.c_str(), "traders", false);
	_dll_handle = DLLHelper::load_library(dllpath.c_str());
	if (_dll_handle == NULL)
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR, "[{}] Loading trading module {} failed", _id.c_str(), dllpath.c_str());
		return failInit();
	}
	else
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO, "[{}] Trader module {} loaded", _id.c_str(), dllpath.c_str());
	}

	FuncCreateTrader pFunCreateTrader = (FuncCreateTrader)DLLHelper::get_symbol(_dll_handle, "createTrader");
	_remover = (FuncDeleteTrader)DLLHelper::get_symbol(_dll_handle, "deleteTrader");
	if (NULL == pFunCreateTrader)
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_FATAL, "[{}] Entrance function createTrader not found", _id.c_str());
		return failInit();
	}
	if (NULL == _remover)
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_FATAL, "[{}] Entrance function deleteTrader not found", _id.c_str());
		return failInit();
	}

	_trader_api = pFunCreateTrader();
	if (NULL == _trader_api)
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_FATAL, "[{}] Creating trading api failed", _id.c_str());
		return failInit();
	}
	_owns_trader_api = true;
	_spi_registered = false;
	_api_released = false;

	if (!_trader_api->init(params))
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR, "[{}] Trader API initialization failed", id);
		return failInit();
	}

	return true;
}

bool TraderAdapter::initExt(const char* id, ITraderApi* api, IBaseDataMgr* bdMgr, ActionPolicyMgr* policyMgr)
{
	if (!canFormatOrderPattern(id))
		return false;

	if (api == NULL)
		return false;
	if (_trader_api != NULL || _cfg != NULL)
		return false;

	configureUnresolvedJournal("");

	_policy_mgr = policyMgr;
	_bd_mgr = bdMgr;
	_id = id;

	_order_pattern = fmt::format("otp.{}", id);

	if (_cfg != NULL)
		return false;

	_save_data = true;
	if (_save_data)
		initSaveData();

	_trader_api = api;
	_spi_registered = false;
	_api_released = false;
	if (!_trader_api->init(NULL))
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR, "[{}] Trader initializing failed", id);
		// The API is externally owned, so release its partial state but never
		// delete it or call a plugin factory deleter from this adapter.
		_trader_api->release();
		_trader_api = NULL;
		_api_released = true;
		_id.clear();
		_order_pattern.clear();
		_bd_mgr = NULL;
		_policy_mgr = NULL;
		_save_data = false;
		_trades_log.reset();
		_orders_log.reset();
		_rt_data_file.clear();
		return false;
	}
	_api_released = false;

	return true;
}

void TraderAdapter::initSaveData()
{
	/*std::string folder = WtHelper::getOutputDir();
	folder += _name;
	folder += "//";*/
	std::stringstream ss;
	ss << WtHelper::getBaseDir() << "traders/" << _id << "//";
	std::string folder = ss.str();
	fs::create_directories(folder.c_str());

	std::string filename = folder + "trades.csv";
	_trades_log.reset(new BoostFile());
	{
		bool isNewFile = !StdFile::exists(filename.c_str());
		_trades_log->create_or_open_file(filename.c_str());
		if (isNewFile)
		{
			_trades_log->write_file("localid,date,time,code,action,volume,price,tradeid,orderid\n");
		}
		else
		{
			_trades_log->seek_to_end();
		}
	}

	filename = folder + "orders.csv";
	_orders_log.reset(new BoostFile());
	{
		bool isNewFile = !StdFile::exists(filename.c_str());
		_orders_log->create_or_open_file(filename.c_str());
		if (isNewFile)
		{
			_orders_log->write_file("localid,date,inserttime,code,action,volume,traded,price,orderid,canceled,remark\n");
		}
		else
		{
			_orders_log->seek_to_end();
		}
	}

	_rt_data_file = folder + "rtdata.json";
}

void TraderAdapter::logTrade(uint32_t localid, const char* stdCode, WTSTradeInfo* trdInfo)
{
	if (_trades_log == NULL || trdInfo == NULL)
		return;

	_trades_log->write_file(fmt::format("{},{},{},{},{},{},{},{},{}\n",
		localid, trdInfo->getTradeDate(), trdInfo->getTradeTime(), stdCode,
		formatAction(trdInfo->getDirection(), trdInfo->getOffsetType()),
		trdInfo->getVolume(), trdInfo->getPrice(), trdInfo->getTradeID(), trdInfo->getRefOrder()));
}

void TraderAdapter::logOrder(uint32_t localid, const char* stdCode, WTSOrderInfo* ordInfo)
{
	if (_orders_log == NULL || ordInfo == NULL)
		return;

	_orders_log->write_file(fmt::format("{},{},{},{},{},{},{},{},{},{},{}\n",
		localid, ordInfo->getOrderDate(), ordInfo->getOrderTime(), stdCode,
		formatAction(ordInfo->getDirection(), ordInfo->getOffsetType()),
		ordInfo->getVolume(), ordInfo->getVolTraded(), ordInfo->getPrice(), 
		ordInfo->getOrderID(), ordInfo->getOrderState()==WOS_Canceled?"TRUE":"FALSE", ordInfo->getStateMsg()));
}

void TraderAdapter::saveData(WTSArray* ayFunds /* = NULL */)
{
	rj::Document root(rj::kObjectType);
	rj::Document::AllocatorType &allocator = root.GetAllocator();

	{//持仓数据保存
		rj::Value jPos(rj::kArrayType);
		/*
		//多仓数据
		double	l_newvol;
		double	l_newavail;
		double	l_prevol;
		double	l_preavail;

		//空仓数据
		double	s_newvol;
		double	s_newavail;
		double	s_prevol;
		double	s_preavail;
		 */

		for (auto it = _positions.begin(); it != _positions.end(); it++)
		{
			const char* stdCode = it->first.c_str();
			const PosItem& pInfo = it->second;

			rj::Value pItem(rj::kObjectType);
			pItem.AddMember("code", rj::Value(stdCode, allocator), allocator);

			rj::Value longItem(rj::kObjectType);
			longItem.AddMember("newvol", pInfo.l_newvol, allocator);
			longItem.AddMember("newavail", pInfo.l_newavail, allocator);
			longItem.AddMember("prevol", pInfo.l_prevol, allocator);
			longItem.AddMember("preavail", pInfo.l_preavail, allocator);
			pItem.AddMember("long", longItem, allocator);

			rj::Value shortItem(rj::kObjectType);
			shortItem.AddMember("newvol", pInfo.s_newvol, allocator);
			shortItem.AddMember("newavail", pInfo.s_newavail, allocator);
			shortItem.AddMember("prevol", pInfo.s_prevol, allocator);
			shortItem.AddMember("preavail", pInfo.s_preavail, allocator);
			pItem.AddMember("short", shortItem, allocator);

			jPos.PushBack(pItem, allocator);
		}

		root.AddMember("positions", jPos, allocator);
	}

	{//资金保存
		rj::Value jFunds(rj::kObjectType);

		if(ayFunds && ayFunds->size() > 0)
		{
			for(auto it = ayFunds->begin(); it != ayFunds->end(); it++)
			{
				WTSAccountInfo* fundInfo = (WTSAccountInfo*)(*it);
				rj::Value fItem(rj::kObjectType);
				fItem.AddMember("prebalance", fundInfo->getPreBalance(), allocator);
				fItem.AddMember("balance", fundInfo->getBalance(), allocator);
				fItem.AddMember("closeprofit", fundInfo->getCloseProfit(), allocator);
				fItem.AddMember("dynprofit", fundInfo->getDynProfit(), allocator);
				fItem.AddMember("margin", fundInfo->getMargin(), allocator);
				fItem.AddMember("fee", fundInfo->getCommission(), allocator);
				fItem.AddMember("available", fundInfo->getAvailable(), allocator);

				fItem.AddMember("deposit", fundInfo->getDeposit(), allocator);
				fItem.AddMember("withdraw", fundInfo->getWithdraw(), allocator);

				jFunds.AddMember(rj::Value(fundInfo->getCurrency(), allocator), fItem, allocator);
			}
		}

		root.AddMember("funds", jFunds, allocator);
	}

	{
		BoostFile bf;
		if (bf.create_new_file(_rt_data_file.c_str()))
		{
			rj::StringBuffer sb;
			rj::PrettyWriter<rj::StringBuffer> writer(sb);
			root.Accept(writer);
			bf.write_file(sb.GetString());
			bf.close_file();
		}
	}
}

const TraderAdapter::RiskParams* TraderAdapter::getRiskParams(const char* stdCode)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, NULL);
	std::string pid = cInfo.stdCommID();
	auto it = _risk_params_map.find(pid);
	if (it != _risk_params_map.end())
		return &it->second;

	it = _risk_params_map.find("default");
	return it == _risk_params_map.end() ? NULL : &it->second;
}

bool TraderAdapter::run()
{
	if (_trader_api == NULL)
		return false;
	if (_api_released)
		return false;

	if (_stat_map == NULL)
		_stat_map = TradeStatMap::create();

	setState(AS_LOGINING);
	// A (re)registered SPI accepts callbacks again; callbacks of an earlier
	// released API instance were drained by release().
	reopenSpiIngress();
	_trader_api->registerSpi(this);
	_spi_registered = true;

	_trader_api->connect();
	return true;
}

void TraderAdapter::release()
{
	// ITraderApi::release() is not a sink-notification drain barrier. Lifecycle
	// owners must use requestStopSinkNotifications()/waitSinkNotificationsStopped().
	setState(AS_NOTLOGIN);

	if (_trader_api && !_api_released)
	{
		if (_spi_registered)
		{
			_trader_api->registerSpi(NULL);
			_spi_registered = false;
		}

		_trader_api->release();
		_api_released = true;
	}
	// NEW-052: callbacks the provider still delivers after release are
	// dropped. release() is not a drain barrier (a callback may be blocked on
	// the caller); the destructor waits for callbacks already running.
	closeSpiIngress(false);

	if (_owns_trader_api)
	{
		// init() requires both factory symbols before it creates an instance.
		if (_trader_api && _remover)
			_remover(_trader_api);
		_trader_api = NULL;
		_owns_trader_api = false;
	}

	if (_dll_handle)
	{
		DLLHelper::free_library(_dll_handle);
		_dll_handle = NULL;
	}
	_remover = NULL;

	if (_cfg)
	{
		_cfg->release();
		_cfg = NULL;
	}

	setState(AS_NOTLOGIN);
	_bd_mgr = NULL;
	_policy_mgr = NULL;
	_risk_params_map.clear();
	_risk_mon_enabled = false;
	_trades_log.reset();
	_orders_log.reset();
}

double TraderAdapter::getPosition(const char* stdCode, bool bValidOnly, int32_t flag /* = 3 */)
{
	auto it = _positions.find(stdCode);
	if (it == _positions.end())
		return 0;

	double ret = 0;
	const PosItem& pItem = it->second;
	if(flag & 1)
	{
		if(bValidOnly)
			ret += (pItem.l_newavail + pItem.l_preavail);
		else
			ret += (pItem.l_newvol + pItem.l_prevol);
	}

	if (flag & 2)
	{
		if (bValidOnly)
			ret -= (pItem.s_newavail + pItem.s_preavail);
		else
			ret -= pItem.s_newvol + pItem.s_prevol;
	}
	return ret;
}

void TraderAdapter::enumPosition(FuncEnumChnlPosCallBack cb)
{
	for(auto& v : _positions)
	{
		const char* stdCode = v.first.c_str();
		const PosItem& pItem = v.second;
		if(decimal::gt(pItem.l_prevol + pItem.l_newvol, 0))
			cb(stdCode, true, pItem.l_prevol, pItem.l_preavail, pItem.l_newvol, pItem.l_newavail);
		if (decimal::gt(pItem.s_prevol + pItem.s_newvol, 0))
			cb(stdCode, false, pItem.s_prevol, pItem.s_preavail, pItem.s_newvol, pItem.s_newavail);
	}
}

OrderMap* TraderAdapter::getOrders(const char* stdCode)
{
	if (_orders == NULL)
		return NULL;

	bool isAll = strlen(stdCode) == 0;

	SpinLock lock(_mtx_orders);
	OrderMap* ret = OrderMap::create();
	for (auto it = _orders->begin(); it != _orders->end(); it++)
	{
		uint32_t localid = it->first;
		WTSOrderInfo* ordInfo = (WTSOrderInfo*)it->second;

		bool matchesCode = isAll || strcmp(ordInfo->getCode(), stdCode) == 0;
		if (!matchesCode && !isAll)
		{
			WTSContractInfo* cInfo = ordInfo->getContractInfo();
			if (cInfo == NULL && _bd_mgr != NULL)
				cInfo = _bd_mgr->getContract(ordInfo->getCode(), ordInfo->getExchg());

			if (cInfo != NULL)
			{
				WTSCommodityInfo* commInfo = cInfo->getCommInfo();
				std::string orderStdCode;
				if (commInfo != NULL && (commInfo->getCategoty() == CC_FutOption || commInfo->getCategoty() == CC_SpotOption))
					orderStdCode = CodeHelper::rawFutOptCodeToStdCode(cInfo->getCode(), cInfo->getExchg());
				else if (CodeHelper::isMonthlyCode(cInfo->getCode()))
					orderStdCode = CodeHelper::rawMonthCodeToStdCode(cInfo->getCode(), cInfo->getExchg());
				else if (commInfo != NULL)
					orderStdCode = CodeHelper::rawFlatCodeToStdCode(cInfo->getCode(), cInfo->getExchg(), cInfo->getProduct());

				matchesCode = !orderStdCode.empty() && orderStdCode == stdCode;
			}
		}

		if (matchesCode)
			ret->add(localid, ordInfo);
	}
	return ret;
}

bool TraderAdapter::isUnresolvedReportPaused() const noexcept
{
	return _account_gate->paused();
}

std::vector<TraderAdapter::UnresolvedTradeReport> TraderAdapter::unresolvedTradeReports() const
{
	std::lock_guard<std::mutex> lock(_unresolved_report_mutex);
	return _unresolved_reports;
}

std::string TraderAdapter::unresolvedReportDiagnostic() const
{
	std::lock_guard<std::mutex> lock(_unresolved_report_mutex);
	return _unresolved_diagnostic;
}

namespace
{
	// Status codes recorded in the order identity journal for terminal states
	// that have no WTSOrderState equivalent.
	const uint32_t IDENTITY_STATUS_NOT_SENT = 0xFFFF0001u;		///< provider call did not happen / failed synchronously
	const uint32_t IDENTITY_STATUS_REJECTED = 0xFFFF0002u;		///< entrust rejected by provider response
	const uint32_t IDENTITY_STATUS_MANUAL = 0xFFFF0003u;		///< closed by audited manual reconciliation
	const size_t LEDGER_TOMBSTONE_LIMIT = 65536;

	struct IdentityJournalAttachment
	{
		std::string path;
		std::shared_ptr<DiffOrderOwnershipJournalV1> journal;
		std::string error;
	};

	// Strict "<pattern>.<digits>" parse; returns 0 when the tag is not ours,
	// UINT32_MAX when it carries our prefix but is malformed/overflowing.
	uint32_t parseLocalTag(const char* tag, size_t capacity, const std::string& pattern)
	{
		if (tag == NULL || pattern.empty())
			return 0;
		const void* terminator = std::memchr(tag, '\0', capacity);
		if (terminator == NULL)
			return UINT32_MAX;
		const std::string value(tag, static_cast<const char*>(terminator));
		if (value.compare(0, pattern.size(), pattern) != 0)
			return 0;
		if (value.size() <= pattern.size() + 1 || value[pattern.size()] != '.')
			return value.size() == pattern.size() ? UINT32_MAX : (value[pattern.size()] == '.' ? UINT32_MAX : 0);
		uint32_t localid = 0;
		for (size_t i = pattern.size() + 1; i < value.size(); ++i)
		{
			const char digit = value[i];
			if (digit < '0' || digit > '9' ||
				localid > ((std::numeric_limits<uint32_t>::max)() - 1 - static_cast<uint32_t>(digit - '0')) / 10)
				return UINT32_MAX;
			localid = localid * 10 + static_cast<uint32_t>(digit - '0');
		}
		return localid == 0 ? UINT32_MAX : localid;
	}

	// NEW-154: identity of one trade report for duplicate detection. The order
	// reference and direction are part of it because the two sides of a self
	// match share the trade id (checkSelfMatch relies on seeing both). An
	// empty trade id cannot be deduplicated.
	std::string tradeReportKey(const WTSTradeInfo* trade)
	{
		if (trade == NULL || trade->getTradeID()[0] == '\0')
			return std::string();
		return fmt::format("{}|{}|{}|{}", trade->getExchg(), trade->getTradeID(), trade->getRefOrder(),
			static_cast<int>(trade->getDirection()));
	}
}

TraderAdapter::OwnerScope::OwnerScope(TraderAdapter* adapter, const std::string& owner, bool shared_capable)
{
	_frame.adapter = adapter;
	_frame.owner = owner;
	_frame.shared_capable = shared_capable;
	_frame.previous = g_owner_scope;
	g_owner_scope = &_frame;
}

TraderAdapter::OwnerScope::~OwnerScope()
{
	g_owner_scope = _frame.previous;
}

TraderAdapter::EntrustObserverScope::EntrustObserverScope(TraderAdapter* adapter, const std::function<void(uint32_t)>& observer)
{
	_frame.adapter = adapter;
	_frame.observer = observer;
	_frame.previous = g_entrust_observer;
	g_entrust_observer = &_frame;
}

TraderAdapter::EntrustObserverScope::~EntrustObserverScope()
{
	g_entrust_observer = _frame.previous;
}

OrderIDs TraderAdapter::buyAsOwner(const std::string& owner, bool shared_capable, const char* stdCode,
	double price, double qty, int flag, bool bForceClose)
{
	OwnerScope scope(this, owner, shared_capable);
	return buy(stdCode, price, qty, flag, bForceClose);
}

OrderIDs TraderAdapter::sellAsOwner(const std::string& owner, bool shared_capable, const char* stdCode,
	double price, double qty, int flag, bool bForceClose)
{
	OwnerScope scope(this, owner, shared_capable);
	return sell(stdCode, price, qty, flag, bForceClose);
}

double TraderAdapter::getUndoneQtyForOwner(const char* stdCode, const std::string& owner) const
{
	std::lock_guard<std::mutex> lock(_ledger_mutex);
	auto it = _owner_undone.find(owner);
	if (it == _owner_undone.end())
		return 0;
	auto code = it->second.find(stdCode);
	return code == it->second.end() ? 0 : code->second;
}

OrderMap* TraderAdapter::getOrdersForOwner(const char* stdCode, const std::string& owner)
{
	OrderMap* all = getOrders(stdCode);
	if (all == NULL)
		return NULL;
	std::unordered_set<uint32_t> owned;
	{
		std::lock_guard<std::mutex> lock(_ledger_mutex);
		for (const auto& item : _ledger)
		{
			if (item.second.owner_id == owner)
				owned.insert(item.first);
		}
	}
	std::vector<uint32_t> drop;
	for (auto it = all->begin(); it != all->end(); ++it)
	{
		if (owned.find(it->first) == owned.end())
			drop.push_back(it->first);
	}
	for (uint32_t localid : drop)
		all->remove(localid);
	return all;
}

void TraderAdapter::pauseAccount(const char* source, const std::string& reason)
{
	_unresolved_report_paused.store(true, std::memory_order_release);
	_account_gate->pause(_id + ":" + (source ? source : ""), reason);
	WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR, "[{}] account {} paused by {}: {}",
		_id.c_str(), _account_gate->key().c_str(), source ? source : "", reason.c_str());
}

bool TraderAdapter::bindAccount(const std::string& account_id, const std::string& audit_log, std::string* error)
{
	auto fail = [&](const std::string& text) {
		if (error)
			*error = text;
		pauseAccount("account_binding", text);
		return false;
	};
	if (account_id.empty())
		return fail("account_id must be a nonempty string");
	std::shared_ptr<AccountTradingGate> previous = _account_gate;
	std::shared_ptr<AccountTradingGate> gate = AccountTradingGate::acquireShared("account:" + account_id);
	if (gate != previous)
	{
		gate->addParticipant(this, [this](const AccountTradingGate::ResumeRequest& request, std::string& diagnostic) {
			return identityResumeParticipant(request, diagnostic);
		});
		if (previous)
		{
			previous->removeParticipant(this);
			if (previous->paused())
				gate->pause(_id + ":carried", "pause carried from previous binding " + previous->key());
		}
		_account_gate = gate;
	}
	_account_id = account_id;
	if (!audit_log.empty())
	{
		std::string text;
		if (!gate->configureAudit(audit_log, text))
			return fail(text);
	}
	return true;
}

uint64_t TraderAdapter::accountPauseGeneration() const
{
	return _account_gate->generation();
}

AccountTradingGate::ResumeStatus TraderAdapter::resumeAccountTrading(
	const AccountTradingGate::ResumeRequest& request, std::string* diagnostic)
{
	AccountTradingGate::ResumeStatus status = _account_gate->resume(request, diagnostic);
	WTSLogger::log_dyn("trader", _id.c_str(), status == AccountTradingGate::ResumeStatus::Ok ? LL_WARN : LL_ERROR,
		"[{}] resume of account {} requested by {}: status {}, {}", _id.c_str(), _account_gate->key().c_str(),
		request.operator_id.c_str(), (int)status, _account_gate->lastDiagnostic().c_str());
	if (status == AccountTradingGate::ResumeStatus::Ok)
		_unresolved_report_paused.store(false, std::memory_order_release);
	return status;
}

bool TraderAdapter::isTradingAdmissionClosed() const noexcept
{
	return _account_gate->paused() || _identity_recovery_pending.load(std::memory_order_acquire);
}

bool TraderAdapter::isIdentityRecoveryPending() const noexcept
{
	return _identity_recovery_pending.load(std::memory_order_acquire);
}

bool TraderAdapter::identityResumeParticipant(const AccountTradingGate::ResumeRequest& request, std::string& diagnostic)
{
	std::shared_ptr<UnresolvedReportJournal> journal;
	{
		std::lock_guard<std::mutex> lock(_unresolved_report_mutex);
		journal = _unresolved_journal;
	}
	if (journal)
	{
		uint64_t highWater = 0;
		auto status = journal->queryPendingHighWater(highWater);
		if (status != UnresolvedReportJournal::Status::Ok)
		{
			diagnostic = _id + ": unresolved report journal unavailable: " + journal->lastError();
			return false;
		}
		if (highWater != 0)
		{
			status = journal->acknowledge(highWater, request.operator_id, request.reason);
			if (status != UnresolvedReportJournal::Status::Ok)
			{
				diagnostic = _id + ": unresolved report acknowledgement failed: " + journal->lastError();
				return false;
			}
		}
	}
	if (_cfg != NULL && _cfg->get("order_identity_journal") != NULL && !_identity_journal)
	{
		diagnostic = _id + ": order identity journal is configured but unavailable";
		return false;
	}
	if (_identity_journal)
	{
		if (!_identity_journal->isHealthy())
		{
			diagnostic = _id + ": order identity journal unhealthy: " + _identity_journal->lastError();
			return false;
		}
		std::lock_guard<std::mutex> identityLock(_identity_mutex);
		for (auto it = _unrecoverable_intents.begin(); it != _unrecoverable_intents.end();)
		{
			const DiffOrderOwnershipJournalV1::Snapshot snap = _identity_journal->snapshot();
			auto found = snap.intents.find(*it);
			double filled = found == snap.intents.end() ? 0.0 : found->second.reported_filled_quantity;
			auto status = _identity_journal->recordOrderProgress(*it, IDENTITY_STATUS_MANUAL, filled, 0.0, true);
			if (status != DiffOrderOwnershipJournalV1::Status::Ok && status != DiffOrderOwnershipJournalV1::Status::Duplicate)
			{
				diagnostic = _id + ": manual close of intent " + *it + " failed: " + _identity_journal->lastError();
				return false;
			}
			it = _unrecoverable_intents.erase(it);
		}
	}
	return true;
}

void TraderAdapter::configureIdentityJournal(const std::string& path, uint32_t floor,
	const std::string& floorEvidence, bool hasFloor)
{
	// Admission stays closed until the journal is open and any startup
	// recovery has been reconciled against the provider order snapshot.
	_identity_recovery_pending.store(true, std::memory_order_release);
	const std::string account = _account_id.empty() ? ("adapter:" + _id) : _account_id;
	std::shared_ptr<void> slot = _account_gate->attachment("order_identity_journal", [&]() {
		std::shared_ptr<IdentityJournalAttachment> attach = std::make_shared<IdentityJournalAttachment>();
		attach->path = path;
		std::shared_ptr<DiffOrderOwnershipJournalV1> journal = std::make_shared<DiffOrderOwnershipJournalV1>();
		std::string error;
		auto status = journal->open(path, account, &error);
		if (status == DiffOrderOwnershipJournalV1::Status::Ok)
			attach->journal = journal;
		else
			attach->error = fmt::format("open failed (status {}): {}", (int)status, error);
		return std::static_pointer_cast<void>(attach);
	});
	std::shared_ptr<IdentityJournalAttachment> attach = std::static_pointer_cast<IdentityJournalAttachment>(slot);
	if (attach->path != path)
	{
		pauseAccount("identity_journal", "account already uses identity journal " + attach->path);
		return;
	}
	if (!attach->journal)
	{
		pauseAccount("identity_journal", "order identity journal " + attach->error);
		return;
	}
	std::shared_ptr<DiffOrderOwnershipJournalV1> journal = attach->journal;
	DiffOrderOwnershipJournalV1::Snapshot snap = journal->snapshot();
	if (!snap.local_order_id_floor_established)
	{
		if (!hasFloor || floorEvidence.empty())
		{
			pauseAccount("identity_journal", "local_order_id_floor and local_order_id_floor_evidence are required for a new identity journal");
			return;
		}
		auto status = journal->advanceLocalOrderIdHighWater(floor, floorEvidence);
		if (status != DiffOrderOwnershipJournalV1::Status::Ok && status != DiffOrderOwnershipJournalV1::Status::Duplicate)
		{
			pauseAccount("identity_journal", fmt::format("local order id floor rejected (status {}): {}", (int)status, journal->lastError()));
			return;
		}
	}
	_identity_owner = "adapter:" + _id;
	_identity_journal = journal;
	size_t pending = 0;
	{
		std::lock_guard<std::mutex> identityLock(_identity_mutex);
		_recovery_intents.clear();
		const std::string ownerPrefix = _identity_owner + "/";
		for (const auto& item : snap.intents)
		{
			if (!item.second.terminal && (item.second.owner_id == _identity_owner ||
				item.second.owner_id.compare(0, ownerPrefix.size(), ownerPrefix) == 0))
				_recovery_intents[item.second.local_order_id] = item.first;
		}
		pending = _recovery_intents.size();
	}
	if (pending == 0)
		_identity_recovery_pending.store(false, std::memory_order_release);
	else
		WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN,
			"[{}] {} nonterminal order intents found in identity journal; admission closed until order snapshot reconciliation",
			_id.c_str(), pending);
}

void TraderAdapter::reconcileIdentityRecovery(const std::vector<std::pair<uint32_t, const WTSOrderInfo*>>& seen)
{
	if (!_identity_journal || !_identity_recovery_pending.load(std::memory_order_acquire))
		return;
	std::vector<std::string> unrecoverable;
	std::map<uint32_t, std::string> recovery;
	{
		std::lock_guard<std::mutex> identityLock(_identity_mutex);
		recovery.swap(_recovery_intents);
	}
	const DiffOrderOwnershipJournalV1::Snapshot snap = _identity_journal->snapshot();
	for (const auto& item : recovery)
	{
		const WTSOrderInfo* order = NULL;
		for (const auto& candidate : seen)
		{
			if (candidate.first == item.first)
			{
				order = candidate.second;
				break;
			}
		}
		if (order == NULL)
		{
			unrecoverable.push_back(item.second);
			continue;
		}
		auto intent = snap.intents.find(item.second);
		if (intent != snap.intents.end() && !intent->second.order_bound)
		{
			auto status = _identity_journal->bindOrder(item.second, item.first, order->getEntrustID());
			if (status != DiffOrderOwnershipJournalV1::Status::Ok && status != DiffOrderOwnershipJournalV1::Status::Duplicate)
			{
				unrecoverable.push_back(item.second);
				continue;
			}
		}
		{
			// Recovered from the durable journal and the provider snapshot: the
			// identity (and its owner) is verified again.
			std::string recoveredOwner;
			const std::string ownerPrefix = _identity_owner + "/";
			if (intent != snap.intents.end() && intent->second.owner_id.compare(0, ownerPrefix.size(), ownerPrefix) == 0)
				recoveredOwner = intent->second.owner_id.substr(ownerPrefix.size());
			std::lock_guard<std::mutex> lock(_ledger_mutex);
			auto entry = _ledger.find(item.first);
			if (entry != _ledger.end())
			{
				entry->second.intent_id = item.second;
				const double accounted = entry->second.accounted;
				ledgerSetAccountedLocked(entry->second, 0);
				entry->second.owner_id = recoveredOwner;
				ledgerSetAccountedLocked(entry->second, accounted);
				entry->second.verified = true;
			}
		}
		if (!order->isAlive())
			journalTerminal(item.first, item.second, (uint32_t)order->getOrderState(), order->getVolTraded());
	}
	if (!unrecoverable.empty())
	{
		std::string list;
		for (const std::string& intent : unrecoverable)
			list += (list.empty() ? "" : ",") + intent;
		{
			std::lock_guard<std::mutex> identityLock(_identity_mutex);
			_unrecoverable_intents.insert(_unrecoverable_intents.end(), unrecoverable.begin(), unrecoverable.end());
		}
		// Card 5a: never resend; the whole account waits for manual reconciliation.
		pauseAccount("identity_recovery", "order intents not recoverable from provider snapshot: " + list);
	}
	_identity_recovery_pending.store(false, std::memory_order_release);
}

void TraderAdapter::journalTerminal(uint32_t localid, const std::string& intent, uint32_t status, double filled)
{
	std::shared_ptr<DiffOrderOwnershipJournalV1> journal = _identity_journal;
	if (!journal)
		return;
	std::string intentId = intent;
	{
		std::lock_guard<std::mutex> lock(_ledger_mutex);
		auto it = _ledger.find(localid);
		if (it != _ledger.end())
		{
			if (it->second.journal_terminal)
				return;
			it->second.journal_terminal = true;
			if (intentId.empty())
				intentId = it->second.intent_id;
		}
	}
	if (intentId.empty())
	{
		std::string owner;
		if (journal->ownerForLocalOrder(localid, owner, &intentId) != DiffOrderOwnershipJournalV1::Status::Ok)
			return;	// not an order registered in this account's journal
	}
	auto result = journal->recordOrderProgress(intentId, status, filled, 0.0, true);
	if (result != DiffOrderOwnershipJournalV1::Status::Ok && result != DiffOrderOwnershipJournalV1::Status::Duplicate)
		pauseAccount("identity_journal", fmt::format("terminal state of order {} not persisted (status {}): {}",
			localid, (int)result, journal->lastError()));
}

uint64_t TraderAdapter::nextEventVersion()
{
	std::lock_guard<std::mutex> lock(_ledger_mutex);
	return ++_event_version;
}

void TraderAdapter::ledgerPruneLocked()
{
	while (_ledger_tombstones.size() > LEDGER_TOMBSTONE_LIMIT)
	{
		const uint32_t victim = _ledger_tombstones.front();
		_ledger_tombstones.pop_front();
		auto it = _ledger.find(victim);
		if (it != _ledger.end() && it->second.state == OrderLedgerState::Terminal)
			_ledger.erase(it);
	}
}

void TraderAdapter::ledgerSetAccountedLocked(OrderLedgerEntry& entry, double accounted)
{
	const double delta = accounted - entry.accounted;
	if (delta != 0)
	{
		updateUndone(entry.std_code.c_str(), entry.sign * delta, true);
		if (!entry.owner_id.empty())
			_owner_undone[entry.owner_id][entry.std_code] += entry.sign * delta;
	}
	entry.accounted = accounted;
}

void TraderAdapter::ledgerRecomputeLocked(OrderLedgerEntry& entry)
{
	// I03-NEW-057: one fill can arrive as an order report (volLeft shrinks)
	// and as a trade report, in either order; deduct it once.
	double left = entry.volume - entry.trade_filled;
	if (entry.order_left >= 0)
		left = (std::min)(left, entry.order_left);
	if (!(left > 0) || !std::isfinite(left))
		left = 0;
	ledgerSetAccountedLocked(entry, left);
}

void TraderAdapter::ledgerTombstoneLocked(uint32_t localid, OrderLedgerEntry& entry, OrderLedgerState state)
{
	ledgerSetAccountedLocked(entry, 0);
	entry.state = state;
	if (state == OrderLedgerState::Terminal)
	{
		_ledger_tombstones.push_back(localid);
		ledgerPruneLocked();
	}
}

bool TraderAdapter::ledgerApplyOrderLocked(uint32_t localid, const std::string& stdCode, bool isBuy,
	bool alive, double volume, double volLeft, uint64_t version)
{
	if (volLeft < 0 || !std::isfinite(volLeft))
		volLeft = 0;
	if (!std::isfinite(volume) || volume < volLeft)
		volume = volLeft;
	auto it = _ledger.find(localid);
	if (it == _ledger.end())
	{
		OrderLedgerEntry entry;
		entry.std_code = stdCode;
		entry.sign = isBuy ? 1.0 : -1.0;
		entry.created_version = entry.last_version = version;
		entry.state = OrderLedgerState::Live;
		entry.volume = volume;
		entry.order_left = volLeft;
		OrderLedgerEntry& stored = _ledger[localid] = entry;
		if (alive)
		{
			ledgerRecomputeLocked(stored);
			return false;
		}
		ledgerTombstoneLocked(localid, stored, OrderLedgerState::Terminal);
		return true;
	}
	OrderLedgerEntry& entry = it->second;
	if (version > entry.last_version)
		entry.last_version = version;
	if (entry.state == OrderLedgerState::Terminal)
		return false;	// tombstone: late/duplicate report never reopens undone qty
	if (!alive)
	{
		ledgerTombstoneLocked(localid, entry, OrderLedgerState::Terminal);
		return true;
	}
	// Remaining quantity only decreases; an Absent tombstone is revived by a
	// newer realtime report. The provider's order volume replaces the entrust
	// quantity once reported.
	if (volume > 0)
		entry.volume = volume;
	entry.order_left = (entry.state == OrderLedgerState::Absent || entry.order_left < 0) ?
		volLeft : (std::min)(entry.order_left, volLeft);
	ledgerRecomputeLocked(entry);
	entry.state = OrderLedgerState::Live;
	return false;
}

bool TraderAdapter::orderIdentity(uint32_t localid, OrderIdentityToken& token) const
{
	std::lock_guard<std::mutex> lock(_ledger_mutex);
	auto it = _ledger.find(localid);
	if (it == _ledger.end() || it->second.intent_id.empty())
		return false;
	token.account = _account_gate->key();
	token.intent_id = it->second.intent_id;
	token.localid = localid;
	return true;
}

bool TraderAdapter::cancelVerified(const OrderIdentityToken& token)
{
	if (!token.valid() || token.account != _account_gate->key())
		return false;
	// User decision 2026-11-04: while paused, only verified live identities
	// may be cancelled; the token itself proves the identity (and its owner).
	const bool paused = isTradingAdmissionClosed();
	{
		std::lock_guard<std::mutex> lock(_ledger_mutex);
		auto it = _ledger.find(token.localid);
		if (it == _ledger.end() || it->second.intent_id != token.intent_id ||
			it->second.state == OrderLedgerState::Terminal || it->second.state == OrderLedgerState::Absent ||
			it->second.cancel_claimed || (paused && (!it->second.verified || it->second.state != OrderLedgerState::Live)))
			return false;
		it->second.cancel_claimed = true;
	}
	struct Unclaim
	{
		TraderAdapter* self; uint32_t localid;
		~Unclaim()
		{
			std::lock_guard<std::mutex> lock(self->_ledger_mutex);
			auto it = self->_ledger.find(localid);
			if (it != self->_ledger.end())
				it->second.cancel_claimed = false;
		}
	} unclaim{ this, token.localid };
	WTSOrderInfo* ordInfo = NULL;
	{
		SpinLock lock(_mtx_orders);
		if (_orders != NULL)
			ordInfo = (WTSOrderInfo*)_orders->grab(token.localid);
	}
	if (ordInfo == NULL)
		return false;
	bool ok = ordInfo->isAlive() && doCancel(ordInfo);
	ordInfo->release();
	return ok;
}

TraderAdapter::OrderLedgerView TraderAdapter::orderLedger(uint32_t localid) const
{
	OrderLedgerView view;
	std::lock_guard<std::mutex> lock(_ledger_mutex);
	auto it = _ledger.find(localid);
	if (it == _ledger.end())
		return view;
	view.found = true;
	view.state = it->second.state;
	view.accounted = it->second.accounted;
	view.last_version = it->second.last_version;
	view.std_code = it->second.std_code;
	view.intent_id = it->second.intent_id;
	view.owner_id = it->second.owner_id;
	view.verified = it->second.verified;
	return view;
}

uint64_t TraderAdapter::ordersSnapshotVersion() const
{
	std::lock_guard<std::mutex> lock(_ledger_mutex);
	return _orders_snapshot_version;
}

void TraderAdapter::retainUnresolvedPayload(const std::string& payload, const char* kind)
{
	try
	{
		UnresolvedTradeReport record;
		record.payload = payload;
		record.diagnostic = "journal not configured; retained in memory only";
		size_t index;
		std::shared_ptr<UnresolvedReportJournal> journal;
		{
			std::lock_guard<std::mutex> lock(_unresolved_report_mutex);
			index = _unresolved_reports.size();
			_unresolved_reports.push_back(record);
			journal = _unresolved_journal;
			_unresolved_diagnostic = record.diagnostic;
		}
		if (journal != NULL)
		{
			uint64_t sequence = 0;
			std::string diagnostic;
			bool persisted = false;
			try
			{
				auto status = journal->append(record.payload, sequence);
				persisted = status == UnresolvedReportJournal::Status::Ok;
				diagnostic = persisted ? "journal append succeeded; reconciliation required" :
					"journal append failed: " + journal->lastError();
			}
			catch (const std::exception& e) { diagnostic = std::string("journal append exception: ") + e.what(); }
			catch (...) { diagnostic = "journal append exception"; }
			std::lock_guard<std::mutex> lock(_unresolved_report_mutex);
			_unresolved_reports[index].persisted = persisted;
			_unresolved_reports[index].journal_sequence = sequence;
			_unresolved_reports[index].diagnostic = diagnostic;
			_unresolved_diagnostic = diagnostic;
		}
	}
	catch (...)
	{
		try {
			std::lock_guard<std::mutex> lock(_unresolved_report_mutex);
			_unresolved_diagnostic = std::string("unable to retain WTS ") + (kind ? kind : "report") + " snapshot";
		} catch (...) {}
	}
}

void TraderAdapter::quarantineOrder(const WTSOrderInfo* order, const char* reason)
{
	pauseAccount("unresolved_report", std::string("unresolved order report: ") + (reason ? reason : ""));
	try
	{
		rj::StringBuffer buffer;
		rj::Writer<rj::StringBuffer> writer(buffer);
		auto str = [&writer](const char* key, const char* value) {
			writer.Key(key); writer.String(value ? value : "");
		};
		auto real = [&writer](const char* key, double value) {
			std::ostringstream out; out.imbue(std::locale::classic());
			out << std::hexfloat << value;
			writer.Key(key); writer.String(out.str().c_str());
		};
		writer.StartObject();
		writer.Key("schema_version"); writer.Uint(1);
		str("record_type", "wts_order_visible_snapshot");
		writer.Key("provider_field_completeness_verified"); writer.Bool(false);
		str("adapter_id", _id.c_str());
		str("reason", reason);
		str("exchange", order->getExchg());
		str("code", order->getCode());
		str("order_id", order->getOrderID());
		str("entrust_id", order->getEntrustID());
		str("user_tag", order->getUserTag());
		writer.Key("order_date"); writer.Uint(order->getOrderDate());
		writer.Key("order_time"); writer.Uint64(order->getOrderTime());
		writer.Key("direction"); writer.Int(static_cast<int>(order->getDirection()));
		writer.Key("offset_type"); writer.Int(static_cast<int>(order->getOffsetType()));
		writer.Key("order_state"); writer.Int(static_cast<int>(order->getOrderState()));
		real("price_hex", order->getPrice());
		real("volume_hex", order->getVolume());
		real("traded_hex", order->getVolTraded());
		real("left_hex", order->getVolLeft());
		writer.EndObject();
		retainUnresolvedPayload(std::string(buffer.GetString(), buffer.GetSize()), "order");
	}
	catch (...) {}
}

void TraderAdapter::configureUnresolvedJournal(const std::string& path)
{
	if (path.empty())
	{
		std::lock_guard<std::mutex> lock(_unresolved_report_mutex);
		_unresolved_journal.reset();
		_unresolved_diagnostic = "journal not configured";
		return;
	}
	// The journal requires reconciliation on every startup, even if empty.
	pauseAccount("unresolved_journal", "journal startup reconciliation required");
	{
		std::lock_guard<std::mutex> lock(_unresolved_report_mutex);
		_unresolved_journal.reset();
		_unresolved_diagnostic = "journal opening; reconciliation required";
	}
	try
	{
		std::shared_ptr<UnresolvedReportJournal> journal = std::make_shared<UnresolvedReportJournal>();
		std::string error;
		auto status = journal->open(path, &error);
		std::string diagnostic;
		if (status != UnresolvedReportJournal::Status::Ok)
			diagnostic = "journal open failed: " + error;
		else
		{
			std::vector<UnresolvedReportJournal::Report> pending;
			auto pendingStatus = journal->pending(pending);
			if (pendingStatus != UnresolvedReportJournal::Status::Ok)
				diagnostic = "journal pending query failed: " + journal->lastError();
			else if (!pending.empty())
				diagnostic = "journal contains unresolved reports; reconciliation required";
			else
				diagnostic = "journal opened; startup reconciliation required";
		}
		std::lock_guard<std::mutex> lock(_unresolved_report_mutex);
		_unresolved_diagnostic = diagnostic;
		_unresolved_journal = std::move(journal);
	}
	catch (const std::exception& e)
	{
		std::lock_guard<std::mutex> lock(_unresolved_report_mutex);
		_unresolved_diagnostic = std::string("journal open exception: ") + e.what();
	}
	catch (...)
	{
		std::lock_guard<std::mutex> lock(_unresolved_report_mutex);
		_unresolved_diagnostic = "journal open exception";
	}
}

void TraderAdapter::quarantineTrade(const WTSTradeInfo* trade, const char* reason)
{
	// Linearization point for new admissions. No gate lock spans provider calls.
	pauseAccount("unresolved_report", std::string("unresolved trade report: ") + (reason ? reason : ""));
	try
	{
		rj::StringBuffer buffer;
		rj::Writer<rj::StringBuffer> writer(buffer);
		auto str = [&writer](const char* key, const char* value) {
			writer.Key(key); writer.String(value ? value : "");
		};
		auto real = [&writer](const char* key, double value) {
			// Hexadecimal strings preserve the visible binary floating value and
			// also allow non-finite values without producing invalid JSON.
			std::ostringstream out; out.imbue(std::locale::classic());
			out << std::hexfloat << value;
			writer.Key(key); writer.String(out.str().c_str());
		};
		writer.StartObject();
		writer.Key("schema_version"); writer.Uint(1);
		str("record_type", "wts_trade_visible_snapshot");
		writer.Key("provider_field_completeness_verified"); writer.Bool(false);
		str("adapter_id", _id.c_str());
		str("reason", reason);
		str("exchange", trade->getExchg());
		str("code", trade->getCode());
		str("trade_id", trade->getTradeID());
		str("ref_order", trade->getRefOrder());
		str("user_tag", trade->getUserTag());
		writer.Key("trade_date"); writer.Uint(trade->getTradeDate());
		writer.Key("trade_time"); writer.Uint64(trade->getTradeTime());
		writer.Key("direction"); writer.Int(static_cast<int>(trade->getDirection()));
		writer.Key("offset_type"); writer.Int(static_cast<int>(trade->getOffsetType()));
		writer.Key("order_type"); writer.Int(static_cast<int>(trade->getOrderType()));
		writer.Key("trade_type"); writer.Int(static_cast<int>(trade->getTradeType()));
		writer.Key("business_type"); writer.Int(static_cast<int>(trade->getBusinessType()));
		writer.Key("is_net"); writer.Bool(trade->isNet());
		writer.Key("is_net_buy"); writer.Bool(trade->isNetBuy());
		real("price_hex", trade->getPrice());
		real("volume_hex", trade->getVolume());
		real("amount_hex", trade->getAmount());
		writer.EndObject();

		UnresolvedTradeReport record;
		record.payload.assign(buffer.GetString(), buffer.GetSize());
		record.diagnostic = "journal not configured; retained in memory only";
		size_t index;
		std::shared_ptr<UnresolvedReportJournal> journal;
		{
			std::lock_guard<std::mutex> lock(_unresolved_report_mutex);
			index = _unresolved_reports.size();
			_unresolved_reports.push_back(record);
			journal = _unresolved_journal;
			_unresolved_diagnostic = record.diagnostic;
		}
		if (journal != NULL)
		{
			uint64_t sequence = 0;
			std::string diagnostic;
			bool persisted = false;
			try
			{
				auto status = journal->append(record.payload, sequence);
				persisted = status == UnresolvedReportJournal::Status::Ok;
				diagnostic = persisted ? "journal append succeeded; reconciliation required" :
					"journal append failed: " + journal->lastError();
			}
			catch (const std::exception& e) { diagnostic = std::string("journal append exception: ") + e.what(); }
			catch (...) { diagnostic = "journal append exception"; }
			std::lock_guard<std::mutex> lock(_unresolved_report_mutex);
			_unresolved_reports[index].persisted = persisted;
			_unresolved_reports[index].journal_sequence = sequence;
			_unresolved_reports[index].diagnostic = diagnostic;
			_unresolved_diagnostic = diagnostic;
		}
	}
	catch (...)
	{
		// Even when retaining the snapshot itself fails (e.g. allocation failure),
		// never reopen the admission gate.
		try {
			std::lock_guard<std::mutex> lock(_unresolved_report_mutex);
			_unresolved_diagnostic = "unable to retain WTS trade snapshot";
		} catch (...) {}
	}
}

namespace
{
	// Flags recorded for submissions that are still inside orderInsert.
	const uint8_t INFLIGHT_REJECTED = 1;	///< rejection or cancel report observed
	const uint8_t INFLIGHT_ACCEPTED = 2;	///< live order or trade report observed
	const uint8_t INFLIGHT_NONE = 0x80;		///< localid is not being submitted

}

uint8_t TraderAdapter::noteInflightReport(uint32_t localid, uint8_t bits)
{
	if (localid == 0)
		return INFLIGHT_NONE;
	std::lock_guard<std::mutex> lock(_inflight_mutex);
	auto it = _inflight_entrusts.find(localid);
	if (it == _inflight_entrusts.end())
		return INFLIGHT_NONE;
	const uint8_t prev = it->second;
	it->second = static_cast<uint8_t>(prev | bits);
	return prev;
}

bool TraderAdapter::frequencyAvailable(bool isOrder, const char* stdCode, const RiskParams* riskPara) const
{
	if (riskPara == NULL)
		return true;
	FrequencyWindow<CodeTimeCacheMap>& window = isOrder ? _order_frequency : _cancel_frequency;
	const uint32_t boundary = isOrder ? riskPara->_order_times_boundary : riskPara->_cancel_times_boundary;
	const uint32_t seconds = isOrder ? riskPara->_order_stat_timespan : riskPara->_cancel_stat_timespan;
	return window.available(stdCode, boundary, seconds, rateNow());
}

uint32_t TraderAdapter::doEntrust(WTSEntrust* entrust)
{
	if (isTradingAdmissionClosed())
		return UINT_MAX;
	// Risk checks are performed with the caller's standardized code. Preserve
	// that key before replacing the submitted code with the exchange alias.
	const std::string riskCode = entrust->getCode();

	if (!checkOrderLimits(riskCode.c_str()))
		return UINT_MAX;
	const RiskParams* riskPara = _risk_mon_enabled ? getRiskParams(riskCode.c_str()) : NULL;
	const uint32_t boundary = riskPara ? riskPara->_order_times_boundary : 0;
	const uint32_t seconds = riskPara ? riskPara->_order_stat_timespan : 0;
	// Per-request reservation: N requests (committed or still pending) are
	// admitted in the sliding window ending now; N+1 never reaches the API.
	FrequencyWindow<CodeTimeCacheMap>::Reservation reservation(_order_frequency, riskCode,
		boundary, seconds, rateNow());
	if (!reservation)
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR,
			"[{}] {} entrust rejected by frequency limit: {} requests within {} seconds, temporarily banned until the window expires",
			_id.c_str(), riskCode.c_str(), boundary, seconds);
		return UINT_MAX;
	}

	if (isTradingAdmissionClosed())
		return UINT_MAX;
	// Owner of this entry (innermost OwnerScope bound to this adapter).
	std::string owner;
	bool sharedCapable = false;
	for (OwnerScope::Frame* frame = g_owner_scope; frame != NULL; frame = frame->previous)
	{
		if (frame->adapter == this)
		{
			owner = frame->owner;
			sharedCapable = frame->shared_capable;
			break;
		}
	}
	if (!owner.empty())
	{
		// Cards 5b/5c: shared ownership of an account contract requires owner
		// routing and a host-identity capable unit; otherwise non-shared mode.
		const bool shared = sharedCapable && isOwnerRoutingEnabled();
		std::string diagnostic;
		if (!_account_gate->claimContract(riskCode, owner, shared, diagnostic))
		{
			WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR, "[{}] entrust rejected: {}", _id.c_str(), diagnostic.c_str());
			return UINT_MAX;
		}
	}
	_trader_api->makeEntrustID(entrust->getEntrustID(), 64);

	WTSContractInfo* cInfo = entrust->getContractInfo();
	if (cInfo == NULL) cInfo = getContract(entrust->getCode());
	if (cInfo == NULL)
	{
		// L01-014: unknown/unparseable code; nothing reserved or sent yet.
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR, "[{}] entrust rejected: contract of {} unavailable",
			_id.c_str(), riskCode.c_str());
		return UINT_MAX;
	}

	/*
	 *	By Wesley @ 2024.04.12
	 *	将AltCode设置为交易code，altcode为交易所原始合约代码
	 */
	entrust->setCode(cInfo->getAltCode());

	entrust->setExchange(cInfo->getExchg());

	const bool isBuy = (entrust->getDirection() == WDT_LONG && entrust->getOffsetType() == WOT_OPEN) ||
		(entrust->getDirection() == WDT_SHORT && entrust->getOffsetType() != WOT_OPEN);
	const double qty = entrust->getVolume();
	// Versioned identity (card 5): unique per boot, durable with a journal.
	const std::string intentId = fmt::format("{}:{}:{}", _id, _boot_nonce,
		_intent_sequence.fetch_add(1, std::memory_order_relaxed) + 1);
	std::shared_ptr<DiffOrderOwnershipJournalV1> journal = _identity_journal;
	uint32_t localid = 0;
	if (journal)
	{
		// Card 5a: the identity is durably registered before any provider call;
		// the reserved id comes from the account WAL high-water, never reused.
		const std::string journalOwner = owner.empty() ? _identity_owner : _identity_owner + "/" + owner;
		auto status = journal->prepareOrderIntent(journalOwner, intentId, riskCode,
			isBuy ? qty : -qty, localid);
		if (status != DiffOrderOwnershipJournalV1::Status::Ok)
		{
			pauseAccount("identity_journal", fmt::format("order intent registration failed before send (status {}): {}",
				(int)status, journal->lastError()));
			return UINT_MAX;	// reservation not begun: rolled back
		}
	}
	else
		localid = makeLocalOrderID();
	char* usertag = entrust->getUserTag();
	wt_strcpy(usertag, _order_pattern.c_str(), _order_pattern.size());
	usertag[_order_pattern.size()] =  '.';
	fmtutil::format_to(usertag + _order_pattern.size() + 1, "{}", localid);

	// A live ledger entry under this id would alias two orders: fail closed.
	bool aliased = false;
	{
		std::lock_guard<std::mutex> lock(_ledger_mutex);
		auto existing = _ledger.find(localid);
		aliased = existing != _ledger.end() && existing->second.state != OrderLedgerState::Terminal;
	}
	if (aliased)
	{
		pauseAccount("identity", fmt::format("local order id {} is still live; refusing to reuse it", localid));
		journalTerminal(0, intentId, IDENTITY_STATUS_NOT_SENT, 0);
		return UINT_MAX;
	}

	auto closeUnsent = [&]() {
		{
			std::lock_guard<std::mutex> lock(_ledger_mutex);
			auto it = _ledger.find(localid);
			if (it != _ledger.end() && it->second.intent_id == intentId && it->second.state != OrderLedgerState::Terminal)
				ledgerTombstoneLocked(localid, it->second, OrderLedgerState::Terminal);
		}
		journalTerminal(localid, intentId, IDENTITY_STATUS_NOT_SENT, 0);
	};

	// R15-001: per-order caller data is registered before the provider call,
	// so synchronous reports can already find it. An observer failure aborts
	// the entry before anything is sent.
	for (EntrustObserverScope::Frame* frame = g_entrust_observer; frame != NULL; frame = frame->previous)
	{
		if (frame->adapter != this)
			continue;
		bool observed = false;
		try
		{
			if (frame->observer)
				frame->observer(localid);
			observed = true;
		}
		catch (...) {}
		if (!observed)
		{
			WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR,
				"[{}] entrust of local order {} aborted before send: entry observer failed", _id.c_str(), localid);
			journalTerminal(0, intentId, IDENTITY_STATUS_NOT_SENT, 0);
			return UINT_MAX;
		}
		break;
	}

	if (isTradingAdmissionClosed())
	{
		journalTerminal(0, intentId, IDENTITY_STATUS_NOT_SENT, 0);
		return UINT_MAX;
	}
	reservation.beginSubmission(rateNow());
	if (isTradingAdmissionClosed())
	{
		reservation.rollback(); // provider has not been called
		journalTerminal(0, intentId, IDENTITY_STATUS_NOT_SENT, 0);
		return UINT_MAX;
	}
	{
		// The ledger entry owns this order's undone quantity from now on; it is
		// created before the provider call so synchronous reports find it.
		std::lock_guard<std::mutex> lock(_ledger_mutex);
		OrderLedgerEntry entry;
		entry.std_code = riskCode;
		entry.intent_id = intentId;
		entry.owner_id = owner;
		entry.verified = true;
		entry.sign = isBuy ? 1.0 : -1.0;
		entry.created_version = entry.last_version = ++_event_version;
		entry.state = OrderLedgerState::Submitting;
		entry.volume = qty;
		OrderLedgerEntry& stored = _ledger[localid] = entry;
		ledgerRecomputeLocked(stored);
	}

	// Track the localid while the provider call is in progress so reports
	// delivered synchronously (or concurrently) can be reconciled below.
	struct InflightGuard
	{
		TraderAdapter* self;
		uint32_t localid;
		bool active;
		uint8_t finish()
		{
			uint8_t bits = 0;
			if (active)
			{
				std::lock_guard<std::mutex> lock(self->_inflight_mutex);
				auto it = self->_inflight_entrusts.find(localid);
				if (it != self->_inflight_entrusts.end())
				{
					bits = it->second;
					self->_inflight_entrusts.erase(it);
				}
				active = false;
			}
			return bits;
		}
		~InflightGuard() { finish(); }
	};
	{
		std::lock_guard<std::mutex> lock(_inflight_mutex);
		_inflight_entrusts[localid] = 0;
	}
	InflightGuard inflight{ this, localid, true };

	// An exception leaves the reservation committed and the undone quantity
	// reserved (unknown outcome); the account is paused for reconciliation.
	int32_t ret = -1;
	try
	{
		ret = _trader_api->orderInsert(entrust);
	}
	catch (...)
	{
		pauseAccount("order_insert", fmt::format("orderInsert of local order {} threw; outcome unknown", localid));
		throw;
	}
	const uint8_t seen = inflight.finish();

	if (seen & INFLIGHT_REJECTED)
	{
		// The provider processed the request and already reported it dead.
		reservation.commit();
		{
			std::lock_guard<std::mutex> lock(_ledger_mutex);
			auto it = _ledger.find(localid);
			if (it != _ledger.end() && it->second.state != OrderLedgerState::Terminal)
				ledgerTombstoneLocked(localid, it->second, OrderLedgerState::Terminal);
		}
		journalTerminal(localid, intentId, IDENTITY_STATUS_REJECTED, 0);
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR,
			"[{}] Order {} of {} was rejected synchronously by provider, ret: {}", _id.c_str(), localid, riskCode.c_str(), ret);
		return UINT_MAX;
	}

	if (ret < 0 && (seen & INFLIGHT_ACCEPTED) == 0)
	{
		// Synchronous failure: the request did not reach the provider, so
		// the frequency slot is returned.
		reservation.rollback();
		closeUnsent();
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR, "[{}] Order placing failed: {}", _id.c_str(), ret);
		return UINT_MAX;
	}

	if (ret < 0)
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR,
			"[{}] Order {} returned {} but provider already reported it live; keeping it", _id.c_str(), localid, ret);
	}
	reservation.commit();
	{
		std::lock_guard<std::mutex> lock(_ledger_mutex);
		auto it = _ledger.find(localid);
		if (it != _ledger.end() && it->second.state == OrderLedgerState::Submitting)
			it->second.state = OrderLedgerState::Live;
	}
	if (journal)
	{
		auto status = journal->bindOrder(intentId, localid, entrust->getEntrustID());
		if (status != DiffOrderOwnershipJournalV1::Status::Ok && status != DiffOrderOwnershipJournalV1::Status::Duplicate)
			pauseAccount("identity_journal", fmt::format("order {} sent but binding not persisted (status {}): {}",
				localid, (int)status, journal->lastError()));
	}
	return localid;
}

bool TraderAdapter::submitSplit(OrderIDs& ids, const char* stdCode, bool isBuy, double qty,
	const std::function<uint32_t()>& submit)
{
	(void)stdCode; (void)isBuy; (void)qty;
	// doEntrust reserves undone qty in the per-order ledger right before the
	// provider call and releases it exactly once on any terminal outcome.
	const uint32_t localid = submit();
	if (localid != UINT_MAX)
	{
		ids.emplace_back(localid);
		return true;
	}
	return false;
}

// Caller holds _ledger_mutex.
void TraderAdapter::updateUndone(const char* stdCode, double qty, bool bOuput /* = false */)
{
	double& undone = _undone_qty[stdCode];
	double oldQty = undone;
	undone += qty;

	if (bOuput)
		WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO, "[{}] {} qty of undone order updated, {} -> {}", _id.c_str(), stdCode, oldQty, undone);
}

WTSContractInfo* TraderAdapter::getContract(const char* stdCode)
{
	if (_bd_mgr == NULL || stdCode == NULL)
		return NULL;
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, NULL);
	return _bd_mgr->getContract(cInfo._code, cInfo._exchg);
}

WTSCommodityInfo* TraderAdapter::getCommodify(const char* stdCode)
{
	if (_bd_mgr == NULL || stdCode == NULL)
		return NULL;
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, NULL);
	return _bd_mgr->getCommodity(cInfo._exchg, cInfo._product);
}

bool TraderAdapter::checkCancelLimits(const char* stdCode)
{
	if (!_risk_mon_enabled)
		return true;

	const RiskParams* riskPara = getRiskParams(stdCode);
	{
		std::lock_guard<std::mutex> lock(_risk_exclude_mutex);
		if (_exclude_codes.find(stdCode) != _exclude_codes.end())
			return false;
		WTSTradeStateInfo* statInfo = _stat_map ? (WTSTradeStateInfo*)_stat_map->get(stdCode) : NULL;
		if (riskPara && statInfo && riskPara->_cancel_total_limits != 0 &&
			statInfo->total_cancels() >= riskPara->_cancel_total_limits)
		{
			_exclude_codes.insert(stdCode);
			return false;
		}
	}
	return frequencyAvailable(false, stdCode, riskPara);
}

bool TraderAdapter::isTradeEnabled(const char* stdCode) const
{
	if (!_risk_mon_enabled)
		return true;
	{
		std::lock_guard<std::mutex> lock(_risk_exclude_mutex);
		if (_exclude_codes.find(stdCode) != _exclude_codes.end())
			return false;
	}
	// A full order window is a temporary ban that lifts automatically when
	// recorded requests expire.
	return frequencyAvailable(true, stdCode, const_cast<TraderAdapter*>(this)->getRiskParams(stdCode));
}

bool TraderAdapter::checkOrderLimits(const char* stdCode)
{
	if (!_risk_mon_enabled)
		return true;

	const RiskParams* riskPara = getRiskParams(stdCode);
	{
		std::lock_guard<std::mutex> lock(_risk_exclude_mutex);
		if (_exclude_codes.find(stdCode) != _exclude_codes.end())
			return false;
		WTSTradeStateInfo* statInfo = _stat_map ? (WTSTradeStateInfo*)_stat_map->get(stdCode) : NULL;
		if (riskPara && statInfo && riskPara->_order_total_limits != 0 &&
			statInfo->total_orders() >= riskPara->_order_total_limits)
		{
			_exclude_codes.insert(stdCode);
			return false;
		}
	}
	return frequencyAvailable(true, stdCode, riskPara);
}

OrderIDs TraderAdapter::buy(const char* stdCode, double price, double qty, int flag, bool bForceClose, WTSContractInfo* cInfo /* = NULL */)
{
	OrderIDs ret;
	if (isTradingAdmissionClosed())
		return ret;
	if (qty == 0)
		return ret;

	if (isSelfMatched(stdCode))
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR,
			"[{0}] Instructions on {1} are forbidden: {1} is in self matches", _id.c_str(), stdCode);
		return ret;
	}

	if(cInfo == NULL) cInfo = getContract(stdCode);
	// L01-014: an unknown or unparseable standard code never reaches the
	// ledger or the provider (fail closed instead of dereferencing NULL).
	if (cInfo == NULL || cInfo->getCommInfo() == NULL || cInfo->getCommInfo()->getSessionInfo() == NULL)
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR, "[{}] {} rejected: contract, commodity or session of {} unavailable",
			_id.c_str(), "buy", stdCode ? stdCode : "");
		return ret;
	}
	WTSCommodityInfo* commInfo = cInfo->getCommInfo();
	WTSSessionInfo* sInfo = commInfo->getSessionInfo();

	if (!sInfo->isInTradingTime(WtHelper::getTime(), true))
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR,
			"[{}] Buying {} of quantity {}, {:04d} not in trading time", _id.c_str(), stdCode, qty, WtHelper::getTime());
		return ret;
	}


	WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO, "[{}] Buying {} of quantity {}", _id.c_str(), stdCode, qty);

	bool stopped = false;

	// Snapshot: re-entrant reports may modify _positions while splits are submitted.
	const PosItem pItem = _positions[stdCode];
	WTSTradeStateInfo* statInfo = (WTSTradeStateInfo*)_stat_map->get(stdCode);
	if (statInfo == NULL)
	{
		statInfo = WTSTradeStateInfo::create(stdCode);
		_stat_map->add(stdCode, statInfo, false);
	}
	TradeStatInfo& statItem = statInfo->statInfo();

	const ActionRuleGroup& ruleGP = _policy_mgr->getActionRules(commInfo->getFullPid());

	double left = qty;

	double unitQty = (price == 0.0) ? cInfo->getMaxMktVol() : cInfo->getMaxLmtVol();
	if (decimal::eq(unitQty, 0))
		unitQty = DBL_MAX;

	for (auto it = ruleGP.begin(); it != ruleGP.end(); it++)
	{
		const ActionRule& curRule = (*it);
		if(curRule._atype == AT_Open && !bForceClose)
		{
			//先检查是否已经到了限额
			//买入开仓, 即开多仓
			double maxQty = left;

			if (curRule._limit_l != 0)
			{
				if (statItem.l_openvol >= curRule._limit_l)
				{
					WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN, "[{}] {} long position opened today is up to limit {}", _id.c_str(), stdCode, curRule._limit_l);
					continue;
				}
				else
				{
					maxQty = min(maxQty, curRule._limit_l - statItem.l_openvol);
				}
			}

			if (curRule._limit != 0)
			{
				if (statItem.l_openvol + statItem.s_openvol >= curRule._limit)
				{
					WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN, "[{}] {} position opened today is up to limit {}", _id.c_str(), stdCode, curRule._limit);
					continue;
				}
				else
				{
					maxQty = min(maxQty, curRule._limit - statItem.l_openvol - statItem.s_openvol);
				}
			}

			//这里还要考虑单笔最大委托数量
			double leftQty = maxQty;
			for (;;)
			{
				double curQty = min(leftQty, unitQty);
				if (!submitSplit(ret, stdCode, true, curQty, [&]() { return openLong(stdCode, price, curQty, flag, cInfo); }))
				{
					stopped = true;
					break;
				}

				leftQty -= curQty;

				if (decimal::eq(leftQty, 0))
					break;
			}			

			left -= maxQty;

			WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO, 
				"[{}] Signal of buying {} of quantity {} triggered: Opening long of quantity {}", _id.c_str(), stdCode, qty, maxQty);
		}
		else if(curRule._atype == AT_CloseToday)
		{
			double maxQty = 0;
			//如果要区分平昨平今的品种, 则只读取可平今仓即可
			//如果不区分平昨平今的品种, 则读取全部可平, 因为读取可拼今仓也没意义
			if (commInfo->getCoverMode() == CM_CoverToday)
				maxQty = min(left, pItem.s_newavail);	//先看看可平今仓
			else
				maxQty = min(left, pItem.avail_pos(false));
			
			
			//如果要检查净今仓，但是昨仓不为0，则跳过该条规则
			if (!bForceClose && curRule._pure && !decimal::eq(pItem.s_prevol, 0.0))
			{
				WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN,
					"[{}] Closing new short position of {} skipped because of non-zero pre short position", _id.c_str(), stdCode);
				continue;
			}

			//这里还要考虑单笔最大委托数量
			//if (maxQty > 0)
			if (decimal::gt(maxQty, 0))
			{
				double leftQty = maxQty;
				for (;;)
				{
					double curQty = min(leftQty, unitQty);
					if (!submitSplit(ret, stdCode, true, curQty, [&]() { return closeShort(stdCode, price, curQty, (commInfo->getCoverMode() == CM_CoverToday), flag, cInfo); }))//如果不支持平今, 则直接下平仓标记即可
					{
						stopped = true;
						break;
					}

					leftQty -= curQty;

					//if (leftQty == 0)
					if (decimal::eq(leftQty, 0))
						break;
				}
				left -= maxQty;

				if (commInfo->getCoverMode() == CM_CoverToday)
				{
					WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO, 
						"[{}] Signal of buying {} of quantity {} triggered: Closing new short of quantity {}", _id.c_str(), stdCode, qty, maxQty);
				}
				else
				{
					WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO, 
						"[{}] Signal of buying {} of quantity {} triggered: Closing short of quantity {}", _id.c_str(), stdCode, qty, maxQty);
				}
			}
			
		}
		else if (curRule._atype == AT_CloseYestoday)
		{
			//平昨比较简单, 因为不需要区分标记
			double maxQty = min(left, pItem.s_preavail);

			//如果要检查净昨仓，但是今仓不为0，则跳过该条规则
			if (!bForceClose && curRule._pure && !decimal::eq(pItem.s_newvol, 0.0))
			{
				WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN,
					"[{}] Closing pre short position of {} skipped because of non-zero new short position", _id.c_str(), stdCode);
				continue;
			}

			//这里还要考虑单笔最大委托数量
			//if (maxQty > 0)
			if (decimal::gt(maxQty, 0))
			{
				double leftQty = maxQty;
				for (;;)
				{
					double curQty = min(leftQty, unitQty);
					if (!submitSplit(ret, stdCode, true, curQty, [&]() { return closeShort(stdCode, price, curQty, false, flag, cInfo); }))//如果不支持平今, 则直接下平仓标记即可
					{
						stopped = true;
						break;
					}

					leftQty -= curQty;

					//if (leftQty == 0)
					if (decimal::eq(leftQty, 0))
						break;
				}

				left -= maxQty;

				if (commInfo->getCoverMode() == CM_CoverToday)
				{
					WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO, 
						"[{}] Signal of buying {} of quantity {} triggered: Closing old short of quantity {}", _id.c_str(), stdCode, qty, maxQty);
				}
				else
				{
					WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO,
						"[{}] Signal of buying {} of quantity {} triggered: Closing short of quantity {}", _id.c_str(), stdCode, qty, maxQty);
				}
			}
		}
		else if (curRule._atype == AT_Close)
		{
			//如果只是平仓, 则分情况处理
			//如果区分平昨平今, 则要先平昨再平今
			//如果不区分平昨平今, 则统一平仓
			if (commInfo->getCoverMode() != CM_CoverToday)
			{
				double maxQty = min(pItem.avail_pos(false), left);
				
				//if (maxQty > 0)
				if (decimal::gt(maxQty, 0))
				{
					double leftQty = maxQty;
					for (;;)
					{
						double curQty = min(leftQty, unitQty);
						if (!submitSplit(ret, stdCode, true, curQty, [&]() { return closeShort(stdCode, price, curQty, false, flag, cInfo); }))
						{
							stopped = true;
							break;
						}

						leftQty -= curQty;

						//if (leftQty == 0)
						if (decimal::eq(leftQty, 0))
							break;
					}
					left -= maxQty;

					WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO,
						"[{}] Signal of buying {} of quantity {} triggered: Closing short of quantity {}", _id.c_str(), stdCode, qty, maxQty);
				}
			}
			else
			{
				//if (pItem.s_preavail > 0)
				if (decimal::gt(pItem.s_preavail, 0))
				{
					//先将可平昨仓平仓
					double maxQty = min(pItem.s_preavail, qty);
					double leftQty = maxQty;
					for (;;)
					{
						double curQty = min(leftQty, unitQty);
						if (!submitSplit(ret, stdCode, true, curQty, [&]() { return closeShort(stdCode, price, curQty, false, flag, cInfo); }))
						{
							stopped = true;
							break;
						}

						leftQty -= curQty;

						//if (leftQty == 0)
						if (decimal::eq(leftQty, 0))
							break;
					}
					left -= maxQty;

					WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO,
						"[{}] Signal of buying {} of quantity {} triggered: Closing old short of quantity {}", _id.c_str(), stdCode, qty, maxQty);
				}

				//if (left > 0 && pItem.s_newavail > 0)
				if (decimal::gt(left, 0) && decimal::gt(pItem.s_newavail, 0))
				{
					//再将可平今仓平仓
					//TODO: 这里还有一个控制, 就是强制锁今仓的话, 这段逻辑就跳过去了
					double maxQty = min(pItem.s_newavail, left);
					double leftQty = maxQty;
					for (;;)
					{
						double curQty = min(leftQty, unitQty);
						if (!submitSplit(ret, stdCode, true, curQty, [&]() { return closeShort(stdCode, price, curQty, true, flag, cInfo); }))
						{
							stopped = true;
							break;
						}

						leftQty -= curQty;

						//if (leftQty == 0)
						if (decimal::eq(leftQty, 0))
							break;
					}
					left -= maxQty;

					WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO,
						"[{}] Signal of buying {} of quantity {} triggered: Closing new short of quantity {}", _id.c_str(), stdCode, qty, maxQty);
				}
			}
		}

		if (stopped)
			break;

		if(left == 0)
			break;
	}

	if (stopped)
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR,
			"[{}] Signal of buying {} of quantity {} stopped after a rejected split, {} splits accepted", _id.c_str(), stdCode, qty, ret.size());
	}
	else if(left > 0)
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR, 
			"[{}] Signal of buying {} of quantity {} left quantity of {} not triggered", _id.c_str(), stdCode, qty, left);
	}

	return ret;
}

OrderIDs TraderAdapter::sell(const char* stdCode, double price, double qty, int flag, bool bForceClose, WTSContractInfo* cInfo /* = NULL */)
{
	OrderIDs ret;
	if (isTradingAdmissionClosed())
		return ret;
	if (qty == 0)
		return ret;

	if(isSelfMatched(stdCode))
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR,
			"[{0}] Instructions on {1} are forbidden: {1} is in self matches", _id.c_str(), stdCode);
		return ret;
	}

	if (cInfo == NULL) cInfo = getContract(stdCode);
	// L01-014: an unknown or unparseable standard code never reaches the
	// ledger or the provider (fail closed instead of dereferencing NULL).
	if (cInfo == NULL || cInfo->getCommInfo() == NULL || cInfo->getCommInfo()->getSessionInfo() == NULL)
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR, "[{}] {} rejected: contract, commodity or session of {} unavailable",
			_id.c_str(), "sell", stdCode ? stdCode : "");
		return ret;
	}
	WTSCommodityInfo* commInfo = cInfo->getCommInfo();
	WTSSessionInfo* sInfo = commInfo->getSessionInfo();

	if(!sInfo->isInTradingTime(WtHelper::getTime(), true))
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR, 
			"[{}] Selling {} of quantity {}, {:04d} not in trading time", _id.c_str(), stdCode, qty, WtHelper::getTime());
		return ret;
	}

	WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO, "[{}] Selling {} of quantity {}", _id.c_str(), stdCode, qty);

	bool stopped = false;

	// Snapshot: re-entrant reports may modify _positions while splits are submitted.
	const PosItem pItem = _positions[stdCode];	
	WTSTradeStateInfo* statInfo = (WTSTradeStateInfo*)_stat_map->get(stdCode);
	if (statInfo == NULL)
	{
		statInfo = WTSTradeStateInfo::create(stdCode);
		_stat_map->add(stdCode, statInfo, false);
	}
	TradeStatInfo& statItem = statInfo->statInfo();

	const ActionRuleGroup& ruleGP = _policy_mgr->getActionRules(commInfo->getFullPid());

	double left = qty;

	double unitQty = (price == 0.0) ? cInfo->getMaxMktVol() : cInfo->getMaxLmtVol();
	if (decimal::eq(unitQty, 0))
		unitQty = DBL_MAX;

	for (auto it = ruleGP.begin(); it != ruleGP.end(); it++)
	{
		const ActionRule& curRule = (*it);
		if (curRule._atype == AT_Open && !bForceClose)
		{
			//先检查是否已经到了限额
			//买入开仓, 即开多仓
			double maxQty = left;

			if (curRule._limit_s != 0)
			{
				if (statItem.s_openvol >= curRule._limit_s)
				{
					WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN, "[{}] {} short position opened today is up to limit {}", _id.c_str(), stdCode, curRule._limit_l);
					continue;
				}
				else
				{
					maxQty = min(maxQty, curRule._limit_s - statItem.s_openvol);
				}
			}

			if (curRule._limit != 0)
			{
				if (statItem.l_openvol + statItem.s_openvol >= curRule._limit)
				{
					WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN, "[{}] {} position opened today is up to limit {}", _id.c_str(), stdCode, curRule._limit);
					continue;
				}
				else
				{
					maxQty = min(maxQty, curRule._limit - statItem.l_openvol - statItem.s_openvol);
				}
			}

			//这里还要考虑单笔最大委托数量
			double leftQty = maxQty;
			for (;;)
			{
				double curQty = min(leftQty, unitQty);
				if (!submitSplit(ret, stdCode, false, curQty, [&]() { return openShort(stdCode, price, curQty, flag, cInfo); }))
				{
					stopped = true;
					break;
				}

				leftQty -= curQty;

				if (decimal::eq(leftQty, 0))
					break;
			}

			left -= maxQty;

			WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO,
				"[{}] Signal of selling {} of quantity {} triggered: Opening short of quantity {}", _id.c_str(), stdCode, qty, maxQty);
		}
		else if (curRule._atype == AT_CloseToday)
		{
			double maxQty = 0;
			//如果要区分平昨平今的品种, 则只读取可平今仓即可
			//如果不区分平昨平今的品种, 则读取全部可平, 因为读取可平今仓也没意义
			if (commInfo->getCoverMode() == CM_CoverToday)
				maxQty = min(left, pItem.l_newavail);	//先看看可平今仓
			else
				maxQty = min(left, pItem.avail_pos(true));

			//如果要检查净今仓，但是昨仓不为0，则跳过该条规则
			if (!bForceClose && curRule._pure && !decimal::eq(pItem.l_prevol, 0.0))
			{
				WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN,
					"[{}] Closing new long position of {} skipped because of non-zero pre long position", _id.c_str(), stdCode);
				continue;
			}

			//这里还要考虑单笔最大委托数量
			if(decimal::gt(maxQty, 0))
			{
				double leftQty = maxQty;
				for (;;)
				{
					double curQty = min(leftQty, unitQty);
					if (!submitSplit(ret, stdCode, false, curQty, [&]() { return closeLong(stdCode, price, curQty, (commInfo->getCoverMode() == CM_CoverToday), flag, cInfo); }))//如果不支持平今, 则直接下平仓标记即可
					{
						stopped = true;
						break;
					}

					leftQty -= curQty;

					if (decimal::eq(leftQty, 0))
						break;
				}
				left -= maxQty;

				if (commInfo->getCoverMode() == CM_CoverToday)
				{
					WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO,
						"[{}] Signal of selling {} of quantity {} triggered: Closing new long of quantity {}", _id.c_str(), stdCode, qty, maxQty);
				}
				else
				{
					WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO,
						"[{}] Signal of selling {} of quantity {} triggered: Closing long of quantity {}", _id.c_str(), stdCode, qty, maxQty);
				}
			}

		}
		else if (curRule._atype == AT_CloseYestoday)
		{
			//平昨比较简单, 因为不需要区分标记
			double maxQty = min(left, pItem.l_preavail);
			
			//如果要检查净昨仓，但是今仓不为0，则跳过该条规则
			if (!bForceClose && curRule._pure && !decimal::eq(pItem.l_newvol, 0.0))
			{
				WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN,
					"[{}] Closing pre long position of {} skipped because of non-zero new long position", _id.c_str(), stdCode);
				continue;
			}

			//这里还要考虑单笔最大委托数量
			if(decimal::gt(maxQty, 0))
			{
				double leftQty = maxQty;
				for (;;)
				{
					double curQty = min(leftQty, unitQty);
					if (!submitSplit(ret, stdCode, false, curQty, [&]() { return closeLong(stdCode, price, curQty, false, flag, cInfo); }))//如果不支持平今, 则直接下平仓标记即可
					{
						stopped = true;
						break;
					}

					leftQty -= curQty;

					if (decimal::eq(leftQty, 0))
						break;
				}
				left -= maxQty;

				if (commInfo->getCoverMode() == CM_CoverToday)
				{
					WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO,
						"[{}] Signal of selling {} of quantity {} triggered: Closing old long of quantity {}", _id.c_str(), stdCode, qty, maxQty);
				}
				else
				{
					WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO,
						"[{}] Signal of selling {} of quantity {} triggered: Closing long of quantity {}", _id.c_str(), stdCode, qty, maxQty);
				}
			}
		}
		else if (curRule._atype == AT_Close)
		{
			//如果只是平仓, 则分情况处理
			//如果区分平昨平今, 则要先平昨再平今
			//如果不区分平昨平今, 则统一平仓
			if (commInfo->getCoverMode() != CM_CoverToday)
			{
				double maxQty = min(pItem.avail_pos(true), left);	//不区分平昨平今, 则读取全部可平量
				if(decimal::gt(maxQty, 0))
				{
					double leftQty = maxQty;
					for (;;)
					{
						double curQty = min(leftQty, unitQty);
						if (!submitSplit(ret, stdCode, false, curQty, [&]() { return closeLong(stdCode, price, curQty, false, flag, cInfo); }))
						{
							stopped = true;
							break;
						}

						leftQty -= curQty;

						if (decimal::eq(leftQty, 0))
							break;
					}
					left -= maxQty;

					WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO,
						"[{}] Signal of selling {} of quantity {} triggered: Closing long of quantity {}", _id.c_str(), stdCode, qty, maxQty);
				}
				
			}
			else
			{
				if (decimal::gt(left, 0) && decimal::gt(pItem.l_preavail, 0))
				{
					//先将可平昨仓平仓
					double maxQty = min(pItem.l_preavail, qty);
					if(decimal::gt(maxQty, 0))
					{
						double leftQty = maxQty;
						for (;;)
						{
							double curQty = min(leftQty, unitQty);
							if (!submitSplit(ret, stdCode, false, curQty, [&]() { return closeLong(stdCode, price, curQty, false, flag, cInfo); }))
							{
								stopped = true;
								break;
							}

							leftQty -= curQty;

							if (decimal::eq(leftQty, 0))
								break;
						}
						left -= maxQty;

						WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO,
							"[{}] Signal of selling {} of quantity {} triggered: Closing old long of quantity {}", _id.c_str(), stdCode, qty, maxQty);
					}
					
				}

				if (decimal::gt(left, 0) && decimal::gt(pItem.l_newavail, 0))
				{
					//再将可平今仓平仓
					//TODO: 这里还有一个控制, 就是强制锁今仓的话, 这段逻辑就跳过去了
					double maxQty = min(pItem.l_newavail, left);
					if(decimal::gt(maxQty, 0))
					{
						double leftQty = maxQty;
						for (;;)
						{
							double curQty = min(leftQty, unitQty);
							if (!submitSplit(ret, stdCode, false, curQty, [&]() { return closeLong(stdCode, price, curQty, true, flag, cInfo); }))
							{
								stopped = true;
								break;
							}

							leftQty -= curQty;

							if (decimal::eq(leftQty, 0))
								break;
						}
						left -= maxQty;

						WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO,
							"[{}] Signal of selling {} of quantity {} triggered: Closing new long of quantity {}", _id.c_str(), stdCode, qty, maxQty);
					}
					
				}
			}
		}

		if (stopped)
			break;

		if (left == 0)
			break;
	}

	if (stopped)
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR,
			"[{}] Signal of selling {} of quantity {} stopped after a rejected split, {} splits accepted", _id.c_str(), stdCode, qty, ret.size());
	}
	else if (left > 0)
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR, 
			"[{}] Signal of buying {} of quantity {} left quantity of {} not triggered", _id.c_str(), stdCode, qty, left);
	}

	return ret;
}

bool TraderAdapter::doCancel(WTSOrderInfo* ordInfo)
{
	if (ordInfo == NULL || !ordInfo->isAlive())
		return false;

	WTSContractInfo* cInfo = ordInfo->getContractInfo();
	if (cInfo == NULL || cInfo->getCommInfo() == NULL)
		return false;
	WTSCommodityInfo* commInfo = cInfo->getCommInfo();
	std::string stdCode;
	if (commInfo->getCategoty() == CC_FutOption || commInfo->getCategoty() == CC_SpotOption)
		stdCode = CodeHelper::rawFutOptCodeToStdCode(cInfo->getCode(), cInfo->getExchg());
	else if (CodeHelper::isMonthlyCode(cInfo->getCode()))//如果是分月合约
		stdCode = CodeHelper::rawMonthCodeToStdCode(cInfo->getCode(), cInfo->getExchg());
	else
		stdCode = CodeHelper::rawFlatCodeToStdCode(cInfo->getCode(), cInfo->getExchg(), cInfo->getProduct());
	if (stdCode.empty())
		return false;	// unparseable code: fail closed, nothing sent
	//撤单频率检查
	if (!checkCancelLimits(stdCode.c_str()))
		return false;
	const RiskParams* riskPara = _risk_mon_enabled ? getRiskParams(stdCode.c_str()) : NULL;
	const uint32_t boundary = riskPara ? riskPara->_cancel_times_boundary : 0;
	const uint32_t seconds = riskPara ? riskPara->_cancel_stat_timespan : 0;
	// Cancels use the same window boundary contract as orders (card 7d).
	FrequencyWindow<CodeTimeCacheMap>::Reservation reservation(_cancel_frequency, stdCode,
		boundary, seconds, rateNow());
	if (!reservation)
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR,
			"[{}] {} cancel rejected by frequency limit: {} requests within {} seconds, temporarily banned until the window expires",
			_id.c_str(), stdCode.c_str(), boundary, seconds);
		return false;
	}

	WTSEntrustAction* action = WTSEntrustAction::create(ordInfo->getCode(), cInfo->getExchg());
	action->setEntrustID(ordInfo->getEntrustID());
	action->setOrderID(ordInfo->getOrderID());
	reservation.beginSubmission(rateNow());
	int ret = -1;
	try
	{
		ret = _trader_api->orderAction(action);
	}
	catch (...)
	{
		action->release();
		throw;	// unknown outcome: the reservation stays committed
	}
	action->release();
	bool isSent = (ret >= 0);
	if (isSent)
		reservation.commit();
	else
		reservation.rollback();	// synchronous failure: the slot is returned
	return isSent;
}

bool TraderAdapter::cancelAdmissibleLocked(uint32_t localid, const std::string* owner, bool paused) const
{
	auto it = _ledger.find(localid);
	if (it == _ledger.end())
		return owner == nullptr && !paused;	// unknown identity: legacy bare cancel only while trading is open
	const OrderLedgerEntry& entry = it->second;
	if (entry.state == OrderLedgerState::Terminal || entry.state == OrderLedgerState::Absent || entry.cancel_claimed)
		return false;
	if (owner != nullptr)
	{
		if (entry.owner_id != *owner)
			return false;	// owner routing: never cancel another owner's order
	}
	else if (!entry.owner_id.empty())
		return false;		// owned orders are cancelled through their owner only
	// Card 6a + user decision 2026-11-04: while the account is paused (or the
	// startup identity recovery is pending) only verified live identities pass.
	if (paused)
		return entry.verified && entry.state == OrderLedgerState::Live;
	return true;
}

bool TraderAdapter::cancelClaimed(uint32_t localid, const std::string* owner)
{
	const bool paused = isTradingAdmissionClosed();
	std::lock_guard<std::mutex> lock(_ledger_mutex);
	if (!cancelAdmissibleLocked(localid, owner, paused))
		return false;
	auto it = _ledger.find(localid);
	if (it != _ledger.end())
		it->second.cancel_claimed = true;
	return true;
}

void TraderAdapter::cancelUnclaim(uint32_t localid)
{
	std::lock_guard<std::mutex> lock(_ledger_mutex);
	auto it = _ledger.find(localid);
	if (it != _ledger.end())
		it->second.cancel_claimed = false;
}

bool TraderAdapter::cancelOne(uint32_t localid, const std::string* owner)
{
	if (_orders == NULL || _orders->size() == 0)
		return false;
	if (!cancelClaimed(localid, owner))
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN,
			"[{}] cancel of order {} rejected: identity/owner not verifiable{}", _id.c_str(), localid,
			isTradingAdmissionClosed() ? " while account is paused" : "");
		return false;
	}
	struct Unclaim { TraderAdapter* self; uint32_t localid; ~Unclaim() { self->cancelUnclaim(localid); } } unclaim{ this, localid };

	WTSOrderInfo* ordInfo = NULL;
	{
		SpinLock lock(_mtx_orders);
		ordInfo = (WTSOrderInfo*)_orders->grab(localid);
		if (ordInfo == NULL)
			return false;
	}
	if (!ordInfo->isAlive())
	{
		ordInfo->release();
		return false;
	}

	bool bRet = doCancel(ordInfo);

	ordInfo->release();

	return bRet;
}

bool TraderAdapter::cancel(uint32_t localid)
{
	return cancelOne(localid, nullptr);
}

bool TraderAdapter::cancelForOwner(uint32_t localid, const std::string& owner)
{
	if (owner.empty())
		return false;
	return cancelOne(localid, &owner);
}

OrderIDs TraderAdapter::cancel(const char* stdCode, bool isBuy, double qty /* = 0 */)
{
	return cancelByCode(stdCode, isBuy, qty, nullptr);
}

OrderIDs TraderAdapter::cancelForOwner(const char* stdCode, bool isBuy, double qty, const std::string& owner)
{
	if (owner.empty())
		return OrderIDs();
	return cancelByCode(stdCode, isBuy, qty, &owner);
}

OrderIDs TraderAdapter::cancelByCode(const char* stdCode, bool isBuy, double qty, const std::string* owner)
{
	OrderIDs ret;
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, NULL);

	// Snapshot candidates under the order lock: doCancel may re-enter
	// onPushOrder synchronously, which mutates _orders.
	std::vector<std::pair<uint32_t, WTSOrderInfo*>> candidates;
	bool isAll = strlen(stdCode) == 0;
	{
		SpinLock lock(_mtx_orders);
		if (_orders != NULL)
		{
			for (auto it = _orders->begin(); it != _orders->end(); it++)
			{
				WTSOrderInfo* orderInfo = (WTSOrderInfo*)it->second;
				if (!orderInfo->isAlive())
					continue;

				bool bBuy = (orderInfo->getDirection() == WDT_LONG && orderInfo->getOffsetType() == WOT_OPEN) || (orderInfo->getDirection() == WDT_SHORT && orderInfo->getOffsetType() != WOT_OPEN);
				if (bBuy != isBuy)
					continue;

				if (isAll || strcmp(orderInfo->getCode(), cInfo._code) == 0)
				{
					orderInfo->retain();
					candidates.emplace_back(it->first, orderInfo);
				}
			}
		}
	}

	struct ReleaseAll
	{
		std::vector<std::pair<uint32_t, WTSOrderInfo*>>& items;
		~ReleaseAll() { for (auto& item : items) item.second->release(); }
	} releaser{ candidates };

	double actQty = 0;
	for (auto& item : candidates)
	{
		if (!decimal::eq(qty, 0) && decimal::ge(actQty, qty))
			break;
		WTSOrderInfo* orderInfo = item.second;
		const double leftQty = orderInfo->getVolLeft();
		// Each candidate is verified and claimed individually (owner, pause).
		if (!cancelClaimed(item.first, owner))
			continue;
		bool sent = false;
		{
			// The claim is released even if the provider call throws.
			struct Unclaim { TraderAdapter* self; uint32_t localid; ~Unclaim() { self->cancelUnclaim(localid); } } unclaim{ this, item.first };
			sent = doCancel(orderInfo);
		}
		if (sent)
		{
			actQty += leftQty;
			ret.emplace_back(item.first);
		}
	}

	return ret;
}

uint32_t TraderAdapter::openLong(const char* stdCode, double price, double qty, int flag, WTSContractInfo* cInfo /* = NULL */)
{
	if (isTradingAdmissionClosed())
		return UINT_MAX;
	WTSEntrust* entrust = WTSEntrust::create(stdCode, qty, price);
	entrust->setContractInfo(cInfo == NULL ? getContract(stdCode) : cInfo);
	if(decimal::eq(price, 0.0))
		entrust->setPriceType(WPT_ANYPRICE);
	else
		entrust->setPriceType(WPT_LIMITPRICE);
	entrust->setOrderFlag((WTSOrderFlag)(WOF_NOR + flag));

	entrust->setDirection(WDT_LONG);
	entrust->setOffsetType(WOT_OPEN);

	uint32_t ret = doEntrust(entrust);
	entrust->release();
	return ret;
}

uint32_t TraderAdapter::openShort(const char* stdCode, double price, double qty, int flag, WTSContractInfo* cInfo /* = NULL */)
{
	if (isTradingAdmissionClosed())
		return UINT_MAX;
	WTSEntrust* entrust = WTSEntrust::create(stdCode, qty, price);
	entrust->setContractInfo(cInfo == NULL ? getContract(stdCode) : cInfo);
	if (decimal::eq(price, 0.0))
		entrust->setPriceType(WPT_ANYPRICE);
	else
		entrust->setPriceType(WPT_LIMITPRICE);
	entrust->setOrderFlag((WTSOrderFlag)(WOF_NOR + flag));

	entrust->setDirection(WDT_SHORT);
	entrust->setOffsetType(WOT_OPEN);

	uint32_t ret = doEntrust(entrust);
	entrust->release();
	return ret;
}

uint32_t TraderAdapter::closeLong(const char* stdCode, double price, double qty, bool isToday, int flag, WTSContractInfo* cInfo /* = NULL */)
{
	if (isTradingAdmissionClosed())
		return UINT_MAX;
	WTSEntrust* entrust = WTSEntrust::create(stdCode, qty, price);
	entrust->setContractInfo(cInfo == NULL ? getContract(stdCode) : cInfo);
	if (decimal::eq(price, 0.0))
		entrust->setPriceType(WPT_ANYPRICE);
	else
		entrust->setPriceType(WPT_LIMITPRICE);
	entrust->setOrderFlag((WTSOrderFlag)(WOF_NOR + flag));

	entrust->setDirection(WDT_LONG);
	entrust->setOffsetType(isToday ? WOT_CLOSETODAY : WOT_CLOSE);

	uint32_t ret = doEntrust(entrust);
	entrust->release();
	return ret;
}

uint32_t TraderAdapter::closeShort(const char* stdCode, double price, double qty, bool isToday, int flag, WTSContractInfo* cInfo /* = NULL */)
{
	if (isTradingAdmissionClosed())
		return UINT_MAX;
	WTSEntrust* entrust = WTSEntrust::create(stdCode, qty, price);
	entrust->setContractInfo(cInfo == NULL ? getContract(stdCode) : cInfo);
	if (decimal::eq(price, 0.0))
		entrust->setPriceType(WPT_ANYPRICE);
	else
		entrust->setPriceType(WPT_LIMITPRICE);
	entrust->setOrderFlag((WTSOrderFlag)(WOF_NOR + flag));

	entrust->setDirection(WDT_SHORT);
	entrust->setOffsetType(isToday ? WOT_CLOSETODAY : WOT_CLOSE);

	uint32_t ret = doEntrust(entrust);
	entrust->release();
	return ret;
}


#pragma region "ITraderSpi接口"
void TraderAdapter::handleEvent(WTSTraderEvent e, int32_t ec)
{
	SpiIngress spiIngress(this);
	if (!spiIngress)
		return;
	if(e == WTE_Connect)
	{
		if(ec == 0)
		{
			if (_cfg == NULL)
			{
				// Decision card 10 = A (BUG-I02-NEW-016): an API supplied through
				// initExt() authenticates and logs in by itself. This adapter holds
				// no credentials for it, so WTE_Connect only reports the channel state;
				// the login outcome arrives through onLoginResult() from that API.
				// The same branch keeps a late connect event after release() (which
				// clears _cfg) from dereferencing a missing configuration.
				if (_trader_api != NULL && !_owns_trader_api && !_api_released)
					WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO, "[{}] Trading channel connected, external trader API performs its own login", _id.c_str());
				else
					WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN, "[{}] Connect event ignored: no configured credentials and no active external trader API", _id.c_str());
			}
			else
			{
				// Config init() path: the adapter owns the plugin API and logs in
				// with the configured account.
				_trader_api->login(_cfg->getCString("user"), _cfg->getCString("pass"), WT_PRODUCT);
			}
		}
		else
		{
			WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR,"[{}] Trading channel connecting failed: {}", _id.c_str(), ec);
		}
	}
	else if(e == WTE_Close)
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR,"[{}] Trading channel disconnected: {}", _id.c_str(), ec);
		bool shouldDrain = false;
		{
			std::lock_guard<std::mutex> lock(_sink_mutex);
			_state = AS_NOTLOGIN;
			std::vector<SinkRegistrationPtr> recipients;
			recipients.reserve(_sink_registrations.size());
			for (auto it = _sink_registrations.begin(); it != _sink_registrations.end(); ++it)
				recipients.push_back(it->second);
			shouldDrain = queueChannelNotificationLocked(false, recipients);
		}
		if (shouldDrain)
			drainChannelNotifications();
	}
}

void TraderAdapter::onLoginResult(bool bSucc, const char* msg, uint32_t tradingdate)
{
	SpiIngress spiIngress(this);
	if (!spiIngress)
		return;
	if(!bSucc)
	{
		setState(AS_LOGINFAILED);
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR,"[{}] Trader login failed: {}", _id.c_str(), msg);

		if (_notifier)
			_notifier->notify(id(), fmt::format("login failed: {}", msg).c_str());
	}
	else
	{
		setState(AS_LOGINED);
		WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO,"[{}] Trader login succeed, trading date: {}", _id.c_str(), tradingdate);
		_trading_day = tradingdate;
		{
			std::lock_guard<std::mutex> lock(_ledger_mutex);
			_positions_query_version = ++_event_version;	// NEW-154: position snapshot epoch
			if (_trade_dedupe_day != tradingdate)
			{
				// Trade ids are only unique within a trading day.
				_pushed_trades.clear();
				_counted_trades.clear();
				_trade_dedupe_day = tradingdate;
			}
		}
		_trader_api->queryPositions();	//查持仓
	}
}

void TraderAdapter::onLogout()
{
	SpiIngress spiIngress(this);
	if (!spiIngress)
		return;
	
}

void TraderAdapter::onRspEntrust(WTSEntrust* entrust, WTSError *err)
{
	SpiIngress spiIngress(this);
	if (!spiIngress)
		return;
	if (err && err->getErrorCode() != WEC_NONE)
	{
		// An external order's tag must not consume this adapter's pending
		// quantity or be interpreted as one of its local order identifiers.
		auto rejectExternal = [this]() {
			WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN,
				"[{}] Rejected entrust has no valid local order tag; pending quantity unchanged", _id.c_str());
		};
		if (entrust == NULL || _order_pattern.empty())
		{
			rejectExternal();
			return;
		}
		const char* tag = entrust->getUserTag();
		const void* terminator = std::memchr(tag, '\0', sizeof(entrust->m_strUserTag));
		if (terminator == NULL)
		{
			rejectExternal();
			return;
		}
		const std::string value(tag, static_cast<const char*>(terminator));
		const std::string prefix = _order_pattern + ".";
		if (value.size() <= prefix.size() || value.compare(0, prefix.size(), prefix) != 0)
		{
			rejectExternal();
			return;
		}
		uint32_t localid = 0;
		for (size_t i = prefix.size(); i < value.size(); ++i)
		{
			const char digit = value[i];
			if (digit < '0' || digit > '9' ||
				localid > ((std::numeric_limits<uint32_t>::max)() - static_cast<uint32_t>(digit - '0')) / 10)
			{
				rejectExternal();
				return;
			}
			localid = localid * 10 + static_cast<uint32_t>(digit - '0');
		}
		if (localid == 0)
		{
			rejectExternal();
			return;
		}
		noteInflightReport(localid, INFLIGHT_REJECTED);
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR, err->getMessage());
		// The per-order ledger releases this order's undone quantity exactly
		// once; duplicate or unknown rejections change nothing (R3-008).
		bool firstTerminal = false;
		std::string ledgerCode;
		std::string intentId;
		std::string entrustOwner;
		{
			std::lock_guard<std::mutex> lock(_ledger_mutex);
			auto it = _ledger.find(localid);
			if (it != _ledger.end())
			{
				it->second.last_version = ++_event_version;
				// I03-NEW-057: read the entry before tombstoning; pruning may
				// move dense-map entries and invalidate `it`.
				ledgerCode = it->second.std_code;
				intentId = it->second.intent_id;
				entrustOwner = it->second.owner_id;
				if (it->second.state != OrderLedgerState::Terminal)
				{
					ledgerTombstoneLocked(localid, it->second, OrderLedgerState::Terminal);
					firstTerminal = true;
				}
			}
		}
		if (!firstTerminal)
		{
			WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN,
				"[{}] Rejection of order {} is duplicate or unknown; pending quantity unchanged", _id.c_str(), localid);
			return;
		}
		journalTerminal(localid, intentId, IDENTITY_STATUS_REJECTED, 0);
		WTSContractInfo* cInfo = entrust->getContractInfo();
		if (cInfo == NULL || cInfo->getCommInfo() == NULL)
		{
			dispatchOrderEvent(entrustOwner, [&](ITrdNotifySink* sink) {
				sink->on_entrust(localid, ledgerCode.c_str(), false, err->getMessage());
			});
			return;
		}
		WTSCommodityInfo* commInfo = cInfo->getCommInfo();
		std::string stdCode;
		if (commInfo->getCategoty() == CC_FutOption || commInfo->getCategoty() == CC_SpotOption)
			stdCode = CodeHelper::rawFutOptCodeToStdCode(cInfo->getCode(), cInfo->getExchg());
		else if (CodeHelper::isMonthlyCode(cInfo->getCode()))//如果是分月合约
			stdCode = CodeHelper::rawMonthCodeToStdCode(cInfo->getCode(), cInfo->getExchg());
		else
			stdCode = CodeHelper::rawFlatCodeToStdCode(cInfo->getCode(), cInfo->getExchg(), cInfo->getProduct());

		bool isLong = (entrust->getDirection() == WDT_LONG);
		bool isToday = (entrust->getOffsetType() == WOT_CLOSETODAY);
		bool isOpen = (entrust->getOffsetType() == WOT_OPEN);
		double qty = entrust->getVolume();

		std::string action;
		if (isOpen)
			action = "open ";
		else if (isToday)
			action = "closetoday ";
		else
			action = "close ";
		action += isLong ? "long" : "short";

		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR, 
			"[{}] Order placing failed: {}, instrument: {}, action: {}, qty: {}", _id.c_str(), err->getMessage(), entrust->getCode(), action.c_str(), qty);

		if (stdCode.empty())
			stdCode = ledgerCode;

		dispatchOrderEvent(entrustOwner, [&](ITrdNotifySink* sink) {
			sink->on_entrust(localid, stdCode.c_str(), false, err->getMessage());
		});

		if (_notifier)
			_notifier->notify(id(), fmt::format(" Order placing failed: {}", err->getMessage()).c_str());
	}
}

void TraderAdapter::onRspAccount(WTSArray* ayAccounts)
{
	SpiIngress spiIngress(this);
	if (!spiIngress)
		return;
	if (_save_data)
	{
		saveData(ayAccounts);
	}

	if(ayAccounts)
	{
		//通知所有监听接口
		forEachSink([&](ITrdNotifySink* sink) {
			for (uint32_t idx = 0; idx < ayAccounts->size(); idx++)
			{
				WTSAccountInfo* fundInfo = (WTSAccountInfo*)ayAccounts->at(idx);
				sink->on_account(fundInfo->getCurrency(), fundInfo->getPreBalance(), fundInfo->getBalance(), fundInfo->getBalance() + fundInfo->getDynProfit(), fundInfo->getAvailable(),
					fundInfo->getCloseProfit(), fundInfo->getDynProfit(), fundInfo->getMargin(), fundInfo->getCommission(), fundInfo->getDeposit(), fundInfo->getWithdraw());
			}
		});
	}

	bool becameReady = false;
	bool shouldDrain = false;
	{
		std::lock_guard<std::mutex> lock(_sink_mutex);
		if (_state == AS_TRADES_QRYED)
		{
			_state = AS_ALLREADY;
			becameReady = true;
			std::vector<SinkRegistrationPtr> recipients;
			recipients.reserve(_sink_registrations.size());
			for (auto it = _sink_registrations.begin(); it != _sink_registrations.end(); ++it)
				recipients.push_back(it->second);
			shouldDrain = queueChannelNotificationLocked(true, recipients);
		}
	}

	if (becameReady)
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO, "[{}] Trading channel ready", _id.c_str());
	}
	if (shouldDrain)
	{
		drainChannelNotifications();
	}
}

void TraderAdapter::onRspPosition(const WTSArray* ayPositions)
{
	SpiIngress spiIngress(this);
	if (!spiIngress)
		return;
	if (ayPositions && ayPositions->size() > 0)
	{
		for (auto it = ayPositions->begin(); it != ayPositions->end(); it++)
		{
			WTSPositionItem* pItem = (WTSPositionItem*)(*it);
			if (pItem == NULL)
				continue;
			WTSContractInfo* cInfo = pItem->getContractInfo();
			WTSCommodityInfo* commInfo = cInfo ? cInfo->getCommInfo() : NULL;
			std::string stdCode;
			if (commInfo != NULL)
			{
				if (commInfo->getCategoty() == CC_FutOption || commInfo->getCategoty() == CC_SpotOption)
					stdCode = CodeHelper::rawFutOptCodeToStdCode(cInfo->getCode(), cInfo->getExchg());
				else if (CodeHelper::isMonthlyCode(cInfo->getCode()))//如果是分月合约
					stdCode = CodeHelper::rawMonthCodeToStdCode(cInfo->getCode(), cInfo->getExchg());
				else
					stdCode = CodeHelper::rawFlatCodeToStdCode(cInfo->getCode(), cInfo->getExchg(), cInfo->getProduct());
			}
			if (stdCode.empty())
			{
				// L01-014: never write an empty ledger key; retain and pause the account.
				const char* reason = cInfo == NULL ? "contract unavailable" :
					(commInfo == NULL ? "commodity unavailable" : "standard code invalid");
				pauseAccount("unresolved_report", std::string("unresolved position report: ") + reason);
				try
				{
					rj::StringBuffer buffer;
					rj::Writer<rj::StringBuffer> writer(buffer);
					auto real = [&writer](const char* key, double value) {
						std::ostringstream out; out.imbue(std::locale::classic());
						out << std::hexfloat << value;
						writer.Key(key); writer.String(out.str().c_str());
					};
					writer.StartObject();
					writer.Key("schema_version"); writer.Uint(1);
					writer.Key("record_type"); writer.String("wts_position_visible_snapshot");
					writer.Key("provider_field_completeness_verified"); writer.Bool(false);
					writer.Key("adapter_id"); writer.String(_id.c_str());
					writer.Key("reason"); writer.String(reason);
					writer.Key("exchange"); writer.String(pItem->getExchg());
					writer.Key("code"); writer.String(pItem->getCode());
					writer.Key("direction"); writer.Int(static_cast<int>(pItem->getDirection()));
					real("pre_hex", pItem->getPrePosition());
					real("new_hex", pItem->getNewPosition());
					writer.EndObject();
					retainUnresolvedPayload(std::string(buffer.GetString(), buffer.GetSize()), "position");
				}
				catch (...) {}
				continue;
			}
			{
				// NEW-154 (5d): a realtime trade newer than this position query
				// already moved the position; the snapshot row is stale for it.
				// A position is a level and a trade a delta: when no earlier
				// snapshot established this contract's baseline, the adapter
				// only holds the newer deltas and cannot tell whether the row
				// already includes them, so the account is paused (fail closed).
				bool ambiguous = false;
				bool ignored = false;
				uint64_t newerVersion = 0;
				uint64_t epoch = 0;
				{
					std::lock_guard<std::mutex> lock(_ledger_mutex);
					auto newer = _position_event_version.find(stdCode);
					epoch = _positions_query_version;
					if (newer != _position_event_version.end() && newer->second > _positions_query_version)
					{
						ignored = true;
						newerVersion = newer->second;
						ambiguous = _position_baseline.find(stdCode) == _position_baseline.end();
					}
					else
						_position_baseline.insert(stdCode);
				}
				if (ignored)
				{
					WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN,
						"[{}] position snapshot row of {} ignored: realtime trade version {} is newer than snapshot epoch {}",
						_id.c_str(), stdCode.c_str(), newerVersion, epoch);
					if (ambiguous)
						pauseAccount("position_snapshot", fmt::format(
							"position of {} has no snapshot baseline and a newer realtime trade; reconciliation required", stdCode));
					continue;
				}
			}
			PosItem& pos = _positions[stdCode];
			if (pItem->getDirection() == WDT_LONG)
			{
				pos.l_newavail = pItem->getAvailNewPos();
				pos.l_newvol = pItem->getNewPosition();
				pos.l_preavail = pItem->getAvailPrePos();
				pos.l_prevol = pItem->getPrePosition();
			}
			else
			{
				pos.s_newavail = pItem->getAvailNewPos();
				pos.s_newvol = pItem->getNewPosition();
				pos.s_preavail = pItem->getAvailPrePos();
				pos.s_prevol = pItem->getPrePosition();
			}
		}

		for (auto it = _positions.begin(); it != _positions.end(); it++)
		{
			const char* stdCode = it->first.c_str();
			const PosItem& pItem = it->second;
			printPosition(stdCode, pItem);
			forEachSink([&](ITrdNotifySink* sink) {
				sink->on_position(stdCode, true, pItem.l_prevol, pItem.l_preavail, pItem.l_newvol, pItem.l_newavail, _trading_day);
				sink->on_position(stdCode, false, pItem.s_prevol, pItem.s_preavail, pItem.s_newvol, pItem.s_newavail, _trading_day);
			});
		}
	}

	WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO,"[{}] Position data updated", _id.c_str());

	if (transitionState(AS_LOGINED, AS_POSITION_QRYED))
	{
		{
			std::lock_guard<std::mutex> lock(_ledger_mutex);
			_orders_query_version = ++_event_version;
		}
		_trader_api->queryOrders();
	}
}

void TraderAdapter::onRspOrders(const WTSArray* ayOrders)
{
	SpiIngress spiIngress(this);
	if (!spiIngress)
		return;
	// Card 5d: the snapshot epoch is the event version at which the query was
	// issued. Realtime events carry newer versions and win over the snapshot;
	// terminal entries are tombstones; live entries absent from a complete
	// snapshot become Absent tombstones (revivable only by a newer realtime
	// event). A NULL array is a failed/unknown query and changes nothing.
	std::vector<std::pair<uint32_t, const WTSOrderInfo*>> seenOwn;
	if (ayOrders)
	{
		if (_orders == NULL)
			_orders = OrderMap::create();

		uint64_t snapshotEpoch = 0;
		{
			std::lock_guard<std::mutex> lock(_ledger_mutex);
			snapshotEpoch = _orders_query_version;
			_orders_snapshot_version = snapshotEpoch;
		}
		std::unordered_set<uint32_t> seenIds;

		for (auto it = ayOrders->begin(); it != ayOrders->end(); it++)
		{
			WTSOrderInfo* orderInfo = (WTSOrderInfo*)(*it);
			if (orderInfo == NULL)
				continue;

			WTSContractInfo* cInfo = orderInfo->getContractInfo();
			if (cInfo == NULL || cInfo->getCommInfo() == NULL)
			{
				quarantineOrder(orderInfo, cInfo == NULL ? "contract unavailable" : "commodity unavailable");
				continue;
			}

			bool isBuy = (orderInfo->getDirection() == WDT_LONG && orderInfo->getOffsetType() == WOT_OPEN) || (orderInfo->getDirection() == WDT_SHORT && orderInfo->getOffsetType() != WOT_OPEN);

			WTSCommodityInfo* commInfo = cInfo->getCommInfo();
			std::string stdCode;
			if (commInfo->getCategoty() == CC_FutOption || commInfo->getCategoty() == CC_SpotOption)
				stdCode = CodeHelper::rawFutOptCodeToStdCode(cInfo->getCode(), cInfo->getExchg());
			else if (CodeHelper::isMonthlyCode(cInfo->getCode()))//如果是分月合约
				stdCode = CodeHelper::rawMonthCodeToStdCode(cInfo->getCode(), cInfo->getExchg());
			else
				stdCode = CodeHelper::rawFlatCodeToStdCode(cInfo->getCode(), cInfo->getExchg(), cInfo->getProduct());
			const uint32_t taggedId = parseLocalTag(orderInfo->getUserTag(), sizeof(orderInfo->m_strUserTag), _order_pattern);
			if (stdCode.empty() || taggedId == UINT32_MAX)
			{
				quarantineOrder(orderInfo, stdCode.empty() ? "standard code invalid" : "malformed local order tag");
				continue;
			}
			orderInfo->setCode(cInfo->getCode());

			_orderids.insert(orderInfo->getOrderID());

			WTSTradeStateInfo* statInfo = (WTSTradeStateInfo*)_stat_map->get(stdCode.c_str());
			if (statInfo == NULL)
			{
				statInfo = WTSTradeStateInfo::create(stdCode.c_str());
				_stat_map->add(stdCode, statInfo, false);
			}
			TradeStatInfo& statItem = statInfo->statInfo();
			if (isBuy)
			{
				statItem.b_orders++;
				statItem.b_ordqty += orderInfo->getVolume();

				if (orderInfo->isError())
				{
					statItem.b_wrongs++;
					statItem.b_wrongqty += orderInfo->getVolume() - orderInfo->getVolTraded();
				}
				else if (orderInfo->getOrderState() == WOS_Canceled)
				{			
					if (orderInfo->getOrderFlag() == WOF_NOR)
					{
						statItem.b_cancels++;
						statItem.b_canclqty += orderInfo->getVolume() - orderInfo->getVolTraded();
					}
					else
					{
						statItem.b_auto_cancels++;
						statItem.b_auto_canclqty += orderInfo->getVolume() - orderInfo->getVolTraded();
					}
				}
				
			}
			else
			{
				statItem.s_orders++;
				statItem.s_ordqty += orderInfo->getVolume();

				if (orderInfo->isError())
				{
					statItem.s_wrongs++;
					statItem.s_wrongqty += orderInfo->getVolume() - orderInfo->getVolTraded();
				}
				else if (orderInfo->getOrderState() == WOS_Canceled)
				{
					if (orderInfo->getOrderFlag() == WOF_NOR)
					{
						statItem.s_cancels++;
						statItem.s_canclqty += orderInfo->getVolume() - orderInfo->getVolTraded();
					}
					else
					{
						statItem.s_auto_cancels++;
						statItem.s_auto_canclqty += orderInfo->getVolume() - orderInfo->getVolTraded();
					}
				}
			}			

			if (taggedId == 0)
				continue;
			const uint32_t localid = taggedId;
			seenIds.insert(localid);
			seenOwn.emplace_back(localid, orderInfo);

			bool keepActive = false;
			bool firstTerminal = false;
			std::string intentId;
			{
				std::lock_guard<std::mutex> lock(_ledger_mutex);
				auto entry = _ledger.find(localid);
				if (entry != _ledger.end() && entry->second.last_version > snapshotEpoch)
				{
					// A realtime event newer than this snapshot already updated
					// the order: the snapshot row is stale for it.
					keepActive = false;
				}
				else if (entry != _ledger.end() && entry->second.state == OrderLedgerState::Terminal)
				{
					keepActive = false;	// tombstone: never resurrected by a snapshot
				}
				else
				{
					firstTerminal = ledgerApplyOrderLocked(localid, stdCode, isBuy, orderInfo->isAlive(),
						orderInfo->getVolume(), orderInfo->getVolLeft(), snapshotEpoch);
					keepActive = orderInfo->isAlive();
				}
				entry = _ledger.find(localid);
				if (entry != _ledger.end())
					intentId = entry->second.intent_id;
			}
			if (firstTerminal)
				journalTerminal(localid, intentId, (uint32_t)orderInfo->getOrderState(), orderInfo->getVolTraded());

			if (keepActive)
			{
				SpinLock lock(_mtx_orders);
				_orders->add(localid, orderInfo);
			}
		}

		// NEW-154: ITraderSpi::onRspOrders cannot say whether a query failed.
		// Providers deliver an empty (reused, cleared) array both for "no
		// orders" and for an error response, so an empty snapshot is treated
		// like a failed query (fail closed): no live order is tombstoned and no
		// reserved quantity is released. Absence is only trusted in a
		// non-empty snapshot; a partially failed non-empty snapshot is still
		// indistinguishable at this interface.
		const bool trustAbsence = ayOrders->size() != 0;
		if (!trustAbsence)
			WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN,
				"[{}] empty order snapshot epoch {} cannot be told apart from a failed query; live orders kept", _id.c_str(), snapshotEpoch);
		std::vector<uint32_t> absent;
		{
			std::lock_guard<std::mutex> inflightLock(_inflight_mutex);
			std::lock_guard<std::mutex> lock(_ledger_mutex);
			for (auto& item : _ledger)
			{
				if (!trustAbsence)
					break;
				OrderLedgerEntry& entry = item.second;
				if (seenIds.count(item.first) != 0)
					continue;
				if (entry.state != OrderLedgerState::Live && entry.state != OrderLedgerState::Submitting)
					continue;
				if (entry.created_version > snapshotEpoch || entry.last_version > snapshotEpoch)
					continue;	// newer than the query: the snapshot cannot speak for it
				if (_inflight_entrusts.find(item.first) != _inflight_entrusts.end())
					continue;
				ledgerTombstoneLocked(item.first, entry, OrderLedgerState::Absent);
				absent.push_back(item.first);
			}
			for (auto it = _undone_qty.begin(); it != _undone_qty.end(); it++)
				WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO,
					"[{}]{} undone quantity {}", _id.c_str(), it->first.c_str(), it->second);
		}
		for (uint32_t localid : absent)
		{
			{
				SpinLock lock(_mtx_orders);
				_orders->remove(localid);
			}
			WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN,
				"[{}] Order {} absent from order snapshot epoch {}; marked as absent tombstone", _id.c_str(), localid, snapshotEpoch);
		}
	}
	reconcileIdentityRecovery(seenOwn);

	if (transitionState(AS_POSITION_QRYED, AS_ORDERS_QRYED))
	{
		_trader_api->queryTrades();
	}
}

void TraderAdapter::printPosition(const char* code, const PosItem& pItem)
{
	WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO, "[{}] {} position updated, long:{}[{}]|{}[{}], short:{}[{}]|{}[{}]",
		_id.c_str(), code, pItem.l_prevol, pItem.l_preavail, pItem.l_newvol, pItem.l_newavail, 
		pItem.s_prevol, pItem.s_preavail, pItem.s_newvol, pItem.s_newavail);
}

void TraderAdapter::onRspTrades(const WTSArray* ayTrades)
{
	SpiIngress spiIngress(this);
	if (!spiIngress)
		return;
	if (ayTrades)
	{
		for (auto it = ayTrades->begin(); it != ayTrades->end(); it++)
		{
			WTSTradeInfo* tInfo = (WTSTradeInfo*)(*it);

			if (tInfo == NULL)
				continue;
			WTSContractInfo* cInfo = tInfo->getContractInfo();
			if (cInfo == NULL && _bd_mgr != NULL)
				cInfo = _bd_mgr->getContract(tInfo->getCode(), tInfo->getExchg());
			if (cInfo == NULL || cInfo->getCommInfo() == NULL)
			{
				quarantineTrade(tInfo, cInfo == NULL ? "contract unavailable" : "commodity unavailable");
				continue;
			}

			WTSCommodityInfo* commInfo = cInfo->getCommInfo();
			std::string stdCode;
			if (commInfo->getCategoty() == CC_Future)
				stdCode = CodeHelper::rawMonthCodeToStdCode(cInfo->getCode(), cInfo->getExchg());
			else if (commInfo->getCategoty() == CC_FutOption || commInfo->getCategoty() == CC_SpotOption)
				stdCode = CodeHelper::rawFutOptCodeToStdCode(cInfo->getCode(), cInfo->getExchg());
			else if (commInfo->getCategoty() == CC_Stock)
				stdCode = CodeHelper::rawFlatCodeToStdCode(cInfo->getCode(), cInfo->getExchg(), cInfo->getProduct());
			else
				stdCode = CodeHelper::rawFlatCodeToStdCode(cInfo->getCode(), cInfo->getExchg(), commInfo->getProduct());
			if (stdCode.empty())
			{
				quarantineTrade(tInfo, "standard code invalid");
				continue;
			}
			tInfo->setCode(cInfo->getCode());

			WTSTradeStateInfo* statInfo = (WTSTradeStateInfo*)_stat_map->get(stdCode.c_str());
			if(statInfo == NULL)
			{
				statInfo = WTSTradeStateInfo::create(stdCode.c_str());
				_stat_map->add(stdCode, statInfo, false);
			}
			TradeStatInfo& statItem = statInfo->statInfo();

			bool isLong = (tInfo->getDirection() == WDT_LONG);
			bool isOpen = (tInfo->getOffsetType() == WOT_OPEN);
			bool isCloseT = (tInfo->getOffsetType() == WOT_CLOSETODAY);
			double qty = tInfo->getVolume();
			{
				// NEW-154: a trade already counted (realtime push or an earlier
				// query) is not counted again; self-match checks still run.
				std::lock_guard<std::mutex> lock(_ledger_mutex);
				const std::string key = tradeReportKey(tInfo);
				if (!key.empty() && !_counted_trades.insert(key).second)
					qty = 0;
			}

			if (isLong)
			{
				if (isOpen)
					statItem.l_openvol += qty;
				else if (isCloseT)
					statItem.l_closetvol += qty;
				else
					statItem.l_closevol += qty;
			}
			else
			{
				if (isOpen)
					statItem.s_openvol += qty;
				else if (isCloseT)
					statItem.s_closetvol += qty;
				else
					statItem.s_closevol += qty;
			}

			checkSelfMatch(stdCode.c_str(), tInfo);
		}

		for (auto it = _stat_map->begin(); it != _stat_map->end(); it++)
		{
			const char* stdCode = it->first.c_str();
			WTSTradeStateInfo* pItem = (WTSTradeStateInfo*)it->second;
			WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO, 
				"[{}] {} action stats updated, long opened: {}, long closed: {}, new long closed: {}, short opened: {}, short closed: {}, new short closed: {}",
				_id.c_str(), stdCode, pItem->open_volume_long(), pItem->close_volume_long(), pItem->closet_volume_long(),
				pItem->open_volume_short(), pItem->close_volume_short(), pItem->closet_volume_short());
		}
	}

	if (transitionState(AS_ORDERS_QRYED, AS_TRADES_QRYED))
	{
		_trader_api->queryAccount();
	}
}

bool TraderAdapter::checkSelfMatch(const char* stdCode, WTSTradeInfo* tInfo)
{
	if (tInfo == NULL)
		return false;

	const char* tid = tInfo->getTradeID();
	const char* refid = tInfo->getRefOrder();

	auto it = _trade_refs.find(tid);
	if (it != _trade_refs.end())
	{
		/*
		 *	By Wesley @ 2022.03.07
		 *	如果成交单号已经存在，则检查关联订单号是否相同
		 */
		const std::string& oid = it->second;
		if (oid.compare(refid) != 0)
		{
			//同一个成交单的关联订单ID不同，这一定是自成交了
			WTSLogger::log_dyn("trader", _id.c_str(), LL_FATAL, 
				"[{0}] Self matching detected on {1}!!! Instructions on {1} will be forbidden!!!", _id.c_str(), stdCode);
			_self_matches.insert(stdCode);
			return true;
		}
		else
		{
			//关联订单一样，说明是重复推送，不用管了
		}
	}
	else
	{
		_trade_refs[tid] = refid;
	}

	return false;
}

inline const char* stateToName(WTSOrderState woState)
{
	if (woState == WOS_AllTraded)
		return "AllTrd";
	else if (woState == WOS_PartTraded_NotQueuing || woState == WOS_PartTraded_Queuing)
		return "PrtTrd";
	else if (woState == WOS_NotTraded_Queuing)
		return "UnTrd";
	else if (woState == WOS_Canceled || woState == WOS_NotTraded_NotQueuing)
		return "Cncld";
	else if (woState == WOS_Submitting)
		return "Smtting";
	else if (woState == WOS_Nottouched)
		return "UnSmt";
	else
		return "Error";
}

void TraderAdapter::onPushOrder(WTSOrderInfo* orderInfo)
{
	SpiIngress spiIngress(this);
	if (!spiIngress)
		return;
	if (orderInfo == NULL)
		return;

	WTSContractInfo* cInfo = orderInfo->getContractInfo();
	if (cInfo == NULL)
	{
		quarantineOrder(orderInfo, "contract unavailable");
		return;
	}
	WTSCommodityInfo* commInfo = cInfo->getCommInfo();
	if (commInfo == NULL)
	{
		quarantineOrder(orderInfo, "commodity unavailable");
		return;
	}
	std::string stdCode;
	if (commInfo->getCategoty() == CC_FutOption || commInfo->getCategoty() == CC_SpotOption)
		stdCode = CodeHelper::rawFutOptCodeToStdCode(cInfo->getCode(), cInfo->getExchg());
	else if (CodeHelper::isMonthlyCode(cInfo->getCode()))//如果是分月合约
		stdCode = CodeHelper::rawMonthCodeToStdCode(cInfo->getCode(), cInfo->getExchg());
	else
		stdCode = CodeHelper::rawFlatCodeToStdCode(cInfo->getCode(), cInfo->getExchg(), cInfo->getProduct());
	if (stdCode.empty())
	{
		quarantineOrder(orderInfo, "standard code invalid");
		return;
	}
	const uint32_t taggedId = parseLocalTag(orderInfo->getUserTag(), sizeof(orderInfo->m_strUserTag), _order_pattern);
	if (taggedId == UINT32_MAX)
	{
		quarantineOrder(orderInfo, "malformed local order tag");
		return;
	}

	orderInfo->setCode(cInfo->getCode());

	bool isBuy = orderInfo->isNet() ? orderInfo->isNetBuy() : orderInfo->isBuyAction();
	
	//撤销的话, 要更新统计数据
	if (orderInfo->getOrderState() == WOS_Canceled)
	{
		WTSTradeStateInfo* statInfo = (WTSTradeStateInfo*)_stat_map->get(stdCode.c_str());
		if (statInfo == NULL)
		{
			statInfo = WTSTradeStateInfo::create(stdCode.c_str());
			_stat_map->add(stdCode, statInfo, false);
		}
		TradeStatInfo& statItem = statInfo->statInfo();
		if(isBuy)
		{
			if (orderInfo->isError())//错单要和撤单区分开
			{
				statItem.b_wrongs++;
				statItem.b_wrongqty += orderInfo->getVolume() - orderInfo->getVolTraded();
			}
			else
			{
				//只有普通订单的撤单才计入统计
				if (orderInfo->getOrderFlag() == WOF_NOR)
				{
					statItem.b_cancels++;
					statItem.b_canclqty += orderInfo->getVolume() - orderInfo->getVolTraded();
				}
				else
				{
					statItem.b_auto_cancels++;
					statItem.b_auto_canclqty += orderInfo->getVolume() - orderInfo->getVolTraded();
				}
			}
		}
		else
		{
			if (orderInfo->isError())//错单要和撤单区分开
			{
				statItem.s_wrongs++;
				statItem.s_wrongqty += orderInfo->getVolume() - orderInfo->getVolTraded();
			}
			else
			{
				//只有普通订单的撤单才计入统计
				if (orderInfo->getOrderFlag() == WOF_NOR)
				{
					statItem.s_cancels++;
					statItem.s_canclqty += orderInfo->getVolume() - orderInfo->getVolTraded();
				}
				else
				{
					statItem.s_auto_cancels++;
					statItem.s_auto_canclqty += orderInfo->getVolume() - orderInfo->getVolTraded();
				}
			}
		}
	}

	WTSLogger::log_dyn("trader", _id.c_str(), LL_DEBUG, "[{}] Order notified, instrument: {}, usertag: {}, state: {}", _id.c_str(), stdCode.c_str(), orderInfo->getUserTag(), stateToName(orderInfo->getOrderState()));

	//先检查该订单是不是第一次推送过来
	//如果是第一次推送过来, 则要根据开平更新可平
	if (strlen(orderInfo->getOrderID()) > 0)
	{
		auto it = _orderids.find(orderInfo->getOrderID());
		if (it == _orderids.end())
		{
			//先把订单号缓存起来, 防止重复处理
			_orderids.insert(orderInfo->getOrderID());

			WTSTradeStateInfo* statInfo = (WTSTradeStateInfo*)_stat_map->get(stdCode.c_str());
			if (statInfo == NULL)
			{
				statInfo = WTSTradeStateInfo::create(stdCode.c_str());
				_stat_map->add(stdCode, statInfo, false);
			}
			TradeStatInfo& statItem = statInfo->statInfo();
			if (isBuy)
			{
				statItem.b_orders++;
				statItem.b_ordqty += orderInfo->getVolume();
			}
			else
			{
				statItem.s_orders++;
				statItem.s_ordqty += orderInfo->getVolume();
			}

			//只有平仓需要更新可平
			if(!orderInfo->isNet())
			{
				if (orderInfo->getOffsetType() != WOT_OPEN)
				{
					//const char* code = stdCode.c_str();
					bool isLong = (orderInfo->getDirection() == WDT_LONG);
					bool isToday = (orderInfo->getOffsetType() == WOT_CLOSETODAY);
					double qty = orderInfo->getVolume();

					PosItem& pItem = _positions[stdCode];
					if (isLong)	//平多
					{
						if (isToday)
						{
							pItem.l_newavail -= min(pItem.l_newavail, qty);	//如果是平今, 则只需要更新可平今仓
						}
						else
						{
							double left = qty;

							//如果是平仓, 则先更新可平昨仓, 还有剩余, 再更新可平今仓
							//如果品种区分平昨平今, 也按照这个流程, 因为平昨的总数量不可能超出昨仓
							double maxQty = min(pItem.l_preavail, qty);
							pItem.l_preavail -= maxQty;
							pItem.l_preavail = max(pItem.l_preavail, 0.0);
							left -= maxQty;

							if (left > 0)
							{
								pItem.l_newavail -= min(pItem.l_newavail, left);
								pItem.l_newavail = max(pItem.l_newavail, 0.0);
							}
						}
					}
					else //平空
					{
						if (isToday)
						{
							pItem.s_newavail -= min(pItem.s_newavail, qty);
						}
						else
						{
							double left = qty;

							double maxQty = min(pItem.s_preavail, qty);
							pItem.s_preavail -= maxQty;
							pItem.s_preavail = max(pItem.s_preavail, 0.0);
							left -= maxQty;

							if (left > 0)
							{
								pItem.s_newavail -= min(pItem.s_newavail, left);
								pItem.s_newavail = max(pItem.s_newavail, 0.0);
							}
						}
					}
					printPosition(stdCode.c_str(), pItem);
				}
			}
		}
		else if (orderInfo->getOrderState() == WOS_Canceled && !orderInfo->isNet() && orderInfo->getOffsetType() != WOT_OPEN)
		{
			//如果订单不是第一次推送, 且撤销了, 则要更新可平量
			//const char* code = orderInfo->getCode();
			bool isLong = (orderInfo->getDirection() == WDT_LONG);
			bool isToday = (orderInfo->getOffsetType() == WOT_CLOSETODAY);
			double qty = orderInfo->getVolume() - orderInfo->getVolTraded();

			PosItem& pItem = _positions[stdCode];
			if (isLong)	//平多
			{
				if (isToday)
				{
					pItem.l_newavail += qty;	//如果是平今, 则只需要更新可平今仓
				}
				else
				{
					pItem.l_preavail += qty;
					if (pItem.l_preavail > pItem.l_prevol)
					{
						pItem.l_newavail += (pItem.l_preavail - pItem.l_prevol);
						pItem.l_preavail = pItem.l_prevol;
					}
				}
			}
			else //平空
			{
				if (isToday)
				{
					pItem.s_newavail += qty;
				}
				else
				{
					pItem.s_preavail += qty;
					if (pItem.s_preavail > pItem.s_prevol)
					{
						pItem.s_newavail += (pItem.s_preavail - pItem.s_prevol);
						pItem.s_preavail = pItem.s_prevol;
					}
				}
			}
			printPosition(stdCode.c_str(), pItem);
		}
	}

	

	uint32_t localid = 0;
	std::string orderOwner;

	//先看看是不是wt发出去的单子
	if (taggedId != 0)
	{
		localid = taggedId;

		const bool canceled = orderInfo->getOrderState() == WOS_Canceled;
		noteInflightReport(localid, canceled ? INFLIGHT_REJECTED : INFLIGHT_ACCEPTED);
		// Per-order ledger: remaining quantity only shrinks and a terminal
		// entry is a tombstone, so duplicate/late reports are idempotent.
		bool firstTerminal = false;
		std::string intentId;
		{
			std::lock_guard<std::mutex> lock(_ledger_mutex);
			firstTerminal = ledgerApplyOrderLocked(localid, stdCode, isBuy, orderInfo->isAlive(),
				orderInfo->getVolume(), orderInfo->getVolLeft(), ++_event_version);
			auto it = _ledger.find(localid);
			if (it != _ledger.end())
			{
				intentId = it->second.intent_id;
				orderOwner = it->second.owner_id;
			}
		}
		if (firstTerminal)
			journalTerminal(localid, intentId, (uint32_t)orderInfo->getOrderState(), orderInfo->getVolTraded());
		if (canceled && firstTerminal)
		{
			double qty = orderInfo->getVolume() - orderInfo->getVolTraded();
			WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO, "[{}] Order {} of {} canceled:{}, action: {}, leftqty: {}",
				_id.c_str(), orderInfo->getUserTag(), stdCode.c_str(), orderInfo->getStateMsg(),
				formatAction(orderInfo->getDirection(), orderInfo->getOffsetType()), qty);
		}
	}

	//如果是wt发出去的单子则需要更新内部数据
	if(localid != 0)
	{
		{
			SpinLock lock(_mtx_orders);
			if (!orderInfo->isAlive())
			{
				if (_orders)
					_orders->remove(localid);
			}
			else
			{
				if (_orders == NULL)
					_orders = OrderMap::create();

				_orders->add(localid, orderInfo);
			}
		}

		//通知所有监听接口（owner 路由：已归属订单只送达其 owner 与非 owner 观察者）
		dispatchOrderEvent(orderOwner, [&](ITrdNotifySink* sink) {
			sink->on_order(localid, stdCode.c_str(), isBuy, orderInfo->getVolume(), orderInfo->getVolLeft(), orderInfo->getPrice(), orderInfo->getOrderState() == WOS_Canceled);
		});
	}

	//不管是不是内部订单,订单结束了,都要写到日志里
	if (_save_data && !orderInfo->isAlive())
	{
		logOrder(localid, stdCode.c_str(), orderInfo);
	}

	if (_notifier)
		_notifier->notify(id(), localid, stdCode.c_str(), orderInfo);
}

void TraderAdapter::onPushTrade(WTSTradeInfo* tradeRecord)
{
	SpiIngress spiIngress(this);
	if (!spiIngress)
		return;
	if (tradeRecord == NULL)
		return;
	WTSContractInfo* cInfo = tradeRecord->getContractInfo();
	if (cInfo == NULL && _bd_mgr != NULL)
		cInfo = _bd_mgr->getContract(tradeRecord->getCode(), tradeRecord->getExchg());
	if (cInfo == NULL)
	{
		quarantineTrade(tradeRecord, "contract unavailable");
		return;
	}

	WTSCommodityInfo* commInfo = cInfo->getCommInfo();
	if (commInfo == NULL)
	{
		quarantineTrade(tradeRecord, "commodity unavailable");
		return;
	}
	if (cInfo->getCode()[0] == '\0' || cInfo->getExchg()[0] == '\0')
	{
		quarantineTrade(tradeRecord, "contract code unavailable");
		return;
	}
	std::string stdCode;
	if (commInfo->getCategoty() == CC_Future)
		stdCode = CodeHelper::rawMonthCodeToStdCode(cInfo->getCode(), cInfo->getExchg());
	else if (commInfo->getCategoty() == CC_FutOption || commInfo->getCategoty() == CC_SpotOption)
		stdCode = CodeHelper::rawFutOptCodeToStdCode(cInfo->getCode(), cInfo->getExchg());
	else if (commInfo->getCategoty() == CC_Stock)
		stdCode = CodeHelper::rawFlatCodeToStdCode(cInfo->getCode(), cInfo->getExchg(), cInfo->getProduct());
	else
		stdCode = CodeHelper::rawFlatCodeToStdCode(cInfo->getCode(), cInfo->getExchg(), commInfo->getProduct());

	if (stdCode.empty())
	{
		quarantineTrade(tradeRecord, "standard code invalid");
		return;
	}
	const uint32_t taggedId = parseLocalTag(tradeRecord->getUserTag(), sizeof(tradeRecord->m_strUserTag), _order_pattern);
	if (taggedId == UINT32_MAX)
	{
		quarantineTrade(tradeRecord, "malformed local order tag");
		return;
	}
	tradeRecord->setCode(cInfo->getCode());
	WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO, 
		"[{}] Trade notified, instrument: {}, usertag: {}, trdqty: {}, trdprice: {}", 
			_id.c_str(), stdCode.c_str(), tradeRecord->getUserTag(), tradeRecord->getVolume(), tradeRecord->getPrice());

	bool countStats = true;
	{
		std::lock_guard<std::mutex> lock(_ledger_mutex);
		// NEW-154: a redelivered realtime trade (same trade id, order reference
		// and direction) is applied once: no second position, statistic, ledger
		// or sink effect.
		const std::string key = tradeReportKey(tradeRecord);
		if (!key.empty() && !_pushed_trades.insert(key).second)
		{
			WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN,
				"[{}] duplicate trade push {} of {} ignored", _id.c_str(), tradeRecord->getTradeID(), stdCode.c_str());
			return;
		}
		if (!key.empty())
			countStats = _counted_trades.insert(key).second;
		_position_event_version[stdCode] = ++_event_version;
	}
	PosItem& pItem = _positions[stdCode];
	WTSTradeStateInfo* statInfo = (WTSTradeStateInfo*)_stat_map->get(stdCode.c_str());
	if (statInfo == NULL)
	{
		statInfo = WTSTradeStateInfo::create(stdCode.c_str());
		_stat_map->add(stdCode, statInfo, false);
	}

	// A trade already counted by the trade query keeps its position effect but
	// is not counted in the statistics twice.
	TradeStatInfo uncountedStats;
	TradeStatInfo& statItem = countStats ? statInfo->statInfo() : uncountedStats;
	double vol = tradeRecord->getVolume();
	bool isNet = tradeRecord->isNet();
	bool isBuy = isNet ? tradeRecord->isNetBuy() : tradeRecord->isBuyAction();

	if(!isNet)
	{
		//非净头寸模式
		bool isLong = (tradeRecord->getDirection() == WDT_LONG);
		bool isOpen = (tradeRecord->getOffsetType() == WOT_OPEN);

		if (isLong)
		{
			if (isOpen)
			{
				pItem.l_newvol += vol;

				if (!commInfo->isT1())	//如果不是T1，则更新可用持仓
					pItem.l_newavail += vol;

				statItem.l_openvol += vol;
			}
			else if (tradeRecord->getOffsetType() == WOT_CLOSETODAY)
			{
				pItem.l_newvol -= vol;

				statItem.l_closevol += vol;
			}
			else
			{
				double left = vol;
				double maxVol = min(left, pItem.l_prevol);
				pItem.l_prevol -= maxVol;
				left -= maxVol;
				pItem.l_newvol -= left;

				statItem.l_closevol += vol;
			}
		}
		else
		{
			if (isOpen)
			{
				pItem.s_newvol += vol;
				if (!commInfo->isT1())	//如果不是T1，则更新可用持仓
					pItem.s_newavail += vol;

				statItem.s_openvol += vol;
			}
			else if (tradeRecord->getOffsetType() == WOT_CLOSETODAY)
			{
				pItem.s_newvol -= vol;

				statItem.s_closevol += vol;
			}
			else
			{
				double left = vol;
				double maxVol = min(left, pItem.s_prevol);
				pItem.s_prevol -= maxVol;
				left -= maxVol;
				pItem.s_newvol -= left;

				statItem.s_closevol += vol;
			}
		}
	}
	else
	{
		/*
		 *	By Wesley @ 2024.06.26
		 *	净头寸模式，则根据买卖方向，先平后开
		 */
		double left = vol;
		if(!isBuy)
		{
			double maxVol = min(left, pItem.l_prevol);
			if (decimal::gt(maxVol, 0))
			{
				pItem.l_prevol -= maxVol;
				left -= maxVol;
				statItem.l_closevol += maxVol;
			}

			maxVol = min(left, pItem.l_newvol);
			if(decimal::gt(maxVol, 0))
			{				
				pItem.l_newvol -= maxVol;
				left -= maxVol;
				statItem.l_closevol += maxVol;
			}

			if (decimal::gt(left, 0))
			{
				pItem.s_newvol += left;
				statItem.s_openvol += left;
			}
		}
		else
		{
			double maxVol = min(left, pItem.s_prevol);
			if (decimal::gt(maxVol, 0))
			{
				pItem.s_prevol -= maxVol;
				left -= maxVol;
				statItem.s_closevol += maxVol;
			}

			maxVol = min(left, pItem.s_newvol);
			if (decimal::gt(maxVol, 0))
			{
				pItem.s_newvol -= maxVol;
				left -= maxVol;
				statItem.s_closevol += maxVol;
			}

			if (decimal::gt(left, 0))
			{
				pItem.l_newvol += left;
				statItem.l_openvol += left;
			}
		}

		pItem.l_newavail = pItem.l_newvol;
		pItem.l_preavail = pItem.l_prevol;
		pItem.s_newavail = pItem.s_newvol;
		pItem.s_preavail = pItem.s_prevol;
	}
	
	printPosition(stdCode.c_str(), pItem);

	//如果是自己的订单，则更新未完成单
	uint32_t localid = 0;
	std::string tradeOwner;
	bool ownerResolved = true;
	if (taggedId != 0)
	{
		localid = taggedId;
		noteInflightReport(localid, INFLIGHT_ACCEPTED);

		std::lock_guard<std::mutex> lock(_ledger_mutex);
		auto it = _ledger.find(localid);
		if (it != _ledger.end())
		{
			it->second.last_version = ++_event_version;
			const std::string tradeId = tradeRecord->getTradeID();
			const bool duplicate = !tradeId.empty() && !it->second.trade_ids.insert(tradeId).second;
			if (!duplicate && it->second.state != OrderLedgerState::Terminal)
			{
				it->second.trade_filled += vol;
				ledgerRecomputeLocked(it->second);
			}
			tradeOwner = it->second.owner_id;
			ownerResolved = it->second.verified;
		}
		else
			ownerResolved = false;
	}

	// NEW-008 + L01-014: with owner routing, an own-tagged trade whose order
	// identity (and so its owner) this adapter cannot verify is never guessed
	// or broadcast into an owner's ledger. The durable identity journal may
	// still name the owner; otherwise the trade is retained and the account
	// paused for manual reconciliation. Observers still see it as unowned.
	if (!ownerResolved && isOwnerRoutingEnabled())
	{
		std::string journalOwner;
		std::shared_ptr<DiffOrderOwnershipJournalV1> journal = _identity_journal;
		const std::string ownerPrefix = _identity_owner + "/";
		if (journal && journal->ownerForLocalOrder(localid, journalOwner) == DiffOrderOwnershipJournalV1::Status::Ok &&
			(journalOwner == _identity_owner || journalOwner.compare(0, ownerPrefix.size(), ownerPrefix) == 0))
		{
			tradeOwner = journalOwner == _identity_owner ? std::string() : journalOwner.substr(ownerPrefix.size());
		}
		else
		{
			tradeOwner.clear();
			quarantineTrade(tradeRecord, "order owner unresolvable under owner routing");
		}
	}

	dispatchOrderEvent(tradeOwner, [&](ITrdNotifySink* sink) {
		sink->on_trade(localid, stdCode.c_str(), isBuy, vol, tradeRecord->getPrice());
	});

	if (_save_data)
	{
		logTrade(localid, stdCode.c_str(), tradeRecord);
	}

	checkSelfMatch(stdCode.c_str(), tradeRecord);

	if (_notifier)
		_notifier->notify(id(), localid, stdCode.c_str(), tradeRecord);

	_trader_api->queryAccount();
}

void TraderAdapter::onTraderError(WTSError* err, void* /*pData*/ /* = NULL */)
{
	SpiIngress spiIngress(this);
	if (!spiIngress)
		return;
	if(err)
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR,"[{}] Error of trading channel occured: {}", _id.c_str(), err->getMessage());

	if (_notifier)
		_notifier->notify(id(), fmtutil::format("Trading channel error: {}", err->getMessage()));
}

IBaseDataMgr* TraderAdapter::getBaseDataMgr()
{
	return _bd_mgr;
}

void TraderAdapter::handleTraderLog(WTSLogLevel ll, const char* message)
{
	SpiIngress spiIngress(this);
	if (!spiIngress)
		return;
	WTSLogger::log_dyn_raw("trader", _id.c_str(), ll, message);
}

void TraderAdapter::queryFund()
{
	if (!isReady())
		return;

	_trader_api->queryAccount();
}

#pragma endregion "ITraderSpi接口"


//////////////////////////////////////////////////////////////////////////
//CTPWrapperMgr
TraderAdapterMgr::~TraderAdapterMgr()
{
	release();
	if (!_adapters.empty())
		std::terminate(); // The manager cannot be destroyed from a sink callback.
}

bool TraderAdapterMgr::addAdapter(const char* tname, TraderAdapterPtr& adapter)
{
	if (adapter == NULL || strlen(tname) == 0)
		return false;

	auto it = _adapters.find(tname);
	if(it != _adapters.end())
	{
		WTSLogger::error("Same name of trading channels: {}", tname);
		return false;
	}

	_adapters[tname] = adapter;

	return true;
}

AccountTradingGate::ResumeStatus TraderAdapterMgr::resumeAccountTrading(const char* tname,
	const AccountTradingGate::ResumeRequest& request, std::string* diagnostic)
{
	TraderAdapterPtr adapter = getAdapter(tname ? tname : "");
	if (!adapter)
	{
		if (diagnostic)
			*diagnostic = std::string("unknown trader adapter ") + (tname ? tname : "");
		return AccountTradingGate::ResumeStatus::InvalidRequest;
	}
	return adapter->resumeAccountTrading(request, diagnostic);
}

uint64_t TraderAdapterMgr::accountPauseGeneration(const char* tname)
{
	TraderAdapterPtr adapter = getAdapter(tname ? tname : "");
	return adapter ? adapter->accountPauseGeneration() : 0;
}

TraderAdapterPtr TraderAdapterMgr::getAdapter(const char* tname)
{
	auto it = _adapters.find(tname);
	if (it != _adapters.end())
	{
		return it->second;
	}

	return TraderAdapterPtr();
}

void TraderAdapterMgr::run()
{
	for (auto it = _adapters.begin(); it != _adapters.end(); it++)
	{
		it->second->run();
	}

	WTSLogger::info("{} trading channels started", _adapters.size());
}

void TraderAdapterMgr::release()
{
	requestStopSinkNotifications();
	if (TraderAdapter::isInAnySinkCallback())
		return;

	for (auto it = _adapters.begin(); it != _adapters.end(); it++)
	{
		it->second->release();
	}

	if (!waitSinkNotificationsStopped())
		return;

	_adapters.clear();
}

void TraderAdapterMgr::requestStopSinkNotifications()
{
	std::vector<TraderAdapterPtr> adapters;
	adapters.reserve(_adapters.size());
	for (auto it = _adapters.begin(); it != _adapters.end(); ++it)
		adapters.push_back(it->second);

	for (const TraderAdapterPtr& adapter : adapters)
		if (adapter)
			adapter->requestStopSinkNotifications();
}

bool TraderAdapterMgr::waitSinkNotificationsStopped()
{
	if (TraderAdapter::isInAnySinkCallback())
		return false;

	std::vector<TraderAdapterPtr> adapters;
	adapters.reserve(_adapters.size());
	for (auto it = _adapters.begin(); it != _adapters.end(); ++it)
		adapters.push_back(it->second);

	// Validate the entire set before blocking on any adapter. Manager callers
	// must request all first so no adapter is left accepting new notifications.
	for (const TraderAdapterPtr& adapter : adapters)
	{
		if (!adapter)
			continue;
		std::lock_guard<std::mutex> lock(adapter->_sink_mutex);
		if (!adapter->_sink_notifications_stopping)
			return false;
	}

	bool allStopped = true;
	for (const TraderAdapterPtr& adapter : adapters)
		if (adapter && !adapter->waitSinkNotificationsStopped())
			allStopped = false;
	return allStopped;
}

bool TraderAdapterMgr::isInSinkCallback() const
{
	for (auto it = _adapters.begin(); it != _adapters.end(); ++it)
	{
		if (it->second && it->second->isInSinkCallback())
			return true;
	}
	return false;
}

void TraderAdapterMgr::refresh_funds()
{
	for (auto it = _adapters.begin(); it != _adapters.end(); it++)
	{
		it->second->queryFund();
	}
}
