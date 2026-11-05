/*!
 * \file IExecPluginV2.h
 * \brief Versioned C entry point for execution-unit plugins.
 *
 * This header deliberately does not include ExecuteDefs.h and does not change
 * the v1 C++ interfaces.  The opaque handles passed through this table still
 * refer to WonderTrader C++ objects; host and plugin therefore need compatible
 * compiler, standard-library, CRT, and WonderTrader object ABIs.
 */
#ifndef WT_I_EXEC_PLUGIN_V2_H_
#define WT_I_EXEC_PLUGIN_V2_H_

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  define WT_EXEC_PLUGIN_V2_CALL __cdecl
#  define WT_EXEC_PLUGIN_V2_EXPORT __declspec(dllexport)
#else
#  define WT_EXEC_PLUGIN_V2_CALL
#  if defined(__GNUC__) || defined(__clang__)
#    define WT_EXEC_PLUGIN_V2_EXPORT __attribute__((visibility("default")))
#  else
#    define WT_EXEC_PLUGIN_V2_EXPORT
#  endif
#endif

#ifdef __cplusplus
#  define WT_EXEC_PLUGIN_V2_EXTERN_C extern "C"
#else
#  define WT_EXEC_PLUGIN_V2_EXTERN_C extern
#endif

#define WT_EXEC_PLUGIN_V2_ABI_MAJOR 2u
#define WT_EXEC_PLUGIN_V2_ABI_MINOR 2u
#define WT_EXEC_PLUGIN_V2_ABI_VERSION \
    ((WT_EXEC_PLUGIN_V2_ABI_MAJOR << 16) | WT_EXEC_PLUGIN_V2_ABI_MINOR)
#define WT_EXEC_PLUGIN_V2_GETTER_SYMBOL "getExecPluginApiV2"

/* Stable return codes for callbacks which report an operation result. */
typedef int32_t WtExecPluginV2Status;
enum {
	WT_EXEC_PLUGIN_V2_OK = 0,
	WT_EXEC_PLUGIN_V2_INVALID_ARGUMENT = -1,
	WT_EXEC_PLUGIN_V2_UNSUPPORTED_KIND = -2,
	WT_EXEC_PLUGIN_V2_INVALID_CONFIGURATION = -3,
	WT_EXEC_PLUGIN_V2_INITIALIZATION_FAILED = -4,
	WT_EXEC_PLUGIN_V2_INTERNAL_ERROR = -5
};

typedef uint32_t WtExecPluginV2UnitKind;
enum {
	WT_EXEC_PLUGIN_V2_UNIT_PLAIN = 1,
	WT_EXEC_PLUGIN_V2_UNIT_DIFF = 2,
	WT_EXEC_PLUGIN_V2_UNIT_ARBI = 3
};

/* All object pointers are opaque to the C ABI and owned by the plugin. */
typedef void* (WT_EXEC_PLUGIN_V2_CALL *WtExecPluginV2CreateFactoryFn)(void);
typedef void (WT_EXEC_PLUGIN_V2_CALL *WtExecPluginV2DestroyFactoryFn)(void* factory);
/* The returned name is borrowed and remains valid until destroy_factory. */
typedef const char* (WT_EXEC_PLUGIN_V2_CALL *WtExecPluginV2GetFactoryNameFn)(void* factory);
/*
 * Factory and unit creation are synchronous. create_factory and create_unit
 * must not start or leave asynchronous work. create_unit's unit_name is borrowed
 * for the duration of the callback; copy it if it must be retained.
 * In particular, create_unit may be followed directly by destroy_unit if the
 * host cannot publish a wrapper for the returned unit; destroy_unit must be
 * safe on a unit whose initialization was never attempted or did not succeed.
 * ABI 2.2 has no factory request/wait hooks, so factory-only background work
 * is unsupported. Asynchronous work may be started only by a successfully
 * initialized unit (for example, by initialize_unit when it returns success)
 * and must be covered by that unit's quiesce callbacks.
 */
typedef void* (WT_EXEC_PLUGIN_V2_CALL *WtExecPluginV2CreateUnitFn)(
	void* factory, WtExecPluginV2UnitKind kind, const char* unit_name);
typedef void (WT_EXEC_PLUGIN_V2_CALL *WtExecPluginV2DestroyUnitFn)(void* factory, void* unit);

/*
 * The host calls initialize_unit exactly once for a newly created unit.  The
 * opaque context/configuration pointers are the host's ExecuteContext and
 * WTSVariant objects for this build.  A zero status means initialization
 * succeeded.  Any non-zero status means the unit must not be registered,
 * called, or otherwise used; the host may only pass it to destroy_unit.
 *
 * On every unsuccessful return, the callback must synchronously stop and
 * drain any asynchronous work it started and release every borrow of
 * execute_context, std_code, and configuration before returning. The host
 * destroys the unit immediately after failure and will not call quiesce for
 * it. Work which remains active after success must be owned by this initialized
 * unit and covered by its quiesce callbacks. If initialization encounters an
 * exception internally, the plugin must catch it, perform the same cleanup,
 * and return a failure status; no C++ exception may cross this callback.
 *
 * If error_capacity is nonzero, implementations must always NUL-terminate
 * error_text (including on truncation).  On success, an empty string is
 * recommended.  No C++ exception may cross this callback boundary.
 */
typedef WtExecPluginV2Status (WT_EXEC_PLUGIN_V2_CALL *WtExecPluginV2InitializeUnitFn)(
	void* factory,
	void* unit,
	void* execute_context,
	const char* std_code,
	void* configuration,
	char* error_text,
	uint32_t error_capacity);

/*
 * ABI 2.1 quiesce capability. The request callback must be nonblocking,
 * idempotent, and initiate shutdown of work owned by the plugin for this unit.
 * can_wait is a nonblocking preflight for the current calling thread: it
 * returns 1 only if that thread can safely call wait without waiting for itself
 * (for example, joining its own worker), and 0 if it cannot. The host must call
 * wait only after a 1 result. wait may block until plugin-owned work has
 * stopped and returns 1 on success or 0 on failure; a failed wait may be
 * retried. None of these callbacks may throw a C++ exception across the C ABI
 * boundary. These callbacks do not cover host-owned executor pools or other
 * host work. After the host has sent quiesce requests to all units, each
 * unit's wait must converge independently and must not depend on a later
 * unit's wait callback. ABI 2.2 has no factory request/wait hooks, so there is
 * no separate factory-scope lifecycle participant in this ABI. Any shared
 * worker must be managed by the participating initialized units so that every
 * per-unit wait converges independently after all per-unit requests have been
 * sent.
 */
typedef uint32_t WtExecPluginV2Capabilities;
/*
 * Advertising CAP_FACTORY_QUIESCE_PREFLIGHT commits the plugin to the ABI 2.2
 * safe-teardown contract: creation callbacks leave no asynchronous work;
 * failed initialization synchronously drains work and releases host borrows;
 * all work surviving successful initialization is unit-owned and covered by
 * per-unit quiesce; no factory-only background work exists; and the factory
 * preflight callback is safe to call concurrently with create_unit,
 * initialize_unit, and plugin-owned unit work. A plugin unable to make all of
 * these guarantees must not advertise this capability. The host must reject
 * ABI 2.2 plugins lacking this capability or its callback when safe teardown
 * is required.
 */
enum {
	WT_EXEC_PLUGIN_V2_CAP_QUIESCE = 1u << 0,
	WT_EXEC_PLUGIN_V2_CAP_FACTORY_QUIESCE_PREFLIGHT = 1u << 1,
	/*
	 * Additive capability (iteration 03, decision cards 5b/5c; ABI table and
	 * version unchanged). The plugin's units understand host-verified order
	 * identity: they issue automatic (e.g. timeout) cancels only through
	 * ExecuteContext::cancel(localid) for ids returned by that context, and
	 * treat a false return as "identity not verifiable" (fail closed, no
	 * retry, no bare-id fallback). They enable automatic timeout cancels only
	 * when the host sets the reserved configuration key
	 * WT_EXEC_PLUGIN_V2_HOST_ORDER_IDENTITY_KEY to 1 for that unit; hosts
	 * always overwrite that key. Plugins lacking this bit are treated as old
	 * identity ABI: non-shared owner mode and no automatic timeout cancel.
	 */
	WT_EXEC_PLUGIN_V2_CAP_HOST_ORDER_IDENTITY = 1u << 2
};
#define WT_EXEC_PLUGIN_V2_HOST_ORDER_IDENTITY_KEY "__wt_host_order_identity"
typedef WtExecPluginV2Status (WT_EXEC_PLUGIN_V2_CALL *WtExecPluginV2RequestQuiesceFn)(
	void* factory, void* unit);
/*
 * Nonblocking preflight for the current calling thread. Returns 1 only when
 * that thread can safely wait without self-joining; 0 means do not call wait.
 * Other values are invalid. This callback must not throw across the C ABI.
 */
typedef int32_t (WT_EXEC_PLUGIN_V2_CALL *WtExecPluginV2CanWaitQuiesceFn)(
	void* factory, void* unit);
/* Returns 1 when quiescence completed, 0 on failure; other values are invalid.
 * This callback must not throw across the C ABI. */
typedef int32_t (WT_EXEC_PLUGIN_V2_CALL *WtExecPluginV2WaitQuiesceFn)(
	void* factory, void* unit);

/*
 * ABI 2.2 factory-wide preflight. This nonblocking callback is called before
 * the host sends any per-unit quiesce request. The host may call it while
 * create_unit or initialize_unit callbacks are in flight; it must be
 * thread-safe and data-race-free with those callbacks and with plugin-owned
 * unit work. It must not block, mutate state, close admissions, or start
 * shutdown. It must inspect active work owned by the factory, including units
 * whose initialization is in progress, and return 1 only if the current
 * calling thread can safely perform the complete request/wait phase without
 * waiting for itself; return 0 unless safety can be established, and treat
 * other values as invalid. It does not stop or join workers. The host may call
 * it again after closing unit admissions and draining in-flight host callbacks;
 * the host must honor this final result before sending quiesce requests. An
 * earlier successful probe does not close admissions or freeze factory state.
 *
 * ABI 2.2 has no factory request/wait hooks, so factory-only asynchronous work
 * is unsupported. Any shared worker must be controlled by the participating
 * initialized units so that, after all per-unit requests have been sent,
 * every per-unit wait converges independently and does not depend on a later
 * unit's wait. A plugin unable to provide these guarantees must not advertise
 * WT_EXEC_PLUGIN_V2_CAP_FACTORY_QUIESCE_PREFLIGHT. This preflight-only field
 * does not itself provide factory request/wait hooks. This callback must not
 * throw across the C ABI.
 */
typedef int32_t (WT_EXEC_PLUGIN_V2_CALL *WtExecPluginV2CanWaitFactoryQuiesceFn)(
	void* factory);

/*
 * A plugin returns a pointer to a static table with this layout.  The table
 * and all callbacks remain valid until the dynamic library is unloaded.
 *
 * struct_size must be at least WT_EXEC_PLUGIN_V2_REQUIRED_SIZE and may be
 * larger to append fields in a future minor version.  abi_version's high 16
 * bits are the major version and its low 16 bits are the minor version.  A
 * host must require major == WT_EXEC_PLUGIN_V2_ABI_MAJOR, struct_size large
 * enough for every callback it uses, and every such callback non-NULL.
 * ABI 2.1 hosts which require safe plugin teardown must also require the
 * quiesce capability and all three quiesce callbacks. ABI 2.2 hosts must also
 * require the factory-quiesce-preflight capability and callback, and must run
 * that preflight before sending any per-unit request. The preflight may race
 * with create/init callbacks and the plugin must satisfy its concurrency
 * contract; hosts must run it again after closing unit admissions and
 * draining in-flight callbacks, and honor that result before sending any
 * quiesce request. A successful preflight is valid for the
 * current calling thread only. Hosts must read an appended
 * field only when struct_size covers that field. Unknown trailing fields are
 * ignored. Existing fields must never be reordered, removed, or have their
 * signatures changed. reserved[2] remains zero.
 */
typedef struct WtExecPluginApiV2 {
	uint32_t struct_size;
	uint32_t abi_version;
	uint32_t reserved[2]; /* Must remain zero in ABI major 2. */
	WtExecPluginV2CreateFactoryFn create_factory;
	WtExecPluginV2DestroyFactoryFn destroy_factory;
	WtExecPluginV2GetFactoryNameFn get_factory_name;
	WtExecPluginV2CreateUnitFn create_unit;
	WtExecPluginV2DestroyUnitFn destroy_unit;
	WtExecPluginV2InitializeUnitFn initialize_unit;
	/* ABI 2.1 append-only tail. */
	WtExecPluginV2Capabilities capabilities;
	WtExecPluginV2RequestQuiesceFn request_quiesce;
	WtExecPluginV2CanWaitQuiesceFn can_wait_quiesce;
	WtExecPluginV2WaitQuiesceFn wait_quiesce;
	/* ABI 2.2 append-only tail; ABI 2.0 and 2.1 field offsets stay unchanged. */
	WtExecPluginV2CanWaitFactoryQuiesceFn can_wait_factory_quiesce;
} WtExecPluginApiV2;

#define WT_EXEC_PLUGIN_V2_REQUIRED_SIZE \
	((uint32_t)(offsetof(WtExecPluginApiV2, initialize_unit) + \
	            sizeof(((WtExecPluginApiV2*)0)->initialize_unit)))

#define WT_EXEC_PLUGIN_V2_QUIESCE_SIZE \
	((uint32_t)(offsetof(WtExecPluginApiV2, wait_quiesce) + \
	            sizeof(((WtExecPluginApiV2*)0)->wait_quiesce)))

#define WT_EXEC_PLUGIN_V2_FACTORY_PREFLIGHT_SIZE \
	((uint32_t)(offsetof(WtExecPluginApiV2, can_wait_factory_quiesce) + \
	            sizeof(((WtExecPluginApiV2*)0)->can_wait_factory_quiesce)))

/* A getter accepting the requested major version and returning a static table. */
typedef const WtExecPluginApiV2* (WT_EXEC_PLUGIN_V2_CALL *WtExecPluginV2GetterFn)(
	uint32_t requested_major);

/*
 * Plugin implementation example:
 *
 * WT_EXEC_PLUGIN_V2_EXTERN_C WT_EXEC_PLUGIN_V2_EXPORT
 * const WtExecPluginApiV2* WT_EXEC_PLUGIN_V2_CALL
 * getExecPluginApiV2(uint32_t requested_major);
 *
 * A plugin must return NULL for an unsupported requested major.  A host which
 * cannot discover and validate this getter must reject the plugin before
 * creating a v1 factory or calling any unversioned destroy function.  This is
 * required because old plugin binaries may use incompatible legacy typedefs.
 */

#endif /* WT_I_EXEC_PLUGIN_V2_H_ */
