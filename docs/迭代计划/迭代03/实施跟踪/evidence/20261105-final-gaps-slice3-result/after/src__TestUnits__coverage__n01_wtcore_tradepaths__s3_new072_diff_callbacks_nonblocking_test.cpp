/**
 * I03-NEW-072 (card 9d=A): WtDiffExecuter's adapter callbacks on_tick /
 * on_trade / on_order / on_entrust must not block the calling (adapter
 * callback) thread while the execution unit of the same code is still being
 * created on another thread.
 *
 * Contract: an event that arrives while the unit is being created is queued
 * for that creation; when the unit becomes READY the queued events are
 * delivered to it in arrival order (after the unit's own channel-ready /
 * position-recovery work) exactly once; when the creation FAILS they are
 * discarded with a warning and the executor no longer reports a complete
 * quiesce. The host-side diff ledger is still booked for a trade.
 *
 * The real WtExeFact WtDiffMinImpactExeUnit is used; its initialization is held
 * inside the host commodity lookup (blocking executor stub) so the creation is
 * deterministically in flight. A feeder thread delivers the four callbacks and
 * is given a bounded time to return; the red control (callbacks waiting for the
 * creation) is reported as an assertion failure instead of a hang because the
 * creation is released after the bound in every case.
 */
#include "r71_wtcore_env.h"

#include <Includes/ILogHandler.h>
#include <WTSTools/WTSLogger.h>
#include <WTSUtils/WTSCfgLoader.h>

#include <WtCore/WtDiffExecuter.h>
#include <WtCore/WtExecMgr.h>
#include <WtCore/WtExecuterFactory.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace wtp;

namespace {

const char* const kNew072Code = "SHFE.ag.2512";

// Holds the first commodity lookup (made by the unit's initialize) until released.
class S3New072BlockingStub final : public r71::R71ExecStub
{
public:
	void blockFirstLookup()
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_block = true;
		_entered = false;
		_released = false;
	}

	bool waitEntered(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(_mutex);
		return _cv.wait_for(lock, timeout, [this]() { return _entered; });
	}

	void release(wtp::WTSCommodityInfo* resultForBlockedLookup)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_blocked_result = resultForBlockedLookup;
		_released = true;
		_cv.notify_all();
	}

	wtp::WTSCommodityInfo* get_comm_info(const char* code) override
	{
		const uint32_t call = ++lookups;
		if (call == 1)
		{
			std::unique_lock<std::mutex> lock(_mutex);
			if (_block)
			{
				_entered = true;
				_cv.notify_all();
				_cv.wait(lock, [this]() { return _released; });
				_block = false;
				return _blocked_result;
			}
		}
		return r71::R71ExecStub::get_comm_info(code);
	}

	std::atomic<uint32_t> lookups{0};

private:
	std::mutex _mutex;
	std::condition_variable _cv;
	bool _block = false;
	bool _entered = false;
	bool _released = false;
	wtp::WTSCommodityInfo* _blocked_result = NULL;
};

class S3New072WarnCapture final : public ILogHandler
{
public:
	S3New072WarnCapture()
	{
		static const char* loggerConfig = R"({
            "root":{"level":"info","async":false,"sinks":[]},
            "dyn_pattern":{"executer":{"level":"info","async":false,"sinks":[]}}
        })";
		WTSLogger::init(loggerConfig, false, NULL);
		WTSLogger::registerHandler(this);
	}
	~S3New072WarnCapture() { WTSLogger::registerHandler(NULL); }

	void handleLogAppend(WTSLogLevel level, const char* message) override
	{
		if (level != LL_WARN || message == NULL)
			return;
		std::lock_guard<std::mutex> lock(_mutex);
		_warnings.emplace_back(message);
	}

	size_t countContaining(const char* text) const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		size_t n = 0;
		for (const std::string& w : _warnings)
			if (w.find(text) != std::string::npos)
				++n;
		return n;
	}

private:
	mutable std::mutex _mutex;
	std::vector<std::string> _warnings;
};

class S3New072DiffCallbacks : public ::testing::Test
{
protected:
	r71::R71GenEnv env;
	r71::R71PluginDir plug;
	r71::R71TraderEnv traderEnv;
	r71::R71DataMgr dm;
	WtExecuterFactory factory;
	TraderAdapter trader;
	S3New072BlockingStub stub;
	WtExecuterMgr manager;
	std::shared_ptr<WtDiffExecuter> diff;
	wtp::WTSCommodityInfo* comm = NULL;
	wtp::WTSSessionInfo* sess = NULL;

	void SetUp() override
	{
		ASSERT_TRUE(plug.ok()) << "libWtExeFact is required to create the real execution unit";
		ASSERT_TRUE(factory.loadFactories(plug.dir().c_str()));

		traderEnv.bd.regisSession("TRADING");
		sess = traderEnv.bd.session("TRADING");
		ASSERT_NE(sess, nullptr);
		sess->addTradingSection(900, 1500);
		comm = r71::makeComm("SHFE", "ag", "TRADING", TM_Both);
		comm->setSessionInfo(sess);
		WTSContractInfo* agct = r71::makeCt("SHFE", "ag", "ag2512");
		traderEnv.bd.regis(comm, agct);
		traderEnv.bd.regisAlias(kNew072Code, agct);

		stub.comm_ = comm;
		stub.sess_ = sess;
		stub.hot_ = &traderEnv.hot;
		ASSERT_TRUE(traderEnv.build(trader, "s3_new072"));

		const std::string json = "{\"scale\":1.0,\"poolsize\":0,\"policy\":{\"default\":{"
			"\"name\":\"WtExeFact.WtDiffMinImpactExeUnit\",\"lots\":1,\"minopenlots\":1,"
			"\"offset\":0,\"expire\":0,\"pricemode\":0,\"span\":0,\"byrate\":false}}}";
		const std::string file = r71::temp_root() + "s3_new072_diff.json";
		ASSERT_TRUE(r71::write_file(file, json));
		WTSVariant* cfg = WTSCfgLoader::load_from_file(file.c_str());
		ASSERT_NE(cfg, nullptr);
		// One executer name per case: the Diff snapshot of an earlier case in the
		// same process would otherwise recreate the unit during init.
		const std::string executerName = std::string("s3_new072_") +
			::testing::UnitTest::GetInstance()->current_test_info()->name();
		diff = std::make_shared<WtDiffExecuter>(&factory, executerName.c_str(), &dm, &traderEnv.bd);
		manager.add_executer(diff);
		diff->setStub(&stub);
		ASSERT_TRUE(diff->init(cfg));
		cfg->release();
		diff->setTrader(&trader);
		trader.addSink(diff.get());
	}

	// Starts the creation of the unit (on_position_changed on its own thread) and
	// waits until it is held inside the unit's initialization.
	bool startBlockedCreation(std::thread& creator)
	{
		stub.blockFirstLookup();
		creator = std::thread([this]() { diff->on_position_changed(kNew072Code, 1.0); });
		return stub.waitEntered(std::chrono::seconds(2));
	}

	// Delivers on_tick/on_order/on_entrust/on_trade from a feeder thread and
	// reports whether all of them returned within the bound.
	bool feedCallbacksWithin(std::thread& feeder, std::chrono::milliseconds bound)
	{
		std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
		WtDiffExecuter* executer = diff.get();
		feeder = std::thread([executer, done]() {
			WTSTickData* tick = r71::makeTick("SHFE", kNew072Code, 20250110, 20250110, 93105000, 3501.0);
			executer->on_tick(kNew072Code, tick);
			tick->release();
			executer->on_order(901, kNew072Code, true, 1.0, 1.0, 3501.0, false);
			executer->on_entrust(901, kNew072Code, true, "s3_new072");
			executer->on_trade(902, kNew072Code, true, 0.0, 3501.0);
			done->store(true, std::memory_order_release);
		});
		const auto deadline = std::chrono::steady_clock::now() + bound;
		while (!done->load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		return done->load(std::memory_order_acquire);
	}
};

} // namespace

TEST_F(S3New072DiffCallbacks, CallbacksDuringCreationReturnPromptlyAndAreDeliveredAfterReady)
{
	const uint32_t inserts = traderEnv.api.n_insert.load();
	std::thread creator;
	if (!startBlockedCreation(creator))
	{
		stub.release(comm);
		creator.join();
		FAIL() << "the unit creation was not held inside its initialization";
	}
	EXPECT_EQ(ExecUnitInitState::INITIALIZING, diff->queryUnitInitStatus(kNew072Code).state);

	std::thread feeder;
	const bool returnedWhileCreating = feedCallbacksWithin(feeder, std::chrono::milliseconds(500));
	const ExecUnitInitState stateWhileFeeding = diff->queryUnitInitStatus(kNew072Code).state;
	stub.release(comm);
	creator.join();
	feeder.join();

	EXPECT_TRUE(returnedWhileCreating)
		<< "an adapter callback waited for the execution-unit creation of the same code";
	EXPECT_EQ(ExecUnitInitState::INITIALIZING, stateWhileFeeding);
	const ExecUnitInitStatus ready = diff->queryUnitInitStatus(kNew072Code);
	EXPECT_EQ(ExecUnitInitState::READY, ready.state) << ready.error;
	// The queued tick reached the unit after its target was recovered: the unit
	// dispatched the one-lot target exactly once.
	EXPECT_EQ(inserts + 1u, traderEnv.api.n_insert.load());

	WtExecQuiesceCoordinator coordinator({ &manager });
	EXPECT_TRUE(coordinator.quiesce_all());
	EXPECT_TRUE(coordinator.is_complete());
}

TEST_F(S3New072DiffCallbacks, CallbacksDuringFailedCreationReturnPromptlyAndAreDroppedWithWarning)
{
	S3New072WarnCapture warnings;
	const uint32_t inserts = traderEnv.api.n_insert.load();
	std::thread creator;
	if (!startBlockedCreation(creator))
	{
		stub.release(NULL);
		creator.join();
		FAIL() << "the unit creation was not held inside its initialization";
	}

	std::thread feeder;
	const bool returnedWhileCreating = feedCallbacksWithin(feeder, std::chrono::milliseconds(500));
	// The held lookup now returns no commodity: the unit's initialization fails.
	stub.release(NULL);
	creator.join();
	feeder.join();

	EXPECT_TRUE(returnedWhileCreating)
		<< "an adapter callback waited for the execution-unit creation of the same code";
	const ExecUnitInitStatus failed = diff->queryUnitInitStatus(kNew072Code);
	EXPECT_EQ(ExecUnitInitState::FAILED, failed.state);
	EXPECT_EQ(inserts, traderEnv.api.n_insert.load()) << "no event reached a failed unit";
	EXPECT_EQ(1u, warnings.countContaining("deferred callback")) << "dropping queued events is reported once";

	WtExecQuiesceCoordinator coordinator({ &manager });
	(void)coordinator.quiesce_all();
	EXPECT_FALSE(coordinator.is_complete()) << "dropped order/trade events keep the quiesce incomplete";
}
