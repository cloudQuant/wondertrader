/*!
 * \file TraderCTP.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#pragma once

#include <string>
#include <queue>
#include <stdint.h>
#include <atomic>
#include <mutex>
#include <unordered_map>

#include "../Includes/WTSTypes.h"
#include "../Includes/ITraderApi.h"
#include "../Includes/WTSCollection.hpp"

#ifdef __APPLE__
#include <API/CTP6.6.9/ThostFtdcTraderApi.h>
#else
#include <API/CTP6.3.15/ThostFtdcTraderApi.h>
#endif

#include "../Share/StdUtils.hpp"
#include "../Share/DLLHelper.hpp"
#include "../Share/WtKVCache.hpp"
#include "VendorTdLifecycle.hpp"

USING_NS_WTP;

class TraderCTP : public ITraderApi, public CThostFtdcTraderSpi
{
public:
	TraderCTP();
	virtual ~TraderCTP();

public:
	typedef enum
	{
		WS_NOTLOGIN,		//未登录
		WS_LOGINING,		//正在登录
		WS_LOGINED,			//已登录
		WS_LOGINFAILED,		//登录失败
		WS_CONFIRM_QRYED,
		WS_CONFIRMED,		//已确认
		WS_ALLREADY			//全部就绪
	} WrapperState;


private:

	int confirm();

	int queryConfirm();

	int authenticate(uint64_t attempt);

	int doLogin(uint64_t attempt);

	//////////////////////////////////////////////////////////////////////////
	//ITraderApi接口
public:
	virtual bool init(WTSVariant* params) override;

	virtual void release() override;

	virtual void registerSpi(ITraderSpi *listener) override;

	virtual bool makeEntrustID(char* buffer, int length) override;

	virtual void connect() override;

	virtual void disconnect() override;

	virtual bool isConnected() override;

	virtual int login(const char* user, const char* pass, const char* productInfo) override;

	virtual int logout() override;

	virtual int orderInsert(WTSEntrust* eutrust) override;

	virtual int orderAction(WTSEntrustAction* action) override;

	virtual int queryAccount() override;

	virtual int queryPositions() override;

	virtual int queryOrders() override;

	virtual int queryTrades() override;

	virtual int querySettlement(uint32_t uDate) override;


	//////////////////////////////////////////////////////////////////////////
	//CTP交易接口实现
public:
	virtual void OnFrontConnected() override;

	virtual void OnFrontDisconnected(int nReason) override;

	virtual void OnHeartBeatWarning(int nTimeLapse) override;

	virtual void OnRspAuthenticate(CThostFtdcRspAuthenticateField *pRspAuthenticateField, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	virtual void OnRspUserLogin(CThostFtdcRspUserLoginField *pRspUserLogin, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	virtual void OnRspUserLogout(CThostFtdcUserLogoutField *pUserLogout, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	virtual void OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField *pSettlementInfoConfirm, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	virtual void OnRspQrySettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField *pSettlementInfoConfirm, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	virtual void OnRspQryTradingAccount(CThostFtdcTradingAccountField *pTradingAccount, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	virtual void OnRspOrderInsert(CThostFtdcInputOrderField *pInputOrder, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	virtual void OnRspOrderAction(CThostFtdcInputOrderActionField *pInputOrderAction, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	virtual void OnRspQryInvestorPosition(CThostFtdcInvestorPositionField *pInvestorPosition, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	virtual void OnRspQrySettlementInfo(CThostFtdcSettlementInfoField *pSettlementInfo, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	///请求查询成交响应
	virtual void OnRspQryTrade(CThostFtdcTradeField *pTrade, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	virtual void OnRspQryOrder(CThostFtdcOrderField *pOrder, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	virtual void OnRspError(CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	virtual void OnRtnOrder(CThostFtdcOrderField *pOrder) override;

	virtual void OnRtnTrade(CThostFtdcTradeField *pTrade) override;

	virtual void OnErrRtnOrderInsert(CThostFtdcInputOrderField *pInputOrder, CThostFtdcRspInfoField *pRspInfo) override;

	virtual void OnRtnInstrumentStatus(CThostFtdcInstrumentStatusField *pInstrumentStatus) override;

	//////////////////////////////////////////////////////////////////////////
	//卡9 生命周期（BUG-T25-005、T2-CTP-02/03）：void release()/disconnect() 的可观察结果
public:
	wt_vendor_td::StopState lifecycleState() const { return m_lifecycle.stopState(); }
	// 仅当本实例从未创建 SDK 对象、当前线程不在本实例回调/调用/worker 内、且停止已完成时，
	// 工厂 remover 才可删除；否则须隔离到进程退出（卡9a/9b）。
	bool removableByFactory() const;
	// 当前线程是否在本实例自己的回调、接口调用或查询 worker 内（卡9d 重入）。
	bool onProviderThread() const { return wt_vendor_td::Lifecycle::reentered(this); }

private:
	void			shutdown(const char* reason);
	void			emitConnectFailure(const char* reason);
	ITraderSpi*		currentSink() const { return m_sink.load(std::memory_order_acquire); }

private:
	WTSOrderInfo*	makeOrderInfo(CThostFtdcOrderField* orderField);
	WTSEntrust*		makeEntrust(CThostFtdcInputOrderField *entrustField);
	WTSEntrustAction*	makeAction(CThostFtdcInputOrderActionField *entrustField);
	WTSError*		makeError(CThostFtdcRspInfoField* rspInfo, WTSErroCode ec = WEC_NONE);
	WTSTradeInfo*	makeTradeInfo(CThostFtdcTradeField *tradeField);

	void			generateEntrustID(char* buffer, uint32_t frontid, uint32_t sessionid, uint32_t orderRef);
	bool			extractEntrustID(const char* entrustid, uint32_t &frontid, uint32_t &sessionid, uint32_t &orderRef);
	struct OrderIdentity
	{
		uint64_t epoch = 0;
		uint32_t front = 0;
		uint32_t session = 0;
		bool submitted = false;
		std::string canonical;
		std::string tag;
		std::string broker;
		std::string investor;
		uint32_t tradingDate = 0;
		std::string exchange;
		std::string instrument;
	};
	bool reserveIdentityLocked(uint32_t& orderRef, std::string& canonical, size_t capacity);

	uint32_t		genRequestID();

protected:
	std::string		m_strBroker;
	std::vector<std::string> m_strFront;

	std::string		m_strUser;
	std::string		m_strPass;

	std::string		m_strAppID;
	std::string		m_strAuthCode;

	std::string		m_strProdInfo;

	bool			m_bQuickStart;

	std::string		m_strTag;

	std::string		m_strSettleInfo;
	// I03-NEW-131: date of the latest cached settlement row, and the date of the
	// outstanding settlement request (0 when none). Both are reset when a query ends.
	uint32_t		m_uSettleDate = 0;
	uint32_t		m_uSettleQryDate = 0;

	std::string		m_strUserName;
	std::string		m_strFlowDir;

	std::atomic<ITraderSpi*>	m_sink;
	uint64_t		m_uLastQryTime;

	uint32_t					m_lDate;
	uint32_t					m_frontID;		//前置编号
	uint32_t					m_sessionID;	//会话编号
	std::atomic<uint32_t>		m_orderRef;		//报单引用
	mutable std::mutex			m_identityMutex;
	mutable std::mutex			m_cacheMutex;
	std::unordered_map<uint32_t, OrderIdentity> m_identities;
	std::unordered_map<std::string, std::string> m_orderSysTags;
	uint64_t					m_identityEpoch = 0;
	uint64_t				m_loginAttempt = 0;
	uint32_t					m_loginRequestID = 0;
	uint32_t					m_authRequestID = 0;
	uint32_t					m_confirmQueryRequestID = 0;
	uint32_t					m_confirmRequestID = 0;
	uint32_t					m_inflightInserts = 0;
	bool						m_identityReady = false;

	std::atomic<WrapperState>		m_wrapperState;

	CThostFtdcTraderApi*		m_pUserAPI;
	std::atomic<uint32_t>		m_iRequestID;

	typedef WTSHashMap<std::string> PositionMap;
	PositionMap*				m_mapPosition;
	WTSArray*					m_ayTrades;
	WTSArray*					m_ayOrders;
	// K42 / L01-014: host features declared through setTraderSpiFeatures, and the state of
	// the order query in progress (an error or a dropped row makes it not COMPLETE).
	std::atomic<uint64_t>		m_spiFeatures{ 0 };
	bool						m_qryOrdersFailed = false;
	bool						m_qryOrdersDropped = false;
public:
	void setSpiFeatures(uint64_t features) { m_spiFeatures.store(features, std::memory_order_release); }
	void reportUnresolved(const char* kind, const char* reason, const std::string& raw);
protected:
	WTSArray*					m_ayPosDetail;

	IBaseDataMgr*				m_bdMgr;

	typedef std::queue<CommonExecuter>	QueryQue;
	QueryQue				m_queQuery;
	std::atomic<bool>			m_bInQuery;
	StdUniqueMutex			m_mtxQuery;
	uint64_t				m_lastQryTime;

	std::atomic<bool>			m_bStopped;
	StdThreadPtr			m_thrdWorker;

	std::string		m_strModule;
	DllHandle		m_hInstCTP;
	typedef CThostFtdcTraderApi* (*CTPCreator)(const char *);
	CTPCreator		m_funcCreator;

	//委托单标记缓存器
	WtKVCache		m_eidCache;
	//订单标记缓存器
	WtKVCache		m_oidCache;

	//卡9：接口/回调准入、worker 标记与停止结果
	wt_vendor_td::Lifecycle	m_lifecycle;
	uint32_t				m_stopBoundMs = wt_vendor_td::DEFAULT_STOP_BOUND_MS;
};

