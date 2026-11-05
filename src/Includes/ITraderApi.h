/*!
 * \file ITraderApi.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 交易通道对接接口定义文件
 */
#pragma once
#include <set>
#include <map>
#include <stdint.h>
#include <functional>
#include "WTSTypes.h"

NS_WTP_BEGIN
class WTSVariant;
class WTSEntrust;
class WTSOrderInfo;
class WTSTradeInfo;
class WTSEntrustAction;
class WTSAccountInfo;
class WTSPositionItem;
class WTSContractInfo;
class WTSError;
class WTSTickData;
class WTSNotify;
class WTSArray;
class IBaseDataMgr;

typedef std::function<void()>	CommonExecuter;

#pragma region "Stock Trading API definations"
/*
 *	股票交易接口回调
 *	Added By Wesley @ 2020/05/06
 */
class IStkTraderSpi
{

};

/*
 *	股票交易接口
 *	Added By Wesley @ 2020/05/06
 *	预留起来以后使用,先把接口的相互框架搭建起来
 *	主要提供融资融券等股票特有接口
 */
class IStkTraderApi
{
};
#pragma endregion

#pragma region "Option Trading API definations"
/*
 *	期权交易接口回调
 *	Added By Wesley @ 2020/05/06
 */
class IOptTraderSpi
{
public:
	virtual void onRspEntrustOpt(WTSEntrust* /*entrust*/, WTSError */*err*/) {}
	virtual void onRspOrdersOpt(const WTSArray* /*ayOrders*/) {}
	virtual void onPushOrderOpt(WTSOrderInfo* /*orderInfo*/) {}
};

/*
 *	期权交易接口
 *	Added By Wesley @ 2020/05/06
 *	预留起来以后使用,先把接口的相互框架搭建起来
 *	主要提供报价、行权等期权特有接口
 */
class IOptTraderApi
{
public:
	/*
	 *	下单接口
	 *	entrust 下单的具体数据结构
	 */
	virtual int orderInsertOpt(WTSEntrust* /*eutrust*/) { return -1; }

	/*
	 *	订单操作接口
	 *	action	操作的具体数据结构
	 */
	virtual int orderActionOpt(WTSEntrustAction* /*action*/) { return -1; }

	/*
	 *	查询期权订单
	 */
	virtual int	queryOrdersOpt(WTSBusinessType /*bType*/) { return -1; }
};
#pragma endregion


//委托回调接口
/* Decision K42: order-query completeness and the host feature negotiation. */
#define WT_QUERY_STATUS_UNKNOWN		0u	/* legacy onRspOrders */
#define WT_QUERY_STATUS_COMPLETE	1u
#define WT_QUERY_STATUS_PARTIAL		2u
#define WT_QUERY_STATUS_FAILED		3u
#define WT_TRADER_SPI_FEATURE_QUERY_STATUS	(UINT64_C(1) << 0)
/* BUG-L01-014 (card 6): the host accepts onUnresolvedProviderReport. */
#define WT_TRADER_SPI_FEATURE_RAW_UNRESOLVED	(UINT64_C(1) << 1)
/* Optional provider export: void setTraderSpiFeatures(ITraderApi* api, uint64_t features).
 * The host calls it once after createTrader and before init; a provider that does not
 * export it, or a host that never calls it, keeps the legacy callbacks. */
#define WT_TRADER_SPI_FEATURES_SYMBOL	"setTraderSpiFeatures"

class ITraderSpi
{
public:
	/*
	 *	获取基础数据管理器
	 */
	virtual IBaseDataMgr*	getBaseDataMgr() = 0;

	/*
	 *	处理交易接口的日志
	 */
	virtual void handleTraderLog(WTSLogLevel /*ll*/, const char* /*message*/){}

	/*
	 *	获取股票交易接口Spi
	 */
	virtual IStkTraderSpi* getStkSpi(){ return NULL; }

	/*
	 *	获取期权交易接口Spi
	 */
	virtual IOptTraderSpi* getOptSpi(){ return NULL; }

public:
	/*
	 *	处理交易接口事件
	 */
	virtual void handleEvent(WTSTraderEvent e, int32_t ec) = 0;

	/*
	 *	登录回报
	 */
	virtual void onLoginResult(bool bSucc, const char* msg, uint32_t tradingdate) = 0;

	/*
	 *	注销回报
	 */
	virtual void onLogout(){}

	/*
	 *	委托回报
	 */
	virtual void onRspEntrust(WTSEntrust* /*entrust*/, WTSError */*err*/){}

	/*
	 * 资金查询回报
	 */
	virtual void onRspAccount(WTSArray* /*ayAccounts*/) {}

	/*
	 *	持仓查询回报
	 */
	virtual void onRspPosition(const WTSArray* /*ayPositions*/){}

	/*
	 *	订单查询回报
	 */
	virtual void onRspOrders(const WTSArray* /*ayOrders*/){}

	/*
	 *	成交查询回报
	 */
	virtual void onRspTrades(const WTSArray* /*ayTrades*/){}

	/*
	 *	结算单查询回报
	 */
	virtual void onRspSettlementInfo(uint32_t /*uDate*/, const char* /*content*/){}

	/*
	 *	订单回报推送
	 */
	virtual void onPushOrder(WTSOrderInfo* /*orderInfo*/){}

	/*
	 *	成交回报推送
	 */
	virtual void onPushTrade(WTSTradeInfo* /*tradeRecord*/){}

	/*
	 *	交易接口错误回报
	 */
	virtual void onTraderError(WTSError* /*err*/, void* /*pData*/ = NULL){}

	/*
	 *	合约状态推送
	 */
	virtual void onPushInstrumentStatus(const char* /*exchg*/, const char* /*code*/, WTSTradeStatus /*state*/) {}

	/*
	 *	Decision K42 (2026-11-04, BUG-I02-NEW-154): versioned order-query result, appended
	 *	to the vtable (existing slots unchanged). status is WT_QUERY_STATUS_COMPLETE (every
	 *	order of the account is in ayOrders, possibly none), _PARTIAL (some rows only;
	 *	nothing may be concluded from a missing order) or _FAILED (ayOrders may be NULL).
	 *	A provider may call it only after the host declared WT_TRADER_SPI_FEATURE_QUERY_STATUS
	 *	through the provider's optional WT_TRADER_SPI_FEATURES_SYMBOL export; otherwise it
	 *	keeps calling onRspOrders, which hosts treat conservatively. The default keeps a
	 *	complete result on the legacy path and ignores the others.
	 */
	virtual void onRspOrdersWithStatus(const WTSArray* ayOrders, uint32_t status)
	{
		if (status == WT_QUERY_STATUS_COMPLETE)
			onRspOrders(ayOrders);
	}

	/*
	 *	BUG-L01-014 (card 6, appended): a report the provider received but could not turn
	 *	into a WTS object (unknown or unparseable contract, malformed fields). kind names
	 *	the report ("order", "trade", "position", ...), reason why it was not converted, raw
	 *	the provider's own text of every SDK field it has (e.g. JSON). The host keeps it for
	 *	reconciliation and pauses the account; it is never applied automatically. Call it
	 *	only after the host declared WT_TRADER_SPI_FEATURE_RAW_UNRESOLVED.
	 */
	virtual void onUnresolvedProviderReport(const char* /*kind*/, const char* /*reason*/, const char* /*raw*/) {}
};

//下单接口管理接口
class ITraderApi
{
public:
	virtual ~ITraderApi(){}

	virtual IStkTraderApi* getStkTrader() { return NULL; }
	virtual IOptTraderApi* getOptTrader() { return NULL; }

public:
	/*
	 *	初始化解析管理器
	 */
	virtual bool init(WTSVariant */*params*/) { return false; }

	/*
	 *	释放解析管理器
	 */
	virtual void release(){}

	/*
	 *	注册回调接口
	 */
	virtual void registerSpi(ITraderSpi */*listener*/) {}


	//////////////////////////////////////////////////////////////////////////
	//业务逻辑接口

	/*
	 *	连接服务器
	 */
	virtual void connect() {}

	/*
	 *	断开连接
	 */
	virtual void disconnect() {}

	/*
	 *	是否完全就绪（K55，BUG-R60-006，2026-11-06）
	 *	只有登录成功且交易通道可用（可以下单）时返回 true；
	 *	初始、仅连上通道未登录、登录失败、logout、断开、release 之后都返回 false。
	 *	内置宿主不依赖本接口判断就绪，以 onLoginResult 为准；本合同供第三方调用方使用。
	 */
	virtual bool isConnected() { return false; }

	/*
	 *	生成委托单号
	 */
	virtual bool makeEntrustID(char* /*buffer*/, int /*length*/){ return false; }

	/*
	 *	登录接口
	 */
	virtual int login(const char* /*user*/, const char* /*pass*/, const char* /*productInfo*/) { return -1; }

	/*
	 *	注销接口
	 */
	virtual int logout() { return -1; }

	/*
	 *	下单接口
	 *	entrust 下单的具体数据结构
	 */
	virtual int orderInsert(WTSEntrust* /*eutrust*/) { return -1; }

	/*
	 *	订单操作接口
	 *	action	操作的具体数据结构
	 */
	virtual int orderAction(WTSEntrustAction* /*action*/) { return -1; }

	/*
	 *	查询账户信息
	 */
	virtual int queryAccount() { return -1; }

	/*
	 *	查询持仓信息
	 */
	virtual int queryPositions() { return -1; }

	/*
	 *	查询所有订单
	 */
	virtual int queryOrders() { return -1; }

	/*
	 *	查询成交明细
	 */
	virtual int	queryTrades() { return -1; }

	/*
	 *	查询结算单
	 */
	virtual int querySettlement(uint32_t /*uDate*/){ return 0; }

};

NS_WTP_END

//获取IDataMgr的函数指针类型
typedef wtp::ITraderApi* (*FuncCreateTrader)();
typedef void(*FuncDeleteTrader)(wtp::ITraderApi* &trader);
typedef void(*FuncSetTraderSpiFeatures)(wtp::ITraderApi* trader, uint64_t features);
