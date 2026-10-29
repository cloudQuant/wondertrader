/*!
 * \file ctpmini_trader_callbacks_test.cpp
 * \project WonderTrader — 覆盖率补写第六波（收发族）lane T3 / TraderCTPMini
 *
 * 手法：TraderCTPMini 的转换/构造函数与状态字段都是 protected（TraderCTPMini.h:125-158），
 *       所以用「测试派生类 + using 声明」暴露，不用 `#define private public`。
 *       厂商 CThostFtdcTraderApi 有 49 个纯虚函数且 init() 需 dlopen 厂商库（本机没有），
 *       m_pUserAPI 恒为 NULL → 所有下单/查询的正向分支只能 DISABLED（见 bug 文件）。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "TraderCTPMini/TraderCTPMini.h"

#include "WTSDataDef.hpp"
#include "WTSTradeDef.hpp"
#include "WTSContractInfo.hpp"
#include "IBaseDataMgr.h"
#include "ITraderApi.h"
#include "TimeUtils.hpp"

USING_NS_WTP;

namespace {

//////////////////////////////////////////////////////////////////////////
// 派生探针：暴露 protected 成员，并提供状态注入口
//////////////////////////////////////////////////////////////////////////
class Probe : public TraderCTPMini
{
public:
	using TraderCTPMini::IsErrorRspInfo;
	using TraderCTPMini::wrapPriceType;
	using TraderCTPMini::wrapDirectionType;
	using TraderCTPMini::wrapOffsetType;
	using TraderCTPMini::wrapTimeCondition;
	using TraderCTPMini::wrapActionFlag;
	using TraderCTPMini::wrapPosDirection;
	using TraderCTPMini::wrapOrderState;
	using TraderCTPMini::makeOrderInfo;
	using TraderCTPMini::makeEntrust;
	using TraderCTPMini::makeError;
	using TraderCTPMini::makeTradeRecord;
	using TraderCTPMini::generateEntrustID;
	using TraderCTPMini::extractEntrustID;
	using TraderCTPMini::genRequestID;

	void setState(WrapperState s) { m_wrapperState = s; }
	void setSession(uint32_t front, int32_t session, uint32_t ref)
	{
		m_frontID = front;
		m_sessionID = session;
		m_orderRef.store(ref);
	}
	void setBdMgr(IBaseDataMgr* mgr) { m_bdMgr = mgr; }
	void setTradeDate(uint32_t d) { m_lDate = d; }
	void setBrokerUser(const char* b, const char* u) { m_strBroker = b; m_strUser = u; }
	WrapperState state() const { return m_wrapperState; }
};

//////////////////////////////////////////////////////////////////////////
// 假 bdMgr：只认一个合约
//////////////////////////////////////////////////////////////////////////
class FakeDataMgr : public IBaseDataMgr
{
public:
	WTSContractInfo* ct;
	FakeDataMgr() : ct(NULL) {}
	virtual WTSCommodityInfo* getCommodity(const char* /*exchgpid*/) override { return NULL; }
	virtual WTSCommodityInfo* getCommodity(const char* /*exchg*/, const char* /*pid*/) override { return NULL; }
	virtual WTSContractInfo* getContract(const char* code, const char* /*exchg*/ = "", uint32_t /*uDate*/ = 0) override
	{
		if (ct == NULL || code == NULL)
			return NULL;
		return strcmp(code, ct->getCode()) == 0 ? ct : NULL;
	}
	virtual WTSArray* getContracts(const char* /*exchg*/ = "", uint32_t /*uDate*/ = 0) override { return NULL; }
	virtual WTSSessionInfo* getSession(const char* /*sid*/) override { return NULL; }
	virtual WTSSessionInfo* getSessionByCode(const char* /*code*/, const char* /*exchg*/ = "") override { return NULL; }
	virtual WTSArray* getAllSessions() override { return NULL; }
	virtual bool isHoliday(const char* /*pid*/, uint32_t /*uDate*/, bool /*isTpl*/ = false) override { return false; }
	virtual uint32_t calcTradingDate(const char* /*stdPID*/, uint32_t /*uDate*/, uint32_t /*uTime*/, bool /*isSession*/ = false) override { return 0; }
	virtual uint64_t getBoundaryTime(const char* /*stdPID*/, uint32_t /*tDate*/, bool /*isSession*/ = false, bool /*isStart*/ = true) override { return 0; }
};

//////////////////////////////////////////////////////////////////////////
// 假交易回调 sink：录下所有回抛，用于证伪
//////////////////////////////////////////////////////////////////////////
class FakeTraderSink : public ITraderSpi
{
public:
	std::vector<std::pair<int32_t, int32_t> > events;
	std::vector<std::string> logs;
	std::vector<WTSLogLevel> logLevels;
	int loginResults;
	bool lastLoginSucc;
	std::string lastLoginMsg;
	int32_t lastTraderErrorCode;
	std::string lastTraderErrorMsg;
	uint32_t lastTradeDate;
	int entrustRsp;
	int accountRsp;
	int orderPush;
	int tradePush;
	int errorRsp;
	uint32_t lastOrderCnt;
	uint32_t lastAccountCnt;
	uint32_t lastPosCnt;
	uint32_t lastTradeCnt;
	uint32_t lastOrderCntRsp;
	double posTotal, posNew, posPre, posAvailNew, posAvailPre, posAvg, posMargin;
	WTSDirectionType posDir;
	double tradeAmount, tradeVol, tradePrice;
	uint32_t tradeDate;
	uint64_t tradeTime;
	uint32_t orderDate;
	uint64_t orderTime;
	double volLeft, volTraded;
	WTSOrderFlag orderFlag;
	WTSOrderState orderState;
	std::string entrustID, userTag, code, exchg, orderID;
	std::string statusExchg, statusCode;
	WTSTradeStatus status;
	FakeDataMgr* mgr;

	FakeTraderSink() : loginResults(0), lastLoginSucc(false), lastTraderErrorCode((int32_t)WEC_NONE), lastTradeDate(0), entrustRsp(0)
		, accountRsp(0), orderPush(0), tradePush(0), errorRsp(0)
		, lastOrderCnt(0), lastAccountCnt(0), lastPosCnt(0), lastTradeCnt(0), lastOrderCntRsp(0)
		, posTotal(0), posNew(0), posPre(0), posAvailNew(0), posAvailPre(0), posAvg(0), posMargin(0)
		, posDir(WDT_LONG), tradeAmount(0), tradeVol(0), tradePrice(0), tradeDate(0), tradeTime(0)
		, orderDate(0), orderTime(0), volLeft(0), volTraded(0), orderFlag(WOF_NOR), orderState(WOS_Submitting)
		, status(TS_None), mgr(NULL)
	{
	}

	virtual IBaseDataMgr* getBaseDataMgr() override { return mgr; }
	virtual void handleTraderLog(WTSLogLevel ll, const char* message) override
	{
		logLevels.push_back(ll);
		logs.push_back(message == NULL ? std::string("<null>") : message);
	}
	virtual void handleEvent(WTSTraderEvent e, int32_t ec) override { events.push_back(std::make_pair((int32_t)e, ec)); }
	virtual void onLoginResult(bool bSucc, const char* msg, uint32_t tradingdate) override
	{
		++loginResults;
		lastLoginSucc = bSucc;
		lastLoginMsg = msg;
		lastTradeDate = tradingdate;
	}
	virtual void onRspEntrust(WTSEntrust* /*entrust*/, WTSError* /*err*/) override { ++entrustRsp; }
	virtual void onRspAccount(WTSArray* ayAccounts) override { ++accountRsp; lastAccountCnt = ayAccounts == NULL ? 0 : ayAccounts->size(); }
	virtual void onRspPosition(const WTSArray* ayPositions) override
	{
		lastPosCnt = ayPositions == NULL ? 0 : ((WTSArray*)ayPositions)->size();
		if (lastPosCnt > 0)
		{
			WTSPositionItem* it = (WTSPositionItem*)((WTSArray*)ayPositions)->at(0);
			posDir = it->getDirection();
			posTotal = it->getTotalPosition();
			posNew = it->getNewPosition();
			posPre = it->getPrePosition();
			posAvailNew = it->getAvailNewPos();
			posAvailPre = it->getAvailPrePos();
			posAvg = it->getAvgPrice();
			posMargin = it->getMargin();
			code = it->getCode();
		}
	}
	virtual void onRspOrders(const WTSArray* ayOrders) override { lastOrderCntRsp = ayOrders == NULL ? 0 : ((WTSArray*)ayOrders)->size(); }
	virtual void onRspTrades(const WTSArray* ayTrades) override { lastTradeCnt = ayTrades == NULL ? 0 : ((WTSArray*)ayTrades)->size(); }
	virtual void onPushOrder(WTSOrderInfo* o) override
	{
		++orderPush;
		if (o == NULL)
			return;
		orderDate = o->getOrderDate();
		orderTime = o->getOrderTime();
		orderFlag = o->getOrderFlag();
		orderState = o->getOrderState();
		volLeft = o->getVolLeft();
		volTraded = o->getVolTraded();
		entrustID = o->getEntrustID();
		userTag = o->getUserTag();
		code = o->getCode();
		exchg = o->getExchg();
		orderID = o->getOrderID();
	}
	virtual void onPushTrade(WTSTradeInfo* t) override
	{
		++tradePush;
		if (t == NULL)
			return;
		tradeAmount = t->getAmount();
		tradeVol = t->getVolume();
		tradePrice = t->getPrice();
		tradeDate = t->getTradeDate();
		tradeTime = t->getTradeTime();
		code = t->getCode();
	}
	virtual void onTraderError(WTSError* err, void* /*pData*/ = NULL) override
	{
		++errorRsp;
		if (err != NULL)
		{
			// Copy only fields needed by the assertion; the caller owns/releases WTSError.
			lastTraderErrorCode = (int32_t)err->getErrorCode();
			lastTraderErrorMsg = err->getMessage();
		}
	}
	virtual void onPushInstrumentStatus(const char* exchg, const char* code, WTSTradeStatus st) override
	{
		statusExchg = exchg;
		statusCode = code;
		status = st;
	}
};

// 合约夹具
struct CommFixture
{
	WTSCommodityInfo* comm;
	WTSContractInfo* ct;
	FakeDataMgr mgr;

	explicit CommFixture(const char* exchg = "SHFE", CoverMode cm = CM_OpenCover, uint32_t vs = 10)
	{
		comm = WTSCommodityInfo::create("ru", "橡胶", exchg, "SHFE_day", "SHFE", "CNY");
		comm->setVolScale(vs);
		comm->setCoverMode(cm);
		comm->setCategory(CC_Future);
		ct = WTSContractInfo::create("ru2601", "橡胶2601", exchg, "ru");
		ct->setCommInfo(comm);
		ct->setVolScale(vs);
		mgr.ct = ct;
	}
	~CommFixture()
	{
		ct->release();
		comm->release();
	}
};

CThostFtdcRspInfoField okRsp()
{
	CThostFtdcRspInfoField r;
	memset(&r, 0, sizeof(r));
	return r;
}

CThostFtdcRspInfoField errRsp(int id, const char* msg)
{
	CThostFtdcRspInfoField r = okRsp();
	r.ErrorID = id;
	strncpy(r.ErrorMsg, msg, sizeof(r.ErrorMsg) - 1);
	return r;
}

} // namespace

//////////////////////////////////////////////////////////////////////////
// 未登录 / 未就绪的拒绝分支（全部不依赖厂商 API）
//////////////////////////////////////////////////////////////////////////
TEST(ctpmini, not_ready_channel_rejects_every_request)
{
	Probe api;
	FakeTraderSink sink;
	sink.mgr = NULL;
	api.registerSpi(&sink);

	// isConnected 只在 WS_ALLREADY 后才为真（:1328）
	EXPECT_FALSE(api.isConnected());
	EXPECT_EQ(TraderCTPMini::WS_NOTLOGIN, api.state());

	// m_pUserAPI==NULL 或未就绪 → 每个入口都必须 -1（:276/:306/:326/:423/:462/:485/:508/:531）
	EXPECT_EQ(-1, api.login("u", "p", "prod"));
	EXPECT_EQ(-1, api.logout());
	EXPECT_EQ(-1, api.orderInsert(NULL));
	EXPECT_EQ(-1, api.orderAction(NULL));
	EXPECT_EQ(-1, api.queryAccount());
	EXPECT_EQ(-1, api.queryPositions());
	EXPECT_EQ(-1, api.queryOrders());
	EXPECT_EQ(-1, api.queryTrades());

	// 登录失败也不能把状态推到就绪
	api.setState(TraderCTPMini::WS_LOGINED);
	EXPECT_FALSE(api.isConnected());
	EXPECT_EQ(-1, api.queryAccount());
	EXPECT_EQ(-1, api.orderAction(NULL));
}

TEST(ctpmini, ready_state_still_rejects_without_api_pointer)
{
	Probe api;
	api.setState(TraderCTPMini::WS_ALLREADY);
	EXPECT_TRUE(api.isConnected());

	// orderAction 的 eid 解析先于 api 解引用：畸形 eid 也要 -1 而不是崩
	WTSEntrustAction* act = WTSEntrustAction::create("ru2601", "SHFE");
	act->setEntrustID("garbage-no-separator");
	act->setActionFlag(WAF_CANCEL);
	EXPECT_EQ(-1, api.orderAction(act));
	act->release();

	// query* 走 `m_pUserAPI == NULL` 分支
	EXPECT_EQ(-1, api.queryOrders());
}

//////////////////////////////////////////////////////////////////////////
// makeEntrustID / generate / extract
//////////////////////////////////////////////////////////////////////////
TEST(ctpmini, make_entrust_id_rejects_bad_buffer)
{
	Probe api;
	char buf[64];
	EXPECT_FALSE(api.makeEntrustID(NULL, sizeof(buf)));
	EXPECT_FALSE(api.makeEntrustID(buf, 0));
}

TEST(ctpmini, make_entrust_id_formats_front_session_ref_and_increments)
{
	Probe api;
	api.setSession(7, 9, 100);

	char buf[64];
	memset(buf, 0, sizeof(buf));
	ASSERT_TRUE(api.makeEntrustID(buf, sizeof(buf)));
	// {:06d}#{:010d}#{:06d}，orderref = fetch_add(1)+1 → 101（:244）
	EXPECT_STREQ("000007#0000000009#000101", buf);

	char buf2[64];
	ASSERT_TRUE(api.makeEntrustID(buf2, sizeof(buf2)));
	EXPECT_STREQ("000007#0000000009#000102", buf2);
}

TEST(ctpmini, generate_and_extract_entrust_id_roundtrip)
{
	Probe api;
	char buf[64];
	memset(buf, 0, sizeof(buf));
	api.generateEntrustID(buf, 12345, 6789, 42);
	EXPECT_STREQ("012345#0000006789#000042", buf);

	uint32_t front = 0, session = 0, ref = 0;
	ASSERT_TRUE(api.extractEntrustID(buf, front, session, ref));
	EXPECT_EQ(12345u, front);
	EXPECT_EQ(6789u, session);
	EXPECT_EQ(42u, ref);

	// 缺分隔符 → false（:1282/:1289）
	EXPECT_FALSE(api.extractEntrustID("123456789", front, session, ref));
	EXPECT_FALSE(api.extractEntrustID("1#2", front, session, ref));
}

TEST(ctpmini, request_id_is_monotonic)
{
	Probe api;
	const uint32_t a = api.genRequestID();
	const uint32_t b = api.genRequestID();
	EXPECT_EQ(a + 1, b);
}

//////////////////////////////////////////////////////////////////////////
// 代码/标志双向换算
//////////////////////////////////////////////////////////////////////////
TEST(ctpmini, wrap_direction_and_offset_tables)
{
	Probe api;
	// WTS -> CTP
	EXPECT_EQ((int)THOST_FTDC_D_Buy, api.wrapDirectionType(WDT_LONG, WOT_OPEN));
	EXPECT_EQ((int)THOST_FTDC_D_Sell, api.wrapDirectionType(WDT_LONG, WOT_CLOSE));
	EXPECT_EQ((int)THOST_FTDC_D_Sell, api.wrapDirectionType(WDT_SHORT, WOT_OPEN));
	EXPECT_EQ((int)THOST_FTDC_D_Buy, api.wrapDirectionType(WDT_SHORT, WOT_CLOSE));

	EXPECT_EQ((int)THOST_FTDC_OF_Open, api.wrapOffsetType(WOT_OPEN));
	EXPECT_EQ((int)THOST_FTDC_OF_Close, api.wrapOffsetType(WOT_CLOSE));
	EXPECT_EQ((int)THOST_FTDC_OF_CloseToday, api.wrapOffsetType(WOT_CLOSETODAY));
	EXPECT_EQ((int)THOST_FTDC_OF_Close, api.wrapOffsetType(WOT_CLOSEYESTERDAY));
	EXPECT_EQ((int)THOST_FTDC_OF_ForceClose, api.wrapOffsetType(WOT_FORCECLOSE));

	// CTP -> WTS（含卖开=空头、卖平=多头）
	EXPECT_EQ(WDT_LONG, api.wrapDirectionType(THOST_FTDC_D_Buy, THOST_FTDC_OF_Open));
	EXPECT_EQ(WDT_SHORT, api.wrapDirectionType(THOST_FTDC_D_Buy, THOST_FTDC_OF_Close));
	EXPECT_EQ(WDT_SHORT, api.wrapDirectionType(THOST_FTDC_D_Sell, THOST_FTDC_OF_Open));
	EXPECT_EQ(WDT_LONG, api.wrapDirectionType(THOST_FTDC_D_Sell, THOST_FTDC_OF_Close));

	EXPECT_EQ(WOT_OPEN, api.wrapOffsetType(THOST_FTDC_OF_Open));
	EXPECT_EQ(WOT_CLOSE, api.wrapOffsetType(THOST_FTDC_OF_Close));
	EXPECT_EQ(WOT_CLOSETODAY, api.wrapOffsetType(THOST_FTDC_OF_CloseToday));
	EXPECT_EQ(WOT_FORCECLOSE, api.wrapOffsetType(THOST_FTDC_OF_CloseYesterday));

	EXPECT_EQ(WDT_LONG, api.wrapPosDirection(THOST_FTDC_PD_Long));
	EXPECT_EQ(WDT_SHORT, api.wrapPosDirection(THOST_FTDC_PD_Short));
}

TEST(ctpmini, wrap_price_time_action_and_order_state)
{
	Probe api;
	// 市价单在中金所要转成五档即时价（:1025）
	EXPECT_EQ((int)THOST_FTDC_OPT_FiveLevelPrice, api.wrapPriceType(WPT_ANYPRICE, true));
	EXPECT_EQ((int)THOST_FTDC_OPT_AnyPrice, api.wrapPriceType(WPT_ANYPRICE, false));
	EXPECT_EQ((int)THOST_FTDC_OPT_LimitPrice, api.wrapPriceType(WPT_LIMITPRICE, true));
	EXPECT_EQ((int)THOST_FTDC_OPT_BestPrice, api.wrapPriceType(WPT_BESTPRICE, false));
	EXPECT_EQ((int)THOST_FTDC_OPT_LastPrice, api.wrapPriceType(WPT_LASTPRICE, false));

	EXPECT_EQ(WPT_ANYPRICE, api.wrapPriceType(THOST_FTDC_OPT_AnyPrice));
	EXPECT_EQ(WPT_ANYPRICE, api.wrapPriceType(THOST_FTDC_OPT_FiveLevelPrice));
	EXPECT_EQ(WPT_LIMITPRICE, api.wrapPriceType(THOST_FTDC_OPT_LimitPrice));
	EXPECT_EQ(WPT_BESTPRICE, api.wrapPriceType(THOST_FTDC_OPT_BestPrice));
	EXPECT_EQ(WPT_LASTPRICE, api.wrapPriceType(THOST_FTDC_OPT_LastPrice));

	EXPECT_EQ((int)THOST_FTDC_TC_IOC, api.wrapTimeCondition(WTC_IOC));
	EXPECT_EQ((int)THOST_FTDC_TC_GFD, api.wrapTimeCondition(WTC_GFD));
	EXPECT_EQ((int)THOST_FTDC_TC_GFS, api.wrapTimeCondition(WTC_GFS));
	EXPECT_EQ(WTC_IOC, api.wrapTimeCondition(THOST_FTDC_TC_IOC));
	EXPECT_EQ(WTC_GFD, api.wrapTimeCondition(THOST_FTDC_TC_GFD));
	EXPECT_EQ(WTC_GFS, api.wrapTimeCondition(THOST_FTDC_TC_GFS));

	EXPECT_EQ((int)THOST_FTDC_AF_Delete, api.wrapActionFlag(WAF_CANCEL));
	EXPECT_EQ((int)THOST_FTDC_AF_Modify, api.wrapActionFlag(WAF_MODIFY));

	// Unknown 状态要回落成「已提交」（:1067）
	EXPECT_EQ(WOS_Submitting, api.wrapOrderState(THOST_FTDC_OST_Unknown));
	EXPECT_EQ((WTSOrderState)THOST_FTDC_OST_AllTraded, api.wrapOrderState(THOST_FTDC_OST_AllTraded));
}

TEST(ctpmini, is_error_rsp_info_and_make_error)
{
	Probe api;
	CThostFtdcRspInfoField r0 = okRsp();
	CThostFtdcRspInfoField r1 = errRsp(39, "资金不足");

	EXPECT_FALSE(api.IsErrorRspInfo(NULL));
	EXPECT_FALSE(api.IsErrorRspInfo(&r0));
	EXPECT_TRUE(api.IsErrorRspInfo(&r1));

	WTSError* err = api.makeError(&r1);
	ASSERT_NE((WTSError*)NULL, err);
	EXPECT_EQ((WTSErroCode)39, err->getErrorCode());
	EXPECT_STREQ("资金不足", err->getMessage());
	err->release();
}

//////////////////////////////////////////////////////////////////////////
// 连接/登录类回调
//////////////////////////////////////////////////////////////////////////
TEST(ctpmini, front_events_move_state_and_notify_sink)
{
	Probe api;
	FakeTraderSink sink;
	api.registerSpi(&sink);

	api.OnFrontConnected();
	ASSERT_EQ(1u, sink.events.size());
	EXPECT_EQ((int32_t)WTE_Connect, sink.events[0].first);
	EXPECT_EQ(0, sink.events[0].second);

	// 断开要把状态打回未登录，并把原因码透传（:559）
	api.setState(TraderCTPMini::WS_ALLREADY);
	api.OnFrontDisconnected(-100);
	EXPECT_EQ(TraderCTPMini::WS_NOTLOGIN, api.state());
	ASSERT_EQ(2u, sink.events.size());
	EXPECT_EQ((int32_t)WTE_Close, sink.events[1].first);
	EXPECT_EQ(-100, sink.events[1].second);

	api.OnRspUserLogout(NULL, NULL, 1, true);
	ASSERT_EQ(3u, sink.events.size());
	EXPECT_EQ((int32_t)WTE_Logout, sink.events[2].first);

	// 心跳与 RspError 目前是空实现/仅日志
	api.OnHeartBeatWarning(5);
	EXPECT_TRUE(sink.logs.empty());
	sink.mgr = NULL;
	api.OnRspError(NULL, 1, true);
	EXPECT_EQ(3u, sink.events.size());
}

TEST(ctpmini, register_spi_caches_bd_mgr_pointer)
{
	Probe api;
	FakeTraderSink sink;
	CommFixture fx;
	sink.mgr = &fx.mgr;

	api.registerSpi(&sink);
	// registerSpi:256 把 sink->getBaseDataMgr() 存进 m_bdMgr；这里 sink 没有 mgr，
	// 所以只能验证「注册/反注册不崩」，真正的 mgr 注入在下面各转换用例里
	api.OnFrontConnected();
	EXPECT_EQ(1u, sink.events.size());
	api.registerSpi(NULL);
	EXPECT_NO_THROW(api.OnRspError(NULL, 0, false));
}

TEST(ctpmini, authenticate_failure_marks_login_failed_and_reports)
{
	Probe api;
	FakeTraderSink sink;
	api.registerSpi(&sink);
	api.setBrokerUser("9999", "tester");

	CThostFtdcRspInfoField bad = errRsp(2, "认证失败");
	api.OnRspAuthenticate(NULL, &bad, 1, true);

	EXPECT_EQ(TraderCTPMini::WS_LOGINFAILED, api.state());
	EXPECT_EQ(1, sink.loginResults);
	EXPECT_FALSE(sink.lastLoginSucc);
	EXPECT_STREQ("认证失败", sink.lastLoginMsg.c_str());
	EXPECT_EQ(0u, sink.lastTradeDate);
	// 失败分支要留下日志
	ASSERT_TRUE(sink.logs.size() > 0);
}

TEST(ctpmini, order_action_success_does_not_push_trader_error)
{
	Probe api;
	FakeTraderSink sink;
	api.registerSpi(&sink);

	CThostFtdcRspInfoField good = okRsp();
	api.OnRspOrderAction(NULL, &good, 1, true);
	EXPECT_EQ(0, sink.errorRsp);
}

TEST(ctpmini, order_action_error_pushes_cancel_error_once)
{
	Probe api;
	FakeTraderSink sink;
	api.registerSpi(&sink);

	CThostFtdcRspInfoField bad = errRsp(15, "撤单失败");
	api.OnRspOrderAction(NULL, &bad, 2, true);
	ASSERT_EQ(1, sink.errorRsp);
	EXPECT_EQ((int32_t)WEC_ORDERCANCEL, sink.lastTraderErrorCode);
	EXPECT_EQ("撤单失败", sink.lastTraderErrorMsg);

	// OnRspOrderInsert/OnErrRtnOrderInsert 没有 bdMgr 时 makeEntrust 会解引用 NULL →
	// 这两条只能 DISABLED（见 ctpminiDISABLED 用例）
}

TEST(ctpmini, instrument_status_push_is_forwarded)
{
	Probe api;
	FakeTraderSink sink;
	api.registerSpi(&sink);

	CThostFtdcInstrumentStatusField st;
	memset(&st, 0, sizeof(st));
	strncpy(st.ExchangeID, "SHFE", sizeof(st.ExchangeID) - 1);
	strncpy(st.InstrumentID, "ru2601", sizeof(st.InstrumentID) - 1);
	st.InstrumentStatus = THOST_FTDC_IS_Closed;

	api.OnRtnInstrumentStatus(&st);
	EXPECT_STREQ("SHFE", sink.statusExchg.c_str());
	EXPECT_STREQ("ru2601", sink.statusCode.c_str());
	EXPECT_EQ((WTSTradeStatus)THOST_FTDC_IS_Closed, sink.status);

	// 无 sink 时不得崩（:1324 有 if(m_sink)）
	api.registerSpi(NULL);
	api.OnRtnInstrumentStatus(&st);
}

//////////////////////////////////////////////////////////////////////////
// 厂商结构 → WTS 结构
//////////////////////////////////////////////////////////////////////////
TEST(ctpmini, make_order_info_maps_fields_and_night_order_date)
{
	Probe api;
	FakeTraderSink sink;
	CommFixture fx;
	sink.mgr = &fx.mgr;
	api.registerSpi(&sink);
	api.setBdMgr(&fx.mgr);
	api.setTradeDate(20260105);		// 周一
	api.setSession(3, 4, 0);

	CThostFtdcOrderField o;
	memset(&o, 0, sizeof(o));
	strncpy(o.InstrumentID, "ru2601", sizeof(o.InstrumentID) - 1);
	strncpy(o.ExchangeID, "SHFE", sizeof(o.ExchangeID) - 1);
	strncpy(o.OrderSysID, "123456", sizeof(o.OrderSysID) - 1);
	strncpy(o.OrderRef, "88", sizeof(o.OrderRef) - 1);
	strncpy(o.InsertTime, "22:30:00", sizeof(o.InsertTime) - 1);
	strncpy(o.StatusMsg, "全部成交", sizeof(o.StatusMsg) - 1);
	o.LimitPrice = 15000.0;
	o.VolumeTotalOriginal = 10;
	o.VolumeTraded = 4;
	o.Direction = THOST_FTDC_D_Sell;
	o.CombOffsetFlag[0] = THOST_FTDC_OF_CloseToday;
	o.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
	o.TimeCondition = THOST_FTDC_TC_IOC;
	o.VolumeCondition = THOST_FTDC_VC_CV;
	o.OrderStatus = THOST_FTDC_OST_PartTradedQueueing;
	o.OrderSubmitStatus = THOST_FTDC_OSS_InsertSubmitted;
	o.FrontID = 3;
	o.SessionID = 4;

	WTSOrderInfo* p = api.makeOrderInfo(&o);
	ASSERT_NE((WTSOrderInfo*)NULL, p);
	EXPECT_EQ(20260102u, p->getOrderDate());	// 夜盘 + 周一 → 上个周五
	EXPECT_EQ((uint64_t)TimeUtils::makeTime(20260102, 223000000), p->getOrderTime());
	EXPECT_DOUBLE_EQ(15000.0, p->getPrice());
	EXPECT_DOUBLE_EQ(10.0, p->getVolume());
	EXPECT_DOUBLE_EQ(6.0, p->getVolLeft());		// 10-4
	EXPECT_DOUBLE_EQ(4.0, p->getVolTraded());
	EXPECT_EQ(WDT_LONG, p->getDirection());		// 卖平 → 多头方向
	EXPECT_EQ(WOT_CLOSETODAY, p->getOffsetType());
	EXPECT_EQ(WPT_LIMITPRICE, p->getPriceType());
	EXPECT_EQ(WOF_FOK, p->getOrderFlag());		// IOC + CV → FOK
	EXPECT_EQ(WOS_PartTraded_Queuing, p->getOrderState());
	EXPECT_STREQ("ru2601", p->getCode());
	EXPECT_STREQ("SHFE", p->getExchg());
	EXPECT_STREQ("123456", p->getOrderID());
	EXPECT_STREQ("000003#0000000004#000088", p->getEntrustID());
	// eid 缓存未命中 → userTag 回落成 entrustID（:1151）
	EXPECT_STREQ(p->getEntrustID(), p->getUserTag());
	p->release();

	// IOC + AV/MV → FAK
	o.VolumeCondition = THOST_FTDC_VC_AV;
	o.TimeCondition = THOST_FTDC_TC_GFD;
	WTSOrderInfo* p2 = api.makeOrderInfo(&o);
	ASSERT_NE((WTSOrderInfo*)NULL, p2);
	EXPECT_EQ(WOF_NOR, p2->getOrderFlag());		// GFD → 普通
	p2->release();

	o.TimeCondition = THOST_FTDC_TC_IOC;
	WTSOrderInfo* p3 = api.makeOrderInfo(&o);
	ASSERT_NE((WTSOrderInfo*)NULL, p3);
	EXPECT_EQ(WOF_FAK, p3->getOrderFlag());
	p3->release();
}

TEST(ctpmini, make_order_info_and_trade_return_null_for_unknown_contract)
{
	Probe api;
	FakeTraderSink sink;
	CommFixture fx;
	sink.mgr = &fx.mgr;
	api.registerSpi(&sink);
	api.setBdMgr(&fx.mgr);

	CThostFtdcOrderField o;
	memset(&o, 0, sizeof(o));
	strncpy(o.InstrumentID, "unknown_code", sizeof(o.InstrumentID) - 1);
	EXPECT_EQ((WTSOrderInfo*)NULL, api.makeOrderInfo(&o));

	CThostFtdcTradeField t;
	memset(&t, 0, sizeof(t));
	strncpy(t.InstrumentID, "unknown_code", sizeof(t.InstrumentID) - 1);
	EXPECT_EQ((WTSTradeInfo*)NULL, api.makeTradeRecord(&t));

	CThostFtdcInputOrderField e;
	memset(&e, 0, sizeof(e));
	strncpy(e.InstrumentID, "unknown_code", sizeof(e.InstrumentID) - 1);
	EXPECT_EQ((WTSEntrust*)NULL, api.makeEntrust(&e));

	// 推送路径拿到 NULL 结构也不能上抛
	api.OnRtnOrder(&o);
	api.OnRtnTrade(&t);
	EXPECT_EQ(0, sink.orderPush);
	EXPECT_EQ(0, sink.tradePush);
}

TEST(ctpmini, make_trade_record_computes_amount_and_night_trade_date)
{
	Probe api;
	FakeTraderSink sink;
	CommFixture fx("SHFE", CM_OpenCover, 10);
	sink.mgr = &fx.mgr;
	api.registerSpi(&sink);
	api.setBdMgr(&fx.mgr);
	api.setTradeDate(20260106);		// 周二

	CThostFtdcTradeField t;
	memset(&t, 0, sizeof(t));
	strncpy(t.InstrumentID, "ru2601", sizeof(t.InstrumentID) - 1);
	strncpy(t.ExchangeID, "SHFE", sizeof(t.ExchangeID) - 1);
	strncpy(t.TradeID, "T1", sizeof(t.TradeID) - 1);
	strncpy(t.OrderSysID, "999", sizeof(t.OrderSysID) - 1);
	strncpy(t.TradeTime, "14:05:30", sizeof(t.TradeTime) - 1);
	t.Volume = 3;
	t.Price = 15000.0;
	t.Direction = THOST_FTDC_D_Buy;
	t.OffsetFlag = THOST_FTDC_OF_Open;

	WTSTradeInfo* p = api.makeTradeRecord(&t);
	ASSERT_NE((WTSTradeInfo*)NULL, p);
	EXPECT_EQ(WDT_LONG, p->getDirection());
	EXPECT_EQ(WOT_OPEN, p->getOffsetType());
	EXPECT_DOUBLE_EQ(3.0, p->getVolume());
	EXPECT_DOUBLE_EQ(15000.0, p->getPrice());
	EXPECT_DOUBLE_EQ(10 * 3 * 15000.0, p->getAmount());	// volscale*vol*price
	EXPECT_EQ(20260106u, p->getTradeDate());
	p->release();

	// 夜盘成交：周二交易日 → 用前一天
	strncpy(t.TradeTime, "21:00:00", sizeof(t.TradeTime) - 1);
	WTSTradeInfo* p2 = api.makeTradeRecord(&t);
	ASSERT_NE((WTSTradeInfo*)NULL, p2);
	EXPECT_EQ(20260105u, p2->getTradeDate());
	p2->release();
}

TEST(ctpmini, rtn_trade_and_order_reach_the_sink)
{
	Probe api;
	FakeTraderSink sink;
	CommFixture fx("SHFE", CM_OpenCover, 10);
	sink.mgr = &fx.mgr;
	api.registerSpi(&sink);
	api.setBdMgr(&fx.mgr);
	api.setTradeDate(20260105);

	CThostFtdcTradeField t;
	memset(&t, 0, sizeof(t));
	strncpy(t.InstrumentID, "ru2601", sizeof(t.InstrumentID) - 1);
	strncpy(t.ExchangeID, "SHFE", sizeof(t.ExchangeID) - 1);
	strncpy(t.TradeTime, "09:30:00", sizeof(t.TradeTime) - 1);
	t.Volume = 2;
	t.Price = 100.0;
	api.OnRtnTrade(&t);
	EXPECT_EQ(1, sink.tradePush);
	EXPECT_DOUBLE_EQ(2000.0, sink.tradeAmount);
	EXPECT_EQ(20260105u, sink.tradeDate);

	CThostFtdcOrderField o;
	memset(&o, 0, sizeof(o));
	strncpy(o.InstrumentID, "ru2601", sizeof(o.InstrumentID) - 1);
	strncpy(o.InsertTime, "09:31:00", sizeof(o.InsertTime) - 1);
	o.LimitPrice = 100.0;
	o.VolumeTotalOriginal = 5;
	o.VolumeTraded = 1;
	o.TimeCondition = THOST_FTDC_TC_GFD;
	o.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
	api.OnRtnOrder(&o);
	EXPECT_EQ(1, sink.orderPush);
	EXPECT_DOUBLE_EQ(4.0, sink.volLeft);
	EXPECT_EQ(WOS_NotTraded_Queuing, sink.orderState);
}

//////////////////////////////////////////////////////////////////////////
// 查询回报：资金 / 持仓 / 订单 / 成交
//////////////////////////////////////////////////////////////////////////
TEST(ctpmini, rsp_qry_trading_account_builds_balance_and_clears)
{
	Probe api;
	FakeTraderSink sink;
	CommFixture fx;
	sink.mgr = &fx.mgr;
	api.registerSpi(&sink);

	CThostFtdcTradingAccountField a;
	memset(&a, 0, sizeof(a));
	a.PreBalance = 100000.0;
	a.CloseProfit = 500.0;
	a.PositionProfit = 300.0;
	a.CurrMargin = 20000.0;
	a.Available = 80000.0;
	a.Commission = 50.0;
	a.FrozenMargin = 10.0;
	a.FrozenCommission = 1.0;
	a.Deposit = 1000.0;
	a.Withdraw = 200.0;
	CThostFtdcRspInfoField rsp = okRsp();

	// 非末笔：只累积，不上抛
	api.OnRspQryTradingAccount(&a, &rsp, 1, false);
	EXPECT_EQ(0, sink.accountRsp);

	api.OnRspQryTradingAccount(&a, &rsp, 2, true);
	EXPECT_EQ(1, sink.accountRsp);
	// 上抛时数组里应有 2 笔（第 1 次累积 + 第 2 次）
	EXPECT_EQ(2u, sink.lastAccountCnt);

	// 错误回报：不追加记录。首笔必须成功创建容器，否则 :718 会对 NULL 的
	// m_ayFunds 调 clear() → 崩溃（该分支见 DISABLED 用例）
	Probe api2;
	FakeTraderSink sink2;
	api2.registerSpi(&sink2);
	api2.OnRspQryTradingAccount(&a, &rsp, 3, false);	// 建容器 + 1 笔
	CThostFtdcRspInfoField bad = errRsp(4, "no data");
	api2.OnRspQryTradingAccount(&a, &bad, 4, true);		// 错误笔不入表
	EXPECT_EQ(1, sink2.accountRsp);
	EXPECT_EQ(1u, sink2.lastAccountCnt);
}

TEST(ctpmini, rsp_qry_position_cover_one_computes_available)
{
	Probe api;
	FakeTraderSink sink;
	CommFixture fx("SHFE", CM_OpenCover, 10);
	sink.mgr = &fx.mgr;
	api.registerSpi(&sink);
	api.setBdMgr(&fx.mgr);

	CThostFtdcInvestorPositionField p;
	memset(&p, 0, sizeof(p));
	strncpy(p.InstrumentID, "ru2601", sizeof(p.InstrumentID) - 1);
	p.PosiDirection = THOST_FTDC_PD_Long;
	p.PositionDate = THOST_FTDC_PSD_History;
	p.Position = 10;
	p.TodayPosition = 4;
	p.UseMargin = 5000.0;
	p.PositionProfit = 200.0;
	p.PositionCost = 80000.0;
	p.LongFrozen = 3;
	p.ShortFrozen = 0;
	CThostFtdcRspInfoField rsp = okRsp();

	api.OnRspQryInvestorPosition(&p, &rsp, 1, true);

	EXPECT_EQ(1u, sink.lastPosCnt);
	EXPECT_EQ(WDT_LONG, sink.posDir);
	// CoverOne：new=TodayPosition=4，pre=10-4=6
	EXPECT_DOUBLE_EQ(4.0, sink.posNew);
	EXPECT_DOUBLE_EQ(6.0, sink.posPre);
	EXPECT_DOUBLE_EQ(14.0, sink.posTotal);
	// availNew = 4 - ShortFrozen(0) = 4
	EXPECT_DOUBLE_EQ(4.0, sink.posAvailNew);
	// availPre = new+pre - longFrozen - shortFrozen - availNew = 14-3-0-4 = 7
	EXPECT_DOUBLE_EQ(7.0, sink.posAvailPre);
	EXPECT_DOUBLE_EQ(5000.0, sink.posMargin);
	// avg = cost / total / volscale = 80000/14/10
	EXPECT_DOUBLE_EQ(80000.0 / 14.0 / 10.0, sink.posAvg);
	EXPECT_STREQ("ru2601", sink.code.c_str());

	// 空头 + 今日仓：走 ShortFrozen 分支
	Probe api2;
	FakeTraderSink sink2;
	api2.registerSpi(&sink2);
	CommFixture fx2("SHFE", CM_CoverToday, 10);
	api2.setBdMgr(&fx2.mgr);
	p.PosiDirection = THOST_FTDC_PD_Short;
	p.PositionDate = THOST_FTDC_PSD_Today;
	p.Position = 5;
	p.TodayPosition = 5;
	p.ShortFrozen = 2;
	api2.OnRspQryInvestorPosition(&p, &rsp, 2, true);
	// CoverToday：PositionDate=Today → new=Position=5，pre=0
	EXPECT_DOUBLE_EQ(5.0, sink2.posNew);
	EXPECT_DOUBLE_EQ(0.0, sink2.posPre);
	// availNew = 5 - LongFrozen(3) = 2 → 不为 0，说明用的是 LongFrozen（空头走 else 分支）
	EXPECT_DOUBLE_EQ(2.0, sink2.posAvailNew);

	// 未知合约：不入表，末笔仍要回调空数组
	Probe api3;
	FakeTraderSink sink3;
	api3.registerSpi(&sink3);
	api3.setBdMgr(&fx.mgr);
	strncpy(p.InstrumentID, "unknown", sizeof(p.InstrumentID) - 1);
	api3.OnRspQryInvestorPosition(&p, &rsp, 3, true);
	EXPECT_EQ(0u, sink3.lastPosCnt);
}

TEST(ctpmini, rsp_qry_orders_and_trades_forward_counts)
{
	Probe api;
	FakeTraderSink sink;
	CommFixture fx;
	sink.mgr = &fx.mgr;
	api.registerSpi(&sink);
	api.setBdMgr(&fx.mgr);
	api.setTradeDate(20260105);

	CThostFtdcOrderField o;
	memset(&o, 0, sizeof(o));
	strncpy(o.InstrumentID, "ru2601", sizeof(o.InstrumentID) - 1);
	strncpy(o.InsertTime, "10:00:00", sizeof(o.InsertTime) - 1);
	o.TimeCondition = THOST_FTDC_TC_GFD;
	CThostFtdcTradeField t;
	memset(&t, 0, sizeof(t));
	strncpy(t.InstrumentID, "ru2601", sizeof(t.InstrumentID) - 1);
	strncpy(t.TradeTime, "10:00:01", sizeof(t.TradeTime) - 1);
	CThostFtdcRspInfoField rsp = okRsp();

	api.OnRspQryOrder(&o, &rsp, 1, false);
	api.OnRspQryOrder(&o, &rsp, 2, true);
	EXPECT_EQ(2u, sink.lastOrderCntRsp);

	Probe api2;
	FakeTraderSink sink2;
	api2.registerSpi(&sink2);
	api2.setBdMgr(&fx.mgr);
	api2.OnRspQryTrade(&t, &rsp, 1, true);
	EXPECT_EQ(1u, sink2.lastTradeCnt);

	// 错误回报：不追加记录
	Probe api3;
	FakeTraderSink sink3;
	api3.registerSpi(&sink3);
	CThostFtdcRspInfoField bad = errRsp(7, "e");
	api3.OnRspQryOrder(NULL, &bad, 1, false);
	api3.OnRspQryTrade(NULL, &bad, 1, false);
	EXPECT_EQ(0u, sink3.lastOrderCntRsp);
}

TEST(ctpmini, release_is_safe_before_any_api_was_created)
{
	Probe api;
	// m_pUserAPI/各数组均为 NULL/未创建，release 只走守卫（:139-162）
	EXPECT_NO_THROW(api.release());
	api.setState(TraderCTPMini::WS_ALLREADY);
	EXPECT_NO_THROW(api.release());
}

//////////////////////////////////////////////////////////////////////////
// 崩溃/起线程路径：整条 DISABLED
//////////////////////////////////////////////////////////////////////////
TEST(ctpmini, DISABLED_init_and_connect_need_vendor_dylib)
{
	GTEST_SKIP() << "init():132 get_symbol 失败后 m_funcCreator 为空；connect():169 直接调用 → SIGSEGV，"
					"且 connect() 会起 StdThread 查询线程（本 lane 禁止起线程）";
}

TEST(ctpmini, DISABLED_disconnect_joins_worker_thread)
{
	GTEST_SKIP() << "disconnect():229 需要 m_thrdWorker->join()，未起线程时无害但起线程后会挂；留待有 shim 再说";
}

TEST(ctpmini, DISABLED_order_insert_positive_path_needs_api)
{
	GTEST_SKIP() << "orderInsert 正向分支要求 m_pUserAPI 非空 + eid 缓存已 init（否则 put 解引用空 block）";
}

TEST(ctpmini, DISABLED_login_and_rsp_user_login_needs_api)
{
	GTEST_SKIP() << "login() 非空指针分支 → authenticate():1342 解引用 m_pUserAPI；"
					"OnRspUserLogin:600 同样取 GetTradingDay";
}

TEST(ctpmini, DISABLED_qry_account_last_without_record_crashes)
{
	GTEST_SKIP() << "OnRspQryTradingAccount:718 在没有任何记录时 m_ayFunds 仍为 NULL 却调用 clear() → 空指针解引用（P0 bug）";
}
