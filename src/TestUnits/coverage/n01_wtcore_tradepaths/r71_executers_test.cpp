/**
 * r71_executers_test.cpp — lane R71 / Local/Arbi/Diff 执行器余量补测
 *
 * 覆盖 WtLocalExecuter.cpp / WtArbiExecuter.cpp / WtDiffExecuter.cpp：
 *   getUnit 的已存在/自动创建/通道就绪回调
 *   set_position 的组合匹配、目标变更、过期目标清零、strict_sync 通道持仓
 *   on_tick/on_order/on_trade/on_entrust/on_channel_ready/lost/on_account 向真实单元转发
 *   on_position_changed 的增量目标与差量更新
 *   getPosition/getUndoneQty/getOrders/getTicks/grabLastTick 的 trader/data_mgr 分支
 *
 * 执行单元由真实 WtExeFact 插件创建（只加载该 dylib 的临时符号链接）；
 * 交易侧用进程内 ITraderApi 替身，不连接任何柜台。
 */
#include "r71_wtcore_env.h"

#include <WTSUtils/WTSCfgLoader.h>

#include <WtCore/WtArbiExecuter.h>
#include <WtCore/WtDiffExecuter.h>
#include <WtCore/WtExecMgr.h>
#include <WtCore/WtExecuterSchedule.h>
#include <WtCore/WtLocalExecuter.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

using namespace wtp;

typedef wt_hashmap<std::string, double> R71PositionTargets;

namespace {

class R71RejectingPool
{
public:
	template <typename Task>
	bool schedule(const Task&)
	{
		++schedule_calls;
		return false;
	}
	uint32_t schedule_calls = 0;
};

class R71BlockingExecStub final : public r71::R71ExecStub
{
public:
	void blockFirstCommodityLookup()
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_block_first = true;
		_entered = false;
		_released = false;
	}

	void setFirstCommodityLookupHook(std::function<void()> hook)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_first_lookup_hook = std::move(hook);
	}

	bool waitForFirstCommodityLookup(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(_mutex);
		return _condition.wait_for(lock, timeout, [this]() { return _entered; });
	}

	void releaseFirstCommodityLookup()
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_released = true;
		_condition.notify_all();
	}

	wtp::WTSCommodityInfo* get_comm_info(const char* code) override
	{
		const uint32_t call = ++commodity_queries;
		std::function<void()> hook;
		bool should_block = false;
		{
			std::lock_guard<std::mutex> lock(_mutex);
			if (call == 1)
			{
				hook = _first_lookup_hook;
				should_block = _block_first;
			}
		}
		if (hook)
			hook();
		if (should_block)
		{
			std::unique_lock<std::mutex> lock(_mutex);
			_entered = true;
			_condition.notify_all();
			_condition.wait(lock, [this]() { return _released; });
			_block_first = false;
		}
		return r71::R71ExecStub::get_comm_info(code);
	}

	std::atomic<uint32_t> commodity_queries{0};

private:
	std::mutex _mutex;
	std::condition_variable _condition;
	std::function<void()> _first_lookup_hook;
	bool _block_first = false;
	bool _entered = false;
	bool _released = false;
};

template <typename BaseExecuter>
class R71ObservableExecuter : public BaseExecuter
{
public:
	template <typename... Args>
	explicit R71ObservableExecuter(Args&&... args)
		: BaseExecuter(std::forward<Args>(args)...)
	{
	}

	void blockFirstReadyCallback(bool throwAfterRelease)
	{
		std::lock_guard<std::mutex> lock(_ready_mutex);
		_block_first_ready = true;
		_throw_first_ready = throwAfterRelease;
		_ready_entered = false;
		_ready_released = false;
	}

	bool waitForFirstReadyCallback(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(_ready_mutex);
		return _ready_condition.wait_for(lock, timeout, [this]() { return _ready_entered; });
	}

	void releaseFirstReadyCallback()
	{
		std::lock_guard<std::mutex> lock(_ready_mutex);
		_ready_released = true;
		_ready_condition.notify_all();
	}

	void enableChannelReadyProbe()
	{
		_channel_ready_probe.store(true);
	}

	void disableChannelReadyProbe()
	{
		_channel_ready_probe.store(false);
	}

	double getUndoneQty(const char* code) override
	{
		const uint32_t call = ++ready_queries;
		bool should_throw = false;
		if (call == 1)
		{
			std::unique_lock<std::mutex> lock(_ready_mutex);
			if (_block_first_ready)
			{
				_ready_entered = true;
				_ready_condition.notify_all();
				_ready_condition.wait(lock, [this]() { return _ready_released; });
				_block_first_ready = false;
				should_throw = _throw_first_ready;
			}
		}
		if (should_throw)
			throw std::runtime_error("R71 injected channel callback failure");
		if (_channel_ready_probe.load())
			return 1.0;
		return BaseExecuter::getUndoneQty(code);
	}

	OrderIDs cancel(const char* code, bool isBuy, double qty) override
	{
		if (_channel_ready_probe.load())
			channel_ready_cancel_requests.fetch_add(1);
		return BaseExecuter::cancel(code, isBuy, qty);
	}

	void writeLog(const char* message) override
	{
		if (message != NULL && std::strstr(message, "avaliable update") != NULL)
			available_updates.fetch_add(1);
		BaseExecuter::writeLog(message);
	}

	std::atomic<uint32_t> ready_queries{0};
	std::atomic<uint32_t> available_updates{0};
	std::atomic<uint32_t> channel_ready_cancel_requests{0};

private:
	std::atomic<bool> _channel_ready_probe{false};
	std::mutex _ready_mutex;
	std::condition_variable _ready_condition;
	bool _block_first_ready = false;
	bool _throw_first_ready = false;
	bool _ready_entered = false;
	bool _ready_released = false;
};

class R71CapturingTraderApi : public r71::R71FakeTraderApi
{
public:
	int orderInsert(WTSEntrust* entrust) override
	{
		last_usertag = entrust->getUserTag();
		const char* localid = std::strrchr(last_usertag.c_str(), '.');
		last_localid = localid == NULL ? 0 : static_cast<uint32_t>(std::strtoul(localid + 1, NULL, 10));
		return r71::R71FakeTraderApi::orderInsert(entrust);
	}

	uint32_t last_localid = 0;
	std::string last_usertag;
};

WTSOrderInfo* r71MakeOrderReport(WTSContractInfo* contract, const std::string& usertag,
	const char* orderId, WTSDirectionType direction, WTSOffsetType offset, double price,
	double volume, double volumeTraded, double volumeLeft, WTSOrderState state)
{
	WTSOrderInfo* order = WTSOrderInfo::create();
	if (order == NULL)
		return NULL;
	order->setContractInfo(contract);
	order->setDirection(direction);
	order->setOffsetType(offset);
	order->setPrice(price);
	order->setVolume(volume);
	order->setVolTraded(volumeTraded);
	order->setVolLeft(volumeLeft);
	order->setUserTag(usertag.c_str());
	order->setOrderID(orderId);
	order->setOrderState(state);
	return order;
}

WTSTradeInfo* r71MakeTradeReport(WTSContractInfo* contract, const std::string& usertag,
	const char* tradeId, const char* orderId, WTSDirectionType direction,
	WTSOffsetType offset, double price, double volume)
{
	if (contract == NULL)
		return NULL;
	WTSTradeInfo* trade = WTSTradeInfo::create(contract->getCode(), contract->getExchg());
	if (trade == NULL)
		return NULL;
	trade->setContractInfo(contract);
	trade->setUserTag(usertag.c_str());
	trade->setTradeID(tradeId);
	trade->setRefOrder(orderId);
	trade->setDirection(direction);
	trade->setOffsetType(offset);
	trade->setPrice(price);
	trade->setVolume(volume);
	trade->setTradeDate(20250110);
	trade->setTradeTime(93100000);
	return trade;
}

// 2026-11-03 Diff 目标批次重构（P02-005，evidence/20261103-date-diff-review）起，
// Diff 在同一更新临界区先提交目标/差额账本，再以非阻塞方式请求单元；在途创建由
// registerCreatedUnit 在注册时读取最新账本与通道状态补投。因此同码调用方不再阻塞
// 等待创建者（原 DIFF_WAITS_FOR_CREATOR 预期），与 Arbi 一样记账后立即返回。
enum class R71PendingCreationBehavior
{
	LOCAL_COALESCES_TARGET,
	ARBI_RETURNS_BEFORE_REGISTRATION,
	DIFF_RECORDS_TARGET_BEFORE_REGISTRATION
};

struct R71SubmittedOrder
{
	uint32_t localid = 0;
	std::string usertag;
	WTSDirectionType direction = WDT_LONG;
	WTSOffsetType offset = WOT_OPEN;
	double price = 0.0;
	double volume = 0.0;
};

class R71LocalExecQuiesceGuard
{
public:
	R71LocalExecQuiesceGuard(WtExecuterMgr& manager, TraderAdapter& trader, ITrdNotifySink* sink)
		: _manager(manager), _trader(trader), _sink(sink) {}

	~R71LocalExecQuiesceGuard()
	{
		WtExecQuiesceCoordinator coordinator({ &_manager });
		if (!coordinator.quiesce_all() || !coordinator.is_complete())
		{
			ADD_FAILURE() << "R71 executor quiesce did not complete";
			std::abort();
		}
		// TraderAdapter does not own sinks. removeSink waits for any callback that
		// already started, so the manager may safely destroy the executer afterward.
		if (!_trader.removeSink(_sink))
		{
			ADD_FAILURE() << "TraderAdapter still has an active R71 executor callback";
			std::abort();
		}
	}

private:
	WtExecuterMgr& _manager;
	TraderAdapter& _trader;
	ITrdNotifySink* _sink;
};

class R71ExecGroupQuiesceGuard
{
public:
	template <typename Executer>
	void add(const std::shared_ptr<Executer>& executer, TraderAdapter& trader,
		ITrdNotifySink* sink)
	{
		_manager.add_executer(executer);
		_sinks.emplace_back(&trader, sink);
	}

	~R71ExecGroupQuiesceGuard()
	{
		WtExecQuiesceCoordinator coordinator({ &_manager });
		if (!coordinator.quiesce_all() || !coordinator.is_complete())
		{
			ADD_FAILURE() << "R71 executor group quiesce did not complete";
			std::abort();
		}
		for (const auto& registration : _sinks)
		{
			if (!registration.first->removeSink(registration.second))
			{
				ADD_FAILURE() << "TraderAdapter still has an active R71 executor callback";
				std::abort();
			}
		}
	}

private:
	WtExecuterMgr _manager;
	std::vector<std::pair<TraderAdapter*, ITrdNotifySink*> > _sinks;
};

class R71ExecTest : public ::testing::Test
{
protected:
    r71::R71GenEnv env;
    r71::R71PluginDir plug;
    r71::R71TraderEnv traderEnv;
    r71::R71ExecStub stub;
    r71::R71DataMgr dm;
    WtExecuterFactory factory;
    TraderAdapter trader;

    wtp::WTSCommodityInfo* comm = NULL;
    wtp::WTSSessionInfo* sess = NULL;
	std::vector<std::shared_ptr<R71BlockingExecStub> > quiesce_stubs;
	WtExecuterMgr quiesce_manager;

	void TearDown() override
	{
		WtExecQuiesceCoordinator coordinator({ &quiesce_manager });
		EXPECT_TRUE(coordinator.quiesce_all());
		EXPECT_TRUE(coordinator.is_complete());
	}

	template <typename Executer>
	std::shared_ptr<Executer> ownUntilQuiesced(const std::shared_ptr<Executer>& executer)
	{
		quiesce_manager.add_executer(executer);
		return executer;
	}

	std::shared_ptr<R71BlockingExecStub> keepBlockingStub()
	{
		std::shared_ptr<R71BlockingExecStub> blockingStub(new R71BlockingExecStub());
		quiesce_stubs.push_back(blockingStub);
		return blockingStub;
	}

    void SetUp() override
    {
        ASSERT_TRUE(plug.ok()) << "缺少 libWtExeFact，无法创建真实执行单元";
        ASSERT_TRUE(factory.loadFactories(plug.dir().c_str()));

        traderEnv.bd.regisSession("TRADING");
        sess = traderEnv.bd.session("TRADING");
        ASSERT_NE(sess, nullptr);
        sess->addTradingSection(900, 1500);

        comm = r71::makeComm("SHFE", "ag", "TRADING", TM_Both);
        comm->setSessionInfo(sess);
        WTSContractInfo* agct = r71::makeCt("SHFE", "ag", "ag2512");
        traderEnv.bd.regis(comm, agct);
        traderEnv.bd.regisAlias("SHFE.ag.2512", agct);

        WTSCommodityInfo* rb = r71::makeComm("SHFE", "rb", "TRADING", TM_Both);
        rb->setSessionInfo(sess);
        traderEnv.bd.regis(rb, r71::makeCt("SHFE", "rb", "rb2510"));

        WTSCommodityInfo* au = r71::makeComm("SHFE", "au", "TRADING", TM_Both);
        au->setSessionInfo(sess);
        traderEnv.bd.regis(au, r71::makeCt("SHFE", "au", "au2212"));

        WTSCommodityInfo* cu = r71::makeComm("SHFE", "cu", "TRADING", TM_Both);
        cu->setSessionInfo(sess);
        traderEnv.bd.regis(cu, r71::makeCt("SHFE", "cu", "cu2510"));

        stub.comm_ = comm;
        stub.sess_ = sess;
        stub.hot_ = &traderEnv.hot;

        ASSERT_TRUE(traderEnv.build(trader, "r71_exec"));
    }

	bool buildCapturingTrader(TraderAdapter& adapter, R71CapturingTraderApi& api,
		const char* id)
	{
		if (!traderEnv.policy_ok() ||
			!adapter.initExt(id, &api, &traderEnv.bd, traderEnv.policy()) || !adapter.run())
			return false;
		adapter.onLoginResult(true, "r71", 20250110);
		adapter.onRspPosition(static_cast<const WTSArray*>(NULL));
		adapter.onRspOrders(static_cast<const WTSArray*>(NULL));
		adapter.onRspTrades(static_cast<const WTSArray*>(NULL));
		adapter.onRspAccount(static_cast<WTSArray*>(NULL));
		return adapter.state() == TraderAdapter::AS_ALLREADY;
	}

    WTSVariant* cfg(const char* name, const char* policyUnit, const char* extra)
    {
        std::string json = std::string("{\"scale\":1.0,\"poolsize\":0,")
            + (extra != NULL ? extra : "")
            + "\"policy\":{\"default\":{\"name\":\"" + policyUnit
            + "\",\"lots\":1,\"minopenlots\":1,\"offset\":0,\"expire\":0,"
              "\"pricemode\":0,\"span\":0,\"byrate\":false}}}";
        std::string file = r71::temp_root() + name + ".json";
        if (!r71::write_file(file, json))
            return NULL;
        return WTSCfgLoader::load_from_file(file.c_str());
    }

    WTSTickData* tick(uint32_t atime, double price, const char* code = "SHFE.ag.2512")
    {
        return r71::makeTick("SHFE", code, 20250110, 20250110, atime, price);
    }

	template <typename Executer>
	void verifyConcurrentCreation(const std::shared_ptr<Executer>& executer_ptr,
		R71BlockingExecStub& blockingStub,
		const char* name, const char* unitName, R71PendingCreationBehavior behavior,
		uint32_t expectedReadyCallbacks, R71ExecGroupQuiesceGuard& quiesceGuard,
		TraderAdapter& isolatedTrader, R71CapturingTraderApi& api)
	{
		ASSERT_NE(executer_ptr, nullptr);
		Executer& executer = *executer_ptr;
		executer.setStub(&blockingStub);
		WTSVariant* params = cfg(name, unitName, NULL);
		ASSERT_NE(params, nullptr);
		ASSERT_TRUE(executer.init(params));
		params->release();
		executer.setTrader(&isolatedTrader);
		isolatedTrader.addSink(&executer);
		quiesceGuard.add(executer_ptr, isolatedTrader, &executer);
		executer.enableChannelReadyProbe();

		// Keep the channel down until the first creation is blocked in its commodity
		// lookup, then deliver ready/lost/ready before the candidate is registered.
		blockingStub.blockFirstCommodityLookup();
		EXPECT_TRUE(isolatedTrader.checkOrderLimits("SHFE.ag.2512"))
			<< name << " should be eligible to reserve its initial target";

		std::thread creator([&executer]() {
			executer.on_position_changed("SHFE.ag.2512", 1.0);
		});
		const bool initialization_blocked = blockingStub.waitForFirstCommodityLookup(
			std::chrono::seconds(2));
		if (!initialization_blocked)
		{
			blockingStub.releaseFirstCommodityLookup();
			creator.join();
			const ExecUnitInitStatus status = executer.queryUnitInitStatus("SHFE.ag.2512");
			ADD_FAILURE() << name << " did not enter its first commodity lookup; calls="
				<< blockingStub.commodity_queries.load() << ", state="
				<< static_cast<int>(status.state) << ", error=" << status.error;
			return;
		}
		const ExecUnitInitStatus initializing = executer.queryUnitInitStatus("SHFE.ag.2512");
		EXPECT_EQ(initializing.query_result, ExecUnitInitQueryResult::OK);
		EXPECT_EQ(initializing.state, ExecUnitInitState::INITIALIZING);
		EXPECT_GT(initializing.generation, 0u);
		EXPECT_TRUE(initializing.error.empty());
		EXPECT_EQ(executer.clearFailedUnitInitStatus("SHFE.ag.2512"),
			ExecUnitInitClearResult::NOT_FAILED);

		std::atomic<bool> second_started(false);
		std::atomic<bool> second_finished(false);
		std::thread concurrentUpdate([&executer, &second_started, &second_finished]() {
			second_started.store(true);
			executer.on_position_changed("SHFE.ag.2512", 1.0);
			second_finished.store(true);
		});
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (!second_started.load() && std::chrono::steady_clock::now() < deadline)
			std::this_thread::yield();
		EXPECT_TRUE(second_started.load());
		ExecUnitInitStatus pending = executer.queryUnitInitStatus("SHFE.ag.2512");
		const auto pending_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		{
			while (!second_finished.load() && std::chrono::steady_clock::now() < pending_deadline)
				std::this_thread::yield();
			EXPECT_TRUE(second_finished.load());
			pending = executer.queryUnitInitStatus("SHFE.ag.2512");
			EXPECT_EQ(pending.waiting_callers, 0u)
				<< (behavior == R71PendingCreationBehavior::LOCAL_COALESCES_TARGET ?
					"Local merges the in-flight target without waiting" :
					behavior == R71PendingCreationBehavior::ARBI_RETURNS_BEFORE_REGISTRATION ?
					"Arbi publishes target first and returns when the unit is not registered yet" :
					"Diff commits the ledger first and returns while the creator is still pending");
		}
		EXPECT_EQ(pending.state, ExecUnitInitState::INITIALIZING);
		EXPECT_GT(pending.generation, 0u);

		// Local preserves both ready callbacks in the transition FIFO. Arbi suppresses
		// the old ready recovery by epoch, while Diff sends the current ready callback
		// only after registration observes the final channel state.
		executer.on_channel_ready();
		executer.on_channel_lost();
		executer.on_channel_ready();

		blockingStub.releaseFirstCommodityLookup();
		creator.join();
		concurrentUpdate.join();
		const ExecUnitInitStatus ready = executer.queryUnitInitStatus("SHFE.ag.2512");
		EXPECT_EQ(ready.query_result, ExecUnitInitQueryResult::OK);
		EXPECT_EQ(ready.state, ExecUnitInitState::READY);
		EXPECT_EQ(ready.generation, initializing.generation);
		EXPECT_TRUE(ready.error.empty());
		EXPECT_EQ(ready.waiting_callers, 0u);
		EXPECT_EQ(executer.clearFailedUnitInitStatus("SHFE.ag.2512"),
			ExecUnitInitClearResult::NOT_FAILED);
		EXPECT_EQ(blockingStub.commodity_queries.load(), 1u)
			<< "the two same-code target changes must share one creation generation";
		EXPECT_EQ(executer.channel_ready_cancel_requests.load(), expectedReadyCallbacks)
			<< "the real plugin's unmanaged-order cancellation proves ready callback delivery";
		EXPECT_EQ(api.n_action, 0u)
			<< "the probe presents an unmanaged quantity, but the isolated adapter has no order to cancel";

		// A post-registration edge is the independent positive control for the real
		// plugin callback probe; it must add exactly one ready callback for every style.
		executer.on_channel_lost();
		executer.on_channel_ready();
		EXPECT_EQ(executer.channel_ready_cancel_requests.load(), expectedReadyCallbacks + 1);
		executer.disableChannelReadyProbe();
		EXPECT_DOUBLE_EQ(isolatedTrader.getUndoneQty("SHFE.ag.2512"), 0.0);

		const char* const code = "SHFE.ag.2512";
		WTSContractInfo* contract = traderEnv.bd.getContract("ag2512", "SHFE");
		ASSERT_NE(contract, nullptr);
		const auto submitTick = [&](uint32_t time, uint32_t expectedInserts,
			R71SubmittedOrder* submitted) {
			WTSTickData* marketTick = tick(time, 3500.0 + static_cast<double>(time - 93100000) / 1000.0);
			ASSERT_NE(marketTick, nullptr);
			executer.on_tick(code, marketTick);
			marketTick->release();
			EXPECT_EQ(api.n_insert, expectedInserts);
			if (submitted != NULL && api.n_insert == expectedInserts)
			{
				submitted->localid = api.last_localid;
				submitted->usertag = api.last_usertag;
				submitted->direction = api.last_dir;
				submitted->offset = api.last_off;
				submitted->price = api.last_price;
				submitted->volume = api.last_vol;
			}
		};
		double filledPosition = 0.0;
		const auto pushFilledOrder = [&](const R71SubmittedOrder& submitted,
			const char* orderId, const char* tradeId) {
			ASSERT_NE(submitted.localid, 0u);
			ASSERT_FALSE(submitted.usertag.empty());
			EXPECT_DOUBLE_EQ(submitted.volume, 1.0);

			WTSOrderInfo* pendingOrder = r71MakeOrderReport(contract, submitted.usertag,
				orderId, submitted.direction, submitted.offset, submitted.price,
				submitted.volume, 0.0, submitted.volume, WOS_NotTraded_Queuing);
			ASSERT_NE(pendingOrder, nullptr);
			isolatedTrader.onPushOrder(pendingOrder);
			pendingOrder->release();
			OrderMap* orders = isolatedTrader.getOrders(code);
			ASSERT_NE(orders, nullptr);
			ASSERT_EQ(orders->size(), 1u);
			WTSOrderInfo* tracked = static_cast<WTSOrderInfo*>(orders->get(submitted.localid));
			ASSERT_NE(tracked, nullptr);
			EXPECT_STREQ(tracked->getUserTag(), submitted.usertag.c_str());
			orders->release();
			EXPECT_DOUBLE_EQ(isolatedTrader.getUndoneQty(code), submitted.volume);

			WTSTradeInfo* trade = r71MakeTradeReport(contract, submitted.usertag,
				tradeId, orderId, submitted.direction, submitted.offset,
				submitted.price, submitted.volume);
			ASSERT_NE(trade, nullptr);
			isolatedTrader.onPushTrade(trade);
			trade->release();
			filledPosition += submitted.volume;
			EXPECT_DOUBLE_EQ(isolatedTrader.getPosition(code, true), filledPosition);
			EXPECT_DOUBLE_EQ(isolatedTrader.getUndoneQty(code), 0.0);

			WTSOrderInfo* filledOrder = r71MakeOrderReport(contract, submitted.usertag,
				orderId, submitted.direction, submitted.offset, submitted.price,
				submitted.volume, submitted.volume, 0.0, WOS_AllTraded);
			ASSERT_NE(filledOrder, nullptr);
			isolatedTrader.onPushOrder(filledOrder);
			filledOrder->release();
			orders = isolatedTrader.getOrders(code);
			ASSERT_NE(orders, nullptr);
			EXPECT_EQ(orders->size(), 0u);
			orders->release();
		};

		const uint32_t baseInsert = api.n_insert;
		R71SubmittedOrder firstOrder;
		submitTick(93100000, baseInsert + 1, &firstOrder);
		ASSERT_NE(firstOrder.localid, 0u);
		ASSERT_FALSE(firstOrder.usertag.empty());
		// While the first order is outstanding, another fresh tick must not overlap it.
		submitTick(93101000, baseInsert + 1, NULL);
		EXPECT_DOUBLE_EQ(isolatedTrader.getUndoneQty(code), firstOrder.volume);
		pushFilledOrder(firstOrder, "R71-CONCURRENT-1", "R71-CONCURRENT-T1");
		EXPECT_DOUBLE_EQ(isolatedTrader.getPosition(code, true), firstOrder.volume);

		R71SubmittedOrder secondOrder;
		submitTick(93102000, baseInsert + 2, &secondOrder);
		ASSERT_NE(secondOrder.localid, 0u);
		ASSERT_NE(secondOrder.localid, firstOrder.localid);
		ASSERT_NE(secondOrder.usertag, firstOrder.usertag);
		// The second outstanding order also blocks an overlapping tick.
		submitTick(93103000, baseInsert + 2, NULL);
		EXPECT_DOUBLE_EQ(isolatedTrader.getUndoneQty(code), secondOrder.volume);
		pushFilledOrder(secondOrder, "R71-CONCURRENT-2", "R71-CONCURRENT-T2");
		EXPECT_EQ(api.n_insert, baseInsert + 2)
			<< "both concurrent +1 changes must be retained and dispatched one lot at a time";
		EXPECT_DOUBLE_EQ(isolatedTrader.getPosition(code, true), 2.0);
		EXPECT_DOUBLE_EQ(isolatedTrader.getUndoneQty(code), 0.0);
	}

	template <typename Executer>
	void verifyFailedInitializationWaiters(Executer& executer,
		R71BlockingExecStub& blockingStub, const char* name, const char* unitName,
		R71PendingCreationBehavior behavior, double waiterDelta)
	{
		executer.setStub(&blockingStub);
		WTSVariant* params = cfg(name, unitName, NULL);
		ASSERT_NE(params, nullptr);
		ASSERT_TRUE(executer.init(params));
		params->release();
		executer.setTrader(&trader);
		trader.addSink(&executer);
		blockingStub.comm_ = NULL;
		blockingStub.blockFirstCommodityLookup();

		std::thread creator([&executer]() {
			executer.on_position_changed("SHFE.ag.2512", 1.0);
		});
		const bool initialization_blocked = blockingStub.waitForFirstCommodityLookup(
			std::chrono::seconds(2));
		EXPECT_TRUE(initialization_blocked);
		if (!initialization_blocked)
		{
			blockingStub.releaseFirstCommodityLookup();
			creator.join();
			return;
		}

		const ExecUnitInitStatus initializing = executer.queryUnitInitStatus("SHFE.ag.2512");
		EXPECT_EQ(initializing.state, ExecUnitInitState::INITIALIZING);
		EXPECT_GT(initializing.generation, 0u);
		const uint32_t waiter_count = 6;
		std::atomic<uint32_t> started(0);
		std::atomic<uint32_t> returned(0);
		std::vector<std::thread> waiters;
		waiters.reserve(waiter_count);
		for (uint32_t i = 0; i < waiter_count; ++i)
		{
			waiters.emplace_back([&executer, &started, &returned, waiterDelta]() {
				started.fetch_add(1);
				executer.on_position_changed("SHFE.ag.2512", waiterDelta);
				returned.fetch_add(1);
			});
		}
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (started.load() != waiter_count && std::chrono::steady_clock::now() < deadline)
			std::this_thread::yield();
		EXPECT_EQ(started.load(), waiter_count);
		ExecUnitInitStatus pending = initializing;
		{
			const auto return_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (returned.load() < waiter_count &&
				std::chrono::steady_clock::now() < return_deadline)
				std::this_thread::yield();
			pending = executer.queryUnitInitStatus("SHFE.ag.2512");
			EXPECT_EQ(returned.load(), waiter_count)
				<< (behavior == R71PendingCreationBehavior::LOCAL_COALESCES_TARGET ?
					"Local coalesces in-flight targets without waiting" :
					behavior == R71PendingCreationBehavior::ARBI_RETURNS_BEFORE_REGISTRATION ?
					"Arbi records the target and returns before registration" :
					"Diff records the target and returns while its creator is pending");
			EXPECT_EQ(pending.waiting_callers, 0u)
				<< "Local, Arbi and Diff do not expose these callers as blocked waiters";
		}
		EXPECT_EQ(pending.generation, initializing.generation);

		blockingStub.releaseFirstCommodityLookup();
		creator.join();
		for (std::thread& waiter : waiters)
			waiter.join();

		EXPECT_EQ(blockingStub.commodity_queries.load(), 1u);
		const ExecUnitInitStatus failed = executer.queryUnitInitStatus("SHFE.ag.2512");
		EXPECT_EQ(failed.state, ExecUnitInitState::FAILED);
		EXPECT_EQ(failed.generation, initializing.generation);
	}
};

// 覆盖 WtLocalExecuter.cpp:170-222 getUnit、L274-316 查询、L465-495 on_position_changed、
// L667-723/737-791/798-857/875-902 单元回调转发
TEST_F(R71ExecTest, LocalUnitLifecycleAndCallbackDispatch)
{
    R71CapturingTraderApi api;
    TraderAdapter isolatedTrader;
    ASSERT_TRUE(traderEnv.policy_ok());
    ASSERT_TRUE(isolatedTrader.initExt("r71_local_case", &api, &traderEnv.bd, traderEnv.policy()));
    ASSERT_TRUE(isolatedTrader.run());
    isolatedTrader.onLoginResult(true, "r71", 20250110);
    isolatedTrader.onRspPosition(static_cast<const WTSArray*>(nullptr));
    isolatedTrader.onRspOrders(static_cast<const WTSArray*>(nullptr));
    isolatedTrader.onRspTrades(static_cast<const WTSArray*>(nullptr));
    isolatedTrader.onRspAccount(static_cast<WTSArray*>(nullptr));
    ASSERT_EQ(isolatedTrader.state(), TraderAdapter::AS_ALLREADY);

    WtExecuterMgr localManager;
    std::shared_ptr<WtLocalExecuter> local(new WtLocalExecuter(&factory, "r71_local", &dm));
    R71LocalExecQuiesceGuard quiesceGuard(localManager, isolatedTrader, local.get());
    localManager.add_executer(local);
    local->setStub(&stub);
    WTSVariant* c = cfg("r71_local", "WtExeFact.WtMinImpactExeUnit", NULL);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(local->init(c));
    c->release();
    local->setTrader(&isolatedTrader);
    isolatedTrader.addSink(local.get());

    const char* const code = "SHFE.ag.2512";
    WTSContractInfo* agContract = traderEnv.bd.getContract("ag2512", "SHFE");
    ASSERT_NE(agContract, nullptr);
    const uint32_t base = api.n_insert;
	// 第一次仓位变化负责自动创建单元，目标 2
    local->on_position_changed(code, 2.0);
    WTSTickData* t1 = tick(93100000, 3500.0);
    ASSERT_NE(t1, nullptr);
    local->on_tick(code, t1);
    t1->release();
    ASSERT_EQ(api.n_insert, base + 1);
    ASSERT_NE(api.last_localid, 0u);
    const uint32_t firstLocalId = api.last_localid;
    const std::string firstUsertag = api.last_usertag;
    const double firstPrice = api.last_price;
    const double firstVolume = api.last_vol;
    const WTSDirectionType firstDirection = api.last_dir;
    const WTSOffsetType firstOffset = api.last_off;
    EXPECT_DOUBLE_EQ(isolatedTrader.getUndoneQty(code), 1.0);
    OrderMap* orders = local->getOrders(code);
    EXPECT_EQ(orders, nullptr); // the API accepted the insert, but no adapter order report arrived
    if (orders != nullptr)
        orders->release();

	// The next target is retained, while a still-pending first order blocks a duplicate insert.
    local->on_position_changed(code, 1.0);
    WTSTickData* t2 = tick(93101000, 3501.0);
    ASSERT_NE(t2, nullptr);
    local->on_tick(code, t2);
    t2->release();
    EXPECT_EQ(api.n_insert, base + 1);
    EXPECT_DOUBLE_EQ(isolatedTrader.getUndoneQty(code), 1.0);

    // Direct executor callbacks reach the unit, but do not update the adapter ledger or order map.
    local->on_order(firstLocalId, code, true, firstVolume, firstVolume, firstPrice, false);
    local->on_trade(firstLocalId, code, true, firstVolume, firstPrice);
    local->on_entrust(firstLocalId, code, true, "ok");
    local->on_account("CNY", 1, 1, 1, 1, 0, 0, 0, 0, 0, 0);
    EXPECT_DOUBLE_EQ(local->getPosition(code), 0.0);
    EXPECT_DOUBLE_EQ(local->getUndoneQty(code), 1.0);
    orders = local->getOrders(code);
    EXPECT_EQ(orders, nullptr);
    if (orders != nullptr)
        orders->release();

    // A report through TraderAdapter creates the real order snapshot.
    WTSOrderInfo* pendingOrder = r71MakeOrderReport(agContract, firstUsertag, "R71-LOCAL-1",
        firstDirection, firstOffset, firstPrice, firstVolume, 0.0, firstVolume, WOS_NotTraded_Queuing);
    ASSERT_NE(pendingOrder, nullptr);
    isolatedTrader.onPushOrder(pendingOrder);
    pendingOrder->release();
    orders = local->getOrders(code);
    ASSERT_NE(orders, nullptr);
    ASSERT_EQ(orders->size(), 1u);
    orders->release();
    EXPECT_DOUBLE_EQ(local->getUndoneQty(code), 1.0);
    EXPECT_DOUBLE_EQ(local->getPosition(code), 0.0);

	// 单元已存在 -> 通道事件同步转发
    local->on_channel_lost();
    local->on_channel_ready();
    EXPECT_FALSE(local->cancel(42u));
    local->on_channel_lost();
    EXPECT_TRUE(local->buy(code, 3500.0, 1.0).empty());
    local->on_channel_ready();
    EXPECT_EQ(local->buy(code, 3500.0, 1.0).size(), 1u);

	// on_position 先登记通道持仓；hot 返回空 -> 自动清理链早退
    local->on_position(code, true, 1, 1, 1, 1, 20250110);
    EXPECT_EQ(traderEnv.hot.n_prev_, 1u);

	// data_mgr 非空 -> 行情查询转发
    WTSTickSlice* ticks = local->getTicks(code, 1);
    ASSERT_NE(ticks, nullptr);
    ticks->release();
    WTSTickData* last = local->grabLastTick(code);
    ASSERT_NE(last, nullptr);
    last->release();
}

// 覆盖 WtLocalExecuter.cpp:75-158 init（含 string 型 includes/excludes）、
// L505-658 set_position 的组合匹配/目标循环/过期清零/strict_sync
TEST_F(R71ExecTest, LocalSetPositionGroupsStaleAndStrictSync)
{
    std::shared_ptr<WtLocalExecuter> local = ownUntilQuiesced(
		std::shared_ptr<WtLocalExecuter>(new WtLocalExecuter(&factory, "r71_local_pos", &dm)));
    local->setStub(&stub);
    WTSVariant* c = cfg("r71_local_pos", "WtExeFact.WtMinImpactExeUnit",
        "\"strict_sync\":true,\"clear\":{\"active\":true,\"includes\":\"SHFE.ag\",\"excludes\":\"DCE.i\"},");
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(local->init(c));
    c->release();
    local->setTrader(&trader);
    trader.addSink(local.get());

    // 目标集合里显式登记通道持仓之外的 cu
    local->on_position("SHFE.cu.2510", true, 0, 0, 1, 1, 20250110);
    EXPECT_EQ(traderEnv.hot.n_prev_, 1u); // 上面这行 hot 调用不算，自动清理分支随后再查

    wt_hashmap<std::string, double> targets;
    targets["SHFE.rb.2510"] = 3.0;
    targets["SHFE.au.2212"] = 3.0;
    local->set_position(targets);

    // 新目标为空：旧目标清零；strict_sync 再清 cu（不在 _target_pos）
    wt_hashmap<std::string, double> empty;
    local->set_position(empty);
    WTSTickData* t3 = tick(93102000, 3500.0, "SHFE.cu.2510");
    ASSERT_NE(t3, nullptr);
    local->on_tick("SHFE.cu.2510", t3);
    t3->release();
}

// 覆盖 WtArbiExecuter.cpp:163-202 getUnit、L217-281 查询、L404-540 set_position、
// L548-698 行情/回报转发、L707-801 通道/资金转发
TEST_F(R71ExecTest, ArbiUnitDispatchAndSetPosition)
{
	R71CapturingTraderApi api;
	TraderAdapter isolatedTrader;
	ASSERT_TRUE(traderEnv.policy_ok());
	ASSERT_TRUE(isolatedTrader.initExt("r71_exec_case", &api, &traderEnv.bd, traderEnv.policy()));
	ASSERT_TRUE(isolatedTrader.run());
	isolatedTrader.onLoginResult(true, "r71", 20250110);
	isolatedTrader.onRspPosition(static_cast<const WTSArray*>(nullptr));
	isolatedTrader.onRspOrders(static_cast<const WTSArray*>(nullptr));
	isolatedTrader.onRspTrades(static_cast<const WTSArray*>(nullptr));
	isolatedTrader.onRspAccount(static_cast<WTSArray*>(nullptr));
	ASSERT_EQ(isolatedTrader.state(), TraderAdapter::AS_ALLREADY);

	WtExecuterMgr localManager;
	std::shared_ptr<WtArbiExecuter> arbi(
		new WtArbiExecuter(&factory, "r71_arbi", &dm));
	R71LocalExecQuiesceGuard quiesceGuard(localManager, isolatedTrader, arbi.get());
	localManager.add_executer(arbi);
    arbi->setStub(&stub);
    WTSVariant* c = cfg("r71_arbi", "WtExeFact.WtMinImpactExeUnit",
        "\"strict_sync\":true,\"groups\":{\"R71G\":{\"SHFE.rb.2510\":2.0,\"SHFE.au.2212\":2.0}},");
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(arbi->init(c));
    c->release();
    arbi->setTrader(&isolatedTrader);
    isolatedTrader.addSink(arbi.get());

    const uint32_t base = api.n_insert;
    arbi->on_position_changed("SHFE.ag.2512", 2.0);
    WTSTickData* t1 = tick(93100000, 3500.0);
    ASSERT_NE(t1, nullptr);
    arbi->on_tick("SHFE.ag.2512", t1);
    t1->release();
    ASSERT_EQ(api.n_insert, base + 1);
    ASSERT_NE(api.last_localid, 0u);
    EXPECT_DOUBLE_EQ(isolatedTrader.getUndoneQty("SHFE.ag.2512"), 1.0);
    OrderMap* orders = arbi->getOrders("SHFE.ag.2512");
    EXPECT_EQ(orders, nullptr); // orderInsert accepted, but no order report arrived yet
    if (orders != nullptr)
        orders->release();

    arbi->on_position_changed("SHFE.ag.2512", 1.0);
    WTSTickData* t2 = tick(93101000, 3501.0);
    ASSERT_NE(t2, nullptr);
    arbi->on_tick("SHFE.ag.2512", t2);
    t2->release();
    EXPECT_EQ(api.n_insert, base + 1); // the first order is still pending
    EXPECT_DOUBLE_EQ(isolatedTrader.getUndoneQty("SHFE.ag.2512"), 1.0);

    // 组合匹配：3 手按 2 手一组可合成 1 组
    wt_hashmap<std::string, double> targets;
    targets["SHFE.rb.2510"] = 3.0;
    targets["SHFE.au.2212"] = 3.0;
    arbi->set_position(targets);

    arbi->on_position("SHFE.cu.2510", true, 0, 0, 1, 1, 20250110);
    wt_hashmap<std::string, double> empty;
    arbi->set_position(empty); // 旧目标清零 + strict_sync 清 cu

    arbi->on_order(api.last_localid, "SHFE.ag.2512", true, 1, api.last_price, false);
    arbi->on_trade(api.last_localid, "SHFE.ag.2512", true, 1, api.last_price);
    arbi->on_entrust(api.last_localid, "SHFE.ag.2512", true, "ok");
    arbi->on_account("CNY", 1, 1, 1, 1, 0, 0, 0, 0, 0, 0);
    arbi->on_channel_lost();
    arbi->on_channel_ready();

    EXPECT_DOUBLE_EQ(arbi->getPosition("SHFE.ag.2512"), 0.0);
    EXPECT_DOUBLE_EQ(isolatedTrader.getUndoneQty("SHFE.ag.2512"), 1.0);
    orders = arbi->getOrders("SHFE.ag.2512");
    EXPECT_EQ(orders, nullptr); // direct executor callbacks do not mutate TraderAdapter
    if (orders != nullptr)
        orders->release();

    // A real adapter order report, unlike the direct executor callbacks above,
    // creates the local snapshot. Use the local ID emitted in the submitted user tag.
    WTSContractInfo* agContract = traderEnv.bd.getContract("ag2512", "SHFE");
    ASSERT_NE(agContract, nullptr);
    WTSOrderInfo* pendingOrder = WTSOrderInfo::create();
    ASSERT_NE(pendingOrder, nullptr);
    pendingOrder->setContractInfo(agContract);
    pendingOrder->setDirection(api.last_dir);
    pendingOrder->setOffsetType(api.last_off);
    pendingOrder->setPrice(api.last_price);
    pendingOrder->setVolume(api.last_vol);
    pendingOrder->setVolLeft(api.last_vol);
    pendingOrder->setUserTag(api.last_usertag.c_str());
    pendingOrder->setOrderID("R71-ARBI-1");
    pendingOrder->setOrderState(WOS_NotTraded_Queuing);
    isolatedTrader.onPushOrder(pendingOrder);
    pendingOrder->release();

    orders = arbi->getOrders("SHFE.ag.2512");
    ASSERT_NE(orders, nullptr);
    EXPECT_EQ(orders->size(), 1u);
    orders->release();
    EXPECT_DOUBLE_EQ(isolatedTrader.getUndoneQty("SHFE.ag.2512"), 1.0);

    WTSTickSlice* ticks = arbi->getTicks("SHFE.ag.2512", 1);
    ASSERT_NE(ticks, nullptr);
    ticks->release();
    WTSTickData* last = arbi->grabLastTick("SHFE.ag.2512");
    ASSERT_NE(last, nullptr);
    last->release();
}

TEST_F(R71ExecTest, ArbiInitialTargetReplaySubmitsOnFirstTick)
{
	std::shared_ptr<WtArbiExecuter> arbi = ownUntilQuiesced(
		std::shared_ptr<WtArbiExecuter>(new WtArbiExecuter(&factory, "r71_arbi_initial_target", &dm)));
	arbi->setStub(&stub);
	WTSVariant* c = cfg("r71_arbi_initial_target", "WtExeFact.WtMinImpactExeUnit", NULL);
	ASSERT_NE(c, nullptr);
	ASSERT_TRUE(arbi->init(c));
	c->release();
	arbi->setTrader(&trader);
	trader.addSink(arbi.get());
	ASSERT_EQ(trader.state(), TraderAdapter::AS_ALLREADY);

	const uint32_t base = traderEnv.api.n_insert;
	arbi->on_position_changed("SHFE.ag.2512", 2.0);
	const ExecUnitInitStatus status = arbi->queryUnitInitStatus("SHFE.ag.2512");
	ASSERT_EQ(status.query_result, ExecUnitInitQueryResult::OK);
	ASSERT_EQ(status.state, ExecUnitInitState::READY);
	EXPECT_EQ(traderEnv.api.n_insert, base);

	WTSTickData* marketTick = tick(93100000, 3500.0);
	ASSERT_NE(marketTick, nullptr);
	arbi->on_tick("SHFE.ag.2512", marketTick);
	marketTick->release();

	EXPECT_EQ(traderEnv.api.n_insert, base + 1);
	EXPECT_DOUBLE_EQ(traderEnv.api.last_vol, 1.0);
	EXPECT_EQ(traderEnv.api.last_dir, WDT_LONG);
	EXPECT_EQ(traderEnv.api.last_off, WOT_OPEN);
}

TEST_F(R71ExecTest, ArbiReadyWithoutActionableTargetDoesNotSubmit)
{
	std::shared_ptr<WtArbiExecuter> arbi = ownUntilQuiesced(
		std::shared_ptr<WtArbiExecuter>(new WtArbiExecuter(&factory, "r71_arbi_no_target", &dm)));
	arbi->setStub(&stub);
	WTSVariant* c = cfg("r71_arbi_no_target", "WtExeFact.WtMinImpactExeUnit", NULL);
	ASSERT_NE(c, nullptr);
	ASSERT_TRUE(arbi->init(c));
	c->release();
	arbi->setTrader(&trader);
	trader.addSink(arbi.get());
	ASSERT_EQ(trader.state(), TraderAdapter::AS_ALLREADY);

	const uint32_t base = traderEnv.api.n_insert;
	WTSTickData* tickBeforeTarget = tick(93100000, 3500.0);
	ASSERT_NE(tickBeforeTarget, nullptr);
	arbi->on_tick("SHFE.ag.2512", tickBeforeTarget);
	tickBeforeTarget->release();
	EXPECT_EQ(arbi->queryUnitInitStatus("SHFE.ag.2512").state, ExecUnitInitState::UNKNOWN);

	// A zero target creates the unit after the ready event but is not an order target.
	arbi->on_position_changed("SHFE.ag.2512", 0.0);
	const ExecUnitInitStatus status = arbi->queryUnitInitStatus("SHFE.ag.2512");
	ASSERT_EQ(status.query_result, ExecUnitInitQueryResult::OK);
	ASSERT_EQ(status.state, ExecUnitInitState::READY);
	WTSTickData* marketTick = tick(93101000, 3501.0);
	ASSERT_NE(marketTick, nullptr);
	arbi->on_tick("SHFE.ag.2512", marketTick);
	marketTick->release();

	EXPECT_EQ(traderEnv.api.n_insert, base);
}

// 覆盖 WtDiffExecuter.cpp:243-286 getUnit、L304-368 查询、L501-545 on_position_changed、
// L554-642 set_position、L650-817 行情/差量与通道恢复、L862-884 on_account
TEST_F(R71ExecTest, DiffUnitDispatchAndChannelRecovery)
{
    R71CapturingTraderApi api;
    TraderAdapter isolatedTrader;
    ASSERT_TRUE(traderEnv.policy_ok());
    ASSERT_TRUE(isolatedTrader.initExt("r71_diff_case", &api, &traderEnv.bd, traderEnv.policy()));
    ASSERT_TRUE(isolatedTrader.run());
    isolatedTrader.onLoginResult(true, "r71", 20250110);
    isolatedTrader.onRspPosition(static_cast<const WTSArray*>(nullptr));
    isolatedTrader.onRspOrders(static_cast<const WTSArray*>(nullptr));
    isolatedTrader.onRspTrades(static_cast<const WTSArray*>(nullptr));
    isolatedTrader.onRspAccount(static_cast<WTSArray*>(nullptr));
    ASSERT_EQ(isolatedTrader.state(), TraderAdapter::AS_ALLREADY);

    WtExecuterMgr localManager;
    std::shared_ptr<WtDiffExecuter> diff(
		new WtDiffExecuter(&factory, "r71_diff", &dm, &traderEnv.bd));
    R71LocalExecQuiesceGuard quiesceGuard(localManager, isolatedTrader, diff.get());
    localManager.add_executer(diff);
    diff->setStub(&stub);
    WTSVariant* c = cfg("r71_diff", "WtExeFact.WtDiffMinImpactExeUnit",
        NULL);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(diff->init(c));
    c->release();
    diff->setTrader(&isolatedTrader);
    isolatedTrader.addSink(diff.get());

    const char* const code = "SHFE.ag.2512";
    WTSContractInfo* agContract = traderEnv.bd.getContract("ag2512", "SHFE");
    ASSERT_NE(agContract, nullptr);
    const uint32_t base = api.n_insert;
    diff->on_position_changed(code, 2.0);
    WTSTickData* t1 = tick(93100000, 3500.0);
    ASSERT_NE(t1, nullptr);
    diff->on_tick(code, t1);
    t1->release();
    ASSERT_EQ(api.n_insert, base + 1);
    ASSERT_NE(api.last_localid, 0u);
    const uint32_t localid = api.last_localid;
    const std::string usertag = api.last_usertag;
    const double price = api.last_price;
    const double volume = api.last_vol;
    const WTSDirectionType direction = api.last_dir;
    const WTSOffsetType offset = api.last_off;
    EXPECT_DOUBLE_EQ(diff->getUndoneQty(code), 1.0);
    OrderMap* orders = diff->getOrders(code);
    EXPECT_EQ(orders, nullptr); // orderInsert accepted, but no adapter order report arrived
    if (orders != nullptr)
        orders->release();

    // Direct executor reports use the captured ID but do not mutate TraderAdapter's ledger.
    diff->on_trade(localid, code, true, volume, price);
    EXPECT_DOUBLE_EQ(diff->getPosition(code), 0.0);
    EXPECT_DOUBLE_EQ(diff->getUndoneQty(code), 1.0);
    orders = diff->getOrders(code);
    EXPECT_EQ(orders, nullptr);
    if (orders != nullptr)
        orders->release();

    wt_hashmap<std::string, double> targets;
    targets[code] = 3.0;
    diff->set_position(targets);
    WTSTickData* t2 = tick(93101000, 3502.0);
    ASSERT_NE(t2, nullptr);
    diff->on_tick(code, t2);
    t2->release();
    EXPECT_EQ(api.n_insert, base + 1); // the first order is still pending
    EXPECT_DOUBLE_EQ(diff->getUndoneQty(code), 1.0);

    diff->on_order(localid, code, true, volume, volume, price, false);
    diff->on_entrust(localid, code, true, "ok");
    diff->on_account("CNY", 1, 1, 1, 1, 0, 0, 0, 0, 0, 0);

    WTSOrderInfo* pendingOrder = r71MakeOrderReport(agContract, usertag, "R71-DIFF-1",
        direction, offset, price, volume, 0.0, volume, WOS_NotTraded_Queuing);
    ASSERT_NE(pendingOrder, nullptr);
    isolatedTrader.onPushOrder(pendingOrder);
    pendingOrder->release();
    orders = diff->getOrders(code);
    ASSERT_NE(orders, nullptr);
    EXPECT_EQ(orders->size(), 1u);
    orders->release();

    diff->on_channel_lost();
    diff->on_channel_ready(); // 恢复 _diff_pos
    EXPECT_EQ(api.n_insert, base + 1);
    EXPECT_DOUBLE_EQ(diff->getUndoneQty(code), 1.0);

    // 空目标 -> 过期目标清零（_bd_mgr 已登记标准代码别名，能进入 set_position(0) 分支）
    wt_hashmap<std::string, double> empty;
    diff->set_position(empty);

    EXPECT_EQ(api.n_insert, base + 1);
    EXPECT_DOUBLE_EQ(diff->getPosition(code), 0.0);
    EXPECT_DOUBLE_EQ(diff->getUndoneQty(code), 1.0);
    orders = diff->getOrders(code);
    ASSERT_NE(orders, nullptr);
    EXPECT_EQ(orders->size(), 1u);
    orders->release();

	WTSTickSlice* ticks = diff->getTicks(code, 1);
    ASSERT_NE(ticks, nullptr);
    ticks->release();
	WTSTickData* last = diff->grabLastTick(code);
    ASSERT_NE(last, nullptr);
    last->release();
}

// Failed V2 initialization must leave no usable unit behind. The first
// attempt deliberately removes the commodity from the execution context; after
// restoring it, the same code must be able to create a fresh unit and trade.
TEST_F(R71ExecTest, LocalFailedInitializationRetriesAfterV2CleanupWithoutQueuedCallbacks)
{
	const std::string oldStyle = GTEST_FLAG_GET(death_test_style);
	GTEST_FLAG_SET(death_test_style, "threadsafe");
	EXPECT_EXIT({
		const uint32_t base = traderEnv.api.n_insert;
		WtExecuterMgr manager;
		auto local = std::make_shared<WtLocalExecuter>(&factory, "r71_local_retry", &dm);
		manager.add_executer(local);
		local->setStub(&stub);
		WTSVariant* c = cfg("r71_local_retry", "WtExeFact.WtMinImpactExeUnit", NULL);
		ASSERT_NE(c, nullptr);
		ASSERT_TRUE(local->init(c));
		c->release();
		local->setTrader(&trader);
		trader.addSink(local.get());
		// addSink delivers ready immediately. Keep the failed generation callback-free
		// so the V2 synchronous cleanup proof can authorize a later retry.
		local->on_channel_lost();

		stub.comm_ = NULL;
		local->on_position_changed("SHFE.ag.2512", 1.0);
		const ExecUnitInitStatus failed_status = local->queryUnitInitStatus("SHFE.ag.2512");
		ASSERT_EQ(failed_status.query_result, ExecUnitInitQueryResult::OK);
		ASSERT_EQ(failed_status.state, ExecUnitInitState::FAILED);
		ASSERT_GT(failed_status.generation, 0u);
		EXPECT_FALSE(failed_status.error.empty());

		stub.comm_ = comm;
		// A zero delta triggers creation while preserving the original target for replay.
		local->on_position_changed("SHFE.ag.2512", 0.0);
		const ExecUnitInitStatus ready_status = local->queryUnitInitStatus("SHFE.ag.2512");
		EXPECT_EQ(ready_status.query_result, ExecUnitInitQueryResult::OK);
		EXPECT_EQ(ready_status.state, ExecUnitInitState::READY);
		EXPECT_GT(ready_status.generation, failed_status.generation);
		local->on_channel_ready();
		WTSTickData* valid_tick = tick(93101000, 3501.0);
		ASSERT_NE(valid_tick, nullptr);
		local->on_tick("SHFE.ag.2512", valid_tick);
		valid_tick->release();
		EXPECT_EQ(traderEnv.api.n_insert, base + 1);

		stub.comm_ = comm;
		WtExecQuiesceCoordinator coordinator({ &manager });
		const bool quiesced = coordinator.quiesce_all();
		const bool complete = coordinator.is_complete();
		if (!quiesced || !complete || ::testing::Test::HasFailure())
			std::exit(92);
		std::cerr << "r71_local_v2_clean_retry_passed" << std::endl;
		std::exit(0);
	}, ::testing::ExitedWithCode(0), "r71_local_v2_clean_retry_passed");
	GTEST_FLAG_SET(death_test_style, oldStyle);
}

TEST_F(R71ExecTest, ArbiFailedInitializationRetriesAfterV2CleanupAndReplaysOriginalTarget)
{
	const std::string oldStyle = GTEST_FLAG_GET(death_test_style);
	GTEST_FLAG_SET(death_test_style, "threadsafe");
	EXPECT_EXIT({
		const uint32_t base = traderEnv.api.n_insert;
		WtExecuterMgr manager;
		R71BlockingExecStub blockingStub;
		blockingStub.comm_ = comm;
		blockingStub.sess_ = sess;
		blockingStub.hot_ = &traderEnv.hot;
		auto arbi = std::make_shared<WtArbiExecuter>(&factory, "r71_arbi_retry", &dm);
		manager.add_executer(arbi);
		arbi->setStub(&blockingStub);
		WTSVariant* c = cfg("r71_arbi_retry", "WtExeFact.WtMinImpactExeUnit", NULL);
		ASSERT_NE(c, nullptr);
		ASSERT_TRUE(arbi->init(c));
		c->release();
		arbi->setTrader(&trader);
		trader.addSink(arbi.get());

		blockingStub.comm_ = NULL;
		arbi->on_position_changed("SHFE.ag.2512", 1.0);
		const ExecUnitInitStatus failed_status = arbi->queryUnitInitStatus("SHFE.ag.2512");
		ASSERT_EQ(failed_status.query_result, ExecUnitInitQueryResult::OK);
		ASSERT_EQ(failed_status.state, ExecUnitInitState::FAILED);
		ASSERT_GT(failed_status.generation, 0u);
		EXPECT_FALSE(failed_status.error.empty());

		blockingStub.comm_ = comm;
		// Arbi does not accept the ready callback until after successful init.
		// The zero-delta request retries without changing the failed target of one lot.
		arbi->on_position_changed("SHFE.ag.2512", 0.0);
		const ExecUnitInitStatus ready_status = arbi->queryUnitInitStatus("SHFE.ag.2512");
		EXPECT_EQ(ready_status.query_result, ExecUnitInitQueryResult::OK);
		EXPECT_EQ(ready_status.state, ExecUnitInitState::READY);
		EXPECT_GT(ready_status.generation, failed_status.generation);
		EXPECT_EQ(blockingStub.commodity_queries.load(), 2u);

		WTSTickData* valid_tick = tick(93103000, 3501.0);
		ASSERT_NE(valid_tick, nullptr);
		arbi->on_tick("SHFE.ag.2512", valid_tick);
		valid_tick->release();
		EXPECT_EQ(traderEnv.api.n_insert, base + 1);

		WtExecQuiesceCoordinator coordinator({ &manager });
		const bool quiesced = coordinator.quiesce_all();
		const bool complete = coordinator.is_complete();
		if (!quiesced || !complete || ::testing::Test::HasFailure())
			std::exit(92);
		std::cerr << "r71_arbi_v2_clean_retry_replayed_original_target" << std::endl;
		std::exit(0);
	}, ::testing::ExitedWithCode(0), "r71_arbi_v2_clean_retry_replayed_original_target");
	GTEST_FLAG_SET(death_test_style, oldStyle);
}

TEST_F(R71ExecTest, DiffV2FailedInitializationStillRequiresSafeAutomaticRetry)
{
	// This is an explicit Iteration 02 acceptance requirement. Keep the READY/new
	// generation expectation; the current Diff retry_blocked path is the product gap.
	const std::string oldStyle = GTEST_FLAG_GET(death_test_style);
	GTEST_FLAG_SET(death_test_style, "threadsafe");
	EXPECT_EXIT({
		const uint32_t base = traderEnv.api.n_insert;
		WtExecuterMgr manager;
		R71BlockingExecStub blockingStub;
		blockingStub.comm_ = comm;
		blockingStub.sess_ = sess;
		blockingStub.hot_ = &traderEnv.hot;
		auto diff = std::make_shared<WtDiffExecuter>(&factory, "r71_diff_v2_retry_requirement", &dm,
			&traderEnv.bd);
		manager.add_executer(diff);
		diff->setStub(&blockingStub);
		WTSVariant* c = cfg("r71_diff_v2_retry_requirement", "WtExeFact.WtDiffMinImpactExeUnit", NULL);
		ASSERT_NE(c, nullptr);
		ASSERT_TRUE(diff->init(c));
		c->release();
		diff->setTrader(&trader);
		trader.addSink(diff.get());

		blockingStub.comm_ = NULL;
		diff->on_position_changed("SHFE.ag.2512", 1.0);
		const ExecUnitInitStatus failed_status = diff->queryUnitInitStatus("SHFE.ag.2512");
		ASSERT_EQ(failed_status.query_result, ExecUnitInitQueryResult::OK);
		ASSERT_EQ(failed_status.state, ExecUnitInitState::FAILED);
		ASSERT_GT(failed_status.generation, 0u);
		EXPECT_FALSE(failed_status.error.empty());

		blockingStub.comm_ = comm;
		// The failed call has returned and committed target=1 before this absolute
		// target request. ABI 2.2 cleanup is proven and no callback was accepted in
		// the failed generation, so this remains an explicit retry acceptance item.
		R71PositionTargets recoveredTargets;
		recoveredTargets["SHFE.ag.2512"] = 1.0;
		diff->set_position(recoveredTargets);
		const ExecUnitInitStatus ready_status = diff->queryUnitInitStatus("SHFE.ag.2512");
		if (ready_status.query_result != ExecUnitInitQueryResult::OK ||
			ready_status.state != ExecUnitInitState::READY ||
			ready_status.generation <= failed_status.generation ||
			blockingStub.commodity_queries.load() != 2u)
		{
			std::cerr << "R71_DIFF_V2_SAFE_RETRY_CHECK: retry did not start a ready generation; state="
				<< static_cast<int>(ready_status.state) << " failed_generation="
				<< failed_status.generation << " observed_generation=" << ready_status.generation
				<< " commodity_queries=" << blockingStub.commodity_queries.load() << std::endl;
			std::exit(94);
		}

		WTSTickData* valid_tick = tick(93105000, 3501.0);
		ASSERT_NE(valid_tick, nullptr);
		diff->on_tick("SHFE.ag.2512", valid_tick);
		valid_tick->release();
		if (traderEnv.api.n_insert != base + 1)
		{
			std::cerr << "R71_DIFF_V2_SAFE_RETRY_CHECK: recovered unit did not dispatch the target"
				<< std::endl;
			std::exit(95);
		}
		std::cerr << "R71_DIFF_V2_SAFE_RETRY_CHECK: passed" << std::endl;
		WtExecQuiesceCoordinator coordinator({ &manager });
		(void)coordinator.quiesce_all();
		std::exit(0);
	}, ::testing::ExitedWithCode(0), "R71_DIFF_V2_SAFE_RETRY_CHECK");
	GTEST_FLAG_SET(death_test_style, oldStyle);
}

// 决策 K06=B（2026-11-04 用户裁定）：可证明无副作用的创建失败（自动重试允许）在后续代创建成功后，
// 协调 quiesce 可以完成；未重试成功前保持不完整。改前任一创建失败都使 quiesce 永久不完整。
TEST_F(R71ExecTest, K06DiffCleanFailureFollowedBySuccessfulRetryLetsQuiesceComplete)
{
	const std::string oldStyle = GTEST_FLAG_GET(death_test_style);
	GTEST_FLAG_SET(death_test_style, "threadsafe");
	EXPECT_EXIT({
		WtExecuterMgr manager;
		R71BlockingExecStub blockingStub;
		blockingStub.comm_ = comm;
		blockingStub.sess_ = sess;
		blockingStub.hot_ = &traderEnv.hot;
		auto diff = std::make_shared<WtDiffExecuter>(&factory, "r71_k06_retry", &dm, &traderEnv.bd);
		manager.add_executer(diff);
		diff->setStub(&blockingStub);
		WTSVariant* c = cfg("r71_k06_retry", "WtExeFact.WtDiffMinImpactExeUnit", NULL);
		if (c == NULL || !diff->init(c))
			std::exit(90);
		c->release();
		diff->setTrader(&trader);
		trader.addSink(diff.get());

		blockingStub.comm_ = NULL;	// clean failure before any side effect
		diff->on_position_changed("SHFE.ag.2512", 1.0);
		if (diff->queryUnitInitStatus("SHFE.ag.2512").state != ExecUnitInitState::FAILED)
			std::exit(91);
		blockingStub.comm_ = comm;
		R71PositionTargets targets;
		targets["SHFE.ag.2512"] = 1.0;
		diff->set_position(targets);
		if (diff->queryUnitInitStatus("SHFE.ag.2512").state != ExecUnitInitState::READY)
			std::exit(92);

		WtExecQuiesceCoordinator coordinator({ &manager });
		const bool quiesced = coordinator.quiesce_all();
		const bool complete = coordinator.is_complete();
		std::cerr << "K06_CLEAN_RETRY quiesced=" << quiesced << " complete=" << complete << std::endl;
		std::exit(quiesced && complete ? 0 : 93);
	}, ::testing::ExitedWithCode(0), "K06_CLEAN_RETRY quiesced=1 complete=1");
	GTEST_FLAG_SET(death_test_style, oldStyle);
}

TEST_F(R71ExecTest, K06DiffCleanFailureWithoutSuccessfulRetryKeepsQuiesceIncomplete)
{
	const std::string oldStyle = GTEST_FLAG_GET(death_test_style);
	GTEST_FLAG_SET(death_test_style, "threadsafe");
	EXPECT_EXIT({
		WtExecuterMgr manager;
		R71BlockingExecStub blockingStub;
		blockingStub.comm_ = comm;
		blockingStub.sess_ = sess;
		blockingStub.hot_ = &traderEnv.hot;
		auto diff = std::make_shared<WtDiffExecuter>(&factory, "r71_k06_noretry", &dm, &traderEnv.bd);
		manager.add_executer(diff);
		diff->setStub(&blockingStub);
		WTSVariant* c = cfg("r71_k06_noretry", "WtExeFact.WtDiffMinImpactExeUnit", NULL);
		if (c == NULL || !diff->init(c))
			std::exit(90);
		c->release();
		diff->setTrader(&trader);
		trader.addSink(diff.get());

		blockingStub.comm_ = NULL;
		diff->on_position_changed("SHFE.ag.2512", 1.0);
		if (diff->queryUnitInitStatus("SHFE.ag.2512").state != ExecUnitInitState::FAILED)
			std::exit(91);

		WtExecQuiesceCoordinator coordinator({ &manager });
		const bool quiesced = coordinator.quiesce_all();
		const bool complete = coordinator.is_complete();
		std::cerr << "K06_NO_RETRY quiesced=" << quiesced << " complete=" << complete << std::endl;
		std::exit(complete ? 93 : 0);
	}, ::testing::ExitedWithCode(0), "K06_NO_RETRY");
	GTEST_FLAG_SET(death_test_style, oldStyle);
}

TEST_F(R71ExecTest, UnitInitializationStatusIsQueryableAndFailedStatusCanBeCleared)
{
	const std::string oldStyle = GTEST_FLAG_GET(death_test_style);
	GTEST_FLAG_SET(death_test_style, "threadsafe");
	EXPECT_EXIT({
		auto local = std::make_shared<WtLocalExecuter>(&factory, "r71_local_status", &dm);
		local->setStub(&stub);
		WTSVariant* c = cfg("r71_local_status", "WtExeFact.WtMinImpactExeUnit", NULL);
		ASSERT_NE(c, nullptr);
		ASSERT_TRUE(local->init(c));
		c->release();
		local->setTrader(&trader);
		trader.addSink(local.get());
		// addSink sends channel-ready immediately for this ready adapter. Move the
		// executor to channel-lost before reserving a unit so no ready callback is
		// accepted into the generation; this isolates the V2 cleanup-proof retry path.
		local->on_channel_lost();

		WtExecuterMgr quiesceManager;
		quiesceManager.add_executer(local);
		WtExecuterMgr manager;
		manager.add_executer(std::make_shared<IExecCommand>("r71_unsupported_status"));
		manager.add_executer(local);
		EXPECT_EQ(manager.query_exec_unit_init_status("missing", "SHFE.ag.2512").query_result,
			ExecUnitInitQueryResult::EXECUTOR_NOT_FOUND);
		EXPECT_EQ(manager.clear_failed_exec_unit_init_status("missing", "SHFE.ag.2512"),
			ExecUnitInitClearResult::EXECUTOR_NOT_FOUND);
		EXPECT_EQ(manager.query_exec_unit_init_status("r71_unsupported_status", "SHFE.ag.2512").query_result,
			ExecUnitInitQueryResult::UNSUPPORTED);
		EXPECT_EQ(manager.clear_failed_exec_unit_init_status("r71_unsupported_status", "SHFE.ag.2512"),
			ExecUnitInitClearResult::UNSUPPORTED);
		EXPECT_EQ(manager.query_exec_unit_init_status(NULL, "SHFE.ag.2512").query_result,
			ExecUnitInitQueryResult::INVALID_ARGUMENT);
		EXPECT_EQ(manager.clear_failed_exec_unit_init_status("r71_local_status", ""),
			ExecUnitInitClearResult::INVALID_ARGUMENT);

		ExecUnitInitStatus absent = manager.query_exec_unit_init_status(
			"r71_local_status", "SHFE.ag.2512");
		EXPECT_EQ(absent.query_result, ExecUnitInitQueryResult::OK);
		EXPECT_EQ(absent.state, ExecUnitInitState::UNKNOWN);
		EXPECT_FALSE(absent.error.empty());

		stub.comm_ = NULL;
		local->on_position_changed("SHFE.ag.2512", 1.0);
		ExecUnitInitStatus failed = manager.query_exec_unit_init_status(
			"r71_local_status", "SHFE.ag.2512");
		EXPECT_EQ(failed.query_result, ExecUnitInitQueryResult::OK);
		ASSERT_EQ(failed.state, ExecUnitInitState::FAILED);
		ASSERT_GT(failed.generation, 0u);
		EXPECT_FALSE(failed.error.empty());
		EXPECT_NE(failed.error.find("execution-unit configuration was rejected"), std::string::npos);

		EXPECT_EQ(manager.clear_failed_exec_unit_init_status("r71_local_status", "SHFE.ag.2512"),
			ExecUnitInitClearResult::CLEARED);
		EXPECT_EQ(manager.clear_failed_exec_unit_init_status("r71_local_status", "SHFE.ag.2512"),
			ExecUnitInitClearResult::NOT_FOUND);
		ExecUnitInitStatus cleared = manager.query_exec_unit_init_status(
			"r71_local_status", "SHFE.ag.2512");
		EXPECT_EQ(cleared.query_result, ExecUnitInitQueryResult::OK);
		EXPECT_EQ(cleared.state, ExecUnitInitState::UNKNOWN);

		stub.comm_ = comm;
		local->on_position_changed("SHFE.ag.2512", 1.0);
		ExecUnitInitStatus ready = manager.query_exec_unit_init_status(
			"r71_local_status", "SHFE.ag.2512");
		EXPECT_EQ(ready.query_result, ExecUnitInitQueryResult::OK);
		EXPECT_EQ(ready.state, ExecUnitInitState::READY);
		EXPECT_GT(ready.generation, failed.generation);
		EXPECT_EQ(manager.clear_failed_exec_unit_init_status("r71_local_status", "SHFE.ag.2512"),
			ExecUnitInitClearResult::NOT_FAILED);
		EXPECT_EQ(manager.query_exec_unit_init_status("r71_local_status", "SHFE.ag.2512").state,
			ExecUnitInitState::READY);

		WtExecQuiesceCoordinator coordinator({ &quiesceManager });
		const bool quiesced = coordinator.quiesce_all();
		const bool complete = coordinator.is_complete();
		if (!quiesced || !complete ||
			::testing::Test::HasFailure())
			std::exit(91);
		std::cerr << "r71_expected_complete_status_quiesce coordinator=true complete=true" << std::endl;
		std::exit(0);
	}, ::testing::ExitedWithCode(0), "r71_expected_complete_status_quiesce");
	GTEST_FLAG_SET(death_test_style, oldStyle);
}

TEST_F(R71ExecTest, ExecManagerRegistrySupportsConcurrentAddsAndSnapshots)
{
	WtExecuterMgr manager;
	manager.add_executer(std::make_shared<IExecCommand>("r71_registry_seed"));
	std::atomic<bool> writer_done(false);
	std::atomic<uint32_t> reader_started(0);
	std::atomic<uint32_t> reader_iterations(0);
	std::atomic<uint32_t> unexpected_query_results(0);

	std::thread writer([&]() {
		while (reader_started.load() == 0)
			std::this_thread::yield();
		for (uint32_t i = 0; i < 128; ++i)
		{
			const std::string id = "r71_registry_" + std::to_string(i);
			manager.add_executer(std::make_shared<IExecCommand>(id.c_str()));
			std::this_thread::yield();
		}
		writer_done.store(true);
	});
	std::thread reader([&]() {
		reader_started.store(1);
		while (!writer_done.load())
		{
			if (manager.query_exec_unit_init_status("r71_registry_seed", "SHFE.ag.2512").query_result !=
				ExecUnitInitQueryResult::UNSUPPORTED)
				unexpected_query_results.fetch_add(1);
			manager.enum_executer([](ExecCmdPtr) {});
			wt_hashmap<std::string, double> empty_targets;
			manager.set_positions(empty_targets);
			manager.handle_pos_change("SHFE.ag.2512", 1.0, 1.0);
			manager.handle_tick("SHFE.ag.2512", NULL);
			manager.add_target_to_cache("SHFE.ag.2512", 1.0);
			manager.commit_cached_targets();
			reader_iterations.fetch_add(1);
		}
	});
	writer.join();
	reader.join();
	EXPECT_GT(reader_iterations.load(), 0u);
	EXPECT_EQ(unexpected_query_results.load(), 0u);

	bool added_from_callback = false;
	manager.enum_executer([&](ExecCmdPtr) {
		if (!added_from_callback)
		{
			added_from_callback = true;
			manager.add_executer(std::make_shared<IExecCommand>("r71_registry_callback_add"));
		}
	});
	EXPECT_TRUE(added_from_callback);
	EXPECT_EQ(manager.query_exec_unit_init_status("r71_registry_callback_add", "SHFE.ag.2512").query_result,
		ExecUnitInitQueryResult::UNSUPPORTED);
}

TEST_F(R71ExecTest, ConcurrentSameCodeCreationAndPreRegistrationChannelEdges)
{
	R71CapturingTraderApi localApi;
	R71CapturingTraderApi arbiApi;
	R71CapturingTraderApi diffApi;
	TraderAdapter localTrader;
	TraderAdapter arbiTrader;
	TraderAdapter diffTrader;
	R71ExecGroupQuiesceGuard quiesceGuard;

    {
		ASSERT_TRUE(buildCapturingTrader(localTrader, localApi, "r71_local_concurrent"));
        std::shared_ptr<R71BlockingExecStub> blockingStub = keepBlockingStub();
        blockingStub->comm_ = comm;
        blockingStub->sess_ = sess;
        blockingStub->hot_ = &traderEnv.hot;
		auto local = std::shared_ptr<R71ObservableExecuter<WtLocalExecuter> >(
			new R71ObservableExecuter<WtLocalExecuter>(&factory, "r71_local_concurrent", &dm));
		verifyConcurrentCreation(local, *blockingStub, "r71_local_concurrent",
			"WtExeFact.WtMinImpactExeUnit", R71PendingCreationBehavior::LOCAL_COALESCES_TARGET,
			2u, quiesceGuard, localTrader, localApi);
    }

    {
		ASSERT_TRUE(buildCapturingTrader(arbiTrader, arbiApi, "r71_arbi_concurrent"));
        std::shared_ptr<R71BlockingExecStub> blockingStub = keepBlockingStub();
        blockingStub->comm_ = comm;
        blockingStub->sess_ = sess;
        blockingStub->hot_ = &traderEnv.hot;
		auto arbi = std::shared_ptr<R71ObservableExecuter<WtArbiExecuter> >(
			new R71ObservableExecuter<WtArbiExecuter>(&factory, "r71_arbi_concurrent", &dm));
		verifyConcurrentCreation(arbi, *blockingStub, "r71_arbi_concurrent",
			"WtExeFact.WtMinImpactExeUnit", R71PendingCreationBehavior::ARBI_RETURNS_BEFORE_REGISTRATION,
			1u, quiesceGuard, arbiTrader, arbiApi);
    }

    {
		ASSERT_TRUE(buildCapturingTrader(diffTrader, diffApi, "r71_diff_concurrent"));
		std::shared_ptr<R71BlockingExecStub> blockingStub = keepBlockingStub();
		blockingStub->comm_ = comm;
		blockingStub->sess_ = sess;
		blockingStub->hot_ = &traderEnv.hot;
		auto diff = std::shared_ptr<R71ObservableExecuter<WtDiffExecuter> >(
			new R71ObservableExecuter<WtDiffExecuter>(&factory, "r71_diff_concurrent", &dm,
				&traderEnv.bd));
		verifyConcurrentCreation(diff, *blockingStub, "r71_diff_concurrent",
			"WtExeFact.WtDiffMinImpactExeUnit", R71PendingCreationBehavior::DIFF_RECORDS_TARGET_BEFORE_REGISTRATION,
			1u, quiesceGuard, diffTrader, diffApi);
    }
}

TEST_F(R71ExecTest, LocalFailedInitializationRequestsCoalesceAndFailClosedAfterReadyCallback)
{
	const std::string oldStyle = GTEST_FLAG_GET(death_test_style);
	GTEST_FLAG_SET(death_test_style, "threadsafe");
	EXPECT_EXIT({
		WtExecuterMgr manager;
		R71BlockingExecStub blockingStub;
		blockingStub.comm_ = comm;
		blockingStub.sess_ = sess;
		blockingStub.hot_ = &traderEnv.hot;
		auto local = std::make_shared<WtLocalExecuter>(
			&factory, "r71_local_failed_waiters", &dm);
		manager.add_executer(local);
		const uint32_t base = traderEnv.api.n_insert;
		verifyFailedInitializationWaiters(*local, blockingStub, "r71_local_failed_waiters",
			"WtExeFact.WtMinImpactExeUnit", R71PendingCreationBehavior::LOCAL_COALESCES_TARGET, 1.0);
		const ExecUnitInitStatus failed = local->queryUnitInitStatus("SHFE.ag.2512");
		ASSERT_EQ(failed.state, ExecUnitInitState::FAILED);
		local->on_position_changed("SHFE.ag.2512", 0.0);
		const ExecUnitInitStatus afterRequest = local->queryUnitInitStatus("SHFE.ag.2512");
		EXPECT_EQ(afterRequest.state, ExecUnitInitState::FAILED);
		EXPECT_EQ(afterRequest.generation, failed.generation);
		EXPECT_EQ(blockingStub.commodity_queries.load(), 1u)
			<< "the queued ready callback makes this generation fail closed";
		EXPECT_EQ(traderEnv.api.n_insert, base);
		WtExecQuiesceCoordinator coordinator({ &manager });
		const bool quiesced = coordinator.quiesce_all();
		const bool complete = coordinator.is_complete();
		if (quiesced || complete || ::testing::Test::HasFailure())
			std::exit(93);
		std::cerr << "r71_local_queued_ready_callback_retry_blocked" << std::endl;
		std::exit(0);
	}, ::testing::ExitedWithCode(0), "r71_local_queued_ready_callback_retry_blocked");
	GTEST_FLAG_SET(death_test_style, oldStyle);
}


TEST_F(R71ExecTest, ArbiFailedInitializationRequestsReturnBeforeRegistration)
{
	const std::string oldStyle = GTEST_FLAG_GET(death_test_style);
	GTEST_FLAG_SET(death_test_style, "threadsafe");
	EXPECT_EXIT({
		WtExecuterMgr manager;
		R71BlockingExecStub blockingStub;
		blockingStub.comm_ = comm;
		blockingStub.sess_ = sess;
		blockingStub.hot_ = &traderEnv.hot;
		auto arbi = std::make_shared<WtArbiExecuter>(
			&factory, "r71_arbi_failed_waiters", &dm);
		manager.add_executer(arbi);
		verifyFailedInitializationWaiters(*arbi, blockingStub, "r71_arbi_failed_waiters",
			"WtExeFact.WtMinImpactExeUnit", R71PendingCreationBehavior::ARBI_RETURNS_BEFORE_REGISTRATION,
			1.0);
		const ExecUnitInitStatus failed = arbi->queryUnitInitStatus("SHFE.ag.2512");
		ASSERT_EQ(failed.state, ExecUnitInitState::FAILED);
		ASSERT_GT(failed.generation, 0u);
		EXPECT_EQ(blockingStub.commodity_queries.load(), 1u)
			<< "the six pre-registration calls return without starting more attempts";
		WtExecQuiesceCoordinator coordinator({ &manager });
		const bool quiesced = coordinator.quiesce_all();
		const bool complete = coordinator.is_complete();
		if (!quiesced || !complete || ::testing::Test::HasFailure())
			std::exit(93);
		std::cerr << "r71_arbi_pre_registration_callers_returned_without_retry" << std::endl;
		std::exit(0);
	}, ::testing::ExitedWithCode(0), "r71_arbi_pre_registration_callers_returned_without_retry");
	GTEST_FLAG_SET(death_test_style, oldStyle);
}


TEST_F(R71ExecTest, DiffFailedInitializationWaitersShareOneGeneration)
{
	const std::string oldStyle = GTEST_FLAG_GET(death_test_style);
	GTEST_FLAG_SET(death_test_style, "threadsafe");
	EXPECT_EXIT({
		WtExecuterMgr manager;
		R71BlockingExecStub blockingStub;
		blockingStub.comm_ = comm;
		blockingStub.sess_ = sess;
		blockingStub.hot_ = &traderEnv.hot;
		auto diff = std::make_shared<WtDiffExecuter>(
			&factory, "r71_diff_failed_waiters", &dm, &traderEnv.bd);
		manager.add_executer(diff);
		verifyFailedInitializationWaiters(*diff, blockingStub, "r71_diff_failed_waiters",
			"WtExeFact.WtDiffMinImpactExeUnit", R71PendingCreationBehavior::DIFF_RECORDS_TARGET_BEFORE_REGISTRATION,
			1.0);
		const ExecUnitInitStatus failed = diff->queryUnitInitStatus("SHFE.ag.2512");
		ASSERT_EQ(failed.state, ExecUnitInitState::FAILED);
		EXPECT_EQ(blockingStub.commodity_queries.load(), 1u)
			<< "all six callers joined the failed generation before it failed; none is a retry request";
		WtExecQuiesceCoordinator coordinator({ &manager });
		const bool quiesced = coordinator.quiesce_all();
		const bool complete = coordinator.is_complete();
		if (quiesced || complete || ::testing::Test::HasFailure())
			std::exit(93);
		std::cerr << "r71_diff_waiters_shared_failed_generation" << std::endl;
		std::exit(0);
	}, ::testing::ExitedWithCode(0), "r71_diff_waiters_shared_failed_generation");
	GTEST_FLAG_SET(death_test_style, oldStyle);
}

TEST_F(R71ExecTest, SameThreadInitializationReentryFailsWithoutDuplicateUnit)
{
    std::shared_ptr<R71BlockingExecStub> blockingStub = keepBlockingStub();
    blockingStub->comm_ = comm;
    blockingStub->sess_ = sess;
    blockingStub->hot_ = &traderEnv.hot;

    auto local = ownUntilQuiesced(std::shared_ptr<R71ObservableExecuter<WtLocalExecuter> >(
		new R71ObservableExecuter<WtLocalExecuter>(&factory, "r71_local_reentry", &dm)));
    local->setStub(blockingStub.get());
    WTSVariant* params = cfg("r71_local_reentry", "WtExeFact.WtMinImpactExeUnit", NULL);
    ASSERT_NE(params, nullptr);
    ASSERT_TRUE(local->init(params));
    params->release();
    local->setTrader(&trader);
    trader.addSink(local.get());
    local->on_channel_lost();

    std::atomic<bool> reentry_returned(false);
    blockingStub->setFirstCommodityLookupHook([&]() {
        local->on_position_changed("SHFE.ag.2512", 100.0);
        reentry_returned.store(true);
    });
    local->on_position_changed("SHFE.ag.2512", 1.0);
    blockingStub->setFirstCommodityLookupHook(std::function<void()>());
    EXPECT_TRUE(reentry_returned.load());
    EXPECT_EQ(blockingStub->commodity_queries.load(), 1u);

    local->on_channel_ready();
    WTSTickData* marketTick = tick(93100000, 3500.0);
    ASSERT_NE(marketTick, nullptr);
    local->on_tick("SHFE.ag.2512", marketTick);
    marketTick->release();
    EXPECT_EQ(traderEnv.api.n_insert, 1u);
}

TEST_F(R71ExecTest, ThrowingInitializationChannelCallbackDoesNotBlockConcurrentTargetUpdate)
{
	const std::string oldStyle = GTEST_FLAG_GET(death_test_style);
	GTEST_FLAG_SET(death_test_style, "threadsafe");
	EXPECT_EXIT({
		WtExecuterMgr manager;
		R71BlockingExecStub blockingStub;
		blockingStub.comm_ = comm;
		blockingStub.sess_ = sess;
		blockingStub.hot_ = &traderEnv.hot;
		auto local = std::make_shared<R71ObservableExecuter<WtLocalExecuter>>(
			&factory, "r71_local_callback_throw", &dm);
		manager.add_executer(local);
		local->setStub(&blockingStub);
		WTSVariant* params = cfg("r71_local_callback_throw", "WtExeFact.WtMinImpactExeUnit", NULL);
		ASSERT_NE(params, nullptr);
		ASSERT_TRUE(local->init(params));
		params->release();
		local->setTrader(&trader);
		trader.addSink(local.get());
		local->on_channel_lost();
		local->on_channel_ready();
		local->blockFirstReadyCallback(true);

		std::atomic<bool> creator_finished(false);
		std::thread creator([&]() {
			local->on_position_changed("SHFE.ag.2512", 1.0);
			creator_finished.store(true);
		});
		const bool callback_blocked = local->waitForFirstReadyCallback(std::chrono::seconds(2));
		EXPECT_TRUE(callback_blocked);
		if (!callback_blocked)
		{
			local->releaseFirstReadyCallback();
			creator.join();
			std::exit(94);
		}

		const ExecUnitInitStatus initializing = local->queryUnitInitStatus("SHFE.ag.2512");
		EXPECT_EQ(initializing.state, ExecUnitInitState::INITIALIZING);
		std::atomic<bool> waiter_started(false);
		std::atomic<bool> waiter_finished(false);
		std::thread waiter([&]() {
			waiter_started.store(true);
			local->on_position_changed("SHFE.ag.2512", 1.0);
			waiter_finished.store(true);
		});
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (!waiter_started.load() && std::chrono::steady_clock::now() < deadline)
			std::this_thread::yield();
		EXPECT_TRUE(waiter_started.load());
		const auto target_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (!waiter_finished.load() && std::chrono::steady_clock::now() < target_deadline)
		{
			std::this_thread::yield();
		}
		EXPECT_TRUE(waiter_finished.load())
			<< "Local merges target updates while the registered unit drains callbacks";
		EXPECT_EQ(local->queryUnitInitStatus("SHFE.ag.2512").waiting_callers, 0u);
		local->releaseFirstReadyCallback();
		creator.join();
		waiter.join();

		EXPECT_TRUE(creator_finished.load());
		EXPECT_TRUE(waiter_finished.load());
		EXPECT_EQ(blockingStub.commodity_queries.load(), 1u);
		EXPECT_EQ(local->ready_queries.load(), 1u);
		const ExecUnitInitStatus failed = local->queryUnitInitStatus("SHFE.ag.2512");
		EXPECT_EQ(failed.state, ExecUnitInitState::FAILED);
		EXPECT_EQ(failed.generation, initializing.generation);

		// The plugin initialized before the host ready callback threw. Its callback
		// may have produced an external side effect, so keep this generation blocked
		// from automatic retry after the concurrent target update has returned.
		local->on_position_changed("SHFE.ag.2512", 1.0);
		const ExecUnitInitStatus stillFailed = local->queryUnitInitStatus("SHFE.ag.2512");
		EXPECT_EQ(stillFailed.state, ExecUnitInitState::FAILED);
		EXPECT_EQ(stillFailed.generation, failed.generation);
		EXPECT_EQ(local->clearFailedUnitInitStatus("SHFE.ag.2512"),
			ExecUnitInitClearResult::RETRY_BLOCKED);
		EXPECT_EQ(blockingStub.commodity_queries.load(), 1u);
		EXPECT_EQ(local->ready_queries.load(), 1u);

		WTSTickData* marketTick = tick(93100000, 3500.0);
		ASSERT_NE(marketTick, nullptr);
		local->on_tick("SHFE.ag.2512", marketTick);
		marketTick->release();
		EXPECT_EQ(traderEnv.api.n_insert, 0u);

		WtExecQuiesceCoordinator coordinator({ &manager });
		const bool quiesced = coordinator.quiesce_all();
		const bool complete = coordinator.is_complete();
		if (quiesced || complete ||
			::testing::Test::HasFailure())
			std::exit(95);
		std::cerr << "r71_expected_incomplete_callback_throw_quiesce coordinator=false complete=false" << std::endl;
		std::exit(0);
	}, ::testing::ExitedWithCode(0), "r71_expected_incomplete_callback_throw_quiesce");
	GTEST_FLAG_SET(death_test_style, oldStyle);
}

TEST_F(R71ExecTest, AccountFanoutSkipsInitializingUnitUntilReady)
{
	WtExecuterMgr manager;
	R71BlockingExecStub blockingStub;
	blockingStub.comm_ = comm;
	blockingStub.sess_ = sess;
	blockingStub.hot_ = &traderEnv.hot;
	auto local = std::make_shared<R71ObservableExecuter<WtLocalExecuter>>(
		&factory, "r71_local_account_fanout", &dm);
	manager.add_executer(local);
	local->setStub(&blockingStub);
	const std::string policyPath = r71::temp_root() + "r71_local_account_fanout.json";
	const std::string policyConfig =
		R"json({"scale":1.0,"poolsize":0,"policy":{"default":{"name":"WtExeFact.WtStockMinImpactExeUnit","lots":1,"minopenlots":1,"offset":0,"expire":0,"pricemode":0,"span":0,"byrate":false,"min_order":1.0}}})json";
	ASSERT_TRUE(r71::write_file(policyPath, policyConfig));
	WTSVariant* params = WTSCfgLoader::load_from_file(policyPath.c_str());
	ASSERT_NE(params, nullptr);
	ASSERT_TRUE(local->init(params));
	params->release();
	local->setTrader(&trader);
	trader.addSink(local.get());
	R71LocalExecQuiesceGuard quiesceGuard(manager, trader, local.get());
	local->on_channel_lost();
	local->on_channel_ready();
	local->blockFirstReadyCallback(false);

		std::thread creator([local]() {
			local->on_position_changed("SHFE.ag.2512", 1.0);
		});
		const bool callback_blocked = local->waitForFirstReadyCallback(std::chrono::seconds(2));
		EXPECT_TRUE(callback_blocked);
		if (!callback_blocked)
		{
			local->releaseFirstReadyCallback();
			creator.join();
			FAIL() << "unit initialization did not enter the channel ready callback";
			return;
		}

		const ExecUnitInitStatus initializing = local->queryUnitInitStatus("SHFE.ag.2512");
		EXPECT_EQ(initializing.state, ExecUnitInitState::INITIALIZING);
		local->on_account("CNY", 0, 1, 1, 100, 0, 0, 0, 0, 0, 0);
		EXPECT_EQ(local->available_updates.load(), 0u);
		local->releaseFirstReadyCallback();
		creator.join();

		const ExecUnitInitStatus ready = local->queryUnitInitStatus("SHFE.ag.2512");
		EXPECT_EQ(ready.state, ExecUnitInitState::READY);
		EXPECT_EQ(ready.generation, initializing.generation);
		EXPECT_EQ(local->available_updates.load(), 1u);
}

} // namespace

TEST(R71ExecScheduleTest, RejectedChannelDrainRunsFallbackSynchronously)
{
	R71RejectingPool pool;
	uint32_t queued_calls = 0;
	uint32_t fallback_calls = 0;
	const bool queued = wtp::WtExecuterSchedule::scheduleOrRunFallback(
		pool,
		[&queued_calls]() { ++queued_calls; },
		[&fallback_calls]() { ++fallback_calls; });

	EXPECT_FALSE(queued);
	EXPECT_EQ(pool.schedule_calls, 1u);
	EXPECT_EQ(queued_calls, 0u);
	EXPECT_EQ(fallback_calls, 1u);
}
