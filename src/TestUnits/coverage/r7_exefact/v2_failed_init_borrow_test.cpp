#include <gtest/gtest.h>

#include "Includes/ExecuteDefs.h"
#include "Includes/IExecPluginV2.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "Includes/WTSVariant.hpp"

#include <stdexcept>

extern "C" const WtExecPluginApiV2* WT_EXEC_PLUGIN_V2_CALL
getExecPluginApiV2(uint32_t requested_major);

namespace
{
using namespace wtp;

class FailedInitContext final : public ExecuteContext
{
public:
	explicit FailedInitContext(bool throw_on_commodity_lookup)
		: _throw_on_commodity_lookup(throw_on_commodity_lookup)
	{}

	WTSTickSlice* getTicks(const char*, uint32_t, uint64_t) override { return nullptr; }
	WTSTickData* grabLastTick(const char*) override { return nullptr; }
	double getPosition(const char*, bool = true, int32_t = 3) override { return 0; }
	OrderMap* getOrders(const char*) override { return nullptr; }
	double getUndoneQty(const char*) override { return 0; }
	OrderIDs buy(const char*, double, double, bool) override { return {}; }
	OrderIDs sell(const char*, double, double, bool) override { return {}; }
	bool cancel(uint32_t) override { return false; }
	OrderIDs cancel(const char*, bool, double) override { return {}; }
	void writeLog(const char*) override {}
	WTSCommodityInfo* getCommodityInfo(const char*) override
	{
		if (_throw_on_commodity_lookup)
			throw std::runtime_error("controlled commodity lookup failure");
		return nullptr;
	}
	WTSSessionInfo* getSessionInfo(const char*) override { return nullptr; }
	uint64_t getCurTime() override { return 0; }

private:
	bool _throw_on_commodity_lookup;
};

class SuccessLogFailureContext final : public ExecuteContext
{
public:
	SuccessLogFailureContext()
	{
		comm = WTSCommodityInfo::create("IF", "IF test", "CFFEX", "R7_TEST", "CHN");
		comm->setPriceTick(0.2);
		sess = WTSSessionInfo::create("R7_TEST", "R7 test session");
		sess->addTradingSection(900, 1500);
	}
	~SuccessLogFailureContext() override
	{
		if (comm)
			comm->release();
		if (sess)
			sess->release();
	}

	WTSTickSlice* getTicks(const char*, uint32_t, uint64_t) override { return nullptr; }
	WTSTickData* grabLastTick(const char*) override { return nullptr; }
	double getPosition(const char*, bool = true, int32_t = 3) override { return 0; }
	OrderMap* getOrders(const char*) override { return nullptr; }
	double getUndoneQty(const char*) override { return 0; }
	OrderIDs buy(const char*, double, double, bool) override { return {}; }
	OrderIDs sell(const char*, double, double, bool) override { return {}; }
	bool cancel(uint32_t) override { return false; }
	OrderIDs cancel(const char*, bool, double) override { return {}; }
	void writeLog(const char*) override
	{
		++log_calls;
		throw std::runtime_error("controlled successful-init log failure");
	}
	WTSCommodityInfo* getCommodityInfo(const char*) override
	{
		++commodity_lookups;
		return comm;
	}
	WTSSessionInfo* getSessionInfo(const char*) override
	{
		++session_lookups;
		return sess;
	}
	uint64_t getCurTime() override { return 0; }

	WTSCommodityInfo* comm{ nullptr };
	WTSSessionInfo* sess{ nullptr };
	uint32_t commodity_lookups{ 0 };
	uint32_t session_lookups{ 0 };
	uint32_t log_calls{ 0 };
};

class ScopedPluginObjects
{
public:
	explicit ScopedPluginObjects(const WtExecPluginApiV2* api)
		: _api(api), _factory(api ? api->create_factory() : nullptr)
	{}

	~ScopedPluginObjects()
	{
		// Keep the plugin factory alive until its failed unit is destroyed.
		if (_unit != nullptr)
			_api->destroy_unit(_factory, _unit);
		if (_factory != nullptr)
			_api->destroy_factory(_factory);
	}

	void* factory() const { return _factory; }
	void* unit() const { return _unit; }
	void createUnit(const char* unit_name = "WtTWapExeUnit")
	{
		if (_factory != nullptr)
			_unit = _api->create_unit(_factory, WT_EXEC_PLUGIN_V2_UNIT_PLAIN, unit_name);
	}

private:
	const WtExecPluginApiV2* _api;
	void* _factory;
	void* _unit{ nullptr };
};

void expect_failed_init_clears_host_borrow(bool throw_on_commodity_lookup,
	WtExecPluginV2Status expected_status)
{
	const WtExecPluginApiV2* api = getExecPluginApiV2(WT_EXEC_PLUGIN_V2_ABI_MAJOR);
	ASSERT_NE(api, nullptr);
	ASSERT_GE(api->struct_size, WT_EXEC_PLUGIN_V2_REQUIRED_SIZE);

	ScopedPluginObjects plugin(api);
	ASSERT_NE(plugin.factory(), nullptr);
	plugin.createUnit();
	ASSERT_NE(plugin.unit(), nullptr);

	FailedInitContext context(throw_on_commodity_lookup);
	WTSVariant* configuration = WTSVariant::createObject();
	ASSERT_NE(configuration, nullptr);

	char error[128] = {};
	const WtExecPluginV2Status status = api->initialize_unit(plugin.factory(), plugin.unit(),
		&context, "CFFEX.IF.2601", configuration, error, sizeof(error));
	configuration->release();

	EXPECT_EQ(status, expected_status);
	EXPECT_NE(error[0], '\0');
	EXPECT_FALSE(static_cast<ExecuteUnit*>(plugin.unit())->hasBorrowedContext());
}

TEST(wt_exefact_v2_failed_init, invalid_configuration_releases_context_before_return)
{
	// TWAP stores _ctx before looking up these references; the unavailable
	// commodity then makes its real V2 callback return INVALID_CONFIGURATION.
	expect_failed_init_clears_host_borrow(false, WT_EXEC_PLUGIN_V2_INVALID_CONFIGURATION);
}

TEST(wt_exefact_v2_failed_init, exception_releases_context_before_return)
{
	// Throw from the first host call after ExecuteUnit::init stored _ctx.
	expect_failed_init_clears_host_borrow(true, WT_EXEC_PLUGIN_V2_INITIALIZATION_FAILED);
}

TEST(wt_exefact_v2_failed_init, successful_init_log_exception_releases_metadata_and_rejects_retry)
{
	const WtExecPluginApiV2* api = getExecPluginApiV2(WT_EXEC_PLUGIN_V2_ABI_MAJOR);
	ASSERT_NE(api, nullptr);
	ASSERT_GE(api->struct_size, WT_EXEC_PLUGIN_V2_REQUIRED_SIZE);

	ScopedPluginObjects plugin(api);
	ASSERT_NE(plugin.factory(), nullptr);
	plugin.createUnit("WtMinImpactExeUnit");
	ASSERT_NE(plugin.unit(), nullptr);

	SuccessLogFailureContext context;
	ASSERT_EQ(context.comm->retainCount(), 1u);
	ASSERT_EQ(context.sess->retainCount(), 1u);
	WTSVariant* configuration = WTSVariant::createObject();
	ASSERT_NE(configuration, nullptr);
	configuration->append("offset", 0);
	configuration->append("expire", 0);
	configuration->append("pricemode", 0);
	configuration->append("span", 0);
	configuration->append("byrate", false);
	configuration->append("lots", 2.0);
	configuration->append("rate", 0.0);

	char error[128] = {};
	const WtExecPluginV2Status status = api->initialize_unit(plugin.factory(), plugin.unit(),
		&context, "CFFEX.IF.2601", configuration, error, sizeof(error));
	EXPECT_EQ(status, WT_EXEC_PLUGIN_V2_INITIALIZATION_FAILED);
	EXPECT_NE(error[0], '\0');
	EXPECT_FALSE(static_cast<ExecuteUnit*>(plugin.unit())->hasBorrowedContext());
	EXPECT_EQ(context.comm->retainCount(), 1u);
	EXPECT_EQ(context.sess->retainCount(), 1u);
	EXPECT_EQ(context.commodity_lookups, 1u);
	EXPECT_EQ(context.session_lookups, 1u);
	EXPECT_EQ(context.log_calls, 1u);

	// Probe the plugin's same-instance guard; it must reject without re-entering
	// host callbacks. A V2 host normally destroys the failed unit immediately.
	char retry_error[128] = {};
	const WtExecPluginV2Status retry_status = api->initialize_unit(plugin.factory(), plugin.unit(),
		&context, "CFFEX.IF.2601", configuration, retry_error, sizeof(retry_error));
	EXPECT_EQ(retry_status, WT_EXEC_PLUGIN_V2_INITIALIZATION_FAILED);
	EXPECT_STREQ(retry_error, "execution-unit initialization was already attempted");
	EXPECT_EQ(context.commodity_lookups, 1u);
	EXPECT_EQ(context.session_lookups, 1u);
	EXPECT_EQ(context.log_calls, 1u);
	EXPECT_EQ(context.comm->retainCount(), 1u);
	EXPECT_EQ(context.sess->retainCount(), 1u);

	configuration->release();
}
}
