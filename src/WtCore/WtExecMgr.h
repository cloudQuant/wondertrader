#pragma once
#include <boost/core/noncopyable.hpp>

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>
#include "WtLocalExecuter.h"
#include "ExecCommandQuiesce.h"

NS_WTP_BEGIN
class WtFilterMgr;
class WtExecQuiesceCoordinator;

typedef std::function<void(ExecCmdPtr)> EnumExecuterCb;

class WtExecuterMgr : private boost::noncopyable
{
public:
	WtExecuterMgr():_filter_mgr(NULL){}

	inline void set_filter_mgr(WtFilterMgr* mgr) { _filter_mgr = mgr; }

	inline void	add_executer(ExecCmdPtr executer)
	{
		if (executer == NULL)
			return;
		std::lock_guard<std::mutex> lock(_executers_mtx);
		if (_quiesce_requested)
			return;
		_executers[executer->name()] = executer;
	}

	void	enum_executer(EnumExecuterCb cb);

	// Legacy local wait cannot prove the cross-manager protocol. It is true only
	// after the shared coordinator completion token is published.
	bool wait_quiesced() const;

	// Status snapshots copy the error text. Clear only removes a terminal FAILED
	// record; callers should reclaim failures they no longer need (one record per
	// failed code is retained without a fixed capacity limit).
	ExecUnitInitStatus query_exec_unit_init_status(const char* executorId, const char* stdCode) const;
	ExecUnitInitClearResult clear_failed_exec_unit_init_status(const char* executorId, const char* stdCode);
	ExecCmdPtr find_executer(const char* executorId) const;

	void	set_positions(wt_hashmap<std::string, double> target_pos);
	void	handle_pos_change(const char* stdCode, double targetPos, double diffPos, const char* execid = "ALL");
	void	handle_tick(const char* stdCode, WTSTickData* curTick);

	/*
	 *	加载路由规则
	 */
	bool	load_router_rules(WTSVariant* config);

	/*
	 *	
	 */
	inline const wt_hashset<std::string>& get_route_by_strategy(const char* strategyid)
	{
		// I03-NEW-124: built once by the (thread-safe) static initializer; the
		// CTA pool calls this from handle_pos_change() concurrently, so the
		// set must not be filled lazily on first use.
		static const wt_hashset<std::string> ALL_EXECUTERS = []() {
			wt_hashset<std::string> s;
			s.insert("ALL");
			return s;
		}();

		if (_router_rules_strategy.empty())
			return ALL_EXECUTERS;

		auto it = _router_rules_strategy.find(strategyid);
		if (it == _router_rules_strategy.end())
			return ALL_EXECUTERS;

		return it->second;
	}

	/*
	 *	清除缓存的目标仓位
	 */
	inline void	clear_cached_targets()
	{
		_all_cached_targets.clear();
	}

	/*
	 *	将目标仓位加入缓存
	 *	@stdCode	合约代码
	 *	@targetPos	目标仓位
	 *	@execid		执行器ID
	 */
	void	add_target_to_cache(const char* stdCode, double targetPos, const char* execid = "ALL");

	/*
	 *	提交缓存的目标头寸
	 *	@scale	风控系数
	 */
	void	commit_cached_targets(double scale = 1.0);

private:
	friend class WtExecQuiesceCoordinator;
	bool begin_quiesce_stage(std::vector<ExecCmdPtr>& executers);
	bool request_quiesce_all();
	bool preflight_quiesce_all();
	bool drain_host_quiesced_all();
	bool snapshot_quiesce_plugin_owners();
	bool mark_global_plugin_wait_succeeded();
	bool complete_after_global_plugin_wait();

	typedef wt_hashmap<std::string, ExecCmdPtr> ExecuterMap;
	std::vector<ExecCmdPtr> snapshot_executers() const;
	mutable std::mutex	_executers_mtx;
	ExecuterMap		_executers;
	bool			_quiesce_requested = false;
	bool			_quiesce_request_succeeded = false;
	bool			_quiesce_stage_in_progress = false;
	bool			_quiesce_host_preflight_succeeded = false;
	bool			_quiesce_host_drained = false;
	bool			_quiesce_plugin_snapshot_complete = false;
	bool			_quiesce_global_plugin_wait_succeeded = false;
	std::vector<ExecCmdPtr> _quiesce_executer_owners;
	std::vector<ExecQuiesceOwnerPtr> _quiesce_plugin_owners;
	std::shared_ptr<std::atomic<bool> > _global_quiesce_completion;
	WtFilterMgr*	_filter_mgr;

	typedef wt_hashmap<std::string, double> TargetsMap;
	wt_hashmap<std::string, TargetsMap>	_all_cached_targets;

	typedef wt_hashset<std::string>	ExecuterSet;
	wt_hashmap<std::string, ExecuterSet>	_router_rules_strategy;

	wt_hashset<std::string>	_routed_executers;
};

/**
 * Coordinates executor shutdown across every manager owned by one runtime.
 * The caller must keep all managers and their host/context owners alive until
 * quiesce_all() returns true, and keep factories alive because hosts expose
 * borrowed factory pointers. A false result is retryable; the coordinator and
 * managers retain all known wrapper, host, and factory snapshots. Successful
 * requests, preflights, waits, and acknowledgements may be repeated.
 * All managers in one runtime must be claimed by the same coordinator. The
 * first successful batch bind permanently assigns each manager's completion
 * token; a competing overlapping coordinator fails without binding any of its
 * otherwise-unclaimed managers. Such a failure can be retried safely, but a
 * manager already claimed by another coordinator cannot be transferred.
 */
class WtExecQuiesceCoordinator : private boost::noncopyable
{
public:
	explicit WtExecQuiesceCoordinator(const std::vector<WtExecuterMgr*>& managers);
	bool quiesce_all();
	bool is_complete() const;

private:
	struct FactoryParticipant
	{
		const void* identity;
		WtExecuterFactory* factory;
		IExecFactoryQuiesceLifecycle* lifecycle;
	};

	bool snapshot_factory_participants();
	bool bind_global_completion_all();
	bool close_factory_config_admissions();
	bool preflight_factories();
	bool close_and_wait_factory_operations();
	bool snapshot_factory_plugin_owners(
		const std::vector<ExecQuiesceOwnerPtr>& hostOwners, bool hostSnapshotsComplete);
	bool request_plugin_owners();
	bool preflight_plugin_owners();
	bool wait_plugin_owners();
	void append_plugin_owner(const ExecQuiesceOwnerPtr& owner,
		std::vector<ExecQuiesceOwnerPtr>& owners, bool& complete);

	std::vector<WtExecuterMgr*> _managers;
	std::vector<FactoryParticipant> _factory_participants;
	std::vector<ExecPluginUnitSnapshot> _factory_plugin_snapshots;
	std::vector<ExecQuiesceOwnerPtr> _plugin_owners;
	std::shared_ptr<std::atomic<bool> > _completion;
	mutable std::mutex _mutex;
	bool _in_progress = false;
	bool _complete = false;
	bool _valid = true;
	bool _factory_participants_complete = false;
};
NS_WTP_END
