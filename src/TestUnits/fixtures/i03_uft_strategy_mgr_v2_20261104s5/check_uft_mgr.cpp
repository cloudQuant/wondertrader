/*
 * Card 4b=A / 9a: WtUftCore UftStrategyMgr against the local fake strategy
 * plugins of ../i03_strategy_plugin_abi_20261104s5 (UFT family variants plus an
 * HFT plugin as a foreign family). Counters exported by the same module image
 * prove which plugin entry points ran. Compiled against the current manager
 * (green) or the pre-change copy (red control) through -I.
 */
#include <gtest/gtest.h>

#include <dlfcn.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "UftStrategyMgr.h"

namespace
{
std::string root()
{
	const char* r = std::getenv("WT_ABI_PLUGIN_ROOT");
	return r ? r : "";
}
std::string dirOf(const char* family, const char* variant) { return root() + "/" + family + "/" + variant; }
std::string pathOf(const char* family, const char* variant) { return dirOf(family, variant) + "/libfake.dylib"; }

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
}

TEST(S5UftStrategyMgrV2, ValidVersionedPluginLoadsOnce)
{
	ASSERT_FALSE(root().empty());
	UftStrategyMgr mgr;
	ASSERT_TRUE(mgr.loadFactories(dirOf("UFT", "V2_OK").c_str()));
	Counters c(pathOf("UFT", "V2_OK"));
	EXPECT_EQ(1, c.creates());
	EXPECT_EQ(0, c.legacy());
	auto strategy = mgr.createStrategy("abi_fake", "unit", "uft_ok");
	ASSERT_TRUE(strategy && strategy->self());
	EXPECT_TRUE(mgr.loadFactories(dirOf("UFT", "V2_OK").c_str()));
	EXPECT_EQ(1, c.creates()) << "rescan must not create a second factory";
}

TEST(S5UftStrategyMgrV2, LegacyAndInvalidTablesNeverReachCreator)
{
	ASSERT_FALSE(root().empty());
	for (const char* variant : { "LEGACY", "LEGACY_BYREF", "BAD_REVISION", "BAD_FAMILY", "BAD_MAJOR",
		"NULL_TABLE", "THROW_GETTER", "RESERVED", "NULL_DESTROY", "SHORT_TABLE" })
	{
		UftStrategyMgr mgr;
		EXPECT_FALSE(mgr.loadFactories(dirOf("UFT", variant).c_str())) << variant;
		Counters c(pathOf("UFT", variant));
		EXPECT_EQ(0, c.legacy()) << "legacy creator ran for " << variant;
		EXPECT_EQ(0, c.creates()) << variant;
		EXPECT_FALSE(mgr.createStrategy("abi_fake", "unit", "uft_rejected")) << variant;
		EXPECT_FALSE(mgr.loadFactories(dirOf("UFT", variant).c_str())) << variant;
		EXPECT_EQ(0, c.legacy()) << variant;
	}
	UftStrategyMgr mgr;
	EXPECT_FALSE(mgr.loadFactories(dirOf("UFT", "NULL_FACTORY").c_str()));
	Counters c(pathOf("UFT", "NULL_FACTORY"));
	EXPECT_EQ(1, c.creates());
	EXPECT_FALSE(mgr.createStrategy("abi_fake", "unit", "uft_null"));
	mgr.loadFactories(dirOf("UFT", "NULL_FACTORY").c_str());
	EXPECT_EQ(1, c.creates()) << "a failed creator is never retried";
}

TEST(S5UftStrategyMgrV2, ForeignFamilyPluginIsSkippedWithoutRunningIt)
{
	ASSERT_FALSE(root().empty());
	UftStrategyMgr mgr;
	EXPECT_TRUE(mgr.loadFactories(dirOf("HFT", "V2_OK").c_str()));
	Counters c(pathOf("HFT", "V2_OK"));
	EXPECT_EQ(0, c.getterCalls());
	EXPECT_EQ(0, c.creates());
	EXPECT_FALSE(mgr.createStrategy("abi_fake", "unit", "uft_foreign"));
}

// Card 9a: the factory is destroyed after the last strategy wrapper only when
// the plugin advertises synchronous teardown; otherwise it is quarantined.
TEST(S5UftStrategyMgrV2, FactoryTeardownFollowsCapability)
{
	ASSERT_FALSE(root().empty());
	{
		UftStrategyPtr outlives;
		{
			UftStrategyMgr mgr;
			ASSERT_TRUE(mgr.loadFactories(dirOf("UFT", "V2_OK").c_str()));
			outlives = mgr.createStrategy("abi_fake", "unit", "uft_cap_ok");
			ASSERT_TRUE(outlives && outlives->self());
		}
		Counters c(pathOf("UFT", "V2_OK"));
		EXPECT_EQ(0, c.factoryDeletes()) << "factory must outlive the external wrapper";
		outlives.reset();
		EXPECT_EQ(1, c.strategyDeletes());
		EXPECT_EQ(1, c.factoryDeletes());
	}
	{
		{
			UftStrategyMgr mgr;
			ASSERT_TRUE(mgr.loadFactories(dirOf("UFT", "V2_NOCAP").c_str()));
			auto s = mgr.createStrategy("abi_fake", "unit", "uft_cap_none");
			ASSERT_TRUE(s && s->self());
		}
		Counters c(pathOf("UFT", "V2_NOCAP"));
		EXPECT_EQ(1, c.strategyDeletes());
		EXPECT_EQ(0, c.factoryDeletes()) << "without teardown capability the factory stays quarantined";
	}
}
