/*!
 * \file WtExecuter.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 *
 * \brief 本地执行器实现文件
 * \details 实现了本地执行器类，用于处理交易指令的执行、仓位管理和交易回报等
 */
#include "WtLocalExecuter.h"
#include "WtExecuterSchedule.h"
#include "ExecPolicyConfig.h"
#include "TraderAdapter.h"
#include "WtEngine.h"

#include "../Share/CodeHelper.hpp"
#include "../Includes/IDataManager.h"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/IHotMgr.h"
#include "../Includes/WTSContractInfo.hpp"
#include "../Share/decimal.h"

#include "../WTSTools/WTSLogger.h"

#include <cmath>
#include <cstdio>
#include <locale>
#include <sstream>
#include <exception>
#include <iterator>
#include <utility>
#include <vector>

USING_NS_WTP;

namespace
{

bool readExecutionScale(WTSVariant* params, double& result)
{
	WTSVariant* value = params->get("scale");
	if (value == NULL) return false;
	switch (value->type())
	{
	case WTSVariant::VT_Int32:
	case WTSVariant::VT_Uint32:
	case WTSVariant::VT_Int64:
	case WTSVariant::VT_Uint64:
	case WTSVariant::VT_Real:
		result = value->asDouble();
		break;
	case WTSVariant::VT_String:
	{
		std::istringstream input(value->asString());
		input.imbue(std::locale::classic());
		input >> result;
		// libc++ may set failbit for a representable subnormal while still
		// consuming the complete token and assigning the correct value.
		const bool subnormal = input.fail() && input.eof() &&
			std::fpclassify(result) == FP_SUBNORMAL;
		if (input.bad() || (input.fail() && !subnormal)) return false;
		if (subnormal) input.clear(input.rdstate() & ~std::ios::failbit);
		input >> std::ws;
		if (!input.eof()) return false;
		break;
	}
	default:
		return false;
	}
	return std::isfinite(result) && result > 0.0;
}
const std::size_t kLocalExecutorThreadContextDepth = 64;
const uint64_t kLocalIngressClosedBit = uint64_t(1) << 63;
const uint64_t kLocalIngressCountMask = ~kLocalIngressClosedBit;
thread_local const WtLocalExecuter* g_localExecutorIngress[kLocalExecutorThreadContextDepth] = {};
thread_local std::size_t g_localExecutorIngressDepth = 0;
thread_local std::size_t g_localExecutorIngressOverflow = 0;
thread_local const WtLocalExecuter* g_localExecutorCallbacks[kLocalExecutorThreadContextDepth] = {};
thread_local std::size_t g_localExecutorCallbackDepth = 0;
thread_local std::size_t g_localExecutorCallbackOverflow = 0;

bool pushLocalExecutorThreadContext(const WtLocalExecuter* executor,
	const WtLocalExecuter** stack, std::size_t& depth, std::size_t& overflow)
{
	if (depth == kLocalExecutorThreadContextDepth)
	{
		++overflow;
		return false;
	}
	stack[depth++] = executor;
	return true;
}

void popLocalExecutorThreadContext(const WtLocalExecuter* stack[], std::size_t& depth,
	std::size_t& overflow, bool registered)
{
	if (!registered)
	{
		if (overflow != 0)
			--overflow;
		return;
	}
	if (depth != 0)
		stack[--depth] = NULL;
}

bool containsLocalExecutorThreadContext(const WtLocalExecuter* executor,
	const WtLocalExecuter* const stack[], std::size_t depth, std::size_t overflow)
{
	if (overflow != 0)
		return true; // Conservatively reject waits while any frame is untracked.
	for (std::size_t i = 0; i < depth; ++i)
	{
		if (stack[i] == executor)
			return true;
	}
	return false;
}

class LocalExecutorCallbackScope
{
public:
	explicit LocalExecutorCallbackScope(const WtLocalExecuter* executor,
		std::atomic<std::size_t>& activeCallbackDrains, std::condition_variable& condition)
		: _registered(pushLocalExecutorThreadContext(executor,
			g_localExecutorCallbacks, g_localExecutorCallbackDepth, g_localExecutorCallbackOverflow))
		, _active_callback_drains(activeCallbackDrains)
		, _condition(condition)
	{
		_active_callback_drains.fetch_add(1, std::memory_order_acq_rel);
	}

	~LocalExecutorCallbackScope()
	{
		popLocalExecutorThreadContext(g_localExecutorCallbacks, g_localExecutorCallbackDepth,
			g_localExecutorCallbackOverflow, _registered);
		_active_callback_drains.fetch_sub(1, std::memory_order_acq_rel);
		_condition.notify_all();
	}

	LocalExecutorCallbackScope(const LocalExecutorCallbackScope&) = delete;
	LocalExecutorCallbackScope& operator=(const LocalExecutorCallbackScope&) = delete;

private:
	bool _registered;
	std::atomic<std::size_t>& _active_callback_drains;
	std::condition_variable& _condition;
};
}

WtLocalExecuter::IngressLease::IngressLease(WtLocalExecuter* owner)
	: _owner(owner), _acquired(owner != NULL && owner->beginIngress()), _thread_marker_registered(false)
{
	if (_acquired)
		_thread_marker_registered = pushLocalExecutorThreadContext(_owner, g_localExecutorIngress,
			g_localExecutorIngressDepth, g_localExecutorIngressOverflow);
}

WtLocalExecuter::IngressLease::~IngressLease()
{
	if (!_acquired)
		return;
	popLocalExecutorThreadContext(g_localExecutorIngress, g_localExecutorIngressDepth,
		g_localExecutorIngressOverflow, _thread_marker_registered);
	_owner->endIngress();
}

bool WtLocalExecuter::beginIngress()
{
	uint64_t observed = _ingress_gate.load(std::memory_order_acquire);
	for (;;)
	{
		if ((observed & kLocalIngressClosedBit) != 0 ||
			(observed & kLocalIngressCountMask) == kLocalIngressCountMask)
			return false;
		if (_ingress_gate.compare_exchange_weak(observed, observed + 1,
			std::memory_order_acq_rel, std::memory_order_acquire))
			return true;
	}
}

void WtLocalExecuter::endIngress()
{
	_ingress_gate.fetch_sub(1, std::memory_order_acq_rel);
	std::lock_guard<std::mutex> lock(_quiesce_mutex);
	_quiesce_condition.notify_all();
}

bool WtLocalExecuter::ingressClosed() const
{
	return (_ingress_gate.load(std::memory_order_acquire) & kLocalIngressClosedBit) != 0;
}

uint64_t WtLocalExecuter::activeIngressCount() const
{
	return _ingress_gate.load(std::memory_order_acquire) & kLocalIngressCountMask;
}

void WtLocalExecuter::requestQuiesce()
{
	_ingress_gate.fetch_or(kLocalIngressClosedBit, std::memory_order_acq_rel);
	_quiesce_requested.store(true, std::memory_order_release);
}

bool WtLocalExecuter::canWaitQuiescedOnCurrentThread() const
{
	return preflightQuiesceOnCurrentThread();
}

WtExecuterFactory* WtLocalExecuter::quiesceFactory() const
{
	return _factory;
}

bool WtLocalExecuter::preflightQuiesceOnCurrentThread() const
{
	// This host preflight is checked globally before any manager blocks. Other
	// admitted ingress may drain later; only a wait from this same Local ingress
	// or callback stack can wait for itself.
	if (containsLocalExecutorThreadContext(this, g_localExecutorIngress,
		g_localExecutorIngressDepth, g_localExecutorIngressOverflow) ||
		containsLocalExecutorThreadContext(this, g_localExecutorCallbacks,
			g_localExecutorCallbackDepth, g_localExecutorCallbackOverflow))
		return false;
	return true;
}


bool WtLocalExecuter::drainHostQuiesced()
{
	if (containsLocalExecutorThreadContext(this, g_localExecutorIngress,
		g_localExecutorIngressDepth, g_localExecutorIngressOverflow) ||
		containsLocalExecutorThreadContext(this, g_localExecutorCallbacks,
			g_localExecutorCallbackDepth, g_localExecutorCallbackOverflow))
		return false;
	if (!ingressClosed() || !_quiesce_requested.load(std::memory_order_acquire))
		return false;
	{
		std::unique_lock<std::mutex> lock(_quiesce_mutex);
		if (_host_drained)
			return !_quiesce_incomplete.load(std::memory_order_acquire);
		if (_quiesce_wait_in_progress)
		{
			try
			{
				_quiesce_condition.wait(lock, [this]() {
					return _host_drained || !_quiesce_wait_in_progress;
				});
			}
			catch (...)
			{
				return false;
			}
			return _host_drained && !_quiesce_incomplete.load(std::memory_order_acquire);
		}
		_quiesce_wait_in_progress = true;
		try
		{
			_quiesce_condition.wait(lock, [this]() {
				return activeIngressCount() == 0;
			});
		}
		catch (...)
		{
			_quiesce_wait_in_progress = false;
			_quiesce_condition.notify_all();
			return false;
		}
	}

	// This stage owns only host queues and callback drains. It deliberately does
	// not call plugin lifecycle hooks.
	bool poolWaitSucceeded = true;
	try
	{
		if (_pool)
			_pool->wait();
	}
	catch (...)
	{
		poolWaitSucceeded = false;
	}
	const bool incomplete = _quiesce_incomplete.load(std::memory_order_acquire);
	const bool hostDrained = poolWaitSucceeded && activeIngressCount() == 0 &&
		_active_callback_drains.load(std::memory_order_acquire) == 0;

	{
		std::lock_guard<std::mutex> lock(_quiesce_mutex);
		_host_drained = hostDrained;
		_quiesce_wait_in_progress = false;
		_quiesce_condition.notify_all();
	}
	return hostDrained && !incomplete;
}

bool WtLocalExecuter::snapshotQuiescePluginOwners(std::vector<ExecQuiesceOwnerPtr>& out) const
{
	out.clear();
	if (!ingressClosed() || !_quiesce_requested.load(std::memory_order_acquire) ||
		_quiesce_incomplete.load(std::memory_order_acquire) ||
		activeIngressCount() != 0 ||
		_active_callback_drains.load(std::memory_order_acquire) != 0)
		return false;
	{
		std::lock_guard<std::mutex> lock(_quiesce_mutex);
		if (!_host_drained || _quiesce_wait_in_progress)
			return false;
	}

	std::vector<ExecuteUnitPtr> units;
	{
		SpinLock lock(_mtx_units);
		auto appendUnique = [&units](const ExecuteUnitPtr& unit) {
			if (unit == NULL)
				return;
			for (const ExecuteUnitPtr& existing : units)
				if (existing == unit)
					return;
			units.push_back(unit);
		};
		for (const auto& item : _unit_map)
			appendUnique(item.second);
		for (const ExecuteUnitPtr& unit : _retained_units)
			appendUnique(unit);
		for (const auto& item : _unit_states)
		{
			const std::shared_ptr<UnitLifecycleState>& state = item.second;
			std::lock_guard<std::mutex> stateLock(state->mutex);
			appendUnique(state->result);
			for (const UnitCallbackEvent& event : state->callback_events)
				appendUnique(event.unit);
		}
	}

	out.reserve(units.size());
	for (const ExecuteUnitPtr& unit : units)
		out.push_back(std::static_pointer_cast<IExecCommandQuiesce>(unit));
	return true;
}

bool WtLocalExecuter::completeQuiesceAfterGlobalPluginWait()
{
	std::unique_lock<std::mutex> lock(_quiesce_mutex, std::try_to_lock);
	if (!lock.owns_lock())
		return false;
	if (_quiesced)
		return true;
	if (!ingressClosed() || !_quiesce_requested.load(std::memory_order_acquire) || !_host_drained ||
		_quiesce_wait_in_progress || _quiesce_incomplete.load(std::memory_order_acquire) ||
		activeIngressCount() != 0 ||
		_active_callback_drains.load(std::memory_order_acquire) != 0)
		return false;
	_quiesced = true; // Called only after the global coordinator's plugin wait-all succeeds.
	_quiesce_condition.notify_all();
	return true;
}

bool WtLocalExecuter::waitQuiesced()
{
	if (!preflightQuiesceOnCurrentThread())
		return false;
	{
		std::lock_guard<std::mutex> lock(_quiesce_mutex);
		if (_quiesced)
			return true;
	}
	if (!drainHostQuiesced())
		return false;
	// Local owns plugin units and borrows their factory/context. Only the
	// global staged coordinator can prove the factory and plugin wait-all and
	// publish the acknowledgement that makes this legacy entry point succeed.
	return false;
}


/**
 * @brief 构造函数
 * @param factory 执行器工厂指针
 * @param name 执行器名称
 * @param dataMgr 数据管理器指针
 * @details 初始化执行器的基本参数，包括名称、工厂、数据管理器等
 */
WtLocalExecuter::WtLocalExecuter(WtExecuterFactory* factory, const char* name, IDataManager* dataMgr)
	: IExecCommand(name)
	, _trader(NULL)
, _factory(factory)
	, _data_mgr(dataMgr)
	, _config(NULL)
	, _scale(1.0)
	, _auto_clear(true)
	, _strict_sync(false)
	, _channel_ready(false)
	{
}


/**
 * @brief 析构函数
 * @details 仅在控制面确认 host 与全部插件 unit 已 quiesce 后销毁借用上下文
 */
WtLocalExecuter::~WtLocalExecuter()
{
	std::lock_guard<std::mutex> lock(_quiesce_mutex);
	if (!_quiesced)
		std::terminate();
}

/**
 * @brief 设置交易适配器
 * @param adapter 交易适配器指针
 * @details 设置交易适配器并检查交易通道是否就绪，更新内部通道状态
 */
void WtLocalExecuter::setTrader(TraderAdapter* adapter)
{
	IngressLease ingress(this);
	if (!ingress)
		return;
	_trader = adapter;
}

void WtLocalExecuter::onUnitCreationReserved(const std::string& /*code*/)
{
}

void WtLocalExecuter::onUnitGenerationRetainedForQuiesce(const std::string& /*code*/,
	uint64_t /*generation*/, const ExecuteUnitPtr& /*unit*/)
{
}

/**
 * @brief 初始化执行器
 * @param params 配置参数对象
 * @return 初始化是否成功
 * @details 根据配置参数初始化执行器，包括设置缩放比例、严格同步模式、创建线程池、
 * 读取自动清理策略和代码组等配置。这些配置将影响执行器的行为和性能。
 */
bool WtLocalExecuter::validateConfig(WTSVariant* params, std::string& error)
{
	if (params == NULL || params->type() != WTSVariant::VT_Object)
	{
		error = "executer configuration must be an object";
		return false;
	}
	double scale = 0.0;
	if (!readExecutionScale(params, scale))
	{
		error = "scale must be a finite positive number";
		return false;
	}
	execution_groups::Groups groupRules;
	std::string groupError;
	if (!execution_groups::parse(params, groupRules, groupError))
	{
		error = "invalid group configuration: " + groupError;
		return false;
	}
	return exec_policy_config::validateClear(params, error) &&
		exec_policy_config::validatePolicy(params, error);
}

bool WtLocalExecuter::init(WTSVariant* params)
{
	IngressLease ingress(this);
	if (!ingress)
		return false;
	if (params == NULL)
		return false;

	// Card 8b=A: validate the complete configuration (scale, groups, clear and
	// every policy entry) before retaining it or creating the thread pool.
	std::string configError;
	if (!validateConfig(params, configError))
	{
		WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR, "Invalid executer configuration: {}", configError);
		return false;
	}

	double scale = 0.0;
	if (!readExecutionScale(params, scale))
	{
		WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR, "Invalid execution scale");
		return false;
	}

	execution_groups::Groups groupRules;
	std::string groupError;
	if (!execution_groups::parse(params, groupRules, groupError))
	{
		WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR, "Invalid group configuration: {}", groupError);
		return false;
	}
	_group_rules.swap(groupRules);

	_config = params;
	_config->retain();

	// 设置缩放比例
	_scale = scale;
	// 设置是否严格同步
	_strict_sync  = params->getBoolean("strict_sync");

	// 创建线程池（如果配置了池大小）
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

	for (const execution_groups::Group& group : _group_rules)
	{
		CodeGroupPtr gpInfo(new CodeGroup);
		wt_strcpy(gpInfo->_name, group.name.c_str(), group.name.size());
		for (const auto& item : group.items)
		{
			gpInfo->_items[item.first] = item.second;
			_code_to_groups[item.first] = gpInfo;
		}
		_groups[group.name] = gpInfo;
	}

	WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "Local executer inited, scale: {}, auto_clear: {}, strict_sync: {}, thread poolsize: {}, code_groups: {}",
		_scale, _auto_clear, _strict_sync, poolsize, _groups.size());

	return true;
}

/**
 * @brief 获取执行单元
 * @param stdCode 标准合约代码
 * @param bAutoCreate 是否自动创建，默认为true
 * @return 执行单元是否可用
 * @details 根据标准合约代码获取相应的执行单元。如果已存在，直接返回；
 * 如果不存在且bAutoCreate为true，则创建新的执行单元并初始化。
 * 执行单元的策略配置来自配置文件中的policy部分，优先使用与商品ID匹配的策略，
 * 如果找不到对应策略，则使用default策略。
 */
bool WtLocalExecuter::getUnit(const char* stdCode, bool bAutoCreate /* = true */)
{
	if (stdCode == NULL || stdCode[0] == '\0')
	{
		WTSLogger::error("Cannot get execution unit for an empty code");
		return false;
	}

	const std::string code(stdCode);
	std::shared_ptr<UnitLifecycleState> state;
	bool registeredDuringCreation = false;
	bool creator = false;
	bool externalCodeEntered = false;
	bool unitInitialized = false;
	auto newState = std::make_shared<UnitLifecycleState>();
	{
		SpinLock lock(_mtx_units);
		auto pending = _unit_states.find(code);
		if (pending != _unit_states.end())
		{
			state = pending->second;
			std::lock_guard<std::mutex> stateLock(state->mutex);
			if (state->creation_complete && state->init_status == ExecUnitInitState::FAILED)
			{
				if (!bAutoCreate)
					return false;
				// An initialized generation may have emitted orders or consumed
				// reports before its callback failed. It remains fail-closed; a new
				// target does not establish a safe recovery snapshot. Factory,
				// initialization, and unit callbacks are third-party code too.
				if (state->runtime_failed || !state->callback_drain_quiesced)
					return false;
				state = newState;
				state->init_generation = _next_unit_init_generation++;
				state->creation_started = true;
				state->creation_owner = std::this_thread::get_id();
				pending->second = state;
				if (_channel_ready.load())
				{
					std::lock_guard<std::mutex> newStateLock(state->mutex);
					const uint64_t epoch = _channel_epoch.load();
					enqueueUnitCallbackLocked(state, ExecuteUnitPtr(),
						[](ExecuteUnit* target) { target->on_channel_ready(); }, true, epoch);
				}
				uint64_t clearRevision = 0;
				{
					std::lock_guard<std::mutex> positionLock(_position_state_mutex);
					auto clearIt = _pending_clear_position.find(code);
					if (clearIt != _pending_clear_position.end() && !clearIt->second.uncertain)
						clearRevision = clearIt->second.revision;
				}
				if (clearRevision != 0)
				{
					std::lock_guard<std::mutex> newStateLock(state->mutex);
					(void)enqueueClearPositionLocked(code, clearRevision, ExecuteUnitPtr(), state);
				}
				creator = true;
			}
			else if (!state->creation_complete && !state->creation_started && bAutoCreate)
			{
				state->creation_started = true;
				state->creation_owner = std::this_thread::get_id();
				creator = true;
			}
			else if (!state->creation_complete && !state->creation_started && !bAutoCreate)
			{
				return false;
			}
			else
			{
				auto mapped = _unit_map.find(code);
				if (mapped != _unit_map.end())
					registeredDuringCreation = true;
			}
		}
		else
		{
			auto it = _unit_map.find(code);
			if (it != _unit_map.end())
				return true;
			if (!bAutoCreate)
				return false;
			state = newState;
			state->init_generation = _next_unit_init_generation++;
			state->creation_started = true;
			state->creation_owner = std::this_thread::get_id();
			_unit_states.emplace(code, state);
			if (_channel_ready.load())
			{
				std::lock_guard<std::mutex> stateLock(state->mutex);
				const uint64_t epoch = _channel_epoch.load();
				enqueueUnitCallbackLocked(state, ExecuteUnitPtr(),
					[](ExecuteUnit* target) { target->on_channel_ready(); }, true, epoch);
			}
			creator = true;
		}
	}

	if (!creator)
	{
		std::unique_lock<std::mutex> lock(state->mutex);
		const std::thread::id current = std::this_thread::get_id();
		if (!state->creation_complete &&
			(state->creation_owner == current || (state->callback_active && state->callback_owner == current)))
		{
			// Initialization has succeeded and the candidate is already registered.
			// A same-thread callback may safely query it while channel notification is
			// still being drained; before registration, reject reentry to avoid deadlock.
			if (registeredDuringCreation)
				return true;
			lock.unlock();
			WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
				"Reentrant unit creation for {} was rejected", stdCode);
			return false;
		}
		// Only failures before entering third-party factory/plugin code can be
		// replaced. Runtime failures remain FAILED even after the old wrapper drains.
		const uint64_t waiting_generation = state->init_generation;
		++state->waiting_callers;
		state->condition.wait(lock, [&state]() { return state->creation_complete; });
		--state->waiting_callers;
		if (state->init_generation != waiting_generation)
			return false;
		return state->result != NULL && state->init_status == ExecUnitInitState::READY;
	}

	ExecuteUnitPtr unit;
	auto failCreation = [&](const char* reason) -> bool
	{
		// Fail closed after plugin entry unless ABI 2.2 proves that a normal
		// failed initialization was synchronously destroyed and this generation
		// has no accepted callback work to discard. A pure config preflight
		// failure with no queued work has no outstanding side effect to drain.
		ExecuteUnitPtr mappedUnit;
		std::deque<UnitCallbackEvent> discardedEvents;
		uint64_t failedRuntimeGeneration = 0;
		bool failedInitCleanupProven = false;
		{
			SpinLock lock(_mtx_units);
			auto unitIt = _unit_map.find(code);
			if (unitIt != _unit_map.end())
			{
				mappedUnit = unitIt->second;
				retainUnitLocked(mappedUnit);
				_unit_map.erase(unitIt);
			}
			std::lock_guard<std::mutex> stateLock(state->mutex);
			const bool acceptedHostCallbacksWillBeDiscarded =
				!state->callback_events.empty() || state->callback_active ||
				state->callback_drain_scheduled;
			// _target_pos and _pending_clear_position are authoritative business
			// state retained for a later retry. Only their accepted callback work
			// in this generation is considered discarded here.
			const bool noOtherUnitGeneration = mappedUnit == NULL && state->result == NULL;
			failedInitCleanupProven = externalCodeEntered && !unitInitialized && unit != NULL &&
				unit->failedInitializationWasSafelyDestroyed() &&
				!acceptedHostCallbacksWillBeDiscarded && !state->callback_failed &&
				noOtherUnitGeneration;
			if ((unitInitialized || (externalCodeEntered && unit != NULL)) &&
				!failedInitCleanupProven)
				retainUnitLocked(unit);
			if (state->result)
				retainUnitLocked(state->result);
			for (const UnitCallbackEvent& event : state->callback_events)
				if (event.unit)
					retainUnitLocked(event.unit);
			const bool runtimeFailure = unitInitialized ||
				(externalCodeEntered && !failedInitCleanupProven) ||
				acceptedHostCallbacksWillBeDiscarded || mappedUnit != NULL || state->result != NULL;
			if (runtimeFailure)
				_quiesce_incomplete.store(true, std::memory_order_release);
			state->callback_failed = true;
			state->runtime_failed = state->runtime_failed || runtimeFailure;
			discardedEvents.swap(state->callback_events);
			state->callback_drain_scheduled = false;
			state->target_reconcile_event_queued = false;
			state->clear_position_event_queued = false;
			if (state->runtime_failed)
			{
				state->callback_drain_quiesced = false;
				state->retired_generation = state->init_generation;
				failedRuntimeGeneration = state->init_generation;
			}
		}
		if ((unitInitialized || externalCodeEntered) && unit != NULL && !failedInitCleanupProven)
		{
			try { onUnitGenerationRetainedForQuiesce(code, state->init_generation, unit); }
			catch (...) {}
		}
		else if (mappedUnit != NULL)
		{
			try { onUnitGenerationRetainedForQuiesce(code, state->init_generation, mappedUnit); }
			catch (...) {}
		}
		// Any plugin unit moved into _retained_units above remains alive;
		// discarded callback references may now be released without destroying it.
		discardedEvents.clear();
		unit.reset();
		mappedUnit.reset(); // plugin destruction must happen outside the container lock
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			state->result.reset();
			state->init_status = ExecUnitInitState::FAILED;
			state->init_error = reason ? reason : "unknown execution-unit initialization failure";
			state->creation_complete = true;
			state->creation_owner = std::thread::id();
			state->callback_drain_quiesced = failedRuntimeGeneration == 0;
			state->callback_active = false;
			state->callback_owner = std::thread::id();
		}
		state->condition.notify_all();
		try
		{
			WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
				"Execution unit initialization for {} failed: {}", stdCode, reason ? reason : "unknown error");
		}
		catch (...)
		{
			// The reservation and waiters are already cleaned up; logging must not
			// reopen the failed creation path or strand another caller.
		}
		return false;
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

		// Once control enters the execution-unit factory, neither factory nor
		// plugin initialization has a no-side-effect contract. Any failure from
		// this point is retained as fail-closed and cannot be retried in-process.
		externalCodeEntered = true;
		unit = _factory->createExeUnit(name);
		if (unit == NULL)
			return failCreation("execution-unit creation failed");
		std::string error;
		unitInitialized = unit->initialize(this, stdCode, cfg, error);
		if (!unitInitialized || unit->self() == NULL)
			return failCreation(error.empty() ? "plugin initialization failed" : error.c_str());

		{
			SpinLock lock(_mtx_units);
			_unit_map[code] = unit;
			std::lock_guard<std::mutex> stateLock(state->mutex);
			bool hasTarget = false;
			{
				std::lock_guard<std::mutex> positionLock(_position_state_mutex);
				hasTarget = _target_pos.find(code) != _target_pos.end();
			}
			if (hasTarget)
				(void)enqueueTargetReconciliationLocked(code, unit, state);
			uint64_t clearRevision = 0;
			{
				std::lock_guard<std::mutex> positionLock(_position_state_mutex);
				auto clearIt = _pending_clear_position.find(code);
				if (clearIt != _pending_clear_position.end() && !clearIt->second.uncertain)
					clearRevision = clearIt->second.revision;
			}
			if (clearRevision != 0)
				(void)enqueueClearPositionLocked(code, clearRevision, unit, state);
		}

		for (;;)
		{
			drainUnitCallbacks(code, state);
			bool failed = false;
			bool completed = false;
			{
				SpinLock lock(_mtx_units);
				std::lock_guard<std::mutex> stateLock(state->mutex);
				failed = state->callback_failed;
				if (failed)
				{
					completed = false;
				}
				else if (state->callback_events.empty() && !state->callback_active &&
					!state->callback_drain_scheduled)
				{
					state->result = std::move(unit);
					state->init_status = ExecUnitInitState::READY;
					state->init_error.clear();
					state->creation_complete = true;
					state->creation_owner = std::thread::id();
					completed = true;
				}
			}
			if (failed)
				return failCreation("channel callback threw during unit initialization");
			if (completed)
			{
				state->condition.notify_all();
				return true;
			}
		}
	}
	catch (const std::exception& ex)
	{
		return failCreation(ex.what());
	}
	catch (...)
	{
		return failCreation("unexpected exception while creating execution unit");
	}
}

void WtLocalExecuter::retainUnitLocked(const ExecuteUnitPtr& unit)
{
	if (unit == NULL)
		return;
	for (const ExecuteUnitPtr& retained : _retained_units)
		if (retained == unit)
			return;
	_retained_units.push_back(unit);
}

ExecUnitInitStatus WtLocalExecuter::queryUnitInitStatus(const char* stdCode) const
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

ExecUnitInitClearResult WtLocalExecuter::clearFailedUnitInitStatus(const char* stdCode)
{
	IngressLease ingress(this);
	if (!ingress)
		return ExecUnitInitClearResult::RETRY_BLOCKED;
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
	if (state->runtime_failed)
		return ExecUnitInitClearResult::RETRY_BLOCKED;
	if (!state->callback_drain_quiesced)
		return ExecUnitInitClearResult::NOT_FAILED;

	_unit_states.erase(it);
	return ExecUnitInitClearResult::CLEARED;
}

bool WtLocalExecuter::enqueueUnitCallbackLocked(
	const std::shared_ptr<UnitLifecycleState>& state, const ExecuteUnitPtr& unit,
	std::function<void(ExecuteUnit*)> callback, bool isChannelEvent, uint64_t channelEpoch)
{
	if (state == NULL || !callback || state->callback_failed ||
		state->init_status == ExecUnitInitState::FAILED)
		return false;

	UnitCallbackEvent event;
	event.unit = unit;
	event.callback = std::move(callback);
	event.init_generation = state->init_generation;
	event.channel_epoch = channelEpoch;
	event.is_channel_event = isChannelEvent;
	state->callback_events.push_back(std::move(event));

	if (state->creation_complete && !state->callback_active && !state->callback_drain_scheduled)
	{
		state->callback_drain_scheduled = true;
		return true;
	}
	return false;
}

bool WtLocalExecuter::enqueueTargetReconciliationLocked(const std::string& code,
	const ExecuteUnitPtr& unit, const std::shared_ptr<UnitLifecycleState>& state)
{
	if (state == NULL || state->callback_failed ||
		state->init_status == ExecUnitInitState::FAILED || state->target_reconcile_event_queued)
		return false;

	std::weak_ptr<UnitLifecycleState> weakState = state;
	UnitCallbackEvent event;
	event.unit = unit;
	event.init_generation = state->init_generation;
	event.is_target_reconciliation = true;
	event.callback = [this, code, weakState](ExecuteUnit* targetUnit) {
		if (weakState.expired())
			return;
		bool allowed = false;
		double target = 0.0;
		{
			std::lock_guard<std::mutex> positionLock(_position_state_mutex);
			auto it = _target_pos.find(code);
			if (it == _target_pos.end())
				return;
			target = it->second;
			auto allowedIt = _target_pos_allowed.find(code);
			allowed = allowedIt != _target_pos_allowed.end() && allowedIt->second;
		}
		if (!allowed)
			return;
		targetUnit->set_position(code.c_str(), target * _scale);
	};
	state->callback_events.push_front(std::move(event));
	state->target_reconcile_event_queued = true;
	if (state->creation_complete && !state->callback_active && !state->callback_drain_scheduled)
	{
		state->callback_drain_scheduled = true;
		return true;
	}
	return false;
}

bool WtLocalExecuter::enqueueClearPositionLocked(const std::string& code, uint64_t revision,
	const ExecuteUnitPtr& unit, const std::shared_ptr<UnitLifecycleState>& state)
{
	if (state == NULL || revision == 0 || state->callback_failed ||
		state->init_status == ExecUnitInitState::FAILED)
		return false;
	if (state->clear_position_event_queued)
	{
		// A queued clear will observe the newest intent when it begins execution.
		state->clear_position_event_revision = revision;
		return false;
	}

	UnitCallbackEvent event;
	event.unit = unit;
	event.init_generation = state->init_generation;
	event.is_clear_position = true;
	event.clear_position_revision = revision;
	event.callback = [code](ExecuteUnit* targetUnit) { targetUnit->clear_all_position(code.c_str()); };

	auto insertAt = state->callback_events.end();
	if (!state->creation_complete)
	{
		// During initialization, reconcile the authoritative target first, then
		// channel readiness, then clear before ticks/orders queued behind them.
		auto target = state->callback_events.end();
		for (auto it = state->callback_events.begin(); it != state->callback_events.end(); ++it)
			if (it->is_target_reconciliation)
				target = it;
		if (target != state->callback_events.end())
			insertAt = std::next(target);
		else
			insertAt = state->callback_events.begin();
		for (auto it = state->callback_events.begin(); it != state->callback_events.end(); ++it)
		{
			if (it->is_target_reconciliation || it->is_channel_event)
				insertAt = std::next(it);
			else
				break;
		}
	}
	state->callback_events.insert(insertAt, std::move(event));
	state->clear_position_event_queued = true;
	state->clear_position_event_revision = revision;
	if (state->creation_complete && !state->callback_active && !state->callback_drain_scheduled)
	{
		state->callback_drain_scheduled = true;
		return true;
	}
	return false;
}

void WtLocalExecuter::queueClearPositionIntent(const std::string& code)
{
	std::shared_ptr<UnitLifecycleState> state;
	bool reserved = false;
	bool retrySafeFailure = false;
	bool schedule = false;
	uint64_t revision = 0;
	{
		SpinLock unitsLock(_mtx_units);
		{
			std::lock_guard<std::mutex> positionLock(_position_state_mutex);
			auto pending = _pending_clear_position.find(code);
			if (pending != _pending_clear_position.end() && pending->second.uncertain)
				return;
			revision = _next_clear_position_revision++;
			ClearPositionIntent intent;
			intent.revision = revision;
			intent.uncertain = false;
			_pending_clear_position[code] = intent;
		}

		auto stateIt = _unit_states.find(code);
		if (stateIt == _unit_states.end())
		{
			state = std::make_shared<UnitLifecycleState>();
			state->init_generation = _next_unit_init_generation++;
			_unit_states.emplace(code, state);
			if (_channel_ready.load())
			{
				std::lock_guard<std::mutex> stateLock(state->mutex);
				const uint64_t epoch = _channel_epoch.load();
				(void)enqueueUnitCallbackLocked(state, ExecuteUnitPtr(),
					[](ExecuteUnit* targetUnit) { targetUnit->on_channel_ready(); }, true, epoch);
			}
			reserved = true;
		}
		else
			state = stateIt->second;

		std::lock_guard<std::mutex> stateLock(state->mutex);
		if (state->init_status == ExecUnitInitState::FAILED || state->callback_failed)
			retrySafeFailure = !state->runtime_failed && state->callback_drain_quiesced;
		else
		{
			auto unitIt = _unit_map.find(code);
			const ExecuteUnitPtr unit = unitIt == _unit_map.end() ? ExecuteUnitPtr() : unitIt->second;
			schedule = enqueueClearPositionLocked(code, revision, unit, state);
			reserved = reserved || !state->creation_started;
		}
	}

	if (reserved)
	{
		try { onUnitCreationReserved(code); } catch (...) {}
	}
	if (reserved || retrySafeFailure)
		(void)getUnit(code.c_str());
	else if (schedule)
		scheduleUnitCallbackDrain(code, state);
}

bool WtLocalExecuter::enqueueCallbackForCode(const std::string& code,
	std::function<void(ExecuteUnit*)> callback)
{
	std::shared_ptr<UnitLifecycleState> state;
	bool schedule = false;
	{
		SpinLock unitsLock(_mtx_units);
		auto stateIt = _unit_states.find(code);
		if (stateIt == _unit_states.end())
			return false;
		state = stateIt->second;
		std::lock_guard<std::mutex> stateLock(state->mutex);
		if (state->init_status == ExecUnitInitState::FAILED || state->callback_failed)
			return true;
		auto unitIt = _unit_map.find(code);
		const ExecuteUnitPtr unit = unitIt == _unit_map.end() ? ExecuteUnitPtr() : unitIt->second;
		// Queue during initialization and while another same-code callback is
		// running. Never wait here: plugin code may join a helper that re-enters.
		schedule = enqueueUnitCallbackLocked(state, unit, std::move(callback));
	}
	if (schedule)
		scheduleUnitCallbackDrain(code, state);
	return true;
}

void WtLocalExecuter::scheduleUnitCallbackDrain(const std::string& code,
	const std::shared_ptr<UnitLifecycleState>& state)
{
	auto drain = [this, code, state]() { drainUnitCallbacks(code, state); };
	if (_pool)
		WtExecuterSchedule::scheduleOrRunFallback(*_pool, drain, drain);
	else
		drain();
}

void WtLocalExecuter::drainUnitCallbacks(const std::string& code,
	const std::shared_ptr<UnitLifecycleState>& state)
{
	if (state == NULL)
		return;
	LocalExecutorCallbackScope callbackScope(this, _active_callback_drains, _quiesce_condition);
	{
		std::lock_guard<std::mutex> lock(state->mutex);
		if (state->callback_active)
			return;
		state->callback_drain_scheduled = false;
		state->callback_active = true;
		state->callback_drain_quiesced = false;
		state->callback_owner = std::this_thread::get_id();
	}

	for (;;)
	{
		UnitCallbackEvent event;
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			if (state->callback_failed || state->init_status == ExecUnitInitState::FAILED ||
				state->callback_events.empty())
			{
				state->callback_active = false;
				state->callback_drain_quiesced = true;
				state->callback_owner = std::thread::id();
				state->condition.notify_all();
				return;
			}
			else
			{
				event = std::move(state->callback_events.front());
				state->callback_events.pop_front();
				if (event.is_target_reconciliation)
					state->target_reconcile_event_queued = false;
				if (event.is_clear_position)
				{
					// Coalesce any clear intents received before dispatch into this one
					// operation. A new request arriving during the plugin callback queues
					// a separate later operation.
					event.clear_position_revision = state->clear_position_event_revision;
					state->clear_position_event_queued = false;
				}
			}
		}
		// Validate the generation and channel epoch under the same short lock
		// used to publish channel transitions. Never hold either lock in plugin code.
		bool current = false;
		ExecuteUnitPtr callbackUnit;
		{
			SpinLock unitsLock(_mtx_units);
			auto currentState = _unit_states.find(code);
			if (currentState != _unit_states.end() && currentState->second == state)
			{
				std::lock_guard<std::mutex> stateLock(state->mutex);
				current = event.init_generation == state->init_generation &&
					state->init_status != ExecUnitInitState::FAILED;
				if (current && event.is_channel_event)
				{
					// Preserve every channel transition in per-code FIFO order. The
					// global epoch may already be newer while an earlier transition is
					// blocked in another callback.
					current = event.channel_epoch > state->last_channel_epoch;
					if (current)
						state->last_channel_epoch = event.channel_epoch;
				}
				if (current)
				{
					callbackUnit = event.unit;
					if (callbackUnit == NULL)
					{
						auto unitIt = _unit_map.find(code);
						if (unitIt != _unit_map.end())
							callbackUnit = unitIt->second;
					}
				}
			}
		}
		if (!current || callbackUnit == NULL)
			continue;

		bool callbackSucceeded = false;
		try
		{
			if (callbackUnit->self() != NULL)
			{
				event.callback(callbackUnit->self());
				callbackSucceeded = true;
			}
		}
		catch (...)
		{
			// The current callback and any queued successors are not proven complete.
			_quiesce_incomplete.store(true, std::memory_order_release);
			bool initializing = false;
			uint64_t failedRuntimeGeneration = 0;
			std::deque<UnitCallbackEvent> discardedEvents;
			ExecuteUnitPtr detachedUnit;
			{
				SpinLock unitsLock(_mtx_units);
				auto stateIt = _unit_states.find(code);
				if (callbackUnit != NULL)
					retainUnitLocked(callbackUnit);
				if (stateIt != _unit_states.end() && stateIt->second == state)
				{
					auto unitIt = _unit_map.find(code);
					if (unitIt != _unit_map.end() && unitIt->second == callbackUnit)
					{
						detachedUnit = unitIt->second;
						_unit_map.erase(unitIt);
					}
				}
				std::lock_guard<std::mutex> lock(state->mutex);
				initializing = !state->creation_complete;
				state->callback_failed = true;
				// The plugin callback may have emitted an order before throwing, even
				// during initialization. Without a trade/order recovery contract this
				// generation must remain fail-closed.
				state->runtime_failed = true;
				state->init_error = "execution-unit callback threw";
				state->init_status = ExecUnitInitState::FAILED;
				state->result.reset();
				state->creation_complete = true;
				state->creation_owner = std::thread::id();
				state->retired_generation = state->init_generation;
				state->callback_drain_quiesced = false;
				failedRuntimeGeneration = state->init_generation;
				state->target_reconcile_event_queued = false;
				state->clear_position_event_queued = false;
				discardedEvents.swap(state->callback_events);
				state->callback_drain_scheduled = false;
				if (event.is_clear_position)
				{
					std::lock_guard<std::mutex> positionLock(_position_state_mutex);
					auto pending = _pending_clear_position.find(code);
					if (pending != _pending_clear_position.end())
						pending->second.uncertain = true;
				}
				if (initializing)
				{
					// The creator still owns its local ExecuteUnitPtr. Let getUnit's
					// failure path release it and open the quiesced barrier only then.
					state->callback_active = false;
					state->callback_owner = std::thread::id();
				}
			}
			state->condition.notify_all();
			try
			{
				WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
					"Execution-unit callback for {} threw; the unit is fail-closed", code.c_str());
			}
			catch (...)
			{
			}
			if (failedRuntimeGeneration != 0)
			{
				if (!initializing)
				{
					try { onUnitGenerationRetainedForQuiesce(code, failedRuntimeGeneration, callbackUnit); }
					catch (...) {}
				}
				// Preserve failed plugin units in _retained_units. The lifecycle
				// coordinator may release them only after every plugin wait succeeds.
				discardedEvents.clear();
				detachedUnit.reset();
				event.unit.reset();
				callbackUnit.reset();
				{
					std::lock_guard<std::mutex> lock(state->mutex);
					state->callback_active = false;
					state->callback_owner = std::thread::id();
				}
				state->condition.notify_all();
				(void)initializing;
				return;
			}
		}
		if (callbackSucceeded && event.is_clear_position)
		{
			bool scheduleFollowup = false;
			uint64_t followupRevision = 0;
			{
				SpinLock unitsLock(_mtx_units);
				auto stateIt = _unit_states.find(code);
				if (stateIt != _unit_states.end() && stateIt->second == state)
				{
					std::lock_guard<std::mutex> stateLock(state->mutex);
					if (state->clear_position_event_revision == event.clear_position_revision)
						state->clear_position_event_queued = false;
					{
						std::lock_guard<std::mutex> positionLock(_position_state_mutex);
						auto pending = _pending_clear_position.find(code);
						if (pending != _pending_clear_position.end())
						{
							if (pending->second.revision == event.clear_position_revision)
								_pending_clear_position.erase(pending);
							else if (!pending->second.uncertain)
								followupRevision = pending->second.revision;
						}
					}
					if (followupRevision != 0 && !state->clear_position_event_queued)
						scheduleFollowup = enqueueClearPositionLocked(code, followupRevision,
							callbackUnit, state);
				}
			}
			if (scheduleFollowup)
				scheduleUnitCallbackDrain(code, state);
		}
	}
}

void WtLocalExecuter::submitTargetBatch(const wt_hashmap<std::string, double>& targets, bool incremental)
{
	struct Action
	{
		Action(const std::string& valueCode, double valueTarget, bool valueBypass = false)
			: code(valueCode), target(valueTarget), bypass(valueBypass) {}
		std::string code;
		double target = 0.0;
		bool allowed = true;
		bool bypass = false;
		bool create = false;
		bool reserved = false;
		bool schedule = false;
		bool terminal = false;
		std::shared_ptr<UnitLifecycleState> state;
	};
	wt_hashmap<std::string, double> previous;
	std::vector<std::string> channelHolds;
	uint64_t revision = 0;
	{
		std::lock_guard<std::mutex> lock(_position_state_mutex);
		previous = _target_pos;
		revision = _target_revision;
		if (_strict_sync) channelHolds.assign(_channel_holds.begin(), _channel_holds.end());
	}
	wt_hashmap<std::string, double> proposed = incremental ? previous : targets;
	if (incremental)
	{
		const auto& change = *targets.begin();
		const auto it = previous.find(change.first);
		const double oldTarget = it == previous.end() ? 0.0 : it->second;
		const double value = oldTarget + change.second;
		if (!std::isfinite(value)) return;
		proposed[change.first] = value;
	}
	execution_groups::Positions realTargets, physical;
	std::string error;
	if (!execution_groups::calculate(_group_rules, proposed, _scale, realTargets, physical, error, !incremental))
	{
		WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR, "Target batch rejected: {}", error);
		return;
	}
	for (const auto& item : physical)
	{
		if (item.second >= 0.0) continue;
		WTSCommodityInfo* commodity = _stub ? _stub->get_comm_info(item.first.c_str()) : NULL;
		if (!commodity || !commodity->canShort())
		{
			WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
				"Target batch rejected: short permission unavailable for {}", item.first);
			return;
		}
	}
	std::vector<Action> actions;
	if (incremental)
	{
		const std::string& code = targets.begin()->first;
		actions.emplace_back(code, realTargets.at(code));
	}
	else
	{
		for (const auto& target : realTargets) actions.emplace_back(target.first, target.second);
		for (const auto& target : previous)
			if (!realTargets.count(target.first)) actions.emplace_back(target.first, 0.0, true);
		for (const std::string& code : channelHolds)
			if (!previous.count(code) && !realTargets.count(code)) actions.emplace_back(code, 0.0, true);
	}
	for (Action& action : actions)
		if (!action.bypass) action.allowed = !_trader || _trader->checkOrderLimits(action.code.c_str());
	bool stale = false;
	{
		SpinLock unitsLock(_mtx_units);
		{
			std::lock_guard<std::mutex> positionLock(_position_state_mutex);
			if (_target_revision != revision)
				stale = true;
			else
			{
				auto newTargets = _target_pos;
				auto newAllowed = _target_pos_allowed;
				for (const Action& action : actions)
				{
					newTargets[action.code] = action.target;
					newAllowed[action.code] = action.allowed;
				}
				_target_pos.swap(newTargets);
				_target_pos_allowed.swap(newAllowed);
				++_target_revision;
			}
		}
		if (!stale)
		for (Action& action : actions)
		{
			auto stateIt = _unit_states.find(action.code);
			if (stateIt != _unit_states.end())
			{
				action.state = stateIt->second;
				std::lock_guard<std::mutex> stateLock(action.state->mutex);
				if (action.state->init_status == ExecUnitInitState::FAILED || action.state->callback_failed)
				{
					action.create = true;
					if (action.state->runtime_failed)
					{
						action.terminal = true;
						action.state->init_error = fmtutil::format(
							"execution-unit callback threw; target {} retained but not applied after generation {} entered terminal FAILED",
							action.target, action.state->init_generation);
					}
				}
				else if (!action.state->creation_complete)
				{
					auto unitIt = _unit_map.find(action.code);
					const ExecuteUnitPtr unit = unitIt == _unit_map.end() ? ExecuteUnitPtr() : unitIt->second;
					action.schedule = enqueueTargetReconciliationLocked(action.code, unit, action.state);
					action.create = !action.state->creation_started;
				}
				else if (action.allowed)
				{
					auto unitIt = _unit_map.find(action.code);
					const std::string code = action.code; const double target = action.target * _scale;
					action.schedule = unitIt != _unit_map.end() && enqueueUnitCallbackLocked(action.state, unitIt->second,
						[code, target](ExecuteUnit* unit) { unit->set_position(code.c_str(), target); });
				}
			}
			else
			{
				action.state = std::make_shared<UnitLifecycleState>();
				action.state->init_generation = _next_unit_init_generation++;
				_unit_states.emplace(action.code, action.state);
				if (_channel_ready.load())
				{
					std::lock_guard<std::mutex> stateLock(action.state->mutex);
					const uint64_t epoch = _channel_epoch.load();
					(void)enqueueUnitCallbackLocked(action.state, ExecuteUnitPtr(),
						[](ExecuteUnit* unit) { unit->on_channel_ready(); }, true, epoch);
				}
				action.create = true;
				action.reserved = true;
			}
		}
	}
	if (stale)
	{
		WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR, "Target batch rejected: target state changed during validation");
		return;
	}
	for (Action& action : actions)
	{
		if (action.terminal)
			WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
				"Target position for {} retained but not applied: execution unit is terminal FAILED", action.code);
		if (action.create && !action.terminal)
		{
			if (action.reserved) { try { onUnitCreationReserved(action.code); } catch (...) {} }
			(void)getUnit(action.code.c_str());
		}
		if (action.schedule) scheduleUnitCallbackDrain(action.code, action.state);
	}
}



//////////////////////////////////////////////////////////////////////////
//ExecuteContext
#pragma region Context回调接口
/**
 * @brief 获取tick数据切片
 * @param stdCode 标准合约代码
 * @param count 请求的tick数量
 * @param etime 结束时间，默认为0（不使用）
 * @return tick数据切片指针
 * @details 从数据管理器中获取指定合约的最近count个tick数据。
 * 如果数据管理器不可用，则返回NULL。
 */
WTSTickSlice* WtLocalExecuter::getTicks(const char* stdCode, uint32_t count, uint64_t /*etime*/ /* = 0 */)
{
	// 检查数据管理器是否可用
	if (_data_mgr == NULL)
		return NULL;

	// 从数据管理器获取tick切片
	return _data_mgr->get_tick_slice(stdCode, count);
}

/**
 * @brief 获取最新的tick数据
 * @param stdCode 标准合约代码
 * @return 最新的tick数据指针
 * @details 从数据管理器中获取指定合约的最新tick数据。
 * 如果数据管理器不可用，则返回NULL。
 * 注意：这个函数会增加返回的tick数据的引用计数，使用后需要调用release释放。
 */
WTSTickData* WtLocalExecuter::grabLastTick(const char* stdCode)
{
	// 检查数据管理器是否可用
	if (_data_mgr == NULL)
		return NULL;

	// 从数据管理器获取最新tick数据
	return _data_mgr->grab_last_tick(stdCode);
}

/**
 * @brief 获取指定合约的持仓量
 * @param stdCode 标准合约代码
 * @param validOnly 是否只计算有效持仓，默认为true
 * @param flag 持仓标记，默认为3（全部持仓），1表示多头持仓，2表示空头持仓
 * @return 持仓量
 * @details 从交易适配器中获取指定合约的持仓量。
 * 如果交易适配器不可用，则返回0。
 */
double WtLocalExecuter::getPosition(const char* stdCode, bool validOnly /* = true */, int32_t flag /* = 3 */)
{
	// 检查交易适配器是否可用
	if (NULL == _trader)
		return 0.0;

	// 从交易适配器获取持仓量
	return _trader->getPosition(stdCode, validOnly, flag);
}

/**
 * @brief 获取未完成的委托数量
 * @param stdCode 标准合约代码
 * @return 未完成的委托数量
 * @details 从交易适配器中获取指定合约的未完成委托数量。
 * 如果交易适配器不可用，则返回0。
 */
double WtLocalExecuter::getUndoneQty(const char* stdCode)
{
	// 检查交易适配器是否可用
	if (NULL == _trader)
		return 0.0;

	// 从交易适配器获取未完成委托数量
	return _trader->getUndoneQty(stdCode);
}

/**
 * @brief 获取指定合约的订单列表
 * @param stdCode 标准合约代码
 * @return 订单映射指针
 * @details 从交易适配器中获取指定合约的所有订单。
 * 如果交易适配器不可用，则返回NULL。
 */
OrderMap* WtLocalExecuter::getOrders(const char* stdCode)
{
	// 检查交易适配器是否可用
	if (NULL == _trader)
		return NULL;

	// 从交易适配器获取订单列表
	return _trader->getOrders(stdCode);
}

/**
 * @brief 发送买入委托
 * @param stdCode 标准合约代码
 * @param price 委托价格
 * @param qty 委托数量
 * @param bForceClose 是否强制平仓，默认为false
 * @return 订单ID集合
 * @details 通过交易适配器发送买入委托。
 * 如果交易通道未就绪，则返回空的订单ID集合。
 */
OrderIDs WtLocalExecuter::buy(const char* stdCode, double price, double qty, bool bForceClose/* = false*/)
{
	// 检查交易通道是否就绪
	if (!_channel_ready)
		return OrderIDs();

	// 通过交易适配器发送买入委托，偏移量设置为0
	return _trader->buy(stdCode, price, qty, 0, bForceClose);
}

/**
 * @brief 发送卖出委托
 * @param stdCode 标准合约代码
 * @param price 委托价格
 * @param qty 委托数量
 * @param bForceClose 是否强制平仓，默认为false
 * @return 订单ID集合
 * @details 通过交易适配器发送卖出委托。
 * 如果交易通道未就绪，则返回空的订单ID集合。
 */
OrderIDs WtLocalExecuter::sell(const char* stdCode, double price, double qty, bool bForceClose/* = false*/)
{
	// 检查交易通道是否就绪
	if (!_channel_ready)
		return OrderIDs();

	// 通过交易适配器发送卖出委托，偏移量设置为0
	return _trader->sell(stdCode, price, qty, 0, bForceClose);
}

/**
 * @brief 取消指定订单
 * @param localid 本地订单ID
 * @return 取消是否成功
 * @details 通过交易适配器取消指定的订单。
 * 如果交易通道未就绪，则返回false。
 */
bool WtLocalExecuter::cancel(uint32_t localid)
{
	// 检查交易通道是否就绪
	if (!_channel_ready)
		return false;

	// 通过交易适配器取消订单
	return _trader->cancel(localid);
}

/**
 * @brief 批量取消指定合约的委托
 * @param stdCode 标准合约代码
 * @param isBuy 是否为买入委托
 * @param qty 要取消的数量
 * @return 被取消的订单ID集合
 * @details 通过交易适配器批量取消指定合约、指定方向的委托。
 * 如果交易通道未就绪，则返回空的订单ID集合。
 */
OrderIDs WtLocalExecuter::cancel(const char* stdCode, bool isBuy, double qty)
{
	// 检查交易通道是否就绪
	if (!_channel_ready)
		return OrderIDs();

	// 通过交易适配器批量取消委托
	return _trader->cancel(stdCode, isBuy, qty);
}

/**
 * @brief 写入日志
 * @param message 日志消息
 * @details 将消息写入到日志系统中，并自动添加执行器名称前缀。
 * 使用线程本地存储的缓冲区来提高性能。
 */
void WtLocalExecuter::writeLog(const char* message)
{
	// 使用线程本地存储的缓冲区，避免多线程冲突
	static thread_local char szBuf[MAX_LOG_BUF_SIZE] = { 0 };
	// 固定缓冲区最多保留 MAX_LOG_BUF_SIZE - 1 字节，并始终保留 NUL 终止符。
	// 消息（以及极长名称）超出容量时由 snprintf 确定性截断。
	std::snprintf(szBuf, sizeof(szBuf), "[%s]%s", _name.c_str(), message ? message : "");
	// 写入日志
	WTSLogger::log_dyn_raw("executer", _name.c_str(), LL_INFO, szBuf);
}

/**
 * @brief 获取商品信息
 * @param stdCode 标准合约代码
 * @return 商品信息指针
 * @details 从交易引擎核心中获取指定合约的商品信息。
 * 商品信息包含合约的交易所、品种、合约代码、手续费率、保证金率等信息。
 */
WTSCommodityInfo* WtLocalExecuter::getCommodityInfo(const char* stdCode)
{
	// 从交易引擎核心获取商品信息
	return _stub->get_comm_info(stdCode);
}

/**
 * @brief 获取交易时段信息
 * @param stdCode 标准合约代码
 * @return 交易时段信息指针
 * @details 从交易引擎核心中获取指定合约的交易时段信息。
 * 交易时段信息包含开盘时间、收盘时间、交易时段等信息。
 */
WTSSessionInfo* WtLocalExecuter::getSessionInfo(const char* stdCode)
{
	// 从交易引擎核心获取交易时段信息
	return _stub->get_sess_info(stdCode);
}

/**
 * @brief 获取当前时间
 * @return 当前时间戳，格式为时间戳格式
 * @details 从交易引擎核心中获取当前的真实时间。
 * 这个时间用于执行器的时间同步和交易决策。
 */
uint64_t WtLocalExecuter::getCurTime()
{
	// 从交易引擎核心获取当前真实时间
	return _stub->get_real_time();
	//return TimeUtils::makeTime(_stub->get_date(), _stub->get_raw_time() * 100000 + _stub->get_secs());
}

#pragma endregion Context回调接口
//ExecuteContext
//////////////////////////////////////////////////////////////////////////


#pragma region 外部接口
/**
 * @brief 持仓变动回调
 * @param stdCode 标准合约代码
 * @param diffPos 持仓变动量，正值表示增加，负值表示减少
 * @details 当策略引擎要求修改目标仓位时调用此函数。
 * 函数将更新内部持仓目标，并将缩放后的持仓目标传送给执行单元。
 * 同时考虑了订单限制和缩放比例。
 */
void WtLocalExecuter::on_position_changed(const char* stdCode, double diffPos)
{
	IngressLease ingress(this);
	if (!ingress || stdCode == NULL || stdCode[0] == '\0' ||
		!std::isfinite(diffPos) || !std::isfinite(diffPos * _scale)) return;
	wt_hashmap<std::string, double> targets;
	targets.emplace(stdCode, diffPos);
	submitTargetBatch(targets, true);
}

void WtLocalExecuter::set_position(const wt_hashmap<std::string, double>& targets)
{
	IngressLease ingress(this);
	if (!ingress) return;
	submitTargetBatch(targets, false);
}

/**
 * @brief 实时行情回调
 * @param stdCode 标准合约代码
 * @param newTick 新的行情数据
 * @details 当收到新的行情数据时调用此函数，并将其转发给相应的执行单元。
 * 如果启用了线程池，则异步处理行情数据。
 */
void WtLocalExecuter::on_tick(const char* stdCode, WTSTickData* newTick)
{
	IngressLease ingress(this);
	if (!ingress)
		return;
	if (stdCode == NULL || stdCode[0] == '\0')
		return;
	const std::string code = stdCode ? stdCode : "";
	if (newTick != NULL)
		newTick->retain();
	std::shared_ptr<WTSTickData> tickRef(newTick, [](WTSTickData* tick) {
		if (tick != NULL)
			tick->release();
	});
	auto callback = [tickRef](ExecuteUnit* targetUnit) { targetUnit->on_tick(tickRef.get()); };
	(void)enqueueCallbackForCode(code, std::move(callback));
}

/**
 * @brief 成交回调
 * @param localid 本地订单ID
 * @param stdCode 标准合约代码
 * @param isBuy 是否为买入
 * @param vol 成交量
 * @param price 成交价格
 * @details 当收到成交回报时调用此函数，并将其转发给相应的执行单元。
 * 如果启用了线程池，则异步处理成交数据。
 */
void WtLocalExecuter::on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price)
{
	IngressLease ingress(this);
	if (!ingress)
		return;
	if (stdCode == NULL || stdCode[0] == '\0')
		return;
	const std::string code = stdCode;
	auto callback = [localid, code, isBuy, vol, price](ExecuteUnit* targetUnit) {
		targetUnit->on_trade(localid, code.c_str(), isBuy, vol, price);
	};
	(void)enqueueCallbackForCode(code, std::move(callback));
}

/**
 * @brief 订单状态回调
 * @param localid 本地订单ID
 * @param stdCode 标准合约代码
 * @param isBuy 是否为买入
 * @param totalQty 总数量
 * @param leftQty 剩余数量
 * @param price 委托价格
 * @param isCanceled 是否已取消，默认为false
 * @details 当订单状态变化时调用此函数，并将状态变化转发给相应的执行单元。
 * 如果启用了线程池，则异步处理订单状态。
 */
void WtLocalExecuter::on_order(uint32_t localid, const char* stdCode, bool isBuy,
	double /*totalQty*/, double leftQty, double price, bool isCanceled)
{
	IngressLease ingress(this);
	if (!ingress)
		return;
	if (stdCode == NULL || stdCode[0] == '\0')
		return;
	const std::string code = stdCode;
	auto callback = [localid, code, isBuy, leftQty, price, isCanceled](ExecuteUnit* targetUnit) {
		targetUnit->on_order(localid, code.c_str(), isBuy, leftQty, price, isCanceled);
	};
	(void)enqueueCallbackForCode(code, std::move(callback));
}

/**
 * @brief 委托回报回调
 * @param localid 本地订单ID
 * @param stdCode 标准合约代码
 * @param bSuccess 委托是否成功
 * @param message 委托回报消息
 * @details 当收到委托回报时调用此函数，并将回报信息转发给相应的执行单元。
 * 如果启用了线程池，则异步处理委托回报。
 */
void WtLocalExecuter::on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message)
{
	IngressLease ingress(this);
	if (!ingress)
		return;
	if (stdCode == NULL || stdCode[0] == '\0')
		return;
	const std::string code = stdCode;
	const std::string msg = message ? message : "";
	auto callback = [localid, code, bSuccess, msg](ExecuteUnit* targetUnit) {
		targetUnit->on_entrust(localid, code.c_str(), bSuccess, msg.c_str());
	};
	(void)enqueueCallbackForCode(code, std::move(callback));
}

/**
 * @brief 交易通道就绪回调
 * @details 当交易通道就绪时调用此函数，设置通道状态并将此消息转发给所有执行单元。
 * 如果启用了线程池，则异步通知所有执行单元。
 */
void WtLocalExecuter::on_channel_ready()
{
	IngressLease ingress(this);
	if (!ingress)
		return;
	struct DrainTask
	{
		std::string code;
		std::shared_ptr<UnitLifecycleState> state;
	};
	std::vector<DrainTask> drains;
	{
		SpinLock unitsLock(_mtx_units);
		if (_channel_ready.exchange(true))
			return;
		const uint64_t epoch = _channel_epoch.fetch_add(1) + 1;
		for (const auto& item : _unit_states)
		{
			auto unitIt = _unit_map.find(item.first);
			ExecuteUnitPtr unit = unitIt == _unit_map.end() ? ExecuteUnitPtr() : unitIt->second;
			std::lock_guard<std::mutex> stateLock(item.second->mutex);
			if (enqueueUnitCallbackLocked(item.second, unit,
				[](ExecuteUnit* unit) { unit->on_channel_ready(); }, true, epoch))
				drains.push_back({item.first, item.second});
		}
	}
	for (const DrainTask& task : drains)
		scheduleUnitCallbackDrain(task.code, task.state);
}

/**
 * @brief 交易通道断开回调
 * @details 当交易通道断开时调用此函数，设置通道状态并将此消息转发给所有执行单元。
 * 如果启用了线程池，则异步通知所有执行单元。
 */
void WtLocalExecuter::on_channel_lost()
{
	IngressLease ingress(this);
	if (!ingress)
		return;
	struct DrainTask
	{
		std::string code;
		std::shared_ptr<UnitLifecycleState> state;
	};
	std::vector<DrainTask> drains;
	{
		SpinLock unitsLock(_mtx_units);
		if (!_channel_ready.exchange(false))
			return;
		const uint64_t epoch = _channel_epoch.fetch_add(1) + 1;
		for (const auto& item : _unit_states)
		{
			auto unitIt = _unit_map.find(item.first);
			ExecuteUnitPtr unit = unitIt == _unit_map.end() ? ExecuteUnitPtr() : unitIt->second;
			std::lock_guard<std::mutex> stateLock(item.second->mutex);
			if (enqueueUnitCallbackLocked(item.second, unit,
				[](ExecuteUnit* unit) { unit->on_channel_lost(); }, true, epoch))
				drains.push_back({item.first, item.second});
		}
	}
	for (const DrainTask& task : drains)
		scheduleUnitCallbackDrain(task.code, task.state);
}

/**
 * @brief 账户资金变动回调
 * @param currency 货币代码
 * @param prebalance 前结余额
 * @param balance 静态权益
 * @param dynbalance 动态权益
 * @param avaliable 可用资金
 * @param closeprofit 平仓盈亏
 * @param dynprofit 浮动盈亏
 * @param margin 占用保证金
 * @param fee 手续费
 * @param deposit 入金
 * @param withdraw 出金
 * @details 当收到账户资金变动消息时调用此函数，并将更新信息转发给所有执行单元。
 * 如果启用了线程池，则异步通知所有执行单元。
 */
void WtLocalExecuter::on_account(const char* currency, double prebalance, double balance,
	double dynbalance, double avaliable, double closeprofit, double dynprofit, double margin,
	double fee, double deposit, double withdraw)
{
	IngressLease ingress(this);
	if (!ingress)
		return;
	struct UnitTarget
	{
		std::string code;
		std::shared_ptr<UnitLifecycleState> state;
	};
	std::vector<UnitTarget> drains;
	const std::string cur = currency ? currency : "";
	{
		SpinLock unitsLock(_mtx_units);
		drains.reserve(_unit_states.size());
		for (const auto& item : _unit_states)
		{
			auto unitIt = _unit_map.find(item.first);
			std::lock_guard<std::mutex> stateLock(item.second->mutex);
			if (item.second->init_status == ExecUnitInitState::FAILED || item.second->callback_failed)
				continue;
			auto callback = [cur, prebalance, balance, dynbalance, avaliable, closeprofit,
				dynprofit, margin, fee, deposit, withdraw](ExecuteUnit* targetUnit) {
				targetUnit->on_account(cur.c_str(), prebalance, balance, dynbalance, avaliable,
					closeprofit, dynprofit, margin, fee, deposit, withdraw);
			};
			const ExecuteUnitPtr unit = unitIt == _unit_map.end() ? ExecuteUnitPtr() : unitIt->second;
			if (enqueueUnitCallbackLocked(item.second, unit, std::move(callback)))
				drains.push_back({item.first, item.second});
		}
	}
	for (const UnitTarget& target : drains)
		scheduleUnitCallbackDrain(target.code, target.state);
}

/**
 * @brief 持仓信息回调
 * @param stdCode 标准合约代码
 * @param isLong 是否为多头持仓
 * @param prevol 之前持仓量
 * @param preavail 之前可用持仓量
 * @param newvol 新持仓量
 * @param newavail 新可用持仓量
 * @param tradingday 交易日
 * @details 当收到持仓变动信息时调用此函数，并将合约代码添加到通道持仓集合中。
 * 此函数还包含自动清理过期主力合约的逻辑，通过检查当前合约是否是上一期的主力合约，
 * 并根据包含和排除列表决定是否进行自动清仓。
 */
void WtLocalExecuter::on_position(const char* stdCode, bool /*isLong*/, double /*prevol*/, double /*preavail*/, double /*newvol*/, double /*newavail*/, uint32_t tradingday)
{
	IngressLease ingress(this);
	if (!ingress)
		return;
    if (stdCode == NULL || stdCode[0] == '\0')
        return;

    // 将合约添加到通道持仓集合中
    {
        std::lock_guard<std::mutex> lock(_position_state_mutex);
        if (_channel_holds.insert(stdCode).second) ++_target_revision;
    }

    /*
     *  By Wesley @ 2021.12.14
     *  先检查自动清理过期主力合约的标记是否为true
     *  如果不为true，则直接退出该逻辑
     */
    if (!_auto_clear)
        return;

    // 如果不是分月期货合约，直接退出
    if (!CodeHelper::isStdMonthlyFutCode(stdCode))
        return;

    // 获取热力合约管理器
    IHotMgr* hotMgr = _stub->get_hot_mon();
    // 解析标准合约代码
    CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, NULL);
    // 获取上一期的主力合约
    std::string prevCode = hotMgr->getPrevRawCode(cInfo._exchg, cInfo._product, tradingday);

    // 如果当前合约不是上一期的主力合约，则直接退出
    if (prevCode != cInfo._code)
        return;

    // 记录日志，显示找到了上一期的主力合约
    WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "Prev hot contract of {}.{} on {} is {}", cInfo._exchg, cInfo._product, tradingday, prevCode);

    // 创建产品完整标识符
    thread_local static char fullPid[64] = { 0 };
    fmtutil::format_to(fullPid, "{}.{}", cInfo._exchg, cInfo._product);

    // 先检查排除列表
    // 如果在排除列表中，则直接退出
    auto it = _clear_excludes.find(fullPid);
    if(it != _clear_excludes.end())
    {
        WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "Position of {}, as prev hot contract, won't be cleared for it's in exclude list", stdCode);
        return;
    }

    // 如果包含列表不为空，再检查是否在包含列表中
    // 如果为空，则全部清理，不再进入该逻辑
    if(!_clear_includes.empty())
    {
        it = _clear_includes.find(fullPid);
        if (it == _clear_includes.end())
        {
            WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "Position of {}, as prev hot contract, won't be cleared for it's not in include list", stdCode);
            return;
        }
    }

    // 最后再进行自动清理
    WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "Position of {}, as prev hot contract, will be cleared", stdCode);
	queueClearPositionIntent(stdCode);
}

#pragma endregion 外部接口
