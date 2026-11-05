// Local fake risk-monitor plugin (card 9a / BUG-I02-NEW-034). The monitor
// starts a worker in init() that keeps calling its WtPortContext. With
// -DFAKE_SYNC_STOP it exports the IRiskMonPluginV2 capability and stop()
// joins the worker. Without it, stop() only requests stop and the worker keeps
// using the context for a while (an unproven third-party plugin).
// No provider, network or order is involved.
#include "../../../Includes/RiskMonDefs.h"
#include "../../../Includes/IRiskMonPluginV2.h"
#include "../../../Includes/WTSRiskDef.hpp"

#include <atomic>
#include <chrono>
#include <thread>

#define FAKE_EXPORT extern "C" __attribute__((visibility("default")))

using namespace wtp;

namespace {
std::atomic<int> g_factory_deletes{0}, g_creates{0}, g_stops{0}, g_monitor_deletes{0}, g_ctx_calls{0}, g_late_calls{0}, g_null_funds{0};
std::atomic<bool> g_worker_done{false};

class Monitor : public WtRiskMonitor
{
public:
	~Monitor() override
	{
		_stop.store(true);
		if (_worker.joinable())
			_worker.join();
	}
	const char* getName() override { return "fake"; }
	const char* getFactName() override { return "fake-fact"; }
	void init(WtPortContext* ctx, WTSVariant*) override
	{
		_ctx = ctx;
		_worker = std::thread([this] {
#ifndef FAKE_SYNC_STOP
			std::chrono::steady_clock::time_point stopAt{};
#endif
			for (;;)
			{
				WTSPortFundInfo* fund = _ctx->getFundInfo();
				if (fund == nullptr)
					++g_null_funds;
				else
					(void)fund->fundInfo()._balance;
				(void)_ctx->isInTrading();
				(void)_ctx->getTradingDate();
				++g_ctx_calls;
				if (_stop.load())
				{
#ifdef FAKE_SYNC_STOP
					break;
#else
					// Unproven plugin: keeps working for 300 ms after stop().
					if (stopAt == std::chrono::steady_clock::time_point{})
						stopAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
					++g_late_calls;
					if (std::chrono::steady_clock::now() >= stopAt)
						break;
#endif
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
			g_worker_done.store(true);
		});
	}
	void stop() override
	{
		++g_stops;
		_stop.store(true);
#ifdef FAKE_SYNC_STOP
		if (_worker.joinable() && _worker.get_id() != std::this_thread::get_id())
			_worker.join();
#endif
	}

private:
	std::atomic<bool> _stop{false};
	std::thread _worker;
};

class Factory : public IRiskMonitorFact
{
public:
	~Factory() override { ++g_factory_deletes; }
	const char* getName() override { return "fake-fact"; }
	void enumRiskMonitors(FuncEnumRiskMonCallback) override {}
	WtRiskMonitor* createRiskMonotor(const char*) override { ++g_creates; return new Monitor(); }
	bool deleteRiskMonotor(WtRiskMonitor* unit) override { ++g_monitor_deletes; delete unit; return true; }
};
}

FAKE_EXPORT IRiskMonitorFact* createRiskMonFact() { return new Factory(); }
FAKE_EXPORT void deleteRiskMonFact(IRiskMonitorFact* fact) { delete fact; }

#ifdef FAKE_SYNC_STOP
namespace {
void destroyFactoryV2(void* fact) { delete static_cast<IRiskMonitorFact*>(fact); }
}
FAKE_EXPORT const WtRiskMonPluginCapsV2* getRiskMonPluginCapsV2(uint32_t major)
{
	static const WtRiskMonPluginCapsV2 caps = { sizeof(WtRiskMonPluginCapsV2),
		WT_RISKMON_PLUGIN_V2_ABI_VERSION, WT_RISKMON_PLUGIN_V2_CAP_SYNC_STOP, 0, destroyFactoryV2 };
	return major == WT_RISKMON_PLUGIN_V2_ABI_MAJOR ? &caps : nullptr;
}
#endif

FAKE_EXPORT int fixture_factory_deletes() { return g_factory_deletes.load(); }
FAKE_EXPORT int fixture_creates() { return g_creates.load(); }
FAKE_EXPORT int fixture_stops() { return g_stops.load(); }
FAKE_EXPORT int fixture_monitor_deletes() { return g_monitor_deletes.load(); }
FAKE_EXPORT int fixture_ctx_calls() { return g_ctx_calls.load(); }
FAKE_EXPORT int fixture_late_calls() { return g_late_calls.load(); }
FAKE_EXPORT int fixture_null_funds() { return g_null_funds.load(); }
FAKE_EXPORT int fixture_worker_done() { return g_worker_done.load() ? 1 : 0; }
