/*
 * xtp_lifecycle_s5_test.cpp — TraderXTP 回调生命周期（迭代03 s5，BUG-R93-005 本机部分，卡9）
 *
 *  - registerSpi(NULL) 后迟到的错误回调不得解引用空 sink；
 *  - 已进入包装层的 SDK 回调计数：release()/reconnect() 有界等待其返回后才 Release()；
 *    超时保留 API、实例不可证明静止，deleteTrader 隔离（9a）；
 *  - SDK 回调线程内 deleteTrader 隔离而非 delete（9b/9d）；
 *  - 被替换/释放的 API 的登录会话迟到 OnDisconnected 不得对新 API 重登；
 *  - init 缺模块显式失败，已初始化实例拒绝重复 init（9c）。
 * 真实 XTP SDK 的回调排空/延迟销毁合同仍为外部条件，本文件只证明包装层行为。
 * 每例 fork 子进程（旧实现可能 SIGSEGV/terminate）；进程内 fake API，不连接柜台。
 * WT_S5_RED_BASELINE：以改前产品源码构建红控时不访问改前不存在的成员。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/executor_work_guard.hpp>

#include <API/XTP2.2.32.2/xtp_trader_api.h>

#include "Includes/IBaseDataMgr.h"
#include "Includes/ITraderApi.h"
#include "Includes/WTSCollection.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSTradeDef.hpp"
#include "Includes/WTSVariant.hpp"
#include "Share/Converter.hpp"
#include "Share/DLLHelper.hpp"
#include "Share/ModuleHelper.hpp"
#include "Share/StdUtils.hpp"
#include "Share/WtKVCache.hpp"

#define private public
#define protected public
#include "TraderXTP/TraderXTP.h"
#undef protected
#undef private

#include "r93_xtp_api_stub.h"

extern "C" void deleteTrader(ITraderApi*& trader);

namespace
{
	namespace fs = std::filesystem;

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

	class CountingStub final : public R93XtpApiStub
	{
	public:
		std::atomic<int> releases{ 0 };
		std::atomic<int> logins{ 0 };
		void Release() override { releases.fetch_add(1); }
		uint64_t Login(const char* ip, int port, const char* user, const char* password,
			XTP_PROTOCOL_TYPE sock_type, const char* local_ip = NULL) override
		{
			logins.fetch_add(1);
			return R93XtpApiStub::Login(ip, port, user, password, sock_type, local_ip);
		}
	};

	CountingStub* g_stub = NULL;
	XTP::API::TraderApi* make_stub(uint8_t, const char*, XTP_LOG_LEVEL) { return g_stub; }

	class NullBdMgr : public IBaseDataMgr
	{
	public:
		WTSCommodityInfo* getCommodity(const char*) override { return NULL; }
		WTSCommodityInfo* getCommodity(const char*, const char*) override { return NULL; }
		WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t = 0) override
		{
			if (ct != NULL && code != NULL && exchg != NULL && strcmp(code, ct->getCode()) == 0 && strcmp(exchg, ct->getExchg()) == 0)
				return ct;
			return NULL;
		}
		WTSContractInfo* ct = NULL;
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
			if (e == WTE_Close)
				closes.fetch_add(1);
			else
				connects.fetch_add(1);
			if (on_event)
				on_event(e, ec);
		}
		void handleTraderLog(WTSLogLevel, const char*) override {}
		void onLoginResult(bool, const char*, uint32_t) override { logins.fetch_add(1); }
		void onRspEntrust(WTSEntrust*, WTSError*) override { entrusts.fetch_add(1); }
		void onTraderError(WTSError*, void* = NULL) override { errors.fetch_add(1); }

		NullBdMgr bd;
		std::atomic<uint32_t> closes{ 0 }, connects{ 0 }, logins{ 0 }, entrusts{ 0 }, errors{ 0 };
		std::function<void(WTSTraderEvent, int32_t)> on_event;
	};

	class Barrier
	{
	public:
		void arrive() { std::lock_guard<std::mutex> l(m); entered = true; cv.notify_all(); }
		void block() { std::unique_lock<std::mutex> l(m); cv.wait(l, [this] { return proceed; }); }
		bool wait_entered(int ms)
		{
			std::unique_lock<std::mutex> l(m);
			return cv.wait_for(l, std::chrono::milliseconds(ms), [this] { return entered; });
		}
		void open() { std::lock_guard<std::mutex> l(m); proceed = true; cv.notify_all(); }

	private:
		std::mutex m;
		std::condition_variable cv;
		bool entered = false;
		bool proceed = false;
	};

	std::atomic<bool> g_destroyed{ false };
	class Probe final : public TraderXTP
	{
	public:
		~Probe() { g_destroyed.store(true); }
	};

	WTSVariant* make_cfg(const char* module)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("user", "xtpuser");
		cfg->append("pass", "xtppass");
		cfg->append("acckey", "acc-key-s5");
		cfg->append("host", "127.0.0.1");
		cfg->append("port", (int32_t)11101);
		cfg->append("client", (int32_t)1);
		const fs::path flow = fs::temp_directory_path() / ("wt_xtp_s5_" + std::to_string((long)::getpid()));
		cfg->append("flowdir", flow.string().c_str());
		if (module != NULL)
			cfg->append("xtpmodule", module);
		return cfg;
	}

	/* 缺 vendor 模块时 init 失败，但配置已解析；测试注入 fake creator 后连接。 */
	Probe* make_probe(Sink& sink, CountingStub& stub)
	{
		g_stub = &stub;
		Probe* obj = new Probe();
		obj->registerSpi(&sink);
		WTSVariant* cfg = make_cfg("wt_xtp_s5_module_absent");
		(void)obj->init(cfg);
		cfg->release();
		obj->m_funcCreator = &make_stub;
		obj->_reconnect_delay = std::chrono::milliseconds(30);
		return obj;
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

	XTPRI error_info(int id, const char* msg)
	{
		XTPRI info;
		memset(&info, 0, sizeof(info));
		info.error_id = id;
		snprintf(info.error_msg, sizeof(info.error_msg), "%s", msg);
		return info;
	}
}

/* 红控：registerSpi(NULL) 后错误下单/撤单回调解引用空 sink（旧实现 SIGSEGV）。 */
TEST(XtpS5Lifecycle, error_callbacks_after_register_spi_null_do_not_crash)
{
	expect_child_ok(run_forked([]() {
		CountingStub stub;
		Sink sink;
		WTSCommodityInfo* comm = WTSCommodityInfo::create("stock600", "stock", "SSE", "TRADE", "CHINA");
		comm->setCategory(CC_Stock);
		WTSContractInfo* ct = WTSContractInfo::create("600000", "600000", "SSE", "stock600");
		ct->setCommInfo(comm);
		sink.bd.ct = ct;
		Probe* obj = make_probe(sink, stub);
		obj->registerSpi(NULL);
		XTPOrderInfo order;
		memset(&order, 0, sizeof(order));
		order.market = XTP_MKT_SH_A;
		strncpy(order.ticker, "600000", sizeof(order.ticker) - 1);
		order.quantity = 100;
		order.price = 10.0;
		XTPRI err = error_info(11, "rejected");
		obj->OnOrderEvent(&order, &err, 1);
		XTPOrderCancelInfo cancel;
		memset(&cancel, 0, sizeof(cancel));
		obj->OnCancelOrderError(&cancel, &err, 1);
		if (sink.entrusts.load() != 0 || sink.errors.load() != 0)
			return (int)DELIVERED_LATE;
		delete obj;
		return (int)OK;
	}));
}

/* 9b/9d 红控：SDK 回调线程内 deleteTrader（旧实现在回调里 delete，worker 仍可 join →
 * terminate）；新实现隔离、出参置空、不删除。 */
TEST(XtpS5Lifecycle, delete_trader_from_sdk_callback_quarantines)
{
	expect_child_ok(run_forked([]() {
		CountingStub stub;
		Sink sink;
		Probe* obj = make_probe(sink, stub);
		ITraderApi* handle = obj;
		std::atomic<bool> nulled{ false };
		sink.on_event = [&](WTSTraderEvent e, int32_t) {
			if (e != WTE_Close)
				return;
			ITraderApi* victim = handle;
			deleteTrader(victim);
			nulled.store(victim == NULL);
		};
		obj->connect();
		std::thread vendor([&]() { obj->OnDisconnected(0, 1); });
		vendor.join();
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		if (!nulled.load())
			return (int)STATE;
		if (g_destroyed.load())
			return (int)NOT_QUARANTINED;
		return (int)OK;
	}));
}

/* 红控：已进入包装层的 SDK 回调未返回时，release 不得 Release() API。 */
TEST(XtpS5Lifecycle, release_waits_for_inflight_callback_before_api_release)
{
	expect_child_ok(run_forked([]() {
		CountingStub stub;
		Sink sink;
		Probe* obj = make_probe(sink, stub);
		Barrier cb;
		sink.on_event = [&](WTSTraderEvent e, int32_t) {
			if (e == WTE_Close)
			{
				cb.arrive();
				cb.block();
			}
		};
		obj->connect();
		std::thread vendor([&]() { obj->OnDisconnected(0, 7); });
		if (!cb.wait_entered(3000))
		{
			cb.open();
			vendor.join();
			return (int)NOT_STARTED;
		}
		std::atomic<bool> returned{ false };
		std::thread releaser([&]() { obj->release(); returned.store(true); });
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		const bool early = returned.load() || stub.releases.load() != 0;
		cb.open();
		vendor.join();
		releaser.join();
		if (early)
			return (int)RELEASE_EARLY;
		if (stub.releases.load() != 1)
			return (int)RELEASE_COUNT;
#ifndef WT_S5_RED_BASELINE
		if (!obj->isQuiesced())
			return (int)STATE;
#endif
		delete obj;
		return (int)OK;
	}));
}

/* 9a：回调在时限内不返回 → 不 Release()、实例不可证明静止、remover 隔离。 */
TEST(XtpS5Lifecycle, callback_drain_timeout_retains_api_and_remover_quarantines)
{
	expect_child_ok(run_forked([]() {
		CountingStub stub;
		Sink sink;
		Probe* obj = make_probe(sink, stub);
#ifndef WT_S5_RED_BASELINE
		obj->_drain_timeout_ms = 200;
#endif
		Barrier cb;
		sink.on_event = [&](WTSTraderEvent e, int32_t) {
			if (e == WTE_Close)
			{
				cb.arrive();
				cb.block();
			}
		};
		obj->connect();
		std::thread vendor([&]() { obj->OnDisconnected(0, 7); });
		if (!cb.wait_entered(3000))
		{
			cb.open();
			vendor.join();
			return (int)NOT_STARTED;
		}
		obj->release();
		const int releasedWhileActive = stub.releases.load();
#ifndef WT_S5_RED_BASELINE
		const bool uncertain = !obj->isQuiesced();
		ITraderApi* handle = obj;
		deleteTrader(handle);
		const bool quarantined = handle == NULL && !g_destroyed.load();
#else
		const bool uncertain = true;
		const bool quarantined = true;
#endif
		cb.open();
		vendor.join();
		if (releasedWhileActive != 0)
			return (int)RELEASE_EARLY;
		if (!uncertain)
			return (int)STATE;
		if (!quarantined)
			return (int)NOT_QUARANTINED;
		return (int)OK;
	}));
}

/* 红控：被替换 API 的登录会话迟到 OnDisconnected 不得关闭/重登新 API 的会话。 */
TEST(XtpS5Lifecycle, late_disconnect_of_replaced_api_session_does_not_relogin)
{
	expect_child_ok(run_forked([]() {
		CountingStub stub;
		Sink sink;
		Probe* obj = make_probe(sink, stub);
		obj->connect();
		stub.login_ret_ = 1001;
		if (obj->login("xtpuser", "xtppass", "") != 0 || !wait_for([&]() { return sink.logins.load() == 1; }, 2000))
			return (int)NOT_STARTED;
		// connect() on a live instance replaces the API (reconnect); log in again.
		obj->connect();
		stub.login_ret_ = 2002;
		if (obj->login("xtpuser", "xtppass", "") != 0)
			return (int)NOT_STARTED;
		const int loginsBefore = stub.logins.load();
		const uint32_t closesBefore = sink.closes.load();
		std::thread vendor([&]() { obj->OnDisconnected(1001, 9); });
		vendor.join();
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
		const bool relogin = stub.logins.load() != loginsBefore;
		const bool closed = sink.closes.load() != closesBefore;
		obj->release();
		if (relogin || closed)
			return (int)DELIVERED_LATE;
		// Positive control: the current session's disconnect is still handled.
		delete obj;
		return (int)OK;
	}));
}

/* 正控：当前会话的 OnDisconnected 仍发布 WTE_Close 并按 worker 代重登。 */
TEST(XtpS5Lifecycle, disconnect_of_current_session_still_relogins)
{
	expect_child_ok(run_forked([]() {
		CountingStub stub;
		Sink sink;
		Probe* obj = make_probe(sink, stub);
		obj->connect();
		stub.login_ret_ = 3003;
		if (obj->login("xtpuser", "xtppass", "") != 0 || !wait_for([&]() { return sink.logins.load() == 1; }, 2000))
			return (int)NOT_STARTED;
		const int loginsBefore = stub.logins.load();
		std::thread vendor([&]() { obj->OnDisconnected(3003, 9); });
		vendor.join();
		if (!wait_for([&]() { return stub.logins.load() > loginsBefore; }, 2000))
			return (int)TIMEOUT;
		if (sink.closes.load() != 1)
			return (int)STATE;
		obj->release();
		delete obj;
		return (int)OK;
	}));
}

/* 9c 与失败传播红控：缺模块 init 返回 false；已初始化实例拒绝重复 init 且保留原配置。 */
TEST(XtpS5Lifecycle, init_fails_without_module_and_rejects_repeated_init)
{
	expect_child_ok(run_forked([]() {
		Sink sink;
		Probe* obj = new Probe();
		obj->registerSpi(&sink);
		WTSVariant* cfg = make_cfg("wt_xtp_s5_module_absent");
		const bool first = obj->init(cfg);
		cfg->release();
		if (first)
			return (int)STATE;
		// Simulate a successfully initialized instance (creator resolved).
		obj->m_funcCreator = &make_stub;
		WTSVariant* other = make_cfg("wt_xtp_s5_module_absent");
		other->append("user", "otheruser");
		const bool second = obj->init(other);
		other->release();
		if (second)
			return (int)ADMISSION;
		{
			std::lock_guard<std::mutex> lock(obj->_login_state_mutex);
			if (obj->_user != "xtpuser")
				return (int)ADMISSION;
		}
		obj->m_funcCreator = NULL;
		delete obj;
		return (int)OK;
	}));
}
