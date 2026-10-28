/*
 * r61_traderctpmini_api_test.cpp — lane R61 补测：TraderCTPMini 的「有 API 指针」半边
 *
 * 既有 lane T3 的用例只覆盖了 m_pUserAPI==NULL 的拒绝分支；本文件用一个
 * CThostFtdcTraderApi 的机械空实现（r61_ctpmini_api_stub.h，49 个纯虚函数逐条实现，
 * 不起 socket/线程/不加载厂商库）注入 TraderCTPMini 的 protected m_pUserAPI，
 * 从而确定性地覆盖：
 *   - init 的配置读取（含 front/broker/user/pass 四键齐全的合法路径）；
 *   - connect/disconnect 的流文件目录、订阅模式、creator 调用与 release 回调；
 *   - login/authenticate/doLogin/logout 的请求组装；
 *   - OnRspUserLogin 成功分支（交易日、eid/oid 缓存落盘、ALLREADY）；
 *   - 下单/撤单请求组装（三种 OrderFlag）与 eid/oid 缓存联动；
 *   - 四个查询入口的请求 lambda；
 *   - 持仓查询的 CM_CoverToday 昨仓/今仓、组合、负仓兜底分支；
 *   - 文件级 strToTime。
 *
 * 厂商 API 的真实发送没有被伪造：stub 只记录本进程内调用，
 * 没有连接、没有网络、没有线程。所有落盘都限制在 mkdtemp 目录并清理。
 */
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "r61_ctpmini_api_stub.h"

#include "TraderCTPMini/TraderCTPMini.h"

#include "WTSVariant.hpp"
#include "WTSDataDef.hpp"
#include "WTSTradeDef.hpp"
#include "WTSContractInfo.hpp"
#include "IBaseDataMgr.h"
#include "TimeUtils.hpp"

USING_NS_WTP;

/* TraderCTPMini.cpp:43-57 的文件级函数（外部链接，跨 TU 可直调） */
uint32_t strToTime(const char* strTime);

namespace
{
	namespace fs = std::filesystem;

	//////////////////////////////////////////////////////////////////////
	// 临时目录（只落在系统临时目录，析构时递归删除）
	//////////////////////////////////////////////////////////////////////
	struct R61MiniTempDir
	{
		std::string path;
		R61MiniTempDir()
		{
			char tmpl[128];
			std::snprintf(tmpl, sizeof(tmpl), "/tmp/wt_r61_ctpmini_XXXXXX");
			const char* made = mkdtemp(tmpl);
			if (made != nullptr)
				path = made;
		}
		~R61MiniTempDir()
		{
			if (!path.empty())
			{
				std::error_code ec;
				fs::remove_all(path, ec);
			}
		}
	};

	//////////////////////////////////////////////////////////////////////
	// 派生探针：只暴露 protected 成员
	//////////////////////////////////////////////////////////////////////
	class R61MiniProbe : public TraderCTPMini
	{
	public:
		using TraderCTPMini::IsErrorRspInfo;

		void harden()
		{
			m_frontID = 0;
			m_sessionID = 0;
			m_orderRef = 0;
			m_lDate = 0;
			m_pUserAPI = NULL;
			m_sink = NULL;
			m_bdMgr = NULL;
			m_mapPosition = NULL;
			m_ayTrades = NULL;
			m_ayOrders = NULL;
			m_ayPosDetail = NULL;
			m_ayFunds = NULL;
			m_wrapperState = WS_NOTLOGIN;
			m_bInQuery = false;
			m_bQuickStart = false;
		}

		void setApi(CThostFtdcTraderApi* api) { m_pUserAPI = api; }
		CThostFtdcTraderApi* api() const { return m_pUserAPI; }
		void setState(WrapperState s) { m_wrapperState = s; }
		WrapperState state() const { return m_wrapperState; }
		void setIds(uint32_t front, int32_t session, uint32_t ref)
		{
			m_frontID = front;
			m_sessionID = session;
			m_orderRef.store(ref);
		}
		uint32_t orderRef() const { return m_orderRef.load(); }
		uint32_t tradeDate() const { return m_lDate; }
		void setBrokerUser(const char* broker, const char* user) { m_strBroker = broker; m_strUser = user; }
		void setPass(const char* pass) { m_strPass = pass; }
		void setFront(const char* front) { m_strFront = front; }
		void setAuth(const char* appid, const char* authcode)
		{
			m_strAppID = appid;
			m_strAuthCode = authcode;
		}
		void setFlowDir(const std::string& dir) { m_strFlowDir = dir; }
		const std::string& flowDir() const { return m_strFlowDir; }
		const std::string& front() const { return m_strFront; }
		const std::string& broker() const { return m_strBroker; }
		const std::string& user() const { return m_strUser; }
		const std::string& pass() const { return m_strPass; }
		const std::string& appid() const { return m_strAppID; }
		const std::string& authcode() const { return m_strAuthCode; }
		const std::string& module() const { return m_strModule; }
		bool quickStart() const { return m_bQuickStart; }
		void setQuick(bool quick) { m_bQuickStart = quick; }
		void setCreator(CTPCreator creator) { m_funcCreator = creator; }
		void setDummyWorker() { m_thrdWorker.reset(new StdThread([]() {})); }
		bool workerNull() const { return m_thrdWorker == NULL; }

		bool initCaches(const std::string& dir)
		{
			std::error_code ec;
			fs::create_directories(dir, ec);
			const std::string eidPath = dir + "/eid.sc";
			const std::string oidPath = dir + "/oid.sc";
			const bool eidOk = m_eidCache.init(eidPath.c_str(), m_lDate, nullptr);
			const bool oidOk = m_oidCache.init(oidPath.c_str(), m_lDate, nullptr);
			return eidOk && oidOk;
		}
		std::string eidGet(const char* key) { return m_eidCache.get(key); }
		std::string oidGet(const char* key) { return m_oidCache.get(key); }

		size_t queueSize() const { return m_queQuery.size(); }
		void drainQueue()
		{
			while (!m_queQuery.empty())
			{
				CommonExecuter handler = m_queQuery.front();
				handler();
				m_queQuery.pop();
			}
		}
	};

	//////////////////////////////////////////////////////////////////////
	// 假 bdMgr：按代码查找合约
	//////////////////////////////////////////////////////////////////////
	class R61MiniDataMgr : public IBaseDataMgr
	{
	public:
		std::map<std::string, WTSContractInfo*> contracts;
		void add(WTSContractInfo* ct) { contracts[ct->getCode()] = ct; }

		WTSCommodityInfo* getCommodity(const char*) override { return nullptr; }
		WTSCommodityInfo* getCommodity(const char*, const char*) override { return nullptr; }
		WTSContractInfo* getContract(const char* code, const char* = "", uint32_t = 0) override
		{
			if (code == nullptr)
				return nullptr;
			auto it = contracts.find(code);
			return it == contracts.end() ? nullptr : it->second;
		}
		WTSArray* getContracts(const char* = "", uint32_t = 0) override { return nullptr; }
		WTSSessionInfo* getSession(const char*) override { return nullptr; }
		WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return nullptr; }
		WTSArray* getAllSessions() override { return nullptr; }
		bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
		uint32_t calcTradingDate(const char*, uint32_t, uint32_t, bool = false) override { return 0; }
		uint64_t getBoundaryTime(const char*, uint32_t, bool = false, bool = true) override { return 0; }
	};

	//////////////////////////////////////////////////////////////////////
	// 假 sink：记录回调参数
	//////////////////////////////////////////////////////////////////////
	class R61MiniSink : public ITraderSpi
	{
	public:
		struct PosCap
		{
			std::string code;
			double total = 0, newPos = 0, prePos = 0, availNew = 0, availPre = 0, avg = 0, margin = 0;
			int direction = -1;
		};

		IBaseDataMgr* mgr = nullptr;
		std::vector<std::pair<WTSTraderEvent, int32_t>> events;
		std::vector<std::tuple<bool, std::string, uint32_t>> logins;
		std::vector<std::pair<WTSLogLevel, std::string>> logs;
		std::vector<PosCap> positions;
		uint32_t entrustRpss = 0, accountCalls = 0, orderCalls = 0, tradeCalls = 0;
		uint32_t lastAccountSize = 0, lastOrderSize = 0, lastTradeSize = 0;
		bool lastOrdersNull = false;
		std::string entrustUserTag, pushOrderUserTag, pushTradeUserTag, pushOrderID;

		IBaseDataMgr* getBaseDataMgr() override { return mgr; }
		void handleEvent(WTSTraderEvent e, int32_t ec) override { events.emplace_back(e, ec); }
		void onLoginResult(bool ok, const char* msg, uint32_t date) override
		{
			logins.emplace_back(ok, msg == nullptr ? "" : msg, date);
		}
		void handleTraderLog(WTSLogLevel ll, const char* message) override
		{
			logs.emplace_back(ll, message == nullptr ? "" : message);
		}
		void onRspEntrust(WTSEntrust* entrust, WTSError*) override
		{
			++entrustRpss;
			if (entrust != nullptr)
				entrustUserTag = entrust->getUserTag();
		}
		void onRspAccount(WTSArray* ay) override
		{
			++accountCalls;
			lastAccountSize = ay == nullptr ? 0 : (uint32_t)ay->size();
		}
		void onRspOrders(const WTSArray* ay) override
		{
			++orderCalls;
			lastOrdersNull = ay == nullptr;
			lastOrderSize = ay == nullptr ? 0 : (uint32_t)ay->size();
		}
		void onRspTrades(const WTSArray* ay) override
		{
			++tradeCalls;
			lastTradeSize = ay == nullptr ? 0 : (uint32_t)ay->size();
		}
		void onRspPosition(const WTSArray* ay) override
		{
			if (ay == nullptr)
				return;
			for (uint32_t i = 0; i < ay->size(); i++)
			{
				WTSPositionItem* item = (WTSPositionItem*)((WTSArray*)ay)->at(i);
				PosCap c;
				c.code = item->getCode();
				c.total = item->getTotalPosition();
				c.newPos = item->getNewPosition();
				c.prePos = item->getPrePosition();
				c.availNew = item->getAvailNewPos();
				c.availPre = item->getAvailPrePos();
				c.avg = item->getAvgPrice();
				c.margin = item->getMargin();
				c.direction = item->getDirection();
				positions.push_back(c);
			}
		}
		void onPushOrder(WTSOrderInfo* o) override
		{
			if (o == nullptr)
				return;
			pushOrderUserTag = o->getUserTag();
			pushOrderID = o->getOrderID();
		}
		void onPushTrade(WTSTradeInfo* t) override
		{
			if (t == nullptr)
				return;
			pushTradeUserTag = t->getUserTag();
		}

		bool hasLog(const char* needle) const
		{
			for (const auto& lg : logs)
			{
				if (lg.second.find(needle) != std::string::npos)
					return true;
			}
			return false;
		}
	};

	//////////////////////////////////////////////////////////////////////
	// 合约夹具
	//////////////////////////////////////////////////////////////////////
	struct R61MiniContract
	{
		WTSCommodityInfo* comm = nullptr;
		WTSContractInfo* ct = nullptr;

		R61MiniContract(const char* pid, const char* code, const char* exchg,
			CoverMode cover = CM_OpenCover, ContractCategory cat = CC_Future, double scale = 10)
		{
			comm = WTSCommodityInfo::create(pid, pid, exchg, "R61", "trading");
			comm->setVolScale(scale);
			comm->setCoverMode(cover);
			comm->setCategory(cat);
			ct = WTSContractInfo::create(code, code, exchg, pid);
			ct->setCommInfo(comm);
			ct->setVolScale(scale);
		}
		~R61MiniContract()
		{
			if (ct != nullptr)
				ct->release();
			if (comm != nullptr)
				comm->release();
		}
	};

	//////////////////////////////////////////////////////////////////////
	// 可记录请求的 API stub
	//////////////////////////////////////////////////////////////////////
	class R61MiniApi : public R61CtpMiniApiStub
	{
	public:
		uint32_t release_calls = 0, init_calls = 0, spi_regs = 0, front_regs = 0;
		THOST_TE_RESUME_TYPE pub_topic = THOST_TERT_RESTART, priv_topic = THOST_TERT_RESTART;

		uint32_t auth_calls = 0, login_calls = 0, logout_calls = 0;
		std::string auth_broker, auth_user, auth_appid, auth_code;
		std::string login_broker, login_user, login_pass, login_product;
		std::string logout_broker, logout_user;

		uint32_t order_calls = 0, action_calls = 0;
		CThostFtdcInputOrderField order{};
		CThostFtdcInputOrderActionField action{};

		uint32_t qry_account = 0, qry_position = 0, qry_order = 0, qry_trade = 0;

		void Release() override { ++n_calls_; ++release_calls; }
		void Init(bool) override { ++n_calls_; ++init_calls; }
		void RegisterSpi(CThostFtdcTraderSpi*) override { ++n_calls_; ++spi_regs; }
		void RegisterFront(char*) override { ++n_calls_; ++front_regs; }
		void SubscribePublicTopic(THOST_TE_RESUME_TYPE t) override { ++n_calls_; pub_topic = t; }
		void SubscribePrivateTopic(THOST_TE_RESUME_TYPE t) override { ++n_calls_; priv_topic = t; }

		int ReqAuthenticate(CThostFtdcReqAuthenticateField* p, int) override
		{
			++n_calls_; ++auth_calls;
			auth_broker = p->BrokerID; auth_user = p->UserID;
			auth_appid = p->AppID; auth_code = p->AuthCode;
			return stub_ret_;
		}
		int ReqUserLogin(CThostFtdcReqUserLoginField* p, int) override
		{
			++n_calls_; ++login_calls;
			login_broker = p->BrokerID; login_user = p->UserID;
			login_pass = p->Password; login_product = p->UserProductInfo;
			return stub_ret_;
		}
		int ReqUserLogout(CThostFtdcUserLogoutField* p, int) override
		{
			++n_calls_; ++logout_calls;
			logout_broker = p->BrokerID; logout_user = p->UserID;
			return stub_ret_;
		}
		int ReqOrderInsert(CThostFtdcInputOrderField* p, int) override
		{
			++n_calls_; ++order_calls; order = *p; return stub_ret_;
		}
		int ReqOrderAction(CThostFtdcInputOrderActionField* p, int) override
		{
			++n_calls_; ++action_calls; action = *p; return stub_ret_;
		}
		int ReqQryTradingAccount(CThostFtdcQryTradingAccountField*, int) override
		{
			++n_calls_; ++qry_account; return stub_ret_;
		}
		int ReqQryInvestorPosition(CThostFtdcQryInvestorPositionField*, int) override
		{
			++n_calls_; ++qry_position; return stub_ret_;
		}
		int ReqQryOrder(CThostFtdcQryOrderField*, int) override
		{
			++n_calls_; ++qry_order; return stub_ret_;
		}
		int ReqQryTrade(CThostFtdcQryTradeField*, int) override
		{
			++n_calls_; ++qry_trade; return stub_ret_;
		}
	};

	R61MiniApi* g_created_mini_api = nullptr;
	CThostFtdcTraderApi* r61_mini_creator(const char*)
	{
		return g_created_mini_api;
	}

	const R61MiniSink::PosCap* findPos(const std::vector<R61MiniSink::PosCap>& v, const char* code, int direction)
	{
		for (const auto& p : v)
		{
			if (p.code == code && p.direction == direction)
				return &p;
		}
		return nullptr;
	}

	CThostFtdcRspInfoField r61_mini_ok()
	{
		CThostFtdcRspInfoField f;
		std::memset(&f, 0, sizeof(f));
		return f;
	}
}

/* TraderCTPMini.cpp:43-57 —— 冒号剔除 + to_uint32 */
TEST(R61_TraderCTPMini, str_to_time_strips_colons)
{
	EXPECT_EQ(93001u, strToTime("09:30:01"));
	EXPECT_EQ(2100u, strToTime("21:00"));
	EXPECT_EQ(0u, strToTime(""));
	EXPECT_EQ(123u, strToTime("1:2:3"));
}

/* TraderCTPMini.cpp:100-137 init 的默认值与自定义配置 */
TEST(R61_TraderCTPMini, init_reads_config_and_applies_defaults)
{
	R61MiniProbe probe;
	probe.harden();

	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("front", "tcp://1.2.3.4:1234");
	cfg->append("broker", "9999");
	cfg->append("user", "tester");
	cfg->append("pass", "secret");
	EXPECT_TRUE(probe.init(cfg));
	EXPECT_EQ("tcp://1.2.3.4:1234", probe.front());
	EXPECT_EQ("9999", probe.broker());
	EXPECT_EQ("tester", probe.user());
	EXPECT_EQ("secret", probe.pass());
	EXPECT_NE(std::string::npos, probe.flowDir().find("CTPMiniTDFlow"));
	EXPECT_NE(std::string::npos, probe.module().find("thosttraderapi"));
	EXPECT_FALSE(probe.quickStart());
	cfg->release();

	R61MiniProbe custom;
	custom.harden();
	WTSVariant* cfg2 = WTSVariant::createObject();
	cfg2->append("front", "tcp://5.6.7.8:1234");
	cfg2->append("broker", "8888");
	cfg2->append("user", "u2");
	cfg2->append("pass", "p2");
	cfg2->append("appid", "app2");
	cfg2->append("authcode", "auth2");
	cfg2->append("flowdir", "r61miniflow");
	cfg2->append("ctpmodule", "r61customapi");
	cfg2->append("quick", true);
	EXPECT_TRUE(custom.init(cfg2));
	EXPECT_NE(std::string::npos, custom.flowDir().find("r61miniflow"));
	EXPECT_NE(std::string::npos, custom.module().find("r61customapi"));
	EXPECT_EQ("app2", custom.appid());
	EXPECT_EQ("auth2", custom.authcode());
	EXPECT_TRUE(custom.quickStart());
	cfg2->release();
}

/* TraderCTPMini.cpp:164-221 connect；223-234 disconnect；139-162 release */
TEST(R61_TraderCTPMini, connect_subscribes_topics_and_disconnect_releases_api)
{
	R61MiniTempDir tmp;
	ASSERT_FALSE(tmp.path.empty());

	R61MiniProbe probe;
	probe.harden();
	probe.setFlowDir(tmp.path + "/");
	probe.setBrokerUser("9999", "tester");
	probe.setFront("tcp://127.0.0.1:1");

	R61MiniApi api;
	g_created_mini_api = &api;
	probe.setCreator(&r61_mini_creator);
	probe.setDummyWorker();		/* 阻止 connect 拉起常驻查询线程 */

	probe.connect();
	EXPECT_EQ(&api, probe.api());
	EXPECT_EQ(1u, api.spi_regs);
	EXPECT_EQ(1u, api.front_regs);
	EXPECT_EQ(1u, api.init_calls);
	EXPECT_EQ((int)THOST_TERT_RESUME, (int)api.pub_topic);
	EXPECT_EQ((int)THOST_TERT_RESUME, (int)api.priv_topic);
	EXPECT_TRUE(fs::exists(tmp.path + "/flows/9999/tester"));

	probe.disconnect();
	EXPECT_TRUE(probe.workerNull());
	ASSERT_EQ(1u, probe.queueSize());
	probe.drainQueue();
	EXPECT_EQ(nullptr, probe.api());
	EXPECT_EQ(1u, api.release_calls);

	/* quick 分支 */
	R61MiniApi api2;
	g_created_mini_api = &api2;
	probe.setQuick(true);
	probe.setDummyWorker();
	probe.connect();
	EXPECT_EQ((int)THOST_TERT_QUICK, (int)api2.pub_topic);
	EXPECT_EQ((int)THOST_TERT_QUICK, (int)api2.priv_topic);
	probe.disconnect();
	probe.drainQueue();
	EXPECT_EQ(1u, api2.release_calls);
	g_created_mini_api = nullptr;
}

/* TraderCTPMini.cpp:270-285 login；304-322 logout；287-302 doLogin；
 * 572-587 OnRspAuthenticate 成功/失败；1333-1345 authenticate */
TEST(R61_TraderCTPMini, login_authenticate_and_logout_assemble_requests)
{
	R61MiniApi api;
	R61MiniSink sink;
	R61MiniProbe probe;
	probe.harden();
	probe.setApi(&api);
	probe.setBrokerUser("9999", "tester");
	probe.setAuth("appX", "authY");
	probe.registerSpi(&sink);

	EXPECT_EQ(0, probe.login("alice", "secret", "prod"));
	EXPECT_EQ(TraderCTPMini::WS_LOGINING, probe.state());
	ASSERT_EQ(1u, api.auth_calls);
	EXPECT_EQ("9999", api.auth_broker);
	EXPECT_EQ("alice", api.auth_user);
	EXPECT_EQ("appX", api.auth_appid);
	EXPECT_EQ("authY", api.auth_code);

	EXPECT_EQ(0, probe.logout());
	ASSERT_EQ(1u, api.logout_calls);
	EXPECT_EQ("9999", api.logout_broker);
	EXPECT_EQ("alice", api.logout_user);

	api.stub_ret_ = -3;
	EXPECT_EQ(0, probe.logout());
	EXPECT_TRUE(sink.hasLog("Sending logout request failed"));

	/* 认证成功 → doLogin；stub 返回 -4 覆盖发送失败日志 */
	api.stub_ret_ = -4;
	probe.OnRspAuthenticate(nullptr, nullptr, 1, true);
	ASSERT_EQ(1u, api.login_calls);
	EXPECT_EQ("9999", api.login_broker);
	EXPECT_EQ("alice", api.login_user);
	EXPECT_EQ("secret", api.login_pass);
	EXPECT_EQ("prod", api.login_product);
	EXPECT_TRUE(sink.hasLog("Sending login request failed"));
}

/* TraderCTPMini.cpp:589-646 OnRspUserLogin 成功 + eid/oid 缓存；
 * 324-419 orderInsert；935-959 OnRtnOrder/OnRtnTrade 的 userTag 回填 */
TEST(R61_TraderCTPMini, user_login_success_initializes_caches_and_usertags)
{
	R61MiniTempDir tmp;
	ASSERT_FALSE(tmp.path.empty());

	R61MiniContract fx("ru", "ru2601", "SHFE", CM_OpenCover, CC_Future, 10);
	R61MiniDataMgr mgr;
	mgr.add(fx.ct);
	R61MiniApi api;
	api.trading_day_ = "20260401";
	R61MiniSink sink;
	sink.mgr = &mgr;
	R61MiniProbe probe;
	probe.harden();
	probe.setApi(&api);
	probe.setFlowDir(tmp.path + "/");
	probe.setBrokerUser("9999", "tester");
	probe.registerSpi(&sink);

	CThostFtdcRspUserLoginField lf;
	std::memset(&lf, 0, sizeof(lf));
	lf.FrontID = 3;
	lf.SessionID = 4;
	std::snprintf(lf.MaxOrderRef, sizeof(lf.MaxOrderRef), "%s", "8");
	std::snprintf(lf.LoginTime, sizeof(lf.LoginTime), "%s", "09:00:00");

	probe.OnRspUserLogin(&lf, nullptr, 1, true);
	EXPECT_EQ(TraderCTPMini::WS_ALLREADY, probe.state());
	EXPECT_TRUE(probe.isConnected());
	EXPECT_EQ(20260401u, probe.tradeDate());
	EXPECT_EQ(8u, probe.orderRef());
	ASSERT_EQ(1u, sink.logins.size());
	EXPECT_TRUE(std::get<0>(sink.logins[0]));
	EXPECT_EQ(20260401u, std::get<2>(sink.logins[0]));
	EXPECT_TRUE(fs::exists(tmp.path + "/local/9999/tester_eid.sc"));
	EXPECT_TRUE(fs::exists(tmp.path + "/local/9999/tester_oid.sc"));

	WTSEntrust* ent = WTSEntrust::create("ru2601", 2, 15000.0, "SHFE");
	ent->setContractInfo(fx.ct);
	ent->setDirection(WDT_LONG);
	ent->setOffsetType(WOT_OPEN);
	ent->setPriceType(WPT_LIMITPRICE);
	ent->setOrderFlag(WOF_NOR);
	ent->setUserTag("tag1");
	ent->setEntrustID("000003#0000000004#000009");

	EXPECT_EQ(0, probe.orderInsert(ent));
	ASSERT_EQ(1u, api.order_calls);
	EXPECT_EQ("9", std::string(api.order.OrderRef));
	EXPECT_EQ("tag1", probe.eidGet("000003#0000000004#000009"));

	/* 委托回报 / 柜台拒单回报：makeEntrust 从 eidCache 回填 userTag */
	CThostFtdcInputOrderField io;
	std::memset(&io, 0, sizeof(io));
	std::snprintf(io.InstrumentID, sizeof(io.InstrumentID), "%s", "ru2601");
	std::snprintf(io.ExchangeID, sizeof(io.ExchangeID), "%s", "SHFE");
	std::snprintf(io.OrderRef, sizeof(io.OrderRef), "%s", "9");
	io.VolumeTotalOriginal = 2;
	io.LimitPrice = 15000.0;
	io.Direction = THOST_FTDC_D_Buy;
	io.CombOffsetFlag[0] = THOST_FTDC_OF_Open;
	io.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
	io.TimeCondition = THOST_FTDC_TC_GFD;
	CThostFtdcRspInfoField okRsp = r61_mini_ok();
	probe.OnRspOrderInsert(&io, &okRsp, 10, true);
	ASSERT_EQ(1u, sink.entrustRpss);
	EXPECT_EQ("tag1", sink.entrustUserTag);

	CThostFtdcRspInfoField err = r61_mini_ok();
	err.ErrorID = 44;
	std::snprintf(err.ErrorMsg, sizeof(err.ErrorMsg), "%s", "risk denied");
	probe.OnErrRtnOrderInsert(&io, &err);
	ASSERT_EQ(2u, sink.entrustRpss);
	EXPECT_EQ("tag1", sink.entrustUserTag);

	/* 回报同一 eid → eidCache 命中并写入 oidCache */
	CThostFtdcOrderField of;
	std::memset(&of, 0, sizeof(of));
	std::snprintf(of.InstrumentID, sizeof(of.InstrumentID), "%s", "ru2601");
	std::snprintf(of.ExchangeID, sizeof(of.ExchangeID), "%s", "SHFE");
	std::snprintf(of.OrderRef, sizeof(of.OrderRef), "%s", "9");
	std::snprintf(of.OrderSysID, sizeof(of.OrderSysID), "%s", "SYS9");
	std::snprintf(of.InsertTime, sizeof(of.InsertTime), "%s", "10:00:00");
	of.FrontID = 3;
	of.SessionID = 4;
	of.LimitPrice = 15000.0;
	of.VolumeTotalOriginal = 2;
	of.Direction = THOST_FTDC_D_Buy;
	of.CombOffsetFlag[0] = THOST_FTDC_OF_Open;
	of.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
	of.TimeCondition = THOST_FTDC_TC_GFD;
	of.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;

	probe.OnRtnOrder(&of);
	EXPECT_EQ("tag1", sink.pushOrderUserTag);
	EXPECT_EQ("SYS9", sink.pushOrderID);
	EXPECT_EQ("tag1", probe.oidGet("SYS9"));

	/* 成交按 OrderSysID 从 oidCache 回填 userTag */
	CThostFtdcTradeField tf;
	std::memset(&tf, 0, sizeof(tf));
	std::snprintf(tf.InstrumentID, sizeof(tf.InstrumentID), "%s", "ru2601");
	std::snprintf(tf.ExchangeID, sizeof(tf.ExchangeID), "%s", "SHFE");
	std::snprintf(tf.OrderSysID, sizeof(tf.OrderSysID), "%s", "SYS9");
	std::snprintf(tf.TradeID, sizeof(tf.TradeID), "%s", "T9");
	std::snprintf(tf.TradeTime, sizeof(tf.TradeTime), "%s", "10:00:01");
	tf.Volume = 2;
	tf.Price = 15000.0;
	tf.Direction = THOST_FTDC_D_Buy;
	tf.OffsetFlag = THOST_FTDC_OF_Open;
	probe.OnRtnTrade(&tf);
	EXPECT_EQ("tag1", sink.pushTradeUserTag);

	ent->release();
}

/* TraderCTPMini.cpp:324-419 orderInsert 三种 OrderFlag；421-458 orderAction；
 * 460-551 四个查询入口的 lambda */
TEST(R61_TraderCTPMini, order_flags_action_and_queries_are_driven_by_stub)
{
	R61MiniTempDir tmp;
	ASSERT_FALSE(tmp.path.empty());

	R61MiniContract fx("ru", "ru2601", "SHFE", CM_OpenCover, CC_Future, 10);
	R61MiniDataMgr mgr;
	mgr.add(fx.ct);
	R61MiniApi api;
	R61MiniSink sink;
	sink.mgr = &mgr;
	R61MiniProbe probe;
	probe.harden();
	probe.setApi(&api);
	probe.setState(TraderCTPMini::WS_ALLREADY);
	probe.setBrokerUser("9999", "tester");
	probe.setIds(1, 2, 0);
	ASSERT_TRUE(probe.initCaches(tmp.path + "/cache"));
	probe.registerSpi(&sink);

	WTSEntrust* ent = WTSEntrust::create("ru2601", 3, 15000.0, "SHFE");
	ent->setContractInfo(fx.ct);
	ent->setDirection(WDT_SHORT);
	ent->setOffsetType(WOT_CLOSETODAY);
	ent->setPriceType(WPT_LIMITPRICE);

	ent->setOrderFlag(WOF_NOR);
	EXPECT_EQ(0, probe.orderInsert(ent));
	ASSERT_EQ(1u, api.order_calls);
	EXPECT_EQ((int)THOST_FTDC_TC_GFD, (int)api.order.TimeCondition);
	EXPECT_EQ((int)THOST_FTDC_VC_AV, (int)api.order.VolumeCondition);
	EXPECT_EQ((int)THOST_FTDC_D_Buy, (int)api.order.Direction);
	EXPECT_EQ((int)THOST_FTDC_OF_CloseToday, (int)api.order.CombOffsetFlag[0]);
	EXPECT_EQ(3, api.order.VolumeTotalOriginal);

	ent->setOrderFlag(WOF_FAK);
	EXPECT_EQ(0, probe.orderInsert(ent));
	ASSERT_EQ(2u, api.order_calls);
	EXPECT_EQ((int)THOST_FTDC_TC_IOC, (int)api.order.TimeCondition);
	EXPECT_EQ((int)THOST_FTDC_VC_AV, (int)api.order.VolumeCondition);
	/* 无 userTag 的第二单仍复用同一 OrderRef（BUG-R61-005） */
	EXPECT_EQ("0", std::string(api.order.OrderRef));

	ent->setOrderFlag(WOF_FOK);
	EXPECT_EQ(0, probe.orderInsert(ent));
	ASSERT_EQ(3u, api.order_calls);
	EXPECT_EQ((int)THOST_FTDC_TC_IOC, (int)api.order.TimeCondition);
	EXPECT_EQ((int)THOST_FTDC_VC_CV, (int)api.order.VolumeCondition);
	EXPECT_EQ("0", std::string(api.order.OrderRef));

	/* 带 userTag：OrderRef 取 eid 第三段，并写入 eidCache */
	ent->setUserTag("tagF");
	ent->setEntrustID("000001#0000000002#000077");
	EXPECT_EQ(0, probe.orderInsert(ent));
	ASSERT_EQ(4u, api.order_calls);
	EXPECT_EQ("77", std::string(api.order.OrderRef));
	EXPECT_EQ("tagF", probe.eidGet("000001#0000000002#000077"));

	api.stub_ret_ = -9;
	EXPECT_EQ(0, probe.orderInsert(ent));
	EXPECT_TRUE(sink.hasLog("Order inserting failed"));

	/* 撤单 */
	api.stub_ret_ = 0;
	WTSEntrustAction* act = WTSEntrustAction::create("ru2601", "SHFE");
	act->setEntrustID("000001#0000000002#000077");
	act->setActionFlag(WAF_CANCEL);
	act->setOrderID("SYS77");
	EXPECT_EQ(0, probe.orderAction(act));
	ASSERT_EQ(1u, api.action_calls);
	EXPECT_EQ(1u, api.action.FrontID);
	EXPECT_EQ(2, api.action.SessionID);
	EXPECT_EQ("77", std::string(api.action.OrderRef));
	EXPECT_EQ((int)THOST_FTDC_AF_Delete, (int)api.action.ActionFlag);
	EXPECT_EQ("ru2601", std::string(api.action.InstrumentID));
	EXPECT_EQ("SYS77", std::string(api.action.OrderSysID));
	EXPECT_EQ("SHFE", std::string(api.action.ExchangeID));

	api.stub_ret_ = -5;
	EXPECT_EQ(0, probe.orderAction(act));
	EXPECT_TRUE(sink.hasLog("Sending cancel request failed"));

	act->setEntrustID("malformed");
	EXPECT_EQ(-1, probe.orderAction(act));

	/* 四个查询入口只入队，不启动线程；手工 drain 执行请求 lambda */
	EXPECT_EQ(0, probe.queryAccount());
	EXPECT_EQ(0, probe.queryPositions());
	EXPECT_EQ(0, probe.queryOrders());
	EXPECT_EQ(0, probe.queryTrades());
	ASSERT_EQ(4u, probe.queueSize());
	probe.drainQueue();
	EXPECT_EQ(1u, api.qry_account);
	EXPECT_EQ(1u, api.qry_position);
	EXPECT_EQ(1u, api.qry_order);
	EXPECT_EQ(1u, api.qry_trade);

	probe.release();
	EXPECT_EQ(1u, api.release_calls);

	ent->release();
	act->release();
}

/* TraderCTPMini.cpp:722-868 —— CoverToday 昨仓/今仓、零仓、组合与负仓兜底 */
TEST(R61_TraderCTPMini, position_query_covers_cover_today_combo_and_negative)
{
	R61MiniContract fx("ru", "ru2601", "SHFE", CM_CoverToday, CC_Future, 10);
	R61MiniContract fxCombo("sp", "sp2601", "SHFE", CM_CoverToday, CC_Combination, 1);
	R61MiniContract fxNeg("neg", "neg2601", "SHFE", CM_OpenCover, CC_Future, 1);

	R61MiniDataMgr mgr;
	mgr.add(fx.ct);
	mgr.add(fxCombo.ct);
	mgr.add(fxNeg.ct);
	R61MiniSink sink;
	sink.mgr = &mgr;

	R61MiniProbe probe;
	probe.harden();
	probe.registerSpi(&sink);

	CThostFtdcInvestorPositionField p;
	std::memset(&p, 0, sizeof(p));

	/* CoverToday + 昨仓 + 多头：availPre = Position - ShortFrozen */
	std::snprintf(p.InstrumentID, sizeof(p.InstrumentID), "%s", "ru2601");
	p.PosiDirection = THOST_FTDC_PD_Long;
	p.PositionDate = THOST_FTDC_PSD_History;
	p.Position = 7;
	p.TodayPosition = 2;
	p.LongFrozen = 1;
	p.ShortFrozen = 2;
	p.UseMargin = 300.0;
	p.PositionProfit = 30.0;
	p.PositionCost = 7000.0;
	probe.OnRspQryInvestorPosition(&p, nullptr, 1, false);

	/* 同合约空头 + 今仓：availNew 扣 LongFrozen 后负值归零 */
	p.PosiDirection = THOST_FTDC_PD_Short;
	p.PositionDate = THOST_FTDC_PSD_Today;
	p.Position = 3;
	p.TodayPosition = 3;
	p.LongFrozen = 5;
	p.ShortFrozen = 0;
	p.UseMargin = 100.0;
	p.PositionCost = 3000.0;
	probe.OnRspQryInvestorPosition(&p, nullptr, 2, false);

	/* 零仓：avgPrice 走 0 分支 */
	p.Position = 0;
	p.TodayPosition = 0;
	p.LongFrozen = 0;
	p.UseMargin = 0.0;
	p.PositionCost = 0.0;
	probe.OnRspQryInvestorPosition(&p, nullptr, 3, false);

	/* 组合合约：跳过可用量计算 */
	std::snprintf(p.InstrumentID, sizeof(p.InstrumentID), "%s", "sp2601");
	p.PosiDirection = THOST_FTDC_PD_Long;
	p.PositionDate = THOST_FTDC_PSD_Today;
	p.Position = 2;
	p.TodayPosition = 2;
	p.UseMargin = 50.0;
	p.PositionCost = 100.0;
	probe.OnRspQryInvestorPosition(&p, nullptr, 4, false);

	/* 负仓 + 零保证金：套利兜底把可用量归零 */
	std::snprintf(p.InstrumentID, sizeof(p.InstrumentID), "%s", "neg2601");
	p.PositionDate = THOST_FTDC_PSD_History;
	p.Position = -2;
	p.TodayPosition = 0;
	p.UseMargin = 0.0;
	p.PositionCost = 0.0;
	probe.OnRspQryInvestorPosition(&p, nullptr, 5, false);

	/* 未知合约：不入表 */
	std::snprintf(p.InstrumentID, sizeof(p.InstrumentID), "%s", "ghost");
	probe.OnRspQryInvestorPosition(&p, nullptr, 6, false);

	/* 末笔用未知合约收尾，避免同一 key 二次累计 */
	probe.OnRspQryInvestorPosition(&p, nullptr, 7, true);

	ASSERT_EQ(4u, sink.positions.size());

	const R61MiniSink::PosCap* longPos = findPos(sink.positions, "ru2601", WDT_LONG);
	ASSERT_NE(nullptr, longPos);
	EXPECT_DOUBLE_EQ(7.0, longPos->total);
	EXPECT_DOUBLE_EQ(0.0, longPos->newPos);
	EXPECT_DOUBLE_EQ(7.0, longPos->prePos);
	EXPECT_DOUBLE_EQ(5.0, longPos->availPre);	/* 7 - ShortFrozen 2 */
	EXPECT_DOUBLE_EQ(100.0, longPos->avg);		/* 7000/7/10 */
	EXPECT_DOUBLE_EQ(300.0, longPos->margin);

	const R61MiniSink::PosCap* shortPos = findPos(sink.positions, "ru2601", WDT_SHORT);
	ASSERT_NE(nullptr, shortPos);
	EXPECT_DOUBLE_EQ(0.0, shortPos->total);
	EXPECT_DOUBLE_EQ(0.0, shortPos->newPos);
	EXPECT_DOUBLE_EQ(0.0, shortPos->availNew);
	EXPECT_DOUBLE_EQ(0.0, shortPos->avg);

	const R61MiniSink::PosCap* combo = findPos(sink.positions, "sp2601", WDT_LONG);
	ASSERT_NE(nullptr, combo);
	EXPECT_DOUBLE_EQ(2.0, combo->total);
	EXPECT_DOUBLE_EQ(0.0, combo->availNew);

	const R61MiniSink::PosCap* neg = findPos(sink.positions, "neg2601", WDT_LONG);
	ASSERT_NE(nullptr, neg);
	EXPECT_DOUBLE_EQ(-2.0, neg->total);
	EXPECT_DOUBLE_EQ(0.0, neg->availNew);
	EXPECT_DOUBLE_EQ(0.0, neg->availPre);
	EXPECT_DOUBLE_EQ(0.0, neg->avg);

	/* 错误回报不再入表 */
	CThostFtdcRspInfoField err = r61_mini_ok();
	err.ErrorID = 9;
	std::snprintf(p.InstrumentID, sizeof(p.InstrumentID), "%s", "ru2601");
	probe.OnRspQryInvestorPosition(&p, &err, 8, false);
}
