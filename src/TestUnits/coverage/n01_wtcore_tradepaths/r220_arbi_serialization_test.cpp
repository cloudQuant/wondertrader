#include <WtCore/WtArbiExecuter.h>
#include <WtCore/WtExecMgr.h>
#include <WtCore/WtExecuterFactory.h>
#include <WTSUtils/WTSCfgLoader.h>

#include <boost/core/noncopyable.hpp>
#include <Includes/FasterDefs.h>
#include <Includes/ITraderApi.h>
#include <Share/BoostFile.hpp>
#include <Share/DLLHelper.hpp>
#include <Share/SpinMutex.hpp>
#include <Share/StdUtils.hpp>

#define private public
#include <WtCore/TraderAdapter.h>
#undef private

#include "r71_wtcore_env.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace wtp;

namespace {

class ArbiProbe
{
public:
	void enter()
	{
		const int current = ++active;
		int previous = max_active.load();
		while (previous < current && !max_active.compare_exchange_weak(previous, current))
		{
		}
	}

	void leave()
	{
		--active;
		condition.notify_all();
	}

	void onPosition(double target)
	{
		enter();
		std::function<void()> hook;
		bool should_throw = false;
		{
			std::unique_lock<std::mutex> lock(mutex);
			positions.push_back(target);
			event_order.push_back("position:" + std::to_string(target));
			if (block_position)
			{
				position_entered = true;
				condition.notify_all();
				condition.wait(lock, [this]() { return release_position; });
				block_position = false;
			}
			hook = position_hook;
			position_hook = std::function<void()>();
			should_throw = throw_next_position;
			throw_next_position = false;
		}
		try
		{
			if (hook)
				hook();
		}
		catch (...)
		{
			leave();
			throw;
		}
		leave();
		if (should_throw)
			throw std::runtime_error("ArbiProbe injected set_position failure");
	}

	void onChannel(const char* event)
	{
		enter();
		std::unique_lock<std::mutex> lock(mutex);
		channel_events.push_back(event);
		if (block_ready && std::string(event) == "ready")
		{
			ready_entered = true;
			condition.notify_all();
			condition.wait(lock, [this]() { return release_ready; });
			block_ready = false;
		}
		std::function<void()> hook = channel_hook;
		channel_hook = std::function<void()>();
		lock.unlock();
		try
		{
			if (hook)
				hook();
		}
		catch (...)
		{
			leave();
			throw;
		}
		leave();
	}

	void onTick()
	{
		std::lock_guard<std::mutex> lock(mutex);
		++ticks;
		event_order.push_back("tick");
		condition.notify_all();
	}

	void onAccount(double balance)
	{
		std::lock_guard<std::mutex> lock(mutex);
		account_balances.push_back(balance);
		condition.notify_all();
	}

	bool waitPositionEntered(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(mutex);
		return condition.wait_for(lock, timeout, [this]() { return position_entered; });
	}

	bool waitPositionCount(size_t count, std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(mutex);
		return condition.wait_for(lock, timeout, [this, count]() { return positions.size() >= count; });
	}

	bool waitChannelCount(size_t count, std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(mutex);
		return condition.wait_for(lock, timeout, [this, count]() { return channel_events.size() >= count; });
	}

	bool waitTickCount(size_t count, std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(mutex);
		return condition.wait_for(lock, timeout, [this, count]() { return ticks >= count; });
	}

	void releasePosition()
	{
		std::lock_guard<std::mutex> lock(mutex);
		release_position = true;
		condition.notify_all();
	}

	void releaseReady()
	{
		std::lock_guard<std::mutex> lock(mutex);
		release_ready = true;
		condition.notify_all();
	}

	void blockNextPosition()
	{
		std::lock_guard<std::mutex> lock(mutex);
		block_position = true;
		position_entered = false;
		release_position = false;
	}

	void blockNextReady()
	{
		std::lock_guard<std::mutex> lock(mutex);
		block_ready = true;
		ready_entered = false;
		release_ready = false;
	}

	void throwNextPosition()
	{
		std::lock_guard<std::mutex> lock(mutex);
		throw_next_position = true;
	}

	std::vector<double> positionSnapshot()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return positions;
	}

	std::vector<std::string> channelSnapshot()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return channel_events;
	}

	std::vector<std::string> eventOrderSnapshot()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return event_order;
	}

	std::vector<double> accountSnapshot()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return account_balances;
	}

	std::mutex mutex;
	std::condition_variable condition;
	std::vector<double> positions;
	std::vector<std::string> channel_events;
	std::vector<std::string> event_order;
	std::vector<double> account_balances;
	std::function<void()> position_hook;
	std::function<void()> channel_hook;
	uint32_t ticks = 0;
	std::atomic<int> active{ 0 };
	std::atomic<int> max_active{ 0 };
	bool block_position = false;
	bool position_entered = false;
	bool release_position = false;
	bool block_ready = false;
	bool ready_entered = false;
	bool release_ready = false;
	bool throw_next_position = false;
};

class ArbiProbeUnit : public ExecuteUnit
{
public:
	explicit ArbiProbeUnit(const std::shared_ptr<ArbiProbe>& probe) : _probe(probe) {}
	const char* getName() override { return "ArbiProbeUnit"; }
	const char* getFactName() override { return "ArbiProbeFact"; }
	void set_position(const char*, double target) override { _probe->onPosition(target); }
	void on_tick(WTSTickData*) override { _probe->onTick(); }
	void on_trade(uint32_t, const char*, bool, double, double) override {}
	void on_order(uint32_t, const char*, bool, double, double, bool) override {}
	void on_entrust(uint32_t, const char*, bool, const char*) override {}
	void on_channel_ready() override { _probe->onChannel("ready"); }
	void on_channel_lost() override { _probe->onChannel("lost"); }
	void on_account(const char*, double, double balance, double, double, double, double,
		double, double, double, double) override { _probe->onAccount(balance); }

private:
	std::shared_ptr<ArbiProbe> _probe;
};

// This legacy test unit models a third-party unit with its own background
// worker. It has no versioned factory preflight contract, so host quiescence
// must retain it and report incomplete while that worker is still live.
class R220BackgroundArbiUnit : public ArbiProbeUnit
{
public:
	explicit R220BackgroundArbiUnit(const std::shared_ptr<ArbiProbe>& probe)
		: ArbiProbeUnit(probe)
		, _background([this]() {
			std::unique_lock<std::mutex> lock(_mutex);
			_entered = true;
			_condition.notify_all();
			_condition.wait(lock, [this]() { return _released; });
			_done = true;
			_condition.notify_all();
		})
	{
	}

	~R220BackgroundArbiUnit() override
	{
		releaseBackground();
		if (_background.joinable())
			_background.join();
	}

	bool waitBackgroundEntered(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(_mutex);
		return _condition.wait_for(lock, timeout, [this]() { return _entered; });
	}

	bool backgroundDone()
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _done;
	}

	bool waitBackgroundDone(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(_mutex);
		return _condition.wait_for(lock, timeout, [this]() { return _done; });
	}

	void releaseBackground()
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_released = true;
		_condition.notify_all();
	}

private:
	std::mutex _mutex;
	std::condition_variable _condition;
	bool _entered = false;
	bool _released = false;
	bool _done = false;
	std::thread _background;
};

class ArbiProbeFact : public IExecuterFact
{
public:
	const char* getName() override { return "ArbiProbeFact"; }
	void enumExeUnit(FuncEnumUnitCallback) override {}
	ExecuteUnit* createExeUnit(const char*) override { return NULL; }
	ExecuteUnit* createDiffExeUnit(const char*) override { return NULL; }
	ExecuteUnit* createArbiExeUnit(const char*) override { return NULL; }
	bool deleteExeUnit(ExecuteUnit* unit) override { delete unit; return true; }
};

class R220ThrowingArbi : public WtArbiExecuter
{
public:
	using WtArbiExecuter::WtArbiExecuter;
	WTSTickData* injected_tick = NULL;
	std::atomic<bool> throw_on_first_callback{ true };
	std::atomic<bool> request_quiesce_on_failure{ false };
	void writeLog(const char* message) override
	{
		std::lock_guard<std::mutex> lock(log_mutex);
		logs.push_back(message ? message : "");
	}
	bool hasLogContaining(const std::string& needle)
	{
		std::lock_guard<std::mutex> lock(log_mutex);
		return std::any_of(logs.begin(), logs.end(), [&needle](const std::string& line) {
			return line.find(needle) != std::string::npos;
		});
	}

	double getUndoneQty(const char* code) override
	{
		if (throw_on_first_callback.exchange(false))
		{
			on_tick(code, injected_tick);
			if (request_quiesce_on_failure.exchange(false))
				requestQuiesce();
			throw std::runtime_error("R220 injected initialization callback failure");
		}
		return 0.0;
	}

	private:
	std::mutex log_mutex;
	std::vector<std::string> logs;
};

} // namespace

namespace wtp {

class WtArbiExecuterTestAccess
{
public:
	static std::unique_ptr<WtArbiExecuter::QuiesceIngress> holdIngress(
		WtArbiExecuter& executer)
	{
		return std::unique_ptr<WtArbiExecuter::QuiesceIngress>(
			new WtArbiExecuter::QuiesceIngress(&executer));
	}

	static void registerUnit(WtArbiExecuter& executer, const std::string& code,
		const std::shared_ptr<ArbiProbe>& probe, IExecuterFact* fact, bool withPool = true)
	{
		if (withPool && !executer._pool)
			executer._pool.reset(new boost::threadpool::pool(2));
		ExecuteUnitPtr unit(new ExeUnitWrapper(new ArbiProbeUnit(probe), fact));
		std::shared_ptr<WtArbiExecuter::UnitLifecycleState> state(
			new WtArbiExecuter::UnitLifecycleState());
		state->creation_complete = true;
		state->result = unit;
		state->retained_unit = unit;
		state->init_status = ExecUnitInitState::READY;
		state->init_generation = 1;
		state->code = code;
		SpinLock lock(executer._mtx_units);
		executer._unit_map[code] = unit;
		executer._unit_states[code] = state;
	}

	static R220BackgroundArbiUnit* registerBackgroundUnit(WtArbiExecuter& executer,
		const std::string& code, const std::shared_ptr<ArbiProbe>& probe,
		IExecuterFact* fact)
	{
		R220BackgroundArbiUnit* backgroundUnit = new R220BackgroundArbiUnit(probe);
		ExecuteUnitPtr unit(new ExeUnitWrapper(backgroundUnit, fact));
		std::shared_ptr<WtArbiExecuter::UnitLifecycleState> state(
			new WtArbiExecuter::UnitLifecycleState());
		state->creation_complete = true;
		state->result = unit;
		state->retained_unit = unit;
		state->init_status = ExecUnitInitState::READY;
		state->init_generation = 1;
		state->code = code;
		SpinLock lock(executer._mtx_units);
		executer._unit_map[code] = unit;
		executer._unit_states[code] = state;
		return backgroundUnit;
	}

	static std::shared_ptr<WtArbiExecuter::UnitLifecycleState> beginPending(
		WtArbiExecuter& executer, const std::string& code)
	{
		std::shared_ptr<WtArbiExecuter::UnitLifecycleState> state(
			new WtArbiExecuter::UnitLifecycleState());
		state->creation_owner = std::this_thread::get_id();
		state->init_generation = 1;
		state->code = code;
		SpinLock lock(executer._mtx_units);
		executer._unit_states[code] = state;
		return state;
	}

	static void attachPendingUnit(WtArbiExecuter& executer, const std::string& code,
		const std::shared_ptr<ArbiProbe>& probe, IExecuterFact* fact)
	{
		ExecuteUnitPtr unit(new ExeUnitWrapper(new ArbiProbeUnit(probe), fact));
		SpinLock lock(executer._mtx_units);
		auto stateIt = executer._unit_states.find(code);
		if (stateIt != executer._unit_states.end())
		{
			std::lock_guard<std::mutex> stateLock(stateIt->second->mutex);
			if (!stateIt->second->callback_active)
				executer.reserveQuiesceDrain();
			stateIt->second->callback_active = true;
			stateIt->second->callback_drain_started = false;
			stateIt->second->callback_owner = std::this_thread::get_id();
		}
		executer._unit_map[code] = unit;
	}

	static ExecuteUnitPtr mappedUnit(WtArbiExecuter& executer, const std::string& code)
	{
		SpinLock lock(executer._mtx_units);
		auto it = executer._unit_map.find(code);
		return it == executer._unit_map.end() ? ExecuteUnitPtr() : it->second;
	}

	static ExecuteUnitPtr stateOwner(WtArbiExecuter& executer, const std::string& code)
	{
		SpinLock lock(executer._mtx_units);
		auto it = executer._unit_states.find(code);
		if (it == executer._unit_states.end())
			return ExecuteUnitPtr();
		std::lock_guard<std::mutex> stateLock(it->second->mutex);
		return it->second->retained_unit != NULL ?
			it->second->retained_unit : it->second->result;
	}

	static void makeInitializing(WtArbiExecuter& executer, const std::string& code)
	{
		SpinLock lock(executer._mtx_units);
		auto state = executer._unit_states.at(code);
		std::lock_guard<std::mutex> stateLock(state->mutex);
		state->creation_complete = false;
		state->result.reset();
		state->init_status = ExecUnitInitState::INITIALIZING;
	}

	static size_t queuedEvents(const std::shared_ptr<WtArbiExecuter::UnitLifecycleState>& state)
	{
		std::lock_guard<std::mutex> lock(state->mutex);
		return state->events.size();
	}

	static size_t queuedEvents(WtArbiExecuter& executer, const std::string& code)
	{
		SpinLock lock(executer._mtx_units);
		auto it = executer._unit_states.find(code);
		if (it == executer._unit_states.end())
			return 0;
		std::lock_guard<std::mutex> stateLock(it->second->mutex);
		return it->second->events.size();
	}

	static void drainPending(WtArbiExecuter& executer, const std::string& code)
	{
		ExecuteUnitPtr unit;
		std::shared_ptr<WtArbiExecuter::UnitLifecycleState> state;
		{
			SpinLock lock(executer._mtx_units);
			unit = executer._unit_map[code];
			state = executer._unit_states[code];
		}
		executer.drainUnitEvents(unit, state, true);
		{
			SpinLock lock(executer._mtx_units);
			std::lock_guard<std::mutex> stateLock(state->mutex);
			state->result = unit;
			state->retained_unit = unit;
			state->init_status = ExecUnitInitState::READY;
			state->creation_complete = true;
			state->creation_owner = std::thread::id();
		}
		state->condition.notify_all();
	}

	static void attemptReservedDrain(WtArbiExecuter& executer, const std::string& code)
	{
		ExecuteUnitPtr unit;
		std::shared_ptr<WtArbiExecuter::UnitLifecycleState> state;
		{
			SpinLock lock(executer._mtx_units);
			unit = executer._unit_map.at(code);
			state = executer._unit_states.at(code);
		}
		executer.drainUnitEvents(unit, state, true);
	}

	static double target(WtArbiExecuter& executer, const std::string& code)
	{
		std::lock_guard<std::mutex> lock(executer._mtx_positions);
		auto it = executer._target_pos.find(code);
		return it == executer._target_pos.end() ? 0.0 : it->second;
	}
	static bool channelReady(WtArbiExecuter& executer) { return executer._channel_ready.load(); }

	static uint64_t beginBatch(WtArbiExecuter& executer) { return executer.beginPositionBatch(); }
	static void applyBatch(WtArbiExecuter& executer,
		const wt_hashmap<std::string, double>& targets, uint64_t sequence)
	{
		executer.set_position_with_sequence(targets, sequence);
	}
	static std::vector<std::string> snapshotMissing(WtArbiExecuter& executer,
		const wt_hashmap<std::string, double>& targets)
	{
		return executer.snapshotMissingTargets(targets);
	}
	static void clearMissing(WtArbiExecuter& executer, const std::vector<std::string>& codes,
		const wt_hashmap<std::string, double>& targets, uint64_t sequence)
	{
		executer.clearMissingTargets(codes, targets, sequence);
	}
	static void setStrictSync(WtArbiExecuter& executer, bool value) { executer._strict_sync = value; }
	static void addChannelHold(WtArbiExecuter& executer, const std::string& code)
	{
		std::lock_guard<std::mutex> lock(executer._mtx_positions);
		executer._channel_holds.insert(code);
	}
	static std::vector<std::string> snapshotUnmanaged(WtArbiExecuter& executer)
	{
		return executer.snapshotUnmanagedChannelHolds();
	}
	static void clearUnmanaged(WtArbiExecuter& executer, const std::vector<std::string>& codes,
		uint64_t sequence)
	{
		executer.clearUnmanagedChannelHolds(codes, sequence);
	}
};

} // namespace wtp

TEST(WtArbiExecuterSerialization, SameCodePositionsAreFifoAndNeverOverlap)
{
	ArbiProbeFact fact;
	auto probe = std::make_shared<ArbiProbe>();
	WtArbiExecuter executer(NULL, "arbi_same_code_serialization", NULL);
	wtp::WtArbiExecuterTestAccess::registerUnit(executer, "SHFE.rb.2510", probe, &fact);
	probe->blockNextPosition();

	executer.on_position_changed("SHFE.rb.2510", 1.0);
	const bool firstEntered = probe->waitPositionEntered(std::chrono::seconds(2));
	if (!firstEntered)
		probe->releasePosition();
	ASSERT_TRUE(firstEntered);
	executer.on_position_changed("SHFE.rb.2510", 2.0);
	probe->releasePosition();

	ASSERT_TRUE(probe->waitPositionCount(2, std::chrono::seconds(2)));
	const std::vector<double> positions = probe->positionSnapshot();
	ASSERT_EQ(positions.size(), 2u);
	EXPECT_DOUBLE_EQ(positions[0], 1.0);
	EXPECT_DOUBLE_EQ(positions[1], 3.0);
	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, "SHFE.rb.2510"), 3.0);
	EXPECT_EQ(probe->max_active.load(), 1);
}

TEST(WtArbiExecuterSerialization, OlderPositionBatchCannotClearNewerTargetSnapshots)
{
	ArbiProbeFact fact;
	auto probe = std::make_shared<ArbiProbe>();
	WtArbiExecuter executer(NULL, "arbi_stale_position_batch", NULL);
	wtp::WtArbiExecuterTestAccess::registerUnit(executer, "SHFE.rb.2510", probe, &fact, false);
	wtp::WtArbiExecuterTestAccess::registerUnit(executer, "SHFE.ag.2512", probe, &fact, false);

	const uint64_t oldBatch = wtp::WtArbiExecuterTestAccess::beginBatch(executer);
	wt_hashmap<std::string, double> emptyTargets;
	const std::vector<std::string> oldCodes =
		wtp::WtArbiExecuterTestAccess::snapshotMissing(executer, emptyTargets);
	const uint64_t newBatch = wtp::WtArbiExecuterTestAccess::beginBatch(executer);
	wt_hashmap<std::string, double> newTargets;
	newTargets["SHFE.rb.2510"] = 7.0;
	wtp::WtArbiExecuterTestAccess::applyBatch(executer, newTargets, newBatch);
	wtp::WtArbiExecuterTestAccess::clearMissing(executer, oldCodes, emptyTargets, oldBatch);
	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, "SHFE.rb.2510"), 7.0);

	wtp::WtArbiExecuterTestAccess::setStrictSync(executer, true);
	wtp::WtArbiExecuterTestAccess::addChannelHold(executer, "SHFE.ag.2512");
	const uint64_t oldStrictBatch = wtp::WtArbiExecuterTestAccess::beginBatch(executer);
	const std::vector<std::string> oldHolds =
		wtp::WtArbiExecuterTestAccess::snapshotUnmanaged(executer);
	executer.on_position_changed("SHFE.ag.2512", 5.0);
	wtp::WtArbiExecuterTestAccess::clearUnmanaged(executer, oldHolds, oldStrictBatch);
	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, "SHFE.ag.2512"), 5.0);

	const std::vector<double> positions = probe->positionSnapshot();
	ASSERT_EQ(positions.size(), 2u);
	EXPECT_DOUBLE_EQ(positions[0], 7.0);
	EXPECT_DOUBLE_EQ(positions[1], 5.0);
}

TEST(WtArbiExecuterSerialization, PublicSetPositionAppliesGroupTargetsAndClearsOnEmptyInput)
{
	ArbiProbeFact fact;
	auto groupProbe = std::make_shared<ArbiProbe>();
	auto rbProbe = std::make_shared<ArbiProbe>();
	auto auProbe = std::make_shared<ArbiProbe>();
	WtArbiExecuter executer(NULL, "arbi_group_targets", NULL);
	std::shared_ptr<WTSVariant> config(WTSCfgLoader::load_from_content(
		R"json({"scale":1.0,"poolsize":0,"strict_sync":false,
		"groups":{"SHFE.ags":{"SHFE.rb.2510":2.0,"SHFE.au.2212":2.0}},
		"policy":{"default":{"name":"WtExeFact.WtMinImpactExeUnit"}}})json"),
		[](WTSVariant* value) { if (value) value->release(); });
	ASSERT_NE(config.get(), nullptr);
	ASSERT_TRUE(executer.init(config.get()));

	const std::string groupCode = "SHFE.ags";
	const std::string rbCode = "SHFE.rb.2510";
	const std::string auCode = "SHFE.au.2212";
	wtp::WtArbiExecuterTestAccess::registerUnit(executer, groupCode, groupProbe, &fact, false);
	wtp::WtArbiExecuterTestAccess::registerUnit(executer, rbCode, rbProbe, &fact, false);
	wtp::WtArbiExecuterTestAccess::registerUnit(executer, auCode, auProbe, &fact, false);

	auto expectLastTarget = [](const std::shared_ptr<ArbiProbe>& probe,
		double expected, size_t expectedCount) {
		const std::vector<double> positions = probe->positionSnapshot();
		ASSERT_EQ(positions.size(), expectedCount);
		EXPECT_DOUBLE_EQ(positions.back(), expected);
	};

	wt_hashmap<std::string, double> negativeTargets;
	negativeTargets[rbCode] = -6.0;
	negativeTargets[auCode] = -8.0;
	executer.set_position(negativeTargets);
	EXPECT_TRUE(groupProbe->positionSnapshot().empty());
	expectLastTarget(rbProbe, -6.0, 1);
	expectLastTarget(auProbe, -8.0, 1);
	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, rbCode), -6.0);
	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, auCode), -8.0);

	// Fractional member targets stay on the original path; the existing Arbi
	// dispatch rounds unit callbacks while the target ledger retains precision.
	wt_hashmap<std::string, double> fractionalGroupTargets;
	fractionalGroupTargets[rbCode] = 7.5;
	fractionalGroupTargets[auCode] = 8.0;
	executer.set_position(fractionalGroupTargets);
	EXPECT_TRUE(groupProbe->positionSnapshot().empty());
	expectLastTarget(rbProbe, 8.0, 2);
	expectLastTarget(auProbe, 8.0, 2);
	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, rbCode), 7.5);
	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, auCode), 8.0);

	// Integer member targets may also imply a fractional group count.
	wt_hashmap<std::string, double> fractionalCountTargets;
	fractionalCountTargets[rbCode] = 7.0;
	fractionalCountTargets[auCode] = 8.0;
	executer.set_position(fractionalCountTargets);
	EXPECT_TRUE(groupProbe->positionSnapshot().empty());
	expectLastTarget(rbProbe, 7.0, 3);
	expectLastTarget(auProbe, 8.0, 3);
	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, rbCode), 7.0);
	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, auCode), 8.0);

	wt_hashmap<std::string, double> targets;
	targets[rbCode] = 6.0;
	targets[auCode] = 8.0;
	executer.set_position(targets);

	expectLastTarget(groupProbe, 3.0, 1);
	expectLastTarget(rbProbe, 0.0, 4);
	expectLastTarget(auProbe, 2.0, 4);
	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, groupCode), 3.0);
	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, rbCode), 0.0);
	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, auCode), 2.0);

	executer.set_position(targets);
	expectLastTarget(groupProbe, 3.0, 2);
	expectLastTarget(rbProbe, 0.0, 5);
	expectLastTarget(auProbe, 2.0, 5);
	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, groupCode), 3.0);
	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, rbCode), 0.0);
	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, auCode), 2.0);

	wt_hashmap<std::string, double> empty;
	executer.set_position(empty);
	expectLastTarget(groupProbe, 0.0, 3);
	expectLastTarget(rbProbe, 0.0, 6);
	expectLastTarget(auProbe, 0.0, 6);
	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, groupCode), 0.0);
	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, rbCode), 0.0);
	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, auCode), 0.0);
}

TEST(WtArbiExecuterSerialization, OverlappingGroupsPreserveOriginalTargets)
{
	ArbiProbeFact fact;
	auto firstGroupProbe = std::make_shared<ArbiProbe>();
	auto secondGroupProbe = std::make_shared<ArbiProbe>();
	auto rbProbe = std::make_shared<ArbiProbe>();
	auto auProbe = std::make_shared<ArbiProbe>();
	auto cuProbe = std::make_shared<ArbiProbe>();
	WtArbiExecuter executer(NULL, "arbi_overlapping_group_targets", NULL);
	std::shared_ptr<WTSVariant> config(WTSCfgLoader::load_from_content(
		R"json({"scale":1.0,"poolsize":0,"strict_sync":false,
		"groups":{"SHFE.ags":{"SHFE.rb.2510":2.0,"SHFE.au.2212":2.0},
		"SHFE.agc":{"SHFE.rb.2510":1.0,"SHFE.cu.2212":1.0}},
		"policy":{"default":{"name":"WtExeFact.WtMinImpactExeUnit"}}})json"),
		[](WTSVariant* value) { if (value) value->release(); });
	ASSERT_NE(config.get(), nullptr);
	ASSERT_TRUE(executer.init(config.get()));

	const std::string firstGroupCode = "SHFE.ags";
	const std::string secondGroupCode = "SHFE.agc";
	const std::string rbCode = "SHFE.rb.2510";
	const std::string auCode = "SHFE.au.2212";
	const std::string cuCode = "SHFE.cu.2212";
	wtp::WtArbiExecuterTestAccess::registerUnit(executer, firstGroupCode, firstGroupProbe, &fact, false);
	wtp::WtArbiExecuterTestAccess::registerUnit(executer, secondGroupCode, secondGroupProbe, &fact, false);
	wtp::WtArbiExecuterTestAccess::registerUnit(executer, rbCode, rbProbe, &fact, false);
	wtp::WtArbiExecuterTestAccess::registerUnit(executer, auCode, auProbe, &fact, false);
	wtp::WtArbiExecuterTestAccess::registerUnit(executer, cuCode, cuProbe, &fact, false);

	wt_hashmap<std::string, double> targets;
	targets[rbCode] = 6.0;
	targets[auCode] = 8.0;
	targets[cuCode] = 5.0;
	executer.set_position(targets);

	EXPECT_TRUE(firstGroupProbe->positionSnapshot().empty());
	EXPECT_TRUE(secondGroupProbe->positionSnapshot().empty());
	ASSERT_EQ(rbProbe->positionSnapshot().size(), 1u);
	ASSERT_EQ(auProbe->positionSnapshot().size(), 1u);
	ASSERT_EQ(cuProbe->positionSnapshot().size(), 1u);
	EXPECT_DOUBLE_EQ(rbProbe->positionSnapshot().back(), 6.0);
	EXPECT_DOUBLE_EQ(auProbe->positionSnapshot().back(), 8.0);
	EXPECT_DOUBLE_EQ(cuProbe->positionSnapshot().back(), 5.0);
	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, rbCode), 6.0);
	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, auCode), 8.0);
	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, cuCode), 5.0);
}

TEST(WtArbiExecuterSerialization, OlderTargetBatchCannotWriteAfterNewerBatchStarts)
{
	ArbiProbeFact fact;
	auto probe = std::make_shared<ArbiProbe>();
	WtArbiExecuter executer(NULL, "arbi_stale_target_batch", NULL);
	wtp::WtArbiExecuterTestAccess::registerUnit(executer, "SHFE.rb.2510", probe, &fact, false);

	const uint64_t oldBatch = wtp::WtArbiExecuterTestAccess::beginBatch(executer);
	wt_hashmap<std::string, double> oldTargets;
	oldTargets["SHFE.rb.2510"] = 9.0;
	std::mutex gateMutex;
	std::condition_variable gateCondition;
	bool applyOldBatch = false;
	std::thread olderBatch([&]() {
		std::unique_lock<std::mutex> lock(gateMutex);
		gateCondition.wait(lock, [&applyOldBatch]() { return applyOldBatch; });
		lock.unlock();
		wtp::WtArbiExecuterTestAccess::applyBatch(executer, oldTargets, oldBatch);
	});

	const uint64_t newBatch = wtp::WtArbiExecuterTestAccess::beginBatch(executer);
	wt_hashmap<std::string, double> newTargets;
	wtp::WtArbiExecuterTestAccess::applyBatch(executer, newTargets, newBatch);
	{
		std::lock_guard<std::mutex> lock(gateMutex);
		applyOldBatch = true;
	}
	gateCondition.notify_one();
	olderBatch.join();

	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, "SHFE.rb.2510"), 0.0);
	EXPECT_TRUE(probe->positionSnapshot().empty());
}

TEST(WtArbiExecuterSerialization, DifferentCodesCanRunInParallel)
{
	ArbiProbeFact fact;
	auto probe = std::make_shared<ArbiProbe>();
	WtArbiExecuter executer(NULL, "arbi_cross_code_parallel", NULL);
	wtp::WtArbiExecuterTestAccess::registerUnit(executer, "SHFE.rb.2510", probe, &fact);
	wtp::WtArbiExecuterTestAccess::registerUnit(executer, "SHFE.ag.2512", probe, &fact);
	probe->blockNextPosition();

	executer.on_position_changed("SHFE.rb.2510", 1.0);
	executer.on_position_changed("SHFE.ag.2512", 1.0);
	const bool bothEntered = probe->waitPositionCount(2, std::chrono::seconds(2));
	if (!bothEntered)
		probe->releasePosition();
	ASSERT_TRUE(bothEntered);
	EXPECT_EQ(probe->max_active.load(), 2);
	probe->releasePosition();
}

TEST(WtArbiExecuterSerialization, ReadyAndLostCallbacksRemainOrdered)
{
	ArbiProbeFact fact;
	auto probe = std::make_shared<ArbiProbe>();
	WtArbiExecuter executer(NULL, "arbi_channel_order", NULL);
	wtp::WtArbiExecuterTestAccess::registerUnit(executer, "SHFE.rb.2510", probe, &fact);
	probe->blockNextReady();

	executer.on_channel_ready();
	const bool readyEntered = probe->waitChannelCount(1, std::chrono::seconds(2));
	if (!readyEntered)
		probe->releaseReady();
	ASSERT_TRUE(readyEntered);
	executer.on_channel_lost();
	probe->releaseReady();

	ASSERT_TRUE(probe->waitChannelCount(2, std::chrono::seconds(2)));
	const std::vector<std::string> events = probe->channelSnapshot();
	ASSERT_EQ(events.size(), 2u);
	EXPECT_EQ(events[0], "ready");
	EXPECT_EQ(events[1], "lost");
}

TEST(WtArbiExecuterSerialization, RepeatedReadyAndNullTraderAssignmentAreIdempotent)
{
	ArbiProbeFact fact;
	auto probe = std::make_shared<ArbiProbe>();
	WtArbiExecuter executer(NULL, "arbi_ready_idempotent", NULL);
	wtp::WtArbiExecuterTestAccess::registerUnit(executer, "SHFE.rb.2510", probe, &fact, false);

	executer.on_channel_ready();
	executer.setTrader(NULL);
	executer.on_channel_ready();
	const std::vector<std::string> events = probe->channelSnapshot();
	ASSERT_EQ(events.size(), 1u);
	EXPECT_EQ(events[0], "ready");
}

TEST(WtArbiExecuterSerialization, ReadyCallbackAfterTraderBindingIsObservableAndIdempotent)
{
	ArbiProbeFact fact;
	auto probe = std::make_shared<ArbiProbe>();
	WtArbiExecuter executer(NULL, "arbi_ready_snapshot_replay", NULL);
	wtp::WtArbiExecuterTestAccess::registerUnit(executer, "SHFE.rb.2510", probe, &fact, false);
	TraderAdapter trader;
	trader._state = TraderAdapter::AS_ALLREADY;

	executer.setTrader(&trader);
	EXPECT_FALSE(wtp::WtArbiExecuterTestAccess::channelReady(executer));
	EXPECT_TRUE(probe->channelSnapshot().empty());
	// Model the adapter's initial READY event, delivered after sink registration.
	executer.on_channel_ready();
	executer.on_channel_ready();

	const std::vector<std::string> events = probe->channelSnapshot();
	ASSERT_EQ(events.size(), 1u);
	EXPECT_EQ(events[0], "ready");
}

TEST(WtArbiExecuterSerialization, QueuedReadyRecoveryIsDiscardedAfterChannelEpochChanges)
{
	ArbiProbeFact fact;
	auto probe = std::make_shared<ArbiProbe>();
	WtArbiExecuter executer(NULL, "arbi_stale_recovery", NULL);
	wtp::WtArbiExecuterTestAccess::registerUnit(executer, "SHFE.rb.2510", probe, &fact);
	probe->blockNextPosition();

	executer.on_position_changed("SHFE.rb.2510", 1.0);
	const bool positionEntered = probe->waitPositionEntered(std::chrono::seconds(2));
	if (!positionEntered)
		probe->releasePosition();
	ASSERT_TRUE(positionEntered);
	executer.on_channel_ready();
	executer.on_channel_lost();
	probe->releasePosition();

	ASSERT_TRUE(probe->waitChannelCount(1, std::chrono::seconds(2)));
	const std::vector<std::string> events = probe->channelSnapshot();
	ASSERT_EQ(events.size(), 1u);
	EXPECT_EQ(events[0], "lost");
}

TEST(WtArbiExecuterSerialization, LostEventRemainsObservableAcrossReadyLostReadyBurst)
{
	ArbiProbeFact fact;
	auto probe = std::make_shared<ArbiProbe>();
	WtArbiExecuter executer(NULL, "arbi_ready_lost_ready", NULL);
	wtp::WtArbiExecuterTestAccess::registerUnit(executer, "SHFE.rb.2510", probe, &fact);
	probe->blockNextPosition();

	executer.on_position_changed("SHFE.rb.2510", 1.0);
	const bool positionEntered = probe->waitPositionEntered(std::chrono::seconds(2));
	if (!positionEntered)
		probe->releasePosition();
	ASSERT_TRUE(positionEntered);
	executer.on_channel_ready();
	executer.on_channel_lost();
	executer.on_channel_ready();
	probe->releasePosition();

	ASSERT_TRUE(probe->waitChannelCount(2, std::chrono::seconds(2)));
	const std::vector<std::string> events = probe->channelSnapshot();
	ASSERT_EQ(events.size(), 2u);
	EXPECT_EQ(events[0], "lost");
	EXPECT_EQ(events[1], "ready");
}

TEST(WtArbiExecuterSerialization, ReentrantPositionCallbackQueuesWithoutDeadlock)
{
	ArbiProbeFact fact;
	auto probe = std::make_shared<ArbiProbe>();
	WtArbiExecuter executer(NULL, "arbi_position_reentry", NULL);
	wtp::WtArbiExecuterTestAccess::registerUnit(executer, "SHFE.rb.2510", probe, &fact, false);
	probe->position_hook = [&executer]() {
		executer.on_position_changed("SHFE.rb.2510", 1.0);
	};

	executer.on_position_changed("SHFE.rb.2510", 1.0);
	EXPECT_EQ(probe->positionSnapshot().size(), 2u);
	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, "SHFE.rb.2510"), 2.0);
	EXPECT_EQ(probe->max_active.load(), 1);
}

TEST(WtArbiExecuterSerialization, NoPoolBusyCallerReturnsAfterEnqueue)
{
	ArbiProbeFact fact;
	auto probe = std::make_shared<ArbiProbe>();
	WtArbiExecuter executer(NULL, "arbi_no_pool_sync", NULL);
	wtp::WtArbiExecuterTestAccess::registerUnit(executer, "SHFE.rb.2510", probe, &fact, false);
	probe->blockNextPosition();

	std::thread first([&]() { executer.on_position_changed("SHFE.rb.2510", 1.0); });
	const bool firstEntered = probe->waitPositionEntered(std::chrono::seconds(2));
	if (!firstEntered)
	{
		probe->releasePosition();
		first.join();
		ASSERT_TRUE(firstEntered);
	}

	std::atomic<bool> secondReturned{ false };
	std::thread second([&]() {
		executer.on_position_changed("SHFE.rb.2510", 2.0);
		secondReturned.store(true);
	});
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while ((wtp::WtArbiExecuterTestAccess::target(executer, "SHFE.rb.2510") != 3.0 ||
		wtp::WtArbiExecuterTestAccess::queuedEvents(executer, "SHFE.rb.2510") == 0 ||
		!secondReturned.load()) && std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	const bool secondQueued = wtp::WtArbiExecuterTestAccess::target(executer, "SHFE.rb.2510") == 3.0 &&
		wtp::WtArbiExecuterTestAccess::queuedEvents(executer, "SHFE.rb.2510") > 0;
	EXPECT_TRUE(secondQueued);
	EXPECT_TRUE(secondReturned.load());
	probe->releasePosition();
	first.join();
	second.join();
	EXPECT_TRUE(secondReturned.load());
	EXPECT_EQ(probe->positionSnapshot().size(), 2u);
}

TEST(WtArbiExecuterSerialization, PublicSetPositionQueuesTargetBeforeJoinedTickDuringInitialization)
{
	ArbiProbeFact fact;
	auto probe = std::make_shared<ArbiProbe>();
	WtArbiExecuter executer(NULL, "arbi_public_target_before_tick", NULL);
	const std::string code = "SHFE.rb.2510";
	wtp::WtArbiExecuterTestAccess::registerUnit(executer, code, probe, &fact, false);
	wtp::WtArbiExecuterTestAccess::makeInitializing(executer, code);
	std::shared_ptr<WTSTickData> tick(WTSTickData::create(code.c_str()),
		[](WTSTickData* value) { if (value) value->release(); });
	ASSERT_NE(tick.get(), nullptr);
	struct JoinState
	{
		std::atomic<bool> helper_done{ false };
		std::atomic<bool> joined_inside_callback{ false };
	};
	auto joinState = std::make_shared<JoinState>();
	probe->position_hook = [&executer, tick, joinState, code]() {
		std::thread helper([&executer, tick, joinState, code]() {
			executer.on_tick(code.c_str(), tick.get());
			joinState->helper_done.store(true);
		});
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (!joinState->helper_done.load() && std::chrono::steady_clock::now() < deadline)
			std::this_thread::yield();
		if (joinState->helper_done.load())
		{
			helper.join();
			joinState->joined_inside_callback.store(true);
		}
		else
			helper.detach();
	};

	wt_hashmap<std::string, double> targets;
	targets[code] = 4.0;
	executer.set_position(targets);
	wtp::WtArbiExecuterTestAccess::drainPending(executer, code);
	if (!joinState->helper_done.load())
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (!joinState->helper_done.load() && std::chrono::steady_clock::now() < deadline)
			std::this_thread::yield();
	}

	EXPECT_TRUE(joinState->joined_inside_callback.load());
	EXPECT_TRUE(joinState->helper_done.load());
	EXPECT_TRUE(probe->waitTickCount(1, std::chrono::seconds(2)));
	EXPECT_EQ((std::vector<std::string>{"position:4.000000", "tick"}), probe->eventOrderSnapshot());
	EXPECT_EQ(tick->retainCount(), 1u);
}

TEST(WtArbiExecuterSerialization, ChannelReadyCallbackCanJoinSameCodeSubmission)
{
	ArbiProbeFact fact;
	auto probe = std::make_shared<ArbiProbe>();
	WtArbiExecuter executer(NULL, "arbi_ready_callback_join", NULL);
	const std::string code = "SHFE.rb.2510";
	wtp::WtArbiExecuterTestAccess::registerUnit(executer, code, probe, &fact, false);
	struct JoinState
	{
		std::thread helper;
		std::atomic<bool> done{ false };
		std::atomic<bool> joined_inside_callback{ false };
	};
	auto joinState = std::make_shared<JoinState>();
	probe->channel_hook = [&executer, joinState, code]() {
		joinState->helper = std::thread([&executer, joinState, code]() {
			executer.on_position_changed(code.c_str(), 1.0);
			joinState->done.store(true);
		});
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (!joinState->done.load() && std::chrono::steady_clock::now() < deadline)
			std::this_thread::yield();
		if (joinState->done.load())
		{
			joinState->helper.join();
			joinState->joined_inside_callback.store(true);
		}
	};

	executer.on_channel_ready();
	if (joinState->helper.joinable())
		joinState->helper.join();
	EXPECT_TRUE(joinState->joined_inside_callback.load());
	EXPECT_EQ(probe->max_active.load(), 1);
	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, code), 1.0);
	const std::vector<double> positions = probe->positionSnapshot();
	ASSERT_EQ(positions.size(), 1u);
	EXPECT_DOUBLE_EQ(positions.front(), 1.0);
}

TEST(WtArbiExecuterSerialization, AccountUpdateDuringInitializationIsDeliveredAfterRegistration)
{
	ArbiProbeFact fact;
	auto probe = std::make_shared<ArbiProbe>();
	WtArbiExecuter executer(NULL, "arbi_pending_account", NULL);
	const std::string code = "SHFE.rb.2510";
	auto state = wtp::WtArbiExecuterTestAccess::beginPending(executer, code);

	executer.on_account("CNY", 1000, 1200, 1200, 800, 0, 0, 100, 0, 0, 0);
	EXPECT_EQ(wtp::WtArbiExecuterTestAccess::queuedEvents(state), 1u);
	wtp::WtArbiExecuterTestAccess::attachPendingUnit(executer, code, probe, &fact);
	wtp::WtArbiExecuterTestAccess::drainPending(executer, code);

	EXPECT_EQ(probe->accountSnapshot(), (std::vector<double>{1200.0}));
	EXPECT_EQ(executer.queryUnitInitStatus(code.c_str()).state, ExecUnitInitState::READY);
}

TEST(WtArbiExecuterSerialization, InitialDrainReservationRejectsCompetingReservedCaller)
{
	ArbiProbeFact fact;
	auto probe = std::make_shared<ArbiProbe>();
	WtArbiExecuter executer(NULL, "arbi_initial_drain_owner", NULL);
	const std::string code = "SHFE.rb.2510";
	auto state = wtp::WtArbiExecuterTestAccess::beginPending(executer, code);
	wtp::WtArbiExecuterTestAccess::attachPendingUnit(executer, code, probe, &fact);

	// The publishing thread owns the initial drain reservation. New callbacks
	// queue behind it, and another reserved caller must not consume the queue.
	executer.on_position_changed(code.c_str(), 1.0);
	EXPECT_TRUE(probe->positionSnapshot().empty());
	EXPECT_EQ(wtp::WtArbiExecuterTestAccess::queuedEvents(state), 1u);
	std::thread competing([&]() {
		wtp::WtArbiExecuterTestAccess::attemptReservedDrain(executer, code);
	});
	competing.join();
	EXPECT_TRUE(probe->positionSnapshot().empty());
	EXPECT_EQ(wtp::WtArbiExecuterTestAccess::queuedEvents(state), 1u);

	wtp::WtArbiExecuterTestAccess::drainPending(executer, code);
	EXPECT_EQ(probe->positionSnapshot(), (std::vector<double>{1.0}));
	EXPECT_EQ(probe->max_active.load(), 1);
}

TEST(WtArbiExecuterSerialization, SupersededQueuedTargetIsSkippedButLaterEventsRemain)
{
	ArbiProbeFact fact;
	auto probe = std::make_shared<ArbiProbe>();
	WtArbiExecuter executer(NULL, "arbi_superseded_targets", NULL);
	const std::string code = "SHFE.rb.2510";
	wtp::WtArbiExecuterTestAccess::registerUnit(executer, code, probe, &fact, false);
	std::shared_ptr<WTSTickData> tick(WTSTickData::create(code.c_str()),
		[](WTSTickData* value) { if (value) value->release(); });
	ASSERT_NE(tick.get(), nullptr);
	probe->blockNextPosition();

	std::thread first([&]() { executer.on_position_changed(code.c_str(), 1.0); });
	const bool firstEntered = probe->waitPositionEntered(std::chrono::seconds(2));
	if (!firstEntered)
	{
		probe->releasePosition();
		first.join();
		ASSERT_TRUE(firstEntered);
	}
	executer.on_position_changed(code.c_str(), 2.0);
	executer.on_tick(code.c_str(), tick.get());
	executer.on_position_changed(code.c_str(), 4.0);
	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, code), 7.0);
	EXPECT_EQ(wtp::WtArbiExecuterTestAccess::queuedEvents(executer, code), 3u);

	probe->releasePosition();
	first.join();
	EXPECT_TRUE(probe->waitTickCount(1, std::chrono::seconds(2)));
	EXPECT_EQ(probe->positionSnapshot(), (std::vector<double>{1.0, 7.0}));
	EXPECT_EQ(probe->eventOrderSnapshot(),
		(std::vector<std::string>{"position:1.000000", "tick", "position:7.000000"}));
	EXPECT_EQ(probe->max_active.load(), 1);
	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, code), 7.0);
	EXPECT_EQ(tick->retainCount(), 1u);
}

TEST(WtArbiExecuterSerialization, StartingNewBatchInvalidatesQueuedTargetBeforeApply)
{
	ArbiProbeFact fact;
	auto probe = std::make_shared<ArbiProbe>();
	WtArbiExecuter executer(NULL, "arbi_batch_start_invalidates_target", NULL);
	const std::string code = "SHFE.rb.2510";
	wtp::WtArbiExecuterTestAccess::registerUnit(executer, code, probe, &fact, false);
	probe->blockNextPosition();

	std::thread first([&]() { executer.on_position_changed(code.c_str(), 1.0); });
	const bool firstEntered = probe->waitPositionEntered(std::chrono::seconds(2));
	if (!firstEntered)
	{
		probe->releasePosition();
		first.join();
		ASSERT_TRUE(firstEntered);
	}

	wt_hashmap<std::string, double> oldBatchTargets;
	oldBatchTargets[code] = 2.0;
	const uint64_t oldBatch = wtp::WtArbiExecuterTestAccess::beginBatch(executer);
	wtp::WtArbiExecuterTestAccess::applyBatch(executer, oldBatchTargets, oldBatch);
	EXPECT_EQ(wtp::WtArbiExecuterTestAccess::queuedEvents(executer, code), 1u);

	// Pause the newer full snapshot after its sequence is published but before
	// any per-code target is applied. The old queued target must already be stale.
	const uint64_t newerBatch = wtp::WtArbiExecuterTestAccess::beginBatch(executer);
	EXPECT_GT(newerBatch, oldBatch);
	probe->releasePosition();
	first.join();
	EXPECT_EQ(probe->positionSnapshot(), (std::vector<double>{1.0}));

	wt_hashmap<std::string, double> newerBatchTargets;
	newerBatchTargets[code] = 5.0;
	wtp::WtArbiExecuterTestAccess::applyBatch(executer, newerBatchTargets, newerBatch);
	EXPECT_EQ(probe->positionSnapshot(), (std::vector<double>{1.0, 5.0}));
	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, code), 5.0);
}

TEST(WtArbiExecuterSerialization, PublicSetPositionConcurrentClearLeavesLatestTarget)
{
	ArbiProbeFact fact;
	auto probe = std::make_shared<ArbiProbe>();
	WtArbiExecuter executer(NULL, "arbi_public_position_batches", NULL);
	const std::string code = "SHFE.rb.2510";
	wtp::WtArbiExecuterTestAccess::registerUnit(executer, code, probe, &fact, false);
	probe->blockNextPosition();

	std::thread older([&]() {
		wt_hashmap<std::string, double> targets;
		targets[code] = 9.0;
		executer.set_position(targets);
	});
	const bool firstEntered = probe->waitPositionEntered(std::chrono::seconds(2));
	if (!firstEntered)
	{
		probe->releasePosition();
		older.join();
		ASSERT_TRUE(firstEntered);
	}
	wt_hashmap<std::string, double> emptyTargets;
	executer.set_position(emptyTargets);
	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, code), 0.0);
	probe->releasePosition();
	older.join();

	const std::vector<double> positions = probe->positionSnapshot();
	ASSERT_EQ(positions.size(), 2u);
	EXPECT_DOUBLE_EQ(positions[0], 9.0);
	EXPECT_DOUBLE_EQ(positions[1], 0.0);
	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, code), 0.0);
}

TEST(WtArbiExecuterSerialization, ReadyCallbackFailureDisablesUnitAndReportsStatus)
{
	ArbiProbeFact fact;
	auto probe = std::make_shared<ArbiProbe>();
	WtArbiExecuter executer(NULL, "arbi_ready_callback_failure", NULL);
	wtp::WtArbiExecuterTestAccess::registerUnit(executer, "SHFE.rb.2510", probe, &fact, false);
	probe->position_hook = [&executer]() {
		executer.on_position_changed("SHFE.rb.2510", 1.0);
	};
	probe->throwNextPosition();

	executer.on_position_changed("SHFE.rb.2510", 1.0);
	const ExecUnitInitStatus status = executer.queryUnitInitStatus("SHFE.rb.2510");
	EXPECT_EQ(status.query_result, ExecUnitInitQueryResult::OK);
	EXPECT_EQ(status.state, ExecUnitInitState::FAILED);
	EXPECT_NE(status.error.find("ArbiProbe injected"), std::string::npos);
	EXPECT_EQ(executer.clearFailedUnitInitStatus("SHFE.rb.2510"), ExecUnitInitClearResult::RETRY_BLOCKED);
	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, "SHFE.rb.2510"), 2.0);

	WTSTickData* tick = WTSTickData::create("SHFE.rb.2510");
	ASSERT_NE(tick, nullptr);
	executer.on_tick("SHFE.rb.2510", tick);
	EXPECT_EQ(tick->retainCount(), 1u);
	tick->release();
	const uint64_t failedGeneration = status.generation;
	executer.on_position_changed("SHFE.rb.2510", 1.0);
	const ExecUnitInitStatus stillFailed = executer.queryUnitInitStatus("SHFE.rb.2510");
	EXPECT_EQ(stillFailed.state, ExecUnitInitState::FAILED);
	EXPECT_EQ(stillFailed.generation, failedGeneration);
	EXPECT_EQ(probe->positionSnapshot().size(), 1u);
}

TEST(WtArbiExecuterSerialization, StartupCallbackFailureDiscardsRetainedTickAndStaysFailed)
{
	r71::R71PluginDir pluginDir;
	ASSERT_TRUE(pluginDir.ok());
	WtExecuterFactory factory;
	ASSERT_TRUE(factory.loadFactories(pluginDir.dir().c_str()));

	std::shared_ptr<WTSSessionInfo> session(WTSSessionInfo::create("TRADING", "TRADING", 0),
		[](WTSSessionInfo* value) { if (value) value->release(); });
	ASSERT_NE(session.get(), nullptr);
	session->addTradingSection(900, 1500);
	std::shared_ptr<WTSCommodityInfo> commodity(r71::makeComm("SHFE", "rb", "TRADING", TM_Both),
		[](WTSCommodityInfo* value) { if (value) value->release(); });
	ASSERT_NE(commodity.get(), nullptr);
	commodity->setSessionInfo(session.get());
	r71::R71ExecStub stub;
	stub.comm_ = commodity.get();
	stub.sess_ = session.get();

	const std::string configPath = r71::temp_root() + "r220_arbi_tick_retry.json";
	ASSERT_TRUE(r71::write_file(configPath,
		R"json({"scale":1.0,"poolsize":0,"policy":{"default":{"name":"WtExeFact.WtMinImpactExeUnit","lots":1,"minopenlots":1,"offset":0,"expire":0,"pricemode":0,"span":0,"byrate":false}}})json"));
	std::shared_ptr<WTSVariant> config(WTSCfgLoader::load_from_file(configPath.c_str()),
		[](WTSVariant* value) { if (value) value->release(); });
	ASSERT_NE(config.get(), nullptr);

	std::shared_ptr<WTSTickData> tick(WTSTickData::create("SHFE.rb.2510"),
		[](WTSTickData* value) { if (value) value->release(); });
	ASSERT_NE(tick.get(), nullptr);
	{
		R220ThrowingArbi executer(&factory, "arbi_tick_retry", NULL);
		executer.setStub(&stub);
		executer.injected_tick = tick.get();
		executer.request_quiesce_on_failure = true;
		ASSERT_TRUE(executer.init(config.get()));
		executer.on_channel_ready();

		executer.on_position_changed("SHFE.rb.2510", 1.0);
		EXPECT_EQ(tick->retainCount(), 1u);
		EXPECT_FALSE(executer.waitQuiesced());
		ExecuteUnitPtr retained = wtp::WtArbiExecuterTestAccess::stateOwner(
			executer, "SHFE.rb.2510");
		ASSERT_NE(retained.get(), nullptr);
		EXPECT_EQ(wtp::WtArbiExecuterTestAccess::mappedUnit(
			executer, "SHFE.rb.2510").get(), retained.get());
		ExecUnitInitStatus failed = executer.queryUnitInitStatus("SHFE.rb.2510");
		EXPECT_EQ(failed.state, ExecUnitInitState::FAILED);
		EXPECT_NE(failed.error.find("R220 injected"), std::string::npos);

		const uint64_t failedGeneration = failed.generation;
		EXPECT_EQ(executer.clearFailedUnitInitStatus("SHFE.rb.2510"), ExecUnitInitClearResult::RETRY_BLOCKED);
		executer.on_channel_lost();
		executer.on_position_changed("SHFE.rb.2510", 1.0);
		const ExecUnitInitStatus stillFailed = executer.queryUnitInitStatus("SHFE.rb.2510");
		EXPECT_EQ(stillFailed.state, ExecUnitInitState::FAILED);
		EXPECT_EQ(stillFailed.generation, failedGeneration);
		EXPECT_EQ(tick->retainCount(), 1u);

		// The callback error is sticky, so the executor-level operation remains
		// incomplete. Quiesce the built-in unit explicitly to make fixture teardown
		// safe while the owning executor correctly retains its context.
		retained->requestQuiesce();
		ASSERT_TRUE(retained->canWaitQuiescedOnCurrentThread());
		EXPECT_TRUE(retained->waitQuiesced());
	}
}

TEST(WtArbiExecuterSerialization, PluginInitializationFailureCanRetryAfterEnvironmentRecovers)
{
	r71::R71PluginDir pluginDir;
	ASSERT_TRUE(pluginDir.ok());
	WtExecuterFactory factory;
	ASSERT_TRUE(factory.loadFactories(pluginDir.dir().c_str()));

	std::shared_ptr<WTSSessionInfo> session(WTSSessionInfo::create("TRADING", "TRADING", 0),
		[](WTSSessionInfo* value) { if (value) value->release(); });
	ASSERT_NE(session.get(), nullptr);
	session->addTradingSection(900, 1500);
	std::shared_ptr<WTSCommodityInfo> commodity(r71::makeComm("SHFE", "rb", "TRADING", TM_Both),
		[](WTSCommodityInfo* value) { if (value) value->release(); });
	ASSERT_NE(commodity.get(), nullptr);
	commodity->setSessionInfo(session.get());
	r71::R71ExecStub stub;
	stub.comm_ = NULL;
	stub.sess_ = session.get();

	const std::string configPath = r71::temp_root() + "r220_arbi_plugin_init_fail.json";
	ASSERT_TRUE(r71::write_file(configPath,
		R"json({"scale":1.0,"poolsize":0,"policy":{"default":{"name":"WtExeFact.WtMinImpactExeUnit","lots":1,"minopenlots":1,"offset":0,"expire":0,"pricemode":0,"span":0,"byrate":false}}})json"));
	std::shared_ptr<WTSVariant> config(WTSCfgLoader::load_from_file(configPath.c_str()),
		[](WTSVariant* value) { if (value) value->release(); });
	ASSERT_NE(config.get(), nullptr);

	auto executer = std::make_shared<R220ThrowingArbi>(&factory, "arbi_plugin_init_fail", nullptr);
	executer->throw_on_first_callback = false;
	executer->setStub(&stub);
	ASSERT_TRUE(executer->init(config.get()));
	WtExecuterMgr manager;
	manager.add_executer(executer);
	executer->on_position_changed("SHFE.rb.2510", 1.0);
	const ExecUnitInitStatus failed = executer->queryUnitInitStatus("SHFE.rb.2510");
	ASSERT_EQ(failed.state, ExecUnitInitState::FAILED);

	stub.comm_ = commodity.get();
	EXPECT_EQ(executer->clearFailedUnitInitStatus("SHFE.rb.2510"), ExecUnitInitClearResult::CLEARED);

	// ABI 2.2 proves the failed initializer synchronously destroyed its unit.
	// A zero-delta trigger retries without changing the original target of +1.
	executer->on_position_changed("SHFE.rb.2510", 0.0);
	const ExecUnitInitStatus recovered = executer->queryUnitInitStatus("SHFE.rb.2510");
	EXPECT_EQ(recovered.state, ExecUnitInitState::READY);
	EXPECT_GT(recovered.generation, failed.generation);

	WtExecQuiesceCoordinator coordinator(std::vector<WtExecuterMgr*>{ &manager });
	ASSERT_TRUE(coordinator.quiesce_all());
	EXPECT_TRUE(coordinator.is_complete());
}

TEST(WtArbiExecuterSerialization, InitializedV21PluginOwnerIsRetainedUntilItsHooksQuiesce)
{
	r71::R71PluginDir pluginDir;
	ASSERT_TRUE(pluginDir.ok());
	WtExecuterFactory factory;
	ASSERT_TRUE(factory.loadFactories(pluginDir.dir().c_str()));

	std::shared_ptr<WTSSessionInfo> session(WTSSessionInfo::create("TRADING", "TRADING", 0),
		[](WTSSessionInfo* value) { if (value) value->release(); });
	ASSERT_NE(session.get(), nullptr);
	session->addTradingSection(900, 1500);
	std::shared_ptr<WTSCommodityInfo> commodity(r71::makeComm("SHFE", "rb", "TRADING", TM_Both),
		[](WTSCommodityInfo* value) { if (value) value->release(); });
	ASSERT_NE(commodity.get(), nullptr);
	commodity->setSessionInfo(session.get());
	r71::R71ExecStub stub;
	stub.comm_ = commodity.get();
	stub.sess_ = session.get();

	const std::string configPath = r71::temp_root() + "r220_arbi_v21_quiesce.json";
	ASSERT_TRUE(r71::write_file(configPath,
		R"json({"scale":1.0,"poolsize":0,"policy":{"default":{"name":"WtExeFact.WtMinImpactExeUnit","lots":1,"minopenlots":1,"offset":0,"expire":0,"pricemode":0,"span":0,"byrate":false}}})json"));
	std::shared_ptr<WTSVariant> config(WTSCfgLoader::load_from_file(configPath.c_str()),
		[](WTSVariant* value) { if (value) value->release(); });
	ASSERT_NE(config.get(), nullptr);

	R220ThrowingArbi executer(&factory, "arbi_v21_quiesce", NULL);
	executer.throw_on_first_callback = false;
	executer.setStub(&stub);
	ASSERT_TRUE(executer.init(config.get()));
	executer.on_position_changed("SHFE.rb.2510", 1.0);
	const std::string code = "SHFE.rb.2510";
	ASSERT_EQ(executer.queryUnitInitStatus(code.c_str()).state, ExecUnitInitState::READY);
	ExecuteUnitPtr owner = wtp::WtArbiExecuterTestAccess::stateOwner(executer, code);
	ASSERT_NE(owner.get(), nullptr);
	ASSERT_NE(owner->self(), nullptr);

	executer.requestQuiesce();
	// The control thread is not on a host callback/ingress stack. This says it
	// is safe to wait on this thread, not that plugin owners are quiesced.
	EXPECT_TRUE(executer.canWaitQuiescedOnCurrentThread());
	// This drains the host ingress/pool and retains the stable unit snapshot.
	// Plugin request/preflight belongs to the coordinator-wide stage, so this
	// executor wait remains false while the unit is still owned here.
	EXPECT_FALSE(executer.waitQuiesced());
	EXPECT_EQ(wtp::WtArbiExecuterTestAccess::mappedUnit(executer, code).get(), owner.get());
	EXPECT_EQ(wtp::WtArbiExecuterTestAccess::stateOwner(executer, code).get(), owner.get());
	std::vector<ExecQuiesceOwnerPtr> pluginOwners;
	ASSERT_TRUE(executer.snapshotQuiescePluginOwners(pluginOwners));
	ASSERT_EQ(pluginOwners.size(), 1u);
	EXPECT_EQ(pluginOwners.front().get(),
		std::static_pointer_cast<IExecCommandQuiesce>(owner).get());

	// Complete the ABI 2.1 unit hook with its own positive preflight so fixture
	// teardown can safely release the real initialized plugin and borrowed context.
	owner->requestQuiesce();
	ASSERT_TRUE(owner->canWaitQuiescedOnCurrentThread());
	EXPECT_TRUE(owner->waitQuiesced());
	// Completing this legacy unit directly is only fixture cleanup; it does not
	// provide the manager's global acknowledgement or authorize host teardown.
	EXPECT_FALSE(executer.waitQuiesced());
}

TEST(WtArbiExecuterSerialization, QuiesceWaitsForAcceptedFifoAndRejectsNewIngress)
{
	ArbiProbeFact fact;
	auto probe = std::make_shared<ArbiProbe>();
	WtArbiExecuter executer(NULL, "arbi_quiesce_fifo", NULL);
	wtp::WtArbiExecuterTestAccess::registerUnit(executer, "SHFE.rb.2510", probe, &fact);
	probe->position_hook = [&executer]() {
		// This nested producer belongs to the already accepted callback and must
		// finish before stop is considered drained.
		executer.on_account("CNY", 1000, 1200, 1200, 800, 0, 0, 100, 0, 0, 0);
	};
	probe->blockNextPosition();

	executer.on_position_changed("SHFE.rb.2510", 1.0);
	const bool entered = probe->waitPositionEntered(std::chrono::seconds(2));
	if (!entered)
		probe->releasePosition();
	ASSERT_TRUE(entered);

	// This second update is accepted before stop, but remains behind the gated
	// callback in the per-code FIFO queue.
	executer.on_position_changed("SHFE.rb.2510", 1.0);
	executer.requestQuiesce();
	EXPECT_FALSE(executer.completeQuiesceAfterGlobalPluginWait());
	executer.on_position_changed("SHFE.rb.2510", 1.0);
	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, "SHFE.rb.2510"), 2.0);

	std::atomic<bool> waiterStarted{ false };
	std::atomic<bool> waiterDone{ false };
	bool waitResult = false;
	std::thread waiter([&]() {
		waiterStarted = true;
		waitResult = executer.waitQuiesced();
		waiterDone = true;
	});
	while (!waiterStarted.load())
		std::this_thread::yield();
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	const bool returnedBeforeRelease = waiterDone.load();
	probe->releasePosition();
	waiter.join();

	EXPECT_FALSE(returnedBeforeRelease);
	// This test unit is an unversioned in-process test double, so a successful
	// host drain must not be reported as verified plugin quiescence.
	EXPECT_FALSE(waitResult);
	const std::vector<double> positions = probe->positionSnapshot();
	ASSERT_EQ(positions.size(), 2u);
	EXPECT_DOUBLE_EQ(positions[0], 1.0);
	EXPECT_DOUBLE_EQ(positions[1], 2.0);
	EXPECT_EQ(probe->accountSnapshot(), (std::vector<double>{1200.0}));
	EXPECT_TRUE(executer.canWaitQuiescedOnCurrentThread());
	EXPECT_FALSE(executer.waitQuiesced());
	EXPECT_EQ(wtp::WtArbiExecuterTestAccess::mappedUnit(
		executer, "SHFE.rb.2510").get(),
		wtp::WtArbiExecuterTestAccess::stateOwner(
			executer, "SHFE.rb.2510").get());
}

TEST(WtArbiExecuterSerialization, UnsupportedPluginBackgroundThreadKeepsOwnerPinned)
{
	ArbiProbeFact fact;
	auto probe = std::make_shared<ArbiProbe>();
	WtArbiExecuter executer(NULL, "arbi_quiesce_plugin_thread", NULL);
	R220BackgroundArbiUnit* unit =
		wtp::WtArbiExecuterTestAccess::registerBackgroundUnit(
			executer, "SHFE.rb.2510", probe, &fact);
	ASSERT_NE(unit, nullptr);
	ASSERT_TRUE(unit->waitBackgroundEntered(std::chrono::seconds(2)));

	executer.requestQuiesce();
	// The test-only in-process factory has no WtExecuterFactory identity. The
	// host preflight only checks this caller's stack; the retained legacy unit
	// still prevents overall waiting from reporting completion.
	EXPECT_EQ(executer.quiesceFactory(), nullptr);
	EXPECT_TRUE(executer.preflightQuiesceOnCurrentThread());
	EXPECT_TRUE(executer.drainHostQuiesced());
	std::vector<ExecQuiesceOwnerPtr> owners;
	ASSERT_TRUE(executer.snapshotQuiescePluginOwners(owners));
	ASSERT_EQ(owners.size(), 1u);
	EXPECT_EQ(owners.front().get(), std::static_pointer_cast<IExecCommandQuiesce>(
		wtp::WtArbiExecuterTestAccess::stateOwner(executer, "SHFE.rb.2510")).get());
	EXPECT_FALSE(executer.waitQuiesced());
	EXPECT_FALSE(unit->backgroundDone());
	EXPECT_EQ(wtp::WtArbiExecuterTestAccess::mappedUnit(
		executer, "SHFE.rb.2510").get(),
		wtp::WtArbiExecuterTestAccess::stateOwner(
			executer, "SHFE.rb.2510").get());

	unit->releaseBackground();
	EXPECT_TRUE(unit->waitBackgroundDone(std::chrono::seconds(2)));
}

TEST(WtArbiExecuterSerialization, QuiesceFailsWhenCallbackDropsAcceptedQueuedEvent)
{
	ArbiProbeFact fact;
	auto probe = std::make_shared<ArbiProbe>();
	WtArbiExecuter executer(NULL, "arbi_quiesce_callback_failure", NULL);
	wtp::WtArbiExecuterTestAccess::registerUnit(executer, "SHFE.rb.2510", probe, &fact);
	std::atomic<bool> workerCanWait{ true };
	std::atomic<bool> workerWaitResult{ true };
	probe->position_hook = [&]() {
		workerCanWait = executer.canWaitQuiescedOnCurrentThread();
		workerWaitResult = executer.waitQuiesced();
	};
	probe->blockNextPosition();
	probe->throwNextPosition();

	executer.on_position_changed("SHFE.rb.2510", 1.0);
	const bool entered = probe->waitPositionEntered(std::chrono::seconds(2));
	if (!entered)
	{
		probe->releasePosition();
		ASSERT_TRUE(entered);
	}

	// Queue a second accepted callback behind the one that will fail.
	executer.on_position_changed("SHFE.rb.2510", 1.0);
	executer.requestQuiesce();
	executer.on_position_changed("SHFE.rb.2510", 1.0);
	EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer, "SHFE.rb.2510"), 2.0);

	probe->releasePosition();
	EXPECT_FALSE(executer.waitQuiesced());
	EXPECT_FALSE(executer.waitQuiesced());
	EXPECT_FALSE(workerCanWait.load());
	EXPECT_FALSE(workerWaitResult.load());
	std::vector<ExecQuiesceOwnerPtr> failedOwners;
	ASSERT_TRUE(executer.snapshotQuiescePluginOwners(failedOwners));
	ASSERT_EQ(failedOwners.size(), 1u);
	EXPECT_FALSE(executer.completeQuiesceAfterGlobalPluginWait());
	const ExecUnitInitStatus failed = executer.queryUnitInitStatus("SHFE.rb.2510");
	EXPECT_EQ(failed.state, ExecUnitInitState::FAILED);
	EXPECT_NE(failed.error.find("ArbiProbe injected"), std::string::npos);
	const std::vector<double> positions = probe->positionSnapshot();
	ASSERT_EQ(positions.size(), 1u);
	EXPECT_DOUBLE_EQ(positions[0], 1.0);
	EXPECT_EQ(wtp::WtArbiExecuterTestAccess::mappedUnit(executer, "SHFE.rb.2510").get(),
		wtp::WtArbiExecuterTestAccess::stateOwner(executer, "SHFE.rb.2510").get());
}

TEST(WtArbiExecuterSerialization, SynchronousFallbackCallbackCannotWaitForItself)
{
	ArbiProbeFact fact;
	auto probe = std::make_shared<ArbiProbe>();
	WtArbiExecuter executer(NULL, "arbi_quiesce_sync_fallback", NULL);
	wtp::WtArbiExecuterTestAccess::registerUnit(executer, "SHFE.rb.2510", probe, &fact, false);
	probe->blockNextPosition();
	bool callbackCanWait = true;
	bool callbackWaitResult = true;
	probe->position_hook = [&]() {
		callbackCanWait = executer.canWaitQuiescedOnCurrentThread();
		callbackWaitResult = executer.waitQuiesced();
	};

	// Without a pool, drain dispatch is synchronous on this ingress stack. Hold
	// the callback while a control thread waits to prove ingress itself is
	// counted, then check callback re-entry refuses to wait for its own drain.
	std::thread producer([&]() { executer.on_position_changed("SHFE.rb.2510", 1.0); });
	const bool entered = probe->waitPositionEntered(std::chrono::seconds(2));
	if (!entered)
	{
		probe->releasePosition();
		producer.join();
		FAIL() << "synchronous callback did not reach its gate";
		return;
	}
	executer.requestQuiesce();
	std::atomic<bool> waiterStarted{ false };
	std::atomic<bool> waiterDone{ false };
	bool waitResult = false;
	std::thread waiter([&]() {
		waiterStarted = true;
		waitResult = executer.waitQuiesced();
		waiterDone = true;
	});
	while (!waiterStarted.load())
		std::this_thread::yield();
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	const bool returnedBeforeRelease = waiterDone.load();
	probe->releasePosition();
	producer.join();
	waiter.join();

	EXPECT_FALSE(callbackCanWait);
	EXPECT_FALSE(callbackWaitResult);
	EXPECT_FALSE(returnedBeforeRelease);
	EXPECT_FALSE(waitResult);
	EXPECT_TRUE(executer.canWaitQuiescedOnCurrentThread());
	EXPECT_FALSE(executer.waitQuiesced());
	EXPECT_EQ(wtp::WtArbiExecuterTestAccess::mappedUnit(
		executer, "SHFE.rb.2510").get(),
		wtp::WtArbiExecuterTestAccess::stateOwner(
			executer, "SHFE.rb.2510").get());
	EXPECT_EQ(probe->positionSnapshot().size(), 1u);
}

TEST(WtArbiExecuterSerialization, EmptyExecutorCanCompleteHostQuiescence)
{
	WtArbiExecuter executer(NULL, "arbi_quiesce_empty", NULL);
	executer.requestQuiesce();
	EXPECT_TRUE(executer.canWaitQuiescedOnCurrentThread());
	EXPECT_TRUE(executer.drainHostQuiesced());
	std::vector<ExecQuiesceOwnerPtr> owners;
	EXPECT_TRUE(executer.snapshotQuiescePluginOwners(owners));
	EXPECT_TRUE(owners.empty());
	EXPECT_TRUE(executer.completeQuiesceAfterGlobalPluginWait());
	EXPECT_TRUE(executer.waitQuiesced());
}

TEST(WtArbiExecuterSerialization, PreflightAllowsOtherThreadIngressAndDrainWaits)
{
	WtArbiExecuter executer(NULL, "arbi_quiesce_ingress_preflight", NULL);
	std::mutex gateMutex;
	std::condition_variable gateCondition;
	bool ingressReady = false;
	bool releaseIngress = false;
	bool ingressAccepted = false;
	std::thread producer([&]() {
		auto ingress = wtp::WtArbiExecuterTestAccess::holdIngress(executer);
		ingressAccepted = static_cast<bool>(*ingress);
		std::unique_lock<std::mutex> lock(gateMutex);
		ingressReady = true;
		gateCondition.notify_all();
		gateCondition.wait(lock, [&]() { return releaseIngress; });
	});
	{
		std::unique_lock<std::mutex> lock(gateMutex);
		gateCondition.wait(lock, [&]() { return ingressReady; });
	}
	executer.requestQuiesce();
	EXPECT_TRUE(ingressAccepted);
	EXPECT_TRUE(executer.canWaitQuiescedOnCurrentThread());
	std::atomic<bool> waiterStarted{ false };
	std::atomic<bool> waiterDone{ false };
	bool waitResult = false;
	std::thread waiter([&]() {
		waiterStarted = true;
		waitResult = executer.waitQuiesced();
		waiterDone = true;
	});
	while (!waiterStarted.load())
		std::this_thread::yield();
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	const bool returnedBeforeIngressExit = waiterDone.load();
	{
		std::lock_guard<std::mutex> lock(gateMutex);
		releaseIngress = true;
	}
	gateCondition.notify_all();
	producer.join();
	waiter.join();
	EXPECT_FALSE(returnedBeforeIngressExit);
	EXPECT_TRUE(waitResult);
	EXPECT_TRUE(executer.canWaitQuiescedOnCurrentThread());
	EXPECT_TRUE(executer.waitQuiesced());
}
