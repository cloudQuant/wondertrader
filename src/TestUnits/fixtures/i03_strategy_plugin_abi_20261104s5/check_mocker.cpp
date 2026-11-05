/*
 * Card 4b=A / 9a: backtest mocker factory loading (CTA/SEL/HFT/UFT) against
 * the local fake plugins built by run.sh (WT_ABI_PLUGIN_ROOT). The checker
 * opens each module itself before the mocker runs, so the image (and its
 * counters) survives a mocker dlclose and every entry-point call is visible.
 */
#include <gtest/gtest.h>

#include <dlfcn.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "WtBtCore/CtaMocker.h"
#include "WtBtCore/SelMocker.h"
#include "WtBtCore/HftMocker.h"
#include "WtBtCore/UftMocker.h"
#include "WtBtCore/HisDataReplayer.h"
#include "Includes/WTSVariant.hpp"

USING_NS_WTP;

namespace
{
#ifdef __APPLE__
const char* kExt = "dylib";
#else
const char* kExt = "so";
#endif

std::string pathOf(const char* family, const char* variant)
{
	const char* r = std::getenv("WT_ABI_PLUGIN_ROOT");
	return std::string(r ? r : "") + "/" + family + "/" + variant + "/libfake." + kExt;
}

struct Counters
{
	explicit Counters(const std::string& path) : handle(dlopen(path.c_str(), RTLD_NOW)) {}
	~Counters() { if (handle) dlclose(handle); }
	int get(const char* name) const
	{
		if (!handle) return -1000;
		auto fn = reinterpret_cast<int (*)()>(dlsym(handle, name));
		return fn ? fn() : -2000;
	}
	int creates() const { return get("fixture_creates"); }
	int legacy() const { return get("fixture_legacy_creates"); }
	int factoryDeletes() const { return get("fixture_factory_deletes"); }
	int strategyDeletes() const { return get("fixture_strategy_deletes"); }
	void* handle;
};

WTSVariant* makeCfg(const std::string& module)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("module", module.c_str());
	WTSVariant* stra = WTSVariant::createObject();
	stra->append("name", "unit");
	stra->append("id", "abi_mocker");
	cfg->append("strategy", stra);
	stra->release();
	return cfg;
}

template <typename Mocker> struct Init;
template <> struct Init<CtaMocker>
{
	static CtaMocker* make(HisDataReplayer* r) { return new CtaMocker(r, "abi_cta", 0, false); }
	static bool run(CtaMocker* m, WTSVariant* c) { return m->init_cta_factory(c); }
};
template <> struct Init<SelMocker>
{
	static SelMocker* make(HisDataReplayer* r) { return new SelMocker(r, "abi_sel", 0); }
	static bool run(SelMocker* m, WTSVariant* c) { return m->init_sel_factory(c); }
};
template <> struct Init<HftMocker>
{
	static HftMocker* make(HisDataReplayer* r) { return new HftMocker(r, "abi_hft"); }
	static bool run(HftMocker* m, WTSVariant* c) { return m->init_hft_factory(c); }
};
template <> struct Init<UftMocker>
{
	static UftMocker* make(HisDataReplayer* r) { return new UftMocker(r, "abi_uft"); }
	static bool run(UftMocker* m, WTSVariant* c) { return m->init_uft_factory(c); }
};

struct Outcome
{
	bool initialized = false;
	int creates = 0, legacy = 0, factoryDeletes = 0, strategyDeletes = 0;
};

template <typename Mocker>
Outcome load(const char* family, const char* variant)
{
	Outcome out;
	const std::string path = pathOf(family, variant);
	Counters c(path);
	EXPECT_NE(nullptr, c.handle) << path;
	HisDataReplayer replayer;
	{
		Mocker* mocker = Init<Mocker>::make(&replayer);
		WTSVariant* cfg = makeCfg(path);
		out.initialized = Init<Mocker>::run(mocker, cfg);
		cfg->release();
		delete mocker;
	}
	out.creates = c.creates();
	out.legacy = c.legacy();
	out.factoryDeletes = c.factoryDeletes();
	out.strategyDeletes = c.strategyDeletes();
	return out;
}

const std::vector<const char*> kInvalid = {
	"LEGACY", "LEGACY_BYREF", "BAD_REVISION", "BAD_FAMILY", "BAD_MAJOR", "NULL_TABLE", "THROW_GETTER",
	"RESERVED", "NULL_DESTROY", "SHORT_TABLE" };

template <typename Mocker>
void matrix(const char* family)
{
	{
		const Outcome o = load<Mocker>(family, "V2_OK");
		EXPECT_TRUE(o.initialized) << family;
		EXPECT_EQ(1, o.creates) << family;
		EXPECT_EQ(0, o.legacy) << family;
		EXPECT_EQ(1, o.strategyDeletes) << family;
		EXPECT_EQ(1, o.factoryDeletes) << family << ": capable plugin destroyed on release";
	}
	{
		const Outcome o = load<Mocker>(family, "V2_NOCAP");
		EXPECT_TRUE(o.initialized) << family;
		EXPECT_EQ(1, o.strategyDeletes) << family;
		EXPECT_EQ(0, o.factoryDeletes) << family << ": plugin without teardown capability is retained";
	}
	for (const char* variant : kInvalid)
	{
		const Outcome o = load<Mocker>(family, variant);
		EXPECT_FALSE(o.initialized) << family << "/" << variant;
		EXPECT_EQ(0, o.legacy) << family << "/" << variant << ": legacy creator must never run";
		EXPECT_EQ(0, o.creates) << family << "/" << variant;
	}
	{
		const Outcome o = load<Mocker>(family, "NULL_FACTORY");
		EXPECT_FALSE(o.initialized) << family;
		EXPECT_EQ(1, o.creates) << family;
		EXPECT_EQ(0, o.legacy) << family;
	}
}
}

TEST(S5StrategyAbiMocker, CtaMockerLoadsOnlyValidatedVersionedPlugins) { matrix<CtaMocker>("CTA"); }
TEST(S5StrategyAbiMocker, SelMockerLoadsOnlyValidatedVersionedPlugins) { matrix<SelMocker>("SEL"); }
TEST(S5StrategyAbiMocker, HftMockerLoadsOnlyValidatedVersionedPlugins) { matrix<HftMocker>("HFT"); }
TEST(S5StrategyAbiMocker, UftMockerLoadsOnlyValidatedVersionedPlugins) { matrix<UftMocker>("UFT"); }

// BUG-R31-002 backtest side: a CTA binary configured as an HFT/UFT strategy
// module never reaches its creator.
TEST(S5StrategyAbiMocker, CrossFamilyModulesNeverReachCreator)
{
	for (const char* variant : { "LEGACY", "V2_OK" })
	{
		const Outcome h = load<HftMocker>("CTA", variant);
		EXPECT_FALSE(h.initialized) << variant;
		EXPECT_EQ(0, h.legacy) << variant;
		EXPECT_EQ(0, h.creates) << variant;
		const Outcome u = load<UftMocker>("CTA", variant);
		EXPECT_FALSE(u.initialized) << variant;
		EXPECT_EQ(0, u.legacy) << variant;
		EXPECT_EQ(0, u.creates) << variant;
	}
}
