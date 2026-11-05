/**
 * t6_009_mocker_lifecycle_test.cpp  (BUG-T6-009 / BUG-I02-NEW-020，迭代03 卡9)
 *
 * 合同（卡9：9a 无法证明停止则 quarantine、9b 工厂 remover + 线程退出确认、
 * 9c 拒绝重复 init、9d 回调内同步重入只请求停止、延后到安全点 join）：
 *  - release()/deleteTrader 在外部线程：关闭入口 gate、join 撮合线程与 io worker，
 *    之后恰好一次归还 _awaits 持有的未成交订单引用；已撤单/已移除订单不重复释放；
 *    创建引用由 _orders 持有并在析构归还。
 *  - 在自身回调线程上调用 release()：不自 join，只关闭入口并请求停止；随后外部
 *    release() 完成 join。
 *  - 在自身回调线程上调用 deleteTrader：无法确认线程退出，实例 quarantine 到进程退出，
 *    出参置空，不 delete。
 *  - 多个外部线程并发 release() 与请求调用：串行收尾，关闭后请求返回 -1。
 *
 * 引用计数观测：Sink 对每个推送的 WTSOrderInfo 额外 retain 一次（只首次见到时）。
 * 只用公开接口和本机 UDP 临时端口；不调 login 时撮合线程不启动，订单保持未成交。
 */
#include "r103_mocker_common.h"

#include <set>

extern "C" void deleteTrader(ITraderApi*& trader);
extern "C" ITraderApi* createTrader();

namespace
{
	class RetainingSink : public r103m::Sink
	{
	public:
		~RetainingSink()
		{
			release_held();
		}

		void onPushOrder(WTSOrderInfo* info) override
		{
			r103m::Sink::onPushOrder(info);
			if (info == NULL)
				return;
			std::lock_guard<std::mutex> lk(_held_mtx);
			if (_held.insert(info).second)
			{
				info->retain();
				_by_id[info->getOrderID()] = info;
			}
		}

		WTSOrderInfo* held(const std::string& id)
		{
			std::lock_guard<std::mutex> lk(_held_mtx);
			auto it = _by_id.find(id);
			return it == _by_id.end() ? NULL : it->second;
		}

		std::vector<std::string> held_ids()
		{
			std::lock_guard<std::mutex> lk(_held_mtx);
			std::vector<std::string> ids;
			for (auto& kv : _by_id)
				ids.push_back(kv.first);
			return ids;
		}

		void release_held()
		{
			std::lock_guard<std::mutex> lk(_held_mtx);
			for (WTSOrderInfo* info : _held)
				info->release();
			_held.clear();
			_by_id.clear();
		}

	private:
		std::mutex _held_mtx;
		std::set<WTSOrderInfo*> _held;
		std::map<std::string, WTSOrderInfo*> _by_id;
	};

	/* handleEvent(WTE_Connect) 在 io worker 上执行，用来驱动“回调内”生命周期调用 */
	class CallbackActionSink : public r103m::Sink
	{
	public:
		enum Action { None, Release, Delete };

		void handleEvent(WTSTraderEvent e, int32_t ec) override
		{
			r103m::Sink::handleEvent(e, ec);
			if (e != WTE_Connect)
				return;
			if (action == Release && api != NULL)
			{
				api->release();
				// 关闭后同一线程上的同步重入请求必须被拒绝
				insert_after_release_rc.store(api->queryOrders());
			}
			else if (action == Delete && api != NULL)
			{
				ITraderApi* victim = api;
				deleteTrader(victim);
				delete_nulled.store(victim == NULL);
			}
			callback_done.store(true);
		}

		Action action { None };
		ITraderApi* api { NULL };
		std::atomic<bool> callback_done { false };
		std::atomic<int> insert_after_release_rc { 99 };
		std::atomic<bool> delete_nulled { false };
	};

	class T6009Mocker : public ::testing::Test
	{
	protected:
		/* 合约由 sink 的 BdMgr 接管所有权并在其析构时释放 */
		void register_ag(r103m::Sink& sink)
		{
			ag_comm_ = WTSCommodityInfo::create("ag", "沪银", "SHFE", "TRADING", "CHINA");
			ag_comm_->setCategory(CC_Future);
			ag_comm_->setCoverMode(CM_OpenCover);
			ag_comm_->setPriceMode(PM_Both);
			ag_comm_->setTradingMode(TM_Both);
			ag_comm_->setPriceTick(1.0);
			ag_comm_->setVolScale(15);
			ag_ct_ = WTSContractInfo::create("ag2512", "沪银2512", "SHFE", "ag");
			ag_ct_->setTotalIndex(1);
			sink.bd_.regis(ag_comm_, ag_ct_);
		}

		static bool init_and_connect(ITraderApi* api, r103m::Sink& sink, uint32_t id)
		{
			WTSVariant* cfg = r103m::make_cfg(id);
			const bool ok = api->init(cfg);
			cfg->release();
			if (!ok)
				return false;
			api->connect();
			return r103m::wait_until([&sink]() { return sink.n_event() >= 1u; });
		}

		r103m::TempCwd cwd_;
		WTSCommodityInfo* ag_comm_ { NULL };
		WTSContractInfo* ag_ct_ { NULL };
	};
}

/*
 * 原验收条件：受控订单插入形成未撮合订单，然后正常 release/delete；缓存和 retain 的
 * 订单引用均归还，已撮合/已移除订单不重复释放。
 * A 未成交：_orders(1) + _awaits(1) + sink(1) = 3；release 后 2（_awaits 归还恰好一次）；
 *   再次 release 不变；deleteTrader 后 1（_orders 归还）。
 * B 已撤单（已从 _awaits 移除）：_orders(1) + sink(1) = 2；release 后仍 2（不重复释放）；
 *   deleteTrader 后 1。重复释放会降到 0，sink 最终 release 时触发 ASan UAF。
 */
TEST_F(T6009Mocker, PendingAndCanceledOrderRefsReturnedExactlyOnce)
{
	RetainingSink sink;
	register_ag(sink);
	ITraderApi* api = createTrader();
	ASSERT_NE(api, nullptr);
	api->registerSpi(&sink);
	ASSERT_TRUE(init_and_connect(api, sink, 96091));

	WTSEntrust* a = r103m::make_entrust("ag2512", "SHFE", ag_ct_, 7000.0, 2.0, WDT_LONG, WOT_OPEN);
	a->setEntrustID("t6009-a");
	ASSERT_EQ(0, api->orderInsert(a));
	a->release();
	ASSERT_TRUE(r103m::wait_until([&sink]() { return sink.n_push_order() >= 1u; }));
	const std::string idA = sink.last_order_id();

	WTSEntrust* b = r103m::make_entrust("ag2512", "SHFE", ag_ct_, 7001.0, 1.0, WDT_LONG, WOT_OPEN);
	b->setEntrustID("t6009-b");
	ASSERT_EQ(0, api->orderInsert(b));
	b->release();
	ASSERT_TRUE(r103m::wait_until([&sink]() { return sink.n_push_order() >= 2u; }));
	const std::string idB = sink.last_order_id();
	ASSERT_NE(idA, idB);

	WTSEntrustAction* cancel = WTSEntrustAction::create();
	cancel->setOrderID(idB.c_str());
	cancel->setEntrustID("t6009-b");
	ASSERT_EQ(0, api->orderAction(cancel));
	cancel->release();
	ASSERT_TRUE(r103m::wait_until([&sink]() { return sink.n_push_order() >= 3u; }));
	EXPECT_EQ(sink.state, (int)WOS_Canceled);

	WTSOrderInfo* ordA = sink.held(idA);
	WTSOrderInfo* ordB = sink.held(idB);
	ASSERT_NE(ordA, nullptr);
	ASSERT_NE(ordB, nullptr);
	EXPECT_EQ(3u, ordA->retainCount());
	EXPECT_EQ(2u, ordB->retainCount());

	api->release();
	EXPECT_EQ(2u, ordA->retainCount()) << "await cache must return its pending-order reference";
	EXPECT_EQ(2u, ordB->retainCount()) << "removed order must not be released again";
	EXPECT_EQ(-1, api->orderInsert(NULL)) << "admission must be closed after release";

	api->release();
	EXPECT_EQ(2u, ordA->retainCount());
	EXPECT_EQ(2u, ordB->retainCount());

	deleteTrader(api);
	EXPECT_EQ(api, nullptr);
	EXPECT_EQ(1u, ordA->retainCount());
	EXPECT_EQ(1u, ordB->retainCount());
	sink.release_held();
}

/* 直接析构（外部线程）也恰好一次归还；不经 release() 先行 */
TEST_F(T6009Mocker, DestructorAloneReturnsPendingOrderRefs)
{
	RetainingSink sink;
	register_ag(sink);
	WTSOrderInfo* ord = NULL;
	{
		TraderMocker mocker;
		mocker.registerSpi(&sink);
		ASSERT_TRUE(init_and_connect(&mocker, sink, 96092));
		WTSEntrust* a = r103m::make_entrust("ag2512", "SHFE", ag_ct_, 7000.0, 3.0, WDT_LONG, WOT_OPEN);
		ASSERT_EQ(0, mocker.orderInsert(a));
		a->release();
		ASSERT_TRUE(r103m::wait_until([&sink]() { return sink.n_push_order() >= 1u; }));
		// onPushOrder is delivered before the insert handler appends the order to
		// _orders/_awaits. The account query runs after that handler on the same io
		// worker, so its reply is the barrier before observing the reference count
		// (s5 2026-11-04: one loaded run read 2 without this barrier).
		const uint32_t accounts = sink.n_account();
		ASSERT_EQ(0, mocker.queryAccount());
		ASSERT_TRUE(r103m::wait_until([&sink, accounts]() { return sink.n_account() > accounts; }));
		ord = sink.held(sink.last_order_id());
		ASSERT_NE(ord, nullptr);
		EXPECT_EQ(3u, ord->retainCount());
	}
	EXPECT_EQ(1u, ord->retainCount());
}

/* 9d：回调线程（io worker）内 release() 不自 join；外部 release() 完成收尾 */
TEST_F(T6009Mocker, ReleaseFromWorkerCallbackDefersJoin)
{
	CallbackActionSink sink;
	register_ag(sink);
	ITraderApi* api = createTrader();
	sink.action = CallbackActionSink::Release;
	sink.api = api;
	api->registerSpi(&sink);
	ASSERT_TRUE(init_and_connect(api, sink, 96093));
	ASSERT_TRUE(r103m::wait_until([&sink]() { return sink.callback_done.load(); }));
	EXPECT_EQ(-1, sink.insert_after_release_rc.load());
	EXPECT_EQ(-1, api->queryAccount());

	api->release();			// 外部线程：join io worker
	deleteTrader(api);
	EXPECT_EQ(api, nullptr);
}

/* 9a/9b：回调线程内 deleteTrader 无法确认线程退出 -> quarantine，出参置空，不 delete */
TEST_F(T6009Mocker, DeleteTraderFromWorkerCallbackQuarantines)
{
	// sink 必须比被 quarantine 的实例活得久（实例常驻到进程退出，不再回调）
	static CallbackActionSink* sink = new CallbackActionSink();
	register_ag(*sink);

	ITraderApi* api = createTrader();
	sink->action = CallbackActionSink::Delete;
	sink->api = api;
	api->registerSpi(sink);
	ASSERT_TRUE(init_and_connect(api, *sink, 96094));
	ASSERT_TRUE(r103m::wait_until([]() { return sink->callback_done.load(); }));
	EXPECT_TRUE(sink->delete_nulled.load());
	// quarantine 后入口已关闭
	EXPECT_EQ(-1, api->queryOrders());
}

/* 9c：成功 init 的实例拒绝再次 init，原配置保留 */
TEST_F(T6009Mocker, RepeatedInitRejected)
{
	TraderMocker mocker;
	WTSVariant* cfg = r103m::make_cfg(96095);
	EXPECT_TRUE(mocker.init(cfg));
	EXPECT_FALSE(mocker.init(cfg));
	cfg->release();
	mocker.release();
}

/* 9b：多个外部线程并发 release()/disconnect() 与请求调用；串行收尾、无自 join、无数据竞争 */
TEST_F(T6009Mocker, ConcurrentExternalReleaseAndRequests)
{
	RetainingSink sink;
	register_ag(sink);
	ITraderApi* api = createTrader();
	api->registerSpi(&sink);
	ASSERT_TRUE(init_and_connect(api, sink, 96096));
	ASSERT_EQ(0, api->login("u", "p", ""));
	EXPECT_TRUE(api->isConnected());

	std::atomic<bool> go { false };
	std::atomic<uint32_t> accepted { 0 };
	std::atomic<uint32_t> rejected { 0 };
	std::vector<std::thread> threads;
	for (int t = 0; t < 4; t++)
	{
		threads.emplace_back([&, t]() {
			while (!go.load()) std::this_thread::yield();
			for (int i = 0; i < 50; i++)
			{
				WTSEntrust* e = r103m::make_entrust("ag2512", "SHFE", ag_ct_, 7000.0 + t, 1.0, WDT_LONG, WOT_OPEN);
				const int rc = api->orderInsert(e);
				e->release();
				(rc == 0 ? accepted : rejected).fetch_add(1);
				(void)api->isConnected();
				(void)api->queryOrders();
			}
		});
	}
	threads.emplace_back([&]() { while (!go.load()) std::this_thread::yield(); api->disconnect(); });
	threads.emplace_back([&]() { while (!go.load()) std::this_thread::yield(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); api->release(); });
	threads.emplace_back([&]() { while (!go.load()) std::this_thread::yield(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); api->release(); });
	go.store(true);
	for (auto& th : threads)
		th.join();

	EXPECT_EQ(200u, accepted.load() + rejected.load());
	EXPECT_FALSE(api->isConnected());
	EXPECT_EQ(-1, api->orderInsert(NULL));

	// 所有订单引用：release 后 _awaits 已归还；删除后只剩 sink 的一份
	std::vector<WTSOrderInfo*> held;
	for (const std::string& id : sink.held_ids())
		held.push_back(sink.held(id));
	deleteTrader(api);
	for (WTSOrderInfo* o : held)
		EXPECT_EQ(1u, o->retainCount()) << o->getOrderID();
	sink.release_held();
}
