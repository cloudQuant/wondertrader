#include "Includes/UftStrategyDefs.h"
#include "Includes/IUftStraCtx.h"
#include "Includes/WTSVariant.hpp"

#include <cstdlib>
#include <fstream>
#include <string>

namespace {

const char* kFactoryName = "R95FakeUft";

void appendEvent(const std::string& event)
{
	const char* path = std::getenv("WT_R95_UFT_STRATEGY_LOG");
	if (path == nullptr || path[0] == '\0')
		return;
	std::ofstream out(path, std::ios::app);
	if (out)
		out << event << '\n';
}

class R95FakeStrategy final : public UftStrategy
{
public:
	explicit R95FakeStrategy(const char* id) : UftStrategy(id) {}
	~R95FakeStrategy() override { appendEvent(std::string("strategy.destroy:") + id()); }

	const char* getName() override { return id(); }
	const char* getFactName() override { return kFactoryName; }

	bool init(WTSVariant* cfg) override
	{
		const bool succeed = cfg != nullptr && cfg->getBoolean("succeed");
		appendEvent(std::string("strategy.init:") + id() + (succeed ? ":true" : ":false"));
		return succeed;
	}

	void on_init(IUftStraCtx*) override {}

	void on_channel_ready(IUftStraCtx* ctx) override
	{
		appendEvent(std::string("strategy.ready:") + id() + ":" +
			std::to_string(ctx == nullptr ? 0 : ctx->id()));
	}
};

class R95FakeStrategyFactory final : public IUftStrategyFact
{
public:
	~R95FakeStrategyFactory() override { appendEvent("factory.destroy"); }

	const char* getName() override { return kFactoryName; }

	void enumStrategy(FuncEnumUftStrategyCallback cb) override
	{
		if (cb != nullptr)
		{
			cb(kFactoryName, "Success", false);
			cb(kFactoryName, "Failure", true);
		}
	}

	UftStrategy* createStrategy(const char* name, const char* id) override
	{
		if (name == nullptr || id == nullptr)
			return nullptr;
		if (std::string(name) != "Success" && std::string(name) != "Failure")
			return nullptr;
		appendEvent(std::string("strategy.create:") + id);
		return new R95FakeStrategy(id);
	}

	bool deleteStrategy(UftStrategy* strategy) override
	{
		delete strategy;
		return true;
	}
};

} // namespace

extern "C"
{
	EXPORT_FLAG IUftStrategyFact* createStrategyFact()
	{
		appendEvent("factory.create");
		return new R95FakeStrategyFactory();
	}

	EXPORT_FLAG void deleteStrategyFact(IUftStrategyFact* fact)
	{
		delete fact;
	}
}

// Versioned UFT strategy plugin entry (iteration 03, card 4b=A). Hosts no
// longer call the legacy exports above. The factory and its strategies start
// no asynchronous work, so the fixture advertises synchronous teardown (9a).
#include "Includes/IStrategyPluginV2.h"

namespace
{
	void* WT_STRA_PLUGIN_V2_CALL r95CreateFactoryV2()
	{
		try
		{
			appendEvent("factory.create");
			return static_cast<void*>(static_cast<IUftStrategyFact*>(new R95FakeStrategyFactory()));
		}
		catch (...)
		{
			return nullptr;
		}
	}

	void WT_STRA_PLUGIN_V2_CALL r95DestroyFactoryV2(void* fact)
	{
		delete static_cast<IUftStrategyFact*>(fact);
	}

	const WtStrategyPluginApiV2 g_r95_plugin_api_v2 = {
		sizeof(WtStrategyPluginApiV2),
		WT_STRA_PLUGIN_V2_ABI_VERSION,
		WT_STRA_PLUGIN_V2_FAMILY_UFT,
		WT_STRA_PLUGIN_V2_OBJECT_REVISION,
		WT_STRA_PLUGIN_V2_CAP_SYNC_TEARDOWN,
		{ 0, 0, 0 },
		r95CreateFactoryV2,
		r95DestroyFactoryV2
	};
}

WT_STRA_PLUGIN_V2_EXTERN_C WT_STRA_PLUGIN_V2_EXPORT
const WtStrategyPluginApiV2* WT_STRA_PLUGIN_V2_CALL getUftStrategyPluginApiV2(uint32_t requested_major)
{
	return requested_major == WT_STRA_PLUGIN_V2_ABI_MAJOR ? &g_r95_plugin_api_v2 : nullptr;
}
