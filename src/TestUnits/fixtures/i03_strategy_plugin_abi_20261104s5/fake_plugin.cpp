/*
 * Local fake strategy-factory plugin for the card 4b=A ABI matrix.
 *
 * Build with exactly one FAKE_FAMILY_{CTA,SEL,HFT,UFT} and one FAKE_VARIANT_*:
 *   V2_OK          valid versioned table, synchronous-teardown capability
 *   V2_NOCAP       valid versioned table without the teardown capability
 *   LEGACY         only the unversioned v1 exports (historical binary, by-value deleter)
 *   LEGACY_BYREF   only v1 exports with the historical by-reference deleter
 *   BAD_REVISION   object revision differs from the host headers
 *   BAD_FAMILY     table reports another family
 *   BAD_MAJOR      table reports ABI major 3
 *   NULL_TABLE     getter returns NULL
 *   THROW_GETTER   getter throws
 *   RESERVED       reserved field is non-zero
 *   NULL_DESTROY   destroy_factory is NULL
 *   SHORT_TABLE    struct_size smaller than the ABI 2.0 layout
 *   NULL_FACTORY   create_factory returns NULL
 * Every variant except V2_* also exports the legacy creator, so a host that
 * still trusts v1 symbols is detected through fixture_legacy_creates().
 * No thread, network, provider or real order is involved.
 */
#include "../../../Includes/IStrategyPluginV2.h"

#if defined(FAKE_FAMILY_CTA)
#include "../../../Includes/CtaStrategyDefs.h"
typedef CtaStrategy StrategyBase; typedef ICtaStrategyFact FactoryBase; typedef FuncEnumStrategyCallback EnumCb;
#define FAKE_FAMILY WT_STRA_PLUGIN_V2_FAMILY_CTA
#define FAKE_GETTER getCtaStrategyPluginApiV2
#define FAKE_LEGACY_CREATE createStrategyFact
#define FAKE_LEGACY_DELETE deleteStrategyFact
#elif defined(FAKE_FAMILY_SEL)
#include "../../../Includes/SelStrategyDefs.h"
typedef SelStrategy StrategyBase; typedef ISelStrategyFact FactoryBase; typedef FuncEnumSelStrategyCallback EnumCb;
#define FAKE_FAMILY WT_STRA_PLUGIN_V2_FAMILY_SEL
#define FAKE_GETTER getSelStrategyPluginApiV2
#define FAKE_LEGACY_CREATE createSelStrategyFact
#define FAKE_LEGACY_DELETE deleteSelStrategyFact
#elif defined(FAKE_FAMILY_HFT)
#include "../../../Includes/HftStrategyDefs.h"
typedef HftStrategy StrategyBase; typedef IHftStrategyFact FactoryBase; typedef FuncEnumHftStrategyCallback EnumCb;
#define FAKE_FAMILY WT_STRA_PLUGIN_V2_FAMILY_HFT
#define FAKE_GETTER getHftStrategyPluginApiV2
#define FAKE_LEGACY_CREATE createStrategyFact
#define FAKE_LEGACY_DELETE deleteStrategyFact
#elif defined(FAKE_FAMILY_UFT)
#include "../../../Includes/UftStrategyDefs.h"
typedef UftStrategy StrategyBase; typedef IUftStrategyFact FactoryBase; typedef FuncEnumUftStrategyCallback EnumCb;
#define FAKE_FAMILY WT_STRA_PLUGIN_V2_FAMILY_UFT
#define FAKE_GETTER getUftStrategyPluginApiV2
#define FAKE_LEGACY_CREATE createStrategyFact
#define FAKE_LEGACY_DELETE deleteStrategyFact
#else
#error "select a FAKE_FAMILY_*"
#endif

#include <atomic>
#include <cstring>
#include <stdexcept>

#define FAKE_EXPORT extern "C" __attribute__((visibility("default")))

namespace
{
std::atomic<int> g_creates{0}, g_factory_deletes{0}, g_legacy_creates{0},
	g_strategy_creates{0}, g_strategy_deletes{0}, g_getter_calls{0};

class Strategy : public StrategyBase
{
public:
	explicit Strategy(const char* id) : StrategyBase(id) {}
	const char* getName() override { return "unit"; }
	const char* getFactName() override { return "abi_fake"; }
#if defined(FAKE_FAMILY_HFT)
	void on_init(IHftStraCtx*) override {}
#elif defined(FAKE_FAMILY_UFT)
	void on_init(IUftStraCtx*) override {}
#endif
};

class Factory : public FactoryBase
{
public:
	~Factory() override { ++g_factory_deletes; }
	const char* getName() override { return "abi_fake"; }
	void enumStrategy(EnumCb) override {}
	StrategyBase* createStrategy(const char* name, const char* id) override
	{
		if (std::strcmp(name, "unit") != 0)
			return nullptr;
		++g_strategy_creates;
		return new Strategy(id);
	}
	bool deleteStrategy(StrategyBase* s) override
	{
		++g_strategy_deletes;
		delete s;
		return true;
	}
};

#if !defined(FAKE_VARIANT_LEGACY) && !defined(FAKE_VARIANT_LEGACY_BYREF)
void* WT_STRA_PLUGIN_V2_CALL create_v2()
{
	++g_creates;
#if defined(FAKE_VARIANT_NULL_FACTORY)
	return nullptr;
#else
	try { return static_cast<void*>(static_cast<FactoryBase*>(new Factory())); }
	catch (...) { return nullptr; }
#endif
}

void WT_STRA_PLUGIN_V2_CALL destroy_v2(void* fact)
{
	delete static_cast<FactoryBase*>(fact);
}

WtStrategyPluginApiV2 make_table()
{
	WtStrategyPluginApiV2 api;
	std::memset(&api, 0, sizeof(api));
	api.struct_size = sizeof(WtStrategyPluginApiV2);
	api.abi_version = WT_STRA_PLUGIN_V2_ABI_VERSION;
	api.family = FAKE_FAMILY;
	api.object_revision = WT_STRA_PLUGIN_V2_OBJECT_REVISION;
	api.capabilities = WT_STRA_PLUGIN_V2_CAP_SYNC_TEARDOWN;
	api.create_factory = create_v2;
	api.destroy_factory = destroy_v2;
#if defined(FAKE_VARIANT_V2_NOCAP)
	api.capabilities = 0;
#elif defined(FAKE_VARIANT_BAD_REVISION)
	api.object_revision = WT_STRA_PLUGIN_V2_OBJECT_REVISION - 1;
#elif defined(FAKE_VARIANT_BAD_FAMILY)
	api.family = FAKE_FAMILY == WT_STRA_PLUGIN_V2_FAMILY_CTA ? WT_STRA_PLUGIN_V2_FAMILY_HFT : WT_STRA_PLUGIN_V2_FAMILY_CTA;
#elif defined(FAKE_VARIANT_BAD_MAJOR)
	api.abi_version = (3u << 16);
#elif defined(FAKE_VARIANT_RESERVED)
	api.reserved[1] = 7;
#elif defined(FAKE_VARIANT_NULL_DESTROY)
	api.destroy_factory = nullptr;
#elif defined(FAKE_VARIANT_SHORT_TABLE)
	api.struct_size = (uint32_t)offsetof(WtStrategyPluginApiV2, destroy_factory);
#endif
	return api;
}
const WtStrategyPluginApiV2 g_api = make_table();
#endif
}

#if !defined(FAKE_VARIANT_V2_OK) && !defined(FAKE_VARIANT_V2_NOCAP)
// Historical unversioned entry points. Hosts implementing card 4b=A must never
// call them; the counter proves it.
FAKE_EXPORT FactoryBase* FAKE_LEGACY_CREATE()
{
	++g_legacy_creates;
	return new Factory();
}
#if defined(FAKE_VARIANT_LEGACY_BYREF)
FAKE_EXPORT void FAKE_LEGACY_DELETE(FactoryBase*& f) { delete f; f = nullptr; }
#else
FAKE_EXPORT void FAKE_LEGACY_DELETE(FactoryBase* f) { delete f; }
#endif
#endif

#if !defined(FAKE_VARIANT_LEGACY) && !defined(FAKE_VARIANT_LEGACY_BYREF)
FAKE_EXPORT const WtStrategyPluginApiV2* FAKE_GETTER(uint32_t requested_major)
{
	++g_getter_calls;
#if defined(FAKE_VARIANT_NULL_TABLE)
	(void)requested_major;
	return nullptr;
#elif defined(FAKE_VARIANT_THROW_GETTER)
	(void)requested_major;
	throw std::runtime_error("getter failure");
#else
	return requested_major == WT_STRA_PLUGIN_V2_ABI_MAJOR ? &g_api : nullptr;
#endif
}
#endif

FAKE_EXPORT int fixture_creates() { return g_creates.load(); }
FAKE_EXPORT int fixture_factory_deletes() { return g_factory_deletes.load(); }
FAKE_EXPORT int fixture_legacy_creates() { return g_legacy_creates.load(); }
FAKE_EXPORT int fixture_strategy_creates() { return g_strategy_creates.load(); }
FAKE_EXPORT int fixture_strategy_deletes() { return g_strategy_deletes.load(); }
FAKE_EXPORT int fixture_getter_calls() { return g_getter_calls.load(); }
