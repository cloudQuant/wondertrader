/*!
 * \file TraderCTPOpt.h
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
#include <unordered_set>

#include "../Includes/WTSTypes.h"
#include "../Includes/ITraderApi.h"
#include "../Includes/WTSCollection.hpp"

#include <API/CTPOpt3.5.8/ThostFtdcTraderApi.h>

#include "../Share/IniHelper.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/DLLHelper.hpp"
#include "../TraderCTP/VendorTdLifecycle.hpp"

USING_NS_WTP;

class TraderCTPOpt : public ITraderApi, public IOptTraderApi, public CThostFtdcTraderSpi
{
public:
	TraderCTPOpt();
	virtual ~TraderCTPOpt();

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

	int authenticate();

	int doLogin();

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

	virtual int orderInsert(WTSEntrust* entrust) override;

	virtual int orderAction(WTSEntrustAction* action) override;

	virtual int queryAccount() override;

	virtual int queryPositions() override;

	virtual int queryOrders() override;

	virtual int queryTrades() override;

	virtual int querySettlement(uint32_t uDate) override;


	//////////////////////////////////////////////////////////////////////////
	//IOptTraderApi
	virtual int orderInsertOpt(WTSEntrust* entrust) override;

	virtual int orderActionOpt(WTSEntrustAction* action) override;

	virtual int	queryOrdersOpt(WTSBusinessType bType) override;


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
	///执行宣告录入请求响应
	virtual void OnRspExecOrderInsert(CThostFtdcInputExecOrderField *pInputExecOrder, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	///执行宣告操作请求响应
	virtual void OnRspExecOrderAction(CThostFtdcInputExecOrderActionField *pInputExecOrderAction, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	///执行宣告通知
	virtual void OnRtnExecOrder(CThostFtdcExecOrderField *pExecOrder) override;

	///执行宣告录入错误回报
	virtual void OnErrRtnExecOrderInsert(CThostFtdcInputExecOrderField *pInputExecOrder, CThostFtdcRspInfoField *pRspInfo) override;

	virtual void OnRspQryExecOrder(CThostFtdcExecOrderField *pExecOrder, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	//////////////////////////////////////////////////////////////////////////
	//卡9 生命周期（T1-CTPOpt-01/02）：void release()/disconnect() 的可观察结果
public:
	wt_vendor_td::StopState lifecycleState() const { return m_lifecycle.stopState(); }
	// 仅当本实例从未创建 SDK 对象、当前线程不在本实例回调/调用/worker 内且无在途准入时，
	// 工厂 remover 才可删除；否则须隔离到进程退出（卡9a/9b）。
	bool removableByFactory() const;
	bool onProviderThread() const { return wt_vendor_td::Lifecycle::reentered(this); }

private:
	void			shutdown(const char* reason);
	ITraderSpi*		currentSink() const { return m_bscSink.load(std::memory_order_acquire); }
	IOptTraderSpi*	currentOptSink() const { return m_optSink.load(std::memory_order_acquire); }
	// T2-CTP-08 / 卡6d：为报单分配或保留唯一 OrderRef，并建立 tag 关联。
	bool			assignOrderIdentity(WTSEntrust* entrust, uint32_t& orderRef, std::string& tag);

protected:
	/*
	*	检查错误信息
	*/
	bool IsErrorRspInfo(CThostFtdcRspInfoField *pRspInfo);


	WTSOrderInfo*	makeOrderInfo(CThostFtdcOrderField* orderField);
	WTSOrderInfo*	makeOrderInfo(CThostFtdcExecOrderField* orderField);
	WTSEntrust*		makeEntrust(CThostFtdcInputOrderField *entrustField);
	WTSError*		makeError(CThostFtdcRspInfoField* rspInfo);
	WTSTradeInfo*	makeTradeRecord(CThostFtdcTradeField *tradeField);
	WTSEntrust*		makeEntrust(CThostFtdcInputExecOrderField *entrustField);

	void			generateEntrustID(char* buffer, uint32_t frontid, uint32_t sessionid, uint32_t orderRef);
	bool			extractEntrustID(const char* entrustid, uint32_t &frontid, uint32_t &sessionid, uint32_t &orderRef);

	//uint64_t		calcCommission(uint32_t qty, uint32_t price, WTSOffsetType flag, WTSContractInfo* ct);
	//uint64_t		calcMargin(uint32_t qty, uint32_t price, WTSDirectionType direct, WTSContractInfo* ct);

	uint32_t		genRequestID();

	// 丢弃当前未完成的持仓聚合，不发布部分快照。
	void			discardPositionBatch();

	//void			triggerQuery();

protected:
	std::string		m_strBroker;
	std::string		m_strFront;

	std::string		m_strUser;
	std::string		m_strPass;

	std::string		m_strAppID;
	std::string		m_strAuthCode;
	std::string		m_strFlowDir;

	std::string		m_strProdInfo;

	bool			m_bQuickStart;

	std::string		m_strTag;

	std::string		m_strUserName;

	std::string		m_strSettleInfo;

	std::atomic<ITraderSpi*>	m_bscSink;
	std::atomic<IOptTraderSpi*>	m_optSink;
	uint64_t		m_uLastQryTime;

	uint32_t					m_lDate;
	TThostFtdcFrontIDType		m_frontID;		//前置编号
	TThostFtdcSessionIDType		m_sessionID;	//会话编号
	std::atomic<uint32_t>		m_orderRef;		//报单引用

	std::atomic<WrapperState>	m_wrapperState;

	CThostFtdcTraderApi*		m_pUserAPI;
	std::atomic<uint32_t>		m_iRequestID;

	typedef WTSHashMap<std::string> PositionMap;
	PositionMap*				m_mapPosition;
	bool					m_bPositionQueryIncomplete;
	// BUG-R65-002 / 迭代03 卡14：持仓查询在途请求号，0 表示无在途请求。
	// SDK 同步返回 0 只表示"已发送"；只有该请求号的末帧才是"已确认"。
	std::atomic<int>			m_iPosQryReqID;
	// 当前聚合持仓所属请求号，仅在 SPI 回调线程读写。
	int						m_iPosAggReqID;
	WTSArray*					m_ayTrades;
	WTSArray*					m_ayOrders;
	WTSArray*					m_ayExecOrds;
	WTSArray*					m_ayPosDetail;

	IBaseDataMgr*				m_bdMgr;

	typedef std::queue<CommonExecuter>	QueryQue;
	QueryQue				m_queQuery;
	std::atomic<bool>		m_bInQuery;
	StdUniqueMutex			m_mtxQuery;
	uint64_t				m_lastQryTime;

	std::atomic<bool>		m_bStopped;
	StdThreadPtr			m_thrdWorker;

	std::string		m_strModule;
	DllHandle		m_hInstCTP;
	typedef CThostFtdcTraderApi* (*CTPCreator)(const char *);
	CTPCreator		m_funcCreator;

	IniHelper		m_iniHelper;

	//卡9：接口/回调准入、worker 标记与停止结果
	wt_vendor_td::Lifecycle	m_lifecycle;
	uint32_t				m_stopBoundMs = wt_vendor_td::DEFAULT_STOP_BOUND_MS;

	//T2-CTP-08：本会话已提交的 OrderRef（不回收、不重复），按前置/会话区分
	std::mutex					m_identityMutex;
	//entrusts/orders 标记缓存的读写互斥（下单线程与 SDK 回调线程并发访问）
	std::mutex					m_iniMutex;
	std::unordered_set<uint32_t>	m_submittedRefs;
	uint64_t					m_identitySession = 0;
};

