/*
 * t4_003_femas_lifecycle_test.cpp — BUG-T4-003 / BUG-T4-002（迭代03，卡9）
 *
 * TraderFemas 统一停止协议（9a=A+C、9b=A、9c=A、9d=A）：
 *  - 禁止新工作：release()/disconnect() 请求后所有入口返回 -1、不再调度查询、
 *    不再向宿主投递回调；已 release 或已 connect 的实例拒绝再次 connect（9c）。
 *  - 保活：worker 持有常驻 work guard，空闲后仍执行后续查询；出站 API 调用持
 *    lease，release() 等其结束后才 RegisterSpi(NULL)/Release()；已进入包装层的
 *    SPI 回调计数，release() 有界等待其返回。
 *  - 可观察关闭：lifecycleState()/isQuiesced()；排空超时记 LS_UNQUIESCED，保留
 *    API 不 Release，工厂 remover quarantine 到进程退出。
 *  - 非 worker join：worker/回调线程内的 release() 只请求停止（9d），由外部
 *    release()/析构/remover 完成；回调线程内 deleteTrader 隔离而非 delete。
 *
 * 每例在 fork 子进程中运行（旧实现会 std::terminate），子进程只用进程内 fake API，
 * 不加载厂商库、不连接柜台。WT_T4003_BASELINE 用于以改前产品源码构建红控：此时
 * 不调用改前不存在的 lifecycleState()/isQuiesced()/排空时限接口，只做行为断言。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

#include "TraderFemas/TraderFemas.h"
#include "r93_femas_api_stub.h"

#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSTradeDef.hpp"

extern "C" void deleteTrader(ITraderApi*& trader);

namespace
{
	namespace fs = std::filesystem;

	/* 子进程退出码：0 通过；其余见各例。86 = std::terminate；134 = abort。 */
	enum ChildCode
	{
		OK = 0,
		NOT_STARTED = 3,
		TIMEOUT = 4,
		RELEASE_EARLY = 8,
		RELEASE_COUNT = 9,
		QUERY_NOT_RUN = 7,
		ADMISSION = 10,
		DELIVERED_LATE = 11,
		STATE = 12,
		NOT_QUARANTINED = 13,
		ORDERING = 14,
	};

	class Barrier
	{
	public:
		void arrive()
		{
			std::lock_guard<std::mutex> l(m);
			entered = true;
			cv.notify_all();
		}
		void block()
		{
			std::unique_lock<std::mutex> l(m);
			cv.wait(l, [this] { return proceed; });
		}
		bool wait_entered(int ms)
		{
			std::unique_lock<std::mutex> l(m);
			return cv.wait_for(l, std::chrono::milliseconds(ms), [this] { return entered; });
		}
		void open()
		{
			std::lock_guard<std::mutex> l(m);
			proceed = true;
			cv.notify_all();
		}

	private:
		std::mutex m;
		std::condition_variable cv;
		bool entered = false;
		bool proceed = false;
	};

	class FakeApi final : public R93FemasApiStub
	{
	public:
		std::atomic<int> creates{ 0 };
		std::atomic<int> inits{ 0 };
		std::atomic<int> releases{ 0 };
		std::atomic<int> seq{ 0 };
		std::atomic<int> release_seq{ 0 };
		std::atomic<int> request_done_seq{ 0 };
		std::atomic<int> qry_account{ 0 };
		std::atomic<int> order_inserts{ 0 };
		std::atomic<bool> block_query{ false };
		std::atomic<bool> block_insert{ false };
		std::atomic<bool> callback_in_query{ false };
		Barrier query_barrier;
		Barrier insert_barrier;
		std::function<void()> on_init;

		void Init() override
		{
			inits.fetch_add(1);
			if (on_init)
				on_init();
		}
		void Release() override
		{
			release_seq.store(seq.fetch_add(1) + 1);
			releases.fetch_add(1);
		}
		int ReqQryInvestorAccount(CUstpFtdcQryInvestorAccountField*, int requestId) override
		{
			qry_account.fetch_add(1);
			if (block_query.load())
			{
				query_barrier.arrive();
				query_barrier.block();
				request_done_seq.store(seq.fetch_add(1) + 1);
			}
			if (callback_in_query.load() && spi_ != NULL)
			{
				CUstpFtdcRspInvestorAccountField account = {};
				CUstpFtdcRspInfoField ok = {};
				spi_->OnRspQryInvestorAccount(&account, &ok, requestId, true);
			}
			return 0;
		}
		int ReqOrderInsert(CUstpFtdcInputOrderField* order, int requestId) override
		{
			R93FemasApiStub::ReqOrderInsert(order, requestId);
			order_inserts.fetch_add(1);
			if (block_insert.load())
			{
				insert_barrier.arrive();
				insert_barrier.block();
				request_done_seq.store(seq.fetch_add(1) + 1);
			}
			return 0;
		}
	};

	FakeApi* g_fake = NULL;
	CUstpFtdcTraderApi* make_fake(const char*)
	{
		if (g_fake != NULL)
			g_fake->creates.fetch_add(1);
		return g_fake;
	}

	class NullBdMgr : public IBaseDataMgr
	{
	public:
		WTSCommodityInfo* getCommodity(const char*) override { return NULL; }
		WTSCommodityInfo* getCommodity(const char*, const char*) override { return NULL; }
		WTSContractInfo* getContract(const char*, const char* = "", uint32_t = 0) override { return NULL; }
		WTSArray* getContracts(const char* = "", uint32_t = 0) override { return NULL; }
		WTSSessionInfo* getSession(const char*) override { return NULL; }
		WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return NULL; }
		WTSArray* getAllSessions() override { return NULL; }
		bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
		uint32_t calcTradingDate(const char*, uint32_t uDate, uint32_t, bool = false) override { return uDate; }
		uint64_t getBoundaryTime(const char*, uint32_t tDate, bool = false, bool = true) override { return tDate; }
	};

	class Sink : public ITraderSpi
	{
	public:
		IBaseDataMgr* getBaseDataMgr() override { return &bd; }
		void handleEvent(WTSTraderEvent e, int32_t ec) override
		{
			events.fetch_add(1);
			if (on_event)
				on_event(e, ec);
		}
		void handleTraderLog(WTSLogLevel, const char*) override { logs.fetch_add(1); }
		void onLoginResult(bool, const char*, uint32_t) override { logins.fetch_add(1); }
		void onRspAccount(WTSArray*) override
		{
			accounts.fetch_add(1);
			if (on_account)
				on_account();
		}
		void onPushOrder(WTSOrderInfo*) override { orders.fetch_add(1); }
		void onPushTrade(WTSTradeInfo*) override { trades.fetch_add(1); }
		void onPushInstrumentStatus(const char*, const char*, WTSTradeStatus) override { statuses.fetch_add(1); }

		uint32_t business() const
		{
			return events.load() + logins.load() + accounts.load() + orders.load() + trades.load() + statuses.load();
		}

		NullBdMgr bd;
		std::atomic<uint32_t> events{ 0 }, logs{ 0 }, logins{ 0 }, accounts{ 0 }, orders{ 0 }, trades{ 0 }, statuses{ 0 };
		std::function<void(WTSTraderEvent, int32_t)> on_event;
		std::function<void()> on_account;
	};

	class Probe final : public TraderFemas
	{
	public:
		void setup(const std::string& dir)
		{
			m_funcCreator = &make_fake;
			m_strFlowDir = dir;
			m_strBroker = "broker";
			m_strUser = "user";
		}
		void ready() { m_wrapperState = WS_ALLREADY; }
		bool workerJoinable() const { return m_thrdWorker && m_thrdWorker->joinable(); }
		bool workerStarted() const { return m_thrdWorker != NULL; }
#ifndef WT_T4003_BASELINE
		void setDrainTimeout(uint32_t ms) { m_drainTimeoutMs = ms; }
		int state() const { return (int)lifecycleState(); }
		bool quiesced() const { return isQuiesced(); }
#else
		void setDrainTimeout(uint32_t) {}
		int state() const { return -1; }
		bool quiesced() const { return true; }
#endif
	};

	std::string flow_dir()
	{
		const fs::path dir = fs::temp_directory_path() / ("wt_t4003_" + std::to_string((long)::getpid()));
		std::error_code ec;
		fs::create_directories(dir, ec);
		return dir.string() + "/";
	}

	template <typename Pred>
	bool wait_for(Pred pred, int ms)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (pred())
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		return pred();
	}

	/* fork 子进程运行 body；terminate→86，alarm 10s；返回子进程状态 */
	int run_forked(const std::function<int()>& body)
	{
		std::fflush(stdout);
		std::fflush(stderr);
		const pid_t child = ::fork();
		if (child == 0)
		{
			std::set_terminate([] {
				std::fprintf(stderr, "child: std::terminate\n");
				std::fflush(stderr);
				std::_Exit(86);
			});
			::alarm(10);
			const int code = body();
			std::fflush(stdout);
			std::fflush(stderr);
			std::_Exit(code);
		}
		int status = 0;
		if (child < 0 || ::waitpid(child, &status, 0) != child)
			return -1;
		return status;
	}

	void expect_child_ok(int status)
	{
		ASSERT_TRUE(WIFEXITED(status)) << "child killed by signal " << (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
		EXPECT_EQ(0, WEXITSTATUS(status)) << "child exit code (86=terminate, see ChildCode)";
	}

	WTSContractInfo* make_contract(WTSCommodityInfo*& comm)
	{
		comm = WTSCommodityInfo::create("rb", "rb", "SHFE", "TRADING", "CHINA");
		comm->setCategory(CC_Future);
		WTSContractInfo* ct = WTSContractInfo::create("rb2401", "rb2401", "SHFE", "rb");
		ct->setCommInfo(comm);
		return ct;
	}
}

/* 红控：release 后 worker 仍 joinable，delete 触发 std::terminate（exit 86）。 */
TEST(T4003FemasLifecycle, release_joins_worker_and_delete_does_not_terminate)
{
	expect_child_ok(run_forked([]() {
		FakeApi fake;
		g_fake = &fake;
		Sink sink;
		Probe* obj = new Probe();
		obj->registerSpi(&sink);
		obj->setup(flow_dir());
		obj->connect();
		if (!obj->workerJoinable())
			return (int)NOT_STARTED;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		obj->release();
		if (fake.releases.load() != 1)
			return (int)RELEASE_COUNT;
		delete obj;	// old: joinable std::thread destroyed -> terminate
		return (int)OK;
	}));
}

/* 红控：worker 空闲后 io_context 无 work 而停止，后续查询永不执行。 */
TEST(T4003FemasLifecycle, idle_worker_still_executes_later_query)
{
	expect_child_ok(run_forked([]() {
		FakeApi fake;
		g_fake = &fake;
		Sink sink;
		Probe* obj = new Probe();
		obj->registerSpi(&sink);
		obj->setup(flow_dir());
		obj->connect();
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		obj->ready();
		if (obj->queryAccount() != 0)
			return (int)ADMISSION;
		if (!wait_for([&]() { return fake.qry_account.load() == 1; }, 2000))
			return (int)QUERY_NOT_RUN;
		obj->release();
		delete obj;
		return (int)OK;
	}));
}

/* 红控：查询在 worker 上阻塞于 SDK 调用时，release 提前返回并 Release()。 */
TEST(T4003FemasLifecycle, release_waits_for_inflight_request_before_api_release)
{
	expect_child_ok(run_forked([]() {
		FakeApi fake;
		g_fake = &fake;
		fake.block_query.store(true);
		Sink sink;
		Probe* obj = new Probe();
		obj->registerSpi(&sink);
		obj->setup(flow_dir());
		fake.on_init = [&]() { obj->ready(); (void)obj->queryAccount(); };
		obj->connect();
		if (!fake.query_barrier.wait_entered(3000))
			return (int)QUERY_NOT_RUN;
		std::atomic<bool> returned{ false };
		std::thread releaser([&]() { obj->release(); returned.store(true); });
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		const bool early = returned.load() || fake.releases.load() != 0;
		fake.query_barrier.open();
		releaser.join();
		if (early)
			return (int)RELEASE_EARLY;
		if (fake.releases.load() != 1)
			return (int)RELEASE_COUNT;
		if (!(fake.request_done_seq.load() != 0 && fake.request_done_seq.load() < fake.release_seq.load()))
			return (int)ORDERING;
		delete obj;
		return (int)OK;
	}));
}

/* 红控：宿主线程上的下单阻塞于 SDK 调用时，release 不得并发 Release()。 */
TEST(T4003FemasLifecycle, release_waits_for_inflight_order_insert_before_api_release)
{
	expect_child_ok(run_forked([]() {
		FakeApi fake;
		g_fake = &fake;
		fake.block_insert.store(true);
		Sink sink;
		Probe* obj = new Probe();
		obj->registerSpi(&sink);
		obj->setup(flow_dir());
		obj->connect();
		obj->ready();
		WTSCommodityInfo* comm = NULL;
		WTSContractInfo* ct = make_contract(comm);
		WTSEntrust* entrust = WTSEntrust::create("rb2401", 1, 3500, "SHFE");
		entrust->setContractInfo(ct);
		entrust->setDirection(WDT_LONG);
		entrust->setOffsetType(WOT_OPEN);
		entrust->setPriceType(WPT_LIMITPRICE);
		entrust->setOrderFlag(WOF_NOR);
		std::atomic<int> insertRc{ 99 };
		std::thread sender([&]() { insertRc.store(obj->orderInsert(entrust)); });
		if (!fake.insert_barrier.wait_entered(3000))
		{
			fake.insert_barrier.open();
			sender.join();
			return (int)NOT_STARTED;
		}
		std::atomic<bool> returned{ false };
		std::thread releaser([&]() { obj->release(); returned.store(true); });
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		const bool early = returned.load() || fake.releases.load() != 0;
		fake.insert_barrier.open();
		sender.join();
		releaser.join();
		if (early)
			return (int)RELEASE_EARLY;
		if (insertRc.load() != 0 || fake.releases.load() != 1)
			return (int)RELEASE_COUNT;
		if (!(fake.request_done_seq.load() < fake.release_seq.load()))
			return (int)ORDERING;
		delete obj;
		entrust->release();
		ct->release();
		comm->release();
		return (int)OK;
	}));
}

/* 9d 红控：回调内 release 不得在回调栈上 Release() API；外部 release 完成收尾。 */
TEST(T4003FemasLifecycle, release_from_callback_defers_and_external_release_completes)
{
	expect_child_ok(run_forked([]() {
		FakeApi fake;
		g_fake = &fake;
		Sink sink;
		Probe* obj = new Probe();
		obj->registerSpi(&sink);
		obj->setup(flow_dir());
		std::atomic<int> releasesInCallback{ -1 };
		std::atomic<int> queryInCallback{ 99 };
		sink.on_event = [&](WTSTraderEvent e, int32_t) {
			if (e != WTE_Connect)
				return;
			obj->release();
			releasesInCallback.store(fake.releases.load());
			obj->ready();
			queryInCallback.store(obj->queryAccount());
		};
		fake.on_init = [&]() { fake.spi_->OnFrontConnected(); };
		obj->connect();
		if (releasesInCallback.load() != 0)
			return (int)RELEASE_EARLY;
		if (queryInCallback.load() >= 0)
			return (int)ADMISSION;
#ifndef WT_T4003_BASELINE
		if (obj->state() != TraderFemas::LS_STOP_REQUESTED)
			return (int)STATE;
#endif
		obj->release();
		if (fake.releases.load() != 1)
			return (int)RELEASE_COUNT;
#ifndef WT_T4003_BASELINE
		if (obj->state() != TraderFemas::LS_RELEASED || !obj->quiesced())
			return (int)STATE;
#endif
		delete obj;
		return (int)OK;
	}));
}

/* 9a/9b 红控：worker 线程上的回调内 deleteTrader（旧实现在 worker 上 delete，
 * joinable worker 析构 → terminate）；新实现隔离实例、出参置空、不 delete。 */
TEST(T4003FemasLifecycle, delete_trader_from_callback_quarantines)
{
	expect_child_ok(run_forked([]() {
		FakeApi fake;
		g_fake = &fake;
		fake.callback_in_query.store(true);
		Sink sink;
		Probe* obj = new Probe();
		ITraderApi* handle = obj;
		obj->registerSpi(&sink);
		obj->setup(flow_dir());
		std::atomic<bool> nulled{ false };
		std::atomic<bool> done{ false };
		sink.on_account = [&]() {
			ITraderApi* victim = handle;
			deleteTrader(victim);
			nulled.store(victim == NULL);
			done.store(true);
		};
		// Queue the query on the Init() stack so it is posted before the worker's
		// first run (independent of the idle-worker defect covered separately).
		std::atomic<int> queued{ 99 };
		fake.on_init = [&]() { obj->ready(); queued.store(obj->queryAccount()); };
		obj->connect();
		if (queued.load() != 0)
			return (int)ADMISSION;
		if (!wait_for([&]() { return done.load(); }, 3000))
			return (int)TIMEOUT;
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		if (!nulled.load())
			return (int)NOT_QUARANTINED;
		// The quarantined instance stays alive and closed; nothing more is delivered.
		const uint32_t delivered = sink.business();
		obj->OnFrontDisconnected(1);
		if (sink.business() != delivered)
			return (int)DELIVERED_LATE;
		return (int)OK;
	}));
}

/* 9c 红控：重复 connect 不得再建 API；release 后所有入口拒绝、connect 不复活实例。 */
TEST(T4003FemasLifecycle, admission_closed_after_release_and_repeated_connect_rejected)
{
	expect_child_ok(run_forked([]() {
		FakeApi fake;
		g_fake = &fake;
		Sink sink;
		Probe* obj = new Probe();
		obj->registerSpi(&sink);
		obj->setup(flow_dir());
		obj->connect();
		obj->connect();
		if (fake.creates.load() != 1 || fake.inits.load() != 1)
			return (int)ADMISSION;
		obj->ready();
		obj->release();
		WTSCommodityInfo* comm = NULL;
		WTSContractInfo* ct = make_contract(comm);
		WTSEntrust* entrust = WTSEntrust::create("rb2401", 1, 3500, "SHFE");
		entrust->setContractInfo(ct);
		WTSEntrustAction* action = WTSEntrustAction::create("rb2401", "SHFE");
		action->setEntrustID("1");
		action->setOrderID("1");
		obj->ready();
		const bool rejected = obj->orderInsert(entrust) < 0 && obj->orderAction(action) < 0 &&
			obj->queryAccount() < 0 && obj->queryPositions() < 0 && obj->queryOrders() < 0 &&
			obj->queryTrades() < 0 && obj->login("u", "p", "x") < 0 && obj->logout() < 0;
		const int insertsBefore = fake.order_inserts.load();
		obj->connect();
		entrust->release();
		action->release();
		ct->release();
		comm->release();
		if (!rejected || insertsBefore != 0)
			return (int)ADMISSION;
		if (fake.creates.load() != 1 || fake.inits.load() != 1)
			return (int)ADMISSION;
		delete obj;
		return (int)OK;
	}));
}

/* 隔离红控：对象存活期内迟到的 SPI 回调不得在 release 后投递到宿主。 */
TEST(T4003FemasLifecycle, late_callback_after_release_is_not_delivered)
{
	expect_child_ok(run_forked([]() {
		FakeApi fake;
		g_fake = &fake;
		Sink sink;
		Probe* obj = new Probe();
		obj->registerSpi(&sink);
		obj->setup(flow_dir());
		obj->connect();
		obj->release();
		const uint32_t delivered = sink.business();
		CUstpFtdcInstrumentStatusField status = {};
		strncpy(status.ExchangeID, "SHFE", sizeof(status.ExchangeID) - 1);
		strncpy(status.InstrumentID, "rb2401", sizeof(status.InstrumentID) - 1);
		obj->OnFrontConnected();
		obj->OnFrontDisconnected(1);
		obj->OnRtnInstrumentStatus(&status);
		if (sink.business() != delivered)
			return (int)DELIVERED_LATE;
		delete obj;
		return (int)OK;
	}));
}

/* 9a：已进入包装层的 SPI 回调未在时限内返回时，release 不 Release() API，状态为
 * LS_UNQUIESCED，remover 隔离实例；正控：回调在时限内返回则 release 等待后释放。 */
TEST(T4003FemasLifecycle, callback_drain_timeout_reports_unquiesced_and_remover_quarantines)
{
	expect_child_ok(run_forked([]() {
		FakeApi fake;
		g_fake = &fake;
		Sink sink;
		Probe* obj = new Probe();
		obj->registerSpi(&sink);
		obj->setup(flow_dir());
		Barrier cb;
		sink.on_event = [&](WTSTraderEvent e, int32_t) {
			if (e == WTE_Close)
			{
				cb.arrive();
				cb.block();
			}
		};
		obj->connect();
		std::thread vendor([&]() { obj->OnFrontDisconnected(7); });
		if (!cb.wait_entered(3000))
		{
			cb.open();
			vendor.join();
			return (int)NOT_STARTED;
		}
		obj->setDrainTimeout(200);
		obj->release();
		const int releasesWhileActive = fake.releases.load();
#ifndef WT_T4003_BASELINE
		const bool unquiesced = obj->state() == TraderFemas::LS_UNQUIESCED && !obj->quiesced();
		ITraderApi* handle = obj;
		deleteTrader(handle);
		const bool quarantined = handle == NULL;
#else
		const bool unquiesced = true;
		const bool quarantined = true;
#endif
		cb.open();
		vendor.join();
		if (releasesWhileActive != 0)
			return (int)RELEASE_EARLY;
		if (!unquiesced)
			return (int)STATE;
		if (!quarantined)
			return (int)NOT_QUARANTINED;

		// Positive control on a second instance: the callback returns within the
		// bound, release() waits for it and then releases the API exactly once.
		FakeApi fake2;
		g_fake = &fake2;
		Sink sink2;
		Probe* obj2 = new Probe();
		obj2->registerSpi(&sink2);
		obj2->setup(flow_dir());
		Barrier cb2;
		sink2.on_event = [&](WTSTraderEvent e, int32_t) {
			if (e == WTE_Close)
			{
				cb2.arrive();
				cb2.block();
			}
		};
		obj2->connect();
		std::thread vendor2([&]() { obj2->OnFrontDisconnected(8); });
		if (!cb2.wait_entered(3000))
		{
			cb2.open();
			vendor2.join();
			return (int)NOT_STARTED;
		}
		std::atomic<bool> returned{ false };
		std::thread releaser([&]() { obj2->release(); returned.store(true); });
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
		const bool early = returned.load() || fake2.releases.load() != 0;
		cb2.open();
		vendor2.join();
		releaser.join();
		if (early)
			return (int)RELEASE_EARLY;
		if (fake2.releases.load() != 1 || !obj2->quiesced())
			return (int)RELEASE_COUNT;
		delete obj2;
		return (int)OK;
	}));
}
