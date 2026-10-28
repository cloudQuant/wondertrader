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
#include <cstring>
#include <filesystem>
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
		IBaseDataMgr* getBaseDataMgr() override { return &bd_; }
		void handleEvent(WTSTraderEvent e, int32_t ec) override { events.push_back({ (int)e, ec }); }
		void handleTraderLog(WTSLogLevel, const char* message) override
		{
			logs.push_back(message ? message : "");
		}
		void onLoginResult(bool succ, const char* msg, uint32_t date) override
		{
			logins.push_back({ succ, msg ? msg : "", date });
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

		struct LoginRec { bool succ; std::string msg; uint32_t date; };

		R93XtpBdMgr bd_;
		std::vector<std::pair<int, int32_t>> events;
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
			ASSERT_TRUE(api_.init(cfg));
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
			// 产品构造函数漏初始化 _state/_sessionid（BUG-R93-007）；留下确定值，
			// 避免同进程后续用例复用这块内存时读到本用例的 ALLREADY。
			api_._state = TraderXTP::TS_NOTLOGIN;
			api_._sessionid = 0;
			api_._inited = false;
			ct_->release();
			comm_->release();
		}

		void make_ready()
		{
			api_._api = &stub_;
			api_._state = TraderXTP::TS_ALLREADY;
			api_._sessionid = 5555;
			api_._tradingday = 20260105;
			api_._inited = true;
		}

		void preset_worker()
		{
			// 未启动的 std::thread 占位（非 joinable），只让 connect 的判空跳过。
			api_._thrd_worker.reset(new StdThread());
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
	/// L651-686（reconnect：目录、creator、QUICK 订阅、软件版本/Key、心跳、注册 SPI、
	/// WTE_Connect）、L688-710（connect 不重复起线程、disconnect→release）。
	TEST_F(R93XtpTest, connect_reconnect_and_release_use_stub)
	{
		g_r93_xtp_stub = &stub_;
		api_.m_funcCreator = &r93_xtp_create;
		preset_worker();

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
	}

	/* ------------------------------------------------------------------ */
	/* 登录失败 / 成功 / 重连                                                */
	/* ------------------------------------------------------------------ */

	/// TraderXTP.cpp L753-831：Login 返回 0 → GetApiLastError → TS_LOGINFAILED →
	/// 异步 onLoginResult(false)；请求参数（host/port/user/pass/TCP）。
	TEST_F(R93XtpTest, login_failure_publishes_error_asynchronously)
	{
		api_._api = &stub_;
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
		EXPECT_TRUE(sink_.logins.empty());
		ASSERT_FALSE(sink_.logs.empty());
		EXPECT_NE(std::string::npos, sink_.logs.back().find("bad password"));

		api_._asyncio.run();
		ASSERT_EQ(1u, sink_.logins.size());
		EXPECT_FALSE(sink_.logins[0].succ);
		EXPECT_EQ("bad password", sink_.logins[0].msg);
		EXPECT_FALSE(api_._inited);
	}

	/// TraderXTP.cpp L753-817：首次成功登录初始化 eid/oid 缓存、异步置 ALLREADY；
	/// 第二次登录走「Connection recovered」分支；logout 传 session_id。
	TEST_F(R93XtpTest, login_success_then_recovery_and_logout)
	{
		api_._api = &stub_;
		stub_.login_ret_ = 12345;

		ASSERT_EQ(0, api_.login("xtpuser", "xtppass", ""));
		EXPECT_EQ(TraderXTP::TS_LOGINED, api_._state);
		EXPECT_TRUE(api_._inited);
		EXPECT_EQ(12345u, api_._sessionid);
		EXPECT_EQ(20260105u, api_._tradingday);
		EXPECT_TRUE(sink_.logins.empty());

		api_._asyncio.run();
		EXPECT_EQ(TraderXTP::TS_ALLREADY, api_._state);
		EXPECT_TRUE(api_.isConnected());
		ASSERT_EQ(1u, sink_.logins.size());
		EXPECT_TRUE(sink_.logins[0].succ);
		EXPECT_EQ(20260105u, sink_.logins[0].date);
		EXPECT_NE(std::string::npos, sink_.logs.back().find("Login succeed"));

		stub_.login_ret_ = 67890;
		ASSERT_EQ(0, api_.login("xtpuser", "xtppass", ""));
		EXPECT_EQ(67890u, api_._sessionid);
		EXPECT_EQ(TraderXTP::TS_ALLREADY, api_._state);
		EXPECT_EQ(1u, sink_.logins.size());
		EXPECT_NE(std::string::npos, sink_.logs.back().find("Connection recovered"));

		ASSERT_EQ(0, api_.logout());
		EXPECT_EQ(1, stub_.n_logout_);
		EXPECT_EQ(67890u, stub_.logout_session_);
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
		api_._api = &stub_;
		ASSERT_EQ(0, api_.login("xtpuser", "xtppass", ""));
		api_._asyncio.run();
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

	/// 现状缺陷：disconnect()→release() 只清 _api/容器，不重置 _state；
	/// 已就绪连接断开后 isConnected() 仍为 true。修复后应为 false，故整条禁用。
	TEST_F(R93XtpTest, DISABLED_disconnect_should_reset_connected_state)
	{
		api_._api = &stub_;
		api_._state = TraderXTP::TS_ALLREADY;
		api_.disconnect();
		EXPECT_FALSE(api_.isConnected());
	}
} // namespace
