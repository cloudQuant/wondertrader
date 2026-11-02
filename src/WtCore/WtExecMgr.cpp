#include "WtExecMgr.h"
#include "WtFilterMgr.h"

#include "../Share/decimal.h"
#include "../Includes/WTSVariant.hpp"

#include "../WTSTools/WTSLogger.h"

#include <algorithm>
#include <utility>

USING_NS_WTP;

//////////////////////////////////////////////////////////////////////////
#pragma region "WtExecuterMgr"
std::vector<ExecCmdPtr> WtExecuterMgr::snapshot_executers() const
{
	std::vector<ExecCmdPtr> snapshot;
	std::lock_guard<std::mutex> lock(_executers_mtx);
	snapshot.reserve(_executers.size());
	for (const auto& item : _executers)
		snapshot.push_back(item.second);
	return snapshot;
}

void WtExecuterMgr::enum_executer(EnumExecuterCb cb)
{
	const std::vector<ExecCmdPtr> executers = snapshot_executers();
	for (const ExecCmdPtr& executer : executers)
	{
		cb(executer);
	}
}

bool WtExecuterMgr::begin_quiesce_stage(std::vector<ExecCmdPtr>& executers)
{
	std::lock_guard<std::mutex> lock(_executers_mtx);
	if (!_quiesce_requested || !_quiesce_request_succeeded || _quiesce_stage_in_progress)
		return false;
	try
	{
		executers = _quiesce_executer_owners;
	}
	catch (...)
	{
		return false;
	}
	_quiesce_stage_in_progress = true;
	return true;
}

bool WtExecuterMgr::request_quiesce_all()
{
	std::vector<ExecCmdPtr> executers;
	{
		std::lock_guard<std::mutex> lock(_executers_mtx);
		if (_quiesce_request_succeeded)
			return true;
		if (_quiesce_stage_in_progress)
			return false;
		if (!_quiesce_requested)
		{
			std::vector<ExecCmdPtr> snapshot;
			try
			{
				snapshot.reserve(_executers.size());
				for (const auto& item : _executers)
					snapshot.push_back(item.second);
			}
			catch (...)
			{
				return false;
			}
			_quiesce_executer_owners.swap(snapshot);
			_quiesce_requested = true;
		}
		try
		{
			executers = _quiesce_executer_owners;
		}
		catch (...)
		{
			return false;
		}
		_quiesce_stage_in_progress = true;
	}

	bool allRequested = true;
	for (const ExecCmdPtr& executer : executers)
	{
		if (executer == NULL)
		{
			allRequested = false;
			continue;
		}
		IExecCommandQuiesce* quiesce = dynamic_cast<IExecCommandQuiesce*>(executer.get());
		IExecCommandQuiesceStages* stages =
			dynamic_cast<IExecCommandQuiesceStages*>(executer.get());
		if (quiesce == NULL || stages == NULL)
			allRequested = false;
		if (quiesce != NULL)
		{
			try { quiesce->requestQuiesce(); }
			catch (...) { allRequested = false; }
		}
	}
	{
		std::lock_guard<std::mutex> lock(_executers_mtx);
		_quiesce_request_succeeded = allRequested;
		_quiesce_stage_in_progress = false;
	}
	return allRequested;
}

bool WtExecuterMgr::preflight_quiesce_all()
{
	std::vector<ExecCmdPtr> executers;
	if (!begin_quiesce_stage(executers))
		return false;

	bool allCanDrain = true;
	for (const ExecCmdPtr& executer : executers)
	{
		IExecCommandQuiesceStages* stages = executer == NULL ? NULL :
			dynamic_cast<IExecCommandQuiesceStages*>(executer.get());
		if (stages == NULL)
		{
			allCanDrain = false;
			continue;
		}
		try
		{
			if (!stages->preflightQuiesceOnCurrentThread())
				allCanDrain = false;
		}
		catch (...)
		{
			allCanDrain = false;
		}
	}
	{
		std::lock_guard<std::mutex> lock(_executers_mtx);
		_quiesce_host_preflight_succeeded = allCanDrain;
		_quiesce_stage_in_progress = false;
	}
	return allCanDrain;
}

bool WtExecuterMgr::drain_host_quiesced_all()
{
	std::vector<ExecCmdPtr> executers;
	{
		std::lock_guard<std::mutex> lock(_executers_mtx);
		if (_quiesce_host_drained)
			return true;
	}
	if (!begin_quiesce_stage(executers))
		return false;
	{
		std::lock_guard<std::mutex> lock(_executers_mtx);
		if (!_quiesce_host_preflight_succeeded)
		{
			_quiesce_stage_in_progress = false;
			return false;
		}
	}

	bool allDrained = true;
	for (const ExecCmdPtr& executer : executers)
	{
		IExecCommandQuiesceStages* stages = executer == NULL ? NULL :
			dynamic_cast<IExecCommandQuiesceStages*>(executer.get());
		if (stages == NULL)
		{
			allDrained = false;
			continue;
		}
		try
		{
			if (!stages->drainHostQuiesced())
				allDrained = false;
		}
		catch (...)
		{
			allDrained = false;
		}
	}
	{
		std::lock_guard<std::mutex> lock(_executers_mtx);
		_quiesce_host_drained = allDrained;
		_quiesce_stage_in_progress = false;
	}
	return allDrained;
}

bool WtExecuterMgr::snapshot_quiesce_plugin_owners()
{
	std::vector<ExecCmdPtr> executers;
	{
		std::lock_guard<std::mutex> lock(_executers_mtx);
		if (_quiesce_plugin_snapshot_complete)
			return true;
		if (!_quiesce_host_drained)
			return false;
	}
	if (!begin_quiesce_stage(executers))
		return false;

	std::vector<ExecQuiesceOwnerPtr> owners;
	{
		std::lock_guard<std::mutex> lock(_executers_mtx);
		try
		{
			// Preserve owners observed during an earlier incomplete attempt. A
			// retry may discover more owners, but must never release partial ones.
			owners = _quiesce_plugin_owners;
		}
		catch (...)
		{
			_quiesce_stage_in_progress = false;
			return false;
		}
	}
	bool allSnapshotted = true;
	try
	{
		for (const ExecCmdPtr& executer : executers)
		{
			IExecCommandQuiesceStages* stages = executer == NULL ? NULL :
				dynamic_cast<IExecCommandQuiesceStages*>(executer.get());
			if (stages == NULL)
			{
				allSnapshotted = false;
				continue;
			}
			std::vector<ExecQuiesceOwnerPtr> executorOwners;
			bool snapshotOk = false;
			try { snapshotOk = stages->snapshotQuiescePluginOwners(executorOwners); }
			catch (...) { snapshotOk = false; }
			if (!snapshotOk)
				allSnapshotted = false;
			// A failed snapshot can still have returned a partial owner set. Keep
			// every owner it supplied while the complete snapshot is retried.
			for (const ExecQuiesceOwnerPtr& owner : executorOwners)
			{
				if (owner == NULL)
				{
					allSnapshotted = false;
					continue;
				}
				const bool duplicate = std::find_if(owners.begin(), owners.end(),
					[&owner](const ExecQuiesceOwnerPtr& current) {
						return current.get() == owner.get();
					}) != owners.end();
				if (!duplicate)
					owners.push_back(owner);
			}
		}
	}
	catch (...)
	{
		allSnapshotted = false;
	}
	{
		std::lock_guard<std::mutex> lock(_executers_mtx);
		_quiesce_plugin_owners.swap(owners);
		if (allSnapshotted)
		{
			_quiesce_plugin_snapshot_complete = true;
		}
		_quiesce_stage_in_progress = false;
	}
	return allSnapshotted;
}

bool WtExecuterMgr::complete_after_global_plugin_wait()
{
	std::vector<ExecCmdPtr> executers;
	{
		std::lock_guard<std::mutex> lock(_executers_mtx);
		if (!_quiesce_global_plugin_wait_succeeded || _quiesce_stage_in_progress)
			return false;
	}
	if (!begin_quiesce_stage(executers))
		return false;

	// This is only an acknowledgement. Hosts must keep all command, unit,
	// context, factory, and module owners alive until the coordinator publishes
	// global completion after every manager acknowledges successfully.
	bool allAcknowledged = true;
	for (const ExecCmdPtr& executer : executers)
	{
		IExecCommandQuiesceStages* stages = executer == NULL ? NULL :
			dynamic_cast<IExecCommandQuiesceStages*>(executer.get());
		if (stages == NULL)
		{
			allAcknowledged = false;
			continue;
		}
		try
		{
			if (!stages->completeQuiesceAfterGlobalPluginWait())
				allAcknowledged = false;
		}
		catch (...)
		{
			allAcknowledged = false;
		}
	}
	{
		std::lock_guard<std::mutex> lock(_executers_mtx);
		_quiesce_stage_in_progress = false;
	}
	return allAcknowledged;
}

bool WtExecuterMgr::mark_global_plugin_wait_succeeded()
{
	std::lock_guard<std::mutex> lock(_executers_mtx);
	if (!_quiesce_plugin_snapshot_complete || _quiesce_stage_in_progress)
		return false;
	_quiesce_global_plugin_wait_succeeded = true;
	return true;
}

bool WtExecuterMgr::wait_quiesced() const
{
	std::shared_ptr<std::atomic<bool> > completion;
	{
		std::lock_guard<std::mutex> lock(_executers_mtx);
		completion = _global_quiesce_completion;
	}
	return completion != NULL && completion->load(std::memory_order_acquire);
}

WtExecQuiesceCoordinator::WtExecQuiesceCoordinator(
	const std::vector<WtExecuterMgr*>& managers)
	: _completion(new std::atomic<bool>(false))
{
	for (WtExecuterMgr* manager : managers)
	{
		if (manager == NULL)
		{
			_valid = false;
			continue;
		}
		if (std::find(_managers.begin(), _managers.end(), manager) == _managers.end())
			_managers.push_back(manager);
	}
}

bool WtExecQuiesceCoordinator::bind_global_completion_all()
{
	// Managers may appear in overlapping coordinator sets. Lock every manager
	// in one stable order, validate the whole set, then bind in a separate pass.
	// A conflict leaves every previously unbound manager untouched.
	std::vector<WtExecuterMgr*> ordered;
	std::vector<std::unique_lock<std::mutex> > locks;
	try
	{
		ordered = _managers;
		std::sort(ordered.begin(), ordered.end(), std::less<WtExecuterMgr*>());
		locks.reserve(ordered.size());
		for (WtExecuterMgr* manager : ordered)
			locks.emplace_back(manager->_executers_mtx);
	}
	catch (...)
	{
		return false;
	}

	for (WtExecuterMgr* manager : ordered)
	{
		if (manager->_global_quiesce_completion != NULL &&
			manager->_global_quiesce_completion != _completion)
			return false;
	}
	for (WtExecuterMgr* manager : ordered)
		manager->_global_quiesce_completion = _completion;
	return true;
}

void WtExecQuiesceCoordinator::append_plugin_owner(
	const ExecQuiesceOwnerPtr& owner,
	std::vector<ExecQuiesceOwnerPtr>& owners, bool& complete)
{
	if (owner == NULL)
	{
		complete = false;
		return;
	}
	const bool duplicate = std::find_if(owners.begin(), owners.end(),
		[&owner](const ExecQuiesceOwnerPtr& current) {
			return current.get() == owner.get();
		}) != owners.end();
	if (!duplicate)
	{
		try { owners.push_back(owner); }
		catch (...) { complete = false; }
	}
}

bool WtExecQuiesceCoordinator::snapshot_factory_participants()
{
	if (_factory_participants_complete)
		return true;

	bool allFound = true;
	for (WtExecuterMgr* manager : _managers)
	{
		std::vector<ExecCmdPtr> hosts;
		{
			std::lock_guard<std::mutex> lock(manager->_executers_mtx);
			if (!manager->_quiesce_request_succeeded)
			{
				allFound = false;
				continue;
			}
			try { hosts = manager->_quiesce_executer_owners; }
			catch (...) { allFound = false; continue; }
		}

		for (const ExecCmdPtr& host : hosts)
		{
			IExecCommandQuiesceStages* stages = host == NULL ? NULL :
				dynamic_cast<IExecCommandQuiesceStages*>(host.get());
			if (stages == NULL)
			{
				allFound = false;
				continue;
			}
			WtExecuterFactory* factory = NULL;
			try { factory = stages->quiesceFactory(); }
			catch (...) { allFound = false; continue; }
			if (factory == NULL)
				continue;

			IExecFactoryQuiesceLifecycle* lifecycle =
				dynamic_cast<IExecFactoryQuiesceLifecycle*>(factory);
			if (lifecycle == NULL)
			{
				allFound = false;
				continue;
			}
			const void* identity = lifecycle->quiesceFactoryIdentity();
			if (identity == NULL)
			{
				allFound = false;
				continue;
			}

			auto existing = std::find_if(_factory_participants.begin(),
				_factory_participants.end(), [identity](const FactoryParticipant& item) {
					return item.identity == identity;
				});
			if (existing != _factory_participants.end())
			{
				if (existing->factory != factory || existing->lifecycle != lifecycle)
					allFound = false;
				continue;
			}
			try
			{
				FactoryParticipant participant = { identity, factory, lifecycle };
				_factory_participants.push_back(participant);
			}
			catch (...)
			{
				allFound = false;
			}
		}
	}
	_factory_participants_complete = allFound;
	return allFound;
}

bool WtExecQuiesceCoordinator::close_factory_config_admissions()
{
	if (!_factory_participants_complete)
		return false;
	for (const FactoryParticipant& participant : _factory_participants)
		participant.lifecycle->closeFactoryConfigAdmissions();
	return true;
}

bool WtExecQuiesceCoordinator::preflight_factories()
{
	if (!_factory_participants_complete)
		return false;
	bool allCanWait = true;
	for (const FactoryParticipant& participant : _factory_participants)
	{
		try
		{
			if (!participant.lifecycle->preflightQuiesceFactoriesOnCurrentThread())
				allCanWait = false;
		}
		catch (...)
		{
			allCanWait = false;
		}
	}
	return allCanWait;
}

bool WtExecQuiesceCoordinator::close_and_wait_factory_operations()
{
	if (!_factory_participants_complete)
		return false;
	bool allClosed = true;
	for (const FactoryParticipant& participant : _factory_participants)
	{
		try
		{
			if (!participant.lifecycle->closeUnitAdmissions())
				allClosed = false;
		}
		catch (...)
		{
			allClosed = false;
		}
	}
	if (!allClosed)
		return false;

	bool allDrained = true;
	for (const FactoryParticipant& participant : _factory_participants)
	{
		try
		{
			if (!participant.lifecycle->waitForFactoryOperationsQuiesced())
				allDrained = false;
		}
		catch (...)
		{
			allDrained = false;
		}
	}
	return allDrained;
}

bool WtExecQuiesceCoordinator::snapshot_factory_plugin_owners(
	const std::vector<ExecQuiesceOwnerPtr>& hostOwners, bool hostSnapshotsComplete)
{
	if (!_factory_participants_complete)
		return false;
	bool allSnapshotted = hostSnapshotsComplete;
	for (const FactoryParticipant& participant : _factory_participants)
	{
		std::vector<ExecPluginUnitSnapshot> snapshots;
		bool snapshotOk = false;
		try
		{
			snapshotOk = participant.lifecycle->snapshotLivePluginUnitOwners(snapshots);
		}
		catch (...)
		{
			snapshotOk = false;
		}
		if (!snapshotOk)
			allSnapshotted = false;

		for (const ExecPluginUnitSnapshot& snapshot : snapshots)
		{
			if (snapshot.owner == NULL)
			{
				allSnapshotted = false;
				continue;
			}
			auto existing = std::find_if(_factory_plugin_snapshots.begin(),
				_factory_plugin_snapshots.end(), [&snapshot](const ExecPluginUnitSnapshot& current) {
					return current.owner.get() == snapshot.owner.get();
				});
			if (existing != _factory_plugin_snapshots.end())
			{
				if (existing->initialized != snapshot.initialized)
					allSnapshotted = false;
			}
			else
			{
				try { _factory_plugin_snapshots.push_back(snapshot); }
				catch (...) { allSnapshotted = false; }
			}
		}
	}

	std::vector<ExecQuiesceOwnerPtr> owners;
	bool ownersComplete = true;
	for (const ExecQuiesceOwnerPtr& owner : hostOwners)
		append_plugin_owner(owner, owners, ownersComplete);

	for (const ExecPluginUnitSnapshot& snapshot : _factory_plugin_snapshots)
	{
		const bool hostOwned = std::find_if(hostOwners.begin(), hostOwners.end(),
			[&snapshot](const ExecQuiesceOwnerPtr& owner) {
				return owner != NULL && owner.get() == snapshot.owner.get();
			}) != hostOwners.end();
		if (snapshot.initialized && !hostOwned)
			allSnapshotted = false;
		// Uninitialized wrappers own no borrowed ExecuteContext and cannot pass
		// the per-unit wait preflight; retain them without adding them to waits.
		if (snapshot.initialized)
		{
			if (hostOwned)
				append_plugin_owner(snapshot.owner, owners, ownersComplete);
		}
		else
		{
			owners.erase(std::remove_if(owners.begin(), owners.end(),
				[&snapshot](const ExecQuiesceOwnerPtr& owner) {
					return owner.get() == snapshot.owner.get();
				}), owners.end());
		}
	}
	if (!ownersComplete)
		allSnapshotted = false;

	if (allSnapshotted)
	{
		_plugin_owners.swap(owners);
	}
	else
	{
		// Keep every host-registered owner observed so far, plus every factory
		// wrapper snapshot, even when an initialized wrapper has no proven host.
		bool retained = true;
		for (const ExecQuiesceOwnerPtr& owner : owners)
			append_plugin_owner(owner, _plugin_owners, retained);
		if (!retained)
			return false;
	}
	return allSnapshotted;
}

bool WtExecQuiesceCoordinator::request_plugin_owners()
{
	bool allRequested = true;
	for (const ExecQuiesceOwnerPtr& owner : _plugin_owners)
	{
		if (owner == NULL)
		{
			allRequested = false;
			continue;
		}
		try { owner->requestQuiesce(); }
		catch (...) { allRequested = false; }
	}
	return allRequested;
}

bool WtExecQuiesceCoordinator::preflight_plugin_owners()
{
	bool allCanWait = true;
	for (const ExecQuiesceOwnerPtr& owner : _plugin_owners)
	{
		if (owner == NULL)
		{
			allCanWait = false;
			continue;
		}
		try
		{
			if (!owner->canWaitQuiescedOnCurrentThread())
				allCanWait = false;
		}
		catch (...)
		{
			allCanWait = false;
		}
	}
	return allCanWait;
}

bool WtExecQuiesceCoordinator::wait_plugin_owners()
{
	bool allQuiesced = true;
	// A retry repeats every wait, including successful owners from prior rounds.
	for (const ExecQuiesceOwnerPtr& owner : _plugin_owners)
	{
		if (owner == NULL)
		{
			allQuiesced = false;
			continue;
		}
		try
		{
			if (!owner->waitQuiesced())
				allQuiesced = false;
		}
		catch (...)
		{
			allQuiesced = false;
		}
	}
	return allQuiesced;
}

bool WtExecQuiesceCoordinator::quiesce_all()
{
	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (_complete || _completion->load(std::memory_order_acquire))
			return true;
		if (!_valid || _in_progress)
			return false;
		_in_progress = true;
	}

	bool allComplete = false;
	try
	{
		const bool allBound = bind_global_completion_all();

		const auto runAll = [this](bool (WtExecuterMgr::*stage)()) {
			bool success = true;
			for (WtExecuterMgr* manager : _managers)
			{
				try
				{
					if (!(manager->*stage)())
						success = false;
				}
				catch (...)
				{
					success = false;
				}
			}
			return success;
		};

		bool factoryParticipantsReady = false;
		bool hostOwnersReady = false;
		std::vector<ExecQuiesceOwnerPtr> hostOwners;
		if (allBound && runAll(&WtExecuterMgr::request_quiesce_all) &&
			snapshot_factory_participants())
		{
			// The first call closes each unique factory's load/config gate. Close
			// every gate before probing any factory so no later factory can load.
			const bool configClosed = close_factory_config_admissions();
			const bool factoriesCanWait = preflight_factories();
			const bool hostsCanWait = runAll(&WtExecuterMgr::preflight_quiesce_all);
			factoryParticipantsReady = configClosed && factoriesCanWait && hostsCanWait;
			if (factoryParticipantsReady && runAll(&WtExecuterMgr::drain_host_quiesced_all))
			{
				// Host ingress remains open to admitted create/init work until every
				// host drains. Close all factory unit gates only after that barrier.
				if (close_and_wait_factory_operations())
				{
					hostOwnersReady = runAll(&WtExecuterMgr::snapshot_quiesce_plugin_owners);
					bool hostOwnersComplete = true;
					for (WtExecuterMgr* manager : _managers)
					{
						std::lock_guard<std::mutex> lock(manager->_executers_mtx);
						for (const ExecQuiesceOwnerPtr& owner : manager->_quiesce_plugin_owners)
							append_plugin_owner(owner, hostOwners, hostOwnersComplete);
					}
					hostOwnersReady = hostOwnersReady && hostOwnersComplete;
					const bool factoryOwnersReady =
						snapshot_factory_plugin_owners(hostOwners, hostOwnersReady);
					const bool secondFactoryPreflight = preflight_factories();
					if (factoryOwnersReady && secondFactoryPreflight &&
						request_plugin_owners() && preflight_plugin_owners() &&
						wait_plugin_owners())
					{
						bool allMarked = true;
						for (WtExecuterMgr* manager : _managers)
						{
							try
							{
								if (!manager->mark_global_plugin_wait_succeeded())
									allMarked = false;
							}
							catch (...)
							{
								allMarked = false;
							}
						}
						if (allMarked && runAll(&WtExecuterMgr::complete_after_global_plugin_wait))
						{
							// The only externally visible success signal is published after
							// all plugin waits and every host acknowledgement have succeeded.
							_completion->store(true, std::memory_order_release);
							allComplete = true;
						}
					}
				}
			}
		}
	}
	catch (...)
	{
		allComplete = false;
	}

	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (allComplete)
			_complete = true;
		_in_progress = false;
	}
	return allComplete;
}

bool WtExecQuiesceCoordinator::is_complete() const
{
	return _completion->load(std::memory_order_acquire);
}

ExecCmdPtr WtExecuterMgr::find_executer(const char* executorId) const
{
	if (executorId == NULL || executorId[0] == '\0')
		return ExecCmdPtr();

	std::lock_guard<std::mutex> lock(_executers_mtx);
	auto it = _executers.find(executorId);
	return it != _executers.end() ? it->second : ExecCmdPtr();
}

ExecUnitInitStatus WtExecuterMgr::query_exec_unit_init_status(const char* executorId, const char* stdCode) const
{
	ExecUnitInitStatus status;
	if (executorId == NULL || executorId[0] == '\0')
	{
		status.query_result = ExecUnitInitQueryResult::INVALID_ARGUMENT;
		status.error = "executor ID is empty";
		return status;
	}
	if (stdCode == NULL || stdCode[0] == '\0')
	{
		status.query_result = ExecUnitInitQueryResult::INVALID_ARGUMENT;
		status.error = "standard code is empty";
		return status;
	}

	ExecCmdPtr executer;
	{
		std::lock_guard<std::mutex> lock(_executers_mtx);
		auto it = _executers.find(executorId);
		if (it != _executers.end())
			executer = it->second;
	}
	if (executer == NULL)
	{
		status.query_result = ExecUnitInitQueryResult::EXECUTOR_NOT_FOUND;
		status.error = "executor is not registered";
		return status;
	}

	const IExecUnitInitStatusProvider* provider =
		dynamic_cast<const IExecUnitInitStatusProvider*>(executer.get());
	if (provider == NULL)
	{
		status.query_result = ExecUnitInitQueryResult::UNSUPPORTED;
		status.error = "executor does not provide unit initialization status";
		return status;
	}
	return provider->queryUnitInitStatus(stdCode);
}

ExecUnitInitClearResult WtExecuterMgr::clear_failed_exec_unit_init_status(
	const char* executorId, const char* stdCode)
{
	if (executorId == NULL || executorId[0] == '\0' || stdCode == NULL || stdCode[0] == '\0')
		return ExecUnitInitClearResult::INVALID_ARGUMENT;

	ExecCmdPtr executer;
	{
		std::lock_guard<std::mutex> lock(_executers_mtx);
		auto it = _executers.find(executorId);
		if (it != _executers.end())
			executer = it->second;
	}
	if (executer == NULL)
		return ExecUnitInitClearResult::EXECUTOR_NOT_FOUND;

	IExecUnitInitStatusProvider* provider =
		dynamic_cast<IExecUnitInitStatusProvider*>(executer.get());
	return provider != NULL ? provider->clearFailedUnitInitStatus(stdCode) : ExecUnitInitClearResult::UNSUPPORTED;
}

void WtExecuterMgr::set_positions(wt_hashmap<std::string, double> target_pos)
{
	if(_filter_mgr != NULL)
	{
		wt_hashmap<std::string, double> des_port;
		for(auto& m : target_pos)
		{
			const auto& stdCode = m.first;
			double& desVol = (double&)m.second;
			double oldVol = desVol;

			bool isFltd = _filter_mgr->is_filtered_by_code(stdCode.c_str(), desVol);
			if (!isFltd)
			{
				if (!decimal::eq(desVol, oldVol))
				{
					//输出日志
					WTSLogger::info("[Filters] {} target position reset by code filter: {} -> {}", stdCode.c_str(), oldVol, desVol);
				}

				des_port[stdCode] = desVol;
			}
			else
			{
				//输出日志
				WTSLogger::info("[Filters] {} target position ignored by filter", stdCode.c_str());
			}
		}

		des_port.swap(target_pos);
	}

	const std::vector<ExecCmdPtr> executers = snapshot_executers();
	for (const ExecCmdPtr& executer : executers)
	{
		if (_filter_mgr && _filter_mgr->is_filtered_by_executer(executer->name()))
		{
			WTSLogger::info("[Filters] Executer {} is filtered, all signals will be ignored", executer->name());
			continue;
		}
		executer->set_position(target_pos);
	}
}

void WtExecuterMgr::handle_pos_change(const char* stdCode, double targetPos, double diffPos, const char* execid /* = "ALL" */)
{
	if(_filter_mgr != NULL)
	{
		double oldVol = targetPos;
		bool isFltd = _filter_mgr->is_filtered_by_code(stdCode, targetPos);
		if (!isFltd)
		{
			if (!decimal::eq(targetPos, oldVol))
			{
				//输出日志
				WTSLogger::info("[Filters] {} target position reset by filter: {} -> {}", stdCode, oldVol, targetPos);
				//差量也要重算
				diffPos += (targetPos - oldVol);
			}
		}
		else
		{
			//输出日志
			WTSLogger::info("[Filters] {} target position ignored by filter", stdCode);
			return;
		}
	}

	const std::vector<ExecCmdPtr> executers = snapshot_executers();
	for (const ExecCmdPtr& executer : executers)
	{
		if (_filter_mgr && _filter_mgr->is_filtered_by_executer(executer->name()))
		{
			WTSLogger::info("[Filters] All signals to executer {} are ignored by executer filter", executer->name());
			continue;
		}

		auto it = _routed_executers.find(executer->name());
		if (it == _routed_executers.end() && strcmp(execid, "ALL") == 0)
			executer->on_position_changed(stdCode, diffPos);
		else if(strcmp(executer->name(), execid) == 0)
			executer->on_position_changed(stdCode, diffPos);
	}
}

void WtExecuterMgr::handle_tick(const char* stdCode, WTSTickData* curTick)
{
	//for (auto it = _executers.begin(); it != _executers.end(); it++)
	//{
	//	ExecCmdPtr& executer = (*it);
	//	executer->on_tick(stdCode, curTick);
	//}

	const std::vector<ExecCmdPtr> executers = snapshot_executers();
	for (const ExecCmdPtr& executer : executers)
	{
		executer->on_tick(stdCode, curTick);
	}
}

void WtExecuterMgr::add_target_to_cache(const char* stdCode, double targetPos, const char* execid /* = "ALL" */)
{
	TargetsMap& targets = _all_cached_targets[execid];
	double& vol = targets[stdCode];
	vol += targetPos;
}

void WtExecuterMgr::commit_cached_targets(double scale /* = 1.0 */)
{
	for(auto& v : _all_cached_targets)
	{	
		//先对组合进行缩放
		[[maybe_unused]] const char* execid = v.first.c_str();
		TargetsMap& target_pos = (TargetsMap&)v.second;
		for(auto& item : target_pos)
		{
			[[maybe_unused]] const auto& stdCode = item.first;
			double& pos = (double&)item.second;

			if(decimal::eq(pos, 0))
				continue;

			double symbol = pos / abs(pos);
			pos = decimal::rnd(abs(pos)*scale)*symbol;
		}

		//然后根据过滤器调整目标仓位
		if (_filter_mgr != NULL)
		{
			TargetsMap des_port;
			for (auto& m : target_pos)
			{
				const auto& stdCode = m.first;
				double& desVol = (double&)m.second;
				double oldVol = desVol;

				bool isFltd = _filter_mgr->is_filtered_by_code(stdCode.c_str(), desVol);
				if (!isFltd)
				{
					if (!decimal::eq(desVol, oldVol))
					{
						//输出日志
						WTSLogger::info("[Filters] {} target position reset by code filter: {} -> {}", stdCode.c_str(), oldVol, desVol);
					}

					des_port[stdCode] = desVol;
				}
				else
				{
					//输出日志
					WTSLogger::info("[Filters] {} target position ignored by filter", stdCode.c_str());
				}
			}

			target_pos.swap(des_port);
		}
	}

	//遍历执行器
	const std::vector<ExecCmdPtr> executers = snapshot_executers();
	for (const ExecCmdPtr& executer : executers)
	{
		if (_filter_mgr && _filter_mgr->is_filtered_by_executer(executer->name()))
		{
			WTSLogger::info("[Filters] Executer {} is filtered, all signals will be ignored", executer->name());
			continue;
		}

		//先找自己对应的组合
		auto it = _all_cached_targets.find(executer->name());

		//如果找不到，就找全部组合
		if (it == _all_cached_targets.end())
			it = _all_cached_targets.find("ALL");

		if (it == _all_cached_targets.end())
			continue;

		executer->set_position(it->second);
	}

	//提交完了以后，清理掉全部缓存的目标仓位
	_all_cached_targets.clear();
}

bool WtExecuterMgr::load_router_rules(WTSVariant* config)
{
	if (config == NULL || !config->isArray())
		return false;

	std::vector<std::pair<std::string, std::vector<std::string>>> parsedRules;
	try
	{
		parsedRules.reserve(config->size());
		for (uint32_t i = 0; i < config->size(); ++i)
		{
			WTSVariant* item = config->get(i);
			if (item == NULL || !item->isObject())
			{
				WTSLogger::error("Router rule {} is invalid: expected an object", i);
				return false;
			}

			WTSVariant* itemStrategy = item->get("strategy");
			if (itemStrategy == NULL)
			{
				WTSLogger::error("Router rule {} is invalid: strategy is missing", i);
				return false;
			}
			if (itemStrategy->type() != WTSVariant::VT_String)
			{
				WTSLogger::error("Router rule {} is invalid: strategy must be a string", i);
				return false;
			}

			WTSVariant* itemExec = item->get("executer");
			if (itemExec == NULL)
			{
				WTSLogger::error("Router rule {} is invalid: executer is missing", i);
				return false;
			}

			std::vector<std::string> executerIds;
			if (itemExec->isArray())
			{
				executerIds.reserve(itemExec->size());
				for (uint32_t k = 0; k < itemExec->size(); ++k)
				{
					WTSVariant* executer = itemExec->get(k);
					if (executer == NULL || executer->type() != WTSVariant::VT_String)
					{
						WTSLogger::error("Router rule {} executer {} is invalid: expected a string", i, k);
						return false;
					}
					executerIds.push_back(executer->asString());
				}
			}
			else if (itemExec->type() == WTSVariant::VT_String)
			{
				executerIds.push_back(itemExec->asString());
			}
			else
			{
				WTSLogger::error("Router rule {} is invalid: executer must be a string or array", i);
				return false;
			}

			parsedRules.emplace_back(itemStrategy->asString(), std::move(executerIds));
		}
	}
	catch (...)
	{
		WTSLogger::error("Router rules could not be validated");
		return false;
	}

	// Apply only after every rule has passed validation. Build candidate copies so
	// even allocation failure cannot leave the live routing tables half-updated.
	try
	{
		auto routerRules = _router_rules_strategy;
		auto routedExecuters = _routed_executers;
		for (const auto& rule : parsedRules)
		{
			for (const std::string& execId : rule.second)
			{
				routerRules[rule.first].insert(execId);
				routedExecuters.insert(execId);
			}
		}
		_router_rules_strategy.swap(routerRules);
		_routed_executers.swap(routedExecuters);
	}
	catch (...)
	{
		WTSLogger::error("Validated router rules could not be committed");
		return false;
	}

	WTSLogger::info("{} router rules loaded", _router_rules_strategy.size());

	return true;
}

#pragma endregion "WtExecuterMgr"
