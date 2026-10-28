/*
 * 覆盖率用例：src/TraderCTPOpt（lane T1 / 第六波·收发族）
 * 形状参照 t_TraderMocker/seam_tradermocker_test.cpp：构造 adapter + 走公开接口；
 * 转换函数/内部状态通过「受控派生探针」（protected 成员对派生类可见）访问，
 * 不使用 #define private public。厂商回调全部用手工构造的结构体直接驱动。
 * 依赖真实 soptthosttraderapi 库或会拉起 worker 线程的路径以 DISABLED_ 保留，
 * 触发条件见 docs/迭代计划/迭代01/bugs/T1_ctpopt_parserxtp.md。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <tuple>
#include <vector>

#include "TraderCTPOpt/TraderCTPOpt.h"

#include <WTSVariant.hpp>
#include <WTSDataDef.hpp>
#include <WTSTradeDef.hpp>
#include <WTSContractInfo.hpp>
#include <IBaseDataMgr.h>
#include <TimeUtils.hpp>

USING_NS_WTP;

/* TraderCTPOpt.cpp 中的文件级 inline 换算函数（外部链接弱符号），跨 TU 声明直调 */
int wrapDirectionType(WTSDirectionType dirType, WTSOffsetType offsetType);
WTSDirectionType wrapDirectionType(TThostFtdcDirectionType dirType, TThostFtdcOffsetFlagType offsetType);
int wrapPosDirType(WTSDirectionType dirType);
WTSDirectionType wrapPosDirType(TThostFtdcPosiDirectionType dirType);
WTSDirectionType wrapPosDirection(TThostFtdcPosiDirectionType dirType);
int wrapOffsetType(WTSOffsetType offType);
WTSOffsetType wrapOffsetType(TThostFtdcOffsetFlagType offType);
int wrapPriceType(WTSPriceType priceType, bool isCFFEX);
WTSPriceType wrapPriceType(TThostFtdcOrderPriceTypeType priceType);
// 主控修正（原为 T1 遗留）：`wrapTimeCondition` 在 TraderCTPOpt.cpp:184/194 是 **TU 内 inline**，
// 跨 TU 再声明成 extern 函数会留下无人定义的引用 —— 语法门禁过、链接必挂
// （实测 ninja: Undefined symbols: wrapTimeCondition(wtp::tagTimeCondition) / (char)）。
// 该映射要能被单测断言，需要产品把它移进头文件或改成成员（属接缝诉求，见 覆盖排除清单/接缝阻塞面）。
WTSOrderState wrapOrderState(TThostFtdcOrderStatusType orderState);
int wrapActionFlag(WTSActionFlag actionFlag);

namespace
{
	/* 受控探针：把 protected 换算/状态入口开放给测试，并把构造期未初始化的
	   指针成员硬化为确定值（缺陷本体仍记录在 bug 文件里） */
	class CtpProbe : public TraderCTPOpt
	{
	public:
		using TraderCTPOpt::IsErrorRspInfo;
		using TraderCTPOpt::makeOrderInfo;
		using TraderCTPOpt::makeEntrust;
		using TraderCTPOpt::makeError;
		using TraderCTPOpt::makeTradeRecord;
		using TraderCTPOpt::generateEntrustID;
		using TraderCTPOpt::extractEntrustID;
		using TraderCTPOpt::genRequestID;
		/* 注意：queryConfirm()/confirm() 在 TraderCTPOpt.h 中是 private（L49-55），
		   继承缝不可达；其守卫分支只经 OnRspQrySettlementInfoConfirm 内部调用间接覆盖。 */

		void harden()
		{
			m_frontID = 0;
			m_sessionID = 0;
			m_orderRef = 0;
			m_ayExecOrds = NULL;
			m_bdMgr = NULL;
			m_mapPosition = NULL;
			m_ayTrades = NULL;
			m_ayOrders = NULL;
			m_ayPosDetail = NULL;
			m_wrapperState = WS_NOTLOGIN;
			m_lDate = 0;
			m_bInQuery = false;
			m_pUserAPI = NULL;
			m_bscSink = NULL;
			m_optSink = NULL;
			m_strSettleInfo.clear();
		}

		void setState(WrapperState s) { m_wrapperState = s; }
		void setIds(int f, int s, uint32_t ref)
		{
			m_frontID = f;
			m_sessionID = s;
			m_orderRef = ref;
		}

		WrapperState state() const { return m_wrapperState; }
		const std::string& front() const { return m_strFront; }
		const std::string& broker() const { return m_strBroker; }
		const std::string& user() const { return m_strUser; }
		const std::string& pass() const { return m_strPass; }
		const std::string& appid() const { return m_strAppID; }
		const std::string& authcode() const { return m_strAuthCode; }
		const std::string& flowdir() const { return m_strFlowDir; }
		const std::string& module() const { return m_strModule; }
		bool quickStart() const { return m_bQuickStart; }
		uint32_t tradeDate() const { return m_lDate; }
		IBaseDataMgr* bdMgr() const { return m_bdMgr; }
		TraderCTPOpt::PositionMap* mapPos() const { return m_mapPosition; }
		WTSArray* ayExecOrds() const { return m_ayExecOrds; }
		WTSArray* ayOrders() const { return m_ayOrders; }
	};

	class FakeTraderSpi : public ITraderSpi
	{
	public:
		struct EntrustCap
		{
			std::string code, exchg, entrustID, userTag;
			double price = 0, volume = 0;
			int direction = -1, priceType = -1, offsetType = -1, orderFlag = -1, businessType = -1;
			int errCode = -1;
			std::string errMsg;
		};
		struct OrderCap
		{
			std::string code, entrustID, orderID, userTag;
			double price = 0, volume = 0, volTraded = 0, volLeft = 0;
			uint32_t orderDate = 0;
			uint64_t orderTime = 0;
			int state = -1, direction = -1, offsetType = -1, businessType = -1, orderFlag = -1;
		};
		struct TradeCap
		{
			std::string code, tradeID, refOrder;
			double price = 0, volume = 0;
			int direction = -1, offsetType = -1, tradeType = -1;
		};

		IBaseDataMgr* getBaseDataMgr() override { return bdMgr; }
		IOptTraderSpi* getOptSpi() override { return optSpi; }
		void handleEvent(WTSTraderEvent e, int32_t ec) override { events.emplace_back(e, ec); }
		void onLoginResult(bool bSucc, const char* msg, uint32_t tradingdate) override
		{
			logins.emplace_back(bSucc, msg, tradingdate);
		}
		void onRspEntrust(WTSEntrust* entrust, WTSError* err) override
		{
			EntrustCap c;
			c.code = entrust->getCode();
			c.price = entrust->getPrice();
			c.volume = entrust->getVolume();
			c.direction = entrust->getDirection();
			c.priceType = entrust->getPriceType();
			c.offsetType = entrust->getOffsetType();
			c.orderFlag = entrust->getOrderFlag();
			c.businessType = entrust->getBusinessType();
			c.entrustID = entrust->getEntrustID();
			c.userTag = entrust->getUserTag();
			if (err != NULL)
			{
				c.errCode = err->getErrorCode();
				c.errMsg = err->getMessage();
			}
			rspEntrusts.push_back(c);
		}
		void onRspAccount(WTSArray* ayAccounts) override
		{
			accountCalls++;
			lastAccountSize = (ayAccounts == NULL) ? 0u : ayAccounts->size();
		}
		void onRspPosition(const WTSArray* ayPositions) override
		{
			positionCalls++;
			lastPositionSize = (ayPositions == NULL) ? 0u : ayPositions->size();
		}
		void onRspOrders(const WTSArray* ayOrders) override
		{
			orderCalls++;
			lastOrderSize = (ayOrders == NULL) ? 0u : ayOrders->size();
			lastOrdersWasNull = (ayOrders == NULL);
		}
		void onRspTrades(const WTSArray* ayTrades) override
		{
			tradeCalls++;
			lastTradeSize = (ayTrades == NULL) ? 0u : ayTrades->size();
		}
		void onRspSettlementInfo(uint32_t uDate, const char* content) override
		{
			settleDate = uDate;
			settleContent = content;
			settleCalls++;
		}
		void onPushOrder(WTSOrderInfo* orderInfo) override
		{
			OrderCap c;
			c.code = orderInfo->getCode();
			c.price = orderInfo->getPrice();
			c.volume = orderInfo->getVolume();
			c.volTraded = orderInfo->getVolTraded();
			c.volLeft = orderInfo->getVolLeft();
			c.direction = orderInfo->getDirection();
			c.offsetType = orderInfo->getOffsetType();
			c.orderFlag = orderInfo->getOrderFlag();
			c.state = orderInfo->getOrderState();
			c.entrustID = orderInfo->getEntrustID();
			c.orderID = orderInfo->getOrderID();
			c.userTag = orderInfo->getUserTag();
			c.orderDate = orderInfo->getOrderDate();
			c.orderTime = orderInfo->getOrderTime();
			pushOrders.push_back(c);
		}
		void onPushTrade(WTSTradeInfo* tradeRecord) override
		{
			TradeCap c;
			c.code = tradeRecord->getCode();
			c.price = tradeRecord->getPrice();
			c.volume = tradeRecord->getVolume();
			c.direction = tradeRecord->getDirection();
			c.offsetType = tradeRecord->getOffsetType();
			c.tradeType = tradeRecord->getTradeType();
			c.tradeID = tradeRecord->getTradeID();
			c.refOrder = tradeRecord->getRefOrder();
			pushTrades.push_back(c);
		}
		void onTraderError(WTSError* err, void* /*pData*/) override
		{
			traderErrors.emplace_back(err->getErrorCode(), err->getMessage());
		}
		void onPushInstrumentStatus(const char* exchg, const char* code, WTSTradeStatus state) override
		{
			instrStatus.emplace_back(exchg, code, (int)state);
		}
		void handleTraderLog(WTSLogLevel ll, const char* message) override
		{
			logs.emplace_back(ll, message);
		}

		bool hasLog(const char* needle) const
		{
			for (const auto& lg : logs)
				if (lg.second.find(needle) != std::string::npos)
					return true;
			return false;
		}

		IBaseDataMgr* bdMgr = nullptr;
		IOptTraderSpi* optSpi = nullptr;
		std::vector<std::pair<WTSTraderEvent, int32_t>> events;
		std::vector<std::tuple<bool, std::string, uint32_t>> logins;
		std::vector<EntrustCap> rspEntrusts;
		std::vector<OrderCap> pushOrders;
		std::vector<TradeCap> pushTrades;
		std::vector<std::pair<int, std::string>> traderErrors;
		std::vector<std::tuple<std::string, std::string, int>> instrStatus;
		std::vector<std::pair<WTSLogLevel, std::string>> logs;
		uint32_t accountCalls = 0, positionCalls = 0, orderCalls = 0, tradeCalls = 0, settleCalls = 0;
		uint32_t lastAccountSize = 0, lastPositionSize = 0, lastOrderSize = 0, lastTradeSize = 0;
		bool lastOrdersWasNull = false;
		uint32_t settleDate = 0;
		std::string settleContent;
	};

	class FakeOptSpi : public IOptTraderSpi
	{
	public:
		void onRspEntrustOpt(WTSEntrust* entrust, WTSError* err) override
		{
			FakeTraderSpi::EntrustCap c;
			c.code = entrust->getCode();
			c.volume = entrust->getVolume();
			c.direction = entrust->getDirection();
			c.offsetType = entrust->getOffsetType();
			c.businessType = entrust->getBusinessType();
			c.entrustID = entrust->getEntrustID();
			if (err != NULL)
			{
				c.errCode = err->getErrorCode();
				c.errMsg = err->getMessage();
			}
			rspEntrustOpts.push_back(c);
		}
		void onRspOrdersOpt(const WTSArray* ayOrders) override
		{
			ordersOptCalls++;
			lastOrdersOptNull = (ayOrders == NULL);
			lastOrdersOptSize = (ayOrders == NULL) ? 0u : ayOrders->size();
		}
		void onPushOrderOpt(WTSOrderInfo* orderInfo) override
		{
			FakeTraderSpi::OrderCap c;
			c.code = orderInfo->getCode();
			c.volume = orderInfo->getVolume();
			c.direction = orderInfo->getDirection();
			c.offsetType = orderInfo->getOffsetType();
			c.orderFlag = orderInfo->getOrderFlag();
			c.state = orderInfo->getOrderState();
			c.businessType = orderInfo->getBusinessType();
			c.entrustID = orderInfo->getEntrustID();
			c.orderID = orderInfo->getOrderID();
			pushOrderOpts.push_back(c);
		}

		std::vector<FakeTraderSpi::EntrustCap> rspEntrustOpts;
		std::vector<FakeTraderSpi::OrderCap> pushOrderOpts;
		uint32_t ordersOptCalls = 0;
		bool lastOrdersOptNull = false;
		uint32_t lastOrdersOptSize = 0;
	};

	class CtpBaseDataMgr : public IBaseDataMgr
	{
	public:
		WTSCommodityInfo* getCommodity(const char*) override { return nullptr; }
		WTSCommodityInfo* getCommodity(const char*, const char*) override { return nullptr; }
		WTSContractInfo* getContract(const char* code, const char* /*exchg*/ = "", uint32_t /*uDate*/ = 0) override
		{
			if (contract == nullptr)
				return nullptr;
			if (std::strcmp(code, contract->getCode()) != 0)
				return nullptr;
			return contract;
		}
		WTSArray* getContracts(const char* = "", uint32_t = 0) override { return nullptr; }
		WTSSessionInfo* getSession(const char*) override { return nullptr; }
		WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return nullptr; }
		WTSArray* getAllSessions() override { return nullptr; }
		bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
		uint32_t calcTradingDate(const char*, uint32_t, uint32_t, bool = false) override { return 0; }
		uint64_t getBoundaryTime(const char*, uint32_t, bool = false, bool = true) override { return 0; }

		WTSContractInfo* contract = nullptr;
	};

	/* 期货合约夹具：CTPOpt 场景走「看涨多头持仓」样例 */
	struct CtpFixture
	{
		CtpFixture()
		{
			comm = WTSCommodityInfo::create("rb", "螺纹钢", "SSE", "SY241", "trading");
			comm->setVolScale(10);
			comm->setCategory(CC_Future);
			comm->setCoverMode(CM_CoverToday);
			ct = WTSContractInfo::create("rb2610", "螺纹钢2610", "SSE", "rb");
			ct->setCommInfo(comm);
			bd.contract = ct;
			spi.bdMgr = &bd;
			spi.optSpi = &opt;
		}
		~CtpFixture()
		{
			if (ct != nullptr) ct->release();
			if (comm != nullptr) comm->release();
		}
		WTSCommodityInfo* comm = nullptr;
		WTSContractInfo* ct = nullptr;
		CtpBaseDataMgr bd;
		FakeTraderSpi spi;
		FakeOptSpi opt;
	};

	extern "C" ITraderApi* createTrader();
	extern "C" void deleteTrader(ITraderApi*& trader);

	CThostFtdcRspInfoField makeRsp(int errId, const char* msg)
	{
		CThostFtdcRspInfoField f;
		std::memset(&f, 0, sizeof(f));
		f.ErrorID = errId;
		std::snprintf(f.ErrorMsg, sizeof(f.ErrorMsg), "%s", msg);
		return f;
	}
}

TEST(TraderCTPOpt, factory_entries_create_and_delete_trader)
{
	ITraderApi* trader = createTrader();
	ASSERT_NE(nullptr, trader);
	EXPECT_NE(nullptr, dynamic_cast<TraderCTPOpt*>(trader));
	deleteTrader(trader);
	EXPECT_EQ(nullptr, trader);

	ITraderApi* nullTrader = nullptr;
	deleteTrader(nullTrader);
	EXPECT_EQ(nullptr, nullTrader);
}

TEST(TraderCTPOpt, init_reads_config_with_documented_defaults)
{
	CtpProbe probe;
	probe.harden();

	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("front", "tcp://1.2.3.4:1234");
	cfg->append("broker", "9999");
	cfg->append("user", "wesley");
	cfg->append("pass", "pwd123");
	cfg->append("appid", "testapp");
	cfg->append("authcode", "auth42");
	/* flowdir / ctpmodule / quick 均缺省 → 走默认分支 */
	EXPECT_TRUE(probe.init(cfg));
	probe.release();

	EXPECT_EQ("tcp://1.2.3.4:1234", probe.front());
	EXPECT_EQ("9999", probe.broker());
	EXPECT_EQ("wesley", probe.user());
	EXPECT_EQ("pwd123", probe.pass());
	EXPECT_EQ("testapp", probe.appid());
	EXPECT_EQ("auth42", probe.authcode());
	EXPECT_NE(std::string::npos, probe.flowdir().find("CTPOptTDFlow"));
	EXPECT_NE(std::string::npos, probe.module().find("soptthosttraderapi_se"));
	EXPECT_FALSE(probe.quickStart());
	EXPECT_FALSE(probe.isConnected());
	cfg->release();
}

TEST(TraderCTPOpt, init_honours_custom_flowdir_module_and_quickstart)
{
	CtpProbe probe;
	probe.harden();

	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("front", "tcp://5.6.7.8:1234");
	cfg->append("broker", "8888");
	cfg->append("user", "u2");
	cfg->append("pass", "p2");
	cfg->append("flowdir", "myctpflow");
	cfg->append("ctpmodule", "customapi");
	cfg->append("quick", true);
	EXPECT_TRUE(probe.init(cfg));

	EXPECT_NE(std::string::npos, probe.flowdir().find("myctpflow"));
	EXPECT_NE(std::string::npos, probe.module().find("customapi"));
	EXPECT_TRUE(probe.quickStart());
	probe.release();
	cfg->release();
}

TEST(TraderCTPOpt, make_entrust_id_writes_zero_padded_triple_and_increments)
{
	CtpProbe probe;
	probe.harden();
	probe.setIds(1, 2, 10);

	char bad[32];
	EXPECT_FALSE(probe.makeEntrustID(NULL, 32));
	EXPECT_FALSE(probe.makeEntrustID(bad, 0));

	char buf[64];
	std::memset(buf, 0, sizeof(buf));
	EXPECT_TRUE(probe.makeEntrustID(buf, sizeof(buf)));
	EXPECT_STREQ("000001#0000000002#000011", buf);

	std::memset(buf, 0, sizeof(buf));
	EXPECT_TRUE(probe.makeEntrustID(buf, sizeof(buf)));
	EXPECT_STREQ("000001#0000000002#000012", buf);

	char gen[64] = { 0 };
	probe.generateEntrustID(gen, 7, 8, 42);
	EXPECT_STREQ("000007#0000000008#000042", gen);

	uint32_t f = 0, s = 0, r = 0;
	EXPECT_TRUE(probe.extractEntrustID(gen, f, s, r));
	EXPECT_EQ(7u, f);
	EXPECT_EQ(8u, s);
	EXPECT_EQ(42u, r);

	EXPECT_FALSE(probe.extractEntrustID("nodelim", f, s, r));
	/* 只有分隔符一次：第二段没有 '#'，必须拒绝 */
	EXPECT_FALSE(probe.extractEntrustID("1#2", f, s, r));
}

TEST(TraderCTPOpt, register_spi_wires_basedata_mgr_and_opt_spi)
{
	CtpProbe probe;
	probe.harden();
	CtpFixture fx;

	probe.registerSpi(&fx.spi);
	EXPECT_EQ(&fx.bd, probe.bdMgr());
	EXPECT_NE(nullptr, probe.bdMgr());

	/* 空 sink 也必须可安全注册 */
	probe.registerSpi(NULL);
	EXPECT_EQ(nullptr, probe.bdMgr());
}

TEST(TraderCTPOpt, unlogged_state_rejects_every_request_with_minus_one)
{
	CtpProbe probe;
	probe.harden();
	CtpFixture fx;
	probe.registerSpi(&fx.spi);

	WTSEntrust* ent = WTSEntrust::create("rb2610", 1, 3500.0, "SSE");
	WTSEntrustAction* act = WTSEntrustAction::create("rb2610", "SSE");
	act->setActionFlag(WAF_CANCEL);

	EXPECT_EQ(-1, probe.login("u", "p", "prod"));
	EXPECT_EQ("u", probe.user());
	EXPECT_EQ("p", probe.pass());
	EXPECT_EQ(-1, probe.logout());
	EXPECT_EQ(-1, probe.orderInsert(ent));
	EXPECT_EQ(-1, probe.orderAction(act));
	EXPECT_EQ(-1, probe.orderInsertOpt(ent));
	EXPECT_EQ(-1, probe.orderActionOpt(act));
	EXPECT_EQ(-1, probe.queryOrdersOpt(BT_EXECUTE));
	EXPECT_EQ(-1, probe.queryAccount());
	EXPECT_EQ(-1, probe.queryPositions());
	EXPECT_EQ(-1, probe.queryOrders());
	EXPECT_EQ(-1, probe.queryTrades());
	EXPECT_EQ(-1, probe.querySettlement(20260105));
	EXPECT_FALSE(probe.isConnected());

	/* 即便伪造「已登录」状态，没有 api 指针依然全部拒绝 */
	probe.setState(TraderCTPOpt::WS_LOGINED);
	EXPECT_EQ(-1, probe.orderInsert(ent));
	probe.setState(TraderCTPOpt::WS_ALLREADY);
	EXPECT_EQ(-1, probe.queryAccount());
	EXPECT_EQ(-1, probe.queryPositions());
	EXPECT_EQ(-1, probe.queryOrders());
	EXPECT_EQ(-1, probe.queryTrades());
	EXPECT_EQ(-1, probe.querySettlement(20260105));
	EXPECT_EQ(-1, probe.orderInsertOpt(ent));
	EXPECT_EQ(-1, probe.queryOrdersOpt(BT_EXECUTE));

	ent->release();
	act->release();
	probe.registerSpi(NULL);
}

TEST(TraderCTPOpt, request_ids_increment_monotonically)
{
	CtpProbe probe;
	probe.harden();
	EXPECT_EQ(1u, probe.genRequestID());
	EXPECT_EQ(2u, probe.genRequestID());
	EXPECT_EQ(3u, probe.genRequestID());
}

TEST(TraderCTPOpt, front_lifecycle_events_forward_to_sink)
{
	CtpProbe probe;
	probe.harden();
	CtpFixture fx;
	probe.registerSpi(&fx.spi);

	probe.OnFrontConnected();
	probe.OnFrontDisconnected(42);
	probe.OnHeartBeatWarning(100);
	probe.OnRspError(NULL, 0, true);

	ASSERT_EQ(2u, fx.spi.events.size());
	EXPECT_EQ(WTE_Connect, fx.spi.events[0].first);
	EXPECT_EQ(0, fx.spi.events[0].second);
	EXPECT_EQ(WTE_Close, fx.spi.events[1].first);
	EXPECT_EQ(42, fx.spi.events[1].second);
	EXPECT_EQ(TraderCTPOpt::WS_NOTLOGIN, probe.state());
	EXPECT_FALSE(probe.isConnected());
	ASSERT_EQ(1u, fx.spi.logs.size());	/* 仅心跳日志；OnRspError 是显式空实现 */
	EXPECT_TRUE(fx.spi.hasLog("Heartbeating"));

	probe.registerSpi(NULL);
	probe.OnFrontConnected();			/* 空 sink 不得崩溃、不得产生事件 */
	EXPECT_EQ(2u, fx.spi.events.size());
}

TEST(TraderCTPOpt, authentication_and_login_failure_branches_report_to_sink)
{
	CtpProbe probe;
	probe.harden();
	CtpFixture fx;
	probe.registerSpi(&fx.spi);

	CThostFtdcRspInfoField authErr = makeRsp(103, "auth failed");
	probe.OnRspAuthenticate(NULL, &authErr, 1, true);
	EXPECT_EQ(TraderCTPOpt::WS_LOGINFAILED, probe.state());
	ASSERT_EQ(1u, fx.spi.logins.size());
	EXPECT_FALSE(std::get<0>(fx.spi.logins[0]));
	EXPECT_EQ("auth failed", std::get<1>(fx.spi.logins[0]));
	EXPECT_EQ(0u, std::get<2>(fx.spi.logins[0]));
	EXPECT_TRUE(fx.spi.hasLog("Authentiation failed"));

	CThostFtdcRspInfoField loginErr = makeRsp(70, "bad password");
	CThostFtdcRspUserLoginField loginField;
	std::memset(&loginField, 0, sizeof(loginField));
	probe.OnRspUserLogin(&loginField, &loginErr, 2, true);
	EXPECT_EQ(TraderCTPOpt::WS_LOGINFAILED, probe.state());
	ASSERT_EQ(2u, fx.spi.logins.size());
	EXPECT_EQ("bad password", std::get<1>(fx.spi.logins[1]));
	EXPECT_TRUE(fx.spi.hasLog("Login failed"));

	/* 登出回报：状态复位 + WTE_Logout 事件 */
	probe.OnRspUserLogout(NULL, NULL, 3, true);
	EXPECT_EQ(TraderCTPOpt::WS_NOTLOGIN, probe.state());
	ASSERT_EQ(3u, fx.spi.events.size() + 0);	/* 前两个事件 + 一个 logout 事件 */
	EXPECT_EQ(WTE_Logout, fx.spi.events[2].first);

	probe.registerSpi(NULL);
}

TEST(TraderCTPOpt, settlement_confirmation_state_machine_reaches_allready)
{
	CtpProbe probe;
	probe.harden();
	CtpFixture fx;
	probe.registerSpi(&fx.spi);

	/* 查询结算确认无返回数据 → WS_CONFIRM_QRYED，内部 confirm() 因 api 缺失被拒（-1 分支） */
	probe.OnRspQrySettlementInfoConfirm(NULL, NULL, 1, false);
	EXPECT_EQ(TraderCTPOpt::WS_CONFIRM_QRYED, probe.state());
	EXPECT_FALSE(probe.isConnected());

	/* 确认回报（无错误、字段非空、状态恰为 CONFIRM_QRYED）→ WS_ALLREADY + 登录成功回报 */
	CThostFtdcSettlementInfoConfirmField conf;
	std::memset(&conf, 0, sizeof(conf));
	probe.OnRspSettlementInfoConfirm(&conf, NULL, 2, true);
	EXPECT_EQ(TraderCTPOpt::WS_ALLREADY, probe.state());
	EXPECT_TRUE(probe.isConnected());
	ASSERT_EQ(1u, fx.spi.logins.size());
	EXPECT_TRUE(std::get<0>(fx.spi.logins[0]));
	EXPECT_EQ(0u, std::get<2>(fx.spi.logins[0]));

	/* 非 CONFIRM_QRYED 状态下的重复确认回报必须被忽略 */
	fx.spi.logins.clear();
	probe.OnRspSettlementInfoConfirm(&conf, NULL, 3, true);
	EXPECT_TRUE(fx.spi.logins.empty());

	/* 带日期的查询回报：ConfirmDate >= 交易日(0) → 直接 ALLREADY */
	CtpProbe probe2;
	probe2.harden();
	probe2.registerSpi(&fx.spi);
	std::snprintf(conf.ConfirmDate, sizeof(conf.ConfirmDate), "%s", "20260101");
	probe2.OnRspQrySettlementInfoConfirm(&conf, NULL, 4, true);
	EXPECT_EQ(TraderCTPOpt::WS_ALLREADY, probe2.state());
	EXPECT_TRUE(probe2.isConnected());
	EXPECT_TRUE(probe2.mapPos() == nullptr);

	probe.registerSpi(NULL);
	probe2.registerSpi(NULL);
}

TEST(TraderCTPOpt, wrap_tables_convert_wts_enums_to_ctp_codes)
{
	EXPECT_EQ((int)THOST_FTDC_D_Buy, wrapDirectionType(WDT_LONG, WOT_OPEN));
	EXPECT_EQ((int)THOST_FTDC_D_Sell, wrapDirectionType(WDT_SHORT, WOT_OPEN));
	EXPECT_EQ((int)THOST_FTDC_D_Sell, wrapDirectionType(WDT_LONG, WOT_CLOSE));
	EXPECT_EQ((int)THOST_FTDC_D_Buy, wrapDirectionType(WDT_SHORT, WOT_CLOSE));

	EXPECT_EQ(WDT_LONG, wrapDirectionType(THOST_FTDC_D_Buy, THOST_FTDC_OF_Open));
	EXPECT_EQ(WDT_SHORT, wrapDirectionType(THOST_FTDC_D_Buy, THOST_FTDC_OF_Close));
	EXPECT_EQ(WDT_SHORT, wrapDirectionType(THOST_FTDC_D_Sell, THOST_FTDC_OF_Open));
	EXPECT_EQ(WDT_LONG, wrapDirectionType(THOST_FTDC_D_Sell, THOST_FTDC_OF_CloseToday));

	EXPECT_EQ((int)THOST_FTDC_PD_Long, wrapPosDirType(WDT_LONG));
	EXPECT_EQ((int)THOST_FTDC_PD_Short, wrapPosDirType(WDT_SHORT));
	EXPECT_EQ((int)THOST_FTDC_PD_Net, wrapPosDirType(WDT_NET));
	EXPECT_EQ(WDT_LONG, wrapPosDirType(THOST_FTDC_PD_Long));
	EXPECT_EQ(WDT_SHORT, wrapPosDirType(THOST_FTDC_PD_Short));
	EXPECT_EQ(WDT_NET, wrapPosDirType(THOST_FTDC_PD_Net));
	EXPECT_EQ(WDT_LONG, wrapPosDirection(THOST_FTDC_PD_Long));
	EXPECT_EQ(WDT_SHORT, wrapPosDirection(THOST_FTDC_PD_Short));

	EXPECT_EQ((int)THOST_FTDC_OF_Open, wrapOffsetType(WOT_OPEN));
	EXPECT_EQ((int)THOST_FTDC_OF_Close, wrapOffsetType(WOT_CLOSE));
	EXPECT_EQ((int)THOST_FTDC_OF_CloseToday, wrapOffsetType(WOT_CLOSETODAY));
	EXPECT_EQ((int)THOST_FTDC_OF_Close, wrapOffsetType(WOT_CLOSEYESTERDAY));
	EXPECT_EQ((int)THOST_FTDC_OF_ForceClose, wrapOffsetType(WOT_FORCECLOSE));
	EXPECT_EQ(WOT_OPEN, wrapOffsetType(THOST_FTDC_OF_Open));
	EXPECT_EQ(WOT_CLOSE, wrapOffsetType(THOST_FTDC_OF_Close));
	EXPECT_EQ(WOT_CLOSETODAY, wrapOffsetType(THOST_FTDC_OF_CloseToday));
	EXPECT_EQ(WOT_FORCECLOSE, wrapOffsetType('Z'));	/* 未识别字符兜底为强平 */

	EXPECT_EQ((int)THOST_FTDC_OPT_AnyPrice, wrapPriceType(WPT_ANYPRICE, false));
	EXPECT_EQ((int)THOST_FTDC_OPT_FiveLevelPrice, wrapPriceType(WPT_ANYPRICE, true));
	EXPECT_EQ((int)THOST_FTDC_OPT_LimitPrice, wrapPriceType(WPT_LIMITPRICE, false));
	EXPECT_EQ((int)THOST_FTDC_OPT_BestPrice, wrapPriceType(WPT_BESTPRICE, false));
	EXPECT_EQ((int)THOST_FTDC_OPT_LastPrice, wrapPriceType(WPT_LASTPRICE, false));
	EXPECT_EQ(WPT_ANYPRICE, wrapPriceType(THOST_FTDC_OPT_AnyPrice));
	EXPECT_EQ(WPT_ANYPRICE, wrapPriceType(THOST_FTDC_OPT_FiveLevelPrice));
	EXPECT_EQ(WPT_LIMITPRICE, wrapPriceType(THOST_FTDC_OPT_LimitPrice));
	EXPECT_EQ(WPT_BESTPRICE, wrapPriceType(THOST_FTDC_OPT_BestPrice));
	EXPECT_EQ(WPT_LASTPRICE, wrapPriceType(THOST_FTDC_OPT_LastPricePlusOneTicks));

	// 同上：这 6 条断言针对的是 TU 内 inline 的映射，无法从测试 TU 调用；已随声明一并移除。

	EXPECT_EQ(WOS_Submitting, wrapOrderState(THOST_FTDC_OST_Unknown));
	EXPECT_EQ(WOS_AllTraded, wrapOrderState(THOST_FTDC_OST_AllTraded));
	EXPECT_EQ(WOS_Canceled, wrapOrderState(THOST_FTDC_OST_Canceled));

	EXPECT_EQ((int)THOST_FTDC_AF_Delete, wrapActionFlag(WAF_CANCEL));
	EXPECT_EQ((int)THOST_FTDC_AF_Modify, wrapActionFlag(WAF_MODIFY));
}

TEST(TraderCTPOpt, make_entrust_converts_input_order_with_ioc_variants)
{
	CtpProbe probe;
	probe.harden();
	CtpFixture fx;
	probe.registerSpi(&fx.spi);
	probe.setIds(7, 8, 0);

	probe.registerSpi(&fx.spi);
	probe.setIds(7, 8, 0);

	CThostFtdcInputOrderField ord;
	std::memset(&ord, 0, sizeof(ord));
	std::snprintf(ord.InstrumentID, sizeof(ord.InstrumentID), "%s", "rb2610");
	std::snprintf(ord.ExchangeID, sizeof(ord.ExchangeID), "%s", "SSE");
	ord.LimitPrice = 3500.0;
	ord.VolumeTotalOriginal = 5;
	ord.Direction = THOST_FTDC_D_Buy;
	ord.CombOffsetFlag[0] = THOST_FTDC_OF_Open;
	ord.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
	ord.TimeCondition = THOST_FTDC_TC_GFD;
	ord.VolumeCondition = THOST_FTDC_VC_AV;
	std::snprintf(ord.OrderRef, sizeof(ord.OrderRef), "%s", "123");

	WTSEntrust* conv = probe.makeEntrust(&ord);
	ASSERT_NE(nullptr, conv);
	EXPECT_STREQ("rb2610", conv->getCode());
	EXPECT_DOUBLE_EQ(3500.0, conv->getPrice());
	EXPECT_DOUBLE_EQ(5.0, conv->getVolume());
	EXPECT_EQ(WDT_LONG, conv->getDirection());
	EXPECT_EQ(WPT_LIMITPRICE, conv->getPriceType());
	EXPECT_EQ(WOT_OPEN, conv->getOffsetType());
	EXPECT_EQ(WOF_NOR, conv->getOrderFlag());
	EXPECT_STREQ("000007#0000000008#000123", conv->getEntrustID());
	conv->release();

	/* IOC + VC_MV → FAK；IOC + VC_CV → FOK */
	ord.TimeCondition = THOST_FTDC_TC_IOC;
	ord.VolumeCondition = THOST_FTDC_VC_MV;
	conv = probe.makeEntrust(&ord);
	ASSERT_NE(nullptr, conv);
	EXPECT_EQ(WOF_FAK, conv->getOrderFlag());
	conv->release();

	ord.VolumeCondition = THOST_FTDC_VC_CV;
	conv = probe.makeEntrust(&ord);
	ASSERT_NE(nullptr, conv);
	EXPECT_EQ(WOF_FOK, conv->getOrderFlag());
	conv->release();

	/* 未知合约 → NULL；OnRspOrderInsert 对该单不透传 */
	std::snprintf(ord.InstrumentID, sizeof(ord.InstrumentID), "%s", "ZZZZ99");
	EXPECT_EQ(nullptr, probe.makeEntrust(&ord));
	ord.TimeCondition = THOST_FTDC_TC_GFD;
	std::snprintf(ord.InstrumentID, sizeof(ord.InstrumentID), "%s", "rb2610");

	CThostFtdcRspInfoField err = makeRsp(55, "risk denied");
	probe.OnRspOrderInsert(&ord, &err, 9, true);
	ASSERT_EQ(1u, fx.spi.rspEntrusts.size());
	EXPECT_EQ(55, fx.spi.rspEntrusts[0].errCode);
	EXPECT_EQ("risk denied", fx.spi.rspEntrusts[0].errMsg);
	EXPECT_STREQ("rb2610", fx.spi.rspEntrusts[0].code.c_str());
	EXPECT_STREQ("000007#0000000008#000123", fx.spi.rspEntrusts[0].entrustID.c_str());

	probe.OnErrRtnOrderInsert(&ord, &err);
	ASSERT_EQ(2u, fx.spi.rspEntrusts.size());
	EXPECT_EQ(55, fx.spi.rspEntrusts[1].errCode);

	probe.registerSpi(NULL);
}

TEST(TraderCTPOpt, make_order_info_maps_ctp_order_fields_to_wts)
{
	CtpProbe probe;
	probe.harden();
	CtpFixture fx;
	probe.registerSpi(&fx.spi);

	CThostFtdcOrderField of;
	std::memset(&of, 0, sizeof(of));
	std::snprintf(of.InstrumentID, sizeof(of.InstrumentID), "%s", "rb2610");
	std::snprintf(of.ExchangeID, sizeof(of.ExchangeID), "%s", "SSE");
	of.LimitPrice = 3600.0;
	of.VolumeTotalOriginal = 4;
	of.VolumeTraded = 1;
	of.Direction = THOST_FTDC_D_Sell;
	of.CombOffsetFlag[0] = THOST_FTDC_OF_CloseToday;
	of.OrderPriceType = THOST_FTDC_OPT_AnyPrice;
	of.TimeCondition = THOST_FTDC_TC_GFD;
	of.OrderStatus = THOST_FTDC_OST_PartTradedQueueing;
	of.OrderSubmitStatus = THOST_FTDC_OSS_InsertRejected;
	of.FrontID = 7;
	of.SessionID = 8;
	std::snprintf(of.OrderRef, sizeof(of.OrderRef), "%s", "9");
	std::snprintf(of.InsertDate, sizeof(of.InsertDate), "%s", "20260105");
	std::snprintf(of.InsertTime, sizeof(of.InsertTime), "%s", "09:30:01");
	std::snprintf(of.OrderSysID, sizeof(of.OrderSysID), "%s", " 10086 ");
	std::snprintf(of.StatusMsg, sizeof(of.StatusMsg), "%s", "partial filled");

	WTSOrderInfo* oi = probe.makeOrderInfo(&of);
	ASSERT_NE(nullptr, oi);
	EXPECT_DOUBLE_EQ(3600.0, oi->getPrice());
	EXPECT_DOUBLE_EQ(4.0, oi->getVolume());
	EXPECT_DOUBLE_EQ(1.0, oi->getVolTraded());
	EXPECT_DOUBLE_EQ(3.0, oi->getVolLeft());
	EXPECT_EQ(WDT_LONG, oi->getDirection());		/* Sell + CloseToday：wrapDirectionType 兜底做多（见换算表用例） */
	EXPECT_EQ(WOT_CLOSETODAY, oi->getOffsetType());
	EXPECT_EQ(WPT_ANYPRICE, oi->getPriceType());
	EXPECT_EQ(WOF_NOR, oi->getOrderFlag());
	EXPECT_EQ(WOS_PartTraded_Queuing, oi->getOrderState());
	EXPECT_STREQ("000007#0000000008#000009", oi->getEntrustID());
	EXPECT_STREQ(" 10086 ", oi->getOrderID());
	EXPECT_EQ(20260105u, oi->getOrderDate());
	EXPECT_EQ(TimeUtils::makeTime(20260105, 93001 * 1000), oi->getOrderTime());
	/* ini 无标签 → userTag 回退为 entrustID */
	EXPECT_STREQ(oi->getEntrustID(), oi->getUserTag());
	oi->release();

	/* 未知合约 → NULL */
	std::snprintf(of.InstrumentID, sizeof(of.InstrumentID), "%s", "NOPE01");
	EXPECT_EQ(nullptr, probe.makeOrderInfo(&of));

	probe.registerSpi(NULL);
}

TEST(TraderCTPOpt, make_trade_record_and_error_conversions)
{
	CtpProbe probe;
	probe.harden();
	CtpFixture fx;
	probe.registerSpi(&fx.spi);

	CThostFtdcTradeField tf;
	std::memset(&tf, 0, sizeof(tf));
	std::snprintf(tf.InstrumentID, sizeof(tf.InstrumentID), "%s", "rb2610");
	std::snprintf(tf.ExchangeID, sizeof(tf.ExchangeID), "%s", "SSE");
	tf.Price = 3610.0;
	tf.Volume = 2;
	std::snprintf(tf.TradeID, sizeof(tf.TradeID), "%s", "T777");
	std::snprintf(tf.OrderSysID, sizeof(tf.OrderSysID), "%s", "10086");
	std::snprintf(tf.TradeDate, sizeof(tf.TradeDate), "%s", "20260105");
	std::snprintf(tf.TradeTime, sizeof(tf.TradeTime), "%s", "10:00:00");
	tf.Direction = THOST_FTDC_D_Buy;
	tf.OffsetFlag = THOST_FTDC_OF_CloseToday;
	tf.TradeType = '0';

	WTSTradeInfo* tr = probe.makeTradeRecord(&tf);
	ASSERT_NE(nullptr, tr);
	EXPECT_STREQ("rb2610", tr->getCode());
	EXPECT_DOUBLE_EQ(3610.0, tr->getPrice());
	EXPECT_DOUBLE_EQ(2.0, tr->getVolume());
	EXPECT_STREQ("T777", tr->getTradeID());
	EXPECT_STREQ("10086", tr->getRefOrder());
	/* Buy + CloseToday → WDT_SHORT（买平=做空方向） */
	EXPECT_EQ(WDT_SHORT, tr->getDirection());
	EXPECT_EQ(WOT_CLOSETODAY, tr->getOffsetType());
	EXPECT_EQ(TimeUtils::makeTime(20260105, 100000 * 1000), tr->getTradeTime());
	tr->release();

	std::snprintf(tf.InstrumentID, sizeof(tf.InstrumentID), "%s", "NOPE01");
	EXPECT_EQ(nullptr, probe.makeTradeRecord(&tf));

	CThostFtdcRspInfoField err = makeRsp(70, "not login");
	WTSError* e = probe.makeError(&err);
	ASSERT_NE(nullptr, e);
	EXPECT_EQ(70, e->getErrorCode());
	EXPECT_STREQ("not login", e->getMessage());
	e->release();

	CThostFtdcRspInfoField ok = makeRsp(0, "");
	EXPECT_FALSE(probe.IsErrorRspInfo(NULL));
	EXPECT_FALSE(probe.IsErrorRspInfo(&ok));
	EXPECT_TRUE(probe.IsErrorRspInfo(&err));

	probe.registerSpi(NULL);
}

TEST(TraderCTPOpt, rtn_order_and_trade_push_converted_records)
{
	CtpProbe probe;
	probe.harden();
	CtpFixture fx;
	probe.registerSpi(&fx.spi);

	CThostFtdcOrderField of;
	std::memset(&of, 0, sizeof(of));
	std::snprintf(of.InstrumentID, sizeof(of.InstrumentID), "%s", "rb2610");
	std::snprintf(of.ExchangeID, sizeof(of.ExchangeID), "%s", "SSE");
	of.LimitPrice = 3500.0;
	of.VolumeTotalOriginal = 3;
	of.VolumeTraded = 3;
	of.Direction = THOST_FTDC_D_Buy;
	of.CombOffsetFlag[0] = THOST_FTDC_OF_Open;
	of.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
	of.TimeCondition = THOST_FTDC_TC_IOC;
	of.VolumeCondition = THOST_FTDC_VC_CV;
	of.OrderStatus = THOST_FTDC_OST_AllTraded;
	of.FrontID = 1;
	of.SessionID = 2;
	std::snprintf(of.OrderRef, sizeof(of.OrderRef), "%s", "5");
	std::snprintf(of.InsertDate, sizeof(of.InsertDate), "%s", "20260106");
	std::snprintf(of.InsertTime, sizeof(of.InsertTime), "%s", "14:59:59");

	probe.OnRtnOrder(&of);
	ASSERT_EQ(1u, fx.spi.pushOrders.size());
	const auto& oc = fx.spi.pushOrders[0];
	EXPECT_EQ("rb2610", oc.code);
	EXPECT_DOUBLE_EQ(3500.0, oc.price);
	EXPECT_DOUBLE_EQ(0.0, oc.volLeft);
	EXPECT_EQ((int)WDT_LONG, oc.direction);
	EXPECT_EQ((int)WOS_AllTraded, oc.state);
	EXPECT_EQ((int)WOF_FOK, oc.orderFlag);	/* IOC + VC_CV → FOK */
	EXPECT_EQ("000001#0000000002#000005", oc.entrustID);

	CThostFtdcTradeField tf;
	std::memset(&tf, 0, sizeof(tf));
	std::snprintf(tf.InstrumentID, sizeof(tf.InstrumentID), "%s", "rb2610");
	std::snprintf(tf.ExchangeID, sizeof(tf.ExchangeID), "%s", "SSE");
	tf.Price = 3500.0;
	tf.Volume = 3;
	std::snprintf(tf.TradeID, sizeof(tf.TradeID), "%s", "T900");
	std::snprintf(tf.TradeDate, sizeof(tf.TradeDate), "%s", "20260106");
	std::snprintf(tf.TradeTime, sizeof(tf.TradeTime), "%s", "14:59:59");
	tf.Direction = THOST_FTDC_D_Buy;
	tf.OffsetFlag = THOST_FTDC_OF_Open;

	probe.OnRtnTrade(&tf);
	ASSERT_EQ(1u, fx.spi.pushTrades.size());
	EXPECT_EQ("T900", fx.spi.pushTrades[0].tradeID);
	EXPECT_DOUBLE_EQ(3.0, fx.spi.pushTrades[0].volume);
	EXPECT_EQ((int)WDT_LONG, fx.spi.pushTrades[0].direction);

	CThostFtdcInstrumentStatusField isf;
	std::memset(&isf, 0, sizeof(isf));
	std::snprintf(isf.ExchangeID, sizeof(isf.ExchangeID), "%s", "SSE");
	std::snprintf(isf.InstrumentID, sizeof(isf.InstrumentID), "%s", "rb2610");
	isf.InstrumentStatus = '0';
	probe.OnRtnInstrumentStatus(&isf);
	ASSERT_EQ(1u, fx.spi.instrStatus.size());
	EXPECT_EQ("SSE", std::get<0>(fx.spi.instrStatus[0]));
	EXPECT_EQ("rb2610", std::get<1>(fx.spi.instrStatus[0]));

	probe.registerSpi(NULL);
}

TEST(TraderCTPOpt, exec_order_paths_route_to_opt_spi)
{
	CtpProbe probe;
	probe.harden();
	CtpFixture fx;
	probe.registerSpi(&fx.spi);
	probe.setIds(3, 4, 0);

	CThostFtdcInputExecOrderField ieo;
	std::memset(&ieo, 0, sizeof(ieo));
	std::snprintf(ieo.InstrumentID, sizeof(ieo.InstrumentID), "%s", "rb2610");
	std::snprintf(ieo.ExchangeID, sizeof(ieo.ExchangeID), "%s", "SSE");
	ieo.Volume = 2;
	ieo.PosiDirection = THOST_FTDC_PD_Short;
	ieo.OffsetFlag = THOST_FTDC_OF_Close;
	std::snprintf(ieo.ExecOrderRef, sizeof(ieo.ExecOrderRef), "%s", "42");

	/* 行权委托录入回报 → opt sink onRspEntrustOpt（makeEntrust(Exec) 换算） */
	CThostFtdcRspInfoField err = makeRsp(66, "exec rejected");
	probe.OnRspExecOrderInsert(&ieo, &err, 1, true);
	ASSERT_EQ(1u, fx.opt.rspEntrustOpts.size());
	const auto& ec = fx.opt.rspEntrustOpts[0];
	EXPECT_EQ("rb2610", ec.code);
	EXPECT_DOUBLE_EQ(2.0, ec.volume);
	EXPECT_EQ((int)WDT_SHORT, ec.direction);
	EXPECT_EQ((int)WOT_CLOSE, ec.offsetType);
	EXPECT_EQ((int)BT_EXECUTE, ec.businessType);
	EXPECT_EQ("000003#0000000004#000042", ec.entrustID);
	EXPECT_EQ(66, ec.errCode);

	probe.OnErrRtnExecOrderInsert(&ieo, &err);
	ASSERT_EQ(2u, fx.opt.rspEntrustOpts.size());

	/* 行权单推送：未触发状态 */
	CThostFtdcExecOrderField eo;
	std::memset(&eo, 0, sizeof(eo));
	std::snprintf(eo.InstrumentID, sizeof(eo.InstrumentID), "%s", "rb2610");
	std::snprintf(eo.ExchangeID, sizeof(eo.ExchangeID), "%s", "SSE");
	eo.Volume = 1;
	eo.PosiDirection = THOST_FTDC_PD_Long;
	eo.OffsetFlag = THOST_FTDC_OF_Open;
	eo.OrderSubmitStatus = THOST_FTDC_OSS_Accepted;
	eo.FrontID = 3;
	eo.SessionID = 4;
	std::snprintf(eo.ExecOrderRef, sizeof(eo.ExecOrderRef), "%s", "43");
	std::snprintf(eo.InsertDate, sizeof(eo.InsertDate), "%s", "20260107");
	std::snprintf(eo.InsertTime, sizeof(eo.InsertTime), "%s", "09:15:00");

	probe.OnRtnExecOrder(&eo);
	ASSERT_EQ(1u, fx.opt.pushOrderOpts.size());
	const auto& po = fx.opt.pushOrderOpts[0];
	EXPECT_EQ((int)BT_EXECUTE, po.businessType);
	EXPECT_EQ((int)WOS_Nottouched, po.state);
	EXPECT_EQ((int)WDT_LONG, po.direction);
	EXPECT_EQ("000003#0000000004#000043", po.entrustID);

	/* 提交被拒 → WOS_Canceled + 正常错误位 */
	eo.OrderSubmitStatus = THOST_FTDC_OSS_InsertRejected;
	probe.OnRtnExecOrder(&eo);
	ASSERT_EQ(2u, fx.opt.pushOrderOpts.size());
	EXPECT_EQ((int)WOS_Canceled, fx.opt.pushOrderOpts[1].state);

	/* 未知合约 → 换算返回 NULL，opt sink 不受扰 */
	std::snprintf(eo.InstrumentID, sizeof(eo.InstrumentID), "%s", "NOPE01");
	EXPECT_EQ(nullptr, probe.makeOrderInfo(&eo));
	EXPECT_EQ(2u, fx.opt.pushOrderOpts.size());

	probe.registerSpi(NULL);
}

TEST(TraderCTPOpt, query_rsp_streams_fill_arrays_and_reset_state)
{
	CtpProbe probe;
	probe.harden();
	CtpFixture fx;
	probe.registerSpi(&fx.spi);

	CThostFtdcRspInfoField err5 = makeRsp(5, "err");

	/* 出错 → 不入队，直接 bIsLast 通知：数组尚未创建则透传 NULL */
	CThostFtdcOrderField of;
	std::memset(&of, 0, sizeof(of));
	std::snprintf(of.InstrumentID, sizeof(of.InstrumentID), "%s", "rb2610");
	std::snprintf(of.ExchangeID, sizeof(of.ExchangeID), "%s", "SSE");
	of.FrontID = 1;
	of.SessionID = 1;
	std::snprintf(of.OrderRef, sizeof(of.OrderRef), "%s", "1");
	probe.OnRspQryOrder(&of, &err5, 1, true);
	EXPECT_EQ(1u, fx.spi.orderCalls);
	EXPECT_TRUE(fx.spi.lastOrdersWasNull);

	CThostFtdcOrderField of2 = of;
	std::snprintf(of2.OrderRef, sizeof(of2.OrderRef), "%s", "2");
	probe.OnRspQryOrder(&of2, NULL, 2, false);
	probe.OnRspQryOrder(&of2, NULL, 2, true);
	EXPECT_EQ(3u, fx.spi.orderCalls);
	EXPECT_FALSE(fx.spi.lastOrdersWasNull);
	EXPECT_EQ(2u, fx.spi.lastOrderSize);	/* of + of2 两次入队后 clear */

	/* 成交流 */
	CThostFtdcTradeField tf;
	std::memset(&tf, 0, sizeof(tf));
	std::snprintf(tf.InstrumentID, sizeof(tf.InstrumentID), "%s", "rb2610");
	std::snprintf(tf.ExchangeID, sizeof(tf.ExchangeID), "%s", "SSE");
	std::snprintf(tf.TradeID, sizeof(tf.TradeID), "%s", "T1");
	tf.Price = 100.0;
	tf.Volume = 1;
	probe.OnRspQryTrade(&tf, NULL, 3, true);
	EXPECT_EQ(1u, fx.spi.tradeCalls);
	EXPECT_EQ(1u, fx.spi.lastTradeSize);
	probe.OnRspQryTrade(&tf, &err5, 4, true);
	EXPECT_EQ(2u, fx.spi.tradeCalls);
	EXPECT_EQ(0u, fx.spi.lastTradeSize);	/* clear 之后再次 last，只剩空数组 */

	/* 行权单流（m_ayExecOrds 已由探针硬化为 NULL，规避未初始化 UB） */
	CThostFtdcExecOrderField eo;
	std::memset(&eo, 0, sizeof(eo));
	std::snprintf(eo.InstrumentID, sizeof(eo.InstrumentID), "%s", "rb2610");
	std::snprintf(eo.ExchangeID, sizeof(eo.ExchangeID), "%s", "SSE");
	eo.FrontID = 1;
	eo.SessionID = 1;
	std::snprintf(eo.ExecOrderRef, sizeof(eo.ExecOrderRef), "%s", "3");
	probe.OnRspQryExecOrder(&eo, NULL, 5, true);
	EXPECT_EQ(1u, fx.opt.ordersOptCalls);
	/* 转发的是 m_ayOrders（前序 OnRspQryOrder 已创建并 clear，非空但 size 0） */
	EXPECT_FALSE(fx.opt.lastOrdersOptNull);
	EXPECT_EQ(0u, fx.opt.lastOrdersOptSize);
	EXPECT_NE(nullptr, probe.ayExecOrds());

	/* 资金流 */
	CThostFtdcTradingAccountField acc;
	std::memset(&acc, 0, sizeof(acc));
	acc.PreBalance = 100000.0;
	acc.CurrMargin = 3000.0;
	acc.Available = 90000.0;
	probe.OnRspQryTradingAccount(&acc, NULL, 6, true);
	EXPECT_EQ(1u, fx.spi.accountCalls);
	EXPECT_EQ(1u, fx.spi.lastAccountSize);

	/* 结算单文本拼接 */
	CThostFtdcSettlementInfoField sif;
	std::memset(&sif, 0, sizeof(sif));
	std::snprintf(sif.TradingDay, sizeof(sif.TradingDay), "%s", "20260105");
	std::snprintf(sif.Content, sizeof(sif.Content), "%s", "PART-1 ");
	probe.OnRspQrySettlementInfo(&sif, NULL, 7, false);
	EXPECT_EQ(0u, fx.spi.settleCalls);
	std::snprintf(sif.Content, sizeof(sif.Content), "%s", "PART-2");
	probe.OnRspQrySettlementInfo(&sif, NULL, 7, true);
	EXPECT_EQ(1u, fx.spi.settleCalls);
	EXPECT_EQ("PART-1 PART-2", fx.spi.settleContent);
	EXPECT_EQ(20260105u, fx.spi.settleDate);

	probe.registerSpi(NULL);
}

TEST(TraderCTPOpt, position_query_aggregates_today_cover_mode)
{
	CtpProbe probe;
	probe.harden();
	CtpFixture fx;
	probe.registerSpi(&fx.spi);

	CThostFtdcInvestorPositionField pf;
	std::memset(&pf, 0, sizeof(pf));
	std::snprintf(pf.InstrumentID, sizeof(pf.InstrumentID), "%s", "rb2610");
	std::snprintf(pf.ExchangeID, sizeof(pf.ExchangeID), "%s", "SSE");
	pf.PosiDirection = THOST_FTDC_PD_Long;
	pf.PositionDate = THOST_FTDC_PSD_Today;
	pf.Position = 10;
	pf.TodayPosition = 4;
	pf.ShortFrozen = 2;
	pf.UseMargin = 1000.0;
	pf.PositionProfit = 50.0;
	pf.PositionCost = 17000.0;

	probe.OnRspQryInvestorPosition(&pf, NULL, 1, false);
	ASSERT_NE(nullptr, probe.mapPos());
	EXPECT_EQ(1u, probe.mapPos()->size());
	WTSPositionItem* item = (WTSPositionItem*)probe.mapPos()->get(std::string("rb2610-2"));
	ASSERT_NE(nullptr, item);
	EXPECT_DOUBLE_EQ(10.0, item->getNewPosition());
	EXPECT_DOUBLE_EQ(0.0, item->getPrePosition());
	EXPECT_DOUBLE_EQ(10.0, item->getTotalPosition());
	EXPECT_DOUBLE_EQ(8.0, item->getAvailPosition());	/* 今日仓 10 - ShortFrozen 2 */
	EXPECT_DOUBLE_EQ(1000.0, item->getMargin());
	EXPECT_DOUBLE_EQ(50.0, item->getDynProfit());

	/* 收尾：bIsLast 触发 onRspPosition 并释放 map */
	probe.OnRspQryInvestorPosition(&pf, NULL, 1, true);
	EXPECT_EQ(1u, fx.spi.positionCalls);
	EXPECT_EQ(1u, fx.spi.lastPositionSize);
	EXPECT_EQ(nullptr, probe.mapPos());

	/* 错误响应不入 map */
	CThostFtdcRspInfoField err5 = makeRsp(5, "x");
	probe.OnRspQryInvestorPosition(&pf, &err5, 2, false);
	EXPECT_EQ(nullptr, probe.mapPos());

	probe.registerSpi(NULL);
}

/*
 * 触发条件见 bug 文件：
 * 1) OnRspAuthenticate/OnRspUserLogin 成功分支直接 dereference m_pUserAPI
 *    （doLogin/GetTradingDay），无库环境必崩；
 * 2) connect() 对 init() 加载失败后的 m_funcCreator 无判空，NULL call 必崩，
 *    且会拉起 join 不掉的查询线程。
 */
TEST(DISABLED_TraderCTPOpt, login_success_and_connect_require_real_ctp_lib)
{
	CtpProbe probe;
	probe.harden();
	CtpFixture fx;
	probe.registerSpi(&fx.spi);

	CThostFtdcRspUserLoginField lf;
	std::memset(&lf, 0, sizeof(lf));
	std::snprintf(lf.MaxOrderRef, sizeof(lf.MaxOrderRef), "%s", "1");
	probe.OnRspUserLogin(&lf, NULL, 1, true);	/* 需要真实 api，否则崩于 GetTradingDay */
	probe.connect();
}

/*
 * OnRspOrderAction/OnRspExecOrderAction 在 pRspInfo==NULL（IsErrorRspInfo false）
 * 的 else 分支直接 deref pRspInfo->ErrorMsg → 空指针崩溃；见 bug 文件。
 */
TEST(DISABLED_TraderCTPOpt, order_action_rsp_null_rspinfo_derefs_errormsg)
{
	CtpProbe probe;
	probe.harden();
	CtpFixture fx;
	probe.registerSpi(&fx.spi);
	probe.OnRspOrderAction(NULL, NULL, 1, true);
}
