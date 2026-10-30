#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <WtBtCore/HftMocker.h>
#include <Includes/WTSVariant.hpp>

namespace
{
std::vector<std::string> g_events;
bool g_strategy_init_result = false;

class FakeHftStrategy final : public HftStrategy
{
public:
	explicit FakeHftStrategy(const char* id) : HftStrategy(id) {}
	~FakeHftStrategy() override { g_events.emplace_back("delete_strategy"); }
	const char* getName() override { return "fake"; }
	const char* getFactName() override { return "fake_factory"; }
	bool init(WTSVariant*) override
	{
		g_events.emplace_back("init_strategy");
		return g_strategy_init_result;
	}
	void on_init(IHftStraCtx*) override {}
};

class FakeHftFactory final : public IHftStrategyFact
{
public:
	~FakeHftFactory() override { g_events.emplace_back("delete_factory"); }
	const char* getName() override { return "fake_factory"; }
	void enumStrategy(FuncEnumHftStrategyCallback) override {}
	HftStrategy* createStrategy(const char* name, const char* id) override
	{
		g_events.emplace_back("create_strategy");
		if (name == nullptr || std::string(name) != "fake_strategy")
			return nullptr;
		return new FakeHftStrategy(id);
	}
	bool deleteStrategy(HftStrategy* strategy) override
	{
		delete strategy;
		return true;
	}
};

IHftStrategyFact* createFakeFactory()
{
	g_events.emplace_back("create_factory");
	return new FakeHftFactory();
}

void deleteFakeFactory(IHftStrategyFact* factory)
{
	delete factory;
}

WTSVariant* makeConfig(bool withStrategy = true)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("module", "fake_module");
	cfg->append("use_newpx", false);
	cfg->append("error_rate", (uint32_t)0);
	cfg->append("match_this_tick", false);
	if (withStrategy)
	{
		WTSVariant* strategy = WTSVariant::createObject();
		strategy->append("name", "fake_strategy");
		strategy->append("id", "fake_id");
		WTSVariant* params = WTSVariant::createObject();
		strategy->append("params", params);
		params->release();
		cfg->append("strategy", strategy);
		strategy->release();
	}
	return cfg;
}
}

struct HftMockerTestAccess
{
	static bool initWithCallbacks(HftMocker& mocker, WTSVariant* cfg,
		FuncCreateHftStraFact creator, FuncDeleteHftStraFact remover)
	{
		return mocker.init_hft_factory_impl(cfg, nullptr, "fake_module", creator, remover);
	}

	static bool hasStrategy(const HftMocker& mocker)
	{
		return mocker._strategy != nullptr;
	}
};

TEST(Q02HftMockerFactoryInit, MissingStrategyConfigurationFailsBeforeFactoryCreation)
{
	g_events.clear();
	WTSVariant* cfg = makeConfig(false);
	HftMocker mocker(nullptr, "q02_hft_missing_strategy");
	EXPECT_FALSE(mocker.init_hft_factory(cfg));
	EXPECT_TRUE(g_events.empty());
	cfg->release();
}

TEST(Q02HftMockerFactoryInit, MissingFactoryExportIsRejectedBeforeCreatorCall)
{
	g_events.clear();
	WTSVariant* cfg = makeConfig();
	HftMocker mocker(nullptr, "q02_hft_missing_export");
	EXPECT_FALSE(HftMockerTestAccess::initWithCallbacks(mocker, cfg, nullptr, deleteFakeFactory));
	EXPECT_TRUE(g_events.empty());
	cfg->release();
}

TEST(Q02HftMockerFactoryInit, FailedStrategyInitReturnsFalseAndCleansStrategyBeforeFactory)
{
	g_events.clear();
	g_strategy_init_result = false;
	WTSVariant* cfg = makeConfig();
	{
		HftMocker mocker(nullptr, "q02_hft_init_failure");
		EXPECT_FALSE(HftMockerTestAccess::initWithCallbacks(
			mocker, cfg, createFakeFactory, deleteFakeFactory));
		EXPECT_EQ((std::vector<std::string>{
			"create_factory", "create_strategy", "init_strategy",
			"delete_strategy", "delete_factory"}), g_events);
	}
	cfg->release();
}

TEST(Q02HftMockerFactoryInit, SuccessfulInitCleansStrategyBeforeFactoryAtDestruction)
{
	g_events.clear();
	g_strategy_init_result = true;
	WTSVariant* cfg = makeConfig();
	{
		HftMocker mocker(nullptr, "q02_hft_init_success");
		ASSERT_TRUE(HftMockerTestAccess::initWithCallbacks(
			mocker, cfg, createFakeFactory, deleteFakeFactory));
		ASSERT_TRUE(HftMockerTestAccess::hasStrategy(mocker));
		EXPECT_EQ((std::vector<std::string>{
			"create_factory", "create_strategy", "init_strategy"}), g_events);
	}
	EXPECT_EQ((std::vector<std::string>{
		"create_factory", "create_strategy", "init_strategy",
		"delete_strategy", "delete_factory"}), g_events);
	cfg->release();
}
