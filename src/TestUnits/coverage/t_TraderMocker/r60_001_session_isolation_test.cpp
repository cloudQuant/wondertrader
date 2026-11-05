/*
 * BUG-R60-001 — TraderMocker 会话隔离（迭代03，卡E工程合同 + 卡9c/9d）
 *
 * 合同（决策积压清单 §3 E 类）：重连使用新会话对象；旧会话的回调在对象存活期内
 * 被隔离，不得落到宿主或复用同一 listener 的新会话。TraderMocker 中“会话对象”
 * 即实例本身：release()/disconnect() 之后实例终止（9c），新会话是新实例。
 *
 *  - async_connect_login_then_immediate_release_*：异步 connect/login 后立即
 *    release，release 返回后旧实例不再产生任何回调；同一 listener 上的新实例
 *    恰好收到自己的连接/登录/查询回报。
 *  - release_from_rsp_entrust_*：io worker 回调内 release（9d 只请求停止），同一
 *    handler 后续的 onPushOrder 与日志不得再送达。
 *  - release_from_matcher_push_*：撮合线程 onPushOrder 回调内 release，同一撮合
 *    轮次的 onPushTrade 与其他订单的成交不得再送达。
 *  - disconnect_ends_session_*：disconnect 与 release 共享清理（关闭端口、拒绝
 *    新请求、实例终止），反复 connect→disconnect→析构 不增长 fd。
 *
 * 只用本机回环 UDP 与进程内 sink，不连接任何真实柜台。
 */
#include "r113_mocker_common.h"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <thread>
#include <vector>

namespace
{
	using r113m::wait_until;

	/* release() 之后到达的任何业务回调都记为 late；日志单独计数。 */
	class IsolationSink : public r113m::Sink
	{
	public:
		TraderMocker* api = nullptr;
		std::atomic<int> mode{ 0 };	// 1: onRspEntrust 内 release；2: 首个已成交 onPushOrder 内 release
		std::atomic<bool> released{ false };
		std::atomic<uint32_t> late{ 0 };
		std::atomic<uint32_t> late_logs{ 0 };

		void handleEvent(WTSTraderEvent e, int32_t ec) override { note(); r113m::Sink::handleEvent(e, ec); }
		void onLoginResult(bool s, const char* m, uint32_t d) override { note(); r113m::Sink::onLoginResult(s, m, d); }
		void onRspAccount(WTSArray* ay) override { note(); r113m::Sink::onRspAccount(ay); }
		void onRspPosition(const WTSArray* ay) override { note(); r113m::Sink::onRspPosition(ay); }
		void onRspOrders(const WTSArray* ay) override { note(); r113m::Sink::onRspOrders(ay); }
		void onRspTrades(const WTSArray* ay) override { note(); r113m::Sink::onRspTrades(ay); }
		void onTraderError(WTSError* err, void* p = NULL) override { note(); r113m::Sink::onTraderError(err, p); }
		void onPushTrade(WTSTradeInfo* t) override { note(); r113m::Sink::onPushTrade(t); }
		void handleTraderLog(WTSLogLevel ll, const char* msg) override
		{
			if (released.load())
				late_logs.fetch_add(1);
			r113m::Sink::handleTraderLog(ll, msg);
		}
		void onRspEntrust(WTSEntrust* e, WTSError* err) override
		{
			note();
			r113m::Sink::onRspEntrust(e, err);
			if (mode.load() == 1 && !released.exchange(true))
				api->release();
		}
		void onPushOrder(WTSOrderInfo* info) override
		{
			note();
			r113m::Sink::onPushOrder(info);
			if (mode.load() == 2 && info != NULL && info->getVolTraded() > 0 && !released.exchange(true))
				api->release();
		}

	private:
		void note()
		{
			if (released.load())
				late.fetch_add(1);
		}
	};

	std::vector<int> open_fds()
	{
		std::vector<int> fds;
		const int limit = getdtablesize();
		for (int fd = 0; fd < limit; ++fd)
		{
			int r;
			do { r = fcntl(fd, F_GETFD); } while (r == -1 && errno == EINTR);
			if (r != -1 || errno != EBADF)
				fds.push_back(fd);
		}
		return fds;
	}

	bool udp_port_bindable(uint16_t port)
	{
		int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
		if (fd < 0)
			return false;
		sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
		addr.sin_port = htons(port);
		const bool ok = ::bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0;
		::close(fd);
		return ok;
	}

	class R60SessionIsolation : public ::testing::Test
	{
	protected:
		void SetUp() override
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
			sink_.bd_.regis(ag_comm_, ag_ct_);
		}

		bool init(TraderMocker& api, uint32_t id, uint16_t port)
		{
			WTSVariant* cfg = r103m::make_cfg(id, port, 1.0, 1.0);
			const bool ok = api.init(cfg);
			cfg->release();
			return ok;
		}

		WTSEntrust* buy(double price, double volume)
		{
			return r103m::make_entrust("ag2512", "SHFE", ag_ct_, price, volume, WDT_LONG, WOT_OPEN);
		}

		bool send_tick(uint16_t port, double price, double qty)
		{
			WTSTickStruct tick{};
			strncpy(tick.exchg, "SHFE", sizeof(tick.exchg) - 1);
			strncpy(tick.code, "ag2512", sizeof(tick.code) - 1);
			tick.price = price;
			tick.action_date = 20200102u;
			tick.trading_date = 20200102u;
			tick.action_time = 100000000u;
			tick.ask_prices[0] = price;
			tick.ask_qty[0] = qty;
			tick.bid_prices[0] = price;
			tick.bid_qty[0] = qty;
			const uint32_t type = 0x200u;
			std::array<unsigned char, sizeof(type) + sizeof(tick)> packet{};
			std::memcpy(packet.data(), &type, sizeof(type));
			std::memcpy(packet.data() + sizeof(type), &tick, sizeof(tick));
			return r103m::send_udp(port, packet.data(), packet.size());
		}

		uint32_t business(const r113m::Sink& s) const
		{
			return s.n_event() + s.n_login() + s.n_entrust() + s.n_push_order() + s.n_trade() +
				s.n_position() + s.n_account() + s.n_orders_query() + s.n_trades_query() + s.n_error();
		}

		r103m::TempCwd cwd_;
		IsolationSink sink_;
		WTSCommodityInfo* ag_comm_ = NULL;
		WTSContractInfo* ag_ct_ = NULL;
	};

	/* 异步 connect/login 后立即 release：release 返回后旧实例零回调；新实例（同一
	 * listener）恰好收到自己的回报。重复多轮以覆盖 handler 已/未执行的两种交错。 */
	TEST_F(R60SessionIsolation, async_connect_login_then_immediate_release_delivers_nothing_afterwards)
	{
		for (uint32_t cycle = 0; cycle < 20; ++cycle)
		{
			uint32_t afterRelease = 0;
			{
				TraderMocker old;
				old.registerSpi(&sink_);
				ASSERT_TRUE(init(old, 9700 + cycle, r103m::pick_free_udp_port())) << "cycle " << cycle;
				old.connect();
				EXPECT_EQ(0, old.login("", "", ""));
				EXPECT_EQ(0, old.queryAccount());
				old.release();
				afterRelease = business(sink_);
				EXPECT_FALSE(old.isConnected()) << "cycle " << cycle;
				EXPECT_FALSE(wait_until([&]() { return business(sink_) != afterRelease; }, 60))
					<< "released instance delivered a callback in cycle " << cycle;
			}
			EXPECT_EQ(afterRelease, business(sink_)) << "destruction delivered a callback in cycle " << cycle;

			const uint32_t events = sink_.n_event();
			const uint32_t logins = sink_.n_login();
			const uint32_t accounts = sink_.n_account();
			TraderMocker fresh;
			fresh.registerSpi(&sink_);
			ASSERT_TRUE(init(fresh, 9750 + cycle, r103m::pick_free_udp_port())) << "cycle " << cycle;
			fresh.connect();
			ASSERT_TRUE(wait_until([&]() { return sink_.n_event() == events + 1; })) << "cycle " << cycle;
			ASSERT_EQ(0, fresh.login("", "", ""));
			ASSERT_TRUE(wait_until([&]() { return sink_.n_login() == logins + 1; })) << "cycle " << cycle;
			ASSERT_EQ(0, fresh.queryAccount());
			ASSERT_TRUE(wait_until([&]() { return sink_.n_account() == accounts + 1; })) << "cycle " << cycle;
			fresh.release();
			EXPECT_EQ(events + 1, sink_.n_event()) << "cycle " << cycle;
			EXPECT_EQ(logins + 1, sink_.n_login()) << "cycle " << cycle;
			EXPECT_EQ(accounts + 1, sink_.n_account()) << "cycle " << cycle;
		}
	}

	/* io worker 回调内 release（9d：只请求停止）：同一 handler 随后的 onPushOrder 和
	 * 日志不得送达；外部 release 完成 join 后也不再有任何回调。 */
	TEST_F(R60SessionIsolation, release_from_rsp_entrust_suppresses_rest_of_handler)
	{
		TraderMocker api;
		sink_.api = &api;
		api.registerSpi(&sink_);
		ASSERT_TRUE(init(api, 9790, r103m::pick_free_udp_port()));
		api.connect();
		ASSERT_TRUE(wait_until([&]() { return sink_.n_event() == 1; }));
		sink_.mode.store(1);
		WTSEntrust* entrust = buy(7000.0, 1.0);
		ASSERT_EQ(0, api.orderInsert(entrust));
		entrust->release();
		ASSERT_TRUE(wait_until([&]() { return sink_.released.load(); }));
		// Give the worker time to run the rest of the handler before checking.
		EXPECT_FALSE(wait_until([&]() { return sink_.late.load() != 0 || sink_.late_logs.load() != 0; }, 200));
		EXPECT_LT(api.queryAccount(), 0) << "admission must be closed by the callback release";
		api.release();
		EXPECT_EQ(1u, sink_.n_entrust());
		EXPECT_EQ(0u, sink_.n_push_order()) << "onPushOrder delivered after release() returned in onRspEntrust";
		EXPECT_EQ(0u, sink_.late.load());
		EXPECT_EQ(0u, sink_.late_logs.load());
	}

	/* 撮合线程 onPushOrder 回调内 release：同一轮次的 onPushTrade 及第二张订单的成交
	 * 不得送达；新实例使用同一 listener 时只收到自己的回报。 */
	TEST_F(R60SessionIsolation, release_from_matcher_push_suppresses_rest_of_pass)
	{
		const uint16_t port = r103m::pick_free_udp_port();
		TraderMocker api;
		sink_.api = &api;
		api.registerSpi(&sink_);
		ASSERT_TRUE(init(api, 9791, port));
		api.connect();
		ASSERT_TRUE(wait_until([&]() { return sink_.n_event() == 1; }));
		ASSERT_EQ(0, api.login("", "", ""));
		ASSERT_TRUE(wait_until([&]() { return sink_.n_login() == 1; }));
		for (int i = 0; i != 2; ++i)
		{
			WTSEntrust* entrust = buy(7000.0, 1.0);
			ASSERT_EQ(0, api.orderInsert(entrust));
			entrust->release();
		}
		ASSERT_TRUE(wait_until([&]() { return sink_.n_push_order() == 2; }));
		// queryOrders runs after both insert handlers on the same io worker.
		ASSERT_EQ(0, api.queryOrders());
		ASSERT_TRUE(wait_until([&]() { return sink_.n_orders_query() == 1; }));

		sink_.mode.store(2);
		bool released = false;
		for (int attempt = 0; attempt < 50 && !released; ++attempt)
		{
			ASSERT_TRUE(send_tick(port, 7000.0, 10.0));
			released = wait_until([&]() { return sink_.released.load(); }, 100);
		}
		ASSERT_TRUE(released) << "no fill reached the matcher";
		EXPECT_FALSE(wait_until([&]() { return sink_.late.load() != 0; }, 200));
		api.release();
		EXPECT_EQ(0u, sink_.late.load()) << "callbacks were delivered after release() returned in onPushOrder";
		EXPECT_EQ(0u, sink_.n_trade()) << "onPushTrade of the same fill was delivered after release";
		EXPECT_EQ(3u, sink_.n_push_order()) << "two queued pushes plus the single fill push";

		// A new session object on the same listener sees only its own reports.
		const uint32_t before = business(sink_);
		TraderMocker fresh;
		fresh.registerSpi(&sink_);
		ASSERT_TRUE(init(fresh, 9792, r103m::pick_free_udp_port()));
		fresh.connect();
		ASSERT_TRUE(wait_until([&]() { return sink_.n_event() == 2; }));
		EXPECT_FALSE(wait_until([&]() { return business(sink_) != before + 1; }, 200));
		fresh.release();
		EXPECT_EQ(before + 1, business(sink_));
	}

	/* disconnect 与 release 共享清理：端口释放、请求拒绝、同实例不可重连；反复
	 * connect→login→disconnect→析构 fd 集合不增长，新实例可正常连接。 */
	TEST_F(R60SessionIsolation, disconnect_ends_session_like_release_and_does_not_grow_fds)
	{
		(void)r103m::pick_free_udp_port();
		std::vector<int> warmed;
		for (uint32_t cycle = 0; cycle < 8; ++cycle)
		{
			const uint16_t port = r103m::pick_free_udp_port();
			{
				TraderMocker api;
				api.registerSpi(&sink_);
				ASSERT_TRUE(init(api, 9800 + cycle, port)) << "cycle " << cycle;
				const uint32_t events = sink_.n_event();
				api.connect();
				ASSERT_TRUE(wait_until([&]() { return sink_.n_event() == events + 1; })) << "cycle " << cycle;
				ASSERT_EQ(0, api.login("", "", ""));
				ASSERT_TRUE(wait_until([&]() { return api.isConnected(); })) << "cycle " << cycle;

				api.disconnect();
				EXPECT_FALSE(api.isConnected()) << "cycle " << cycle;
				EXPECT_TRUE(udp_port_bindable(port)) << "disconnect did not close the UDP socket in cycle " << cycle;
				const uint32_t after = business(sink_);
				EXPECT_LT(api.queryAccount(), 0) << "cycle " << cycle;
				EXPECT_LT(api.queryOrders(), 0) << "cycle " << cycle;
				WTSEntrust* entrust = buy(7000.0, 1.0);
				EXPECT_LT(api.orderInsert(entrust), 0) << "cycle " << cycle;
				EXPECT_EQ(1u, entrust->retainCount()) << "cycle " << cycle;
				entrust->release();
				EXPECT_LT(api.login("", "", ""), 0) << "cycle " << cycle;
				api.connect();
				EXPECT_TRUE(udp_port_bindable(port)) << "disconnected instance re-bound its port in cycle " << cycle;
				EXPECT_FALSE(wait_until([&]() { return business(sink_) != after; }, 60))
					<< "disconnected instance delivered a callback in cycle " << cycle;
				api.disconnect();
				api.release();
			}
			if (cycle == 0)
				warmed = open_fds();
			else
				EXPECT_EQ(warmed, open_fds()) << "cycle " << cycle;
		}

		TraderMocker fresh;
		fresh.registerSpi(&sink_);
		ASSERT_TRUE(init(fresh, 9810, r103m::pick_free_udp_port()));
		const uint32_t events = sink_.n_event();
		fresh.connect();
		EXPECT_TRUE(wait_until([&]() { return sink_.n_event() == events + 1; }));
		fresh.release();
	}
}
