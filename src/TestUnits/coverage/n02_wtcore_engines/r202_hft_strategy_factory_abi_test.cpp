/**
 * R202: HFT host factory deleter ABI positive control.
 *
 * Loads the repository-built HFT strategy plugin and lets HftStrategyMgr and
 * its strategy wrapper reach normal destruction. The plugin exports
 * deleteStrategyFact(IHftStrategyFact*) by value; this catches a host callback
 * typedef that accidentally passes IHftStrategyFact*& across the module edge.
 */
#include <gtest/gtest.h>

#include <type_traits>

#include <WtCore/HftStrategyMgr.h>

#include "../n01_wtcore_tradepaths/r71_wtcore_env.h"

static_assert(std::is_same<FuncDeleteHftStraFact,
	void(*)(IHftStrategyFact*)>::value,
	"HFT factory deleter must match the repository plugin's by-value export");

TEST(R202HftStrategyFactoryAbi, ManagerAndStrategyDestructWithRepositoryPlugin)
{
	r71::R71PluginDir plugin("WtHftStraFact");
	ASSERT_TRUE(plugin.ok()) << "coverage WtHftStraFact plugin is unavailable";

	{
		HftStrategyMgr mgr;
		ASSERT_TRUE(mgr.loadFactories(plugin.dir().c_str()));

		{
			HftStrategyPtr strategy = mgr.createStrategy(
				"WtHftStraFact", "SimpleHft", "r202_hft_factory_abi");
			ASSERT_NE(nullptr, strategy);
			ASSERT_NE(nullptr, strategy->self());
			EXPECT_STREQ("HftDemoStrategy", strategy->self()->getName());
			strategy.reset();
		}
		// mgr's strategy map releases the strategy before its factory map invokes
		// the real plugin's by-value deleteStrategyFact export.
	}
}
