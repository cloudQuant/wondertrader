/**
 * @file WtExecuterFactory.h
 * @brief Execution plugin factory using the versioned plugin ABI.
 */
#pragma once

#include "IExecCommand.h"
#include "ExecCommandQuiesce.h"
#include "../Includes/ExecuteDefs.h"
#include "../Includes/IExecPluginV2.h"
#include "../Share/DLLHelper.hpp"

#include <boost/core/noncopyable.hpp>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

NS_WTP_BEGIN

class ExePluginV2State;
class ExeFactoryAdmissionState;
class ExeUnitContextGate;

/** Owns a plugin-created execution unit and keeps its plugin loaded. */
class ExeUnitWrapper : public IExecCommandQuiesce
{
public:
	ExeUnitWrapper(ExecuteUnit* unitPtr, void* unitHandle,
		const std::shared_ptr<ExePluginV2State>& plugin,
		const std::shared_ptr<ExeFactoryAdmissionState>& admissions);
	/* Source compatibility for in-process test doubles only. The factory loader
	 * never constructs wrappers through the unversioned plugin interface. */
	ExeUnitWrapper(ExecuteUnit* unitPtr, IExecuterFact* fact);
	~ExeUnitWrapper();

	ExecuteUnit* self() { return _unit; }
	/** Borrowed context identity only; this wrapper does not retain its owner. */
	const void* initializedContextIdentity() const;
	bool isInitialized() const;
	/**
	 * True only when ABI 2.2 initialization returned a failure status normally
	 * and the host's immediate destroy_unit callback also returned normally.
	 * This records the plugin's synchronous failed-init cleanup contract; a null
	 * self() pointer alone does not establish that cleanup completed safely.
	 */
	bool failedInitializationWasSafelyDestroyed() const;

	/** Initialize this unit once. A failed unit is destroyed and cannot be used. */
	bool initialize(ExecuteContext* ctx, const char* stdCode, WTSVariant* cfg,
		std::string& error);
	/**
	 * hostVerifiesIdentity: ctx enforces owner-verified order identity on
	 * cancel(localid). The unit then receives the reserved host identity key
	 * set to 1 only if its plugin advertises CAP_HOST_ORDER_IDENTITY; in every
	 * other case the key is overwritten with 0 (fail closed).
	 */
	bool initialize(ExecuteContext* ctx, const char* stdCode, WTSVariant* cfg,
		std::string& error, bool hostVerifiesIdentity);
	/** True when the unit's plugin advertises CAP_HOST_ORDER_IDENTITY (card 5b). */
	bool hostOrderIdentitySupported() const;
private:
	bool initializeUnit(ExecuteContext* ctx, const char* stdCode, WTSVariant* cfg,
		std::string& error);
public:

	/**
	 * V2 plugin-unit quiesce hooks. A failed request/wait keeps the plugin unit
	 * and its module alive so the owning executor can retry. A successfully
	 * initialized plugin unit must be quiesced before this wrapper is destroyed;
	 * destroying it after a failed or omitted wait terminates rather than
	 * releasing code or context that plugin-owned work may still reference.
	 */
	void requestQuiesce() override;
	bool canWaitQuiescedOnCurrentThread() const override;
	bool waitQuiesced() override;

private:
	ExecuteUnit* _unit;
	void* _unit_handle;
	std::shared_ptr<ExePluginV2State> _plugin;
	std::shared_ptr<ExeFactoryAdmissionState> _admissions;
	// Per-unit proxy: order calls are denied until initialization succeeds and
	// after quiesce begins. Timer registration stays unsupported because the
	// context API has no unregister contract. Null contexts remain null; the
	// proxy stays alive through failed destroy_unit and successful quiescence.
	std::unique_ptr<ExeUnitContextGate> _init_context;
	IExecuterFact* _legacy_fact;
	const void* _context_identity;
	bool _init_attempted;
	bool _failed_init_cleanup_proven;
	// Linearizes initialization publication with a concurrent quiesce request.
	// All plain lifecycle state below is protected by this mutex; plugin hooks
	// are always invoked after releasing it.
	mutable std::mutex _lifecycle_mutex;
	bool _initialized;
	bool _quiesce_requested;
	bool _quiesce_request_in_progress;
	bool _quiesce_request_succeeded;
	mutable std::atomic<bool> _quiesce_operation_in_progress;
	bool _quiesced;
};

typedef std::shared_ptr<ExeUnitWrapper> ExecuteUnitPtr;
typedef wt_hashmap<std::string, ExecuteUnitPtr> ExecuteUnitMap;

/**
 * Factory-scope current-thread preflight for ABI V2 execution plugins.
 * Call this on the control thread before sending any per-unit quiesce request.
 * It probes every loaded plugin factory; each plugin checks its active and
 * initializing units without changing state. A false result means the caller
 * must not begin the request/wait phase. Repeated calls are safe.
 */
class IExecFactoryQuiescePreflight
{
public:
	virtual ~IExecFactoryQuiescePreflight() {}
	virtual bool preflightQuiesceFactoriesOnCurrentThread() const = 0;
};

/** Strong-owner snapshot of one live wrapper held by a factory. */
struct ExecPluginUnitSnapshot
{
	ExecQuiesceOwnerPtr owner;
	bool initialized;
};

/**
 * Factory-level staged shutdown controls. Config close is a one-way admission
 * transition that must happen before any factory probe. Unit admission stays
 * open until every host ingress has drained, then closes before the control
 * thread waits for in-flight create/initialize operations. Snapshot owners keep
 * wrappers/modules alive, but do not own ExecuteContext; the coordinator must
 * prove initialized wrappers are registered by a host and retain that host.
 */
class IExecFactoryQuiesceLifecycle : public IExecFactoryQuiescePreflight
{
public:
	virtual ~IExecFactoryQuiesceLifecycle() {}
	virtual const void* quiesceFactoryIdentity() const noexcept = 0;
	/** Irreversible/idempotent admission close; already leased loaders may finish. */
	virtual void closeFactoryConfigAdmissions() noexcept = 0;
	/** Irreversible/idempotent admission close; does not wait for current leases. */
	virtual bool closeUnitAdmissions() noexcept = 0;
	/** Blocking control-thread wait; false until gates close or for a caller holding a lease. */
	virtual bool waitForFactoryOperationsQuiesced() = 0;
	/**
	 * Includes all live wrappers, initialized and uninitialized. False clears out.
	 * The caller must retain host/context owners for initialized wrappers.
	 */
	virtual bool snapshotLivePluginUnitOwners(
		std::vector<ExecPluginUnitSnapshot>& out) const = 0;
};

/** Loads and owns versioned execution plugins. */
class WtExecuterFactory : private boost::noncopyable,
	public IExecFactoryQuiesceLifecycle
{
public:
	WtExecuterFactory();
	~WtExecuterFactory() {}

	bool loadFactories(const char* path);
	/**
	 * Explicit pre-request phase; unit hooks never invoke this implicitly.
	 * Requires config/load admission to be closed and no loader in flight. The
	 * probe is read-only/nonblocking; callbacks run without the factory-map lock.
	 */
	bool preflightQuiesceFactoriesOnCurrentThread() const override;
	const void* quiesceFactoryIdentity() const noexcept override;
	void closeFactoryConfigAdmissions() noexcept override;
	bool closeUnitAdmissions() noexcept override;
	bool waitForFactoryOperationsQuiesced() override;
	bool snapshotLivePluginUnitOwners(
		std::vector<ExecPluginUnitSnapshot>& out) const override;

	ExecuteUnitPtr createExeUnit(const char* name);
	ExecuteUnitPtr createDiffExeUnit(const char* name);
	ExecuteUnitPtr createArbiExeUnit(const char* name);
	ExecuteUnitPtr createExeUnit(const char* factname, const char* unitname);
	ExecuteUnitPtr createDiffExeUnit(const char* factname, const char* unitname);
	ExecuteUnitPtr createArbiExeUnit(const char* factname, const char* unitname);
	/**
	 * Same as the single-name overloads. pluginEntered is set to true only
	 * when control was handed to the plugin's create_unit entry. A null
	 * result with pluginEntered == false is a host-side rejection (malformed
	 * name, closed unit admission or unknown factory) during which no plugin
	 * code ran, so the caller may treat it as a retry-safe preflight failure.
	 */
	ExecuteUnitPtr createExeUnit(const char* name, bool& pluginEntered);
	ExecuteUnitPtr createDiffExeUnit(const char* name, bool& pluginEntered);
	ExecuteUnitPtr createArbiExeUnit(const char* name, bool& pluginEntered);

private:
	ExecuteUnitPtr createUnit(const char* factname, const char* unitname,
		WtExecPluginV2UnitKind kind, bool* pluginEntered = NULL);
	ExecuteUnitPtr createUnit(const char* name, WtExecPluginV2UnitKind kind,
		bool* pluginEntered = NULL);

	wt_hashmap<std::string, std::shared_ptr<ExePluginV2State> > _factories;
	mutable std::mutex _factories_mutex;
	std::shared_ptr<ExeFactoryAdmissionState> _admissions;
	mutable std::mutex _live_units_mutex;
	mutable std::vector<std::weak_ptr<ExeUnitWrapper> > _live_units;
};

NS_WTP_END
