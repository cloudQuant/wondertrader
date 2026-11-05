#include "WtExecuterFactory.h"

#include "../Share/StdUtils.hpp"
#include "../Share/StrUtil.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../WTSTools/WTSLogger.h"

#include <filesystem>
#include <cstddef>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <exception>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

USING_NS_WTP;

namespace
{
struct FactoryLeaseMarker
{
	const void* state;
	FactoryLeaseMarker* previous;
};

thread_local FactoryLeaseMarker* g_factory_lease_marker = NULL;
}

class wtp::ExeFactoryAdmissionState
{
public:
	ExeFactoryAdmissionState() : _config_state(0), _unit_state(0) {}

	bool tryEnter(bool config)
	{
		std::atomic<uint64_t>& state = config ? _config_state : _unit_state;
		uint64_t current = state.load(std::memory_order_acquire);
		for (;;)
		{
			if ((current & kClosedBit) != 0 || (current & kCountMask) == kCountMask)
				return false;
			if (state.compare_exchange_weak(current, current + 1,
				std::memory_order_acq_rel, std::memory_order_acquire))
				return true;
		}
	}

	void leave(bool config) noexcept
	{
		std::atomic<uint64_t>& state = config ? _config_state : _unit_state;
		{
			std::lock_guard<std::mutex> lock(_wait_mutex);
			state.fetch_sub(1, std::memory_order_acq_rel);
		}
		_condition.notify_all();
	}

	void closeConfig() noexcept
	{
		_config_state.fetch_or(kClosedBit, std::memory_order_acq_rel);
		_condition.notify_all();
	}

	bool closeUnits() noexcept
	{
		_unit_state.fetch_or(kClosedBit, std::memory_order_acq_rel);
		_condition.notify_all();
		return true;
	}

	bool configClosedAndIdle() const noexcept
	{
		const uint64_t state = _config_state.load(std::memory_order_acquire);
		return (state & kClosedBit) != 0 && (state & kCountMask) == 0;
	}

	bool unitsClosedAndIdle() const noexcept
	{
		const uint64_t state = _unit_state.load(std::memory_order_acquire);
		return (state & kClosedBit) != 0 && (state & kCountMask) == 0;
	}

	bool unitsClosed() const noexcept
	{
		return (_unit_state.load(std::memory_order_acquire) & kClosedBit) != 0;
	}

	bool currentThreadHasLease() const noexcept
	{
		for (FactoryLeaseMarker* marker = g_factory_lease_marker;
			marker != NULL; marker = marker->previous)
		{
			if (marker->state == this)
				return true;
		}
		return false;
	}

	bool waitForUnitsIdle()
	{
		if (currentThreadHasLease())
			return false;
		std::unique_lock<std::mutex> lock(_wait_mutex);
		_condition.wait(lock, [this]() {
			return (_unit_state.load(std::memory_order_acquire) & kCountMask) == 0;
		});
		return true;
	}

private:
	static const uint64_t kClosedBit = uint64_t(1) << 63;
	static const uint64_t kCountMask = ~kClosedBit;
	std::atomic<uint64_t> _config_state;
	std::atomic<uint64_t> _unit_state;
	std::mutex _wait_mutex;
	std::condition_variable _condition;
};

const uint64_t wtp::ExeFactoryAdmissionState::kClosedBit;
const uint64_t wtp::ExeFactoryAdmissionState::kCountMask;

class wtp::ExePluginV2State
{
public:
	ExePluginV2State(DllHandle module, const WtExecPluginApiV2* api,
		const std::string& path)
		: _module(module), _api(*api), _factory(NULL), _path(path)
	{
	}

	~ExePluginV2State()
	{
		if (_factory != NULL && _api.destroy_factory != NULL)
		{
			try
			{
				_api.destroy_factory(_factory);
			}
			catch (...)
			{
				WTSLogger::error("V2 execution plugin {} threw while destroying its factory", _path);
			}
			_factory = NULL;
		}

		if (_module != NULL)
		{
			DLLHelper::free_library(_module);
			_module = NULL;
		}
	}

	DllHandle _module;
	// Snapshot the validated table while the module is pinned. Later callbacks
	// never reread fields from plugin-owned memory whose claimed size could have
	// changed after registration.
	WtExecPluginApiV2 _api;
	void* _factory;
	std::string _path;
};

namespace
{
const uint32_t kErrorCapacity = 512;

class FactoryOperationLease
{
public:
	FactoryOperationLease(const std::shared_ptr<ExeFactoryAdmissionState>& admissions,
		bool config)
		: _admissions(admissions), _config(config), _acquired(false)
	{
		if (_admissions == NULL || !_admissions->tryEnter(_config))
			return;
		_marker.state = _admissions.get();
		_marker.previous = g_factory_lease_marker;
		g_factory_lease_marker = &_marker;
		_acquired = true;
	}

	~FactoryOperationLease()
	{
		if (!_acquired)
			return;
		FactoryLeaseMarker** current = &g_factory_lease_marker;
		while (*current != NULL && *current != &_marker)
			current = &(*current)->previous;
		if (*current == &_marker)
			*current = _marker.previous;
		_admissions->leave(_config);
	}

	bool acquired() const { return _acquired; }

private:
	std::shared_ptr<ExeFactoryAdmissionState> _admissions;
	bool _config;
	bool _acquired;
	FactoryLeaseMarker _marker;
};

class FactoryThreadMarkerScope
{
public:
	explicit FactoryThreadMarkerScope(const void* state)
	{
		_marker.state = state;
		_marker.previous = g_factory_lease_marker;
		g_factory_lease_marker = &_marker;
	}

	~FactoryThreadMarkerScope()
	{
		FactoryLeaseMarker** current = &g_factory_lease_marker;
		while (*current != NULL && *current != &_marker)
			current = &(*current)->previous;
		if (*current == &_marker)
			*current = _marker.previous;
	}

private:
	FactoryLeaseMarker _marker;
};

bool validateApi(const WtExecPluginApiV2* api, std::string& why)
{
	if (api == NULL)
	{
		why = "V2 getter returned a null API table";
		return false;
	}

	const uint32_t tableSize = api->struct_size;
	const auto covers = [tableSize](size_t offset, size_t fieldSize) {
		return offset <= tableSize && fieldSize <= tableSize - offset;
	};

	if (!covers(offsetof(WtExecPluginApiV2, abi_version), sizeof(api->abi_version)))
	{
		why = "V2 API table does not cover the ABI version field";
		return false;
	}

	if (tableSize < WT_EXEC_PLUGIN_V2_REQUIRED_SIZE)
	{
		why = "V2 API table is too small";
		return false;
	}

	const uint32_t abiMajor = api->abi_version >> 16;
	const uint32_t abiMinor = api->abi_version & 0xffffu;
	if (abiMajor != WT_EXEC_PLUGIN_V2_ABI_MAJOR)
	{
		why = "V2 ABI major version mismatch";
		return false;
	}
	if (abiMinor < WT_EXEC_PLUGIN_V2_ABI_MINOR)
	{
		why = "V2 plugin does not provide the ABI 2.2 factory preflight contract";
		return false;
	}

	if (api->reserved[0] != 0 || api->reserved[1] != 0)
	{
		why = "V2 reserved fields must be zero";
		return false;
	}

	if (tableSize < WT_EXEC_PLUGIN_V2_QUIESCE_SIZE ||
		!covers(offsetof(WtExecPluginApiV2, capabilities), sizeof(api->capabilities)) ||
		!covers(offsetof(WtExecPluginApiV2, request_quiesce), sizeof(api->request_quiesce)) ||
		!covers(offsetof(WtExecPluginApiV2, can_wait_quiesce), sizeof(api->can_wait_quiesce)) ||
		!covers(offsetof(WtExecPluginApiV2, wait_quiesce), sizeof(api->wait_quiesce)))
	{
		why = "V2 API table does not cover the complete quiesce extension";
		return false;
	}

	if ((api->capabilities & WT_EXEC_PLUGIN_V2_CAP_QUIESCE) == 0)
	{
		why = "V2 plugin does not advertise the quiesce capability";
		return false;
	}
	if (api->request_quiesce == NULL || api->can_wait_quiesce == NULL ||
		api->wait_quiesce == NULL)
	{
		why = "V2 API table is missing a required quiesce callback";
		return false;
	}

	if (tableSize < WT_EXEC_PLUGIN_V2_FACTORY_PREFLIGHT_SIZE ||
		!covers(offsetof(WtExecPluginApiV2, can_wait_factory_quiesce),
			sizeof(api->can_wait_factory_quiesce)))
	{
		why = "V2 API table does not cover the factory preflight callback";
		return false;
	}
	if ((api->capabilities & WT_EXEC_PLUGIN_V2_CAP_FACTORY_QUIESCE_PREFLIGHT) == 0)
	{
		why = "V2 plugin does not advertise the factory preflight capability";
		return false;
	}
	if (api->can_wait_factory_quiesce == NULL)
	{
		why = "V2 API table is missing the factory preflight callback";
		return false;
	}

	if (api->create_factory == NULL || api->destroy_factory == NULL ||
		api->get_factory_name == NULL || api->create_unit == NULL ||
		api->destroy_unit == NULL || api->initialize_unit == NULL)
	{
		why = "V2 API table is missing a required callback";
		return false;
	}

	return true;
}

bool destroyUnitNoThrow(const std::shared_ptr<ExePluginV2State>& plugin, void* unit) noexcept
{
	if (plugin == NULL || unit == NULL)
		return false;

	try
	{
		plugin->_api.destroy_unit(plugin->_factory, unit);
		return true;
	}
	catch (...)
	{
		try
		{
			WTSLogger::error("V2 execution plugin {} threw while destroying a unit", plugin->_path);
		}
		catch (...)
		{
		}
		return false;
	}
}

class PendingUnitGuard
{
public:
	PendingUnitGuard(const std::shared_ptr<ExePluginV2State>& plugin, void* unit)
		: _plugin(plugin), _unit(unit)
	{
	}

	~PendingUnitGuard()
	{
		destroyUnitNoThrow(_plugin, _unit);
	}

	void release() { _unit = NULL; }

private:
	std::shared_ptr<ExePluginV2State> _plugin;
	void* _unit;
};
}

namespace
{
struct ContextGateOperationMarker
{
	const wtp::ExeUnitContextGate* gate;
	ContextGateOperationMarker* previous;
};

thread_local ContextGateOperationMarker* g_context_gate_operation = NULL;
}

/**
 * A unit-local ExecuteContext view. Order-changing calls are admitted only in
 * ACTIVE and counted until their host callback returns. Quiesce closes
 * admission atomically, then drains the count without holding a lock across
 * plugin or host calls.
 */
class wtp::ExeUnitContextGate : public ExecuteContext
{
public:
	explicit ExeUnitContextGate(ExecuteContext* context)
		: _context(context), _state(stateWord(Phase::INITIALIZING, 0)), _in_flight_mutex(), _in_flight_condition()
	{
	}

	void activate() noexcept
	{
		uint64_t current = _state.load(std::memory_order_acquire);
		while (phaseOf(current) == Phase::INITIALIZING)
		{
			const uint64_t desired = withPhase(current, Phase::ACTIVE);
			if (_state.compare_exchange_weak(current, desired,
				std::memory_order_acq_rel, std::memory_order_acquire))
				return;
		}
	}

	void beginQuiesce() noexcept
	{
		uint64_t current = _state.load(std::memory_order_acquire);
		while (phaseOf(current) != Phase::QUIESCING && phaseOf(current) != Phase::CLOSED)
		{
			const uint64_t desired = withPhase(current, Phase::QUIESCING);
			if (_state.compare_exchange_weak(current, desired,
				std::memory_order_acq_rel, std::memory_order_acquire))
				return;
		}
	}

	void close() noexcept
	{
		uint64_t current = _state.load(std::memory_order_acquire);
		while (phaseOf(current) != Phase::CLOSED)
		{
			const uint64_t desired = withPhase(current, Phase::CLOSED);
			if (_state.compare_exchange_weak(current, desired,
				std::memory_order_acq_rel, std::memory_order_acquire))
				return;
		}
	}

	bool canWaitForOperationsOnCurrentThread() const noexcept
	{
		for (ContextGateOperationMarker* marker = g_context_gate_operation;
			marker != NULL; marker = marker->previous)
		{
			if (marker->gate == this)
				return false;
		}
		return true;
	}

	bool waitForOperations() noexcept
	{
		if (!canWaitForOperationsOnCurrentThread())
			return false;
		try
		{
			std::unique_lock<std::mutex> lock(_in_flight_mutex);
			_in_flight_condition.wait(lock, [this]() {
				return inFlightCount(_state.load(std::memory_order_acquire)) == 0;
			});
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	WTSTickSlice* getTicks(const char* stdCode, uint32_t count, uint64_t etime = 0) override
	{
		return _context == NULL ? NULL : _context->getTicks(stdCode, count, etime);
	}
	WTSTickData* grabLastTick(const char* stdCode) override
	{
		return _context == NULL ? NULL : _context->grabLastTick(stdCode);
	}
	double getPosition(const char* stdCode, bool validOnly = true, int32_t flag = 3) override
	{
		return _context == NULL ? 0.0 : _context->getPosition(stdCode, validOnly, flag);
	}
	OrderMap* getOrders(const char* stdCode) override
	{
		return _context == NULL ? NULL : _context->getOrders(stdCode);
	}
	double getUndoneQty(const char* stdCode) override
	{
		return _context == NULL ? 0.0 : _context->getUndoneQty(stdCode);
	}
	OrderIDs buy(const char* stdCode, double price, double qty,
		bool bForceClose = false) override
	{
		Operation operation(this);
		if (!operation)
		{
			reportRejectedOperation("buy");
			return OrderIDs();
		}
		return _context == NULL ? OrderIDs() : _context->buy(stdCode, price, qty, bForceClose);
	}
	OrderIDs sell(const char* stdCode, double price, double qty,
		bool bForceClose = false) override
	{
		Operation operation(this);
		if (!operation)
		{
			reportRejectedOperation("sell");
			return OrderIDs();
		}
		return _context == NULL ? OrderIDs() : _context->sell(stdCode, price, qty, bForceClose);
	}
	bool cancel(uint32_t localid) override
	{
		Operation operation(this);
		if (!operation)
		{
			reportRejectedOperation("cancel");
			return false;
		}
		return _context != NULL && _context->cancel(localid);
	}
	OrderIDs cancel(const char* stdCode, bool isBuy, double qty = 0) override
	{
		Operation operation(this);
		if (!operation)
		{
			reportRejectedOperation("cancel-by-contract");
			return OrderIDs();
		}
		return _context == NULL ? OrderIDs() : _context->cancel(stdCode, isBuy, qty);
	}
	void writeLog(const char* message) override
	{
		if (_context != NULL)
			_context->writeLog(message);
	}
	WTSCommodityInfo* getCommodityInfo(const char* stdCode) override
	{
		return _context == NULL ? NULL : _context->getCommodityInfo(stdCode);
	}
	WTSSessionInfo* getSessionInfo(const char* stdCode) override
	{
		return _context == NULL ? NULL : _context->getSessionInfo(stdCode);
	}
	uint64_t getCurTime() override
	{
		return _context == NULL ? 0 : _context->getCurTime();
	}
	bool registerTimer(const char*, uint32_t) override
	{
		// ExecuteContext has no timer-unregister operation. A V2 unit could
		// otherwise leave an accepted host callback behind after failed init or
		// quiescence, so timer registration is intentionally unsupported here.
		reportRejectedOperation("registerTimer (no V2 unregister contract)");
		return false;
	}

private:
	enum class Phase : uint64_t
	{
		INITIALIZING = 0,
		ACTIVE = 1,
		QUIESCING = 2,
		CLOSED = 3
	};
	static const uint64_t kPhaseShift = 62;
	static const uint64_t kCountMask = (uint64_t(1) << kPhaseShift) - 1;

	static uint64_t stateWord(Phase phase, uint64_t count) noexcept
	{
		return (static_cast<uint64_t>(phase) << kPhaseShift) | (count & kCountMask);
	}
	static Phase phaseOf(uint64_t state) noexcept
	{
		return static_cast<Phase>(state >> kPhaseShift);
	}
	static uint64_t withPhase(uint64_t state, Phase phase) noexcept
	{
		return stateWord(phase, inFlightCount(state));
	}
	static uint64_t inFlightCount(uint64_t state) noexcept
	{
		return state & kCountMask;
	}

	bool tryEnterOperation() noexcept
	{
		uint64_t current = _state.load(std::memory_order_acquire);
		for (;;)
		{
			if (phaseOf(current) != Phase::ACTIVE || inFlightCount(current) == kCountMask)
				return false;
			if (_state.compare_exchange_weak(current, current + 1,
				std::memory_order_acq_rel, std::memory_order_acquire))
				return true;
		}
	}

	void finishOperation() noexcept
	{
		try
		{
			{
				std::lock_guard<std::mutex> lock(_in_flight_mutex);
				_state.fetch_sub(1, std::memory_order_acq_rel);
			}
			_in_flight_condition.notify_all();
		}
		catch (...)
		{
			// Failing to update the drain counter must leave quiescence incomplete.
		}
	}

	class Operation
	{
	public:
		explicit Operation(ExeUnitContextGate* gate)
			: _gate(gate), _entered(gate->tryEnterOperation()), _marker{ nullptr, nullptr }
		{
			if (_entered)
			{
				_marker.gate = gate;
				_marker.previous = g_context_gate_operation;
				g_context_gate_operation = &_marker;
			}
		}
		Operation(const Operation&) = delete;
		Operation& operator=(const Operation&) = delete;
		~Operation()
		{
			if (!_entered)
				return;
			ContextGateOperationMarker** current = &g_context_gate_operation;
			while (*current != NULL && *current != &_marker)
				current = &(*current)->previous;
			if (*current == &_marker)
				*current = _marker.previous;
			_gate->finishOperation();
		}
		explicit operator bool() const { return _entered; }
	private:
		ExeUnitContextGate* _gate;
		bool _entered;
		ContextGateOperationMarker _marker;
	};

	void reportRejectedOperation(const char* operation) noexcept
	{
		if (_context == NULL || phaseOf(_state.load(std::memory_order_acquire)) == Phase::CLOSED)
			return;
		try
		{
			char message[160] = { 0 };
			std::snprintf(message, sizeof(message), "V2 execution-unit context rejected %s", operation);
			_context->writeLog(message);
		}
		catch (...)
		{
			// Rejection remains effective if diagnostic logging fails.
		}
	}

	ExecuteContext* _context;
	std::atomic<uint64_t> _state;
	std::mutex _in_flight_mutex;
	std::condition_variable _in_flight_condition;
};

WtExecuterFactory::WtExecuterFactory()
	: _admissions(std::make_shared<ExeFactoryAdmissionState>())
{
}

ExeUnitWrapper::ExeUnitWrapper(ExecuteUnit* unitPtr, void* unitHandle,
	const std::shared_ptr<ExePluginV2State>& plugin,
	const std::shared_ptr<ExeFactoryAdmissionState>& admissions)
	: _unit(unitPtr), _unit_handle(unitHandle), _plugin(plugin),
	  _admissions(admissions), _init_context(), _legacy_fact(NULL), _context_identity(NULL),
	  _init_attempted(false), _failed_init_cleanup_proven(false), _initialized(false),
	  _quiesce_requested(false), _quiesce_request_in_progress(false),
	  _quiesce_request_succeeded(false), _quiesce_operation_in_progress(false),
	  _quiesced(false)
{
}

ExeUnitWrapper::ExeUnitWrapper(ExecuteUnit* unitPtr, IExecuterFact* fact)
	: _unit(unitPtr), _unit_handle(NULL), _admissions(), _init_context(),
	  _legacy_fact(fact), _context_identity(NULL), _init_attempted(false), _failed_init_cleanup_proven(false),
	  _initialized(false), _quiesce_requested(false),
	  _quiesce_request_in_progress(false), _quiesce_request_succeeded(false),
	  _quiesce_operation_in_progress(false), _quiesced(false)
{
}

ExeUnitWrapper::~ExeUnitWrapper()
{
	bool unsafeToDestroy = false;
	void* unitHandle = NULL;
	{
		std::lock_guard<std::mutex> lock(_lifecycle_mutex);
		unsafeToDestroy = _initialized && !_quiesced;
		unitHandle = _unit_handle;
	}
	if (unsafeToDestroy)
	{
		try
		{
			WTSLogger::error("V2 execution unit was destroyed before successful quiescence; terminating to keep plugin code and borrowed context alive");
		}
		catch (...)
		{
		}
		// A plugin worker may still use both its unit and the host-owned
		// ExecuteContext. Pinning only the module cannot preserve that owner;
		// terminating is the only safe fallback if the caller discards it.
		std::terminate();
	}

	if (unitHandle != NULL)
	{
		if (_init_context != NULL)
			_init_context->close();
		destroyUnitNoThrow(_plugin, unitHandle);
	}
	else if (_unit != NULL && _legacy_fact != NULL)
	{
		try
		{
			_legacy_fact->deleteExeUnit(_unit);
		}
		catch (...)
		{
			WTSLogger::error("In-process execution-unit test double threw while destroying a unit");
		}
		_unit = NULL;
	}
}

const void* ExeUnitWrapper::initializedContextIdentity() const
{
	std::lock_guard<std::mutex> lock(_lifecycle_mutex);
	return _initialized ? _context_identity : NULL;
}

bool ExeUnitWrapper::isInitialized() const
{
	std::lock_guard<std::mutex> lock(_lifecycle_mutex);
	return _initialized;
}

bool ExeUnitWrapper::failedInitializationWasSafelyDestroyed() const
{
	std::lock_guard<std::mutex> lock(_lifecycle_mutex);
	return _failed_init_cleanup_proven;
}

bool ExeUnitWrapper::hostOrderIdentitySupported() const
{
	std::shared_ptr<ExePluginV2State> plugin;
	{
		std::lock_guard<std::mutex> lock(_lifecycle_mutex);
		plugin = _plugin;
	}
	return plugin != NULL && (plugin->_api.capabilities & WT_EXEC_PLUGIN_V2_CAP_HOST_ORDER_IDENTITY) != 0;
}

bool ExeUnitWrapper::initialize(ExecuteContext* ctx, const char* stdCode,
	WTSVariant* cfg, std::string& error)
{
	return initialize(ctx, stdCode, cfg, error, false);
}

bool ExeUnitWrapper::initialize(ExecuteContext* ctx, const char* stdCode,
	WTSVariant* cfg, std::string& error, bool hostVerifiesIdentity)
{
	// Per-unit shallow copy: the shared policy node is never mutated, and the
	// reserved host identity key is always overwritten by the host.
	WTSVariant* unitCfg = NULL;
	if (cfg != NULL && cfg->type() == WTSVariant::VT_Object)
	{
		unitCfg = WTSVariant::createObject();
		const WTSVariant::MemberNames names = cfg->memberNames();
		for (const std::string& name : names)
		{
			WTSVariant* child = cfg->get(name.c_str());
			if (child != NULL)
				unitCfg->append(name.c_str(), child, true);
		}
		unitCfg->append(WT_EXEC_PLUGIN_V2_HOST_ORDER_IDENTITY_KEY,
			(uint32_t)((hostVerifiesIdentity && hostOrderIdentitySupported()) ? 1 : 0));
	}
	struct ReleaseCfg { WTSVariant* cfg; ~ReleaseCfg() { if (cfg) cfg->release(); } } releaseCfg{ unitCfg };
	return initializeUnit(ctx, stdCode, unitCfg != NULL ? unitCfg : cfg, error);
}

bool ExeUnitWrapper::initializeUnit(ExecuteContext* ctx, const char* stdCode,
	WTSVariant* cfg, std::string& error)
{
	error.clear();
	FactoryOperationLease operation(_admissions, false);
	if (_admissions != NULL && !operation.acquired())
	{
		error = "execution factory unit admission is closed";
		return false;
	}
	void* unitHandle = NULL;
	std::shared_ptr<ExePluginV2State> plugin;
	{
		std::lock_guard<std::mutex> lock(_lifecycle_mutex);
		if (_quiesced)
		{
			error = "execution unit is already quiesced";
			return false;
		}
		if (_init_attempted)
		{
			error = "execution unit initialization may only be attempted once";
			return false;
		}
		_init_attempted = true;
		unitHandle = _unit_handle;
		plugin = _plugin;
	}
	if (unitHandle == NULL || plugin == NULL || stdCode == NULL)
	{
		error = "V2 execution unit initialization is unavailable for this wrapper";
		if (unitHandle != NULL)
		{
			{
				std::lock_guard<std::mutex> lock(_lifecycle_mutex);
				if (_unit_handle == unitHandle)
				{
					_unit_handle = NULL;
					_unit = NULL;
				}
			}
			(void)destroyUnitNoThrow(plugin, unitHandle);
		}
		return false;
	}

	std::unique_ptr<ExeUnitContextGate> initContext;
	try
	{
		// Preserve the legacy null-context contract. Only non-null contexts need
		// a proxy, so plugins continue to observe nullptr exactly as before.
		if (ctx != NULL)
			initContext.reset(new ExeUnitContextGate(ctx));
	}
	catch (...)
	{
		error = "host failed to allocate the per-unit initialization context gate";
		{
			std::lock_guard<std::mutex> lock(_lifecycle_mutex);
			if (_unit_handle == unitHandle)
			{
				_unit_handle = NULL;
				_unit = NULL;
			}
		}
		(void)destroyUnitNoThrow(plugin, unitHandle);
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(_lifecycle_mutex);
		_init_context = std::move(initContext);
		if (_quiesce_requested && _init_context != NULL)
			_init_context->beginQuiesce();
	}

	char errorText[kErrorCapacity] = { 0 };
	WtExecPluginV2Status status = WT_EXEC_PLUGIN_V2_INTERNAL_ERROR;
	try
	{
		status = plugin->_api.initialize_unit(plugin->_factory, unitHandle,
			ctx == NULL ? NULL : _init_context.get(), stdCode, cfg, errorText, kErrorCapacity);
	}
	catch (...)
	{
		error = "plugin threw during V2 execution unit initialization";
		if (_init_context != NULL)
			_init_context->close();
		{
			std::lock_guard<std::mutex> lock(_lifecycle_mutex);
			if (_unit_handle == unitHandle)
			{
				_unit_handle = NULL;
				_unit = NULL;
			}
		}
		(void)destroyUnitNoThrow(plugin, unitHandle);
		return false;
	}

	errorText[sizeof(errorText) - 1] = '\0';
	if (status != WT_EXEC_PLUGIN_V2_OK)
	{
		if (errorText[0] != '\0')
			error.assign(errorText);
		else
			error = "plugin initialization failed with status " + std::to_string(status);

		if (_init_context != NULL)
			_init_context->close();
		{
			std::lock_guard<std::mutex> lock(_lifecycle_mutex);
			if (_unit_handle == unitHandle)
			{
				_unit_handle = NULL;
				_unit = NULL;
			}
		}
		const bool destroyedNormally = destroyUnitNoThrow(plugin, unitHandle);
		if (destroyedNormally)
		{
			// validateApi admits only ABI 2.2 plugins advertising the required
			// synchronous failed-initialization cleanup contract. A normally
			// returned non-zero status plus a normally returned destroy callback
			// is the only host-observable cleanup proof this wrapper can record.
			// The separate per-unit context gate has denied every buy, sell, and
			// cancel attempted during initialize_unit whenever a context was supplied.
			std::lock_guard<std::mutex> lock(_lifecycle_mutex);
			_failed_init_cleanup_proven = true;
		}
		return false;
	}

	bool quiesceWasRequested = false;
	{
		std::lock_guard<std::mutex> lock(_lifecycle_mutex);
		// This shares a mutex with requestQuiesce(): either this observes the
		// deferred request, or the requester observes the published initialization.
		if (_init_context != NULL)
		{
			if (_quiesce_requested)
				_init_context->beginQuiesce();
			else
				_init_context->activate();
		}
		_initialized = true;
		_context_identity = ctx;
		quiesceWasRequested = _quiesce_requested;
	}
	if (quiesceWasRequested)
		requestQuiesce();

	return true;
}

void ExeUnitWrapper::requestQuiesce()
{
	std::shared_ptr<ExePluginV2State> plugin;
	void* unitHandle = NULL;
	{
		std::lock_guard<std::mutex> lock(_lifecycle_mutex);
		_quiesce_requested = true;
		if (_init_context != NULL)
			_init_context->beginQuiesce();
		if (!_initialized || _plugin == NULL || _unit_handle == NULL ||
			_quiesce_request_succeeded || _quiesce_request_in_progress)
			return;

		_quiesce_request_in_progress = true;
		plugin = _plugin;
		unitHandle = _unit_handle;
	}

	// Publish in-progress before unlocking so synchronous callback reentry sees
	// the operation and returns without recursion. Never invoke plugin code while
	// holding _lifecycle_mutex.
	WtExecPluginV2Status status = WT_EXEC_PLUGIN_V2_INTERNAL_ERROR;
	try
	{
		status = plugin->_api.request_quiesce(plugin->_factory, unitHandle);
	}
	catch (...)
	{
		try
		{
			WTSLogger::error("V2 execution plugin {} threw while requesting unit quiescence", plugin->_path);
		}
		catch (...)
		{
		}
	}

	{
		std::lock_guard<std::mutex> lock(_lifecycle_mutex);
		_quiesce_request_succeeded = status == WT_EXEC_PLUGIN_V2_OK;
		_quiesce_request_in_progress = false;
	}
	if (status != WT_EXEC_PLUGIN_V2_OK)
	{
		try
		{
			WTSLogger::error("V2 execution plugin {} failed its unit quiesce request with status {}", plugin->_path, status);
		}
		catch (...)
		{
		}
	}
}

bool ExeUnitWrapper::canWaitQuiescedOnCurrentThread() const
{
	std::shared_ptr<ExePluginV2State> plugin;
	void* unitHandle = NULL;
	ExeUnitContextGate* contextGate = NULL;
	{
		std::unique_lock<std::mutex> lock(_lifecycle_mutex, std::try_to_lock);
		if (!lock.owns_lock())
			return false;
		if (!_quiesce_requested || _quiesce_request_in_progress)
			return false;
		if (_quiesced)
			return true;
		if (!_initialized)
		{
			// An uninitialized wrapper owns no borrowed ExecuteContext and has
			// started no plugin work. Once create/init admission is closed and
			// all admitted operations have left, it is safe to retire locally.
			if (_admissions != NULL && _admissions->unitsClosedAndIdle())
				return true;
			return false;
		}
		if (!_quiesce_request_succeeded || _plugin == NULL || _unit_handle == NULL)
			return false;
		contextGate = _init_context.get();
		if (contextGate != NULL && !contextGate->canWaitForOperationsOnCurrentThread())
			return false;

		bool expected = false;
		if (!_quiesce_operation_in_progress.compare_exchange_strong(expected, true,
			std::memory_order_acq_rel, std::memory_order_acquire))
			return false;

		plugin = _plugin;
		unitHandle = _unit_handle;
	}

	int32_t result = -1;
	try
	{
		result = plugin->_api.can_wait_quiesce(plugin->_factory, unitHandle);
	}
	catch (...)
	{
		try
		{
			WTSLogger::error("V2 execution plugin {} threw during unit quiesce preflight", plugin->_path);
		}
		catch (...)
		{
		}
	}

	_quiesce_operation_in_progress.store(false, std::memory_order_release);
	if (result != 0 && result != 1)
	{
		try
		{
			WTSLogger::error("V2 execution plugin {} returned invalid unit quiesce preflight value {}", plugin->_path, result);
		}
		catch (...)
		{
		}
	}
	return result == 1;
}

bool ExeUnitWrapper::waitQuiesced()
{
	std::shared_ptr<ExePluginV2State> plugin;
	void* unitHandle = NULL;
	ExeUnitContextGate* contextGate = NULL;
	{
		std::lock_guard<std::mutex> lock(_lifecycle_mutex);
		if (!_quiesce_requested || _quiesce_request_in_progress)
			return false;
		if (_quiesced)
			return true;
		if (!_initialized)
		{
			if (_admissions != NULL && _admissions->unitsClosedAndIdle())
			{
				_quiesced = true;
				return true;
			}
			return false;
		}
		if (!_quiesce_request_succeeded || _plugin == NULL || _unit_handle == NULL)
			return false;
		contextGate = _init_context.get();
		if (contextGate != NULL && !contextGate->canWaitForOperationsOnCurrentThread())
			return false;

		bool expected = false;
		if (!_quiesce_operation_in_progress.compare_exchange_strong(expected, true,
			std::memory_order_acq_rel, std::memory_order_acquire))
			return false;

		plugin = _plugin;
		unitHandle = _unit_handle;
	}

	bool quiesced = false;
	try
	{
		const int32_t canWait = plugin->_api.can_wait_quiesce(plugin->_factory, unitHandle);
		if (canWait == 1)
		{
			// The gate has already stopped new order/timer admissions. Drain any
			// host call admitted before that transition before asking the plugin
			// to release its own callbacks or borrowed context.
			const bool hostOperationsDrained = contextGate == NULL ||
				contextGate->waitForOperations();
			if (hostOperationsDrained)
			{
				const int32_t waitResult = plugin->_api.wait_quiesce(plugin->_factory, unitHandle);
				quiesced = waitResult == 1;
				if (waitResult != 0 && waitResult != 1)
				{
					try
					{
						WTSLogger::error("V2 execution plugin {} returned invalid unit quiesce wait value {}", plugin->_path, waitResult);
					}
					catch (...)
					{
					}
				}
			}
			else
			{
				try
				{
					WTSLogger::error("V2 execution-unit host operations could not be drained on this thread");
				}
				catch (...)
				{
				}
			}
		}
		else if (canWait != 0)
		{
			try
			{
				WTSLogger::error("V2 execution plugin {} returned invalid unit quiesce preflight value {} during wait", plugin->_path, canWait);
			}
			catch (...)
			{
			}
		}
	}
	catch (...)
	{
		try
		{
			WTSLogger::error("V2 execution plugin {} threw while waiting for unit quiescence", plugin->_path);
		}
		catch (...)
		{
		}
	}

	{
		std::lock_guard<std::mutex> lock(_lifecycle_mutex);
		if (quiesced)
		{
			if (contextGate != NULL)
				contextGate->close();
			_quiesced = true;
		}
		_quiesce_operation_in_progress.store(false, std::memory_order_release);
		quiesced = _quiesced;
	}
	return quiesced;
}

bool WtExecuterFactory::loadFactories(const char* path)
{
	FactoryOperationLease loader(_admissions, true);
	if (!loader.acquired())
	{
		WTSLogger::error("Execution factory load rejected after config admission closed");
		return false;
	}

	if (path == NULL)
	{
		WTSLogger::error("Directory of executer factory is null");
		return false;
	}

	fs::path myPath(path);
	std::error_code error;
	if (!fs::is_directory(myPath, error))
	{
		if (error)
			WTSLogger::error("Could not inspect executer factory directory {}: {}", path, error.message());
		else
			WTSLogger::error("Executer factory path {} is not a directory", path);
		return false;
	}

	fs::directory_iterator iter(myPath, error);
	if (error)
	{
		WTSLogger::error("Could not enumerate executer factory directory {}: {}", path, error.message());
		return false;
	}
	const fs::directory_iterator endIter;
	auto advance = [&]() {
		iter.increment(error);
		if (error)
		{
			WTSLogger::error("Could not continue enumerating executer factory directory {}: {}", path, error.message());
			return false;
		}
		return true;
	};
	for (; iter != endIter; )
	{
		const fs::path entryPath = iter->path();
		const bool isDirectory = iter->is_directory(error);
		if (error)
		{
			WTSLogger::error("Could not inspect entry in executer factory directory {}: {}", path, error.message());
			return false;
		}
		if (isDirectory)
		{
			if (!advance())
				return false;
			continue;
		}

#ifdef _WIN32
		if (entryPath.extension() != ".dll")
		{
			if (!advance())
				return false;
			continue;
		}
#elif defined(__APPLE__)
		if (entryPath.extension() != ".dylib")
		{
			if (!advance())
				return false;
			continue;
		}
#else
		if (entryPath.extension() != ".so")
		{
			if (!advance())
				return false;
			continue;
		}
#endif

		const std::string modulePath = entryPath.string();
		DllHandle module = DLLHelper::load_library(modulePath.c_str());
		if (module == NULL)
		{
			if (!advance())
				return false;
			continue;
		}

		WtExecPluginV2GetterFn getter = reinterpret_cast<WtExecPluginV2GetterFn>(
			DLLHelper::get_symbol(module, WT_EXEC_PLUGIN_V2_GETTER_SYMBOL));
		if (getter == NULL)
		{
			WTSLogger::error("Execution plugin {} rejected: versioned V2 getter is missing", modulePath);
			DLLHelper::free_library(module);
			if (!advance())
				return false;
			continue;
		}

		const WtExecPluginApiV2* api = NULL;
		try
		{
			api = getter(WT_EXEC_PLUGIN_V2_ABI_MAJOR);
		}
		catch (...)
		{
			WTSLogger::error("Execution plugin {} rejected: V2 getter threw an exception", modulePath);
			DLLHelper::free_library(module);
			if (!advance())
				return false;
			continue;
		}

		std::string why;
		if (!validateApi(api, why))
		{
			WTSLogger::error("Execution plugin {} rejected: {}", modulePath, why);
			DLLHelper::free_library(module);
			if (!advance())
				return false;
			continue;
		}

		std::shared_ptr<ExePluginV2State> plugin;
		try
		{
			plugin.reset(new ExePluginV2State(module, api, modulePath));
			module = NULL;
			plugin->_factory = plugin->_api.create_factory();
		}
		catch (...)
		{
			if (plugin == NULL && module != NULL)
				DLLHelper::free_library(module);
			WTSLogger::error("Execution plugin {} rejected: factory creation threw an exception", modulePath);
			if (!advance())
				return false;
			continue;
		}

		if (plugin->_factory == NULL)
		{
			WTSLogger::error("Execution plugin {} rejected: create_factory returned null", modulePath);
			if (!advance())
				return false;
			continue;
		}

		const char* pluginName = NULL;
		try
		{
			pluginName = plugin->_api.get_factory_name(plugin->_factory);
		}
		catch (...)
		{
			WTSLogger::error("Execution plugin {} rejected: get_factory_name threw an exception", modulePath);
			if (!advance())
				return false;
			continue;
		}
		if (pluginName == NULL || pluginName[0] == '\0')
		{
			WTSLogger::error("Execution plugin {} rejected: factory name is empty", modulePath);
			if (!advance())
				return false;
			continue;
		}

		const std::string name(pluginName);
		bool duplicate = false;
		{
			std::lock_guard<std::mutex> lock(_factories_mutex);
			if (_factories.find(name) != _factories.end())
			{
				duplicate = true;
			}
			else
			{
				_factories[name] = plugin;
			}
		}
		if (duplicate)
		{
			WTSLogger::error("Execution plugin {} rejected: duplicate factory name {}", modulePath, name);
			if (!advance())
				return false;
			continue;
		}
		WTSLogger::info("Executer factory {} loaded through V2", name);
		// Decision K41 (2026-11-04): a plugin that does not declare host-verified order
		// identity is accepted with its limits (non-shared owner mode, no automatic timeout
		// cancel; every cancel still verifies owner and ledger). Say so once at load time.
		if ((plugin->_api.capabilities & WT_EXEC_PLUGIN_V2_CAP_HOST_ORDER_IDENTITY) == 0)
			WTSLogger::warn("Executer factory {} ({}) does not declare host order identity: its units run in non-shared owner mode "
				"without automatic timeout cancels, and every cancel still needs a verified owner and ledger entry", name, modulePath);
		if (!advance())
			return false;
	}

	return true;
}

bool WtExecuterFactory::preflightQuiesceFactoriesOnCurrentThread() const
{
	if (_admissions == NULL || !_admissions->configClosedAndIdle() ||
		_admissions->currentThreadHasLease())
		return false;

	std::vector<std::shared_ptr<ExePluginV2State> > plugins;
	try
	{
		{
			std::unique_lock<std::mutex> lock(_factories_mutex, std::try_to_lock);
			if (!lock.owns_lock())
				return false;
			plugins.reserve(_factories.size());
			for (auto it = _factories.begin(); it != _factories.end(); ++it)
				plugins.push_back(it->second);
		}
	}
	catch (...)
	{
		try
		{
			WTSLogger::error("Could not snapshot execution plugin factories for quiesce preflight");
		}
		catch (...)
		{
		}
		return false;
	}

	// Plugin callbacks run after releasing _factories_mutex. The snapshot keeps
	// each plugin factory and module alive for the entire preflight pass.
	FactoryThreadMarkerScope callbackScope(_admissions.get());
	bool everyFactoryCanWait = true;
	for (std::vector<std::shared_ptr<ExePluginV2State> >::const_iterator it = plugins.begin();
		it != plugins.end(); ++it)
	{
		const std::shared_ptr<ExePluginV2State>& plugin = *it;
		if (plugin == NULL || plugin->_factory == NULL ||
			plugin->_api.can_wait_factory_quiesce == NULL)
		{
			everyFactoryCanWait = false;
			continue;
		}

		int32_t result = -1;
		try
		{
			result = plugin->_api.can_wait_factory_quiesce(plugin->_factory);
		}
		catch (...)
		{
			try
			{
				WTSLogger::error("V2 execution plugin {} threw during factory quiesce preflight", plugin->_path);
			}
			catch (...)
			{
			}
			everyFactoryCanWait = false;
			continue;
		}

		if (result != 1)
		{
			if (result != 0)
			{
				try
				{
					WTSLogger::error("V2 execution plugin {} returned invalid factory quiesce preflight value {}", plugin->_path, result);
				}
				catch (...)
				{
				}
			}
			everyFactoryCanWait = false;
		}
	}

	return everyFactoryCanWait;
}

const void* WtExecuterFactory::quiesceFactoryIdentity() const noexcept
{
	return _admissions.get();
}

void WtExecuterFactory::closeFactoryConfigAdmissions() noexcept
{
	if (_admissions != NULL)
		_admissions->closeConfig();
}

bool WtExecuterFactory::closeUnitAdmissions() noexcept
{
	return _admissions != NULL && _admissions->closeUnits();
}

bool WtExecuterFactory::waitForFactoryOperationsQuiesced()
{
	if (_admissions == NULL || !_admissions->configClosedAndIdle() ||
		!_admissions->unitsClosed())
		return false;
	return _admissions->waitForUnitsIdle();
}

bool WtExecuterFactory::snapshotLivePluginUnitOwners(
	std::vector<ExecPluginUnitSnapshot>& out) const
{
	out.clear();
	if (_admissions == NULL || !_admissions->configClosedAndIdle() ||
		!_admissions->unitsClosedAndIdle() || _admissions->currentThreadHasLease())
		return false;

	std::vector<std::shared_ptr<ExeUnitWrapper> > live;
	std::vector<std::weak_ptr<ExeUnitWrapper> > remaining;
	try
	{
		std::lock_guard<std::mutex> lock(_live_units_mutex);
		remaining.reserve(_live_units.size());
		live.reserve(_live_units.size());
		for (std::vector<std::weak_ptr<ExeUnitWrapper> >::const_iterator it = _live_units.begin();
			it != _live_units.end(); ++it)
		{
			std::shared_ptr<ExeUnitWrapper> wrapper = it->lock();
			if (wrapper == NULL)
				continue;
			remaining.push_back(wrapper);
			live.push_back(wrapper);
		}
		_live_units.swap(remaining);
	}
	catch (...)
	{
		out.clear();
		return false;
	}

	try
	{
		out.reserve(live.size());
		for (std::vector<std::shared_ptr<ExeUnitWrapper> >::const_iterator it = live.begin();
			it != live.end(); ++it)
		{
			ExecPluginUnitSnapshot item;
			item.owner = std::static_pointer_cast<IExecCommandQuiesce>(*it);
			item.initialized = (*it)->isInitialized();
			out.push_back(item);
		}
	}
	catch (...)
	{
		out.clear();
		return false;
	}

	return true;
}

ExecuteUnitPtr WtExecuterFactory::createUnit(const char* factname, const char* unitname,
	WtExecPluginV2UnitKind kind, bool* pluginEntered)
{
	if (pluginEntered != NULL)
		*pluginEntered = false;
	if (factname == NULL || unitname == NULL || factname[0] == '\0' || unitname[0] == '\0')
		return ExecuteUnitPtr();
	FactoryOperationLease operation(_admissions, false);
	if (!operation.acquired())
		return ExecuteUnitPtr();

	std::shared_ptr<ExePluginV2State> plugin;
	{
		std::lock_guard<std::mutex> lock(_factories_mutex);
		auto it = _factories.find(factname);
		if (it == _factories.end())
			return ExecuteUnitPtr();
		plugin = it->second;
	}
	void* unitHandle = NULL;
	// From here on plugin-owned code runs; a failure can no longer be proven
	// side-effect-free by the host.
	if (pluginEntered != NULL)
		*pluginEntered = true;
	try
	{
		unitHandle = plugin->_api.create_unit(plugin->_factory, kind, unitname);
	}
	catch (...)
	{
		WTSLogger::error("Execution plugin {} threw while creating unit {}.{}", plugin->_path, factname, unitname);
		return ExecuteUnitPtr();
	}

	if (unitHandle == NULL)
	{
		WTSLogger::error("Creating execution unit failed: {}.{}", factname, unitname);
		return ExecuteUnitPtr();
	}

	ExecuteUnit* unit = static_cast<ExecuteUnit*>(unitHandle);
	PendingUnitGuard pending(plugin, unitHandle);
	try
	{
		std::shared_ptr<ExeUnitWrapper> wrapper =
			std::make_shared<ExeUnitWrapper>(unit, unitHandle, plugin, _admissions);
		pending.release();
		{
			std::lock_guard<std::mutex> lock(_live_units_mutex);
			_live_units.push_back(wrapper);
		}
		return wrapper;
	}
	catch (...)
	{
		WTSLogger::error("Could not allocate execution unit wrapper for {}.{}", factname, unitname);
		return ExecuteUnitPtr();
	}
}

ExecuteUnitPtr WtExecuterFactory::createUnit(const char* name, WtExecPluginV2UnitKind kind,
	bool* pluginEntered)
{
	if (pluginEntered != NULL)
		*pluginEntered = false;
	if (name == NULL)
		return ExecuteUnitPtr();

	StringVector parts = StrUtil::split(name, ".");
	if (parts.size() < 2 || parts[0].empty() || parts[1].empty())
		return ExecuteUnitPtr();

	return createUnit(parts[0].c_str(), parts[1].c_str(), kind, pluginEntered);
}

ExecuteUnitPtr WtExecuterFactory::createExeUnit(const char* factname, const char* unitname)
{
	return createUnit(factname, unitname, WT_EXEC_PLUGIN_V2_UNIT_PLAIN);
}

ExecuteUnitPtr WtExecuterFactory::createDiffExeUnit(const char* factname, const char* unitname)
{
	return createUnit(factname, unitname, WT_EXEC_PLUGIN_V2_UNIT_DIFF);
}

ExecuteUnitPtr WtExecuterFactory::createArbiExeUnit(const char* factname, const char* unitname)
{
	return createUnit(factname, unitname, WT_EXEC_PLUGIN_V2_UNIT_ARBI);
}

ExecuteUnitPtr WtExecuterFactory::createExeUnit(const char* name)
{
	return createUnit(name, WT_EXEC_PLUGIN_V2_UNIT_PLAIN);
}

ExecuteUnitPtr WtExecuterFactory::createDiffExeUnit(const char* name)
{
	return createUnit(name, WT_EXEC_PLUGIN_V2_UNIT_DIFF);
}

ExecuteUnitPtr WtExecuterFactory::createArbiExeUnit(const char* name)
{
	return createUnit(name, WT_EXEC_PLUGIN_V2_UNIT_ARBI);
}

ExecuteUnitPtr WtExecuterFactory::createExeUnit(const char* name, bool& pluginEntered)
{
	return createUnit(name, WT_EXEC_PLUGIN_V2_UNIT_PLAIN, &pluginEntered);
}

ExecuteUnitPtr WtExecuterFactory::createDiffExeUnit(const char* name, bool& pluginEntered)
{
	return createUnit(name, WT_EXEC_PLUGIN_V2_UNIT_DIFF, &pluginEntered);
}

ExecuteUnitPtr WtExecuterFactory::createArbiExeUnit(const char* name, bool& pluginEntered)
{
	return createUnit(name, WT_EXEC_PLUGIN_V2_UNIT_ARBI, &pluginEntered);
}
