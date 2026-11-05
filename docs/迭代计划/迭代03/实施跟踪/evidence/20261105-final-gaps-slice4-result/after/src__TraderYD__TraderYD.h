/*!
 * \file TraderYD.h
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
#include <condition_variable>
#include <mutex>
#include <unordered_map>

#include "../Includes/WTSTypes.h"
#include "../Includes/ITraderApi.h"
#include "../Includes/WTSCollection.hpp"

#include <API/yd1.108.360/ydApi.h>

#include "../Share/IniHelper.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/DLLHelper.hpp"
#include "../Share/WtKVCache.hpp"

USING_NS_WTP;

class TraderYD : public ITraderApi, public YDListener
{
public:
	// BUG-L01-014: host features declared through setTraderSpiFeatures.
	void setSpiFeatures(uint64_t features) { m_spiFeatures.store(features); }
	std::atomic<uint64_t>	m_spiFeatures{ 0 };
	// K42 / BUG-I02-NEW-154: set when an order report of this session could not be
	// converted, i.e. the local order cache is missing an order.
	std::atomic<bool>		m_bOrderCacheDropped{ false };

	TraderYD();
	virtual ~TraderYD();

public:
	virtual void notifyReadyForLogin(bool hasLoginFailed) override;


	virtual void notifyLogin(int errorNo, int maxOrderRef, bool isMonitor) override;


	virtual void notifyFinishInit() override;


	virtual void notifyOrder(const YDOrder *pOrder, const YDInstrument *pInstrument, const YDAccount *pAccount) override;


	virtual void notifyTrade(const YDTrade *pTrade, const YDInstrument *pInstrument, const YDAccount *pAccount) override;


	virtual void notifyFailedOrder(const YDInputOrder *pFailedOrder, const YDInstrument *pInstrument, const YDAccount *pAccount) override;


	virtual void notifyFailedCancelOrder(const YDFailedCancelOrder *pFailedCancelOrder, const YDExchange *pExchange, const YDAccount *pAccount) override;


	virtual void notifyAccount(const YDAccount *pAccount) override;

	virtual void notifyCaughtUp() override;

	// YD 1.108 (ydApi.h): startDestroy() is asynchronous; the listener may only be
	// deleted after notifyAfterApiDestroy(). These two are never gated.
	virtual void notifyBeforeApiDestroy() override;

	virtual void notifyAfterApiDestroy() override;

public:
	/*
	 *	Card 9 (9a=A+C, 9b=A, 9c=A, 9d=A) lifecycle state, observable by the host.
	 */
	typedef enum
	{
		YS_RUNNING,			// no stop requested
		YS_STOP_DEFERRED,	// stop requested from a callback/worker; admission closed
		YS_RELEASED,		// worker joined, calls/callbacks drained, notifyAfterApiDestroy received
		YS_DRAIN_UNCERTAIN	// bound expired; the instance must stay alive (quarantine)
	} StopState;

	StopState	stopState() const;
	bool		isCallbackContext() const;
	bool		isQuiesced() const;
	void		detachForQuarantine();

private:
	class ApiLease;
	class CallbackScope;
	friend class ApiLease;
	friend class CallbackScope;
	bool		acquireApiLease();
	void		releaseApiLease();
	bool		enterCallback(bool gated);
	void		leaveCallback();
	void		requestStop();
	void		workerMain();

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

	virtual int orderInsert(WTSEntrust* eutrust) override;

	virtual int orderAction(WTSEntrustAction* action) override;

	virtual int queryAccount() override;

	virtual int queryPositions() override;

	virtual int queryOrders() override;

	virtual int queryTrades() override;


private:
	 inline WTSOrderInfo*	makeOrderInfo(const YDOrder* orderField, const YDInstrument* instInfo) noexcept;
	 inline WTSEntrust*		makeEntrust(const YDInputOrder *entrustField, const YDInstrument* instInfo) noexcept;
	 inline WTSTradeInfo*	makeTradeRecord(const YDTrade *tradeField, const YDInstrument* instInfo) noexcept;
	 inline WTSError*		makeError(int errorno, WTSErroCode ec) noexcept;

	 inline bool		generateEntrustID(uint32_t orderRef, char* buffer) noexcept;
	 inline bool		extractEntrustID(const char* entrustid, uint32_t &orderRef) noexcept;

	inline uint32_t		genRequestID() noexcept
	{
		return m_iRequestID.fetch_add(1) + 1;
	}

	void on_kvcache_message(const char* message);

protected:
	std::string		m_strCfgFile;

	std::string		m_strUser;
	std::string		m_strPass;

	bool			m_bCatchup;
	bool			m_bApiInited;

	/*
	 *	By Wesley @ 2023.12.27
	 *	是否更新本地缓存，默认为false
	 *	如果开启则会在运行中自动更新本地的成交、订单和持仓缓存
	 */
	bool			m_bUpdateCache;

	std::string		m_strUserName;

	ITraderSpi*		m_sink;
	uint64_t		m_uLastQryTime;

	uint32_t					m_lDate;
	std::atomic<uint32_t>		m_orderRef;		//报单引用

	WrapperState				m_wrapperState;

	YDApi*						m_pUserAPI;
	std::atomic<uint32_t>		m_iRequestID;

	typedef WTSHashMap<std::string> DataMap;
	DataMap*					m_mapPosition;
	DataMap*					m_mapTrades;
	DataMap*					m_mapOrders;
	WTSArray*					m_ayFunds;

	// BUG-R93-006: yesterday/today quantities frozen by each close order when it
	// was first seen, keyed by order id, so a cancel restores the order's own
	// sources instead of inferring them from the position-wide totals.
	struct FrozenSources
	{
		double	_pre;
		double	_new;
	};
	std::unordered_map<std::string, FrozenSources>	m_mapFrozen;

	IBaseDataMgr*				m_bdMgr;

	typedef std::queue<CommonExecuter>	QueryQue;
	QueryQue				m_queQuery;
	StdUniqueMutex			m_mtxQuery;
	std::atomic<bool>		m_bStopped;		// admission closed (release/disconnect requested)
	StdThreadPtr			m_thrdWorker;

	mutable std::mutex		m_mtxLifecycle;
	std::condition_variable	m_cvLifecycle;
	std::mutex				m_mtxRelease;		// serializes external release()
	uint32_t				m_apiLeases;		// host calls touching the YD API in flight
	uint32_t				m_activeCallbacks;	// YD callbacks inside this wrapper
	bool					m_workerExited;
	bool					m_destroyStarted;	// startDestroy() called
	bool					m_apiDestroyed;		// notifyAfterApiDestroy() received
	StopState				m_stopState;
	uint32_t				m_drainTimeoutMs;

	std::string		m_strModule;
	DllHandle		m_hInstYD;
	typedef YDApi* (*YDCreator)(const char *);
	YDCreator		m_funcCreator;

	//委托单标记缓存器
	WtKVCache		m_eidCache;
	//订单标记缓存器
	WtKVCache		m_oidCache;
	CacheLogger		m_cacheLogger;
};

