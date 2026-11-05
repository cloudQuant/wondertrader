/*
 * Card 4b=A / 9a: live strategy managers (CTA/SEL/HFT) against local fake
 * plugins. WT_ABI_PLUGIN_ROOT/<FAMILY>/<VARIANT>/libfake.<ext> is produced by
 * run.sh. Each manager loads one variant directory; the test reads counters
 * exported by the same module image to prove which plugin entry points ran.
 */
#include <gtest/gtest.h>

#include <dlfcn.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

// Resolved through -I so that run.sh can compile the same checks against the
// current managers (green) or the saved pre-change managers (red control).
#include "CtaStrategyMgr.h"
#include "SelStrategyMgr.h"
#include "HftStrategyMgr.h"

namespace
{
#ifdef __APPLE__
const char* kExt = "dylib";
#else
const char* kExt = "so";
#endif

std::string root()
{
	const char* r = std::getenv("WT_ABI_PLUGIN_ROOT");
	return r ? r : "";
}

std::string dirOf(const char* family, const char* variant)
{
	return root() + "/" + family + "/" + variant;
}

std::string pathOf(const char* family, const char* variant)
{
	return dirOf(family, variant) + "/libfake." + kExt;
}

// Opens the same image the manager loaded (adds one reference).
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
	int getterCalls() const { return get("fixture_getter_calls"); }
	void* handle;
};

const std::vector<const char*> kInvalidVariants = {
	"LEGACY_BYREF", "BAD_REVISION", "BAD_FAMILY", "BAD_MAJOR", "NULL_TABLE", "THROW_GETTER",
	"RESERVED", "NULL_DESTROY", "SHORT_TABLE" };

template <typename Mgr>
void expectValidVersionedPlugin(const char* family)
{
	ASSERT_FALSE(root().empty()) << "WT_ABI_PLUGIN_ROOT is required";
	const std::string path = pathOf(family, "V2_OK");
	{
		Mgr mgr;
		ASSERT_TRUE(mgr.loadFactories(dirOf(family, "V2_OK").c_str()));
		Counters c(path);
		EXPECT_EQ(1, c.creates());
		EXPECT_EQ(0, c.legacy());
		auto strategy = mgr.createStrategy("abi_fake", "unit", "abi_id");
		ASSERT_TRUE(strategy && strategy->self());
		EXPECT_STREQ("unit", strategy->self()->getName());
		// Same-manager rescan is idempotent and creates no second factory.
		EXPECT_TRUE(mgr.loadFactories(dirOf(family, "V2_OK").c_str()));
		EXPECT_EQ(1, c.creates());
	}
}

template <typename Mgr>
void expectLegacyRejected(const char* family, const char* variant)
{
	ASSERT_FALSE(root().empty()) << "WT_ABI_PLUGIN_ROOT is required";
	Mgr mgr;
	EXPECT_FALSE(mgr.loadFactories(dirOf(family, variant).c_str())) << family << "/" << variant;
	Counters c(pathOf(family, variant));
	EXPECT_EQ(0, c.legacy()) << "legacy creator must never run: " << family << "/" << variant;
	EXPECT_EQ(0, c.creates()) << family << "/" << variant;
	EXPECT_FALSE(mgr.createStrategy("abi_fake", "unit", "abi_rejected")) << family << "/" << variant;
	// A rejected module stays refused on rescan; still no creator call.
	EXPECT_FALSE(mgr.loadFactories(dirOf(family, variant).c_str())) << family << "/" << variant;
	EXPECT_EQ(0, c.legacy());
	EXPECT_EQ(0, c.creates());
}

template <typename Mgr>
void expectInvalidTablesRejected(const char* family)
{
	for (const char* variant : kInvalidVariants)
		expectLegacyRejected<Mgr>(family, variant);
	// create_factory returning NULL: called once, never repeated, not published.
	Mgr mgr;
	const std::string dir = dirOf(family, "NULL_FACTORY");
	const bool first = mgr.loadFactories(dir.c_str());
	Counters c(pathOf(family, "NULL_FACTORY"));
	EXPECT_EQ(1, c.creates());
	EXPECT_EQ(0, c.legacy());
	EXPECT_FALSE(mgr.createStrategy("abi_fake", "unit", "abi_null"));
	const bool second = mgr.loadFactories(dir.c_str());
	EXPECT_EQ(1, c.creates());
	(void)first; (void)second; // SEL keeps its "ignored invalid factory" return contract.
}
}

// ---------------------------------------------------------------- CTA
TEST(S5StrategyAbiLive, CtaValidVersionedPluginLoads) { expectValidVersionedPlugin<CtaStrategyMgr>("CTA"); }
TEST(S5StrategyAbiLive, CtaLegacyBinaryRejectedBeforeCreator) { expectLegacyRejected<CtaStrategyMgr>("CTA", "LEGACY"); }
TEST(S5StrategyAbiLive, CtaInvalidVersionTablesRejected) { expectInvalidTablesRejected<CtaStrategyMgr>("CTA"); }

// ---------------------------------------------------------------- SEL
TEST(S5StrategyAbiLive, SelValidVersionedPluginLoads) { expectValidVersionedPlugin<SelStrategyMgr>("SEL"); }
TEST(S5StrategyAbiLive, SelLegacyBinaryRejectedBeforeCreator) { expectLegacyRejected<SelStrategyMgr>("SEL", "LEGACY"); }
TEST(S5StrategyAbiLive, SelInvalidVersionTablesRejected) { expectInvalidTablesRejected<SelStrategyMgr>("SEL"); }

// ---------------------------------------------------------------- HFT
TEST(S5StrategyAbiLive, HftValidVersionedPluginLoads) { expectValidVersionedPlugin<HftStrategyMgr>("HFT"); }
TEST(S5StrategyAbiLive, HftLegacyBinaryRejectedBeforeCreator) { expectLegacyRejected<HftStrategyMgr>("HFT", "LEGACY"); }
TEST(S5StrategyAbiLive, HftInvalidVersionTablesRejected) { expectInvalidTablesRejected<HftStrategyMgr>("HFT"); }

// Card 9a: only a plugin advertising synchronous teardown has its factory
// destroyed after the last strategy wrapper; otherwise it is quarantined.
TEST(S5StrategyAbiLive, HftFactoryTeardownFollowsCapability)
{
	ASSERT_FALSE(root().empty());
	{
		HftStrategyPtr outlives;
		{
			HftStrategyMgr mgr;
			ASSERT_TRUE(mgr.loadFactories(dirOf("HFT", "V2_OK").c_str()));
			outlives = mgr.createStrategy("abi_fake", "unit", "cap_ok");
			ASSERT_TRUE(outlives && outlives->self());
		}
		Counters c(pathOf("HFT", "V2_OK"));
		EXPECT_EQ(0, c.factoryDeletes()) << "factory must outlive the external wrapper";
		outlives.reset();
		EXPECT_EQ(1, c.strategyDeletes());
		EXPECT_EQ(1, c.factoryDeletes()) << "capable plugin destroyed after its last wrapper";
	}
	{
		{
			HftStrategyMgr mgr;
			ASSERT_TRUE(mgr.loadFactories(dirOf("HFT", "V2_NOCAP").c_str()));
			auto s = mgr.createStrategy("abi_fake", "unit", "cap_none");
			ASSERT_TRUE(s && s->self());
		}
		Counters c(pathOf("HFT", "V2_NOCAP"));
		EXPECT_EQ(1, c.strategyDeletes());
		EXPECT_EQ(0, c.factoryDeletes()) << "plugin without teardown capability stays quarantined";
	}
}

// ---------------------------------------------------------------- R31-002
// A plugin of another family in a manager's directory is never handed to the
// manager's creator path. A versioned foreign plugin is skipped; a legacy
// binary exporting the shared createStrategyFact name is refused.
TEST(S5StrategyAbiLive, CtaPluginsInHftDirectoryNeverReachCreator)
{
	ASSERT_FALSE(root().empty());
	{
		HftStrategyMgr mgr;
		EXPECT_TRUE(mgr.loadFactories(dirOf("CTA", "V2_OK").c_str()));
		Counters c(pathOf("CTA", "V2_OK"));
		EXPECT_EQ(0, c.creates());
		EXPECT_EQ(0, c.getterCalls()) << "foreign getter must not run";
		EXPECT_FALSE(mgr.createStrategy("abi_fake", "unit", "foreign"));
	}
	{
		HftStrategyMgr mgr;
		EXPECT_FALSE(mgr.loadFactories(dirOf("CTA", "LEGACY").c_str()));
		Counters c(pathOf("CTA", "LEGACY"));
		EXPECT_EQ(0, c.legacy());
		EXPECT_FALSE(mgr.createStrategy("abi_fake", "unit", "foreign_legacy"));
	}
	{
		CtaStrategyMgr mgr;
		EXPECT_TRUE(mgr.loadFactories(dirOf("HFT", "V2_OK").c_str()));
		Counters c(pathOf("HFT", "V2_OK"));
		EXPECT_EQ(0, c.getterCalls());
		EXPECT_FALSE(mgr.createStrategy("abi_fake", "unit", "foreign_cta"));
	}
	{
		SelStrategyMgr mgr;
		EXPECT_TRUE(mgr.loadFactories(dirOf("CTA", "V2_OK").c_str()));
		EXPECT_FALSE(mgr.createStrategy("abi_fake", "unit", "foreign_sel"));
	}
}
