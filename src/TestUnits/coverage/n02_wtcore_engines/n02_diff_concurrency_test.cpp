/**
 * Deterministic concurrency checks for WtDiffExecuter per-code callback queues.
 * Fake units are injected through the friend harness so these tests do not load
 * external execution plugins or connect to a trading gateway.
 */
#include <gtest/gtest.h>

#include <TestUnits/coverage/n01_wtcore_tradepaths/r71_wtcore_env.h>

#include <WtCore/WtDiffExecuter.h>
#include <WtCore/WtExecuterFactory.h>
#include <WtCore/WtExecuterSchedule.h>
#include <WtCore/WtHelper.h>
#include <WTSUtils/WTSCfgLoader.h>
#include <Includes/ExecuteDefs.h>
#include <Includes/ILogHandler.h>
#include <rapidjson/document.h>
#include <WTSTools/WTSLogger.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace wtp;
namespace {
namespace fs = std::filesystem;
namespace rj = rapidjson;

class Gate
{
public:
    void arriveAndWait()
    {
        std::unique_lock<std::mutex> lock(_mutex);
        ++_arrivals;
        _condition.notify_all();
        _condition.wait(lock, [this]() { return _released; });
    }

    bool waitForArrivals(unsigned count)
    {
        return waitForArrivals(count, std::chrono::seconds(3));
    }

    bool waitForArrivals(unsigned count, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(_mutex);
        return _condition.wait_for(lock, timeout, [this, count]() {
            return _arrivals >= count;
        });
    }

    void release()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _released = true;
        _condition.notify_all();
    }

private:
    std::mutex _mutex;
    std::condition_variable _condition;
    unsigned _arrivals = 0;
    bool _released = false;
};

class DiffProbeUnit : public ExecuteUnit
{
public:
    DiffProbeUnit() : ExecuteUnit(true) {}

    const char* getName() override { return "DiffProbeUnit"; }
    const char* getFactName() override { return "DiffProbeFactory"; }

    void set_position(const char*, double value) override
    {
        std::shared_ptr<Gate> gate;
        std::function<void(double)> hook;
        bool throwAfterPosition = false;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            ++_active;
            _max_active = std::max(_max_active, _active);
            _positions.push_back(value);
            _events.push_back("position");
            if (_positions.size() == 1)
                gate = _position_gate;
            hook = _position_hook;
            throwAfterPosition = _throw_on_position;
        }
        if (gate)
            gate->arriveAndWait();
        if (hook)
            hook(value);
        {
            std::lock_guard<std::mutex> lock(_mutex);
            --_active;
            _condition.notify_all();
        }
        if (throwAfterPosition)
            throw std::runtime_error("position callback failed");
    }

    void on_tick(WTSTickData*) override { record("tick"); }
    void on_trade(uint32_t, const char*, bool, double, double) override
    {
        record("trade");
        std::lock_guard<std::mutex> lock(_mutex);
        if (_throw_on_trade)
            throw std::runtime_error("trade callback failed");
    }
    void on_order(uint32_t, const char*, bool, double, double, bool) override { record("order"); }
    void on_entrust(uint32_t, const char*, bool, const char*) override { record("entrust"); }
    void on_channel_ready() override
    {
        record("ready");
        std::lock_guard<std::mutex> lock(_mutex);
        if (_throw_on_ready)
            throw std::runtime_error("ready callback failed");
    }
    void on_channel_lost() override { record("lost"); }
    void on_account(const char*, double, double, double, double, double,
        double, double, double, double, double) override { record("account"); }

    void setPositionGate(const std::shared_ptr<Gate>& gate)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _position_gate = gate;
    }

    void setPositionHook(std::function<void(double)> hook)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _position_hook = std::move(hook);
    }

    void setThrowOnReady(bool enabled)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _throw_on_ready = enabled;
    }

    void setThrowOnTrade(bool enabled)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _throw_on_trade = enabled;
    }

    void setThrowOnPosition(bool enabled)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _throw_on_position = enabled;
    }

    std::vector<double> positions() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _positions;
    }

    std::vector<std::string> events() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _events;
    }

    int maxActive() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _max_active;
    }

private:
    void record(const char* name)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _events.emplace_back(name);
    }

    mutable std::mutex _mutex;
    std::condition_variable _condition;
    std::shared_ptr<Gate> _position_gate;
    std::function<void(double)> _position_hook;
    std::vector<double> _positions;
    std::vector<std::string> _events;
    int _active = 0;
    int _max_active = 0;
    bool _throw_on_ready = false;
    bool _throw_on_trade = false;
    bool _throw_on_position = false;
};

class DiffProbeFactory : public IExecuterFact
{
public:
    const char* getName() override { return "DiffProbeFactory"; }
    void enumExeUnit(FuncEnumUnitCallback) override {}
    ExecuteUnit* createExeUnit(const char*) override { return new DiffProbeUnit(); }
    ExecuteUnit* createDiffExeUnit(const char*) override { return new DiffProbeUnit(); }
    ExecuteUnit* createArbiExeUnit(const char*) override { return new DiffProbeUnit(); }
    bool deleteExeUnit(ExecuteUnit* unit) override { delete unit; return true; }
};

class DiffPluginQuiesceProbe
{
public:
    ~DiffPluginQuiesceProbe() { releaseBackground(); joinBackground(); }

    void request()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        ++_requests;
        _condition.notify_all();
    }

    bool canWaitOnCurrentThread() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _requests != 0 && _worker_id != std::this_thread::get_id() && _can_wait;
    }

    bool wait()
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            ++_waits;
            _wait_entered = true;
            _condition.notify_all();
            if (_quiesced)
                return true;
        }
        joinBackground();
        std::lock_guard<std::mutex> lock(_mutex);
        _quiesced = _wait_result;
        return _quiesced;
    }

    bool startBackground(std::function<bool()> preflight)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_worker.joinable())
            return false;
        _worker_started = false;
        _preflight_requested = false;
        _worker_released = false;
        _worker = std::thread([this, preflight = std::move(preflight)]() {
            {
                std::lock_guard<std::mutex> workerLock(_mutex);
                _worker_id = std::this_thread::get_id();
                _worker_started = true;
                _condition.notify_all();
            }
            bool runPreflight = false;
            {
                std::unique_lock<std::mutex> workerLock(_mutex);
                _condition.wait(workerLock, [this]() {
                    return _preflight_requested || _worker_released;
                });
                runPreflight = _preflight_requested;
            }
            if (runPreflight)
            {
                const bool result = preflight();
                std::lock_guard<std::mutex> workerLock(_mutex);
                _worker_preflight_result = result;
                _worker_preflight_done = true;
                _condition.notify_all();
            }
            std::unique_lock<std::mutex> workerLock(_mutex);
            _condition.wait(workerLock, [this]() { return _worker_released; });
            _worker_id = std::thread::id();
        });
        return true;
    }

    bool waitForWorkerStarted(std::chrono::milliseconds timeout = std::chrono::seconds(3))
    {
        std::unique_lock<std::mutex> lock(_mutex);
        return _condition.wait_for(lock, timeout, [this]() { return _worker_started; });
    }

    void triggerWorkerPreflight()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _preflight_requested = true;
        _condition.notify_all();
    }

    bool waitForWorkerPreflight(std::chrono::milliseconds timeout = std::chrono::seconds(3))
    {
        std::unique_lock<std::mutex> lock(_mutex);
        return _condition.wait_for(lock, timeout, [this]() { return _worker_preflight_done; });
    }

    bool workerPreflightResult() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _worker_preflight_result;
    }

    bool waitForWaitEntered(std::chrono::milliseconds timeout = std::chrono::seconds(3))
    {
        std::unique_lock<std::mutex> lock(_mutex);
        return _condition.wait_for(lock, timeout, [this]() { return _wait_entered; });
    }

    void releaseBackground()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _worker_released = true;
        _condition.notify_all();
    }

    void setCanWait(bool value)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _can_wait = value;
    }

    void setWaitResult(bool value)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _wait_result = value;
    }

    int requestCount() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _requests;
    }

    int waitCount() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _waits;
    }

private:
    void joinBackground()
    {
        std::thread worker;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (!_worker.joinable() || _worker.get_id() == std::this_thread::get_id())
                return;
            worker = std::move(_worker);
        }
        worker.join();
    }

    mutable std::mutex _mutex;
    std::condition_variable _condition;
    std::thread _worker;
    std::thread::id _worker_id;
    int _requests = 0;
    int _waits = 0;
    bool _can_wait = true;
    bool _wait_result = true;
    bool _quiesced = false;
    bool _wait_entered = false;
    bool _worker_started = false;
    bool _preflight_requested = false;
    bool _worker_preflight_done = false;
    bool _worker_preflight_result = true;
    bool _worker_released = false;
};

class DiffProbeQuiesceWrapper : public ExeUnitWrapper
{
public:
    DiffProbeQuiesceWrapper(ExecuteUnit* unit, IExecuterFact* fact,
        const std::shared_ptr<DiffPluginQuiesceProbe>& probe)
        : ExeUnitWrapper(unit, fact), _probe(probe) {}

    void requestQuiesce() override { _probe->request(); }
    bool canWaitQuiescedOnCurrentThread() const override
    {
        return _probe->canWaitOnCurrentThread();
    }
    bool waitQuiesced() override { return _probe->wait(); }
    const std::shared_ptr<DiffPluginQuiesceProbe>& probe() const { return _probe; }

private:
    std::shared_ptr<DiffPluginQuiesceProbe> _probe;
};

std::string uniqueName(const char* suffix)
{
#ifdef _WIN32
    const int processId = _getpid();
#else
    const int processId = getpid();
#endif
    return std::string("n02_diff_concurrency_") + std::to_string(processId) + "_" + suffix;
}

bool readSavedPosition(const std::string& path, const char* section,
    const std::string& code, const char* field, double& value)
{
    std::ifstream input(path);
    if (!input.is_open())
        return false;
    const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    rj::Document root;
    root.Parse(content.c_str());
    if (root.HasParseError() || !root.IsObject() || !root.HasMember(section) ||
        !root[section].IsArray())
        return false;
    for (const rj::Value& item : root[section].GetArray())
    {
        if (!item.IsObject() || !item.HasMember("code") || !item["code"].IsString() ||
            code != item["code"].GetString() || !item.HasMember(field) || !item[field].IsNumber())
            continue;
        value = item[field].GetDouble();
        return true;
    }
    return false;
}

class DiffErrorLogCapture : public ILogHandler
{
public:
    DiffErrorLogCapture()
    {
        static const char* loggerConfig = R"({
            "root":{"level":"info","async":false,"sinks":[]},
            "dyn_pattern":{"executer":{"level":"info","async":false,"sinks":[]}}
        })";
        WTSLogger::init(loggerConfig, false, NULL);
        WTSLogger::registerHandler(this);
    }

    ~DiffErrorLogCapture() { WTSLogger::registerHandler(NULL); }

    void handleLogAppend(WTSLogLevel level, const char* message) override
    {
        if (level != LL_ERROR)
            return;
        std::lock_guard<std::mutex> lock(_mutex);
        _messages.emplace_back(message ? message : "");
    }

    std::vector<std::string> messages() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _messages;
    }

private:
    mutable std::mutex _mutex;
    std::vector<std::string> _messages;
};

} // namespace

namespace wtp
{
class WtDiffExecuterConcurrencyTest
{
public:
    static DiffProbeUnit* registerUnit(WtDiffExecuter& executor, DiffProbeFactory& fact,
        const std::string& code, bool retainAsPluginOwner = false)
    {
        std::shared_ptr<DiffPluginQuiesceProbe> quiesceProbe =
            std::make_shared<DiffPluginQuiesceProbe>();
        ExecuteUnitPtr unit(new DiffProbeQuiesceWrapper(new DiffProbeUnit(), &fact,
            quiesceProbe));
        std::shared_ptr<WtDiffExecuter::UnitLifecycleState> state =
            std::make_shared<WtDiffExecuter::UnitLifecycleState>();
        state->creation_complete = true;
        state->std_code = code;
        state->init_status = ExecUnitInitState::READY;
        state->init_generation = 1;
        state->result = unit;
        {
            SpinLock lock(executor._mtx_units);
            executor._unit_map[code] = unit;
            executor._unit_states[code] = state;
        }
        if (retainAsPluginOwner)
            executor.retainQuiesceUnit(unit);
        return static_cast<DiffProbeUnit*>(unit->self());
    }

    static std::shared_ptr<DiffPluginQuiesceProbe> quiesceProbe(
        WtDiffExecuter& executor, const std::string& code)
    {
        ExecuteUnitPtr unit = unitOwner(executor, code);
        auto* wrapper = dynamic_cast<DiffProbeQuiesceWrapper*>(unit.get());
        return wrapper == nullptr ? std::shared_ptr<DiffPluginQuiesceProbe>() :
            wrapper->probe();
    }

    static size_t quiesceOwnerCount(WtDiffExecuter& executor)
    {
        SpinLock lock(executor._mtx_units);
        return executor._quiesce_unit_owners.size();
    }

    static void forgetQuiesceOwnerAfterDirectTestShutdown(WtDiffExecuter& executor,
        const ExecuteUnitPtr& unit)
    {
        SpinLock lock(executor._mtx_units);
        auto& owners = executor._quiesce_unit_owners;
        owners.erase(std::remove(owners.begin(), owners.end(), unit), owners.end());
    }

    static void configurePool(WtDiffExecuter& executor, uint32_t size)
    {
        executor._pool.reset(new boost::threadpool::pool(size));
    }

    static void setDiff(WtDiffExecuter& executor, const std::string& code, double value)
    {
        std::lock_guard<std::mutex> lock(executor._position_mutex);
        executor._diff_pos[code] = value;
        ++executor._position_revision;
    }

    static ExecuteUnitPtr unitOwner(WtDiffExecuter& executor, const std::string& code)
    {
        SpinLock lock(executor._mtx_units);
        auto it = executor._unit_map.find(code);
        return it == executor._unit_map.end() ? ExecuteUnitPtr() : it->second;
    }

    static bool hasUnit(WtDiffExecuter& executor, const std::string& code)
    {
        SpinLock lock(executor._mtx_units);
        return executor._unit_map.find(code) != executor._unit_map.end();
    }

    static ExecuteUnitPtr requestUnit(WtDiffExecuter& executor, const std::string& code)
    {
        return executor.getUnit(code.c_str(), true);
    }

    static void setCreationIncomplete(WtDiffExecuter& executor, const std::string& code)
    {
        std::shared_ptr<WtDiffExecuter::UnitLifecycleState> state;
        {
            SpinLock lock(executor._mtx_units);
            state = executor._unit_states.at(code);
        }
        std::lock_guard<std::mutex> lock(state->mutex);
        state->creation_complete = false;
        state->creation_owner = std::this_thread::get_id();
        state->init_status = ExecUnitInitState::INITIALIZING;
        state->result.reset();
    }

    static void drainCallbacks(WtDiffExecuter& executor, const std::string& code)
    {
        ExecuteUnitPtr unit;
        std::shared_ptr<WtDiffExecuter::UnitLifecycleState> state;
        {
            SpinLock lock(executor._mtx_units);
            unit = executor._unit_map.at(code);
            state = executor._unit_states.at(code);
        }
        executor.drainUnitChannelEvents(unit, state);
    }

    static bool retryBlocked(WtDiffExecuter& executor, const std::string& code)
    {
        std::shared_ptr<WtDiffExecuter::UnitLifecycleState> state;
        {
            SpinLock lock(executor._mtx_units);
            state = executor._unit_states.at(code);
        }
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->retry_blocked;
    }

    static void setSnapshot(WtDiffExecuter& executor, const std::string& code,
        double target, double diff)
    {
        std::lock_guard<std::mutex> lock(executor._position_mutex);
        executor._target_pos[code] = target;
        executor._target_versions[code] = ++executor._target_update_generation;
        executor._diff_pos[code] = diff;
        ++executor._position_revision;
    }

    static uint64_t savedRevision(WtDiffExecuter& executor)
    {
        std::lock_guard<std::mutex> lock(executor._save_mutex);
        return executor._saved_position_revision;
    }

    static uint64_t positionRevision(WtDiffExecuter& executor)
    {
        std::lock_guard<std::mutex> lock(executor._position_mutex);
        return executor._position_revision;
    }

    static bool save(WtDiffExecuter& executor)
    {
        return executor.save_data();
    }

    static void setSaveHooks(WtDiffExecuter& executor,
        std::function<bool(bool)> beforeWrite, std::function<void()> afterCommit,
        std::function<void()> failureMutation = {})
    {
        executor._save_test_hook = std::move(beforeWrite);
        executor._save_after_commit_test_hook = std::move(afterCommit);
        executor._failure_mutation_test_hook = std::move(failureMutation);
    }

    static void setOmittedSweepHook(WtDiffExecuter& executor,
        std::function<void(const std::string&)> hook)
    {
        executor._omitted_sweep_test_hook = std::move(hook);
    }

    static void setUnitLookupHook(WtDiffExecuter& executor,
        std::function<void(const std::string&)> hook)
    {
        executor._unit_lookup_test_hook = std::move(hook);
    }

    static bool quiesceIncomplete(const WtDiffExecuter& executor)
    {
        return executor._quiesce_incomplete.load(std::memory_order_acquire);
    }

    static void markFailed(WtDiffExecuter& executor, const std::string& code,
        const std::string& reason)
    {
        std::shared_ptr<WtDiffExecuter::UnitLifecycleState> state;
        {
            SpinLock lock(executor._mtx_units);
            auto it = executor._unit_states.find(code);
            if (it != executor._unit_states.end())
                state = it->second;
        }
        executor.markUnitFailed(code, state, reason);
    }

    static void load(WtDiffExecuter& executor)
    {
        executor.load_data();
    }

    static double target(WtDiffExecuter& executor, const std::string& code)
    {
        std::lock_guard<std::mutex> lock(executor._position_mutex);
        auto it = executor._target_pos.find(code);
        return it == executor._target_pos.end() ? 0.0 : it->second;
    }

    static double diff(WtDiffExecuter& executor, const std::string& code)
    {
        std::lock_guard<std::mutex> lock(executor._position_mutex);
        auto it = executor._diff_pos.find(code);
        return it == executor._diff_pos.end() ? 0.0 : it->second;
    }

    static void enqueueBlocker(WtDiffExecuter& executor, const std::string& code,
        const std::shared_ptr<Gate>& gate)
    {
        ExecuteUnitPtr unit;
        std::shared_ptr<WtDiffExecuter::UnitLifecycleState> state;
        {
            SpinLock lock(executor._mtx_units);
            unit = executor._unit_map.at(code);
            state = executor._unit_states.at(code);
        }
        const bool schedule = executor.enqueueUnitCallback(state, [gate]() {
            gate->arriveAndWait();
        });
        executor.scheduleUnitCallbackDrain(unit, state, schedule);
    }

    static void waitPool(WtDiffExecuter& executor)
    {
        if (executor._pool)
            executor._pool->wait();
    }
};
} // namespace wtp

namespace {

TEST(WtDiffExecuterConcurrencyTest, SameCodeCallbacksAreFifoAndReentryDoesNotDeadlock)
{
    DiffProbeFactory fact;
    WtExecuterFactory pluginFactory;
    WtDiffExecuter executor(&pluginFactory, "diff_serial_fifo", NULL, NULL);
    WtDiffExecuterConcurrencyTest::configurePool(executor, 2);
    DiffProbeUnit* unit = WtDiffExecuterConcurrencyTest::registerUnit(executor, fact, "SHFE.ag.2512");
    const auto gate = std::make_shared<Gate>();
    unit->setPositionGate(gate);

    executor.on_position_changed("SHFE.ag.2512", 1.0);
    const bool firstStarted = gate->waitForArrivals(1);
    if (!firstStarted)
        gate->release();
    ASSERT_TRUE(firstStarted);
    executor.on_position_changed("SHFE.ag.2512", 2.0);
    EXPECT_EQ(unit->positions().size(), 1u);
    gate->release();
    WtDiffExecuterConcurrencyTest::waitPool(executor);

    EXPECT_EQ(unit->maxActive(), 1);
    EXPECT_EQ(unit->positions(), (std::vector<double>{1.0, 3.0}));
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::target(executor, "SHFE.ag.2512"), 3.0);

    std::atomic<bool> reentered(false);
    unit->setPositionGate(std::shared_ptr<Gate>());
    unit->setPositionHook([&](double) {
        if (!reentered.exchange(true))
            executor.on_position_changed("SHFE.ag.2512", 1.0);
    });
    executor.on_position_changed("SHFE.ag.2512", 1.0);
    WtDiffExecuterConcurrencyTest::waitPool(executor);
    EXPECT_EQ(unit->maxActive(), 1);
    EXPECT_EQ(unit->positions(), (std::vector<double>{1.0, 3.0, 4.0, 5.0}));
}

TEST(WtDiffExecuterConcurrencyTest, NoPoolCrossThreadReentryQueuesWithoutDeadlock)
{
    DiffProbeFactory fact;
    WtExecuterFactory pluginFactory;
    WtDiffExecuter executor(&pluginFactory, "diff_serial_no_pool_reentry", NULL, NULL);
    DiffProbeUnit* unit = WtDiffExecuterConcurrencyTest::registerUnit(executor, fact, "SHFE.ag.2512");
    std::atomic<bool> reentered(false);
    unit->setPositionHook([&](double) {
        if (!reentered.exchange(true))
        {
            std::thread callbackThread([&]() {
                executor.on_position_changed("SHFE.ag.2512", 1.0);
            });
            callbackThread.join();
        }
    });

    executor.on_position_changed("SHFE.ag.2512", 1.0);

    EXPECT_EQ(unit->positions(), (std::vector<double>{1.0, 2.0}));
    EXPECT_EQ(unit->maxActive(), 1);
}

TEST(WtDiffExecuterConcurrencyTest, DifferentCodesCanRunCallbacksInParallel)
{
    DiffProbeFactory fact;
    WtExecuterFactory pluginFactory;
    WtDiffExecuter executor(&pluginFactory, "diff_serial_parallel", NULL, NULL);
    WtDiffExecuterConcurrencyTest::configurePool(executor, 2);
    DiffProbeUnit* first = WtDiffExecuterConcurrencyTest::registerUnit(executor, fact, "SHFE.ag.2512");
    DiffProbeUnit* second = WtDiffExecuterConcurrencyTest::registerUnit(executor, fact, "SHFE.rb.2510");
    const auto gate = std::make_shared<Gate>();
    first->setPositionGate(gate);
    second->setPositionGate(gate);

    executor.on_position_changed("SHFE.ag.2512", 1.0);
    executor.on_position_changed("SHFE.rb.2510", 1.0);
    const bool parallel = gate->waitForArrivals(2);
    gate->release();
    WtDiffExecuterConcurrencyTest::waitPool(executor);

    EXPECT_TRUE(parallel);
    EXPECT_EQ(first->maxActive(), 1);
    EXPECT_EQ(second->maxActive(), 1);
}

TEST(WtDiffExecuterConcurrencyTest, ReadyPrecedesRecoveryAndOldReadyEpochIsSkipped)
{
    DiffProbeFactory fact;
    WtExecuterFactory pluginFactory;
    WtDiffExecuter executor(&pluginFactory, "diff_serial_epoch", NULL, NULL);
    WtDiffExecuterConcurrencyTest::configurePool(executor, 2);
    DiffProbeUnit* unit = WtDiffExecuterConcurrencyTest::registerUnit(executor, fact, "SHFE.ag.2512");
    WtDiffExecuterConcurrencyTest::setDiff(executor, "SHFE.ag.2512", 7.0);

    const auto blocker = std::make_shared<Gate>();
    WtDiffExecuterConcurrencyTest::enqueueBlocker(executor, "SHFE.ag.2512", blocker);
    const bool blockerStarted = blocker->waitForArrivals(1);
    if (!blockerStarted)
        blocker->release();
    ASSERT_TRUE(blockerStarted);

    // Keep epoch 1's ready/recovery pair queued. The ready-lost-ready updates
    // advance the epoch before that old recovery callback can run.
	unit->setPositionGate(std::shared_ptr<Gate>());
	executor.on_channel_ready();
    executor.on_channel_lost();
    executor.on_channel_ready();
    blocker->release();
    WtDiffExecuterConcurrencyTest::waitPool(executor);

    const std::vector<std::string> events = unit->events();
    EXPECT_EQ(events, (std::vector<std::string>{"ready", "lost", "ready", "position"}));
    EXPECT_EQ(unit->positions(), (std::vector<double>{7.0}));
}

TEST(WtDiffExecuterConcurrencyTest, RecoveryUsesDiffCapturedBeforeQueuedTrade)
{
    DiffProbeFactory fact;
    WtExecuterFactory pluginFactory;
    const std::string name = uniqueName("ready_trade_order");
    const std::string path = std::string(WtHelper::getExecDataDir()) + name + ".json";
    std::error_code ec;
    fs::remove(path, ec);

    WtDiffExecuter executor(&pluginFactory, name.c_str(), NULL, NULL);
    WtDiffExecuterConcurrencyTest::configurePool(executor, 2);
    DiffProbeUnit* unit = WtDiffExecuterConcurrencyTest::registerUnit(executor, fact,
        "SHFE.ag.2512");
    WtDiffExecuterConcurrencyTest::setDiff(executor, "SHFE.ag.2512", 10.0);
    ASSERT_TRUE(WtDiffExecuterConcurrencyTest::save(executor));

    const auto blocker = std::make_shared<Gate>();
    WtDiffExecuterConcurrencyTest::enqueueBlocker(executor, "SHFE.ag.2512", blocker);
    ASSERT_TRUE(blocker->waitForArrivals(1));
    executor.on_channel_ready();
    executor.on_trade(52u, "SHFE.ag.2512", true, 4.0, 3500.0);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::diff(executor, "SHFE.ag.2512"), 6.0);

    blocker->release();
    WtDiffExecuterConcurrencyTest::waitPool(executor);
    EXPECT_EQ(unit->positions(), (std::vector<double>{10.0}));
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::diff(executor, "SHFE.ag.2512"), 6.0);
    EXPECT_EQ(unit->events(), (std::vector<std::string>{"ready", "position", "trade"}));
    fs::remove(path, ec);
}

TEST(WtDiffExecuterConcurrencyTest, ReadyTraderBindingWaitsForExplicitPostRegistrationReady)
{
    r71::R71GenEnv gen;
    r71::R71TraderEnv traderEnv;
    TraderAdapter trader;
    ASSERT_TRUE(traderEnv.build(trader, "diff_settrader_ready"));

    DiffProbeFactory fact;
    WtExecuterFactory pluginFactory;
    WtDiffExecuter executor(&pluginFactory, "diff_settrader_ready", NULL, NULL);
    WtDiffExecuterConcurrencyTest::configurePool(executor, 2);
    DiffProbeUnit* unit = WtDiffExecuterConcurrencyTest::registerUnit(executor, fact, "SHFE.ag.2512");
    WtDiffExecuterConcurrencyTest::setDiff(executor, "SHFE.ag.2512", 7.0);

    executor.setTrader(&trader);
    EXPECT_TRUE(unit->events().empty());
    EXPECT_TRUE(unit->positions().empty());

    // Mirrors the runner integration contract: attach first, then publish the
    // already-ready state so recovery cannot send orders before sink binding.
    trader.addSink(&executor);
    executor.on_channel_ready();
    WtDiffExecuterConcurrencyTest::waitPool(executor);

    EXPECT_EQ(unit->events(), (std::vector<std::string>{"ready", "position"}));
    EXPECT_EQ(unit->positions(), (std::vector<double>{7.0}));
}

TEST(WtDiffExecuterConcurrencyTest, RuntimeReadyExceptionFailsUnitAndSkipsRecovery)
{
    DiffProbeFactory fact;
    WtExecuterFactory pluginFactory;
    WtDiffExecuter executor(&pluginFactory, "diff_serial_ready_throw", NULL, NULL);
    WtDiffExecuterConcurrencyTest::configurePool(executor, 2);
    DiffProbeUnit* rawUnit = WtDiffExecuterConcurrencyTest::registerUnit(executor, fact, "SHFE.ag.2512");
    ExecuteUnitPtr unitOwner = WtDiffExecuterConcurrencyTest::unitOwner(executor, "SHFE.ag.2512");
    ASSERT_NE(unitOwner, nullptr);
    DiffProbeUnit* unit = static_cast<DiffProbeUnit*>(unitOwner->self());
    ASSERT_EQ(unit, rawUnit);
    unit->setThrowOnReady(true);
    WtDiffExecuterConcurrencyTest::setDiff(executor, "SHFE.ag.2512", 7.0);

    executor.on_channel_ready();
    WtDiffExecuterConcurrencyTest::waitPool(executor);

    const ExecUnitInitStatus status = executor.queryUnitInitStatus("SHFE.ag.2512");
    EXPECT_EQ(status.state, ExecUnitInitState::FAILED);
    EXPECT_NE(status.error.find("callback threw"), std::string::npos);
    EXPECT_FALSE(WtDiffExecuterConcurrencyTest::hasUnit(executor, "SHFE.ag.2512"));
    EXPECT_EQ(unit->events(), (std::vector<std::string>{"ready"}));
    EXPECT_TRUE(unit->positions().empty());
    EXPECT_EQ(executor.clearFailedUnitInitStatus("SHFE.ag.2512"),
        ExecUnitInitClearResult::RETRY_BLOCKED);
    const ExecUnitInitStatus retained = executor.queryUnitInitStatus("SHFE.ag.2512");
    EXPECT_EQ(retained.state, ExecUnitInitState::FAILED);
    EXPECT_EQ(retained.generation, status.generation);
}

TEST(WtDiffExecuterConcurrencyTest, InitialReadyExceptionBlocksRetryBeforeCreationCompletes)
{
    DiffProbeFactory fact;
    WtExecuterFactory pluginFactory;
    WtDiffExecuter executor(&pluginFactory, "diff_serial_initial_ready_throw", NULL, NULL);
    WtDiffExecuterConcurrencyTest::configurePool(executor, 2);
    DiffProbeUnit* unit = WtDiffExecuterConcurrencyTest::registerUnit(executor, fact,
        "SHFE.ag.2512");
    WtDiffExecuterConcurrencyTest::setDiff(executor, "SHFE.ag.2512", 7.0);
    WtDiffExecuterConcurrencyTest::setCreationIncomplete(executor, "SHFE.ag.2512");
    unit->setThrowOnReady(true);

    executor.on_channel_ready();
    EXPECT_TRUE(unit->events().empty());
    WtDiffExecuterConcurrencyTest::drainCallbacks(executor, "SHFE.ag.2512");

    EXPECT_TRUE(WtDiffExecuterConcurrencyTest::retryBlocked(executor, "SHFE.ag.2512"));
    EXPECT_EQ(unit->events(), (std::vector<std::string>{"ready"}));
    EXPECT_TRUE(unit->positions().empty());
}

TEST(WtDiffExecuterConcurrencyTest, RuntimeFailureAfterPartialTradeBlocksTargetReplay)
{
    r71::R71BaseDataMgr bd;
    bd.regisSession("TRADING");
    WTSSessionInfo* session = bd.session("TRADING");
    ASSERT_NE(session, nullptr);
    session->addTradingSection(900, 1500);
    WTSCommodityInfo* commodity = r71::makeComm("SHFE", "ag", "TRADING", TM_Both);
    commodity->setSessionInfo(session);
    WTSContractInfo* contract = r71::makeCt("SHFE", "ag", "ag2512");
    bd.regis(commodity, contract);
    bd.regisAlias("SHFE.ag.2512", contract);

    DiffProbeFactory fact;
    WtExecuterFactory pluginFactory;
    const std::string name = uniqueName("partial_fill_failure");
    const std::string path = std::string(WtHelper::getExecDataDir()) + name + ".json";
    std::error_code ec;
    fs::remove(path, ec);

    WtDiffExecuter executor(&pluginFactory, name.c_str(), NULL, &bd);
    WtDiffExecuterConcurrencyTest::configurePool(executor, 2);
    DiffProbeUnit* rawUnit = WtDiffExecuterConcurrencyTest::registerUnit(executor, fact,
        "SHFE.ag.2512");
    ExecuteUnitPtr unitOwner = WtDiffExecuterConcurrencyTest::unitOwner(executor,
        "SHFE.ag.2512");
    ASSERT_NE(unitOwner, nullptr);
    DiffProbeUnit* unit = static_cast<DiffProbeUnit*>(unitOwner->self());
    ASSERT_EQ(unit, rawUnit);

    wt_hashmap<std::string, double> targets;
    targets["SHFE.ag.2512"] = 10.0;
    executor.set_position(targets);
    WtDiffExecuterConcurrencyTest::waitPool(executor);
    ASSERT_EQ(unit->positions(), (std::vector<double>{10.0}));

    // The +4 fill is applied to the central diff (+10 -> +6) before the
    // plugin trade callback throws. Replaying the original +10 would duplicate
    // the already-filled quantity.
    unit->setThrowOnTrade(true);
    executor.on_trade(17u, "SHFE.ag.2512", true, 4.0, 3500.0);
    WtDiffExecuterConcurrencyTest::waitPool(executor);

    const ExecUnitInitStatus failed = executor.queryUnitInitStatus("SHFE.ag.2512");
    ASSERT_EQ(failed.state, ExecUnitInitState::FAILED);
    EXPECT_NE(failed.error.find("automatic retry disabled"), std::string::npos);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::diff(executor, "SHFE.ag.2512"), 6.0);
    EXPECT_EQ(executor.clearFailedUnitInitStatus("SHFE.ag.2512"),
        ExecUnitInitClearResult::RETRY_BLOCKED);

    // A later partial fill on the same order is still booked centrally, but
    // the failed plugin receives no further callbacks.
    executor.on_trade(17u, "SHFE.ag.2512", true, 1.0, 3501.0);
    WtDiffExecuterConcurrencyTest::waitPool(executor);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::diff(executor, "SHFE.ag.2512"), 5.0);
    WtDiffExecuter tradeReload(&pluginFactory, name.c_str(), NULL, &bd);
    WtDiffExecuterConcurrencyTest::load(tradeReload);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::diff(tradeReload, "SHFE.ag.2512"), 5.0);
    const ExecUnitInitStatus persistedFailure = tradeReload.queryUnitInitStatus("SHFE.ag.2512");
    EXPECT_EQ(persistedFailure.state, ExecUnitInitState::FAILED);
    EXPECT_EQ(persistedFailure.generation, failed.generation);
    EXPECT_EQ(tradeReload.clearFailedUnitInitStatus("SHFE.ag.2512"),
        ExecUnitInitClearResult::RETRY_BLOCKED);
    tradeReload.on_channel_ready();
    EXPECT_FALSE(WtDiffExecuterConcurrencyTest::hasUnit(tradeReload, "SHFE.ag.2512"));
    EXPECT_EQ(WtDiffExecuterConcurrencyTest::requestUnit(tradeReload, "SHFE.ag.2512"), nullptr);
    EXPECT_FALSE(WtDiffExecuterConcurrencyTest::hasUnit(tradeReload, "SHFE.ag.2512"));

    executor.set_position(targets);
    WtDiffExecuterConcurrencyTest::waitPool(executor);
    targets["SHFE.ag.2512"] = 12.0;
    executor.set_position(targets);
    executor.on_position_changed("SHFE.ag.2512", 1.0);
    WtDiffExecuterConcurrencyTest::waitPool(executor);

    const ExecUnitInitStatus afterReplayAttempt = executor.queryUnitInitStatus("SHFE.ag.2512");
    EXPECT_EQ(afterReplayAttempt.state, ExecUnitInitState::FAILED);
    EXPECT_EQ(afterReplayAttempt.generation, failed.generation);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::target(executor, "SHFE.ag.2512"), 13.0);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::diff(executor, "SHFE.ag.2512"), 8.0);
    EXPECT_EQ(unit->positions(), (std::vector<double>{10.0}));
    EXPECT_EQ(unit->events(), (std::vector<std::string>{"position", "trade"}));
    WtDiffExecuter targetReload(&pluginFactory, name.c_str(), NULL, &bd);
    WtDiffExecuterConcurrencyTest::load(targetReload);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::target(targetReload, "SHFE.ag.2512"), 13.0);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::diff(targetReload, "SHFE.ag.2512"), 8.0);
    fs::remove(path, ec);
}

TEST(WtDiffExecuterConcurrencyTest, PositionChangedTargetAndDiffSurviveReload)
{
    r71::R71GenEnv gen;
    r71::R71BaseDataMgr bd;
    bd.regisSession("TRADING");
    WTSSessionInfo* session = bd.session("TRADING");
    ASSERT_NE(session, nullptr);
    session->addTradingSection(900, 1500);
    WTSCommodityInfo* commodity = r71::makeComm("SHFE", "ag", "TRADING", TM_Both);
    commodity->setSessionInfo(session);
    WTSContractInfo* contract = r71::makeCt("SHFE", "ag", "ag2512");
    bd.regis(commodity, contract);
    bd.regisAlias("SHFE.ag.2512", contract);

    DiffProbeFactory fact;
    WtExecuterFactory pluginFactory;
    const std::string name = uniqueName("position_changed_reload");
    const std::string path = std::string(WtHelper::getExecDataDir()) + name + ".json";
    std::error_code ec;
    fs::remove(path, ec);

    WtDiffExecuter executor(&pluginFactory, name.c_str(), NULL, &bd);
    WtDiffExecuterConcurrencyTest::configurePool(executor, 2);
    DiffProbeUnit* unit = WtDiffExecuterConcurrencyTest::registerUnit(executor, fact,
        "SHFE.ag.2512");

    executor.on_position_changed("SHFE.ag.2512", 5.0);
    WtDiffExecuterConcurrencyTest::waitPool(executor);
    ASSERT_EQ(unit->positions(), (std::vector<double>{5.0}));

    WtDiffExecuter reloaded(&pluginFactory, name.c_str(), NULL, &bd);
    WtDiffExecuterConcurrencyTest::load(reloaded);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::target(reloaded, "SHFE.ag.2512"), 5.0);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::diff(reloaded, "SHFE.ag.2512"), 5.0);

    WtDiffExecuterConcurrencyTest::configurePool(reloaded, 2);
    DiffProbeUnit* reloadedUnit = WtDiffExecuterConcurrencyTest::registerUnit(reloaded,
        fact, "SHFE.ag.2512");
    reloaded.on_channel_ready();
    WtDiffExecuterConcurrencyTest::waitPool(reloaded);
    EXPECT_EQ(reloadedUnit->events(), (std::vector<std::string>{"ready", "position"}));
    EXPECT_EQ(reloadedUnit->positions(), (std::vector<double>{5.0}));
    fs::remove(path, ec);
}

TEST(WtDiffExecuterConcurrencyTest, TradeAndTargetUseConsistentPersistedSnapshot)
{
    DiffProbeFactory fact;
    WtExecuterFactory pluginFactory;
    const std::string name = uniqueName("snapshot");
    const std::string path = std::string(WtHelper::getExecDataDir()) + name + ".json";
    std::error_code ec;
    fs::remove(path, ec);
    WtDiffExecuter executor(&pluginFactory, name.c_str(), NULL, NULL);
    WtDiffExecuterConcurrencyTest::configurePool(executor, 2);
    DiffProbeUnit* unit = WtDiffExecuterConcurrencyTest::registerUnit(executor, fact, "SHFE.ag.2512");

    wt_hashmap<std::string, double> target;
    target["SHFE.ag.2512"] = 100.0;
    const auto start = std::make_shared<Gate>();
    std::thread targetThread([&]() {
        start->arriveAndWait();
        executor.set_position(target);
    });
    std::thread tradeThread([&]() {
        start->arriveAndWait();
        executor.on_trade(9u, "SHFE.ag.2512", true, 10.0, 3500.0);
    });
    const bool bothStarted = start->waitForArrivals(2);
    start->release();
    targetThread.join();
    tradeThread.join();
    ASSERT_TRUE(bothStarted);
    WtDiffExecuterConcurrencyTest::waitPool(executor);

    std::ifstream input(path.c_str());
    ASSERT_TRUE(input.is_open());
    const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    rj::Document root;
    root.Parse(content.c_str());
    ASSERT_FALSE(root.HasParseError());
    ASSERT_TRUE(root.HasMember("targets"));
    ASSERT_TRUE(root.HasMember("diffs"));
    double savedTarget = -1.0;
    double savedDiff = -1.0;
    for (const rj::Value& item : root["targets"].GetArray())
        if (std::string(item["code"].GetString()) == "SHFE.ag.2512")
            savedTarget = item["target"].GetDouble();
    for (const rj::Value& item : root["diffs"].GetArray())
        if (std::string(item["code"].GetString()) == "SHFE.ag.2512")
            savedDiff = item["diff"].GetDouble();

    EXPECT_DOUBLE_EQ(savedTarget, 100.0);
    EXPECT_DOUBLE_EQ(savedDiff, 90.0);
    EXPECT_EQ(unit->maxActive(), 1);
    fs::remove(path, ec);
}

TEST(WtDiffExecuterConcurrencyTest, OmittedTargetZeroUsesSignedTargetDelta)
{
    r71::R71BaseDataMgr bd;
    bd.regisSession("TRADING");
    WTSSessionInfo* session = bd.session("TRADING");
    ASSERT_NE(session, nullptr);
    session->addTradingSection(900, 1500);
    WTSCommodityInfo* commodity = r71::makeComm("SHFE", "ag", "TRADING", TM_Both);
    commodity->setSessionInfo(session);
    WTSContractInfo* contract = r71::makeCt("SHFE", "ag", "ag2512");
    bd.regis(commodity, contract);
    bd.regisAlias("SHFE.ag.2512", contract);

    DiffProbeFactory fact;
    WtExecuterFactory pluginFactory;
    const std::string name = uniqueName("signed_zero_target");
    const std::string path = std::string(WtHelper::getExecDataDir()) + name + ".json";
    std::error_code ec;
    fs::remove(path, ec);

    WtDiffExecuter executor(&pluginFactory, name.c_str(), NULL, &bd);
    WtDiffExecuterConcurrencyTest::configurePool(executor, 2);
    DiffProbeUnit* unit = WtDiffExecuterConcurrencyTest::registerUnit(executor, fact,
        "SHFE.ag.2512");
    wt_hashmap<std::string, double> targets;
    wt_hashmap<std::string, double> emptyTargets;

    targets["SHFE.ag.2512"] = 5.0;
    executor.set_position(targets);
    WtDiffExecuterConcurrencyTest::waitPool(executor);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::diff(executor, "SHFE.ag.2512"), 5.0);
    executor.set_position(emptyTargets);
    WtDiffExecuterConcurrencyTest::waitPool(executor);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::target(executor, "SHFE.ag.2512"), 0.0);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::diff(executor, "SHFE.ag.2512"), 0.0);

    targets["SHFE.ag.2512"] = -5.0;
    executor.set_position(targets);
    WtDiffExecuterConcurrencyTest::waitPool(executor);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::diff(executor, "SHFE.ag.2512"), -5.0);
    executor.set_position(emptyTargets);
    WtDiffExecuterConcurrencyTest::waitPool(executor);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::diff(executor, "SHFE.ag.2512"), 0.0);

    targets["SHFE.ag.2512"] = 5.0;
    executor.set_position(targets);
    WtDiffExecuterConcurrencyTest::waitPool(executor);
    executor.on_trade(41u, "SHFE.ag.2512", true, 4.0, 3500.0);
    WtDiffExecuterConcurrencyTest::waitPool(executor);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::diff(executor, "SHFE.ag.2512"), 1.0);
    executor.set_position(emptyTargets);
    WtDiffExecuterConcurrencyTest::waitPool(executor);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::target(executor, "SHFE.ag.2512"), 0.0);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::diff(executor, "SHFE.ag.2512"), -4.0);
    EXPECT_EQ(unit->positions(), (std::vector<double>{5.0, 0.0, -5.0, 0.0, 5.0, -4.0}));
    fs::remove(path, ec);
}

TEST(WtDiffExecuterConcurrencyTest, OmittedTargetThreeToZeroUsesRawCodeAndExchangeAndPersists)
{
    r71::R71BaseDataMgr bd;
    bd.regisSession("TRADING");
    WTSSessionInfo* session = bd.session("TRADING");
    ASSERT_NE(session, nullptr);
    session->addTradingSection(900, 1500);
    WTSCommodityInfo* commodity = r71::makeComm("SHFE", "ag", "TRADING", TM_Both);
    commodity->setSessionInfo(session);
    WTSContractInfo* contract = r71::makeCt("SHFE", "ag", "ag2512");
    bd.regis(commodity, contract);

    ASSERT_EQ(bd.getContract("ag2512", "SHFE"), contract);
    EXPECT_EQ(bd.getContract("SHFE.ag.2512", "SHFE"), nullptr);
    EXPECT_EQ(bd.getContract("ag2512", ""), nullptr);

    DiffProbeFactory fact;
    WtExecuterFactory pluginFactory;
    const std::string name = uniqueName("omitted_three_to_zero_raw_exchange");
    const std::string path = std::string(WtHelper::getExecDataDir()) + name + ".json";
    std::error_code ec;
    fs::remove(path, ec);

    WtDiffExecuter executor(&pluginFactory, name.c_str(), NULL, &bd);
    WtDiffExecuterConcurrencyTest::configurePool(executor, 2);
    DiffProbeUnit* unit = WtDiffExecuterConcurrencyTest::registerUnit(executor, fact,
        "SHFE.ag.2512");
    wt_hashmap<std::string, double> targets;
    wt_hashmap<std::string, double> emptyTargets;
    targets["SHFE.ag.2512"] = 3.0;

    executor.set_position(targets);
    WtDiffExecuterConcurrencyTest::waitPool(executor);
    ASSERT_EQ(unit->positions(), (std::vector<double>{3.0}));
    ASSERT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::target(executor, "SHFE.ag.2512"), 3.0);
    ASSERT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::diff(executor, "SHFE.ag.2512"), 3.0);

    executor.set_position(emptyTargets);
    WtDiffExecuterConcurrencyTest::waitPool(executor);

    EXPECT_EQ(unit->positions(), (std::vector<double>{3.0, 0.0}));
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::target(executor, "SHFE.ag.2512"), 0.0);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::diff(executor, "SHFE.ag.2512"), 0.0);
    double savedTarget = -1.0;
    double savedDiff = -1.0;
    ASSERT_TRUE(readSavedPosition(path, "targets", "SHFE.ag.2512", "target", savedTarget));
    ASSERT_TRUE(readSavedPosition(path, "diffs", "SHFE.ag.2512", "diff", savedDiff));
    EXPECT_DOUBLE_EQ(savedTarget, 0.0);
    EXPECT_DOUBLE_EQ(savedDiff, 0.0);
    fs::remove(path, ec);
}

TEST(WtDiffExecuterConcurrencyTest, MissingMatchingExchangeRetainsOmittedTargetAndReportsError)
{
    DiffErrorLogCapture logs;
    r71::R71BaseDataMgr bd;
    bd.regisSession("TRADING");
    WTSSessionInfo* session = bd.session("TRADING");
    ASSERT_NE(session, nullptr);
    session->addTradingSection(900, 1500);
    WTSCommodityInfo* otherExchangeCommodity = r71::makeComm("DCE", "ag", "TRADING", TM_Both);
    otherExchangeCommodity->setSessionInfo(session);
    WTSContractInfo* otherExchangeContract = r71::makeCt("DCE", "ag", "ag2512");
    bd.regis(otherExchangeCommodity, otherExchangeContract);

    ASSERT_EQ(bd.getContract("ag2512", "DCE"), otherExchangeContract);
    EXPECT_EQ(bd.getContract("ag2512", "SHFE"), nullptr);
    DiffProbeFactory fact;
    WtExecuterFactory pluginFactory;
    const std::string name = uniqueName("omitted_target_missing_exchange");
    const std::string path = std::string(WtHelper::getExecDataDir()) + name + ".json";
    std::error_code ec;
    fs::remove(path, ec);

    WtDiffExecuter executor(&pluginFactory, name.c_str(), NULL, &bd);
    WtDiffExecuterConcurrencyTest::configurePool(executor, 2);
    DiffProbeUnit* unit = WtDiffExecuterConcurrencyTest::registerUnit(executor, fact,
        "SHFE.ag.2512");
    wt_hashmap<std::string, double> targets;
    wt_hashmap<std::string, double> emptyTargets;
    targets["SHFE.ag.2512"] = 3.0;
    executor.set_position(targets);
    WtDiffExecuterConcurrencyTest::waitPool(executor);
    ASSERT_EQ(unit->positions(), (std::vector<double>{3.0}));

    executor.set_position(emptyTargets);
    WtDiffExecuterConcurrencyTest::waitPool(executor);

    EXPECT_EQ(unit->positions(), (std::vector<double>{3.0}));
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::target(executor, "SHFE.ag.2512"), 3.0);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::diff(executor, "SHFE.ag.2512"), 3.0);
    double savedTarget = -1.0;
    double savedDiff = -1.0;
    ASSERT_TRUE(readSavedPosition(path, "targets", "SHFE.ag.2512", "target", savedTarget));
    ASSERT_TRUE(readSavedPosition(path, "diffs", "SHFE.ag.2512", "diff", savedDiff));
    EXPECT_DOUBLE_EQ(savedTarget, 3.0);
    EXPECT_DOUBLE_EQ(savedDiff, 3.0);
    const std::vector<std::string> errors = logs.messages();
    ASSERT_FALSE(errors.empty());
    EXPECT_NE(errors.back().find("Omitted target for SHFE.ag.2512 cannot be cleared"), std::string::npos);
    EXPECT_NE(errors.back().find("existing target and diff are retained"), std::string::npos);
    fs::remove(path, ec);
}

TEST(WtDiffExecuterConcurrencyTest, NewerReentrantTargetBatchWinsWithoutDroppingTrade)
{
    r71::R71BaseDataMgr bd;
    bd.regisSession("TRADING");
    WTSSessionInfo* session = bd.session("TRADING");
    ASSERT_NE(session, nullptr);
    session->addTradingSection(900, 1500);
    WTSCommodityInfo* agCommodity = r71::makeComm("SHFE", "ag", "TRADING", TM_Both);
    agCommodity->setSessionInfo(session);
    WTSContractInfo* agContract = r71::makeCt("SHFE", "ag", "ag2512");
    bd.regis(agCommodity, agContract);
    bd.regisAlias("SHFE.ag.2512", agContract);
    WTSCommodityInfo* rbCommodity = r71::makeComm("SHFE", "rb", "TRADING", TM_Both);
    rbCommodity->setSessionInfo(session);
    WTSContractInfo* rbContract = r71::makeCt("SHFE", "rb", "rb2510");
    bd.regis(rbCommodity, rbContract);
    bd.regisAlias("SHFE.rb.2510", rbContract);

    DiffProbeFactory fact;
    WtExecuterFactory pluginFactory;
    const std::string name = uniqueName("target_batch_generation");
    const std::string path = std::string(WtHelper::getExecDataDir()) + name + ".json";
    std::error_code ec;
    fs::remove(path, ec);

    WtDiffExecuter executor(&pluginFactory, name.c_str(), NULL, &bd);
    DiffProbeUnit* agUnit = WtDiffExecuterConcurrencyTest::registerUnit(executor, fact,
        "SHFE.ag.2512");
    DiffProbeUnit* rbUnit = WtDiffExecuterConcurrencyTest::registerUnit(executor, fact,
        "SHFE.rb.2510");
    wt_hashmap<std::string, double> initialTargets;
    initialTargets["SHFE.rb.2510"] = 5.0;
    executor.set_position(initialTargets);
    EXPECT_EQ(rbUnit->positions(), (std::vector<double>{5.0}));

    std::atomic<bool> newerBatchStarted(false);
    wt_hashmap<std::string, double> newerTargets;
    newerTargets["SHFE.ag.2512"] = 12.0;
    newerTargets["SHFE.rb.2510"] = 7.0;
    agUnit->setPositionHook([&](double value) {
        if (value != 10.0 || newerBatchStarted.exchange(true))
            return;
        executor.set_position(newerTargets);
        executor.on_trade(81u, "SHFE.rb.2510", true, 1.0, 3500.0);
    });

    wt_hashmap<std::string, double> olderTargets;
    olderTargets["SHFE.ag.2512"] = 10.0;
    executor.set_position(olderTargets);

    EXPECT_TRUE(newerBatchStarted.load());
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::target(executor, "SHFE.ag.2512"), 12.0);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::target(executor, "SHFE.rb.2510"), 7.0);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::diff(executor, "SHFE.ag.2512"), 12.0);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::diff(executor, "SHFE.rb.2510"), 6.0);
    EXPECT_EQ(agUnit->positions(), (std::vector<double>{10.0, 12.0}));
    EXPECT_EQ(rbUnit->positions(), (std::vector<double>{5.0, 7.0}));
    EXPECT_EQ(rbUnit->events(), (std::vector<std::string>{"position", "position", "trade"}));
    fs::remove(path, ec);
}

TEST(WtDiffExecuterConcurrencyTest, OmittedSweepPreservesIncrementalTargetUpdatedAfterBatchStart)
{
    r71::R71BaseDataMgr bd;
    bd.regisSession("TRADING");
    WTSSessionInfo* session = bd.session("TRADING");
    ASSERT_NE(session, nullptr);
    session->addTradingSection(900, 1500);
    WTSCommodityInfo* agCommodity = r71::makeComm("SHFE", "ag", "TRADING", TM_Both);
    agCommodity->setSessionInfo(session);
    WTSContractInfo* agContract = r71::makeCt("SHFE", "ag", "ag2512");
    bd.regis(agCommodity, agContract);
    bd.regisAlias("SHFE.ag.2512", agContract);
    WTSCommodityInfo* rbCommodity = r71::makeComm("SHFE", "rb", "TRADING", TM_Both);
    rbCommodity->setSessionInfo(session);
    WTSContractInfo* rbContract = r71::makeCt("SHFE", "rb", "rb2510");
    bd.regis(rbCommodity, rbContract);
    bd.regisAlias("SHFE.rb.2510", rbContract);

    DiffProbeFactory fact;
    WtExecuterFactory pluginFactory;
    const std::string name = uniqueName("omitted_sweep_incremental_update");
    const std::string path = std::string(WtHelper::getExecDataDir()) + name + ".json";
    std::error_code ec;
    fs::remove(path, ec);

    WtDiffExecuter executor(&pluginFactory, name.c_str(), NULL, &bd);
    DiffProbeUnit* agUnit = WtDiffExecuterConcurrencyTest::registerUnit(executor, fact,
        "SHFE.ag.2512");
    DiffProbeUnit* rbUnit = WtDiffExecuterConcurrencyTest::registerUnit(executor, fact,
        "SHFE.rb.2510");

    wt_hashmap<std::string, double> initialTargets;
    initialTargets["SHFE.rb.2510"] = 5.0;
    executor.set_position(initialTargets);
    EXPECT_EQ(rbUnit->positions(), (std::vector<double>{5.0}));

    bool incrementalUpdateRan = false;
    WtDiffExecuterConcurrencyTest::setOmittedSweepHook(executor,
        [&](const std::string& code) {
            EXPECT_EQ(code, "SHFE.rb.2510");
            incrementalUpdateRan = true;
            executor.on_position_changed(code.c_str(), 2.0);
        });

    wt_hashmap<std::string, double> newerBatchTargets;
    newerBatchTargets["SHFE.ag.2512"] = 10.0;
    executor.set_position(newerBatchTargets);
    WtDiffExecuterConcurrencyTest::setOmittedSweepHook(executor, {});

    EXPECT_TRUE(incrementalUpdateRan);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::target(executor, "SHFE.rb.2510"), 7.0);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::diff(executor, "SHFE.rb.2510"), 7.0);
    EXPECT_EQ(rbUnit->positions(), (std::vector<double>{5.0, 7.0}));
    EXPECT_EQ(agUnit->positions(), (std::vector<double>{10.0}));
    fs::remove(path, ec);
}

TEST(WtDiffExecuterConcurrencyTest, ExplicitTargetBatchPreservesIncrementalUpdateForUnprocessedCode)
{
    r71::R71BaseDataMgr bd;
    bd.regisSession("TRADING");
    WTSSessionInfo* session = bd.session("TRADING");
    ASSERT_NE(session, nullptr);
    session->addTradingSection(900, 1500);
    WTSCommodityInfo* agCommodity = r71::makeComm("SHFE", "ag", "TRADING", TM_Both);
    agCommodity->setSessionInfo(session);
    WTSContractInfo* agContract = r71::makeCt("SHFE", "ag", "ag2512");
    bd.regis(agCommodity, agContract);
    bd.regisAlias("SHFE.ag.2512", agContract);
    WTSCommodityInfo* rbCommodity = r71::makeComm("SHFE", "rb", "TRADING", TM_Both);
    rbCommodity->setSessionInfo(session);
    WTSContractInfo* rbContract = r71::makeCt("SHFE", "rb", "rb2510");
    bd.regis(rbCommodity, rbContract);
    bd.regisAlias("SHFE.rb.2510", rbContract);

    DiffProbeFactory fact;
    WtExecuterFactory pluginFactory;
    const std::string name = uniqueName("explicit_batch_incremental_update");
    const std::string path = std::string(WtHelper::getExecDataDir()) + name + ".json";
    std::error_code ec;
    fs::remove(path, ec);

    WtDiffExecuter executor(&pluginFactory, name.c_str(), NULL, &bd);
    DiffProbeUnit* agUnit = WtDiffExecuterConcurrencyTest::registerUnit(executor, fact,
        "SHFE.ag.2512");
    DiffProbeUnit* rbUnit = WtDiffExecuterConcurrencyTest::registerUnit(executor, fact,
        "SHFE.rb.2510");

    wt_hashmap<std::string, double> batchTargets;
    batchTargets["SHFE.ag.2512"] = 10.0;
    batchTargets["SHFE.rb.2510"] = 20.0;
    auto targetIt = batchTargets.begin();
    const std::string firstCode = targetIt->first;
    ++targetIt;
    const std::string secondCode = targetIt->first;
    DiffProbeUnit* firstUnit = firstCode == "SHFE.ag.2512" ? agUnit : rbUnit;
    DiffProbeUnit* secondUnit = secondCode == "SHFE.ag.2512" ? agUnit : rbUnit;

    wt_hashmap<std::string, double> initialTargets;
    initialTargets[firstCode] = 1.0;
    initialTargets[secondCode] = 2.0;
    executor.set_position(initialTargets);
    EXPECT_EQ(firstUnit->positions(), (std::vector<double>{1.0}));
    EXPECT_EQ(secondUnit->positions(), (std::vector<double>{2.0}));

    std::atomic<bool> incrementalUpdateRan(false);
    firstUnit->setPositionHook([&](double value) {
        if (value != 10.0 || incrementalUpdateRan.exchange(true))
            return;
        executor.on_position_changed(secondCode.c_str(), 2.0);
    });

    executor.set_position(batchTargets);

    EXPECT_TRUE(incrementalUpdateRan.load());
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::target(executor, firstCode), 10.0);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::target(executor, secondCode), 4.0);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::diff(executor, firstCode), 10.0);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::diff(executor, secondCode), 4.0);
    EXPECT_EQ(firstUnit->positions(), (std::vector<double>{1.0, 10.0}));
    EXPECT_EQ(secondUnit->positions(), (std::vector<double>{2.0, 4.0}));
    fs::remove(path, ec);
}

TEST(WtDiffExecuterConcurrencyTest, SnapshotFailureBlocksPositionDispatchAndPersistsTombstoneWhenWritable)
{
    auto exerciseFailure = [](bool useIncrementalUpdate, const std::string& suffix) {
        DiffProbeFactory fact;
        WtExecuterFactory pluginFactory;
        const std::string name = uniqueName(suffix.c_str());
        const std::string path = std::string(WtHelper::getExecDataDir()) + name + ".json";
        std::error_code ec;
        fs::remove_all(path, ec);
        EXPECT_TRUE(fs::create_directory(path, ec));
        EXPECT_FALSE(ec);

        WtDiffExecuter executor(&pluginFactory, name.c_str(), NULL, NULL);
        WtDiffExecuterConcurrencyTest::configurePool(executor, 2);
        WtDiffExecuterConcurrencyTest::registerUnit(executor, fact, "SHFE.ag.2512");
        ExecuteUnitPtr unitOwner = WtDiffExecuterConcurrencyTest::unitOwner(executor,
            "SHFE.ag.2512");
        ASSERT_NE(unitOwner, nullptr);
        DiffProbeUnit* unit = static_cast<DiffProbeUnit*>(unitOwner->self());
        if (useIncrementalUpdate)
            executor.on_position_changed("SHFE.ag.2512", 5.0);
        else
        {
            wt_hashmap<std::string, double> targets;
            targets["SHFE.ag.2512"] = 5.0;
            executor.set_position(targets);
        }
        WtDiffExecuterConcurrencyTest::waitPool(executor);

        const ExecUnitInitStatus failed = executor.queryUnitInitStatus("SHFE.ag.2512");
        EXPECT_EQ(failed.state, ExecUnitInitState::FAILED);
        EXPECT_NE(failed.error.find("snapshot persistence failed"), std::string::npos);
        EXPECT_EQ(executor.clearFailedUnitInitStatus("SHFE.ag.2512"),
            ExecUnitInitClearResult::RETRY_BLOCKED);
        EXPECT_FALSE(WtDiffExecuterConcurrencyTest::hasUnit(executor, "SHFE.ag.2512"));
        EXPECT_TRUE(unit->positions().empty());
        EXPECT_LT(WtDiffExecuterConcurrencyTest::savedRevision(executor),
            WtDiffExecuterConcurrencyTest::positionRevision(executor));

        executor.on_channel_ready();
        WtDiffExecuterConcurrencyTest::waitPool(executor);
        EXPECT_TRUE(unit->positions().empty());
        EXPECT_TRUE(unit->events().empty());

        executor.requestQuiesce();
        EXPECT_FALSE(executor.waitQuiesced());
        EXPECT_FALSE(executor.waitQuiesced());

        EXPECT_GT(fs::remove_all(path, ec), 0u);
        EXPECT_TRUE(WtDiffExecuterConcurrencyTest::save(executor));
        WtDiffExecuter reloaded(&pluginFactory, name.c_str(), NULL, NULL);
        WtDiffExecuterConcurrencyTest::load(reloaded);
        const ExecUnitInitStatus restored = reloaded.queryUnitInitStatus("SHFE.ag.2512");
        EXPECT_EQ(restored.state, ExecUnitInitState::FAILED);
        EXPECT_EQ(restored.generation, failed.generation);
        EXPECT_EQ(reloaded.clearFailedUnitInitStatus("SHFE.ag.2512"),
            ExecUnitInitClearResult::RETRY_BLOCKED);
        EXPECT_EQ(WtDiffExecuterConcurrencyTest::requestUnit(reloaded, "SHFE.ag.2512"), nullptr);
        EXPECT_FALSE(WtDiffExecuterConcurrencyTest::hasUnit(reloaded, "SHFE.ag.2512"));
        reloaded.on_channel_ready();
        EXPECT_FALSE(WtDiffExecuterConcurrencyTest::hasUnit(reloaded, "SHFE.ag.2512"));
        fs::remove(path, ec);
    };

    exerciseFailure(false, "set_position_disk_failure");
    exerciseFailure(true, "position_changed_disk_failure");
}

TEST(WtDiffExecuterConcurrencyTest, QuiesceDrainsAcceptedPoolFifoAndRejectsNewIngress)
{
    DiffProbeFactory fact;
    WtExecuterFactory pluginFactory;
    WtDiffExecuter executor(&pluginFactory, uniqueName("quiesce_pool_fifo").c_str(), NULL, NULL);
    WtDiffExecuterConcurrencyTest::configurePool(executor, 2);
    DiffProbeUnit* unit = WtDiffExecuterConcurrencyTest::registerUnit(executor, fact,
        "SHFE.ag.2512");

    const std::thread::id callerThread = std::this_thread::get_id();
    std::atomic<bool> callbackRanOnPoolWorker(false);
    std::atomic<bool> callbackCanWait(true);
    std::atomic<bool> callbackWaitResult(true);
    unit->setPositionHook([&](double) {
        callbackRanOnPoolWorker.store(std::this_thread::get_id() != callerThread);
        callbackCanWait.store(executor.canWaitQuiescedOnCurrentThread());
        callbackWaitResult.store(executor.waitQuiesced());
    });
    const auto gate = std::make_shared<Gate>();
    unit->setPositionGate(gate);

    executor.on_position_changed("SHFE.ag.2512", 1.0);
    const bool callbackEntered = gate->waitForArrivals(1);
    if (!callbackEntered)
        gate->release();
    ASSERT_TRUE(callbackEntered);

    executor.on_position_changed("SHFE.ag.2512", 1.0);
    executor.requestQuiesce();
    executor.on_position_changed("SHFE.ag.2512", 1.0);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::target(executor, "SHFE.ag.2512"), 2.0);

    std::atomic<bool> waiterStarted(false);
    std::atomic<bool> waiterDone(false);
    bool waitResult = false;
    std::thread waiter([&]() {
        waiterStarted.store(true);
        waitResult = executor.waitQuiesced();
        waiterDone.store(true);
    });
    while (!waiterStarted.load())
        std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const bool returnedBeforeRelease = waiterDone.load();
    gate->release();
    waiter.join();

    EXPECT_FALSE(returnedBeforeRelease);
    EXPECT_TRUE(waitResult);
    EXPECT_TRUE(executor.waitQuiesced());
    EXPECT_TRUE(executor.canWaitQuiescedOnCurrentThread());
    EXPECT_TRUE(callbackRanOnPoolWorker.load());
    EXPECT_FALSE(callbackCanWait.load());
    EXPECT_FALSE(callbackWaitResult.load());
    EXPECT_EQ(unit->positions(), (std::vector<double>{1.0, 2.0}));
}

TEST(WtDiffExecuterConcurrencyTest, QuiesceDefersPluginWorkerWaitUntilCoordinatorPhase)
{
    DiffProbeFactory fact;
    WtExecuterFactory pluginFactory;
    WtDiffExecuter executor(&pluginFactory, uniqueName("quiesce_plugin_worker").c_str(),
        NULL, NULL);
    DiffProbeUnit* unit = WtDiffExecuterConcurrencyTest::registerUnit(executor, fact,
        "SHFE.ag.2512", true);
    ASSERT_NE(unit, nullptr);
    const ExecuteUnitPtr owner = WtDiffExecuterConcurrencyTest::unitOwner(executor,
        "SHFE.ag.2512");
    ASSERT_NE(owner, nullptr);
    EXPECT_EQ(executor.quiesceFactory(), &pluginFactory);
    const auto probe = WtDiffExecuterConcurrencyTest::quiesceProbe(executor,
        "SHFE.ag.2512");
    ASSERT_NE(probe, nullptr);
    ASSERT_TRUE(probe->startBackground([owner]() {
        return owner->canWaitQuiescedOnCurrentThread();
    }));
    ASSERT_TRUE(probe->waitForWorkerStarted());

    executor.requestQuiesce();
    EXPECT_TRUE(executor.preflightQuiesceOnCurrentThread());
    EXPECT_TRUE(executor.drainHostQuiesced());
    std::vector<ExecQuiesceOwnerPtr> owners;
    ASSERT_TRUE(executor.snapshotQuiescePluginOwners(owners));
    ASSERT_EQ(owners.size(), 1u);
    EXPECT_EQ(owners.front().get(), static_cast<IExecCommandQuiesce*>(owner.get()));
    EXPECT_EQ(probe->requestCount(), 0);
    owner->requestQuiesce(); // Direct plugin hook exercise; Diff's coordinator phase is pending.
    EXPECT_GT(probe->requestCount(), 0);
    probe->triggerWorkerPreflight();
    ASSERT_TRUE(probe->waitForWorkerPreflight());
    EXPECT_FALSE(probe->workerPreflightResult());

    EXPECT_FALSE(executor.canWaitQuiescedOnCurrentThread());
    EXPECT_FALSE(executor.waitQuiesced());
    EXPECT_FALSE(WtDiffExecuterConcurrencyTest::quiesceIncomplete(executor));
    EXPECT_EQ(probe->waitCount(), 0);
    EXPECT_EQ(WtDiffExecuterConcurrencyTest::quiesceOwnerCount(executor), 1u);
    probe->releaseBackground();
    EXPECT_TRUE(owner->waitQuiesced());
    EXPECT_TRUE(executor.completeQuiesceAfterGlobalPluginWait());
    EXPECT_TRUE(executor.waitQuiesced());
}

TEST(WtDiffExecuterConcurrencyTest, CoordinatorPendingAndPluginWaitFailureRetainUnitOwner)
{
    DiffProbeFactory fact;
    WtExecuterFactory pluginFactory;
    WtDiffExecuter executor(&pluginFactory, uniqueName("quiesce_plugin_failure").c_str(),
        NULL, NULL);
    WtDiffExecuterConcurrencyTest::registerUnit(executor, fact, "SHFE.ag.2512", true);
    const ExecuteUnitPtr owner = WtDiffExecuterConcurrencyTest::unitOwner(executor,
        "SHFE.ag.2512");
    const auto probe = WtDiffExecuterConcurrencyTest::quiesceProbe(executor,
        "SHFE.ag.2512");
    ASSERT_NE(owner, nullptr);
    ASSERT_NE(probe, nullptr);
    probe->setWaitResult(false);

    executor.requestQuiesce();
    EXPECT_TRUE(executor.preflightQuiesceOnCurrentThread());
    EXPECT_TRUE(executor.drainHostQuiesced());
    std::vector<ExecQuiesceOwnerPtr> owners;
    ASSERT_TRUE(executor.snapshotQuiescePluginOwners(owners));
    ASSERT_EQ(owners.size(), 1u);
    EXPECT_EQ(probe->requestCount(), 0);
    EXPECT_FALSE(executor.waitQuiesced());
    EXPECT_FALSE(WtDiffExecuterConcurrencyTest::quiesceIncomplete(executor));
    EXPECT_EQ(probe->waitCount(), 0);
    EXPECT_TRUE(WtDiffExecuterConcurrencyTest::hasUnit(executor, "SHFE.ag.2512"));
    EXPECT_EQ(WtDiffExecuterConcurrencyTest::unitOwner(executor, "SHFE.ag.2512"), owner);
    EXPECT_EQ(WtDiffExecuterConcurrencyTest::quiesceOwnerCount(executor), 1u);

    owner->requestQuiesce(); // Directly exercise a retryable plugin wait failure.
    EXPECT_FALSE(owner->waitQuiesced());
    EXPECT_EQ(WtDiffExecuterConcurrencyTest::unitOwner(executor, "SHFE.ag.2512"), owner);
    EXPECT_EQ(WtDiffExecuterConcurrencyTest::quiesceOwnerCount(executor), 1u);
    probe->setWaitResult(true);
    EXPECT_TRUE(owner->canWaitQuiescedOnCurrentThread());
    EXPECT_TRUE(owner->waitQuiesced());
    EXPECT_EQ(WtDiffExecuterConcurrencyTest::unitOwner(executor, "SHFE.ag.2512"), owner);
    EXPECT_EQ(WtDiffExecuterConcurrencyTest::quiesceOwnerCount(executor), 1u);
    EXPECT_TRUE(executor.completeQuiesceAfterGlobalPluginWait());
    EXPECT_TRUE(executor.completeQuiesceAfterGlobalPluginWait());
    EXPECT_TRUE(executor.waitQuiesced());
}

TEST(WtDiffExecuterConcurrencyTest, RealV2DiffPluginInitializesAndDirectHookCanShutdown)
{
    r71::R71GenEnv gen;
    r71::R71PluginDir pluginDir;
    ASSERT_TRUE(pluginDir.ok()) << "missing libWtExeFact plugin";

    r71::R71BaseDataMgr bd;
    bd.regisSession("TRADING");
    WTSSessionInfo* session = bd.session("TRADING");
    ASSERT_NE(session, nullptr);
    session->addTradingSection(900, 1500);
    WTSCommodityInfo* commodity = r71::makeComm("SHFE", "ag", "TRADING", TM_Both);
    commodity->setSessionInfo(session);
    WTSContractInfo* contract = r71::makeCt("SHFE", "ag", "ag2512");
    bd.regis(commodity, contract);
    bd.regisAlias("SHFE.ag.2512", contract);

    WtExecuterFactory pluginFactory;
    ASSERT_TRUE(pluginFactory.loadFactories(pluginDir.dir().c_str()));
    const std::string name = uniqueName("real_plugin_quiesce");
    const std::string json =
        "{\"scale\":1.0,\"poolsize\":0,\"policy\":{\"default\":{"
        "\"name\":\"WtExeFact.WtDiffMinImpactExeUnit\",\"offset\":0,"
        "\"expire\":0,\"pricemode\":0,\"span\":0,\"byrate\":false,"
        "\"lots\":1,\"rate\":0.0}}}";
    WTSVariant* params = WTSCfgLoader::load_from_content(json.c_str());
    ASSERT_NE(params, nullptr);
    r71::R71ExecStub stub;
    stub.comm_ = commodity;
    stub.sess_ = session;
    WtDiffExecuter executor(&pluginFactory, name.c_str(), NULL, &bd);
    executor.setStub(&stub);
    ASSERT_TRUE(executor.init(params));
    params->release();

    const ExecuteUnitPtr owner = WtDiffExecuterConcurrencyTest::requestUnit(executor,
        "SHFE.ag.2512");
    ASSERT_NE(owner, nullptr);
    EXPECT_EQ(executor.queryUnitInitStatus("SHFE.ag.2512").state,
        ExecUnitInitState::READY);

    executor.requestQuiesce();
    EXPECT_TRUE(executor.preflightQuiesceOnCurrentThread());
    EXPECT_TRUE(executor.drainHostQuiesced());
    std::vector<ExecQuiesceOwnerPtr> owners;
    ASSERT_TRUE(executor.snapshotQuiescePluginOwners(owners));
    ASSERT_EQ(owners.size(), 1u);
    EXPECT_FALSE(executor.canWaitQuiescedOnCurrentThread());
    EXPECT_FALSE(executor.waitQuiesced());
    EXPECT_FALSE(WtDiffExecuterConcurrencyTest::quiesceIncomplete(executor));
    owner->requestQuiesce();
    EXPECT_TRUE(owner->canWaitQuiescedOnCurrentThread());
    EXPECT_TRUE(owner->waitQuiesced());
    EXPECT_TRUE(executor.completeQuiesceAfterGlobalPluginWait());
    EXPECT_TRUE(executor.waitQuiesced());
}

TEST(WtDiffExecuterConcurrencyTest, QuiescePreflightRejectsSynchronousCallbackSelfWait)
{
    DiffProbeFactory fact;
    WtExecuterFactory pluginFactory;
    WtDiffExecuter executor(&pluginFactory, uniqueName("quiesce_sync_callback").c_str(), NULL, NULL);
    DiffProbeUnit* unit = WtDiffExecuterConcurrencyTest::registerUnit(executor, fact,
        "SHFE.ag.2512");
    bool callbackCanWait = true;
    bool callbackWaitResult = true;
    unit->setPositionHook([&](double) {
        executor.requestQuiesce();
        callbackCanWait = executor.canWaitQuiescedOnCurrentThread();
        callbackWaitResult = executor.waitQuiesced();
    });

    executor.on_position_changed("SHFE.ag.2512", 1.0);

    EXPECT_FALSE(callbackCanWait);
    EXPECT_FALSE(callbackWaitResult);
    EXPECT_TRUE(executor.canWaitQuiescedOnCurrentThread());
    EXPECT_TRUE(executor.waitQuiesced());
    EXPECT_TRUE(executor.waitQuiesced());
    EXPECT_EQ(unit->positions(), (std::vector<double>{1.0}));
}

TEST(WtDiffExecuterConcurrencyTest, QuiesceStaysIncompleteWhenCallbackDropsQueuedEvent)
{
    DiffProbeFactory fact;
    WtExecuterFactory pluginFactory;
    WtDiffExecuter executor(&pluginFactory, uniqueName("quiesce_callback_failure").c_str(), NULL, NULL);
    WtDiffExecuterConcurrencyTest::configurePool(executor, 2);
    DiffProbeUnit* unit = WtDiffExecuterConcurrencyTest::registerUnit(executor, fact,
        "SHFE.ag.2512");
    ExecuteUnitPtr unitOwner = WtDiffExecuterConcurrencyTest::unitOwner(executor,
        "SHFE.ag.2512");
    ASSERT_NE(unitOwner, nullptr);
    unit->setThrowOnPosition(true);
    const auto gate = std::make_shared<Gate>();
    unit->setPositionGate(gate);

    executor.on_position_changed("SHFE.ag.2512", 1.0);
    const bool callbackEntered = gate->waitForArrivals(1);
    if (!callbackEntered)
        gate->release();
    ASSERT_TRUE(callbackEntered);
    executor.on_position_changed("SHFE.ag.2512", 1.0);
    executor.requestQuiesce();
    executor.on_position_changed("SHFE.ag.2512", 1.0);
    gate->release();

    EXPECT_FALSE(executor.waitQuiesced());
    EXPECT_FALSE(executor.waitQuiesced());
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::target(executor, "SHFE.ag.2512"), 2.0);
    EXPECT_EQ(unit->positions(), (std::vector<double>{1.0}));
}

TEST(WtDiffExecuterConcurrencyTest, QueuedOlderSaveCannotOverwriteFailureTombstone)
{
    DiffProbeFactory fact;
    WtExecuterFactory pluginFactory;
    const std::string name = uniqueName("serialized_failure_save");
    const std::string path = std::string(WtHelper::getExecDataDir()) + name + ".json";
    std::error_code ec;
    fs::remove(path, ec);

    WtDiffExecuter executor(&pluginFactory, name.c_str(), NULL, NULL);
    WtDiffExecuterConcurrencyTest::registerUnit(executor, fact, "SHFE.ag.2512");
    WtDiffExecuterConcurrencyTest::setSnapshot(executor, "SHFE.ag.2512", 5.0, 5.0);
    ASSERT_TRUE(WtDiffExecuterConcurrencyTest::save(executor));
    WtDiffExecuterConcurrencyTest::setSnapshot(executor, "SHFE.ag.2512", 6.0, 6.0);

    const auto firstSaveGate = std::make_shared<Gate>();
	const auto failureMutationGate = std::make_shared<Gate>();
    std::atomic<int> hookCalls(0);
    std::atomic<int> commits(0);
    std::atomic<int> statusAtFirstCommit(-1);
    std::atomic<bool> failureStarted(false);
    std::atomic<bool> lateSaveStarted(false);
    std::atomic<bool> oldSaveSucceeded(false);
    std::atomic<bool> lateSaveSucceeded(false);
    std::thread lateSaveThread;
    WtDiffExecuterConcurrencyTest::setSaveHooks(executor,
        [&](bool hasBlockedUnits) {
            const int call = ++hookCalls;
            if (call == 1)
            {
                firstSaveGate->arriveAndWait();
                return true;
            }
            if (call == 2 && hasBlockedUnits)
            {
                // Queue another save while the failure snapshot is being
                // rejected. It must snapshot the blocked state after this
                // failure transaction releases the update lock.
                lateSaveThread = std::thread([&]() {
                    lateSaveStarted = true;
                    lateSaveSucceeded = WtDiffExecuterConcurrencyTest::save(executor);
                });
                while (!lateSaveStarted.load())
                    std::this_thread::yield();
                return false;
            }
            return true;
        },
        [&]() {
            if (commits.fetch_add(1) == 0)
            {
                statusAtFirstCommit = static_cast<int>(
                    executor.queryUnitInitStatus("SHFE.ag.2512").state);
            }
        },
        [&]() { failureMutationGate->arriveAndWait(); });

    std::thread olderSaveThread([&]() {
        oldSaveSucceeded = WtDiffExecuterConcurrencyTest::save(executor);
    });
    const bool olderSnapshotPaused = firstSaveGate->waitForArrivals(1);
    if (!olderSnapshotPaused)
        firstSaveGate->release();
    EXPECT_TRUE(olderSnapshotPaused);
    if (!olderSnapshotPaused)
    {
        olderSaveThread.join();
        WtDiffExecuterConcurrencyTest::setSaveHooks(executor, {}, {});
        fs::remove(path, ec);
        return;
    }

    std::thread failureThread([&]() {
        failureStarted = true;
        WtDiffExecuterConcurrencyTest::markFailed(executor, "SHFE.ag.2512",
            "controlled failure while an older snapshot is pending");
    });
    while (!failureStarted.load())
        std::this_thread::yield();
	const bool failedBeforeOldCommit = failureMutationGate->waitForArrivals(1,
		std::chrono::milliseconds(100));
	EXPECT_FALSE(failedBeforeOldCommit);
    firstSaveGate->release();
	failureMutationGate->release();

    olderSaveThread.join();
    failureThread.join();
    if (lateSaveThread.joinable())
        lateSaveThread.join();

    EXPECT_TRUE(oldSaveSucceeded.load());
    EXPECT_EQ(statusAtFirstCommit.load(), static_cast<int>(ExecUnitInitState::READY));
    EXPECT_EQ(hookCalls.load(), 3);
    EXPECT_TRUE(lateSaveSucceeded.load());
    const ExecUnitInitStatus failed = executor.queryUnitInitStatus("SHFE.ag.2512");
    EXPECT_EQ(failed.state, ExecUnitInitState::FAILED);
    EXPECT_EQ(executor.clearFailedUnitInitStatus("SHFE.ag.2512"),
        ExecUnitInitClearResult::RETRY_BLOCKED);
    EXPECT_EQ(WtDiffExecuterConcurrencyTest::savedRevision(executor),
        WtDiffExecuterConcurrencyTest::positionRevision(executor));

    WtDiffExecuter reloaded(&pluginFactory, name.c_str(), NULL, NULL);
    WtDiffExecuterConcurrencyTest::load(reloaded);
    EXPECT_EQ(reloaded.queryUnitInitStatus("SHFE.ag.2512").state,
        ExecUnitInitState::FAILED);
    EXPECT_EQ(reloaded.clearFailedUnitInitStatus("SHFE.ag.2512"),
        ExecUnitInitClearResult::RETRY_BLOCKED);
    WtDiffExecuterConcurrencyTest::setSaveHooks(executor, {}, {});
    fs::remove(path, ec);
}

TEST(WtDiffExecuterConcurrencyTest, FailedAtomicReplacementKeepsRevisionRetryable)
{
    const std::string name = uniqueName("atomic_save");
    const std::string path = std::string(WtHelper::getExecDataDir()) + name + ".json";
    std::error_code ec;
    fs::remove_all(path, ec);
    ASSERT_TRUE(fs::create_directory(path, ec));
    ASSERT_FALSE(ec);
    const std::string markerPath = path + "/keep.txt";
    {
        std::ofstream marker(markerPath.c_str(), std::ios::out | std::ios::trunc);
        marker << "previous snapshot placeholder";
    }

    WtExecuterFactory pluginFactory;
    WtDiffExecuter executor(&pluginFactory, name.c_str(), NULL, NULL);
    WtDiffExecuterConcurrencyTest::setSnapshot(executor, "SHFE.ag.2512", 12.0, 9.0);

    EXPECT_FALSE(WtDiffExecuterConcurrencyTest::save(executor));
    EXPECT_EQ(WtDiffExecuterConcurrencyTest::savedRevision(executor), 0u);
    EXPECT_TRUE(fs::is_directory(path, ec));
    EXPECT_TRUE(fs::exists(markerPath, ec));
    const std::string tempPrefix = fs::path(path).filename().string() + ".tmp.";
    for (const auto& item : fs::directory_iterator(fs::path(path).parent_path()))
        EXPECT_EQ(item.path().filename().string().find(tempPrefix), std::string::npos);

    ASSERT_GT(fs::remove_all(path, ec), 0u);
    EXPECT_TRUE(WtDiffExecuterConcurrencyTest::save(executor));
    ASSERT_TRUE(fs::is_regular_file(path, ec));
    EXPECT_EQ(WtDiffExecuterConcurrencyTest::savedRevision(executor),
        WtDiffExecuterConcurrencyTest::positionRevision(executor));
    rj::Document root;
    std::ifstream input(path.c_str());
    const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    root.Parse(content.c_str());
    EXPECT_FALSE(root.HasParseError());
    fs::remove(path, ec);
}

TEST(WtDiffExecuterConcurrencyTest, QuiesceIgnoresBroadcastReportsForUnownedCodes)
{
    WtExecuterFactory pluginFactory;
    const std::string name = uniqueName("unknown_broadcast");
    WtDiffExecuter executor(&pluginFactory, name.c_str(), NULL, NULL);

    WTSTickData* tick = WTSTickData::create("SHFE.unknown.2512");
    ASSERT_NE(tick, nullptr);
    executor.on_tick("SHFE.unknown.2512", tick);
    tick->release();
    executor.on_order(71u, "SHFE.unknown.2512", true, 1.0, 1.0, 3500.0);
    executor.on_entrust(71u, "SHFE.unknown.2512", false, "unknown unit");
    // A nonzero id from another executor still must not enter this ledger.
    executor.on_trade(71u, "SHFE.unknown.2512", true, 2.0, 3500.0);

    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::target(executor,
        "SHFE.unknown.2512"), 0.0);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::diff(executor,
        "SHFE.unknown.2512"), 0.0);
    executor.requestQuiesce();
    EXPECT_TRUE(executor.waitQuiesced());
}

TEST(WtDiffExecuterConcurrencyTest, BroadcastReportsOnlyUpdateTheOwningExecuterCode)
{
    DiffProbeFactory fact;
    WtExecuterFactory pluginFactory;
    constexpr const char* codeA = "SHFE.ag.2512";
    constexpr const char* codeB = "SHFE.rb.2510";
    const std::string nameA = uniqueName("broadcast_owner_a");
    const std::string nameB = uniqueName("broadcast_owner_b");
    const std::string pathA = std::string(WtHelper::getExecDataDir()) + nameA + ".json";
    const std::string pathB = std::string(WtHelper::getExecDataDir()) + nameB + ".json";
    std::error_code ec;
    fs::create_directories(fs::path(pathA).parent_path(), ec);
    fs::remove(pathA, ec);
    fs::remove(pathB, ec);

    WtDiffExecuter executorA(&pluginFactory, nameA.c_str(), NULL, NULL);
    WtDiffExecuter executorB(&pluginFactory, nameB.c_str(), NULL, NULL);
    DiffProbeUnit* unitA = WtDiffExecuterConcurrencyTest::registerUnit(executorA,
        fact, codeA);
    DiffProbeUnit* unitB = WtDiffExecuterConcurrencyTest::registerUnit(executorB,
        fact, codeB);
    WtDiffExecuterConcurrencyTest::setSnapshot(executorA, codeA, 10.0, 5.0);
    WtDiffExecuterConcurrencyTest::setSnapshot(executorB, codeB, 20.0, 8.0);

    // Simulate TraderAdapter broadcasting the same code-scoped report to both sinks.
    executorA.on_trade(101u, codeA, true, 2.0, 3500.0);
    executorB.on_trade(101u, codeA, true, 2.0, 3500.0);
    executorA.on_order(101u, codeA, true, 2.0, 0.0, 3500.0);
    executorB.on_order(101u, codeA, true, 2.0, 0.0, 3500.0);

    // localid == 0 is not an executor-owned WonderTrader order.
    executorA.on_trade(0u, codeA, true, 3.0, 3500.0);

    executorA.on_trade(202u, codeB, true, 2.0, 3500.0);
    executorB.on_trade(202u, codeB, true, 2.0, 3500.0);
    executorA.on_order(202u, codeB, true, 2.0, 0.0, 3500.0);
    executorB.on_order(202u, codeB, true, 2.0, 0.0, 3500.0);

    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::diff(executorA, codeA), 3.0);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::diff(executorA, codeB), 0.0);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::diff(executorB, codeA), 0.0);
    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::diff(executorB, codeB), 6.0);
    EXPECT_EQ(unitA->events(), (std::vector<std::string>{"trade", "order"}));
    EXPECT_EQ(unitB->events(), (std::vector<std::string>{"trade", "order"}));
    EXPECT_FALSE(WtDiffExecuterConcurrencyTest::quiesceIncomplete(executorA));
    EXPECT_FALSE(WtDiffExecuterConcurrencyTest::quiesceIncomplete(executorB));
    executorA.requestQuiesce();
    executorB.requestQuiesce();
    EXPECT_TRUE(executorA.waitQuiesced());
    EXPECT_TRUE(executorB.waitQuiesced());

    fs::remove(pathA, ec);
    fs::remove(pathB, ec);
}

TEST(WtDiffExecuterConcurrencyTest, QuiesceMarksLostReportsForOwnedLedgerCodeIncomplete)
{
    WtExecuterFactory pluginFactory;
    const std::string name = uniqueName("owned_without_unit");
    const std::string path = std::string(WtHelper::getExecDataDir()) + name + ".json";
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    fs::remove(path, ec);

    WtDiffExecuter executor(&pluginFactory, name.c_str(), NULL, NULL);
    WtDiffExecuterConcurrencyTest::setSnapshot(executor, "SHFE.ag.2512", 5.0, 5.0);
    executor.on_order(91u, "SHFE.ag.2512", true, 1.0, 1.0, 3500.0);
    executor.on_trade(91u, "SHFE.ag.2512", true, 1.0, 3500.0);

    EXPECT_DOUBLE_EQ(WtDiffExecuterConcurrencyTest::diff(executor,
        "SHFE.ag.2512"), 4.0);
    executor.requestQuiesce();
    EXPECT_FALSE(executor.waitQuiesced());
    fs::remove(path, ec);
}

TEST(WtDiffExecuterConcurrencyTest, QuiesceMarksUnitStateInvalidationDuringOrderOrEntrustIncomplete)
{
    DiffProbeFactory fact;
    WtExecuterFactory pluginFactory;
    constexpr const char* code = "SHFE.ag.2512";

    auto exerciseInvalidation = [&](const std::string& suffix, bool entrust) {
        const std::string name = uniqueName(suffix.c_str());
        const std::string path = std::string(WtHelper::getExecDataDir()) + name + ".json";
        std::error_code ec;
        fs::create_directories(fs::path(path).parent_path(), ec);
        fs::remove(path, ec);

        WtDiffExecuter executor(&pluginFactory, name.c_str(), NULL, NULL);
        WtDiffExecuterConcurrencyTest::registerUnit(executor, fact, code);
        ExecuteUnitPtr unitOwner = WtDiffExecuterConcurrencyTest::unitOwner(executor, code);
        ASSERT_NE(unitOwner, nullptr);
        DiffProbeUnit* unit = static_cast<DiffProbeUnit*>(unitOwner->self());
        ASSERT_NE(unit, nullptr);
        const auto afterLookup = std::make_shared<Gate>();
        WtDiffExecuterConcurrencyTest::setUnitLookupHook(executor,
            [afterLookup](const std::string&) { afterLookup->arriveAndWait(); });

        std::thread notification([&]() {
            if (entrust)
                executor.on_entrust(73u, code, false, "rejected");
            else
                executor.on_order(73u, code, true, 1.0, 1.0, 3500.0);
        });
        const bool reachedLookupBarrier = afterLookup->waitForArrivals(1);
        if (!reachedLookupBarrier)
            afterLookup->release();
        if (!reachedLookupBarrier)
        {
            notification.join();
            ADD_FAILURE() << "notification did not reach the post-lookup barrier";
            fs::remove(path, ec);
            return;
        }

        WtDiffExecuterConcurrencyTest::markFailed(executor, code,
            "controlled failure between unit and state lookup");
        EXPECT_FALSE(WtDiffExecuterConcurrencyTest::quiesceIncomplete(executor));
        EXPECT_EQ(WtDiffExecuterConcurrencyTest::savedRevision(executor),
            WtDiffExecuterConcurrencyTest::positionRevision(executor));
        afterLookup->release();
        notification.join();

        EXPECT_TRUE(WtDiffExecuterConcurrencyTest::quiesceIncomplete(executor));
        executor.requestQuiesce();
        EXPECT_FALSE(executor.waitQuiesced());
        EXPECT_TRUE(unit->events().empty());
        fs::remove(path, ec);
    };

    exerciseInvalidation("stale_order_state", false);
    exerciseInvalidation("stale_entrust_state", true);
}

TEST(WtDiffExecuterConcurrencyTest, RejectedPoolScheduleRunsFallbackSynchronously)
{
    struct RejectingPool
    {
        bool schedule(const std::function<void()>&)
        {
            ++scheduleCalls;
            return false;
        }
        unsigned scheduleCalls = 0;
    } pool;

    unsigned taskCalls = 0;
    const std::function<void()> task = [&]() { ++taskCalls; };
    const bool scheduled = WtExecuterSchedule::scheduleOrRunFallback(pool, task, task);

    EXPECT_FALSE(scheduled);
    EXPECT_EQ(pool.scheduleCalls, 1u);
    EXPECT_EQ(taskCalls, 1u);
}

} // namespace
