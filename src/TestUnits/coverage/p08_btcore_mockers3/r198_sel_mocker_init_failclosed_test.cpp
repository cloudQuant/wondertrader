#include <gtest/gtest.h>

#include <filesystem>

#include <Includes/WTSVariant.hpp>
#include <WtBtCore/SelMocker.h>

#ifndef WT_COV_SEL_PLUGIN_PATH
#error "CoverageTests_btcore must provide WT_COV_SEL_PLUGIN_PATH"
#endif

namespace
{
WTSVariant* makeSelFactoryConfig(const char* strategyName, bool includeParams)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("module", WT_COV_SEL_PLUGIN_PATH);

	WTSVariant* strategy = WTSVariant::createObject();
	if (strategyName != nullptr)
		strategy->append("name", strategyName);
	strategy->append("id", "r198_sel_factory");
	if (includeParams)
	{
		WTSVariant* params = WTSVariant::createObject();
		params->append("days", static_cast<uint32_t>(2));
		params->append("k1", 0.5);
		params->append("k2", 0.5);
		params->append("period", "d");
		params->append("count", static_cast<uint32_t>(5));
		params->append("stock", false);
		params->append("codes", "");
		strategy->append("params", params);
		params->release();
	}
	cfg->append("strategy", strategy);
	strategy->release();
	return cfg;
}
}

TEST(R198SelMockerFactory, rejectsMissingStrategyAndMissingName)
{
	ASSERT_TRUE(std::filesystem::exists(WT_COV_SEL_PLUGIN_PATH));
	SelMocker mocker(nullptr, "r198_sel_missing_config");

	WTSVariant* noStrategy = WTSVariant::createObject();
	noStrategy->append("module", WT_COV_SEL_PLUGIN_PATH);
	EXPECT_FALSE(mocker.init_sel_factory(noStrategy));
	noStrategy->release();

	WTSVariant* noName = makeSelFactoryConfig(nullptr, true);
	EXPECT_FALSE(mocker.init_sel_factory(noName));
	noName->release();
}

TEST(R198SelMockerFactory, rejectsUnknownStrategyAndFalseInitThenAllowsRetry)
{
	ASSERT_TRUE(std::filesystem::exists(WT_COV_SEL_PLUGIN_PATH));
	SelMocker mocker(nullptr, "r198_sel_failed_init");

	WTSVariant* unknown = makeSelFactoryConfig("NoSuchSelection", true);
	EXPECT_FALSE(mocker.init_sel_factory(unknown));
	unknown->release();

	// WtStraDtSel returns false for a null params object. The mocker must propagate
	// the result, destroy the candidate strategy, then release its factory/module.
	WTSVariant* rejected = makeSelFactoryConfig("DualThrustSelection", false);
	EXPECT_FALSE(mocker.init_sel_factory(rejected));
	rejected->release();

	WTSVariant* valid = makeSelFactoryConfig("DualThrustSelection", true);
	EXPECT_TRUE(mocker.init_sel_factory(valid));
	EXPECT_FALSE(mocker.init_sel_factory(valid));
	valid->release();
}
