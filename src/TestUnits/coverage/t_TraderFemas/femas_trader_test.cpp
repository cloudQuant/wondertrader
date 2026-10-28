/*
 * lane T4 —— TraderFemas 收发族用例
 *
 * 生产 TU：src/TraderFemas/TraderFemas.cpp（与本目录一起编进 CoverageTests_t_TraderFemas），
 * 因此可以直接 new 出 TraderFemas 并调用其 protected 成员（用派生探针类暴露，
 * 不使用 #define private public）。
 *
 * 刻意不进入的面（见 docs/迭代计划/迭代01/bugs/T4_femas_ctploader_mini.md）：
 *  - init()：内部 DLLHelper::load_library 会 dlopen 厂商库，路径与产物不稳定 → 不启用；
 *  - connect()/disconnect()：前者创建 Api 实例并起工作线程，后者 post(release)；
 *  - OnRspUserLogin 成功分支：会 m_pUserAPI->GetTradingDay()，无厂商实例必崩；
 *  - login()/logout()/orderInsert() 的「已就绪」正路径：都要 Req* 调用厂商 API。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include <boost/asio/io_context.hpp>

#include "TraderFemas/TraderFemas.h"

#include "Includes/IBaseDataMgr.h"
#include "Includes/ITraderApi.h"
#include "Includes/WTSCollection.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSTradeDef.hpp"
#include "Includes/WTSVariant.hpp"
#include "Share/TimeUtils.hpp"
#include "WTSUtils/WTSCfgLoader.h"

USING_NS_WTP;

/* ------------------------------------------------------------------ */
/* 测试替身                                                             */
/* ------------------------------------------------------------------ */
namespace {

class FakeBaseDataMgr : public IBaseDataMgr
{
public:
	WTSContractInfo*	m_contract = nullptr;
	WTSCommodityInfo*	m_comm = nullptr;
	uint32_t			m_getContractCalls = 0;

	WTSCommodityInfo* getCommodity(const char* /*exchgpid*/) override { return m_comm; }
	WTSCommodityInfo* getCommodity(const char* /*exchg*/, const char* /*pid*/) override { return m_comm; }

	WTSContractInfo* getContract(const char* /*code*/, const char* /*exchg*/, uint32_t /*uDate*/) override
	{
		m_getContractCalls++;
		return m_contract;
	}

	WTSArray* getContracts(const char* /*exchg*/, uint32_t /*uDate*/) override { return nullptr; }
	WTSSessionInfo* getSession(const char* /*sid*/) override { return nullptr; }
	WTSSessionInfo* getSessionByCode(const char* /*code*/, const char* /*exchg*/) override { return nullptr; }
	WTSArray* getAllSessions() override { return nullptr; }
	bool isHoliday(const char* /*pid*/, uint32_t /*uDate*/, bool /*isTpl*/) override { return false; }
	uint32_t calcTradingDate(const char* /*stdPID*/, uint32_t /*uDate*/, uint32_t /*uTime*/, bool /*isSession*/) override { return 0; }
	uint64_t getBoundaryTime(const char* /*stdPID*/, uint32_t /*tDate*/, bool /*isSession*/, bool /*isStart*/) override { return 0; }
};

class FakeTraderSpi : public ITraderSpi
{
public:
	FakeBaseDataMgr	m_mgr;

	struct Event { WTSTraderEvent e; int32_t ec; };
	std::vector<Event> events;

	struct LoginResult { bool ok; std::string msg; uint32_t date; };
	std::vector<LoginResult> logins;

	std::vector<std::string> logs;
	std::vector<WTSLogLevel> logLevels;

	struct EntrustRsp { std::string code; double vol; double price; int32_t errCode; std::string errMsg; };
	std::vector<EntrustRsp> entrustRsps;

	struct AccountSnap { uint32_t size; double balance; double available; std::string currency; };
	std::vector<AccountSnap> accounts;

	struct ArraySnap { uint32_t size; std::string firstCode; };
	std::vector<ArraySnap> positions, orders, trades;

	struct OrderSnap { std::string code, exchg, entrustID, orderID; double price, vol, volTraded, volLeft;
		WTSOrderState state; uint32_t orderDate; uint64_t orderTime; };
	std::vector<OrderSnap> pushedOrders;

	struct TradeSnap { std::string code, tradeID, refOrder; double price, vol, amount; };
	std::vector<TradeSnap> pushedTrades;

	struct ErrorSnap { int32_t code; std::string msg; };
	std::vector<ErrorSnap> traderErrors;

	struct StkStatus { std::string exchg, code; WTSTradeStatus st; };
	std::vector<StkStatus> statuses;

	uint32_t logoutCalls = 0;

	IBaseDataMgr* getBaseDataMgr() override { return &m_mgr; }

	void handleTraderLog(WTSLogLevel ll, const char* message) override
	{
		logLevels.push_back(ll);
		logs.emplace_back(message == nullptr ? "" : message);
	}

	void handleEvent(WTSTraderEvent e, int32_t ec) override { events.push_back({e, ec}); }

	void onLoginResult(bool bSucc, const char* msg, uint32_t tradingdate) override
	{
		logins.push_back({bSucc, msg == nullptr ? "" : msg, tradingdate});
	}

	void onLogout() override { logoutCalls++; }

	void onRspEntrust(WTSEntrust* entrust, WTSError* err) override
	{
		EntrustRsp r;
		if (entrust != nullptr)
		{
			r.code = entrust->getCode();
			r.vol = entrust->getVolume();
			r.price = entrust->getPrice();
		}
		if (err != nullptr)
		{
			r.errCode = static_cast<int32_t>(err->getErrorCode());
			r.errMsg = err->getMessage();
		}
		else
		{
			r.errCode = 0;
		}
		entrustRsps.push_back(r);
	}

	void onRspAccount(WTSArray* ayAccounts) override
	{
		AccountSnap snap{};
		if (ayAccounts != nullptr)
		{
			snap.size = ayAccounts->size();
			WTSObject* obj = ayAccounts->size() > 0 ? ayAccounts->at(0) : nullptr;
			WTSAccountInfo* acc = static_cast<WTSAccountInfo*>(obj);
			if (acc != nullptr)
			{
				snap.balance = acc->getBalance();
				snap.available = acc->getAvailable();
				snap.currency = acc->getCurrency();
			}
		}
		else
		{
			snap.size = 0xffffffffu;	// 区分「没回调」和「回调了空数组」
		}
		accounts.push_back(snap);
	}

	void onRspPosition(const WTSArray* ayPositions) override
	{
		ArraySnap snap{};
		if (ayPositions != nullptr)
		{
			snap.size = ayPositions->size();
			if (ayPositions->size() > 0)
				snap.firstCode = const_cast<WTSArray*>(ayPositions)->at<WTSPositionItem>(0)->getCode();
		}
		positions.push_back(snap);
	}

	void onRspOrders(const WTSArray* ayOrders) override
	{
		ArraySnap snap{};
		if (ayOrders != nullptr)
		{
			snap.size = ayOrders->size();
			if (ayOrders->size() > 0)
				snap.firstCode = const_cast<WTSArray*>(ayOrders)->at<WTSOrderInfo>(0)->getCode();
		}
		orders.push_back(snap);
	}

	void onRspTrades(const WTSArray* ayTrades) override
	{
		ArraySnap snap{};
		if (ayTrades != nullptr)
		{
			snap.size = ayTrades->size();
			if (ayTrades->size() > 0)
				snap.firstCode = const_cast<WTSArray*>(ayTrades)->at<WTSTradeInfo>(0)->getCode();
		}
		trades.push_back(snap);
	}

	void onPushOrder(WTSOrderInfo* orderInfo) override
	{
		if (orderInfo == nullptr)
			return;
		OrderSnap s;
		s.code = orderInfo->getCode();
		s.exchg = orderInfo->getExchg();
		s.entrustID = orderInfo->getEntrustID();
		s.orderID = orderInfo->getOrderID();
		s.price = orderInfo->getPrice();
		s.vol = orderInfo->getVolume();
		s.volTraded = orderInfo->getVolTraded();
		s.volLeft = orderInfo->getVolLeft();
		s.state = orderInfo->getOrderState();
		s.orderDate = orderInfo->getOrderDate();
		s.orderTime = orderInfo->getOrderTime();
		pushedOrders.push_back(s);
	}

	void onPushTrade(WTSTradeInfo* tradeRecord) override
	{
		if (tradeRecord == nullptr)
			return;
		TradeSnap s;
		s.code = tradeRecord->getCode();
		s.tradeID = tradeRecord->getTradeID();
		s.refOrder = tradeRecord->getRefOrder();
		s.price = tradeRecord->getPrice();
		s.vol = tradeRecord->getVolume();
		s.amount = tradeRecord->getAmount();
		pushedTrades.push_back(s);
	}

	void onTraderError(WTSError* err, void* /*pData*/ = nullptr) override
	{
		ErrorSnap s;
		if (err != nullptr)
		{
			s.code = static_cast<int32_t>(err->getErrorCode());
			s.msg = err->getMessage();
		}
		traderErrors.push_back(s);
	}

	void onPushInstrumentStatus(const char* exchg, const char* code, WTSTradeStatus state) override
	{
		statuses.push_back({exchg == nullptr ? "" : exchg, code == nullptr ? "" : code, state});
	}
};

/* 派生探针：只暴露既有的 protected 成员，不改变任何行为 */
class FemasProbe : public TraderFemas
{
public:
	using TraderFemas::IsErrorRspInfo;
	using TraderFemas::genLocalOrdID;
	using TraderFemas::genRequestID;
	using TraderFemas::makeEntrust;
	using TraderFemas::makeError;
	using TraderFemas::makeOrderInfo;
	using TraderFemas::makeTradeRecord;
	using TraderFemas::wrapActionFlag;
	using TraderFemas::wrapDirectionType;
	using TraderFemas::wrapExchg;
	using TraderFemas::wrapOffsetType;
	using TraderFemas::wrapOrderState;
	using TraderFemas::wrapPosDirection;
	using TraderFemas::wrapPriceType;
	using TraderFemas::wrapTimeCondition;

	void setState(WrapperState s) { m_wrapperState = s; }
	WrapperState state() const { return m_wrapperState; }
	void setSessionID(const char* s) { m_strSessionID = s; }
	void setStrand(boost::asio::io_context::strand* s) { m_strandIO = s; }
	void setApi(CUstpFtdcTraderApi* api) { m_pUserAPI = api; }
	bool qryOnline() const { return m_bQryOnline; }
	bool stopped() const { return m_bStopped; }
	size_t queryPending() { return m_queQuery.size(); }
	IBaseDataMgr* bdMgr() const { return m_bdMgr; }
	const std::string& user() const { return m_strUser; }
	const std::string& pass() const { return m_strPass; }
	const std::string& product() const { return m_strProduct; }
};

CUstpFtdcTraderApi* const kApiSentinel = reinterpret_cast<CUstpFtdcTraderApi*>(uintptr_t{0x1});

class TraderFemasTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		probe.setStrand(&strand);
		probe.registerSpi(&sink);
	}

	boost::asio::io_context ioc;
	boost::asio::io_context::strand strand{ioc};
	FakeTraderSpi sink;
	FemasProbe probe;
};

/* 品种/合约夹具：makeTradeRecord 需要 volScale，OnRspQryInvestorPosition 需要 currency */
class FemasContractFixture : public TraderFemasTest
{
protected:
	void SetUp() override
	{
		TraderFemasTest::SetUp();
		comm = WTSCommodityInfo::create("rb", "螺纹钢", "SHFE", "SN001", "trdtpl");
		comm->setVolScale(10);
		comm->setCategory(CC_Future);
		contract = WTSContractInfo::create("rb2401", "螺纹钢2401", "SHFE", "rb");
		contract->setCommInfo(comm);
		sink.m_mgr.m_contract = contract;
		sink.m_mgr.m_comm = comm;
	}

	void TearDown() override
	{
		sink.m_mgr.m_contract = nullptr;
		sink.m_mgr.m_comm = nullptr;
		if (contract != nullptr) { contract->release(); contract = nullptr; }
		if (comm != nullptr) { comm->release(); comm = nullptr; }
		TraderFemasTest::TearDown();
	}

	WTSCommodityInfo* comm = nullptr;
	WTSContractInfo* contract = nullptr;
};

CUstpFtdcRspInfoField makeRsp(int32_t errId, const char* msg)
{
	CUstpFtdcRspInfoField rsp;
	memset(&rsp, 0, sizeof(rsp));
	rsp.ErrorID = errId;
	strncpy(rsp.ErrorMsg, msg, sizeof(rsp.ErrorMsg) - 1);
	return rsp;
}

}	// namespace

/* ------------------------------------------------------------------ */
/* 生命周期 / 状态查询                                                  */
/* ------------------------------------------------------------------ */

TEST_F(TraderFemasTest, fresh_instance_reports_disconnected_and_refuses_every_business_call)
{
	EXPECT_EQ(probe.state(), TraderFemas::WS_NOTLOGIN);
	EXPECT_FALSE(probe.isConnected());

	WTSEntrust* entrust = WTSEntrust::create("rb2401", 2, 3500.0, "SHFE");
	ASSERT_NE(entrust, nullptr);
	WTSEntrustAction* action = WTSEntrustAction::create("rb2401", "SHFE");
	ASSERT_NE(action, nullptr);
	action->setEntrustID("eid-1");
	action->setOrderID("oid-9");

	EXPECT_EQ(probe.orderInsert(entrust), -1);
	EXPECT_EQ(probe.orderAction(action), -1);
	EXPECT_EQ(probe.queryAccount(), -1);
	EXPECT_EQ(probe.queryPositions(), -1);
	EXPECT_EQ(probe.queryOrders(), -1);
	EXPECT_EQ(probe.queryTrades(), -1);
	EXPECT_EQ(probe.logout(), -1);

	entrust->release();
	action->release();
}

TEST_F(TraderFemasTest, login_without_api_handles_credentials_but_still_fails)
{
	// m_strUser/m_strPass/m_strProduct 在空指针检查之前就被赋值 —— 这是可证伪的顺序断言
	EXPECT_EQ(probe.login("u9527", "pwd", "prod-1"), -1);
	EXPECT_EQ(probe.user(), "u9527");
	EXPECT_EQ(probe.pass(), "pwd");
	EXPECT_EQ(probe.product(), "prod-1");
	// 认证流程没有被触发
	EXPECT_EQ(probe.state(), TraderFemas::WS_NOTLOGIN);
	EXPECT_TRUE(sink.logins.empty());
}

TEST_F(TraderFemasTest, ready_state_flips_connectivity_flag)
{
	EXPECT_FALSE(probe.isConnected());
	probe.setState(TraderFemas::WS_LOGINED);
	EXPECT_FALSE(probe.isConnected());
	probe.setState(TraderFemas::WS_ALLREADY);
	EXPECT_TRUE(probe.isConnected());
}

TEST_F(TraderFemasTest, release_marks_stopped_and_is_repeatable)
{
	EXPECT_NO_THROW(probe.release());
	EXPECT_TRUE(probe.stopped());
	EXPECT_NO_THROW(probe.release());
	EXPECT_FALSE(probe.isConnected());
}

TEST_F(TraderFemasTest, register_spi_binds_sink_and_base_data_manager)
{
	EXPECT_EQ(probe.bdMgr(), static_cast<IBaseDataMgr*>(&sink.m_mgr));

	probe.OnFrontConnected();
	ASSERT_EQ(sink.events.size(), 1u);
	EXPECT_EQ(sink.events[0].e, WTE_Connect);
	EXPECT_EQ(sink.events[0].ec, 0);

	// 未注册 sink 时同样的回调必须静默，不能崩
	FemasProbe alone;
	alone.registerSpi(nullptr);
	EXPECT_NO_THROW(alone.OnFrontConnected());
	EXPECT_NO_THROW(alone.OnHeartBeatWarning(3));
}

TEST_F(TraderFemasTest, front_disconnected_resets_state_and_publishes_close_event)
{
	probe.setState(TraderFemas::WS_ALLREADY);
	probe.OnFrontDisconnected(44);

	EXPECT_EQ(probe.state(), TraderFemas::WS_NOTLOGIN);
	EXPECT_FALSE(probe.isConnected());
	EXPECT_FALSE(probe.qryOnline());
	ASSERT_EQ(sink.events.size(), 1u);
	EXPECT_EQ(sink.events[0].e, WTE_Close);
	EXPECT_EQ(sink.events[0].ec, 44);
}

TEST_F(TraderFemasTest, qry_front_disconnected_only_clears_query_flag)
{
	probe.setState(TraderFemas::WS_ALLREADY);
	probe.OnQryFrontDisconnected(7);

	EXPECT_FALSE(probe.qryOnline());
	EXPECT_TRUE(sink.events.empty());
	// 查询前置掉线不该把交易通道踢回未登录
	EXPECT_EQ(probe.state(), TraderFemas::WS_ALLREADY);
	EXPECT_TRUE(probe.isConnected());
}

TEST_F(TraderFemasTest, heartbeat_warning_is_routed_to_sink_log)
{
	probe.OnHeartBeatWarning(5);

	ASSERT_EQ(sink.logs.size(), 1u);
	EXPECT_EQ(sink.logLevels[0], LL_DEBUG);
	EXPECT_NE(sink.logs[0].find("Heartbeating"), std::string::npos);
	EXPECT_NE(sink.logs[0].find("["), std::string::npos);
}

/* ------------------------------------------------------------------ */
/* makeEntrustID                                                        */
/* ------------------------------------------------------------------ */

TEST_F(TraderFemasTest, make_entrust_id_rejects_null_and_zero_length_buffer)
{
	char buf[64];
	memset(buf, 0, sizeof(buf));

	EXPECT_FALSE(probe.makeEntrustID(nullptr, 32));
	EXPECT_FALSE(probe.makeEntrustID(buf, 0));
	// 两个守卫都在写缓冲之前返回
	EXPECT_EQ(buf[0], '\0');
}

TEST_F(TraderFemasTest, make_entrust_id_pads_local_order_id_to_twelve_digits)
{
	char buf[64];
	memset(buf, 0, sizeof(buf));

	// m_uLocalOrdID 从 1 起，fetch_add 后首个可用编号是 2
	ASSERT_TRUE(probe.makeEntrustID(buf, static_cast<int>(sizeof(buf))));
	EXPECT_EQ(std::string(buf), "0000000000002");

	ASSERT_TRUE(probe.makeEntrustID(buf, static_cast<int>(sizeof(buf))));
	EXPECT_EQ(std::string(buf), "0000000000003");
}

TEST_F(TraderFemasTest, make_entrust_id_prefixes_session_id)
{
	probe.setSessionID("A1");
	char buf[64];
	memset(buf, 0, sizeof(buf));

	ASSERT_TRUE(probe.makeEntrustID(buf, static_cast<int>(sizeof(buf))));
	EXPECT_EQ(std::string(buf), "A1000000000002");

	EXPECT_EQ(probe.genRequestID(), 1u);
	EXPECT_EQ(probe.genRequestID(), 2u);
}

/* ------------------------------------------------------------------ */
/* 枚举/编码换算                                                        */
/* ------------------------------------------------------------------ */

TEST_F(TraderFemasTest, wrap_wts_enums_into_femas_codes)
{
	// 价格类型：ANY 在市价单下按交易所分叉（CFFEX 用最优价）
	EXPECT_EQ(probe.wrapPriceType(WPT_ANYPRICE, false), USTP_FTDC_OPT_AnyPrice);
	EXPECT_EQ(probe.wrapPriceType(WPT_ANYPRICE, true), USTP_FTDC_OPT_BestPrice);
	EXPECT_EQ(probe.wrapPriceType(WPT_LIMITPRICE, false), USTP_FTDC_OPT_LimitPrice);
	EXPECT_EQ(probe.wrapPriceType(WPT_LASTPRICE, false), USTP_FTDC_OPT_BestPrice);

	// 方向：多开/空平都是买，多平/空开都是卖
	EXPECT_EQ(probe.wrapDirectionType(WDT_LONG, WOT_OPEN), USTP_FTDC_D_Buy);
	EXPECT_EQ(probe.wrapDirectionType(WDT_LONG, WOT_CLOSE), USTP_FTDC_D_Sell);
	EXPECT_EQ(probe.wrapDirectionType(WDT_SHORT, WOT_OPEN), USTP_FTDC_D_Sell);
	EXPECT_EQ(probe.wrapDirectionType(WDT_SHORT, WOT_CLOSE), USTP_FTDC_D_Buy);

	// 平今用 CloseToday，平昨被降级成 Close（Femas 无单独平昨码）
	EXPECT_EQ(probe.wrapOffsetType(WOT_OPEN), USTP_FTDC_OF_Open);
	EXPECT_EQ(probe.wrapOffsetType(WOT_CLOSE), USTP_FTDC_OF_Close);
	EXPECT_EQ(probe.wrapOffsetType(WOT_CLOSETODAY), USTP_FTDC_OF_CloseToday);
	EXPECT_EQ(probe.wrapOffsetType(WOT_CLOSEYESTERDAY), USTP_FTDC_OF_Close);
	EXPECT_EQ(probe.wrapOffsetType(WOT_FORCECLOSE), USTP_FTDC_OF_ForceClose);

	EXPECT_EQ(probe.wrapTimeCondition(WTC_IOC), USTP_FTDC_TC_IOC);
	EXPECT_EQ(probe.wrapTimeCondition(WTC_GFD), USTP_FTDC_TC_GFD);
	EXPECT_EQ(probe.wrapTimeCondition(WTC_GFS), USTP_FTDC_TC_GFS);

	EXPECT_EQ(probe.wrapActionFlag(WAF_CANCEL), USTP_FTDC_AF_Delete);
	EXPECT_EQ(probe.wrapActionFlag(WAF_MODIFY), USTP_FTDC_AF_Modify);
}

TEST_F(TraderFemasTest, wrap_femas_codes_back_into_wts_enums)
{
	EXPECT_EQ(probe.wrapPriceType(static_cast<TUstpFtdcPriceType>(USTP_FTDC_OPT_AnyPrice)), WPT_ANYPRICE);
	EXPECT_EQ(probe.wrapPriceType(static_cast<TUstpFtdcPriceType>(USTP_FTDC_OPT_BestPrice)), WPT_ANYPRICE);
	EXPECT_EQ(probe.wrapPriceType(static_cast<TUstpFtdcPriceType>(USTP_FTDC_OPT_LimitPrice)), WPT_LIMITPRICE);
	EXPECT_EQ(probe.wrapPriceType(static_cast<TUstpFtdcPriceType>(USTP_FTDC_OPT_FiveLevelPrice)), WPT_LASTPRICE);

	EXPECT_EQ(probe.wrapDirectionType(static_cast<TUstpFtdcDirectionType>(USTP_FTDC_D_Buy),
		static_cast<TUstpFtdcOffsetFlagType>(USTP_FTDC_OF_Open)), WDT_LONG);
	EXPECT_EQ(probe.wrapDirectionType(static_cast<TUstpFtdcDirectionType>(USTP_FTDC_D_Buy),
		static_cast<TUstpFtdcOffsetFlagType>(USTP_FTDC_OF_Close)), WDT_SHORT);
	EXPECT_EQ(probe.wrapDirectionType(static_cast<TUstpFtdcDirectionType>(USTP_FTDC_D_Sell),
		static_cast<TUstpFtdcOffsetFlagType>(USTP_FTDC_OF_Close)), WDT_LONG);

	EXPECT_EQ(probe.wrapPosDirection(static_cast<TUstpFtdcDirectionType>(USTP_FTDC_D_Sell)), WDT_SHORT);

	EXPECT_EQ(probe.wrapOffsetType(static_cast<TUstpFtdcOffsetFlagType>(USTP_FTDC_OF_Open)), WOT_OPEN);
	EXPECT_EQ(probe.wrapOffsetType(static_cast<TUstpFtdcOffsetFlagType>(USTP_FTDC_OF_Close)), WOT_CLOSE);
	EXPECT_EQ(probe.wrapOffsetType(static_cast<TUstpFtdcOffsetFlagType>(USTP_FTDC_OF_CloseToday)), WOT_CLOSETODAY);
	EXPECT_EQ(probe.wrapOffsetType(static_cast<TUstpFtdcOffsetFlagType>(USTP_FTDC_OF_ForceClose)), WOT_FORCECLOSE);

	EXPECT_EQ(probe.wrapTimeCondition(static_cast<TUstpFtdcTimeConditionType>(USTP_FTDC_TC_GFD)), WTC_GFD);
	EXPECT_EQ(probe.wrapTimeCondition(static_cast<TUstpFtdcTimeConditionType>(USTP_FTDC_TC_IOC)), WTC_IOC);
	EXPECT_EQ(probe.wrapTimeCondition(static_cast<TUstpFtdcTimeConditionType>(USTP_FTDC_TC_GFS)), WTC_GFS);

	// 交易所换算只有郑商所改名，其余原样返回
	EXPECT_STREQ(probe.wrapExchg("CZCE"), "ZCE");
	EXPECT_STREQ(probe.wrapExchg("SHFE"), "SHFE");
	EXPECT_STREQ(probe.wrapExchg("DCE"), "DCE");
}

TEST_F(TraderFemasTest, wrap_order_state_clamps_femas_states_above_accepted_no_reply)
{
	EXPECT_EQ(probe.wrapOrderState(static_cast<TUstpFtdcOrderStatusType>(USTP_FTDC_OS_AcceptedNoReply)), WOS_Submitting);
	EXPECT_EQ(probe.wrapOrderState(static_cast<TUstpFtdcOrderStatusType>(USTP_FTDC_OS_Canceled)), WOS_Submitting);
	// 阈值之前的状态按数值直投
	const TUstpFtdcOrderStatusType before = static_cast<TUstpFtdcOrderStatusType>(USTP_FTDC_TC_GFD - 1);
	EXPECT_EQ(probe.wrapOrderState(before), static_cast<WTSOrderState>(static_cast<int>(before)));
}

TEST_F(TraderFemasTest, is_error_rsp_info_and_make_error)
{
	CUstpFtdcRspInfoField ok = makeRsp(0, "");
	CUstpFtdcRspInfoField bad = makeRsp(30, "no such instrument");

	EXPECT_FALSE(probe.IsErrorRspInfo(nullptr));
	EXPECT_FALSE(probe.IsErrorRspInfo(&ok));
	EXPECT_TRUE(probe.IsErrorRspInfo(&bad));

	EXPECT_EQ(probe.makeError(nullptr), nullptr);
	EXPECT_EQ(probe.makeError(&ok), nullptr);

	WTSError* err = probe.makeError(&bad);
	ASSERT_NE(err, nullptr);
	EXPECT_EQ(err->getErrorCode(), static_cast<WTSErroCode>(30));
	EXPECT_STREQ(err->getMessage(), "no such instrument");
	err->release();
}

/* ------------------------------------------------------------------ */
/* 厂商结构 → WTS 结构换算                                              */
/* ------------------------------------------------------------------ */

TEST_F(FemasContractFixture, make_order_info_maps_vendor_order_fields)
{
	CUstpFtdcOrderField ord;
	memset(&ord, 0, sizeof(ord));
	strncpy(ord.InstrumentID, "rb2401", sizeof(ord.InstrumentID) - 1);
	strncpy(ord.ExchangeID, "SHFE", sizeof(ord.ExchangeID) - 1);
	strncpy(ord.TradingDay, "20240105", sizeof(ord.TradingDay) - 1);
	strncpy(ord.InsertTime, "09:30:15", sizeof(ord.InsertTime) - 1);
	strncpy(ord.UserOrderLocalID, "A1000000000002", sizeof(ord.UserOrderLocalID) - 1);
	strncpy(ord.OrderLocalID, "188", sizeof(ord.OrderLocalID) - 1);
	strncpy(ord.OrderSysID, "OS-77", sizeof(ord.OrderSysID) - 1);
	ord.Direction = static_cast<TUstpFtdcDirectionType>(USTP_FTDC_D_Buy);
	ord.OffsetFlag = static_cast<TUstpFtdcOffsetFlagType>(USTP_FTDC_OF_Open);
	ord.OrderPriceType = static_cast<TUstpFtdcPriceType>(USTP_FTDC_OPT_LimitPrice);
	ord.TimeCondition = static_cast<TUstpFtdcTimeConditionType>(USTP_FTDC_TC_GFD);
	ord.VolumeCondition = static_cast<TUstpFtdcVolumeConditionType>(USTP_FTDC_VC_AV);
	ord.LimitPrice = 3521.5;
	ord.Volume = 3;
	ord.VolumeTraded = 1;
	ord.VolumeRemain = 2;
	ord.OrderStatus = static_cast<TUstpFtdcOrderStatusType>(USTP_FTDC_OS_AcceptedNoReply);

	WTSOrderInfo* info = probe.makeOrderInfo(&ord);
	ASSERT_NE(info, nullptr);
	EXPECT_STREQ(info->getCode(), "rb2401");
	EXPECT_STREQ(info->getExchg(), "SHFE");
	EXPECT_DOUBLE_EQ(info->getPrice(), 3521.5);
	EXPECT_DOUBLE_EQ(info->getVolume(), 3);
	EXPECT_DOUBLE_EQ(info->getVolTraded(), 1);
	EXPECT_DOUBLE_EQ(info->getVolLeft(), 2);
	EXPECT_EQ(info->getDirection(), WDT_LONG);
	EXPECT_EQ(info->getOffsetType(), WOT_OPEN);
	EXPECT_EQ(info->getPriceType(), WPT_LIMITPRICE);
	EXPECT_EQ(info->getOrderFlag(), WOF_NOR);
	// "09:30:15" 去掉冒号 -> 93015，再 *1000 交给 makeTime
	EXPECT_EQ(info->getOrderDate(), 20240105u);
	EXPECT_EQ(info->getOrderTime(), TimeUtils::makeTime(20240105, 93015000));
	// UserOrderLocalID 数字非 0 -> 直接当委托号
	EXPECT_STREQ(info->getEntrustID(), "A1000000000002");
	EXPECT_STREQ(info->getOrderID(), "OS-77");
	// 高于 AcceptedNoReply 的状态被夹回 Submitting
	EXPECT_EQ(info->getOrderState(), WOS_Submitting);
	info->release();
}

TEST_F(FemasContractFixture, make_order_info_falls_back_to_local_id_and_maps_fok)
{
	sink.m_mgr.m_contract = nullptr;	// 合约查不到 -> 直接返回 NULL
	CUstpFtdcOrderField ord;
	memset(&ord, 0, sizeof(ord));
	strncpy(ord.InstrumentID, "unknown", sizeof(ord.InstrumentID) - 1);
	EXPECT_EQ(probe.makeOrderInfo(&ord), nullptr);
	sink.m_mgr.m_contract = contract;
	EXPECT_EQ(sink.m_mgr.m_getContractCalls, 1u);

	memset(&ord, 0, sizeof(ord));
	strncpy(ord.InstrumentID, "rb2401", sizeof(ord.InstrumentID) - 1);
	strncpy(ord.ExchangeID, "SHFE", sizeof(ord.ExchangeID) - 1);
	strncpy(ord.TradingDay, "20240105", sizeof(ord.TradingDay) - 1);
	strncpy(ord.InsertTime, "13:45:30", sizeof(ord.InsertTime) - 1);
	strncpy(ord.UserOrderLocalID, "0", sizeof(ord.UserOrderLocalID) - 1);
	strncpy(ord.OrderLocalID, "9527", sizeof(ord.OrderLocalID) - 1);
	ord.TimeCondition = static_cast<TUstpFtdcTimeConditionType>(USTP_FTDC_TC_IOC);
	ord.VolumeCondition = static_cast<TUstpFtdcVolumeConditionType>(USTP_FTDC_VC_CV);
	ord.Direction = static_cast<TUstpFtdcDirectionType>(USTP_FTDC_D_Sell);
	ord.OffsetFlag = static_cast<TUstpFtdcOffsetFlagType>(USTP_FTDC_OF_CloseToday);

	WTSOrderInfo* info = probe.makeOrderInfo(&ord);
	ASSERT_NE(info, nullptr);
	// atoi("0")==0 -> 回落到 OrderLocalID
	EXPECT_STREQ(info->getEntrustID(), "9527");
	// IOC + VC_CV -> FOK
	EXPECT_EQ(info->getOrderFlag(), WOF_FOK);
	EXPECT_EQ(info->getDirection(), WDT_LONG);		// 卖平今在多头上 = LONG
	EXPECT_EQ(info->getOffsetType(), WOT_CLOSETODAY);
	EXPECT_EQ(info->getOrderTime(), TimeUtils::makeTime(20240105, 134530000));
	info->release();
}

TEST_F(FemasContractFixture, make_entrust_maps_vendor_input_order)
{
	CUstpFtdcInputOrderField in;
	memset(&in, 0, sizeof(in));
	strncpy(in.InstrumentID, "rb2401", sizeof(in.InstrumentID) - 1);
	strncpy(in.ExchangeID, "SHFE", sizeof(in.ExchangeID) - 1);
	strncpy(in.UserOrderLocalID, "A1000000000007", sizeof(in.UserOrderLocalID) - 1);
	in.Volume = 5;
	in.LimitPrice = 3600.0;
	in.Direction = static_cast<TUstpFtdcDirectionType>(USTP_FTDC_D_Sell);
	in.OffsetFlag = static_cast<TUstpFtdcOffsetFlagType>(USTP_FTDC_OF_Open);
	in.OrderPriceType = static_cast<TUstpFtdcPriceType>(USTP_FTDC_OPT_AnyPrice);
	in.TimeCondition = static_cast<TUstpFtdcTimeConditionType>(USTP_FTDC_TC_IOC);
	in.VolumeCondition = static_cast<TUstpFtdcVolumeConditionType>(USTP_FTDC_VC_MV);

	WTSEntrust* entrust = probe.makeEntrust(&in);
	ASSERT_NE(entrust, nullptr);
	EXPECT_STREQ(entrust->getCode(), "rb2401");
	EXPECT_DOUBLE_EQ(entrust->getVolume(), 5);
	EXPECT_DOUBLE_EQ(entrust->getPrice(), 3600.0);
	EXPECT_EQ(entrust->getDirection(), WDT_SHORT);	// 空开
	EXPECT_EQ(entrust->getOffsetType(), WOT_OPEN);
	EXPECT_EQ(entrust->getPriceType(), WPT_ANYPRICE);
	EXPECT_EQ(entrust->getOrderFlag(), WOF_FAK);		// IOC + MV -> FAK
	EXPECT_STREQ(entrust->getEntrustID(), "A1000000000007");
	EXPECT_STREQ(entrust->getUserTag(), "A1000000000007");
	entrust->release();

	sink.m_mgr.m_contract = nullptr;
	EXPECT_EQ(probe.makeEntrust(&in), nullptr);
	sink.m_mgr.m_contract = contract;
}

TEST_F(FemasContractFixture, make_trade_record_scales_amount_by_contract_volume_scale)
{
	CUstpFtdcTradeField td;
	memset(&td, 0, sizeof(td));
	strncpy(td.InstrumentID, "rb2401", sizeof(td.InstrumentID) - 1);
	strncpy(td.ExchangeID, "SHFE", sizeof(td.ExchangeID) - 1);
	strncpy(td.TradingDay, "20240105", sizeof(td.TradingDay) - 1);
	strncpy(td.TradeTime, "14:05:09", sizeof(td.TradeTime) - 1);
	strncpy(td.TradeID, "T-2001", sizeof(td.TradeID) - 1);
	strncpy(td.OrderSysID, "OS-77", sizeof(td.OrderSysID) - 1);
	td.TradeVolume = 2;
	td.TradePrice = 3550.0;
	td.Direction = static_cast<TUstpFtdcDirectionType>(USTP_FTDC_D_Buy);
	td.OffsetFlag = static_cast<TUstpFtdcOffsetFlagType>(USTP_FTDC_OF_Open);

	WTSTradeInfo* trade = probe.makeTradeRecord(&td);
	ASSERT_NE(trade, nullptr);
	EXPECT_STREQ(trade->getCode(), "rb2401");
	EXPECT_STREQ(trade->getExchg(), "SHFE");
	EXPECT_STREQ(trade->getTradeID(), "T-2001");
	EXPECT_STREQ(trade->getRefOrder(), "OS-77");
	EXPECT_DOUBLE_EQ(trade->getPrice(), 3550.0);
	EXPECT_DOUBLE_EQ(trade->getVolume(), 2);
	// amount = volScale(10) * 2 * 3550.0
	EXPECT_DOUBLE_EQ(trade->getAmount(), 71000.0);
	EXPECT_EQ(trade->getTradeDate(), 20240105u);
	EXPECT_EQ(trade->getTradeTime(), TimeUtils::makeTime(20240105, 140509000));
	EXPECT_EQ(trade->getDirection(), WDT_LONG);
	EXPECT_EQ(trade->getTradeType(), WTT_Common);
	trade->release();

	sink.m_mgr.m_contract = nullptr;
	EXPECT_EQ(probe.makeTradeRecord(&td), nullptr);
	sink.m_mgr.m_contract = contract;
}

/* ------------------------------------------------------------------ */
/* 回调：未就绪/错误分支                                                */
/* ------------------------------------------------------------------ */

TEST_F(TraderFemasTest, certification_rejection_fails_login_without_touching_api)
{
	CUstpFtdcRspInfoField bad = makeRsp(11, "authcode expired");

	probe.OnRspDSUserCertification(nullptr, &bad, 1, true);

	EXPECT_EQ(probe.state(), TraderFemas::WS_LOGINFAILED);
	ASSERT_EQ(sink.logins.size(), 1u);
	EXPECT_FALSE(sink.logins[0].ok);
	EXPECT_EQ(sink.logins[0].msg, "authcode expired");
	ASSERT_FALSE(sink.logs.empty());
	EXPECT_NE(sink.logs.back().find("Authentiation"), std::string::npos);
}

TEST_F(TraderFemasTest, login_rejection_fails_login_without_touching_api)
{
	CUstpFtdcRspInfoField bad = makeRsp(64, "invalid password");

	probe.OnRspUserLogin(nullptr, &bad, 1, true);

	EXPECT_EQ(probe.state(), TraderFemas::WS_LOGINFAILED);
	ASSERT_EQ(sink.logins.size(), 1u);
	EXPECT_FALSE(sink.logins[0].ok);
	EXPECT_EQ(sink.logins[0].msg, "invalid password");
}

TEST_F(TraderFemasTest, qry_login_completes_initialization_when_already_logined)
{
	probe.setState(TraderFemas::WS_LOGINED);
	EXPECT_FALSE(probe.isConnected());

	probe.OnRspQueryUserLogin(nullptr, nullptr, 1, true);

	EXPECT_TRUE(probe.qryOnline());
	EXPECT_EQ(probe.state(), TraderFemas::WS_ALLREADY);
	EXPECT_TRUE(probe.isConnected());
	ASSERT_EQ(sink.logins.size(), 1u);
	EXPECT_TRUE(sink.logins[0].ok);
	// 注意：m_lDate 没有被构造函数初始化（见 bug 记录），
	// 所以 onLoginResult 上报的交易日在这里是不确定值，只能断言「就绪」这件事本身。
	EXPECT_EQ(sink.logins[0].msg, "");
}

TEST_F(TraderFemasTest, qry_login_before_user_login_does_not_open_channel)
{
	probe.OnRspQueryUserLogin(nullptr, nullptr, 1, true);

	EXPECT_TRUE(probe.qryOnline());
	// 还没拿到用户登录应答，不能宣布就绪
	EXPECT_EQ(probe.state(), TraderFemas::WS_NOTLOGIN);
	EXPECT_FALSE(probe.isConnected());
	EXPECT_TRUE(sink.logins.empty());
}

TEST_F(TraderFemasTest, logout_rsp_returns_channel_to_not_login)
{
	probe.setState(TraderFemas::WS_ALLREADY);
	probe.OnRspUserLogout(nullptr, nullptr, 1, true);

	EXPECT_EQ(probe.state(), TraderFemas::WS_NOTLOGIN);
	EXPECT_FALSE(probe.isConnected());
	ASSERT_EQ(sink.events.size(), 1u);
	EXPECT_EQ(sink.events[0].e, WTE_Logout);
}

TEST_F(TraderFemasTest, order_action_rejection_pushes_cancel_error)
{
	CUstpFtdcRspInfoField bad = makeRsp(21, "order not found");
	probe.OnRspOrderAction(nullptr, &bad, 1, true);

	ASSERT_EQ(sink.traderErrors.size(), 1u);
	EXPECT_EQ(sink.traderErrors[0].code, static_cast<int32_t>(WEC_ORDERCANCEL));
	EXPECT_EQ(sink.traderErrors[0].msg, "order not found");

	// 成功应答不产生错误
	CUstpFtdcRspInfoField ok = makeRsp(0, "");
	probe.OnRspOrderAction(nullptr, &ok, 2, true);
	EXPECT_EQ(sink.traderErrors.size(), 1u);
}

TEST_F(FemasContractFixture, order_insert_response_carries_error_and_entrust)
{
	CUstpFtdcInputOrderField in;
	memset(&in, 0, sizeof(in));
	strncpy(in.InstrumentID, "rb2401", sizeof(in.InstrumentID) - 1);
	strncpy(in.ExchangeID, "SHFE", sizeof(in.ExchangeID) - 1);
	strncpy(in.UserOrderLocalID, "A1000000000009", sizeof(in.UserOrderLocalID) - 1);
	in.Volume = 1;
	in.LimitPrice = 3500.0;
	CUstpFtdcRspInfoField bad = makeRsp(35, "risk denied");

	// 合约查不到 -> makeEntrust 返回 NULL -> 整条回调被丢弃（错误也就丢了）
	sink.m_mgr.m_contract = nullptr;
	probe.OnRspOrderInsert(&in, &bad, 1, true);
	EXPECT_TRUE(sink.entrustRsps.empty());

	sink.m_mgr.m_contract = contract;
	probe.OnRspOrderInsert(&in, &bad, 2, true);
	ASSERT_EQ(sink.entrustRsps.size(), 1u);
	EXPECT_EQ(sink.entrustRsps[0].code, "rb2401");
	EXPECT_DOUBLE_EQ(sink.entrustRsps[0].vol, 1);
	EXPECT_DOUBLE_EQ(sink.entrustRsps[0].price, 3500.0);
	EXPECT_EQ(sink.entrustRsps[0].errCode, 35);
	EXPECT_EQ(sink.entrustRsps[0].errMsg, "risk denied");

	// 无错误的应答也要把委托送上去，err 为 NULL
	CUstpFtdcRspInfoField ok = makeRsp(0, "");
	probe.OnRspOrderInsert(&in, &ok, 3, true);
	ASSERT_EQ(sink.entrustRsps.size(), 2u);
	EXPECT_EQ(sink.entrustRsps[1].errCode, 0);
}

/* ------------------------------------------------------------------ */
/* 查询队列 / 未就绪拒绝                                                */
/* ------------------------------------------------------------------ */

TEST_F(TraderFemasTest, queries_are_enqueued_only_after_channel_is_ready)
{
	probe.setApi(kApiSentinel);
	probe.setState(TraderFemas::WS_ALLREADY);

	EXPECT_EQ(probe.queryPositions(), 0);
	EXPECT_EQ(probe.queryOrders(), 0);
	EXPECT_EQ(probe.queryTrades(), 0);
	EXPECT_EQ(probe.queryAccount(), 0);

	// 四个查询各投递一个 handler；triggerQuery 只是 post，没有线程去跑它
	EXPECT_EQ(probe.queryPending(), 4u);
	// 队列里的 lambda 从未执行，哨兵指针也没有被解引用
	EXPECT_TRUE(sink.positions.empty());
	EXPECT_TRUE(sink.orders.empty());
	EXPECT_TRUE(sink.trades.empty());
	EXPECT_TRUE(sink.accounts.empty());
}

TEST_F(TraderFemasTest, order_insert_reaches_contract_lookup_before_using_api)
{
	probe.setApi(kApiSentinel);
	probe.setState(TraderFemas::WS_ALLREADY);

	WTSEntrust* entrust = WTSEntrust::create("unknown", 1, 3500.0, "SHFE");
	ASSERT_NE(entrust, nullptr);
	// 合约缺失 -> -1；这条分支在触碰厂商指针之前返回，所以哨兵是安全的
	EXPECT_EQ(probe.orderInsert(entrust), -1);
	entrust->release();

	// 状态不是 ALLREADY 时，即便带上了 API 也必须拒绝
	probe.setState(TraderFemas::WS_LOGINED);
	WTSEntrust* other = WTSEntrust::create("rb2401", 1, 3500.0, "SHFE");
	ASSERT_NE(other, nullptr);
	WTSEntrustAction* act = WTSEntrustAction::create("rb2401", "SHFE");
	ASSERT_NE(act, nullptr);
	EXPECT_EQ(probe.orderInsert(other), -1);
	EXPECT_EQ(probe.orderAction(act), -1);
	other->release();
	act->release();
}

/* ------------------------------------------------------------------ */
/* 查询应答                                                             */
/* ------------------------------------------------------------------ */

TEST_F(FemasContractFixture, account_query_response_computes_balance)
{
	CUstpFtdcRspInvestorAccountField acc;
	memset(&acc, 0, sizeof(acc));
	acc.PreBalance = 100000.0;
	acc.CloseProfit = 5000.0;
	acc.PositionProfit = 1200.0;
	acc.Margin = 8000.0;
	acc.Available = 90000.0;
	acc.Fee = 300.0;
	acc.FrozenMargin = 1000.0;
	acc.FrozenFee = 50.0;
	acc.Deposit = 2000.0;
	acc.Withdraw = 400.0;
	CUstpFtdcRspInfoField ok = makeRsp(0, "");

	probe.OnRspQryInvestorAccount(&acc, &ok, 1, false);
	// 非最后一包不落地
	EXPECT_TRUE(sink.accounts.empty());

	probe.OnRspQryInvestorAccount(&acc, &ok, 1, true);
	ASSERT_EQ(sink.accounts.size(), 1u);
	EXPECT_EQ(sink.accounts[0].size, 1u);
	// balance = 100000 + 5000 - 300 + 2000 - 400
	EXPECT_DOUBLE_EQ(sink.accounts[0].balance, 106300.0);
	EXPECT_DOUBLE_EQ(sink.accounts[0].available, 90000.0);
	EXPECT_EQ(sink.accounts[0].currency, "CNY");
}

TEST_F(FemasContractFixture, position_query_response_never_appends_the_item_it_builds)
{
	CUstpFtdcRspInvestorPositionField pos;
	memset(&pos, 0, sizeof(pos));
	strncpy(pos.InstrumentID, "rb2401", sizeof(pos.InstrumentID) - 1);
	strncpy(pos.ExchangeID, "SHFE", sizeof(pos.ExchangeID) - 1);
	pos.Position = 5;
	pos.YdPosition = 3;
	pos.FrozenPosition = 1;
	pos.UsedMargin = 20000.0;
	pos.PositionCost = 175000.0;
	pos.Direction = static_cast<TUstpFtdcDirectionType>(USTP_FTDC_D_Buy);
	CUstpFtdcRspInfoField ok = makeRsp(0, "");

	probe.OnRspQryInvestorPosition(&pos, &ok, 1, true);

	// BUG（见 bugs/T4_femas_ctploader_mini.md）：构造出的 WTSPositionItem 从未 append，
	// 所以对外汇报的持仓数组永远是空的。该断言在修复后会失败，属于故意留下的哨兵。
	ASSERT_EQ(sink.positions.size(), 1u);
	EXPECT_EQ(sink.positions[0].size, 0u);
	EXPECT_EQ(sink.m_mgr.m_getContractCalls, 1u);
}

TEST_F(FemasContractFixture, trade_query_response_reports_appended_records)
{
	CUstpFtdcTradeField td;
	memset(&td, 0, sizeof(td));
	strncpy(td.InstrumentID, "rb2401", sizeof(td.InstrumentID) - 1);
	strncpy(td.ExchangeID, "SHFE", sizeof(td.ExchangeID) - 1);
	strncpy(td.TradingDay, "20240105", sizeof(td.TradingDay) - 1);
	strncpy(td.TradeTime, "09:00:01", sizeof(td.TradeTime) - 1);
	strncpy(td.TradeID, "T-1", sizeof(td.TradeID) - 1);
	td.TradeVolume = 1;
	td.TradePrice = 3500.0;
	CUstpFtdcRspInfoField ok = makeRsp(0, "");
	CUstpFtdcRspInfoField bad = makeRsp(9, "query too frequent");

	probe.OnRspQryTrade(&td, &ok, 1, false);
	EXPECT_TRUE(sink.trades.empty());

	// 错误应答不收集记录，但最后一包仍然要汇报
	probe.OnRspQryTrade(&td, &bad, 1, true);
	ASSERT_EQ(sink.trades.size(), 1u);
	EXPECT_EQ(sink.trades[0].size, 0u);

	probe.OnRspQryTrade(&td, &ok, 2, true);
	ASSERT_EQ(sink.trades.size(), 2u);
	EXPECT_EQ(sink.trades[1].size, 1u);
	EXPECT_EQ(sink.trades[1].firstCode, "rb2401");
}

TEST_F(FemasContractFixture, order_query_and_push_callbacks)
{
	CUstpFtdcOrderField ord;
	memset(&ord, 0, sizeof(ord));
	strncpy(ord.InstrumentID, "rb2401", sizeof(ord.InstrumentID) - 1);
	strncpy(ord.ExchangeID, "SHFE", sizeof(ord.ExchangeID) - 1);
	strncpy(ord.TradingDay, "20240105", sizeof(ord.TradingDay) - 1);
	strncpy(ord.InsertTime, "09:15:00", sizeof(ord.InsertTime) - 1);
	strncpy(ord.UserOrderLocalID, "A1000000000011", sizeof(ord.UserOrderLocalID) - 1);
	strncpy(ord.OrderSysID, "OS-1", sizeof(ord.OrderSysID) - 1);
	ord.Volume = 4;
	ord.VolumeTraded = 2;
	ord.VolumeRemain = 2;
	ord.LimitPrice = 3510.0;
	CUstpFtdcRspInfoField ok = makeRsp(0, "");

	probe.OnRspQryOrder(&ord, &ok, 1, true);
	ASSERT_EQ(sink.orders.size(), 1u);
	EXPECT_EQ(sink.orders[0].size, 1u);
	EXPECT_EQ(sink.orders[0].firstCode, "rb2401");

	probe.OnRtnOrder(&ord);
	ASSERT_EQ(sink.pushedOrders.size(), 1u);
	EXPECT_EQ(sink.pushedOrders[0].code, "rb2401");
	EXPECT_EQ(sink.pushedOrders[0].orderID, "OS-1");
	EXPECT_DOUBLE_EQ(sink.pushedOrders[0].volLeft, 2);

	CUstpFtdcTradeField td;
	memset(&td, 0, sizeof(td));
	strncpy(td.InstrumentID, "rb2401", sizeof(td.InstrumentID) - 1);
	strncpy(td.ExchangeID, "SHFE", sizeof(td.ExchangeID) - 1);
	strncpy(td.TradingDay, "20240105", sizeof(td.TradingDay) - 1);
	strncpy(td.TradeTime, "09:16:00", sizeof(td.TradeTime) - 1);
	strncpy(td.TradeID, "T-9", sizeof(td.TradeID) - 1);
	td.TradeVolume = 2;
	td.TradePrice = 3512.0;
	probe.OnRtnTrade(&td);
	ASSERT_EQ(sink.pushedTrades.size(), 1u);
	EXPECT_EQ(sink.pushedTrades[0].tradeID, "T-9");
	EXPECT_DOUBLE_EQ(sink.pushedTrades[0].amount, 10 * 2 * 3512.0);
}

TEST_F(TraderFemasTest, instrument_status_push_casts_vendor_status)
{
	CUstpFtdcInstrumentStatusField st;
	memset(&st, 0, sizeof(st));
	strncpy(st.ExchangeID, "DCE", sizeof(st.ExchangeID) - 1);
	strncpy(st.InstrumentID, "i2405", sizeof(st.InstrumentID) - 1);
	st.InstrumentStatus = static_cast<TUstpFtdcInstrumentStatusType>(2);

	probe.OnRtnInstrumentStatus(&st);
	ASSERT_EQ(sink.statuses.size(), 1u);
	EXPECT_EQ(sink.statuses[0].exchg, "DCE");
	EXPECT_EQ(sink.statuses[0].code, "i2405");
	EXPECT_EQ(sink.statuses[0].st, static_cast<WTSTradeStatus>(2));

	// 没有 sink 时必须静默返回。
	// 注意：m_sink 没有被构造函数初始化，所以只能先 registerSpi(nullptr) 再触发回调，
	// 否则就是解引用野指针（见 bug 记录）。
	FemasProbe alone;
	alone.registerSpi(nullptr);
	EXPECT_NO_THROW(alone.OnRtnInstrumentStatus(&st));
}

TEST_F(TraderFemasTest, error_rsp_is_written_to_log_with_request_id)
{
	CUstpFtdcRspInfoField bad = makeRsp(70, "not ready");
	probe.OnRspError(&bad, 12, true);

	ASSERT_EQ(sink.logs.size(), 1u);
	EXPECT_EQ(sink.logLevels[0], LL_ERROR);
	EXPECT_NE(sink.logs[0].find("not ready"), std::string::npos);
	EXPECT_NE(sink.logs[0].find("request id: 12"), std::string::npos);

	// 两个空实现回调不应产生任何副作用
	EXPECT_NO_THROW(probe.OnErrRtnOrderInsert(nullptr, &bad));
	EXPECT_NO_THROW(probe.OnErrRtnOrderAction(nullptr, &bad));
	EXPECT_NO_THROW(probe.OnQryFrontConnected());
	EXPECT_TRUE(sink.events.empty());
}

TEST_F(TraderFemasTest, set_and_get_tag)
{
	probe.setTag("T4-lane");
	EXPECT_STREQ(probe.getTag(), "T4-lane");
	probe.setTag("other");
	EXPECT_STREQ(probe.getTag(), "other");
}

/* ------------------------------------------------------------------ */
/* 崩溃/挂死路径：整条 DISABLED，说明见 bugs/T4_femas_ctploader_mini.md  */
/* ------------------------------------------------------------------ */

/*
 * BUG-T4-001/004：m_pUserAPI 为空时，OnRspUserLogin 的成功分支会直接
 * m_pUserAPI->GetTradingDay()（TraderFemas.cpp:578）→ 必崩。
 * 要跑通这条需要一个真实的 CUstpFtdcTraderApi 实例（厂商 .dylib），
 * 而 CUstpFtdcTraderApi 是 47 个纯虚函数的抽象类 + 本仓不产出该库 → 不启用。
 */
TEST_F(TraderFemasTest, DISABLED_user_login_success_branch_needs_vendor_api)
{
	probe.setApi(kApiSentinel);
	CUstpFtdcRspUserLoginField login;
	memset(&login, 0, sizeof(login));
	strncpy(login.MaxOrderLocalID, "000000001234", sizeof(login.MaxOrderLocalID) - 1);
	CUstpFtdcRspInfoField ok = makeRsp(0, "");

	probe.OnRspUserLogin(&login, &ok, 1, true);
	// 期望：WS_LOGINED + 会话号取自 MaxOrderLocalID 去掉左边的 0
	EXPECT_EQ(probe.state(), TraderFemas::WS_LOGINED);
}

/*
 * BUG-T4-003：triggerQuery() 无条件解引用 m_strandIO，而它只在 connect() 里 new。
 * 未 connect 就 queryAccount() → post(*NULL) → 崩。上面的启用用例都把 strand 补上了，
 * 这条保留「真实调用顺序」的原始形状，因此不能启用。
 */
TEST(TraderFemasDisabled, DISABLED_query_account_before_connect_dereferences_null_strand)
{
	FemasProbe probe;
	probe.registerSpi(nullptr);
	probe.setApi(kApiSentinel);
	probe.setState(TraderFemas::WS_ALLREADY);
	EXPECT_EQ(probe.queryAccount(), 0);
}

/*
 * BUG-T4-005：makeEntrustID 只判断 length==0，不校验缓冲区实际容量。
 * 传一个小缓冲区，format_to 仍会写「会话号 + 12 位数字」→ 越界写。
 * 不启用：这是内存破坏，不是可断言的行为差异。
 */
TEST(TraderFemasDisabled, DISABLED_make_entrust_id_ignores_buffer_capacity)
{
	FemasProbe probe;
	probe.registerSpi(nullptr);
	probe.setSessionID("SESSIONVERYLONGID");
	char tiny[8];
	memset(tiny, 0, sizeof(tiny));
	probe.makeEntrustID(tiny, static_cast<int>(sizeof(tiny)));
}

/*
 * init() 走 DLLHelper::load_library（dlopen 厂商库）→ 与「不 dlopen」的约束冲突，
 * 且拿不到符号后 m_funcCreator 仍是未校验状态，connect() 会空指针调用。
 * 本轮只从静态审查记录，不在 CI 里跑。
 */
TEST(TraderFemasDisabled, DISABLED_init_dlopens_vendor_module)
{
	FemasProbe probe;
	probe.registerSpi(nullptr);
	WTSVariant* cfg = WTSCfgLoader::load_from_content(
		"front: tcp://127.0.0.1:1234\nbroker: brk\nuser: usr\npass: pwd\n", true);
	ASSERT_NE(cfg, nullptr);
	EXPECT_TRUE(probe.init(cfg));
	cfg->release();
}
