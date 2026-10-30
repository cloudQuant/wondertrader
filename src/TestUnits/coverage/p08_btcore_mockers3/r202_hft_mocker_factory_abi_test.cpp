/**
 * R202: HftMocker factory/strategy destruction against the repository plugin.
 * The plugin is loaded as a real shared library so the callback ABI boundary
 * is exercised, not just a same-module mock function.
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include <Includes/WTSVariant.hpp>
#include <WtBtCore/HftMocker.h>

#include "r111_common.h"

TEST(R202HftMockerFactoryAbi, DestructorReleasesRealFactoryAndStrategy)
{
	const std::string plugin = r111::plugin_path("WtHftStraFact");
	ASSERT_TRUE(std::filesystem::exists(plugin))
		<< "coverage WtHftStraFact plugin is unavailable: " << plugin;

	HisDataReplayer replayer;
	{
		HftMocker mocker(&replayer, "r202_hft_factory_abi");
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("module", plugin.c_str());

		WTSVariant* strategy = WTSVariant::createObject();
		strategy->append("name", "SimpleHft");
		strategy->append("id", "r202_hft_factory_abi");
		WTSVariant* params = WTSVariant::createObject();
		params->append("code", "SHFE.rb.2405");
		params->append("second", static_cast<uint32_t>(10));
		params->append("freq", static_cast<uint32_t>(0));
		params->append("offset", static_cast<uint32_t>(0));
		params->append("reserve", 0.0);
		params->append("stock", false);
		strategy->append("params", params);
		params->release();
		cfg->append("strategy", strategy);
		strategy->release();

		const bool initialized = mocker.init_hft_factory(cfg);
		cfg->release();
		ASSERT_TRUE(initialized);
		// Scope exit deletes the strategy first, then the factory through the
		// plugin's by-value deleteStrategyFact export.
	}
}
