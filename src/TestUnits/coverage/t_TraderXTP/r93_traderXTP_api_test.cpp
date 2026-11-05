/*
 * lane R93（第十七波）— TraderXTP 有 API 指针半边
 *
 * 目标：src/TraderXTP/TraderXTP.cpp。用 r93_xtp_api_stub.h 的机械空实现替代真实
 * XTP::API::TraderApi，覆盖 connect/reconnect/release、login 失败/成功/重连、
 * logout、orderInsert/orderAction、四个查询入口以及 eid/oid 缓存回填。
 * 不连真实柜台。TraderXTP 的数据成员全是 private，本 TU 按仓库既有先例
 * （p03/p08/p10）在包含完所有依赖头之后用 #define private public 暴露，仅用于注入
 * stub 与读取状态，不改生产代码。
 */
#include <gtest/gtest.h>

#include <cstdlib>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
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

USING_NS_WTP;

namespace
{
	namespace fs = std::filesystem;

	int g_r93_xtp_tmp_seq = 0;

	class R93XtpTempCwd
	{
	public:
		R93XtpTempCwd()
		{
			_old = fs::current_path();
			_dir = fs::temp_directory_path() /
				("wt_r93_xtp_" + std::to_string((long)::getpid()) + "_" + std::to_string(++g_r93_xtp_tmp_seq));
			std::error_code ec;
			fs::create_directories(_dir, ec);
			fs::current_path(_dir, ec);
		}
		~R93XtpTempCwd()
		{
			std::error_code ec;
			fs::current_path(_old, ec);
			fs::remove_all(_dir, ec);
		}
		std::string path(const char* leaf) const { return (_dir / leaf).string(); }

	private:
		fs::path _old;
		fs::path _dir;
	};

	class R93XtpBdMgr : public IBaseDataMgr
	{
	public:
		void regis(WTSCommodityInfo* comm, WTSContractInfo* ct)
		{
			_comm = comm;
			_ct = ct;
			_ct->setCommInfo(comm);
		}
		WTSCommodityInfo* getCommodity(const char*) override { return _comm; }
		WTSCommodityInfo* getCommodity(const char*, const char*) override { return _comm; }
		WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t = 0) override
		{
			if (_ct && strcmp(code ? code : "", _ct->getCode()) == 0 &&
				strcmp(exchg ? exchg : "", _ct->getExchg()) == 0)
				return _ct;
			return NULL;
		}
		WTSArray* getContracts(const char* = "", uint32_t = 0) override { return NULL; }
		WTSSessionInfo* getSession(const char*) override { return NULL; }
		WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return NULL; }
		WTSArray* getAllSessions() override { return NULL; }
		bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
		uint32_t calcTradingDate(const char*, uint32_t uDate, uint32_t, bool = false) override { return uDate; }
		uint64_t getBoundaryTime(const char*, uint32_t tDate, bool = false, bool = true) override { return tDate; }

	private:
		WTSCommodityInfo* _comm = NULL;
		WTSContractInfo* _ct = NULL;
	};

	class R93XtpSink : public ITraderSpi
	{
	public:
		struct LoginRec { bool succ; std::string msg; uint32_t date; };

		IBaseDataMgr* getBaseDataMgr() override { return &bd_; }
		void handleEvent(WTSTraderEvent e, int32_t ec) override
		{
			events.push_back({ (int)e, ec });
			if (event_hook)
				event_hook();
		}
		void handleTraderLog(WTSLogLevel, const char* message) override
		{
			logs.push_back(message ? message : "");
		}
		void onLoginResult(bool succ, const char* msg, uint32_t date) override
		{
			std::lock_guard<std::mutex> lock(logins_mutex_);
			logins.push_back({ succ, msg ? msg : "", date });
			logins_condition_.notify_all();
		}
		bool wait_for_login_count(size_t count, std::chrono::milliseconds timeout)
		{
			std::unique_lock<std::mutex> lock(logins_mutex_);
			return logins_condition_.wait_for(lock, timeout, [this, count]() { return logins.size() >= count; });
		}
		size_t login_count()
		{
			std::lock_guard<std::mutex> lock(logins_mutex_);
			return logins.size();
		}
		LoginRec login_at(size_t index)
		{
			std::lock_guard<std::mutex> lock(logins_mutex_);
			return logins.at(index);
		}
		void onPushOrder(WTSOrderInfo* order) override
		{
			if (!order)
				return;
			orderUserTag_ = order->getUserTag();
			orderID_ = order->getOrderID();
			orderEntrustID_ = order->getEntrustID();
			++n_push_order_;
		}
		void onPushTrade(WTSTradeInfo* trade) override
		{
			if (!trade)
				return;
			tradeUserTag_ = trade->getUserTag();
			tradeRefOrder_ = trade->getRefOrder();
			++n_push_trade_;
		}

		R93XtpBdMgr bd_;
		std::mutex logins_mutex_;
		std::condition_variable logins_condition_;
		std::vector<std::pair<int, int32_t>> events;
		std::function<void()> event_hook;
		std::vector<std::string> logs;
		std::vector<LoginRec> logins;
		std::string orderUserTag_;
		std::string orderID_;
		std::string orderEntrustID_;
		std::string tradeUserTag_;
		std::string tradeRefOrder_;
		int n_push_order_ = 0;
		int n_push_trade_ = 0;
	};

	R93XtpApiStub* g_r93_xtp_stub = NULL;
	XTP::API::TraderApi* r93_xtp_create(uint8_t, const char*, XTP_LOG_LEVEL) { return g_r93_xtp_stub; }

	class R93XtpReleaseGuard
	{
	public:
		explicit R93XtpReleaseGuard(TraderXTP& api) : api_(api) {}
		~R93XtpReleaseGuard() { api_.release(); }

	private:
		TraderXTP& api_;
	};

	class R93XtpBlockingLoginStub : public R93XtpApiStub
	{
	public:
		std::function<void()> login_hook_;

		void Release() override
		{
			release_count_.fetch_add(1);
			R93XtpApiStub::Release();
		}

		uint64_t Login(const char* ip, int port, const char* user, const char* password,
			XTP_PROTOCOL_TYPE protocol, const char* appInfo) override
		{
			const uint64_t result = R93XtpApiStub::Login(ip, port, user, password, protocol, appInfo);
			if (login_hook_)
				login_hook_();
			std::unique_lock<std::mutex> lock(mutex_);
			login_entered_ = true;
			condition_.notify_all();
			condition_.wait(lock, [this]() { return login_released_; });
			return result;
		}

		bool wait_for_login(std::chrono::milliseconds timeout)
		{
			std::unique_lock<std::mutex> lock(mutex_);
			return condition_.wait_for(lock, timeout, [this]() { return login_entered_; });
		}

		void unblock_login()
		{
			std::lock_guard<std::mutex> lock(mutex_);
			login_released_ = true;
			condition_.notify_all();
		}

		int release_count() const { return release_count_.load(); }

	private:
		std::atomic<int> release_count_{0};
		std::mutex mutex_;
		std::condition_variable condition_;
		bool login_entered_ = false;
		bool login_released_ = false;
	};

	class R93XtpBlockingReleaseStub : public R93XtpApiStub
	{
	public:
		void Release() override
		{
			release_count_.fetch_add(1);
			std::unique_lock<std::mutex> lock(mutex_);
			release_entered_ = true;
			condition_.notify_all();
			condition_.wait(lock, [this]() { return release_unblocked_; });
		}
		bool wait_for_release(std::chrono::milliseconds timeout)
		{
			std::unique_lock<std::mutex> lock(mutex_);
			return condition_.wait_for(lock, timeout, [this]() { return release_entered_; });
		}
		void unblock_release()
		{
			std::lock_guard<std::mutex> lock(mutex_);
			release_unblocked_ = true;
			condition_.notify_all();
		}
		int release_count() const { return release_count_.load(); }

	private:
		std::atomic<int> release_count_{0};
		std::mutex mutex_;
		std::condition_variable condition_;
		bool release_entered_ = false;
		bool release_unblocked_ = false;
	};

	class R93XtpBlockingQueryStub : public R93XtpApiStub
	{
	public:
		void Release() override { release_count_.fetch_add(1); }
		int QueryAsset(uint64_t, int) override
		{
			std::unique_lock<std::mutex> lock(mutex_);
			query_entered_ = true;
			condition_.notify_all();
			condition_.wait(lock, [this]() { return query_released_; });
			return query_ret_;
		}
		bool wait_for_query(std::chrono::milliseconds timeout)
		{
			std::unique_lock<std::mutex> lock(mutex_);
			return condition_.wait_for(lock, timeout, [this]() { return query_entered_; });
		}
		void unblock_query()
		{
			std::lock_guard<std::mutex> lock(mutex_);
			query_released_ = true;
			condition_.notify_all();
		}
		int release_count() const { return release_count_.load(); }

	private:
		std::atomic<int> release_count_{0};
		std::mutex mutex_;
		std::condition_variable condition_;
		bool query_entered_ = false;
		bool query_released_ = false;
	};

	uint32_t r93_xtp_eid_ref(const char* eid)
	{
		const char* pos = strrchr(eid, '#');
		return pos ? (uint32_t)strtoul(pos + 1, NULL, 10) : 0;
	}

	class R93XtpTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			WTSVariant* cfg = WTSVariant::createObject();
			cfg->append("user", "xtpuser");
			cfg->append("pass", "xtppass");
			cfg->append("acckey", "acc-key-93");
			cfg->append("host", "127.0.0.1");
			cfg->append("port", (int32_t)11101);
			cfg->append("client", (int32_t)1);
			cfg->append("quick", true);
			cfg->append("hbinterval", (uint32_t)12);
			cfg->append("flowdir", temp_.path("flow").c_str());
			// BUG-R93-005 (s5): init now fails closed when the XTP vendor module is
			// absent (it is not available on macOS). The configuration is parsed
			// before the module load, and the cases inject a fake creator below.
			ASSERT_FALSE(api_.init(cfg));
			cfg->release();

			comm_ = WTSCommodityInfo::create("stock600", "股票", "SSE", "TRADE", "CHINA");
			comm_->setCategory(CC_Stock);
			comm_->setTradingMode(TM_LongT1);
			comm_->setVolScale(1);
			ct_ = WTSContractInfo::create("600000", "浦发银行", "SSE", "stock600");
			sink_.bd_.regis(comm_, ct_);
			api_.registerSpi(&sink_);
		}

		void TearDown() override
		{
			api_.release();
			g_r93_xtp_stub = NULL;
			ct_->release();
			comm_->release();
		}

		void make_ready()
		{
			connect_stub();
			std::lock_guard<std::mutex> lock(api_._login_state_mutex);
			api_._state = TraderXTP::TS_ALLREADY;
			api_._sessionid = 5555;
			api_._tradingday = 20260105;
			api_._inited = true;
		}

		void connect_stub()
		{
			g_r93_xtp_stub = &stub_;
			api_.m_funcCreator = &r93_xtp_create;
			api_.connect();
		}

		R93XtpTempCwd temp_;
		WTSCommodityInfo* comm_ = NULL;
		WTSContractInfo* ct_ = NULL;
		R93XtpSink sink_;
		R93XtpApiStub stub_;
		TraderXTP api_;
	};

	/* ------------------------------------------------------------------ */
	/* 连接 / 释放                                                          */
	/* ------------------------------------------------------------------ */

	/// TraderXTP.cpp L623-640（release 的 RegisterSpi(NULL)+Release 清容器）、
	/// reconnect：目录、creator、QUICK 订阅、软件版本/Key、心跳、注册 SPI、WTE_Connect；
	/// connect 启动真实 worker，disconnect→release 停止并 join worker。
	TEST_F(R93XtpTest, connect_reconnect_and_release_use_stub)
	{
		g_r93_xtp_stub = &stub_;
		api_.m_funcCreator = &r93_xtp_create;

		api_.connect();
		EXPECT_EQ(static_cast<XTP::API::TraderSpi*>(&api_), stub_.spi_);
		EXPECT_EQ(1, stub_.n_subscribe_);
		EXPECT_EQ(XTP_TERT_QUICK, stub_.resume_type_);
		EXPECT_EQ("1.0.0", stub_.software_version_);
		EXPECT_EQ("acc-key-93", stub_.software_key_);
		EXPECT_EQ(15u, stub_.heartbeat_);
		EXPECT_EQ(1, stub_.n_register_spi_);
		ASSERT_EQ(1u, sink_.events.size());
		EXPECT_EQ((int)WTE_Connect, sink_.events[0].first);
		EXPECT_EQ(0, sink_.events[0].second);

		api_.disconnect();
		EXPECT_EQ(1, stub_.n_release_);
		EXPECT_EQ(2, stub_.n_register_spi_);	// 注册 + 释放前置 NULL
		EXPECT_EQ(NULL, stub_.spi_);
		EXPECT_EQ(NULL, api_._api);
		EXPECT_FALSE(api_._thrd_worker);

		api_.release();
		EXPECT_EQ(1, stub_.n_release_);
		g_r93_xtp_stub = NULL;
	}

	TEST_F(R93XtpTest, logout_before_successful_login_is_rejected_without_sdk_call)
	{
		connect_stub();
		EXPECT_EQ(TraderXTP::TS_NOTLOGIN, api_._state);
		EXPECT_EQ(0u, api_._sessionid);
		EXPECT_FALSE(api_.isConnected());

		EXPECT_EQ(-1, api_.logout());
		EXPECT_EQ(0, stub_.n_logout_);
		EXPECT_EQ(0u, stub_.logout_session_);
	}

	TEST_F(R93XtpTest, connect_notification_can_login_while_reconnect_waits)
	{
		std::mutex event_mutex;
		std::condition_variable event_condition;
		bool event_entered = false;
		bool allow_login = false;
		std::atomic<int> event_calls(0);
		std::atomic<int> login_result(-99);
		R93XtpReleaseGuard release_guard(api_);
		g_r93_xtp_stub = &stub_;
		api_.m_funcCreator = &r93_xtp_create;
		sink_.event_hook = [this, &event_mutex, &event_condition, &event_entered, &allow_login, &event_calls, &login_result]() {
			if (event_calls.fetch_add(1) != 0)
				return;
			std::unique_lock<std::mutex> lock(event_mutex);
			event_entered = true;
			event_condition.notify_all();
			event_condition.wait(lock, [&allow_login]() { return allow_login; });
			lock.unlock();
			login_result.store(api_.login("xtpuser", "xtppass", ""));
		};

		std::thread first_connect([&]() { api_.connect(); });
		{
			std::unique_lock<std::mutex> lock(event_mutex);
			if (!event_condition.wait_for(lock, std::chrono::seconds(2), [&event_entered]() { return event_entered; }))
			{
				lock.unlock();
				first_connect.join();
				FAIL() << "WTE_Connect was not delivered to the sink";
				return;
			}
		}

		std::thread second_connect([&]() { api_.connect(); });
		const auto reconfigure_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		bool reconfigure_entered = false;
		while (!reconfigure_entered && std::chrono::steady_clock::now() < reconfigure_deadline)
		{
			{
				std::lock_guard<std::mutex> lock(api_._api_lifecycle_mutex);
				reconfigure_entered = api_._api_reconfiguring;
			}
			std::this_thread::yield();
		}
		{
			std::lock_guard<std::mutex> lock(event_mutex);
			allow_login = true;
			event_condition.notify_all();
		}
		first_connect.join();
		second_connect.join();

		EXPECT_TRUE(reconfigure_entered);
		EXPECT_EQ(0, login_result.load());
		EXPECT_EQ(1, stub_.n_login_);
		EXPECT_EQ(1, stub_.n_release_);
		EXPECT_EQ(&stub_, api_._api);
		sink_.event_hook = std::function<void()>();
	}

	TEST_F(R93XtpTest, failed_reconnect_retry_is_owned_and_cancellable)
	{
		g_r93_xtp_stub = NULL;
		api_.m_funcCreator = &r93_xtp_create;
		api_._reconnect_delay = std::chrono::milliseconds(80);
		api_.connect();
		ASSERT_EQ(1u, sink_.events.size());
		EXPECT_EQ((int)WTE_Connect, sink_.events[0].first);
		EXPECT_EQ(-1, sink_.events[0].second);

		api_.release();
		api_.release();
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		EXPECT_EQ(1u, sink_.events.size());
		g_r93_xtp_stub = NULL;
	}

	TEST_F(R93XtpTest, release_cancels_delayed_relogin_and_is_repeatable)
	{
		g_r93_xtp_stub = &stub_;
		api_.m_funcCreator = &r93_xtp_create;
		api_._inited = true;
		api_._reconnect_delay = std::chrono::milliseconds(80);
		api_.connect();
		api_.scheduleReconnect(api_._worker_generation.load(), true);
		std::this_thread::sleep_for(std::chrono::milliseconds(15));

		api_.release();
		api_.release();
		std::this_thread::sleep_for(std::chrono::milliseconds(100));

		EXPECT_EQ(0, stub_.n_login_);
		EXPECT_EQ(1, stub_.n_release_);
		g_r93_xtp_stub = NULL;
	}

	TEST_F(R93XtpTest, release_joins_worker_blocked_in_login)
	{
		R93XtpBlockingLoginStub blocking_stub;
		R93XtpReleaseGuard release_guard(api_);
		g_r93_xtp_stub = &blocking_stub;
		api_.m_funcCreator = &r93_xtp_create;
		api_._inited = true;
		api_._reconnect_delay = std::chrono::milliseconds(10);
		api_.connect();
		api_.OnDisconnected(0, 1);

		if (!blocking_stub.wait_for_login(std::chrono::seconds(2)))
		{
			blocking_stub.unblock_login();
			api_.release();
			g_r93_xtp_stub = NULL;
			FAIL() << "worker did not enter the fake blocking Login";
			return;
		}

		std::atomic<bool> release_finished(false);
		std::thread release_thread([&]() {
			api_.release();
			release_finished.store(true);
		});
		const auto stop_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		bool stop_requested = false;
		while (!stop_requested && std::chrono::steady_clock::now() < stop_deadline)
		{
			stop_requested = api_._worker_stopping.load();
			std::this_thread::yield();
		}
		const bool release_waited_for_login = !release_finished.load();
		const int release_calls_before_login_returned = blocking_stub.release_count();
		blocking_stub.unblock_login();
		release_thread.join();

		EXPECT_TRUE(stop_requested);
		EXPECT_TRUE(release_waited_for_login);
		EXPECT_EQ(0, release_calls_before_login_returned);
		EXPECT_TRUE(release_finished.load());
		EXPECT_EQ(1, blocking_stub.release_count());
		g_r93_xtp_stub = NULL;
	}

	TEST_F(R93XtpTest, release_waits_for_public_login_and_callback_release_defers)
	{
		R93XtpBlockingLoginStub blocking_stub;
		R93XtpReleaseGuard release_guard(api_);
		g_r93_xtp_stub = &blocking_stub;
		api_.m_funcCreator = &r93_xtp_create;
		api_._inited = true;
		api_.connect();
		sink_.event_hook = [this]() { api_.release(); };
		blocking_stub.login_hook_ = [this]() { api_.OnDisconnected(0, 1); };

		std::atomic<int> login_result(-99);
		std::atomic<bool> login_finished(false);
		std::thread login_thread([&]() {
			login_result.store(api_.login("xtpuser", "xtppass", ""));
			login_finished.store(true);
		});
		if (!blocking_stub.wait_for_login(std::chrono::seconds(2)))
		{
			blocking_stub.unblock_login();
			login_thread.join();
			api_.release();
			g_r93_xtp_stub = NULL;
			FAIL() << "public login did not return from the reentrant SPI callback";
			return;
		}

		std::atomic<bool> release_started(false);
		std::atomic<bool> release_finished(false);
		std::thread release_thread([&]() {
			release_started.store(true);
			api_.release();
			release_finished.store(true);
		});
		while (!release_started.load())
			std::this_thread::yield();
		const auto release_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		bool release_entered = false;
		while (!release_entered && std::chrono::steady_clock::now() < release_deadline)
		{
			{
				std::lock_guard<std::mutex> lock(api_._api_lifecycle_mutex);
				release_entered = api_._api_release_in_progress;
			}
			std::this_thread::yield();
		}
		const bool release_waited_for_public_login = !release_finished.load();
		const int release_calls_before_login_returned = blocking_stub.release_count();
		blocking_stub.unblock_login();
		login_thread.join();
		release_thread.join();

		EXPECT_EQ(0, login_result.load());
		EXPECT_TRUE(login_finished.load());
		EXPECT_TRUE(release_entered);
		EXPECT_TRUE(release_waited_for_public_login);
		EXPECT_EQ(0, release_calls_before_login_returned);
		EXPECT_TRUE(release_finished.load());
		EXPECT_EQ(1, blocking_stub.release_count());
		sink_.event_hook = std::function<void()>();
		g_r93_xtp_stub = NULL;
	}

	TEST_F(R93XtpTest, concurrent_release_calls_release_api_once)
	{
		R93XtpBlockingReleaseStub blocking_stub;
		R93XtpReleaseGuard release_guard(api_);
		g_r93_xtp_stub = &blocking_stub;
		api_.m_funcCreator = &r93_xtp_create;
		api_.connect();

		std::atomic<bool> first_finished(false);
		std::thread first([&]() {
			api_.release();
			first_finished.store(true);
		});
		if (!blocking_stub.wait_for_release(std::chrono::seconds(2)))
		{
			blocking_stub.unblock_release();
			first.join();
			g_r93_xtp_stub = NULL;
			FAIL() << "first release did not enter the fake blocking SDK Release";
			return;
		}

		std::atomic<bool> second_finished(false);
		std::thread second([&]() {
			api_.release();
			second_finished.store(true);
		});
		const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		bool second_waiting_at_gate = false;
		while (!second_waiting_at_gate && std::chrono::steady_clock::now() < wait_deadline)
		{
			{
				std::lock_guard<std::mutex> lock(api_._api_lifecycle_mutex);
				second_waiting_at_gate = api_._api_release_waiters == 1;
			}
			std::this_thread::yield();
		}
		const bool releases_overlapped = !second_finished.load() && blocking_stub.release_count() == 1;
		blocking_stub.unblock_release();
		first.join();
		second.join();

		EXPECT_TRUE(second_waiting_at_gate);
		EXPECT_TRUE(releases_overlapped);
		EXPECT_TRUE(first_finished.load());
		EXPECT_TRUE(second_finished.load());
		EXPECT_EQ(1, blocking_stub.release_count());
		EXPECT_EQ(NULL, api_._api);
		g_r93_xtp_stub = NULL;
	}

	TEST_F(R93XtpTest, reconnect_drains_inflight_sdk_calls_before_replacing_api)
	{
		R93XtpBlockingQueryStub blocking_stub;
		R93XtpReleaseGuard release_guard(api_);
		g_r93_xtp_stub = &blocking_stub;
		api_.m_funcCreator = &r93_xtp_create;
		api_.connect();

		std::atomic<int> query_result(-99);
		std::thread query_thread([&]() { query_result.store(api_.queryAccount()); });
		if (!blocking_stub.wait_for_query(std::chrono::seconds(2)))
		{
			blocking_stub.unblock_query();
			query_thread.join();
			api_.release();
			g_r93_xtp_stub = NULL;
			FAIL() << "query did not enter the fake blocking SDK call";
			return;
		}

		std::atomic<bool> reconnect_finished(false);
		std::thread reconnect_thread([&]() {
			api_.connect();
			reconnect_finished.store(true);
		});
		const auto reconfigure_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		bool reconfigure_entered = false;
		while (!reconfigure_entered && std::chrono::steady_clock::now() < reconfigure_deadline)
		{
			{
				std::lock_guard<std::mutex> lock(api_._api_lifecycle_mutex);
				reconfigure_entered = api_._api_reconfiguring;
			}
			std::this_thread::yield();
		}
		const bool reconnect_waited_for_query = !reconnect_finished.load();
		const int releases_before_query_returned = blocking_stub.release_count();
		blocking_stub.unblock_query();
		query_thread.join();
		reconnect_thread.join();

		EXPECT_TRUE(reconfigure_entered);
		EXPECT_TRUE(reconnect_waited_for_query);
		EXPECT_EQ(0, releases_before_query_returned);
		EXPECT_EQ(0, query_result.load());
		EXPECT_TRUE(reconnect_finished.load());
		EXPECT_EQ(1, blocking_stub.release_count());
		EXPECT_EQ(&blocking_stub, api_._api);
		g_r93_xtp_stub = NULL;
	}

	/* ------------------------------------------------------------------ */
	/* 登录失败 / 成功 / 重连                                                */
	/* ------------------------------------------------------------------ */

	/// TraderXTP.cpp L753-831：Login 返回 0 → GetApiLastError → TS_LOGINFAILED →
	/// 异步 onLoginResult(false)；请求参数（host/port/user/pass/TCP）。
	TEST_F(R93XtpTest, login_failure_publishes_error_asynchronously)
	{
		connect_stub();
		stub_.login_ret_ = 0;
		stub_.setError(42, "bad password");

		ASSERT_EQ(0, api_.login("xtpuser", "xtppass", ""));
		EXPECT_EQ(1, stub_.n_login_);
		EXPECT_EQ("127.0.0.1", stub_.login_ip_);
		EXPECT_EQ(11101, stub_.login_port_);
		EXPECT_EQ("xtpuser", stub_.login_user_);
		EXPECT_EQ("xtppass", stub_.login_pass_);
		EXPECT_EQ(XTP_PROTOCOL_TCP, stub_.login_protocol_);
		EXPECT_EQ(12u, stub_.heartbeat_);
		EXPECT_EQ(TraderXTP::TS_LOGINFAILED, api_._state);
		ASSERT_FALSE(sink_.logs.empty());
		EXPECT_NE(std::string::npos, sink_.logs.back().find("bad password"));

		ASSERT_TRUE(sink_.wait_for_login_count(1, std::chrono::seconds(2)));
		const R93XtpSink::LoginRec result = sink_.login_at(0);
		EXPECT_FALSE(result.succ);
		EXPECT_EQ("bad password", result.msg);
		EXPECT_FALSE(api_._inited);
	}

	TEST_F(R93XtpTest, concurrent_public_logins_do_not_race_credentials_or_cache_init)
	{
		R93XtpBlockingLoginStub blocking_stub;
		R93XtpReleaseGuard release_guard(api_);
		g_r93_xtp_stub = &blocking_stub;
		api_.m_funcCreator = &r93_xtp_create;
		api_.connect();

		std::atomic<int> first_result(-99);
		std::thread first_login([&]() {
			first_result.store(api_.login("first-user", "first-pass", ""));
		});
		if (!blocking_stub.wait_for_login(std::chrono::seconds(2)))
		{
			blocking_stub.unblock_login();
			first_login.join();
			g_r93_xtp_stub = NULL;
			FAIL() << "first public Login did not enter the fake SDK";
			return;
		}

		const int second_result = api_.login("second-user", "second-pass", "");
		std::string active_user;
		std::string active_pass;
		{
			std::lock_guard<std::mutex> lock(api_._login_state_mutex);
			active_user = api_._user;
			active_pass = api_._pass;
		}
		const int login_calls_while_blocked = blocking_stub.n_login_;
		blocking_stub.unblock_login();
		first_login.join();

		EXPECT_EQ(-1, second_result);
		EXPECT_EQ("first-user", active_user);
		EXPECT_EQ("first-pass", active_pass);
		EXPECT_EQ(1, login_calls_while_blocked);
		EXPECT_EQ(0, first_result.load());
		{
			std::lock_guard<std::mutex> lock(api_._login_state_mutex);
			EXPECT_EQ(12345u, api_._sessionid);
			EXPECT_EQ(20260105u, api_._tradingday);
			EXPECT_TRUE(api_._inited);
		}
		g_r93_xtp_stub = NULL;
	}

	TEST_F(R93XtpTest, logout_during_login_is_rejected_then_successful_session_can_logout)
	{
		R93XtpBlockingLoginStub blocking_stub;
		R93XtpReleaseGuard release_guard(api_);
		g_r93_xtp_stub = &blocking_stub;
		api_.m_funcCreator = &r93_xtp_create;
		api_.connect();

		std::atomic<int> login_result(-99);
		std::thread login_thread([&]() {
			login_result.store(api_.login("xtpuser", "xtppass", ""));
		});
		if (!blocking_stub.wait_for_login(std::chrono::seconds(2)))
		{
			blocking_stub.unblock_login();
			login_thread.join();
			g_r93_xtp_stub = NULL;
			FAIL() << "Login did not enter the fake SDK";
			return;
		}

		{
			std::lock_guard<std::mutex> stateLock(api_._login_state_mutex);
			EXPECT_EQ(TraderXTP::TS_LOGINING, api_._state);
			EXPECT_EQ(0u, api_._sessionid);
		}
		EXPECT_EQ(-1, api_.logout());
		EXPECT_EQ(0, blocking_stub.n_logout_);

		blocking_stub.unblock_login();
		login_thread.join();
		EXPECT_EQ(0, login_result.load());
		EXPECT_EQ(0, api_.logout());
		EXPECT_EQ(1, blocking_stub.n_logout_);
		EXPECT_EQ(12345u, blocking_stub.logout_session_);
		g_r93_xtp_stub = NULL;
	}

	/// TraderXTP.cpp L753-817：首次成功登录初始化 eid/oid 缓存、异步置 ALLREADY；
	/// 第二次登录走「Connection recovered」分支；logout 传 session_id。
	TEST_F(R93XtpTest, login_success_then_recovery_and_logout)
	{
		connect_stub();
		stub_.login_ret_ = 12345;

		ASSERT_EQ(0, api_.login("xtpuser", "xtppass", ""));
		EXPECT_TRUE(api_._inited);
		EXPECT_EQ(12345u, api_._sessionid);
		EXPECT_EQ(20260105u, api_._tradingday);

		ASSERT_TRUE(sink_.wait_for_login_count(1, std::chrono::seconds(2)));
		EXPECT_EQ(TraderXTP::TS_ALLREADY, api_._state);
		EXPECT_TRUE(api_.isConnected());
		R93XtpSink::LoginRec result = sink_.login_at(0);
		EXPECT_TRUE(result.succ);
		EXPECT_EQ(20260105u, result.date);
		EXPECT_NE(std::string::npos, sink_.logs.back().find("Login succeed"));

		stub_.login_ret_ = 67890;
		ASSERT_EQ(0, api_.login("xtpuser", "xtppass", ""));
		EXPECT_EQ(67890u, api_._sessionid);
		EXPECT_EQ(TraderXTP::TS_ALLREADY, api_._state);
		EXPECT_EQ(1u, sink_.login_count());
		EXPECT_NE(std::string::npos, sink_.logs.back().find("Connection recovered"));

		ASSERT_EQ(0, api_.logout());
		EXPECT_EQ(1, stub_.n_logout_);
		EXPECT_EQ(67890u, stub_.logout_session_);
		// K55（BUG-R60-006，2026-11-06）：isConnected 表示完全就绪，logout 之后为 false
		EXPECT_FALSE(api_.isConnected());
		EXPECT_EQ(TraderXTP::TS_NOTLOGIN, api_._state);
	}

	/* ------------------------------------------------------------------ */
	/* 下单                                                                  */
	/* ------------------------------------------------------------------ */

	/// TraderXTP.cpp L717-732（eid 解析）、L842-877（下单字段：市场映射、方向/开平、
	/// 数量价格、session、失败日志）。
	TEST_F(R93XtpTest, order_insert_fills_xtp_request_and_logs_failure)
	{
		make_ready();

		char eid[64] = { 0 };
		ASSERT_TRUE(api_.makeEntrustID(eid, sizeof(eid)));
		const uint32_t ref = r93_xtp_eid_ref(eid);

		WTSEntrust* ent = WTSEntrust::create("600000", 200, 10.5, "SSE");
		ent->setEntrustID(eid);
		ent->setDirection(WDT_LONG);
		ent->setOffsetType(WOT_OPEN);
		ASSERT_EQ(0, api_.orderInsert(ent));
		EXPECT_EQ(1, stub_.n_insert_);
		EXPECT_EQ(ref, stub_.insert_req_.order_client_id);
		EXPECT_STREQ("600000", stub_.insert_req_.ticker);
		EXPECT_EQ(XTP_MKT_SH_A, stub_.insert_req_.market);
		EXPECT_DOUBLE_EQ(10.5, stub_.insert_req_.price);
		EXPECT_EQ(200, stub_.insert_req_.quantity);
		EXPECT_EQ(XTP_PRICE_LIMIT, stub_.insert_req_.price_type);
		EXPECT_EQ(XTP_SIDE_BUY, stub_.insert_req_.side);
		EXPECT_EQ(XTP_BUSINESS_TYPE_CASH, stub_.insert_req_.business_type);
		EXPECT_EQ(XTP_POSITION_EFFECT_OPEN, stub_.insert_req_.position_effect);
		EXPECT_EQ(5555u, stub_.insert_session_);
		ent->release();

		WTSEntrust* sz = WTSEntrust::create("000001", 300, 9.8, "SZSE");
		sz->setEntrustID(eid);
		sz->setDirection(WDT_SHORT);
		sz->setOffsetType(WOT_CLOSE);
		ASSERT_EQ(0, api_.orderInsert(sz));
		EXPECT_EQ(2, stub_.n_insert_);
		EXPECT_EQ(XTP_MKT_SZ_A, stub_.insert_req_.market);
		EXPECT_EQ(XTP_SIDE_BUY, stub_.insert_req_.side);	// 空平 = 买
		EXPECT_EQ(XTP_POSITION_EFFECT_CLOSE, stub_.insert_req_.position_effect);
		sz->release();

		stub_.insert_ret_ = 0;
		stub_.setError(13, "no counter");
		WTSEntrust* fail = WTSEntrust::create("600000", 100, 10.6, "SSE");
		fail->setEntrustID(eid);
		ASSERT_EQ(0, api_.orderInsert(fail));
		ASSERT_FALSE(sink_.logs.empty());
		EXPECT_NE(std::string::npos, sink_.logs.back().find("no counter"));
		fail->release();
	}

	TEST_F(R93XtpTest, order_insert_encodes_close_yesterday_offset_in_effect_field)
	{
		make_ready();

		char eid[64] = { 0 };
		ASSERT_TRUE(api_.makeEntrustID(eid, sizeof(eid)));
		WTSEntrust* ent = WTSEntrust::create("510050C2601", 1, 0.123, "SSE");
		ent->setEntrustID(eid);
		ent->setDirection(WDT_SHORT);
		ent->setOffsetType(WOT_CLOSEYESTERDAY);

		// Verify only the mapping field; orderInsert currently selects CASH, so
		// this does not establish that the SDK accepts an option order request.
		ASSERT_EQ(0, api_.orderInsert(ent));
		ASSERT_EQ(1, stub_.n_insert_);
		EXPECT_STREQ("510050C2601", stub_.insert_req_.ticker);
		EXPECT_EQ(XTP_POSITION_EFFECT_CLOSEYESTERDAY, stub_.insert_req_.position_effect);
		ent->release();
	}

	/* ------------------------------------------------------------------ */
	/* 撤单 / 查询                                                           */
	/* ------------------------------------------------------------------ */

	/// TraderXTP.cpp L879-951：撤单 order_xtp_id/session；四个查询的 ticker、
	/// session、request_id 与失败日志。
	TEST_F(R93XtpTest, order_action_and_queries_call_stub_with_session)
	{
		make_ready();

		WTSEntrustAction* action = WTSEntrustAction::create("600000", "SSE");
		action->setOrderID("987650");
		ASSERT_EQ(0, api_.orderAction(action));
		EXPECT_EQ(1, stub_.n_cancel_);
		EXPECT_EQ(987650u, stub_.cancel_order_id_);
		EXPECT_EQ(5555u, stub_.cancel_session_);
		action->release();

		ASSERT_EQ(0, api_.queryAccount());
		EXPECT_EQ(1, stub_.n_query_asset_);
		EXPECT_EQ(5555u, stub_.query_session_);
		EXPECT_GT(stub_.query_request_id_, 0);

		ASSERT_EQ(0, api_.queryPositions());
		EXPECT_EQ(1, stub_.n_query_position_);
		EXPECT_EQ("", stub_.position_ticker_);
		EXPECT_EQ(5555u, stub_.query_session_);

		ASSERT_EQ(0, api_.queryOrders());
		EXPECT_EQ(1, stub_.n_query_orders_);
		EXPECT_EQ(5555u, stub_.query_session_);

		ASSERT_EQ(0, api_.queryTrades());
		EXPECT_EQ(1, stub_.n_query_trades_);
		EXPECT_EQ(5555u, stub_.query_session_);

		stub_.query_ret_ = 1;
		stub_.setError(7, "query failed");
		ASSERT_EQ(0, api_.queryAccount());
		EXPECT_NE(std::string::npos, sink_.logs.back().find("Account querying failed"));

		ASSERT_EQ(0, api_.queryPositions());
		EXPECT_NE(std::string::npos, sink_.logs.back().find("Positions querying failed"));

		ASSERT_EQ(0, api_.queryOrders());
		EXPECT_NE(std::string::npos, sink_.logs.back().find("Orders querying failed"));

		ASSERT_EQ(0, api_.queryTrades());
		EXPECT_NE(std::string::npos, sink_.logs.back().find("Trades querying failed"));
	}

	/* ------------------------------------------------------------------ */
	/* eid/oid 缓存回填                                                      */
	/* ------------------------------------------------------------------ */

	/// TraderXTP.cpp L864-867（下单写 eid）、
	/// L279-292（makeOrderInfo 命中 eid → userTag + oidCache.put）、
	/// L338-340（makeTradeInfo 命中 oid → userTag）。
	TEST_F(R93XtpTest, entrust_and_order_cache_backfills_user_tags)
	{
		connect_stub();
		ASSERT_EQ(0, api_.login("xtpuser", "xtppass", ""));
		ASSERT_TRUE(sink_.wait_for_login_count(1, std::chrono::seconds(2)));
		ASSERT_EQ(TraderXTP::TS_ALLREADY, api_._state);

		char eid[64] = { 0 };
		ASSERT_TRUE(api_.makeEntrustID(eid, sizeof(eid)));
		const uint32_t ref = r93_xtp_eid_ref(eid);

		WTSEntrust* ent = WTSEntrust::create("600000", 100, 10.0, "SSE");
		ent->setEntrustID(eid);
		ent->setUserTag("xtp-tag");
		ent->setDirection(WDT_LONG);
		ent->setOffsetType(WOT_OPEN);
		ASSERT_EQ(0, api_.orderInsert(ent));
		ent->release();

		XTPQueryOrderRsp order;
		memset(&order, 0, sizeof(order));
		order.market = XTP_MKT_SH_A;
		strncpy(order.ticker, "600000", sizeof(order.ticker) - 1);
		order.order_client_id = ref;
		order.order_xtp_id = 777001;
		order.quantity = 100;
		order.price = 10.0;
		order.side = XTP_SIDE_BUY;
		order.position_effect = XTP_POSITION_EFFECT_OPEN;
		order.order_status = XTP_ORDER_STATUS_NOTRADEQUEUEING;
		order.insert_time = 20260105093000000ULL;

		api_.OnOrderEvent(&order, NULL, 1);
		ASSERT_EQ(1, sink_.n_push_order_);
		EXPECT_EQ("xtp-tag", sink_.orderUserTag_);
		EXPECT_EQ("777001", sink_.orderID_);
		EXPECT_EQ(eid, sink_.orderEntrustID_);

		XTPTradeReport trade;
		memset(&trade, 0, sizeof(trade));
		trade.market = XTP_MKT_SH_A;
		strncpy(trade.ticker, "600000", sizeof(trade.ticker) - 1);
		strncpy(trade.exec_id, "EXEC-93", sizeof(trade.exec_id) - 1);
		trade.order_xtp_id = 777001;
		trade.quantity = 100;
		trade.price = 10.0;
		trade.side = XTP_SIDE_BUY;
		trade.trade_time = 20260105093100000ULL;

		api_.OnTradeEvent(&trade, 1);
		ASSERT_EQ(1, sink_.n_push_trade_);
		EXPECT_EQ("xtp-tag", sink_.tradeUserTag_);
		EXPECT_EQ("777001", sink_.tradeRefOrder_);
	}

	/// disconnect() 应先清除就绪态；重连完成前保持未连接，登录成功后再恢复就绪态。
	TEST_F(R93XtpTest, disconnect_resets_state_and_reconnect_requires_login)
	{
		connect_stub();
		api_._state = TraderXTP::TS_ALLREADY;
		api_._inited = true;
		api_.disconnect();
		EXPECT_EQ(TraderXTP::TS_NOTLOGIN, api_._state);
		EXPECT_FALSE(api_.isConnected());
		EXPECT_EQ(NULL, api_._api);

		api_.connect();
		ASSERT_EQ(&stub_, api_._api);
		EXPECT_FALSE(api_.isConnected());

		stub_.login_ret_ = 67890;
		ASSERT_EQ(0, api_.login("xtpuser", "xtppass", ""));
		EXPECT_EQ(TraderXTP::TS_ALLREADY, api_._state);
		EXPECT_TRUE(api_.isConnected());
		g_r93_xtp_stub = NULL;
	}
} // namespace
