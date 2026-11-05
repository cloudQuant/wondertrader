/*!
 * \file IStrategyPluginV2.h
 * \brief Versioned C entry points for CTA/SEL/HFT/UFT strategy-factory plugins.
 *
 * Iteration 03, decision card 4b=A (with card 9a=A+C for teardown).
 *
 * The legacy unversioned exports (createStrategyFact/deleteStrategyFact,
 * createSelStrategyFact/deleteSelStrategyFact) are "v1". They carry no ABI
 * identity: CTA, HFT and UFT plugins even share the same symbol names, and
 * historical binaries were built with different deleter signatures (by value
 * versus by reference). Plugins also inline host header code (for example
 * WTSCommodityInfo getters), so a binary built against older headers cannot be
 * made safe by the host. A host therefore must not call a v1 creator or
 * deleter: it may load the module, but must reject it with a diagnostic before
 * any plugin function runs.
 *
 * This header does not change the v1 C++ interfaces (CtaStrategy,
 * ICtaStrategyFact, ...). The factory pointer passed through this table is the
 * plugin's ICtaStrategyFact / ISelStrategyFact / IHftStrategyFact /
 * IUftStrategyFact object converted to void*; host and plugin therefore still
 * need compatible compiler, standard library, CRT and WonderTrader object ABIs,
 * which object_revision pins to one header revision.
 */
#ifndef WT_I_STRATEGY_PLUGIN_V2_H_
#define WT_I_STRATEGY_PLUGIN_V2_H_

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  define WT_STRA_PLUGIN_V2_CALL __cdecl
#  define WT_STRA_PLUGIN_V2_EXPORT __declspec(dllexport)
#else
#  define WT_STRA_PLUGIN_V2_CALL
#  if defined(__GNUC__) || defined(__clang__)
#    define WT_STRA_PLUGIN_V2_EXPORT __attribute__((visibility("default")))
#  else
#    define WT_STRA_PLUGIN_V2_EXPORT
#  endif
#endif

#ifdef __cplusplus
#  define WT_STRA_PLUGIN_V2_EXTERN_C extern "C"
#else
#  define WT_STRA_PLUGIN_V2_EXTERN_C extern
#endif

#define WT_STRA_PLUGIN_V2_ABI_MAJOR 2u
#define WT_STRA_PLUGIN_V2_ABI_MINOR 0u
#define WT_STRA_PLUGIN_V2_ABI_VERSION \
	((WT_STRA_PLUGIN_V2_ABI_MAJOR << 16) | WT_STRA_PLUGIN_V2_ABI_MINOR)

/*
 * Revision of the WonderTrader object ABI shared with strategy plugins
 * (strategy/context interfaces and every header whose inline code a plugin
 * compiles in, e.g. WTSContractInfo.hpp). It must be bumped whenever such a
 * header changes layout or inline semantics. Hosts accept only an exact match:
 * code inlined from a different header revision cannot be negotiated.
 *
 * 20261104: WTSCommodityInfo carries ready/trading-mode configuration state
 * (card 4a=A); an unconfigured trading mode reads as TM_None.
 */
#define WT_STRA_PLUGIN_V2_OBJECT_REVISION 20261104u

/* One family per getter. A module may export several getters. */
typedef uint32_t WtStraPluginV2Family;
enum {
	WT_STRA_PLUGIN_V2_FAMILY_CTA = 1,
	WT_STRA_PLUGIN_V2_FAMILY_SEL = 2,
	WT_STRA_PLUGIN_V2_FAMILY_HFT = 3,
	WT_STRA_PLUGIN_V2_FAMILY_UFT = 4
};

/*
 * Family-specific getter symbols. A plugin placed in the directory of another
 * family lacks that family's getter and is rejected before any creator runs.
 */
#define WT_CTA_STRA_PLUGIN_V2_GETTER_SYMBOL "getCtaStrategyPluginApiV2"
#define WT_SEL_STRA_PLUGIN_V2_GETTER_SYMBOL "getSelStrategyPluginApiV2"
#define WT_HFT_STRA_PLUGIN_V2_GETTER_SYMBOL "getHftStrategyPluginApiV2"
#define WT_UFT_STRA_PLUGIN_V2_GETTER_SYMBOL "getUftStrategyPluginApiV2"

/* Legacy (v1) symbols. Hosts only probe them to report a rejected old binary. */
#define WT_STRA_PLUGIN_V1_CREATOR_SYMBOL "createStrategyFact"
#define WT_SEL_STRA_PLUGIN_V1_CREATOR_SYMBOL "createSelStrategyFact"

typedef uint32_t WtStraPluginV2Capabilities;
enum {
	/*
	 * Synchronous teardown (card 9a). Advertising this bit commits the plugin
	 * to all of the following:
	 *  - create_factory, the factory's createStrategy and the strategies'
	 *    callbacks start no asynchronous work (threads, timers, detached
	 *    tasks) that can outlive the call which started it, except work that
	 *    is owned by one strategy and stopped by deleteStrategy;
	 *  - deleteStrategy(stra) returns only after every activity of that
	 *    strategy has stopped; afterwards no plugin code calls into the host
	 *    for it and no borrowed host object (context, variant, tick, slice)
	 *    is retained;
	 *  - destroy_factory(factory) is called only after every strategy of the
	 *    factory was deleted, returns only after all factory activity stopped,
	 *    and leaves nothing that runs in the module afterwards.
	 * A host may destroy the factory and unload the module only for plugins
	 * advertising this bit. Without it the host quarantines the factory and
	 * module until process exit and never calls destroy_factory.
	 */
	WT_STRA_PLUGIN_V2_CAP_SYNC_TEARDOWN = 1u << 0
};

/*
 * create_factory returns the family's factory interface pointer converted to
 * void* (for CTA: static_cast<void*>(static_cast<ICtaStrategyFact*>(f))), or
 * NULL on failure. destroy_factory receives exactly that pointer. Neither
 * callback may let a C++ exception escape; a plugin must catch internally and
 * report failure (NULL from create_factory).
 */
typedef void* (WT_STRA_PLUGIN_V2_CALL *WtStraPluginV2CreateFactoryFn)(void);
typedef void (WT_STRA_PLUGIN_V2_CALL *WtStraPluginV2DestroyFactoryFn)(void* factory);

/*
 * A plugin returns a pointer to a static table with this layout; the table
 * and its callbacks remain valid until the module is unloaded.
 *
 * struct_size must be at least WT_STRA_PLUGIN_V2_REQUIRED_SIZE and may grow
 * in a later minor version (append only). abi_version holds major in the high
 * 16 bits and minor in the low 16 bits. family must equal the family of the
 * getter which returned the table. object_revision must equal the host's
 * WT_STRA_PLUGIN_V2_OBJECT_REVISION. Unknown capability bits are ignored;
 * reserved fields must be zero.
 */
typedef struct WtStrategyPluginApiV2 {
	uint32_t struct_size;
	uint32_t abi_version;
	uint32_t family;
	uint32_t object_revision;
	WtStraPluginV2Capabilities capabilities;
	uint32_t reserved[3];
	WtStraPluginV2CreateFactoryFn create_factory;
	WtStraPluginV2DestroyFactoryFn destroy_factory;
} WtStrategyPluginApiV2;

#define WT_STRA_PLUGIN_V2_REQUIRED_SIZE \
	((uint32_t)(offsetof(WtStrategyPluginApiV2, destroy_factory) + \
	            sizeof(((WtStrategyPluginApiV2*)0)->destroy_factory)))

/* A getter must return NULL for any requested major it does not implement. */
typedef const WtStrategyPluginApiV2* (WT_STRA_PLUGIN_V2_CALL *WtStraPluginV2GetterFn)(
	uint32_t requested_major);

/*
 * Plugin implementation example (CTA):
 *
 * static void* WT_STRA_PLUGIN_V2_CALL create_v2(void) {
 *     try { return static_cast<void*>(static_cast<ICtaStrategyFact*>(new MyFact())); }
 *     catch (...) { return NULL; }
 * }
 * static void WT_STRA_PLUGIN_V2_CALL destroy_v2(void* f) {
 *     delete static_cast<ICtaStrategyFact*>(f);
 * }
 * static const WtStrategyPluginApiV2 g_api = {
 *     sizeof(WtStrategyPluginApiV2), WT_STRA_PLUGIN_V2_ABI_VERSION,
 *     WT_STRA_PLUGIN_V2_FAMILY_CTA, WT_STRA_PLUGIN_V2_OBJECT_REVISION,
 *     WT_STRA_PLUGIN_V2_CAP_SYNC_TEARDOWN, {0, 0, 0}, create_v2, destroy_v2 };
 * WT_STRA_PLUGIN_V2_EXTERN_C WT_STRA_PLUGIN_V2_EXPORT
 * const WtStrategyPluginApiV2* WT_STRA_PLUGIN_V2_CALL
 * getCtaStrategyPluginApiV2(uint32_t requested_major) {
 *     return requested_major == WT_STRA_PLUGIN_V2_ABI_MAJOR ? &g_api : NULL;
 * }
 */

#ifdef __cplusplus
#include <string>

namespace wt_stra_plugin
{
inline const char* familyName(uint32_t family) noexcept
{
	switch (family)
	{
	case WT_STRA_PLUGIN_V2_FAMILY_CTA: return "CTA";
	case WT_STRA_PLUGIN_V2_FAMILY_SEL: return "SEL";
	case WT_STRA_PLUGIN_V2_FAMILY_HFT: return "HFT";
	case WT_STRA_PLUGIN_V2_FAMILY_UFT: return "UFT";
	default: return "unknown";
	}
}

inline const char* getterSymbol(uint32_t family) noexcept
{
	switch (family)
	{
	case WT_STRA_PLUGIN_V2_FAMILY_CTA: return WT_CTA_STRA_PLUGIN_V2_GETTER_SYMBOL;
	case WT_STRA_PLUGIN_V2_FAMILY_SEL: return WT_SEL_STRA_PLUGIN_V2_GETTER_SYMBOL;
	case WT_STRA_PLUGIN_V2_FAMILY_HFT: return WT_HFT_STRA_PLUGIN_V2_GETTER_SYMBOL;
	case WT_STRA_PLUGIN_V2_FAMILY_UFT: return WT_UFT_STRA_PLUGIN_V2_GETTER_SYMBOL;
	default: return NULL;
	}
}

/*
 * Validates a table returned by the family getter. The caller must already
 * have obtained it from the getter of expectedFamily. Reads only fields that
 * struct_size covers. On failure, why describes the first violated rule.
 */
inline bool validateApi(const WtStrategyPluginApiV2* api, uint32_t expectedFamily, std::string& why)
{
	if (api == NULL)
	{
		why = "versioned getter returned no API table for the requested major version";
		return false;
	}
	if (api->struct_size < WT_STRA_PLUGIN_V2_REQUIRED_SIZE)
	{
		why = "API table is smaller than the ABI 2.0 layout";
		return false;
	}
	const uint32_t major = api->abi_version >> 16;
	if (major != WT_STRA_PLUGIN_V2_ABI_MAJOR)
	{
		why = "ABI major version " + std::to_string(major) + " is not supported (host " +
			std::to_string(WT_STRA_PLUGIN_V2_ABI_MAJOR) + ")";
		return false;
	}
	if (api->family != expectedFamily)
	{
		why = std::string("plugin family ") + familyName(api->family) + " does not match host family " +
			familyName(expectedFamily);
		return false;
	}
	if (api->object_revision != WT_STRA_PLUGIN_V2_OBJECT_REVISION)
	{
		why = "object ABI revision " + std::to_string(api->object_revision) +
			" differs from host revision " + std::to_string(WT_STRA_PLUGIN_V2_OBJECT_REVISION) +
			"; rebuild the plugin against the current WonderTrader headers";
		return false;
	}
	if (api->reserved[0] != 0 || api->reserved[1] != 0 || api->reserved[2] != 0)
	{
		why = "reserved API fields must be zero";
		return false;
	}
	if (api->create_factory == NULL || api->destroy_factory == NULL)
	{
		why = "API table is missing create_factory or destroy_factory";
		return false;
	}
	return true;
}

inline bool hasSyncTeardown(const WtStrategyPluginApiV2* api) noexcept
{
	return api != NULL && (api->capabilities & WT_STRA_PLUGIN_V2_CAP_SYNC_TEARDOWN) != 0;
}

enum class ProbeResult
{
	Accepted,          // api is a validated table of the requested family
	Rejected,          // a strategy plugin which must not be used (diagnostic in why)
	ForeignFamily,     // a versioned plugin of other families only
	NotStrategyPlugin  // no known strategy plugin entry point
};

/*
 * Classifies a loaded module without calling any v1 function. lookup(name)
 * must return the module's exported symbol or NULL. Only the getter of the
 * requested family may run (once). A module exporting a legacy creator whose
 * name this family used, but no getter for this family, is an old binary and
 * is rejected. A module with only other families' getters is foreign.
 */
template <typename Lookup>
ProbeResult probeModule(Lookup&& lookup, uint32_t family, const WtStrategyPluginApiV2*& api, std::string& why)
{
	api = NULL;
	why.clear();
	const char* symbol = getterSymbol(family);
	if (symbol == NULL)
	{
		why = "unknown strategy plugin family";
		return ProbeResult::Rejected;
	}
	WtStraPluginV2GetterFn getter = reinterpret_cast<WtStraPluginV2GetterFn>(lookup(symbol));
	if (getter == NULL)
	{
		const char* legacy = family == WT_STRA_PLUGIN_V2_FAMILY_SEL ?
			WT_SEL_STRA_PLUGIN_V1_CREATOR_SYMBOL : WT_STRA_PLUGIN_V1_CREATOR_SYMBOL;
		bool foreign = false;
		for (uint32_t other = WT_STRA_PLUGIN_V2_FAMILY_CTA; other <= WT_STRA_PLUGIN_V2_FAMILY_UFT; ++other)
			if (other != family && lookup(getterSymbol(other)) != NULL)
				foreign = true;
		if (foreign)
		{
			why = std::string("module exports no ") + symbol + "; it is a versioned plugin of another family";
			return ProbeResult::ForeignFamily;
		}
		if (lookup(legacy) != NULL)
		{
			why = std::string("legacy unversioned strategy plugin (exports ") + legacy + " but no " + symbol +
				"); rebuild it against IStrategyPluginV2.h";
			return ProbeResult::Rejected;
		}
		return ProbeResult::NotStrategyPlugin;
	}
	const WtStrategyPluginApiV2* candidate = NULL;
	try
	{
		candidate = getter(WT_STRA_PLUGIN_V2_ABI_MAJOR);
	}
	catch (...)
	{
		why = std::string(symbol) + " threw an exception";
		return ProbeResult::Rejected;
	}
	if (!validateApi(candidate, family, why))
		return ProbeResult::Rejected;
	api = candidate;
	return ProbeResult::Accepted;
}
} // namespace wt_stra_plugin
#endif

#endif /* WT_I_STRATEGY_PLUGIN_V2_H_ */
