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
#include <exception>
#include <vector>
#include <cstdio>
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
	if (_quiesced && !_quiesce_incomplete.load(std::memory_order_acquire))
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
			return !_quiesce_incomplete.load(std::memory_order_acquire);
		if (_quiesce_wait_in_progress)
		{
			_quiesce_condition.wait(lock, [this]() { return !_quiesce_wait_in_progress; });
			return _quiesce_host_completed &&
				!_quiesce_incomplete.load(std::memory_order_acquire);
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
	return poolWaitSucceeded && !_quiesce_incomplete.load(std::memory_order_acquire);
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
		_quiesce_incomplete.load(std::memory_order_acquire))
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
			return !_quiesce_incomplete.load(std::memory_order_acquire);
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
		!_quiesce_incomplete.load(std::memory_order_acquire);
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
}

/**
 * @brief 初始化差量执行器
 *
 * @param params 配置参数
 * @return bool 初始化是否成功
 * @details 设置缩放比例、创建线程池并加载数据
 */
bool WtDiffExecuter::init(WTSVariant* params)
{
	if (params == NULL)
		return false;

	_config = params;
	_config->retain();

	_scale = params->getDouble("scale");

	uint32_t poolsize = params->getUInt32("poolsize");
	if(poolsize > 0)
	{
		_pool.reset(new boost::threadpool::pool(poolsize));
	}

	load_data();

	WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}] Diff executer inited, scale: {}, thread poolsize: {}", _name, _scale, poolsize);

	return true;
}

/**
 * @brief 加载执行器数据
 *
 * @details 从文件中读取执行器的目标仓位和差量数据
 *          数据存储在JSON格式的文件中，包含“targets”和“diffs”两部分
 */
void WtDiffExecuter::load_data()
{
	//读取执行器的理论部位，以及待执行的差量
	std::string filename = WtHelper::getExecDataDir();
	filename += _name + ".json";

	if (!StdFile::exists(filename.c_str()))
	{
		return;
	}

	std::string content;
	StdFile::read_file_content(filename.c_str(), content);
	if (content.empty())
		return;

	rj::Document root;
	root.Parse(content.c_str());

	if (root.HasParseError())
		return;

	if (root.HasMember("targets") && root["targets"].IsArray())
	{
		const rj::Value& jTargets = root["targets"];
		for (const rj::Value& jItem : jTargets.GetArray())
		{
			if (!jItem.IsObject() || !jItem.HasMember("code") || !jItem["code"].IsString() ||
				!jItem.HasMember("target") || !jItem["target"].IsNumber())
				continue;
			const char* stdCode = jItem["code"].GetString();
			CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, NULL);
			WTSContractInfo* ct = _bd_mgr ? _bd_mgr->getContract(cInfo._code, cInfo._exchg) : NULL;
			if (ct == NULL)
			{
				WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}] Ticker {} is not valid", _name, stdCode);
				continue;
			}

			double pos = jItem["target"].GetDouble();
			{
				std::lock_guard<std::mutex> lock(_position_mutex);
				_target_pos[stdCode] = pos;
				_target_versions[stdCode] = ++_target_update_generation;
			}
		}
	}

	if (root.HasMember("diffs") && root["diffs"].IsArray())
	{
		const rj::Value& jDiffs = root["diffs"];
		for (const rj::Value& jItem : jDiffs.GetArray())
		{
			if (!jItem.IsObject() || !jItem.HasMember("code") || !jItem["code"].IsString() ||
				!jItem.HasMember("diff") || !jItem["diff"].IsNumber())
				continue;
			const char* stdCode = jItem["code"].GetString();
			CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, NULL);
			WTSContractInfo* ct = _bd_mgr ? _bd_mgr->getContract(cInfo._code, cInfo._exchg) : NULL;
			if (ct == NULL)
			{
				WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}] Ticker {} is not valid", _name, stdCode);
				continue;
			}

			double pos = jItem["diff"].GetDouble();
			{
				std::lock_guard<std::mutex> lock(_position_mutex);
				_diff_pos[stdCode] = pos;
			}
		}
	}

	// Failure tombstones are independent of contract discovery. A restart must
	// retain the fail-closed state even when the current base-data set is partial.
	if (root.HasMember("blocked_units") && root["blocked_units"].IsArray())
	{
		SpinLock unitsLock(_mtx_units);
		for (const rj::Value& item : root["blocked_units"].GetArray())
		{
			if (!item.IsObject() || !item.HasMember("code") || !item["code"].IsString())
				continue;
			const std::string code = item["code"].GetString();
			if (code.empty())
				continue;

			auto state = std::make_shared<UnitLifecycleState>();
			state->std_code = code;
			state->creation_complete = true;
			state->callback_failed = true;
			state->retry_blocked = true;
			state->init_status = ExecUnitInitState::FAILED;
			state->init_generation = item.HasMember("generation") && item["generation"].IsUint64() ?
				item["generation"].GetUint64() : 1;
			state->init_error = item.HasMember("error") && item["error"].IsString() ?
				item["error"].GetString() : "persisted failure tombstone; automatic retry disabled";
			if (_unit_states.find(code) != _unit_states.end())
				continue;
			_unit_states.emplace(code, state);
			_next_unit_init_generation = std::max(_next_unit_init_generation,
				state->init_generation + 1);
		}
	}
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
		BoostFile bf;
		if (bf.create_new_file(tempFilename.c_str()))
		{
			writeSucceeded = bf.write_file(sb.GetString(), sb.GetSize());
			// BoostFile's Windows implementation checks WriteFile's BOOL result,
			// but not its written-byte count. Verify the resulting file length
			// before allowing the temporary snapshot to replace the saved one.
			if (writeSucceeded)
				writeSucceeded = bf.get_file_size() == sb.GetSize();
			bf.close_file();
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
	if (!save_data_locked())
	{
		_quiesce_incomplete.store(true, std::memory_order_release);
		WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
			"[{}] Failure tombstone for {} could not be persisted; in-memory order dispatch remains blocked",
			_name, code);
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

	if (_channel_ready.load() && _channel_epoch.load() == channelEpoch &&
		isCurrentUnit(code, unit, state) && unit->self() != NULL)
	{
		WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO,
			"[{}] Diff of {} recovered to {}", _name, code, diff);
		unit->self()->set_position(code.c_str(), diff);
	}
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
ExecuteUnitPtr WtDiffExecuter::getUnit(const char* stdCode, bool bAutoCreate /* = true */)
{
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
		_quiesce_incomplete.store(true, std::memory_order_release);
		std::unique_lock<std::mutex> updateLock(_position_update_mutex);
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
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			persistFailure = state->retry_blocked;
			if (persistFailure)
				failureReason += " (automatic retry disabled: plugin side effects may have occurred)";
			state->result.reset();
			state->init_status = ExecUnitInitState::FAILED;
			state->init_error = failureReason;
			state->creation_complete = true;
			state->creation_owner = std::thread::id();
			state->callback_events.clear();
		}
		state->condition.notify_all();
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
		unit = _factory->createDiffExeUnit(name);
		if (unit == NULL)
			return failCreation("execution-unit creation failed");
		std::string error;
		if (!unit->initialize(this, stdCode, cfg, error) || unit->self() == NULL)
			return failCreation(error.empty() ? "plugin initialization failed" : error.c_str());
		retainQuiesceUnit(unit);

	{
		// Serialize registration with ready/lost transitions and position updates.
		// Capture recovery data now, so trades queued later cannot change the diff
		// consumed by this already-ordered recovery event.
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
		_unit_map[code] = unit;
		if (_channel_ready.load())
		{
			const uint64_t channelEpoch = _channel_epoch.load();
			enqueueUnitCallback(state, [unit]() { unit->self()->on_channel_ready(); });
			enqueueUnitCallback(state, [this, code, unit, state, channelEpoch,
				recoveryDiff, hasRecoveryDiff]() {
				recoverUnitPosition(code, unit, state, channelEpoch, recoveryDiff, hasRecoveryDiff);
			});
		}
	}

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

	return _trader->buy(stdCode, price, qty, 0, bForceClose);
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

	return _trader->sell(stdCode, price, qty, 0, bForceClose);
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

	return _trader->cancel(localid);
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

	return _trader->cancel(stdCode, isBuy, qty);
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
void WtDiffExecuter::on_position_changed(const char* stdCode, double diffPos)
{
	QuiesceIngress ingress(this);
	if (!ingress)
		return;
	if (stdCode == NULL || stdCode[0] == '\0')
		return;
	if (decimal::eq(diffPos, 0))
		return;

	diffPos = round(diffPos*_scale);
	ExecuteUnitPtr unit = getUnit(stdCode, true);
	std::shared_ptr<UnitLifecycleState> state = unit ? getUnitState(stdCode, unit) :
		std::shared_ptr<UnitLifecycleState>();
	const bool orderAllowed = !_trader || _trader->checkOrderLimits(stdCode);
	const std::string code(stdCode);
	double oldVol = 0.0;
	double targetPos = 0.0;
	double prevDiff = 0.0;
	double thisDiff = 0.0;
	bool scheduleDrain = false;
	{
		std::lock_guard<std::mutex> updateLock(_position_update_mutex);
		{
			std::lock_guard<std::mutex> lock(_position_mutex);
			oldVol = _target_pos[code];
			targetPos = oldVol + diffPos;
			_target_pos[code] = targetPos;
			_target_versions[code] = ++_target_update_generation;
			prevDiff = _diff_pos[code];
			thisDiff = prevDiff + diffPos;
			_diff_pos[code] = thisDiff;
			++_position_revision;
		}
		if (!save_data_locked())
		{
		markUnitFailedLocked(code, state,
				"position snapshot persistence failed; automatic order dispatch disabled");
			_quiesce_incomplete.store(true, std::memory_order_release);
			WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
				"[{}] Position change for {} was recorded in memory but not dispatched because snapshot persistence failed",
				_name, stdCode);
		}
		else if (orderAllowed && unit != NULL && state != NULL)
			scheduleDrain = enqueueUnitCallback(state, [unit, code, thisDiff]() {
				unit->self()->set_position(code.c_str(), thisDiff);
			});
	}

	/*
	 *	By Sunseeeeeker @ 2023.01.10
	 *	更新差量
	*/
	WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}] Target position of {} changed additonally: {} -> {}, diff postion changed: {} -> {}", _name, stdCode, oldVol, targetPos, prevDiff, thisDiff);
	if (unit == NULL || state == NULL)
	{
		_quiesce_incomplete.store(true, std::memory_order_release);
		WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
			"[{}] Target position update for {} was recorded, but no execution unit is available; order not dispatched",
			_name, stdCode);
	}
	else if (!orderAllowed)
		WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}] {} is disabled", _name, stdCode);
	scheduleUnitCallbackDrain(unit, state, scheduleDrain);
}

/**
 * @brief 设置一组目标仓位
 *
 * @param targets 目标仓位映射表，键为合约代码，值为目标仓位量
 * @details 批量设置目标仓位，并计算差量。如果原目标仓位中的合约不在新的目标中，
 *          则自动将其设置为0。最后保存数据到文件。
 */
void WtDiffExecuter::set_position(const wt_hashmap<std::string, double>& targets)
{
	QuiesceIngress ingress(this);
	if (!ingress)
		return;
	uint64_t batchGeneration = 0;
	uint64_t batchStartTargetGeneration = 0;
	{
		std::lock_guard<std::mutex> updateLock(_position_update_mutex);
		batchGeneration = ++_position_batch_generation;
		std::lock_guard<std::mutex> positionLock(_position_mutex);
		batchStartTargetGeneration = _target_update_generation;
	}
	for (const auto& target : targets)
	{
		const std::string code = target.first;
		const char* stdCode = code.c_str();
		double newVol = round(target.second*_scale);
		ExecuteUnitPtr unit = getUnit(stdCode);
		std::shared_ptr<UnitLifecycleState> state = unit ? getUnitState(stdCode, unit) :
			std::shared_ptr<UnitLifecycleState>();

		const bool orderAllowed = !_trader || _trader->checkOrderLimits(stdCode);
		double oldVol = 0.0;
		double prevDiff = 0.0;
		double thisDiff = 0.0;
		bool changed = false;
		bool scheduleDrain = false;
		bool supersededByIncrementalUpdate = false;
		{
			std::lock_guard<std::mutex> updateLock(_position_update_mutex);
			if (batchGeneration != _position_batch_generation)
				return;
			{
				std::lock_guard<std::mutex> lock(_position_mutex);
				auto versionIt = _target_versions.find(code);
				const uint64_t currentVersion = versionIt == _target_versions.end() ? 0 : versionIt->second;
				// A post-start incremental target owns this code even if its entry in
				// this batch has not been reached yet.
				if (currentVersion > batchStartTargetGeneration)
					supersededByIncrementalUpdate = true;
				else
				{
					const bool targetExists = _target_pos.find(code) != _target_pos.end();
					oldVol = _target_pos[code];
					_target_pos[code] = newVol;
					if (!targetExists || oldVol != newVol)
						_target_versions[code] = ++_target_update_generation;
					if (!decimal::eq(oldVol, newVol))
					{
						changed = true;
						prevDiff = _diff_pos[code];
						thisDiff = prevDiff + (newVol - oldVol);
						_diff_pos[code] = thisDiff;
						++_position_revision;
					}
					else if (!targetExists || oldVol != newVol)
					{
						++_position_revision;
					}
				}
			}
			if (!supersededByIncrementalUpdate)
			{
				const bool persisted = save_data_locked();
				if (!persisted)
				{
					markUnitFailedLocked(code, state,
						"position snapshot persistence failed; automatic order dispatch disabled");
					_quiesce_incomplete.store(true, std::memory_order_release);
					WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
						"[{}] Target position for {} was recorded in memory but not dispatched because snapshot persistence failed",
						_name, stdCode);
				}
				else if (changed && orderAllowed && unit != NULL && state != NULL)
					scheduleDrain = enqueueUnitCallback(state, [unit, code, thisDiff]() {
						unit->self()->set_position(code.c_str(), thisDiff);
					});
			}
		}

		if (supersededByIncrementalUpdate)
			continue;
		if (!unit || !state)
		{
			if (changed)
				_quiesce_incomplete.store(true, std::memory_order_release);
			WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
				"[{}] Target position for {} was recorded, but no execution unit is available; order not dispatched",
				_name, stdCode);
		}
		if (!changed)
			continue;
		WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}] Target position of {} changed: {} -> {}, diff postion changed: {} -> {}", _name, stdCode, oldVol, newVol, prevDiff, thisDiff);
		if (unit && state && !orderAllowed)
			WTSLogger::log_dyn("executer", _name.c_str(), LL_WARN, "[{}] {} is disabled due to entrust limit control ", _name, stdCode);
		scheduleUnitCallbackDrain(unit, state, scheduleDrain);
	}

	// Snapshot codes under the position lock; contract lookup and unit creation
	// may call external code and therefore happen after releasing it.
	struct OmittedTarget
	{
		std::string code;
		uint64_t targetVersion;
	};
	std::vector<OmittedTarget> oldCodes;
	{
		std::lock_guard<std::mutex> updateLock(_position_update_mutex);
		if (batchGeneration != _position_batch_generation)
			return;
		std::lock_guard<std::mutex> lock(_position_mutex);
		oldCodes.reserve(_target_pos.size());
		for (const auto& item : _target_pos)
		{
			if (targets.find(item.first) != targets.end() || decimal::eq(item.second, 0))
				continue;
			auto versionIt = _target_versions.find(item.first);
			const uint64_t version = versionIt == _target_versions.end() ? 0 : versionIt->second;
			// An incremental update that arrived after this batch began owns the
			// target, even if it arrived before the omitted-code snapshot was taken.
			if (version <= batchStartTargetGeneration)
				oldCodes.push_back({ item.first, version });
		}
	}
	for (const OmittedTarget& omitted : oldCodes)
	{
		const std::string& code = omitted.code;
		const char* stdCode = code.c_str();
		if (_omitted_sweep_test_hook)
			_omitted_sweep_test_hook(code);
		CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(stdCode, NULL);
		WTSContractInfo* cInfo = _bd_mgr ? _bd_mgr->getContract(codeInfo._code, codeInfo._exchg) : NULL;
		if (cInfo == NULL)
			continue;

		ExecuteUnitPtr unit = getUnit(stdCode);
		std::shared_ptr<UnitLifecycleState> state = unit ? getUnitState(stdCode, unit) :
			std::shared_ptr<UnitLifecycleState>();
		const bool orderAllowed = unit != NULL && state != NULL && (!_trader || _trader->checkOrderLimits(stdCode));
		double pos = 0.0;
		double thisDiff = 0.0;
		bool scheduleDrain = false;
		{
			std::lock_guard<std::mutex> updateLock(_position_update_mutex);
			if (batchGeneration != _position_batch_generation)
				return;
			{
				std::lock_guard<std::mutex> lock(_position_mutex);
				auto targetIt = _target_pos.find(code);
				if (targetIt == _target_pos.end() || decimal::eq(targetIt->second, 0) ||
					targets.find(code) != targets.end())
					continue;
				auto versionIt = _target_versions.find(code);
				const uint64_t currentVersion = versionIt == _target_versions.end() ? 0 : versionIt->second;
				if (currentVersion != omitted.targetVersion ||
					currentVersion > batchStartTargetGeneration)
					continue;

				pos = targetIt->second;
				targetIt->second = 0;
				_target_versions[code] = ++_target_update_generation;
				double& diff = _diff_pos[code];
				// Applying target 0 is the same delta as every other target change:
				// new target - old target == 0 - pos.
				diff += -pos;
				thisDiff = diff;
				++_position_revision;
			}
			if (!save_data_locked())
			{
				markUnitFailedLocked(code, state,
					"position snapshot persistence failed; automatic order dispatch disabled");
				_quiesce_incomplete.store(true, std::memory_order_release);
				WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
					"[{}] Expired target for {} was recorded in memory but not dispatched because snapshot persistence failed",
					_name, stdCode);
			}
			else if (orderAllowed)
				scheduleDrain = enqueueUnitCallback(state, [unit, code, thisDiff]() {
					unit->self()->set_position(code.c_str(), thisDiff);
				});
		}

		WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}] {} is not in target, set to 0 automatically", _name, stdCode);
		if (unit == NULL || state == NULL)
			WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
				"[{}] Expired target for {} was recorded as zero, but no execution unit is available; order not dispatched",
				_name, stdCode);
		else if (!orderAllowed)
			WTSLogger::log_dyn("executer", _name.c_str(), LL_WARN, "[{}] {} is disabled due to entrust limit control ", _name, stdCode);
		if (!unit || !state)
			_quiesce_incomplete.store(true, std::memory_order_release);
		scheduleUnitCallbackDrain(unit, state, scheduleDrain);
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
	ExecuteUnitPtr unit = getUnit(stdCode, false);
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
	ExecuteUnitPtr unit = getUnit(stdCode, false);
	if (_unit_lookup_test_hook)
		_unit_lookup_test_hook(code);
	std::shared_ptr<UnitLifecycleState> state = unit ? getUnitState(stdCode, unit) :
		std::shared_ptr<UnitLifecycleState>();
	const bool pluginAvailable = unit != NULL && state != NULL;

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
	}

	WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}] Diff of {} updated by trade: {} -> {}", _name, stdCode, prevDiff, curDiff);
	if (!pluginAvailable)
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
	ExecuteUnitPtr unit = getUnit(stdCode, false);
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
	ExecuteUnitPtr unit = getUnit(stdCode, false);
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
	const bool hasMessage = message != NULL;
	const std::string msg = hasMessage ? message : "";
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
	// ready queues its ready notification and recovery in that order.
	for (const std::string& code : diffCodes)
	{
		if (getUnit(code.c_str()) == NULL)
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
