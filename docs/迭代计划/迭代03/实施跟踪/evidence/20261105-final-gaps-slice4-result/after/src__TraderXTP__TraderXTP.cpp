/*!
 * \file TraderXTP.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "TraderXTP.h"
#include "../Share/ProviderRawReport.hpp"

#include "../Includes/IBaseDataMgr.h"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/WTSTradeDef.hpp"
#include "../Includes/WTSVariant.hpp"

#include "../Share/ModuleHelper.hpp"
#include "../Share/Converter.hpp"

#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <vector>
#ifndef _WIN32
#include <dlfcn.h>
#endif
namespace fs = std::filesystem;

 //By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"
template<typename... Args>
inline void write_log(ITraderSpi* sink, WTSLogLevel ll, const char* format, const Args&... args)
{
	if (sink == NULL)
		return;

	const char* buffer = fmtutil::format(format, args...);

	sink->handleTraderLog(ll, buffer);
}

namespace
{
	// Card 9d: the TraderXTP whose worker runs on this thread.
	thread_local const TraderXTP* t_xtp_worker_owner = NULL;

	// Card 9a/9b: keep this module mapped while a retained instance may run.
	void pin_xtp_module()
	{
#ifdef _WIN32
		HMODULE self = NULL;
		::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
			reinterpret_cast<LPCSTR>(&pin_xtp_module), &self);
#else
		Dl_info info;
		if (::dladdr(reinterpret_cast<void*>(&pin_xtp_module), &info) != 0 && info.dli_fname != NULL)
			(void)::dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
#endif
	}

	// The registry is never destroyed so retained instances stay reachable.
	void quarantine_xtp(TraderXTP* trader, const char* reason)
	{
		static std::mutex* mtx = new std::mutex();
		static std::vector<TraderXTP*>* retained = new std::vector<TraderXTP*>();
		{
			std::lock_guard<std::mutex> lock(*mtx);
			retained->push_back(trader);
		}
		pin_xtp_module();
		std::fprintf(stderr, "[TraderXTP] deleteTrader: instance quarantined until process exit (%s)\n", reason);
	}
}

extern "C"
{
	// BUG-L01-014 / K42: optional host feature declaration (WT_TRADER_SPI_FEATURES_SYMBOL).
	EXPORT_FLAG void setTraderSpiFeatures(ITraderApi* trader, uint64_t features)
	{
		TraderXTP* self = dynamic_cast<TraderXTP*>(trader);
		if (self != NULL)
			self->setSpiFeatures(features);
	}

	EXPORT_FLAG ITraderApi* createTrader()
	{
		TraderXTP *instance = new TraderXTP();
		return instance;
	}

	EXPORT_FLAG void deleteTrader(ITraderApi* &trader)
	{
		if (NULL != trader)
		{
			TraderXTP* xtp = dynamic_cast<TraderXTP*>(trader);
			if (xtp != NULL)
			{
				if (xtp->isCallbackContext())
				{
					// Card 9d/9b: an SDK callback, API call or the worker cannot
					// confirm its own exit. Close admission, request stop, keep it.
					xtp->release();
					xtp->detachForQuarantine();
					quarantine_xtp(xtp, "called from an SDK callback, API call or worker thread");
					trader = NULL;
					return;
				}
				xtp->release();
				if (!xtp->isQuiesced())
				{
					// Card 9a: the worker, API calls or SDK callbacks did not drain.
					xtp->detachForQuarantine();
					quarantine_xtp(xtp, "worker, API calls or SDK callbacks did not drain");
					trader = NULL;
					return;
				}
			}
			delete trader;
			trader = NULL;
		}
	}
}

const char* ENTRUST_SECTION = "entrusts";
const char* ORDER_SECTION = "orders";

inline bool IsErrorInfo(XTPRI *error_info)
{
	if (error_info && error_info->error_id != 0)
		return true;

	return false;
}

inline WTSDirectionType wrapPosDirection(XTP_POSITION_DIRECTION_TYPE dirType)
{
	switch (dirType)
	{
	case XTP_POSITION_DIRECTION_SHORT: return WDT_SHORT;
	default:
		return WDT_LONG;
	}
}

inline XTP_SIDE_TYPE wrapDirectionType(WTSDirectionType dirType, WTSOffsetType offsetType)
{
	if (WDT_LONG == dirType)
		if (offsetType == WOT_OPEN)
			return XTP_SIDE_BUY;
		else
			return XTP_SIDE_SELL;
	else
		if (offsetType == WOT_OPEN)
			return XTP_SIDE_SELL;
		else
			return XTP_SIDE_BUY;
}

inline WTSDirectionType wrapDirectionType(XTP_SIDE_TYPE side, XTP_POSITION_EFFECT_TYPE pe = 0)
{
	if (XTP_SIDE_BUY == side)
		if (pe == XTP_POSITION_EFFECT_OPEN)
			return WDT_LONG;
		else
			return WDT_SHORT;
	else
		if (pe == XTP_POSITION_EFFECT_OPEN)
			return WDT_SHORT;
		else
			return WDT_LONG;
}

inline XTP_POSITION_EFFECT_TYPE wrapOffsetType(WTSOffsetType offType)
{
	if (WOT_OPEN == offType)
		return XTP_POSITION_EFFECT_OPEN;
	else if (WOT_CLOSE == offType)
		return XTP_POSITION_EFFECT_CLOSE;
	else if (WOT_CLOSETODAY == offType)
		return XTP_POSITION_EFFECT_CLOSETODAY;
	else if (WOT_CLOSEYESTERDAY == offType)
		return XTP_POSITION_EFFECT_CLOSEYESTERDAY;
	else
		return XTP_POSITION_EFFECT_FORCECLOSE;
}

inline WTSOffsetType wrapOffsetType(XTP_SIDE_TYPE /*side*/, XTP_POSITION_EFFECT_TYPE offType = 0)
{
	if (XTP_POSITION_EFFECT_OPEN == offType)
		return WOT_OPEN;
	else if (XTP_POSITION_EFFECT_CLOSE == offType)
		return WOT_CLOSE;
	else if (XTP_POSITION_EFFECT_CLOSETODAY == offType)
		return WOT_CLOSETODAY;
	else if (XTP_POSITION_EFFECT_CLOSEYESTERDAY == offType)
		return WOT_CLOSEYESTERDAY;
	else
		return WOT_FORCECLOSE;
}

inline WTSPriceType wrapPriceType(XTP_PRICE_TYPE priceType)
{
	if (XTP_PRICE_LIMIT == priceType)
		return WPT_LIMITPRICE;
	else 
		return WPT_ANYPRICE;
}

inline WTSOrderState wrapOrderState(XTP_ORDER_STATUS_TYPE orderState)
{
	switch (orderState)
	{
	case XTP_ORDER_STATUS_INIT:
		return WOS_NotTraded_NotQueuing;
	case XTP_ORDER_STATUS_ALLTRADED:
		return WOS_AllTraded;
	case XTP_ORDER_STATUS_PARTTRADEDQUEUEING:
		return WOS_PartTraded_Queuing;
	case XTP_ORDER_STATUS_NOTRADEQUEUEING:
		return WOS_NotTraded_Queuing;
	case XTP_ORDER_STATUS_PARTTRADEDNOTQUEUEING:
	case XTP_ORDER_STATUS_CANCELED:
		return WOS_Canceled;
	default:
		return WOS_Nottouched;
	}
}

inline uint32_t makeRefID()
{
	static std::atomic<uint32_t> auto_refid(0);
	if(auto_refid == 0)
		auto_refid = (uint32_t)((TimeUtils::getLocalTimeNow() - TimeUtils::makeTime(20220101, 0)) / 1000 * 100);
	return auto_refid.fetch_add(1);
}

namespace
{
	struct TraderXTPExecutionFrame
	{
		const TraderXTP* owner;
		bool reentrant_release_context;
		bool api_call_context;
		TraderXTPExecutionFrame* previous;
	};

	thread_local TraderXTPExecutionFrame* g_trader_xtp_execution_frame = NULL;

	class TraderXTPExecutionScope
	{
	public:
		TraderXTPExecutionScope(const TraderXTP* owner, bool reentrant_release_context, bool api_call_context = false)
			: _frame{ owner, reentrant_release_context, api_call_context, g_trader_xtp_execution_frame }
		{
			if (_frame.owner)
				g_trader_xtp_execution_frame = &_frame;
		}

		~TraderXTPExecutionScope()
		{
			leave();
		}

		void setApiCallContext(bool enabled)
		{
			_frame.api_call_context = enabled;
		}

		void leave()
		{
			if (_frame.owner)
			{
				g_trader_xtp_execution_frame = _frame.previous;
				_frame.owner = NULL;
			}
		}

	private:
		TraderXTPExecutionFrame _frame;
	};

	bool isTraderXTPReentrantRelease(const TraderXTP* owner)
	{
		for (TraderXTPExecutionFrame* frame = g_trader_xtp_execution_frame; frame; frame = frame->previous)
			if (frame->owner == owner && frame->reentrant_release_context)
				return true;
		return false;
	}

	bool isTraderXTPApiCallContext(const TraderXTP* owner)
	{
		for (TraderXTPExecutionFrame* frame = g_trader_xtp_execution_frame; frame; frame = frame->previous)
			if (frame->owner == owner && frame->api_call_context)
				return true;
		return false;
	}
}

class TraderXTP::ApiCallLease
{
public:
	ApiCallLease(TraderXTP& owner, bool allowMissingApi, bool exclusive = false, bool nonblockingExclusive = false)
		: _owner(&owner)
		, _exclusive(exclusive)
		, _acquired(exclusive ? owner.beginApiReconfigure(nonblockingExclusive) : owner.beginApiCall(allowMissingApi))
		, _scope(_acquired ? &owner : NULL, true, _acquired && !exclusive)
	{
	}

	~ApiCallLease()
	{
		_scope.leave();
		if (_acquired)
		{
			if (_exclusive)
				_owner->endApiReconfigure();
			else
				_owner->endApiCall();
		}
	}

	bool acquired() const { return _acquired; }
	bool handoffToApiCall()
	{
		if (!_acquired || !_exclusive || !_owner->beginApiNotification())
			return false;
		_exclusive = false;
		_scope.setApiCallContext(true);
		return true;
	}

private:
	TraderXTP* _owner;
	bool _exclusive;
	bool _acquired;
	TraderXTPExecutionScope _scope;
};

// BUG-R93-005 / card 9: counts an SDK callback while it runs inside this
// wrapper; once stop or API replacement is requested it is dropped.
class TraderXTP::CallbackGate
{
public:
	explicit CallbackGate(TraderXTP& owner) : _owner(&owner), _admitted(owner.enterCallback()) {}
	~CallbackGate() { _owner->leaveCallback(); }
	CallbackGate(const CallbackGate&) = delete;
	CallbackGate& operator=(const CallbackGate&) = delete;
	explicit operator bool() const { return _admitted; }

private:
	TraderXTP* _owner;
	bool _admitted;
};


TraderXTP::TraderXTP()
	: _api(NULL)
	, _sink(NULL)
	, _positions(NULL)
	, _trades(NULL)
	, _orders(NULL)
	, _bd_mgr(NULL)
	, _inited(false)
	, _hbInterval(0)
	, _state(TS_NOTLOGIN)
	, _sessionid(0)
	, _tradingday(0)
	, _reqid(1)
	, _ordref(makeRefID())
	, _reconnect_timer(_asyncio)
	, _worker_stopping(true)
	, _worker_generation(0)
	, _reconnect_delay(std::chrono::milliseconds(2000))
	, _api_stopping(false)
	, m_hInstXTP(NULL)
	, m_funcCreator(NULL)
	{
}


TraderXTP::~TraderXTP()
{
	if (isCallbackContext())
	{
		// Card 9b/9d: freeing the object from its own callback or worker leaves
		// that thread running on freed memory; deleteTrader quarantines instead.
		std::fprintf(stderr, "[TraderXTP] Direct delete on an SDK callback/worker thread is unsupported; use deleteTrader\n");
		std::abort();
	}
	release();
	if (!isQuiesced())
	{
		std::fprintf(stderr, "[TraderXTP] Destroyed before SDK callbacks/API calls drained; use deleteTrader\n");
		std::abort();
	}

	// I03-NEW-067: the result containers are created lazily by SDK callbacks and
	// owned by this instance; release() only clears them. Callbacks and API calls
	// are proven drained above, so return ownership exactly once.
	if (_positions != NULL)
	{
		_positions->release();
		_positions = NULL;
	}
	if (_trades != NULL)
	{
		_trades->release();
		_trades = NULL;
	}
	if (_orders != NULL)
	{
		_orders->release();
		_orders = NULL;
	}
}

bool TraderXTP::isCallbackContext() const
{
	return isTraderXTPReentrantRelease(this) || t_xtp_worker_owner == this;
}

bool TraderXTP::isQuiesced() const
{
	{
		std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(_api_lifecycle_mutex));
		if (_drain_uncertain || _api != NULL || _callbacks_in_flight != 0 || _api_calls_in_flight != 0 || _api_release_in_progress)
			return false;
	}
	std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(_worker_mutex));
	return !(_thrd_worker && _thrd_worker->joinable());
}

void TraderXTP::detachForQuarantine()
{
	std::lock_guard<std::mutex> lock(_worker_mutex);
	if (_worker_joining)
		return;	// an external release() is joining it
	if (_thrd_worker && _thrd_worker->joinable())
		_thrd_worker->detach();
}

bool TraderXTP::enterCallback()
{
	std::lock_guard<std::mutex> lock(_api_lifecycle_mutex);
	++_callbacks_in_flight;
	// Dropped after release/disconnect or while the API is being replaced.
	return !_api_stopping && !_api_release_requested && !_api_release_in_progress && !_api_reconfiguring;
}

void TraderXTP::leaveCallback()
{
	// Notify under the lock: a releaser may free this object right after it
	// observes zero callbacks and reacquires the mutex.
	std::lock_guard<std::mutex> lock(_api_lifecycle_mutex);
	if (_callbacks_in_flight > 0)
		--_callbacks_in_flight;
	_api_lifecycle_condition.notify_all();
}

bool TraderXTP::waitCallbacksDrained()
{
	std::unique_lock<std::mutex> lock(_api_lifecycle_mutex);
	return _api_lifecycle_condition.wait_for(lock, std::chrono::milliseconds(_drain_timeout_ms),
		[this]() { return _callbacks_in_flight == 0; });
}

#pragma region "XTP::API::TraderSpi"

WTSEntrust* TraderXTP::makeEntrust(XTPOrderInfo* order_info)
{
	std::string code, exchg;
	if (order_info->market == XTP_MKT_SH_A)
		exchg = "SSE";
	else
		exchg = "SZSE";
	code = order_info->ticker;
	WTSContractInfo* ct = _bd_mgr->getContract(code.c_str(), exchg.c_str());
	if (ct == NULL)
		return NULL;

	WTSCommodityInfo* commInfo = ct->getCommInfo();

	WTSEntrust* pRet = WTSEntrust::create(
		code.c_str(),
		(uint32_t)order_info->quantity,
		order_info->price,
		ct->getExchg());
	pRet->setContractInfo(ct);
	if (commInfo->isStock())
		pRet->setDirection(WDT_LONG);
	else
		pRet->setDirection(wrapDirectionType(order_info->side, order_info->position_effect));
	pRet->setPriceType(wrapPriceType(order_info->price_type));
	if (!commInfo->isOption())
		pRet->setOffsetType((order_info->side == XTP_SIDE_BUY)? WOT_OPEN : WOT_CLOSE);
	else
		pRet->setOffsetType(wrapOffsetType(order_info->side, order_info->position_effect));
	pRet->setOrderFlag(WOF_NOR);

	genEntrustID(pRet->getEntrustID(), order_info->order_client_id);

	const char* usertag = m_eidCache.get(pRet->getEntrustID());
	if (strlen(usertag) > 0)
		pRet->setUserTag(usertag);

	return pRet;
}

WTSOrderInfo* TraderXTP::makeOrderInfo(XTPQueryOrderRsp* order_info)
{
	std::string code, exchg;
	if (order_info->market == XTP_MKT_SH_A)
		exchg = "SSE";
	else
		exchg = "SZSE";
	code = order_info->ticker;
	WTSContractInfo* ct = _bd_mgr->getContract(code.c_str(), exchg.c_str());
	if (ct == NULL)
		return NULL;

	WTSCommodityInfo* commInfo = ct->getCommInfo();

	WTSOrderInfo* pRet = WTSOrderInfo::create();
	pRet->setContractInfo(ct);
	pRet->setPrice(order_info->price);
	pRet->setVolume((uint32_t)order_info->quantity);
	pRet->setPriceType(wrapPriceType(order_info->price_type));
	pRet->setOrderFlag(WOF_NOR);

	if (commInfo->isStock())
		pRet->setDirection(WDT_LONG);
	else
		pRet->setDirection(wrapDirectionType(order_info->side, order_info->position_effect));
	if (!commInfo->isOption())
		pRet->setOffsetType((order_info->side == XTP_SIDE_BUY) ? WOT_OPEN : WOT_CLOSE);
	else
		pRet->setOffsetType(wrapOffsetType(order_info->side, order_info->position_effect));

	pRet->setVolTraded((uint32_t)order_info->qty_traded);
	pRet->setVolLeft((uint32_t)order_info->qty_left);

	pRet->setCode(code.c_str());
	pRet->setExchange(ct->getExchg());

	pRet->setOrderDate((uint32_t)(order_info->insert_time / 1000000000));
	uint32_t uTime = order_info->insert_time % 1000000000;
	pRet->setOrderTime(TimeUtils::makeTime(pRet->getOrderDate(), uTime));

	pRet->setOrderState(wrapOrderState(order_info->order_status));
	if (order_info->order_status >= XTP_ORDER_STATUS_REJECTED)
		pRet->setError(WOEF_Normal);

	genEntrustID(pRet->getEntrustID(), order_info->order_client_id);
	fmtutil::format_to(pRet->getOrderID(), "{}", order_info->order_xtp_id);

	pRet->setStateMsg("");

	const char* usertag = m_eidCache.get(pRet->getEntrustID());
	if (strlen(usertag) == 0)
	{
		pRet->setUserTag(pRet->getEntrustID());
	}
	else
	{
		pRet->setUserTag(usertag);

		if (strlen(pRet->getOrderID()) > 0)
		{
			m_oidCache.put(StrUtil::trim(pRet->getOrderID()).c_str(), usertag, 0);
		}
	}

	return pRet;
}

WTSTradeInfo* TraderXTP::makeTradeInfo(XTPQueryTradeRsp* trade_info)
{
	std::string code, exchg;
	if (trade_info->market == XTP_MKT_SH_A)
		exchg = "SSE";
	else
		exchg = "SZSE";
	code = trade_info->ticker;
	WTSContractInfo* ct = _bd_mgr->getContract(code.c_str(), exchg.c_str());
	if (ct == NULL)
		return NULL;

	WTSCommodityInfo* commInfo = ct->getCommInfo();

	WTSTradeInfo *pRet = WTSTradeInfo::create(code.c_str(), exchg.c_str());
	pRet->setVolume((uint32_t)trade_info->quantity);
	pRet->setPrice(trade_info->price);
	pRet->setTradeID(trade_info->exec_id);
	pRet->setContractInfo(ct);

	uint32_t uTime = (uint32_t)(trade_info->trade_time % 1000000000);
	uint32_t uDate = (uint32_t)(trade_info->trade_time / 1000000000);

	pRet->setTradeDate(uDate);
	pRet->setTradeTime(TimeUtils::makeTime(uDate, uTime));

	if (commInfo->isStock())
		pRet->setDirection(WDT_LONG);
	else
		pRet->setDirection(wrapDirectionType(trade_info->side, trade_info->position_effect));
	if (!commInfo->isOption())
		pRet->setOffsetType((trade_info->side == XTP_SIDE_BUY) ? WOT_OPEN : WOT_CLOSE);
	else
		pRet->setOffsetType(wrapOffsetType(trade_info->side, trade_info->position_effect));

	fmtutil::format_to(pRet->getRefOrder(), "{}", trade_info->order_xtp_id);
	pRet->setTradeType(WTT_Common);

	double amount = trade_info->quantity*pRet->getPrice();
	pRet->setAmount(amount);

	const char* usertag = m_oidCache.get(StrUtil::trim(pRet->getRefOrder()).c_str());
	if (strlen(usertag))
		pRet->setUserTag(usertag);

	return pRet;
}

void TraderXTP::OnDisconnected(uint64_t session_id, int reason)
{
	TraderXTPExecutionScope callbackScope(this, true);
	CallbackGate callbackGate(*this);
	if (!callbackGate)
		return;
	{
		// BUG-R93-005: a disconnect of a login session that belonged to an API
		// instance already replaced/released, or of a session other than the
		// current one, must not close or relogin the current session.
		std::lock_guard<std::mutex> stateLock(_login_state_mutex);
		if (session_id != 0 && (_retired_sessions.count(session_id) != 0 || (_sessionid != 0 && session_id != _sessionid)))
			return;
	}
	if (ITraderSpi* sink = this->sink())
		sink->handleEvent(WTE_Close, reason);

	if (_worker_stopping.load())
		return;
	const uint64_t generation = _worker_generation.load();
	boost::asio::post(_asyncio, [this, generation](){
		if (_worker_stopping.load() || generation != _worker_generation.load())
			return;
		write_log(sink(), LL_WARN, "[TraderrXTP] Connection lost, relogin in 2 seconds...");
		scheduleReconnect(generation, true);
	});
}

void TraderXTP::OnError(XTPRI *error_info)
{
	TraderXTPExecutionScope callbackScope(this, true);
	CallbackGate callbackGate(*this);
	if (!callbackGate)
		return;
	if (sink() && error_info)
		write_log(sink(),LL_ERROR, error_info->error_msg);
}

void TraderXTP::OnOrderEvent(XTPOrderInfo *order_info, XTPRI *error_info, uint64_t /*session_id*/)
{
	TraderXTPExecutionScope callbackScope(this, true);
	CallbackGate callbackGate(*this);
	if (!callbackGate)
		return;
	if(IsErrorInfo(error_info))
	{
		WTSEntrust* entrust = makeEntrust(order_info);

		WTSError* error = WTSError::create(WEC_ORDERINSERT, error_info->error_msg);
		// BUG-R93-005: registerSpi(NULL) may have detached the host.
		if (ITraderSpi* sink = this->sink())
			sink->onRspEntrust(entrust, error);
		error->release();

		// I03-NEW-068: makeEntrust() returns NULL for an unregistered contract;
		// the host still receives the error (it rejects a NULL entrust itself).
		if (entrust != NULL)
			entrust->release();
	}
	else
	{
		WTSOrderInfo *orderInfo = makeOrderInfo(order_info);
		if (orderInfo)
		{
			if (ITraderSpi* sink = this->sink())
				sink->onPushOrder(orderInfo);

			orderInfo->release();
		}
		else
		{
			// BUG-L01-014: keep the unconverted SDK event for reconciliation (raw hex of the struct).
			wt_provider_raw::report(this->sink(), m_spiFeatures.load(), "TraderXTP", "order", "order report could not be converted",
				wt_provider_raw::hexOf("XTPOrderInfo", order_info));
		}
	}
}

void TraderXTP::OnTradeEvent(XTPTradeReport *trade_info, uint64_t /*session_id*/)
{
	TraderXTPExecutionScope callbackScope(this, true);
	CallbackGate callbackGate(*this);
	if (!callbackGate)
		return;
	WTSTradeInfo *trdInfo = makeTradeInfo(trade_info);
	if (trdInfo)
	{
		if (ITraderSpi* sink = this->sink())
			sink->onPushTrade(trdInfo);

		trdInfo->release();
	}
	else
	{
		// BUG-L01-014: keep the unconverted SDK event for reconciliation (raw hex of the struct).
		wt_provider_raw::report(this->sink(), m_spiFeatures.load(), "TraderXTP", "trade", "trade report could not be converted",
			wt_provider_raw::hexOf("XTPTradeReport", trade_info));
	}
}

void TraderXTP::OnCancelOrderError(XTPOrderCancelInfo */*cancel_info*/, XTPRI *error_info, uint64_t /*session_id*/)
{
	TraderXTPExecutionScope callbackScope(this, true);
	CallbackGate callbackGate(*this);
	if (!callbackGate)
		return;
	if (IsErrorInfo(error_info))
	{
		WTSError* error = WTSError::create(WEC_ORDERCANCEL, error_info->error_msg);
		if (ITraderSpi* sink = this->sink())
			sink->onTraderError(error);
		error->release();
	}

}

void TraderXTP::OnQueryOrder(XTPQueryOrderRsp *order_info, XTPRI *error_info, int /*request_id*/, bool is_last, uint64_t /*session_id*/)
{
	TraderXTPExecutionScope callbackScope(this, true);
	CallbackGate callbackGate(*this);
	if (!callbackGate)
		return;
	//if (is_last)
	//{
	//	m_bInQuery = false;
	//	triggerQuery();
	//}

	if (IsErrorInfo(error_info))
	{
		// K42: the SDK documents no "no record" code for order queries, so any
		// error makes the snapshot FAILED (card 14c: fail closed without a contract).
		_qry_orders_failed = true;
	}
	else if (order_info)
	{
		if (NULL == _orders)
			_orders = WTSArray::create();

		WTSOrderInfo* orderInfo = makeOrderInfo(order_info);
		if (orderInfo)
		{
			_orders->append(orderInfo, false);
		}
		else
		{
			// BUG-L01-014 (card 6): keep the unconverted query row (raw hex of the struct); the host pauses the account.
			wt_provider_raw::report(this->sink(), m_spiFeatures.load(), "TraderXTP", "order", "order query row could not be converted",
				wt_provider_raw::hexOf("XTPQueryOrderRsp", order_info));
			_qry_orders_dropped = true;
		}
	}

	if (is_last)
	{
		if (ITraderSpi* sink = this->sink())
		{
			// K42 (BUG-I02-NEW-154): a host that declared query-status support learns
			// whether the snapshot is complete; others keep the legacy callback, which
			// hosts handle conservatively.
			if (m_spiFeatures.load() & WT_TRADER_SPI_FEATURE_QUERY_STATUS)
			{
				if (NULL == _orders)
					_orders = WTSArray::create();
				sink->onRspOrdersWithStatus(_orders, _qry_orders_failed ? WT_QUERY_STATUS_FAILED
					: _qry_orders_dropped ? WT_QUERY_STATUS_PARTIAL : WT_QUERY_STATUS_COMPLETE);
			}
			else
				sink->onRspOrders(_orders);
		}
		_qry_orders_failed = false;
		_qry_orders_dropped = false;

		if (_orders)
			_orders->clear();
	}
}

void TraderXTP::OnQueryTrade(XTPQueryTradeRsp *trade_info, XTPRI *error_info, int /*request_id*/, bool is_last, uint64_t /*session_id*/)
{
	TraderXTPExecutionScope callbackScope(this, true);
	CallbackGate callbackGate(*this);
	if (!callbackGate)
		return;
	//if (is_last)
	//{
	//	m_bInQuery = false;
	//	triggerQuery();
	//}

	if (!IsErrorInfo(error_info) && trade_info)
	{
		if (NULL == _trades)
			_trades = WTSArray::create();

		WTSTradeInfo* trdInfo = makeTradeInfo(trade_info);
		if (trdInfo)
		{
			_trades->append(trdInfo, false);
		}
		else
		{
			// BUG-L01-014 (card 6): keep the unconverted query row (raw hex of the struct); the host pauses the account.
			wt_provider_raw::report(this->sink(), m_spiFeatures.load(), "TraderXTP", "trade", "trade query row could not be converted",
				wt_provider_raw::hexOf("XTPQueryTradeRsp", trade_info));
		}
	}

	if (is_last)
	{
		if (ITraderSpi* sink = this->sink())
			sink->onRspTrades(_trades);

		if (_trades)
			_trades->clear();
	}
}

void TraderXTP::OnQueryPosition(XTPQueryStkPositionRsp *position, XTPRI *error_info, int /*request_id*/, bool is_last, uint64_t /*session_id*/)
{
	TraderXTPExecutionScope callbackScope(this, true);
	CallbackGate callbackGate(*this);
	if (!callbackGate)
		return;
	//if (is_last)
	//{
	//	m_bInQuery = false;
	//	triggerQuery();
	//}

	if (!IsErrorInfo(error_info) && position )
	{
		if (NULL == _positions)
			_positions = PositionMap::create();

		std::string code, exchg;
		if (position->market == XTP_MKT_SH_A)
			exchg = "SSE";
		else
			exchg = "SZSE";
		code += position->ticker;
		WTSContractInfo* contract = _bd_mgr->getContract(code.c_str(), exchg.c_str());
		if (contract)
		{
			WTSCommodityInfo* commInfo = contract->getCommInfo();
			std::string key = fmt::format("{}-{}", code.c_str(), position->position_direction);
			WTSPositionItem* pos = (WTSPositionItem*)_positions->get(key);
			if (pos == NULL)
			{
				pos = WTSPositionItem::create(code.c_str(), commInfo->getCurrency(), commInfo->getExchg());
				pos->setContractInfo(contract);
				_positions->add(key, pos, false);
			}
			pos->setDirection(wrapPosDirection(position->position_direction));

			pos->setNewPosition((double)(position->total_qty - position->yesterday_position));
			pos->setPrePosition((double)position->yesterday_position);

			pos->setMargin(position->total_qty*position->avg_price);
			pos->setDynProfit(0);
			pos->setPositionCost(position->total_qty*position->avg_price);

			pos->setAvgPrice(position->avg_price);

			pos->setAvailNewPos(0);
			pos->setAvailPrePos((double)position->sellable_qty);
		}
	}

	if (is_last)
	{

		WTSArray* ayPos = WTSArray::create();

		if (_positions && _positions->size() > 0)
		{
			for (auto it = _positions->begin(); it != _positions->end(); it++)
			{
				ayPos->append(it->second, true);
			}
		}

		if (ITraderSpi* sink = this->sink())
			sink->onRspPosition(ayPos);

		if (_positions)
		{
			_positions->release();
			_positions = NULL;
		}

		ayPos->release();
	}
}

void TraderXTP::OnQueryAsset(XTPQueryAssetRsp *asset, XTPRI *error_info, int /*request_id*/, bool is_last, uint64_t /*session_id*/)
{
	TraderXTPExecutionScope callbackScope(this, true);
	CallbackGate callbackGate(*this);
	if (!callbackGate)
		return;
	//if (is_last)
	//{
	//	m_bInQuery = false;
	//	triggerQuery();
	//}

	if (is_last && !IsErrorInfo(error_info) && asset)
	{
		WTSAccountInfo* accountInfo = WTSAccountInfo::create();
		accountInfo->setPreBalance(asset->orig_banlance);
		accountInfo->setCloseProfit(0);
		accountInfo->setDynProfit(0);
		accountInfo->setMargin(0);
		accountInfo->setAvailable(asset->buying_power);
		accountInfo->setCommission(asset->fund_sell_fee);
		accountInfo->setFrozenMargin(asset->withholding_amount);
		accountInfo->setFrozenCommission(0);
		if (asset->deposit_withdraw > 0)
			accountInfo->setDeposit(asset->deposit_withdraw);
		else if (asset->deposit_withdraw < 0)
			accountInfo->setWithdraw(0);
		accountInfo->setBalance(asset->total_asset);
		accountInfo->setCurrency("CNY");

		WTSArray * ay = WTSArray::create();
		ay->append(accountInfo, false);
		if (ITraderSpi* sink = this->sink())
			sink->onRspAccount(ay);

		ay->release();
	}
}

#pragma endregion "XTP::API:TraderSpi"

#pragma region "ITraderApi"
bool TraderXTP::init(WTSVariant *params)
{
	if (params == NULL)
	{
		write_log(sink(), LL_ERROR, "[TraderXTP] Cannot initialize with null configuration");
		return false;
	}
	{
		// Card 9c: an initialized instance (module loaded) or one with a live API
		// rejects init() and keeps its original configuration.
		std::lock_guard<std::mutex> lock(_api_lifecycle_mutex);
		if (m_hInstXTP != NULL || m_funcCreator != NULL || _api != NULL)
		{
			write_log(sink(), LL_ERROR, "[TraderXTP] Repeated init rejected");
			return false;
		}
	}
	{
		std::lock_guard<std::mutex> lock(_login_state_mutex);
		_user = params->getCString("user");
		_pass = params->getCString("pass");
	}
	_acckey = params->getCString("acckey");

	_host = params->getCString("host");
	_port = params->getInt32("port");

	_client = params->getInt32("client");

	_quick = params->getBoolean("quick");	

	_flowdir = params->getCString("flowdir");

	_hbInterval = params->getUInt32("hbinterval");

	if (_flowdir.empty())
		_flowdir = "XTPTDFlow";

	_flowdir = StrUtil::standardisePath(_flowdir);

	std::string module = params->getCString("xtpmodule");
	if (module.empty()) module = "xtptraderapi";
	const std::string wrappedModule = DLLHelper::wrap_module(module.c_str(), "lib");
	const char* moduleDir = getBinDir();
	if ((moduleDir == NULL || *moduleDir == '\0') && !fs::path(wrappedModule).is_absolute())
	{
		write_log(sink(), LL_ERROR, "[TraderXTP] Cannot locate trader module directory");
		return false;
	}
	std::string dllpath = (moduleDir == NULL ? std::string() : std::string(moduleDir)) + wrappedModule;

	m_hInstXTP = DLLHelper::load_library(dllpath.c_str());
#ifdef _WIN32
#	ifdef _WIN64
	const char* creatorName = "?CreateTraderApi@TraderApi@API@XTP@@SAPEAV123@EPEBDW4XTP_LOG_LEVEL@@@Z";
#	else
	const char* creatorName = "?CreateTraderApi@TraderApi@API@XTP@@SAPAV123@EPBDW4XTP_LOG_LEVEL@@@Z";
#	endif
#else
	const char* creatorName = "_ZN3XTP3API9TraderApi15CreateTraderApiEhPKc13XTP_LOG_LEVEL";
#endif
	if (m_hInstXTP == NULL)
	{
		// BUG-R93-005: a missing module is an explicit initialization failure.
		write_log(sink(), LL_ERROR, "[TraderXTP] Failed to load trader API module: {}", dllpath);
		return false;
	}
	m_funcCreator = (XTPCreator)DLLHelper::get_symbol(m_hInstXTP, creatorName);
	if (m_funcCreator == NULL)
	{
		write_log(sink(), LL_ERROR, "[TraderXTP] Trader API creator symbol not found in {}: {}", dllpath, creatorName);
		DLLHelper::free_library(m_hInstXTP);
		m_hInstXTP = NULL;
		return false;
	}

	return true;
}

bool TraderXTP::beginApiCall(bool allowMissingApi)
{
	const bool nestedApiCall = isTraderXTPApiCallContext(this);
	std::lock_guard<std::mutex> lock(_api_lifecycle_mutex);
	if (_api_release_requested || _api_release_in_progress ||
		((_api_stopping || _api_reconfiguring) && !nestedApiCall) ||
		(!allowMissingApi && _api == NULL))
		return false;
	++_api_calls_in_flight;
	return true;
}

void TraderXTP::endApiCall()
{
	std::lock_guard<std::mutex> lock(_api_lifecycle_mutex);
	if (_api_calls_in_flight > 0)
		--_api_calls_in_flight;
	_api_lifecycle_condition.notify_all();
}

bool TraderXTP::beginApiReconfigure(bool nonblocking)
{
	std::unique_lock<std::mutex> lock(_api_lifecycle_mutex);
	if (_api_stopping || _api_release_requested || _api_release_in_progress || _api_reconfiguring)
		return false;
	if (nonblocking && _api_calls_in_flight != 0)
		return false;

	_api_stopping = true;
	_api_reconfiguring = true;
	if (!nonblocking)
		_api_lifecycle_condition.wait(lock, [this]() {
			return _api_calls_in_flight == 0 || _api_release_requested || _api_release_in_progress;
		});
	if (_api_release_requested || _api_release_in_progress)
	{
		_api_reconfiguring = false;
		_api_lifecycle_condition.notify_all();
		return false;
	}
	return true;
}

bool TraderXTP::beginApiNotification()
{
	std::lock_guard<std::mutex> lock(_api_lifecycle_mutex);
	if (!_api_reconfiguring || _api_release_requested || _api_release_in_progress)
		return false;

	++_api_calls_in_flight;
	_api_reconfiguring = false;
	_api_stopping = false;
	_api_lifecycle_condition.notify_all();
	return true;
}

void TraderXTP::endApiReconfigure()
{
	std::lock_guard<std::mutex> lock(_api_lifecycle_mutex);
	_api_reconfiguring = false;
	if (!_api_release_requested && !_api_release_in_progress)
		_api_stopping = false;
	_api_lifecycle_condition.notify_all();
}

void TraderXTP::requestApiStop()
{
	std::lock_guard<std::mutex> lock(_api_lifecycle_mutex);
	_api_stopping = true;
	_api_release_requested = true;
}

void TraderXTP::beginApiRelease()
{
	std::unique_lock<std::mutex> lock(_api_lifecycle_mutex);
	_api_stopping = true;
	_api_release_requested = true;
	_api_lifecycle_condition.notify_all();
	if (_api_release_in_progress || _api_reconfiguring)
	{
		++_api_release_waiters;
		_api_lifecycle_condition.wait(lock, [this]() {
			return !_api_release_in_progress && !_api_reconfiguring;
		});
		--_api_release_waiters;
	}
	_api_release_in_progress = true;
}

void TraderXTP::finishApiRelease()
{
	std::lock_guard<std::mutex> lock(_api_lifecycle_mutex);
	_api_release_in_progress = false;
	_api_lifecycle_condition.notify_all();
}

void TraderXTP::release()
{
	if (isCallbackContext())
	{
		// Do not wait for this API call or SDK callback to unwind. An external owner
		// must call release() again to complete API destruction.
		requestApiStop();
		stopWorker(false);
		return;
	}

	beginApiRelease();
	if (!stopWorker())
	{
		finishApiRelease();
		return;
	}

	XTP::API::TraderApi* api = NULL;
	{
		std::unique_lock<std::mutex> lock(_api_lifecycle_mutex);
		_api_lifecycle_condition.wait(lock, [this]() { return _api_calls_in_flight == 0; });
		{
			std::lock_guard<std::mutex> stateLock(_login_state_mutex);
			_state = TS_NOTLOGIN;
			// A late OnDisconnected of this session must not relogin a later API.
			if (_sessionid != 0)
				_retired_sessions.insert(_sessionid);
			_sessionid = 0;
		}
		api = _api;
		_api = NULL;
	}

	bool drained = true;
	if (api)
	{
		try
		{
			api->RegisterSpi(NULL);
		}
		catch (...)
		{
			// Still attempt Release and complete the local shutdown protocol.
		}
		// BUG-R93-005 / card 9a: SDK Release() must not overlap a callback that
		// already entered this wrapper. Bounded; on timeout keep the API alive.
		drained = waitCallbacksDrained();
		if (drained)
		{
			try
			{
				api->Release();
			}
			catch (...)
			{
				// The SDK release call has no error return; do not strand concurrent releasers.
			}
		}
		else
		{
			std::lock_guard<std::mutex> lock(_api_lifecycle_mutex);
			_retained_apis.push_back(api);
			_drain_uncertain = true;
			std::fprintf(stderr, "[TraderXTP] release: SDK callbacks did not drain within %u ms; API kept, instance must be quarantined\n",
				_drain_timeout_ms);
		}
	}
	else if (!waitCallbacksDrained())
	{
		std::lock_guard<std::mutex> lock(_api_lifecycle_mutex);
		_drain_uncertain = true;
		drained = false;
	}

	if (drained)
	{
		if (_orders)
			_orders->clear();

		if (_positions)
			_positions->clear();

		if (_trades)
			_trades->clear();
	}

	finishApiRelease();
}

void TraderXTP::registerSpi(ITraderSpi *listener)
{
	_sink.store(listener, std::memory_order_release);
	if (listener)
	{
		_bd_mgr = listener->getBaseDataMgr();
	}
}

void TraderXTP::reconnect(bool nonblocking)
{
	ApiCallLease apiLease(*this, true, true, nonblocking);
	if (!apiLease.acquired() || _worker_stopping.load())
	{
		if (nonblocking && !_worker_stopping.load())
			scheduleReconnect(_worker_generation.load(), false);
		return;
	}

	if (_api)
	{
		{
			// BUG-R93-005: the replaced API's session is retired; a late
			// OnDisconnected carrying it must not relogin the new API.
			std::lock_guard<std::mutex> stateLock(_login_state_mutex);
			if (_sessionid != 0)
				_retired_sessions.insert(_sessionid);
			_sessionid = 0;
			_state = TS_NOTLOGIN;
		}
		_api->RegisterSpi(NULL);
		// Card 9a: never Release() the old API under a callback that already
		// entered this wrapper (new callbacks are dropped while reconfiguring).
		if (!waitCallbacksDrained())
		{
			{
				std::lock_guard<std::mutex> lock(_api_lifecycle_mutex);
				_retained_apis.push_back(_api);
				_drain_uncertain = true;
			}
			_api = NULL;
			write_log(sink(), LL_ERROR, "[TraderXTP] Previous API callbacks did not drain; reconnect aborted and old API retained");
			return;
		}
		_api->Release();
		_api = NULL;
	}

	std::string user;
	{
		std::lock_guard<std::mutex> lock(_login_state_mutex);
		user = _user;
	}
	std::stringstream ss;
	ss << _flowdir << "flows/" << user << "/";
	fs::create_directories(ss.str().c_str());
	_api = m_funcCreator(_client, ss.str().c_str(), XTP_LOG_LEVEL_DEBUG);			// 创建UserApi
	int32_t connectionResult = 0;
	if (_api == NULL)
	{
		connectionResult = -1;
		write_log(sink(),LL_ERROR, "[TraderrXTP] Module initializing failed");
		scheduleReconnect(_worker_generation.load(), false);
	}
	else
	{
		_api->SubscribePublicTopic(_quick ? XTP_TERT_QUICK : XTP_TERT_RESUME);
		_api->SetSoftwareVersion("1.0.0"); //设定此软件的开发版本号,用户自定义
		_api->SetSoftwareKey(_acckey.c_str());//设定用户的开发代码,在XTP申请开户时,由xtp人员提供
		_api->SetHeartBeatInterval(15);//设定交易服务器超时时间,单位为秒,此为1.1.16新增接口
		_api->RegisterSpi(this);						// 注册事件
	}

	// Convert the exclusive reconfiguration lease into a counted shared callback lease
	// before notifying the adapter. A synchronous handleEvent() -> login() can then
	// acquire its own nested lease, while release/reconnect must wait for this callback.
	if (!apiLease.handoffToApiCall())
		return;
	if (ITraderSpi* sink = this->sink())
		sink->handleEvent(WTE_Connect, connectionResult);
}

void TraderXTP::connect()
{
	if (m_funcCreator == NULL)
	{
		write_log(sink(), LL_ERROR, "[TraderXTP] Cannot connect without a trader API creator");
		return;
	}
	if (isTraderXTPReentrantRelease(this))
		return;
	if (!startWorker() || _worker_stopping.load())
		return;

	reconnect();
}

bool TraderXTP::startWorker()
{
	std::unique_lock<std::mutex> apiLock(_api_lifecycle_mutex);
	if (_api_release_in_progress || _api_reconfiguring)
		return false;
	std::lock_guard<std::mutex> lock(_worker_mutex);
	if (_thrd_worker && _thrd_worker->joinable())
		return !_worker_stopping.load();

	_asyncio.restart();
	_api_release_requested = false;
	_api_stopping = false;
	_worker_stopping.store(false);
	_worker_generation.fetch_add(1);
	_worker.reset(new boost::asio::executor_work_guard<boost::asio::io_context::executor_type>(
		boost::asio::make_work_guard(_asyncio)));
	_thrd_worker.reset(new StdThread([this]() {
		t_xtp_worker_owner = this;
		_asyncio.run();
		t_xtp_worker_owner = NULL;
	}));
	return true;
}

bool TraderXTP::stopWorker(bool joinWorker)
{
	std::unique_lock<std::mutex> workerLock(_worker_mutex);
	_worker_stopping.store(true);
	_worker_generation.fetch_add(1);
	{
		std::lock_guard<std::mutex> timerLock(_reconnect_timer_mutex);
		try
		{
			_reconnect_timer.cancel();
		}
		catch (...)
		{
			// Timer cancellation is best-effort during shutdown. Keep stopping and joining
			// the worker even if this Boost version reports a cancellation error by exception.
		}
	}

	_worker.reset();
	_asyncio.stop();
	if (!joinWorker)
		return false;
	if (_thrd_worker && _thrd_worker->joinable())
	{
		if (_thrd_worker->get_id() == std::this_thread::get_id())
		{
			// The worker cannot join itself. Leave the API alive for an external release();
			// destruction from this worker callback is unsupported.
			return false;
		}
		if (_worker_joining)
		{
			_worker_condition.wait(workerLock, [this]() { return !_worker_joining; });
			return true;
		}

		_worker_joining = true;
		StdThreadPtr worker = _thrd_worker;
		workerLock.unlock();
		try
		{
			worker->join();
		}
		catch (...)
		{
			workerLock.lock();
			_worker_joining = false;
			workerLock.unlock();
			_worker_condition.notify_all();
			return false;
		}
		workerLock.lock();
		if (_thrd_worker == worker)
			_thrd_worker.reset();
		_worker_joining = false;
		workerLock.unlock();
		_worker_condition.notify_all();
		return true;
	}
	_thrd_worker.reset();
	return true;
}

void TraderXTP::scheduleReconnect(uint64_t generation, bool login)
{
	std::lock_guard<std::mutex> lock(_reconnect_timer_mutex);
	if (_worker_stopping.load() || generation != _worker_generation.load())
		return;

	_reconnect_timer.expires_after(_reconnect_delay);
	_reconnect_timer.async_wait([this, generation, login](const boost::system::error_code& ec) {
		if (ec || _worker_stopping.load() || generation != _worker_generation.load())
			return;
		if (login)
			doLogin();
		else
		{
			std::string user;
			{
				std::lock_guard<std::mutex> lock(_login_state_mutex);
				user = _user;
			}
			write_log(sink(), LL_WARN, "[TraderrXTP] {} reconnecting...", user.c_str());
			reconnect(true);
		}
	});
}

void TraderXTP::disconnect()
{
	release();
}

bool TraderXTP::isConnected()
{
	std::lock_guard<std::mutex> lock(_login_state_mutex);
	return _state == TS_ALLREADY;
}

void TraderXTP::genEntrustID(char* buffer, uint32_t orderRef)
{
	//这里不再使用sessionid，因为每次登陆会不同，如果使用的话，可能会造成不唯一的情况
	std::string user;
	uint32_t tradingday;
	{
		std::lock_guard<std::mutex> lock(_login_state_mutex);
		user = _user;
		tradingday = _tradingday;
	}
	fmtutil::format_to(buffer, "{}#{}#{}", user, tradingday, orderRef);
}

bool TraderXTP::extractEntrustID(const char* entrustid, uint32_t &orderRef)
{
	auto idx = StrUtil::findLast(entrustid, '#');
	if (idx == std::string::npos)
		return false;

	orderRef = convert::to_uint32(entrustid + idx + 1);

	return true;
}

bool TraderXTP::makeEntrustID(char* buffer, int length)
{
	if (buffer == NULL || length == 0)
		return false;

	try
	{
		uint32_t orderref = _ordref.fetch_add(1) + 1;
		std::string user;
		uint32_t tradingday;
		{
			std::lock_guard<std::mutex> lock(_login_state_mutex);
			user = _user;
			tradingday = _tradingday;
		}
		fmtutil::format_to(buffer, "{}#{}#{}", user, tradingday, orderref);
		return true;
	}
	catch (...)
	{

	}

	return false;
}

bool TraderXTP::doLogin()
{
	std::unique_lock<std::mutex> loginLock(_login_mutex, std::try_to_lock);
	if (!loginLock.owns_lock())
		return false;
	return doLoginLocked(NULL, NULL, false);
}

bool TraderXTP::doLoginLocked(const char* requestedUser, const char* requestedPass, bool updateCredentials)
{
	ApiCallLease apiLease(*this, false);
	if (!apiLease.acquired())
		return false;

	std::string user;
	std::string pass;
	{
		std::lock_guard<std::mutex> stateLock(_login_state_mutex);
		if (updateCredentials)
		{
			_user = requestedUser ? requestedUser : "";
			_pass = requestedPass ? requestedPass : "";
		}
		user = _user;
		pass = _pass;
		_state = TS_LOGINING;
	}

	_api->SetHeartBeatInterval(_hbInterval);
	uint64_t iResult = _api->Login(_host.c_str(), _port, user.c_str(), pass.c_str(), XTP_PROTOCOL_TCP);
	if (iResult == 0)
	{
		auto error_info = _api->GetApiLastError();
		write_log(sink(), LL_ERROR, "[TraderXTP] Login failed: {}", error_info->error_msg);
		std::string msg = error_info->error_msg;
		{
			std::lock_guard<std::mutex> stateLock(_login_state_mutex);
			_state = TS_LOGINFAILED;
		}
		const uint64_t generation = _worker_generation.load();
		boost::asio::post(_asyncio, [this, msg, generation] {
			if (_worker_stopping.load() || generation != _worker_generation.load())
				return;
			if (ITraderSpi* sink = this->sink())
				sink->onLoginResult(false, msg.c_str(), 0);
		});
	}
	else
	{
		bool alreadyInited;
		{
			std::lock_guard<std::mutex> stateLock(_login_state_mutex);
			_sessionid = iResult;
			alreadyInited = _inited;
		}
		if (!alreadyInited)
		{
			const uint32_t tradingday = convert::to_uint32(_api->GetTradingDay());
			{
				std::lock_guard<std::mutex> stateLock(_login_state_mutex);
				_tradingday = tradingday;
			}
			{
				//初始化委托单缓存器
				std::stringstream ss;
				ss << "./xtpdata/local/";
				std::string path = StrUtil::standardisePath(ss.str());
				if (!StdFile::exists(path.c_str()))
					fs::create_directories(path.c_str());
				ss << user << "_eid.sc";
				m_eidCache.init(ss.str().c_str(), tradingday, [this](const char* message) {
					write_log(sink(), LL_WARN, message);
				});
			}

			{
				//初始化订单标记缓存器
				std::stringstream ss;
				ss << "./xtpdata/local/";
				std::string path = StrUtil::standardisePath(ss.str());
				if (!StdFile::exists(path.c_str()))
					fs::create_directories(path.c_str());
				ss << user << "_oid.sc";
				m_oidCache.init(ss.str().c_str(), tradingday, [this](const char* message) {
					write_log(sink(), LL_WARN, message);
				});
			}

			write_log(sink(), LL_INFO, "[TraderXTP] [{}] Login succeed, trading date: {}, api version: {}", user, tradingday, _api->GetApiVersion());

			{
				std::lock_guard<std::mutex> stateLock(_login_state_mutex);
				_state = TS_LOGINED;
				_inited = true;
			}
			const uint64_t generation = _worker_generation.load();
			boost::asio::post(_asyncio, [this, generation, tradingday] {
				if (_worker_stopping.load() || generation != _worker_generation.load())
					return;
				{
					std::lock_guard<std::mutex> stateLock(_login_state_mutex);
					_state = TS_ALLREADY;
				}
				if (ITraderSpi* sink = this->sink())
					sink->onLoginResult(true, 0, tradingday);
			});
		}
		else
		{
			write_log(sink(), LL_INFO, "[TraderXTP] [{}] Connection recovered", user.c_str());
			std::lock_guard<std::mutex> stateLock(_login_state_mutex);
			_state = TS_ALLREADY;
		}
	}
	return true;
}

int TraderXTP::login(const char* user, const char* pass, const char* /*productInfo*/)
{
	std::unique_lock<std::mutex> loginLock(_login_mutex, std::try_to_lock);
	if (!loginLock.owns_lock())
		return -1;
	return doLoginLocked(user, pass, true) ? 0 : -1;
}

int TraderXTP::logout()
{
	ApiCallLease apiLease(*this, false);
	if (!apiLease.acquired())
		return -1;

	uint64_t sessionid;
	{
		std::lock_guard<std::mutex> stateLock(_login_state_mutex);
		if ((_state != TS_LOGINED && _state != TS_ALLREADY) || _sessionid == 0)
			return -1;
		sessionid = _sessionid;
	}
	_api->Logout(sessionid);
	return 0;
}

int TraderXTP::orderInsert(WTSEntrust* entrust)
{
	ApiCallLease apiLease(*this, false);
	if (!apiLease.acquired())
		return -1;
	uint64_t sessionid;
	{
		std::lock_guard<std::mutex> stateLock(_login_state_mutex);
		if (_state != TS_ALLREADY)
			return -1;
		sessionid = _sessionid;
	}
	XTPOrderInsertInfo req;
	memset(&req, 0, sizeof(req));
	
	uint32_t orderref;
	extractEntrustID(entrust->getEntrustID(), orderref);
	req.order_client_id = orderref;
	strcpy(req.ticker, entrust->getCode());
	req.market = wt_stricmp(entrust->getExchg(), "SSE") == 0 ? XTP_MKT_SH_A : XTP_MKT_SZ_A;
	req.price = entrust->getPrice();
	req.quantity = (int64_t)entrust->getVolume();
	req.price_type = XTP_PRICE_LIMIT;
	req.side = wrapDirectionType(entrust->getDirection(), entrust->getOffsetType());
	req.business_type = XTP_BUSINESS_TYPE_CASH;
	req.position_effect = wrapOffsetType(entrust->getOffsetType());

	if (strlen(entrust->getUserTag()) > 0)
	{
		m_eidCache.put(entrust->getEntrustID(), entrust->getUserTag(), 0);
	}

	uint64_t iResult = _api->InsertOrder(&req, sessionid);
	if (iResult == 0)
	{
		auto error_info = _api->GetApiLastError();
		write_log(sink(),LL_ERROR, "[TraderXTP] Order inserting failed: {}", error_info->error_msg);
	}

	return 0;
}

int TraderXTP::orderAction(WTSEntrustAction* action)
{
	ApiCallLease apiLease(*this, false);
	if (!apiLease.acquired())
		return -1;
	uint64_t sessionid;
	{
		std::lock_guard<std::mutex> stateLock(_login_state_mutex);
		if (_state != TS_ALLREADY)
			return -1;
		sessionid = _sessionid;
	}
	uint64_t iResult = _api->CancelOrder(convert::to_uint64(action->getOrderID()), sessionid);
	if (iResult == 0)
	{
		auto error_info = _api->GetApiLastError();
		write_log(sink(),LL_ERROR, "[TraderXTP] Order cancelling failed: {}", error_info->error_msg);
	}

	return 0;
}

uint32_t TraderXTP::genRequestID()
{
	return _reqid.fetch_add(1) + 1;
}

int TraderXTP::queryAccount()
{
	ApiCallLease apiLease(*this, false);
	if (!apiLease.acquired())
		return -1;

	uint64_t sessionid;
	{
		std::lock_guard<std::mutex> stateLock(_login_state_mutex);
		sessionid = _sessionid;
	}
	int iResult = _api->QueryAsset(sessionid, genRequestID());
	if (iResult != 0)
	{
		auto error_info = _api->GetApiLastError();
		write_log(sink(),LL_ERROR, "[TraderXTP] Account querying failed: {}", error_info->error_msg);
	}

	return 0;
}

int TraderXTP::queryPositions()
{
	ApiCallLease apiLease(*this, false);
	if (!apiLease.acquired())
		return -1;

	uint64_t sessionid;
	{
		std::lock_guard<std::mutex> stateLock(_login_state_mutex);
		sessionid = _sessionid;
	}
	int iResult = _api->QueryPosition("", sessionid, genRequestID());
	if (iResult != 0)
	{
		auto error_info = _api->GetApiLastError();
		write_log(sink(),LL_ERROR, "[TraderXTP] Positions querying failed: {}", error_info->error_msg);
	}

	return 0;
}

int TraderXTP::queryOrders()
{
	ApiCallLease apiLease(*this, false);
	if (!apiLease.acquired())
		return -1;

	XTPQueryOrderReq req;
	memset(&req, 0, sizeof(XTPQueryOrderReq));
	uint64_t sessionid;
	{
		std::lock_guard<std::mutex> stateLock(_login_state_mutex);
		sessionid = _sessionid;
	}
	int iResult = _api->QueryOrders(&req, sessionid, genRequestID());
	if (iResult != 0)
	{
		auto error_info = _api->GetApiLastError();
		write_log(sink(),LL_ERROR, "[TraderXTP] Orders querying failed: {}", error_info->error_msg);
	}

	return 0;
}

int TraderXTP::queryTrades()
{
	ApiCallLease apiLease(*this, false);
	if (!apiLease.acquired())
		return -1;

	XTPQueryTraderReq req;
	memset(&req, 0, sizeof(XTPQueryTraderReq));
	uint64_t sessionid;
	{
		std::lock_guard<std::mutex> stateLock(_login_state_mutex);
		sessionid = _sessionid;
	}
	int iResult = _api->QueryTrades(&req, sessionid, genRequestID());
	if (iResult != 0)
	{
		auto error_info = _api->GetApiLastError();
		write_log(sink(),LL_ERROR, "[TraderXTP] Trades querying failed: {}", error_info->error_msg);
	}

	return 0;
}
#pragma endregion "ITraderApi"
