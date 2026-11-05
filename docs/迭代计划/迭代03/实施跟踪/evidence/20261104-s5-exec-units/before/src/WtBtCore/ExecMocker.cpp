/*!
 * \file ExecMocker.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "ExecMocker.h"
#include "WtHelper.h"

#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSTradeDef.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/decimal.h"
#include "../Share/Converter.hpp"
#include "../WTSTools/WTSLogger.h"

#include <filesystem>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
namespace fs = std::filesystem;

#define PRICE_DOUBLE_TO_INT_P(x) ((int32_t)((x)*10000.0 + 0.5))
#define PRICE_DOUBLE_TO_INT_N(x) ((int32_t)((x)*10000.0 - 0.5))
#define PRICE_DOUBLE_TO_INT(x) (((x)==DBL_MAX)?0:((x)>0?PRICE_DOUBLE_TO_INT_P(x):PRICE_DOUBLE_TO_INT_N(x)))

extern uint32_t makeLocalOrderID();

class ExecMockerPluginCallbackScope
{
public:
	struct Frame
	{
		const ExecMocker* owner;
		Frame* previous;
	};

	explicit ExecMockerPluginCallbackScope(const ExecMocker* owner, bool allowWhenClosed = false)
		: _owner(owner), _entered(owner != NULL && owner->enterPluginCallback(allowWhenClosed)),
		  _frame{ owner, _top }
	{
		if (_entered)
			_top = &_frame;
	}

	~ExecMockerPluginCallbackScope()
	{
		if (_entered)
		{
			_top = _frame.previous;
			_owner->leavePluginCallback();
		}
	}

	bool entered() const { return _entered; }

	static bool activeOnCurrentThread(const ExecMocker* owner)
	{
		for (Frame* frame = _top; frame != NULL; frame = frame->previous)
		{
			if (frame->owner == owner)
				return true;
		}
		return false;
	}

private:
	const ExecMocker* _owner;
	bool _entered;
	Frame _frame;
	static thread_local Frame* _top;
};

thread_local ExecMockerPluginCallbackScope::Frame*
	ExecMockerPluginCallbackScope::_top = NULL;

namespace
{
	void set_init_error(std::string* output, const std::string& message);
}

ExecMocker::ExecMocker(HisDataReplayer* replayer)
	: _replayer(replayer)
	, _init_attempted(false)
	, _exec_unit(NULL)
	, _unit_initialized(false)
	, _quiesce_call_active(false)
	, _quiesce_request_attempted(false)
	, _quiesce_requested(false)
	, _quiesce_waited(false)
	, _quiesce_can_wait_ready(false)
	, _plugin_callbacks_closed(false)
	, _active_plugin_callbacks(0)
	, _position(0)
	, _undone(0)
	, _last_tick(NULL)
, _sig_px(DBL_MAX)
	, _ord_cnt(0)
	, _ord_qty(0)
	, _cacl_cnt(0)
	, _cacl_qty(0)
	, _sig_cnt(0)
	{
}

bool ExecMocker::beginQuiesceOperation(std::string* error)
{
	if (ExecMockerPluginCallbackScope::activeOnCurrentThread(this))
	{
		set_init_error(error, "cannot quiesce an execution plugin from its callback thread");
		return false;
	}

	std::lock_guard<std::mutex> lock(_quiesce_mutex);
	if (_quiesce_call_active)
	{
		set_init_error(error, "another execution plugin quiesce operation is active");
		return false;
	}
	_quiesce_call_active = true;
	return true;
}

void ExecMocker::endQuiesceOperation()
{
	std::lock_guard<std::mutex> lock(_quiesce_mutex);
	_quiesce_call_active = false;
}

bool ExecMocker::isQuiesceComplete() const
{
	std::lock_guard<std::mutex> lock(_quiesce_mutex);
	return _exec_unit == NULL || (_quiesce_waited && _active_plugin_callbacks == 0);
}

bool ExecMocker::enterPluginCallback(bool allowWhenClosed) const
{
	std::lock_guard<std::mutex> lock(_quiesce_mutex);
	if ((_plugin_callbacks_closed || _quiesce_call_active) && !allowWhenClosed &&
		!ExecMockerPluginCallbackScope::activeOnCurrentThread(this))
		return false;
	++_active_plugin_callbacks;
	return true;
}

void ExecMocker::leavePluginCallback() const
{
	std::lock_guard<std::mutex> lock(_quiesce_mutex);
	if (_active_plugin_callbacks == 0)
		std::terminate();
	--_active_plugin_callbacks;
}

bool ExecMocker::requestQuiesce(std::string* error)
{
	if (!beginQuiesceOperation(error))
		return false;
	auto finish = [this](bool result) { endQuiesceOperation(); return result; };

	if (!_exec_unit)
		return finish(true);
	if (!_factory._fact || !_factory._api)
	{
		set_init_error(error, "execution plugin unit has no retained factory or API table");
		return finish(false);
	}

	bool requestAttempted = false;
	bool alreadyRequested = false;
	bool wrongThread = false;
	{
		std::lock_guard<std::mutex> lock(_quiesce_mutex);
		alreadyRequested = _quiesce_waited || _quiesce_requested;
		requestAttempted = _quiesce_request_attempted;
		wrongThread = requestAttempted && _quiesce_thread != std::this_thread::get_id();
	}
	if (alreadyRequested)
		return finish(true);
	if (wrongThread)
	{
		set_init_error(error, "execution plugin quiesce retry must use its original control thread");
		return finish(false);
	}

	if (!requestAttempted)
	{
		// Stop admitting new unit callbacks before the factory preflight. The
		// probe itself is still allowed through the lifecycle callback scope.
		{
			std::lock_guard<std::mutex> lock(_quiesce_mutex);
			_plugin_callbacks_closed = true;
		}

		int32_t factoryPreflight = 0;
		try
		{
			ExecMockerPluginCallbackScope callback(this, true);
			if (!callback.entered())
			{
				{
					std::lock_guard<std::mutex> lock(_quiesce_mutex);
					_plugin_callbacks_closed = false;
				}
				set_init_error(error, "execution plugin factory preflight callback could not be entered");
				return finish(false);
			}
			factoryPreflight = _factory._api->can_wait_factory_quiesce(_factory._fact);
		}
		catch (...)
		{
			{
				std::lock_guard<std::mutex> lock(_quiesce_mutex);
				_plugin_callbacks_closed = false;
			}
			set_init_error(error, "execution plugin factory preflight raised an exception");
			return finish(false);
		}
		if (factoryPreflight != 1)
		{
			{
				std::lock_guard<std::mutex> lock(_quiesce_mutex);
				_plugin_callbacks_closed = false;
			}
			set_init_error(error, "execution plugin factory preflight did not approve the current thread");
			return finish(false);
		}

		std::lock_guard<std::mutex> lock(_quiesce_mutex);
		_quiesce_thread = std::this_thread::get_id();
		// A request may partially take effect before returning an error. Keep the
		// gate closed and retry the idempotent request on this same thread.
		_quiesce_request_attempted = true;
	}
	else
	{
		bool retryOnWrongThread = false;
		{
			std::lock_guard<std::mutex> lock(_quiesce_mutex);
			retryOnWrongThread = _quiesce_thread != std::this_thread::get_id();
		}
		if (retryOnWrongThread)
		{
			set_init_error(error, "execution plugin quiesce retry must use its original control thread");
			return finish(false);
		}
	}

	WtExecPluginV2Status status = WT_EXEC_PLUGIN_V2_INTERNAL_ERROR;
	try
	{
		ExecMockerPluginCallbackScope callback(this, true);
		if (!callback.entered())
			return finish(false);
		status = _factory._api->request_quiesce(_factory._fact, _exec_unit);
	}
	catch (...)
	{
		set_init_error(error, "execution plugin quiesce request raised an exception");
		return finish(false);
	}
	if (status != WT_EXEC_PLUGIN_V2_OK)
	{
		set_init_error(error, "execution plugin quiesce request failed");
		return finish(false);
	}

	{
		std::lock_guard<std::mutex> lock(_quiesce_mutex);
		_quiesce_requested = true;
	}
	return finish(true);
}

bool ExecMocker::canWaitQuiesce(std::string* error)
{
	if (!beginQuiesceOperation(error))
		return false;
	auto finish = [this](bool result) { endQuiesceOperation(); return result; };

	if (!_exec_unit)
		return finish(true);
	bool alreadyWaited = false;
	bool requestMissing = false;
	bool wrongThread = false;
	bool callbacksActive = false;
	{
		std::lock_guard<std::mutex> lock(_quiesce_mutex);
		alreadyWaited = _quiesce_waited;
		_quiesce_can_wait_ready = false;
		requestMissing = !_quiesce_requested;
		wrongThread = _quiesce_thread != std::this_thread::get_id();
		callbacksActive = _active_plugin_callbacks != 0;
	}
	if (alreadyWaited)
		return finish(true);
	if (requestMissing || wrongThread)
	{
		set_init_error(error, "execution plugin request must succeed on this thread before can-wait");
		return finish(false);
	}
	if (callbacksActive)
	{
		set_init_error(error, "execution plugin callbacks are still active; can-wait is unsafe");
		return finish(false);
	}

	int32_t canWait = 0;
	try
	{
		ExecMockerPluginCallbackScope callback(this, true);
		if (!callback.entered())
			return finish(false);
		canWait = _factory._api->can_wait_quiesce(_factory._fact, _exec_unit);
	}
	catch (...)
	{
		set_init_error(error, "execution plugin can-wait callback raised an exception");
		return finish(false);
	}
	if (canWait != 1)
	{
		set_init_error(error, "execution plugin did not approve waiting on the current thread");
		return finish(false);
	}

	{
		std::lock_guard<std::mutex> lock(_quiesce_mutex);
		_quiesce_can_wait_ready = true;
		_can_wait_thread = std::this_thread::get_id();
	}
	return finish(true);
}

bool ExecMocker::waitQuiesce(std::string* error)
{
	if (!beginQuiesceOperation(error))
		return false;
	auto finish = [this](bool result) { endQuiesceOperation(); return result; };

	if (!_exec_unit)
		return finish(true);
	bool alreadyWaited = false;
	bool preflightMissing = false;
	bool callbacksActive = false;
	{
		std::lock_guard<std::mutex> lock(_quiesce_mutex);
		alreadyWaited = _quiesce_waited;
		preflightMissing = !_quiesce_requested || !_quiesce_can_wait_ready ||
			_quiesce_thread != std::this_thread::get_id() ||
			_can_wait_thread != std::this_thread::get_id();
		callbacksActive = _active_plugin_callbacks != 0;
		if (!preflightMissing)
			_quiesce_can_wait_ready = false;
	}
	if (alreadyWaited)
		return finish(true);
	if (preflightMissing)
	{
		set_init_error(error, "execution plugin can-wait must approve this thread before wait");
		return finish(false);
	}
	if (callbacksActive)
	{
		set_init_error(error, "execution plugin callbacks became active before wait");
		return finish(false);
	}

	int32_t waited = 0;
	try
	{
		ExecMockerPluginCallbackScope callback(this, true);
		if (!callback.entered())
			return finish(false);
		waited = _factory._api->wait_quiesce(_factory._fact, _exec_unit);
	}
	catch (...)
	{
		set_init_error(error, "execution plugin wait callback raised an exception");
		return finish(false);
	}
	if (waited != 1)
	{
		set_init_error(error, "execution plugin wait callback did not confirm quiescence");
		return finish(false);
	}

	bool callbacksDrained = false;
	{
		std::lock_guard<std::mutex> lock(_quiesce_mutex);
		callbacksDrained = _active_plugin_callbacks == 0;
		if (callbacksDrained)
			_quiesce_waited = true;
	}
	if (!callbacksDrained)
	{
		set_init_error(error, "execution plugin callbacks remain active after wait");
		return finish(false);
	}
	return finish(true);
}

bool ExecMocker::quiesce(std::string* error)
{
	return requestQuiesce(error) && canWaitQuiesce(error) && waitQuiesce(error);
}

bool ExecMocker::canStopFromCurrentThread() const
{
	// A synchronous reentry from this object's plugin callback could self-join.
	// Callbacks active on other threads are not a reason to defer the control
	// thread: it must be able to stop/join replay work before the final drain.
	if (ExecMockerPluginCallbackScope::activeOnCurrentThread(this))
		return false;

	const WtExecPluginApiV2* api = NULL;
	void* factory = NULL;
	{
		std::unique_lock<std::mutex> lock(_quiesce_mutex, std::try_to_lock);
		if (!lock.owns_lock() || _quiesce_call_active)
			return false;
		if (!_factory._fact)
			return _factory._module_inst == NULL && _exec_unit == NULL;
		if (!_factory._api || !_factory._api->can_wait_factory_quiesce)
			return false;

		api = _factory._api;
		factory = _factory._fact;
		_quiesce_call_active = true;
	}

	int32_t factoryPreflight = 0;
	bool callbackCompleted = false;
	try
	{
		ExecMockerPluginCallbackScope callback(this, true);
		if (callback.entered())
		{
			factoryPreflight = api->can_wait_factory_quiesce(factory);
			callbackCompleted = true;
		}
	}
	catch (...)
	{
		callbackCompleted = false;
	}

	{
		std::lock_guard<std::mutex> lock(_quiesce_mutex);
		_quiesce_call_active = false;
	}
	return callbackCompleted && factoryPreflight == 1;
}

ExecMocker::~ExecMocker()
{
	if (_exec_unit)
	{
		if (!isQuiesceComplete() || !_factory._fact || !_factory._api)
			std::terminate();

		try
		{
			ExecMockerPluginCallbackScope callback(this, true);
			if (!callback.entered())
				std::terminate();
			_factory._api->destroy_unit(_factory._fact, _exec_unit);
		}
		catch (...) { std::terminate(); }
	}
	_exec_unit = NULL;

	if (_factory._fact)
	{
		if (!_factory._api || !_factory._api->destroy_factory)
			std::terminate();
		try
		{
			ExecMockerPluginCallbackScope callback(this, true);
			if (!callback.entered())
				std::terminate();
			_factory._api->destroy_factory(_factory._fact);
		}
		catch (...) { std::terminate(); }
		_factory._fact = NULL;
	}
	if (_factory._module_inst)
	{
		DLLHelper::free_library(_factory._module_inst);
		_factory._module_inst = NULL;
	}

	if (_last_tick)
		_last_tick->release();
}

namespace
{
	void set_init_error(std::string* output, const std::string& message)
	{
		if (output)
			*output = message;
	}

	bool valid_exec_plugin_api(const WtExecPluginApiV2* api)
	{
		if (!api)
			return false;

		// Read only the size prefix first. Do not inspect abi_version or any later
		// field until the advertised table proves that it is present.
		uint32_t tableSize = 0;
		std::memcpy(&tableSize, api, sizeof(tableSize));
		const uint32_t versionEnd = static_cast<uint32_t>(
			offsetof(WtExecPluginApiV2, abi_version) + sizeof(api->abi_version));
		if (tableSize < versionEnd)
			return false;

		uint32_t abiVersion = 0;
		std::memcpy(&abiVersion,
			reinterpret_cast<const char*>(api) + offsetof(WtExecPluginApiV2, abi_version),
			sizeof(abiVersion));
		const uint32_t abiMajor = abiVersion >> 16;
		const uint32_t abiMinor = abiVersion & 0xffffu;
		if (abiMajor != WT_EXEC_PLUGIN_V2_ABI_MAJOR || abiMinor < 2u ||
			tableSize < WT_EXEC_PLUGIN_V2_FACTORY_PREFLIGHT_SIZE)
			return false;

		const WtExecPluginV2Capabilities requiredCapabilities =
			WT_EXEC_PLUGIN_V2_CAP_QUIESCE |
			WT_EXEC_PLUGIN_V2_CAP_FACTORY_QUIESCE_PREFLIGHT;
		return api->reserved[0] == 0 && api->reserved[1] == 0 &&
			(api->capabilities & requiredCapabilities) == requiredCapabilities &&
			api->create_factory && api->destroy_factory && api->get_factory_name &&
			api->create_unit && api->destroy_unit && api->initialize_unit &&
			api->request_quiesce && api->can_wait_quiesce && api->wait_quiesce &&
			api->can_wait_factory_quiesce;
	}
}

bool ExecMocker::init(WTSVariant* cfg, std::string* error)
{
	if (_init_attempted)
	{
		set_init_error(error, "execution mocker initialization may only be attempted once");
		return false;
	}
	_init_attempted = true;

	if (cfg == NULL)
	{
		set_init_error(error, "execution mocker configuration is null");
		return false;
	}

	const char* module = cfg->getCString("module");
	if (module == NULL || module[0] == '\0')
	{
		set_init_error(error, "execution plugin module path is missing");
		return false;
	}

	const char* code = cfg->getCString("code");
	const char* period = cfg->getCString("period");
	if (!code || code[0] == '\0' || !period || period[0] == '\0')
	{
		set_init_error(error, "execution mocker code or period is missing");
		return false;
	}
	_code = code;
	_period = period;
	_volunit = cfg->getDouble("volunit");
	_volmode = cfg->getInt32("volmode");	//数量模式：0-反复正负，-1-一直卖，+1-一直买

	WTSVariant* cfgExec = cfg->get("executer");
	if (!cfgExec)
	{
		set_init_error(error, "execution unit configuration is missing");
		return false;
	}
	const char* name = cfgExec->getCString("name");
	if (!name || name[0] == '\0')
	{
		set_init_error(error, "execution unit name is missing");
		return false;
	}

	DllHandle hInst = DLLHelper::load_library(module);
	if (hInst == NULL)
	{
		set_init_error(error, std::string("loading execution plugin failed: ") + module);
		return false;
	}

	// Validate the versioned entry point before invoking any unversioned v1
	// factory functions. Old binary plugins may disagree on the deleter ABI.
	WtExecPluginV2GetterFn getter = reinterpret_cast<WtExecPluginV2GetterFn>(
		DLLHelper::get_symbol(hInst, WT_EXEC_PLUGIN_V2_GETTER_SYMBOL));
	if (!getter)
	{
		DLLHelper::free_library(hInst);
		set_init_error(error, "execution plugin has no verifiable V2 interface; registration was refused");
		return false;
	}

	const WtExecPluginApiV2* api = NULL;
	try
	{
		ExecMockerPluginCallbackScope callback(this);
		if (!callback.entered())
			throw std::runtime_error("plugin getter callback is closed");
		api = getter(WT_EXEC_PLUGIN_V2_ABI_MAJOR);
	}
	catch (...)
	{
		DLLHelper::free_library(hInst);
		set_init_error(error, "execution plugin V2 getter raised an exception");
		return false;
	}
	if (!valid_exec_plugin_api(api))
	{
		DLLHelper::free_library(hInst);
		set_init_error(error, "execution plugin V2 interface is incompatible or incomplete");
		return false;
	}

	_factory._module_inst = hInst;
	_factory._module_path = module;
	_factory._api = api;
	try
	{
		ExecMockerPluginCallbackScope callback(this);
		if (!callback.entered())
			throw std::runtime_error("plugin factory callback is closed");
		_factory._fact = api->create_factory();
	}
	catch (...)
	{
		set_init_error(error, "creating execution plugin factory raised an exception");
		return false;
	}
	if (_factory._fact == NULL)
	{
		set_init_error(error, "execution plugin factory creation failed");
		return false;
	}

	void* candidate = NULL;
	try
	{
		const char* factoryName = NULL;
		{
			ExecMockerPluginCallbackScope callback(this);
			if (!callback.entered())
				throw std::runtime_error("plugin factory callback is closed");
			factoryName = api->get_factory_name(_factory._fact);
		}
		if (!factoryName || factoryName[0] == '\0')
		{
			set_init_error(error, "execution plugin returned an empty factory name");
			return false;
		}

		{
			ExecMockerPluginCallbackScope callback(this);
			if (!callback.entered())
				throw std::runtime_error("plugin factory callback is closed");
			candidate = api->create_unit(_factory._fact, WT_EXEC_PLUGIN_V2_UNIT_PLAIN, name);
		}
		if (!candidate)
		{
			set_init_error(error, std::string("creating execution unit failed: ") + name);
			return false;
		}

		char errorText[512] = { 0 };
		WtExecPluginV2Status status = WT_EXEC_PLUGIN_V2_INTERNAL_ERROR;
		{
			ExecMockerPluginCallbackScope callback(this);
			if (!callback.entered())
				throw std::runtime_error("plugin initializer callback is closed");
			status = api->initialize_unit(
				_factory._fact, candidate, this, _code.c_str(), cfgExec->get("params"),
				errorText, static_cast<uint32_t>(sizeof(errorText)));
		}
		if (status != WT_EXEC_PLUGIN_V2_OK)
		{
			// A rejected unit is explicitly not initialized/registered by the ABI;
			// its only permitted operation is destruction, so do not send quiesce hooks.
			try
			{
				ExecMockerPluginCallbackScope callback(this);
				if (!callback.entered())
					std::terminate();
				api->destroy_unit(_factory._fact, candidate);
			}
			catch (...) { std::terminate(); }
			candidate = NULL;
			std::string message = errorText[0] ? errorText : "execution unit initialization failed";
			set_init_error(error, message);
			return false;
		}
		_exec_unit = static_cast<ExecuteUnit*>(candidate);
		candidate = NULL;
		const char* id = cfgExec->getCString("id");
		if (id)
			_id = id;
		_unit_initialized = true;
	}
	catch (...)
	{
		if (candidate)
			std::terminate(); // The plugin threw across an ABI callback; teardown state is unknowable.
		set_init_error(error, "execution plugin callback raised an exception");
		return false;
	}

	try
	{
		_matcher.init(cfg->get("matcher"));
		_matcher.regisSink(this);
	}
	catch (...)
	{
		set_init_error(error, "initializing the execution matcher failed");
		return false;
	}

	return true;
}

WTSCommodityInfo* ExecMocker::getCommodityInfo(const char* stdCode)
{
	return _replayer->get_commodity_info(stdCode);
}

WTSSessionInfo* ExecMocker::getSessionInfo(const char* stdCode)
{
	return _replayer->get_session_info(stdCode, true);
}

uint64_t ExecMocker::getCurTime()
{
	return TimeUtils::makeTime(_replayer->get_date(), _replayer->get_raw_time() * 100000 + _replayer->get_secs());
}

void ExecMocker::handle_bar_close(const char* /*stdCode*/, const char* /*period*/, uint32_t /*times*/, WTSBarStruct* /*newBar*/)
{
	//throw std::logic_error("The method or operation is not implemented.");
}

void ExecMocker::handle_session_begin(uint32_t /*curTDate*/)
{
	//throw std::logic_error("The method or operation is not implemented.");
}

void ExecMocker::handle_session_end(uint32_t /*curTDate*/)
{
	_matcher.clear();
	_undone = 0;

	WTSLogger::info("Total entrust:{}, total quantity:{}, total cancels:{}, total cancel quantity:{}, total signals:{}", 
		_ord_cnt, _ord_qty, _cacl_cnt, _cacl_qty, _sig_cnt);
}

void ExecMocker::handle_tick(const char* stdCode, WTSTickData* curTick, uint32_t /*pxType*/)
{
 	if (_last_tick)
	{
		_last_tick->release();
		_last_tick = NULL;
	}

	_last_tick = curTick;
	_last_tick->retain();
	
	_matcher.handle_tick(stdCode, curTick);

	if (_exec_unit && _unit_initialized)
	{
		ExecMockerPluginCallbackScope callback(this);
		if (callback.entered())
			_exec_unit->on_tick(curTick);
	}
}

void ExecMocker::handle_init()
{
	if (!_exec_unit || !_unit_initialized)
		return;

	thread_local static char basePeriod[2] = { 0 };
	basePeriod[0] = _period[0];
	uint32_t times = 1;
	if (_period.size() > 1)
		times = convert::to_uint32(_period.c_str() + 1);

	WTSKlineSlice* kline = _replayer->get_kline_slice(_code.c_str(), basePeriod,  10, times, true);
	if (kline)
		kline->release();

	_replayer->sub_tick(0, _code.c_str());

	_trade_logs << "localid,signaltime,ordertime,bs,sigprice,ordprice,lmtprice,tradetime,trdprice,qty,sigtimespan,exectime,cancel" << std::endl;

	{
		ExecMockerPluginCallbackScope callback(this);
		if (callback.entered())
			_exec_unit->on_channel_ready();
	}

	_sig_time = (uint64_t)_replayer->get_date() * 10000 + _replayer->get_raw_time();

	{
		ExecMockerPluginCallbackScope callback(this);
		if (callback.entered())
			_exec_unit->set_position(_code.c_str(), _volunit);
	}
	WTSLogger::info("Target position updated at the beginning: {}", _volunit);
}

void ExecMocker::handle_schedule(uint32_t uDate, uint32_t uTime)
{
	if (!_exec_unit || !_unit_initialized)
		return;
	if (uTime == 1500)
		return;

	_sig_px = _last_tick->price();
	if (_sig_px == DBL_MAX || _sig_px == FLT_MAX)
		_sig_px = _last_tick->preclose();

	_sig_time = (uint64_t)uDate * 10000 + uTime;
	if(_volmode == 0)
	{
		if (_position <= 0)
			_target = _volunit;
		else
			_target = -_volunit;
	}
	else if (_volmode == -1)
	{
		_target -= _volunit;
	}
	else if (_volmode == 1)
	{
		_target += _volunit;
	}

	{
		ExecMockerPluginCallbackScope callback(this);
		if (callback.entered())
			_exec_unit->set_position(_code.c_str(), _target);
	}
	WTSLogger::info("Target position updated @{}.{}: {}", uDate, uTime, _volunit);
	_sig_cnt++;
}

WTSTickSlice* ExecMocker::getTicks(const char* stdCode, uint32_t count, uint64_t etime /*= 0*/)
{
	return _replayer->get_tick_slice(stdCode, count, etime);
}

WTSTickData* ExecMocker::grabLastTick(const char* stdCode)
{
	return _replayer->get_last_tick(stdCode);
}

double ExecMocker::getPosition(const char* /*stdCode*/, bool /*validOnly*/ /* = true */, int32_t /*flag*/ /* = 3 */)
{
	return _position;
}

OrderMap* ExecMocker::getOrders(const char* stdCode)
{
	OrderMap* orders = OrderMap::create();
	const std::vector<MatchEngine::OrderSnapshot> snapshots = _matcher.get_order_snapshots(stdCode);
	for (const MatchEngine::OrderSnapshot& snapshot : snapshots)
	{
		WTSOrderInfo* orderInfo = WTSOrderInfo::create();
		const char* exchangeEnd = strchr(snapshot.code, '.');
		if (exchangeEnd != NULL && exchangeEnd != snapshot.code &&
			(size_t)(exchangeEnd - snapshot.code) < MAX_EXCHANGE_LENGTH)
			orderInfo->setExchange(snapshot.code, (size_t)(exchangeEnd - snapshot.code));
		else
			orderInfo->setExchange("");
		orderInfo->setCode(snapshot.code);
		orderInfo->setDirection(snapshot.buy ? WDT_LONG : WDT_SHORT);
		orderInfo->setPriceType(WPT_LIMITPRICE);
		orderInfo->setOrderFlag(WOF_NOR);
		orderInfo->setOffsetType(WOT_OPEN);
		orderInfo->setBusinessType(BT_CASH);
		orderInfo->m_bIsNet = false;
		orderInfo->m_bIsBuy = snapshot.buy;
		orderInfo->setVolume(snapshot.qty);
		orderInfo->setVolLeft(snapshot.left);
		orderInfo->setVolTraded(snapshot.traded);
		orderInfo->setPrice(snapshot.price);
		const uint32_t orderDate = (uint32_t)(snapshot.time / 1000000000ULL);
		const uint64_t timeWithinDate = snapshot.time % 1000000000ULL;
		orderInfo->setOrderDate(orderDate);
		orderInfo->setOrderTime(orderDate == 0 ? 0 :
			(uint64_t)TimeUtils::makeTime((long)orderDate, (long)timeWithinDate));
		orderInfo->setEntrustID("");
		orderInfo->setUserTag("");
		orderInfo->setStateMsg("");
		orderInfo->setError(WOEF_None);
		orderInfo->setOrderType(WORT_Normal);
		orderInfo->setContractInfo(NULL);
		orderInfo->setExtras(NULL);
		const std::string localid = std::to_string(snapshot.localid);
		orderInfo->setOrderID(localid.c_str());

		switch (snapshot.state)
		{
		case 0:
			orderInfo->setOrderState(WOS_Submitting);
			break;
		case 1:
			orderInfo->setOrderState(snapshot.traded > 0.0 ?
				WOS_PartTraded_Queuing : WOS_NotTraded_Queuing);
			break;
		case 9:
			orderInfo->setOrderState(WOS_Cancelling);
			break;
		}

		// OrderMap owns a retained reference; drop the creator's reference so
		// releasing the returned map also releases every snapshot order.
		orders->add(snapshot.localid, orderInfo);
		orderInfo->release();
	}

	return orders;
}

double ExecMocker::getUndoneQty(const char* /*stdCode*/)
{
	return _undone;
}

OrderIDs ExecMocker::buy(const char* stdCode, double price, double qty, bool /*bForceClose*/ /* = false */)
{
	uint64_t curTime = (uint64_t)_replayer->get_date() * 1000000000 + (uint64_t)_replayer->get_raw_time() * 100000 + _replayer->get_secs();
	OrderIDs ret = _matcher.buy(stdCode, price, qty, curTime);

	if(!ret.empty())
	{
		_ord_cnt++;
		_ord_qty += qty;

		_undone += (int32_t)qty;
		WTSLogger::info("{}, undone orders updated: {}", __FUNCTION__, _undone);
	}

	return ret;
}

OrderIDs ExecMocker::sell(const char* stdCode, double price, double qty, bool /*bForceClose*/ /* = false */)
{
	uint64_t curTime = (uint64_t)_replayer->get_date() * 1000000000 + (uint64_t)_replayer->get_raw_time() * 100000 + _replayer->get_secs();
	OrderIDs ret = _matcher.sell(stdCode, price, qty, curTime);

	if(!ret.empty())
	{
		_ord_cnt++;
		_ord_qty += qty;
	
		_undone -= (int32_t)qty;
		WTSLogger::info("{}, undone orders updated: {}", __FUNCTION__, _undone);
	}

	return ret;
}

bool ExecMocker::cancel(uint32_t localid)
{
	double change = _matcher.cancel(localid);
	if (decimal::eq(change, 0))
		return false;

	_undone -= change;
	_cacl_cnt++;
	_cacl_qty += abs(change);
	WTSLogger::info("{}, undone orders updated: {}", __FUNCTION__, _undone);

	return true;
}

OrderIDs ExecMocker::cancel(const char* stdCode, bool isBuy, double qty /*= 0*/)
{
	OrderIDs ret = _matcher.cancel(stdCode, isBuy, qty, [this](double change) {
		_undone -= change;

		_cacl_cnt++;
		_cacl_qty += abs(change);
	});
	WTSLogger::info("{}, undone orders updated: {}", __FUNCTION__, _undone);

	return ret;
}

void ExecMocker::writeLog(const char* message)
{
	WTSLogger::log_dyn_raw("executer", _id.c_str(), LL_INFO, message);
}

void ExecMocker::handle_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message, uint64_t /*ordTime*/)
{
	if (!_exec_unit || !_unit_initialized)
		return;
	ExecMockerPluginCallbackScope callback(this);
	if (callback.entered())
		_exec_unit->on_entrust(localid, stdCode, bSuccess, message);
}

void ExecMocker::handle_order(uint32_t localid, const char* stdCode, bool isBuy, double leftover, double price, bool isCanceled, uint64_t ordTime)
{
	uint64_t curTime = (uint64_t)_last_tick->actiondate() * 1000000000 + _last_tick->actiontime();
	uint64_t curUnixTime = TimeUtils::makeTime(_last_tick->actiondate(), _last_tick->actiontime());

	uint64_t sigUnixTime = TimeUtils::makeTime((uint32_t)(_sig_time / 10000), _sig_time % 10000 * 100000);
	uint64_t ordUnixTime = TimeUtils::makeTime((uint32_t)(ordTime / 1000000000), ordTime % 1000000000);

	if(isCanceled)
	{
		if (_sig_px == DBL_MAX)
			_sig_px = _last_tick->preclose();

		_trade_logs << fmt::format("{},{},{},{},{},{},{},{},{},{},{},{},true\n", localid, _sig_time,
			ordTime, (isBuy ? "B" : "S"), _sig_px, 0, price, curTime, price, 0,
			curUnixTime - sigUnixTime, curUnixTime - ordUnixTime);

		_undone -= leftover * (isBuy ? 1 : -1);
		WTSLogger::info("{}, undone orders updated: {}", __FUNCTION__, _undone);
	}

	if (_exec_unit && _unit_initialized)
	{
		ExecMockerPluginCallbackScope callback(this);
		if (callback.entered())
			_exec_unit->on_order(localid, stdCode, isBuy, leftover, price, isCanceled);
	}
}

void ExecMocker::handle_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double fireprice, double price, uint64_t ordTime)
{
	uint64_t curTime = (uint64_t)_last_tick->actiondate() * 1000000000 + _last_tick->actiontime();
	uint64_t curUnixTime = TimeUtils::makeTime(_last_tick->actiondate(), _last_tick->actiontime());

	uint64_t sigUnixTime = TimeUtils::makeTime((uint32_t)(_sig_time / 10000), _sig_time % 10000 * 100000);
	uint64_t ordUnixTime = TimeUtils::makeTime((uint32_t)(ordTime / 1000000000), ordTime % 1000000000);

	if (_sig_px == DBL_MAX)
		_sig_px = _last_tick->preclose();

	_trade_logs << fmt::format("{},{},{},{},{},{},{},{},{},{},{},{},false\n", localid, _sig_time,
		ordTime, (isBuy ? "B" : "S"), _sig_px, fireprice, price, curTime, price, vol,
		curUnixTime - sigUnixTime, curUnixTime - ordUnixTime);

	_position += vol* (isBuy?1:-1);
	_undone -= vol * (isBuy ? 1 : -1);
	WTSLogger::info("{}, undone orders updated: {}", __FUNCTION__, _undone);
	WTSLogger::info("Position updated: {}", _position);

	if (_exec_unit && _unit_initialized)
	{
		ExecMockerPluginCallbackScope callback(this);
		if (callback.entered())
			_exec_unit->on_trade(localid, stdCode, isBuy, vol, price);
	}
}

void ExecMocker::handle_replay_done()
{
	std::string folder = WtHelper::getOutputDir();
	folder += "exec/";
	fs::create_directories(folder.c_str());

	std::stringstream ss;
	ss << folder << "trades_" << _id << ".csv";
	std::string filename = ss.str();
	StdFile::write_file_content(filename.c_str(), _trade_logs.str());
}
