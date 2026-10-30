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
		return BaseExecuter::getUndoneQty(code);
	}

	void writeLog(const char* message) override
	{
		if (message != NULL && std::strstr(message, "avaliable update") != NULL)
			available_updates.fetch_add(1);
		BaseExecuter::writeLog(message);
	}

	std::atomic<uint32_t> ready_queries{0};
	std::atomic<uint32_t> available_updates{0};

private:
	std::mutex _ready_mutex;
	std::condition_variable _ready_condition;
	bool _block_first_ready = false;
	bool _throw_first_ready = false;
	bool _ready_entered = false;
	bool _ready_released = false;
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
	void verifyConcurrentCreation(Executer& executer, R71BlockingExecStub& blockingStub,
		const char* name, const char* unitName, uint32_t baseInsert)
	{
		executer.setStub(&blockingStub);
		WTSVariant* params = cfg(name, unitName, NULL);
		ASSERT_NE(params, nullptr);
		ASSERT_TRUE(executer.init(params));
		params->release();
		executer.setTrader(&trader);
		trader.addSink(&executer);
		executer.on_channel_lost();
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
		EXPECT_EQ(initializing.query_result, ExecUnitInitQueryResult::OK);
		EXPECT_EQ(initializing.state, ExecUnitInitState::INITIALIZING);
		EXPECT_GT(initializing.generation, 0u);
		EXPECT_TRUE(initializing.error.empty());
		EXPECT_EQ(executer.clearFailedUnitInitStatus("SHFE.ag.2512"),
			ExecUnitInitClearResult::NOT_FAILED);

		std::atomic<bool> second_started(false);
		std::thread waiter([&executer, &second_started]() {
			second_started.store(true);
			executer.on_position_changed("SHFE.ag.2512", 1.0);
		});
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (!second_started.load() && std::chrono::steady_clock::now() < deadline)
			std::this_thread::yield();
		EXPECT_TRUE(second_started.load());
		ExecUnitInitStatus pending = executer.queryUnitInitStatus("SHFE.ag.2512");
		const auto waiter_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (pending.waiting_callers < 1 && std::chrono::steady_clock::now() < waiter_deadline)
		{
			std::this_thread::yield();
			pending = executer.queryUnitInitStatus("SHFE.ag.2512");
		}
		EXPECT_EQ(pending.waiting_callers, 1u);

		// These transitions occur before the candidate is registered. Only the final
		// ready state may be delivered when creation completes; stale events must not replay.
		executer.on_channel_ready();
		executer.on_channel_lost();
		executer.on_channel_ready();
		blockingStub.releaseFirstCommodityLookup();
		creator.join();
		waiter.join();
		const ExecUnitInitStatus ready = executer.queryUnitInitStatus("SHFE.ag.2512");
		EXPECT_EQ(ready.query_result, ExecUnitInitQueryResult::OK);
		EXPECT_EQ(ready.state, ExecUnitInitState::READY);
		EXPECT_EQ(ready.generation, initializing.generation);
		EXPECT_TRUE(ready.error.empty());
		EXPECT_EQ(executer.clearFailedUnitInitStatus("SHFE.ag.2512"),
			ExecUnitInitClearResult::NOT_FAILED);

		EXPECT_EQ(blockingStub.commodity_queries.load(), 1u);
		EXPECT_EQ(executer.ready_queries.load(), 1u);
		for (uint32_t i = 0; i < 2; ++i)
		{
			WTSTickData* marketTick = tick(93100000 + i * 1000, 3500.0 + i);
			ASSERT_NE(marketTick, nullptr);
			executer.on_tick("SHFE.ag.2512", marketTick);
			marketTick->release();
		}
		EXPECT_EQ(traderEnv.api.n_insert, baseInsert + 2);
	}

	template <typename Executer>
	void verifyFailedWaitersDoNotRetry(Executer& executer, R71BlockingExecStub& blockingStub,
		const char* name, const char* unitName)
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
		std::vector<std::thread> waiters;
		waiters.reserve(waiter_count);
		for (uint32_t i = 0; i < waiter_count; ++i)
		{
			waiters.emplace_back([&executer, &started]() {
				started.fetch_add(1);
				executer.on_position_changed("SHFE.ag.2512", 1.0);
			});
		}
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (started.load() != waiter_count && std::chrono::steady_clock::now() < deadline)
			std::this_thread::yield();
		EXPECT_EQ(started.load(), waiter_count);
		ExecUnitInitStatus pending = initializing;
		const auto waiter_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (pending.waiting_callers < waiter_count && std::chrono::steady_clock::now() < waiter_deadline)
		{
			std::this_thread::yield();
			pending = executer.queryUnitInitStatus("SHFE.ag.2512");
		}
		EXPECT_EQ(pending.waiting_callers, waiter_count);

		blockingStub.releaseFirstCommodityLookup();
		creator.join();
		for (std::thread& waiter : waiters)
			waiter.join();

		EXPECT_EQ(blockingStub.commodity_queries.load(), 1u);
		const ExecUnitInitStatus failed = executer.queryUnitInitStatus("SHFE.ag.2512");
		EXPECT_EQ(failed.state, ExecUnitInitState::FAILED);
		EXPECT_EQ(failed.generation, initializing.generation);

		// Only this new call, made after the failed generation completed, starts
		// another attempt for the same contract.
		executer.on_position_changed("SHFE.ag.2512", 1.0);
		const ExecUnitInitStatus retried = executer.queryUnitInitStatus("SHFE.ag.2512");
		EXPECT_EQ(retried.state, ExecUnitInitState::FAILED);
		EXPECT_GT(retried.generation, failed.generation);
		EXPECT_EQ(blockingStub.commodity_queries.load(), 2u);
	}
};

// 覆盖 WtLocalExecuter.cpp:170-222 getUnit、L274-316 查询、L465-495 on_position_changed、
// L667-723/737-791/798-857/875-902 单元回调转发
TEST_F(R71ExecTest, LocalUnitLifecycleAndCallbackDispatch)
{
    std::shared_ptr<WtLocalExecuter> local = ownUntilQuiesced(
		std::shared_ptr<WtLocalExecuter>(new WtLocalExecuter(&factory, "r71_local", &dm)));
    local->setStub(&stub);
    WTSVariant* c = cfg("r71_local", "WtExeFact.WtMinImpactExeUnit", NULL);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(local->init(c));
    c->release();
    local->setTrader(&trader);
    trader.addSink(local.get());

    const uint32_t base = traderEnv.api.n_insert;
    // 第一次仓位变化负责自动创建单元，目标 2
    local->on_position_changed("SHFE.ag.2512", 2.0);
    WTSTickData* t1 = tick(93100000, 3500.0);
    ASSERT_NE(t1, nullptr);
    local->on_tick("SHFE.ag.2512", t1);
    t1->release();
    EXPECT_EQ(traderEnv.api.n_insert, base + 1);

    // 增量目标 -> 单元 set_position；新 tick 触发第二笔
    local->on_position_changed("SHFE.ag.2512", 1.0);
    WTSTickData* t2 = tick(93101000, 3501.0);
    ASSERT_NE(t2, nullptr);
    local->on_tick("SHFE.ag.2512", t2);
    t2->release();
    EXPECT_EQ(traderEnv.api.n_insert, base + 2);

    local->on_order(1, "SHFE.ag.2512", true, 1, 0, 3501.0, false);
    local->on_trade(1, "SHFE.ag.2512", true, 1, 3501.0);
    local->on_entrust(1, "SHFE.ag.2512", true, "ok");
    local->on_account("CNY", 1, 1, 1, 1, 0, 0, 0, 0, 0, 0);

    EXPECT_DOUBLE_EQ(local->getPosition("SHFE.ag.2512"), 0.0);
    EXPECT_DOUBLE_EQ(local->getUndoneQty("SHFE.ag.2512"), 0.0);
    OrderMap* orders = local->getOrders("SHFE.ag.2512");
    ASSERT_NE(orders, nullptr);
    orders->release();

    // 单元已存在 -> 通道事件同步转发
    local->on_channel_lost();
    local->on_channel_ready();
    EXPECT_FALSE(local->cancel(42u));
    local->on_channel_lost();
    EXPECT_TRUE(local->buy("SHFE.ag.2512", 3500.0, 1.0).empty());
    local->on_channel_ready();
    EXPECT_EQ(local->buy("SHFE.ag.2512", 3500.0, 1.0).size(), 1u);

    // on_position 先登记通道持仓；hot 返回空 -> 自动清理链早退
    local->on_position("SHFE.ag.2512", true, 1, 1, 1, 1, 20250110);
    EXPECT_EQ(traderEnv.hot.n_prev_, 1u);

    // data_mgr 非空 -> 行情查询转发
    WTSTickSlice* ticks = local->getTicks("SHFE.ag.2512", 1);
    ASSERT_NE(ticks, nullptr);
    ticks->release();
    WTSTickData* last = local->grabLastTick("SHFE.ag.2512");
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
    std::shared_ptr<WtArbiExecuter> arbi = ownUntilQuiesced(
		std::shared_ptr<WtArbiExecuter>(new WtArbiExecuter(&factory, "r71_arbi", &dm)));
    arbi->setStub(&stub);
    WTSVariant* c = cfg("r71_arbi", "WtExeFact.WtMinImpactExeUnit",
        "\"strict_sync\":true,\"groups\":{\"R71G\":{\"SHFE.rb.2510\":2.0,\"SHFE.au.2212\":2.0}},");
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(arbi->init(c));
    c->release();
    arbi->setTrader(&trader);
    trader.addSink(arbi.get());

    const uint32_t base = traderEnv.api.n_insert;
    arbi->on_position_changed("SHFE.ag.2512", 2.0);
    WTSTickData* t1 = tick(93100000, 3500.0);
    ASSERT_NE(t1, nullptr);
    arbi->on_tick("SHFE.ag.2512", t1);
    t1->release();
    EXPECT_EQ(traderEnv.api.n_insert, base + 1);

    arbi->on_position_changed("SHFE.ag.2512", 1.0);
    WTSTickData* t2 = tick(93101000, 3501.0);
    ASSERT_NE(t2, nullptr);
    arbi->on_tick("SHFE.ag.2512", t2);
    t2->release();
    EXPECT_EQ(traderEnv.api.n_insert, base + 2);

    // 组合匹配：3 手按 2 手一组可合成 1 组
    wt_hashmap<std::string, double> targets;
    targets["SHFE.rb.2510"] = 3.0;
    targets["SHFE.au.2212"] = 3.0;
    arbi->set_position(targets);

    arbi->on_position("SHFE.cu.2510", true, 0, 0, 1, 1, 20250110);
    wt_hashmap<std::string, double> empty;
    arbi->set_position(empty); // 旧目标清零 + strict_sync 清 cu

    arbi->on_order(1, "SHFE.ag.2512", true, 1, 0, 3501.0, false);
    arbi->on_trade(1, "SHFE.ag.2512", true, 1, 3501.0);
    arbi->on_entrust(1, "SHFE.ag.2512", true, "ok");
    arbi->on_account("CNY", 1, 1, 1, 1, 0, 0, 0, 0, 0, 0);
    arbi->on_channel_lost();
    arbi->on_channel_ready();

    EXPECT_DOUBLE_EQ(arbi->getPosition("SHFE.ag.2512"), 0.0);
    EXPECT_DOUBLE_EQ(arbi->getUndoneQty("SHFE.ag.2512"), 0.0);
    OrderMap* orders = arbi->getOrders("SHFE.ag.2512");
    ASSERT_NE(orders, nullptr);
    orders->release();

    WTSTickSlice* ticks = arbi->getTicks("SHFE.ag.2512", 1);
    ASSERT_NE(ticks, nullptr);
    ticks->release();
    WTSTickData* last = arbi->grabLastTick("SHFE.ag.2512");
    ASSERT_NE(last, nullptr);
    last->release();
}

// 覆盖 WtDiffExecuter.cpp:243-286 getUnit、L304-368 查询、L501-545 on_position_changed、
// L554-642 set_position、L650-817 行情/差量与通道恢复、L862-884 on_account
TEST_F(R71ExecTest, DiffUnitDispatchAndChannelRecovery)
{
    std::shared_ptr<WtDiffExecuter> diff = ownUntilQuiesced(
		std::shared_ptr<WtDiffExecuter>(new WtDiffExecuter(&factory, "r71_diff", &dm, &traderEnv.bd)));
    diff->setStub(&stub);
    WTSVariant* c = cfg("r71_diff", "WtExeFact.WtDiffMinImpactExeUnit",
        NULL);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(diff->init(c));
    c->release();
    diff->setTrader(&trader);
    trader.addSink(diff.get());

    const uint32_t base = traderEnv.api.n_insert;
    diff->on_position_changed("SHFE.ag.2512", 2.0);
    WTSTickData* t1 = tick(93100000, 3500.0);
    ASSERT_NE(t1, nullptr);
    diff->on_tick("SHFE.ag.2512", t1);
    t1->release();
    EXPECT_EQ(traderEnv.api.n_insert, base + 1);

    diff->on_trade(1, "SHFE.ag.2512", true, 1.0, 3500.0);

    wt_hashmap<std::string, double> targets;
    targets["SHFE.ag.2512"] = 3.0;
    diff->set_position(targets);
    WTSTickData* t2 = tick(93101000, 3502.0);
    ASSERT_NE(t2, nullptr);
    diff->on_tick("SHFE.ag.2512", t2);
    t2->release();

    diff->on_order(1, "SHFE.ag.2512", true, 1, 0, 3502.0, false);
    diff->on_entrust(1, "SHFE.ag.2512", true, "ok");
    diff->on_channel_lost();
    diff->on_channel_ready(); // 恢复 _diff_pos
    diff->on_account("CNY", 1, 1, 1, 1, 0, 0, 0, 0, 0, 0);

    // 空目标 -> 过期目标清零（_bd_mgr 已登记标准代码别名，能进入 set_position(0) 分支）
    wt_hashmap<std::string, double> empty;
    diff->set_position(empty);

    EXPECT_DOUBLE_EQ(diff->getPosition("SHFE.ag.2512"), 0.0);
    EXPECT_DOUBLE_EQ(diff->getUndoneQty("SHFE.ag.2512"), 0.0);
    OrderMap* orders = diff->getOrders("SHFE.ag.2512");
    ASSERT_NE(orders, nullptr);
    orders->release();

    WTSTickSlice* ticks = diff->getTicks("SHFE.ag.2512", 1);
    ASSERT_NE(ticks, nullptr);
    ticks->release();
    WTSTickData* last = diff->grabLastTick("SHFE.ag.2512");
    ASSERT_NE(last, nullptr);
    last->release();
}

// Failed V2 initialization must leave no usable unit behind. The first
// attempt deliberately removes the commodity from the execution context; after
// restoring it, the same code must be able to create a fresh unit and trade.
TEST_F(R71ExecTest, FailedUnitInitializationCanBeRetriedAcrossExecuterKinds)
{
	const std::string oldStyle = GTEST_FLAG_GET(death_test_style);
	GTEST_FLAG_SET(death_test_style, "threadsafe");
	EXPECT_EXIT({
		const uint32_t base = traderEnv.api.n_insert;
		WtExecuterMgr manager;

		{
			auto local = std::make_shared<WtLocalExecuter>(&factory, "r71_local_retry", &dm);
			manager.add_executer(local);
			local->setStub(&stub);
			WTSVariant* c = cfg("r71_local_retry", "WtExeFact.WtMinImpactExeUnit", NULL);
			ASSERT_NE(c, nullptr);
			ASSERT_TRUE(local->init(c));
			c->release();
			local->setTrader(&trader);
			trader.addSink(local.get());

			stub.comm_ = NULL;
			local->on_position_changed("SHFE.ag.2512", 1.0);
			WTSTickData* failed_tick = tick(93100000, 3500.0);
			ASSERT_NE(failed_tick, nullptr);
			local->on_tick("SHFE.ag.2512", failed_tick);
			failed_tick->release();
			EXPECT_EQ(traderEnv.api.n_insert, base);
			const ExecUnitInitStatus failed_status = local->queryUnitInitStatus("SHFE.ag.2512");
			EXPECT_EQ(failed_status.query_result, ExecUnitInitQueryResult::OK);
			ASSERT_EQ(failed_status.state, ExecUnitInitState::FAILED);
			ASSERT_GT(failed_status.generation, 0u);
			EXPECT_FALSE(failed_status.error.empty());

			stub.comm_ = comm;
			local->on_position_changed("SHFE.ag.2512", 1.0);
			WTSTickData* valid_tick = tick(93101000, 3501.0);
			ASSERT_NE(valid_tick, nullptr);
			local->on_tick("SHFE.ag.2512", valid_tick);
			valid_tick->release();
			EXPECT_EQ(traderEnv.api.n_insert, base + 1);
			const ExecUnitInitStatus ready_status = local->queryUnitInitStatus("SHFE.ag.2512");
			EXPECT_EQ(ready_status.query_result, ExecUnitInitQueryResult::OK);
			EXPECT_EQ(ready_status.state, ExecUnitInitState::READY);
			EXPECT_GT(ready_status.generation, failed_status.generation);
		}

		{
			auto arbi = std::make_shared<WtArbiExecuter>(&factory, "r71_arbi_retry", &dm);
			manager.add_executer(arbi);
			arbi->setStub(&stub);
			WTSVariant* c = cfg("r71_arbi_retry", "WtExeFact.WtMinImpactExeUnit", NULL);
			ASSERT_NE(c, nullptr);
			ASSERT_TRUE(arbi->init(c));
			c->release();
			arbi->setTrader(&trader);
			trader.addSink(arbi.get());

			stub.comm_ = NULL;
			arbi->on_position_changed("SHFE.ag.2512", 1.0);
			WTSTickData* failed_tick = tick(93102000, 3500.0);
			ASSERT_NE(failed_tick, nullptr);
			arbi->on_tick("SHFE.ag.2512", failed_tick);
			failed_tick->release();
			EXPECT_EQ(traderEnv.api.n_insert, base + 1);
			const ExecUnitInitStatus failed_status = arbi->queryUnitInitStatus("SHFE.ag.2512");
			EXPECT_EQ(failed_status.query_result, ExecUnitInitQueryResult::OK);
			ASSERT_EQ(failed_status.state, ExecUnitInitState::FAILED);
			ASSERT_GT(failed_status.generation, 0u);
			EXPECT_FALSE(failed_status.error.empty());

			stub.comm_ = comm;
			arbi->on_position_changed("SHFE.ag.2512", 1.0);
			WTSTickData* valid_tick = tick(93103000, 3501.0);
			ASSERT_NE(valid_tick, nullptr);
			arbi->on_tick("SHFE.ag.2512", valid_tick);
			valid_tick->release();
			EXPECT_EQ(traderEnv.api.n_insert, base + 2);
			const ExecUnitInitStatus ready_status = arbi->queryUnitInitStatus("SHFE.ag.2512");
			EXPECT_EQ(ready_status.query_result, ExecUnitInitQueryResult::OK);
			EXPECT_EQ(ready_status.state, ExecUnitInitState::READY);
			EXPECT_GT(ready_status.generation, failed_status.generation);
		}

		{
			auto diff = std::make_shared<WtDiffExecuter>(&factory, "r71_diff_retry", &dm,
				&traderEnv.bd);
			manager.add_executer(diff);
			diff->setStub(&stub);
			WTSVariant* c = cfg("r71_diff_retry", "WtExeFact.WtDiffMinImpactExeUnit", NULL);
			ASSERT_NE(c, nullptr);
			ASSERT_TRUE(diff->init(c));
			c->release();
			diff->setTrader(&trader);
			trader.addSink(diff.get());

			stub.comm_ = NULL;
			diff->on_position_changed("SHFE.ag.2512", 1.0);
			WTSTickData* failed_tick = tick(93104000, 3500.0);
			ASSERT_NE(failed_tick, nullptr);
			diff->on_tick("SHFE.ag.2512", failed_tick);
			failed_tick->release();
			EXPECT_EQ(traderEnv.api.n_insert, base + 2);
			const ExecUnitInitStatus failed_status = diff->queryUnitInitStatus("SHFE.ag.2512");
			EXPECT_EQ(failed_status.query_result, ExecUnitInitQueryResult::OK);
			ASSERT_EQ(failed_status.state, ExecUnitInitState::FAILED);
			ASSERT_GT(failed_status.generation, 0u);
			EXPECT_FALSE(failed_status.error.empty());

			stub.comm_ = comm;
			diff->on_position_changed("SHFE.ag.2512", 1.0);
			WTSTickData* valid_tick = tick(93105000, 3501.0);
			ASSERT_NE(valid_tick, nullptr);
			diff->on_tick("SHFE.ag.2512", valid_tick);
			valid_tick->release();
			EXPECT_EQ(traderEnv.api.n_insert, base + 3);
			const ExecUnitInitStatus ready_status = diff->queryUnitInitStatus("SHFE.ag.2512");
			EXPECT_EQ(ready_status.query_result, ExecUnitInitQueryResult::OK);
			EXPECT_EQ(ready_status.state, ExecUnitInitState::READY);
			EXPECT_GT(ready_status.generation, failed_status.generation);
		}

		stub.comm_ = comm;
		WtExecQuiesceCoordinator coordinator({ &manager });
		const bool quiesced = coordinator.quiesce_all();
		const bool complete = coordinator.is_complete();
		if (quiesced || complete ||
			::testing::Test::HasFailure())
			std::exit(92);
		std::cerr << "r71_expected_incomplete_retry_quiesce coordinator=false complete=false" << std::endl;
		std::exit(0);
	}, ::testing::ExitedWithCode(0), "r71_expected_incomplete_retry_quiesce");
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
		if (quiesced || complete ||
			::testing::Test::HasFailure())
			std::exit(91);
		std::cerr << "r71_expected_incomplete_status_quiesce coordinator=false complete=false" << std::endl;
		std::exit(0);
	}, ::testing::ExitedWithCode(0), "r71_expected_incomplete_status_quiesce");
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
    const uint32_t base = traderEnv.api.n_insert;

    {
        std::shared_ptr<R71BlockingExecStub> blockingStub = keepBlockingStub();
        blockingStub->comm_ = comm;
        blockingStub->sess_ = sess;
        blockingStub->hot_ = &traderEnv.hot;
        auto local = ownUntilQuiesced(std::shared_ptr<R71ObservableExecuter<WtLocalExecuter> >(
			new R71ObservableExecuter<WtLocalExecuter>(&factory, "r71_local_concurrent", &dm)));
        verifyConcurrentCreation(*local, *blockingStub, "r71_local_concurrent",
            "WtExeFact.WtMinImpactExeUnit", base);
    }

    {
        std::shared_ptr<R71BlockingExecStub> blockingStub = keepBlockingStub();
        blockingStub->comm_ = comm;
        blockingStub->sess_ = sess;
        blockingStub->hot_ = &traderEnv.hot;
        auto arbi = ownUntilQuiesced(std::shared_ptr<R71ObservableExecuter<WtArbiExecuter> >(
			new R71ObservableExecuter<WtArbiExecuter>(&factory, "r71_arbi_concurrent", &dm)));
        verifyConcurrentCreation(*arbi, *blockingStub, "r71_arbi_concurrent",
            "WtExeFact.WtMinImpactExeUnit", base + 2);
    }

    {
		std::shared_ptr<R71BlockingExecStub> blockingStub = keepBlockingStub();
		blockingStub->comm_ = comm;
		blockingStub->sess_ = sess;
		blockingStub->hot_ = &traderEnv.hot;
		auto diff = ownUntilQuiesced(std::shared_ptr<R71ObservableExecuter<WtDiffExecuter> >(
			new R71ObservableExecuter<WtDiffExecuter>(&factory, "r71_diff_concurrent", &dm,
				&traderEnv.bd)));
		verifyConcurrentCreation(*diff, *blockingStub, "r71_diff_concurrent",
			"WtExeFact.WtDiffMinImpactExeUnit", base + 4);
    }
}

TEST_F(R71ExecTest, FailedInitializationWaitersShareGenerationAndDoNotRetry)
{
	const std::string oldStyle = GTEST_FLAG_GET(death_test_style);
	GTEST_FLAG_SET(death_test_style, "threadsafe");
	EXPECT_EXIT({
		WtExecuterMgr manager;
		std::vector<std::shared_ptr<R71BlockingExecStub> > blockingStubs;
		{
			auto blockingStub = std::make_shared<R71BlockingExecStub>();
			blockingStub->comm_ = comm;
			blockingStub->sess_ = sess;
			blockingStub->hot_ = &traderEnv.hot;
			blockingStubs.push_back(blockingStub);
			auto local = std::make_shared<WtLocalExecuter>(
				&factory, "r71_local_failed_waiters", &dm);
			manager.add_executer(local);
			verifyFailedWaitersDoNotRetry(*local, *blockingStub, "r71_local_failed_waiters",
				"WtExeFact.WtMinImpactExeUnit");
		}

		{
			auto blockingStub = std::make_shared<R71BlockingExecStub>();
			blockingStub->comm_ = comm;
			blockingStub->sess_ = sess;
			blockingStub->hot_ = &traderEnv.hot;
			blockingStubs.push_back(blockingStub);
			auto arbi = std::make_shared<WtArbiExecuter>(
				&factory, "r71_arbi_failed_waiters", &dm);
			manager.add_executer(arbi);
			verifyFailedWaitersDoNotRetry(*arbi, *blockingStub, "r71_arbi_failed_waiters",
				"WtExeFact.WtMinImpactExeUnit");
		}

		{
			auto blockingStub = std::make_shared<R71BlockingExecStub>();
			blockingStub->comm_ = comm;
			blockingStub->sess_ = sess;
			blockingStub->hot_ = &traderEnv.hot;
			blockingStubs.push_back(blockingStub);
			auto diff = std::make_shared<WtDiffExecuter>(
				&factory, "r71_diff_failed_waiters", &dm, &traderEnv.bd);
			manager.add_executer(diff);
			verifyFailedWaitersDoNotRetry(*diff, *blockingStub, "r71_diff_failed_waiters",
				"WtExeFact.WtDiffMinImpactExeUnit");
		}

		WtExecQuiesceCoordinator coordinator({ &manager });
		const bool quiesced = coordinator.quiesce_all();
		const bool complete = coordinator.is_complete();
		if (quiesced || complete ||
			::testing::Test::HasFailure())
			std::exit(93);
		std::cerr << "r71_expected_incomplete_waiters_quiesce coordinator=false complete=false" << std::endl;
		std::exit(0);
	}, ::testing::ExitedWithCode(0), "r71_expected_incomplete_waiters_quiesce");
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

TEST_F(R71ExecTest, ThrowingInitializationChannelCallbackReleasesWaiters)
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
		ExecUnitInitStatus pending = local->queryUnitInitStatus("SHFE.ag.2512");
		const auto waiter_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (pending.waiting_callers < 1 && std::chrono::steady_clock::now() < waiter_deadline)
		{
			std::this_thread::yield();
			pending = local->queryUnitInitStatus("SHFE.ag.2512");
		}
		EXPECT_EQ(pending.waiting_callers, 1u);
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

		local->on_position_changed("SHFE.ag.2512", 1.0);
		const ExecUnitInitStatus retried = local->queryUnitInitStatus("SHFE.ag.2512");
		EXPECT_EQ(retried.state, ExecUnitInitState::READY);
		EXPECT_GT(retried.generation, failed.generation);
		EXPECT_EQ(blockingStub.commodity_queries.load(), 2u);
		EXPECT_EQ(local->ready_queries.load(), 2u);

		WTSTickData* marketTick = tick(93100000, 3500.0);
		ASSERT_NE(marketTick, nullptr);
		local->on_tick("SHFE.ag.2512", marketTick);
		marketTick->release();
		EXPECT_EQ(traderEnv.api.n_insert, 1u);

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
	const std::string oldStyle = GTEST_FLAG_GET(death_test_style);
	GTEST_FLAG_SET(death_test_style, "threadsafe");
	EXPECT_EXIT({
		WtExecuterMgr manager;
		R71BlockingExecStub blockingStub;
		blockingStub.comm_ = comm;
		blockingStub.sess_ = sess;
		blockingStub.hot_ = &traderEnv.hot;
		auto local = std::make_shared<R71ObservableExecuter<WtLocalExecuter>>(
			&factory, "r71_local_account_fanout", &dm);
		manager.add_executer(local);
		local->setStub(&blockingStub);
		WTSVariant* params = cfg("r71_local_account_fanout", "WtExeFact.WtStockMinImpactExeUnit", NULL);
		ASSERT_NE(params, nullptr);
		ASSERT_TRUE(local->init(params));
		params->release();
		local->setTrader(&trader);
		trader.addSink(local.get());
		local->on_channel_lost();
		local->on_channel_ready();
		local->blockFirstReadyCallback(true);

		std::thread creator([local]() {
			local->on_position_changed("SHFE.ag.2512", 1.0);
		});
		const bool callback_blocked = local->waitForFirstReadyCallback(std::chrono::seconds(2));
		EXPECT_TRUE(callback_blocked);
		if (!callback_blocked)
		{
			local->releaseFirstReadyCallback();
			creator.join();
			std::exit(96);
		}

		const ExecUnitInitStatus initializing = local->queryUnitInitStatus("SHFE.ag.2512");
		EXPECT_EQ(initializing.state, ExecUnitInitState::INITIALIZING);
		local->on_account("CNY", 0, 1, 1, 100, 0, 0, 0, 0, 0, 0);
		EXPECT_EQ(local->available_updates.load(), 0u);
		local->releaseFirstReadyCallback();
		creator.join();

		const ExecUnitInitStatus failed = local->queryUnitInitStatus("SHFE.ag.2512");
		EXPECT_EQ(failed.state, ExecUnitInitState::FAILED);
		EXPECT_EQ(failed.generation, initializing.generation);
		EXPECT_EQ(local->available_updates.load(), 0u);

		// A distinct request starts generation 2 and allows the unit to reach READY.
		local->on_position_changed("SHFE.ag.2512", 1.0);
		const ExecUnitInitStatus ready = local->queryUnitInitStatus("SHFE.ag.2512");
		ASSERT_EQ(ready.state, ExecUnitInitState::READY);
		EXPECT_GT(ready.generation, failed.generation);
		local->on_account("CNY", 0, 1, 1, 200, 0, 0, 0, 0, 0, 0);
		EXPECT_EQ(local->available_updates.load(), 1u);

		WtExecQuiesceCoordinator coordinator({ &manager });
		const bool quiesced = coordinator.quiesce_all();
		const bool complete = coordinator.is_complete();
		if (quiesced || complete ||
			::testing::Test::HasFailure())
			std::exit(97);
		std::cerr << "r71_expected_incomplete_account_fanout_quiesce coordinator=false complete=false" << std::endl;
		std::exit(0);
	}, ::testing::ExitedWithCode(0), "r71_expected_incomplete_account_fanout_quiesce");
	GTEST_FLAG_SET(death_test_style, oldStyle);
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
