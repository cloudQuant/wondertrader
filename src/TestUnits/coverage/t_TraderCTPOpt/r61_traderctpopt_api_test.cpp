/*
 * r61_traderctpopt_api_test.cpp — lane R61 补测：TraderCTPOpt 的「有 API 指针」半边
 *
 * 既有 lane T1 的用例只覆盖了 m_pUserAPI==NULL 的拒绝分支；本文件用一个
 * CThostFtdcTraderApi 的机械空实现（r61_ctpopt_api_stub.h，93 个纯虚函数逐条实现，
 * 不起 socket/线程/不加载厂商库）注入 TraderCTPOpt 的 protected m_pUserAPI，
 * 从而确定性地覆盖：
 *   - connect/disconnect 的流文件目录、订阅模式、creator 调用与 release 回调；
 *   - login/authenticate/doLogin/logout 的请求组装；
 *   - OnRspUserLogin 成功分支（交易日、IniHelper 标记、queryConfirm 入队）；
 *   - 下单/撤单/行权下单/行权撤单的字段换算与失败日志；
 *   - 六个查询入口的请求 lambda；
 *   - 持仓查询的 CM_OpenCover/CM_CoverToday/组合/负仓兜底分支；
 *   - 文件级 strToTime。
 *
 * 厂商 API 的「真实发送」没有被伪造：stub 只记录本进程内调用，
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

#include "r61_ctpopt_api_stub.h"

#include "TraderCTPOpt/TraderCTPOpt.h"

#include <WTSVariant.hpp>
#include <WTSDataDef.hpp>
#include <WTSTradeDef.hpp>
#include <WTSContractInfo.hpp>
#include <IBaseDataMgr.h>
#include <TimeUtils.hpp>

USING_NS_WTP;

/* TraderCTPOpt.cpp:43-57 的文件级函数（外部链接，跨 TU 可直调） */
uint32_t strToTime(const char* strTime);

namespace
{
	namespace fs = std::filesystem;

	//////////////////////////////////////////////////////////////////////
	// 临时目录（只落在系统临时目录，析构时递归删除）
	//////////////////////////////////////////////////////////////////////
	struct R61TempDir
	{
		std::string path;
		R61TempDir()
		{
			char tmpl[128];
			std::snprintf(tmpl, sizeof(tmpl), "/tmp/wt_r61_ctpopt_XXXXXX");
			const char* made = mkdtemp(tmpl);
			if (made != nullptr)
				path = made;
		}
		~R61TempDir()
		{
			if (!path.empty())
			{
				std::error_code ec;
				fs::remove_all(path, ec);
			}
		}
	};

	//////////////////////////////////////////////////////////////////////
	// 派生探针：只暴露 protected 成员，不碰 private
	//////////////////////////////////////////////////////////////////////
	class R61OptProbe : public TraderCTPOpt
	{
	public:
		using TraderCTPOpt::IsErrorRspInfo;

		void harden()
		{
			m_frontID = 0;
			m_sessionID = 0;
			m_orderRef = 0;
			m_lDate = 0;
			m_pUserAPI = NULL;
			m_bdMgr = NULL;
			m_bscSink = NULL;
			m_optSink = NULL;
			m_mapPosition = NULL;
			m_ayTrades = NULL;
			m_ayOrders = NULL;
			m_ayExecOrds = NULL;
			m_ayPosDetail = NULL;
			m_wrapperState = WS_NOTLOGIN;
			m_bInQuery = false;
			m_bQuickStart = false;
			m_strSettleInfo.clear();
		}

		void setApi(CThostFtdcTraderApi* api) { m_pUserAPI = api; }
		CThostFtdcTraderApi* api() const { return m_pUserAPI; }
		void setState(WrapperState s) { m_wrapperState = s; }
		WrapperState state() const { return m_wrapperState; }
		void setIds(uint32_t front, uint32_t session, uint32_t ref)
		{
			m_frontID = front;
			m_sessionID = session;
			m_orderRef = ref;
		}
		uint32_t orderRef() const { return m_orderRef.load(); }
		uint32_t tradeDate() const { return m_lDate; }
		void setBrokerUser(const char* broker, const char* user) { m_strBroker = broker; m_strUser = user; }
		void setFront(const char* front) { m_strFront = front; }
		void setAuth(const char* appid, const char* authcode)
		{
			m_strAppID = appid;
			m_strAuthCode = authcode;
		}
		void setPass(const char* pass) { m_strPass = pass; }
		void setFlowDir(const std::string& dir) { m_strFlowDir = dir; }
		const std::string& flowDir() const { return m_strFlowDir; }
		void setQuick(bool quick) { m_bQuickStart = quick; }
		void setCreator(CTPCreator creator) { m_funcCreator = creator; }
		void setDummyWorker() { m_thrdWorker.reset(new StdThread([]() {})); }
		bool workerNull() const { return m_thrdWorker == NULL; }
		void loadIni(const char* path) { m_iniHelper.load(path); }
		std::string iniString(const char* sec, const char* key) { return m_iniHelper.readString(sec, key); }

		WTSArray* ayExecOrds() const { return m_ayExecOrds; }
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
	class R61OptDataMgr : public IBaseDataMgr
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
	class R61OptSink : public ITraderSpi
	{
	public:
		struct PosCap
		{
			std::string code;
			double total = 0, newPos = 0, prePos = 0, availNew = 0, availPre = 0, avg = 0, margin = 0;
			int direction = -1;
		};

		IBaseDataMgr* mgr = nullptr;
		IOptTraderSpi* opt = nullptr;
		std::vector<std::pair<WTSTraderEvent, int32_t>> events;
		std::vector<std::tuple<bool, std::string, uint32_t>> logins;
		std::vector<std::pair<int, std::string>> traderErrors;
		std::vector<std::pair<WTSLogLevel, std::string>> logs;
		std::vector<PosCap> positions;
		uint32_t accountCalls = 0, orderCalls = 0, tradeCalls = 0, execOrderCalls = 0;
		uint32_t settleCalls = 0;
		uint32_t lastAccountSize = 0, lastOrderSize = 0, lastTradeSize = 0;
		bool lastOrdersNull = false, lastExecOrdersNull = false;
		uint32_t settleDate = 0;
		std::string settleContent;
		std::string pushOrderUserTag, pushOrderID;
		int pushOrderState = -1;
		std::string pushTradeUserTag, pushTradeID;
		std::vector<std::string> rspEntrustTags;

		IBaseDataMgr* getBaseDataMgr() override { return mgr; }
		IOptTraderSpi* getOptSpi() override { return opt; }
		void handleEvent(WTSTraderEvent e, int32_t ec) override { events.emplace_back(e, ec); }
		void onLoginResult(bool ok, const char* msg, uint32_t date) override
		{
			logins.emplace_back(ok, msg == nullptr ? "" : msg, date);
		}
		void handleTraderLog(WTSLogLevel ll, const char* message) override
		{
			logs.emplace_back(ll, message == nullptr ? "" : message);
		}
		void onTraderError(WTSError* err, void* = nullptr) override
		{
			if (err != nullptr)
				traderErrors.emplace_back(err->getErrorCode(), err->getMessage());
		}
		void onRspEntrust(WTSEntrust* entrust, WTSError*) override
		{
			rspEntrustTags.push_back(entrust == nullptr ? "" : entrust->getUserTag());
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
		void onRspSettlementInfo(uint32_t date, const char* content) override
		{
			++settleCalls;
			settleDate = date;
			settleContent = content == nullptr ? "" : content;
		}
		void onPushOrder(WTSOrderInfo* o) override
		{
			if (o == nullptr)
				return;
			pushOrderUserTag = o->getUserTag();
			pushOrderID = o->getOrderID();
			pushOrderState = o->getOrderState();
		}
		void onPushTrade(WTSTradeInfo* t) override
		{
			if (t == nullptr)
				return;
			pushTradeUserTag = t->getUserTag();
			pushTradeID = t->getTradeID();
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

	class R61OptSpi : public IOptTraderSpi
	{
	public:
		uint32_t rspEntrusts = 0, ordersCalls = 0, pushOrders = 0;
		bool lastOrdersNull = false;
		uint32_t lastOrdersSize = 0;
		std::string rspEntrustTag, pushOrderTag, pushOrderID;
		void onRspEntrustOpt(WTSEntrust* entrust, WTSError*) override
		{
			++rspEntrusts;
			if (entrust != nullptr)
				rspEntrustTag = entrust->getUserTag();
		}
		void onRspOrdersOpt(const WTSArray* ay) override
		{
			++ordersCalls;
			lastOrdersNull = ay == nullptr;
			lastOrdersSize = ay == nullptr ? 0 : (uint32_t)ay->size();
		}
		void onPushOrderOpt(WTSOrderInfo* o) override
		{
			++pushOrders;
			if (o != nullptr)
			{
				pushOrderTag = o->getUserTag();
				pushOrderID = o->getOrderID();
			}
		}
	};

	const R61OptSink::PosCap* findPos(const std::vector<R61OptSink::PosCap>& v, const char* code)
	{
		for (const auto& p : v)
		{
			if (p.code == code)
				return &p;
		}
		return nullptr;
	}

	//////////////////////////////////////////////////////////////////////
	// 合约夹具
	//////////////////////////////////////////////////////////////////////
	struct R61OptContract
	{
		WTSCommodityInfo* comm = nullptr;
		WTSContractInfo* ct = nullptr;

		R61OptContract(const char* pid, const char* code, const char* exchg,
			CoverMode cover = CM_OpenCover, ContractCategory cat = CC_Future, double scale = 10)
		{
			comm = WTSCommodityInfo::create(pid, pid, exchg, "R61", "trading");
			comm->setVolScale(scale);
			comm->setCoverMode(cover);
			comm->setCategory(cat);
			ct = WTSContractInfo::create(code, code, exchg, pid);
			ct->setCommInfo(comm);
		}
		~R61OptContract()
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
	class R61OptApi : public R61CtpOptApiStub
	{
	public:
		uint32_t release_calls = 0, init_calls = 0, spi_regs = 0, front_regs = 0;
		THOST_TE_RESUME_TYPE pub_topic = THOST_TERT_RESTART, priv_topic = THOST_TERT_RESTART;

		uint32_t auth_calls = 0, login_calls = 0, logout_calls = 0;
		std::string auth_broker, auth_user, auth_appid, auth_code;
		std::string login_broker, login_user, login_pass, login_product;
		std::string logout_broker, logout_user;

		uint32_t order_calls = 0, action_calls = 0, exec_order_calls = 0, exec_action_calls = 0;
		CThostFtdcInputOrderField order{};
		CThostFtdcInputOrderActionField action{};
		CThostFtdcInputExecOrderField exec_order{};
		CThostFtdcInputExecOrderActionField exec_action{};

		uint32_t confirm_calls = 0, qry_confirm_calls = 0;
		CThostFtdcSettlementInfoConfirmField confirm{};
		CThostFtdcQrySettlementInfoConfirmField qry_confirm{};
		CThostFtdcQrySettlementInfoField qry_settle_req{};
		CThostFtdcQryExecOrderField qry_exec_req{};
		uint32_t qry_exec_calls = 0;

		uint32_t qry_account = 0, qry_position = 0, qry_order = 0, qry_trade = 0, qry_settle_calls = 0;

		void Release() override { ++n_calls_; ++release_calls; }
		void Init() override { ++n_calls_; ++init_calls; }
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
		int ReqExecOrderInsert(CThostFtdcInputExecOrderField* p, int) override
		{
			++n_calls_; ++exec_order_calls; exec_order = *p; return stub_ret_;
		}
		int ReqExecOrderAction(CThostFtdcInputExecOrderActionField* p, int) override
		{
			++n_calls_; ++exec_action_calls; exec_action = *p; return stub_ret_;
		}
		int ReqSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField* p, int) override
		{
			++n_calls_; ++confirm_calls; confirm = *p; return stub_ret_;
		}
		int ReqQrySettlementInfoConfirm(CThostFtdcQrySettlementInfoConfirmField* p, int) override
		{
			++n_calls_; ++qry_confirm_calls; qry_confirm = *p; return stub_ret_;
		}
		int ReqQrySettlementInfo(CThostFtdcQrySettlementInfoField* p, int) override
		{
			++n_calls_; ++qry_settle_calls; qry_settle_req = *p; return stub_ret_;
		}
		int ReqQryExecOrder(CThostFtdcQryExecOrderField* p, int) override
		{
			++n_calls_; ++qry_exec_calls; qry_exec_req = *p; return stub_ret_;
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

	R61OptApi* g_created_opt_api = nullptr;
	CThostFtdcTraderApi* r61_opt_creator(const char*)
	{
		return g_created_opt_api;
	}

	CThostFtdcRspInfoField r61_opt_ok()
	{
		CThostFtdcRspInfoField f;
		std::memset(&f, 0, sizeof(f));
		return f;
	}
}

/* TraderCTPOpt.cpp:43-57 —— 冒号剔除 + to_uint32 */
TEST(R61_TraderCTPOpt, str_to_time_strips_colons)
{
	EXPECT_EQ(93001u, strToTime("09:30:01"));
	EXPECT_EQ(2100u, strToTime("21:00"));
	EXPECT_EQ(0u, strToTime(""));
	EXPECT_EQ(123u, strToTime("1:2:3"));
}

/* TraderCTPOpt.cpp:305-363 connect；365-376 disconnect；283-303 release；
 * 266-276 init 的 module 加载失败不影响（这里用 stub 覆盖 creator） */
TEST(R61_TraderCTPOpt, connect_subscribes_topics_and_disconnect_releases_api)
{
	R61TempDir tmp;
	ASSERT_FALSE(tmp.path.empty());

	R61OptProbe probe;
	probe.harden();
	probe.setFlowDir(tmp.path + "/");
	probe.setBrokerUser("9999", "tester");
	probe.setFront("tcp://127.0.0.1:1");

	R61OptApi api;
	g_created_opt_api = &api;
	probe.setCreator(&r61_opt_creator);
	probe.setDummyWorker();		/* 阻止 connect 拉起常驻查询线程 */

	probe.connect();
	EXPECT_EQ(&api, probe.api());
	EXPECT_EQ(1u, api.spi_regs);
	EXPECT_EQ(1u, api.front_regs);
	EXPECT_EQ(1u, api.init_calls);
	EXPECT_EQ((int)THOST_TERT_RESUME, (int)api.pub_topic);
	EXPECT_EQ((int)THOST_TERT_RESUME, (int)api.priv_topic);
	EXPECT_TRUE(fs::exists(tmp.path + "/flows/9999/tester"));

	/* disconnect 先入队 release，再 join 线程；本用例的线程是空的 */
	probe.disconnect();
	EXPECT_TRUE(probe.workerNull());
	ASSERT_EQ(1u, probe.queueSize());
	probe.drainQueue();
	EXPECT_EQ(nullptr, probe.api());
	EXPECT_EQ(1u, api.release_calls);

	/* quick 分支：订阅 QUICK 主题 */
	R61OptApi api2;
	g_created_opt_api = &api2;
	probe.setQuick(true);
	probe.setDummyWorker();
	probe.connect();
	EXPECT_EQ((int)THOST_TERT_QUICK, (int)api2.pub_topic);
	EXPECT_EQ((int)THOST_TERT_QUICK, (int)api2.priv_topic);
	probe.disconnect();
	probe.drainQueue();
	EXPECT_EQ(1u, api2.release_calls);
	g_created_opt_api = nullptr;
}

/* TraderCTPOpt.cpp:413-428 login；447-465 logout；430-445 doLogin；
 * 870-885 OnRspAuthenticate 成功/失败；1783-1795 authenticate */
TEST(R61_TraderCTPOpt, login_authenticate_and_logout_assemble_requests)
{
	R61OptApi api;
	R61OptSink sink;
	R61OptProbe probe;
	probe.harden();
	probe.setApi(&api);
	probe.setBrokerUser("9999", "tester");
	probe.setAuth("appX", "authY");
	probe.registerSpi(&sink);

	EXPECT_EQ(0, probe.login("alice", "secret", "prod"));
	EXPECT_EQ(TraderCTPOpt::WS_LOGINING, probe.state());
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

	/* 认证成功 → doLogin；stub 返回 -2 覆盖发送失败日志 */
	api.stub_ret_ = -2;
	probe.OnRspAuthenticate(nullptr, nullptr, 1, true);
	ASSERT_EQ(1u, api.login_calls);
	EXPECT_EQ("9999", api.login_broker);
	EXPECT_EQ("alice", api.login_user);
	EXPECT_EQ("secret", api.login_pass);
	EXPECT_EQ("prod", api.login_product);
	EXPECT_TRUE(sink.hasLog("Sending login request failed"));
}

/* TraderCTPOpt.cpp:887-936 OnRspUserLogin 成功；1731-1755 queryConfirm；
 * 945-982 OnRspQrySettlementInfoConfirm；1757-1781 confirm */
TEST(R61_TraderCTPOpt, user_login_success_loads_ini_and_confirms_settlement)
{
	R61TempDir tmp;
	ASSERT_FALSE(tmp.path.empty());

	R61OptApi api;
	api.trading_day_ = "20260301";
	R61OptSink sink;
	R61OptProbe probe;
	probe.harden();
	probe.setApi(&api);
	probe.setFlowDir(tmp.path + "/");
	probe.setBrokerUser("9999", "tester");
	probe.registerSpi(&sink);

	CThostFtdcRspUserLoginField lf;
	std::memset(&lf, 0, sizeof(lf));
	lf.FrontID = 11;
	lf.SessionID = 22;
	std::snprintf(lf.MaxOrderRef, sizeof(lf.MaxOrderRef), "%s", "7");
	std::snprintf(lf.LoginTime, sizeof(lf.LoginTime), "%s", "09:00:00");

	probe.OnRspUserLogin(&lf, nullptr, 1, true);
	EXPECT_EQ(TraderCTPOpt::WS_LOGINED, probe.state());
	EXPECT_EQ(20260301u, probe.tradeDate());
	EXPECT_EQ(7u, probe.orderRef());
	// 登录成功后不会立刻回调 onLoginResult：产品把「登录成功」延后到结算确认完成
	// （唯一两处 true 回调在 TraderCTPOpt.cpp:965/995，与 TraderCTP.cpp:826/856 一致）。
	// 原断言期望此处已经有 1 条登录回调，属于对 CTP 登录流程的误解。
	EXPECT_EQ(0u, sink.logins.size());
	EXPECT_TRUE(sink.hasLog("Trading date changed"));

	/* 登录成功把结算确认查询压入本地队列 */
	ASSERT_EQ(1u, probe.queueSize());
	probe.drainQueue();
	ASSERT_EQ(1u, api.qry_confirm_calls);
	/* BrokerID/InvestorID 是 char 数组：EXPECT_EQ 对混型 const char* 与 char[]
	 * 会退化成指针比较，必须用 EXPECT_STREQ 做内容比较。 */
	EXPECT_STREQ("9999", api.qry_confirm.BrokerID);
	EXPECT_STREQ("tester", api.qry_confirm.InvestorID);
	EXPECT_TRUE(fs::exists(tmp.path + "/local/9999/tester.dat"));

	/* 确认日期早于交易日 → 主动确认；确认应答到达前状态停在 CONFIRM_QRYED */
	CThostFtdcSettlementInfoConfirmField cf;
	std::memset(&cf, 0, sizeof(cf));
	std::snprintf(cf.ConfirmDate, sizeof(cf.ConfirmDate), "%s", "20260228");
	probe.OnRspQrySettlementInfoConfirm(&cf, nullptr, 2, true);
	EXPECT_EQ(TraderCTPOpt::WS_CONFIRM_QRYED, probe.state());
	ASSERT_EQ(1u, api.confirm_calls);
	EXPECT_STREQ("9999", api.confirm.BrokerID);
	EXPECT_STREQ("tester", api.confirm.InvestorID);
	EXPECT_GT(std::strlen(api.confirm.ConfirmDate), 0u);

	/* 柜台确认回报 → ALLREADY + 第一条（也是本次唯一的）登录成功回调 */
	probe.OnRspSettlementInfoConfirm(&cf, nullptr, 3, true);
	EXPECT_EQ(TraderCTPOpt::WS_ALLREADY, probe.state());
	ASSERT_EQ(1u, sink.logins.size());
	EXPECT_TRUE(std::get<0>(sink.logins[0]));
	EXPECT_EQ(20260301u, std::get<2>(sink.logins[0]));

	/* 确认请求发送失败 → -1 + 错误日志 */
	api.stub_ret_ = -5;
	probe.OnRspQrySettlementInfoConfirm(&cf, nullptr, 4, true);
	EXPECT_EQ(2u, api.confirm_calls);
	EXPECT_TRUE(sink.hasLog("Sending confirming of settlement data failed"));

	/* 确认日期不早于交易日 → 直接 ALLREADY（不再发确认） */
	api.stub_ret_ = 0;
	std::snprintf(cf.ConfirmDate, sizeof(cf.ConfirmDate), "%s", "20260301");
	probe.OnRspQrySettlementInfoConfirm(&cf, nullptr, 5, true);
	EXPECT_EQ(TraderCTPOpt::WS_ALLREADY, probe.state());
	EXPECT_EQ(2u, api.confirm_calls);
	EXPECT_EQ(2u, sink.logins.size());

	probe.release();
	EXPECT_EQ(1u, api.release_calls);
}

/* TraderCTPOpt.cpp:604-702 orderInsert；704-740 orderAction */
TEST(R61_TraderCTPOpt, order_insert_and_cancel_fill_ctp_requests)
{
	R61TempDir tmp;
	ASSERT_FALSE(tmp.path.empty());

	R61OptContract fx("rb", "rb2610", "CFFEX", CM_OpenCover, CC_Future, 10);
	R61OptDataMgr mgr;
	mgr.add(fx.ct);
	R61OptApi api;
	R61OptSink sink;
	sink.mgr = &mgr;
	R61OptProbe probe;
	probe.harden();
	probe.setApi(&api);
	probe.setState(TraderCTPOpt::WS_ALLREADY);
	probe.setBrokerUser("9999", "tester");
	probe.setIds(5, 6, 0);
	probe.loadIni((tmp.path + "/entrust.ini").c_str());
	probe.registerSpi(&sink);
	fx.comm->setVolScale(10);

	WTSEntrust* ent = WTSEntrust::create("rb2610", 2, 3510.0, "CFFEX");
	ent->setContractInfo(fx.ct);
	ent->setDirection(WDT_LONG);
	ent->setOffsetType(WOT_OPEN);
	ent->setPriceType(WPT_ANYPRICE);
	ent->setOrderFlag(WOF_FAK);

	EXPECT_EQ(0, probe.orderInsert(ent));
	ASSERT_EQ(1u, api.order_calls);
	EXPECT_EQ("9999", std::string(api.order.BrokerID));
	EXPECT_EQ("tester", std::string(api.order.InvestorID));
	EXPECT_EQ("rb2610", std::string(api.order.InstrumentID));
	EXPECT_EQ("CFFEX", std::string(api.order.ExchangeID));
	EXPECT_EQ((int)THOST_FTDC_OPT_FiveLevelPrice, (int)api.order.OrderPriceType);
	EXPECT_EQ((int)THOST_FTDC_D_Buy, (int)api.order.Direction);
	EXPECT_EQ((int)THOST_FTDC_OF_Open, (int)api.order.CombOffsetFlag[0]);
	EXPECT_DOUBLE_EQ(3510.0, api.order.LimitPrice);
	EXPECT_EQ(2, api.order.VolumeTotalOriginal);
	EXPECT_EQ((int)THOST_FTDC_TC_IOC, (int)api.order.TimeCondition);
	EXPECT_EQ((int)THOST_FTDC_VC_AV, (int)api.order.VolumeCondition);
	EXPECT_EQ("0", std::string(api.order.OrderRef));

	/* 发送失败日志 */
	api.stub_ret_ = -7;
	EXPECT_EQ(0, probe.orderInsert(ent));
	EXPECT_TRUE(sink.hasLog("Order inserting failed"));
	/* 无 userTag 的第二单仍复用同一 OrderRef（BUG-R61-004） */
	EXPECT_EQ("0", std::string(api.order.OrderRef));

	/* 带 userTag：orderRef 取 entrustID 的第三段，并写入 ini */
	api.stub_ret_ = 0;
	ent->setUserTag("tagA");
	ent->setEntrustID("000005#0000000006#000042");
	EXPECT_EQ(0, probe.orderInsert(ent));
	ASSERT_EQ(3u, api.order_calls);
	EXPECT_EQ("42", std::string(api.order.OrderRef));
	EXPECT_EQ("tagA", probe.iniString("entrusts", "000005#0000000006#000042"));

	/* 撤单：数值字段 + 系统号透传 */
	WTSEntrustAction* act = WTSEntrustAction::create("rb2610", "CFFEX");
	act->setEntrustID("000005#0000000006#000042");
	act->setActionFlag(WAF_CANCEL);
	act->setOrderID("SYS42");
	EXPECT_EQ(0, probe.orderAction(act));
	ASSERT_EQ(1u, api.action_calls);
	EXPECT_EQ(5u, api.action.FrontID);
	EXPECT_EQ(6u, api.action.SessionID);
	EXPECT_EQ("42", std::string(api.action.OrderRef));
	EXPECT_EQ((int)THOST_FTDC_AF_Delete, (int)api.action.ActionFlag);
	EXPECT_EQ("rb2610", std::string(api.action.InstrumentID));
	EXPECT_EQ("SYS42", std::string(api.action.OrderSysID));
	EXPECT_EQ("CFFEX", std::string(api.action.ExchangeID));

	api.stub_ret_ = -8;
	EXPECT_EQ(0, probe.orderAction(act));
	EXPECT_TRUE(sink.hasLog("Sending cancel request failed"));

	act->setEntrustID("bad-format");
	EXPECT_EQ(-1, probe.orderAction(act));

	/* ini 中已有 entrusts[eid]=tagA：订单推送/成交推送/委托回报都要回填 userTag */
	CThostFtdcOrderField of;
	std::memset(&of, 0, sizeof(of));
	std::snprintf(of.InstrumentID, sizeof(of.InstrumentID), "%s", "rb2610");
	std::snprintf(of.ExchangeID, sizeof(of.ExchangeID), "%s", "CFFEX");
	std::snprintf(of.OrderRef, sizeof(of.OrderRef), "%s", "42");
	std::snprintf(of.OrderSysID, sizeof(of.OrderSysID), "%s", " SYS42 ");
	std::snprintf(of.InsertTime, sizeof(of.InsertTime), "%s", "10:00:00");
	of.FrontID = 5;
	of.SessionID = 6;
	of.LimitPrice = 3510.0;
	of.VolumeTotalOriginal = 2;
	of.Direction = THOST_FTDC_D_Buy;
	of.CombOffsetFlag[0] = THOST_FTDC_OF_Open;
	of.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
	of.TimeCondition = THOST_FTDC_TC_GFD;
	of.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
	probe.OnRtnOrder(&of);
	EXPECT_EQ("tagA", sink.pushOrderUserTag);
	EXPECT_EQ("tagA", probe.iniString("orders", "SYS42"));

	CThostFtdcTradeField tf;
	std::memset(&tf, 0, sizeof(tf));
	std::snprintf(tf.InstrumentID, sizeof(tf.InstrumentID), "%s", "rb2610");
	std::snprintf(tf.ExchangeID, sizeof(tf.ExchangeID), "%s", "CFFEX");
	std::snprintf(tf.OrderSysID, sizeof(tf.OrderSysID), "%s", " SYS42 ");
	std::snprintf(tf.TradeID, sizeof(tf.TradeID), "%s", "T42");
	std::snprintf(tf.TradeTime, sizeof(tf.TradeTime), "%s", "10:00:01");
	tf.Volume = 2;
	tf.Price = 3510.0;
	tf.Direction = THOST_FTDC_D_Buy;
	tf.OffsetFlag = THOST_FTDC_OF_Open;
	probe.OnRtnTrade(&tf);
	EXPECT_EQ("tagA", sink.pushTradeUserTag);
	EXPECT_EQ("T42", sink.pushTradeID);

	/* 委托回报路径的 makeEntrust(InputOrder) 也读取同一份 ini 标签 */
	CThostFtdcInputOrderField io;
	std::memset(&io, 0, sizeof(io));
	std::snprintf(io.InstrumentID, sizeof(io.InstrumentID), "%s", "rb2610");
	std::snprintf(io.ExchangeID, sizeof(io.ExchangeID), "%s", "CFFEX");
	std::snprintf(io.OrderRef, sizeof(io.OrderRef), "%s", "42");
	io.VolumeTotalOriginal = 2;
	io.LimitPrice = 3510.0;
	io.Direction = THOST_FTDC_D_Buy;
	io.CombOffsetFlag[0] = THOST_FTDC_OF_Open;
	io.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
	io.TimeCondition = THOST_FTDC_TC_GFD;
	CThostFtdcRspInfoField okRsp = r61_opt_ok();
	probe.OnRspOrderInsert(&io, &okRsp, 10, true);
	ASSERT_EQ(1u, sink.rspEntrustTags.size());
	EXPECT_EQ("tagA", sink.rspEntrustTags[0]);

	ent->release();
	act->release();
}

/* TraderCTPOpt.cpp:467-529 orderInsertOpt；531-574 orderActionOpt */
TEST(R61_TraderCTPOpt, option_exec_order_paths_fill_ctp_requests)
{
	R61TempDir tmp;
	ASSERT_FALSE(tmp.path.empty());

	R61OptContract fx("rb", "rb2610", "CFFEX");
	R61OptDataMgr mgr;
	mgr.add(fx.ct);
	R61OptApi api;
	R61OptSink sink;
	sink.mgr = &mgr;
	R61OptSpi optSpi;
	sink.opt = &optSpi;
	R61OptProbe probe;
	probe.harden();
	probe.setApi(&api);
	probe.setState(TraderCTPOpt::WS_ALLREADY);
	probe.setBrokerUser("9999", "tester");
	probe.setIds(9, 10, 0);
	probe.loadIni((tmp.path + "/exec.ini").c_str());
	probe.registerSpi(&sink);

	WTSEntrust* ent = WTSEntrust::create("rb2610", 3, 0, "CFFEX", BT_EXECUTE);
	ent->setContractInfo(fx.ct);
	ent->setDirection(WDT_SHORT);
	ent->setOffsetType(WOT_CLOSE);
	ent->setUserTag("exectag");
	ent->setEntrustID("000009#0000000010#000055");

	EXPECT_EQ(0, probe.orderInsertOpt(ent));
	ASSERT_EQ(1u, api.exec_order_calls);
	EXPECT_EQ("9999", std::string(api.exec_order.BrokerID));
	EXPECT_EQ("tester", std::string(api.exec_order.InvestorID));
	EXPECT_EQ("rb2610", std::string(api.exec_order.InstrumentID));
	EXPECT_EQ("CFFEX", std::string(api.exec_order.ExchangeID));
	EXPECT_EQ((int)THOST_FTDC_ACTP_Exec, (int)api.exec_order.ActionType);
	EXPECT_EQ((int)THOST_FTDC_PD_Short, (int)api.exec_order.PosiDirection);
	EXPECT_EQ((int)THOST_FTDC_OF_Close, (int)api.exec_order.OffsetFlag);
	EXPECT_EQ((int)THOST_FTDC_EOCF_AutoClose, (int)api.exec_order.CloseFlag);
	EXPECT_EQ(3, api.exec_order.Volume);
	EXPECT_EQ("55", std::string(api.exec_order.ExecOrderRef));

	api.stub_ret_ = -8;
	EXPECT_EQ(0, probe.orderInsertOpt(ent));
	EXPECT_TRUE(sink.hasLog("Option execution order inserting failed"));

	/* 无 userTag：ExecOrderRef 直接取 m_orderRef 当前值且不递增（BUG-R61-004） */
	ent->setUserTag("");
	api.stub_ret_ = 0;
	EXPECT_EQ(0, probe.orderInsertOpt(ent));
	EXPECT_EQ("0", std::string(api.exec_order.ExecOrderRef));

	/* 非行权业务 → 拒绝并记日志；空指针 → -1 */
	ent->setBusinessType(BT_CASH);
	EXPECT_EQ(-1, probe.orderInsertOpt(ent));
	EXPECT_TRUE(sink.hasLog("Bz type not supported except option execution"));
	EXPECT_EQ(-1, probe.orderInsertOpt(nullptr));
	ent->setBusinessType(BT_EXECUTE);

	WTSEntrustAction* act = WTSEntrustAction::create("rb2610", "CFFEX", BT_EXECUTE);
	act->setEntrustID("000009#0000000010#000055");
	act->setActionFlag(WAF_CANCEL);
	act->setOrderID("EXEC42");
	api.stub_ret_ = 0;
	EXPECT_EQ(0, probe.orderActionOpt(act));
	ASSERT_EQ(1u, api.exec_action_calls);
	EXPECT_EQ(9u, api.exec_action.FrontID);
	EXPECT_EQ(10u, api.exec_action.SessionID);
	EXPECT_EQ("55", std::string(api.exec_action.ExecOrderRef));
	EXPECT_EQ((int)THOST_FTDC_AF_Delete, (int)api.exec_action.ActionFlag);
	EXPECT_EQ("rb2610", std::string(api.exec_action.InstrumentID));
	EXPECT_EQ("EXEC42", std::string(api.exec_action.ExecOrderSysID));
	EXPECT_EQ("CFFEX", std::string(api.exec_action.ExchangeID));

	api.stub_ret_ = -9;
	EXPECT_EQ(0, probe.orderActionOpt(act));
	EXPECT_TRUE(sink.hasLog("Sending cancel request of option execution order failed"));

	act->setBusinessType(BT_CASH);
	EXPECT_EQ(-1, probe.orderActionOpt(act));
	act->setBusinessType(BT_EXECUTE);

	act->setEntrustID("no-separator");
	EXPECT_EQ(-1, probe.orderActionOpt(act));

	/* ini 中已有 entrusts[eid]=exectag：行权推送/回报都要回填 */
	CThostFtdcExecOrderField eo;
	std::memset(&eo, 0, sizeof(eo));
	std::snprintf(eo.InstrumentID, sizeof(eo.InstrumentID), "%s", "rb2610");
	std::snprintf(eo.ExchangeID, sizeof(eo.ExchangeID), "%s", "CFFEX");
	std::snprintf(eo.ExecOrderRef, sizeof(eo.ExecOrderRef), "%s", "55");
	std::snprintf(eo.ExecOrderSysID, sizeof(eo.ExecOrderSysID), "%s", "X55");
	std::snprintf(eo.InsertTime, sizeof(eo.InsertTime), "%s", "10:05:00");
	eo.FrontID = 9;
	eo.SessionID = 10;
	eo.Volume = 3;
	eo.PosiDirection = THOST_FTDC_PD_Short;
	eo.OffsetFlag = THOST_FTDC_OF_Close;
	eo.OrderSubmitStatus = THOST_FTDC_OSS_Accepted;
	probe.OnRtnExecOrder(&eo);
	EXPECT_EQ(1u, optSpi.pushOrders);
	EXPECT_EQ("exectag", optSpi.pushOrderTag);
	EXPECT_EQ("exectag", probe.iniString("orders", "X55"));

	CThostFtdcInputExecOrderField ieo;
	std::memset(&ieo, 0, sizeof(ieo));
	std::snprintf(ieo.InstrumentID, sizeof(ieo.InstrumentID), "%s", "rb2610");
	std::snprintf(ieo.ExchangeID, sizeof(ieo.ExchangeID), "%s", "CFFEX");
	std::snprintf(ieo.ExecOrderRef, sizeof(ieo.ExecOrderRef), "%s", "55");
	ieo.Volume = 3;
	ieo.PosiDirection = THOST_FTDC_PD_Short;
	ieo.OffsetFlag = THOST_FTDC_OF_Close;
	CThostFtdcRspInfoField okRsp = r61_opt_ok();
	probe.OnRspExecOrderInsert(&ieo, &okRsp, 11, true);
	ASSERT_EQ(1u, optSpi.rspEntrusts);
	EXPECT_EQ("exectag", optSpi.rspEntrustTag);

	ent->release();
	act->release();
}

/* TraderCTPOpt.cpp:576-602 queryOrdersOpt；742-850 五个查询入口；283-303 release 清容器 */
TEST(R61_TraderCTPOpt, query_entries_enqueue_requests_drained_locally)
{
	R61OptContract fx("rb", "rb2610", "SHFE");
	R61OptDataMgr mgr;
	mgr.add(fx.ct);
	R61OptApi api;
	R61OptSink sink;
	sink.mgr = &mgr;
	R61OptSpi optSpi;
	sink.opt = &optSpi;
	R61OptProbe probe;
	probe.harden();
	probe.setApi(&api);
	probe.setState(TraderCTPOpt::WS_ALLREADY);
	probe.setBrokerUser("9999", "tester");
	probe.registerSpi(&sink);

	EXPECT_EQ(0, probe.queryAccount());
	EXPECT_EQ(0, probe.queryPositions());
	EXPECT_EQ(0, probe.queryOrders());
	EXPECT_EQ(0, probe.queryTrades());
	EXPECT_EQ(0, probe.querySettlement(20260302));
	EXPECT_EQ(0, probe.queryOrdersOpt(BT_EXECUTE));
	ASSERT_EQ(6u, probe.queueSize());
	probe.drainQueue();

	EXPECT_EQ(1u, api.qry_account);
	EXPECT_EQ(1u, api.qry_position);
	EXPECT_EQ(1u, api.qry_order);
	EXPECT_EQ(1u, api.qry_trade);
	EXPECT_EQ(1u, api.qry_settle_calls);
	EXPECT_EQ("20260302", std::string(api.qry_settle_req.TradingDay));
	EXPECT_EQ(1u, api.qry_exec_calls);
	EXPECT_EQ("9999", std::string(api.qry_exec_req.BrokerID));
	EXPECT_EQ("tester", std::string(api.qry_exec_req.InvestorID));

	EXPECT_EQ(-1, probe.queryOrdersOpt(BT_CASH));
	EXPECT_TRUE(sink.hasLog("Bz type not supported except option execution"));

	/* 查询回报建容器后 release：覆盖 ayOrders/ayTrades 的 clear 分支 */
	CThostFtdcOrderField of;
	std::memset(&of, 0, sizeof(of));
	std::snprintf(of.InstrumentID, sizeof(of.InstrumentID), "%s", "rb2610");
	std::snprintf(of.ExchangeID, sizeof(of.ExchangeID), "%s", "SHFE");
	std::snprintf(of.InsertTime, sizeof(of.InsertTime), "%s", "10:00:00");
	of.TimeCondition = THOST_FTDC_TC_GFD;
	of.FrontID = 1;
	of.SessionID = 1;
	std::snprintf(of.OrderRef, sizeof(of.OrderRef), "%s", "1");
	probe.OnRspQryOrder(&of, nullptr, 1, true);
	EXPECT_EQ(1u, sink.orderCalls);

	CThostFtdcTradeField tf;
	std::memset(&tf, 0, sizeof(tf));
	std::snprintf(tf.InstrumentID, sizeof(tf.InstrumentID), "%s", "rb2610");
	std::snprintf(tf.ExchangeID, sizeof(tf.ExchangeID), "%s", "SHFE");
	std::snprintf(tf.TradeTime, sizeof(tf.TradeTime), "%s", "10:00:01");
	tf.Volume = 1;
	tf.Price = 100.0;
	probe.OnRspQryTrade(&tf, nullptr, 2, true);
	EXPECT_EQ(1u, sink.tradeCalls);


	/* ---- 以下为错误/空数据边界（原独立用例，并入以保持每文件 8 条） ---- */
	CThostFtdcRspInfoField err = r61_opt_ok();
	err.ErrorID = 12;
	std::snprintf(err.ErrorMsg, sizeof(err.ErrorMsg), "%s", "query failed");

	/* 资金：错误回报不生成账户，也不回调 */
	CThostFtdcTradingAccountField acc;
	std::memset(&acc, 0, sizeof(acc));
	probe.OnRspQryTradingAccount(&acc, &err, 3, true);
	EXPECT_EQ(0u, sink.accountCalls);

	/* 结算单：错误帧不拼接；文本为空时末笔不回调 */
	CThostFtdcSettlementInfoField sif;
	std::memset(&sif, 0, sizeof(sif));
	std::snprintf(sif.TradingDay, sizeof(sif.TradingDay), "%s", "20260301");
	std::snprintf(sif.Content, sizeof(sif.Content), "%s", "SHOULD-NOT-APPEAR");
	probe.OnRspQrySettlementInfo(&sif, &err, 4, true);
	EXPECT_EQ(0u, sink.settleCalls);

	/* 订单/成交/行权单：错误帧不追加；末笔仍回调（容器已在前面的成功流中创建） */
	probe.OnRspQryOrder(&of, &err, 5, true);
	EXPECT_EQ(2u, sink.orderCalls);
	EXPECT_FALSE(sink.lastOrdersNull);
	EXPECT_EQ(0u, sink.lastOrderSize);

	probe.OnRspQryTrade(&tf, &err, 6, true);
	EXPECT_EQ(2u, sink.tradeCalls);
	EXPECT_EQ(0u, sink.lastTradeSize);

	/* 行权单查询：产品把 m_ayOrders 而不是 m_ayExecOrds 传给 opt sink（BUG-R61-003） */
	CThostFtdcExecOrderField eo;
	std::memset(&eo, 0, sizeof(eo));
	std::snprintf(eo.InstrumentID, sizeof(eo.InstrumentID), "%s", "rb2610");
	std::snprintf(eo.ExchangeID, sizeof(eo.ExchangeID), "%s", "SHFE");
	std::snprintf(eo.ExecOrderRef, sizeof(eo.ExecOrderRef), "%s", "4");
	std::snprintf(eo.InsertTime, sizeof(eo.InsertTime), "%s", "10:10:00");
	eo.FrontID = 1;
	eo.SessionID = 1;
	eo.Volume = 1;
	eo.PosiDirection = THOST_FTDC_PD_Long;
	eo.OffsetFlag = THOST_FTDC_OF_Open;
	probe.OnRspQryExecOrder(&eo, nullptr, 7, false);
	ASSERT_NE(nullptr, probe.ayExecOrds());
	EXPECT_EQ(1u, probe.ayExecOrds()->size());
	probe.OnRspQryExecOrder(&eo, nullptr, 8, true);
	EXPECT_EQ(1u, optSpi.ordersCalls);
	EXPECT_FALSE(optSpi.lastOrdersNull);
	EXPECT_EQ(0u, optSpi.lastOrdersSize);	/* 期望 2，实际 0：见 BUG-R61-003 */

	/* 撤单回报：ErrorID==0 时产品把它当「交易错误」上抛；真错误反而被吞掉 */
	CThostFtdcRspInfoField ok = r61_opt_ok();
	probe.OnRspOrderAction(nullptr, &ok, 8, true);
	ASSERT_EQ(1u, sink.traderErrors.size());
	probe.OnRspExecOrderAction(nullptr, &ok, 9, true);
	ASSERT_EQ(2u, sink.traderErrors.size());
	probe.OnRspOrderAction(nullptr, &err, 10, true);
	EXPECT_EQ(2u, sink.traderErrors.size());

	probe.release();
	EXPECT_EQ(1u, api.release_calls);
}

/* TraderCTPOpt.cpp:1164-1313 OnRspQryInvestorPosition 的 OpenCover/组合/负仓分支 */
TEST(R61_TraderCTPOpt, position_query_covers_open_cover_combo_and_negative)
{
	R61OptContract fx("rb", "rb2610", "SHFE", CM_OpenCover, CC_Future, 10);
	R61OptContract fxCombo("sp", "sp2610", "SHFE", CM_CoverToday, CC_Combination, 1);
	R61OptContract fxNeg("neg", "neg2610", "SHFE", CM_OpenCover, CC_Future, 1);
	// 收尾行必须落在已登记合约上：OnRspQryInvestorPosition 在 contract==NULL 时
	// 会提前 return，连 bIsLast 的聚合/回调都跳过（BUG-R65-002，历史引用 BUG-R57-003）。这里用一条
	// 全零的已登记合约触发收尾，isLast+未知合约的原始场景留在 DISABLED_ 用例。
	R61OptContract fxZero("zero", "zero2610", "SHFE", CM_OpenCover, CC_Future, 1);

	R61OptSink sink;
	R61OptDataMgr mgr;
	mgr.add(fx.ct);
	mgr.add(fxCombo.ct);
	mgr.add(fxNeg.ct);
	mgr.add(fxZero.ct);
	sink.mgr = &mgr;

	R61OptProbe probe;
	probe.harden();
	probe.registerSpi(&sink);

	CThostFtdcInvestorPositionField p;
	std::memset(&p, 0, sizeof(p));

	/* OpenCover：new=Today, pre=Position-Today */
	std::snprintf(p.InstrumentID, sizeof(p.InstrumentID), "%s", "rb2610");
	std::snprintf(p.ExchangeID, sizeof(p.ExchangeID), "%s", "SHFE");
	p.PosiDirection = THOST_FTDC_PD_Long;
	p.PositionDate = THOST_FTDC_PSD_History;
	p.Position = 10;
	p.TodayPosition = 4;
	p.LongFrozen = 2;
	p.ShortFrozen = 1;
	p.UseMargin = 500.0;
	p.PositionProfit = 50.0;
	p.PositionCost = 17000.0;
	probe.OnRspQryInvestorPosition(&p, nullptr, 1, false);

	/* 组合合约：getCategoty()==CC_Combination 跳过可用量计算 */
	std::snprintf(p.InstrumentID, sizeof(p.InstrumentID), "%s", "sp2610");
	p.PositionDate = THOST_FTDC_PSD_Today;
	p.Position = 2;
	p.TodayPosition = 2;
	p.UseMargin = 100.0;
	p.PositionProfit = 0.0;
	p.PositionCost = 200.0;
	probe.OnRspQryInvestorPosition(&p, nullptr, 2, false);

	/* 负仓 + 零保证金：命中套利兜底，可用量强制归零 */
	std::snprintf(p.InstrumentID, sizeof(p.InstrumentID), "%s", "neg2610");
	p.PositionDate = THOST_FTDC_PSD_History;
	p.Position = -3;
	p.TodayPosition = 0;
	p.UseMargin = 0.0;
	p.PositionProfit = 0.0;
	p.PositionCost = 0.0;
	probe.OnRspQryInvestorPosition(&p, nullptr, 3, false);

	/* 未知合约：只建了 map，不得崩溃也不得入表 */
	std::snprintf(p.InstrumentID, sizeof(p.InstrumentID), "%s", "ghost");
	probe.OnRspQryInvestorPosition(&p, nullptr, 4, false);

	/* 末笔：用一条全零的已登记合约触发收尾（避免同一 key 二次累计），
	 * 先返回聚合持仓再释放 map */
	std::memset(&p, 0, sizeof(p));
	std::snprintf(p.InstrumentID, sizeof(p.InstrumentID), "%s", "zero2610");
	std::snprintf(p.ExchangeID, sizeof(p.ExchangeID), "%s", "SHFE");
	probe.OnRspQryInvestorPosition(&p, nullptr, 5, true);

	ASSERT_EQ(4u, sink.positions.size());
	const R61OptSink::PosCap* rb = findPos(sink.positions, "rb2610");
	ASSERT_NE(nullptr, rb);
	EXPECT_DOUBLE_EQ(10.0, rb->total);
	EXPECT_DOUBLE_EQ(4.0, rb->newPos);
	EXPECT_DOUBLE_EQ(6.0, rb->prePos);
	EXPECT_DOUBLE_EQ(3.0, rb->availNew);		/* Today 4 - ShortFrozen 1 */
	EXPECT_DOUBLE_EQ(4.0, rb->availPre);		/* 4+6-2-1-3 */
	EXPECT_DOUBLE_EQ(170.0, rb->avg);		/* 17000/10/10 */
	EXPECT_DOUBLE_EQ(500.0, rb->margin);
	EXPECT_EQ(WDT_LONG, rb->direction);

	const R61OptSink::PosCap* sp = findPos(sink.positions, "sp2610");
	ASSERT_NE(nullptr, sp);
	EXPECT_DOUBLE_EQ(2.0, sp->total);
	EXPECT_DOUBLE_EQ(0.0, sp->availNew);		/* 组合分支不设置，保持默认 0 */
	EXPECT_DOUBLE_EQ(0.0, sp->availPre);

	const R61OptSink::PosCap* neg = findPos(sink.positions, "neg2610");
	ASSERT_NE(nullptr, neg);
	EXPECT_DOUBLE_EQ(-3.0, neg->total);
	EXPECT_DOUBLE_EQ(0.0, neg->availNew);
	EXPECT_DOUBLE_EQ(0.0, neg->availPre);
	EXPECT_DOUBLE_EQ(0.0, neg->avg);

	/* map 已在末笔释放；错误回报不再入表 */
	CThostFtdcRspInfoField err = r61_opt_ok();
	err.ErrorID = 7;
	std::snprintf(p.InstrumentID, sizeof(p.InstrumentID), "%s", "rb2610");
	probe.OnRspQryInvestorPosition(&p, &err, 6, false);
}

/* 产品缺陷占位：OnRspQryInvestorPosition 在 contract==NULL 时提前 return
 * （TraderCTPOpt.cpp:1177-1179），跳过了函数尾部 bIsLast 的聚合与
 * onRspPosition 回调；如果查询结果的最后一行来自未登记合约，整批持仓全部丢失。
 * 见 docs/迭代计划/迭代01/bugs/R65.md BUG-R65-002（历史注释曾引用 BUG-R57-003）；默认不执行。 */
TEST(R61_TraderCTPOpt, DISABLED_position_query_last_row_unknown_still_flushes)
{
	R61OptContract fx("rb", "rb2610", "SHFE", CM_OpenCover, CC_Future, 10);
	R61OptSink sink;
	R61OptDataMgr mgr;
	mgr.add(fx.ct);
	sink.mgr = &mgr;

	R61OptProbe probe;
	probe.harden();
	probe.registerSpi(&sink);

	CThostFtdcInvestorPositionField p;
	std::memset(&p, 0, sizeof(p));
	std::snprintf(p.InstrumentID, sizeof(p.InstrumentID), "%s", "rb2610");
	std::snprintf(p.ExchangeID, sizeof(p.ExchangeID), "%s", "SHFE");
	p.PositionDate = THOST_FTDC_PSD_History;
	p.Position = 10;
	p.TodayPosition = 4;
	probe.OnRspQryInvestorPosition(&p, nullptr, 1, false);

	std::memset(&p, 0, sizeof(p));
	std::snprintf(p.InstrumentID, sizeof(p.InstrumentID), "%s", "ghost");
	std::snprintf(p.ExchangeID, sizeof(p.ExchangeID), "%s", "SHFE");
	probe.OnRspQryInvestorPosition(&p, nullptr, 2, true);

	ASSERT_EQ(1u, sink.positions.size());
	EXPECT_EQ(std::string("rb2610"), sink.positions[0].code);
}
