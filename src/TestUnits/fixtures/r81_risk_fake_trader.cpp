#include "Includes/ITraderApi.h"
#include "Includes/WTSTradeDef.hpp"
#include "Includes/WTSVariant.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace
{
std::atomic<uint32_t> g_order_insert_count{ 0 };
std::atomic<uint32_t> g_order_action_count{ 0 };

class R81RiskFakeTrader final : public wtp::ITraderApi
{
public:
	bool init(wtp::WTSVariant*) override { return true; }
	void release() override {}
	void registerSpi(wtp::ITraderSpi*) override {}
	void connect() override {}
	void disconnect() override {}
	bool isConnected() override { return true; }
	bool makeEntrustID(char* buffer, int length) override
	{
		if (buffer == nullptr || length <= 0)
			return false;
		const int written = std::snprintf(buffer, static_cast<size_t>(length), "R81F%04u",
			++_entrust_sequence);
		return written >= 0 && written < length;
	}
	int orderInsert(wtp::WTSEntrust*) override
	{
		g_order_insert_count.fetch_add(1, std::memory_order_relaxed);
		return 0;
	}
	int orderAction(wtp::WTSEntrustAction*) override
	{
		g_order_action_count.fetch_add(1, std::memory_order_relaxed);
		return 0;
	}
	int queryAccount() override { return 0; }
	int queryPositions() override { return 0; }
	int queryOrders() override { return 0; }
	int queryTrades() override { return 0; }

private:
	uint32_t _entrust_sequence = 0;
};
}

#if defined(_WIN32)
#define R81_RISK_EXPORT __declspec(dllexport)
#else
#define R81_RISK_EXPORT __attribute__((visibility("default")))
#endif

extern "C" R81_RISK_EXPORT wtp::ITraderApi* createTrader()
{
	g_order_insert_count.store(0, std::memory_order_relaxed);
	g_order_action_count.store(0, std::memory_order_relaxed);
	return new R81RiskFakeTrader();
}

extern "C" R81_RISK_EXPORT void deleteTrader(wtp::ITraderApi*& trader)
{
	delete trader;
	trader = nullptr;
}

extern "C" R81_RISK_EXPORT uint32_t r81RiskFakeOrderInsertCount()
{
	return g_order_insert_count.load(std::memory_order_relaxed);
}

extern "C" R81_RISK_EXPORT uint32_t r81RiskFakeOrderActionCount()
{
	return g_order_action_count.load(std::memory_order_relaxed);
}
