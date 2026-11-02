#include "r71_wtcore_env.h"

#include <WTSUtils/WTSCfgLoader.h>
#include <WtCore/WtArbiExecuter.h>
#include <WtCore/WtExecMgr.h>

#include <condition_variable>
#include <mutex>
#include <thread>

namespace {

using namespace wtp;

class BlockingRetryStub : public r71::R71ExecStub
{
public:
	wtp::WTSCommodityInfo* get_comm_info(const char*) override
	{
		std::unique_lock<std::mutex> lock(_mutex);
		_entered = true;
		_condition.notify_all();
		_condition.wait(lock, [this]() { return _released; });
		return comm_;
	}

	bool waitUntilEntered()
	{
		std::unique_lock<std::mutex> lock(_mutex);
		return _condition.wait_for(lock, std::chrono::seconds(3),
			[this]() { return _entered; });
	}

	void releaseLookup(wtp::WTSCommodityInfo* comm)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		comm_ = comm;
		_released = true;
		_condition.notify_all();
	}

private:
	std::mutex _mutex;
	std::condition_variable _condition;
	bool _entered = false;
	bool _released = false;
};

class RetryProofExecuterTest : public ::testing::Test
{
protected:
	r71::R71GenEnv gen;
	r71::R71PluginDir plugin;
	r71::R71TraderEnv trader;
	r71::R71DataMgr data;
	WtExecuterFactory factory;
	WtExecuterMgr manager;
	r71::R71ExecStub stub;
	wtp::TraderAdapter traderAdapter;
	wtp::ITrdNotifySink* registeredSink = nullptr;
	wtp::WTSCommodityInfo* commodity = nullptr;
	wtp::WTSSessionInfo* session = nullptr;
	bool quiesceIncompleteExpected = false;

	void SetUp() override
	{
		ASSERT_TRUE(plugin.ok());
		ASSERT_TRUE(factory.loadFactories(plugin.dir().c_str()));
		trader.bd.regisSession("TRADING");
		session = trader.bd.session("TRADING");
		ASSERT_NE(session, nullptr);
		session->addTradingSection(900, 1500);
		commodity = r71::makeComm("SHFE", "ag", "TRADING", TM_Both);
		commodity->setSessionInfo(session);
		trader.bd.regis(commodity, r71::makeCt("SHFE", "ag", "ag2512"));
		stub.comm_ = commodity;
		stub.sess_ = session;
		stub.hot_ = &trader.hot;
	}

	void TearDown() override
	{
		WtExecQuiesceCoordinator coordinator({ &manager });
		EXPECT_EQ(coordinator.quiesce_all(), !quiesceIncompleteExpected);
		EXPECT_EQ(coordinator.is_complete(), !quiesceIncompleteExpected);
		if (registeredSink != nullptr)
			EXPECT_TRUE(traderAdapter.removeSink(registeredSink));
	}

	WTSVariant* config(const char* name, const char* unitName)
	{
		const std::string json = std::string("{\"scale\":1.0,\"poolsize\":0,\"policy\":{") +
			"\"default\":{\"name\":\"" + unitName +
			"\",\"lots\":1,\"minopenlots\":1,\"offset\":0,\"expire\":0,"
			"\"pricemode\":0,\"span\":0,\"byrate\":false}}}";
		const std::string path = r71::temp_root() + name + ".json";
		if (!r71::write_file(path, json))
			return nullptr;
		return WTSCfgLoader::load_from_file(path.c_str());
	}

};

} // namespace

TEST_F(RetryProofExecuterTest, ArbiRetriesOnlyAfterProvenV2CleanupWithoutQueuedCallbacks)
{
	auto arbi = std::make_shared<wtp::WtArbiExecuter>(&factory, "r190_arbi_retry_proof", &data);
	manager.add_executer(arbi);
	arbi->setStub(&stub);
	WTSVariant* cfg = config("r190_arbi_retry_proof", "WtExeFact.WtMinImpactExeUnit");
	ASSERT_NE(cfg, nullptr);
	ASSERT_TRUE(arbi->init(cfg));
	cfg->release();

	// A failed V2 initialization is retryable when the wrapper proves destroy()
	// completed and this generation accepted no channel callback work.
	stub.comm_ = nullptr;
	arbi->on_position_changed("SHFE.ag.2512", 1.0);
	EXPECT_EQ(trader.api.n_insert, 0u);
	const ExecUnitInitStatus failed = arbi->queryUnitInitStatus("SHFE.ag.2512");
	ASSERT_EQ(failed.state, ExecUnitInitState::FAILED);
	EXPECT_EQ(arbi->clearFailedUnitInitStatus("SHFE.ag.2512"),
		ExecUnitInitClearResult::CLEARED);

	ASSERT_TRUE(trader.build(traderAdapter, "r190_arbi_retry_trader"));
	arbi->setTrader(&traderAdapter);
	registeredSink = arbi.get();
	traderAdapter.addSink(registeredSink);
	stub.comm_ = commodity;
	// Retry the failed +1 target with a zero delta. A new +1 here would also
	// produce one order if the original target had been lost.
	arbi->on_position_changed("SHFE.ag.2512", 0.0);
	const ExecUnitInitStatus ready = arbi->queryUnitInitStatus("SHFE.ag.2512");
	EXPECT_EQ(ready.state, ExecUnitInitState::READY);
	EXPECT_GT(ready.generation, failed.generation);

	WTSTickData* marketTick = r71::makeTick("SHFE", "SHFE.ag.2512", 20250110,
		20250110, 93100000, 3500.0);
	ASSERT_NE(marketTick, nullptr);
	arbi->on_tick("SHFE.ag.2512", marketTick);
	marketTick->release();
	EXPECT_EQ(trader.api.n_insert, 1u)
		<< "the target from the failed generation must replay into the replacement unit";
}

TEST_F(RetryProofExecuterTest, ArbiKeepsRetryBlockedWhenChannelCallbacksWereQueuedDuringFailure)
{
	quiesceIncompleteExpected = true;
	BlockingRetryStub blockingStub;
	blockingStub.comm_ = nullptr;
	blockingStub.sess_ = session;
	blockingStub.hot_ = &trader.hot;
	auto arbi = std::make_shared<wtp::WtArbiExecuter>(&factory, "r190_arbi_retry_blocked", &data);
	manager.add_executer(arbi);
	arbi->setStub(&blockingStub);
	WTSVariant* cfg = config("r190_arbi_retry_blocked", "WtExeFact.WtMinImpactExeUnit");
	ASSERT_NE(cfg, nullptr);
	ASSERT_TRUE(arbi->init(cfg));
	cfg->release();

	std::thread creator([&]() { arbi->on_position_changed("SHFE.ag.2512", 1.0); });
	if (!blockingStub.waitUntilEntered())
	{
		blockingStub.releaseLookup(nullptr);
		creator.join();
		FAIL() << "V2 initialization did not enter the blocked commodity lookup";
		return;
	}
	arbi->on_channel_ready();
	arbi->on_channel_lost();
	blockingStub.releaseLookup(nullptr);
	creator.join();

	const ExecUnitInitStatus failed = arbi->queryUnitInitStatus("SHFE.ag.2512");
	ASSERT_EQ(failed.state, ExecUnitInitState::FAILED);
	EXPECT_EQ(arbi->clearFailedUnitInitStatus("SHFE.ag.2512"),
		ExecUnitInitClearResult::RETRY_BLOCKED);
}

