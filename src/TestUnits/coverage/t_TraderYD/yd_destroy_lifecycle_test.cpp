/*
 * yd_destroy_lifecycle_test.cpp — TraderYD 停止/销毁协议（迭代03 s5，卡9）
 *
 * 依据本地 src/API/yd1.108.360/ydApi.h：startDestroy() 异步，第一阶段结束发
 * notifyBeforeApiDestroy()，第二阶段释放全部资源后发 notifyAfterApiDestroy()，
 * “listener 应在此之后由用户删除”。卡9（9a=A+C、9b=A、9c=A、9d=A）：
 *  - 外部 release() 关闭准入，有界等待宿主调用、查询 worker（非 worker 线程 join）
 *    与已进入的回调，然后 startDestroy() 并有界等待 notifyAfterApiDestroy()；
 *  - 超时记 YS_DRAIN_UNCERTAIN，deleteTrader 隔离实例到进程退出（不删除 listener）；
 *  - 回调/worker 线程内 release() 只请求停止，deleteTrader 隔离；
 *  - 已 release/已 connect 的实例拒绝 connect（9c）；停止后回调不再投递。
 *
 * 每例 fork 子进程（旧实现会 std::terminate），只用进程内 YD fake，不加载厂商库。
 * WT_S5_RED_BASELINE：以改前产品源码构建红控时不调用改前不存在的接口。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <string>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

#include "TraderYD/TraderYD.h"
#include "r93_yd_api_stub.h"

#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSTradeDef.hpp"

extern "C" void deleteTrader(ITraderApi*& trader);

namespace
{
	enum ChildCode
	{
		OK = 0,
		NOT_STARTED = 3,
		TIMEOUT = 4,
		RELEASE_EARLY = 8,
		RELEASE_COUNT = 9,
		ADMISSION = 10,
		DELIVERED_LATE = 11,
		STATE = 12,
		NOT_QUARANTINED = 13,
	};

	std::atomic<int> g_seq{ 0 };
	std::atomic<int> g_after_seq{ 0 };
	std::atomic<bool> g_destroyed{ false };
	std::atomic<int> g_creates{ 0 };
	R93YdApiStub* g_stub = NULL;

	YDApi* make_stub(const char*)
	{
		g_creates.fetch_add(1);
		return g_stub;
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
		void handleTraderLog(WTSLogLevel, const char*) override {}
		void onLoginResult(bool, const char*, uint32_t) override { logins.fetch_add(1); }
		uint32_t business() const { return events.load() + logins.load(); }

		NullBdMgr bd;
		std::atomic<uint32_t> events{ 0 }, logins{ 0 };
		std::function<void(WTSTraderEvent, int32_t)> on_event;
	};

	class Probe final : public TraderYD
	{
	public:
		~Probe() { g_destroyed.store(true); }
		void setFactory(YDApi* (*factory)(const char*)) { m_funcCreator = factory; }
		void presetWorker() { m_thrdWorker.reset(new StdThread()); }
		void ready() { m_wrapperState = WS_ALLREADY; }
		void notifyAfterApiDestroy() override
		{
			g_after_seq.store(g_seq.fetch_add(1) + 1);
			TraderYD::notifyAfterApiDestroy();
		}
#ifndef WT_S5_RED_BASELINE
		void setDrainTimeout(uint32_t ms) { m_drainTimeoutMs = ms; }
		int state() const { return (int)stopState(); }
#else
		void setDrainTimeout(uint32_t) {}
		int state() const { return -1; }
#endif
	};

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

	// Records startDestroy() in an atomic so concurrent callbacks can observe it.
	class CountingStub final : public R93YdApiStub
	{
	public:
		std::atomic<int> destroys{ 0 };
		void startDestroy() override
		{
			destroys.fetch_add(1);
			R93YdApiStub::startDestroy();
		}
	};

	Probe* make_probe(Sink& sink, R93YdApiStub& stub)
	{
		g_stub = &stub;
		Probe* obj = new Probe();
		obj->registerSpi(&sink);
		obj->setFactory(&make_stub);
		return obj;
	}
}

/* ydApi.h 红控：release 必须等到 notifyAfterApiDestroy 才返回（旧实现立即返回）。 */
TEST(YdDestroyLifecycle, release_waits_for_after_api_destroy_notification)
{
	expect_child_ok(run_forked([]() {
		R93YdApiStub stub;
		stub.destroy_mode_ = 2;
		stub.destroy_delay_ms_ = 150;
		Sink sink;
		Probe* obj = make_probe(sink, stub);
		obj->connect();
		obj->release();
		const int returnedSeq = g_seq.fetch_add(1) + 1;
		if (stub.n_destroy_ != 1)
			return (int)RELEASE_COUNT;
		if (g_after_seq.load() == 0 || g_after_seq.load() > returnedSeq)
			return (int)RELEASE_EARLY;
#ifndef WT_S5_RED_BASELINE
		if (obj->state() != TraderYD::YS_RELEASED || !obj->isQuiesced())
			return (int)STATE;
#endif
		delete obj;
		return (int)OK;
	}));
}

/* 9b 红控：release 后查询 worker 仍可 join，delete 触发 std::terminate（86）。 */
TEST(YdDestroyLifecycle, worker_joined_and_delete_does_not_terminate)
{
	expect_child_ok(run_forked([]() {
		R93YdApiStub stub;
		Sink sink;
		Probe* obj = make_probe(sink, stub);
		obj->connect();
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		obj->release();
		delete obj;
		return (int)OK;
	}));
}

/* 9a 红控：SDK 未发 notifyAfterApiDestroy 时不得删除 listener；remover 隔离实例，
 * 之后迟到的销毁通知仍落在存活对象上。 */
TEST(YdDestroyLifecycle, missing_after_destroy_notification_keeps_instance_and_remover_quarantines)
{
	expect_child_ok(run_forked([]() {
		R93YdApiStub stub;
		stub.destroy_mode_ = 0;
		Sink sink;
		Probe* obj = make_probe(sink, stub);
		obj->presetWorker();
		obj->connect();
		obj->setDrainTimeout(200);
		ITraderApi* handle = obj;
		deleteTrader(handle);
		if (handle != NULL)
			return (int)STATE;
		if (g_destroyed.load())
			return (int)NOT_QUARANTINED;
#ifndef WT_S5_RED_BASELINE
		if (obj->state() != TraderYD::YS_DRAIN_UNCERTAIN || obj->isQuiesced())
			return (int)STATE;
#endif
		// The SDK finishes later: the notification reaches a live listener.
		stub.listener_->notifyBeforeApiDestroy();
		stub.listener_->notifyAfterApiDestroy();
		if (g_destroyed.load())
			return (int)NOT_QUARANTINED;
		return (int)OK;
	}));
}

/* 9d 红控：YD 回调内 release 不得在回调栈上 startDestroy；外部 release 完成收尾。 */
TEST(YdDestroyLifecycle, release_from_callback_defers_destroy_to_external_release)
{
	expect_child_ok(run_forked([]() {
		R93YdApiStub stub;
		Sink sink;
		Probe* obj = make_probe(sink, stub);
		std::atomic<int> destroyInCallback{ -1 };
		std::atomic<int> loginInCallback{ 99 };
		sink.on_event = [&](WTSTraderEvent e, int32_t) {
			if (e != WTE_Connect)
				return;
			obj->release();
			destroyInCallback.store(stub.n_destroy_);
			loginInCallback.store(obj->login("u", "p", ""));
		};
		obj->connect();
		std::thread vendor([&]() { obj->notifyReadyForLogin(false); });
		vendor.join();
		if (destroyInCallback.load() != 0)
			return (int)RELEASE_EARLY;
		if (loginInCallback.load() >= 0)
			return (int)ADMISSION;
#ifndef WT_S5_RED_BASELINE
		if (obj->state() != TraderYD::YS_STOP_DEFERRED)
			return (int)STATE;
#endif
		obj->release();
		if (stub.n_destroy_ != 1)
			return (int)RELEASE_COUNT;
#ifndef WT_S5_RED_BASELINE
		if (obj->state() != TraderYD::YS_RELEASED)
			return (int)STATE;
#endif
		delete obj;
		return (int)OK;
	}));
}

/* 9a/9b 红控：YD 回调线程内 deleteTrader（旧实现就地 delete，worker 仍可 join →
 * terminate）；新实现隔离、出参置空、不删除。 */
TEST(YdDestroyLifecycle, delete_trader_from_callback_quarantines)
{
	expect_child_ok(run_forked([]() {
		R93YdApiStub stub;
		Sink sink;
		Probe* obj = make_probe(sink, stub);
		ITraderApi* handle = obj;
		std::atomic<bool> nulled{ false };
		sink.on_event = [&](WTSTraderEvent e, int32_t) {
			if (e != WTE_Connect)
				return;
			ITraderApi* victim = handle;
			deleteTrader(victim);
			nulled.store(victim == NULL);
		};
		obj->connect();
		std::thread vendor([&]() { obj->notifyReadyForLogin(false); });
		vendor.join();
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		if (!nulled.load())
			return (int)STATE;
		if (g_destroyed.load())
			return (int)NOT_QUARANTINED;
		const uint32_t delivered = sink.business();
		obj->notifyReadyForLogin(false);
		if (sink.business() != delivered)
			return (int)DELIVERED_LATE;
		return (int)OK;
	}));
}

/* 9c 红控：重复 connect 不得再建 API；release 后入口全部拒绝、connect 不复活。 */
TEST(YdDestroyLifecycle, admission_closed_after_release_and_repeated_connect_rejected)
{
	expect_child_ok(run_forked([]() {
		g_creates.store(0);
		R93YdApiStub stub;
		Sink sink;
		Probe* obj = make_probe(sink, stub);
		obj->presetWorker();
		obj->connect();
		obj->connect();
		if (g_creates.load() != 1 || stub.n_start_ != 1)
			return (int)ADMISSION;
		obj->release();
		obj->ready();
		WTSEntrust* entrust = WTSEntrust::create("ag2512", 1, 7000.0, "SHFE");
		WTSEntrustAction* action = WTSEntrustAction::create("ag2512", "SHFE");
		action->setEntrustID("yduser#0000000001");
		action->setOrderID("1");
		const bool rejected = obj->orderInsert(entrust) < 0 && obj->orderAction(action) < 0 &&
			obj->queryAccount() < 0 && obj->queryPositions() < 0 && obj->queryOrders() < 0 &&
			obj->queryTrades() < 0 && obj->login("u", "p", "") < 0 && obj->logout() < 0;
		entrust->release();
		action->release();
		obj->connect();
		if (!rejected || g_creates.load() != 1 || stub.n_insert_ != 0 || stub.n_cancel_ != 0)
			return (int)ADMISSION;
		delete obj;
		return (int)OK;
	}));
}

/* 隔离红控：release 后对象存活期内迟到的 YD 回调不得投递到宿主。 */
TEST(YdDestroyLifecycle, late_callback_after_release_is_not_delivered)
{
	expect_child_ok(run_forked([]() {
		R93YdApiStub stub;
		Sink sink;
		Probe* obj = make_probe(sink, stub);
		obj->presetWorker();
		obj->connect();
		obj->release();
		const uint32_t delivered = sink.business();
		obj->notifyReadyForLogin(false);
		obj->notifyLogin(1, 0, false);
		obj->notifyCaughtUp();
		if (sink.business() != delivered)
			return (int)DELIVERED_LATE;
		delete obj;
		return (int)OK;
	}));
}

/* 并发（TSan 目标）：YD 线程持续投递回调时外部 release；release 返回后不再投递，
 * 且 startDestroy 不与在途回调重叠。 */
TEST(YdDestroyLifecycle, release_drains_concurrent_callbacks_before_destroy)
{
	expect_child_ok(run_forked([]() {
		CountingStub stub;
		Sink sink;
		Probe* obj = make_probe(sink, stub);
		std::atomic<bool> overlap{ false };
		sink.on_event = [&](WTSTraderEvent, int32_t) {
			const int before = stub.destroys.load();
			std::this_thread::sleep_for(std::chrono::microseconds(200));
			if (before != 0 || stub.destroys.load() != 0)
				overlap.store(true);
		};
		obj->connect();
		std::atomic<bool> stopVendor{ false };
		std::thread vendor([&]() {
			while (!stopVendor.load())
				obj->notifyReadyForLogin(false);
		});
		if (!wait_for([&]() { return sink.events.load() > 20; }, 3000))
		{
			stopVendor.store(true);
			vendor.join();
			return (int)NOT_STARTED;
		}
		obj->release();
		const uint32_t delivered = sink.business();
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		const uint32_t later = sink.business();
		stopVendor.store(true);
		vendor.join();
		if (overlap.load())
			return (int)RELEASE_EARLY;
		if (later != delivered)
			return (int)DELIVERED_LATE;
		delete obj;
		return (int)OK;
	}));
}
