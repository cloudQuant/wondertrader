/*!
 * \file TraderXTP.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#pragma once

#include <stdint.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <unordered_set>
#include <vector>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/steady_timer.hpp>

#include <API/XTP2.2.32.2/xtp_trader_api.h>

#include "../Includes/ITraderApi.h"
#include "../Includes/WTSCollection.hpp"

#include "../Share/StdUtils.hpp"
#include "../Share/DLLHelper.hpp"
#include "../Share/WtKVCache.hpp"

USING_NS_WTP;

class TraderXTP : public XTP::API::TraderSpi, public ITraderApi
{
public:
	// BUG-L01-014: host features declared through setTraderSpiFeatures.
	void setSpiFeatures(uint64_t features) { m_spiFeatures.store(features); }
	std::atomic<uint64_t>	m_spiFeatures{ 0 };

	TraderXTP();
	virtual ~TraderXTP();

	typedef enum
	{
		TS_NOTLOGIN,		//未登录
		TS_LOGINING,		//正在登录
		TS_LOGINED,			//已登录
		TS_LOGINFAILED,		//登录失败
		TS_ALLREADY			//全部就绪
	} TraderState;

public:
	//////////////////////////////////////////////////////////////////////////
	//XTP::API::TraderSpi 接口
	virtual void OnDisconnected(uint64_t session_id, int reason) override;

	virtual void OnError(XTPRI *error_info) override;

	virtual void OnOrderEvent(XTPOrderInfo *order_info, XTPRI *error_info, uint64_t session_id) override;

	virtual void OnTradeEvent(XTPTradeReport *trade_info, uint64_t session_id) override;

	virtual void OnCancelOrderError(XTPOrderCancelInfo *cancel_info, XTPRI *error_info, uint64_t session_id) override;

	virtual void OnQueryOrder(XTPQueryOrderRsp *order_info, XTPRI *error_info, int request_id, bool is_last, uint64_t session_id) override;

	virtual void OnQueryTrade(XTPQueryTradeRsp *trade_info, XTPRI *error_info, int request_id, bool is_last, uint64_t session_id) override;

	virtual void OnQueryPosition(XTPQueryStkPositionRsp *position, XTPRI *error_info, int request_id, bool is_last, uint64_t session_id) override;

	virtual void OnQueryAsset(XTPQueryAssetRsp *asset, XTPRI *error_info, int request_id, bool is_last, uint64_t session_id) override;

public:
	//////////////////////////////////////////////////////////////////////////
	//ITraderApi 接口
	virtual bool init(WTSVariant *params) override;

	virtual void release() override;

	virtual void registerSpi(ITraderSpi *listener) override;

	virtual void connect() override;

	virtual void disconnect() override;

	virtual bool isConnected() override;

	virtual bool makeEntrustID(char* buffer, int length) override;

	virtual int login(const char* user, const char* pass, const char* productInfo) override;

	virtual int logout() override;

	virtual int orderInsert(WTSEntrust* eutrust) override;

	virtual int orderAction(WTSEntrustAction* action) override;

	virtual int queryAccount() override;

	virtual int queryPositions() override;

	virtual int queryOrders() override;

	virtual int queryTrades() override;

public:
	// Card 9 (9a/9b/9d) support for the factory remover and the host.
	// True on this instance's worker or inside one of its SDK callbacks/API calls.
	bool		isCallbackContext() const;
	// True when release() completed: worker joined, API calls and SDK callbacks
	// drained, SDK Release() returned (or no SDK API existed).
	bool		isQuiesced() const;
	// Detach the worker of an instance that is kept alive until process exit.
	void		detachForQuarantine();

private:
	ITraderSpi*	sink() const { return _sink.load(std::memory_order_acquire); }
	class CallbackGate;
	bool		enterCallback();
	void		leaveCallback();
	// Waits (bounded) until no admitted SDK callback is inside this wrapper.
	bool		waitCallbacksDrained();
	void		reconnect(bool nonblocking = false);
	bool		startWorker();
	bool		stopWorker(bool joinWorker = true);
	class ApiCallLease;
	bool		beginApiCall(bool allowMissingApi);
	void		endApiCall();
	bool		beginApiReconfigure(bool nonblocking);
	bool		beginApiNotification();
	void		endApiReconfigure();
	void		requestApiStop();
	void		beginApiRelease();
	void		finishApiRelease();
	void		scheduleReconnect(uint64_t generation, bool login);
	inline uint32_t			genRequestID();
	bool					doLogin();
	bool					doLoginLocked(const char* user, const char* pass, bool updateCredentials);

	inline WTSOrderInfo*	makeOrderInfo(XTPQueryOrderRsp* orderField);
	inline WTSEntrust*		makeEntrust(XTPOrderInfo *entrustField);
	inline WTSTradeInfo*	makeTradeInfo(XTPQueryTradeRsp *tradeField);

	inline bool	extractEntrustID(const char* entrustid, uint32_t &orderRef);
	inline void	genEntrustID(char* buffer, uint32_t orderRef);

private:
	XTP::API::TraderApi*	_api;
	std::atomic<ITraderSpi*>	_sink;	// registerSpi(NULL) may race SDK callbacks

	typedef WTSHashMap<std::string> PositionMap;
	PositionMap*			_positions;
	WTSArray*				_trades;
	WTSArray*				_orders;

	IBaseDataMgr*			_bd_mgr;

	std::string		_user;
	std::string		_pass;
	std::mutex		_login_state_mutex; // Protects credentials and login/session snapshots.
	std::mutex		_login_mutex;
	std::string		_flowdir;

	std::string		_acckey;

	std::string		_host;
	int				_port;
	int				_client;

	bool			_quick;
	bool			_inited;

	uint32_t			_hbInterval;

	TraderState		_state;

	uint64_t		_sessionid;
	uint32_t		_tradingday;
	std::atomic<uint32_t>		_reqid;
	std::atomic<uint32_t>		_ordref;		//报单引用

	boost::asio::io_context		_asyncio;
	boost::asio::steady_timer	_reconnect_timer;
	StdThreadPtr				_thrd_worker;
	typedef std::shared_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> BoostWorkerPtr;
	BoostWorkerPtr				_worker;
	std::mutex				_worker_mutex;
	std::condition_variable	_worker_condition;
	bool				_worker_joining = false;
	std::mutex				_reconnect_timer_mutex;
	std::atomic<bool>		_worker_stopping;
	std::atomic<uint64_t>	_worker_generation;
	std::chrono::milliseconds	_reconnect_delay;
	std::mutex				_api_lifecycle_mutex;
	std::condition_variable	_api_lifecycle_condition;
	uint32_t			_api_calls_in_flight = 0;
	bool				_api_stopping;
	bool				_api_reconfiguring = false;
	bool				_api_release_requested = false;
	bool				_api_release_in_progress = false;
	uint32_t			_api_release_waiters = 0;	// Observable waiters for deterministic lifecycle tests.
	// BUG-R93-005 / card 9: SDK callbacks admitted into this wrapper (protected by
	// _api_lifecycle_mutex). release()/reconnect() wait for them, bounded, before
	// SDK Release(); on timeout the old API is retained (never released) and the
	// instance reports drain-uncertain so the remover quarantines it.
	uint32_t			_callbacks_in_flight = 0;
	bool				_drain_uncertain = false;
	uint32_t			_drain_timeout_ms = 2000;
	std::vector<XTP::API::TraderApi*>	_retained_apis;
	// Login sessions of API instances already replaced or released: a late
	// OnDisconnected for one of them must not trigger a relogin of the current API.
	std::unordered_set<uint64_t>	_retired_sessions;

	DllHandle		m_hInstXTP;
	typedef XTP::API::TraderApi* (*XTPCreator)(uint8_t, const char*, XTP_LOG_LEVEL);
	XTPCreator		m_funcCreator;

	//委托单标记缓存器
	WtKVCache		m_eidCache;
	//订单标记缓存器
	WtKVCache		m_oidCache;
};

