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

namespace
{
WTSVariant* makeHftFactoryConfig(const std::string& plugin, const char* strategyName,
	const char* strategyId, bool withParams = true)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("module", plugin.c_str());

	WTSVariant* strategy = WTSVariant::createObject();
	strategy->append("name", strategyName);
	strategy->append("id", strategyId);
	if (withParams)
	{
		WTSVariant* params = WTSVariant::createObject();
		params->append("code", "SHFE.rb.2405");
		params->append("second", static_cast<uint32_t>(10));
		params->append("freq", static_cast<uint32_t>(0));
		params->append("offset", static_cast<uint32_t>(0));
		params->append("reserve", 0.0);
		params->append("stock", false);
		strategy->append("params", params);
		params->release();
	}
	cfg->append("strategy", strategy);
	strategy->release();
	return cfg;
}
}

TEST(R202HftMockerFactoryAbi, DestructorReleasesRealFactoryAndStrategy)
{
	const std::string plugin = r111::plugin_path("WtHftStraFact");
	ASSERT_TRUE(std::filesystem::exists(plugin))
		<< "coverage WtHftStraFact plugin is unavailable: " << plugin;

	HisDataReplayer replayer;
	{
		HftMocker mocker(&replayer, "r202_hft_factory_abi");
		WTSVariant* cfg = makeHftFactoryConfig(plugin, "SimpleHft", "r202_hft_factory_abi");

		const bool initialized = mocker.init_hft_factory(cfg);
		cfg->release();
		ASSERT_TRUE(initialized);
		// Scope exit deletes the strategy first, then the factory through the
		// plugin's by-value deleteStrategyFact export.
	}
}

TEST(R202HftMockerFactoryAbi, RejectsUnknownStrategyAndAllowsRetry)
{
	const std::string plugin = r111::plugin_path("WtHftStraFact");
	ASSERT_TRUE(std::filesystem::exists(plugin))
		<< "coverage WtHftStraFact plugin is unavailable: " << plugin;

	HisDataReplayer replayer;
	HftMocker mocker(&replayer, "r202_hft_unknown_strategy");
	WTSVariant* unknown = makeHftFactoryConfig(plugin, "NoSuchHftStrategy", "r202_hft_unknown_strategy");
	EXPECT_FALSE(mocker.init_hft_factory(unknown));
	unknown->release();

	// Unknown-name rejection must release the factory/module so a valid retry can initialize.
	WTSVariant* valid = makeHftFactoryConfig(plugin, "SimpleHft", "r202_hft_retry_after_unknown");
	EXPECT_TRUE(mocker.init_hft_factory(valid));
	valid->release();
}

TEST(R202HftMockerFactoryAbi, RejectsMissingStrategyParamsAndAllowsRetry)
{
	const std::string plugin = r111::plugin_path("WtHftStraFact");
	ASSERT_TRUE(std::filesystem::exists(plugin))
		<< "coverage WtHftStraFact plugin is unavailable: " << plugin;

	HisDataReplayer replayer;
	HftMocker mocker(&replayer, "r202_hft_missing_params");
	WTSVariant* missingParams = makeHftFactoryConfig(
		plugin, "SimpleHft", "r202_hft_missing_params", false);
	EXPECT_FALSE(mocker.init_hft_factory(missingParams));
	missingParams->release();

	// The failed init must not publish a strategy or retain the factory/module.
	// Retrying on the same Mocker proves the normal valid path remains available.
	WTSVariant* valid = makeHftFactoryConfig(
		plugin, "SimpleHft", "r202_hft_retry_after_missing_params");
	EXPECT_TRUE(mocker.init_hft_factory(valid));
	valid->release();
}
