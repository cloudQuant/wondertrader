/*!
 * \file WtArbiExecuter.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 *
 * \brief 套利交易执行器实现
 * \details 该文件实现了套利交易执行器，用于执行多个相关合约的组合交易策略
 */
#include "WtArbiExecuter.h"
#include "WtExecuterSchedule.h"
#include "TraderAdapter.h"
#include "WtEngine.h"

#include "../Share/CodeHelper.hpp"
#include "../Includes/IDataManager.h"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/IHotMgr.h"
#include "../Share/decimal.h"

#include "../WTSTools/WTSLogger.h"

#include <algorithm>
#include <cstdio>
#include <tuple>
#include <vector>

USING_NS_WTP;

namespace
{
}


/*!
 * \brief 构造函数
 * \details 初始化套利交易执行器对象，设置基本参数和初始状态
 * \param factory 执行器工厂指针，用于创建执行单元
 * \param name 执行器名称
 * \param dataMgr 数据管理器指针，用于获取市场数据
 */
WtArbiExecuter::WtArbiExecuter(WtExecuterFactory* factory, const char* name, IDataManager* dataMgr)
	: IExecCommand(name)
	, _trader(NULL)
, _factory(factory)
	, _data_mgr(dataMgr)
	, _scale(1.0)
	, _auto_clear(true)
	, _channel_ready(false)
	{
}


/*!
 * \brief 析构函数
 * \details 清理套利交易执行器资源，等待线程池中的任务完成
 */
WtArbiExecuter::~WtArbiExecuter()
{
	// Ordinary runner teardown does not necessarily go through the command
	// manager. Make a best-effort local drain before members are released. If a
	// verified plugin unit remains unquiesced, ExeUnitWrapper deliberately
	// fail-stops in its destructor rather than release a borrowed context.
	requestQuiesce();
	(void)waitQuiesced();
}

WtArbiExecuter::QuiesceIngress::QuiesceIngress(WtArbiExecuter* owner)
	: _owner(owner)
	, _accepted(false)
	, _marker{ NULL, NULL }
{
	if (_owner != NULL)
		_accepted = _owner->enterQuiesceIngress(_marker);
}

WtArbiExecuter::QuiesceIngress::~QuiesceIngress()
{
	if (_accepted)
		_owner->leaveQuiesceIngress(_marker);
}

WtArbiExecuter::QuiesceDrainScope::QuiesceDrainScope(WtArbiExecuter* owner) noexcept
	: _owner(owner)
	, _marker{ NULL, NULL }
{
	if (_owner != NULL)
		_owner->enterQuiesceCallback(_marker);
}

WtArbiExecuter::QuiesceDrainScope::~QuiesceDrainScope()
{
	if (_owner != NULL)
	{
		_owner->leaveQuiesceCallback(_marker);
		_owner->finishQuiesceDrain();
	}
}

WtArbiExecuter::QuiesceThreadMarker*& WtArbiExecuter::currentQuiesceMarker() noexcept
{
	static thread_local QuiesceThreadMarker* current = NULL;
	return current;
}

bool WtArbiExecuter::enterQuiesceIngress(QuiesceThreadMarker& marker)
{
	// A producer call nested on an already accepted ingress/callback stack is
	// part of that accepted work. Keep it admissible through drain completion;
	// unrelated callers are rejected once the close point is published.
	bool acceptedContinuation = false;
	for (QuiesceThreadMarker* current = currentQuiesceMarker(); current != NULL;
		current = current->previous)
	{
		if (current->owner == this)
		{
			acceptedContinuation = true;
			break;
		}
	}
	// Mutex acquisition is the only potentially throwing step and occurs before
	// the count changes. Once accepted, the stack/TLS marker links use only
	// pointer assignments and cannot allocate or throw.
	{
		std::lock_guard<std::mutex> lock(_quiesce_mutex);
		if (_quiesce_requested.load(std::memory_order_acquire) && !acceptedContinuation)
			return false;
		++_quiesce_active_ingress;
	}
	marker.owner = this;
	marker.previous = currentQuiesceMarker();
	currentQuiesceMarker() = &marker;
	return true;
}

void WtArbiExecuter::leaveQuiesceIngress(QuiesceThreadMarker& marker)
{
	QuiesceThreadMarker*& current = currentQuiesceMarker();
	QuiesceThreadMarker** link = &current;
	while (*link != NULL && *link != &marker)
		link = &(*link)->previous;
	if (*link == &marker)
		*link = marker.previous;
	marker.owner = NULL;
	marker.previous = NULL;
	{
		std::lock_guard<std::mutex> lock(_quiesce_mutex);
		if (_quiesce_active_ingress > 0)
			--_quiesce_active_ingress;
	}
	_quiesce_condition.notify_all();
}

void WtArbiExecuter::reserveQuiesceDrain()
{
	std::lock_guard<std::mutex> lock(_quiesce_mutex);
	++_quiesce_active_drains;
}

void WtArbiExecuter::finishQuiesceDrain()
{
	{
		std::lock_guard<std::mutex> lock(_quiesce_mutex);
		if (_quiesce_active_drains > 0)
			--_quiesce_active_drains;
	}
	_quiesce_condition.notify_all();
}

void WtArbiExecuter::enterQuiesceCallback(QuiesceThreadMarker& marker) noexcept
{
	marker.owner = this;
	marker.previous = currentQuiesceMarker();
	currentQuiesceMarker() = &marker;
}

void WtArbiExecuter::leaveQuiesceCallback(QuiesceThreadMarker& marker) noexcept
{
	QuiesceThreadMarker*& current = currentQuiesceMarker();
	QuiesceThreadMarker** link = &current;
	while (*link != NULL && *link != &marker)
		link = &(*link)->previous;
	if (*link == &marker)
		*link = marker.previous;
	marker.owner = NULL;
	marker.previous = NULL;
}

bool WtArbiExecuter::isOnQuiesceThread() const noexcept
{
	for (QuiesceThreadMarker* marker = currentQuiesceMarker(); marker != NULL;
		marker = marker->previous)
	{
		if (marker->owner == this)
			return true;
	}
	return false;
}

std::vector<ExecuteUnitPtr> WtArbiExecuter::snapshotQuiesceUnits() const
{
	std::vector<ExecuteUnitPtr> units;
	SpinLock lock(_mtx_units);
	const auto addUnique = [&units](const ExecuteUnitPtr& unit) {
		if (unit == NULL)
			return;
		for (const ExecuteUnitPtr& current : units)
		{
			if (current.get() == unit.get())
				return;
		}
		units.push_back(unit);
	};
	for (const auto& entry : _unit_map)
		addUnique(entry.second);
	// Keep units that were successfully initialized but later failed startup or
	// callback processing. They may still retain the borrowed ExecuteContext.
	for (const auto& entry : _unit_states)
	{
		const std::shared_ptr<UnitLifecycleState>& state = entry.second;
		if (state == NULL)
			continue;
		std::lock_guard<std::mutex> stateLock(state->mutex);
		addUnique(state->result);
		addUnique(state->retained_unit);
	}
	return units;
}

void WtArbiExecuter::requestQuiesce()
{
	// The atomic transition is the ingress close linearization point. It does
	// not wait behind a producer currently entering its guarded section.
	_quiesce_requested.store(true, std::memory_order_release);
	_quiesce_condition.notify_all();
}

bool WtArbiExecuter::canWaitQuiescedOnCurrentThread() const
{
	return preflightQuiesceOnCurrentThread();
}

bool WtArbiExecuter::preflightQuiesceOnCurrentThread() const
{
	// Factory-wide probes are a separate, deduplicated coordinator stage.
	// This host-only preflight answers whether the current caller is itself on
	// an ingress/callback stack; other threads' accepted ingress is drained later.
	return _quiesce_requested.load(std::memory_order_acquire) &&
		!isOnQuiesceThread();
}

WtExecuterFactory* WtArbiExecuter::quiesceFactory() const
{
	return _factory;
}

bool WtArbiExecuter::drainHostQuiesced()
{
	// This is the host-only phase. The coordinator must already have run the
	// factory-wide preflight for every host before calling it. Keep a local
	// self-wait guard for direct/reentrant use, but do not perform any plugin
	// requests or waits here.
	if (isOnQuiesceThread())
		return false;
	std::unique_lock<std::mutex> lock(_quiesce_mutex);
	if (!_quiesce_requested.load())
		return false;
	_quiesce_condition.wait(lock, [this]() {
		return _quiesce_active_ingress == 0 && _quiesce_active_drains == 0;
	});
	lock.unlock();
	// The reservation covers each callback drain. Wait once more without the
	// quiesce mutex so the pool also finishes its task wrapper/closure teardown.
	try
	{
		if (_pool)
			_pool->wait();
	}
	catch (...)
	{
		_quiesce_incomplete.store(true, std::memory_order_release);
		return false;
	}

	// Initializers are ingress-counted, so after host callbacks and the pool
	// drain this snapshot contains every initialized, failed, or retired unit.
	// Keep the refs for a later coordinator-wide plugin request/preflight phase.
	std::vector<ExecuteUnitPtr> units;
	try
	{
		units = snapshotQuiesceUnits();
	}
	catch (...)
	{
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(_quiesce_mutex);
		_quiesce_units.swap(units);
		_quiesce_snapshot_ready = true;
	}
	return !_quiesce_incomplete.load(std::memory_order_acquire);
}

bool WtArbiExecuter::snapshotQuiescePluginOwners(
	std::vector<ExecQuiesceOwnerPtr>& out) const
{
	out.clear();
	try
	{
		std::vector<ExecQuiesceOwnerPtr> owners;
		std::lock_guard<std::mutex> lock(_quiesce_mutex);
		if (!_quiesce_requested.load(std::memory_order_acquire) ||
			!_quiesce_snapshot_ready || _quiesce_active_ingress != 0 ||
			_quiesce_active_drains != 0)
			return false;
		owners.reserve(_quiesce_units.size());
		for (const ExecuteUnitPtr& unit : _quiesce_units)
		{
			if (unit == NULL)
				return false;
			owners.push_back(std::static_pointer_cast<IExecCommandQuiesce>(unit));
		}
		out.swap(owners);
		return true;
	}
	catch (...)
	{
		out.clear();
		return false;
	}
}

bool WtArbiExecuter::completeQuiesceAfterGlobalPluginWait()
{
	// The coordinator calls this only after every owner in every manager has
	// completed its plugin wait. This acknowledgement is deliberately
	// non-blocking and never invokes plugin code; failed attempts retain the
	// host's strong unit snapshot for a control-thread retry.
	try
	{
		std::unique_lock<std::mutex> lock(_quiesce_mutex, std::try_to_lock);
		if (!lock.owns_lock())
			return false;
		if (!_quiesce_requested.load(std::memory_order_acquire) ||
			!_quiesce_snapshot_ready || _quiesce_active_ingress != 0 ||
			_quiesce_active_drains != 0 ||
			_quiesce_incomplete.load(std::memory_order_acquire))
			return false;
		for (const ExecuteUnitPtr& unit : _quiesce_units)
		{
			if (unit == NULL)
				return false;
		}
		_quiesce_plugins_completed = true;
		return true;
	}
	catch (...)
	{
		return false;
	}
}

bool WtArbiExecuter::waitQuiesced()
{
	// Preserve the legacy one-call entry point's current-thread safety when a
	// real factory is present. The staged coordinator already preflights every
	// host globally before calling drainHostQuiesced().
	if (!preflightQuiesceOnCurrentThread())
		return false;
	{
		std::lock_guard<std::mutex> lock(_quiesce_mutex);
		if (_quiesce_plugins_completed)
			return _quiesce_units.empty() &&
				!_quiesce_incomplete.load(std::memory_order_acquire);
	}
	if (!drainHostQuiesced())
		return false;
	std::vector<ExecQuiesceOwnerPtr> owners;
	if (!snapshotQuiescePluginOwners(owners))
		return false;
	if (!owners.empty())
		return false;
	std::lock_guard<std::mutex> lock(_quiesce_mutex);
	_quiesce_plugins_completed = true;
	return !_quiesce_incomplete.load(std::memory_order_acquire);
}

void WtArbiExecuter::failUnitDrain(const ExecuteUnitPtr& unit,
	const std::shared_ptr<UnitLifecycleState>& state, const char* reason) noexcept
{
	_quiesce_incomplete.store(true);
	std::deque<UnitLifecycleState::Event> discardedEvents;
	bool creationComplete = false;
	const auto updateState = [&]() {
		std::lock_guard<std::mutex> stateLock(state->mutex);
		state->callback_failed = true;
		state->side_effects_possible = true;
		state->callback_active = false;
		state->callback_drain_started = false;
		state->callback_owner = std::thread::id();
		creationComplete = state->creation_complete;
		if (creationComplete)
		{
			state->init_status = ExecUnitInitState::FAILED;
			state->result.reset();
		}
		try
		{
			state->init_error = "execution-unit callback/drain failed; accepted work may have been lost";
			if (reason != NULL && reason[0] != '\0')
				state->init_error += std::string(": ") + reason;
		}
		catch (...)
		{
			state->init_error.clear();
		}
		discardedEvents.swap(state->events);
	};

	try
	{
		SpinLock lock(_mtx_units);
		updateState();
		// Keep the map and retained-state references. An initialized plugin may
		// still use the borrowed ExecuteContext even after callback failure.
	}
	catch (...)
	{
		// Even if the lock/state transition fails, make a best-effort transition
		// so the active drain cannot leave a queued callback permanently running.
		try
		{
			updateState();
		}
		catch (...)
		{
		}
	}
	state->condition.notify_all();
	discardedEvents.clear();
	(void)unit;
	try
	{
		WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
			"Arbi execution-unit callback/drain failed for {}: {}", state->code,
			reason ? reason : "unknown drain failure");
	}
	catch (...)
	{
	}
}

/*!
 * \brief 设置交易适配器
 * \details 仅绑定交易适配器；就绪状态由订阅后的通道回调更新
 * \param adapter 交易适配器指针
 */
void WtArbiExecuter::setTrader(TraderAdapter* adapter)
{
	_trader = adapter;
}

/*!
 * \brief 初始化执行器
 * \details 根据传入的参数配置初始化套利执行器，设置缩放倍数、自动清理策略、线程池和合约组
 * \param params 初始化参数，包含执行器的各种配置
 * \return 是否初始化成功
 */
bool WtArbiExecuter::init(WTSVariant* params)
{
	if (params == NULL)
		return false;

	_config = params;
	_config->retain();

	_scale = params->getDouble("scale");
	_strict_sync  = params->getBoolean("strict_sync");

	uint32_t poolsize = params->getUInt32("poolsize");
	if(poolsize > 0)
	{
		_pool.reset(new boost::threadpool::pool(poolsize));
	}

	/*
	 *	By Wesley @ 2021.12.14
	 *	从配置文件中读取自动清理的策略
	 *	active: 是否启用
	 *	includes: 包含列表，格式如CFFEX.IF
	 *	excludes: 排除列表，格式如CFFEX.IF
	 */
	WTSVariant* cfgClear = params->get("clear");
	if(cfgClear)
	{
		_auto_clear = cfgClear->getBoolean("active");
		WTSVariant* cfgItem = cfgClear->get("includes");
		if(cfgItem)
		{
			if (cfgItem->type() == WTSVariant::VT_String)
				_clear_includes.insert(cfgItem->asCString());
			else if (cfgItem->type() == WTSVariant::VT_Array)
			{
				for(uint32_t i = 0; i < cfgItem->size(); i++)
					_clear_includes.insert(cfgItem->get(i)->asCString());
			}
		}

		cfgItem = cfgClear->get("excludes");
		if (cfgItem)
		{
			if (cfgItem->type() == WTSVariant::VT_String)
				_clear_excludes.insert(cfgItem->asCString());
			else if (cfgItem->type() == WTSVariant::VT_Array)
			{
				for (uint32_t i = 0; i < cfgItem->size(); i++)
					_clear_excludes.insert(cfgItem->get(i)->asCString());
			}
		}
	}

	WTSVariant* cfgGroups = params->get("groups");
	if (cfgGroups)
	{
		auto names = cfgGroups->memberNames();
		for(const std::string& gpname : names)
		{
			CodeGroupPtr& gpInfo = _groups[gpname];
			if (gpInfo == NULL)
			{
				gpInfo.reset(new CodeGroup);
				wt_strcpy(gpInfo->_name, gpname.c_str(), gpname.size());
			}

			WTSVariant* cfgGrp = cfgGroups->get(gpname.c_str());
			auto codes = cfgGrp->memberNames();
			for(const std::string& code : codes)
			{
				gpInfo->_items[code] = cfgGrp->getDouble(code.c_str());
				_code_to_groups[code] = gpInfo;
			}
		}
	}

	WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "Local executer inited, scale: {}, auto_clear: {}, strict_sync: {}, thread poolsize: {}, code_groups: {}",
		_scale, _auto_clear, _strict_sync, poolsize, _groups.size());

	return true;
}

/*! 
 * \brief 获取执行单元
 * \details 根据标准合约代码获取执行单元，如果单元不存在且允许自动创建，则创建新的执行单元
 * \param stdCode 标准合约代码
 * \param bAutoCreate 是否自动创建，默认为true
 * \return 执行单元指针
 */
ExecuteUnitPtr WtArbiExecuter::getUnit(const char* stdCode, bool bAutoCreate /* = true */)
{
	if (stdCode == NULL || stdCode[0] == '\0')
	{
		WTSLogger::error("Cannot get arbi execution unit for an empty code");
		return ExecuteUnitPtr();
	}

	const std::string code(stdCode);
	std::shared_ptr<UnitLifecycleState> state;
	ExecuteUnitPtr registeredDuringCreation;
	bool creator = false;
	auto newState = std::make_shared<UnitLifecycleState>();
	newState->creation_owner = std::this_thread::get_id();
	newState->code = code;
	{
		SpinLock lock(_mtx_units);
		auto pending = _unit_states.find(code);
		if (pending != _unit_states.end())
		{
			state = pending->second;
			std::lock_guard<std::mutex> stateLock(state->mutex);
			if (bAutoCreate && state->creation_complete &&
				state->init_status == ExecUnitInitState::FAILED && !state->side_effects_possible)
			{
				state = newState;
				state->init_generation = _next_unit_init_generation++;
				pending->second = state;
				creator = true;
			}
			else
			{
				auto mapped = _unit_map.find(code);
				if (mapped != _unit_map.end())
					registeredDuringCreation = mapped->second;
			}
		}
		else
		{
			auto it = _unit_map.find(code);
			if (it != _unit_map.end())
				return it->second;
			if (!bAutoCreate)
				return ExecuteUnitPtr();
			state = newState;
			state->init_generation = _next_unit_init_generation++;
			_unit_states.emplace(code, state);
			creator = true;
		}
	}

	if (!creator)
	{
		std::unique_lock<std::mutex> lock(state->mutex);
		if (!state->creation_complete)
		{
			// The initialized unit is safe to return while its startup callbacks drain;
			// they will serialize any newly queued events. Before registration, callers
			// must enqueue against the pending generation instead of waiting, because a
			// plugin callback may be joining this caller from another thread.
			if (registeredDuringCreation != NULL)
				return registeredDuringCreation;
			return ExecuteUnitPtr();
		}
		// A failed attempt is retried only by a later auto-create request.
		return state->result;
	}

	ExecuteUnitPtr unit;
	auto failCreation = [&](const char* reason) -> ExecuteUnitPtr
	{
		ExecuteUnitPtr mappedUnit;
		ExecuteUnitPtr retainedUnit = unit;
		std::deque<UnitLifecycleState::Event> discardedEvents;
		std::string failureDiagnostic;
		bool releaseUnstartedDrain = false;
		bool initializedUnit = unit != NULL && unit->self() != NULL;
		{
			SpinLock lock(_mtx_units);
			auto unitIt = _unit_map.find(code);
			if (unitIt != _unit_map.end())
			{
				mappedUnit = unitIt->second;
				if (mappedUnit != NULL && mappedUnit->self() != NULL)
				{
					initializedUnit = true;
					if (retainedUnit == NULL)
						retainedUnit = mappedUnit;
				}
				if (!initializedUnit)
					_unit_map.erase(unitIt);
			}
		}
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			if (initializedUnit)
				state->retained_unit = retainedUnit;
			state->result.reset();
			state->callback_failed = true;
			releaseUnstartedDrain = state->callback_active && !state->callback_drain_started;
			state->callback_active = false;
			state->callback_drain_started = false;
			state->callback_owner = std::thread::id();
			state->init_status = ExecUnitInitState::FAILED;
			state->init_error = reason ? reason : "unknown execution-unit initialization failure";
			if (state->side_effects_possible &&
				state->init_error.find("automatic retry is disabled") == std::string::npos)
				state->init_error += "; plugin side effects may have occurred, automatic retry is disabled";
			failureDiagnostic = state->init_error;
			state->creation_complete = true;
			state->creation_owner = std::thread::id();
			discardedEvents.swap(state->events);
		}
		// The ingress that initiated this creation cannot deliver its event to
		// an initialized unit, and any startup callbacks queued on this state are
		// discarded. Keep quiesce incomplete even if the queue itself was empty.
		_quiesce_incomplete.store(true);
		if (releaseUnstartedDrain)
			finishQuiesceDrain();
		state->condition.notify_all();
		discardedEvents.clear();
		try
		{
			WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
				"Arbi execution unit initialization for {} failed: {}", stdCode, failureDiagnostic);
		}
		catch (...)
		{
			// Cleanup and waiter notification precede diagnostics.
		}
		return ExecuteUnitPtr();
	};

	try
	{
		CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(stdCode, NULL);
		const std::string commID = codeInfo.stdCommID();
		if (_config == NULL)
			return failCreation("executor configuration is missing");
		WTSVariant* policy = _config->get("policy");
		if (policy == NULL)
			return failCreation("execution policy is missing");
		const std::string des = policy->has(commID.c_str()) ? commID : "default";
		WTSVariant* cfg = policy->get(des.c_str());
		if (cfg == NULL)
			return failCreation("policy entry is missing");
		const char* name = cfg->getCString("name");
		if (name == NULL || name[0] == '\0' || _factory == NULL)
			return failCreation("execution-unit factory or name is invalid");

		// Factory and initialization code is plugin-owned. It can produce external
		// effects before returning an error, so failures after this boundary cannot
		// be retried without an explicit side-effect reconstruction contract.
		{
			std::lock_guard<std::mutex> stateLock(state->mutex);
			state->side_effects_possible = true;
		}
		unit = _factory->createExeUnit(name);
		if (unit == NULL)
			return failCreation("execution-unit creation failed");
		std::string error;
		if (!unit->initialize(this, stdCode, cfg, error) || unit->self() == NULL)
			return failCreation(error.empty() ? "plugin initialization failed" : error.c_str());

		// Publish a strong owner into the already map-owned lifecycle state before
		// any startup callbacks or later allocations can fail. Failed initialized
		// units retain the ExecuteContext they borrowed until an authorized retry
		// of global quiescence succeeds.
		{
			std::lock_guard<std::mutex> stateLock(state->mutex);
			state->retained_unit = unit;
		}
		{
			std::lock_guard<std::mutex> positionLock(_mtx_positions);
			SpinLock lock(_mtx_units);
			// Reserve the initial drain before exposing the unit. Events arriving
			// after publication will queue behind this creator-owned reservation.
			{
				std::lock_guard<std::mutex> stateLock(state->mutex);
				if (!state->callback_active)
					reserveQuiesceDrain();
				state->callback_active = true;
				state->callback_drain_started = false;
				state->callback_owner = std::this_thread::get_id();
			}
			_unit_map[code] = unit;
			auto targetIt = _target_pos.find(code);
			auto dispatchIt = _target_dispatch_allowed.find(code);
			const bool replayTarget = dispatchIt == _target_dispatch_allowed.end() || dispatchIt->second;
			UnitLifecycleState::Event targetEvent;
			bool hasTargetReplay = false;
			if (targetIt != _target_pos.end() && replayTarget)
			{
				uint64_t targetSequence = 0;
				auto sequenceIt = _target_update_sequences.find(code);
				if (sequenceIt != _target_update_sequences.end())
					targetSequence = sequenceIt->second;
				{
					std::lock_guard<std::mutex> stateLock(state->mutex);
					state->initial_target_sequence = targetSequence;
				}
				UnitLifecycleState::Event targetEvent;
				const double traderTarget = round(targetIt->second * _scale);
				targetEvent.callback = [code, traderTarget](ExecuteUnit* target) {
					target->set_position(code.c_str(), traderTarget);
				};
				targetEvent.target_update = true;
				targetEvent.position_sequence = targetSequence;
				hasTargetReplay = true;
			}
			if (_channel_ready.load())
			{
				bool readyAlreadyQueued = false;
				{
					std::lock_guard<std::mutex> stateLock(state->mutex);
					for (const UnitLifecycleState::Event& event : state->events)
					{
						if (event.channel_ready && event.channel_epoch == _channel_epoch)
						{
							readyAlreadyQueued = true;
							break;
						}
					}
				}
				if (!readyAlreadyQueued)
				{
					UnitLifecycleState::Event event;
					event.callback = [](ExecuteUnit* target) { target->on_channel_ready(); };
					event.channel_ready = true;
					event.channel_epoch = _channel_epoch;
					prependUnitEvent(state, event);
				}
			}
			if (hasTargetReplay)
				prependUnitEvent(state, targetEvent);
		}
		// Events submitted while plugin initialization ran have no unit pointer yet.
		// This creator owns the first drain after publishing the initialized unit.
		drainUnitEvents(unit, state, true);

		for (;;)
		{
			drainUnitEvents(unit, state);
			bool failed = false;
			bool completed = false;
			std::string failureReason;
			{
				SpinLock lock(_mtx_units);
				std::lock_guard<std::mutex> stateLock(state->mutex);
				failed = state->callback_failed;
				if (failed)
					failureReason = state->init_error;
				if (!failed && state->events.empty() && !state->callback_active)
				{
					state->result = unit;
					state->init_status = ExecUnitInitState::READY;
					state->init_error.clear();
					state->creation_complete = true;
					state->creation_owner = std::thread::id();
					completed = true;
				}
			}
			if (failed)
				return failCreation(failureReason.empty() ?
					"execution-unit callback threw during initialization" : failureReason.c_str());
			if (completed)
			{
				state->condition.notify_all();
				return unit;
			}
		}
	}
	catch (const std::exception& ex)
	{
		return failCreation(ex.what());
	}
	catch (...)
	{
		return failCreation("unexpected exception while creating arbi execution unit");
	}
}

ExecUnitInitStatus WtArbiExecuter::queryUnitInitStatus(const char* stdCode) const
{
	ExecUnitInitStatus status;
	if (stdCode == NULL || stdCode[0] == '\0')
	{
		status.query_result = ExecUnitInitQueryResult::INVALID_ARGUMENT;
		status.error = "standard code is empty";
		return status;
	}

	SpinLock lock(_mtx_units);
	auto it = _unit_states.find(stdCode);
	if (it == _unit_states.end())
	{
		status.error = "no initialization attempt is recorded for this code";
		return status;
	}

	const std::shared_ptr<UnitLifecycleState>& state = it->second;
	std::lock_guard<std::mutex> stateLock(state->mutex);
	status.state = state->init_status;
	status.generation = state->init_generation;
	status.waiting_callers = state->waiting_callers;
	status.error = state->init_error;
	return status;
}

ExecUnitInitClearResult WtArbiExecuter::clearFailedUnitInitStatus(const char* stdCode)
{
	if (stdCode == NULL || stdCode[0] == '\0')
		return ExecUnitInitClearResult::INVALID_ARGUMENT;

	SpinLock lock(_mtx_units);
	auto it = _unit_states.find(stdCode);
	if (it == _unit_states.end())
		return ExecUnitInitClearResult::NOT_FOUND;

	const std::shared_ptr<UnitLifecycleState> state = it->second;
	std::lock_guard<std::mutex> stateLock(state->mutex);
	if (!state->creation_complete || state->init_status != ExecUnitInitState::FAILED)
		return ExecUnitInitClearResult::NOT_FAILED;
	if (state->side_effects_possible)
		return ExecUnitInitClearResult::RETRY_BLOCKED;

	_unit_states.erase(it);
	return ExecUnitInitClearResult::CLEARED;
}

bool WtArbiExecuter::appendUnitEvent(const std::shared_ptr<UnitLifecycleState>& state,
	const UnitLifecycleState::Event& event)
{
	if (state == NULL)
		return false;
	std::lock_guard<std::mutex> lock(state->mutex);
	if (state->callback_failed)
		return false;
	UnitLifecycleState::Event queued = event;
	if (queued.init_generation == 0)
		queued.init_generation = state->init_generation;
	state->events.push_back(std::move(queued));
	if (state->callback_active)
		return false;
	reserveQuiesceDrain();
	state->callback_active = true;
	state->callback_drain_started = false;
	state->callback_owner = std::thread::id();
	return true;
}

bool WtArbiExecuter::prependUnitEvent(const std::shared_ptr<UnitLifecycleState>& state,
	const UnitLifecycleState::Event& event)
{
	if (state == NULL)
		return false;
	std::lock_guard<std::mutex> lock(state->mutex);
	if (state->callback_failed)
		return false;
	UnitLifecycleState::Event queued = event;
	if (queued.init_generation == 0)
		queued.init_generation = state->init_generation;
	state->events.push_front(std::move(queued));
	if (state->callback_active)
		return false;
	reserveQuiesceDrain();
	state->callback_active = true;
	state->callback_drain_started = false;
	state->callback_owner = std::thread::id();
	return true;
}

void WtArbiExecuter::kickUnitEventDrain(const ExecuteUnitPtr& unit,
	const std::shared_ptr<UnitLifecycleState>& state, bool shouldDrain)
{
	// Without a pool, the caller that starts a drain remains synchronous. A
	// caller that finds an active drain only enqueues and returns, so callbacks
	// may safely join helper threads that submit same-code events.
	if (!shouldDrain || unit == NULL || state == NULL)
		return;
	auto drain = [this, unit, state]() { drainUnitEvents(unit, state, true); };
	if (_pool)
	{
		// Reservation was acquired before scheduling, so a rejected task's
		// synchronous fallback is still covered by ingress and drain accounting.
		WtExecuterSchedule::scheduleOrRunFallback(*_pool, drain, drain);
	}
	else
		drain();
}

void WtArbiExecuter::enqueueUnitEvent(const ExecuteUnitPtr& unit,
	const std::shared_ptr<UnitLifecycleState>& state,
	const std::function<void(ExecuteUnit*)>& callback)
{
	UnitLifecycleState::Event event;
	event.callback = callback;
	const bool shouldDrain = appendUnitEvent(state, event);
	kickUnitEventDrain(unit, state, shouldDrain);
}

void WtArbiExecuter::enqueueStateEvent(const std::shared_ptr<UnitLifecycleState>& state,
	const std::function<void(ExecuteUnit*)>& callback)
{
	if (state == NULL)
		return;
	ExecuteUnitPtr unit;
	bool shouldDrain = false;
	{
		SpinLock lock(_mtx_units);
		auto stateIt = _unit_states.find(state->code);
		if (stateIt == _unit_states.end() || stateIt->second != state)
			return;
		auto unitIt = _unit_map.find(state->code);
		if (unitIt != _unit_map.end())
			unit = unitIt->second;
		UnitLifecycleState::Event event;
		event.callback = callback;
		shouldDrain = appendUnitEvent(state, event);
	}
	// If creation has not published the unit yet, the event remains on this
	// generation's queue and its creator starts the drain after registration.
	if (unit != NULL)
		kickUnitEventDrain(unit, state, shouldDrain);
}

void WtArbiExecuter::drainUnitEvents(const ExecuteUnitPtr& unit,
	const std::shared_ptr<UnitLifecycleState>& state, bool reserved)
{
	if (unit == NULL || state == NULL)
		return;
	bool claimedDrain = false;
	{
		std::unique_lock<std::mutex> lock(state->mutex);
		if (reserved)
		{
			const std::thread::id current = std::this_thread::get_id();
			if (!state->callback_active || state->callback_drain_started ||
				(state->callback_owner != std::thread::id() && state->callback_owner != current))
				return;
			// The first reserved caller claims an unbound scheduler reservation.
			// Creation-time reservations already name their sole permitted owner.
			state->callback_drain_started = true;
			state->callback_owner = current;
			claimedDrain = true;
		}
		else
		{
			while (state->callback_active)
			{
				if (state->callback_owner == std::this_thread::get_id())
					return;
				state->condition.wait(lock, [&state]() { return !state->callback_active; });
			}
			if (state->events.empty() || state->callback_failed)
				return;
			state->callback_active = true;
			state->callback_drain_started = true;
			state->callback_owner = std::this_thread::get_id();
			reserveQuiesceDrain();
			claimedDrain = true;
		}
	}
	if (!claimedDrain)
		return;
	QuiesceDrainScope quiesceDrain(this);

	try
	{
	for (;;)
	{
		UnitLifecycleState::Event event;
		{
			std::unique_lock<std::mutex> lock(state->mutex);
			if (state->callback_failed || state->events.empty())
			{
				state->callback_active = false;
				state->callback_drain_started = false;
				state->callback_owner = std::thread::id();
				state->condition.notify_all();
				return;
			}
			event = std::move(state->events.front());
			state->events.pop_front();
		}

		bool staleGeneration = event.init_generation != state->init_generation;
		bool staleRecovery = false;
		bool staleTarget = false;
		{
			SpinLock lock(_mtx_units);
			auto stateIt = _unit_states.find(state->code);
			auto unitIt = _unit_map.find(state->code);
			staleGeneration = staleGeneration || stateIt == _unit_states.end() ||
				stateIt->second != state || unitIt == _unit_map.end() || unitIt->second != unit;
			if (event.channel_ready)
				staleRecovery = event.channel_epoch != _channel_epoch;
		}
		if (staleGeneration)
			staleRecovery = true;
		// A ready callback is a recovery notification. If the channel changed
		// again before it ran, skip that stale recovery; queued lost/next-ready
		// events still run in FIFO order.
		if (!staleGeneration && event.target_update)
		{
			std::lock_guard<std::mutex> lock(_mtx_positions);
			auto latest = _target_update_sequences.find(state->code);
			staleTarget = _latest_position_batch_sequence > event.position_sequence ||
				(latest != _target_update_sequences.end() &&
					latest->second > event.position_sequence);
		}

		std::string callbackError;
		try
		{
			if (!staleRecovery && !staleTarget && event.callback)
				event.callback(unit->self());
		}
		catch (const std::exception& ex)
		{
			callbackError = ex.what() ? ex.what() : "empty exception message";
			if (callbackError.empty())
				callbackError = "empty exception message";
		}
		catch (...)
		{
			callbackError = "non-standard exception";
		}
		if (!callbackError.empty())
		{
			failUnitDrain(unit, state, callbackError.c_str());
			return;
		}
	}
	}
	catch (const std::exception& ex)
	{
		failUnitDrain(unit, state, ex.what());
	}
	catch (...)
	{
		failUnitDrain(unit, state, "unexpected exception while draining execution-unit events");
	}
}

std::shared_ptr<WtArbiExecuter::UnitLifecycleState> WtArbiExecuter::getUnitState(const char* stdCode)
{
	if (stdCode == NULL || stdCode[0] == '\0')
		return std::shared_ptr<UnitLifecycleState>();
	SpinLock lock(_mtx_units);
	auto it = _unit_states.find(stdCode);
	return it == _unit_states.end() ? std::shared_ptr<UnitLifecycleState>() : it->second;
}


//////////////////////////////////////////////////////////////////////////
//ExecuteContext
#pragma region Context回调接口

/*! 
 * \brief 获取Tick切片数据
 * \details 从数据管理器获取指定合约的Tick切片数据
 * \param stdCode 标准合约代码
 * \param count 请求的Tick数量
 * \param etime 结束时间，默认为0表示当前
 * \return Tick切片数据指针
 */
WTSTickSlice* WtArbiExecuter::getTicks(const char* stdCode, uint32_t count, uint64_t /*etime*/ /* = 0 */)
{
	if (_data_mgr == NULL)
		return NULL;

	return _data_mgr->get_tick_slice(stdCode, count);
}

/*! 
 * \brief 获取最新Tick数据
 * \details 从数据管理器中获取指定合约的最新Tick数据
 * \param stdCode 标准合约代码
 * \return 最新Tick数据指针
 */
WTSTickData* WtArbiExecuter::grabLastTick(const char* stdCode)
{
	if (_data_mgr == NULL)
		return NULL;

	return _data_mgr->grab_last_tick(stdCode);
}

/*! 
 * \brief 获取持仓量
 * \details 从交易适配器中获取指定合约的持仓量
 * \param stdCode 标准合约代码
 * \param validOnly 是否只计算有效仓位，默认为true
 * \param flag 仓位标记，默认为3
 * \return 持仓数量
 */
double WtArbiExecuter::getPosition(const char* stdCode, bool validOnly /* = true */, int32_t flag /* = 3 */)
{
	if (NULL == _trader)
		return 0.0;

	return _trader->getPosition(stdCode, validOnly, flag);
}

/*! 
 * \brief 获取未完成数量
 * \details 从交易适配器中获取指定合约的未完成委托数量
 * \param stdCode 标准合约代码
 * \return 未完成委托数量
 */
double WtArbiExecuter::getUndoneQty(const char* stdCode)
{
	if (NULL == _trader)
		return 0.0;

	return _trader->getUndoneQty(stdCode);
}

/*! 
 * \brief 获取委托映射
 * \details 从交易适配器中获取指定合约的委托映射
 * \param stdCode 标准合约代码
 * \return 委托映射指针
 */
OrderMap* WtArbiExecuter::getOrders(const char* stdCode)
{
	if (NULL == _trader)
		return NULL;

	return _trader->getOrders(stdCode);
}

/*!
 * \brief 买入委托
 * \details 发出买入委托，需要交易通道就绪
 * \param stdCode 标准合约代码
 * \param price 委托价格
 * \param qty 委托数量
 * \param bForceClose 是否强制平仓，默认为false
 * \return 委托ID列表
 */
OrderIDs WtArbiExecuter::buy(const char* stdCode, double price, double qty, bool bForceClose/* = false*/)
{
	if (!_channel_ready)
		return OrderIDs();

	return _trader->buy(stdCode, price, qty, 0, bForceClose);
}

/*!
 * \brief 卖出委托
 * \details 发出卖出委托，需要交易通道就绪
 * \param stdCode 标准合约代码
 * \param price 委托价格
 * \param qty 委托数量
 * \param bForceClose 是否强制平仓，默认为false
 * \return 委托ID列表
 */
OrderIDs WtArbiExecuter::sell(const char* stdCode, double price, double qty, bool bForceClose/* = false*/)
{
	if (!_channel_ready)
		return OrderIDs();

	return _trader->sell(stdCode, price, qty, 0, bForceClose);
}

/*!
 * \brief 撤销委托
 * \details 根据委托ID撤销指定委托，需要交易通道就绪
 * \param localid 本地委托ID
 * \return 是否撤销成功
 */
bool WtArbiExecuter::cancel(uint32_t localid)
{
	if (!_channel_ready)
		return false;

	return _trader->cancel(localid);
}

/*!
 * \brief 撤销委托
 * \details 根据合约代码、方向和数量撤销委托，需要交易通道就绪
 * \param stdCode 标准合约代码
 * \param isBuy 是否买入方向
 * \param qty 要撤销的数量
 * \return 撤销的委托ID列表
 */
OrderIDs WtArbiExecuter::cancel(const char* stdCode, bool isBuy, double qty)
{
	if (!_channel_ready)
		return OrderIDs();

	return _trader->cancel(stdCode, isBuy, qty);
}

/*!
 * \brief 输出日志
 * \details 输出日志信息，并添加执行器名称前缀
 * \param message 日志消息
 */
void WtArbiExecuter::writeLog(const char* message)
{
	static thread_local char szBuf[MAX_LOG_BUF_SIZE] = { 0 };
	// 固定缓冲区最多保留 MAX_LOG_BUF_SIZE - 1 字节，并始终保留 NUL 终止符。
	// 消息（以及极长名称）超出容量时由 snprintf 确定性截断。
	std::snprintf(szBuf, sizeof(szBuf), "[%s]%s", _name.c_str(), message ? message : "");
	WTSLogger::log_dyn_raw("executer", _name.c_str(), LL_INFO, szBuf);
}

/*!
 * \brief 获取商品信息
 * \details 从执行器绑定的执行核心中获取商品信息
 * \param stdCode 标准合约代码
 * \return 商品信息指针
 */
WTSCommodityInfo* WtArbiExecuter::getCommodityInfo(const char* stdCode)
{
	return _stub->get_comm_info(stdCode);
}

/*!
 * \brief 获取交易时段信息
 * \details 从执行器绑定的执行核心中获取交易时段信息
 * \param stdCode 标准合约代码
 * \return 交易时段信息指针
 */
WTSSessionInfo* WtArbiExecuter::getSessionInfo(const char* stdCode)
{
	return _stub->get_sess_info(stdCode);
}

/*!
 * \brief 获取当前时间
 * \details 从执行器绑定的执行核心中获取当前时间
 * \return 当前时间戳
 */
uint64_t WtArbiExecuter::getCurTime()
{
	return _stub->get_real_time();
	//return TimeUtils::makeTime(_stub->get_date(), _stub->get_raw_time() * 100000 + _stub->get_secs());
}

#pragma endregion Context回调接口
//ExecuteContext
//////////////////////////////////////////////////////////////////////////


#pragma region 外部接口
/*!
 * \brief 设置目标仓位
 * \details 根据传入的目标仓位映射表设置合约的目标仓位，并自动处理交易逻辑
 * \param targets 目标仓位映射表，键为标准合约代码，值为目标仓位
 */
void WtArbiExecuter::set_position(const wt_hashmap<std::string, double>& targets)
{
	QuiesceIngress ingress(this);
	if (!ingress)
		return;
	set_position_with_sequence(targets, beginPositionBatch());
}

uint64_t WtArbiExecuter::beginPositionBatch()
{
	std::lock_guard<std::mutex> positionLock(_mtx_positions);
	const uint64_t sequence = _next_position_sequence++;
	_latest_position_batch_sequence = sequence;
	return sequence;
}

void WtArbiExecuter::set_position_with_sequence(
	const wt_hashmap<std::string, double>& targets, uint64_t batchSequence)
{
	/*
	 *	先要把目标头寸进行组合匹配
	 */
	auto real_targets = targets;
	for(auto& v : _groups)
	{
		const CodeGroupPtr& gpInfo = v.second;
		bool bHit = false;
		double gpQty = DBL_MAX;
		for(auto& vi : gpInfo->_items)
		{
			double unit = vi.second;
			auto it = real_targets.find(vi.first);
			if (it == real_targets.end())
			{
				bHit = false;
				break;
			}
			else
			{
				bHit = true;
				//计算最小的组合单位数量
				gpQty = std::min(gpQty, decimal::mod(it->second, unit));
			}
		}

		if(bHit && decimal::gt(gpQty, 0))
		{
			real_targets[gpInfo->_name] = gpQty;
			for (auto& vi : gpInfo->_items)
			{
				double unit = vi.second;
				real_targets[vi.first] -= gpQty * unit;
			}
		}
	}


	bool superseded = false;
	for (auto it = targets.begin(); it != targets.end(); it++)
	{
		const char* stdCode = it->first.c_str();
		double newVol = it->second;
		// 账户的理论持仓要经过修正
		const double traderTarget = round(newVol * _scale);
		const bool orderAllowed = !_trader || _trader->checkOrderLimits(stdCode);
		double oldVol = 0;
		ExecuteUnitPtr unit;
		std::shared_ptr<UnitLifecycleState> state;
		bool shouldDrain = false;
		bool targetQueued = false;
		{
			std::lock_guard<std::mutex> positionLock(_mtx_positions);
			if (_latest_position_batch_sequence > batchSequence)
			{
				superseded = true;
				break;
			}
			auto latest = _target_update_sequences.find(stdCode);
			if (latest != _target_update_sequences.end() && latest->second > batchSequence)
				continue;
			_target_update_sequences[stdCode] = batchSequence;
			oldVol = _target_pos[stdCode];
			_target_pos[stdCode] = newVol;
			_target_dispatch_allowed[stdCode] = orderAllowed;
			if (orderAllowed)
			{
				SpinLock unitsLock(_mtx_units);
				auto unitIt = _unit_map.find(stdCode);
				auto stateIt = _unit_states.find(stdCode);
				if (unitIt != _unit_map.end() && stateIt != _unit_states.end())
				{
					unit = unitIt->second;
					state = stateIt->second;
					uint64_t initialTargetSequence = 0;
					{
						std::lock_guard<std::mutex> stateLock(state->mutex);
						initialTargetSequence = state->initial_target_sequence;
					}
					if (initialTargetSequence >= batchSequence)
						targetQueued = true;
					else
					{
						UnitLifecycleState::Event event;
						event.callback = [code = std::string(stdCode), traderTarget](ExecuteUnit* target) {
							target->set_position(code.c_str(), traderTarget);
						};
						event.target_update = true;
						event.position_sequence = batchSequence;
						shouldDrain = appendUnitEvent(state, event);
						targetQueued = true;
					}
				}
			}
		}

		if(!decimal::eq(oldVol, newVol))
		{
			WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "Target position of {} changed: {} -> {} : {} with scale{}", stdCode, oldVol, newVol, traderTarget, _scale);
		}

		if (!orderAllowed)
		{
			WTSLogger::log_dyn("executer", _name.c_str(), LL_WARN, "{} is disabled due to entrust limit control ", stdCode);
			continue;
		}

		if (unit == NULL)
			unit = getUnit(stdCode);
		if (unit == NULL)
			continue;
		if (state == NULL)
			state = getUnitState(stdCode);
		if (unit == NULL || state == NULL)
			continue;
		if (!targetQueued)
		{
			uint64_t initialTargetSequence = 0;
			{
				std::lock_guard<std::mutex> stateLock(state->mutex);
				initialTargetSequence = state->initial_target_sequence;
			}
			std::lock_guard<std::mutex> positionLock(_mtx_positions);
			auto latest = _target_update_sequences.find(stdCode);
			if (_latest_position_batch_sequence > batchSequence ||
				(latest != _target_update_sequences.end() && latest->second > batchSequence) ||
				initialTargetSequence >= batchSequence)
				continue;
			UnitLifecycleState::Event event;
			event.callback = [code = std::string(stdCode), traderTarget](ExecuteUnit* target) {
				target->set_position(code.c_str(), traderTarget);
			};
			event.target_update = true;
			event.position_sequence = batchSequence;
			shouldDrain = appendUnitEvent(state, event);
		}
		kickUnitEventDrain(unit, state, shouldDrain);
	}
	if (superseded)
		return;

	//在原来的目标头寸中，但是不在新的目标头寸中，则需要自动设置为0
	clearMissingTargets(snapshotMissingTargets(targets), targets, batchSequence);

	//如果开启了严格同步，则需要检查通道持仓
	//如果通道持仓不在管理中，则直接平掉
	if(_strict_sync)
		clearUnmanagedChannelHolds(snapshotUnmanagedChannelHolds(), batchSequence);
}

std::vector<std::string> WtArbiExecuter::snapshotMissingTargets(
	const wt_hashmap<std::string, double>& targets)
	{
	std::vector<std::string> oldCodes;
	std::lock_guard<std::mutex> positionLock(_mtx_positions);
	for (auto it = _target_pos.begin(); it != _target_pos.end(); ++it)
	{
		if (targets.find(it->first) == targets.end())
			oldCodes.push_back(it->first);
	}
	return oldCodes;
}

void WtArbiExecuter::clearMissingTargets(const std::vector<std::string>& codes,
	const wt_hashmap<std::string, double>& targets, uint64_t batchSequence)
{
	for (const std::string& oldCode : codes)
	{
		const char* code = oldCode.c_str();
		ExecuteUnitPtr unit;
		std::shared_ptr<UnitLifecycleState> state;
		bool shouldDrain = false;
		bool targetQueued = false;
		{
			std::lock_guard<std::mutex> positionLock(_mtx_positions);
			if (_latest_position_batch_sequence > batchSequence)
				return;
			auto pos = _target_pos.find(oldCode);
			if (pos == _target_pos.end() || targets.find(oldCode) != targets.end())
				continue;
			auto latest = _target_update_sequences.find(oldCode);
			if (latest != _target_update_sequences.end() && latest->second > batchSequence)
				continue;
			_target_update_sequences[oldCode] = batchSequence;
			pos->second = 0;
			_target_dispatch_allowed[oldCode] = true;
			SpinLock unitsLock(_mtx_units);
			auto unitIt = _unit_map.find(oldCode);
			auto stateIt = _unit_states.find(oldCode);
			if (unitIt != _unit_map.end() && stateIt != _unit_states.end())
			{
				unit = unitIt->second;
				state = stateIt->second;
				uint64_t initialTargetSequence = 0;
				{
					std::lock_guard<std::mutex> stateLock(state->mutex);
					initialTargetSequence = state->initial_target_sequence;
				}
				if (initialTargetSequence >= batchSequence)
					targetQueued = true;
				else
				{
					UnitLifecycleState::Event event;
					event.callback = [oldCode](ExecuteUnit* target) {
						target->set_position(oldCode.c_str(), 0);
					};
					event.target_update = true;
					event.position_sequence = batchSequence;
					shouldDrain = appendUnitEvent(state, event);
					targetQueued = true;
				}
			}
		}
		if (unit == NULL)
			unit = getUnit(code);
		if (unit == NULL)
			continue;
		if (state == NULL)
			state = getUnitState(code);
		if (state == NULL)
			continue;
		if (!targetQueued)
		{
			uint64_t initialTargetSequence = 0;
			{
				std::lock_guard<std::mutex> stateLock(state->mutex);
				initialTargetSequence = state->initial_target_sequence;
			}
			std::lock_guard<std::mutex> positionLock(_mtx_positions);
			auto pos = _target_pos.find(oldCode);
			auto latest = _target_update_sequences.find(oldCode);
			if (_latest_position_batch_sequence > batchSequence || pos == _target_pos.end() ||
				pos->second != 0 || targets.find(oldCode) != targets.end() ||
				(latest != _target_update_sequences.end() && latest->second > batchSequence) ||
				initialTargetSequence >= batchSequence)
				continue;
			UnitLifecycleState::Event event;
			event.callback = [oldCode](ExecuteUnit* target) { target->set_position(oldCode.c_str(), 0); };
			event.target_update = true;
			event.position_sequence = batchSequence;
			shouldDrain = appendUnitEvent(state, event);
		}
		WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "{} is not in target, set to 0 automatically", code);
		kickUnitEventDrain(unit, state, shouldDrain);
	}
}

std::vector<std::string> WtArbiExecuter::snapshotUnmanagedChannelHolds()
{
	std::vector<std::string> unmanagedHolds;
	std::lock_guard<std::mutex> positionLock(_mtx_positions);
	for (const std::string& stdCode : _channel_holds)
	{
		if (_target_pos.find(stdCode) == _target_pos.end())
			unmanagedHolds.push_back(stdCode);
	}
	return unmanagedHolds;
}

void WtArbiExecuter::clearUnmanagedChannelHolds(const std::vector<std::string>& codes,
	uint64_t batchSequence)
{
	for (const std::string& stdCode : codes)
	{
		ExecuteUnitPtr unit;
		std::shared_ptr<UnitLifecycleState> state;
		bool shouldDrain = false;
		bool targetQueued = false;
		{
			std::lock_guard<std::mutex> positionLock(_mtx_positions);
			if (_latest_position_batch_sequence > batchSequence)
				return;
			if (_target_pos.find(stdCode) != _target_pos.end())
				continue;
			auto latest = _target_update_sequences.find(stdCode);
			if (latest != _target_update_sequences.end() && latest->second > batchSequence)
				continue;
			_target_update_sequences[stdCode] = batchSequence;
			_target_pos[stdCode] = 0;
			_target_dispatch_allowed[stdCode] = true;
			SpinLock unitsLock(_mtx_units);
			auto unitIt = _unit_map.find(stdCode);
			auto stateIt = _unit_states.find(stdCode);
			if (unitIt != _unit_map.end() && stateIt != _unit_states.end())
			{
				unit = unitIt->second;
				state = stateIt->second;
				uint64_t initialTargetSequence = 0;
				{
					std::lock_guard<std::mutex> stateLock(state->mutex);
					initialTargetSequence = state->initial_target_sequence;
				}
				if (initialTargetSequence >= batchSequence)
					targetQueued = true;
				else
				{
					UnitLifecycleState::Event event;
					event.callback = [stdCode](ExecuteUnit* target) {
						target->set_position(stdCode.c_str(), 0);
					};
					event.target_update = true;
					event.position_sequence = batchSequence;
					shouldDrain = appendUnitEvent(state, event);
					targetQueued = true;
				}
			}
		}
		if (unit == NULL)
			unit = getUnit(stdCode.c_str());
		if (unit == NULL)
			continue;
		if (state == NULL)
			state = getUnitState(stdCode.c_str());
		if (state == NULL)
			continue;
		if (!targetQueued)
		{
			uint64_t initialTargetSequence = 0;
			{
				std::lock_guard<std::mutex> stateLock(state->mutex);
				initialTargetSequence = state->initial_target_sequence;
			}
			std::lock_guard<std::mutex> positionLock(_mtx_positions);
			if (_latest_position_batch_sequence > batchSequence ||
				_target_pos.find(stdCode) == _target_pos.end() ||
				_target_pos.find(stdCode)->second != 0 ||
				_target_update_sequences[stdCode] > batchSequence ||
				initialTargetSequence >= batchSequence)
				continue;
			UnitLifecycleState::Event event;
			event.callback = [stdCode](ExecuteUnit* target) {
				target->set_position(stdCode.c_str(), 0);
			};
			event.target_update = true;
			event.position_sequence = batchSequence;
			shouldDrain = appendUnitEvent(state, event);
		}
		WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO,
			"{} is not in management, set to 0 due to strict sync mode", stdCode.c_str());
		kickUnitEventDrain(unit, state, shouldDrain);
	}
}

/*!
 * \brief 处理实时行情
 * \details 接收并处理实时行情数据，将行情转发给相应执行单元
 * \param stdCode 标准合约代码
 * \param newTick 最新的行情数据
 */
void WtArbiExecuter::on_tick(const char* stdCode, WTSTickData* newTick)
{
	if (newTick == NULL)
		return;
	QuiesceIngress ingress(this);
	if (!ingress)
		return;
	std::shared_ptr<UnitLifecycleState> state = getUnitState(stdCode);
	if (state == NULL)
		return;
	ExecuteUnitPtr unit = getUnit(stdCode, false);
	newTick->retain();
	std::shared_ptr<WTSTickData> tickRef(newTick, [](WTSTickData* tick) { tick->release(); });
	const std::function<void(ExecuteUnit*)> callback = [tickRef](ExecuteUnit* target) {
		target->on_tick(tickRef.get());
	};
	if (unit != NULL)
		enqueueUnitEvent(unit, state, callback);
	else
		enqueueStateEvent(state, callback);
}

/*!
 * \brief 处理仓位变更
 * \details 接收仓位变更通知，并更新内部仓位缓存
 *          如果合约已存在于仓位缓存中，则将差值叠加到现有仓位
 *          如果合约不存在于仓位缓存中，则创建新的仓位记录
 *          最后记录日志，输出最新的仓位信息
 * \param stdCode 标准合约代码
 * \param diffPos 仓位变化量
 */
void WtArbiExecuter::on_position_changed(const char* stdCode, double diffPos)
{
	if (stdCode == NULL || stdCode[0] == '\0')
		return;
	QuiesceIngress ingress(this);
	if (!ingress)
		return;
	const std::string code(stdCode);
	const bool orderAllowed = !_trader || _trader->checkOrderLimits(stdCode);
	double oldVol = 0;
	double newVol = 0;
	uint64_t batchSequence = 0;
	ExecuteUnitPtr unit;
	std::shared_ptr<UnitLifecycleState> state;
	bool shouldDrain = false;
	bool targetQueued = false;
	{
		std::lock_guard<std::mutex> positionLock(_mtx_positions);
		batchSequence = _next_position_sequence++;
		_target_update_sequences[code] = batchSequence;
		oldVol = _target_pos[code];
		newVol = oldVol + diffPos;
		_target_pos[code] = newVol;
		_target_dispatch_allowed[code] = orderAllowed;
		if (orderAllowed)
		{
			SpinLock unitsLock(_mtx_units);
			auto unitIt = _unit_map.find(code);
			auto stateIt = _unit_states.find(code);
			if (unitIt != _unit_map.end() && stateIt != _unit_states.end())
			{
				unit = unitIt->second;
				state = stateIt->second;
				uint64_t initialTargetSequence = 0;
				{
					std::lock_guard<std::mutex> stateLock(state->mutex);
					initialTargetSequence = state->initial_target_sequence;
				}
				if (initialTargetSequence >= batchSequence)
					targetQueued = true;
				else
				{
					const double traderTarget = round(newVol * _scale);
					UnitLifecycleState::Event event;
					event.callback = [code, traderTarget](ExecuteUnit* target) {
						target->set_position(code.c_str(), traderTarget);
					};
					event.target_update = true;
					event.position_sequence = batchSequence;
					shouldDrain = appendUnitEvent(state, event);
					targetQueued = true;
				}
			}
		}
	}

	double traderTarget = round(newVol * _scale);

	if(!decimal::eq(diffPos, 0))
	{
		WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "Target position of {} changed: {} -> {} : {} with scale:{}", stdCode, oldVol, newVol, traderTarget, _scale);
	}

	if (!orderAllowed)
	{
		WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "{} is disabled", stdCode);
		return;
	}
	if (unit == NULL)
		unit = getUnit(stdCode);
	if (unit == NULL)
		return;
	if (state == NULL)
		state = getUnitState(stdCode);
	if (state == NULL)
		return;
	if (!targetQueued)
	{
		uint64_t initialTargetSequence = 0;
		{
			std::lock_guard<std::mutex> stateLock(state->mutex);
			initialTargetSequence = state->initial_target_sequence;
		}
		std::lock_guard<std::mutex> positionLock(_mtx_positions);
		auto latest = _target_update_sequences.find(code);
		if ((latest != _target_update_sequences.end() && latest->second > batchSequence) ||
			initialTargetSequence >= batchSequence)
			return;
		const double traderTarget = round(newVol * _scale);
		UnitLifecycleState::Event event;
		event.callback = [code, traderTarget](ExecuteUnit* target) {
			target->set_position(code.c_str(), traderTarget);
		};
		event.target_update = true;
		event.position_sequence = batchSequence;
		shouldDrain = appendUnitEvent(state, event);
	}

	kickUnitEventDrain(unit, state, shouldDrain);
}

/*!
 * \brief 处理成交回报
 * \details 接收并处理成交回报，将成交信息转发给相应的执行单元
 *          如果启用了线程池，则在独立线程中异步处理成交信息
 *          否则在当前线程中同步处理
 * \param localid 本地委托ID
 * \param stdCode 标准合约代码
 * \param isBuy 是否买入方向
 * \param vol 成交数量
 * \param price 成交价格
 */
void WtArbiExecuter::on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price)
{
	QuiesceIngress ingress(this);
	if (!ingress)
		return;
	std::shared_ptr<UnitLifecycleState> state = getUnitState(stdCode);
	if (state == NULL)
		return;
	ExecuteUnitPtr unit = getUnit(stdCode, false);
	const std::string code(stdCode);
	const std::function<void(ExecuteUnit*)> callback = [localid, code, isBuy, vol, price](ExecuteUnit* target) {
		target->on_trade(localid, code.c_str(), isBuy, vol, price);
	};
	if (unit != NULL)
		enqueueUnitEvent(unit, state, callback);
	else
		enqueueStateEvent(state, callback);
}

/*!
 * \brief 处理委托回报
 * \details 接收并处理委托回报，将委托信息转发给相应的执行单元
 *          如果启用了线程池，则在独立线程中异步处理委托信息
 *          否则在当前线程中同步处理
 * \param localid 本地委托ID
 * \param stdCode 标准合约代码
 * \param isBuy 是否买入方向
 * \param totalQty 总委托数量
 * \param leftQty 剩余委托数量
 * \param price 委托价格
 * \param isCanceled 是否已撤销，默认为false
 */
void WtArbiExecuter::on_order(uint32_t localid, const char* stdCode, bool isBuy, double /*totalQty*/, double leftQty, double price, bool isCanceled /* = false */)
{
	QuiesceIngress ingress(this);
	if (!ingress)
		return;
	std::shared_ptr<UnitLifecycleState> state = getUnitState(stdCode);
	if (state == NULL)
		return;
	ExecuteUnitPtr unit = getUnit(stdCode, false);
	const std::string code(stdCode);
	const std::function<void(ExecuteUnit*)> callback =
		[localid, code, isBuy, leftQty, price, isCanceled](ExecuteUnit* target) {
			target->on_order(localid, code.c_str(), isBuy, leftQty, price, isCanceled);
		};
	if (unit != NULL)
		enqueueUnitEvent(unit, state, callback);
	else
		enqueueStateEvent(state, callback);
}

/*!
 * \brief 处理委托下达回报
 * \details 接收并处理委托下达回报，将委托下达结果转发给相应的执行单元
 *          委托下达回报包含委托是否成功下达以及相关消息
 *          如果启用了线程池，则在独立线程中异步处理
 *          否则在当前线程中同步处理
 * \param localid 本地委托ID
 * \param stdCode 标准合约代码
 * \param bSuccess 委托是否成功下达
 * \param message 相关消息，如错误原因
 */
void WtArbiExecuter::on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message)
{
	QuiesceIngress ingress(this);
	if (!ingress)
		return;
	std::shared_ptr<UnitLifecycleState> state = getUnitState(stdCode);
	if (state == NULL)
		return;
	ExecuteUnitPtr unit = getUnit(stdCode, false);
	const std::string code(stdCode);
	const std::string msg = message ? message : "";
	const std::function<void(ExecuteUnit*)> callback =
		[localid, code, bSuccess, msg](ExecuteUnit* target) {
			target->on_entrust(localid, code.c_str(), bSuccess, msg.c_str());
		};
	if (unit != NULL)
		enqueueUnitEvent(unit, state, callback);
	else
		enqueueStateEvent(state, callback);
}

/*!
 * \brief 处理交易通道就绪事件
 * \details 当交易通道就绪时，更新内部通道就绪状态并通知所有执行单元
 *          通过循环遍历所有执行单元，将通道就绪事件转发给每一个执行单元
 *          如果启用了线程池，则在独立线程中异步处理
 *          否则在当前线程中同步处理
 */
void WtArbiExecuter::on_channel_ready()
{
	QuiesceIngress ingress(this);
	if (!ingress)
		return;
	std::vector<std::tuple<ExecuteUnitPtr, std::shared_ptr<UnitLifecycleState>, bool>> units;
	{
		SpinLock lock(_mtx_units);
		if (_channel_ready.exchange(true))
			return;
		++_channel_epoch;
		for (auto& item : _unit_states)
		{
			UnitLifecycleState::Event event;
			event.callback = [](ExecuteUnit* target) { target->on_channel_ready(); };
			event.channel_ready = true;
			event.channel_epoch = _channel_epoch;
			const bool shouldDrain = appendUnitEvent(item.second, event);
			auto unitIt = _unit_map.find(item.first);
			if (unitIt != _unit_map.end())
				units.emplace_back(unitIt->second, item.second, shouldDrain);
		}
	}
	for (const auto& entry : units)
		kickUnitEventDrain(std::get<0>(entry), std::get<1>(entry), std::get<2>(entry));
}

/*!
 * \brief 处理交易通道丢失事件
 * \details 当交易通道丢失时，更新内部通道状态并通知所有执行单元
 *          通过循环遍历所有执行单元，将通道丢失事件转发给每一个执行单元
 *          如果启用了线程池，则在独立线程中异步处理
 *          否则在当前线程中同步处理
 */
void WtArbiExecuter::on_channel_lost()
{
	QuiesceIngress ingress(this);
	if (!ingress)
		return;
	std::vector<std::tuple<ExecuteUnitPtr, std::shared_ptr<UnitLifecycleState>, bool>> units;
	{
		SpinLock lock(_mtx_units);
		if (!_channel_ready.exchange(false))
			return;
		++_channel_epoch;
		for (auto& item : _unit_states)
		{
			UnitLifecycleState::Event event;
			event.callback = [](ExecuteUnit* target) { target->on_channel_lost(); };
			event.channel_epoch = _channel_epoch;
			const bool shouldDrain = appendUnitEvent(item.second, event);
			auto unitIt = _unit_map.find(item.first);
			if (unitIt != _unit_map.end())
				units.emplace_back(unitIt->second, item.second, shouldDrain);
		}
	}
	for (const auto& entry : units)
		kickUnitEventDrain(std::get<0>(entry), std::get<1>(entry), std::get<2>(entry));
}

/*!
 * \brief 处理账户资金更新
 * \details 接收并处理账户资金更新通知，将资金信息转发给所有的执行单元
 *          通过循环遍历所有执行单元，将账户资金信息转发给每一个执行单元
 *          如果启用了线程池，则在独立线程中异步处理
 *          否则在当前线程中同步处理
 * \param currency 货币代码
 * \param prebalance 前一交易日结算后的账户余额
 * \param balance 当前结算后的账户余额
 * \param dynbalance 动态权益（包含浮动盈亏）
 * \param avaliable 可用资金
 * \param closeprofit 平仓盈亏
 * \param dynprofit 浮动盈亏
 * \param margin 保证金
 * \param fee 手续费
 * \param deposit 入金
 * \param withdraw 出金
 */
void WtArbiExecuter::on_account(const char* currency, double prebalance, double balance, double dynbalance, 
	double avaliable, double closeprofit, double dynprofit, double margin, double fee, double deposit, double withdraw)
{
	QuiesceIngress ingress(this);
	if (!ingress)
		return;
	std::vector<std::shared_ptr<UnitLifecycleState>> states;
	{
		SpinLock lock(_mtx_units);
		states.reserve(_unit_states.size());
		for (auto& item : _unit_states)
		{
			std::lock_guard<std::mutex> stateLock(item.second->mutex);
			if (!item.second->callback_failed &&
				(!item.second->creation_complete || item.second->init_status == ExecUnitInitState::READY))
				states.push_back(item.second);
		}
	}
	const std::string strCur = currency ? currency : "";
	for (const std::shared_ptr<UnitLifecycleState>& state : states)
	{
		enqueueStateEvent(state, [strCur, prebalance, balance, dynbalance, avaliable,
			closeprofit, dynprofit, margin, fee, deposit, withdraw](ExecuteUnit* target) {
			target->on_account(strCur.c_str(), prebalance, balance, dynbalance,
				avaliable, closeprofit, dynprofit, margin, fee, deposit, withdraw);
		});
	}
}

/*!
 * \brief 处理持仓更新通知
 * \details 接收交易账户持仓更新通知，处理两个主要功能：
 *          1. 记录交易通道持有的合约到集合中，用于后续严格同步功能
 *          2. 实现过期主力合约自动清理功能：
 *             - 首先检查是否启用了自动清理功能
 *             - 然后判断是否为分月期货合约
 *             - 确认当前合约是否为上一期的主力合约
 *             - 考虑排除列表和包含列表的设置
 *             - 最后执行自动清理操作
 * \param stdCode 标准合约代码
 * \param isLong 是否为多头持仓
 * \param prevol 之前的持仓量
 * \param preavail 之前的可用持仓量
 * \param newvol 新的持仓量
 * \param newavail 新的可用持仓量
 * \param tradingday 交易日
 */
void WtArbiExecuter::on_position(const char* stdCode, bool /*isLong*/, double /*prevol*/, double /*preavail*/, double /*newvol*/, double /*newavail*/, uint32_t tradingday)
{
	QuiesceIngress ingress(this);
	if (!ingress)
		return;
	{
		std::lock_guard<std::mutex> positionLock(_mtx_positions);
		_channel_holds.insert(stdCode);
	}

	/*
	 *	By Wesley @ 2021.12.14
	 *	先检查自动清理过期主力合约的标记是否为true
	 *	如果不为true，则直接退出该逻辑
	 */
	if (!_auto_clear)
		return;

	//如果不是分月期货合约，直接退出
	if (!CodeHelper::isStdMonthlyFutCode(stdCode))
		return;

	IHotMgr* hotMgr = _stub->get_hot_mon();
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, NULL);
	//获取上一期的主力合约
	std::string prevCode = hotMgr->getPrevRawCode(cInfo._exchg, cInfo._product, tradingday);

	//如果当前合约不是上一期的主力合约，则直接退出
	if (prevCode != cInfo._code)
		return;

	WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "Prev hot contract of {}.{} on {} is {}", cInfo._exchg, cInfo._product, tradingday, prevCode);

	thread_local static char fullPid[64] = { 0 };
	fmtutil::format_to(fullPid, "{}.{}", cInfo._exchg, cInfo._product);

	//先检查排除列表
	//如果在排除列表中，则直接退出
	auto it = _clear_excludes.find(fullPid);
	if(it != _clear_excludes.end())
	{
		WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "Position of {}, as prev hot contract, won't be cleared for it's in exclude list", stdCode);
		return;
	}

	//如果包含列表不为空，再检查是否在包含列表中
	//如果为空，则全部清理，不再进入该逻辑
	if(!_clear_includes.empty())
	{
		it = _clear_includes.find(fullPid);
		if (it == _clear_includes.end())
		{
			WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "Position of {}, as prev hot contract, won't be cleared for it's not in include list", stdCode);
			return;
		}
	}

	//最后再进行自动清理
	WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "Position of {}, as prev hot contract, will be cleared", stdCode);
	std::shared_ptr<UnitLifecycleState> state = getUnitState(stdCode);
	ExecuteUnitPtr unit = getUnit(stdCode);
	if (state == NULL)
		state = getUnitState(stdCode);
	if (state == NULL)
		return;
	const std::string code(stdCode);
	const std::function<void(ExecuteUnit*)> callback = [code](ExecuteUnit* target) {
		target->clear_all_position(code.c_str());
	};
	if (unit != NULL)
		enqueueUnitEvent(unit, state, callback);
	else
		enqueueStateEvent(state, callback);
}

#pragma endregion 外部接口
