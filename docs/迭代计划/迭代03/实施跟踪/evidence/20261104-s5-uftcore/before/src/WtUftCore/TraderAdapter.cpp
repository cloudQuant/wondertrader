/*!
 * \file TraderAdapter.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "TraderAdapter.h"
#include "WtHelper.h"
#include "ITrdNotifySink.h"
#include "ActionPolicyMgr.h"

#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSTradeDef.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/WTSRiskDef.hpp"
#include "../Share/Converter.hpp"

#include <atomic>
#include <mutex>

#include "../WTSTools/WTSLogger.h"
#include "../Share/TimeUtils.hpp"
#include "../Share/decimal.h"
#include "../Share/DLLHelper.hpp"
#include "../Share/StrUtil.hpp"

#include <exception>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>

namespace rj = rapidjson;
using namespace std;

namespace
{
// The usertag contents need the "wtp." prefix, adapter id, separator,
// uint32_t local order id, and a trailing NUL.
constexpr std::size_t kUserTagBufferBytes = sizeof(((WTSEntrustStruct*)nullptr)->m_strUserTag);
constexpr std::size_t kOrderTagPrefixBytes = sizeof("wtp.") - 1;
constexpr std::size_t kMaxLocalOrderIdDigits = std::numeric_limits<uint32_t>::digits10 + 1;
static_assert(kUserTagBufferBytes > kOrderTagPrefixBytes + 1 + kMaxLocalOrderIdDigits,
	"WTSEntrust usertag buffer cannot hold the order-tag suffix");
constexpr std::size_t kMaxAdapterIdBytes = kUserTagBufferBytes - 1 -
	kOrderTagPrefixBytes - 1 - kMaxLocalOrderIdDigits;

bool isValidOrderTagAdapterId(const char* id) noexcept
{
	if (id == nullptr)
		return false;

	for (std::size_t i = 0; i <= kMaxAdapterIdBytes; ++i)
	{
		if (id[i] == '\0')
			return true;
	}
	return false;
}

thread_local std::vector<const wtp::TraderAdapter*> g_sink_notification_adapters;
thread_local std::vector<const wtp::TraderAdapter*> g_api_call_adapters;
thread_local std::vector<const wtp::TraderAdapter*> g_spi_callback_adapters;

class SinkNotificationCallbackScope
{
public:
	explicit SinkNotificationCallbackScope(const wtp::TraderAdapter* adapter)
		: _adapter(adapter)
	{
		g_sink_notification_adapters.push_back(adapter);
	}

	~SinkNotificationCallbackScope()
	{
		if (!g_sink_notification_adapters.empty() && g_sink_notification_adapters.back() == _adapter)
			g_sink_notification_adapters.pop_back();
		else
		{
			auto it = std::find(g_sink_notification_adapters.rbegin(), g_sink_notification_adapters.rend(), _adapter);
			if (it != g_sink_notification_adapters.rend())
				g_sink_notification_adapters.erase(std::next(it).base());
		}
	}

private:
	const wtp::TraderAdapter* _adapter;
};

bool isInSinkNotificationCallback(const wtp::TraderAdapter* adapter)
{
	return std::find(g_sink_notification_adapters.begin(), g_sink_notification_adapters.end(), adapter) != g_sink_notification_adapters.end();
}

bool isInApiCall(const wtp::TraderAdapter* adapter)
{
	return std::find(g_api_call_adapters.begin(), g_api_call_adapters.end(), adapter) != g_api_call_adapters.end();
}

bool isInSpiCallback(const wtp::TraderAdapter* adapter)
{
	return std::find(g_spi_callback_adapters.begin(), g_spi_callback_adapters.end(), adapter) != g_spi_callback_adapters.end();
}

template<typename Cleanup>
class ScopeExit
{
public:
	explicit ScopeExit(Cleanup cleanup)
		: _cleanup(std::move(cleanup))
		, _active(true)
	{}
	~ScopeExit() noexcept
	{
		if (_active)
		{
			try { _cleanup(); }
			catch (...) {}
		}
	}
	void dismiss() { _active = false; }

private:
	Cleanup _cleanup;
	bool _active;
};

void logAdapterException(const char* id, const char* operation) noexcept
{
	try
	{
		WTSLogger::log_dyn("trader", id, LL_ERROR,
			"[{}] Trader adapter operation {} threw; lifecycle remains fail-closed", id, operation);
	}
	catch (...) {}
}

bool isValidStandardOrderCode(const char* stdCode) noexcept
{
	if (stdCode == nullptr)
		return false;

	// WTSEntrust stores the incoming full code in a fixed-size instrument field.
	std::size_t fullCodeLen = 0;
	while (fullCodeLen < MAX_INSTRUMENT_LENGTH && stdCode[fullCodeLen] != '\0')
		++fullCodeLen;
	if (fullCodeLen == MAX_INSTRUMENT_LENGTH)
		return false;

	std::size_t exchangeLen = 0;
	std::size_t fieldCount = 1;
	bool afterSeparator = false;
	for (std::size_t i = 0; i < fullCodeLen; ++i)
	{
		if (stdCode[i] != '.')
			continue;

		if (i == 0 || stdCode[i - 1] == '.' || i + 1 == fullCodeLen)
			return false;
		if (!afterSeparator)
		{
			exchangeLen = i;
			afterSeparator = true;
		}
		++fieldCount;
	}

	// This is a structural and fixed-buffer safety check only. It accepts the
	// supported two-to-four-field shape but does not validate option semantics.
	if (!afterSeparator || exchangeLen >= MAX_EXCHANGE_LENGTH || fieldCount > 4)
		return false;

	return true;
}
}

inline uint32_t makeLocalOrderID()
{
	static std::atomic<uint32_t> _auto_order_id{ 0 };
	// Local ID 0 is reserved as the no-order sentinel; keep initialization state separate.
	static std::once_flag _auto_order_id_once;
	std::call_once(_auto_order_id_once, []() {
		uint32_t curYear = TimeUtils::getCurDate() / 10000 * 10000 + 101;
		uint32_t seed = (uint32_t)((TimeUtils::getLocalTimeNow() - TimeUtils::makeTime(curYear, 0)) / 1000 * 50);
		if (seed == 0)
			seed = 1;
		_auto_order_id.store(seed, std::memory_order_relaxed);
	});

	return _auto_order_id.fetch_add(1, std::memory_order_relaxed);
}

inline const char* formatAction(WTSDirectionType dType, WTSOffsetType oType)
{
	if(dType == WDT_LONG)
	{
		if (oType == WOT_OPEN)
			return "OL";
		else if (oType == WOT_CLOSE)
			return "CL";
		else
			return "CNL";
	}
	else
	{
		if (oType == WOT_OPEN)
			return "OS";
		else if (oType == WOT_CLOSE)
			return "CS";
		else
			return "CNS";
	}
}

TraderAdapter::TraderAdapter()
	: _cfg(NULL)
	, _id("")
	, _trading_day(0)
	, _trader_api(NULL)
	, _remover(NULL)
	, _dll_handle(NULL)
	, _owns_trader_api(false)
	, _spi_registered(false)
	, _api_released(false)
	, _api_release_failed(false)
	, _plugin_delete_failed(false)
	, _state(AS_NOTLOGIN)
	, _orders(NULL)
	, _stat_map(NULL)
, _risk_mon_enabled(false)
	{
}


TraderAdapter::~TraderAdapter()
{
	requestStopSinkNotifications();
	closeSpiCallbackAdmission();
	if (!waitSinkNotificationsStopped())
		std::terminate();
	{
		std::lock_guard<std::mutex> lock(_mtx_api_lifecycle);
		// Destruction from this object's own initialization/release callback would
		// return into a caller that is still using the object.
		if ((_initialization_in_progress && _initialization_thread == std::this_thread::get_id()) ||
			(_api_release_in_progress && _api_release_thread == std::this_thread::get_id()))
			std::terminate();
	}
	release();
	if (!isReleaseComplete())
		std::terminate();
}

void TraderAdapter::addSink(ITrdNotifySink* sink)
{
	if (sink == NULL)
		return;

	ChannelStatusNotification notification;
	notification.sinks.reserve(1);
	notification.sinks.push_back(sink);
	notification.ready = true;
	bool shouldDrain = false;
	{
		std::lock_guard<std::mutex> lock(_mtx_sink_notifications);
		if (_sink_notifications_stop_requested)
			return;

		auto inserted = _sinks.insert(sink);
		if (!inserted.second || !_sink_channel_ready)
			return;

		notification.trading_day = _trading_day.load(std::memory_order_acquire);
		try
		{
			enqueueChannelStatusLocked(std::move(notification), shouldDrain);
		}
		catch (...)
		{
			_sinks.erase(sink);
			throw;
		}
	}

	if (shouldDrain)
		drainChannelStatusNotifications();
}

void TraderAdapter::notifySinks(const SinkCallback& callback)
{
	std::vector<ITrdNotifySink*> sinks;
	{
		std::lock_guard<std::mutex> lock(_mtx_sink_notifications);
		if (_sink_notifications_stop_requested || _sinks.empty())
			return;

		sinks.assign(_sinks.begin(), _sinks.end());
		++_sink_notifications_in_flight;
	}

	dispatchReservedSinks(sinks, callback);
}

void TraderAdapter::notifyChannelReadyIfExpected()
{
	bool shouldDrain = false;
	{
		std::lock_guard<std::mutex> lock(_mtx_sink_notifications);
		AdapterState expected = AS_TRADES_QRYED;
		if (!_state.compare_exchange_strong(expected, AS_ALLREADY, std::memory_order_acq_rel))
			return;

		_sink_channel_ready = true;
		if (_sink_notifications_stop_requested || _sinks.empty())
			return;

		ChannelStatusNotification notification;
		notification.sinks.assign(_sinks.begin(), _sinks.end());
		notification.ready = true;
		notification.trading_day = _trading_day.load(std::memory_order_acquire);
		enqueueChannelStatusLocked(std::move(notification), shouldDrain);
	}

	WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO, "[{}] Trading channel ready", _id.c_str());
	if (shouldDrain)
		drainChannelStatusNotifications();
}

void TraderAdapter::notifyChannelLost()
{
	bool shouldDrain = false;
	{
		std::lock_guard<std::mutex> lock(_mtx_sink_notifications);
		_sink_channel_ready = false;
		_state.store(AS_NOTLOGIN, std::memory_order_release);
		if (_sink_notifications_stop_requested || _sinks.empty())
			return;

		ChannelStatusNotification notification;
		notification.sinks.assign(_sinks.begin(), _sinks.end());
		notification.ready = false;
		notification.trading_day = 0;
		enqueueChannelStatusLocked(std::move(notification), shouldDrain);
	}

	if (shouldDrain)
		drainChannelStatusNotifications();
}

void TraderAdapter::enqueueChannelStatusLocked(ChannelStatusNotification&& notification, bool& shouldDrain)
{
	if (notification.sinks.empty())
		return;
	// Ready/lost snapshots enter this queue under the same lock as the readiness flag.
	// A single drainer preserves their order while invoking every sink without a lock.
	_sink_channel_status_queue.push_back(std::move(notification));
	++_sink_notifications_in_flight;
	if (!_sink_channel_status_draining)
	{
		_sink_channel_status_draining = true;
		shouldDrain = true;
	}
}

void TraderAdapter::drainChannelStatusNotifications()
{
	std::exception_ptr firstException;
	for (;;)
	{
		ChannelStatusNotification notification;
		{
			std::lock_guard<std::mutex> lock(_mtx_sink_notifications);
			if (_sink_channel_status_queue.empty())
			{
				_sink_channel_status_draining = false;
				_cv_sink_notifications.notify_all();
				break;
			}
			notification = std::move(_sink_channel_status_queue.front());
			_sink_channel_status_queue.pop_front();
		}

		try
		{
			SinkNotificationCallbackScope callbackScope(this);
			for (ITrdNotifySink* sink : notification.sinks)
			{
				try
				{
					if (notification.ready)
						sink->on_channel_ready(notification.trading_day);
					else
						sink->on_channel_lost();
				}
				catch (...)
				{
					if (!firstException)
						firstException = std::current_exception();
				}
			}
		}
		catch (...)
		{
			if (!firstException)
				firstException = std::current_exception();
		}
		finishSinkNotification();
	}

	if (firstException)
		std::rethrow_exception(firstException);
}

void TraderAdapter::dispatchReservedSinks(const std::vector<ITrdNotifySink*>& sinks, const SinkCallback& callback)
{
	try
	{
		SinkNotificationCallbackScope callbackScope(this);
		for (ITrdNotifySink* sink : sinks)
			callback(sink);
	}
	catch (...)
	{
		finishSinkNotification();
		throw;
	}

	finishSinkNotification();
}

void TraderAdapter::finishSinkNotification()
{
	std::lock_guard<std::mutex> lock(_mtx_sink_notifications);
	if (_sink_notifications_in_flight > 0)
		--_sink_notifications_in_flight;
	if (_sink_notifications_in_flight == 0)
	{
		if (_sink_notifications_stop_requested)
			_sinks.clear();
		_cv_sink_notifications.notify_all();
	}
}

void TraderAdapter::requestStopSinkNotifications()
{
	std::lock_guard<std::mutex> lock(_mtx_sink_notifications);
	_sink_notifications_stop_requested = true;
	_sink_channel_ready = false;
	_state.store(AS_NOTLOGIN, std::memory_order_release);
	while (!_sink_channel_status_queue.empty())
	{
		_sink_channel_status_queue.pop_front();
		if (_sink_notifications_in_flight > 0)
			--_sink_notifications_in_flight;
	}
	if (_sink_notifications_in_flight == 0)
	{
		_sinks.clear();
		_cv_sink_notifications.notify_all();
	}
}

bool TraderAdapter::waitSinkNotificationsStopped()
{
	if (isInSinkNotificationCallback() || isInApiCall() || isInSpiCallback())
		return false;

	std::unique_lock<std::mutex> lock(_mtx_sink_notifications);
	if (!_sink_notifications_stop_requested)
		return false;
	_cv_sink_notifications.wait(lock, [this]() {
		return _sink_notifications_in_flight == 0 && !_sink_channel_status_draining && _api_calls_in_progress == 0;
	});
	_sinks.clear();
	return true;
}

bool TraderAdapter::isInSinkNotificationCallback() const
{
	return ::isInSinkNotificationCallback(this);
}

bool TraderAdapter::isInApiCall() const
{
	return ::isInApiCall(this);
}

bool TraderAdapter::isInSpiCallback() const
{
	return ::isInSpiCallback(this);
}

bool TraderAdapter::beginSpiCallback()
{
	std::lock_guard<std::mutex> lock(_mtx_spi_callbacks);
	if (!_spi_callback_admission_open)
		return false;
	++_spi_callbacks_in_progress;
	return true;
}

void TraderAdapter::finishSpiCallback()
{
	std::lock_guard<std::mutex> lock(_mtx_spi_callbacks);
	if (_spi_callbacks_in_progress > 0)
		--_spi_callbacks_in_progress;
	_cv_spi_callbacks.notify_all();
}

void TraderAdapter::closeSpiCallbackAdmission()
{
	std::lock_guard<std::mutex> lock(_mtx_spi_callbacks);
	_spi_callback_admission_open = false;
	_cv_spi_callbacks.notify_all();
}

bool TraderAdapter::waitForSpiCallbacksStopped()
{
	if (isInSpiCallback())
		return false;
	std::unique_lock<std::mutex> lock(_mtx_spi_callbacks);
	_cv_spi_callbacks.wait(lock, [this]() { return _spi_callbacks_in_progress == 0; });
	return true;
}

TraderAdapter::ApiCallLease::ApiCallLease(TraderAdapter* owner, bool requireIdle, bool loginAttempt)
	: _owner(owner)
	, _active(owner && owner->beginApiCall(requireIdle, loginAttempt))
{
	if (_active)
	{
		try
		{
			g_api_call_adapters.push_back(owner);
		}
		catch (...)
		{
			_owner->finishApiCall();
			_active = false;
			throw;
		}
	}
}

TraderAdapter::ApiCallLease::~ApiCallLease()
{
	if (_active)
	{
		if (!g_api_call_adapters.empty() && g_api_call_adapters.back() == _owner)
			g_api_call_adapters.pop_back();
		else
		{
			auto it = std::find(g_api_call_adapters.rbegin(), g_api_call_adapters.rend(), _owner);
			if (it != g_api_call_adapters.rend())
				g_api_call_adapters.erase(std::next(it).base());
		}
		_owner->finishApiCall();
	}
}

TraderAdapter::SpiCallbackLease::SpiCallbackLease(TraderAdapter* owner)
	: _owner(owner)
	, _active(owner && owner->beginSpiCallback())
{
	if (_active)
	{
		try
		{
			g_spi_callback_adapters.push_back(owner);
		}
		catch (...)
		{
			_owner->finishSpiCallback();
			_active = false;
			throw;
		}
	}
}

TraderAdapter::SpiCallbackLease::~SpiCallbackLease()
{
	if (_active)
	{
		if (!g_spi_callback_adapters.empty() && g_spi_callback_adapters.back() == _owner)
			g_spi_callback_adapters.pop_back();
		else
		{
			auto it = std::find(g_spi_callback_adapters.rbegin(), g_spi_callback_adapters.rend(), _owner);
			if (it != g_spi_callback_adapters.rend())
				g_spi_callback_adapters.erase(std::next(it).base());
		}
		_owner->finishSpiCallback();
	}
}

bool TraderAdapter::beginApiCall(bool requireIdle, bool loginAttempt)
{
	std::lock_guard<std::mutex> lock(_mtx_sink_notifications);
	if (_sink_notifications_stop_requested || (requireIdle && _api_calls_in_progress != 0))
		return false;
	std::lock_guard<std::mutex> lifecycleLock(_mtx_api_lifecycle);
	if (_initialization_in_progress || _api_release_in_progress || _lifecycle_closed)
		return false;
	if (_trader_api == NULL)
		return false;
	++_api_calls_in_progress;
	if (loginAttempt)
		_state.store(AS_LOGINING, std::memory_order_release);
	return true;
}

bool TraderAdapter::canContinueApiStartup() const
{
	std::lock_guard<std::mutex> lock(_mtx_sink_notifications);
	return !_sink_notifications_stop_requested;
}

void TraderAdapter::finishApiCall()
{
	std::lock_guard<std::mutex> lock(_mtx_sink_notifications);
	if (_api_calls_in_progress > 0)
		--_api_calls_in_progress;
	_cv_sink_notifications.notify_all();
}

bool TraderAdapter::beginInitialization()
{
	std::lock_guard<std::mutex> sinkLock(_mtx_sink_notifications);
	if (_sink_notifications_stop_requested || _sink_notifications_in_flight != 0 ||
		_sink_channel_status_draining || _api_calls_in_progress != 0 || !_sink_channel_status_queue.empty())
		return false;

	std::lock_guard<std::mutex> lifecycleLock(_mtx_api_lifecycle);
	if (_lifecycle_closed || _api_release_in_progress || _initialization_in_progress ||
		_trader_api != NULL || _cfg != NULL || _dll_handle != NULL)
		return false;

	_initialization_in_progress = true;
	_initialization_thread = std::this_thread::get_id();
	_api_released = false;
	_api_release_failed = false;
	_plugin_delete_failed = false;
	_sink_channel_ready = false;
	_state.store(AS_NOTLOGIN, std::memory_order_release);
	_trading_day.store(0, std::memory_order_release);
	{
		std::lock_guard<std::mutex> spiLock(_mtx_spi_callbacks);
		_spi_callback_admission_open = true;
	}
	return true;
}

void TraderAdapter::finishInitialization()
{
	std::lock_guard<std::mutex> lock(_mtx_api_lifecycle);
	_initialization_in_progress = false;
	_initialization_thread = std::thread::id();
	_cv_api_lifecycle.notify_all();
}

bool TraderAdapter::cleanupApiAndConfig()
{
	closeSpiCallbackAdmission();
	if (isInSpiCallback())
		return false;

	if (_trader_api != NULL)
	{
		if (_spi_registered)
		{
			try
			{
				_trader_api->registerSpi(NULL);
				_spi_registered = false;
			}
			catch (...)
			{
				// Continue to API release, which may still provide the provider's stop fence.
				logAdapterException(_id.c_str(), "registerSpi(NULL)");
			}
		}

		if (!_api_released)
		{
			if (_api_release_failed)
			{
				logAdapterException(_id.c_str(), "ITraderApi::release previously failed");
				return false;
			}
			try
			{
				// Admission is already closed. Drain admitted callbacks after provider
				// release returns; calls made later through the raw SPI pointer remain
				// outside this adapter-side barrier.
				_trader_api->release();
			}
			catch (...)
			{
				_api_release_failed = true;
				logAdapterException(_id.c_str(), "ITraderApi::release");
				return false;
			}
			_api_released = true;
			_spi_registered = false;
		}
	}

	// Provider release may join callbacks already in progress. Wait only after that
	// stop request has returned, and before deleting the API or unloading its module.
	if (!waitForSpiCallbacksStopped())
		return false;

	if (_trader_api != NULL && _owns_trader_api)
	{
		if (_plugin_delete_failed || _remover == NULL)
		{
			logAdapterException(_id.c_str(), "deleteTrader unavailable or previously failed");
			return false;
		}
		ITraderApi* api = _trader_api;
		try
		{
			_remover(api);
		}
		catch (...)
		{
			// The deleter may have partially destroyed the object. Never call it twice
			// or unload the module/config that object may still reference.
			_plugin_delete_failed = true;
			logAdapterException(_id.c_str(), "deleteTrader");
			return false;
		}
	}
	_trader_api = NULL;

	_owns_trader_api = false;
	_spi_registered = false;
	if (_dll_handle != NULL)
	{
		DLLHelper::free_library(_dll_handle);
		_dll_handle = NULL;
	}
	_remover = NULL;

	// Keep the retained configuration alive until the API has stopped and the
	// plugin-owned API object has been destroyed.
	if (_cfg != NULL)
	{
		_cfg->release();
		_cfg = NULL;
	}

	_api_released = true;
	_state.store(AS_NOTLOGIN, std::memory_order_release);
	_trading_day.store(0, std::memory_order_release);
	_bd_mgr = NULL;
	_policy_mgr = NULL;
	_risk_params_map.clear();
	_risk_mon_enabled = false;
	return true;
}

void TraderAdapter::rollbackInitialization()
{
	bool cleanupComplete = false;
	try
	{
		cleanupComplete = cleanupApiAndConfig();
	}
	catch (...)
	{
		logAdapterException(_id.c_str(), "initialization rollback");
	}
	if (!cleanupComplete)
	{
		std::lock_guard<std::mutex> lock(_mtx_api_lifecycle);
		_lifecycle_closed = true;
		return;
	}
	_id.clear();
	_order_pattern.clear();
}

bool TraderAdapter::isReleaseComplete() const
{
	std::lock_guard<std::mutex> lock(_mtx_api_lifecycle);
	if (!_lifecycle_closed || _api_release_in_progress ||
		_trader_api != NULL || _dll_handle != NULL || _cfg != NULL)
		return false;
	std::lock_guard<std::mutex> spiLock(_mtx_spi_callbacks);
	return !_spi_callback_admission_open && _spi_callbacks_in_progress == 0;
}

bool TraderAdapter::initExt(const char* id, ITraderApi* api, IBaseDataMgr* bdMgr, ActionPolicyMgr* policyMgr)
{
	if (!isValidOrderTagAdapterId(id) || api == NULL || !beginInitialization())
		return false;
	ScopeExit rollback([this]() {
		rollbackInitialization();
		finishInitialization();
	});

	_policy_mgr = policyMgr;
	_bd_mgr = bdMgr;
	_id = id;
	_order_pattern = fmtutil::format<64>("wtp.{}", id);
	_trader_api = api;
	_owns_trader_api = false;
	_api_released = false;

	bool initSucceeded = false;
	try
	{
		initSucceeded = _trader_api->init(NULL);
	}
	catch (...)
	{
		logAdapterException(_id.c_str(), "ITraderApi::init (external API)");
		return false;
	}
	if (!initSucceeded)
		return false;

	rollback.dismiss();
	finishInitialization();
	return true;
}

bool TraderAdapter::init(const char* id, WTSVariant* params, IBaseDataMgr* bdMgr, ActionPolicyMgr* policyMgr)
{
	if (!isValidOrderTagAdapterId(id) || params == NULL || !beginInitialization())
		return false;
	ScopeExit rollback([this]() {
		rollbackInitialization();
		finishInitialization();
	});

	_policy_mgr = policyMgr;
	_bd_mgr = bdMgr;
	_id = id;
	_order_pattern = fmtutil::format<64>("wtp.{}", id);
	_cfg = params;
	_cfg->retain();

	//这里解析流量风控参数
	WTSVariant* cfgRisk = params->get("riskmon");
	if (cfgRisk)
	{
		if (cfgRisk->getBoolean("active"))
		{
			_risk_mon_enabled = true;
			WTSVariant* cfgPolicy = cfgRisk->get("policy");
			if (cfgPolicy == NULL || cfgPolicy->type() != WTSVariant::VT_Object)
			{
				WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR, "[{}] Active risk control requires a policy object", _id.c_str());
				return false;
			}

			auto keys = cfgPolicy->memberNames();
			for (auto it = keys.begin(); it != keys.end(); it++)
			{
				const char* product = (*it).c_str();
				WTSVariant*	vProdItem = cfgPolicy->get(product);
				RiskParams& rParam = _risk_params_map[product];
				rParam._cancel_total_limits = vProdItem->getUInt32("cancel_total_limits");
				rParam._cancel_times_boundary = vProdItem->getUInt32("cancel_times_boundary");
				rParam._cancel_stat_timespan = vProdItem->getUInt32("cancel_stat_timespan");

				rParam._order_total_limits = vProdItem->getUInt32("order_total_limits");
				rParam._order_times_boundary = vProdItem->getUInt32("order_times_boundary");
				rParam._order_stat_timespan = vProdItem->getUInt32("order_stat_timespan");

				WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO, "[{}] Risk control rule {} of trading channel loaded", _id.c_str(), product);
			}

			auto it = _risk_params_map.find("default");
			if (it == _risk_params_map.end())
			{
				WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN, "[{}] Some instruments may not be monitored due to no default risk control rule of trading channel", _id.c_str());
			}
		}
		else
		{
			WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN, "[{}] Risk control rule of trading channel not activated", _id.c_str());
		}
	}
	else
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN, "[{}] No risk control rule setup of trading channel", _id.c_str());
	}

	if (params->getString("module").empty())
		return false;

	std::string module = DLLHelper::wrap_module(params->getCString("module"), "lib");
	std::string dllpath = WtHelper::getModulePath(module.c_str(), "traders", true);
	if (!StdFile::exists(dllpath.c_str()))
		dllpath = WtHelper::getModulePath(module.c_str(), "traders", false);
	_dll_handle = DLLHelper::load_library(dllpath.c_str());
	if (_dll_handle == NULL)
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR, "[{}] Loading trading module {} failed", _id.c_str(), dllpath.c_str());
		return false;
	}

	WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO, "[{}] Trader module {} loaded", _id.c_str(), dllpath.c_str());
	FuncCreateTrader createTrader = (FuncCreateTrader)DLLHelper::get_symbol(_dll_handle, "createTrader");
	_remover = (FuncDeleteTrader)DLLHelper::get_symbol(_dll_handle, "deleteTrader");
	if (createTrader == NULL || _remover == NULL)
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_FATAL, "[{}] Trader module must export createTrader and deleteTrader", _id.c_str());
		return false;
	}

	try
	{
		_trader_api = createTrader();
	}
	catch (...)
	{
		logAdapterException(_id.c_str(), "createTrader");
		return false;
	}
	if (_trader_api == NULL)
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_FATAL, "[{}] Creating trading api failed", _id.c_str());
		return false;
	}
	_owns_trader_api = true;
	_api_released = false;

	bool initSucceeded = false;
	try
	{
		initSucceeded = _trader_api->init(params);
	}
	catch (...)
	{
		logAdapterException(_id.c_str(), "ITraderApi::init");
		return false;
	}
	if (!initSucceeded)
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR, "[{}] Trader API initialization failed", _id.c_str());
		return false;
	}

	rollback.dismiss();
	finishInitialization();
	return true;
}

bool TraderAdapter::run()
{
	ApiCallLease startCall(this, true, true);
	if (!startCall)
		return false;

	if (_stat_map == NULL)
		_stat_map = TradeStatMap::create();

	_trader_api->registerSpi(this);
	_spi_registered = true;
	if (!canContinueApiStartup())
		return false;
	_trader_api->connect();
	return true;
}

void TraderAdapter::release()
{
	requestStopSinkNotifications();
	closeSpiCallbackAdmission();
	if (!waitSinkNotificationsStopped())
		return;

	{
		std::unique_lock<std::mutex> lock(_mtx_api_lifecycle);
		if (_initialization_in_progress && _initialization_thread == std::this_thread::get_id())
			return;
		_cv_api_lifecycle.wait(lock, [this]() { return !_initialization_in_progress; });
		if (_api_release_in_progress)
		{
			if (_api_release_thread == std::this_thread::get_id())
				return;
			_cv_api_lifecycle.wait(lock, [this]() { return !_api_release_in_progress; });
			return;
		}
		if (_lifecycle_closed && _trader_api == NULL && _dll_handle == NULL && _cfg == NULL)
			return;

		_lifecycle_closed = true;
		_api_release_in_progress = true;
		_api_release_thread = std::this_thread::get_id();
	}

	ScopeExit releaseComplete([this]() {
		std::lock_guard<std::mutex> lock(_mtx_api_lifecycle);
		_api_release_in_progress = false;
		_api_release_thread = std::thread::id();
		_cv_api_lifecycle.notify_all();
	});

	bool cleanupComplete = false;
	try
	{
		cleanupComplete = cleanupApiAndConfig();
	}
	catch (...)
	{
		logAdapterException(_id.c_str(), "adapter resource cleanup");
	}
	if (!cleanupComplete)
		return;
	if (_stat_map)
	{
		_stat_map->release();
		_stat_map = NULL;
	}
	// SPI callbacks have been drained by cleanupApiAndConfig(); the map owns one
	// reference per stored order, while getOrders() snapshots retain their own.
	{
		SpinLock lock(_mtx_orders);
		if (_orders)
		{
			_orders->release();
			_orders = NULL;
		}
	}
}

double TraderAdapter::enumPosition(const char* stdCode /* = "" */)
{
	/*
	 *	By Wesley @ 2022.03.19
	 *	这里改成回调的方式
	 *	不然接口会设计得很复杂
	 */
	double ret = 0;
	bool bAll = (strlen(stdCode) == 0);
	for (auto it = _positions.begin(); it != _positions.end(); it++)
	{
		if (!bAll && strcmp(it->first.c_str(), stdCode) != 0)
			continue;

		const PosItem& pItem = it->second;
		notifySinks([&](ITrdNotifySink* sink) {
			sink->on_position(stdCode, true, pItem.l_prevol, pItem.l_preavail, pItem.l_newvol, pItem.l_newavail, _trading_day);
			sink->on_position(stdCode, false, pItem.s_prevol, pItem.s_preavail, pItem.s_newvol, pItem.s_newavail, _trading_day);
		});
		ret += pItem.total_pos(true) + pItem.total_pos(false);
	}

	return ret;
}

double TraderAdapter::getPosition(const char* stdCode, bool bValidOnly, int32_t flag /* = 3 */)
{
	auto it = _positions.find(stdCode);
	if (it == _positions.end())
		return 0;

	double ret = 0;
	const PosItem& pItem = it->second;
	if(flag & 1)
	{
		if(bValidOnly)
			ret += (pItem.l_newavail + pItem.l_preavail);
		else
			ret += (pItem.l_newvol + pItem.l_prevol);
	}

	if (flag & 2)
	{
		if (bValidOnly)
			ret -= (pItem.s_newavail + pItem.s_preavail);
		else
			ret -= pItem.s_newvol + pItem.s_prevol;
	}
	return ret;
}

OrderMap* TraderAdapter::getOrders(const char* stdCode)
{
	ApiCallLease apiCall(this);
	if (!apiCall)
		return NULL;

	bool isAll = strlen(stdCode) == 0;

	SpinLock lock(_mtx_orders);
	if (_orders == NULL)
		return NULL;

	OrderMap* ret = OrderMap::create();
	for (auto it = _orders->begin(); it != _orders->end(); it++)
	{
		uint32_t localid = it->first;
		WTSOrderInfo* ordInfo = (WTSOrderInfo*)it->second;

		bool matchesCode = isAll || strcmp(ordInfo->getCode(), stdCode) == 0;
		if (!matchesCode && !isAll)
		{
			WTSContractInfo* cInfo = ordInfo->getContractInfo();
			if (cInfo == NULL)
				cInfo = _bd_mgr->getContract(ordInfo->getCode(), ordInfo->getExchg());

			matchesCode = cInfo != NULL && strcmp(cInfo->getFullCode(), stdCode) == 0;
		}

		if (matchesCode)
			ret->add(localid, ordInfo);
	}
	return ret;
}

void TraderAdapter::updateUndone(const char* stdCode, double qty)
{
	std::lock_guard<std::mutex> lock(_mtx_undone_qty);
	double& undone = _undone_qty[stdCode];
	[[maybe_unused]] double oldQty = undone;
	undone += qty;
}

bool TraderAdapter::rollbackUndoneIfNonzero(const char* stdCode, double qty)
{
	std::lock_guard<std::mutex> lock(_mtx_undone_qty);
	auto it = _undone_qty.find(stdCode);
	if (it == _undone_qty.end() || decimal::eq(it->second, 0))
		return false;

	it->second -= qty;
	return true;
}

uint32_t TraderAdapter::doEntrust(WTSEntrust* entrust)
{
	const char* codeInput = entrust == nullptr ? nullptr : entrust->getCode();
	if (!isValidStandardOrderCode(codeInput))
		return UINT_MAX;

	const std::string stdCode(codeInput);
	const std::size_t pos = StrUtil::findFirst(stdCode.c_str(), '.');
	if (pos == std::string::npos)
		return UINT_MAX;

	ApiCallLease apiCall(this);
	if (!apiCall)
		return UINT_MAX;

	_trader_api->makeEntrustID(entrust->getEntrustID(), 64);

	entrust->setExchange(stdCode.c_str(), pos);
	const std::string code = stdCode.substr(pos + 1);
	entrust->setCode(code.c_str());
	//if(entrust->getContractInfo() == NULL)
	//{
	//	WTSContractInfo* cInfo = _bd_mgr->getContract(entrust->getCode(), entrust->getExchg());
	//	entrust->setContractInfo(cInfo);
	//}

	uint32_t localid = makeLocalOrderID();
	char* usertag = entrust->getUserTag();
	wt_strcpy(usertag, _order_pattern.c_str(), _order_pattern.size());
	usertag[_order_pattern.size()] = '.';
	fmtutil::format_to(usertag + _order_pattern.size() + 1, "{}", localid);

	if (_risk_mon_enabled && !checkOrderLimits(stdCode.c_str()))
	{
		rollbackUndoneIfNonzero(stdCode.c_str(), entrust->getVolume());
		return UINT_MAX;
	}
	
	int32_t ret = _trader_api->orderInsert(entrust);
	if(ret < 0)
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR, "[{}] Order placing failed: {}", _id, ret);
		return UINT_MAX;
	}
	if (_risk_mon_enabled)
	{
		const RiskParams* riskPara = getRiskParams(stdCode.c_str());
		if (riskPara != NULL && riskPara->_order_times_boundary != 0)
			_order_time_cache[stdCode].emplace_back(TimeUtils::getLocalTimeNow());
	}
	return localid;
}

WTSContractInfo* TraderAdapter::getContract(const char* stdCode)
{
	char buf[64] = { 0 };
	wt_strcpy(buf, stdCode);
	auto idx = StrUtil::findFirst(buf, '.');
	buf[idx] = '\0';
	return _bd_mgr->getContract(buf + idx + 1, buf);
}

bool TraderAdapter::doCancel(WTSOrderInfo* ordInfo)
{
	if (ordInfo == NULL || !ordInfo->isAlive())
		return false;

	WTSContractInfo* cInfo = ordInfo->getContractInfo();
	if(cInfo == NULL)
	{
		if (_bd_mgr == NULL)
		{
			WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN,
				"[{}] Cannot cancel order {}: base data manager is not available",
				_id.c_str(), ordInfo->getOrderID());
			return false;
		}
		cInfo = _bd_mgr->getContract(ordInfo->getCode(), ordInfo->getExchg());
		if (cInfo == NULL)
		{
			WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN,
				"[{}] Cannot cancel order {}: contract {}.{} is not present in base data",
				_id.c_str(), ordInfo->getOrderID(), ordInfo->getExchg(), ordInfo->getCode());
			return false;
		}
	}
	const std::string stdCode = cInfo->getFullCode();

	if (_risk_mon_enabled && !checkCancelLimits(stdCode.c_str()))
		return false;

	WTSEntrustAction* action = WTSEntrustAction::create(ordInfo->getCode(), cInfo->getExchg());
	action->setEntrustID(ordInfo->getEntrustID());
	action->setOrderID(ordInfo->getOrderID());
	ApiCallLease apiCall(this);
	if (!apiCall)
	{
		action->release();
		return false;
	}
	int ret = _trader_api->orderAction(action);
	bool isSent = (ret >= 0);
	action->release();
	if (isSent && _risk_mon_enabled)
	{
		const RiskParams* riskPara = getRiskParams(stdCode.c_str());
		if (riskPara != NULL && riskPara->_cancel_times_boundary != 0)
			_cancel_time_cache[stdCode].emplace_back(TimeUtils::getLocalTimeNow());
	}
	return isSent;
}

bool TraderAdapter::cancel(uint32_t localid)
{
	ApiCallLease apiCall(this);
	if (!apiCall)
		return false;

	WTSOrderInfo* ordInfo = NULL;
	{
		SpinLock lock(_mtx_orders);
		if (_orders == NULL || _orders->size() == 0)
			return false;
		ordInfo = (WTSOrderInfo*)_orders->grab(localid);
		if (ordInfo == NULL)
			return false;
	}
	
	bool bRet = doCancel(ordInfo);

	ordInfo->release();

	return bRet;
}

OrderIDs TraderAdapter::cancelAll(const char* stdCode)
{
	OrderIDs ret;
	ApiCallLease apiCall(this);
	if (!apiCall)
		return ret;

	[[maybe_unused]] double actQty = 0;
	bool isAll = strlen(stdCode) == 0;
	std::vector<std::pair<uint32_t, WTSOrderInfo*>> aliveOrders;
	{
		SpinLock lock(_mtx_orders);
		if (_orders != NULL && _orders->size() > 0)
		{
			aliveOrders.reserve(_orders->size());
			for (auto it = _orders->begin(); it != _orders->end(); it++)
			{
				WTSOrderInfo* orderInfo = (WTSOrderInfo*)it->second;
				if (!orderInfo->isAlive())
					continue;
				orderInfo->retain();
				aliveOrders.emplace_back(it->first, orderInfo);
			}
		}
	}
	ScopeExit releaseOrders([&aliveOrders]() {
		for (const auto& order : aliveOrders)
			order.second->release();
	});

	for (const auto& order : aliveOrders)
	{
		WTSOrderInfo* orderInfo = order.second;
		WTSContractInfo* cInfo = orderInfo->getContractInfo();
		if (cInfo == NULL && _bd_mgr != NULL)
			cInfo = _bd_mgr->getContract(orderInfo->getCode(), orderInfo->getExchg());

		if (cInfo == NULL)
		{
			WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN,
				"[{}] Cannot cancel order {}: contract {}.{} is not present in base data",
				_id.c_str(), orderInfo->getOrderID(), orderInfo->getExchg(), orderInfo->getCode());
			continue;
		}

		if (isAll || strcmp(stdCode, cInfo->getFullCode()) == 0)
		{
			if (doCancel(orderInfo))
				ret.emplace_back(order.first);
		}
	}

	return ret;
}

uint32_t TraderAdapter::getInfos(const char* stdCode)
{
	WTSTradeStateInfo* statInfo = (WTSTradeStateInfo*)_stat_map->get(stdCode);
	if (statInfo == NULL)
		return 0;

	return statInfo->infos();
}

OrderIDs TraderAdapter::buy(const char* stdCode, double price, double qty, int flag, bool bForceClose, WTSContractInfo* cInfo /* = NULL */)
{
	OrderIDs ret;
	if (!isValidStandardOrderCode(stdCode))
		return ret;
	if (qty == 0)
		return ret;

	//if(_risk_mon_enabled && !checkOrderLimits(stdCode))
	//{
	//	WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN, "{} is forbidden to trade", stdCode);
	//	return ret;
	//}

	if (cInfo == NULL) cInfo = getContract(stdCode);
	if (cInfo == NULL)
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN,
			"[{}] Cannot buy {}: contract is not present in base data", _id.c_str(), stdCode);
		return ret;
	}
	WTSCommodityInfo* commInfo = cInfo->getCommInfo();

	WTSLogger::log_dyn("trader", _id.c_str(), LL_DEBUG, "[{}] Buying {} of quantity {}", _id.c_str(), stdCode, qty);

	const PosItem& pItem = _positions[stdCode];
	WTSTradeStateInfo* statInfo = (WTSTradeStateInfo*)_stat_map->get(stdCode);
	if (statInfo == NULL)
	{
		statInfo = WTSTradeStateInfo::create(stdCode);
		_stat_map->add(stdCode, statInfo, false);
	}
	TradeStatInfo& statItem = statInfo->statInfo();
	const ActionRuleGroup& ruleGP = _policy_mgr->getActionRules(commInfo->getFullPid());

	double left = qty;

	double unitQty = (price == 0.0) ? cInfo->getMaxMktVol() : cInfo->getMaxLmtVol();
	if (decimal::eq(unitQty, 0))
		unitQty = DBL_MAX;

	for (auto it = ruleGP.begin(); it != ruleGP.end(); it++)
	{
		const ActionRule& curRule = (*it);
		if (curRule._atype == AT_Open && !bForceClose)
		{
			//先检查是否已经到了限额
			//买入开仓, 即开多仓
			double maxQty = left;

			if (curRule._limit_l != 0)
			{
				if (statItem.l_openvol >= curRule._limit_l)
				{
					WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN, "[{}] {} long position opened today is up to limit {}", _id.c_str(), stdCode, curRule._limit_l);
					continue;
				}
				else
				{
					maxQty = min(maxQty, curRule._limit_l - statItem.l_openvol);
				}
			}

			if (curRule._limit != 0)
			{
				if (statItem.l_openvol + statItem.s_openvol >= curRule._limit)
				{
					WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN, "[{}] {} position opened today is up to limit {}", _id.c_str(), stdCode, curRule._limit);
					continue;
				}
				else
				{
					maxQty = min(maxQty, curRule._limit - statItem.l_openvol - statItem.s_openvol);
				}
			}

			//这里还要考虑单笔最大委托数量
			double leftQty = maxQty;
			for (;;)
			{
				double curQty = min(leftQty, unitQty);
				uint32_t localid = openLong(stdCode, price, curQty, flag);
				ret.emplace_back(localid);

				leftQty -= curQty;

				if (decimal::eq(leftQty, 0))
					break;
			}

			left -= maxQty;

			WTSLogger::log_dyn("trader", _id.c_str(), LL_DEBUG,
				"[{}] Signal of buying {} of quantity {} triggered: Opening long of quantity {}", _id.c_str(), stdCode, qty, maxQty);
		}
		else if (curRule._atype == AT_CloseToday)
		{
			double maxQty = 0;
			//如果要区分平昨平今的品种, 则只读取可平今仓即可
			//如果不区分平昨平今的品种, 则读取全部可平, 因为读取可拼今仓也没意义
			if (commInfo->getCoverMode() == CM_CoverToday)
				maxQty = min(left, pItem.s_newavail);	//先看看可平今仓
			else
				maxQty = min(left, pItem.avail_pos(false));


			//如果要检查净今仓，但是昨仓不为0，则跳过该条规则
			if (!bForceClose && curRule._pure && !decimal::eq(pItem.s_prevol, 0.0))
			{
				WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN,
					"[{}] Closing new short position of {} skipped because of non-zero pre short position", _id.c_str(), stdCode);
				continue;
			}

			//这里还要考虑单笔最大委托数量
			//if (maxQty > 0)
			if (decimal::gt(maxQty, 0))
			{
				double leftQty = maxQty;
				for (;;)
				{
					double curQty = min(leftQty, unitQty);
					uint32_t localid = closeShort(stdCode, price, curQty, (commInfo->getCoverMode() == CM_CoverToday), flag);//如果不支持平今, 则直接下平仓标记即可
					ret.emplace_back(localid);

					leftQty -= curQty;

					//if (leftQty == 0)
					if (decimal::eq(leftQty, 0))
						break;
				}
				left -= maxQty;

				if (commInfo->getCoverMode() == CM_CoverToday)
				{
					WTSLogger::log_dyn("trader", _id.c_str(), LL_DEBUG,
						"[{}] Signal of buying {} of quantity {} triggered: Closing new short of quantity {}", _id.c_str(), stdCode, qty, maxQty);
				}
				else
				{
					WTSLogger::log_dyn("trader", _id.c_str(), LL_DEBUG,
						"[{}] Signal of buying {} of quantity {} triggered: Closing short of quantity {}", _id.c_str(), stdCode, qty, maxQty);
				}
			}

		}
		else if (curRule._atype == AT_CloseYestoday)
		{
			//平昨比较简单, 因为不需要区分标记
			double maxQty = min(left, pItem.s_preavail);

			//如果要检查净昨仓，但是今仓不为0，则跳过该条规则
			if (!bForceClose && curRule._pure && !decimal::eq(pItem.s_newvol, 0.0))
			{
				WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN,
					"[{}] Closing pre short position of {} skipped because of non-zero new short position", _id.c_str(), stdCode);
				continue;
			}

			//这里还要考虑单笔最大委托数量
			//if (maxQty > 0)
			if (decimal::gt(maxQty, 0))
			{
				double leftQty = maxQty;
				for (;;)
				{
					double curQty = min(leftQty, unitQty);
					uint32_t localid = closeShort(stdCode, price, curQty, false, flag);//如果不支持平今, 则直接下平仓标记即可
					ret.emplace_back(localid);

					leftQty -= curQty;

					//if (leftQty == 0)
					if (decimal::eq(leftQty, 0))
						break;
				}

				left -= maxQty;

				if (commInfo->getCoverMode() == CM_CoverToday)
				{
					WTSLogger::log_dyn("trader", _id.c_str(), LL_DEBUG,
						"[{}] Signal of buying {} of quantity {} triggered: Closing old short of quantity {}", _id.c_str(), stdCode, qty, maxQty);
				}
				else
				{
					WTSLogger::log_dyn("trader", _id.c_str(), LL_DEBUG,
						"[{}] Signal of buying {} of quantity {} triggered: Closing short of quantity {}", _id.c_str(), stdCode, qty, maxQty);
				}
			}
		}
		else if (curRule._atype == AT_Close)
		{
			//如果只是平仓, 则分情况处理
			//如果区分平昨平今, 则要先平昨再平今
			//如果不区分平昨平今, 则统一平仓
			if (commInfo->getCoverMode() != CM_CoverToday)
			{
				double maxQty = min(pItem.avail_pos(false), left);

				//if (maxQty > 0)
				if (decimal::gt(maxQty, 0))
				{
					double leftQty = maxQty;
					for (;;)
					{
						double curQty = min(leftQty, unitQty);
						uint32_t localid = closeShort(stdCode, price, curQty, false, flag);
						ret.emplace_back(localid);

						leftQty -= curQty;

						//if (leftQty == 0)
						if (decimal::eq(leftQty, 0))
							break;
					}
					left -= maxQty;

					WTSLogger::log_dyn("trader", _id.c_str(), LL_DEBUG,
						"[{}] Signal of buying {} of quantity {} triggered: Closing short of quantity {}", _id.c_str(), stdCode, qty, maxQty);
				}
			}
			else
			{
				//if (pItem.s_preavail > 0)
				if (decimal::gt(pItem.s_preavail, 0))
				{
					//先将可平昨仓平仓
					double maxQty = min(pItem.s_preavail, qty);
					double leftQty = maxQty;
					for (;;)
					{
						double curQty = min(leftQty, unitQty);
						uint32_t localid = closeShort(stdCode, price, curQty, false, flag);
						ret.emplace_back(localid);

						leftQty -= curQty;

						//if (leftQty == 0)
						if (decimal::eq(leftQty, 0))
							break;
					}
					left -= maxQty;

					WTSLogger::log_dyn("trader", _id.c_str(), LL_DEBUG,
						"[{}] Signal of buying {} of quantity {} triggered: Closing old short of quantity {}", _id.c_str(), stdCode, qty, maxQty);
				}

				//if (left > 0 && pItem.s_newavail > 0)
				if (decimal::gt(left, 0) && decimal::gt(pItem.s_newavail, 0))
				{
					//再将可平今仓平仓
					//TODO: 这里还有一个控制, 就是强制锁今仓的话, 这段逻辑就跳过去了
					double maxQty = min(pItem.s_newavail, left);
					double leftQty = maxQty;
					for (;;)
					{
						double curQty = min(leftQty, unitQty);
						uint32_t localid = closeShort(stdCode, price, curQty, true, flag);
						ret.emplace_back(localid);

						leftQty -= curQty;

						//if (leftQty == 0)
						if (decimal::eq(leftQty, 0))
							break;
					}
					left -= maxQty;

					WTSLogger::log_dyn("trader", _id.c_str(), LL_DEBUG,
						"[{}] Signal of buying {} of quantity {} triggered: Closing new short of quantity {}", _id.c_str(), stdCode, qty, maxQty);
				}
			}
		}

		if (left == 0)
			break;
	}

	if (left > 0)
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR,
			"[{}] Signal of buying {} of quantity {} left quantity of {} not triggered", _id.c_str(), stdCode, qty, left);
	}

	return ret;
}

OrderIDs TraderAdapter::sell(const char* stdCode, double price, double qty, int flag, bool bForceClose, WTSContractInfo* cInfo /* = NULL */)
{
	OrderIDs ret;
	if (!isValidStandardOrderCode(stdCode))
		return ret;
	if (qty == 0)
		return ret;

	//if (_risk_mon_enabled && !checkOrderLimits(stdCode))
	//{
	//	WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN, "{} is forbidden to trade", stdCode);
	//	return ret;
	//}

	if (cInfo == NULL) cInfo = getContract(stdCode);
	if (cInfo == NULL)
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN,
			"[{}] Cannot sell {}: contract is not present in base data", _id.c_str(), stdCode);
		return ret;
	}
	WTSCommodityInfo* commInfo = cInfo->getCommInfo();

	const PosItem& pItem = _positions[stdCode];
	WTSTradeStateInfo* statInfo = (WTSTradeStateInfo*)_stat_map->get(stdCode);
	if (statInfo == NULL)
	{
		statInfo = WTSTradeStateInfo::create(stdCode);
		_stat_map->add(stdCode, statInfo, false);
	}
	TradeStatInfo& statItem = statInfo->statInfo();

	const ActionRuleGroup& ruleGP = _policy_mgr->getActionRules(commInfo->getFullPid());

	double left = qty;

	double unitQty = (price == 0.0) ? cInfo->getMaxMktVol() : cInfo->getMaxLmtVol();
	if (decimal::eq(unitQty, 0))
		unitQty = DBL_MAX;

	for (auto it = ruleGP.begin(); it != ruleGP.end(); it++)
	{
		const ActionRule& curRule = (*it);
		if (curRule._atype == AT_Open && !bForceClose)
		{
			//先检查是否已经到了限额
			//买入开仓, 即开多仓
			double maxQty = left;

			if (curRule._limit_s != 0)
			{
				if (statItem.s_openvol >= curRule._limit_s)
				{
					WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN, "[{}] {} short position opened today is up to limit {}", _id.c_str(), stdCode, curRule._limit_l);
					continue;
				}
				else
				{
					maxQty = min(maxQty, curRule._limit_s - statItem.s_openvol);
				}
			}

			if (curRule._limit != 0)
			{
				if (statItem.l_openvol + statItem.s_openvol >= curRule._limit)
				{
					WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN, "[{}] {} position opened today is up to limit {}", _id.c_str(), stdCode, curRule._limit);
					continue;
				}
				else
				{
					maxQty = min(maxQty, curRule._limit - statItem.l_openvol - statItem.s_openvol);
				}
			}

			//这里还要考虑单笔最大委托数量
			double leftQty = maxQty;
			for (;;)
			{
				double curQty = min(leftQty, unitQty);
				uint32_t localid = openShort(stdCode, price, curQty, flag);
				ret.emplace_back(localid);

				leftQty -= curQty;

				if (decimal::eq(leftQty, 0))
					break;
			}

			left -= maxQty;

			WTSLogger::log_dyn("trader", _id.c_str(), LL_DEBUG,
				"[{}] Signal of selling {} of quantity {} triggered: Opening short of quantity {}", _id.c_str(), stdCode, qty, maxQty);
		}
		else if (curRule._atype == AT_CloseToday)
		{
			double maxQty = 0;
			//如果要区分平昨平今的品种, 则只读取可平今仓即可
			//如果不区分平昨平今的品种, 则读取全部可平, 因为读取可平今仓也没意义
			if (commInfo->getCoverMode() == CM_CoverToday)
				maxQty = min(left, pItem.l_newavail);	//先看看可平今仓
			else
				maxQty = min(left, pItem.avail_pos(true));

			//如果要检查净今仓，但是昨仓不为0，则跳过该条规则
			if (!bForceClose && curRule._pure && !decimal::eq(pItem.l_prevol, 0.0))
			{
				WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN,
					"[{}] Closing new long position of {} skipped because of non-zero pre long position", _id.c_str(), stdCode);
				continue;
			}

			//这里还要考虑单笔最大委托数量
			if (decimal::gt(maxQty, 0))
			{
				double leftQty = maxQty;
				for (;;)
				{
					double curQty = min(leftQty, unitQty);
					uint32_t localid = closeLong(stdCode, price, curQty, (commInfo->getCoverMode() == CM_CoverToday), flag);//如果不支持平今, 则直接下平仓标记即可
					ret.emplace_back(localid);

					leftQty -= curQty;

					if (decimal::eq(leftQty, 0))
						break;
				}
				left -= maxQty;

				if (commInfo->getCoverMode() == CM_CoverToday)
				{
					WTSLogger::log_dyn("trader", _id.c_str(), LL_DEBUG,
						"[{}] Signal of selling {} of quantity {} triggered: Closing new long of quantity {}", _id.c_str(), stdCode, qty, maxQty);
				}
				else
				{
					WTSLogger::log_dyn("trader", _id.c_str(), LL_DEBUG,
						"[{}] Signal of selling {} of quantity {} triggered: Closing long of quantity {}", _id.c_str(), stdCode, qty, maxQty);
				}
			}

		}
		else if (curRule._atype == AT_CloseYestoday)
		{
			//平昨比较简单, 因为不需要区分标记
			double maxQty = min(left, pItem.l_preavail);

			//如果要检查净昨仓，但是今仓不为0，则跳过该条规则
			if (!bForceClose && curRule._pure && !decimal::eq(pItem.l_newvol, 0.0))
			{
				WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN,
					"[{}] Closing pre long position of {} skipped because of non-zero new long position", _id.c_str(), stdCode);
				continue;
			}

			//这里还要考虑单笔最大委托数量
			if (decimal::gt(maxQty, 0))
			{
				double leftQty = maxQty;
				for (;;)
				{
					double curQty = min(leftQty, unitQty);
					uint32_t localid = closeLong(stdCode, price, curQty, false, flag);//如果不支持平今, 则直接下平仓标记即可
					ret.emplace_back(localid);

					leftQty -= curQty;

					if (decimal::eq(leftQty, 0))
						break;
				}
				left -= maxQty;

				if (commInfo->getCoverMode() == CM_CoverToday)
				{
					WTSLogger::log_dyn("trader", _id.c_str(), LL_DEBUG,
						"[{}] Signal of selling {} of quantity {} triggered: Closing old long of quantity {}", _id.c_str(), stdCode, qty, maxQty);
				}
				else
				{
					WTSLogger::log_dyn("trader", _id.c_str(), LL_DEBUG,
						"[{}] Signal of selling {} of quantity {} triggered: Closing long of quantity {}", _id.c_str(), stdCode, qty, maxQty);
				}
			}
		}
		else if (curRule._atype == AT_Close)
		{
			//如果只是平仓, 则分情况处理
			//如果区分平昨平今, 则要先平昨再平今
			//如果不区分平昨平今, 则统一平仓
			if (commInfo->getCoverMode() != CM_CoverToday)
			{
				double maxQty = min(pItem.avail_pos(true), left);	//不区分平昨平今, 则读取全部可平量
				if (decimal::gt(maxQty, 0))
				{
					double leftQty = maxQty;
					for (;;)
					{
						double curQty = min(leftQty, unitQty);
						uint32_t localid = closeLong(stdCode, price, curQty, false, flag);
						ret.emplace_back(localid);

						leftQty -= curQty;

						if (decimal::eq(leftQty, 0))
							break;
					}
					left -= maxQty;

					WTSLogger::log_dyn("trader", _id.c_str(), LL_DEBUG,
						"[{}] Signal of selling {} of quantity {} triggered: Closing long of quantity {}", _id.c_str(), stdCode, qty, maxQty);
				}

			}
			else
			{
				if (decimal::gt(left, 0) && decimal::gt(pItem.l_preavail, 0))
				{
					//先将可平昨仓平仓
					double maxQty = min(pItem.l_preavail, qty);
					if (decimal::gt(maxQty, 0))
					{
						double leftQty = maxQty;
						for (;;)
						{
							double curQty = min(leftQty, unitQty);
							uint32_t localid = closeLong(stdCode, price, curQty, false, flag);
							ret.emplace_back(localid);

							leftQty -= curQty;

							if (decimal::eq(leftQty, 0))
								break;
						}
						left -= maxQty;

						WTSLogger::log_dyn("trader", _id.c_str(), LL_DEBUG,
							"[{}] Signal of selling {} of quantity {} triggered: Closing old long of quantity {}", _id.c_str(), stdCode, qty, maxQty);
					}

				}

				if (decimal::gt(left, 0) && decimal::gt(pItem.l_newavail, 0))
				{
					//再将可平今仓平仓
					//TODO: 这里还有一个控制, 就是强制锁今仓的话, 这段逻辑就跳过去了
					double maxQty = min(pItem.l_newavail, left);
					if (decimal::gt(maxQty, 0))
					{
						double leftQty = maxQty;
						for (;;)
						{
							double curQty = min(leftQty, unitQty);
							uint32_t localid = closeLong(stdCode, price, curQty, true, flag);
							ret.emplace_back(localid);

							leftQty -= curQty;

							if (decimal::eq(leftQty, 0))
								break;
						}
						left -= maxQty;

						WTSLogger::log_dyn("trader", _id.c_str(), LL_DEBUG,
							"[{}] Signal of selling {} of quantity {} triggered: Closing new long of quantity {}", _id.c_str(), stdCode, qty, maxQty);
					}

				}
			}
		}

		if (left == 0)
			break;
	}

	if (left > 0)
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR,
			"[{}] Signal of buying {} of quantity {} left quantity of {} not triggered", _id.c_str(), stdCode, qty, left);
	}

	return ret;
}


uint32_t TraderAdapter::openLong(const char* stdCode, double price, double qty, int flag /* = 0 */)
{
	//if (_risk_mon_enabled && !checkOrderLimits(stdCode))
	//{
	//	WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN, "{} is forbidden to trade", stdCode);
	//	return 0;
	//}
	if (!isValidStandardOrderCode(stdCode))
		return UINT_MAX;

	WTSEntrust* entrust = WTSEntrust::create(stdCode, qty, price);
	if(price == 0.0)
	{
		entrust->setPriceType(WPT_ANYPRICE);
	}
	else
	{
		entrust->setPriceType(WPT_LIMITPRICE);
	}
	entrust->setOrderFlag((WTSOrderFlag)(WOF_NOR + flag));

	entrust->setDirection(WDT_LONG);
	entrust->setOffsetType(WOT_OPEN);

	updateUndone(stdCode, qty);

	uint32_t ret = doEntrust(entrust);
	entrust->release();
	return ret;
}

uint32_t TraderAdapter::openShort(const char* stdCode, double price, double qty, int flag/* = 0*/)
{
	//if (_risk_mon_enabled && !checkOrderLimits(stdCode))
	//{
	//	WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN, "{} is forbidden to trade", stdCode);
	//	return 0;
	//}
	if (!isValidStandardOrderCode(stdCode))
		return UINT_MAX;

	WTSEntrust* entrust = WTSEntrust::create(stdCode, qty, price);
	if (price == 0.0)
	{
		entrust->setPriceType(WPT_ANYPRICE);
	}
	else
	{
		entrust->setPriceType(WPT_LIMITPRICE);
	}
	entrust->setOrderFlag((WTSOrderFlag)(WOF_NOR + flag));

	entrust->setDirection(WDT_SHORT);
	entrust->setOffsetType(WOT_OPEN);

	updateUndone(stdCode, qty);

	uint32_t ret = doEntrust(entrust);
	entrust->release();
	return ret;
}

uint32_t TraderAdapter::closeLong(const char* stdCode, double price, double qty, bool isToday /* = false */, int flag/* = 0*/)
{
	//if (_risk_mon_enabled && !checkOrderLimits(stdCode))
	//{
	//	WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN, "{} is forbidden to trade", stdCode);
	//	return 0;
	//}
	if (!isValidStandardOrderCode(stdCode))
		return UINT_MAX;

	WTSEntrust* entrust = WTSEntrust::create(stdCode, qty, price);
	if (price == 0.0)
	{
		entrust->setPriceType(WPT_ANYPRICE);
	}
	else
	{
		entrust->setPriceType(WPT_LIMITPRICE);
	}
	entrust->setOrderFlag((WTSOrderFlag)(WOF_NOR + flag));

	entrust->setDirection(WDT_LONG);
	entrust->setOffsetType(isToday ? WOT_CLOSETODAY : WOT_CLOSE);

	updateUndone(stdCode, qty);

	uint32_t ret = doEntrust(entrust);
	entrust->release();
	return ret;
}

uint32_t TraderAdapter::closeShort(const char* stdCode, double price, double qty, bool isToday /* = false */, int flag/* = 0*/)
{
	//if (_risk_mon_enabled && !checkOrderLimits(stdCode))
	//{
	//	WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN, "{} is forbidden to trade", stdCode);
	//	return 0;
	//}
	if (!isValidStandardOrderCode(stdCode))
		return UINT_MAX;

	WTSEntrust* entrust = WTSEntrust::create(stdCode, qty, price);
	if (price == 0.0)
	{
		entrust->setPriceType(WPT_ANYPRICE);
	}
	else
	{
		entrust->setPriceType(WPT_LIMITPRICE);
	}
	entrust->setOrderFlag((WTSOrderFlag)(WOF_NOR + flag));

	entrust->setDirection(WDT_SHORT);
	entrust->setOffsetType(isToday ? WOT_CLOSETODAY : WOT_CLOSE);

	updateUndone(stdCode, qty);

	uint32_t ret = doEntrust(entrust);
	entrust->release();
	return ret;
}


#pragma region "ITraderSpi接口"
void TraderAdapter::handleEvent(WTSTraderEvent e, int32_t ec)
{
	SpiCallbackLease callback(this);
	if (!callback)
		return;

	if(e == WTE_Connect)
	{
		if(ec == 0)
		{
			ApiCallLease loginCall(this, false, true);
			if (!loginCall)
				return;
			_trader_api->login(_cfg->getCString("user"), _cfg->getCString("pass"), _cfg->getCString("product"));
		}
		else
		{
			WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR,"[{}] Trading channel connecting failed: {}", _id.c_str(), ec);
		}
	}
	else if(e == WTE_Close)
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR,"[{}] Trading channel disconnected: {}", _id.c_str(), ec);
		notifyChannelLost();
	}
}

void TraderAdapter::onLoginResult(bool bSucc, const char* msg, uint32_t tradingdate)
{
	SpiCallbackLease callback(this);
	if (!callback)
		return;

	// Ignore a response after close unless a login attempt is currently active.
	// The legacy SPI has no session token, so an old response arriving during a later
	// reconnect cannot be distinguished from that reconnect's own response.
	AdapterState expected = AS_LOGINING;
	if(!bSucc)
	{
		if (!_state.compare_exchange_strong(expected, AS_LOGINFAILED, std::memory_order_acq_rel))
			return;
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR,"[{}] Trader login failed: {}", _id.c_str(), msg);
	}
	else
	{
		if (!_state.compare_exchange_strong(expected, AS_LOGINED, std::memory_order_acq_rel))
			return;
		WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO,"[{}] Trader login succeed, trading date: {}", _id.c_str(), tradingdate);
		_trading_day = tradingdate;
		ApiCallLease apiCall(this);
		if (apiCall)
			_trader_api->queryPositions();	//查持仓
	}
}

void TraderAdapter::onLogout()
{
	SpiCallbackLease callback(this);
	if (!callback)
		return;

	WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN, "[{}] Trading channel logged out", _id.c_str());
	notifyChannelLost();
}

void TraderAdapter::onRspEntrust(WTSEntrust* entrust, WTSError *err)
{
	SpiCallbackLease callback(this);
	if (!callback)
		return;

	if (err && err->getErrorCode() != WEC_NONE)
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR,err->getMessage());
		const std::string userTag = entrust->getUserTag();
		const std::string ownTagPrefix = _order_pattern + ".";
		if (userTag.compare(0, ownTagPrefix.size(), ownTagPrefix) != 0 || userTag.size() == ownTagPrefix.size())
		{
			WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR,
				"[{}] Cannot reconcile rejected entrust without this adapter's user tag; undone quantity unchanged", _id.c_str());
			return;
		}

		WTSContractInfo* cInfo = _bd_mgr->getContract(entrust->getCode(), entrust->getExchg());
		if (cInfo == NULL)
			return;

		std::string stdCode = cInfo->getFullCode();

		bool isLong = (entrust->getDirection() == WDT_LONG);
		bool isToday = (entrust->getOffsetType() == WOT_CLOSETODAY);
		bool isOpen = (entrust->getOffsetType() == WOT_OPEN);
		double qty = entrust->getVolume();

		std::string action;
		if (isOpen)
			action = "open ";
		else if (isToday)
			action = "closetoday ";
		else
			action = "close ";
		action += isLong ? "long" : "short";

		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR, 
			"[{}] Order placing failed: {}, instrument: {}, action: {}, qty: {}", _id.c_str(), err->getMessage(), entrust->getCode(), action.c_str(), qty);

		if (!rollbackUndoneIfNonzero(stdCode.c_str(), qty))
			return;


		if (strlen(entrust->getUserTag()) > 0)
		{
			char* userTag = (char*)entrust->getUserTag();
			userTag += _order_pattern.size() + 1;
			uint32_t localid = convert::to_uint32(userTag);

			const std::string errorMessage = err->getMessage();
			notifySinks([&](ITrdNotifySink* sink) {
				sink->on_entrust(localid, stdCode.c_str(), false, errorMessage.c_str());
			});
		}
	}
}

void TraderAdapter::onRspAccount(WTSArray* /*ayAccounts*/)
{
	SpiCallbackLease callback(this);
	if (!callback)
		return;

	notifyChannelReadyIfExpected();
}

void TraderAdapter::onRspPosition(const WTSArray* ayPositions)
{
	SpiCallbackLease callback(this);
	if (!callback)
		return;

	if (ayPositions && ayPositions->size() > 0)
	{
		for (auto it = ayPositions->begin(); it != ayPositions->end(); it++)
		{
			WTSPositionItem* pItem = (WTSPositionItem*)(*it);
			WTSContractInfo* cInfo = _bd_mgr->getContract(pItem->getCode(), pItem->getExchg());
			if (cInfo == NULL)
				continue;

			std::string stdCode = cInfo->getFullCode();

			PosItem& pos = _positions[stdCode];
			if (pItem->getDirection() == WDT_LONG)
			{
				pos.l_newavail = pItem->getAvailNewPos();
				pos.l_newvol = pItem->getNewPosition();
				pos.l_preavail = pItem->getAvailPrePos();
				pos.l_prevol = pItem->getPrePosition();
			}
			else
			{
				pos.s_newavail = pItem->getAvailNewPos();
				pos.s_newvol = pItem->getNewPosition();
				pos.s_preavail = pItem->getAvailPrePos();
				pos.s_prevol = pItem->getPrePosition();
			}
		}

		for (auto it = _positions.begin(); it != _positions.end(); it++)
		{
			const char* stdCode = it->first.c_str();
			const PosItem& pItem = it->second;
			printPosition(stdCode, pItem);
			notifySinks([&](ITrdNotifySink* sink) {
				sink->on_position(stdCode, true, pItem.l_prevol, pItem.l_preavail, pItem.l_newvol, pItem.l_newavail, _trading_day);
				sink->on_position(stdCode, false, pItem.s_prevol, pItem.s_preavail, pItem.s_newvol, pItem.s_newavail, _trading_day);
			});
		}
	}

	WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO,"[{}] Position data updated", _id.c_str());

	AdapterState expected = AS_LOGINED;
	if (_state.compare_exchange_strong(expected, AS_POSITION_QRYED, std::memory_order_acq_rel))
	{
		ApiCallLease apiCall(this);
		if (apiCall)
			_trader_api->queryOrders();
	}
}

void TraderAdapter::onRspOrders(const WTSArray* ayOrders)
{
	SpiCallbackLease callback(this);
	if (!callback)
		return;

	if (ayOrders)
	{
		{
			SpinLock lock(_mtx_orders);
			if (_orders == NULL)
				_orders = OrderMap::create();
		}

		std::lock_guard<std::mutex> undoneLock(_mtx_undone_qty);
		_undone_qty.clear();

		for (auto it = ayOrders->begin(); it != ayOrders->end(); it++)
		{
			WTSOrderInfo* orderInfo = (WTSOrderInfo*)(*it);
			if (orderInfo == NULL)
				continue;

			WTSContractInfo* cInfo = orderInfo->getContractInfo();
			if(cInfo == NULL)
				cInfo = _bd_mgr->getContract(orderInfo->getCode(), orderInfo->getExchg());

			if (cInfo == NULL)
				continue;

			bool isBuy = (orderInfo->getDirection() == WDT_LONG && orderInfo->getOffsetType() == WOT_OPEN) || (orderInfo->getDirection() == WDT_SHORT && orderInfo->getOffsetType() != WOT_OPEN);

			std::string stdCode = cInfo->getFullCode();

			_orderids.insert(orderInfo->getOrderID());		

			//更新统计信息
			WTSTradeStateInfo* statInfo = (WTSTradeStateInfo*)_stat_map->get(stdCode.c_str());
			if (statInfo == NULL)
			{
				statInfo = WTSTradeStateInfo::create(stdCode.c_str());
				_stat_map->add(stdCode, statInfo, false);
			}
			TradeStatInfo& statItem = statInfo->statInfo();
			statItem._infos++;	//无论什么状态，挂单信息量+1
			if (isBuy)
			{
				statItem.b_orders++;
				statItem.b_ordqty += orderInfo->getVolume();

				if (orderInfo->isError())
				{
					statItem.b_wrongs++;
					statItem.b_wrongqty += orderInfo->getVolume() - orderInfo->getVolTraded();
				}
				else if (orderInfo->getOrderState() == WOS_Canceled)
				{
					if (orderInfo->getOrderFlag() == WOF_NOR)
					{
						statItem.b_cancels++;
						statItem.b_canclqty += orderInfo->getVolume() - orderInfo->getVolTraded();
					}
					else
					{
						statItem.b_auto_cancels++;
						statItem.b_auto_canclqty += orderInfo->getVolume() - orderInfo->getVolTraded();
					}

					//撤单信息量+1
					statItem._infos++;
				}

			}
			else
			{
				statItem.s_orders++;
				statItem.s_ordqty += orderInfo->getVolume();

				if (orderInfo->isError())
				{
					statItem.s_wrongs++;
					statItem.s_wrongqty += orderInfo->getVolume() - orderInfo->getVolTraded();
				}
				else if (orderInfo->getOrderState() == WOS_Canceled)
				{
					if (orderInfo->getOrderFlag() == WOF_NOR)
					{
						statItem.s_cancels++;
						statItem.s_canclqty += orderInfo->getVolume() - orderInfo->getVolTraded();
					}
					else
					{
						statItem.s_auto_cancels++;
						statItem.s_auto_canclqty += orderInfo->getVolume() - orderInfo->getVolTraded();
					}

					//撤单信息量+1
					statItem._infos++;
				}
			}

			if (!orderInfo->isAlive())
				continue;

			if (!StrUtil::startsWith(orderInfo->getUserTag(), _order_pattern.c_str(), true))
				continue;;

			char* userTag = (char*)orderInfo->getUserTag();
			userTag += _order_pattern.size() + 1;
			uint32_t localid = convert::to_uint32(userTag);

			{
				SpinLock lock(_mtx_orders);
				if (_orders == NULL)
					_orders = OrderMap::create();
				_orders->add(localid, orderInfo);
			}

			double& curQty = _undone_qty[stdCode];
			curQty += orderInfo->getVolLeft();
		}

		for (auto it = _undone_qty.begin(); it != _undone_qty.end(); it++)
		{
			const char* stdCode = it->first.c_str();
			const double& curQty = _undone_qty[stdCode];

			WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO, 
				"[{}]{} undone quantity {}", _id.c_str(), stdCode, curQty);
		}
	}

	AdapterState expected = AS_POSITION_QRYED;
	if (_state.compare_exchange_strong(expected, AS_ORDERS_QRYED, std::memory_order_acq_rel))
	{
		ApiCallLease apiCall(this);
		if (apiCall)
			_trader_api->queryTrades();
	}
}

void TraderAdapter::printPosition(const char* code, const PosItem& pItem)
{
	WTSLogger::log_dyn("trader", _id.c_str(), LL_DEBUG, "[{}] {} position updated, long:{}[{}]|{}[{}], short:{}[{}]|{}[{}]",
		_id.c_str(), code, pItem.l_prevol, pItem.l_preavail, pItem.l_newvol, pItem.l_newavail, 
		pItem.s_prevol, pItem.s_preavail, pItem.s_newvol, pItem.s_newavail);
}

void TraderAdapter::onRspTrades(const WTSArray* ayTrades)
{
	SpiCallbackLease callback(this);
	if (!callback)
		return;

	if (ayTrades)
	{
		for (auto it = ayTrades->begin(); it != ayTrades->end(); it++)
		{
			WTSTradeInfo* tInfo = (WTSTradeInfo*)(*it);

			WTSContractInfo* cInfo = tInfo->getContractInfo();
			if (cInfo == NULL && _bd_mgr != NULL)
				cInfo = _bd_mgr->getContract(tInfo->getCode(), tInfo->getExchg());
			if (cInfo == NULL)
				continue;

			const char* stdCode = cInfo->getFullCode();

			WTSTradeStateInfo* statInfo = (WTSTradeStateInfo*)_stat_map->get(stdCode);
			if (statInfo == NULL)
			{
				statInfo = WTSTradeStateInfo::create(stdCode);
				_stat_map->add(stdCode, statInfo, false);
			}
			TradeStatInfo& statItem = statInfo->statInfo();

			bool isLong = (tInfo->getDirection() == WDT_LONG);
			bool isOpen = (tInfo->getOffsetType() == WOT_OPEN);
			bool isCloseT = (tInfo->getOffsetType() == WOT_CLOSETODAY);
			double qty = tInfo->getVolume();

			if (isLong)
			{
				if (isOpen)
					statItem.l_openvol += qty;
				else if (isCloseT)
					statItem.l_closetvol += qty;
				else
					statItem.l_closevol += qty;
			}
			else
			{
				if (isOpen)
					statItem.s_openvol += qty;
				else if (isCloseT)
					statItem.s_closetvol += qty;
				else
					statItem.s_closevol += qty;
			}
		}

		for (auto it = _stat_map->begin(); it != _stat_map->end(); it++)
		{
			const char* stdCode = it->first.c_str();
			WTSTradeStateInfo* pItem = (WTSTradeStateInfo*)it->second;
			WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO,
				"[{}] {} action stats updated, long opened: {}, long closed: {}, new long closed: {}, short opened: {}, short closed: {}, new short closed: {}",
				_id.c_str(), stdCode, pItem->open_volume_long(), pItem->close_volume_long(), pItem->closet_volume_long(),
				pItem->open_volume_short(), pItem->close_volume_short(), pItem->closet_volume_short());
		}
	}

	AdapterState expected = AS_ORDERS_QRYED;
	if (_state.compare_exchange_strong(expected, AS_TRADES_QRYED, std::memory_order_acq_rel))
	{
		ApiCallLease apiCall(this);
		if (apiCall)
			_trader_api->queryAccount();
	}
}

inline const char* stateToName(WTSOrderState woState)
{
	if (woState == WOS_AllTraded)
		return "AllTrd";
	else if (woState == WOS_PartTraded_NotQueuing || woState == WOS_PartTraded_Queuing)
		return "PrtTrd";
	else if (woState == WOS_NotTraded_NotQueuing || woState == WOS_NotTraded_Queuing)
		return "UnTrd";
	else if (woState == WOS_Canceled)
		return "Cncld";
	else if (woState == WOS_Submitting)
		return "Smtting";
	else if (woState == WOS_Nottouched)
		return "UnSmt";
	else
		return "Error";
}

void TraderAdapter::onPushOrder(WTSOrderInfo* orderInfo)
{
	SpiCallbackLease callback(this);
	if (!callback)
		return;

	if (orderInfo == NULL)
		return;

	WTSContractInfo* cInfo = orderInfo->getContractInfo();
	if(cInfo == NULL) cInfo = _bd_mgr->getContract(orderInfo->getCode(), orderInfo->getExchg());
	if (cInfo == NULL)
		return;

	const char* fullCode = cInfo->getFullCode();

	bool isBuy = (orderInfo->getDirection() == WDT_LONG && orderInfo->getOffsetType() == WOT_OPEN) || (orderInfo->getDirection() == WDT_SHORT && orderInfo->getOffsetType() != WOT_OPEN);

	WTSTradeStateInfo* statInfo = (WTSTradeStateInfo*)_stat_map->get(fullCode);
	if (statInfo == NULL)
	{
		statInfo = WTSTradeStateInfo::create(fullCode);
		_stat_map->add(fullCode, statInfo, false);
	}
	TradeStatInfo& statItem = statInfo->statInfo();

	//撤销的话, 要更新统计数据
	if (orderInfo->getOrderState() == WOS_Canceled)
	{
		statItem._infos++;	//撤单成功信息量+1
		if (isBuy)
		{
			if (orderInfo->isError())//错单要和撤单区分开
			{
				statItem.b_wrongs++;
				statItem.b_wrongqty += orderInfo->getVolume() - orderInfo->getVolTraded();
			}
			else
			{
				//只有普通订单的撤单才计入统计
				if(orderInfo->getOrderFlag() == WOF_NOR)
				{
					statItem.b_cancels++;
					statItem.b_canclqty += orderInfo->getVolume() - orderInfo->getVolTraded();
				}
				else
				{
					statItem.b_auto_cancels++;
					statItem.b_auto_canclqty += orderInfo->getVolume() - orderInfo->getVolTraded();
				}
			}
		}
		else
		{
			if (orderInfo->isError())//错单要和撤单区分开
			{
				statItem.s_wrongs++;
				statItem.s_wrongqty += orderInfo->getVolume() - orderInfo->getVolTraded();
			}
			else
			{
				if (orderInfo->getOrderFlag() == WOF_NOR)
				{
					statItem.s_cancels++;
					statItem.s_canclqty += orderInfo->getVolume() - orderInfo->getVolTraded();
				}
				else
				{
					statItem.s_auto_cancels++;
					statItem.s_auto_canclqty += orderInfo->getVolume() - orderInfo->getVolTraded();
				}
			}
		}
	}


	WTSLogger::log_dyn("trader", _id.c_str(), LL_DEBUG,"[{}] Order notified, instrument: {}, usertag: {}, state: {}", _id.c_str(), fullCode, orderInfo->getUserTag(), stateToName(orderInfo->getOrderState()));

	//如果订单撤销, 并且是wt的订单, 则要先更新未完成数量
	if (orderInfo->getOrderState() == WOS_Canceled && StrUtil::startsWith(orderInfo->getUserTag(), _order_pattern.c_str(), true))
	{
		//撤单的时候, 要更新未完成
		bool isLong = (orderInfo->getDirection() == WDT_LONG);
		bool isOpen = (orderInfo->getOffsetType() == WOT_OPEN);
		[[maybe_unused]] bool isToday = (orderInfo->getOffsetType() == WOT_CLOSETODAY);
		double qty = orderInfo->getVolume() - orderInfo->getVolTraded();

		[[maybe_unused]] bool isBuy = (isLong&&isOpen) || (!isLong&&!isOpen);
		//double oldQty = _undone_qty[stdCode];
		//double newQty = oldQty - qty*(isBuy ? 1 : -1);
		//_undone_qty[stdCode] = newQty;
		//WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO, "[{}] {} qty of undone order updated, {} -> {}", _id.c_str(), stdCode.c_str(), oldQty, newQty);
		updateUndone(fullCode, -qty);

		WTSLogger::log_dyn("trader", _id.c_str(), LL_DEBUG, "[{}] Order {} of {} canceled:{}, action: {}, leftqty: {}",
			_id.c_str(), orderInfo->getUserTag(), fullCode, orderInfo->getStateMsg(),
			formatAction(orderInfo->getDirection(), orderInfo->getOffsetType()), qty);
	}

	//先检查该订单是不是第一次推送过来
	//如果是第一次推送过来, 则要根据开平更新可平
	if (strlen(orderInfo->getOrderID()) > 0)
	{
		auto it = _orderids.find(orderInfo->getOrderID());
		if (it == _orderids.end())
		{
			//先把订单号缓存起来, 防止重复处理
			_orderids.insert(orderInfo->getOrderID());
			statItem._infos++;	//下单成功信息量+1

			//只有平仓需要更新可平
			if (orderInfo->getOffsetType() != WOT_OPEN)
			{
				//const char* code = stdCode.c_str();
				bool isLong = (orderInfo->getDirection() == WDT_LONG);
				bool isToday = (orderInfo->getOffsetType() == WOT_CLOSETODAY);
				double qty = orderInfo->getVolume();

				PosItem& pItem = _positions[fullCode];
				if (isLong)	//平多
				{
					if (isToday)
					{
						pItem.l_newavail -= min(pItem.l_newavail, qty);	//如果是平今, 则只需要更新可平今仓
					}
					else
					{
						double left = qty;

						//如果是平仓, 则先更新可平昨仓, 还有剩余, 再更新可平今仓
						//如果品种区分平昨平今, 也按照这个流程, 因为平昨的总数量不可能超出昨仓
						double maxQty = min(pItem.l_preavail, qty);
						pItem.l_preavail -= maxQty;
						left -= maxQty;

						if (left > 0)
							pItem.l_newavail -= min(pItem.l_newavail, left);
					}
				}
				else //平空
				{
					if (isToday)
					{
						pItem.s_newavail -= min(pItem.s_newavail, qty);
					}
					else
					{
						double left = qty;

						double maxQty = min(pItem.s_preavail, qty);
						pItem.s_preavail -= maxQty;
						left -= maxQty;

						if (left > 0)
							pItem.s_newavail -= min(pItem.s_newavail, left);
					}
				}
				printPosition(fullCode, pItem);
			}
		}
		else if (orderInfo->getOrderState() == WOS_Canceled && orderInfo->getOffsetType() != WOT_OPEN)
		{
			//如果订单不是第一次推送, 且撤销了, 则要更新可平量
			//const char* code = orderInfo->getCode();
			bool isLong = (orderInfo->getDirection() == WDT_LONG);
			bool isToday = (orderInfo->getOffsetType() == WOT_CLOSETODAY);
			double qty = orderInfo->getVolume() - orderInfo->getVolTraded();

			PosItem& pItem = _positions[fullCode];
			if (isLong)	//平多
			{
				if (isToday)
				{
					pItem.l_newavail += qty;	//如果是平今, 则只需要更新可平今仓
				}
				else
				{
					pItem.l_preavail += qty;
					if (pItem.l_preavail > pItem.l_prevol)
					{
						pItem.l_newavail += (pItem.l_preavail - pItem.l_prevol);
						pItem.l_preavail = pItem.l_prevol;
					}
				}
			}
			else //平空
			{
				if (isToday)
				{
					pItem.s_newavail += qty;
				}
				else
				{
					pItem.s_preavail += qty;
					if (pItem.s_preavail > pItem.s_prevol)
					{
						pItem.s_newavail += (pItem.s_preavail - pItem.s_prevol);
						pItem.s_preavail = pItem.s_prevol;
					}
				}
			}
			printPosition(fullCode, pItem);
		}
	}

	uint32_t localid = 0;

	//先看看是不是wt发出去的单子
	if (StrUtil::startsWith(orderInfo->getUserTag(), _order_pattern.c_str(), true))
	{
		char* userTag = (char*)orderInfo->getUserTag();
		userTag += _order_pattern.size() + 1;
		localid = convert::to_uint32(userTag);
	}

	//如果是wt发出去的单子则需要更新内部数据
	if(localid != 0)
	{
		{
			SpinLock lock(_mtx_orders);
			if (!orderInfo->isAlive() && _orders)
			{
				_orders->remove(localid);
			}
			else
			{
				if (_orders == NULL)
					_orders = OrderMap::create();

				_orders->add(localid, orderInfo);
			}
		}
		

		uint32_t offset;
		if (orderInfo->getOffsetType() == WOT_OPEN)
			offset = 0;
		else if (orderInfo->getOffsetType() == WOT_CLOSE)
			offset = 1;
		else
			offset = 2;

		//通知所有监听接口
		const bool isLong = orderInfo->getDirection() == WDT_LONG;
		const double totalQty = orderInfo->getVolume();
		const double leftQty = orderInfo->getVolLeft();
		const double price = orderInfo->getPrice();
		const bool isCanceled = orderInfo->getOrderState() == WOS_Canceled;
		notifySinks([&](ITrdNotifySink* sink) {
			sink->on_order(localid, fullCode, isLong, offset, totalQty, leftQty, price, isCanceled);
		});
	}
}

void TraderAdapter::onPushTrade(WTSTradeInfo* tradeRecord)
{
	SpiCallbackLease callback(this);
	if (!callback)
		return;

	WTSContractInfo* cInfo = tradeRecord->getContractInfo();
	if (cInfo == NULL && _bd_mgr != NULL)
		cInfo = _bd_mgr->getContract(tradeRecord->getCode(), tradeRecord->getExchg());
	if (cInfo == NULL)
		return;

	WTSCommodityInfo* commInfo = cInfo->getCommInfo();

	bool isLong = (tradeRecord->getDirection() == WDT_LONG);
	bool isOpen = (tradeRecord->getOffsetType() == WOT_OPEN);
	[[maybe_unused]] bool isBuy = (tradeRecord->getDirection() == WDT_LONG && tradeRecord->getOffsetType() == WOT_OPEN) || (tradeRecord->getDirection() == WDT_SHORT && tradeRecord->getOffsetType() != WOT_OPEN);

	std::string stdCode = cInfo->getFullCode();

	WTSLogger::log_dyn("trader", _id.c_str(), LL_DEBUG,
		"[{}] Trade notified, instrument: {}, usertag: {}, trdqty: {}, trdprice: {}", 
			_id.c_str(), stdCode.c_str(), tradeRecord->getUserTag(), tradeRecord->getVolume(), tradeRecord->getPrice());

	//如果是自己的订单，则更新未完成单
	uint32_t localid = 0;
	if (StrUtil::startsWith(tradeRecord->getUserTag(), _order_pattern.c_str(), true))
	{
		char* userTag = (char*)tradeRecord->getUserTag();
		userTag += _order_pattern.size() + 1;
		localid = convert::to_uint32(userTag);

		//double oldQty = _undone_qty[stdCode];
		//double newQty = oldQty - tradeRecord->getVolume()*(isBuy ? 1 : -1);
		//_undone_qty[stdCode] = newQty;
		//WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO, 
		//	"[{}] {} qty of undone orders updated, {} -> {}", _id.c_str(), stdCode.c_str(), oldQty, newQty);
		updateUndone(stdCode.c_str(), -tradeRecord->getVolume());
	}

	PosItem& pItem = _positions[stdCode];
	WTSTradeStateInfo* statInfo = (WTSTradeStateInfo*)_stat_map->get(stdCode.c_str());
	if (statInfo == NULL)
	{
		statInfo = WTSTradeStateInfo::create(stdCode.c_str());
		_stat_map->add(stdCode, statInfo, false);
	}

	double vol = tradeRecord->getVolume();
	if(isLong)
	{
		if (isOpen)
		{
			pItem.l_newvol += vol;

			if(!commInfo->isT1())	//如果不是T1，则更新可用持仓
				pItem.l_newavail += vol;
		}
		else if (tradeRecord->getOffsetType() == WOT_CLOSETODAY)
		{
			pItem.l_newvol -= vol;
		}
		else
		{
			double left = vol;
			double maxVol = min(left, pItem.l_prevol);
			pItem.l_prevol -= maxVol;
			left -= maxVol;
			pItem.l_newvol -= left;
		}
	}
	else
	{
		if (isOpen)
		{
			pItem.s_newvol += vol;
			if (!commInfo->isT1())	//如果不是T1，则更新可用持仓
				pItem.s_newavail += vol;
		}
		else if (tradeRecord->getOffsetType() == WOT_CLOSETODAY)
		{
			pItem.s_newvol -= vol;
		}
		else
		{
			double left = vol;
			double maxVol = min(left, pItem.s_prevol);
			pItem.s_prevol -= maxVol;
			left -= maxVol;
			pItem.s_newvol -= left;
		}
	}

	printPosition(stdCode.c_str(), pItem);


	uint32_t offset;
	if (tradeRecord->getOffsetType() == WOT_OPEN)
		offset = 0;
	else if (tradeRecord->getOffsetType() == WOT_CLOSE)
		offset = 1;
	else
		offset = 2;
	const double price = tradeRecord->getPrice();
	notifySinks([&](ITrdNotifySink* sink) {
		sink->on_trade(localid, stdCode.c_str(), isLong, offset, vol, price);
	});

	ApiCallLease apiCall(this);
	if (apiCall)
		_trader_api->queryAccount();
}

void TraderAdapter::onTraderError(WTSError* err, void* /*pData*/ /* = NULL */)
{
	SpiCallbackLease callback(this);
	if (!callback)
		return;

	if(err)
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR,"[{}] Error of trading channel occured: {}", _id.c_str(), err->getMessage());
}

IBaseDataMgr* TraderAdapter::getBaseDataMgr()
{
	SpiCallbackLease callback(this);
	if (!callback)
		return NULL;

	return _bd_mgr;
}

void TraderAdapter::handleTraderLog(WTSLogLevel ll, const char* message)
{
	SpiCallbackLease callback(this);
	if (!callback)
		return;

	WTSLogger::log_dyn_raw("trader", _id.c_str(), ll, message);
}

bool TraderAdapter::checkCancelLimits(const char* stdCode)
{
	if (!_risk_mon_enabled || stdCode == NULL || stdCode[0] == '\0')
		return true;

	if (_exclude_codes.find(stdCode) != _exclude_codes.end())
		return false;

	const RiskParams* riskPara = getRiskParams(stdCode);
	if (riskPara == NULL)
		return true;

	WTSTradeStateInfo* statInfo = _stat_map == NULL ? NULL : (WTSTradeStateInfo*)_stat_map->get(stdCode);
	if (statInfo && riskPara->_cancel_total_limits != 0 && statInfo->total_cancels() >= riskPara->_cancel_total_limits)
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR, "[{}] {} cancel {} times totaly, beyond boundary {} times, adding to excluding list",
			_id.c_str(), stdCode, statInfo->total_cancels(), riskPara->_cancel_total_limits);
		_exclude_codes.insert(stdCode);
		return false;
	}

	if (riskPara->_cancel_times_boundary != 0)
	{
		auto it = _cancel_time_cache.find(stdCode);
		if (it != _cancel_time_cache.end())
		{
			TimeCacheList& cache = (TimeCacheList&)it->second;
			const uint64_t now = static_cast<uint64_t>(TimeUtils::getLocalTimeNow());
			const uint64_t windowMs = static_cast<uint64_t>(riskPara->_cancel_stat_timespan) * 1000U;
			const uint64_t startTime = now > windowMs ? now - windowMs : 0;
			auto firstInWindow = std::lower_bound(cache.begin(), cache.end(), startTime);
			cache.erase(cache.begin(), firstInWindow);
			if (cache.size() >= riskPara->_cancel_times_boundary)
			{
				WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR, "[{}] {} cancel {} times within {} seconds, beyond boundary {} times, adding to excluding list",
					_id.c_str(), stdCode, cache.size(), riskPara->_cancel_stat_timespan, riskPara->_cancel_times_boundary);
				_exclude_codes.insert(stdCode);
				return false;
			}
		}
	}

	return true;
}

bool TraderAdapter::isTradeEnabled(const char* stdCode) const
{
	if (!_risk_mon_enabled)
		return true;

	if (_exclude_codes.find(stdCode) != _exclude_codes.end())
		return false;

	return true;
}

bool TraderAdapter::checkOrderLimits(const char* stdCode)
{
	if (!_risk_mon_enabled || stdCode == NULL || stdCode[0] == '\0')
		return true;

	if (_exclude_codes.find(stdCode) != _exclude_codes.end())
		return false;

	const RiskParams* riskPara = getRiskParams(stdCode);
	if (riskPara == NULL)
		return true;

	WTSTradeStateInfo* statInfo = _stat_map == NULL ? NULL : (WTSTradeStateInfo*)_stat_map->get(stdCode);
	if (statInfo && riskPara->_order_total_limits != 0 && statInfo->total_orders() >= riskPara->_order_total_limits)
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR, "[{}] {} entrust {} times totally, beyond boundary {} times, adding to excluding list",
			_id.c_str(), stdCode, statInfo->total_orders(), riskPara->_order_total_limits);
		_exclude_codes.insert(stdCode);
		return false;
	}

	if (riskPara->_order_times_boundary != 0)
	{
		auto it = _order_time_cache.find(stdCode);
		if (it != _order_time_cache.end())
		{
			TimeCacheList& cache = (TimeCacheList&)it->second;
			const uint64_t now = static_cast<uint64_t>(TimeUtils::getLocalTimeNow());
			const uint64_t windowMs = static_cast<uint64_t>(riskPara->_order_stat_timespan) * 1000U;
			const uint64_t startTime = now > windowMs ? now - windowMs : 0;
			auto firstInWindow = std::lower_bound(cache.begin(), cache.end(), startTime);
			cache.erase(cache.begin(), firstInWindow);
			if (cache.size() >= riskPara->_order_times_boundary)
			{
				WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR, "[{}] {} entrust {} times within {} seconds, beyond boundary {} times, adding to excluding list",
					_id.c_str(), stdCode, cache.size(), riskPara->_order_stat_timespan, riskPara->_order_times_boundary);
				_exclude_codes.insert(stdCode);
				return false;
			}
		}
	}

	return true;
}

const TraderAdapter::RiskParams* TraderAdapter::getRiskParams(const char* stdCode)
{
	auto idx = StrUtil::findFirst(stdCode, '.');
	auto eIdx = idx + 1;
	while (isalpha(stdCode[eIdx]))
		eIdx++;


	auto it = _risk_params_map.find(std::string(stdCode + idx + 1, eIdx - idx + 1));
	if (it != _risk_params_map.end())
		return &it->second;

	it = _risk_params_map.find("default");
	return it == _risk_params_map.end() ? NULL : &it->second;
}

#pragma endregion "ITraderSpi接口"


//////////////////////////////////////////////////////////////////////////
//TraderAdapterMgr
bool TraderAdapterMgr::addAdapter(const char* tname, TraderAdapterPtr& adapter)
{
	if (adapter == NULL || strlen(tname) == 0)
		return false;

	std::lock_guard<std::mutex> lock(_mtx_lifecycle);
	if (_sink_notifications_stop_requested)
		return false;

	auto it = _adapters.find(tname);
	if(it != _adapters.end())
	{
		WTSLogger::error("Same name of trading channels: {}", tname);
		return false;
	}

	_adapters[tname] = adapter;

	return true;
}

TraderAdapterPtr TraderAdapterMgr::getAdapter(const char* tname)
{
	std::lock_guard<std::mutex> lock(_mtx_lifecycle);
	auto it = _adapters.find(tname);
	if (it != _adapters.end())
	{
		return it->second;
	}

	return TraderAdapterPtr();
}

void TraderAdapterMgr::run()
{
	std::vector<TraderAdapterPtr> adapters;
	{
		std::lock_guard<std::mutex> lock(_mtx_lifecycle);
		adapters.reserve(_adapters.size());
		for (auto it = _adapters.begin(); it != _adapters.end(); ++it)
			adapters.push_back(it->second);
	}
	for (const TraderAdapterPtr& adapter : adapters)
	{
		adapter->run();
	}

	WTSLogger::info("{} trading channels started", adapters.size());
}

void TraderAdapterMgr::release()
{
	{
		std::unique_lock<std::mutex> lock(_mtx_lifecycle);
		while (_release_in_progress)
		{
			bool callbackReentry = _release_thread == std::this_thread::get_id();
			if (!callbackReentry)
			{
				for (auto it = _adapters.begin(); it != _adapters.end(); ++it)
				{
					if (it->second->isInSinkNotificationCallback() || it->second->isInApiCall() || it->second->isInSpiCallback())
					{
						callbackReentry = true;
						break;
					}
				}
			}
			if (callbackReentry)
				return;
			_cv_lifecycle.wait(lock, [this]() { return !_release_in_progress; });
		}
		if (_adapters.empty())
		{
			_sink_notifications_stop_requested = false;
			return;
		}
		_release_in_progress = true;
		_release_thread = std::this_thread::get_id();
	}
	ScopeExit releaseComplete([this]() {
		std::lock_guard<std::mutex> lock(_mtx_lifecycle);
		_release_in_progress = false;
		_release_thread = std::thread::id();
		_cv_lifecycle.notify_all();
	});

	requestStopSinkNotifications();
	std::vector<TraderAdapterPtr> adapters;
	{
		std::lock_guard<std::mutex> lock(_mtx_lifecycle);
		adapters.reserve(_adapters.size());
		for (auto it = _adapters.begin(); it != _adapters.end(); ++it)
			adapters.push_back(it->second);
	}
	for (const TraderAdapterPtr& adapter : adapters)
		adapter->closeSpiCallbackAdmission();
	if (!waitSinkNotificationsStopped())
		return;

	for (const TraderAdapterPtr& adapter : adapters)
	{
		adapter->release();
	}
	for (const TraderAdapterPtr& adapter : adapters)
	{
		if (!adapter->isReleaseComplete())
		{
			WTSLogger::error("Trading adapter teardown failed; retaining adapter ownership");
			return;
		}
	}

	{
		std::lock_guard<std::mutex> lock(_mtx_lifecycle);
		_adapters.clear();
		_sink_notifications_stop_requested = false;
		_release_in_progress = false;
		_release_thread = std::thread::id();
		_cv_lifecycle.notify_all();
	}
	releaseComplete.dismiss();
}

void TraderAdapterMgr::requestStopSinkNotifications()
{
	std::lock_guard<std::mutex> lock(_mtx_lifecycle);
	_sink_notifications_stop_requested = true;
	for (auto it = _adapters.begin(); it != _adapters.end(); ++it)
		it->second->requestStopSinkNotifications();
}

bool TraderAdapterMgr::waitSinkNotificationsStopped()
{
	std::vector<TraderAdapterPtr> adapters;
	{
		std::lock_guard<std::mutex> lock(_mtx_lifecycle);
		if (!_sink_notifications_stop_requested)
			return false;
		adapters.reserve(_adapters.size());
		for (auto it = _adapters.begin(); it != _adapters.end(); ++it)
			adapters.push_back(it->second);
	}

	// Preflight every adapter first. Waiting on one adapter before discovering that
	// another is the current callback could deadlock cross-channel callback graphs.
	for (const TraderAdapterPtr& adapter : adapters)
	{
		if (adapter->isInSinkNotificationCallback() || adapter->isInApiCall() || adapter->isInSpiCallback())
			return false;
	}

	for (const TraderAdapterPtr& adapter : adapters)
	{
		if (!adapter->waitSinkNotificationsStopped())
			return false;
	}

	return true;
}
