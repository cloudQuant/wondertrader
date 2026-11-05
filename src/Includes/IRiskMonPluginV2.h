/*!
 * \file IRiskMonPluginV2.h
 * \brief Versioned capability export for portfolio risk-monitor plugins.
 *
 * Iteration 03, card 9a=A+C (BUG-I02-NEW-034). The legacy C++ interfaces in
 * RiskMonDefs.h (WtRiskMonitor, IRiskMonitorFact, createRiskMonFact,
 * deleteRiskMonFact) are unchanged. A risk-monitor module may additionally
 * export the getter below to declare that WtRiskMonitor::stop() quiesces the
 * monitor. A host can only release the portfolio fund object and destroy the
 * monitor after a proven stop; a module without this declaration is kept in
 * quarantine: the monitor object, the module and the fund object it may still
 * read stay alive until process exit, and the monitor's context is detached so
 * that late calls receive inert answers instead of reaching a destroyed engine.
 */
#ifndef WT_I_RISKMON_PLUGIN_V2_H_
#define WT_I_RISKMON_PLUGIN_V2_H_

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  define WT_RISKMON_PLUGIN_V2_CALL __cdecl
#else
#  define WT_RISKMON_PLUGIN_V2_CALL
#endif

#define WT_RISKMON_PLUGIN_V2_ABI_MAJOR 2u
#define WT_RISKMON_PLUGIN_V2_ABI_MINOR 0u
#define WT_RISKMON_PLUGIN_V2_ABI_VERSION \
	((WT_RISKMON_PLUGIN_V2_ABI_MAJOR << 16) | WT_RISKMON_PLUGIN_V2_ABI_MINOR)
#define WT_RISKMON_PLUGIN_V2_GETTER_SYMBOL "getRiskMonPluginCapsV2"

enum {
	/*
	 * Synchronous stop. WtRiskMonitor::stop() is idempotent and, when called
	 * from a thread that is not owned by the monitor, returns only after every
	 * activity of the monitor has stopped. After it returns the monitor never
	 * calls its WtPortContext again and keeps no pointer obtained from
	 * WtPortContext::getFundInfo() (that pointer is borrowed: valid only until
	 * stop() returns; a monitor needing longer access must retain() it and
	 * release() it itself).
	 */
	WT_RISKMON_PLUGIN_V2_CAP_SYNC_STOP = 1u << 0
};

/*
 * Destroys a factory returned by createRiskMonFact(); the pointer is passed by
 * value (I03-NEW-062). RiskMonDefs.h's legacy FuncDeleteRiskMonFact typedef
 * takes the pointer by reference while plugins historically export the
 * deleter by value, so a host never calls the legacy deleter; it only calls
 * this verified one. Must not throw across the C ABI. The host calls it only
 * after every monitor of the factory has been stopped and deleted.
 */
typedef void (WT_RISKMON_PLUGIN_V2_CALL *WtRiskMonPluginV2DestroyFactoryFn)(void* factory);

typedef struct WtRiskMonPluginCapsV2 {
	uint32_t struct_size;
	uint32_t abi_version;
	uint32_t capabilities;
	uint32_t reserved; /* must be zero */
	/* Appended in ABI 2.0 (iteration 03); read only when struct_size covers it. */
	WtRiskMonPluginV2DestroyFactoryFn destroy_factory;
} WtRiskMonPluginCapsV2;

#define WT_RISKMON_PLUGIN_V2_BASE_SIZE ((uint32_t)offsetof(WtRiskMonPluginCapsV2, destroy_factory))
#define WT_RISKMON_PLUGIN_V2_DESTROY_SIZE \
	((uint32_t)(offsetof(WtRiskMonPluginCapsV2, destroy_factory) + sizeof(((WtRiskMonPluginCapsV2*)0)->destroy_factory)))

/* Returns a static table, or NULL for an unsupported requested major. */
typedef const WtRiskMonPluginCapsV2* (WT_RISKMON_PLUGIN_V2_CALL *WtRiskMonPluginV2GetterFn)(
	uint32_t requested_major);

#ifdef __cplusplus
namespace wt_riskmon_plugin
{
/* True only for a well-formed ABI 2 table declaring synchronous stop. */
inline bool wellFormed(const WtRiskMonPluginCapsV2* caps) noexcept
{
	return caps != 0 && caps->struct_size >= WT_RISKMON_PLUGIN_V2_BASE_SIZE &&
		(caps->abi_version >> 16) == WT_RISKMON_PLUGIN_V2_ABI_MAJOR && caps->reserved == 0;
}

inline bool declaresSyncStop(const WtRiskMonPluginCapsV2* caps) noexcept
{
	return wellFormed(caps) && (caps->capabilities & WT_RISKMON_PLUGIN_V2_CAP_SYNC_STOP) != 0;
}

/* The verified by-value factory deleter, or NULL when the table lacks it. */
inline WtRiskMonPluginV2DestroyFactoryFn factoryDestroyer(const WtRiskMonPluginCapsV2* caps) noexcept
{
	return wellFormed(caps) && caps->struct_size >= WT_RISKMON_PLUGIN_V2_DESTROY_SIZE ?
		caps->destroy_factory : 0;
}
} // namespace wt_riskmon_plugin
#endif

#endif /* WT_I_RISKMON_PLUGIN_V2_H_ */
