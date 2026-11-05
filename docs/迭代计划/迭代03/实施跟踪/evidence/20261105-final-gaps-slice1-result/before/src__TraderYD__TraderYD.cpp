/*!
 * \file TraderYD.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "TraderYD.h"
#include "../Share/ProviderRawReport.hpp"
#include <API/yd1.108.360/ydError.h>

#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/WTSTradeDef.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/WTSCollection.hpp"

#include "../Share/decimal.h"
#include "../Share/Converter.hpp"
#include "../Share/ModuleHelper.hpp"

#include <filesystem>
#include <limits>
#include <memory>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
#ifndef _WIN32
#include <dlfcn.h>
#endif
namespace fs = std::filesystem;

const char* ENTRUST_SECTION = "entrusts";
const char* ORDER_SECTION = "orders";

//By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"
template<typename... Args>
inline void write_log(ITraderSpi* sink, WTSLogLevel ll, const char* format, const Args&... args) noexcept
{
	if (sink == NULL)
		return;

	static thread_local char buffer[512] = { 0 };
	fmtutil::format_to(buffer, format, args...);

	sink->handleTraderLog(ll, buffer);
}

constexpr inline WTSDirectionType wrapPosDirection(int dirType) noexcept
{
	return (YD_PD_Long == dirType) ? WDT_LONG : WDT_SHORT;
}

constexpr inline WTSDirectionType wrapDirectionType(int dirType, int offsetType) noexcept
{
	return (YD_D_Buy == dirType) ? ((offsetType == YD_OF_Open)? WDT_LONG: WDT_SHORT) : ((offsetType == YD_OF_Open) ? WDT_SHORT : WDT_LONG);
}

constexpr inline int wrapDirectionType(WTSDirectionType dirType, WTSOffsetType offsetType) noexcept
{
	return (WDT_LONG == dirType) ? ((offsetType == WOT_OPEN)? YD_D_Buy: YD_D_Sell) : ((offsetType == WOT_OPEN) ? YD_D_Sell : YD_D_Buy);
}

constexpr inline int wrapPriceType(WTSPriceType pType, WTSOrderFlag oFlag) noexcept
{
	switch (oFlag)
	{
	case WOF_FAK: return YD_ODT_FAK;
	case WOF_FOK: return YD_ODT_FOK;
	default:
		break;
	}

	return (pType == WPT_LIMITPRICE) ? YD_ODT_Limit : YD_ODT_Market;	
}

constexpr inline WTSOffsetType wrapOffsetType(int offType) noexcept
{
	switch (offType)
	{
	case YD_OF_Open: 
		return WOT_OPEN;
	case YD_OF_Close: 
		return WOT_CLOSE;
	case YD_OF_CloseToday: 
		return WOT_CLOSETODAY;
	case YD_OF_CloseYesterday: 
		return WOT_CLOSEYESTERDAY;
	default:
		return WOT_FORCECLOSE;
	}
}

constexpr inline int wrapOffsetType(WTSOffsetType offType) noexcept
{
	switch (offType)
	{
	case WOT_OPEN:
		return YD_OF_Open;
	case WOT_CLOSE:
		return YD_OF_Close;
	case WOT_CLOSETODAY:
		return YD_OF_CloseToday;
	case WOT_CLOSEYESTERDAY:
		return YD_OF_CloseYesterday;
	default:
		return YD_OF_ForceClose;
	}
}

constexpr inline WTSOrderState wrapOrderState(int orderState) noexcept
{
	switch (orderState)
	{
	case YD_OS_Accepted: return WOS_NotTraded_NotQueuing;
	case YD_OS_Queuing: return WOS_NotTraded_Queuing;
	case YD_OS_Canceled:
	case YD_OS_Rejected:
		return WOS_Canceled;
	case YD_OS_AllTraded: return WOS_AllTraded;
	default:
		return WOS_Submitting;
	}
}

constexpr inline const char* getErrorMsg(int ydError) noexcept
{
	switch (ydError)
	{
	case YD_ERROR_NoError: return "NoError";
	case YD_ERROR_NoPositionToClose: return "NoPositionToClose";
	case YD_ERROR_NoMoneyToOpen: return "NoMoneyToOpen";
	case YD_ERROR_SystemNotReady: return "SystemNotReady";
	case YD_ERROR_OrderFieldError: return "OrderFieldError";
	case YD_ERROR_MemoryExceed: return "MemoryExceed";
	case YD_ERROR_NoTradingCodeInExchange: return "NoTradingCodeInExchange";
	case YD_ERROR_CanNotSendToExchange: return "CanNotSendToExchange";
	case YD_ERROR_NoTradingRight: return "NoTradingRight";
	case YD_ERROR_InvalidOrderVolume: return "InvalidOrderVolume";
	case YD_ERROR_InvalidClientAPP: return "InvalidClientAPP";
	case YD_ERROR_PositionLimitExceed: return "PositionLimitExceed";
	case YD_ERROR_TradeVolumeExceed: return "TradeVolumeExceed";
	case YD_ERROR_OrderCancelLimitExceed: return "OrderCancelLimitExceed";
	case YD_ERROR_OrderOpenLimitExceed: return "OrderOpenLimitExceed";
	case YD_ERROR_InvalidConnectionID: return "InvalidConnectionID";
	case YD_ERROR_AlreadyLogined: return "AlreadyLogined";
	case YD_ERROR_PasswordError: return "PasswordError";
	case YD_ERROR_TooManyRequests: return "TooManyRequests";
	case YD_ERROR_InvalidUsername: return "InvalidUsername";
	case YD_ERROR_InsertOrderTooFast: return "InsertOrderTooFast";
	case YD_ERROR_PossibleSelfTrade: return "PossibleSelfTrade";
	case YD_ERROR_NoAdminRight: return "NoAdminRight";
	case YD_ERROR_InvalidAddress: return "InvalidAddress";
	case YD_ERROR_OrderTypeNotSupported: return "OrderTypeNotSupported";
	case YD_ERROR_CancelOrderFieldError: return "CancelOrderFieldError";
	case YD_ERROR_InvalidExchange: return "InvalidExchange";
	case YD_ERROR_OrderNotFound: return "OrderNotFound";
	case YD_ERROR_OrderNotBelongToAccount: return "OrderNotBelongToAccount";
	case YD_ERROR_OrderFinished: return "OrderFinished";
	case YD_ERROR_OnlyLimitOrderCanBeCanceled: return "OnlyLimitOrderCanBeCanceled";
	case YD_ERROR_ClientReportError: return "ClientReportError";
	case YD_ERROR_TooManyOrders: return "TooManyOrders";
	case YD_ERROR_InstrumentCanNotTrade: return "InstrumentCanNotTrade";
	case YD_ERROR_YDOrderFlagNotSupported: return "YDOrderFlagNotSupported";
	case YD_ERROR_NotOptionInstrument: return "NotOptionInstrument";
	case YD_ERROR_PriceOutOfLimit: return "PriceOutOfLimit";
	case YD_ERROR_CrossPriceInQuote: return "CrossPriceInQuote";
	case YD_ERROR_QuoteFieldError: return "QuoteFieldError";
	case YD_ERROR_QuoteVolumeError: return "QuoteVolumeError";
	case YD_ERROR_QuoteNotFound: return "QuoteNotFound";
	case YD_ERROR_CancelQuoteFieldError: return "CancelQuoteFieldError";
	case YD_ERROR_QuoteNotBelongToAccount: return "QuoteNotBelongToAccount";
	case YD_ERROR_QuoteFinished: return "QuoteFinished";
	case YD_ERROR_QuoteNotSupported: return "QuoteNotSupported";
	case YD_ERROR_CannotCancelQuoteDerivedOrder: return "CannotCancelQuoteDerivedOrder";
	case YD_ERROR_TooManyLogines: return "TooManyLogines";
	case YD_ERROR_NoEnoughPositiontoMakeCombPosition: return "NoEnoughPositiontoMakeCombPosition";
	case YD_ERROR_NoEnoughCombPosition: return "NoEnoughCombPosition";
	case YD_ERROR_NoMoneyForSplitCombPosition: return "NoMoneyForSplitCombPosition";
	case YD_ERROR_InvalidCombPosition: return "InvalidCombPosition";
	case YD_ERROR_CannotSelectConnection: return "CannotSelectConnection";
	case YD_ERROR_SelectConnectionTooFrequently: return "SelectConnectionTooFrequently";
	case YD_ERROR_InvalidSelectConnection: return "InvalidSelectConnection";
	case YD_ERROR_TooLowApiVersion: return "TooLowApiVersion";
	case YD_ERROR_InvalidTradingRight: return "InvalidTradingRight";
	case YD_ERROR_InvalidProduct: return "InvalidProduct";
	case YD_ERROR_InvalidAlterMoneyField: return "InvalidAlterMoneyField";
	case YD_ERROR_TooHighApiVersion: return "TooHighApiVersion";
	case YD_ERROR_MoneyUsageTooLow: return "MoneyUsageTooLow";
	case YD_ERROR_InvalidInstrumentPairToExecuteTogether: return "InvalidInstrumentPairToExecuteTogether";
	case YD_ERROR_NotOnExpireDay: return "NotOnExpireDay";
	case YD_ERROR_NotProperTime: return "NotProperTime";
	case YD_ERROR_OptonLongPositionCostLimitExceed: return "OptonLongPositionCostLimitExceed";
	case YD_ERROR_PriceToTriggerFuse: return "PriceToTriggerFuse";
	case YD_ERROR_ExchangeDoesNotSupport: return "ExchangeDoesNotSupport";

	/// error numbers generated by ydAPI
	case YD_ERROR_CannotSend: return "CannotSend";
	case YD_ERROR_TooManyInMultiOrders: return "TooManyInMultiOrders";

	/// error numbers generated by exchanges
	case YD_ERROR_ExchangeReportError: return "ExchangeReportError";

	//上期所的错误码
	case 1026:	return "NotInTradingTime";
	case 1028:	return "OrderAllTraded";
	default:
		return "Unknown Error";
	}
}

namespace
{
	// Card 9d: the TraderYD whose query worker runs on this thread, and the
	// innermost TraderYD YD callback on this thread.
	thread_local const TraderYD* t_yd_worker_owner = NULL;
	thread_local const TraderYD* t_yd_callback_owner = NULL;

	// Card 9a/9b: keep this module mapped while a retained instance may run.
	void pin_yd_module()
	{
#ifdef _WIN32
		HMODULE self = NULL;
		::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
			reinterpret_cast<LPCSTR>(&pin_yd_module), &self);
#else
		Dl_info info;
		if (::dladdr(reinterpret_cast<void*>(&pin_yd_module), &info) != 0 && info.dli_fname != NULL)
			(void)::dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
#endif
	}

	// The registry is never destroyed so retained instances stay reachable.
	void quarantine_yd(TraderYD* trader, const char* reason)
	{
		static std::mutex* mtx = new std::mutex();
		static std::vector<TraderYD*>* retained = new std::vector<TraderYD*>();
		{
			std::lock_guard<std::mutex> lock(*mtx);
			retained->push_back(trader);
		}
		pin_yd_module();
		std::fprintf(stderr, "[TraderYD] deleteTrader: instance quarantined until process exit (%s)\n", reason);
	}
}

extern "C"
{
	// BUG-L01-014 / K42: optional host feature declaration (WT_TRADER_SPI_FEATURES_SYMBOL).
	EXPORT_FLAG void setTraderSpiFeatures(ITraderApi* trader, uint64_t features)
	{
		TraderYD* self = dynamic_cast<TraderYD*>(trader);
		if (self != NULL)
			self->setSpiFeatures(features);
	}

	EXPORT_FLAG ITraderApi* createTrader()
	{
		TraderYD *instance = new TraderYD();
		return instance;
	}

	EXPORT_FLAG void deleteTrader(ITraderApi* &trader)
	{
		if (NULL != trader)
		{
			TraderYD* yd = dynamic_cast<TraderYD*>(trader);
			if (yd != NULL)
			{
				if (yd->isCallbackContext())
				{
					// Card 9d/9b: a YD callback or the query worker cannot confirm
					// its own exit nor wait for notifyAfterApiDestroy().
					yd->release();
					yd->detachForQuarantine();
					quarantine_yd(yd, "called from a YD callback or worker thread");
					trader = NULL;
					return;
				}
				yd->release();
				if (!yd->isQuiesced())
				{
					// Card 9a / ydApi.h: the listener may not be deleted before
					// notifyAfterApiDestroy() and drained callbacks.
					yd->detachForQuarantine();
					quarantine_yd(yd, "YD destroy notification or callbacks did not complete");
					trader = NULL;
					return;
				}
			}
			delete trader;
			trader = NULL;
		}
	}
}

class TraderYD::ApiLease
{
public:
	explicit ApiLease(TraderYD* owner) : _owner(owner), _held(owner->acquireApiLease()) {}
	~ApiLease() { if (_held) _owner->releaseApiLease(); }
	ApiLease(const ApiLease&) = delete;
	ApiLease& operator=(const ApiLease&) = delete;
	explicit operator bool() const { return _held; }

private:
	TraderYD*	_owner;
	bool		_held;
};

class TraderYD::CallbackScope
{
public:
	CallbackScope(TraderYD* owner, bool gated)
		: _owner(owner)
		, _previous(t_yd_callback_owner)
	{
		t_yd_callback_owner = owner;
		_admitted = owner->enterCallback(gated);
	}
	~CallbackScope()
	{
		_owner->leaveCallback();
		t_yd_callback_owner = _previous;
	}
	CallbackScope(const CallbackScope&) = delete;
	CallbackScope& operator=(const CallbackScope&) = delete;
	bool admitted() const { return _admitted; }

private:
	TraderYD*		_owner;
	const TraderYD*	_previous;
	bool			_admitted = false;
};

// Every YD trading callback counts itself while inside this wrapper and is
// dropped once release() has been requested (card 9 isolation).
#define YD_CALLBACK_SCOPE() \
	CallbackScope ydCallbackScope(this, true); \
	if (!ydCallbackScope.admitted()) \
		return

TraderYD::TraderYD()
	: m_bCatchup(false)
	, m_bApiInited(false)
	, m_sink(NULL)
	, m_uLastQryTime(0)
	, m_wrapperState(WS_NOTLOGIN)
	, m_pUserAPI(NULL)
	, m_iRequestID(0)
	, m_mapPosition(NULL)
	, m_mapTrades(NULL)
	, m_mapOrders(NULL)
	, m_ayFunds(NULL)
	, m_bStopped(false)
	, m_apiLeases(0)
	, m_activeCallbacks(0)
	, m_workerExited(true)
	, m_destroyStarted(false)
	, m_apiDestroyed(false)
	, m_stopState(YS_RUNNING)
	, m_drainTimeoutMs(2000)
	, m_hInstYD(NULL)
	, m_funcCreator(NULL)
	{
}


TraderYD::~TraderYD()
{
	if (isCallbackContext())
	{
		std::fprintf(stderr, "[TraderYD] Direct delete on a YD callback/worker thread is unsupported; use deleteTrader\n");
		std::abort();
	}
	release();
	if (!isQuiesced())
	{
		// ydApi.h: the listener must outlive notifyAfterApiDestroy().
		std::fprintf(stderr, "[TraderYD] Destroyed before notifyAfterApiDestroy/callback drain; use deleteTrader\n");
		std::abort();
	}

	// I03-NEW-067: the result containers are created lazily by YD callbacks and
	// owned by this instance; release() only clears them. The worker, host calls
	// and callbacks are proven drained above, so return ownership exactly once.
	DataMap** maps[] = { &m_mapOrders, &m_mapPosition, &m_mapTrades };
	for (DataMap** slot : maps)
	{
		if (*slot != NULL)
		{
			(*slot)->release();
			*slot = NULL;
		}
	}
	if (m_ayFunds != NULL)
	{
		m_ayFunds->release();
		m_ayFunds = NULL;
	}
}

TraderYD::StopState TraderYD::stopState() const
{
	std::lock_guard<std::mutex> lock(m_mtxLifecycle);
	return m_stopState;
}

bool TraderYD::isCallbackContext() const
{
	return t_yd_worker_owner == this || t_yd_callback_owner == this;
}

bool TraderYD::isQuiesced() const
{
	return stopState() == YS_RELEASED;
}

bool TraderYD::acquireApiLease()
{
	std::lock_guard<std::mutex> lock(m_mtxLifecycle);
	if (m_bStopped.load(std::memory_order_acquire))
		return false;
	++m_apiLeases;
	return true;
}

void TraderYD::releaseApiLease()
{
	std::lock_guard<std::mutex> lock(m_mtxLifecycle);
	if (m_apiLeases > 0)
		--m_apiLeases;
	m_cvLifecycle.notify_all();
}

bool TraderYD::enterCallback(bool gated)
{
	std::lock_guard<std::mutex> lock(m_mtxLifecycle);
	++m_activeCallbacks;
	return !gated || !m_bStopped.load(std::memory_order_acquire);
}

void TraderYD::leaveCallback()
{
	// Notify under the lock: a releaser may free this object as soon as it
	// observes the drained state and reacquires the mutex.
	std::lock_guard<std::mutex> lock(m_mtxLifecycle);
	if (m_activeCallbacks > 0)
		--m_activeCallbacks;
	m_cvLifecycle.notify_all();
}

void TraderYD::requestStop()
{
	std::lock_guard<std::mutex> lock(m_mtxLifecycle);
	m_bStopped.store(true, std::memory_order_release);
	if (m_stopState == YS_RUNNING)
		m_stopState = YS_STOP_DEFERRED;
}

void TraderYD::detachForQuarantine()
{
	std::unique_lock<std::mutex> releaseLock(m_mtxRelease, std::try_to_lock);
	if (!releaseLock.owns_lock())
		return;
	StdThreadPtr worker;
	{
		std::lock_guard<std::mutex> lock(m_mtxLifecycle);
		worker = m_thrdWorker;
	}
	if (worker && worker->joinable())
		worker->detach();
}

void TraderYD::workerMain()
{
	{
		// Barrier: connect() publishes m_thrdWorker under this mutex.
		std::lock_guard<std::mutex> lock(m_mtxLifecycle);
	}
	t_yd_worker_owner = this;
	while (!m_bStopped.load(std::memory_order_acquire))
	{
		CommonExecuter handler;
		{
			StdUniqueLock lock(m_mtxQuery);
			if (!m_queQuery.empty())
			{
				handler = m_queQuery.front();
				m_queQuery.pop();
			}
		}
		if (!handler)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}
		handler();
	}
	t_yd_worker_owner = NULL;
	std::lock_guard<std::mutex> lock(m_mtxLifecycle);
	m_workerExited = true;
	m_cvLifecycle.notify_all();
}

void TraderYD::notifyBeforeApiDestroy()
{
	CallbackScope scope(this, false);
}

void TraderYD::notifyAfterApiDestroy()
{
	CallbackScope scope(this, false);
	std::lock_guard<std::mutex> lock(m_mtxLifecycle);
	m_apiDestroyed = true;
	m_cvLifecycle.notify_all();
}


void TraderYD::notifyReadyForLogin(bool /*hasLoginFailed*/)
{
	YD_CALLBACK_SCOPE();
	if (m_sink)
		m_sink->handleEvent(WTE_Connect, 0);
}

void TraderYD::on_kvcache_message(const char* message)
{
	write_log(m_sink, LL_WARN, "[TraderYD] WtKVCache message: {}", message);
}

void TraderYD::notifyLogin(int errorNo, int maxOrderRef, bool /*isMonitor*/)
{
	YD_CALLBACK_SCOPE();
	if (errorNo == 0)
	{
		m_wrapperState = WS_LOGINED;

		// 保存会话参数
		m_orderRef = maxOrderRef;
		///获取当前交易日
		m_lDate = m_pUserAPI->getTradingDay();

		write_log(m_sink, LL_INFO, "[TraderYD] {} Login succeed, Trading Day: {}, AutoUpdateCache: {}, YDApi version: {}",
			m_strUser.c_str(), m_lDate, m_bUpdateCache, m_pUserAPI->getVersion());

		{
			//初始化委托单缓存器
			std::stringstream ss;
			ss <<  "ydlocal/" ;
			std::string path = StrUtil::standardisePath(ss.str());
			if (!StdFile::exists(path.c_str()))
				fs::create_directories(path.c_str());
			ss << m_strUser << "_eid.sc";
			m_eidCache.init(ss.str().c_str(), m_lDate, m_cacheLogger);
		}

		{
			//初始化订单标记缓存器
			std::stringstream ss;
			ss << "ydlocal/";
			std::string path = StrUtil::standardisePath(ss.str());
			if (!StdFile::exists(path.c_str()))
				fs::create_directories(path.c_str());
			ss << m_strUser << "_oid.sc";
			m_oidCache.init(ss.str().c_str(), m_lDate, m_cacheLogger);
		}
	}
	else
	{
		write_log(m_sink, LL_ERROR, "[TraderYD] {} Login failed: {}", m_strUser.c_str(), getErrorMsg(errorNo));
		m_wrapperState = WS_LOGINFAILED;

		if (m_sink)
			m_sink->onLoginResult(false, "Login failed", 0);
	}
}

void TraderYD::notifyFailedOrder(const YDInputOrder *pFailedOrder, const YDInstrument *pInstrument, const YDAccount */*pAccount*/)
{
	YD_CALLBACK_SCOPE();
	if (!m_bCatchup)
		return;

	WTSEntrust* entrust = makeEntrust(pFailedOrder, pInstrument);
	if (entrust)
	{
		WTSError *err = makeError(pFailedOrder->ErrorNo, WEC_ORDERINSERT);
		if (m_sink)
			m_sink->onRspEntrust(entrust, err);
		entrust->release();
		err->release();
	}
	else
	{
		WTSError* error = makeError(pFailedOrder->ErrorNo, WEC_ORDERINSERT);
		if (m_sink)
			m_sink->onTraderError(error);
		error->release();
	}
}

void TraderYD::notifyFailedCancelOrder(const YDFailedCancelOrder *pFailedCancelOrder, const YDExchange */*pExchange*/, const YDAccount */*pAccount*/)
{
	YD_CALLBACK_SCOPE();
	if (!m_bCatchup)
		return;

	if(pFailedCancelOrder->ErrorNo == 1028)
	{
		//1028是易达返回的待撤订单已成交的错误信息，可以不用发给系统，直接忽略
	}
	else
	{
		WTSError* error = makeError(pFailedCancelOrder->ErrorNo, WEC_ORDERCANCEL);
		if (m_sink)
			m_sink->onTraderError(error);
		error->release();
	}	
}


void TraderYD::notifyFinishInit()
{
	YD_CALLBACK_SCOPE();
	/*
	 *	这里的处理逻辑比较复杂
	 *	在第一次登陆成功以后，底层会去查询各种数据
	 *	底层查完了以后，会触发这个回调
	 *	这里需要进行拉取上日结算以后的数据
	 *	然后再利用回报进行数据拼接
	 *	最后在notifyCatchup的时候，再通知listener
	 */

	//先查资金
	{
		const YDAccount* accInfo = m_pUserAPI->getMyAccount();

		WTSAccountInfo* accountInfo = WTSAccountInfo::create();
		accountInfo->setPreBalance(accInfo->PreBalance);
		accountInfo->setDeposit(accInfo->Deposit);
		accountInfo->setWithdraw(accInfo->Withdraw);
		accountInfo->setBalance(accountInfo->getPreBalance() + accountInfo->getDeposit() - accountInfo->getWithdraw());
		accountInfo->setCurrency("CNY");

		if (m_ayFunds == NULL)
			m_ayFunds = WTSArray::create();

		m_ayFunds->append(accountInfo, false);
	}

	//再查持仓
	{
		if (NULL == m_mapPosition)
			m_mapPosition = DataMap::create();

		int cnt = m_pUserAPI->getPrePositionCount();
		for (int i = 0; i < cnt; i++)
		{
			const YDPrePosition* pInfo = m_pUserAPI->getPrePosition(i);
			const YDInstrument* instInfo = pInfo->m_pInstrument;

			WTSContractInfo* contract = m_bdMgr->getContract(instInfo->InstrumentID, instInfo->m_pExchange->ExchangeID);
			if (contract)
			{
				WTSCommodityInfo* commInfo = contract->getCommInfo();
				std::string key = fmtutil::format("{}-{}", contract->getCode(), wrapPosDirection(pInfo->PositionDirection));
				WTSPositionItem* pos = (WTSPositionItem*)m_mapPosition->get(key);
				if (pos == NULL)
				{
					pos = WTSPositionItem::create(contract->getCode(), commInfo->getCurrency(), commInfo->getExchg());
					pos->setDirection(wrapPosDirection(pInfo->PositionDirection));
					pos->setContractInfo(contract);
					m_mapPosition->add(key, pos, false);
				}

				pos->setPrePosition(pInfo->PrePosition);
				pos->setNewPosition(0);

				pos->setMargin(0);
				pos->setDynProfit(0);
				pos->setPositionCost(pInfo->PrePosition*commInfo->getVolScale()*pInfo->PreSettlementPrice);

				if (pos->getTotalPosition() != 0)
				{
					pos->setAvgPrice(pInfo->PreSettlementPrice);
				}
				else
				{
					pos->setAvgPrice(0);
				}

				pos->setAvailPrePos(pos->getPrePosition());
				pos->setAvailNewPos(0);

				write_log(m_sink, LL_INFO, "[TraderYD] {} PrePosition of {} updated:{}[{}]", 
					pos->getDirection() == WDT_LONG ? "Long" : "Short", contract->getFullCode(), pos->getTotalPosition(), pos->getAvailPosition());
			}
		}
	}
}


void TraderYD::notifyOrder(const YDOrder *pOrder, const YDInstrument *pInstrument, const YDAccount */*pAccount*/)
{
	YD_CALLBACK_SCOPE();
	WTSOrderInfo *orderInfo = makeOrderInfo(pOrder, pInstrument);
	if (orderInfo)
	{
		//先往缓存里丢
		if (!m_bCatchup || m_bUpdateCache)
		{
			if (NULL == m_mapOrders)
				m_mapOrders = DataMap::create();

			const char* oid = orderInfo->getOrderID();
			auto it = m_mapOrders->find(oid);
			if (it == m_mapOrders->end())
			{
				//如果该订单是第一次被推送
				//则检查是否是平仓委托
				//如果是平仓委托，需要调整冻结手数
				if (orderInfo->getOffsetType() != WOT_OPEN)
				{
					const char* key = fmtutil::format("{}-{}", orderInfo->getCode(), orderInfo->getDirection());
					WTSPositionItem* pos = (WTSPositionItem*)m_mapPosition->get(key);
					[[maybe_unused]] double preQty = pos->getPrePosition();
					[[maybe_unused]] double newQty = pos->getNewPosition();
					double availPre = pos->getAvailPrePos();
					double availNew = pos->getAvailNewPos();

					WTSCommodityInfo* commInfo = orderInfo->getContractInfo()->getCommInfo();
					FrozenSources frozen = { 0.0, 0.0 };
					if (commInfo->getCoverMode() == CM_CoverToday)
					{
						if (orderInfo->getOffsetType() == WOT_CLOSETODAY)
							frozen._new = orderInfo->getVolume();
						else
							frozen._pre = orderInfo->getVolume();
					}
					else
					{
						//不区分平昨平今，则先冻结昨仓，再冻结今仓
						// BUG-R93-006: the remainder beyond the available yesterday
						// quantity is frozen from today (the comparison was inverted,
						// so it was never frozen). Never freeze a negative amount.
						frozen._pre = std::max(0.0, std::min(availPre, orderInfo->getVolume()));
						if (decimal::lt(frozen._pre, orderInfo->getVolume()))
							frozen._new = orderInfo->getVolume() - frozen._pre;
					}
					availPre -= frozen._pre;
					availNew -= frozen._new;
					m_mapFrozen[oid] = frozen;

					pos->setAvailPrePos(availPre);
					pos->setAvailNewPos(availNew);
				}
			}
			else
			{
				WTSOrderInfo* preOrd = (WTSOrderInfo*)it->second;
				//如果订单不是第一次被推送，则看是否是撤单
				//如果是撤单，并且之间订单状态还是有效的，则对平仓委托要释放冻结的手数
				if (preOrd->isAlive() && orderInfo->getOrderState() == WOS_Canceled && orderInfo->getOffsetType() != WOT_OPEN)
				{
					std::string key = fmtutil::format("{}-{}", orderInfo->getCode(), orderInfo->getDirection());
					WTSPositionItem* pos = (WTSPositionItem*)m_mapPosition->get(key);
					double availPre = pos->getAvailPrePos();
					double availNew = pos->getAvailNewPos();

					double untrade = std::max(0.0, orderInfo->getVolume() - orderInfo->getVolTraded());

					// BUG-R93-006: restore the untraded part to the sources this
					// order froze when it was first seen. Fills consume yesterday
					// first (see notifyTrade), so the untraded remainder is returned
					// to today first, then to yesterday, never beyond what this
					// order froze. Partial-fill/TradeVolume vendor semantics remain
					// unverified (external evidence required).
					auto fit = m_mapFrozen.find(oid);
					if (fit != m_mapFrozen.end())
					{
						const FrozenSources& frozen = fit->second;
						const double restoreNew = std::min(frozen._new, untrade);
						const double restorePre = std::min(frozen._pre, untrade - restoreNew);
						availNew += restoreNew;
						availPre += restorePre;
						m_mapFrozen.erase(fit);
					}

					pos->setAvailPrePos(availPre);
					pos->setAvailNewPos(availNew);
				}
			}
			m_mapOrders->add(oid, orderInfo, false);
		}

		//如果已经追上了，则直接主推出去
		if (m_sink && m_bCatchup)
		{
			m_sink->onPushOrder(orderInfo);
		}
	}
	else
	{
		// BUG-L01-014: keep the unconverted SDK event for reconciliation (raw hex of the struct).
		wt_provider_raw::report(m_sink, m_spiFeatures.load(), "TraderYD", "order", "order report could not be converted",
			(wt_provider_raw::hexOf("YDOrder", pOrder) + ";InstrumentID=" + ((pInstrument != NULL) ? std::string(pInstrument->InstrumentID, strnlen(pInstrument->InstrumentID, sizeof(pInstrument->InstrumentID))) : std::string())));
	}
}


void TraderYD::notifyTrade(const YDTrade *pTrade, const YDInstrument *pInstrument, const YDAccount */*pAccount*/)
{
	YD_CALLBACK_SCOPE();
	WTSTradeInfo *trdInfo = makeTradeRecord(pTrade, pInstrument);
	if (trdInfo)
	{
		/*
		 *	By Wesley @ 2023.12.27
		 *	如果没有追上，或者强制要求更新缓存，就更新缓存
		 */
		if (!m_bCatchup || m_bUpdateCache)
		{
			if (NULL == m_mapTrades)
				m_mapTrades = DataMap::create();

			const char* tid = trdInfo->getTradeID();
			WTSContractInfo* contract = trdInfo->getContractInfo();
			WTSCommodityInfo* commInfo = contract->getCommInfo();
			auto it = m_mapTrades->find(tid);
			if (it == m_mapTrades->end())
			{
				m_mapTrades->add(tid, trdInfo, false);

				//成交回报，主要更新持仓
				std::string key = fmtutil::format("{}-{}", trdInfo->getCode(), trdInfo->getDirection());
				WTSPositionItem* pos = (WTSPositionItem*)m_mapPosition->get(key);
				if (pos == NULL)
				{
					pos = WTSPositionItem::create(contract->getCode(), commInfo->getCurrency(), commInfo->getExchg());
					pos->setContractInfo(contract);
					pos->setDirection(trdInfo->getDirection());
					m_mapPosition->add(key, pos, false);
				}

				double preQty = pos->getPrePosition();
				double newQty = pos->getNewPosition();
				[[maybe_unused]] double availPre = pos->getAvailPrePos();
				double availNew = pos->getAvailNewPos();

				double qty = trdInfo->getVolume();

				if (trdInfo->getOffsetType() == WOT_OPEN)
				{
					newQty += qty;
					availNew += qty;

					//开仓一定是今仓
					pos->setNewPosition(newQty);
					pos->setAvailNewPos(availNew);
				}
				else
				{
					//平仓要区分
					if (commInfo->getCoverMode() == CM_CoverToday)
					{
						//平仓不用更新可用持仓
						//因为可用持仓在订单回报的地方已经更新过了
						if (trdInfo->getOffsetType() == WOT_CLOSETODAY)
							newQty -= qty;
						else
							preQty -= qty;
					}
					else
					{
						//不区分平昨平今，则先减掉昨仓，在调整今仓
						// R93-006 s5: the part beyond yesterday's quantity closes today's
						// position (same order as the freeze in notifyOrder). The old
						// comparison was inverted and never reduced today's quantity.
						double maxQty = std::max(0.0, std::min(preQty, qty));
						preQty -= maxQty;
						if (decimal::lt(maxQty, qty))
						{
							newQty -= (qty - maxQty);
						}
					}

					pos->setNewPosition(newQty);
					pos->setPrePosition(preQty);
				}
			}
		}

		if (m_sink && m_bCatchup)
			m_sink->onPushTrade(trdInfo);
	}
	else
	{
		// BUG-L01-014: keep the unconverted SDK event for reconciliation (raw hex of the struct).
		wt_provider_raw::report(m_sink, m_spiFeatures.load(), "TraderYD", "trade", "trade report could not be converted",
			(wt_provider_raw::hexOf("YDTrade", pTrade) + ";InstrumentID=" + ((pInstrument != NULL) ? std::string(pInstrument->InstrumentID, strnlen(pInstrument->InstrumentID, sizeof(pInstrument->InstrumentID))) : std::string())));
	}
}

void TraderYD::notifyCaughtUp()
{
	YD_CALLBACK_SCOPE();
	m_bCatchup = true;

	if(!m_bApiInited)
	{
		//全部初始化好了以后，再通知登录成功
		m_wrapperState = WS_ALLREADY;

		if (m_sink)
			m_sink->onLoginResult(true, "", m_lDate);

		m_bApiInited = true;
	}
}

void TraderYD::notifyAccount(const YDAccount *accInfo)
{
	YD_CALLBACK_SCOPE();
	if (m_ayFunds == NULL || m_ayFunds->size() == 0)
		return;

	if (accInfo == NULL)
		return;

	WTSAccountInfo* accountInfo = static_cast<WTSAccountInfo*>(m_ayFunds->at(0));
	accountInfo->setPreBalance(accInfo->PreBalance);
	accountInfo->setDeposit(accInfo->Deposit);
	accountInfo->setWithdraw(accInfo->Withdraw);
}

bool TraderYD::init(WTSVariant* config)
{
	m_strCfgFile = config->getCString("config");
	m_strUser = config->getCString("user");
	m_strPass = config->getCString("pass");

	std::string module = config->getCString("ydmodule");
	if (module.empty())
		module = "yd";

	m_bUpdateCache = config->getBoolean("update_cache");

	m_strModule = getBinDir() + DLLHelper::wrap_module(module.c_str(), "");

	m_cacheLogger = std::bind(&TraderYD::on_kvcache_message, this, std::placeholders::_1);
	m_hInstYD = DLLHelper::load_library(m_strModule.c_str());
	if (m_hInstYD == NULL)
	{
		write_log(m_sink, LL_ERROR, "[TraderYD] Failed to load YD API module: {}", m_strModule.c_str());
		return false;
	}

	m_funcCreator = (YDCreator)DLLHelper::get_symbol(m_hInstYD, "makeYDApi");
	if (m_funcCreator == NULL)
	{
		write_log(m_sink, LL_ERROR, "[TraderYD] YD API module has no makeYDApi factory: {}", m_strModule.c_str());
		DLLHelper::free_library(m_hInstYD);
		m_hInstYD = NULL;
		return false;
	}

	return true;
}

void TraderYD::release()
{
	if (isCallbackContext())
	{
		// Card 9d: never wait for the worker, the callbacks or the YD destroy
		// notifications on a YD callback or the worker. The stop is completed by a
		// later external release(), the destructor, or never (quarantine).
		requestStop();
		return;
	}

	std::lock_guard<std::mutex> releaseLock(m_mtxRelease);
	requestStop();
	{
		std::lock_guard<std::mutex> lock(m_mtxLifecycle);
		if (m_stopState == YS_RELEASED)
			return;
	}

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(m_drainTimeoutMs);
	bool drained = true;

	// 1. Host calls that may still use the YD API (leases) and the query worker.
	{
		std::unique_lock<std::mutex> lock(m_mtxLifecycle);
		if (!m_cvLifecycle.wait_until(lock, deadline, [this]() { return m_apiLeases == 0; }))
			drained = false;
	}
	StdThreadPtr worker;
	{
		std::lock_guard<std::mutex> lock(m_mtxLifecycle);
		worker = m_thrdWorker;
	}
	if (drained && worker && worker->joinable())
	{
		std::unique_lock<std::mutex> lock(m_mtxLifecycle);
		const bool exited = m_cvLifecycle.wait_until(lock, deadline, [this]() { return m_workerExited; });
		lock.unlock();
		if (exited)
			worker->join();
		else
			drained = false;
	}

	// 2. YD callbacks already inside this wrapper may still read API data, which
	//    stays valid only until startDestroy() completes its first period.
	if (drained)
	{
		std::unique_lock<std::mutex> lock(m_mtxLifecycle);
		if (!m_cvLifecycle.wait_until(lock, deadline, [this]() { return m_activeCallbacks == 0; }))
			drained = false;
	}

	// 3. ydApi.h: startDestroy() is asynchronous and the listener (this object)
	//    may only be deleted after notifyAfterApiDestroy().
	YDApi* api = m_pUserAPI;
	if (drained && api != NULL)
	{
		bool start = false;
		{
			std::lock_guard<std::mutex> lock(m_mtxLifecycle);
			start = !m_destroyStarted;
			m_destroyStarted = true;
		}
		if (start)
			api->startDestroy();
		std::unique_lock<std::mutex> lock(m_mtxLifecycle);
		if (!m_cvLifecycle.wait_until(lock, deadline, [this]() { return m_apiDestroyed && m_activeCallbacks == 0; }))
			drained = false;
	}

	if (!drained)
	{
		{
			std::lock_guard<std::mutex> lock(m_mtxLifecycle);
			m_stopState = YS_DRAIN_UNCERTAIN;
		}
		std::fprintf(stderr, "[TraderYD] release: worker, calls, callbacks or notifyAfterApiDestroy did not complete within %u ms; instance kept\n",
			m_drainTimeoutMs);
		return;
	}

	// The YD API freed itself; it must never be used or deleted again.
	m_pUserAPI = NULL;

	if (m_mapOrders)
		m_mapOrders->clear();

	m_mapFrozen.clear();

	if (m_mapPosition)
		m_mapPosition->clear();

	if (m_mapTrades)
		m_mapTrades->clear();

	std::lock_guard<std::mutex> lock(m_mtxLifecycle);
	m_stopState = YS_RELEASED;
}

void TraderYD::connect()
{
	if (m_funcCreator == NULL)
	{
		write_log(m_sink, LL_ERROR, "[TraderYD] Cannot connect: YD API factory is unavailable");
		return;
	}

	{
		// Card 9c: a stopped instance is terminal and a connected one keeps its API.
		std::lock_guard<std::mutex> lock(m_mtxLifecycle);
		if (m_bStopped.load(std::memory_order_acquire))
		{
			write_log(m_sink, LL_ERROR, "[TraderYD] Cannot connect after release");
			return;
		}
		if (m_pUserAPI != NULL)
		{
			write_log(m_sink, LL_ERROR, "[TraderYD] Repeated connect rejected; the existing YD API is kept");
			return;
		}
	}

	ApiLease lease(this);
	if (!lease)
		return;

	m_pUserAPI = m_funcCreator(m_strCfgFile.c_str());
	if (m_pUserAPI == NULL)
	{
		write_log(m_sink, LL_ERROR, "[TraderYD] YD API factory returned no API instance");
		return;
	}

	m_pUserAPI->start(this);

	if (m_thrdWorker == NULL && !m_bStopped.load(std::memory_order_acquire))
	{
		// Publish the thread before it runs any handler.
		std::lock_guard<std::mutex> lock(m_mtxLifecycle);
		m_workerExited = false;
		m_thrdWorker.reset(new StdThread([this]() { workerMain(); }));
	}
}

void TraderYD::disconnect()
{
	ApiLease lease(this);
	if (!lease || m_pUserAPI == NULL)
		return;

	m_pUserAPI->disconnect();
}

bool TraderYD::makeEntrustID(char* buffer, int length)
{
	if (buffer == NULL || length == 0)
		return false;

	uint32_t orderref = m_orderRef.fetch_add(1) + 1;
	fmtutil::format_to(buffer, "{}#{:010d}", m_strUser.c_str(), orderref);
	return true;
}

void TraderYD::registerSpi(ITraderSpi *listener)
{
	m_sink = listener;
	if (m_sink)
	{
		m_bdMgr = listener->getBaseDataMgr();
	}
}

int TraderYD::login(const char* user, const char* pass, const char* /*productInfo*/)
{
	m_strUser = user;
	m_strPass = pass;

	ApiLease lease(this);
	if (!lease || m_pUserAPI == NULL)
	{
		return -1;
	}

	m_wrapperState = WS_LOGINING;
	doLogin();

	return 0;
}

int TraderYD::doLogin()
{
	if (m_pUserAPI == NULL)
	{
		return 0;
	}

	if (!m_pUserAPI->login(m_strUser.c_str(), m_strPass.c_str(), NULL, NULL))
	{
		if (m_sink)
			write_log(m_sink, LL_ERROR, "[TraderYD] Sending login request failed");
	}

	return 0;
}

int TraderYD::logout()
{
	ApiLease lease(this);
	if (!lease || m_pUserAPI == NULL)
	{
		return -1;
	}

	return 0;
}

int TraderYD::orderInsert(WTSEntrust* entrust)
{
	ApiLease lease(this);
	if (!lease || m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	const YDInstrument* pInst = m_pUserAPI->getInstrumentByID(entrust->getCode());

	thread_local static YDInputOrder req;
	memset(&req, 0, sizeof(req));

	const char* userTag = entrust->getUserTag();
	const char* entrustid = entrust->getEntrustID();
	if (StrUtil::isEmpty(userTag))
	{
		///报单引用
		req.OrderRef = m_orderRef.fetch_add(0);
	}
	else
	{
		uint32_t orderref;
		extractEntrustID(entrustid, orderref);
		///报单引用
		req.OrderRef = orderref;
	}

	if (!StrUtil::isEmpty(userTag))
	{
		m_eidCache.put(entrustid, userTag, 0);
	}

	req.Price = entrust->getPrice();
	req.Direction = wrapDirectionType(entrust->getDirection(), entrust->getOffsetType());
	req.OffsetFlag = wrapOffsetType(entrust->getOffsetType());
	req.HedgeFlag = YD_HF_Speculation;
	req.OrderVolume = (int)entrust->getVolume();
	// 使用下一个下一个报单引用。YD服务器不检查OrderRef，只是将其用于在报单和成交回报中返回
	// 用户可以自行选择OrderRef的编码方式
	// 对于非本系统本次运行产生的报单，系统返回的OrderRef一律是-1
	// YDClient产生的报单，OrderRef一律是0
	// 这个例子使用限价单
	req.OrderType = wrapPriceType(entrust->getPriceType(), entrust->getOrderFlag());
	// 说明是普通报单
	req.YDOrderFlag = YD_YOF_Normal;
	// 说明如何选择连接
	req.ConnectionSelectionType = YD_CS_Any;
	// 如果ConnectionSelectionType不是YD_CS_Any，需要指定ConnectionID，范围是0到对应的YDExchange中的ConnectionCount-1
	req.ConnectionID = 0;
	// inputOrder中的RealConnectionID和ErrorNo是在返回时由服务器填写的
	if(!m_pUserAPI->insertOrder(&req, pInst))
	{
		write_log(m_sink, LL_ERROR, "[TraderYD] Order inserting failed");
	}
	return 0;
}

int TraderYD::orderAction(WTSEntrustAction* action)
{
	ApiLease lease(this);
	if (!lease || m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
		return -1;

	uint32_t orderref;
	if (!extractEntrustID(action->getEntrustID(), orderref))
		return -1;

	const YDInstrument* pInst = m_pUserAPI->getInstrumentByID(action->getCode());
	const YDExchange* pExchg = pInst->m_pExchange;
	const YDAccount* pAccount = m_pUserAPI->getMyAccount();

	thread_local static YDCancelOrder req;
	memset(&req, 0, sizeof(req));

	req.OrderSysID = convert::to_int32(action->getOrderID());
	req.YDOrderFlag = YD_YOF_Normal;
	req.ConnectionSelectionType = YD_CS_Any;
	req.ConnectionID = 0;

	if(!m_pUserAPI->cancelOrder(&req, pExchg, pAccount))
	{
		write_log(m_sink, LL_ERROR, "[TraderYD] Sending cancel request failed");
	}

	return 0;
}

int TraderYD::queryAccount()
{
	if (m_bStopped.load(std::memory_order_acquire) || m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	{
		StdUniqueLock lock(m_mtxQuery);
		m_queQuery.push([this]() {
			if (m_sink) m_sink->onRspAccount(m_ayFunds);
		});
	}

	return 0;
}

int TraderYD::queryPositions()
{
	if (m_bStopped.load(std::memory_order_acquire) || m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	{
		StdUniqueLock lock(m_mtxQuery);
		m_queQuery.push([this]() {
			WTSArray* ayPos = WTSArray::create();

			if (m_mapPosition && m_mapPosition->size() > 0)
			{
				for (auto it = m_mapPosition->begin(); it != m_mapPosition->end(); it++)
				{
					ayPos->append(it->second, true);
				}
			}

			if (m_sink)
				m_sink->onRspPosition(ayPos);

			ayPos->release();
		});
	}

	return 0;
}

int TraderYD::queryOrders()
{
	if (m_bStopped.load(std::memory_order_acquire) || m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	{
		StdUniqueLock lock(m_mtxQuery);
		m_queQuery.push([this]() {
			std::unique_ptr<WTSArray, void(*)(WTSArray*)> ayOrders(
				WTSArray::create(), [](WTSArray* ay) { if (ay) ay->release(); });
			if(m_mapOrders)
			{
				for(auto it = m_mapOrders->begin(); it != m_mapOrders->end(); it++)
				{
					ayOrders->append(it->second, true);
				}
			}
			if (m_sink) m_sink->onRspOrders(ayOrders.get());
		});
	}

	return 0;
}

int TraderYD::queryTrades()
{
	if (m_bStopped.load(std::memory_order_acquire) || m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	{
		StdUniqueLock lock(m_mtxQuery);
		m_queQuery.push([this]() {
			std::unique_ptr<WTSArray, void(*)(WTSArray*)> ayTrades(
				WTSArray::create(), [](WTSArray* ay) { if (ay) ay->release(); });
			if (m_mapTrades)
			{
				for (auto it = m_mapTrades->begin(); it != m_mapTrades->end(); it++)
				{
					ayTrades->append(it->second, true);
				}
			}
			if (m_sink) m_sink->onRspTrades(ayTrades.get());
		});
	}

	return 0;
}

WTSOrderInfo* TraderYD::makeOrderInfo(const YDOrder* orderField, const YDInstrument* instInfo) noexcept
{
	const YDExchange* exchgInfo = instInfo->m_pExchange;

	WTSContractInfo* cInfo = m_bdMgr->getContract(instInfo->InstrumentID, exchgInfo->ExchangeID);
	if (cInfo == NULL)
		return NULL;

	WTSOrderInfo* pRet = WTSOrderInfo::create();
	pRet->setContractInfo(cInfo);
	pRet->setPrice(orderField->Price);
	pRet->setVolume(orderField->OrderVolume);
	pRet->setDirection(wrapDirectionType(orderField->Direction, orderField->OffsetFlag));
	pRet->setOffsetType(wrapOffsetType(orderField->OffsetFlag));

	pRet->setPriceType(decimal::eq(orderField->Price, 0.0) ? WPT_ANYPRICE : WPT_LIMITPRICE);
	pRet->setOrderFlag((orderField->OrderType == YD_ODT_FAK) ? WOF_FAK : ((orderField->OrderType == YD_ODT_FOK) ? WOF_FOK : WOF_NOR));
	pRet->setVolTraded(orderField->TradeVolume);
	pRet->setVolLeft(orderField->OrderVolume - orderField->TradeVolume);

	pRet->setCode(cInfo->getCode());
	pRet->setExchange(cInfo->getExchg());

	uint32_t uDate = m_lDate;
	uint32_t uTime = orderField->InsertTime;
	pRet->setOrderDate(uDate);
	pRet->setOrderTime(TimeUtils::makeTime(uDate, uTime * 1000));

	pRet->setOrderState(wrapOrderState(orderField->OrderStatus));
	if(orderField->OrderStatus == YD_OS_Rejected)
	{
		pRet->setError(orderField->ErrorNo == YD_ERROR_PossibleSelfTrade ? WOEF_SelfTrade : WOEF_Normal);
	}
	else
	{
		pRet->setError(WOEF_None);
	}

	generateEntrustID(orderField->OrderRef, pRet->getEntrustID());
	convert::to_str(pRet->getOrderID(), 64, orderField->OrderSysID);
	if(pRet->isError())
		fmtutil::format_to(pRet->getStateMsg(), "{}({})", getErrorMsg(orderField->ErrorNo), orderField->ErrorNo);

	const char* usertag = m_eidCache.get(pRet->getEntrustID());
	if (StrUtil::isEmpty(usertag))
	{
		pRet->setUserTag(pRet->getEntrustID());
	}
	else
	{
		pRet->setUserTag(usertag);

		if (!StrUtil::isEmpty(pRet->getOrderID()))
		{
			m_oidCache.put_if_none(pRet->getOrderID(), usertag, 0);
		}
	}

	return pRet;
}

WTSEntrust* TraderYD::makeEntrust(const YDInputOrder *entrustField, const YDInstrument* instInfo) noexcept
{
	WTSContractInfo* ct = m_bdMgr->getContract(instInfo->InstrumentID, instInfo->m_pExchange->ExchangeID);
	if (ct == NULL)
		return NULL;

	WTSEntrust* pRet = WTSEntrust::create(
		ct->getCode(),
		entrustField->OrderVolume,
		entrustField->Price,
		ct->getExchg());
	pRet->setContractInfo(ct);

	pRet->setDirection(wrapDirectionType(entrustField->Direction, entrustField->OffsetFlag));
	pRet->setOffsetType(wrapOffsetType(entrustField->OffsetFlag));
	
	pRet->setPriceType(decimal::eq(entrustField->Price, 0.0) ? WPT_ANYPRICE : WPT_LIMITPRICE);
	pRet->setOrderFlag((entrustField->OrderType == YD_ODT_FAK) ? WOF_FAK : ((entrustField->OrderType == YD_ODT_FOK) ? WOF_FOK : WOF_NOR));

	generateEntrustID(entrustField->OrderRef, pRet->getEntrustID());
	const char* usertag = m_eidCache.get(pRet->getEntrustID());
	if (!StrUtil::isEmpty(usertag))
		pRet->setUserTag(usertag);

	return pRet;
}

WTSError* TraderYD::makeError(int errorno, WTSErroCode ec) noexcept
{
	WTSError* pRet = WTSError::create(ec, fmtutil::format("{}({})", getErrorMsg(errorno), errorno));
	return pRet;
}

WTSTradeInfo* TraderYD::makeTradeRecord(const YDTrade *tradeField, const YDInstrument* instInfo) noexcept
{
	WTSContractInfo* cInfo = m_bdMgr->getContract(instInfo->InstrumentID, instInfo->m_pExchange->ExchangeID);
	if (cInfo == NULL)
		return NULL;

	WTSCommodityInfo* commInfo = cInfo->getCommInfo();
	[[maybe_unused]] WTSSessionInfo* sInfo = commInfo->getSessionInfo();

	WTSTradeInfo *pRet = WTSTradeInfo::create(cInfo->getCode(), commInfo->getExchg());
	pRet->setContractInfo(cInfo);
	pRet->setVolume(tradeField->Volume);
	pRet->setPrice(tradeField->Price);
	convert::to_str(pRet->getTradeID(), 64, tradeField->TradeID);

	uint32_t uDate = m_lDate;
	uint32_t uTime = tradeField->TradeTime;

	pRet->setTradeDate(uDate);
	pRet->setTradeTime(TimeUtils::makeTime(uDate, uTime * 1000));

	WTSDirectionType dType = wrapDirectionType(tradeField->Direction, tradeField->OffsetFlag);

	pRet->setDirection(dType);
	pRet->setOffsetType(wrapOffsetType(tradeField->OffsetFlag));
	convert::to_str(pRet->getRefOrder(), 64, tradeField->OrderSysID);
	pRet->setTradeType(WTT_Common);

	double amount = commInfo->getVolScale()*tradeField->Volume*pRet->getPrice();
	pRet->setAmount(amount);

	pRet->setUserTag(m_oidCache.get(pRet->getRefOrder()));

	return pRet;
}

bool TraderYD::generateEntrustID(uint32_t orderRef, char* buffer) noexcept
{
	fmtutil::format_to(buffer, "{}#{:010d}", m_strUser.c_str(), orderRef);
	return true;
}

bool TraderYD::extractEntrustID(const char* entrustid, uint32_t &orderRef) noexcept
{
	if (entrustid == NULL)
		return false;

	auto idx = StrUtil::findFirst(entrustid, '#');
	auto idx2 = StrUtil::findLast(entrustid, '#');
	if (idx == std::string::npos || idx != idx2)
		return false;

	const char* suffix = entrustid + idx + 1;
	if (*suffix == '\0')
		return false;

	uint32_t parsedOrderRef = 0;
	for (const char* p = suffix; *p != '\0'; ++p)
	{
		if (*p < '0' || *p > '9')
			return false;

		const uint32_t digit = static_cast<uint32_t>(*p - '0');
		if (parsedOrderRef > (std::numeric_limits<uint32_t>::max() - digit) / 10)
			return false;

		parsedOrderRef = parsedOrderRef * 10 + digit;
	}

	orderRef = parsedOrderRef;

	return true;
}

bool TraderYD::isConnected()
{
	return (m_wrapperState == WS_ALLREADY);
}
