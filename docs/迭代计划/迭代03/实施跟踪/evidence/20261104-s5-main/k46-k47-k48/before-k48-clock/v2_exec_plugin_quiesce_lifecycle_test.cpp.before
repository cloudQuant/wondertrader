#include <gtest/gtest.h>

#include "WtCore/WtExecuterFactory.h"
#include "WtCore/ExecCommandQuiesce.h"
#include "WtCore/WtExecMgr.h"
#include "WtCore/WtLocalExecuter.h"
#include "Includes/WTSVariant.hpp"
#include "WTSUtils/WTSCfgLoader.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifndef WT_COV_EXEC_V2_VALID_PATH
#error WT_COV_EXEC_V2_VALID_PATH must name the valid ABI 2.2 fixture
#endif
#ifndef WT_COV_EXEC_V2_REQUEST_FAIL_PATH
#error WT_COV_EXEC_V2_REQUEST_FAIL_PATH must name the retryable request fixture
#endif
#ifndef WT_COV_EXEC_V2_REQUEST_THROW_PATH
#error WT_COV_EXEC_V2_REQUEST_THROW_PATH must name the throwing request fixture
#endif
#ifndef WT_COV_EXEC_V2_CAN_WAIT_FALSE_PATH
#error WT_COV_EXEC_V2_CAN_WAIT_FALSE_PATH must name the false-preflight fixture
#endif
#ifndef WT_COV_EXEC_V2_CAN_WAIT_INVALID_PATH
#error WT_COV_EXEC_V2_CAN_WAIT_INVALID_PATH must name the invalid-preflight fixture
#endif
#ifndef WT_COV_EXEC_V2_CAN_WAIT_THROW_PATH
#error WT_COV_EXEC_V2_CAN_WAIT_THROW_PATH must name the throwing preflight fixture
#endif
#ifndef WT_COV_EXEC_V2_WAIT_FAIL_PATH
#error WT_COV_EXEC_V2_WAIT_FAIL_PATH must name the retryable wait fixture
#endif
#ifndef WT_COV_EXEC_V2_WAIT_THROW_PATH
#error WT_COV_EXEC_V2_WAIT_THROW_PATH must name the throwing wait fixture
#endif
#ifndef WT_COV_EXEC_V2_INIT_RACE_PATH
#error WT_COV_EXEC_V2_INIT_RACE_PATH must name the gated initialization fixture
#endif
#ifndef WT_COV_EXEC_V2_INIT_FAIL_PATH
#error WT_COV_EXEC_V2_INIT_FAIL_PATH must name the normal failed-initialization fixture
#endif
#ifndef WT_COV_EXEC_V2_FACTORY_PREFLIGHT_FALSE_PATH
#error WT_COV_EXEC_V2_FACTORY_PREFLIGHT_FALSE_PATH must name the transient false factory preflight fixture
#endif
#ifndef WT_COV_EXEC_V2_FACTORY_PREFLIGHT_INVALID_PATH
#error WT_COV_EXEC_V2_FACTORY_PREFLIGHT_INVALID_PATH must name the transient invalid factory preflight fixture
#endif
#ifndef WT_COV_EXEC_V2_FACTORY_PREFLIGHT_THROW_PATH
#error WT_COV_EXEC_V2_FACTORY_PREFLIGHT_THROW_PATH must name the transient throwing factory preflight fixture
#endif

namespace
{
namespace fs = std::filesystem;
using namespace wtp;

class ScopedCallbackLog
{
public:
	explicit ScopedCallbackLog(const std::string& path)
	{
		const char* previous = std::getenv("WT_COV_EXEC_V2_LOG");
		if (previous != nullptr)
			_previous = previous;
#if defined(_WIN32)
		_putenv_s("WT_COV_EXEC_V2_LOG", path.c_str());
#else
		setenv("WT_COV_EXEC_V2_LOG", path.c_str(), 1);
#endif
	}

	~ScopedCallbackLog()
	{
#if defined(_WIN32)
		_putenv_s("WT_COV_EXEC_V2_LOG", _previous.c_str());
#else
		if (_previous.empty())
			unsetenv("WT_COV_EXEC_V2_LOG");
		else
			setenv("WT_COV_EXEC_V2_LOG", _previous.c_str(), 1);
#endif
	}

private:
	std::string _previous;
};

class ScopedInitGatePath
{
public:
	explicit ScopedInitGatePath(const std::string& path,
		const char* environmentName = "WT_COV_EXEC_V2_INIT_GATE")
		: _name(environmentName == nullptr ? "WT_COV_EXEC_V2_INIT_GATE" : environmentName)
	{
		const char* previous = std::getenv(_name.c_str());
		if (previous != nullptr)
			_previous = previous;
#if defined(_WIN32)
		_putenv_s(_name.c_str(), path.c_str());
#else
		setenv(_name.c_str(), path.c_str(), 1);
#endif
	}

	~ScopedInitGatePath()
	{
#if defined(_WIN32)
		_putenv_s(_name.c_str(), _previous.c_str());
#else
		if (_previous.empty())
			unsetenv(_name.c_str());
		else
			setenv(_name.c_str(), _previous.c_str(), 1);
#endif
	}

private:
	std::string _name;
	std::string _previous;
};

class ScopedEnvironmentFlag
{
public:
	explicit ScopedEnvironmentFlag(const char* name)
		: _name(name == nullptr ? "" : name)
	{
		const char* previous = std::getenv(_name.c_str());
		if (previous != nullptr)
			_previous = previous;
#if defined(_WIN32)
		_putenv_s(_name.c_str(), "1");
#else
		setenv(_name.c_str(), "1", 1);
#endif
	}

	~ScopedEnvironmentFlag()
	{
#if defined(_WIN32)
		_putenv_s(_name.c_str(), _previous.c_str());
#else
		if (_previous.empty())
			unsetenv(_name.c_str());
		else
			setenv(_name.c_str(), _previous.c_str(), 1);
#endif
	}

private:
	std::string _name;
	std::string _previous;
};

class ScopedLoadGatePath
{
public:
	explicit ScopedLoadGatePath(const std::string& path)
	{
		const char* previous = std::getenv("WT_COV_EXEC_V2_LOAD_GATE");
		if (previous != nullptr)
			_previous = previous;
#if defined(_WIN32)
		_putenv_s("WT_COV_EXEC_V2_LOAD_GATE", path.c_str());
#else
		setenv("WT_COV_EXEC_V2_LOAD_GATE", path.c_str(), 1);
#endif
	}

	~ScopedLoadGatePath()
	{
#if defined(_WIN32)
		_putenv_s("WT_COV_EXEC_V2_LOAD_GATE", _previous.c_str());
#else
		if (_previous.empty())
			unsetenv("WT_COV_EXEC_V2_LOAD_GATE");
		else
			setenv("WT_COV_EXEC_V2_LOAD_GATE", _previous.c_str(), 1);
#endif
	}

private:
	std::string _previous;
};

class ScopedLoadThrowFlag
{
public:
	ScopedLoadThrowFlag()
	{
		const char* previous = std::getenv("WT_COV_EXEC_V2_LOAD_THROW");
		if (previous != nullptr)
			_previous = previous;
#if defined(_WIN32)
	_putenv_s("WT_COV_EXEC_V2_LOAD_THROW", "1");
#else
	setenv("WT_COV_EXEC_V2_LOAD_THROW", "1", 1);
#endif
	}

	~ScopedLoadThrowFlag()
	{
#if defined(_WIN32)
		_putenv_s("WT_COV_EXEC_V2_LOAD_THROW", _previous.c_str());
#else
		if (_previous.empty())
			unsetenv("WT_COV_EXEC_V2_LOAD_THROW");
		else
			setenv("WT_COV_EXEC_V2_LOAD_THROW", _previous.c_str(), 1);
#endif
	}

private:
	std::string _previous;
};

class ScopedInitReentryFlag
{
public:
	ScopedInitReentryFlag()
	{
		const char* previous = std::getenv("WT_COV_EXEC_V2_INIT_REENTER");
		if (previous != nullptr)
			_previous = previous;
#if defined(_WIN32)
	_putenv_s("WT_COV_EXEC_V2_INIT_REENTER", "1");
#else
	setenv("WT_COV_EXEC_V2_INIT_REENTER", "1", 1);
#endif
	}

	~ScopedInitReentryFlag()
	{
#if defined(_WIN32)
		_putenv_s("WT_COV_EXEC_V2_INIT_REENTER", _previous.c_str());
#else
		if (_previous.empty())
			unsetenv("WT_COV_EXEC_V2_INIT_REENTER");
		else
			setenv("WT_COV_EXEC_V2_INIT_REENTER", _previous.c_str(), 1);
#endif
	}

private:
	std::string _previous;
};

struct FactoryWaitReentryProbe
{
	IExecFactoryQuiesceLifecycle* lifecycle;
	std::atomic<bool> invoked;
	std::atomic<bool> waitResult;
	FactoryWaitReentryProbe() : lifecycle(nullptr), invoked(false), waitResult(false) {}
};

void reenterFactoryWait(void* user)
{
	FactoryWaitReentryProbe* probe = static_cast<FactoryWaitReentryProbe*>(user);
	probe->invoked.store(true, std::memory_order_release);
	probe->waitResult.store(probe->lifecycle->waitForFactoryOperationsQuiesced(),
		std::memory_order_release);
}

struct UnitRequestReentryProbe
{
	IExecCommandQuiesce* unit;
	std::atomic<bool> invoked;
	UnitRequestReentryProbe() : unit(nullptr), invoked(false) {}
};

void reenterUnitQuiesceRequest(void* user)
{
	UnitRequestReentryProbe* probe = static_cast<UnitRequestReentryProbe*>(user);
	probe->invoked.store(true, std::memory_order_release);
	probe->unit->requestQuiesce();
}

struct UnitOrderPreflightProbe
{
	IExecCommandQuiesce* unit;
	std::atomic<bool> invoked;
	std::atomic<bool> canWaitResult;
	UnitOrderPreflightProbe() : unit(nullptr), invoked(false), canWaitResult(true) {}
};

void preflightQuiesceFromHostOrder(void* user)
{
	UnitOrderPreflightProbe* probe = static_cast<UnitOrderPreflightProbe*>(user);
	probe->canWaitResult.store(probe->unit->canWaitQuiescedOnCurrentThread(),
		std::memory_order_release);
	probe->invoked.store(true, std::memory_order_release);
}

class TestExecuteContext final : public ExecuteContext
{
public:
	typedef void (*LogCallback)(void* user);
	typedef void (*BuyCallback)(void* user);

	explicit TestExecuteContext(LogCallback callback = nullptr, void* user = nullptr)
		: buyCalls(0), sellCalls(0), cancelIdCalls(0), cancelByCodeCalls(0),
		  timerCalls(0), blockBuy(false), buyEntered(false), releaseBuy(false),
		  buyFinished(false), buyInFlight(false), waitQuiesceObserved(false),
		  waitQuiesceObservedBuyInFlight(false), _callback(callback), _user(user),
		  _buy_callback(nullptr), _buy_user(nullptr)
	{
	}

	void setBuyCallback(BuyCallback callback, void* user)
	{
		_buy_callback = callback;
		_buy_user = user;
	}

	WTSTickSlice* getTicks(const char*, uint32_t, uint64_t = 0) override { return nullptr; }
	WTSTickData* grabLastTick(const char*) override { return nullptr; }
	double getPosition(const char*, bool = true, int32_t = 3) override { return 0.0; }
	OrderMap* getOrders(const char*) override { return nullptr; }
	double getUndoneQty(const char*) override { return 0.0; }
	OrderIDs buy(const char*, double, double, bool = false) override
	{
		buyInFlight.store(true, std::memory_order_release);
		buyCalls.fetch_add(1, std::memory_order_relaxed);
		if (blockBuy.load(std::memory_order_acquire))
		{
			buyEntered.store(true, std::memory_order_release);
			while (!releaseBuy.load(std::memory_order_acquire))
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (_buy_callback != nullptr)
			_buy_callback(_buy_user);
		buyInFlight.store(false, std::memory_order_release);
		buyFinished.store(true, std::memory_order_release);
		return OrderIDs(1, 11);
	}
	OrderIDs sell(const char*, double, double, bool = false) override
	{
		sellCalls.fetch_add(1, std::memory_order_relaxed);
		return OrderIDs(1, 12);
	}
	bool cancel(uint32_t) override
	{
		cancelIdCalls.fetch_add(1, std::memory_order_relaxed);
		return true;
	}
	OrderIDs cancel(const char*, bool, double = 0) override
	{
		cancelByCodeCalls.fetch_add(1, std::memory_order_relaxed);
		return OrderIDs(1, 13);
	}
	void writeLog(const char* message) override
	{
		const std::string text = message == nullptr ? "" : message;
		{
			std::lock_guard<std::mutex> lock(_log_mutex);
			_log_messages.push_back(text);
		}
		if (text == "fake wait quiesce entry")
		{
			waitQuiesceObservedBuyInFlight.store(
				buyInFlight.load(std::memory_order_acquire), std::memory_order_release);
			waitQuiesceObserved.store(true, std::memory_order_release);
			return;
		}
		if (_callback != nullptr)
			_callback(_user);
	}
	WTSCommodityInfo* getCommodityInfo(const char*) override { return nullptr; }
	WTSSessionInfo* getSessionInfo(const char*) override { return nullptr; }
	uint64_t getCurTime() override { return 0; }
	bool registerTimer(const char*, uint32_t) override
	{
		timerCalls.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	std::size_t countLogContaining(const std::string& text) const
	{
		std::lock_guard<std::mutex> lock(_log_mutex);
		return static_cast<std::size_t>(std::count_if(_log_messages.begin(), _log_messages.end(),
			[&text](const std::string& message) { return message.find(text) != std::string::npos; }));
	}

	std::size_t countLogExact(const std::string& text) const
	{
		std::lock_guard<std::mutex> lock(_log_mutex);
		return static_cast<std::size_t>(std::count(_log_messages.begin(), _log_messages.end(), text));
	}

	std::atomic<uint32_t> buyCalls;
	std::atomic<uint32_t> sellCalls;
	std::atomic<uint32_t> cancelIdCalls;
	std::atomic<uint32_t> cancelByCodeCalls;
	std::atomic<uint32_t> timerCalls;
	std::atomic<bool> blockBuy;
	std::atomic<bool> buyEntered;
	std::atomic<bool> releaseBuy;
	std::atomic<bool> buyFinished;
	std::atomic<bool> buyInFlight;
	std::atomic<bool> waitQuiesceObserved;
	std::atomic<bool> waitQuiesceObservedBuyInFlight;

private:
	LogCallback _callback;
	void* _user;
	BuyCallback _buy_callback;
	void* _buy_user;
	mutable std::mutex _log_mutex;
	std::vector<std::string> _log_messages;
};

void preserveUnquiescedUnit(ExecuteUnitPtr& unit,
	std::unique_ptr<TestExecuteContext>& context)
{
	new ExecuteUnitPtr(unit);
	unit.reset();
	context.release();
}

std::string readLog(const fs::path& path)
{
	std::ifstream input(path.string().c_str(), std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::vector<std::string> logEvents(const std::string& log)
{
	std::istringstream input(log);
	std::vector<std::string> events;
	std::string event;
	while (std::getline(input, event))
		events.push_back(event);
	return events;
}

bool containsEvent(const std::string& log, const std::string& expected)
{
	const std::vector<std::string> events = logEvents(log);
	return std::find(events.begin(), events.end(), expected) != events.end();
}

std::size_t countEvent(const std::string& log, const std::string& expected)
{
	const std::vector<std::string> events = logEvents(log);
	return static_cast<std::size_t>(std::count(events.begin(), events.end(), expected));
}

class V2ExecPluginQuiesceLifecycleTest : public ::testing::Test
{
protected:
	void TearDown() override
	{
		bool quiesced = false;
		bool factoryPreflightPassed = !_initializedSuccessfully || _factoryPreflightPassed;
		if (_factoryLifecycle != nullptr)
			_factoryLifecycle->closeFactoryConfigAdmissions();
		if (_initializedSuccessfully && !factoryPreflightPassed && _factoryPreflight != nullptr)
			factoryPreflightPassed = preflightFactories();
		if (_unit != nullptr && _quiesce != nullptr && factoryPreflightPassed)
		{
			for (int attempt = 0; attempt != 8 && !quiesced; ++attempt)
			{
				_quiesce->requestQuiesce();
				quiesced = _quiesce->waitQuiesced();
			}
		}

		const bool retainedForSafety = _unit != nullptr && _initializedSuccessfully && !quiesced;
		if (retainedForSafety)
		{
			// These fixtures do not retain a host context or start workers. If a
			// regression prevents cleanup, keep the test object alive until process
			// exit so the test reports a failure instead of invoking its destructor.
			new ExecuteUnitPtr(std::move(_unit));
		}
		_unit.reset();
		_factory.reset();
		_factoryPreflight = nullptr;
		_factoryLifecycle = nullptr;
		_quiesce = nullptr;
		_callbackLog.reset();
		if (!retainedForSafety)
		{
			std::error_code ec;
			fs::remove_all(_root, ec);
		}
	}

	bool prepareFactoryObject(const char* pluginPath)
	{
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		_root = fs::temp_directory_path() /
			("wt_exec_v2_quiesce_" + std::to_string(stamp) +
				"_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
		std::error_code ec;
		fs::create_directories(_root, ec);
		if (ec)
			return false;

		const fs::path source(pluginPath);
		fs::copy_file(source, _root / source.filename(), fs::copy_options::overwrite_existing, ec);
		if (ec)
			return false;

		_logPath = _root / "callbacks.log";
		_callbackLog.reset(new ScopedCallbackLog(_logPath.string()));
		_factory.reset(new WtExecuterFactory());
		_factoryPreflight = dynamic_cast<IExecFactoryQuiescePreflight*>(_factory.get());
		_factoryLifecycle = dynamic_cast<IExecFactoryQuiesceLifecycle*>(_factory.get());
		return _factoryPreflight != nullptr && _factoryLifecycle != nullptr;
	}

	bool loadFactory(const char* pluginPath)
	{
		return prepareFactoryObject(pluginPath) &&
			_factory->loadFactories(_root.string().c_str());
	}

	bool createUnit(const char* pluginPath)
	{
		if (!loadFactory(pluginPath))
			return false;
		_unit = _factory->createExeUnit("FakeV2Factory.Unit");
		if (_unit == nullptr)
			return false;

		_quiesce = dynamic_cast<IExecCommandQuiesce*>(_unit.get());
		return _quiesce != nullptr;
	}

	bool initializeCreatedUnit()
	{
		if (_unit == nullptr)
			return false;
		std::string error;
		if (!_unit->initialize(nullptr, "TEST", nullptr, error))
			return false;
		_initializedSuccessfully = true;
		return true;
	}

	bool prepare(const char* pluginPath)
	{
		if (!createUnit(pluginPath) || !initializeCreatedUnit())
			return false;
		_factoryLifecycle->closeFactoryConfigAdmissions();
		return true;
	}

	std::string log() const { return readLog(_logPath); }
	fs::path initGatePath() const { return _root / "init.gate"; }
	fs::path afterInitGatePath() const { return _root / "after_init.gate"; }
	fs::path loadGatePath() const { return _root / "load.gate"; }
	bool preflightFactories()
	{
		if (_factoryPreflight == nullptr)
			return false;
		const bool passed = _factoryPreflight->preflightQuiesceFactoriesOnCurrentThread();
		if (passed)
			_factoryPreflightPassed = true;
		return passed;
	}
	void releaseFactoryOwner()
	{
		_factory.reset();
		_factoryPreflight = nullptr;
		_factoryLifecycle = nullptr;
	}

	bool initFakeLocal(WtExecuterMgr& manager,
		std::shared_ptr<WtLocalExecuter>& local)
	{
		local.reset(new WtLocalExecuter(_factory.get(), "failed_init_local", nullptr));
		manager.add_executer(std::static_pointer_cast<IExecCommand>(local));
		const std::string config =
			"{\"scale\":1.0,\"poolsize\":0,\"policy\":{\"default\":{"
			"\"name\":\"FakeV2Factory.Unit\"}}}";
		WTSVariant* params = WTSCfgLoader::load_from_content(config);
		if (params == nullptr)
		{
			WtExecQuiesceCoordinator cleanup({ &manager });
			const bool safe = cleanup.quiesce_all() && cleanup.is_complete() &&
				manager.wait_quiesced();
			if (!safe)
				preserveUnquiescedLocal(local);
			return false;
		}
		const bool initialized = local->init(params);
		params->release();
		if (!initialized)
		{
			WtExecQuiesceCoordinator cleanup({ &manager });
			const bool safe = cleanup.quiesce_all() && cleanup.is_complete() &&
				manager.wait_quiesced();
			if (!safe)
				preserveUnquiescedLocal(local);
		}
		return initialized;
	}

	void preserveUnquiescedLocal(const std::shared_ptr<WtLocalExecuter>& local)
	{
		// A failed coordinator leaves this executor intentionally fail-stop. Keep
		// its borrowed factory and context alive so the assertion remains visible.
		new std::shared_ptr<WtLocalExecuter>(local);
		_factory.release();
		_factoryPreflight = nullptr;
		_factoryLifecycle = nullptr;
	}
	void releaseQuiescedUnit()
	{
		if (_unit != nullptr && _initializedSuccessfully && _quiesce != nullptr)
		{
			bool quiesced = false;
			for (int attempt = 0; attempt != 8 && !quiesced; ++attempt)
			{
				quiesced = _quiesce->waitQuiesced();
				if (!quiesced)
					_quiesce->requestQuiesce();
			}
			if (!quiesced)
				return;
		}
		_unit.reset();
		_factory.reset();
		_factoryPreflight = nullptr;
		_factoryLifecycle = nullptr;
		_quiesce = nullptr;
		_initializedSuccessfully = false;
	}

	fs::path _root;
	fs::path _logPath;
	std::unique_ptr<ScopedCallbackLog> _callbackLog;
	std::unique_ptr<WtExecuterFactory> _factory;
	ExecuteUnitPtr _unit;
	IExecCommandQuiesce* _quiesce = nullptr;
	IExecFactoryQuiescePreflight* _factoryPreflight = nullptr;
	IExecFactoryQuiesceLifecycle* _factoryLifecycle = nullptr;
	bool _factoryPreflightPassed = false;
	bool _initializedSuccessfully = false;
};

TEST_F(V2ExecPluginQuiesceLifecycleTest, SuccessfulLifecycleCallsHooksInOrderBeforeDestroy)
{
	ASSERT_TRUE(prepare(WT_COV_EXEC_V2_VALID_PATH));
	ASSERT_TRUE(preflightFactories());
	EXPECT_EQ("get_api\ncreate_factory\ncreate_unit\ninitialize_unit\n"
		"can_wait_factory_quiesce\n", log());
	EXPECT_FALSE(containsEvent(log(), "request_quiesce"));
	releaseFactoryOwner();
	_quiesce->requestQuiesce();
	EXPECT_TRUE(_quiesce->canWaitQuiescedOnCurrentThread());
	EXPECT_TRUE(_quiesce->waitQuiesced());
	ASSERT_NE(nullptr, _unit->self());
	EXPECT_EQ("get_api\ncreate_factory\ncreate_unit\ninitialize_unit\n"
		"can_wait_factory_quiesce\nrequest_quiesce\ncan_wait_quiesce\n"
		"can_wait_quiesce\nwait_quiesce\n",
		log());

	releaseQuiescedUnit();
	EXPECT_EQ("get_api\ncreate_factory\ncreate_unit\ninitialize_unit\n"
		"can_wait_factory_quiesce\nrequest_quiesce\ncan_wait_quiesce\n"
		"can_wait_quiesce\nwait_quiesce\n"
		"destroy_unit\ndestroy_factory\n", log());
}

TEST_F(V2ExecPluginQuiesceLifecycleTest, NormalFailedInitializationProvesSynchronousCleanup)
{
	ASSERT_TRUE(createUnit(WT_COV_EXEC_V2_INIT_FAIL_PATH));
	std::string error;
	EXPECT_FALSE(_unit->initialize(nullptr, "TEST", nullptr, error));
	EXPECT_EQ("controlled initialization failure", error);
	EXPECT_TRUE(_unit->failedInitializationWasSafelyDestroyed());
	EXPECT_FALSE(_unit->isInitialized());
	EXPECT_EQ(nullptr, _unit->self());
	EXPECT_EQ("get_api\ncreate_factory\ncreate_unit\ninitialize_unit\ndestroy_unit\n", log());
}

TEST_F(V2ExecPluginQuiesceLifecycleTest, NullContextRemainsNullAcrossV2Initialization)
{
	ASSERT_TRUE(createUnit(WT_COV_EXEC_V2_VALID_PATH));
	_factoryLifecycle->closeFactoryConfigAdmissions();
	ASSERT_TRUE(preflightFactories());

	std::string error;
	ASSERT_TRUE(_unit->initialize(nullptr, "NULL_CONTEXT_PROBE", nullptr, error)) << error;
	EXPECT_EQ(nullptr, _unit->initializedContextIdentity());
	EXPECT_TRUE(containsEvent(log(), "null_context_preserved"));
	EXPECT_FALSE(containsEvent(log(), "null_context_unexpectedly_wrapped"));

	_initializedSuccessfully = true;
	_quiesce->requestQuiesce();
	EXPECT_TRUE(_quiesce->canWaitQuiescedOnCurrentThread());
	EXPECT_TRUE(_quiesce->waitQuiesced());
	releaseQuiescedUnit();
}

TEST_F(V2ExecPluginQuiesceLifecycleTest, PluginRequestMayReenterHostContextWithoutLifecycleLock)
{
	ASSERT_TRUE(loadFactory(WT_COV_EXEC_V2_VALID_PATH));
	ExecuteUnitPtr unit = _factory->createExeUnit("FakeV2Factory.ContextGateUnit");
	ASSERT_NE(nullptr, unit);
	IExecCommandQuiesce* quiesce = dynamic_cast<IExecCommandQuiesce*>(unit.get());
	ASSERT_NE(nullptr, quiesce);
	UnitRequestReentryProbe probe;
	probe.unit = quiesce;
	std::unique_ptr<TestExecuteContext> context(
		new TestExecuteContext(reenterUnitQuiesceRequest, &probe));
	std::string error;
	ASSERT_TRUE(unit->initialize(context.get(), "TEST", nullptr, error)) << error;

	ScopedEnvironmentFlag requestReentry("WT_COV_EXEC_V2_REQUEST_REENTER_HOST");
	quiesce->requestQuiesce();
	EXPECT_TRUE(probe.invoked.load(std::memory_order_acquire));
	EXPECT_EQ(1U, countEvent(log(), "request_quiesce"));
	EXPECT_TRUE(quiesce->canWaitQuiescedOnCurrentThread());
	const bool quiesced = quiesce->waitQuiesced();
	EXPECT_TRUE(quiesced);
	if (quiesced)
	{
		unit.reset();
		context.reset();
	}
	else
		preserveUnquiescedUnit(unit, context);
}

TEST_F(V2ExecPluginQuiesceLifecycleTest,
	InitOrderGateIsUnitLocalAndActivatesOnlyAfterSuccessfulInitialization)
{
	ASSERT_TRUE(loadFactory(WT_COV_EXEC_V2_VALID_PATH));
	ScopedInitGatePath initGate(initGatePath().string());
	ScopedInitGatePath afterInitGate(afterInitGatePath().string(),
		"WT_COV_EXEC_V2_AFTER_INIT_GATE");

	ExecuteUnitPtr activeUnit = _factory->createExeUnit("FakeV2Factory.ContextGateUnit");
	ExecuteUnitPtr initializingUnit = _factory->createExeUnit("FakeV2Factory.ContextGateUnit");
	ASSERT_NE(nullptr, activeUnit);
	ASSERT_NE(nullptr, initializingUnit);
	IExecCommandQuiesce* activeQuiesce =
		dynamic_cast<IExecCommandQuiesce*>(activeUnit.get());
	ASSERT_NE(nullptr, activeQuiesce);
	std::unique_ptr<TestExecuteContext> activeContext(new TestExecuteContext());
	std::unique_ptr<TestExecuteContext> initializingContext(new TestExecuteContext());

	std::string activeError;
	ASSERT_TRUE(activeUnit->initialize(activeContext.get(), "AFTER_INIT_ORDER", nullptr, activeError))
		<< activeError;

	std::atomic<bool> failedUnitInitialized(true);
	std::string failedError;
	std::thread failedInitializer([&]() {
		failedUnitInitialized.store(initializingUnit->initialize(
			initializingContext.get(), "BLOCKED_INIT", nullptr, failedError),
			std::memory_order_release);
	});

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!containsEvent(log(), "init_trade_attempts_finished") &&
		std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const bool blockedAttemptsFinished = containsEvent(log(), "init_trade_attempts_finished");

	{
		std::ofstream gate(afterInitGatePath().string().c_str(), std::ios::binary);
		gate << "open";
	}
	const auto orderDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (activeContext->buyCalls.load(std::memory_order_acquire) == 0 &&
		std::chrono::steady_clock::now() < orderDeadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const auto workerDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!containsEvent(log(), "post_init_buy_finished") &&
		std::chrono::steady_clock::now() < workerDeadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	EXPECT_EQ(2U, activeContext->buyCalls.load(std::memory_order_acquire));
	EXPECT_EQ(1U, activeContext->sellCalls.load(std::memory_order_acquire));
	EXPECT_EQ(1U, activeContext->cancelIdCalls.load(std::memory_order_acquire));
	EXPECT_EQ(1U, activeContext->cancelByCodeCalls.load(std::memory_order_acquire));
	EXPECT_TRUE(containsEvent(log(), "post_init_buy_finished"));
	EXPECT_TRUE(containsEvent(log(), "post_init_trade_operations_forwarded"));
	EXPECT_EQ(0U, initializingContext->buyCalls.load(std::memory_order_acquire));
	EXPECT_EQ(0U, initializingContext->sellCalls.load(std::memory_order_acquire));
	EXPECT_EQ(0U, initializingContext->cancelIdCalls.load(std::memory_order_acquire));
	EXPECT_EQ(0U, initializingContext->cancelByCodeCalls.load(std::memory_order_acquire));
	EXPECT_EQ(0U, initializingContext->timerCalls.load(std::memory_order_acquire));
	EXPECT_EQ(2U, initializingContext->countLogExact(
		"V2 execution-unit context rejected buy"));
	EXPECT_EQ(2U, initializingContext->countLogExact(
		"V2 execution-unit context rejected sell"));
	EXPECT_EQ(2U, initializingContext->countLogExact(
		"V2 execution-unit context rejected cancel"));
	EXPECT_EQ(2U, initializingContext->countLogExact(
		"V2 execution-unit context rejected cancel-by-contract"));
	EXPECT_EQ(2U, initializingContext->countLogExact(
		"V2 execution-unit context rejected registerTimer (no V2 unregister contract)"));
	EXPECT_EQ(0U, activeContext->timerCalls.load(std::memory_order_acquire));
	EXPECT_EQ(2U, activeContext->countLogExact(
		"V2 execution-unit context rejected registerTimer (no V2 unregister contract)"));

	{
		std::ofstream gate(initGatePath().string().c_str(), std::ios::binary);
		gate << "open";
	}
	failedInitializer.join();
	EXPECT_TRUE(blockedAttemptsFinished);
	EXPECT_FALSE(failedUnitInitialized.load(std::memory_order_acquire));
	EXPECT_EQ("controlled failure after blocked trade attempts", failedError);
	EXPECT_TRUE(initializingUnit->failedInitializationWasSafelyDestroyed());

	activeQuiesce->requestQuiesce();
	EXPECT_TRUE(activeQuiesce->canWaitQuiescedOnCurrentThread());
	const bool activeQuiesced = activeQuiesce->waitQuiesced();
	EXPECT_TRUE(activeQuiesced);
	if (activeQuiesced)
	{
		activeUnit.reset();
		activeContext.reset();
	}
	else
		preserveUnquiescedUnit(activeUnit, activeContext);
	initializingUnit.reset();
	initializingContext.reset();
}

TEST_F(V2ExecPluginQuiesceLifecycleTest, QuiesceDuringInitializeCannotBeOvertakenByActivation)
{
	ASSERT_TRUE(loadFactory(WT_COV_EXEC_V2_VALID_PATH));
	ScopedInitGatePath initGate(initGatePath().string());
	ScopedInitGatePath afterInitGate(afterInitGatePath().string(),
		"WT_COV_EXEC_V2_AFTER_INIT_GATE");
	ExecuteUnitPtr unit = _factory->createExeUnit("FakeV2Factory.ContextGateUnit");
	ASSERT_NE(nullptr, unit);
	IExecCommandQuiesce* quiesce = dynamic_cast<IExecCommandQuiesce*>(unit.get());
	ASSERT_NE(nullptr, quiesce);
	std::unique_ptr<TestExecuteContext> context(new TestExecuteContext());
	std::atomic<bool> initialized(false);
	std::string error;
	std::thread initializer([&]() {
		initialized.store(unit->initialize(context.get(), "BLOCKED_SUCCESS", nullptr, error),
			std::memory_order_release);
	});

	const auto initDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!containsEvent(log(), "context_gate_init_waiting") &&
		std::chrono::steady_clock::now() < initDeadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const bool initializationWasBlocked = containsEvent(log(), "context_gate_init_waiting");
	quiesce->requestQuiesce();
	{
		std::ofstream gate(initGatePath().string().c_str(), std::ios::binary);
		gate << "open";
	}
	initializer.join();

	EXPECT_TRUE(initializationWasBlocked);
	EXPECT_TRUE(initialized.load(std::memory_order_acquire)) << error;
	EXPECT_EQ(1U, countEvent(log(), "request_quiesce"));
	{
		std::ofstream gate(afterInitGatePath().string().c_str(), std::ios::binary);
		gate << "open";
	}
	const auto workerDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!containsEvent(log(), "post_init_buy_finished") &&
		std::chrono::steady_clock::now() < workerDeadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	EXPECT_TRUE(containsEvent(log(), "post_init_buy_finished"));
	EXPECT_EQ(0U, context->buyCalls.load(std::memory_order_acquire));
	EXPECT_EQ(0U, context->sellCalls.load(std::memory_order_acquire));
	EXPECT_EQ(0U, context->cancelIdCalls.load(std::memory_order_acquire));
	EXPECT_EQ(0U, context->cancelByCodeCalls.load(std::memory_order_acquire));
	EXPECT_EQ(3U, context->countLogExact("V2 execution-unit context rejected buy"));
	EXPECT_EQ(1U, context->countLogExact("V2 execution-unit context rejected sell"));
	EXPECT_EQ(1U, context->countLogExact("V2 execution-unit context rejected cancel"));
	EXPECT_EQ(1U, context->countLogExact(
		"V2 execution-unit context rejected cancel-by-contract"));
	EXPECT_EQ(0U, context->timerCalls.load(std::memory_order_acquire));
	EXPECT_EQ(2U, context->countLogExact(
		"V2 execution-unit context rejected registerTimer (no V2 unregister contract)"));
	EXPECT_TRUE(containsEvent(log(), "post_init_trade_operations_rejected"));

	const bool quiesced = quiesce->waitQuiesced();
	EXPECT_TRUE(quiesced);
	if (quiesced)
	{
		unit.reset();
		context.reset();
	}
	else
		preserveUnquiescedUnit(unit, context);
}

TEST_F(V2ExecPluginQuiesceLifecycleTest, QuiesceDrainsAnAdmittedHostOrderBeforePluginWait)
{
	ASSERT_TRUE(loadFactory(WT_COV_EXEC_V2_VALID_PATH));
	ScopedInitGatePath afterInitGate(afterInitGatePath().string(),
		"WT_COV_EXEC_V2_AFTER_INIT_GATE");
	ExecuteUnitPtr unit = _factory->createExeUnit("FakeV2Factory.ContextGateUnit");
	ASSERT_NE(nullptr, unit);
	IExecCommandQuiesce* quiesce = dynamic_cast<IExecCommandQuiesce*>(unit.get());
	ASSERT_NE(nullptr, quiesce);
	std::unique_ptr<TestExecuteContext> context(new TestExecuteContext());
	context->blockBuy.store(true, std::memory_order_release);

	std::string error;
	ASSERT_TRUE(unit->initialize(context.get(), "AFTER_INIT_ORDER", nullptr, error)) << error;
	{
		std::ofstream gate(afterInitGatePath().string().c_str(), std::ios::binary);
		gate << "open";
	}
	const auto enteredDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!context->buyEntered.load(std::memory_order_acquire) &&
		std::chrono::steady_clock::now() < enteredDeadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const bool orderEnteredHostBeforeQuiesce = context->buyEntered.load(std::memory_order_acquire);

	quiesce->requestQuiesce();
	std::atomic<bool> waitResult(false);
	std::thread waiter([&]() {
		waitResult.store(quiesce->waitQuiesced(), std::memory_order_release);
	});
	const auto preflightDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (countEvent(log(), "can_wait_quiesce") == 0 &&
		std::chrono::steady_clock::now() < preflightDeadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const bool canWaitObserved = countEvent(log(), "can_wait_quiesce") != 0;
	const bool pluginWaitWasDeferred = canWaitObserved && !containsEvent(log(), "wait_quiesce");
	context->releaseBuy.store(true, std::memory_order_release);
	waiter.join();

	EXPECT_TRUE(orderEnteredHostBeforeQuiesce);
	EXPECT_TRUE(canWaitObserved)
		<< "waiter must reach the observable plugin preflight before drain is checked";
	EXPECT_TRUE(pluginWaitWasDeferred)
		<< "plugin wait must not begin while a host order call is still in flight";
	EXPECT_TRUE(context->buyFinished.load(std::memory_order_acquire));
	EXPECT_TRUE(waitResult.load(std::memory_order_acquire));
	EXPECT_TRUE(context->waitQuiesceObserved.load(std::memory_order_acquire));
	EXPECT_FALSE(context->waitQuiesceObservedBuyInFlight.load(std::memory_order_acquire))
		<< "plugin wait_quiesce must not overlap the host buy call";
	EXPECT_EQ(1U, context->buyCalls.load(std::memory_order_acquire));
	EXPECT_EQ(0U, context->sellCalls.load(std::memory_order_acquire));
	EXPECT_EQ(0U, context->cancelIdCalls.load(std::memory_order_acquire));
	EXPECT_EQ(0U, context->cancelByCodeCalls.load(std::memory_order_acquire));
	EXPECT_EQ(1U, context->countLogExact("V2 execution-unit context rejected buy"));
	EXPECT_EQ(1U, context->countLogExact("V2 execution-unit context rejected sell"));
	EXPECT_EQ(1U, context->countLogExact("V2 execution-unit context rejected cancel"));
	EXPECT_EQ(1U, context->countLogExact(
		"V2 execution-unit context rejected cancel-by-contract"));
	EXPECT_TRUE(containsEvent(log(), "post_init_trade_operations_rejected"));
	EXPECT_EQ(0U, context->timerCalls.load(std::memory_order_acquire));
	EXPECT_EQ(2U, context->countLogExact(
		"V2 execution-unit context rejected registerTimer (no V2 unregister contract)"));
	if (waitResult.load(std::memory_order_acquire))
	{
		unit.reset();
		context.reset();
	}
	else
		preserveUnquiescedUnit(unit, context);
}

TEST_F(V2ExecPluginQuiesceLifecycleTest, ReentrantQuiescePreflightFromHostOrderDoesNotWaitOnItself)
{
	ASSERT_TRUE(loadFactory(WT_COV_EXEC_V2_VALID_PATH));
	ScopedInitGatePath afterInitGate(afterInitGatePath().string(),
		"WT_COV_EXEC_V2_AFTER_INIT_GATE");
	ExecuteUnitPtr unit = _factory->createExeUnit("FakeV2Factory.ContextGateUnit");
	ASSERT_NE(nullptr, unit);
	IExecCommandQuiesce* quiesce = dynamic_cast<IExecCommandQuiesce*>(unit.get());
	ASSERT_NE(nullptr, quiesce);
	std::unique_ptr<TestExecuteContext> context(new TestExecuteContext());
	context->blockBuy.store(true, std::memory_order_release);
	UnitOrderPreflightProbe probe;
	probe.unit = quiesce;
	context->setBuyCallback(preflightQuiesceFromHostOrder, &probe);

	std::string error;
	ASSERT_TRUE(unit->initialize(context.get(), "AFTER_INIT_ORDER", nullptr, error)) << error;
	{
		std::ofstream gate(afterInitGatePath().string().c_str(), std::ios::binary);
		gate << "open";
	}
	const auto enteredDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!context->buyEntered.load(std::memory_order_acquire) &&
		std::chrono::steady_clock::now() < enteredDeadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const bool orderEnteredHost = context->buyEntered.load(std::memory_order_acquire);
	quiesce->requestQuiesce();
	context->releaseBuy.store(true, std::memory_order_release);
	const auto callbackDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!probe.invoked.load(std::memory_order_acquire) &&
		std::chrono::steady_clock::now() < callbackDeadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const bool callbackReturned = probe.invoked.load(std::memory_order_acquire);
	const bool callbackRefusedSelfWait = !probe.canWaitResult.load(std::memory_order_acquire);
	const auto workerDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!containsEvent(log(), "post_init_buy_finished") &&
		std::chrono::steady_clock::now() < workerDeadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const bool workerFinished = containsEvent(log(), "post_init_buy_finished");
	const bool quiesced = quiesce->waitQuiesced();

	EXPECT_TRUE(orderEnteredHost);
	EXPECT_TRUE(callbackReturned);
	EXPECT_TRUE(callbackRefusedSelfWait);
	EXPECT_TRUE(workerFinished);
	EXPECT_TRUE(quiesced);
	if (quiesced)
	{
		unit.reset();
		context.reset();
	}
	else
		preserveUnquiescedUnit(unit, context);
}

TEST_F(V2ExecPluginQuiesceLifecycleTest, LocalKeepsFailedStatusAndQuiescesWithRetainedInitialTarget)
{
	ASSERT_TRUE(loadFactory(WT_COV_EXEC_V2_INIT_FAIL_PATH));
	WtExecuterMgr manager;
	std::shared_ptr<WtLocalExecuter> local;
	ASSERT_TRUE(initFakeLocal(manager, local));
	local->on_position_changed("SHFE.rb.2510", 1.0);

	const ExecUnitInitStatus status = manager.query_exec_unit_init_status(
		"failed_init_local", "SHFE.rb.2510");
	EXPECT_EQ(status.query_result, ExecUnitInitQueryResult::OK);
	EXPECT_EQ(status.state, ExecUnitInitState::FAILED);
	EXPECT_EQ(status.error, "controlled initialization failure");

	WtExecQuiesceCoordinator coordinator({ &manager });
	const bool safe = coordinator.quiesce_all() && coordinator.is_complete() &&
		manager.wait_quiesced();
	EXPECT_TRUE(safe)
		<< "the initial target is retained as business state, not discarded callback work";
	if (!safe)
		preserveUnquiescedLocal(local);

	if (safe)
	{
		std::vector<ExecPluginUnitSnapshot> liveUnits;
		ASSERT_TRUE(_factoryLifecycle->snapshotLivePluginUnitOwners(liveUnits));
		EXPECT_TRUE(liveUnits.empty()) << "a failed unit must not remain registered or owned";
	}
}

TEST_F(V2ExecPluginQuiesceLifecycleTest, LocalFailsClosedWhenInitDiscardsAnAcceptedChannelCallback)
{
	ASSERT_TRUE(loadFactory(WT_COV_EXEC_V2_INIT_FAIL_PATH));
	WtExecuterMgr manager;
	std::shared_ptr<WtLocalExecuter> local;
	ASSERT_TRUE(initFakeLocal(manager, local));
	local->on_channel_ready();
	local->on_position_changed("SHFE.rb.2510", 1.0);
	EXPECT_EQ(ExecUnitInitState::FAILED,
		manager.query_exec_unit_init_status("failed_init_local", "SHFE.rb.2510").state);

	WtExecQuiesceCoordinator coordinator({ &manager });
	const bool safe = coordinator.quiesce_all() && coordinator.is_complete() &&
		manager.wait_quiesced();
	EXPECT_FALSE(safe) << "a channel callback queued for the failed generation was discarded";
	if (!safe)
		preserveUnquiescedLocal(local);
}

TEST_F(V2ExecPluginQuiesceLifecycleTest, LocalFailsClosedWhenInitializationThrows)
{
	ASSERT_TRUE(loadFactory(WT_COV_EXEC_V2_INIT_RACE_PATH));
	WtExecuterMgr manager;
	std::shared_ptr<WtLocalExecuter> local;
	ASSERT_TRUE(initFakeLocal(manager, local));
	ScopedEnvironmentFlag throwInit("WT_COV_EXEC_V2_INIT_THROW");
	local->on_position_changed("SHFE.rb.2510", 1.0);
	EXPECT_EQ(ExecUnitInitState::FAILED,
		manager.query_exec_unit_init_status("failed_init_local", "SHFE.rb.2510").state);

	WtExecQuiesceCoordinator coordinator({ &manager });
	const bool safe = coordinator.quiesce_all() && coordinator.is_complete() &&
		manager.wait_quiesced();
	EXPECT_FALSE(safe) << "an initialization exception does not satisfy the ABI failed-init contract";
	if (!safe)
		preserveUnquiescedLocal(local);
}

TEST_F(V2ExecPluginQuiesceLifecycleTest, InitializationExceptionDoesNotProveSynchronousCleanup)
{
	ASSERT_TRUE(createUnit(WT_COV_EXEC_V2_INIT_RACE_PATH));
	ScopedEnvironmentFlag throwInit("WT_COV_EXEC_V2_INIT_THROW");
	std::string error;
	EXPECT_FALSE(_unit->initialize(nullptr, "TEST", nullptr, error));
	EXPECT_NE(std::string::npos, error.find("threw during V2 execution unit initialization"));
	EXPECT_FALSE(_unit->failedInitializationWasSafelyDestroyed());
	EXPECT_FALSE(_unit->isInitialized());
	EXPECT_EQ(nullptr, _unit->self());
	EXPECT_EQ("get_api\ncreate_factory\ncreate_unit\ninitialize_unit\ndestroy_unit\n", log());
}

TEST_F(V2ExecPluginQuiesceLifecycleTest, DestroyExceptionDoesNotProveSynchronousCleanup)
{
	ASSERT_TRUE(createUnit(WT_COV_EXEC_V2_INIT_FAIL_PATH));
	ScopedEnvironmentFlag throwDestroy("WT_COV_EXEC_V2_DESTROY_THROW");
	std::string error;
	EXPECT_FALSE(_unit->initialize(nullptr, "TEST", nullptr, error));
	EXPECT_EQ("controlled initialization failure", error);
	EXPECT_FALSE(_unit->failedInitializationWasSafelyDestroyed());
	EXPECT_FALSE(_unit->isInitialized());
	EXPECT_EQ(nullptr, _unit->self());
	EXPECT_EQ("get_api\ncreate_factory\ncreate_unit\ninitialize_unit\ndestroy_unit\n", log());
}

TEST_F(V2ExecPluginQuiesceLifecycleTest, CreateExceptionProducesNoUnitForHostRegistration)
{
	ASSERT_TRUE(loadFactory(WT_COV_EXEC_V2_INIT_RACE_PATH));
	ScopedEnvironmentFlag throwCreate("WT_COV_EXEC_V2_CREATE_THROW");
	EXPECT_EQ(nullptr, _factory->createExeUnit("FakeV2Factory.Unit"));
	EXPECT_EQ("get_api\ncreate_factory\ncreate_unit\n", log());
}

TEST_F(V2ExecPluginQuiesceLifecycleTest, FactoryPreflightRunsBeforeUnitCreation)
{
	ASSERT_TRUE(loadFactory(WT_COV_EXEC_V2_VALID_PATH));
	EXPECT_FALSE(preflightFactories());
	EXPECT_EQ("get_api\ncreate_factory\n", log());
	_factoryLifecycle->closeFactoryConfigAdmissions();
	EXPECT_TRUE(preflightFactories());
	EXPECT_EQ("get_api\ncreate_factory\ncan_wait_factory_quiesce\n", log());
	EXPECT_FALSE(containsEvent(log(), "create_unit"));
	_factory.reset();
	_factoryPreflight = nullptr;
	_factoryLifecycle = nullptr;
	EXPECT_EQ("get_api\ncreate_factory\ncan_wait_factory_quiesce\n"
		"destroy_factory\n", log());
}

TEST_F(V2ExecPluginQuiesceLifecycleTest, ConfigCloseKeepsAnAdmittedLoaderAndRejectsLaterLoads)
{
	ASSERT_TRUE(prepareFactoryObject(WT_COV_EXEC_V2_INIT_RACE_PATH));
	ScopedLoadGatePath loadGate(loadGatePath().string());
	bool loaded = false;
	std::thread loader([&]() {
		loaded = _factory->loadFactories(_root.string().c_str());
	});

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!containsEvent(log(), "create_factory") &&
		std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const bool loaderEnteredPlugin = containsEvent(log(), "create_factory");
	_factoryLifecycle->closeFactoryConfigAdmissions();
	const bool probeDuringLoad = preflightFactories();
	const bool lateLoadAccepted = _factory->loadFactories(_root.string().c_str());
	{
		std::ofstream gate(loadGatePath().string().c_str(), std::ios::binary);
		gate << "open";
	}
	loader.join();

	EXPECT_TRUE(loaderEnteredPlugin);
	EXPECT_FALSE(probeDuringLoad);
	EXPECT_FALSE(lateLoadAccepted);
	EXPECT_TRUE(loaded);
	EXPECT_TRUE(preflightFactories());
	EXPECT_EQ(1U, countEvent(log(), "can_wait_factory_quiesce"));
	EXPECT_EQ("get_api\ncreate_factory\ncan_wait_factory_quiesce\n", log());

	EXPECT_TRUE(_factoryLifecycle->closeUnitAdmissions());
	EXPECT_TRUE(_factoryLifecycle->waitForFactoryOperationsQuiesced());
	std::vector<ExecPluginUnitSnapshot> snapshot;
	EXPECT_TRUE(_factoryLifecycle->snapshotLivePluginUnitOwners(snapshot));
	EXPECT_TRUE(snapshot.empty());
	_factory.reset();
	_factoryPreflight = nullptr;
	_factoryLifecycle = nullptr;
	EXPECT_EQ("get_api\ncreate_factory\ncan_wait_factory_quiesce\n"
		"destroy_factory\n", log());
}

TEST_F(V2ExecPluginQuiesceLifecycleTest, LoaderExceptionReleasesItsAdmissionLease)
{
	ASSERT_TRUE(prepareFactoryObject(WT_COV_EXEC_V2_INIT_RACE_PATH));
	ScopedLoadGatePath loadGate(loadGatePath().string());
	ScopedLoadThrowFlag throwFlag;
	bool loaded = false;
	std::thread loader([&]() {
		loaded = _factory->loadFactories(_root.string().c_str());
	});

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!containsEvent(log(), "create_factory") &&
		std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const bool loaderEnteredPlugin = containsEvent(log(), "create_factory");
	_factoryLifecycle->closeFactoryConfigAdmissions();
	const bool probeDuringLoad = preflightFactories();
	{
		std::ofstream gate(loadGatePath().string().c_str(), std::ios::binary);
		gate << "open";
	}
	loader.join();

	EXPECT_TRUE(loaderEnteredPlugin);
	EXPECT_FALSE(probeDuringLoad);
	EXPECT_TRUE(loaded);
	EXPECT_TRUE(preflightFactories());
	EXPECT_FALSE(containsEvent(log(), "can_wait_factory_quiesce"));
	EXPECT_EQ("get_api\ncreate_factory\n", log());

	EXPECT_TRUE(_factoryLifecycle->closeUnitAdmissions());
	EXPECT_TRUE(_factoryLifecycle->waitForFactoryOperationsQuiesced());
	std::vector<ExecPluginUnitSnapshot> snapshot;
	EXPECT_TRUE(_factoryLifecycle->snapshotLivePluginUnitOwners(snapshot));
	EXPECT_TRUE(snapshot.empty());
}

TEST_F(V2ExecPluginQuiesceLifecycleTest, ClosedUnitAdmissionRejectsDirectLateCreateAndInit)
{
	ASSERT_TRUE(loadFactory(WT_COV_EXEC_V2_VALID_PATH));
	_unit = _factory->createExeUnit("FakeV2Factory.Unit");
	ASSERT_NE(nullptr, _unit);
	_quiesce = dynamic_cast<IExecCommandQuiesce*>(_unit.get());
	ASSERT_NE(nullptr, _quiesce);
	_factoryLifecycle->closeFactoryConfigAdmissions();
	ASSERT_TRUE(preflightFactories());
	ASSERT_TRUE(_factoryLifecycle->closeUnitAdmissions());
	ASSERT_TRUE(_factoryLifecycle->waitForFactoryOperationsQuiesced());

	EXPECT_EQ(nullptr, _factory->createExeUnit("FakeV2Factory.Unit"));
	std::string error;
	EXPECT_FALSE(_unit->initialize(nullptr, "TEST", nullptr, error));
	EXPECT_NE(std::string::npos, error.find("admission is closed"));
	EXPECT_NE(nullptr, _unit->self());

	std::vector<ExecPluginUnitSnapshot> snapshot;
	ASSERT_TRUE(_factoryLifecycle->snapshotLivePluginUnitOwners(snapshot));
	ASSERT_EQ(1U, snapshot.size());
	EXPECT_FALSE(snapshot[0].initialized);
	EXPECT_EQ(_quiesce, snapshot[0].owner.get());

	_quiesce->requestQuiesce();
	EXPECT_TRUE(_quiesce->canWaitQuiescedOnCurrentThread());
	EXPECT_TRUE(_quiesce->waitQuiesced());
	snapshot.clear();
	releaseQuiescedUnit();
	EXPECT_EQ("get_api\ncreate_factory\ncreate_unit\ncan_wait_factory_quiesce\n"
		"destroy_unit\ndestroy_factory\n", log());
}

TEST_F(V2ExecPluginQuiesceLifecycleTest, UnitGateWaitsForAnAdmittedInitializeToExit)
{
	ASSERT_TRUE(loadFactory(WT_COV_EXEC_V2_INIT_RACE_PATH));
	_unit = _factory->createExeUnit("FakeV2Factory.Unit");
	ASSERT_NE(nullptr, _unit);
	_quiesce = dynamic_cast<IExecCommandQuiesce*>(_unit.get());
	ASSERT_NE(nullptr, _quiesce);
	_factoryLifecycle->closeFactoryConfigAdmissions();
	ASSERT_TRUE(preflightFactories());

	const fs::path gatePath = initGatePath();
	ScopedInitGatePath initGate(gatePath.string());
	std::atomic<bool> initialized(false);
	std::thread initializer([&]() {
		std::string error;
		initialized.store(_unit->initialize(nullptr, "TEST", nullptr, error),
			std::memory_order_release);
	});

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!containsEvent(log(), "initialize_unit") &&
		std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const bool initEnteredPlugin = containsEvent(log(), "initialize_unit");
	const bool closed = _factoryLifecycle->closeUnitAdmissions();
	std::atomic<bool> waitStarted(false);
	std::atomic<bool> waitDone(false);
	std::atomic<bool> waitResult(false);
	std::thread waiter([&]() {
		waitStarted.store(true, std::memory_order_release);
		waitResult.store(_factoryLifecycle->waitForFactoryOperationsQuiesced(),
			std::memory_order_release);
		waitDone.store(true, std::memory_order_release);
	});

	const auto waitDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!waitStarted.load(std::memory_order_acquire) &&
		std::chrono::steady_clock::now() < waitDeadline)
	{
		std::this_thread::yield();
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	const bool waiterStayedBlocked = !waitDone.load(std::memory_order_acquire);
	const bool lateCreateAccepted = _factory->createExeUnit("FakeV2Factory.Unit") != nullptr;
	{
		std::ofstream gate(gatePath.string().c_str(), std::ios::binary);
		gate << "open";
	}
	initializer.join();
	waiter.join();

	EXPECT_TRUE(initEnteredPlugin);
	EXPECT_TRUE(closed);
	EXPECT_TRUE(waiterStayedBlocked);
	EXPECT_FALSE(lateCreateAccepted);
	EXPECT_TRUE(initialized.load(std::memory_order_acquire));
	EXPECT_TRUE(waitResult.load(std::memory_order_acquire));
	_initializedSuccessfully = true;
	ASSERT_TRUE(preflightFactories());
	std::vector<ExecPluginUnitSnapshot> snapshot;
	ASSERT_TRUE(_factoryLifecycle->snapshotLivePluginUnitOwners(snapshot));
	ASSERT_EQ(1U, snapshot.size());
	EXPECT_TRUE(snapshot[0].initialized);
	EXPECT_EQ(_quiesce, snapshot[0].owner.get());

	_quiesce->requestQuiesce();
	EXPECT_TRUE(_quiesce->canWaitQuiescedOnCurrentThread());
	EXPECT_TRUE(_quiesce->waitQuiesced());
	releaseQuiescedUnit();
}

TEST_F(V2ExecPluginQuiesceLifecycleTest, ReentrantFactoryWaitFailsWithoutWaitingOnItsInitializeLease)
{
	ASSERT_TRUE(loadFactory(WT_COV_EXEC_V2_INIT_RACE_PATH));
	_unit = _factory->createExeUnit("FakeV2Factory.Unit");
	ASSERT_NE(nullptr, _unit);
	_quiesce = dynamic_cast<IExecCommandQuiesce*>(_unit.get());
	ASSERT_NE(nullptr, _quiesce);
	_factoryLifecycle->closeFactoryConfigAdmissions();
	ASSERT_TRUE(preflightFactories());

	ScopedInitGatePath initGate(initGatePath().string());
	ScopedInitReentryFlag reentryFlag;
	FactoryWaitReentryProbe probe;
	probe.lifecycle = _factoryLifecycle;
	TestExecuteContext context(reenterFactoryWait, &probe);
	std::atomic<bool> initialized(false);
	std::thread initializer([&]() {
		std::string error;
		initialized.store(_unit->initialize(
			&context, "TEST", nullptr, error),
			std::memory_order_release);
	});

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!containsEvent(log(), "initialize_unit") &&
		std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const bool initEnteredPlugin = containsEvent(log(), "initialize_unit");
	const bool closed = _factoryLifecycle->closeUnitAdmissions();
	{
		std::ofstream gate(initGatePath().string().c_str(), std::ios::binary);
		gate << "open";
	}
	initializer.join();

	EXPECT_TRUE(initEnteredPlugin);
	EXPECT_TRUE(closed);
	EXPECT_TRUE(initialized.load(std::memory_order_acquire));
	EXPECT_TRUE(probe.invoked.load(std::memory_order_acquire));
	EXPECT_FALSE(probe.waitResult.load(std::memory_order_acquire));
	_initializedSuccessfully = true;
	EXPECT_TRUE(_factoryLifecycle->waitForFactoryOperationsQuiesced());
	ASSERT_TRUE(preflightFactories());
	std::vector<ExecPluginUnitSnapshot> snapshot;
	ASSERT_TRUE(_factoryLifecycle->snapshotLivePluginUnitOwners(snapshot));
	ASSERT_EQ(1U, snapshot.size());
	EXPECT_TRUE(snapshot[0].initialized);
	EXPECT_EQ(_quiesce, snapshot[0].owner.get());

	_quiesce->requestQuiesce();
	EXPECT_TRUE(_quiesce->canWaitQuiescedOnCurrentThread());
	EXPECT_TRUE(_quiesce->waitQuiesced());
	releaseQuiescedUnit();
}

TEST_F(V2ExecPluginQuiesceLifecycleTest, RequestFailureCanRetryWithoutDestroyingUnit)
{
	ASSERT_TRUE(prepare(WT_COV_EXEC_V2_REQUEST_FAIL_PATH));
	ASSERT_TRUE(preflightFactories());
	_quiesce->requestQuiesce();
	releaseFactoryOwner();
	EXPECT_FALSE(_quiesce->canWaitQuiescedOnCurrentThread());
	EXPECT_FALSE(_quiesce->waitQuiesced());
	ASSERT_NE(nullptr, _unit->self());
	EXPECT_FALSE(containsEvent(log(), "destroy_unit"));
	EXPECT_FALSE(containsEvent(log(), "destroy_factory"));

	_quiesce->requestQuiesce();
	EXPECT_TRUE(_quiesce->canWaitQuiescedOnCurrentThread());
	EXPECT_TRUE(_quiesce->waitQuiesced());
	releaseQuiescedUnit();
	EXPECT_EQ("get_api\ncreate_factory\ncreate_unit\ninitialize_unit\n"
		"can_wait_factory_quiesce\nrequest_quiesce\nrequest_quiesce\ncan_wait_quiesce\n"
		"can_wait_quiesce\nwait_quiesce\ndestroy_unit\ndestroy_factory\n", log());
}

TEST_F(V2ExecPluginQuiesceLifecycleTest, ThrowingRequestCanRetryWithoutDestroyingUnit)
{
	ASSERT_TRUE(prepare(WT_COV_EXEC_V2_REQUEST_THROW_PATH));
	ASSERT_TRUE(preflightFactories());
	_quiesce->requestQuiesce();
	releaseFactoryOwner();
	EXPECT_FALSE(_quiesce->canWaitQuiescedOnCurrentThread());
	EXPECT_FALSE(_quiesce->waitQuiesced());
	ASSERT_NE(nullptr, _unit->self());
	EXPECT_FALSE(containsEvent(log(), "destroy_unit"));
	EXPECT_FALSE(containsEvent(log(), "destroy_factory"));

	_quiesce->requestQuiesce();
	EXPECT_TRUE(_quiesce->canWaitQuiescedOnCurrentThread());
	EXPECT_TRUE(_quiesce->waitQuiesced());
	releaseQuiescedUnit();
	EXPECT_EQ("get_api\ncreate_factory\ncreate_unit\ninitialize_unit\n"
		"can_wait_factory_quiesce\nrequest_quiesce\nrequest_quiesce\ncan_wait_quiesce\n"
		"can_wait_quiesce\nwait_quiesce\ndestroy_unit\ndestroy_factory\n", log());
}

class V2ExecPluginFactoryPreflightTest
	: public V2ExecPluginQuiesceLifecycleTest,
	  public ::testing::WithParamInterface<const char*>
{
};

TEST_P(V2ExecPluginFactoryPreflightTest, FailureAndThrowKeepOwnersAndCanRetryBeforeUnitRequest)
{
	ASSERT_TRUE(prepare(GetParam()));
	EXPECT_FALSE(preflightFactories());
	EXPECT_NE(nullptr, _unit->self());
	EXPECT_FALSE(containsEvent(log(), "request_quiesce"));
	EXPECT_FALSE(containsEvent(log(), "destroy_unit"));
	EXPECT_FALSE(containsEvent(log(), "destroy_factory"));
	EXPECT_EQ(1U, countEvent(log(), "can_wait_factory_quiesce"));

	EXPECT_TRUE(preflightFactories());
	EXPECT_EQ(2U, countEvent(log(), "can_wait_factory_quiesce"));
	_quiesce->requestQuiesce();
	EXPECT_TRUE(_quiesce->canWaitQuiescedOnCurrentThread());
	releaseFactoryOwner();
	EXPECT_TRUE(_quiesce->waitQuiesced());
	EXPECT_NE(nullptr, _unit->self());
	EXPECT_FALSE(containsEvent(log(), "destroy_unit"));
	EXPECT_FALSE(containsEvent(log(), "destroy_factory"));

	releaseQuiescedUnit();
	EXPECT_EQ(1U, countEvent(log(), "destroy_unit"));
	EXPECT_EQ(1U, countEvent(log(), "destroy_factory"));
	EXPECT_EQ("get_api\ncreate_factory\ncreate_unit\ninitialize_unit\n"
		"can_wait_factory_quiesce\ncan_wait_factory_quiesce\n"
		"request_quiesce\ncan_wait_quiesce\ncan_wait_quiesce\nwait_quiesce\n"
		"destroy_unit\ndestroy_factory\n", log());
}

INSTANTIATE_TEST_SUITE_P(
	FactoryProbeFailures,
	V2ExecPluginFactoryPreflightTest,
	::testing::Values(
		WT_COV_EXEC_V2_FACTORY_PREFLIGHT_FALSE_PATH,
		WT_COV_EXEC_V2_FACTORY_PREFLIGHT_INVALID_PATH,
		WT_COV_EXEC_V2_FACTORY_PREFLIGHT_THROW_PATH));

TEST_F(V2ExecPluginQuiesceLifecycleTest, QuiesceRequestDuringInitializationIsNotLost)
{
	ASSERT_TRUE(createUnit(WT_COV_EXEC_V2_INIT_RACE_PATH));
	_factoryLifecycle->closeFactoryConfigAdmissions();
	const fs::path gatePath = initGatePath();
	ScopedInitGatePath initGate(gatePath.string());
	bool initialized = false;
	std::thread initializer([&]() {
		std::string error;
		initialized = _unit->initialize(nullptr, "TEST", nullptr, error);
	});

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!containsEvent(log(), "initialize_unit") &&
		std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const bool initializeCallbackEntered = containsEvent(log(), "initialize_unit");
	bool factoryPreflightDone = false;
	if (initializeCallbackEntered)
	{
		EXPECT_TRUE(preflightFactories());
		factoryPreflightDone = true;
		// The plugin initialization callback is held at the gate, so the wrapper
		// has not published _initialized yet. This request must be observed by the
		// same lifecycle-mutex transaction that publishes initialization success.
		_quiesce->requestQuiesce();
		EXPECT_FALSE(containsEvent(log(), "request_quiesce"));
	}

	{
		std::ofstream gate(gatePath.string().c_str(), std::ios::binary);
		gate << "open";
	}
	initializer.join();
	_initializedSuccessfully = initialized;
	EXPECT_TRUE(initializeCallbackEntered);
	ASSERT_TRUE(initialized);
	if (!factoryPreflightDone)
		EXPECT_TRUE(preflightFactories());
	if (!initializeCallbackEntered)
		_quiesce->requestQuiesce();

	EXPECT_TRUE(containsEvent(log(), "request_quiesce"));
	EXPECT_TRUE(_quiesce->canWaitQuiescedOnCurrentThread());
	EXPECT_TRUE(_quiesce->waitQuiesced());
	EXPECT_EQ("get_api\ncreate_factory\ncreate_unit\ninitialize_unit\n"
		"can_wait_factory_quiesce\nrequest_quiesce\ncan_wait_quiesce\n"
		"can_wait_quiesce\nwait_quiesce\n",
		log());

	releaseQuiescedUnit();
	EXPECT_TRUE(containsEvent(log(), "destroy_unit"));
	EXPECT_TRUE(containsEvent(log(), "destroy_factory"));
}

class V2ExecPluginQuiescePreflightTest
	: public V2ExecPluginQuiesceLifecycleTest,
	  public ::testing::WithParamInterface<const char*>
{
};

TEST_P(V2ExecPluginQuiescePreflightTest, FalseInvalidOrThrowingPreflightCanRetry)
{
	ASSERT_TRUE(prepare(GetParam()));
	ASSERT_TRUE(preflightFactories());
	_quiesce->requestQuiesce();
	releaseFactoryOwner();
	EXPECT_FALSE(_quiesce->canWaitQuiescedOnCurrentThread());
	ASSERT_NE(nullptr, _unit->self());
	EXPECT_FALSE(containsEvent(log(), "wait_quiesce"));
	EXPECT_EQ(1U, countEvent(log(), "can_wait_quiesce"));
	EXPECT_EQ(0U, countEvent(log(), "wait_quiesce"));
	EXPECT_FALSE(containsEvent(log(), "destroy_unit"));
	EXPECT_FALSE(containsEvent(log(), "destroy_factory"));

	// waitQuiesced performs its own second preflight. The fixtures make the
	// first result transient so a control-thread retry can safely drain and own
	// destruction.
	EXPECT_TRUE(_quiesce->waitQuiesced());
	EXPECT_TRUE(containsEvent(log(), "wait_quiesce"));
	EXPECT_EQ(2U, countEvent(log(), "can_wait_quiesce"));
	EXPECT_EQ(1U, countEvent(log(), "wait_quiesce"));
	EXPECT_FALSE(containsEvent(log(), "destroy_unit"));
	EXPECT_FALSE(containsEvent(log(), "destroy_factory"));
	releaseQuiescedUnit();
	EXPECT_EQ(1U, countEvent(log(), "destroy_unit"));
	EXPECT_EQ(1U, countEvent(log(), "destroy_factory"));
}

INSTANTIATE_TEST_SUITE_P(
	TransientFailures,
	V2ExecPluginQuiescePreflightTest,
	::testing::Values(
		WT_COV_EXEC_V2_CAN_WAIT_FALSE_PATH,
		WT_COV_EXEC_V2_CAN_WAIT_INVALID_PATH,
		WT_COV_EXEC_V2_CAN_WAIT_THROW_PATH));

TEST_F(V2ExecPluginQuiesceLifecycleTest, FailedWaitCanRetryWithoutDestroyingUnit)
{
	ASSERT_TRUE(prepare(WT_COV_EXEC_V2_WAIT_FAIL_PATH));
	ASSERT_TRUE(preflightFactories());
	_quiesce->requestQuiesce();
	releaseFactoryOwner();
	EXPECT_TRUE(_quiesce->canWaitQuiescedOnCurrentThread());
	EXPECT_FALSE(_quiesce->waitQuiesced());
	ASSERT_NE(nullptr, _unit->self());
	EXPECT_FALSE(containsEvent(log(), "destroy_unit"));
	EXPECT_FALSE(containsEvent(log(), "destroy_factory"));

	EXPECT_TRUE(_quiesce->canWaitQuiescedOnCurrentThread());
	EXPECT_TRUE(_quiesce->waitQuiesced());
	releaseQuiescedUnit();
	EXPECT_EQ("get_api\ncreate_factory\ncreate_unit\ninitialize_unit\n"
		"can_wait_factory_quiesce\nrequest_quiesce\ncan_wait_quiesce\n"
		"can_wait_quiesce\nwait_quiesce\n"
		"can_wait_quiesce\ncan_wait_quiesce\nwait_quiesce\n"
		"destroy_unit\ndestroy_factory\n", log());
}

TEST_F(V2ExecPluginQuiesceLifecycleTest, ThrowingWaitCanRetryWithoutDestroyingUnit)
{
	ASSERT_TRUE(prepare(WT_COV_EXEC_V2_WAIT_THROW_PATH));
	ASSERT_TRUE(preflightFactories());
	_quiesce->requestQuiesce();
	releaseFactoryOwner();
	EXPECT_TRUE(_quiesce->canWaitQuiescedOnCurrentThread());
	EXPECT_FALSE(_quiesce->waitQuiesced());
	ASSERT_NE(nullptr, _unit->self());
	EXPECT_FALSE(containsEvent(log(), "destroy_unit"));
	EXPECT_FALSE(containsEvent(log(), "destroy_factory"));

	EXPECT_TRUE(_quiesce->canWaitQuiescedOnCurrentThread());
	EXPECT_TRUE(_quiesce->waitQuiesced());
	releaseQuiescedUnit();
	EXPECT_EQ("get_api\ncreate_factory\ncreate_unit\ninitialize_unit\n"
		"can_wait_factory_quiesce\nrequest_quiesce\ncan_wait_quiesce\n"
		"can_wait_quiesce\nwait_quiesce\n"
		"can_wait_quiesce\ncan_wait_quiesce\nwait_quiesce\n"
		"destroy_unit\ndestroy_factory\n", log());
}
}
