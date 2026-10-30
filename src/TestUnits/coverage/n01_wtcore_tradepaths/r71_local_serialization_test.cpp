/**
 * R71 Local executor callback ordering and position-map synchronization tests.
 *
 * The latch context observes real execution-unit callbacks without connecting
 * to a trading venue. Tests use a two-worker pool and bounded waits so failures
 * report instead of hanging the test process.
 */
#include "r71_wtcore_env.h"

#include <WTSUtils/WTSCfgLoader.h>

#include <WtCore/WtLocalExecuter.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <future>
#include <iostream>
#include <sstream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace wtp;

namespace {

namespace fs = std::filesystem;

const char* kAgCode = "SHFE.ag.2512";
const char* kRbCode = "SHFE.rb.2510";

class R71CurrentDirectoryLease
{
public:
	explicit R71CurrentDirectoryLease(const char* name)
		: _lock(_mutex), _previous(fs::current_path())
	{
		_directory = fs::path(r71::temp_root()) / name;
		std::error_code ec;
		fs::remove_all(_directory, ec);
		fs::create_directories(_directory, ec);
		if (ec)
			throw std::runtime_error("cannot create R71 VWAP working directory");
		fs::current_path(_directory);
	}

	~R71CurrentDirectoryLease()
	{
		std::error_code ec;
		fs::current_path(_previous, ec);
		fs::remove_all(_directory, ec);
	}

private:
	static std::mutex _mutex;
	std::unique_lock<std::mutex> _lock;
	fs::path _previous;
	fs::path _directory;
};

std::mutex R71CurrentDirectoryLease::_mutex;

bool writeVwapProfile(const std::string& sessionId, uint32_t tradingMinutes)
{
	std::ostringstream json;
	json << "{\"version\":2,\"profiles\":[{\"session_id\":\"" << sessionId
		<< "\",\"offset_minutes\":0,\"sections\":[{\"from\":900,\"to\":1500}],"
		<< "\"bucket_unit\":\"trading_minute\",\"value_mode\":\"cumulative\","
		<< "\"close_inclusive\":true,\"values\":[";
	for (uint32_t i = 0; i <= tradingMinutes; ++i)
	{
		if (i != 0)
			json << ',';
		json << 1;
	}
	json << "]}]}";
	std::ofstream out("Vwap_ag.txt", std::ios::out | std::ios::trunc);
	if (!out.is_open())
		return false;
	out << json.str();
	out.close();
	return out.good();
}

class R71LocalSerialProbe : public WtLocalExecuter
{
public:
	R71LocalSerialProbe(WtExecuterFactory* factory, const char* name, IDataManager* dataMgr)
		: WtLocalExecuter(factory, name, dataMgr)
	{
	}

	~R71LocalSerialProbe() override
	{
		if (!finishSingleExecutorQuiesce())
			std::terminate();
	}

	bool finishSingleExecutorQuiesce()
	{
		requestQuiesce();
		WtExecuterFactory* factory = quiesceFactory();
		IExecFactoryQuiesceLifecycle* factoryLifecycle = factory == NULL ? NULL :
			dynamic_cast<IExecFactoryQuiesceLifecycle*>(factory);
		if (factory != NULL && factoryLifecycle == NULL)
			return false;
		if (factoryLifecycle != NULL)
		{
			factoryLifecycle->closeFactoryConfigAdmissions();
			if (!factoryLifecycle->preflightQuiesceFactoriesOnCurrentThread())
				return false;
		}
		if (!preflightQuiesceOnCurrentThread())
			return false;
		if (!drainHostQuiesced())
			return false;

		std::vector<ExecQuiesceOwnerPtr> owners;
		std::vector<ExecPluginUnitSnapshot> factoryOwners;
		if (factoryLifecycle != NULL &&
			(!factoryLifecycle->closeUnitAdmissions() ||
				!factoryLifecycle->waitForFactoryOperationsQuiesced() ||
				!factoryLifecycle->snapshotLivePluginUnitOwners(factoryOwners)))
			return false;
		if (!snapshotQuiescePluginOwners(owners))
			return false;
		for (const ExecPluginUnitSnapshot& snapshot : factoryOwners)
		{
			if (snapshot.owner == NULL)
				return false;
			const bool duplicate = std::find_if(owners.begin(), owners.end(),
				[&snapshot](const ExecQuiesceOwnerPtr& current) {
					return current.get() == snapshot.owner.get();
				}) != owners.end();
			if (!duplicate)
				owners.push_back(snapshot.owner);
		}
		if (factoryLifecycle != NULL &&
			!factoryLifecycle->preflightQuiesceFactoriesOnCurrentThread())
			return false;
		for (const ExecQuiesceOwnerPtr& owner : owners)
			owner->requestQuiesce();
		for (const ExecQuiesceOwnerPtr& owner : owners)
			if (!owner->canWaitQuiescedOnCurrentThread())
				return false;
		bool allQuiesced = true;
		for (const ExecQuiesceOwnerPtr& owner : owners)
			if (!owner->waitQuiesced())
				allQuiesced = false;
		return allQuiesced && completeQuiesceAfterGlobalPluginWait();
	}

	void blockFirstReadyQueries(const std::set<std::string>& codes)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_gate_codes = codes;
		_gate_seen.clear();
		_gate_entered.clear();
		_gate_released = false;
	}

	void requestQuiesceOnNextQuery()
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_quiesce_on_next_query = true;
		_quiesce_attempted = false;
		_quiesce_wait_result = true;
		_quiesce_can_wait_result = true;
	}

	bool waitForQuiesceAttempt(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(_mutex);
		return _condition.wait_for(lock, timeout, [this]() { return _quiesce_attempted; });
	}

	bool quiesceWaitResult() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _quiesce_wait_result;
	}

	bool quiesceCanWaitResult() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _quiesce_can_wait_result;
	}

	bool waitForGateCount(std::size_t count, std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(_mutex);
		return _condition.wait_for(lock, timeout, [this, count]() {
			return _gate_entered.size() >= count;
		});
	}

	void releaseGate()
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_gate_released = true;
		_condition.notify_all();
	}

	void blockCreationReservation(const std::string& code)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_reservation_code = code;
		_reservation_entered = false;
		_reservation_released = false;
	}

	bool waitForReservation(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(_mutex);
		return _condition.wait_for(lock, timeout, [this]() { return _reservation_entered; });
	}

	void releaseReservation()
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_reservation_released = true;
		_condition.notify_all();
	}

	bool waitForRetainedUnit(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(_mutex);
		return _condition.wait_for(lock, timeout, [this]() { return !_retained_unit.expired(); });
	}

	bool hasRetainedUnit() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return !_retained_unit.expired();
	}

	void startControlledPluginWorker()
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_worker_started = false;
		_worker_release = false;
	}

	bool waitForControlledPluginWorker(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(_mutex);
		return _condition.wait_for(lock, timeout, [this]() { return _worker_started; });
	}

	void reenterOnNextQuery(const std::string& code, double delta, bool throwAfter)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_reenter_code = code;
		_reenter_delta = delta;
		_reenter_pending = true;
		_reenter_throw = throwAfter;
		_reenter_finished = false;
		_reenter_with_helper = false;
		_helper_finished = false;
	}

	void spawnAndJoinReentryOnNextQuery(const std::string& code, double delta)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_reenter_code = code;
		_reenter_delta = delta;
		_reenter_pending = true;
		_reenter_throw = false;
		_reenter_finished = false;
		_reenter_with_helper = true;
		_helper_finished = false;
	}

	bool waitForReentry(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(_mutex);
		return _condition.wait_for(lock, timeout, [this]() { return _reenter_finished; });
	}

	bool waitForHelper(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(_mutex);
		return _condition.wait_for(lock, timeout, [this]() { return _helper_finished; });
	}

	bool waitForTargetLogs(std::size_t count, std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(_mutex);
		return _condition.wait_for(lock, timeout, [this, count]() {
			return _target_logs.size() >= count;
		});
	}

	std::vector<std::string> targetLogs() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _target_logs;
	}

	std::vector<std::string> pluginLogs() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _plugin_logs;
	}

	std::vector<std::thread::id> gateThreads() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		std::vector<std::thread::id> result;
		for (const auto& entry : _gate_entered)
			result.push_back(entry.second);
		return result;
	}

	bool waitForQueryCount(const std::string& code, std::size_t count,
		std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(_mutex);
		return _condition.wait_for(lock, timeout, [this, &code, count]() {
			auto it = _query_count.find(code);
			return it != _query_count.end() && it->second >= count;
		});
	}

	bool waitForChannelReadyLogs(std::size_t count, std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(_mutex);
		return _condition.wait_for(lock, timeout, [this, count]() {
			return _channel_ready_logs >= count;
		});
	}

	std::size_t maxConcurrentQueries(const std::string& code) const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		auto it = _max_active_queries.find(code);
		return it == _max_active_queries.end() ? 0 : it->second;
	}

	double getUndoneQty(const char* stdCode) override
	{
		const std::string code = stdCode ? stdCode : "";
		bool shouldReenter = false;
		bool shouldThrow = false;
		bool shouldBlock = false;
		bool shouldRequestQuiesce = false;
		double reenterDelta = 0.0;
		bool spawnAndJoin = false;
		{
			std::lock_guard<std::mutex> lock(_mutex);
			const std::size_t active = ++_active_queries[code];
			++_query_count[code];
			_max_active_queries[code] = std::max(_max_active_queries[code], active);
			if (_gate_codes.count(code) != 0 && _gate_seen.insert(code).second)
			{
				_gate_entered.emplace_back(code, std::this_thread::get_id());
				shouldBlock = true;
				_condition.notify_all();
			}
			if (_quiesce_on_next_query)
			{
				_quiesce_on_next_query = false;
				shouldRequestQuiesce = true;
			}
			if (_reenter_pending && code == _reenter_code)
			{
				_reenter_pending = false;
				shouldReenter = true;
				shouldThrow = _reenter_throw;
				reenterDelta = _reenter_delta;
				spawnAndJoin = _reenter_with_helper;
			}
		}

		if (shouldRequestQuiesce)
		{
			requestQuiesce();
			const bool canWait = canWaitQuiescedOnCurrentThread();
			const bool waitResult = waitQuiesced();
			std::lock_guard<std::mutex> lock(_mutex);
			_quiesce_can_wait_result = canWait;
			_quiesce_wait_result = waitResult;
			_quiesce_attempted = true;
			_condition.notify_all();
		}

		if (shouldReenter)
		{
			if (spawnAndJoin)
			{
				std::promise<void> completed;
				std::future<void> done = completed.get_future();
				std::thread helper([this, code, reenterDelta, signal = std::move(completed)]() mutable {
					on_position_changed(code.c_str(), reenterDelta);
					{
						std::lock_guard<std::mutex> lock(_mutex);
						_helper_finished = true;
						_condition.notify_all();
					}
					signal.set_value();
				});
				const bool joinedWithoutDeadlock = done.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
				if (joinedWithoutDeadlock)
					helper.join();
				else
				{
					helper.detach();
					shouldThrow = true;
				}
				std::lock_guard<std::mutex> lock(_mutex);
				_reenter_finished = joinedWithoutDeadlock;
				_condition.notify_all();
			}
			else
			{
				// Same-thread plugin re-entry must queue without recursively draining.
				on_position_changed(code.c_str(), reenterDelta);
				std::lock_guard<std::mutex> lock(_mutex);
				_reenter_finished = true;
				_condition.notify_all();
			}
		}

		if (shouldBlock)
		{
			std::unique_lock<std::mutex> lock(_mutex);
			_condition.wait(lock, [this]() { return _gate_released; });
		}

		{
			std::lock_guard<std::mutex> lock(_mutex);
			--_active_queries[code];
			_condition.notify_all();
		}
		if (shouldThrow)
			throw std::runtime_error("R71 injected Local channel callback failure");
		return 0.0;
	}

	void writeLog(const char* message) override
	{
		if (message != NULL)
		{
			std::lock_guard<std::mutex> lock(_mutex);
			_plugin_logs.emplace_back(message);
			if (std::strstr(message, "is set tb be") != NULL)
				_target_logs.emplace_back(message);
			if (std::strstr(message, "channel reconciliation deferred") != NULL)
				++_channel_ready_logs;
			_condition.notify_all();
		}
		WtLocalExecuter::writeLog(message);
	}

	void onUnitGenerationRetainedForQuiesce(const std::string& /*code*/,
		uint64_t /*generation*/, const ExecuteUnitPtr& unit) override
	{
		bool startWorker = false;
		{
			std::lock_guard<std::mutex> lock(_mutex);
			_retained_unit = unit;
			_fake_worker_unit = unit != NULL ? unit->self() : NULL;
			startWorker = !_fake_plugin_worker.joinable() && _worker_release == false;
			_condition.notify_all();
		}
		if (startWorker)
		{
			_fake_plugin_worker = std::thread([this]() {
				std::unique_lock<std::mutex> lock(_mutex);
				_worker_started = _fake_worker_unit != NULL;
				_condition.notify_all();
				_condition.wait(lock, [this]() { return _worker_release; });
			});
		}
	}

	void onUnitCreationReserved(const std::string& code) override
	{
		std::unique_lock<std::mutex> lock(_mutex);
		if (_reservation_code != code || _reservation_entered)
			return;
		_reservation_entered = true;
		_condition.notify_all();
		_condition.wait(lock, [this]() { return _reservation_released; });
		_reservation_code.clear();
	}

private:
	mutable std::mutex _mutex;
	std::condition_variable _condition;
	std::set<std::string> _gate_codes;
	std::set<std::string> _gate_seen;
	std::vector<std::pair<std::string, std::thread::id>> _gate_entered;
	bool _gate_released = true;
	bool _quiesce_on_next_query = false;
	bool _quiesce_attempted = false;
	bool _quiesce_wait_result = true;
	bool _quiesce_can_wait_result = true;
	std::string _reenter_code;
	double _reenter_delta = 0.0;
	bool _reenter_pending = false;
	bool _reenter_throw = false;
	bool _reenter_finished = false;
	bool _reenter_with_helper = false;
	bool _helper_finished = false;
	std::weak_ptr<ExeUnitWrapper> _retained_unit;
	std::thread _fake_plugin_worker;
	const ExecuteUnit* _fake_worker_unit = NULL;
	bool _worker_started = false;
	bool _worker_release = true;
	std::string _reservation_code;
	bool _reservation_entered = false;
	bool _reservation_released = true;
	std::size_t _channel_ready_logs = 0;
	std::map<std::string, std::size_t> _active_queries;
	std::map<std::string, std::size_t> _max_active_queries;
	std::map<std::string, std::size_t> _query_count;
	std::vector<std::string> _target_logs;
	std::vector<std::string> _plugin_logs;
};

class R71GateReleaseGuard
{
public:
	explicit R71GateReleaseGuard(R71LocalSerialProbe& local) : _local(local) {}
	~R71GateReleaseGuard() { _local.releaseGate(); }

	R71GateReleaseGuard(const R71GateReleaseGuard&) = delete;
	R71GateReleaseGuard& operator=(const R71GateReleaseGuard&) = delete;

private:
	R71LocalSerialProbe& _local;
};

class R71LocalSerialTest : public ::testing::Test
{
protected:
	r71::R71GenEnv genEnv;
	r71::R71PluginDir pluginDir;
	r71::R71TraderEnv traderEnv;
	r71::R71ExecStub stub;
	r71::R71DataMgr dataMgr;
	WtExecuterFactory factory;
	TraderAdapter trader;
	WTSCommodityInfo* commodity = NULL;
	WTSSessionInfo* session = NULL;

	void SetUp() override
	{
		ASSERT_TRUE(pluginDir.ok()) << "缺少 libWtExeFact，无法创建真实执行单元";
		ASSERT_TRUE(factory.loadFactories(pluginDir.dir().c_str()));

		traderEnv.bd.regisSession("TRADING");
		session = traderEnv.bd.session("TRADING");
		ASSERT_NE(session, nullptr);
		session->addTradingSection(900, 1500);

		commodity = r71::makeComm("SHFE", "ag", "TRADING", TM_Both);
		commodity->setSessionInfo(session);
		WTSContractInfo* agContract = r71::makeCt("SHFE", "ag", "ag2512");
		traderEnv.bd.regis(commodity, agContract);
		traderEnv.bd.regisAlias(kAgCode, agContract);

		WTSCommodityInfo* rb = r71::makeComm("SHFE", "rb", "TRADING", TM_Both);
		rb->setSessionInfo(session);
		WTSContractInfo* rbContract = r71::makeCt("SHFE", "rb", "rb2510");
		traderEnv.bd.regis(rb, rbContract);
		traderEnv.bd.regisAlias(kRbCode, rbContract);

		stub.comm_ = commodity;
		stub.sess_ = session;
		stub.hot_ = &traderEnv.hot;
		ASSERT_TRUE(traderEnv.build(trader, "r71_local_serial"));
	}

	WTSVariant* config(const char* name, uint32_t poolSize,
		const char* unitName = "WtExeFact.WtMinImpactExeUnit", bool strictSync = false)
	{
		const bool vwap = std::strstr(unitName, "VWap") != NULL;
		const bool stock = std::strstr(unitName, "StockMinImpact") != NULL;
		const std::string unitConfig = vwap
			? "\"lots\":1,\"minopenlots\":0,\"begin_time\":900,\"end_time\":1500,"
			  "\"total_times\":1,\"price_mode\":0,\"price_offset\":0,\"tail_secs\":0,\"ord_sticky\":0"
			: stock
			? "\"lots\":1,\"min_order\":1,\"minopenlots\":1,\"offset\":0,\"expire\":0,"
			  "\"pricemode\":0,\"span\":0,\"byrate\":false"
			: "\"lots\":1,\"minopenlots\":1,\"offset\":0,\"expire\":0,"
			  "\"pricemode\":0,\"span\":0,\"byrate\":false";
		const std::string json = std::string("{\"scale\":1.0,\"strict_sync\":")
			+ (strictSync ? "true" : "false") + ",\"poolsize\":"
			+ std::to_string(poolSize)
			+ ",\"policy\":{\"default\":{\"name\":\"" + unitName + "\"," + unitConfig + "}}}";
		const std::string path = r71::temp_root() + name + ".json";
		if (!r71::write_file(path, json))
			return NULL;
		return WTSCfgLoader::load_from_file(path.c_str());
	}

	bool initLocal(R71LocalSerialProbe& local, const char* name, uint32_t poolSize,
		bool attachTrader = false,
		const char* unitName = "WtExeFact.WtMinImpactExeUnit", bool strictSync = false)
	{
		local.setStub(&stub);
		WTSVariant* cfg = config(name, poolSize, unitName, strictSync);
		if (cfg == NULL)
			return false;
		const bool ok = local.init(cfg);
		cfg->release();
		local.setTrader(attachTrader ? &trader : NULL);
		// addSink atomically registers the sink and delivers current channel state.
		if (attachTrader)
			trader.addSink(&local);
		return ok;
	}

	bool initVwapLocal(R71LocalSerialProbe& local, const char* name)
	{
		local.setStub(&stub);
		WTSVariant* cfg = config(name, 2, "WtExeFact.WtVWapExeUnit");
		if (cfg == NULL)
			return false;
		const bool ok = local.init(cfg);
		cfg->release();
		local.setTrader(&trader);
		trader.addSink(&local);
		return ok;
	}
};

TEST_F(R71LocalSerialTest, QuiesceDrainsAcceptedCallbacksAndRejectsNewIngress)
{
	R71LocalSerialProbe local(&factory, "r71_local_quiesce_drain", &dataMgr);
	ASSERT_TRUE(initLocal(local, "r71_local_quiesce_drain", 2));
	local.on_position_changed(kAgCode, 0.0);
	ASSERT_EQ(local.queryUnitInitStatus(kAgCode).state, ExecUnitInitState::READY);

	local.blockFirstReadyQueries({kAgCode});
	R71GateReleaseGuard gateRelease(local);
	local.on_channel_ready();
	const bool entered = local.waitForGateCount(1, std::chrono::seconds(5));
	if (!entered)
		local.releaseGate();
	ASSERT_TRUE(entered) << "channel-ready callback did not enter its controlled gate";

	// The accepted target update queues behind the blocked callback. Quiesce must
	// retain and drain it, while the later target update must be rejected.
	local.on_position_changed(kAgCode, 2.0);
	local.requestQuiesce();
	EXPECT_TRUE(local.preflightQuiesceOnCurrentThread())
		<< "stable factory preflight failed while only an accepted host callback was running";
	local.on_position_changed(kAgCode, 10.0);
	std::future<bool> drained = std::async(std::launch::async, [&local]() {
		return local.drainHostQuiesced();
	});
	const bool waitedForCallback = drained.wait_for(std::chrono::milliseconds(50)) ==
		std::future_status::timeout;
	local.releaseGate();
	const bool didDrain = drained.get();

	EXPECT_TRUE(waitedForCallback)
		<< "waitQuiesced returned before the accepted plugin callback was released";
	EXPECT_TRUE(didDrain);
	ASSERT_TRUE(local.waitForTargetLogs(1, std::chrono::seconds(5)));
	const auto targets = local.targetLogs();
	ASSERT_EQ(targets.size(), 1u);
	EXPECT_NE(targets[0].find("is set tb be 2"), std::string::npos)
		<< "the target update accepted before quiesce was lost";
	std::vector<ExecQuiesceOwnerPtr> owners;
	ASSERT_TRUE(local.snapshotQuiescePluginOwners(owners));
	ASSERT_EQ(owners.size(), 1u) << "the real initialized unit was missing from the stable owner set";
	EXPECT_TRUE(local.canWaitQuiescedOnCurrentThread());
	EXPECT_TRUE(local.finishSingleExecutorQuiesce())
		<< "single-executor test coordinator did not finish the real plugin lifecycle";
	EXPECT_TRUE(local.waitQuiesced()) << "quiesced wait should be idempotent";
}

TEST_F(R71LocalSerialTest, QuiesceWaitFromPluginCallbackReturnsFalseWithoutBlocking)
{
	R71LocalSerialProbe local(&factory, "r71_local_quiesce_reentrant", &dataMgr);
	ASSERT_TRUE(initLocal(local, "r71_local_quiesce_reentrant", 2));
	local.on_position_changed(kAgCode, 0.0);
	ASSERT_EQ(local.queryUnitInitStatus(kAgCode).state, ExecUnitInitState::READY);

	local.blockFirstReadyQueries({kAgCode});
	local.requestQuiesceOnNextQuery();
	R71GateReleaseGuard gateRelease(local);
	local.on_channel_ready();
	const bool attempted = local.waitForQuiesceAttempt(std::chrono::seconds(5));
	const bool entered = local.waitForGateCount(1, std::chrono::seconds(5));
	if (!entered)
		local.releaseGate();
	ASSERT_TRUE(attempted) << "plugin callback did not try to wait for quiescence";
	ASSERT_TRUE(entered) << "channel-ready callback did not enter its controlled gate";

	const bool callbackCanWait = local.quiesceCanWaitResult();
	const bool callbackWaited = local.quiesceWaitResult();
	std::future<bool> drained = std::async(std::launch::async, [&local]() {
		return local.drainHostQuiesced();
	});
	const bool waitedForCallback = drained.wait_for(std::chrono::milliseconds(50)) ==
		std::future_status::timeout;
	local.releaseGate();
	const bool didDrain = drained.get();

	EXPECT_FALSE(callbackCanWait)
		<< "callback preflight allowed waiting on its own worker";
	EXPECT_FALSE(callbackWaited)
		<< "plugin callback waited for its own pool drain";
	EXPECT_TRUE(waitedForCallback)
		<< "control-thread wait returned before the plugin callback was released";
	EXPECT_TRUE(didDrain);
	EXPECT_TRUE(local.canWaitQuiescedOnCurrentThread());
	EXPECT_TRUE(local.finishSingleExecutorQuiesce());
}

TEST_F(R71LocalSerialTest, QuiesceWaitsForIngressProducerBeforeDrainingAcceptedTarget)
{
	R71LocalSerialProbe local(&factory, "r71_local_quiesce_ingress", &dataMgr);
	ASSERT_TRUE(initLocal(local, "r71_local_quiesce_ingress", 2));
	local.blockCreationReservation(kAgCode);
	std::thread producer([&local]() { local.on_position_changed(kAgCode, 1.0); });
	const bool reserved = local.waitForReservation(std::chrono::seconds(5));
	if (!reserved)
	{
		local.releaseReservation();
		producer.join();
	}
	ASSERT_TRUE(reserved) << "target producer did not block after entering ingress";

	local.requestQuiesce();
	local.on_position_changed(kAgCode, 5.0); // rejected after the gate closes
	EXPECT_TRUE(local.preflightQuiesceOnCurrentThread())
		<< "control-thread preflight should only reject waiting from its own ingress/callback stack";
	std::future<bool> drained = std::async(std::launch::async, [&local]() {
		return local.drainHostQuiesced();
	});
	const bool waitedForIngress = drained.wait_for(std::chrono::milliseconds(50)) ==
		std::future_status::timeout;
	local.releaseReservation();
	producer.join();
	const bool didDrain = drained.get();

	EXPECT_TRUE(waitedForIngress)
		<< "waitQuiesced returned while a pre-quiesce ingress producer was still active";
	EXPECT_TRUE(didDrain);
	ASSERT_TRUE(local.waitForTargetLogs(1, std::chrono::seconds(5)));
	const auto targets = local.targetLogs();
	ASSERT_EQ(targets.size(), 1u);
	EXPECT_NE(targets[0].find("is set tb be 1"), std::string::npos)
		<< "the target accepted by the blocked ingress producer was not drained";
	EXPECT_TRUE(local.finishSingleExecutorQuiesce());
}

TEST_F(R71LocalSerialTest, NoPoolSynchronousCallbackCanRequestQuiesce)
{
	R71LocalSerialProbe local(&factory, "r71_local_quiesce_sync", &dataMgr);
	ASSERT_TRUE(initLocal(local, "r71_local_quiesce_sync", 0));
	local.on_position_changed(kAgCode, 0.0);
	ASSERT_EQ(local.queryUnitInitStatus(kAgCode).state, ExecUnitInitState::READY);
	local.requestQuiesceOnNextQuery();
	local.on_channel_ready(); // no pool: callback/fallback executes on this ingress thread

	ASSERT_TRUE(local.waitForQuiesceAttempt(std::chrono::seconds(1)));
	EXPECT_FALSE(local.quiesceCanWaitResult())
		<< "synchronous callback preflight allowed waiting on its active ingress";
	EXPECT_FALSE(local.quiesceWaitResult())
		<< "synchronous callback waited for its own ingress to return";
	EXPECT_TRUE(local.canWaitQuiescedOnCurrentThread());
	EXPECT_TRUE(local.finishSingleExecutorQuiesce())
		<< "control-thread quiescence did not complete after synchronous callback returned";
}

TEST_F(R71LocalSerialTest, SameCodeCallbacksStayFifoAndReentrantCallbackDoesNotWait)
{
	R71LocalSerialProbe local(&factory, "r71_local_fifo", &dataMgr);
	ASSERT_TRUE(initLocal(local, "r71_local_fifo", 2));
	local.on_position_changed(kAgCode, 0.0); // create and warm the unit before latching

	local.on_channel_lost();
	local.blockFirstReadyQueries({kAgCode});
	R71GateReleaseGuard gateRelease(local);
	local.reenterOnNextQuery(kAgCode, 1.0, false);
	local.on_channel_ready();
	ASSERT_TRUE(local.waitForGateCount(1, std::chrono::seconds(5)));
	ASSERT_TRUE(local.waitForReentry(std::chrono::seconds(5)))
		<< "plugin callback re-entry blocked instead of queueing";

	local.on_position_changed(kAgCode, 2.0);
	local.on_position_changed(kAgCode, 3.0);
	EXPECT_TRUE(local.targetLogs().empty())
		<< "same-code position callbacks ran concurrently with channel recovery";
	local.releaseGate();
	ASSERT_TRUE(local.waitForTargetLogs(3, std::chrono::seconds(5)));

	const std::vector<std::string> updates = local.targetLogs();
	ASSERT_EQ(updates.size(), 3u);
	EXPECT_NE(updates[0].find("is set tb be 1"), std::string::npos);
	EXPECT_NE(updates[1].find("is set tb be 3"), std::string::npos);
	EXPECT_NE(updates[2].find("is set tb be 6"), std::string::npos);
	EXPECT_EQ(local.maxConcurrentQueries(kAgCode), 1u);
}

TEST_F(R71LocalSerialTest, InitializationCallbackCanSpawnAndJoinSameCodePositionEvent)
{
	R71LocalSerialProbe local(&factory, "r71_local_spawn_join", &dataMgr);
	ASSERT_TRUE(initLocal(local, "r71_local_spawn_join", 2, true));
	local.spawnAndJoinReentryOnNextQuery(kAgCode, 1.0);

	// Creating the unit synchronously invokes the queued initial ready callback.
	// That plugin callback spawns a helper thread which re-enters this same code
	// and joins it; initialization must queue the event without waiting on itself.
	local.on_position_changed(kAgCode, 0.0);
	ASSERT_TRUE(local.waitForHelper(std::chrono::seconds(5)))
		<< "helper thread did not exit after the initialization attempt settled";
	ASSERT_TRUE(local.waitForReentry(std::chrono::seconds(5)))
		<< "initialization callback deadlocked while joining helper-thread re-entry";
	const ExecUnitInitStatus ready = local.queryUnitInitStatus(kAgCode);
	ASSERT_EQ(ready.state, ExecUnitInitState::READY);
	ASSERT_TRUE(local.waitForTargetLogs(1, std::chrono::seconds(5)));
	const std::vector<std::string> updates = local.targetLogs();
	ASSERT_EQ(updates.size(), 1u);
	EXPECT_NE(updates[0].find("is set tb be 1"), std::string::npos);
}

TEST_F(R71LocalSerialTest, AccountEventQueuedDuringInitializationRunsAfterSuccessfulReady)
{
	R71LocalSerialProbe local(&factory, "r71_local_account_during_init", &dataMgr);
	ASSERT_TRUE(initLocal(local, "r71_local_account_during_init", 2, true,
		"WtExeFact.WtStockMinImpactExeUnit"));
	local.blockFirstReadyQueries({kAgCode});

	std::thread creator([&local]() { local.on_position_changed(kAgCode, 0.0); });
	const bool entered = local.waitForGateCount(1, std::chrono::seconds(5));
	if (!entered)
	{
		local.releaseGate();
		creator.join();
	}
	ASSERT_TRUE(entered) << "stock unit did not block inside its channel-ready callback";

	local.on_account("CNY", 0, 1, 1, 100, 0, 0, 0, 0, 0, 0);
	const auto pendingLogs = local.pluginLogs();
	EXPECT_FALSE(std::any_of(pendingLogs.begin(), pendingLogs.end(), [](const std::string& log) {
		return log.find("avaliable update") != std::string::npos;
	}))
		<< "initializing unit received account data before its ready callback finished";
	local.releaseGate();
	creator.join();

	ASSERT_EQ(local.queryUnitInitStatus(kAgCode).state, ExecUnitInitState::READY);
	const auto logs = local.pluginLogs();
	EXPECT_TRUE(std::any_of(logs.begin(), logs.end(), [](const std::string& log) {
		return log.find("avaliable update 0->:100") != std::string::npos;
	})) << "successful generation did not receive the queued account update";
}

TEST_F(R71LocalSerialTest, LatestTargetReconciliationPrecedesTickQueuedDuringInitialization)
{
	R71LocalSerialProbe local(&factory, "r71_local_target_before_tick", &dataMgr);
	ASSERT_TRUE(initLocal(local, "r71_local_target_before_tick", 2, true));
	local.blockFirstReadyQueries({kAgCode});

	std::thread creator([&local]() { local.on_position_changed(kAgCode, 0.0); });
	const bool entered = local.waitForGateCount(1, std::chrono::seconds(5));
	if (!entered)
	{
		local.releaseGate();
		creator.join();
	}
	ASSERT_TRUE(entered) << "initial unit callback did not reach the controlled gate";

	WTSTickData* tick = r71::makeTick("SHFE", kAgCode, 20250110, 20250110,
		93100000, 3500.0, 3499.0, 3501.0);
	if (tick == NULL)
	{
		local.releaseGate();
		creator.join();
	}
	ASSERT_NE(tick, nullptr);
	local.on_tick(kAgCode, tick);
	tick->release();
	// This target update arrives after the tick entered the lifecycle queue. The
	// coalesced authoritative-target barrier must still be applied before it.
	local.on_position_changed(kAgCode, 1.0);
	local.releaseGate();
	creator.join();

	ASSERT_TRUE(local.waitForTargetLogs(1, std::chrono::seconds(5)));
	const auto targetLogs = local.targetLogs();
	ASSERT_EQ(targetLogs.size(), 1u);
	EXPECT_NE(targetLogs[0].find("is set tb be 1"), std::string::npos);
	const auto pluginLogs = local.pluginLogs();
	EXPECT_TRUE(std::any_of(pluginLogs.begin(), pluginLogs.end(), [](const std::string& log) {
		return log.find("No lastest tick data") != std::string::npos;
	})) << "target callback ran after the queued tick instead of before it";
	EXPECT_GE(traderEnv.api.n_insert, 1u)
		<< "the queued fresh tick did not execute against the reconciled target";
}

TEST_F(R71LocalSerialTest, TargetReservationCapturesTicksBeforeCreatorStarts)
{
	R71LocalSerialProbe local(&factory, "r71_local_reserved_target_before_tick", &dataMgr);
	ASSERT_TRUE(initLocal(local, "r71_local_reserved_target_before_tick", 2, true));
	local.blockCreationReservation(kAgCode);

	std::thread creator([&local]() { local.on_position_changed(kAgCode, 1.0); });
	const bool reserved = local.waitForReservation(std::chrono::seconds(5));
	if (!reserved)
	{
		local.releaseReservation();
		creator.join();
	}
	ASSERT_TRUE(reserved) << "target update did not publish its lifecycle reservation";

	WTSTickData* tick = r71::makeTick("SHFE", kAgCode, 20250110, 20250110,
		93100000, 3500.0, 3499.0, 3501.0);
	if (tick == NULL)
	{
		local.releaseReservation();
		creator.join();
	}
	ASSERT_NE(tick, nullptr);
	local.on_tick(kAgCode, tick);
	tick->release();
	local.releaseReservation();
	creator.join();

	ASSERT_EQ(local.queryUnitInitStatus(kAgCode).state, ExecUnitInitState::READY);
	ASSERT_TRUE(local.waitForTargetLogs(1, std::chrono::seconds(5)));
	const auto targets = local.targetLogs();
	ASSERT_EQ(targets.size(), 1u);
	EXPECT_NE(targets.front().find("is set tb be 1"), std::string::npos);
	EXPECT_GE(traderEnv.api.n_insert, 1u)
		<< "the tick arriving after target publication was lost before lifecycle creation";
}

TEST_F(R71LocalSerialTest, StrictSyncUnmanagedHoldBecomesAuthoritativeZeroTarget)
{
	R71LocalSerialProbe local(&factory, "r71_local_strict_zero_target", &dataMgr);
	ASSERT_TRUE(initLocal(local, "r71_local_strict_zero_target", 2, true,
		"WtExeFact.WtStockMinImpactExeUnit", true));
	WTSContractInfo* contract = traderEnv.bd.getContract("rb2510", "SHFE");
	ASSERT_NE(contract, nullptr);
	WTSPositionItem* position = WTSPositionItem::create("rb2510", "CNY", "SHFE");
	ASSERT_NE(position, nullptr);
	position->setContractInfo(contract);
	position->setDirection(WDT_LONG);
	position->setNewPosition(10.0);
	position->setAvailNewPos(10.0);
	WTSArray* positions = WTSArray::create();
	ASSERT_NE(positions, nullptr);
	positions->append(position);
	position->release();
	trader.onRspPosition(positions);
	positions->release();
	wt_hashmap<std::string, double> noTargets;
	local.set_position(noTargets);

	ASSERT_TRUE(local.waitForTargetLogs(1, std::chrono::seconds(5)));
	const auto targets = local.targetLogs();
	ASSERT_EQ(targets.size(), 1u);
	EXPECT_NE(targets.front().find(kRbCode), std::string::npos);
	EXPECT_NE(targets.front().find("is set tb be 0"), std::string::npos);
	EXPECT_EQ(local.queryUnitInitStatus(kRbCode).state, ExecUnitInitState::READY);
}

TEST_F(R71LocalSerialTest, DifferentContractsCanRunCallbacksInParallel)
{
	R71LocalSerialProbe local(&factory, "r71_local_parallel", &dataMgr);
	ASSERT_TRUE(initLocal(local, "r71_local_parallel", 2));
	local.on_position_changed(kAgCode, 0.0);
	local.on_position_changed(kRbCode, 0.0);

	local.on_channel_lost();
	local.blockFirstReadyQueries({kAgCode, kRbCode});
	local.on_channel_ready();
	ASSERT_TRUE(local.waitForGateCount(2, std::chrono::seconds(5)))
		<< "two contract callbacks did not enter concurrently with poolsize=2";
	const std::vector<std::thread::id> threads = local.gateThreads();
	ASSERT_EQ(threads.size(), 2u);
	EXPECT_NE(threads[0], threads[1]);
	local.releaseGate();
}

TEST_F(R71LocalSerialTest, ChannelEventsContinueAfterBlockedReadyCallback)
{
	R71LocalSerialProbe local(&factory, "r71_local_epoch", &dataMgr);
	ASSERT_TRUE(initLocal(local, "r71_local_epoch", 2));
	local.on_position_changed(kAgCode, 0.0);

	local.on_channel_lost();
	local.blockFirstReadyQueries({kAgCode});
	local.on_channel_ready();
	ASSERT_TRUE(local.waitForGateCount(1, std::chrono::seconds(5)));
	local.on_channel_lost();
	local.on_channel_ready();
	local.releaseGate();

	// Both ready callbacks run around the queued lost event. Each ready callback
	// queries twice; the lost callback is made observable with the real VWAP unit
	// in the next test. Per-code callbacks must never overlap.
	ASSERT_TRUE(local.waitForQueryCount(kAgCode, 4, std::chrono::seconds(5)));
	EXPECT_EQ(local.maxConcurrentQueries(kAgCode), 1u);
}

TEST_F(R71LocalSerialTest, ChannelLostTransitionIsDeliveredBeforeQueuedTickAndRecovery)
{
	R71CurrentDirectoryLease cwd("r71_vwap_channel_fifo");
	ASSERT_TRUE(writeVwapProfile("TRADING", session->getTradingMins()));

	R71LocalSerialProbe local(&factory, "r71_vwap_channel_fifo", &dataMgr);
	ASSERT_TRUE(initVwapLocal(local, "r71_vwap_channel_fifo"));
	local.on_position_changed(kAgCode, 0.0); // create the real VWAP unit
	ASSERT_TRUE(local.waitForChannelReadyLogs(1, std::chrono::seconds(5)));

	// Hold the first valid tick inside the plugin while transitions and another
	// tick queue behind it. The target is set only after this first tick enters,
	// so it cannot place an order before the queued lost event is observed.
	local.blockFirstReadyQueries({kAgCode});
	WTSTickData* first = r71::makeTick("SHFE", kAgCode, 20250110, 20250110,
		93100000, 3500.0, 3499.0, 3501.0);
	ASSERT_NE(first, nullptr);
	local.on_tick(kAgCode, first);
	first->release();
	ASSERT_TRUE(local.waitForGateCount(1, std::chrono::seconds(5)));

	local.on_position_changed(kAgCode, 1.0);
	local.on_channel_lost();
	WTSTickData* duringLoss = r71::makeTick("SHFE", kAgCode, 20250110, 20250110,
		93200000, 3500.0, 3499.0, 3501.0);
	ASSERT_NE(duringLoss, nullptr);
	local.on_tick(kAgCode, duringLoss);
	duringLoss->release();
	local.on_channel_ready();
	local.releaseGate();

	ASSERT_TRUE(local.waitForChannelReadyLogs(2, std::chrono::seconds(5)))
		<< "recovery ready callback did not run after the queued loss tick";
	EXPECT_EQ(traderEnv.api.n_insert, 0u)
		<< "the tick queued after channel loss was processed as if the unit were ready";

	// A fresh tick after recovery is executable. Queue another loss/ready pair
	// behind it; the third ready log is a FIFO barrier after order submission.
	WTSTickData* afterRecovery = r71::makeTick("SHFE", kAgCode, 20250110, 20250110,
		93300000, 3500.0, 3499.0, 3501.0);
	ASSERT_NE(afterRecovery, nullptr);
	local.on_tick(kAgCode, afterRecovery);
	afterRecovery->release();
	local.on_channel_lost();
	local.on_channel_ready();
	ASSERT_TRUE(local.waitForChannelReadyLogs(3, std::chrono::seconds(5)));
	EXPECT_GE(traderEnv.api.n_insert, 1u)
		<< "a fresh tick after the delivered recovery transition did not execute";
}

TEST_F(R71LocalSerialTest, RolloverClearDuringFailedTeardownIsRetainedWithoutUnsafeReplay)
{
	EXPECT_EXIT({
		R71LocalSerialProbe* local = new R71LocalSerialProbe(
			&factory, "r71_local_clear_during_failed_teardown", &dataMgr);
		if (!initLocal(*local, "r71_local_clear_during_failed_teardown", 2, true,
			"WtExeFact.WtMinImpactExeUnit"))
			std::exit(41);
		local->on_position_changed(kAgCode, 0.0);
		if (local->queryUnitInitStatus(kAgCode).state != ExecUnitInitState::READY)
			std::exit(42);
		traderEnv.hot.prev_ = "ag2512";
		local->reenterOnNextQuery(kAgCode, 1.0, true);
		local->on_channel_lost();
		local->on_channel_ready();
		if (!local->waitForReentry(std::chrono::seconds(5)))
			std::exit(43);

		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		ExecUnitInitStatus failed;
		do
		{
			failed = local->queryUnitInitStatus(kAgCode);
			if (failed.state == ExecUnitInitState::FAILED)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		} while (std::chrono::steady_clock::now() < deadline);
		if (failed.state != ExecUnitInitState::FAILED)
			std::exit(44);
		auto countNoTickLogs = [](const std::vector<std::string>& logs) {
			return static_cast<std::size_t>(std::count_if(logs.begin(), logs.end(), [](const std::string& log) {
				return log.find("No lastest tick data of SHFE.ag.2512, execute later") != std::string::npos;
			}));
		};
		const std::size_t noTickLogsBeforeClear = countNoTickLogs(local->pluginLogs());
		local->on_position(kAgCode, true, 1, 1, 1, 1, 20250110);
		const auto afterClear = local->pluginLogs();
		if (local->clearFailedUnitInitStatus(kAgCode) != ExecUnitInitClearResult::RETRY_BLOCKED ||
			countNoTickLogs(afterClear) != noTickLogsBeforeClear)
			std::exit(45);
		local->requestQuiesce();
		const bool waited = local->waitQuiesced();
		const bool retainedUnit = local->hasRetainedUnit();
		std::cerr << "wait=" << (waited ? "true" : "false")
			<< " retained=" << (retainedUnit ? "true" : "false")
			<< " no_unsafe_replay=true" << std::endl;
		std::exit(!waited && retainedUnit ? 0 : 46);
	}, ::testing::ExitedWithCode(0), "wait=false retained=true no_unsafe_replay=true");
}

TEST_F(R71LocalSerialTest, InitializationCallbackFailureCannotRetryWithoutRecoveryContract)
{
	EXPECT_EXIT({
		R71LocalSerialProbe* local = new R71LocalSerialProbe(
			&factory, "r71_local_generation", &dataMgr);
		if (!initLocal(*local, "r71_local_generation", 2, true))
			std::exit(31);
		local->reenterOnNextQuery(kAgCode, 7.0, true);
		local->on_position_changed(kAgCode, 0.0);

		const ExecUnitInitStatus failed = local->queryUnitInitStatus(kAgCode);
		if (failed.state != ExecUnitInitState::FAILED || !local->targetLogs().empty())
			std::exit(32);
		local->on_position_changed(kAgCode, 2.0);
		const ExecUnitInitStatus stillFailed = local->queryUnitInitStatus(kAgCode);
		if (stillFailed.state != ExecUnitInitState::FAILED ||
			stillFailed.generation != failed.generation ||
			local->clearFailedUnitInitStatus(kAgCode) != ExecUnitInitClearResult::RETRY_BLOCKED ||
			!local->targetLogs().empty())
			std::exit(33);
		local->requestQuiesce();
		const bool waited = local->waitQuiesced();
		const bool retained = local->hasRetainedUnit();
		std::cerr << "wait=" << (waited ? "true" : "false")
			<< " retained=" << (retained ? "true" : "false") << std::endl;
		std::exit(!waited && retained ? 0 : 34);
	}, ::testing::ExitedWithCode(0), "wait=false retained=true");
}

TEST_F(R71LocalSerialTest, FailedInitializedUnitStaysOwnedWhileFakePluginWorkerIsActive)
{
	EXPECT_EXIT({
		// Isolate the deliberately incomplete lifecycle in a child. The fake worker
		// holds a raw unit token until process exit, so no unsafe destructor runs.
		R71LocalSerialProbe* local = new R71LocalSerialProbe(
			&factory, "r71_local_retained_worker", &dataMgr);
		// Bind the trader so channel-ready reaches the unit during first creation;
		// otherwise target 0 equals the unit's initial target and never calls the
		// injected getUndoneQty failure below.
		if (!initLocal(*local, "r71_local_retained_worker", 2, true))
			std::exit(21);
		local->startControlledPluginWorker();
		local->reenterOnNextQuery(kAgCode, 7.0, true);
		local->on_position_changed(kAgCode, 0.0);
		if (local->queryUnitInitStatus(kAgCode).state != ExecUnitInitState::FAILED ||
			!local->waitForRetainedUnit(std::chrono::seconds(2)) ||
			!local->waitForControlledPluginWorker(std::chrono::seconds(2)))
			std::exit(22);

		local->requestQuiesce();
		const bool waited = local->waitQuiesced();
		const bool retained = local->hasRetainedUnit();
		std::cerr << "wait=" << (waited ? "true" : "false")
			<< " retained=" << (retained ? "true" : "false")
			<< " fake_worker=active" << std::endl;
		std::exit(!waited && retained ? 0 : 23);
	}, ::testing::ExitedWithCode(0), "wait=false retained=true fake_worker=active");
}

TEST_F(R71LocalSerialTest, PreflightFailureBeforePluginEntryCanBeCleared)
{
	R71LocalSerialProbe local(&factory, "r71_local_preflight_failure", &dataMgr);
	local.setStub(&stub);
	const std::string path = r71::temp_root() + "r71_local_preflight_failure.json";
	ASSERT_TRUE(r71::write_file(path, R"json({"scale":1.0,"poolsize":2})json"));
	WTSVariant* cfg = WTSCfgLoader::load_from_file(path.c_str());
	ASSERT_NE(cfg, nullptr);
	ASSERT_TRUE(local.init(cfg));
	cfg->release();

	local.on_position_changed(kAgCode, 0.0);
	const ExecUnitInitStatus failed = local.queryUnitInitStatus(kAgCode);
	ASSERT_EQ(failed.state, ExecUnitInitState::FAILED);
	EXPECT_EQ(local.clearFailedUnitInitStatus(kAgCode), ExecUnitInitClearResult::CLEARED)
		<< "a missing-policy preflight failure ran no factory or plugin code";
}

TEST_F(R71LocalSerialTest, ReadyCallbackFailureStaysLockedAcrossTargetAndClearRequests)
{
	EXPECT_EXIT({
		R71LocalSerialProbe* local = new R71LocalSerialProbe(
			&factory, "r71_local_ready_callback_failure", &dataMgr);
		if (!initLocal(*local, "r71_local_ready_callback_failure", 2, true))
			std::exit(51);
		traderEnv.hot.prev_ = "ag2512";
		local->on_position_changed(kAgCode, 1.0);
		if (local->queryUnitInitStatus(kAgCode).state != ExecUnitInitState::READY)
			std::exit(52);

		local->reenterOnNextQuery(kAgCode, 2.0, true);
		local->on_channel_lost();
		local->on_channel_ready();
		if (!local->waitForReentry(std::chrono::seconds(5)))
			std::exit(53);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		ExecUnitInitStatus failed;
		do
		{
			failed = local->queryUnitInitStatus(kAgCode);
			if (failed.state == ExecUnitInitState::FAILED)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		} while (std::chrono::steady_clock::now() < deadline);
		if (failed.state != ExecUnitInitState::FAILED ||
			failed.error.find("callback threw") == std::string::npos ||
			local->clearFailedUnitInitStatus(kAgCode) != ExecUnitInitClearResult::RETRY_BLOCKED)
			std::exit(54);

		const uint32_t ordersBeforeDeferredTargets = traderEnv.api.n_insert;
		local->on_position_changed(kAgCode, 1.0);
		const ExecUnitInitStatus firstDeferred = local->queryUnitInitStatus(kAgCode);
		if (firstDeferred.state != ExecUnitInitState::FAILED ||
			firstDeferred.generation != failed.generation ||
			firstDeferred.error.find("target 4 retained but not applied") == std::string::npos)
			std::exit(55);
		local->on_position_changed(kAgCode, 2.0);
		const ExecUnitInitStatus secondDeferred = local->queryUnitInitStatus(kAgCode);
		if (secondDeferred.state != ExecUnitInitState::FAILED ||
			secondDeferred.generation != failed.generation ||
			secondDeferred.error.find("target 6 retained but not applied") == std::string::npos ||
			traderEnv.api.n_insert != ordersBeforeDeferredTargets)
			std::exit(56);

		local->on_position(kAgCode, true, 1, 1, 1, 1, 20250110);
		local->requestQuiesce();
		const bool waited = local->waitQuiesced();
		const bool retainedUnit = local->hasRetainedUnit();
		std::cerr << "wait=" << (waited ? "true" : "false")
			<< " retained=" << (retainedUnit ? "true" : "false")
			<< " no_new_orders=true" << std::endl;
		std::exit(!waited && retainedUnit ? 0 : 57);
	}, ::testing::ExitedWithCode(0), "wait=false retained=true no_new_orders=true");
}

} // namespace
