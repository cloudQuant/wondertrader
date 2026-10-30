/*!
 * \file IParserLifecycleV2.h
 * \brief Versioned C ABI for safely draining and destroying parser providers.
 *
 * This is an additive ABI. It does not alter IParserApi or IParserSpi. The
 * parser API itself remains a WonderTrader C++ object, so a host and provider
 * still need compatible compiler, standard-library, CRT, and object ABIs.
 */
#ifndef WT_I_PARSER_LIFECYCLE_V2_H_
#define WT_I_PARSER_LIFECYCLE_V2_H_

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  define WT_PARSER_LIFECYCLE_V2_CALL __cdecl
#  define WT_PARSER_LIFECYCLE_V2_EXPORT __declspec(dllexport)
#else
#  define WT_PARSER_LIFECYCLE_V2_CALL
#  if defined(__GNUC__) || defined(__clang__)
#    define WT_PARSER_LIFECYCLE_V2_EXPORT __attribute__((visibility("default")))
#  else
#    define WT_PARSER_LIFECYCLE_V2_EXPORT
#  endif
#endif

#ifdef __cplusplus
#  define WT_PARSER_LIFECYCLE_V2_EXTERN_C extern "C"
#else
#  define WT_PARSER_LIFECYCLE_V2_EXTERN_C extern
#endif

#define WT_PARSER_LIFECYCLE_V2_ABI_MAJOR 2u
#define WT_PARSER_LIFECYCLE_V2_ABI_MINOR 0u
/* abi_version packs major in the high 16 bits and minor in the low 16 bits. */
#define WT_PARSER_LIFECYCLE_V2_ABI_VERSION \
	((WT_PARSER_LIFECYCLE_V2_ABI_MAJOR << 16) | WT_PARSER_LIFECYCLE_V2_ABI_MINOR)
#define WT_PARSER_LIFECYCLE_V2_GETTER_SYMBOL "getParserLifecycleApiV2"

/* Fixed-width results used by every callback in this table. */
typedef int32_t WtParserLifecycleV2Status;
enum {
	/* Factory preflight, stop requests, wait-safety probes, and destroy success. */
	WT_PARSER_LIFECYCLE_V2_OK = 0,
	/* wait_drained completed its promise; only that callback was in progress. */
	WT_PARSER_LIFECYCLE_V2_DRAINED = 1,
	/* wait_drained timed out or work is still active; retry may make progress. */
	WT_PARSER_LIFECYCLE_V2_PENDING = 2,
	/* The current thread must not wait because it could be waiting for itself. */
	WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK = 3,
	WT_PARSER_LIFECYCLE_V2_INVALID_ARGUMENT = -1,
	WT_PARSER_LIFECYCLE_V2_UNSUPPORTED = -2,
	/* State or ownership is uncertain; the host must keep the module and handle. */
	WT_PARSER_LIFECYCLE_V2_ERROR = -3
};

/*
 * A provider advertising these bits commits to the corresponding callbacks
 * and contracts below. Hosts must check the bit and the callback pointer.
 */
typedef uint64_t WtParserLifecycleV2Capabilities;
#define WT_PARSER_LIFECYCLE_V2_CAP_FACTORY_PREFLIGHT (UINT64_C(1) << 0)
#define WT_PARSER_LIFECYCLE_V2_CAP_API_DRAIN (UINT64_C(1) << 1)
#define WT_PARSER_LIFECYCLE_V2_REQUIRED_CAPABILITIES \
	(WT_PARSER_LIFECYCLE_V2_CAP_FACTORY_PREFLIGHT | WT_PARSER_LIFECYCLE_V2_CAP_API_DRAIN)

/*
 * Called after resolving and validating this table, but before the legacy
 * createParser entry point. This probe must be nonblocking, repeatable, and
 * side-effect free. OK means the provider is ready to attempt synchronous
 * API creation and has the advertised lifecycle hooks; it does not promise
 * that allocation or later API initialization will succeed. It must not start
 * work or change admission state. It may run concurrently with createParser,
 * init, and provider-owned API work for other parser instances, so its
 * implementation must be thread-safe and data-race-free with those operations.
 * A successful result is a point-in-time permission to attempt creation; it
 * does not freeze factory state or serialize another create/init call. Any
 * exception must be caught by the provider.
 */
typedef WtParserLifecycleV2Status (WT_PARSER_LIFECYCLE_V2_CALL *
	WtParserLifecycleV2PreflightFactoryFn)(void);

/*
 * Request shutdown of all asynchronous work owned by this returned parser
 * API and stop admitting new work that could cause callbacks. This callback
 * must be nonblocking and idempotent: repeated calls,
 * including calls after a partial shutdown attempt, are safe. It is valid for
 * an API whose init returned false or threw, and for an API not yet initialized.
 * OK means the stop request was accepted (or had already been accepted).
 */
typedef WtParserLifecycleV2Status (WT_PARSER_LIFECYCLE_V2_CALL *
	WtParserLifecycleV2RequestStopFn)(void* parser_api);

/*
 * Nonblocking probe for the current calling thread. Return OK only when this
 * thread can wait for this API without waiting for itself (for example, it is
 * not currently inside a callback which the API must drain). Return
 * WOULD_DEADLOCK when waiting from this thread could self-deadlock, and ERROR
 * when safety cannot be established. The result applies only to this call and
 * thread; the host must fail closed for every result other than OK.
 */
typedef WtParserLifecycleV2Status (WT_PARSER_LIFECYCLE_V2_CALL *
	WtParserLifecycleV2CanWaitFn)(void* parser_api);

/*
 * Wait for all API-owned asynchronous work and callbacks to drain. The host
 * calls this only after request_stop and a successful can_wait probe. A
 * timeout of zero requests a poll; 0xFFFFFFFFu requests an unbounded wait.
 * Return DRAINED only when no operation attributable to this API is in flight
 * or queued, no IParserSpi callback attributable to it is executing or can
 * begin later, and the provider has released every borrow of host-owned
 * callback/context/configuration data for this API. This promise applies after
 * this wait_drained callback returns; the callback itself is necessarily still
 * on the stack while producing the result. Return PENDING on timeout or while
 * work remains, WOULD_DEADLOCK if the current thread became unsafe, and ERROR
 * if drain state is uncertain. PENDING may be retried. Every exception must be
 * caught inside the provider.
 */
typedef WtParserLifecycleV2Status (WT_PARSER_LIFECYCLE_V2_CALL *
	WtParserLifecycleV2WaitDrainedFn)(void* parser_api, uint32_t timeout_ms);

/*
 * Destroy the API inside its provider module after wait_drained returned
 * DRAINED. The host passes the address of its opaque void* handle; it must not
 * cast IParserApi** to void**. The provider performs any final
 * IParserApi::release and destruction while its own code and allocator are
 * loaded. Any final IParserApi::release performed here must be synchronous,
 * cleanup-only, and must not issue callbacks or start asynchronous work. On OK
 * it sets *parser_api to NULL, and the host clears its associated typed
 * IParserApi pointer. On any other result it must leave the handle live and
 * retryable; the host retains the handle and keeps the module loaded. This
 * callback must not throw across the C ABI.
 */
typedef WtParserLifecycleV2Status (WT_PARSER_LIFECYCLE_V2_CALL *
	WtParserLifecycleV2DestroyApiFn)(void** parser_api);

/*
 * Provider-owned static table returned by getParserLifecycleApiV2(requested_major).
 * struct_size and abi_version are fixed-width and lead the table. abi_version
 * stores major in bits 31..16 and minor in bits 15..0; requested_major names
 * only the major. The host requires an exact major match. Minor releases within
 * major 2 are append-only: a host may accept a newer minor only when struct_size
 * covers every field/callback it requires and the required capability contract
 * is present. Hosts ignore unknown capability bits and unknown trailing fields;
 * providers preserve the complete known prefix and its semantics. Existing
 * fields must never be reordered, removed, or have their signatures changed.
 * reserved[2] must be zero for ABI major 2.
 */
typedef struct WtParserLifecycleApiV2 {
	uint32_t struct_size;
	uint32_t abi_version;
	WtParserLifecycleV2Capabilities capabilities;
	uint32_t reserved[2];
	WtParserLifecycleV2PreflightFactoryFn preflight_factory;
	WtParserLifecycleV2RequestStopFn request_stop;
	WtParserLifecycleV2CanWaitFn can_wait;
	WtParserLifecycleV2WaitDrainedFn wait_drained;
	WtParserLifecycleV2DestroyApiFn destroy_api;
} WtParserLifecycleApiV2;

#define WT_PARSER_LIFECYCLE_V2_HEADER_SIZE \
	((uint32_t)(offsetof(WtParserLifecycleApiV2, capabilities) + \
	            sizeof(((WtParserLifecycleApiV2*)0)->capabilities)))
#define WT_PARSER_LIFECYCLE_V2_FACTORY_PREFLIGHT_SIZE \
	((uint32_t)(offsetof(WtParserLifecycleApiV2, preflight_factory) + \
	            sizeof(((WtParserLifecycleApiV2*)0)->preflight_factory)))
#define WT_PARSER_LIFECYCLE_V2_API_DRAIN_SIZE \
	((uint32_t)(offsetof(WtParserLifecycleApiV2, destroy_api) + \
	            sizeof(((WtParserLifecycleApiV2*)0)->destroy_api)))
#define WT_PARSER_LIFECYCLE_V2_REQUIRED_SIZE WT_PARSER_LIFECYCLE_V2_API_DRAIN_SIZE

/* Getter for the provider-owned static table. Return NULL for an unknown major. */
typedef const WtParserLifecycleApiV2* (WT_PARSER_LIFECYCLE_V2_CALL *
	WtParserLifecycleV2GetterFn)(uint32_t requested_major);

/*
 * Lifecycle and ownership contract for a host using this table:
 *
 * 1. Resolve the getter, validate the major, struct_size, required capability
 *    bits, reserved fields, and every required callback before calling the
 *    provider's legacy createParser. Call preflight_factory first; do not call
 *    createParser unless it returns OK. The getter/table and preflight must
 *    remain safe if another host thread is concurrently creating or initializing
 *    a different parser instance from the same provider. This header does not
 *    decide the host's policy for providers which do not export this V2 getter.
 * 2. createParser must return synchronously and must not let an exception cross
 *    its entry point. If it returns NULL or fails, it must leave no asynchronous
 *    work behind because there is no API handle to drain. If it returns a
 *    non-NULL API, every asynchronous task started by createParser or init must
 *    be owned by that API and covered by its lifecycle callbacks. The provider
 *    catches exceptions at every C ABI entry point. This ABI has no
 *    factory-level request-stop or wait hooks, so factory-only asynchronous
 *    work is forbidden. Every background task must be owned by one or more
 *    returned API handles and drained through those handles. A shared worker
 *    is allowed only when every API that participates in it can stop it: the
 *    host sends request_stop to all participating APIs before waiting on any
 *    one of them, and each API's wait_drained must then converge independently
 *    without relying on another API's wait_drained returning.
 * 3. Passing a non-NULL API to ParserAdapter::initExt transfers lifecycle
 *    ownership to the adapter at entry; the original caller must not separately
 *    free it. The Runner retains the API handle, provider module, and every host
 *    object borrowed by the provider (including IParserSpi and retained
 *    initialization data). It retains them even when init returns false or
 *    throws. It requests stop, probes wait safety on the actual waiting thread,
 *    and retries PENDING as appropriate.
 * 4. Only DRAINED authorizes dropping host borrows or calling destroy_api.
 *    The provider's ordinary IParserApi::release is not proof of drain. Keep
 *    the provider module loaded until destroy_api succeeds and clears the API
 *    handle. If destroy fails, preserve the owner/module and handle for safe
 *    retry or quarantine.
 *
 * These callbacks describe provider-owned work. The host remains responsible
 * for its own callbacks and objects, and must not unload the module while any
 * table callback or parser API operation may still be executing.
 */

#endif /* WT_I_PARSER_LIFECYCLE_V2_H_ */
