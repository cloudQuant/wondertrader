#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "WtCore/WtExecMgr.h"
#include "WtCore/ExecCommandQuiesce.h"

USING_NS_WTP;

namespace {

class EventLog
{
public:
	void record(const std::string& event)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_events.push_back(event);
	}

	std::vector<std::string> events() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _events;
	}

	size_t indexOf(const std::string& event) const
	{
		const std::vector<std::string> copy = events();
		const auto it = std::find(copy.begin(), copy.end(), event);
		return it == copy.end() ? copy.size() : static_cast<size_t>(it - copy.begin());
	}

private:
	mutable std::mutex _mutex;
	std::vector<std::string> _events;
};

size_t firstIndex(EventLog& log, const std::vector<std::string>& events)
{
	size_t first = log.events().size();
	for (const std::string& event : events)
		first = std::min(first, log.indexOf(event));
	return first;
}

size_t lastIndex(EventLog& log, const std::vector<std::string>& events)
{
	size_t last = 0;
	for (const std::string& event : events)
		last = std::max(last, log.indexOf(event));
	return last;
}

void expectAllBefore(EventLog& log, const std::vector<std::string>& before,
	const std::vector<std::string>& after)
{
	for (const std::string& event : before)
		EXPECT_LT(log.indexOf(event), log.events().size()) << event;
	for (const std::string& event : after)
		EXPECT_LT(log.indexOf(event), log.events().size()) << event;
	EXPECT_LT(lastIndex(log, before), firstIndex(log, after));
}

class FakePluginQuiesceOwner : public IExecCommandQuiesce
{
public:
	FakePluginQuiesceOwner(const std::string& name, EventLog* log,
		int successAfterWaits = 1, bool gateWait = false)
		: _name(name), _log(log), _success_after_waits(successAfterWaits),
		  _wait_released(!gateWait)
	{
	}

	void requestQuiesce() override
	{
		{
			std::lock_guard<std::mutex> lock(_mutex);
			++_request_calls;
		}
		if (_log != NULL)
			_log->record("plugin." + _name + ".request");
	}

	bool canWaitQuiescedOnCurrentThread() const override
	{
		if (_log != NULL)
			_log->record("plugin." + _name + ".preflight");
		return _can_wait.load(std::memory_order_acquire);
	}

	bool waitQuiesced() override
	{
		std::unique_lock<std::mutex> lock(_mutex);
		++_wait_calls;
		if (_log != NULL)
			_log->record("plugin." + _name + ".wait");
		_condition.notify_all();
		_condition.wait(lock, [this]() { return _wait_released; });
		return _wait_calls >= _success_after_waits;
	}

	void setCanWait(bool value) { _can_wait.store(value, std::memory_order_release); }

	bool waitForWait(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(_mutex);
		return _condition.wait_for(lock, timeout, [this]() { return _wait_calls > 0; });
	}

	void releaseWait()
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_wait_released = true;
		_condition.notify_all();
	}

	int requestCalls() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _request_calls;
	}

	int waitCalls() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _wait_calls;
	}

private:
	std::string _name;
	EventLog* _log;
	int _success_after_waits;
	mutable std::mutex _mutex;
	std::condition_variable _condition;
	bool _wait_released;
	int _request_calls = 0;
	int _wait_calls = 0;
	std::atomic<bool> _can_wait{ true };
};

class FakeQuiesceFactory : public WtExecuterFactory
{
public:
	FakeQuiesceFactory(const std::string& name, EventLog* log)
		: _name(name), _log(log)
	{
	}

	const void* quiesceFactoryIdentity() const noexcept override { return this; }

	void closeFactoryConfigAdmissions() noexcept override
	{
		_config_closed.store(true, std::memory_order_release);
		++_close_config_calls;
		if (_log != NULL)
			_log->record("factory." + _name + ".close_config");
	}

	bool preflightQuiesceFactoriesOnCurrentThread() const override
	{
		const int call = ++_preflight_calls;
		if (_log != NULL)
			_log->record("factory." + _name + ".preflight." + std::to_string(call));
		if (!_config_closed.load(std::memory_order_acquire))
			return false;
		std::lock_guard<std::mutex> lock(_mutex);
		return _preflight_results.empty() || _preflight_results[std::min<size_t>(
			static_cast<size_t>(call - 1), _preflight_results.size() - 1)];
	}

	bool closeUnitAdmissions() noexcept override
	{
		_unit_admissions_closed.store(true, std::memory_order_release);
		++_close_unit_calls;
		if (_log != NULL)
			_log->record("factory." + _name + ".close_units");
		return true;
	}

	bool waitForFactoryOperationsQuiesced() override
	{
		++_operation_wait_calls;
		if (_log != NULL)
			_log->record("factory." + _name + ".wait_operations");
		return _unit_admissions_closed.load(std::memory_order_acquire) &&
			_operation_wait_result.load(std::memory_order_acquire);
	}

	bool snapshotLivePluginUnitOwners(std::vector<ExecPluginUnitSnapshot>& out) const override
	{
		++_snapshot_calls;
		if (_log != NULL)
			_log->record("factory." + _name + ".snapshot");
		out.clear();
		std::lock_guard<std::mutex> lock(_mutex);
		if (!_snapshot_result)
			return false;
		for (const auto& entry : _owners)
		{
			ExecQuiesceOwnerPtr owner = entry.first.lock();
			if (owner != NULL)
				out.push_back(ExecPluginUnitSnapshot{ owner, entry.second });
		}
		return true;
	}

	void addOwner(const ExecQuiesceOwnerPtr& owner, bool initialized = true)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_owners.push_back(std::make_pair(std::weak_ptr<IExecCommandQuiesce>(owner), initialized));
	}

	void setPreflightResults(const std::vector<bool>& results)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_preflight_results = results;
	}

	void setOperationWaitResult(bool result)
	{
		_operation_wait_result.store(result, std::memory_order_release);
	}

	void setSnapshotResult(bool result)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_snapshot_result = result;
	}
	int closeConfigCalls() const { return _close_config_calls.load(); }
	int preflightCalls() const { return _preflight_calls.load(); }
	int closeUnitCalls() const { return _close_unit_calls.load(); }
	int operationWaitCalls() const { return _operation_wait_calls.load(); }
	int snapshotCalls() const { return _snapshot_calls.load(); }

private:
	std::string _name;
	EventLog* _log;
	mutable std::mutex _mutex;
	std::vector<std::pair<std::weak_ptr<IExecCommandQuiesce>, bool> > _owners;
	std::vector<bool> _preflight_results;
	std::atomic<bool> _config_closed{ false };
	std::atomic<bool> _unit_admissions_closed{ false };
	std::atomic<bool> _operation_wait_result{ true };
	bool _snapshot_result = true;
	std::atomic<int> _close_config_calls{ 0 };
	mutable std::atomic<int> _preflight_calls{ 0 };
	std::atomic<int> _close_unit_calls{ 0 };
	std::atomic<int> _operation_wait_calls{ 0 };
	mutable std::atomic<int> _snapshot_calls{ 0 };
};

class FakeStagedExecuter : public IExecCommand,
	public IExecCommandQuiesce,
	public IExecCommandQuiesceStages
{
public:
	FakeStagedExecuter(const char* name, EventLog* log)
		: IExecCommand(name), _log(log)
	{
	}

	void requestQuiesce() override
	{
		++_request_calls;
		if (_log != NULL)
			_log->record("host." + std::string(name()) + ".request");
		WtExecuterMgr* manager = _manager;
		if (manager != NULL)
			_request_can_query_registry.store(
				manager->find_executer(name()).get() == this, std::memory_order_release);
	}

	bool canWaitQuiescedOnCurrentThread() const override { return false; }

	bool waitQuiesced() override
	{
		++_legacy_wait_calls;
		return false;
	}

	bool preflightQuiesceOnCurrentThread() const override
	{
		const int call = ++_preflight_calls;
		if (_log != NULL)
			_log->record("host." + std::string(name()) + ".preflight." + std::to_string(call));

		bool result = true;
		{
			std::lock_guard<std::mutex> lock(_mutex);
			if (!_preflight_results.empty())
				result = _preflight_results[std::min<size_t>(
					static_cast<size_t>(call - 1), _preflight_results.size() - 1)];
		}
		{
			std::lock_guard<std::mutex> lock(_unsafe_mutex);
			if (_unsafe_thread == std::this_thread::get_id())
				result = false;
		}
		return result;
	}

	bool drainHostQuiesced() override
	{
		++_drain_calls;
		if (_log != NULL)
			_log->record("host." + std::string(name()) + ".drain");
		return _drain_result.load(std::memory_order_acquire);
	}

	bool snapshotQuiescePluginOwners(std::vector<ExecQuiesceOwnerPtr>& out) const override
	{
		++_snapshot_calls;
		if (_log != NULL)
			_log->record("host." + std::string(name()) + ".snapshot");
		std::lock_guard<std::mutex> lock(_mutex);
		out = _plugin_owners;
		return _snapshot_result;
	}

	WtExecuterFactory* quiesceFactory() const override { return _factory; }

	bool completeQuiesceAfterGlobalPluginWait() override
	{
		const int call = ++_ack_calls;
		if (_log != NULL)
			_log->record("host." + std::string(name()) + ".ack");
		std::lock_guard<std::mutex> lock(_mutex);
		if (_ack_results.empty())
			return true;
		return _ack_results[std::min<size_t>(
			static_cast<size_t>(call - 1), _ack_results.size() - 1)];
	}

	void addPluginOwner(const ExecQuiesceOwnerPtr& owner)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_plugin_owners.push_back(owner);
	}

	void dropPluginOwnerReferences()
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_plugin_owners.clear();
	}

	void setManager(WtExecuterMgr* manager) { _manager = manager; }
	void setFactory(WtExecuterFactory* factory) { _factory = factory; }
	void setPreflightResults(const std::vector<bool>& results)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_preflight_results = results;
	}
	void setAckResults(const std::vector<bool>& results)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_ack_results = results;
	}
	void setSnapshotResult(bool value)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_snapshot_result = value;
	}
	void setDrainResult(bool value) { _drain_result.store(value, std::memory_order_release); }
	void setUnsafeThread(std::thread::id thread)
	{
		std::lock_guard<std::mutex> lock(_unsafe_mutex);
		_unsafe_thread = thread;
	}

	int requestCalls() const { return _request_calls.load(); }
	int preflightCalls() const { return _preflight_calls.load(); }
	int drainCalls() const { return _drain_calls.load(); }
	int snapshotCalls() const { return _snapshot_calls.load(); }
	int ackCalls() const { return _ack_calls.load(); }
	int legacyWaitCalls() const { return _legacy_wait_calls.load(); }
	bool requestCanQueryRegistry() const { return _request_can_query_registry.load(); }

private:
	EventLog* _log;
	WtExecuterMgr* _manager = NULL;
	WtExecuterFactory* _factory = NULL;
	mutable std::mutex _mutex;
	std::vector<ExecQuiesceOwnerPtr> _plugin_owners;
	std::vector<bool> _preflight_results;
	std::vector<bool> _ack_results;
	bool _snapshot_result = true;
	std::atomic<bool> _drain_result{ true };
	mutable std::atomic<int> _preflight_calls{ 0 };
	std::atomic<int> _request_calls{ 0 };
	std::atomic<int> _drain_calls{ 0 };
	mutable std::atomic<int> _snapshot_calls{ 0 };
	std::atomic<int> _ack_calls{ 0 };
	std::atomic<int> _legacy_wait_calls{ 0 };
	std::atomic<bool> _request_can_query_registry{ false };
	mutable std::mutex _unsafe_mutex;
	std::thread::id _unsafe_thread;
};

class LegacyQuiesceExecuter : public IExecCommand, public IExecCommandQuiesce
{
public:
	explicit LegacyQuiesceExecuter(const char* name) : IExecCommand(name) {}
	void requestQuiesce() override { ++request_calls; }
	bool canWaitQuiescedOnCurrentThread() const override { return true; }
	bool waitQuiesced() override { return true; }
	std::atomic<int> request_calls{ 0 };
};

class PluginWaitReleaseGuard
{
public:
	explicit PluginWaitReleaseGuard(std::vector<std::shared_ptr<FakePluginQuiesceOwner> > owners)
		: _owners(std::move(owners)) {}
	~PluginWaitReleaseGuard()
	{
		for (const auto& owner : _owners)
			owner->releaseWait();
	}

private:
	std::vector<std::shared_ptr<FakePluginQuiesceOwner> > _owners;
};

TEST(WtExecMgrQuiesceCoordinator, RunsEveryPhaseAcrossManagersBeforeAdvancing)
{
	EventLog log;
	FakeQuiesceFactory firstFactory("first", &log);
	FakeQuiesceFactory secondFactory("second", &log);
	WtExecuterMgr firstManager;
	WtExecuterMgr secondManager;
	std::shared_ptr<FakeStagedExecuter> first(new FakeStagedExecuter("first", &log));
	std::shared_ptr<FakeStagedExecuter> second(new FakeStagedExecuter("second", &log));
	std::shared_ptr<FakePluginQuiesceOwner> firstPlugin(
		new FakePluginQuiesceOwner("first-unit", &log));
	std::shared_ptr<FakePluginQuiesceOwner> secondPlugin(
		new FakePluginQuiesceOwner("second-unit", &log));
	first->addPluginOwner(firstPlugin);
	second->addPluginOwner(secondPlugin);
	firstFactory.addOwner(ExecQuiesceOwnerPtr(firstPlugin));
	secondFactory.addOwner(ExecQuiesceOwnerPtr(secondPlugin));
	first->setFactory(&firstFactory);
	second->setFactory(&secondFactory);
	first->setManager(&firstManager);
	second->setManager(&secondManager);
	firstManager.add_executer(ExecCmdPtr(first));
	secondManager.add_executer(ExecCmdPtr(second));
	WtExecQuiesceCoordinator coordinator({ &firstManager, &secondManager });

	EXPECT_FALSE(firstManager.wait_quiesced());
	ASSERT_TRUE(coordinator.quiesce_all());
	EXPECT_TRUE(coordinator.is_complete());
	EXPECT_TRUE(firstManager.wait_quiesced());
	EXPECT_TRUE(secondManager.wait_quiesced());
	EXPECT_TRUE(first->requestCanQueryRegistry());

	expectAllBefore(log,
		{ "host.first.request", "host.second.request" },
		{ "factory.first.close_config", "factory.second.close_config" });
	expectAllBefore(log,
		{ "factory.first.close_config", "factory.second.close_config" },
		{ "factory.first.preflight.1", "factory.second.preflight.1" });
	expectAllBefore(log,
		{ "factory.first.preflight.1", "factory.second.preflight.1",
		  "host.first.preflight.1", "host.second.preflight.1" },
		{ "host.first.drain", "host.second.drain" });
	expectAllBefore(log,
		{ "host.first.drain", "host.second.drain" },
		{ "factory.first.close_units", "factory.second.close_units" });
	expectAllBefore(log,
		{ "factory.first.close_units", "factory.second.close_units" },
		{ "factory.first.wait_operations", "factory.second.wait_operations" });
	expectAllBefore(log,
		{ "factory.first.wait_operations", "factory.second.wait_operations" },
		{ "host.first.snapshot", "host.second.snapshot",
		  "factory.first.snapshot", "factory.second.snapshot" });
	expectAllBefore(log,
		{ "host.first.snapshot", "host.second.snapshot",
		  "factory.first.snapshot", "factory.second.snapshot" },
		{ "factory.first.preflight.2", "factory.second.preflight.2" });
	expectAllBefore(log,
		{ "factory.first.preflight.2", "factory.second.preflight.2" },
		{ "plugin.first-unit.request", "plugin.second-unit.request" });
	expectAllBefore(log,
		{ "plugin.first-unit.request", "plugin.second-unit.request" },
		{ "plugin.first-unit.preflight", "plugin.second-unit.preflight" });
	expectAllBefore(log,
		{ "plugin.first-unit.preflight", "plugin.second-unit.preflight" },
		{ "plugin.first-unit.wait", "plugin.second-unit.wait" });
	expectAllBefore(log,
		{ "plugin.first-unit.wait", "plugin.second-unit.wait" },
		{ "host.first.ack", "host.second.ack" });
	EXPECT_EQ(first->legacyWaitCalls(), 0);
	EXPECT_EQ(second->legacyWaitCalls(), 0);
	EXPECT_EQ(firstFactory.closeConfigCalls(), 1);
	EXPECT_EQ(secondFactory.closeConfigCalls(), 1);
	EXPECT_EQ(firstFactory.preflightCalls(), 2);
	EXPECT_EQ(secondFactory.preflightCalls(), 2);
	EXPECT_EQ(firstFactory.closeUnitCalls(), 1);
	EXPECT_EQ(secondFactory.closeUnitCalls(), 1);
	EXPECT_EQ(firstFactory.operationWaitCalls(), 1);
	EXPECT_EQ(secondFactory.operationWaitCalls(), 1);
	EXPECT_EQ(firstFactory.snapshotCalls(), 1);
	EXPECT_EQ(secondFactory.snapshotCalls(), 1);
}

TEST(WtExecMgrQuiesceCoordinator, SharedFactoryAndUnitOwnersDeduplicateAndRetryAcrossManagers)
{
	EventLog log;
	FakeQuiesceFactory factory("shared", &log);
	WtExecuterMgr firstManager;
	WtExecuterMgr secondManager;
	std::shared_ptr<FakeStagedExecuter> first(new FakeStagedExecuter("first", &log));
	std::shared_ptr<FakeStagedExecuter> second(new FakeStagedExecuter("second", &log));
	std::shared_ptr<FakePluginQuiesceOwner> plugin(new FakePluginQuiesceOwner("shared-unit", &log));
	std::shared_ptr<FakePluginQuiesceOwner> retryPlugin(
		new FakePluginQuiesceOwner("retry-unit", &log, 2));
	first->addPluginOwner(plugin);
	second->addPluginOwner(plugin);
	second->addPluginOwner(retryPlugin);
	factory.addOwner(ExecQuiesceOwnerPtr(plugin));
	factory.addOwner(ExecQuiesceOwnerPtr(retryPlugin));
	first->setFactory(&factory);
	second->setFactory(&factory);
	firstManager.add_executer(ExecCmdPtr(first));
	secondManager.add_executer(ExecCmdPtr(second));
	WtExecQuiesceCoordinator coordinator({ &firstManager, &secondManager });

	EXPECT_FALSE(coordinator.quiesce_all());
	EXPECT_EQ(plugin->waitCalls(), 1);
	EXPECT_EQ(retryPlugin->waitCalls(), 1);
	EXPECT_EQ(first->ackCalls(), 0);
	EXPECT_EQ(second->ackCalls(), 0);

	ASSERT_TRUE(coordinator.quiesce_all());
	EXPECT_EQ(factory.closeConfigCalls(), 2);
	EXPECT_EQ(factory.preflightCalls(), 4);
	EXPECT_EQ(factory.closeUnitCalls(), 2);
	EXPECT_EQ(factory.operationWaitCalls(), 2);
	EXPECT_EQ(factory.snapshotCalls(), 2);
	EXPECT_EQ(plugin->requestCalls(), 2);
	EXPECT_EQ(plugin->waitCalls(), 2);
	EXPECT_EQ(retryPlugin->requestCalls(), 2);
	EXPECT_EQ(retryPlugin->waitCalls(), 2);
	EXPECT_EQ(first->ackCalls(), 1);
	EXPECT_EQ(second->ackCalls(), 1);
	EXPECT_TRUE(firstManager.wait_quiesced());
	EXPECT_TRUE(secondManager.wait_quiesced());
}

TEST(WtExecMgrQuiesceCoordinator, UnknownInitializedFactoryWrapperFailsClosedAndStaysOwned)
{
	EventLog log;
	FakeQuiesceFactory factory("direct", &log);
	WtExecuterMgr manager;
	std::shared_ptr<FakeStagedExecuter> host(new FakeStagedExecuter("host", &log));
	std::shared_ptr<FakePluginQuiesceOwner> direct(new FakePluginQuiesceOwner("direct-unit", &log));
	factory.addOwner(ExecQuiesceOwnerPtr(direct), true);
	host->setFactory(&factory);
	manager.add_executer(ExecCmdPtr(host));
	std::weak_ptr<FakePluginQuiesceOwner> directWeak(direct);
	WtExecQuiesceCoordinator coordinator({ &manager });

	EXPECT_FALSE(coordinator.quiesce_all());
	EXPECT_EQ(direct->requestCalls(), 0);
	EXPECT_EQ(direct->waitCalls(), 0);
	EXPECT_EQ(host->ackCalls(), 0);
	EXPECT_FALSE(manager.wait_quiesced());
	direct.reset();
	EXPECT_FALSE(directWeak.expired());

	EXPECT_FALSE(coordinator.quiesce_all());
	EXPECT_FALSE(directWeak.expired());
	EXPECT_EQ(directWeak.lock()->requestCalls(), 0);
	EXPECT_EQ(directWeak.lock()->waitCalls(), 0);
}

TEST(WtExecMgrQuiesceCoordinator, UninitializedDirectFactoryWrapperIsRetainedButNotWaited)
{
	EventLog log;
	FakeQuiesceFactory factory("uninitialized", &log);
	WtExecuterMgr manager;
	std::shared_ptr<FakeStagedExecuter> host(new FakeStagedExecuter("host", &log));
	std::shared_ptr<FakePluginQuiesceOwner> unit(new FakePluginQuiesceOwner("unit", &log));
	factory.addOwner(ExecQuiesceOwnerPtr(unit), false);
	host->setFactory(&factory);
	manager.add_executer(ExecCmdPtr(host));
	std::weak_ptr<FakePluginQuiesceOwner> unitWeak(unit);
	WtExecQuiesceCoordinator coordinator({ &manager });

	ASSERT_TRUE(coordinator.quiesce_all());
	unit.reset();
	EXPECT_FALSE(unitWeak.expired());
	EXPECT_EQ(unitWeak.lock()->requestCalls(), 0);
	EXPECT_EQ(unitWeak.lock()->waitCalls(), 0);
	EXPECT_TRUE(manager.wait_quiesced());
}

TEST(WtExecMgrQuiesceCoordinator, GlobalHostPreflightFailurePreventsEveryDrain)
{
	EventLog log;
	WtExecuterMgr firstManager;
	WtExecuterMgr secondManager;
	std::shared_ptr<FakeStagedExecuter> first(new FakeStagedExecuter("first", &log));
	std::shared_ptr<FakeStagedExecuter> second(new FakeStagedExecuter("second", &log));
	std::shared_ptr<FakePluginQuiesceOwner> plugin(new FakePluginQuiesceOwner("unit", &log));
	first->addPluginOwner(plugin);
	second->setPreflightResults({ false, true });
	firstManager.add_executer(ExecCmdPtr(first));
	secondManager.add_executer(ExecCmdPtr(second));
	WtExecQuiesceCoordinator coordinator({ &firstManager, &secondManager });

	EXPECT_FALSE(coordinator.quiesce_all());
	EXPECT_FALSE(firstManager.wait_quiesced());
	EXPECT_EQ(first->requestCalls(), 1);
	EXPECT_EQ(second->requestCalls(), 1);
	EXPECT_EQ(first->preflightCalls(), 1);
	EXPECT_EQ(second->preflightCalls(), 1);
	EXPECT_EQ(first->drainCalls(), 0);
	EXPECT_EQ(second->drainCalls(), 0);
	EXPECT_EQ(plugin->requestCalls(), 0);
	EXPECT_EQ(plugin->waitCalls(), 0);
	EXPECT_EQ(firstManager.find_executer("first").get(), first.get());

	ASSERT_TRUE(coordinator.quiesce_all());
	EXPECT_EQ(first->requestCalls(), 1);
	EXPECT_EQ(second->requestCalls(), 1);
	EXPECT_EQ(first->drainCalls(), 1);
	EXPECT_EQ(second->drainCalls(), 1);
	EXPECT_EQ(plugin->requestCalls(), 1);
}

TEST(WtExecMgrQuiesceCoordinator, StableFactoryPreflightFailureSendsNoUnitRequest)
{
	EventLog log;
	FakeQuiesceFactory factory("stable", &log);
	WtExecuterMgr manager;
	std::shared_ptr<FakeStagedExecuter> host(new FakeStagedExecuter("host", &log));
	std::shared_ptr<FakePluginQuiesceOwner> plugin(new FakePluginQuiesceOwner("unit", &log));
	host->addPluginOwner(plugin);
	factory.addOwner(ExecQuiesceOwnerPtr(plugin));
	factory.setPreflightResults({ true, false, true, true });
	host->setFactory(&factory);
	manager.add_executer(ExecCmdPtr(host));
	WtExecQuiesceCoordinator coordinator({ &manager });

	EXPECT_FALSE(coordinator.quiesce_all());
	EXPECT_EQ(host->drainCalls(), 1);
	EXPECT_EQ(host->snapshotCalls(), 1);
	EXPECT_EQ(factory.preflightCalls(), 2);
	EXPECT_EQ(plugin->requestCalls(), 0);
	EXPECT_EQ(plugin->waitCalls(), 0);
	EXPECT_FALSE(manager.wait_quiesced());
	EXPECT_EQ(manager.find_executer("host").get(), host.get());

	ASSERT_TRUE(coordinator.quiesce_all());
	EXPECT_EQ(host->snapshotCalls(), 1);
	EXPECT_EQ(factory.preflightCalls(), 4);
	EXPECT_EQ(plugin->requestCalls(), 1);
	EXPECT_TRUE(manager.wait_quiesced());
}

TEST(WtExecMgrQuiesceCoordinator, FactoryOperationWaitFailurePreservesHostsAndRetries)
{
	EventLog log;
	FakeQuiesceFactory factory("operations", &log);
	WtExecuterMgr manager;
	std::shared_ptr<FakeStagedExecuter> host(new FakeStagedExecuter("host", &log));
	std::shared_ptr<FakePluginQuiesceOwner> plugin(new FakePluginQuiesceOwner("unit", &log));
	host->addPluginOwner(plugin);
	factory.addOwner(ExecQuiesceOwnerPtr(plugin));
	factory.setOperationWaitResult(false);
	host->setFactory(&factory);
	manager.add_executer(ExecCmdPtr(host));
	WtExecQuiesceCoordinator coordinator({ &manager });

	EXPECT_FALSE(coordinator.quiesce_all());
	EXPECT_EQ(host->drainCalls(), 1);
	EXPECT_EQ(factory.closeUnitCalls(), 1);
	EXPECT_EQ(factory.operationWaitCalls(), 1);
	EXPECT_EQ(host->snapshotCalls(), 0);
	EXPECT_EQ(plugin->requestCalls(), 0);
	EXPECT_FALSE(manager.wait_quiesced());
	EXPECT_EQ(manager.find_executer("host").get(), host.get());

	factory.setOperationWaitResult(true);
	ASSERT_TRUE(coordinator.quiesce_all());
	EXPECT_EQ(host->snapshotCalls(), 1);
	EXPECT_EQ(factory.operationWaitCalls(), 2);
	EXPECT_EQ(plugin->requestCalls(), 1);
	EXPECT_EQ(plugin->waitCalls(), 1);
}

TEST(WtExecMgrQuiesceCoordinator, FactorySnapshotFailureRetainsHostOwnerAndRetries)
{
	EventLog log;
	FakeQuiesceFactory factory("snapshot", &log);
	WtExecuterMgr manager;
	std::shared_ptr<FakeStagedExecuter> host(new FakeStagedExecuter("host", &log));
	std::shared_ptr<FakePluginQuiesceOwner> plugin(new FakePluginQuiesceOwner("unit", &log));
	host->addPluginOwner(plugin);
	factory.addOwner(ExecQuiesceOwnerPtr(plugin));
	factory.setSnapshotResult(false);
	host->setFactory(&factory);
	manager.add_executer(ExecCmdPtr(host));
	std::weak_ptr<FakePluginQuiesceOwner> pluginWeak(plugin);
	WtExecQuiesceCoordinator coordinator({ &manager });

	EXPECT_FALSE(coordinator.quiesce_all());
	EXPECT_EQ(plugin->requestCalls(), 0);
	EXPECT_EQ(plugin->waitCalls(), 0);
	host->dropPluginOwnerReferences();
	plugin.reset();
	EXPECT_FALSE(pluginWeak.expired());
	EXPECT_FALSE(manager.wait_quiesced());

	factory.setSnapshotResult(true);
	ASSERT_TRUE(coordinator.quiesce_all());
	EXPECT_EQ(pluginWeak.lock()->requestCalls(), 1);
	EXPECT_EQ(pluginWeak.lock()->waitCalls(), 1);
	EXPECT_FALSE(pluginWeak.expired());
}

TEST(WtExecMgrQuiesceCoordinator, PartialSnapshotOwnersStayRetainedForRetry)
{
	EventLog log;
	WtExecuterMgr manager;
	std::shared_ptr<FakeStagedExecuter> host(new FakeStagedExecuter("host", &log));
	std::shared_ptr<FakePluginQuiesceOwner> plugin(new FakePluginQuiesceOwner("unit", &log));
	host->addPluginOwner(plugin);
	host->setSnapshotResult(false);
	manager.add_executer(ExecCmdPtr(host));
	std::weak_ptr<FakePluginQuiesceOwner> pluginWeak(plugin);
	WtExecQuiesceCoordinator coordinator({ &manager });

	EXPECT_FALSE(coordinator.quiesce_all());
	EXPECT_EQ(plugin->requestCalls(), 0);
	host->dropPluginOwnerReferences();
	plugin.reset();
	EXPECT_FALSE(pluginWeak.expired());

	host->setSnapshotResult(true);
	ASSERT_TRUE(coordinator.quiesce_all());
	EXPECT_EQ(pluginWeak.lock()->waitCalls(), 1);
	EXPECT_FALSE(pluginWeak.expired());
}

TEST(WtExecMgrQuiesceCoordinator, PluginWaitPreflightFailureStartsNoWait)
{
	EventLog log;
	WtExecuterMgr firstManager;
	WtExecuterMgr secondManager;
	std::shared_ptr<FakeStagedExecuter> first(new FakeStagedExecuter("first", &log));
	std::shared_ptr<FakeStagedExecuter> second(new FakeStagedExecuter("second", &log));
	std::shared_ptr<FakePluginQuiesceOwner> firstPlugin(
		new FakePluginQuiesceOwner("first-unit", &log, 1, true));
	std::shared_ptr<FakePluginQuiesceOwner> secondPlugin(
		new FakePluginQuiesceOwner("second-unit", &log));
	first->addPluginOwner(firstPlugin);
	second->addPluginOwner(secondPlugin);
	secondPlugin->setCanWait(false);
	firstManager.add_executer(ExecCmdPtr(first));
	secondManager.add_executer(ExecCmdPtr(second));
	WtExecQuiesceCoordinator coordinator({ &firstManager, &secondManager });

	std::future<bool> attempt = std::async(std::launch::async, [&coordinator]() {
		return coordinator.quiesce_all();
	});
	PluginWaitReleaseGuard releaseGate({ firstPlugin });
	const std::future_status result = attempt.wait_for(std::chrono::milliseconds(500));
	if (result != std::future_status::ready)
		firstPlugin->releaseWait();
	EXPECT_EQ(result, std::future_status::ready);
	EXPECT_FALSE(attempt.get());
	EXPECT_EQ(firstPlugin->waitCalls(), 0);
	EXPECT_EQ(secondPlugin->waitCalls(), 0);
	EXPECT_FALSE(firstManager.wait_quiesced());

	firstPlugin->releaseWait();
	secondPlugin->setCanWait(true);
	EXPECT_TRUE(coordinator.quiesce_all());
	EXPECT_EQ(firstPlugin->waitCalls(), 1);
	EXPECT_EQ(secondPlugin->waitCalls(), 1);
}

TEST(WtExecMgrQuiesceCoordinator, RetriesEveryPluginWaitAndRetainsOwnersAfterFailedWait)
{
	EventLog log;
	WtExecuterMgr firstManager;
	WtExecuterMgr secondManager;
	std::shared_ptr<FakeStagedExecuter> first(new FakeStagedExecuter("first", &log));
	std::shared_ptr<FakeStagedExecuter> second(new FakeStagedExecuter("second", &log));
	std::shared_ptr<FakePluginQuiesceOwner> firstPlugin(
		new FakePluginQuiesceOwner("first-unit", &log, 1));
	std::shared_ptr<FakePluginQuiesceOwner> secondPlugin(
		new FakePluginQuiesceOwner("second-unit", &log, 2));
	first->addPluginOwner(firstPlugin);
	second->addPluginOwner(secondPlugin);
	firstManager.add_executer(ExecCmdPtr(first));
	secondManager.add_executer(ExecCmdPtr(second));
	std::weak_ptr<FakePluginQuiesceOwner> firstPluginWeak(firstPlugin);
	std::weak_ptr<FakePluginQuiesceOwner> secondPluginWeak(secondPlugin);
	WtExecQuiesceCoordinator coordinator({ &firstManager, &secondManager });

	EXPECT_FALSE(coordinator.quiesce_all());
	EXPECT_EQ(firstPlugin->waitCalls(), 1);
	EXPECT_EQ(secondPlugin->waitCalls(), 1);
	EXPECT_EQ(first->ackCalls(), 0);
	EXPECT_EQ(second->ackCalls(), 0);
	EXPECT_FALSE(firstManager.wait_quiesced());
	EXPECT_FALSE(secondManager.wait_quiesced());
	EXPECT_EQ(firstManager.find_executer("first").get(), first.get());

	// Drop every fake-host/external unit reference. The manager snapshot must
	// retain both plugin owners despite a failed global wait phase.
	first->dropPluginOwnerReferences();
	second->dropPluginOwnerReferences();
	firstPlugin.reset();
	secondPlugin.reset();
	EXPECT_FALSE(firstPluginWeak.expired());
	EXPECT_FALSE(secondPluginWeak.expired());

	ASSERT_TRUE(coordinator.quiesce_all());
	EXPECT_EQ(firstPluginWeak.lock()->waitCalls(), 2);
	EXPECT_EQ(secondPluginWeak.lock()->waitCalls(), 2);
	EXPECT_EQ(first->ackCalls(), 1);
	EXPECT_EQ(second->ackCalls(), 1);
	EXPECT_TRUE(firstManager.wait_quiesced());
	EXPECT_TRUE(secondManager.wait_quiesced());
	EXPECT_FALSE(firstPluginWeak.expired());
	EXPECT_FALSE(secondPluginWeak.expired());
}

TEST(WtExecMgrQuiesceCoordinator, OldWaitCannotReportSuccessBeforeAllAcksSucceed)
{
	EventLog log;
	WtExecuterMgr firstManager;
	WtExecuterMgr secondManager;
	std::shared_ptr<FakeStagedExecuter> first(new FakeStagedExecuter("first", &log));
	std::shared_ptr<FakeStagedExecuter> second(new FakeStagedExecuter("second", &log));
	std::shared_ptr<FakePluginQuiesceOwner> firstPlugin(new FakePluginQuiesceOwner("p1", &log));
	std::shared_ptr<FakePluginQuiesceOwner> secondPlugin(new FakePluginQuiesceOwner("p2", &log));
	first->addPluginOwner(firstPlugin);
	second->addPluginOwner(secondPlugin);
	first->setAckResults({ true });
	second->setAckResults({ false, true });
	firstManager.add_executer(ExecCmdPtr(first));
	secondManager.add_executer(ExecCmdPtr(second));
	WtExecQuiesceCoordinator coordinator({ &firstManager, &secondManager });

	EXPECT_FALSE(coordinator.quiesce_all());
	EXPECT_EQ(first->ackCalls(), 1);
	EXPECT_EQ(second->ackCalls(), 1);
	EXPECT_FALSE(firstManager.wait_quiesced());
	EXPECT_FALSE(secondManager.wait_quiesced());
	EXPECT_EQ(firstManager.find_executer("first").get(), first.get());
	EXPECT_EQ(secondManager.find_executer("second").get(), second.get());

	ASSERT_TRUE(coordinator.quiesce_all());
	EXPECT_EQ(first->ackCalls(), 2);
	EXPECT_EQ(second->ackCalls(), 2);
	EXPECT_EQ(firstPlugin->waitCalls(), 2);
	EXPECT_EQ(secondPlugin->waitCalls(), 2);
	EXPECT_TRUE(firstManager.wait_quiesced());
	EXPECT_TRUE(secondManager.wait_quiesced());
	EXPECT_EQ(firstManager.find_executer("first").get(), first.get());
	EXPECT_EQ(secondManager.find_executer("second").get(), second.get());
}

TEST(WtExecMgrQuiesceCoordinator, OverlappingManagerClaimsBindAtomicallyAndCanRetry)
{
	EventLog log;
	WtExecuterMgr leftOnlyManager;
	WtExecuterMgr sharedManager;
	WtExecuterMgr rightOnlyManager;
	std::shared_ptr<FakeStagedExecuter> leftOnly(
		new FakeStagedExecuter("left-only", &log));
	std::shared_ptr<FakeStagedExecuter> shared(
		new FakeStagedExecuter("shared", &log));
	std::shared_ptr<FakeStagedExecuter> rightOnly(
		new FakeStagedExecuter("right-only", &log));
	leftOnlyManager.add_executer(ExecCmdPtr(leftOnly));
	sharedManager.add_executer(ExecCmdPtr(shared));
	rightOnlyManager.add_executer(ExecCmdPtr(rightOnly));
	// Put each coordinator's unique manager before the shared one. A coordinator
	// that binds sequentially can then claim its unique manager before losing the
	// race on sharedManager, leaving exactly the partial token this test guards.
	WtExecQuiesceCoordinator left({ &leftOnlyManager, &sharedManager });
	WtExecQuiesceCoordinator right({ &rightOnlyManager, &sharedManager });

	std::promise<void> startPromise;
	std::shared_future<void> start = startPromise.get_future().share();
	std::future<bool> leftAttempt = std::async(std::launch::async, [&left, start]() {
		start.wait();
		return left.quiesce_all();
	});
	std::future<bool> rightAttempt = std::async(std::launch::async, [&right, start]() {
		start.wait();
		return right.quiesce_all();
	});
	startPromise.set_value();

	const bool leftWon = leftAttempt.get();
	const bool rightWon = rightAttempt.get();
	ASSERT_NE(leftWon, rightWon);
	EXPECT_EQ(sharedManager.wait_quiesced(), true);
	EXPECT_EQ(leftOnlyManager.wait_quiesced(), leftWon);
	EXPECT_EQ(rightOnlyManager.wait_quiesced(), rightWon);
	EXPECT_EQ(shared->requestCalls(), 1);

	// Retrying the losing coordinator is safe and must not start its unique
	// manager. The failed batch claim leaves that manager available to a new
	// coordinator, proving that no partial token binding escaped.
	if (leftWon)
	{
		EXPECT_FALSE(right.quiesce_all());
		EXPECT_EQ(rightOnly->requestCalls(), 0);
		WtExecQuiesceCoordinator recovery({ &rightOnlyManager });
		ASSERT_TRUE(recovery.quiesce_all());
		EXPECT_EQ(rightOnly->requestCalls(), 1);
	}
	else
	{
		EXPECT_FALSE(left.quiesce_all());
		EXPECT_EQ(leftOnly->requestCalls(), 0);
		WtExecQuiesceCoordinator recovery({ &leftOnlyManager });
		ASSERT_TRUE(recovery.quiesce_all());
		EXPECT_EQ(leftOnly->requestCalls(), 1);
	}
}

TEST(WtExecMgrQuiesceCoordinator, ConcurrentRetryIsRejectedUntilCurrentRoundReturns)
{
	EventLog log;
	WtExecuterMgr manager;
	std::shared_ptr<FakeStagedExecuter> host(new FakeStagedExecuter("host", &log));
	std::shared_ptr<FakePluginQuiesceOwner> plugin(
		new FakePluginQuiesceOwner("unit", &log, 1, true));
	host->addPluginOwner(plugin);
	manager.add_executer(ExecCmdPtr(host));
	WtExecQuiesceCoordinator coordinator({ &manager });
	std::future<bool> firstRound = std::async(std::launch::async, [&coordinator]() {
		return coordinator.quiesce_all();
	});
	PluginWaitReleaseGuard releaseGate({ plugin });

	ASSERT_TRUE(plugin->waitForWait(std::chrono::milliseconds(500)));
	EXPECT_FALSE(coordinator.quiesce_all());
	plugin->releaseWait();
	EXPECT_TRUE(firstRound.get());
	EXPECT_TRUE(coordinator.is_complete());
}

TEST(WtExecMgrQuiesceCoordinator, MissingStageSupportFailsClosedAndRetainsOwners)
{
	EventLog log;
	WtExecuterMgr manager;
	std::shared_ptr<FakeStagedExecuter> supported(new FakeStagedExecuter("supported", &log));
	std::shared_ptr<LegacyQuiesceExecuter> legacy(new LegacyQuiesceExecuter("legacy"));
	manager.add_executer(ExecCmdPtr(supported));
	manager.add_executer(ExecCmdPtr(legacy));
	WtExecQuiesceCoordinator coordinator({ &manager });

	EXPECT_FALSE(coordinator.quiesce_all());
	EXPECT_EQ(supported->requestCalls(), 1);
	EXPECT_EQ(supported->drainCalls(), 0);
	EXPECT_EQ(legacy->request_calls.load(), 1);
	EXPECT_FALSE(manager.wait_quiesced());
	EXPECT_EQ(manager.find_executer("supported").get(), supported.get());
	EXPECT_EQ(manager.find_executer("legacy").get(), legacy.get());

	std::shared_ptr<FakeStagedExecuter> late(new FakeStagedExecuter("late", &log));
	manager.add_executer(ExecCmdPtr(late));
	EXPECT_EQ(manager.find_executer("late").get(), nullptr);
}

TEST(WtExecMgrQuiesceCoordinator, CallbackThreadPreflightReturnsBeforeAnyHostDrain)
{
	EventLog log;
	WtExecuterMgr manager;
	std::shared_ptr<FakeStagedExecuter> host(new FakeStagedExecuter("host", &log));
	manager.add_executer(ExecCmdPtr(host));
	WtExecQuiesceCoordinator coordinator({ &manager });

	std::promise<void> enteredPromise;
	std::future<void> entered = enteredPromise.get_future();
	std::future<bool> callback = std::async(std::launch::async, [&]() {
		host->setUnsafeThread(std::this_thread::get_id());
		enteredPromise.set_value();
		return coordinator.quiesce_all();
	});
	EXPECT_EQ(entered.wait_for(std::chrono::milliseconds(500)), std::future_status::ready);
	EXPECT_FALSE(callback.get());
	EXPECT_EQ(host->drainCalls(), 0);
	EXPECT_FALSE(manager.wait_quiesced());

	host->setUnsafeThread(std::thread::id());
	EXPECT_TRUE(coordinator.quiesce_all());
	EXPECT_TRUE(manager.wait_quiesced());
}

} // namespace
