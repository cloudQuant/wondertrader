/*!
 * \file WtDiffExecuter.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 *
 * \brief 差量执行器实现文件
 * \details 差量执行器(WtDiffExecuter)用于处理目标仓位与实际仓位之间的差值，
 *          通过计算差量并执行相应的交易操作，实现仓位的精确控制。
 */
#include "WtDiffExecuter.h"
#include "WtExecuterSchedule.h"
#include "ExecPolicyConfig.h"
#include "TraderAdapter.h"
#include "WtEngine.h"
#include "WtHelper.h"

#include "../Share/CodeHelper.hpp"
#include "../Includes/IDataManager.h"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/IHotMgr.h"
#include "../Includes/IBaseDataMgr.h"
#include "../Share/decimal.h"

#include "../WTSTools/WTSLogger.h"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <map>
#include <vector>
#include <cstdio>
#include <locale>
#include <sstream>
#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif
namespace rj = rapidjson;

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
std::atomic<uint64_t> s_diff_save_serial(0);

std::string makeDiffTempFilename(const std::string& filename)
{
#ifdef _WIN32
	const int processId = _getpid();
#else
	const int processId = getpid();
#endif
	return filename + ".tmp." + std::to_string(processId) + "." +
		std::to_string(++s_diff_save_serial);
}

bool replaceDiffFileAtomically(const std::string& source, const std::string& target)
{
#ifdef _WIN32
	return MoveFileExA(source.c_str(), target.c_str(),
		MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
	return std::rename(source.c_str(), target.c_str()) == 0;
#endif
}

const rj::Value* getUniqueMember(const rj::Value& object, const char* key)
{
	if (!object.IsObject())
		return NULL;

	const rj::Value* value = NULL;
	for (rj::Value::ConstMemberIterator it = object.MemberBegin(); it != object.MemberEnd(); ++it)
	{
		if (it->name.IsString() && it->name.GetStringLength() == strlen(key) &&
			memcmp(it->name.GetString(), key, it->name.GetStringLength()) == 0)
		{
			if (value != NULL)
				return NULL;
			value = &it->value;
		}
	}
	return value;
}

bool hasAtMostOneMember(const rj::Value& object, const char* key)
{
	if (!object.IsObject())
		return false;

	unsigned count = 0;
	for (rj::Value::ConstMemberIterator it = object.MemberBegin(); it != object.MemberEnd(); ++it)
	{
		if (it->name.IsString() && it->name.GetStringLength() == strlen(key) &&
			memcmp(it->name.GetString(), key, it->name.GetStringLength()) == 0 && ++count > 1)
			return false;
	}
	return true;
}

bool parseDiffPositionSection(const rj::Value& section, const char* valueKey,
	std::map<std::string, double>& out, std::string& error)
{
	if (section.IsObject())
	{
		for (rj::Value::ConstMemberIterator it = section.MemberBegin(); it != section.MemberEnd(); ++it)
		{
			if (!it->name.IsString() || it->name.GetStringLength() == 0 || !it->value.IsNumber())
			{
				error = "object-map entries must have a non-empty code and numeric value";
				return false;
			}
			const std::string code(it->name.GetString(), it->name.GetStringLength());
			const double value = it->value.GetDouble();
			if (code.find('\0') != std::string::npos || !std::isfinite(value) ||
				!out.emplace(code, value).second)
			{
				error = "object-map contains an invalid or duplicate code/value";
				return false;
			}
		}
		return true;
	}

	if (!section.IsArray())
	{
		error = "expected an array of records or a historical object-map";
		return false;
	}

	for (const rj::Value& item : section.GetArray())
	{
		const rj::Value* codeValue = getUniqueMember(item, "code");
		const rj::Value* positionValue = getUniqueMember(item, valueKey);
		if (!item.IsObject() || codeValue == NULL || !codeValue->IsString() ||
			codeValue->GetStringLength() == 0 || positionValue == NULL || !positionValue->IsNumber())
		{
			error = std::string("array records require string code and numeric ") + valueKey;
			return false;
		}

		const std::string code(codeValue->GetString(), codeValue->GetStringLength());
		const double value = positionValue->GetDouble();
		if (code.find('\0') != std::string::npos || !std::isfinite(value) ||
			!out.emplace(code, value).second)
		{
			error = "array records contain an invalid or duplicate code/value";
			return false;
		}
	}
	return true;
}
}

namespace wtp
{
std::size_t (*WtDiffExecuter::_save_write_test_hook)(void*, const void*, std::size_t) = NULL;
int (*WtDiffExecuter::_save_flush_test_hook)(void*) = NULL;
int (*WtDiffExecuter::_save_close_test_hook)(void*) = NULL;
}

/**
 * @brief 差量执行器构造函数
 *
 * @param factory 执行器工厂指针
 * @param name 执行器名称
 * @param dataMgr 数据管理器指针
 * @param bdMgr 基础数据管理器指针
 */
WtDiffExecuter::WtDiffExecuter(WtExecuterFactory* factory, const char* name, IDataManager* dataMgr, IBaseDataMgr* bdMgr)
	: IExecCommand(name)
	, _trader(NULL)
	, _factory(factory)
	, _data_mgr(dataMgr)
	, _bd_mgr(bdMgr)
	// Must be NULL before init(): getUnit() and failed-init paths test it, and
	// ASan's allocation fill exposed it as uninitialized (I03 eager validation).
	, _config(NULL)
, _scale(1.0)
	, _channel_ready(false)
	{
}


/**
 * @brief 差量执行器析构函数
 * @details 等待线程池中的任务完成再退出
 */
WtDiffExecuter::~WtDiffExecuter()
{
	requestQuiesce();
	const bool hostAndPluginsDrained = waitQuiesced();
	bool hostDrained = false;
	bool pluginsDrained = false;
	{
		std::lock_guard<std::mutex> lock(_quiesce_mutex);
		hostDrained = _quiesce_host_completed;
		pluginsDrained = _quiesce_plugins_completed;
	}
	if (!hostDrained || !pluginsDrained)
	{
		try
		{
			WTSLogger::error("Diff executor {} cannot be destroyed before host and plugin work is quiesced (wait result: {})",
				_name, hostAndPluginsDrained);
		}
		catch (...)
		{
		}
		// The unit wrappers borrow this executor as ExecuteContext. Continuing
		// destruction would release that context while plugin-owned work may still
		// reference it; fail closed before any member owner is released.
		std::terminate();
	}
	account_net_limits::Registry::instance().release(this);
}

WtDiffExecuter::QuiesceIngress::QuiesceIngress(WtDiffExecuter* owner)
	: _owner(owner)
	, _accepted(false)
	, _uncaught_exceptions(std::uncaught_exceptions())
	, _marker{ NULL, NULL }
{
	if (_owner != NULL)
		_accepted = _owner->enterQuiesceIngress(_marker);
}

bool WtDiffExecuter::quiesceIncomplete() const
{
	if (_quiesce_incomplete.load(std::memory_order_acquire))
		return true;
	std::lock_guard<std::mutex> lock(_clean_failure_mutex);
	return !_clean_failure_codes.empty();
}

WtDiffExecuter::QuiesceIngress::~QuiesceIngress()
{
	if (_accepted)
	{
		if (std::uncaught_exceptions() > _uncaught_exceptions)
			_owner->_quiesce_incomplete.store(true, std::memory_order_release);
		_owner->leaveQuiesceIngress(_marker);
	}
}

WtDiffExecuter::QuiesceDrainScope::QuiesceDrainScope(WtDiffExecuter* owner,
	bool reserved) noexcept
	: _owner(owner)
	, _reserved(reserved)
	, _entered(false)
	, _marker{ NULL, NULL }
{
}

WtDiffExecuter::QuiesceDrainScope::~QuiesceDrainScope()
{
	if (_owner == NULL)
		return;
	if (_entered)
		_owner->leaveQuiesceCallback(_marker);
	if (_reserved)
		_owner->finishQuiesceDrain();
}

void WtDiffExecuter::QuiesceDrainScope::enter() noexcept
{
	if (_owner == NULL || _entered)
		return;
	_owner->enterQuiesceCallback(_marker);
	_entered = true;
}

WtDiffExecuter::QuiesceThreadMarker*& WtDiffExecuter::currentQuiesceMarker() noexcept
{
	static thread_local QuiesceThreadMarker* current = NULL;
	return current;
}

bool WtDiffExecuter::enterQuiesceIngress(QuiesceThreadMarker& marker)
{
	{
		std::lock_guard<std::mutex> lock(_quiesce_mutex);
		if (_quiesce_requested.load(std::memory_order_acquire))
			return false;
		++_quiesce_active_ingress;
	}
	marker.owner = this;
	marker.previous = currentQuiesceMarker();
	currentQuiesceMarker() = &marker;
	return true;
}

void WtDiffExecuter::leaveQuiesceIngress(QuiesceThreadMarker& marker)
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
		if (_quiesce_active_ingress != 0)
			--_quiesce_active_ingress;
	}
	_quiesce_condition.notify_all();
}

void WtDiffExecuter::reserveQuiesceDrain()
{
	std::lock_guard<std::mutex> lock(_quiesce_mutex);
	++_quiesce_active_drains;
}

void WtDiffExecuter::finishQuiesceDrain()
{
	{
		std::lock_guard<std::mutex> lock(_quiesce_mutex);
		if (_quiesce_active_drains != 0)
			--_quiesce_active_drains;
	}
	_quiesce_condition.notify_all();
}

void WtDiffExecuter::enterQuiesceCallback(QuiesceThreadMarker& marker) noexcept
{
	marker.owner = this;
	marker.previous = currentQuiesceMarker();
	currentQuiesceMarker() = &marker;
}

void WtDiffExecuter::leaveQuiesceCallback(QuiesceThreadMarker& marker) noexcept
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

void WtDiffExecuter::retainQuiesceUnit(const ExecuteUnitPtr& unit)
{
	if (unit == NULL)
		return;

	bool added = false;
	{
		SpinLock lock(_mtx_units);
		const auto it = std::find(_quiesce_unit_owners.begin(),
			_quiesce_unit_owners.end(), unit);
		if (it == _quiesce_unit_owners.end())
		{
			_quiesce_unit_owners.push_back(unit);
			_quiesce_owner_generation.fetch_add(1, std::memory_order_release);
			added = true;
		}
	}

	// A creator may finish initialization after the ingress gate closes. Its
	// accepted lease keeps the host drain from completing before this owner is
	// retained. Plugin requests belong to the coordinator's later stable-snapshot
	// phase, so this path only records the owner.
	if (added && _quiesce_requested.load(std::memory_order_acquire))
	{
		{
			std::lock_guard<std::mutex> lock(_quiesce_mutex);
			_quiesce_plugins_completed = false;
			_quiesced = false;
			_quiesce_snapshot_taken = false;
		}
		_quiesce_condition.notify_all();
	}
}

std::vector<ExecuteUnitPtr> WtDiffExecuter::snapshotQuiesceUnits() const
{
	std::vector<ExecuteUnitPtr> units;
	SpinLock lock(_mtx_units);
	units = _quiesce_unit_owners;
	return units;
}

void WtDiffExecuter::requestQuiesce()
{
	// The lease's admission check reads this atomic while holding the ingress
	// mutex. A lease that observed false before this store is already admitted
	// and counted before it releases that mutex; the host drain waits for it.
	// Closing the gate itself therefore needs no blocking mutex acquisition.
	_quiesce_requested.store(true, std::memory_order_release);
	_quiesce_condition.notify_all();
}

WtExecuterFactory* WtDiffExecuter::quiesceFactory() const
{
	return _factory;
}

bool WtDiffExecuter::canWaitQuiescedOnCurrentThread() const
{
	if (!_quiesce_requested.load(std::memory_order_acquire) ||
		!preflightQuiesceOnCurrentThread())
		return false;

	std::unique_lock<std::mutex> lock(_quiesce_mutex, std::try_to_lock);
	if (!lock.owns_lock())
		return false;
	if (_quiesced && !quiesceIncomplete())
		return true;
	lock.unlock();

	// Owners are retained monotonically for the lifetime of this executor. The
	// atomic generation therefore answers whether the legacy single-command
	// wait can finish without blocking on plugin-owned work.
	return _quiesce_owner_generation.load(std::memory_order_acquire) == 0;
}

bool WtDiffExecuter::preflightQuiesceOnCurrentThread() const
{
	for (QuiesceThreadMarker* marker = currentQuiesceMarker(); marker != NULL;
		marker = marker->previous)
	{
		if (marker->owner == this)
			return false;
	}
	return true;
}

bool WtDiffExecuter::drainHostQuiesced()
{
	for (QuiesceThreadMarker* marker = currentQuiesceMarker(); marker != NULL;
		marker = marker->previous)
	{
		if (marker->owner == this)
			return false;
	}

	{
		std::unique_lock<std::mutex> lock(_quiesce_mutex);
		if (!_quiesce_requested.load(std::memory_order_acquire))
			return false;
		if (_quiesce_host_completed)
			return !quiesceIncomplete();
		if (_quiesce_wait_in_progress)
		{
			_quiesce_condition.wait(lock, [this]() { return !_quiesce_wait_in_progress; });
			return _quiesce_host_completed &&
				!quiesceIncomplete();
		}
		_quiesce_wait_in_progress = true;
		_quiesce_condition.wait(lock, [this]() {
			return _quiesce_active_ingress == 0 && _quiesce_active_drains == 0;
		});
	}

	// Callback reservations cover task execution. Waiting on the pool as well
	// waits for each accepted worker closure to return after its drain completes.
	bool poolWaitSucceeded = true;
	try
	{
		if (_pool)
			_pool->wait();
	}
	catch (...)
	{
		poolWaitSucceeded = false;
		_quiesce_incomplete.store(true, std::memory_order_release);
	}
	if (poolWaitSucceeded)
	{
		std::lock_guard<std::mutex> lock(_quiesce_mutex);
		_quiesce_host_completed = true;
	}

	{
		std::lock_guard<std::mutex> lock(_quiesce_mutex);
		_quiesce_host_completed = poolWaitSucceeded;
		_quiesce_wait_in_progress = false;
	}
	_quiesce_condition.notify_all();
	return poolWaitSucceeded && !quiesceIncomplete();
}

bool WtDiffExecuter::snapshotQuiescePluginOwners(
	std::vector<ExecQuiesceOwnerPtr>& out) const
{
	out.clear();
	{
		std::lock_guard<std::mutex> lock(_quiesce_mutex);
		if (!_quiesce_requested.load(std::memory_order_acquire) ||
			!_quiesce_host_completed || _quiesce_active_ingress != 0 ||
			_quiesce_active_drains != 0)
			return false;
	}

	std::vector<ExecuteUnitPtr> units;
	uint64_t generation = 0;
	try
	{
		SpinLock lock(_mtx_units);
		units = _quiesce_unit_owners;
		generation = _quiesce_owner_generation.load(std::memory_order_acquire);
	}
	catch (...)
	{
		return false;
	}

	try
	{
		out.reserve(units.size());
		for (const ExecuteUnitPtr& unit : units)
		{
			if (unit == NULL)
			{
				out.clear();
				return false;
			}
			std::shared_ptr<IExecCommandQuiesce> quiesce =
				std::static_pointer_cast<IExecCommandQuiesce>(unit);
			const auto duplicate = std::find_if(out.begin(), out.end(),
				[quiesce](const ExecQuiesceOwnerPtr& existing) {
					return existing.get() == quiesce.get();
				});
			if (duplicate == out.end())
				out.push_back(std::move(quiesce));
		}
	}
	catch (...)
	{
		out.clear();
		return false;
	}

	// Hold the owner lock through snapshot publication. Otherwise a new owner
	// could be retained after the generation check but before the snapshot cache
	// is updated, causing this call to report a stale successful snapshot.
	{
		SpinLock unitsLock(_mtx_units);
		if (_quiesce_owner_generation.load(std::memory_order_acquire) != generation)
		{
			out.clear();
			return false;
		}
		std::unique_lock<std::mutex> lock(_quiesce_mutex, std::try_to_lock);
		if (!lock.owns_lock())
		{
			out.clear();
			return false;
		}
		if (!_quiesce_host_completed || _quiesce_active_ingress != 0 ||
			_quiesce_active_drains != 0)
		{
			out.clear();
			return false;
		}
		_quiesce_snapshot_generation = generation;
		_quiesce_snapshot_taken = true;
	}
	return true;
}

bool WtDiffExecuter::completeQuiesceAfterGlobalPluginWait()
{
	// The coordinator calls this only after every owner returned successful wait.
	// Revalidate that no owner was added after the stable snapshot before allowing
	// the executor and its borrowed context to be reclaimed.
	std::unique_lock<std::mutex> quiesceLock(_quiesce_mutex, std::try_to_lock);
	if (!quiesceLock.owns_lock())
		return false;
	if (!_quiesce_requested.load(std::memory_order_acquire) ||
		!_quiesce_host_completed || !_quiesce_snapshot_taken ||
		_quiesce_active_ingress != 0 || _quiesce_active_drains != 0 ||
		_quiesce_owner_generation.load(std::memory_order_acquire) !=
			_quiesce_snapshot_generation ||
		quiesceIncomplete())
		return false;

	_quiesce_plugins_completed = true;
	_quiesced = true;
	_quiesce_condition.notify_all();
	return true;
}

bool WtDiffExecuter::waitQuiesced()
{
	// A callback/worker re-entering the legacy wait API must return immediately,
	// before it can contend on the state mutex or invoke any factory probe.
	for (QuiesceThreadMarker* marker = currentQuiesceMarker(); marker != NULL;
		marker = marker->previous)
	{
		if (marker->owner == this)
			return false;
	}
	{
		std::lock_guard<std::mutex> lock(_quiesce_mutex);
		if (_quiesced)
			return !quiesceIncomplete();
	}
	// Keep the legacy wait entry point safe from host callback reentry. Factory
	// worker preflight belongs to the coordinator's deduplicated factory phase.
	if (!preflightQuiesceOnCurrentThread())
		return false;

	const bool hostDrained = drainHostQuiesced();
	std::vector<ExecQuiesceOwnerPtr> owners;
	if (!snapshotQuiescePluginOwners(owners))
		return false;

	if (!owners.empty())
	{
		std::lock_guard<std::mutex> lock(_quiesce_mutex);
		_quiesce_plugins_completed = false;
		_quiesced = false;
		return false;
	}

	const bool quiesced = hostDrained &&
		!quiesceIncomplete();
	{
		std::lock_guard<std::mutex> lock(_quiesce_mutex);
		_quiesce_plugins_completed = true;
		_quiesced = quiesced;
	}
	_quiesce_condition.notify_all();
	return quiesced;
}

/**
 * @brief 设置交易适配器
 *
 * @param adapter 交易适配器指针
 * @details 仅记录交易适配器；通道就绪事件由通知接收器注册后的适配器回调发送
 */
void WtDiffExecuter::setTrader(TraderAdapter* adapter)
{
	_trader = adapter;
	if (_trader != NULL)
		_trader->declareOwnerSink(this, ExecOwnerModes::ownerId(name()));

	// Card 8 / BUG-P02-005: targets restored by load_data() are already
	// committed; publish them (unchecked) so the other executers of this
	// account include them in the shared per-contract net-limit judgement.
	const std::shared_ptr<const void> accountKeepalive = _trader ?
		std::shared_ptr<const void>(_trader->accountGate()) : std::shared_ptr<const void>();
	const void* accountKey = accountKeepalive ? accountKeepalive.get() : static_cast<const void*>(this);
	std::lock_guard<std::mutex> updateLock(_position_update_mutex);
	std::lock_guard<std::mutex> positionLock(_position_mutex);
	account_net_limits::Positions accountTargets;
	for (const auto& item : _target_pos)
		if (item.second != 0.0) accountTargets[item.first] = item.second;
	account_net_limits::Positions previousAccount;
	const void* previousAccountKey = NULL;
	std::string ignored;
	(void)account_net_limits::Registry::instance().apply(accountKey, accountKeepalive, this, _name,
		_account_limits, accountTargets, previousAccount, previousAccountKey, ignored, true);
}

/**
 * @brief 初始化差量执行器
 *
 * @param params 配置参数
 * @return bool 初始化是否成功
 * @details 设置缩放比例、创建线程池并加载数据
 */
bool WtDiffExecuter::validateConfig(WTSVariant* params, std::string& error)
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
	account_net_limits::Limits accountLimits;
	if (!account_net_limits::parse(params, accountLimits, error))
		return false;
	return exec_policy_config::validatePolicy(params, error);
}

bool WtDiffExecuter::init(WTSVariant* params)
{
	// I03-NEW-070: a first init() that does not complete leaves the executor in a
	// failed state; ingestion and snapshot saves are then refused.
	const bool firstInit = (_config == NULL);
	struct InitOutcome
	{
		WtDiffExecuter* owner;
		bool firstInit;
		bool succeeded = false;
		~InitOutcome()
		{
			if (firstInit)
				owner->_init_failed.store(!succeeded, std::memory_order_release);
		}
	} outcome{ this, firstInit };

	if (params == NULL)
		return false;

	// Card 8b=A: validate the complete configuration before retaining it,
	// creating the thread pool or reading the persisted snapshot.
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

	account_net_limits::Limits accountLimits;
	std::string accountError;
	if (!account_net_limits::parse(params, accountLimits, accountError))
	{
		WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR, "Invalid account limits: {}", accountError);
		return false;
	}
	_account_limits = accountLimits;

	_config = params;
	_config->retain();

	_scale = scale;

	uint32_t poolsize = params->getUInt32("poolsize");
	if(poolsize > 0)
	{
		_pool.reset(new boost::threadpool::pool(poolsize));
	}

	if (!load_data())
	{
		_config->release();
		_config = NULL;
		_pool.reset();
		return false;
	}

	WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}] Diff executer inited, scale: {}, thread poolsize: {}", _name, _scale, poolsize);

	outcome.succeeded = true;
	return true;
}

/**
 * @brief 加载执行器数据
 *
 * @details 从文件中读取执行器的目标仓位和差量数据
 *          数据存储在JSON格式的文件中，包含“targets”和“diffs”两部分
 */
bool WtDiffExecuter::load_data()
{
	// Load into temporary containers and publish only after the complete snapshot
	// (including optional failure tombstones) has passed schema validation.
	std::string filename = WtHelper::getExecDataDir();
	filename += _name + ".json";

	std::error_code existsError;
	const bool exists = std::filesystem::exists(filename, existsError);
	if (existsError)
	{
		WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
			"[{}] Cannot inspect Diff snapshot {}: {}", _name, filename, existsError.message());
		return false;
	}
	if (!exists)
		return true;

	std::string content;
	try
	{
		StdFile::read_file_content(filename.c_str(), content);
	}
	catch (const std::exception& error)
	{
		WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
			"[{}] Cannot read Diff snapshot {}: {}", _name, filename, error.what());
		return false;
	}

	rj::Document root;
	root.Parse(content.data(), content.size());

	std::map<std::string, double> parsedTargets;
	std::map<std::string, double> parsedDiffs;
	std::map<std::string, std::shared_ptr<UnitLifecycleState>> parsedBlockedUnits;
	std::string schemaError;
	const rj::Value* jTargets = NULL;
	const rj::Value* jDiffs = NULL;
	if (root.HasParseError())
	{
		schemaError = "invalid JSON syntax";
	}
	else if (!root.IsObject())
	{
		schemaError = "snapshot root must be an object";
	}
	else
	{
		jTargets = getUniqueMember(root, "targets");
		jDiffs = getUniqueMember(root, "diffs");
		if (jTargets == NULL || jDiffs == NULL)
		{
			schemaError = "required targets and diffs members must each appear exactly once";
		}
		else if (jTargets->IsArray() != jDiffs->IsArray() ||
			jTargets->IsObject() != jDiffs->IsObject())
		{
			schemaError = "targets and diffs must use the same supported snapshot format";
		}
		else if (!parseDiffPositionSection(*jTargets, "target", parsedTargets, schemaError) ||
			!parseDiffPositionSection(*jDiffs, "diff", parsedDiffs, schemaError))
		{
			// Preserve the first schema error reported by the section parser.
		}
		else if (!hasAtMostOneMember(root, "blocked_units"))
		{
			schemaError = "blocked_units member must not be duplicated";
		}
		else
		{
			const rj::Value* jBlockedUnits = getUniqueMember(root, "blocked_units");
			if (jBlockedUnits != NULL && !jBlockedUnits->IsArray())
			{
				schemaError = "blocked_units must be an array when present";
			}
			else if (jBlockedUnits != NULL)
			{
				for (const rj::Value& item : jBlockedUnits->GetArray())
				{
					const rj::Value* codeValue = getUniqueMember(item, "code");
					const rj::Value* generationValue = getUniqueMember(item, "generation");
					const rj::Value* errorValue = getUniqueMember(item, "error");
					if (!item.IsObject() || codeValue == NULL || !codeValue->IsString() ||
						codeValue->GetStringLength() == 0 ||
						!hasAtMostOneMember(item, "generation") ||
						!hasAtMostOneMember(item, "error") ||
						(generationValue != NULL && (!generationValue->IsUint64() || generationValue->GetUint64() == 0)) ||
						(errorValue != NULL && !errorValue->IsString()))
					{
						schemaError = "blocked_units entries require a unique string code and valid optional generation/error";
						break;
					}

					const std::string code(codeValue->GetString(), codeValue->GetStringLength());
					if (code.find('\0') != std::string::npos || parsedBlockedUnits.find(code) != parsedBlockedUnits.end())
					{
						schemaError = "blocked_units contains an invalid or duplicate code";
						break;
					}

					auto state = std::make_shared<UnitLifecycleState>();
					state->std_code = code;
					state->creation_complete = true;
					state->callback_failed = true;
					state->retry_blocked = true;
					state->init_status = ExecUnitInitState::FAILED;
					state->init_generation = generationValue != NULL ? generationValue->GetUint64() : 1;
					state->init_error = errorValue != NULL ?
						std::string(errorValue->GetString(), errorValue->GetStringLength()) :
						"persisted failure tombstone; automatic retry disabled";
					parsedBlockedUnits.emplace(code, state);
				}
			}
		}
	}

	if (!schemaError.empty())
	{
		WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
			"[{}] Rejected Diff snapshot {}: {}", _name, filename, schemaError);
		return false;
	}

	// Retain the established behavior for structurally valid entries whose
	// contract is absent from the current base-data set, but make all parsing
	// failures above atomic and fail the executor initialization visibly.
	wt_hashmap<std::string, double> loadedTargets;
	wt_hashmap<std::string, double> loadedDiffs;
	for (const auto& item : parsedTargets)
	{
		CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(item.first.c_str(), NULL);
		WTSContractInfo* contract = codeInfo._code[0] != '\0' && codeInfo._exchg[0] != '\0' && _bd_mgr ?
			_bd_mgr->getContract(codeInfo._code, codeInfo._exchg) : NULL;
		if (contract == NULL)
		{
			WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO,
				"[{}] Ticker {} is not valid", _name, item.first);
			continue;
		}
		loadedTargets.emplace(item.first, item.second);
	}
	for (const auto& item : parsedDiffs)
	{
		CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(item.first.c_str(), NULL);
		WTSContractInfo* contract = codeInfo._code[0] != '\0' && codeInfo._exchg[0] != '\0' && _bd_mgr ?
			_bd_mgr->getContract(codeInfo._code, codeInfo._exchg) : NULL;
		if (contract == NULL)
		{
			WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO,
				"[{}] Ticker {} is not valid", _name, item.first);
			continue;
		}
		loadedDiffs.emplace(item.first, item.second);
	}

	std::map<std::string, uint64_t> loadedTargetVersions;
	uint64_t targetGeneration = 0;
	for (const auto& item : loadedTargets)
		loadedTargetVersions.emplace(item.first, ++targetGeneration);

	uint64_t nextUnitGeneration = 1;
	for (const auto& item : parsedBlockedUnits)
	{
		const uint64_t generation = item.second->init_generation;
		if (generation == (std::numeric_limits<uint64_t>::max)())
		{
			WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
				"[{}] Rejected Diff snapshot {}: blocked unit generation overflows", _name, filename);
			return false;
		}
		nextUnitGeneration = (std::max)(nextUnitGeneration, generation + 1);
	}

	{
		std::lock_guard<std::mutex> positionsLock(_position_mutex);
		SpinLock unitsLock(_mtx_units);
		_target_pos.swap(loadedTargets);
		_diff_pos.swap(loadedDiffs);
		_target_versions.swap(loadedTargetVersions);
		_target_update_generation = targetGeneration;
		_unit_states.swap(parsedBlockedUnits);
		_next_unit_init_generation = nextUnitGeneration;
	}
	return true;
}

/**
 * @brief 保存执行器数据
 *
 * @details 将目标仓位和差量数据保存到JSON格式的文件中
 *          文件包含“targets”和“diffs”两部分，分别存储目标仓位和差量仓位
 */
bool WtDiffExecuter::save_data()
{
	std::lock_guard<std::mutex> updateLock(_position_update_mutex);
	return save_data_locked();
}

bool WtDiffExecuter::save_data_locked()
{
	// The commit lock is acquired before taking any data snapshot. All position
	// and failure-state mutations use _position_update_mutex before this lock, so
	// an older snapshot cannot commit after a newer failure transition.
	std::lock_guard<std::mutex> saveLock(_save_mutex);
	if (_init_failed.load(std::memory_order_acquire))
	{
		WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
			"[{}] Diff snapshot is not written because init failed; the rejected file is kept for review", _name);
		return false;
	}
	struct BlockedUnitSnapshot
	{
		std::string code;
		uint64_t generation;
		std::string error;
	};
	std::vector<std::pair<std::string, double>> targets;
	std::vector<std::pair<std::string, double>> diffs;
	std::vector<BlockedUnitSnapshot> blockedUnits;
	uint64_t revision = 0;
	{
		std::lock_guard<std::mutex> lock(_position_mutex);
		revision = _position_revision;
		targets.reserve(_target_pos.size());
		diffs.reserve(_diff_pos.size());
		for (const auto& item : _target_pos)
			targets.emplace_back(item.first, item.second);
		for (const auto& item : _diff_pos)
			diffs.emplace_back(item.first, item.second);
	}
	{
		SpinLock unitsLock(_mtx_units);
		blockedUnits.reserve(_unit_states.size());
		for (const auto& item : _unit_states)
		{
			const std::shared_ptr<UnitLifecycleState>& state = item.second;
			std::lock_guard<std::mutex> stateLock(state->mutex);
			if (state->creation_complete && state->init_status == ExecUnitInitState::FAILED &&
				state->retry_blocked)
				blockedUnits.push_back({ item.first, state->init_generation, state->init_error });
		}
	}

	if (revision <= _saved_position_revision)
		return true;

	rj::Document root(rj::kObjectType);
	rj::Document::AllocatorType &allocator = root.GetAllocator();

	{//目标持仓数据保存
		rj::Value jTarget(rj::kArrayType);

		for (const auto& v : targets)
		{
			rj::Value jItem(rj::kObjectType);
			jItem.AddMember("code", rj::Value(v.first.c_str(), allocator), allocator);
			jItem.AddMember("target", v.second, allocator);

			jTarget.PushBack(jItem, allocator);
		}

		root.AddMember("targets", jTarget, allocator);
	}

	{//差量持仓数据保存
		rj::Value jDiff(rj::kArrayType);

		for (const auto& v : diffs)
		{
			rj::Value jItem(rj::kObjectType);
			jItem.AddMember("code", rj::Value(v.first.c_str(), allocator), allocator);
			jItem.AddMember("diff", v.second, allocator);

			jDiff.PushBack(jItem, allocator);
		}

		root.AddMember("diffs", jDiff, allocator);
	}

	{//失败状态持久化，阻止重启后自动重放目标仓位
		rj::Value jBlocked(rj::kArrayType);
		for (const BlockedUnitSnapshot& item : blockedUnits)
		{
			rj::Value jItem(rj::kObjectType);
			jItem.AddMember("code", rj::Value(item.code.c_str(), allocator), allocator);
			jItem.AddMember("generation", item.generation, allocator);
			jItem.AddMember("error", rj::Value(item.error.c_str(), allocator), allocator);
			jBlocked.PushBack(jItem, allocator);
		}
		root.AddMember("blocked_units", jBlocked, allocator);
	}
	if (_save_test_hook && !_save_test_hook(!blockedUnits.empty()))
	{
		WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
			"[{}] Injected Diff snapshot failure at revision {}; revision remains dirty",
			_name, revision);
		return false;
	}

	rj::StringBuffer sb;
	rj::PrettyWriter<rj::StringBuffer> writer(sb);
	root.Accept(writer);

	std::string filename = WtHelper::getExecDataDir();
	filename += _name + ".json";
	const std::string tempFilename = makeDiffTempFilename(filename);
	bool writeSucceeded = false;
	{
		std::FILE* output = std::fopen(tempFilename.c_str(), "wb");
		if (output != NULL)
		{
			const std::size_t bytesWrittenCount = _save_write_test_hook ?
				_save_write_test_hook(output, sb.GetString(), sb.GetSize()) :
				std::fwrite(sb.GetString(), 1, sb.GetSize(), output);
			const bool bytesWritten = bytesWrittenCount == sb.GetSize();
			const int flushResult = _save_flush_test_hook ?
				_save_flush_test_hook(output) : std::fflush(output);
			const bool flushed = flushResult == 0;
			const int closeResult = _save_close_test_hook ?
				_save_close_test_hook(output) : std::fclose(output);
			writeSucceeded = bytesWritten && flushed && closeResult == 0;
		}
	}
	if (!writeSucceeded)
	{
		std::remove(tempFilename.c_str());
		WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
			"[{}] Failed to write Diff snapshot {}; revision {} remains dirty",
			_name, filename, revision);
		return false;
	}
	if (!replaceDiffFileAtomically(tempFilename, filename))
	{
		std::remove(tempFilename.c_str());
		WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
			"[{}] Failed to replace Diff snapshot {}; revision {} remains dirty",
			_name, filename, revision);
		return false;
	}
	_saved_position_revision = revision;
	if (_save_after_commit_test_hook)
		_save_after_commit_test_hook();
	return true;
}

bool WtDiffExecuter::enqueueUnitCallback(const std::shared_ptr<UnitLifecycleState>& state,
	std::function<void()> callback)
{
	if (state == NULL || !callback)
		return false;

	std::lock_guard<std::mutex> lock(state->mutex);
	if (state->callback_failed)
	{
		_quiesce_incomplete.store(true, std::memory_order_release);
		return false;
	}
	state->callback_events.push_back(std::move(callback));
	if (!state->callback_active && state->creation_complete && !state->callback_drain_scheduled)
	{
		state->callback_drain_scheduled = true;
		return true;
	}
	return false;
}

bool WtDiffExecuter::deferPendingUnitCallback(const std::string& code,
	std::function<void(const ExecuteUnitPtr&)> callback)
{
	// I03-NEW-072 (card 9d=A): an adapter callback never waits for a creation in
	// flight (on another thread, or re-entrantly on this one). The event joins
	// that creation instead: before registration it is kept with the generation
	// and queued behind the registration callbacks; after registration it joins
	// the FIFO that the creator drains before publishing READY. Holding the unit
	// registry and state locks keeps both the completion and the failure path
	// from running in between. Returns false when no creation is pending, so the
	// caller resolves the unit again.
	if (!callback)
		return false;
	SpinLock lock(_mtx_units);
	auto stateIt = _unit_states.find(code);
	if (stateIt == _unit_states.end())
		return false;
	const std::shared_ptr<UnitLifecycleState> state = stateIt->second;
	std::lock_guard<std::mutex> stateLock(state->mutex);
	if (state->creation_complete)
		return false;
	if (state->callback_failed)
	{
		_quiesce_incomplete.store(true, std::memory_order_release);
		return true;
	}
	auto unitIt = _unit_map.find(code);
	if (unitIt == _unit_map.end())
	{
		state->deferred_events.push_back(std::move(callback));
		return true;
	}
	ExecuteUnitPtr registered = unitIt->second;
	state->callback_events.push_back([registered, callback]() { callback(registered); });
	return true;
}

void WtDiffExecuter::scheduleUnitCallbackDrain(const ExecuteUnitPtr& unit,
	const std::shared_ptr<UnitLifecycleState>& state, bool schedule)
{
	// A busy drain will consume newly queued work. Enqueuers do not wait here:
	// a plugin callback may be joining a thread that reentered this executor.
	if (!schedule || unit == NULL || state == NULL)
		return;
	try
	{
		reserveQuiesceDrain();
	}
	catch (...)
	{
		_quiesce_incomplete.store(true, std::memory_order_release);
		throw;
	}
	std::shared_ptr<std::atomic<bool>> reservation;
	try
	{
		reservation = std::make_shared<std::atomic<bool>>(true);
	}
	catch (...)
	{
		_quiesce_incomplete.store(true, std::memory_order_release);
		finishQuiesceDrain();
		throw;
	}
	auto drain = [this, unit, state, reservation]() {
		const bool reserved = reservation->exchange(false, std::memory_order_acq_rel);
		drainUnitChannelEvents(unit, state, reserved);
	};
	try
	{
		if (_pool)
			WtExecuterSchedule::scheduleOrRunFallback(*_pool, drain, drain);
		else
			drain();
	}
	catch (...)
	{
		_quiesce_incomplete.store(true, std::memory_order_release);
		if (reservation->exchange(false, std::memory_order_acq_rel))
			finishQuiesceDrain();
		throw;
	}
}

void WtDiffExecuter::markUnitFailed(const std::string& code,
	const std::shared_ptr<UnitLifecycleState>& state, const std::string& reason)
{
	std::lock_guard<std::mutex> updateLock(_position_update_mutex);
	markUnitFailedLocked(code, state, reason);
}

void WtDiffExecuter::markUnitFailedLocked(const std::string& code,
	const std::shared_ptr<UnitLifecycleState>& state, const std::string& reason)
{
	markUnitFailedInMemoryLocked(code, state, reason);
	if (!save_data_locked())
	{
		_quiesce_incomplete.store(true, std::memory_order_release);
		WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
			"[{}] Failure tombstone for {} could not be persisted; in-memory order dispatch remains blocked",
			_name, code);
	}
}

void WtDiffExecuter::markUnitFailedInMemoryLocked(const std::string& code,
	const std::shared_ptr<UnitLifecycleState>& state, const std::string& reason)
{
	_pending_target_notification.erase(code);
	std::shared_ptr<UnitLifecycleState> failedState = state;
	{
		SpinLock unitsLock(_mtx_units);
		auto stateIt = _unit_states.find(code);
		if (state != NULL && stateIt != _unit_states.end() && stateIt->second != state)
			failedState = stateIt->second;
		if (failedState == NULL && stateIt != _unit_states.end())
			failedState = stateIt->second;
		if (failedState == NULL)
		{
			failedState = std::make_shared<UnitLifecycleState>();
			failedState->std_code = code;
			failedState->init_generation = _next_unit_init_generation++;
			_unit_states[code] = failedState;
		}
		else if (stateIt == _unit_states.end())
			_unit_states[code] = failedState;
		std::lock_guard<std::mutex> stateLock(failedState->mutex);
		failedState->callback_failed = true;
		failedState->retry_blocked = true;
		if (!failedState->callback_events.empty())
			_quiesce_incomplete.store(true, std::memory_order_release);
		failedState->callback_events.clear();
		failedState->init_status = ExecUnitInitState::FAILED;
		failedState->init_error = reason;
		failedState->result.reset();
		failedState->creation_complete = true;
		failedState->creation_owner = std::thread::id();
		auto unitIt = _unit_map.find(code);
		if (unitIt != _unit_map.end())
			_unit_map.erase(unitIt);
	}
	if (_failure_mutation_test_hook)
		_failure_mutation_test_hook();
	failedState->condition.notify_all();
	{
		std::lock_guard<std::mutex> lock(_position_mutex);
		++_position_revision;
	}
}

std::shared_ptr<WtDiffExecuter::UnitLifecycleState> WtDiffExecuter::getUnitState(
	const char* stdCode, const ExecuteUnitPtr& unit) const
{
	if (stdCode == NULL || unit == NULL)
		return std::shared_ptr<UnitLifecycleState>();
	SpinLock lock(_mtx_units);
	auto unitIt = _unit_map.find(stdCode);
	if (unitIt == _unit_map.end() || unitIt->second != unit)
		return std::shared_ptr<UnitLifecycleState>();
	auto stateIt = _unit_states.find(stdCode);
	return stateIt == _unit_states.end() ? std::shared_ptr<UnitLifecycleState>() : stateIt->second;
}

bool WtDiffExecuter::ownsCode(const std::string& code)
{
	std::lock_guard<std::mutex> updateLock(_position_update_mutex);
	return ownsCodeLocked(code);
}

bool WtDiffExecuter::ownsCodeLocked(const std::string& code) const
{
	// A code remains owned if it has ever entered the target/diff ledger or has
	// a lifecycle record, including a failed-unit tombstone.
	{
		std::lock_guard<std::mutex> positionLock(_position_mutex);
		if (_target_pos.find(code) != _target_pos.end() ||
			_diff_pos.find(code) != _diff_pos.end())
			return true;
	}
	SpinLock unitsLock(_mtx_units);
	return _unit_map.find(code) != _unit_map.end() ||
		_unit_states.find(code) != _unit_states.end();
}

bool WtDiffExecuter::isRetryBlockedUnit(const std::string& code) const
{
	SpinLock lock(_mtx_units);
	auto stateIt = _unit_states.find(code);
	if (stateIt == _unit_states.end())
		return false;

	const std::shared_ptr<UnitLifecycleState>& state = stateIt->second;
	std::lock_guard<std::mutex> stateLock(state->mutex);
	return state->creation_complete && state->init_status == ExecUnitInitState::FAILED &&
		state->retry_blocked;
}

bool WtDiffExecuter::isCurrentUnit(const std::string& code, const ExecuteUnitPtr& unit,
	const std::shared_ptr<UnitLifecycleState>& state) const
{
	if (unit == NULL || state == NULL)
		return false;
	SpinLock lock(_mtx_units);
	auto unitIt = _unit_map.find(code);
	auto stateIt = _unit_states.find(code);
	return unitIt != _unit_map.end() && unitIt->second == unit &&
		stateIt != _unit_states.end() && stateIt->second == state;
}

void WtDiffExecuter::recoverUnitPosition(const std::string& code, const ExecuteUnitPtr& unit,
	const std::shared_ptr<UnitLifecycleState>& state, uint64_t channelEpoch,
	double diff, bool hasDiff)
{
	if (!hasDiff || !_channel_ready.load() || _channel_epoch.load() != channelEpoch ||
		!isCurrentUnit(code, unit, state))
		return;
	if (_trader && !_trader->checkOrderLimits(code.c_str()))
		return;

	if (_channel_ready.load() && _channel_epoch.load() == channelEpoch &&
		isCurrentUnit(code, unit, state) && unit->self() != NULL)
	{
		WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO,
			"[{}] Diff of {} recovered to {}", _name, code, diff);
		unit->self()->set_position(code.c_str(), diff);
	}
}

bool WtDiffExecuter::registerCreatedUnit(const std::string& code, const ExecuteUnitPtr& unit,
	const std::shared_ptr<UnitLifecycleState>& state)
{
	std::lock_guard<std::mutex> updateLock(_position_update_mutex);
	double recoveryDiff = 0.0;
	bool hasRecoveryDiff = false;
	{
		std::lock_guard<std::mutex> positionLock(_position_mutex);
		auto diffIt = _diff_pos.find(code);
		if (diffIt != _diff_pos.end())
		{
			recoveryDiff = diffIt->second;
			hasRecoveryDiff = true;
		}
	}
	SpinLock unitsLock(_mtx_units);
	auto stateIt = _unit_states.find(code);
	{
		std::lock_guard<std::mutex> stateLock(state->mutex);
		if (stateIt == _unit_states.end() || stateIt->second != state || state->callback_failed)
			return false;
	}
	_unit_map[code] = unit;
	const bool pendingTarget = _pending_target_notification.erase(code) != 0;
	if (_channel_ready.load())
	{
		const uint64_t channelEpoch = _channel_epoch.load();
		enqueueUnitCallback(state, [unit]() { unit->self()->on_channel_ready(); });
		enqueueUnitCallback(state, [this, code, unit, state, channelEpoch,
			recoveryDiff, hasRecoveryDiff]() {
			recoverUnitPosition(code, unit, state, channelEpoch, recoveryDiff, hasRecoveryDiff);
		});
	}
	else if (pendingTarget && hasRecoveryDiff)
	{
		enqueueUnitCallback(state, [this, code, unit, recoveryDiff]() {
			if (!_trader || _trader->checkOrderLimits(code.c_str()))
				unit->self()->set_position(code.c_str(), recoveryDiff);
		});
	}
	// I03-NEW-072: adapter callbacks that arrived during the creation follow the
	// unit's own channel/recovery callbacks, in arrival order.
	std::deque<std::function<void(const ExecuteUnitPtr&)>> deferred;
	{
		std::lock_guard<std::mutex> stateLock(state->mutex);
		deferred.swap(state->deferred_events);
	}
	for (auto& event : deferred)
	{
		std::function<void(const ExecuteUnitPtr&)> deliver = std::move(event);
		enqueueUnitCallback(state, [unit, deliver]() { deliver(unit); });
	}
	return true;
}

/**
 * @brief 获取指定合约的执行单元
 *
 * @param stdCode 标准合约代码
 * @param bAutoCreate 如果不存在是否自动创建
 * @return ExecuteUnitPtr 执行单元指针
 * @details 根据合约代码获取执行单元，如果不存在且bAutoCreate为true，
 *          则根据配置创建新的执行单元
 */
ExecuteUnitPtr WtDiffExecuter::getUnit(const char* stdCode, bool bAutoCreate /* = true */,
	bool waitForCreation /* = true */, bool* creationPending /* = NULL */)
{
	if (creationPending != NULL)
		*creationPending = false;
	if (stdCode == NULL || stdCode[0] == '\0')
	{
		WTSLogger::error("Cannot get diff execution unit for an empty code");
		return ExecuteUnitPtr();
	}

	const std::string code(stdCode);
	std::shared_ptr<UnitLifecycleState> state;
	ExecuteUnitPtr registeredDuringCreation;
	bool creator = false;
	auto newState = std::make_shared<UnitLifecycleState>();
	newState->creation_owner = std::this_thread::get_id();
	newState->std_code = code;
	{
		SpinLock lock(_mtx_units);
		auto pending = _unit_states.find(code);
		if (pending != _unit_states.end())
		{
			state = pending->second;
			std::lock_guard<std::mutex> stateLock(state->mutex);
			if (bAutoCreate && state->creation_complete &&
				state->init_status == ExecUnitInitState::FAILED && !state->retry_blocked)
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
		if (!state->creation_complete && !waitForCreation)
		{
			if (creationPending != NULL)
				*creationPending = true;
			return ExecuteUnitPtr();
		}
		const std::thread::id current = std::this_thread::get_id();
		if (!state->creation_complete &&
			(state->creation_owner == current || (state->callback_active && state->callback_owner == current)))
		{
			// Initialization has succeeded and the candidate is already registered.
			// A same-thread callback may safely query it while channel notification is
			// still being drained; before registration, reject reentry to avoid deadlock.
			if (registeredDuringCreation != NULL)
				return registeredDuringCreation;
			lock.unlock();
			WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
				"Reentrant diff unit creation for {} was rejected", stdCode);
			return ExecuteUnitPtr();
		}
		// This shared state belongs to one attempt generation. A failed attempt is
		// returned to every waiter; a later call retries only failures before any
		// plugin code could have produced an external side effect.
		const uint64_t waiting_generation = state->init_generation;
		++state->waiting_callers;
		state->condition.wait(lock, [&state]() { return state->creation_complete; });
		--state->waiting_callers;
		if (state->init_generation != waiting_generation)
			return ExecuteUnitPtr();
		return state->result;
	}

	ExecuteUnitPtr unit;
	auto failCreation = [&](const char* reason) -> ExecuteUnitPtr
	{
		std::unique_lock<std::mutex> updateLock(_position_update_mutex);
		_pending_target_notification.erase(code);
		ExecuteUnitPtr mappedUnit;
		{
			SpinLock lock(_mtx_units);
			auto unitIt = _unit_map.find(code);
			if (unitIt != _unit_map.end())
			{
				mappedUnit = unitIt->second;
				_unit_map.erase(unitIt);
			}
		}
		std::string failureReason = reason ? reason : "unknown execution-unit initialization failure";
		bool persistFailure = false;
		size_t droppedCallbacks = 0;
		// ABI 2.2 (IExecPluginV2.h): initialize returned failure normally and the
		// host's immediate destroy_unit also returned normally, so no plugin-owned
		// work or unit survives. Old-ABI and unproven failures stay fail-closed.
		const bool failedInitCleanupProven = unit != NULL &&
			unit->failedInitializationWasSafelyDestroyed();
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			// Targets and diffs are committed to the ledger before getUnit() runs
			// (submitPositionTargets), and registerCreatedUnit() snapshots the
			// latest diff under _position_update_mutex, which this failure path
			// also holds. A retry generation therefore cannot observe a stale
			// ledger. It is allowed only if this generation was never registered
			// and accepted no host callback work that would be discarded here.
			const bool acceptedHostWork = !state->callback_events.empty() ||
				state->callback_active || state->callback_failed || state->result != NULL;
			if (state->retry_blocked && failedInitCleanupProven && mappedUnit == NULL &&
				!acceptedHostWork)
				state->retry_blocked = false;
			persistFailure = state->retry_blocked;
			if (persistFailure)
				failureReason += " (automatic retry disabled: plugin side effects may have occurred)";
			state->result.reset();
			state->init_status = ExecUnitInitState::FAILED;
			state->init_error = failureReason;
			state->creation_complete = true;
			state->creation_owner = std::thread::id();
			droppedCallbacks = state->deferred_events.size() + state->callback_events.size();
			state->deferred_events.clear();
			state->callback_events.clear();
		}
		state->condition.notify_all();
		if (droppedCallbacks != 0)
			_quiesce_incomplete.store(true, std::memory_order_release);
		// K06=B: a failure that may have produced plugin side effects keeps quiesce
		// incomplete for good; a provably side-effect-free one only until a later
		// generation of the same code is created successfully.
		if (persistFailure)
			_quiesce_incomplete.store(true, std::memory_order_release);
		else
		{
			std::lock_guard<std::mutex> cleanLock(_clean_failure_mutex);
			_clean_failure_codes.insert(code);
		}
		if (persistFailure)
		{
			{
				std::lock_guard<std::mutex> lock(_position_mutex);
				++_position_revision;
			}
			if (!save_data_locked())
			{
				_quiesce_incomplete.store(true, std::memory_order_release);
				WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
					"[{}] Failure tombstone for {} could not be persisted; automatic retry is blocked only in memory",
					_name, code);
			}
		}
		updateLock.unlock();
		unit.reset();
		mappedUnit.reset();
		try
		{
			WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
				"Diff execution unit initialization for {} failed: {}", stdCode, failureReason);
			if (droppedCallbacks != 0)
				WTSLogger::log_dyn("executer", _name.c_str(), LL_WARN,
					"[{}] {} deferred callback(s) for {} were dropped because its execution unit failed to initialize",
					_name, droppedCallbacks, stdCode);
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

		// Factories and initialize callbacks execute plugin-owned code. Once
		// entered, a failure cannot safely replay this code's old target because
		// the plugin may already have emitted an external order.
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			state->retry_blocked = true;
		}
		bool pluginEntered = true;
		unit = _factory->createDiffExeUnit(name, pluginEntered);
		if (unit == NULL)
		{
			// A host-side factory rejection ran no plugin code; keep it retryable.
			if (!pluginEntered)
			{
				std::lock_guard<std::mutex> lock(state->mutex);
				state->retry_blocked = false;
			}
			return failCreation("execution-unit creation failed");
		}
		std::string error;
		if (!unit->initialize(this, stdCode, cfg, error, true) || unit->self() == NULL)
			return failCreation(error.empty() ? "plugin initialization failed" : error.c_str());
		_owner_modes.setSharedCapable(stdCode, unit->hostOrderIdentitySupported());
		retainQuiesceUnit(unit);

		if (!registerCreatedUnit(code, unit, state))
			return failCreation("execution-unit registration blocked by a prior failure");

		for (;;)
		{
			drainUnitChannelEvents(unit, state);
			bool failed = false;
			bool completed = false;
			{
				SpinLock lock(_mtx_units);
				std::lock_guard<std::mutex> stateLock(state->mutex);
				failed = state->callback_failed;
				if (!failed && state->callback_events.empty() && !state->callback_active)
				{
					state->result = unit;
					state->init_status = ExecUnitInitState::READY;
					state->init_error.clear();
					state->creation_complete = true;
					state->creation_owner = std::thread::id();
					completed = true;
				}
			}
			if (completed)
			{
				// K06=B: a successful later generation clears an earlier clean failure.
				std::lock_guard<std::mutex> cleanLock(_clean_failure_mutex);
				_clean_failure_codes.erase(code);
			}
			if (failed)
				return failCreation("channel callback threw during unit initialization");
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
		return failCreation("unexpected exception while creating diff execution unit");
	}
}

ExecUnitInitStatus WtDiffExecuter::queryUnitInitStatus(const char* stdCode) const
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
		// I03-NEW-070: an executer whose init failed rejects every target before a unit
		// is created; report that as a failed initialization rather than "no attempt".
		if (_init_failed.load(std::memory_order_acquire))
		{
			status.state = ExecUnitInitState::FAILED;
			status.error = _config == NULL ? "executor configuration is missing" : "executer initialization failed";
			return status;
		}
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

ExecUnitInitClearResult WtDiffExecuter::clearFailedUnitInitStatus(const char* stdCode)
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
	if (state->retry_blocked)
		return ExecUnitInitClearResult::RETRY_BLOCKED;

	_unit_states.erase(it);
	return ExecUnitInitClearResult::CLEARED;
}

void WtDiffExecuter::drainUnitChannelEvents(const ExecuteUnitPtr& unit,
	const std::shared_ptr<UnitLifecycleState>& state, bool reserved)
{
	QuiesceDrainScope quiesceDrain(this, reserved);
	if (unit == NULL || state == NULL)
		return;
	// Retain drain ownership across FIFO events. A nested call starts with
	// ownsDrain=false and returns instead of recursively dispatching callbacks.
	bool ownsDrain = false;
	for (;;)
	{
		std::function<void()> callback;
		{
			std::unique_lock<std::mutex> lock(state->mutex);
			if (!ownsDrain && state->callback_active)
			{
				if (state->callback_owner == std::this_thread::get_id())
					return;
				state->condition.wait(lock, [&state]() { return !state->callback_active; });
				continue;
			}
			if (!ownsDrain)
			{
				state->callback_drain_scheduled = false;
				if (state->callback_events.empty() || state->callback_failed)
					return;
				state->callback_active = true;
				state->callback_owner = std::this_thread::get_id();
				ownsDrain = true;
				quiesceDrain.enter();
			}
			callback = std::move(state->callback_events.front());
			state->callback_events.pop_front();
		}
		bool callback_threw = false;
		try
		{
			callback();
		}
		catch (...)
		{
			callback_threw = true;
			_quiesce_incomplete.store(true, std::memory_order_release);
			std::lock_guard<std::mutex> updateLock(_position_update_mutex);
			std::string failedCode;
			bool failedAtRuntime = false;
			{
				SpinLock unitsLock(_mtx_units);
				std::lock_guard<std::mutex> lock(state->mutex);
				failedCode = state->std_code;
				state->callback_failed = true;
				state->retry_blocked = true;
				state->callback_events.clear();
				if (state->creation_complete)
				{
					failedAtRuntime = true;
					state->init_status = ExecUnitInitState::FAILED;
					state->init_error = "execution-unit callback threw after initialization; "
						"automatic retry disabled because plugin side effects may have occurred";
					state->result.reset();
					auto unitIt = _unit_map.find(failedCode);
					if (unitIt != _unit_map.end() && unitIt->second == unit)
						_unit_map.erase(unitIt);
				}
			}
			if (failedAtRuntime)
			{
				{
					std::lock_guard<std::mutex> lock(_position_mutex);
					++_position_revision;
				}
				if (!save_data_locked())
					WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
						"[{}] Failure tombstone for {} could not be persisted; automatic retry is blocked only in memory",
						_name, failedCode);
				try
				{
					WTSLogger::error("Diff execution unit {} callback threw; unit marked failed", failedCode);
				}
				catch (...) {}
			}
		}
		bool finished = false;
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			if (state->callback_events.empty() || state->callback_failed)
			{
				state->callback_active = false;
				state->callback_owner = std::thread::id();
				state->condition.notify_all();
				finished = true;
			}
		}
		if (callback_threw)
		{
			try { WTSLogger::error("Diff execution-unit callback threw"); }
			catch (...) {}
		}
		if (finished)
			return;
	}
}


//////////////////////////////////////////////////////////////////////////
//ExecuteContext
/**
 * @brief 以下是ExecuteContext接口的实现
 * @details 这些方法提供了执行单元与执行器之间的交互接口
 */
#pragma region Context回调接口
/**
 * @brief 获取指定合约的tick数据切片
 *
 * @param stdCode 标准合约代码
 * @param count 需要获取的tick数量
 * @param etime 结束时间，默认为0
 * @return WTSTickSlice* tick数据切片指针
 */
WTSTickSlice* WtDiffExecuter::getTicks(const char* stdCode, uint32_t count, uint64_t /*etime*/ /* = 0 */)
{
	if (_data_mgr == NULL)
		return NULL;

	return _data_mgr->get_tick_slice(stdCode, count);
}

/**
 * @brief 获取指定合约的最新tick数据
 *
 * @param stdCode 标准合约代码
 * @return WTSTickData* 最新tick数据指针
 */
WTSTickData* WtDiffExecuter::grabLastTick(const char* stdCode)
{
	if (_data_mgr == NULL)
		return NULL;

	return _data_mgr->grab_last_tick(stdCode);
}

/**
 * @brief 获取指定合约的持仓量
 *
 * @param stdCode 标准合约代码
 * @param validOnly 是否只返回有效持仓，默认为true
 * @param flag 持仓标记，默认为3（同时返回多空持仓）
 * @return double 持仓量
 */
double WtDiffExecuter::getPosition(const char* stdCode, bool validOnly /* = true */, int32_t flag /* = 3 */)
{
	if (NULL == _trader)
		return 0.0;

	return _trader->getPosition(stdCode, validOnly, flag);
}

/**
 * @brief 获取指定合约的未完成委托数量
 *
 * @param stdCode 标准合约代码
 * @return double 未完成委托数量
 */
double WtDiffExecuter::getUndoneQty(const char* stdCode)
{
	if (NULL == _trader)
		return 0.0;

	if (_trader->isOwnerRoutingEnabled())
		return _trader->getUndoneQtyForOwner(stdCode, ExecOwnerModes::ownerId(name()));
	return _trader->getUndoneQty(stdCode);
}

/**
 * @brief 获取指定合约的订单列表
 *
 * @param stdCode 标准合约代码
 * @return OrderMap* 订单映射表指针
 */
OrderMap* WtDiffExecuter::getOrders(const char* stdCode)
{
	if (NULL == _trader)
		return NULL;

	if (_trader->isOwnerRoutingEnabled())
		return _trader->getOrdersForOwner(stdCode, ExecOwnerModes::ownerId(name()));
	return _trader->getOrders(stdCode);
}

/**
 * @brief 发送买入委托
 *
 * @param stdCode 标准合约代码
 * @param price 委托价格
 * @param qty 委托数量
 * @param bForceClose 是否强制平仓，默认为false
 * @return OrderIDs 订单ID列表
 */
OrderIDs WtDiffExecuter::buy(const char* stdCode, double price, double qty, bool bForceClose/* = false*/)
{
	if (stdCode == NULL || _trader == NULL || !_channel_ready || isRetryBlockedUnit(stdCode))
		return OrderIDs();

	// Card 5c: the order belongs to this executer (owner-verified cancels/routing).
	return _trader->buyAsOwner(ExecOwnerModes::ownerId(name()), _owner_modes.sharedCapable(stdCode),
		stdCode, price, qty, 0, bForceClose);
}

/**
 * @brief 发送卖出委托
 *
 * @param stdCode 标准合约代码
 * @param price 委托价格
 * @param qty 委托数量
 * @param bForceClose 是否强制平仓，默认为false
 * @return OrderIDs 订单ID列表
 */
OrderIDs WtDiffExecuter::sell(const char* stdCode, double price, double qty, bool bForceClose/* = false*/)
{
	if (stdCode == NULL || _trader == NULL || !_channel_ready || isRetryBlockedUnit(stdCode))
		return OrderIDs();

	return _trader->sellAsOwner(ExecOwnerModes::ownerId(name()), _owner_modes.sharedCapable(stdCode),
		stdCode, price, qty, 0, bForceClose);
}

/**
 * @brief 根据本地订单ID撤销订单
 *
 * @param localid 本地订单ID
 * @return bool 撤单是否成功
 */
bool WtDiffExecuter::cancel(uint32_t localid)
{
	if (!_channel_ready)
		return false;

	// Owner-verified: never another owner's order; while paused only verified identities.
	return _trader->cancelForOwner(localid, ExecOwnerModes::ownerId(name()));
}

/**
 * @brief 根据合约代码、买卖方向和数量撤销订单
 *
 * @param stdCode 合约代码
 * @param isBuy 是否为买单
 * @param qty 要撤销的数量
 * @return OrderIDs 撤销的订单ID列表
 */
OrderIDs WtDiffExecuter::cancel(const char* stdCode, bool isBuy, double qty)
{
	if (!_channel_ready)
		return OrderIDs();

	return _trader->cancelForOwner(stdCode, isBuy, qty, ExecOwnerModes::ownerId(name()));
}

/**
 * @brief 写入日志
 *
 * @param message 日志消息
 * @details 将消息写入日志系统，并自动添加执行器名称前缀
 */
void WtDiffExecuter::writeLog(const char* message)
{
	static thread_local char szBuf[2048] = { 0 };
	fmtutil::format_to(szBuf, "[{}] {}", _name.c_str(), message);
	WTSLogger::log_dyn_raw("executer", _name.c_str(), LL_INFO, szBuf);
}

/**
 * @brief 获取商品信息
 *
 * @param stdCode 标准合约代码
 * @return WTSCommodityInfo* 商品信息指针
 * @details 从引擎中获取指定合约的商品信息
 */
WTSCommodityInfo* WtDiffExecuter::getCommodityInfo(const char* stdCode)
{
	return _stub->get_comm_info(stdCode);
}

/**
 * @brief 获取交易时段信息
 *
 * @param stdCode 标准合约代码
 * @return WTSSessionInfo* 交易时段信息指针
 * @details 从引擎中获取指定合约的交易时段信息
 */
WTSSessionInfo* WtDiffExecuter::getSessionInfo(const char* stdCode)
{
	return _stub->get_sess_info(stdCode);
}

/**
 * @brief 获取当前时间
 *
 * @return uint64_t 当前时间戳
 * @details 从引擎中获取当前实时时间
 */
uint64_t WtDiffExecuter::getCurTime()
{
	return _stub->get_real_time();
	//return TimeUtils::makeTime(_stub->get_date(), _stub->get_raw_time() * 100000 + _stub->get_secs());
}

#pragma endregion Context回调接口
//ExecuteContext
//////////////////////////////////////////////////////////////////////////


/**
 * @brief 以下是外部接口的实现
 * @details 这些方法提供了与外部系统交互的接口，包括处理仓位变化、行情更新、交易回报等
 */
#pragma region 外部接口
/**
 * @brief 处理仓位变化
 *
 * @param stdCode 标准合约代码
 * @param diffPos 仓位变化量
 * @details 当收到仓位变化通知时，更新目标仓位和差量仓位，
 *          并通知相应的执行单元进行处理
 */
WtDiffExecuter::BatchRegistration::~BatchRegistration()
{
	if (ticket != 0)
	{
		std::lock_guard<std::mutex> lock(owner->_position_update_mutex);
		owner->_active_batch_targets.erase(ticket);
	}
}

void WtDiffExecuter::on_position_changed(const char* stdCode, double diffPos)
{
	QuiesceIngress ingress(this);
	if (!ingress || stdCode == NULL || stdCode[0] == '\0' || diffPos == 0.0 ||
		!std::isfinite(diffPos) || !std::isfinite(diffPos * _scale))
		return;
	if (_init_failed.load(std::memory_order_acquire))
	{
		WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
			"[{}] Position change of {} rejected: executer init failed", _name, stdCode);
		return;
	}
	wt_hashmap<std::string, double> targets;
	targets.emplace(stdCode, diffPos * _scale);
	submitPositionTargets(targets, true);
}

void WtDiffExecuter::set_position(const wt_hashmap<std::string, double>& targets)
{
	QuiesceIngress ingress(this);
	if (!ingress)
		return;
	if (_init_failed.load(std::memory_order_acquire))
	{
		WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
			"[{}] {} target(s) rejected: executer init failed", _name, targets.size());
		return;
	}
	wt_hashmap<std::string, double> scaled;
	for (const auto& target : targets)
	{
		const double value = target.second * _scale;
		if (target.first.empty() || !std::isfinite(target.second) || !std::isfinite(value))
			return;
		scaled.emplace(target.first, value);
	}
	submitPositionTargets(scaled, false);
}

void WtDiffExecuter::submitPositionTargets(const wt_hashmap<std::string, double>& targets,
	bool incremental)
{
	struct Change { std::string code; double diff; bool notify; };
	struct Drain { ExecuteUnitPtr unit; std::shared_ptr<UnitLifecycleState> state; bool schedule; };
	uint64_t ticket = 0;
	uint64_t incrementalWatermark = 0;
	std::set<std::string> omittedCandidates;
	BatchRegistration registration(this);
	if (!incremental)
	{
		std::lock_guard<std::mutex> updateLock(_position_update_mutex);
		ticket = _next_batch_ticket++;
		incrementalWatermark = _incremental_generation;
		auto inserted = _active_batch_targets.emplace(ticket, std::set<std::string>());
		registration.ticket = ticket;
		for (const auto& target : targets)
			inserted.first->second.insert(target.first);
		for (const auto& earlier : _active_batch_targets)
			if (earlier.first < ticket)
				omittedCandidates.insert(earlier.second.begin(), earlier.second.end());
		std::lock_guard<std::mutex> positionLock(_position_mutex);
		for (const auto& target : _target_pos)
			omittedCandidates.insert(target.first);
	}
	if (!incremental && _batch_preflight_test_hook)
		_batch_preflight_test_hook(ticket);

	std::set<std::string> clearable;
	for (const std::string& code : omittedCandidates)
	{
		if (targets.find(code) != targets.end()) continue;
		if (_omitted_sweep_test_hook) _omitted_sweep_test_hook(code);
		CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(code.c_str(), NULL);
		WTSContractInfo* contract = _bd_mgr ? _bd_mgr->getContract(codeInfo._code, codeInfo._exchg) : NULL;
		if (contract != NULL)
			clearable.insert(code);
		else
			WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
				"[{}] Omitted target for {} cannot be cleared because contract metadata is unavailable; existing target and diff are retained", _name, code);
	}

	std::vector<Change> changes;
	std::vector<Change> process;
	std::vector<Drain> drains;
	std::vector<std::string> createCodes;
	bool persisted = true;
	const std::shared_ptr<const void> accountKeepalive = _trader ?
		std::shared_ptr<const void>(_trader->accountGate()) : std::shared_ptr<const void>();
	const void* accountKey = accountKeepalive ? accountKeepalive.get() : static_cast<const void*>(this);
	{
		std::lock_guard<std::mutex> updateLock(_position_update_mutex);
		if (!incremental && _last_committed_batch_ticket > ticket) return;
		{
			std::lock_guard<std::mutex> positionLock(_position_mutex);
			auto nextTargets = _target_pos;
			auto nextDiffs = _diff_pos;
			auto nextVersions = _target_versions;
			auto nextIncrementalVersions = _incremental_versions;
			uint64_t targetGeneration = _target_update_generation;
			uint64_t incrementGeneration = _incremental_generation;
			bool valid = true;
			auto stage = [&](const std::string& code, double value) {
				auto old = nextTargets.find(code);
				const bool existed = old != nextTargets.end();
				const double oldTarget = existed ? old->second : 0.0;
				auto diff = nextDiffs.find(code);
				const double oldDiff = diff == nextDiffs.end() ? 0.0 : diff->second;
				const double newTarget = incremental ? oldTarget + value : value;
				const double delta = incremental ? value : newTarget - oldTarget;
				const double newDiff = oldDiff + delta;
				if (!std::isfinite(newTarget) || !std::isfinite(delta) || !std::isfinite(newDiff))
				{
					valid = false;
					return;
				}
				const bool changed = newTarget != oldTarget;
				if (incremental || !existed || changed)
				{
					nextTargets[code] = newTarget;
					nextVersions[code] = ++targetGeneration;
					if (incremental) nextIncrementalVersions[code] = ++incrementGeneration;
					if (changed || incremental) nextDiffs[code] = newDiff;
					changes.push_back({code, newDiff, changed || incremental});
				}
				process.push_back({code, newDiff, changed || incremental});
			};
			for (const auto& target : targets)
			{
				if (!incremental)
				{
					auto version = _incremental_versions.find(target.first);
					if (version != _incremental_versions.end() && version->second > incrementalWatermark) continue;
				}
				stage(target.first, target.second);
				if (!valid) break;
			}
			if (valid && !incremental)
				for (const std::string& code : clearable)
				{
					auto old = nextTargets.find(code);
					if (old == nextTargets.end() || old->second == 0.0) continue;
					auto version = _incremental_versions.find(code);
					if (version != _incremental_versions.end() && version->second > incrementalWatermark) continue;
					stage(code, 0.0);
					if (!valid) break;
				}
			if (!valid) return;
			// Card 8 / BUG-P02-005: the complete target set (already scaled to
			// trader quantity) must keep every contract's account net position
			// within the limit shared by all executers on this account.
			account_net_limits::Positions accountTargets;
			for (const auto& item : nextTargets)
				if (item.second != 0.0) accountTargets[item.first] = item.second;
			account_net_limits::Positions previousAccount;
			const void* previousAccountKey = NULL;
			std::string accountError;
			if (!account_net_limits::Registry::instance().apply(accountKey, accountKeepalive, this, _name,
				_account_limits, accountTargets, previousAccount, previousAccountKey, accountError))
			{
				WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR, "[{}] Target batch rejected: {}",
					_name, accountError);
				return;
			}
			_target_pos.swap(nextTargets);
			_diff_pos.swap(nextDiffs);
			_target_versions.swap(nextVersions);
			_incremental_versions.swap(nextIncrementalVersions);
			_target_update_generation = targetGeneration;
			_incremental_generation = incrementGeneration;
			if (!incremental)
			{
				_last_committed_batch_ticket = ticket;
				_position_batch_generation = ticket;
			}
			if (!changes.empty()) ++_position_revision;
		}
		if (!changes.empty() && !save_data_locked())
		{
			persisted = false;
			_quiesce_incomplete.store(true, std::memory_order_release);
			for (const Change& change : process)
				markUnitFailedInMemoryLocked(change.code, std::shared_ptr<UnitLifecycleState>(),
					"position snapshot persistence failed; automatic order dispatch disabled");
			if (!save_data_locked())
				WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
					"[{}] Position failure tombstones could not be persisted", _name);
		}
		if (persisted)
		{
			SpinLock unitsLock(_mtx_units);
			for (const Change& change : process)
			{
				auto unitIt = _unit_map.find(change.code);
				auto stateIt = _unit_states.find(change.code);
				if (unitIt != _unit_map.end() && stateIt != _unit_states.end())
				{
					if (!change.notify) continue;
					const ExecuteUnitPtr unit = unitIt->second;
					const std::shared_ptr<UnitLifecycleState> state = stateIt->second;
					const std::string code = change.code;
					const double diff = change.diff;
					const bool schedule = enqueueUnitCallback(state, [this, unit, code, diff]() {
						if (!_trader || _trader->checkOrderLimits(code.c_str()))
							unit->self()->set_position(code.c_str(), diff);
					});
					drains.push_back({unit, state, schedule});
				}
				else
				{
					bool failed = false;
					if (stateIt != _unit_states.end())
					{
						std::lock_guard<std::mutex> stateLock(stateIt->second->mutex);
						failed = stateIt->second->creation_complete && stateIt->second->retry_blocked;
					}
					if (failed) continue;
					if (change.notify) _pending_target_notification.insert(change.code);
					createCodes.push_back(change.code);
				}
			}
		}
	}
	if (!persisted) return;
	for (const Drain& drain : drains)
		scheduleUnitCallbackDrain(drain.unit, drain.state, drain.schedule);
	for (const std::string& code : createCodes)
	{
		if (_trader && !_trader->checkOrderLimits(code.c_str())) continue;
		if (_target_lookup_test_hook) _target_lookup_test_hook(code);
		bool pending = false;
		if (getUnit(code.c_str(), true, false, &pending) == NULL && !pending)
		{
			// K06=B: after a provably side-effect-free failure the code stays in
			// _clean_failure_codes (quiesce incomplete) until a later generation
			// succeeds; any other missing unit keeps quiesce incomplete for good.
			bool cleanFailure = false;
			{
				std::lock_guard<std::mutex> cleanLock(_clean_failure_mutex);
				cleanFailure = _clean_failure_codes.count(code) != 0;
			}
			if (!cleanFailure)
				_quiesce_incomplete.store(true, std::memory_order_release);
			WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
				"[{}] Target for {} was recorded, but no execution unit is available", _name, code);
		}
	}
}

/**
 * @brief 处理行情数据更新
 *
 * @param stdCode 合约代码
 * @param newTick 最新的行情数据
 */
void WtDiffExecuter::on_tick(const char* stdCode, WTSTickData* newTick)
{
	QuiesceIngress ingress(this);
	if (!ingress)
		return;
	if (newTick == NULL || stdCode == NULL || stdCode[0] == '\0')
		return;
	const std::string code(stdCode);
	if (!ownsCode(code))
		return;
	bool creationPending = false;
	ExecuteUnitPtr unit = getUnit(stdCode, false, false, &creationPending);
	if (unit == NULL && creationPending)
	{
		// I03-NEW-072: do not block the adapter callback thread on the creation.
		newTick->retain();
		std::shared_ptr<WTSTickData> pendingTick(newTick, [](WTSTickData* value) { value->release(); });
		if (deferPendingUnitCallback(code, [pendingTick](const ExecuteUnitPtr& target) {
			target->self()->on_tick(pendingTick.get());
		}))
			return;
		unit = getUnit(stdCode, false, false, &creationPending);
	}
	if (unit == NULL)
	{
		_quiesce_incomplete.store(true, std::memory_order_release);
		return;
	}
	if (_unit_lookup_test_hook && stdCode != NULL)
		_unit_lookup_test_hook(stdCode);
	std::shared_ptr<UnitLifecycleState> state = getUnitState(stdCode, unit);
	if (state == NULL)
	{
		_quiesce_incomplete.store(true, std::memory_order_release);
		return;
	}
	newTick->retain();
	std::shared_ptr<WTSTickData> tick(newTick, [](WTSTickData* value) { value->release(); });
	const bool scheduleDrain = enqueueUnitCallback(state, [unit, tick]() {
		unit->self()->on_tick(tick.get());
	});
	scheduleUnitCallbackDrain(unit, state, scheduleDrain);
}

/**
 * @brief 处理成交回报
 *
 * @param localid 本地订单ID
 * @param stdCode 合约代码
 * @param isBuy 是否为买单
 * @param vol 成交数量
 * @param price 成交价格
 */
void WtDiffExecuter::on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price)
{
	QuiesceIngress ingress(this);
	if (!ingress)
		return;
	if (stdCode == NULL || stdCode[0] == '\0' || localid == 0)
		return;

	const std::string code(stdCode);
	// TraderAdapter broadcasts reports to every sink. Preserve localid's
	// existing zero/nonzero meaning, then filter by this executor's code ledger.
	// The sink carries no executor identity, so same-code multi-executor
	// attribution cannot be inferred here.
	if (!ownsCode(code))
		return;
	bool creationPending = false;
	ExecuteUnitPtr unit = getUnit(stdCode, false, false, &creationPending);
	if (_unit_lookup_test_hook)
		_unit_lookup_test_hook(code);
	std::shared_ptr<UnitLifecycleState> state = unit ? getUnitState(stdCode, unit) :
		std::shared_ptr<UnitLifecycleState>();
	bool pluginAvailable = unit != NULL && state != NULL;
	bool deferred = false;

	double prevDiff = 0.0;
	double curDiff = 0.0;
	bool scheduleDrain = false;
	{
		std::lock_guard<std::mutex> updateLock(_position_update_mutex);
		if (!ownsCodeLocked(code))
		{
			_quiesce_incomplete.store(true, std::memory_order_release);
			return;
		}
		{
			std::lock_guard<std::mutex> lock(_position_mutex);
			prevDiff = _diff_pos[code];
			curDiff = prevDiff - vol * (isBuy ? 1 : -1);
			_diff_pos[code] = curDiff;
			++_position_revision;
		}
		if (!save_data_locked())
		{
			markUnitFailedLocked(code, state,
				"trade snapshot persistence failed; automatic order dispatch disabled");
			_quiesce_incomplete.store(true, std::memory_order_release);
			WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
				"[{}] Trade for {} was booked in memory but plugin notification was suppressed because snapshot persistence failed",
				_name, stdCode);
		}
		else if (pluginAvailable)
			scheduleDrain = enqueueUnitCallback(state, [localid, unit, code, isBuy, vol, price]() {
				unit->self()->on_trade(localid, code.c_str(), isBuy, vol, price);
			});
		else if (creationPending)
		{
			// I03-NEW-072: the diff is booked now; the plugin notification joins the
			// creation in flight instead of blocking this callback thread.
			deferred = deferPendingUnitCallback(code, [localid, code, isBuy, vol, price](const ExecuteUnitPtr& target) {
				target->self()->on_trade(localid, code.c_str(), isBuy, vol, price);
			});
			if (!deferred)
			{
				unit = getUnit(stdCode, false, false, &creationPending);
				state = unit ? getUnitState(stdCode, unit) : std::shared_ptr<UnitLifecycleState>();
				pluginAvailable = unit != NULL && state != NULL;
				if (pluginAvailable)
					scheduleDrain = enqueueUnitCallback(state, [localid, unit, code, isBuy, vol, price]() {
						unit->self()->on_trade(localid, code.c_str(), isBuy, vol, price);
					});
			}
		}
	}

	WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}] Diff of {} updated by trade: {} -> {}", _name, stdCode, prevDiff, curDiff);
	if (!pluginAvailable && !deferred)
	{
		_quiesce_incomplete.store(true, std::memory_order_release);
		WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
			"[{}] Trade for {} updated the remaining diff, but no execution unit was available for notification",
			_name, stdCode);
	}
	scheduleUnitCallbackDrain(unit, state, scheduleDrain);
}

/**
 * @brief 处理订单状态回报
 *
 * @param localid 本地订单ID
 * @param stdCode 合约代码
 * @param isBuy 是否为买单
 * @param totalQty 总数量
 * @param leftQty 剩余数量
 * @param price 价格
 * @param isCanceled 是否已撤销
 */
void WtDiffExecuter::on_order(uint32_t localid, const char* stdCode, bool isBuy, double /*totalQty*/, double leftQty, double price, bool isCanceled /* = false */)
{
	QuiesceIngress ingress(this);
	if (!ingress)
		return;
	if (stdCode == NULL || stdCode[0] == '\0')
		return;
	const std::string code(stdCode);
	if (!ownsCode(code))
		return;
	bool creationPending = false;
	ExecuteUnitPtr unit = getUnit(stdCode, false, false, &creationPending);
	if (unit == NULL && creationPending)
	{
		// I03-NEW-072: do not block the adapter callback thread on the creation.
		if (deferPendingUnitCallback(code, [localid, code, isBuy, leftQty, price, isCanceled](const ExecuteUnitPtr& target) {
			target->self()->on_order(localid, code.c_str(), isBuy, leftQty, price, isCanceled);
		}))
			return;
		unit = getUnit(stdCode, false, false, &creationPending);
	}
	if (unit == NULL)
	{
		// Order updates are durable lifecycle information, unlike ticks. Losing
		// one makes a successful quiesce claim unsafe.
		_quiesce_incomplete.store(true, std::memory_order_release);
		return;
	}
	if (_unit_lookup_test_hook)
		_unit_lookup_test_hook(code);
	std::shared_ptr<UnitLifecycleState> state = getUnitState(stdCode, unit);
	if (state == NULL)
	{
		_quiesce_incomplete.store(true, std::memory_order_release);
		return;
	}
	const bool scheduleDrain = enqueueUnitCallback(state, [localid, unit, code, isBuy, leftQty, price, isCanceled]() {
		unit->self()->on_order(localid, code.c_str(), isBuy, leftQty, price, isCanceled);
	});
	scheduleUnitCallbackDrain(unit, state, scheduleDrain);
}

/**
 * @brief 处理委托回报
 *
 * @param localid 本地订单ID
 * @param stdCode 合约代码
 * @param bSuccess 是否成功
 * @param message 回报消息
 */
void WtDiffExecuter::on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message)
{
	QuiesceIngress ingress(this);
	if (!ingress)
		return;
	if (stdCode == NULL || stdCode[0] == '\0')
		return;
	const std::string code(stdCode);
	if (!ownsCode(code))
		return;
	const bool hasMessage = message != NULL;
	const std::string msg = hasMessage ? message : "";
	bool creationPending = false;
	ExecuteUnitPtr unit = getUnit(stdCode, false, false, &creationPending);
	if (unit == NULL && creationPending)
	{
		// I03-NEW-072: do not block the adapter callback thread on the creation.
		if (deferPendingUnitCallback(code, [localid, code, bSuccess, msg, hasMessage](const ExecuteUnitPtr& target) {
			target->self()->on_entrust(localid, code.c_str(), bSuccess, hasMessage ? msg.c_str() : NULL);
		}))
			return;
		unit = getUnit(stdCode, false, false, &creationPending);
	}
	if (unit == NULL)
	{
		// Entrust results must not disappear behind a successful drain result.
		_quiesce_incomplete.store(true, std::memory_order_release);
		return;
	}
	if (_unit_lookup_test_hook)
		_unit_lookup_test_hook(code);
	std::shared_ptr<UnitLifecycleState> state = getUnitState(stdCode, unit);
	if (state == NULL)
	{
		_quiesce_incomplete.store(true, std::memory_order_release);
		return;
	}
	const bool scheduleDrain = enqueueUnitCallback(state, [unit, localid, code, bSuccess, msg, hasMessage]() {
		unit->self()->on_entrust(localid, code.c_str(), bSuccess, hasMessage ? msg.c_str() : NULL);
	});
	scheduleUnitCallbackDrain(unit, state, scheduleDrain);
}

/**
 * @brief 处理交易通道就绪事件
 *
 * @details 当交易通道就绪时，通知所有执行单元并恢复差量持仓
 */
void WtDiffExecuter::on_channel_ready()
{
	QuiesceIngress ingress(this);
	if (!ingress)
		return;
	struct PendingDrain
	{
		ExecuteUnitPtr unit;
		std::shared_ptr<UnitLifecycleState> state;
		bool schedule;
	};
	std::vector<PendingDrain> drains;
	std::vector<std::string> diffCodes;
	{
		// Channel transitions and position/trade updates share one ordering lock.
		// Recovery captures the diff at this point and is queued before later trades.
		std::lock_guard<std::mutex> updateLock(_position_update_mutex);
		if (_channel_ready.exchange(true))
			return;
		const uint64_t channelEpoch = ++_channel_epoch;
		wt_hashmap<std::string, double> diffSnapshot;
		{
			std::lock_guard<std::mutex> positionLock(_position_mutex);
			diffSnapshot = _diff_pos;
		}
		diffCodes.reserve(diffSnapshot.size());
		for (const auto& item : diffSnapshot)
			diffCodes.push_back(item.first);
		{
			SpinLock unitsLock(_mtx_units);
			for (auto& item : _unit_map)
			{
				auto stateIt = _unit_states.find(item.first);
				if (stateIt == _unit_states.end())
					continue;
				const ExecuteUnitPtr unit = item.second;
				const std::shared_ptr<UnitLifecycleState> state = stateIt->second;
				const auto diffIt = diffSnapshot.find(item.first);
				const bool hasDiff = diffIt != diffSnapshot.end();
				const double diff = hasDiff ? diffIt->second : 0.0;
				auto readySchedule = enqueueUnitCallback(state, [unit]() {
					unit->self()->on_channel_ready();
				});
				enqueueUnitCallback(state, [this, code = item.first, unit, state, channelEpoch,
					diff, hasDiff]() {
					recoverUnitPosition(code, unit, state, channelEpoch, diff, hasDiff);
				});
				drains.push_back({ unit, state, readySchedule });
			}
		}
	}
	// A persisted diff may not have a unit yet. Creating it while the channel is
	// ready queues its ready notification and recovery in that order. Do not
	// wait for another thread's in-flight creation: its ledger entry is already
	// committed, and registerCreatedUnit() observes _channel_ready under the same
	// update lock, so it queues ready + recovery itself. Waiting here would block
	// the adapter callback thread behind plugin initialization (card 9d=A) and
	// deadlocks when that initialization waits for this thread.
	for (const std::string& code : diffCodes)
	{
		bool pending = false;
		if (getUnit(code.c_str(), true, false, &pending) == NULL && !pending)
			_quiesce_incomplete.store(true, std::memory_order_release);
	}

	for (const PendingDrain& entry : drains)
		scheduleUnitCallbackDrain(entry.unit, entry.state, entry.schedule);
}

/**
 * @brief 处理交易通道断开事件
 *
 * @details 当交易通道断开时，通知所有执行单元
 */
void WtDiffExecuter::on_channel_lost()
{
	QuiesceIngress ingress(this);
	if (!ingress)
		return;
	struct PendingDrain
	{
		ExecuteUnitPtr unit;
		std::shared_ptr<UnitLifecycleState> state;
		bool schedule;
	};
	std::vector<PendingDrain> drains;
	{
		std::lock_guard<std::mutex> updateLock(_position_update_mutex);
		SpinLock lock(_mtx_units);
		if (!_channel_ready.exchange(false))
			return;
		++_channel_epoch;
		for (auto& item : _unit_map)
		{
			auto stateIt = _unit_states.find(item.first);
			if (stateIt == _unit_states.end())
				continue;
			const ExecuteUnitPtr unit = item.second;
			const std::shared_ptr<UnitLifecycleState> state = stateIt->second;
			const bool schedule = enqueueUnitCallback(state, [unit]() {
				unit->self()->on_channel_lost();
			});
			drains.push_back({ unit, state, schedule });
		}
	}
	for (const PendingDrain& entry : drains)
		scheduleUnitCallbackDrain(entry.unit, entry.state, entry.schedule);
}

/**
 * @brief 处理账户资金更新
 *
 * @param currency 货币类型
 * @param prebalance 前结余额
 * @param balance 结余额
 * @param dynbalance 动态权益
 * @param avaliable 可用资金
 * @param closeprofit 平仓盈亏
 * @param dynprofit 浮动盈亏
 * @param margin 保证金
 * @param fee 手续费
 * @param deposit 入金
 * @param withdraw 出金
 */
void WtDiffExecuter::on_account(const char* currency, double prebalance, double balance, double dynbalance,
	double avaliable, double closeprofit, double dynprofit, double margin, double fee, double deposit, double withdraw)
{
	QuiesceIngress ingress(this);
	if (!ingress)
		return;
	struct ReadyUnit
	{
		ExecuteUnitPtr unit;
		std::shared_ptr<UnitLifecycleState> state;
	};
	std::vector<ReadyUnit> units;
	{
		SpinLock lock(_mtx_units);
		units.reserve(_unit_map.size());
		for (auto& item : _unit_map)
		{
			auto stateIt = _unit_states.find(item.first);
			if (stateIt == _unit_states.end())
				continue;
			std::lock_guard<std::mutex> stateLock(stateIt->second->mutex);
			if (stateIt->second->creation_complete &&
				stateIt->second->init_status == ExecUnitInitState::READY)
				units.push_back({ item.second, stateIt->second });
		}
	}
	const bool hasCurrency = currency != NULL;
	const std::string currencyValue = hasCurrency ? currency : "";
	for (const ReadyUnit& entry : units)
	{
		const bool scheduleDrain = enqueueUnitCallback(entry.state,
			[unit = entry.unit, currencyValue, hasCurrency, prebalance, balance, dynbalance,
				avaliable, closeprofit, dynprofit, margin, fee, deposit, withdraw]() {
				unit->self()->on_account(hasCurrency ? currencyValue.c_str() : NULL,
					prebalance, balance, dynbalance, avaliable, closeprofit, dynprofit,
					margin, fee, deposit, withdraw);
			});
		scheduleUnitCallbackDrain(entry.unit, entry.state, scheduleDrain);
	}
}

/**
 * @brief 处理持仓更新
 *
 * @param stdCode 合约代码
 * @param isLong 是否为多头仓位
 * @param prevol 前持仓量
 * @param preavail 前可用持仓量
 * @param newvol 新持仓量
 * @param newavail 新可用持仓量
 * @param tradingday 交易日
 */
void WtDiffExecuter::on_position(const char* /*stdCode*/, bool /*isLong*/, double /*prevol*/, double /*preavail*/, double /*newvol*/, double /*newavail*/, uint32_t /*tradingday*/)
{
	QuiesceIngress ingress(this);
	if (!ingress)
		return;
	// 注意：当前实现为空，可能在子类中实现
}

#pragma endregion 外部接口
