#include <gtest/gtest.h>

#include "WtCore/WtExecuterFactory.h"

#include <chrono>
#include <atomic>
#include <thread>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#ifndef WT_COV_EXEC_V2_INIT_FAIL_DRAINS_WORKER_PATH
#error WT_COV_EXEC_V2_INIT_FAIL_DRAINS_WORKER_PATH must name the worker-drain fixture
#endif

namespace
{
namespace fs = std::filesystem;
using namespace wtp;

class ScopedEnvVar
{
public:
	ScopedEnvVar(const char* name, const std::string& value) : _name(name)
	{
		const char* previous = std::getenv(name);
		if (previous != nullptr)
			_previous = previous;
#ifdef _WIN32
		_putenv_s(name, value.c_str());
#else
		setenv(name, value.c_str(), 1);
#endif
	}

	~ScopedEnvVar()
	{
#ifdef _WIN32
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

std::string readLog(const fs::path& path)
{
	std::ifstream input(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(input),
		std::istreambuf_iterator<char>());
}

std::vector<std::string> events(const std::string& log)
{
	std::vector<std::string> result;
	std::size_t start = 0;
	while (start < log.size())
	{
		const std::size_t end = log.find('\n', start);
		result.push_back(log.substr(start, end == std::string::npos ? end : end - start));
		if (end == std::string::npos)
			break;
		start = end + 1;
	}
	return result;
}

std::size_t eventIndex(const std::vector<std::string>& log, const std::string& event);
std::size_t eventCount(const std::vector<std::string>& log, const std::string& event);

bool waitForEventCount(const fs::path& path, const std::string& event,
	std::size_t expected, std::chrono::milliseconds timeout)
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	do
	{
		const std::vector<std::string> current = events(readLog(path));
		if (eventCount(current, event) >= expected)
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	} while (std::chrono::steady_clock::now() < deadline);
	return false;
}

std::size_t eventIndex(const std::vector<std::string>& log, const std::string& event)
{
	for (std::size_t i = 0; i < log.size(); ++i)
		if (log[i] == event)
			return i;
	return std::string::npos;
}

std::size_t eventCount(const std::vector<std::string>& log, const std::string& event)
{
	std::size_t count = 0;
	for (const std::string& current : log)
		if (current == event)
			++count;
	return count;
}

std::string uniqueName()
{
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
#ifdef _WIN32
	const int processId = _getpid();
#else
	const int processId = static_cast<int>(::getpid());
#endif
	return "wt_exec_v2_init_drain_" + std::to_string(processId) + "_" +
		std::to_string(stamp);
}

bool quiesceInitializedUnits(WtExecuterFactory& factory,
	const std::vector<std::pair<ExecuteUnitPtr*, IExecCommandQuiesce*> >& units)
{
	if (units.empty())
		return true;
	factory.closeFactoryConfigAdmissions();
	if (!factory.preflightQuiesceFactoriesOnCurrentThread())
		return false;
	if (!factory.closeUnitAdmissions() || !factory.waitForFactoryOperationsQuiesced() ||
		!factory.preflightQuiesceFactoriesOnCurrentThread())
		return false;
	for (const auto& item : units)
		item.second->requestQuiesce();
	for (const auto& item : units)
		if (!item.second->canWaitQuiescedOnCurrentThread())
			return false;
	for (const auto& item : units)
		if (!item.second->waitQuiesced())
			return false;
	return true;
}
}

TEST(WtExecPluginV2InitFailure, StopsAndJoinsWorkerBeforeHostDestroysFailedUnit)
{
	const fs::path root = fs::temp_directory_path() / uniqueName();
	std::error_code ec;
	fs::create_directories(root, ec);
	ASSERT_FALSE(ec) << ec.message();

	const fs::path fixture(WT_COV_EXEC_V2_INIT_FAIL_DRAINS_WORKER_PATH);
	const fs::path copiedFixture = root / fixture.filename();
	fs::copy_file(fixture, copiedFixture, fs::copy_options::overwrite_existing, ec);
	ASSERT_FALSE(ec) << ec.message();
	const fs::path logPath = root / "callbacks.log";
	ScopedEnvVar logEnv("WT_COV_EXEC_V2_DRAIN_LOG", logPath.string());
	ScopedEnvVar gateEnv("WT_COV_EXEC_V2_DRAIN_INIT_GATE", "");

	std::string error;
	{
		WtExecuterFactory factory;
		ASSERT_TRUE(factory.loadFactories(root.string().c_str()));
		ExecuteUnitPtr unit = factory.createExeUnit("InitFailDrainsWorker.Unit");
		ASSERT_NE(nullptr, unit);
		const bool initialized = unit->initialize(nullptr, "TEST", nullptr, error);
		EXPECT_FALSE(initialized);
		EXPECT_NE(std::string::npos, error.find("controlled initialization failure after worker drain"));
		if (initialized)
		{
			auto* hooks = dynamic_cast<IExecCommandQuiesce*>(unit.get());
			const bool cleanupComplete = hooks != nullptr && quiesceInitializedUnits(
				factory, std::vector<std::pair<ExecuteUnitPtr*, IExecCommandQuiesce*> >{
					{ &unit, hooks }});
			EXPECT_TRUE(cleanupComplete);
			if (!cleanupComplete)
				new ExecuteUnitPtr(std::move(unit));
		}
		unit.reset();
	}

	const std::vector<std::string> log = events(readLog(logPath));
	const std::size_t workerStarted = eventIndex(log, "worker_started");
	const std::size_t workerExited = eventIndex(log, "worker_exited");
	const std::size_t workerJoined = eventIndex(log, "worker_joined");
	const std::size_t initFailed = eventIndex(log, "initialize_failed");
	const std::size_t destroyAfterExit = eventIndex(log, "destroy_after_worker_exit");
	const std::size_t destroyUnit = eventIndex(log, "destroy_unit");
	const std::size_t destroyFactory = eventIndex(log, "destroy_factory");

	ASSERT_NE(std::string::npos, workerStarted);
	ASSERT_NE(std::string::npos, workerExited);
	ASSERT_NE(std::string::npos, workerJoined);
	ASSERT_NE(std::string::npos, initFailed);
	ASSERT_NE(std::string::npos, destroyAfterExit);
	ASSERT_NE(std::string::npos, destroyUnit);
	ASSERT_NE(std::string::npos, destroyFactory);
	EXPECT_LT(workerStarted, workerExited);
	EXPECT_LT(workerExited, workerJoined);
	EXPECT_LT(workerJoined, initFailed);
	EXPECT_LT(initFailed, destroyAfterExit);
	EXPECT_LT(destroyAfterExit, destroyUnit);
	EXPECT_LT(destroyUnit, destroyFactory);
	EXPECT_EQ(std::string::npos, eventIndex(log, "destroy_while_worker_joinable"));
	EXPECT_EQ(std::string::npos, eventIndex(log, "destroy_before_worker_exit"));
	EXPECT_EQ(std::string::npos, eventIndex(log, "request_quiesce"));
	EXPECT_EQ(std::string::npos, eventIndex(log, "can_wait_quiesce"));
	EXPECT_EQ(std::string::npos, eventIndex(log, "wait_quiesce"));

	fs::remove_all(root, ec);
	EXPECT_FALSE(ec) << ec.message();
}

TEST(WtExecPluginV2InitFailure, FactoryPreflightIsNonblockingDuringGatedInitialization)
{
	const fs::path root = fs::temp_directory_path() / uniqueName();
	std::error_code ec;
	fs::create_directories(root, ec);
	ASSERT_FALSE(ec) << ec.message();

	const fs::path fixture(WT_COV_EXEC_V2_INIT_FAIL_DRAINS_WORKER_PATH);
	fs::copy_file(fixture, root / fixture.filename(), fs::copy_options::overwrite_existing, ec);
	ASSERT_FALSE(ec) << ec.message();
	const fs::path logPath = root / "callbacks.log";
	const fs::path gatePath = root / "initialize.gate";
	ScopedEnvVar logEnv("WT_COV_EXEC_V2_DRAIN_LOG", logPath.string());
	ScopedEnvVar gateEnv("WT_COV_EXEC_V2_DRAIN_INIT_GATE", gatePath.string());

	std::string initError;
	std::string secondInitError;
	std::atomic<bool> firstInitialized(false);
	std::atomic<bool> secondInitialized(false);
	{
		WtExecuterFactory factory;
		ASSERT_TRUE(factory.loadFactories(root.string().c_str()));
		ExecuteUnitPtr firstUnit = factory.createExeUnit("InitFailDrainsWorker.First");
		ExecuteUnitPtr secondUnit = factory.createExeUnit("InitFailDrainsWorker.Second");
		ASSERT_NE(nullptr, firstUnit);
		ASSERT_NE(nullptr, secondUnit);
		auto* firstHooks = dynamic_cast<IExecCommandQuiesce*>(firstUnit.get());
		auto* secondHooks = dynamic_cast<IExecCommandQuiesce*>(secondUnit.get());
		ASSERT_NE(nullptr, firstHooks);
		ASSERT_NE(nullptr, secondHooks);

		std::thread firstInitializer([&]() {
			firstInitialized.store(firstUnit->initialize(nullptr, "TEST", nullptr, initError),
				std::memory_order_release);
		});
		std::thread secondInitializer([&]() {
			secondInitialized.store(secondUnit->initialize(
				nullptr, "TEST2", nullptr, secondInitError), std::memory_order_release);
		});
		const bool bothEnteredGate = waitForEventCount(logPath, "initialize_waiting", 2,
			std::chrono::seconds(5));
		factory.closeFactoryConfigAdmissions();

		std::atomic<bool> preflightDone(false);
		std::atomic<bool> preflightResult(false);
		std::thread preflight([&]() {
			preflightResult.store(factory.preflightQuiesceFactoriesOnCurrentThread(),
				std::memory_order_release);
			preflightDone.store(true, std::memory_order_release);
		});
		const auto probeDeadline = std::chrono::steady_clock::now() +
			std::chrono::seconds(1);
		while (!preflightDone.load(std::memory_order_acquire) &&
			std::chrono::steady_clock::now() < probeDeadline)
			std::this_thread::yield();
		const bool probeReturnedBeforeInit = preflightDone.load(std::memory_order_acquire);

		// Always open the gate before joining, even if the preflight regresses and blocks.
		{
			std::ofstream gate(gatePath, std::ios::binary);
			gate << "open";
		}
		firstInitializer.join();
		secondInitializer.join();
		preflight.join();

		EXPECT_TRUE(bothEnteredGate);
		EXPECT_TRUE(probeReturnedBeforeInit);
		EXPECT_FALSE(preflightResult.load(std::memory_order_acquire));
		EXPECT_TRUE(firstInitialized.load(std::memory_order_acquire)) << initError;
		EXPECT_TRUE(secondInitialized.load(std::memory_order_acquire)) << secondInitError;

		std::vector<std::pair<ExecuteUnitPtr*, IExecCommandQuiesce*> > initializedUnits;
		if (firstInitialized.load(std::memory_order_acquire))
			initializedUnits.push_back({ &firstUnit, firstHooks });
		if (secondInitialized.load(std::memory_order_acquire))
			initializedUnits.push_back({ &secondUnit, secondHooks });
		const bool cleanupComplete = quiesceInitializedUnits(factory, initializedUnits);
		if (!initializedUnits.empty())
		{
			EXPECT_TRUE(cleanupComplete);
			if (!cleanupComplete)
			{
				if (firstInitialized.load(std::memory_order_acquire))
					new ExecuteUnitPtr(std::move(firstUnit));
				if (secondInitialized.load(std::memory_order_acquire))
					new ExecuteUnitPtr(std::move(secondUnit));
			}
		}
		firstUnit.reset();
		secondUnit.reset();
	}

	const std::vector<std::string> log = events(readLog(logPath));
	const std::size_t initializeWaiting = eventIndex(log, "initialize_waiting");
	const std::size_t firstFactoryProbe = eventIndex(log, "can_wait_factory_quiesce");
	const std::size_t initializedEvent = eventIndex(log, "initialize_succeeded");
	const std::size_t request = eventIndex(log, "request_quiesce");
	const std::size_t joined = eventIndex(log, "worker_joined");
	const std::size_t destroy = eventIndex(log, "destroy_unit");
	ASSERT_NE(std::string::npos, initializeWaiting);
	ASSERT_NE(std::string::npos, firstFactoryProbe);
	ASSERT_NE(std::string::npos, initializedEvent);
	ASSERT_NE(std::string::npos, request);
	ASSERT_NE(std::string::npos, joined);
	ASSERT_NE(std::string::npos, destroy);
	EXPECT_LT(initializeWaiting, firstFactoryProbe);
	EXPECT_LT(firstFactoryProbe, initializedEvent);
	EXPECT_LT(initializedEvent, request);
	EXPECT_LT(request, joined);
	EXPECT_LT(joined, destroy);
	EXPECT_EQ(2U, eventCount(log, "worker_started"));
	EXPECT_EQ(2U, eventCount(log, "worker_exited"));
	EXPECT_EQ(2U, eventCount(log, "worker_joined"));
	EXPECT_EQ(2U, eventCount(log, "destroy_unit"));

	fs::remove_all(root, ec);
	EXPECT_FALSE(ec) << ec.message();
}
