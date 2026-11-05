// Local fake parser provider for the data-kit parser host lifecycle checks
// (iteration 03, card 9). No vendor SDK, network or market data is used.
//
// The provider owns one callback thread per initialized API. The thread calls
// IParserSpi::handleParserLog until a V2 stop request arrives. The legacy
// IParserApi::release() deliberately does NOT stop the thread, modelling a
// provider whose release is not a drain barrier. The legacy deleteParser
// refuses (and counts) deletion while the thread may still call the SPI.
//
// Build with -DFAKE_NO_V2 for a legacy module that does not export the V2 getter.
#include "Includes/IParserApi.h"
#include "Includes/IParserLifecycleV2.h"
#include "Includes/WTSDataDef.hpp"

#include <cstring>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace
{
enum Counter
{
	C_CREATE, C_REGISTER, C_UNREGISTER, C_INIT, C_SUBSCRIBE, C_CONNECT, C_RELEASE,
	C_DELETE_OK, C_DELETE_UNSAFE, C_REQUEST_STOP, C_CAN_WAIT, C_WAIT, C_DESTROY,
	C_CALLBACKS, C_CALLBACKS_AFTER_RELEASE, C_PREFLIGHT, C_LIVE, C_COUNT
};
std::atomic<int> g_counts[C_COUNT];

std::atomic<int> g_init_ok{1};
std::atomic<int> g_start_thread{1};
std::atomic<int> g_wait_mode{0};		// 0 normal, 1 always PENDING, 2 ERROR
std::atomic<int> g_destroy_mode{0};		// 0 delete, 1 ERROR
std::atomic<int> g_init_throws{0};
std::atomic<int> g_callback_interval_us{200};
std::atomic<int> g_quote_mode{0};		// 1: deliver CFFEX.IC2204 ticks through handleQuote
// Optional host hook invoked from the provider thread inside a callback
// window: lets a test call adapter release() from within a callback.
std::atomic<void (*)(void*)> g_in_callback_hook{nullptr};
std::atomic<void*> g_in_callback_ctx{nullptr};

class FakeParser final : public wtp::IParserApi
{
public:
	FakeParser() { ++g_counts[C_LIVE]; }
	~FakeParser() override
	{
		_stop = true;
		if (_thread.joinable() && _thread.get_id() != std::this_thread::get_id())
			_thread.join();
		--g_counts[C_LIVE];
	}

	void registerSpi(wtp::IParserSpi* spi) override
	{
		++g_counts[spi == nullptr ? C_UNREGISTER : C_REGISTER];
		_spi.store(spi);
	}

	bool init(wtp::WTSVariant*) override
	{
		++g_counts[C_INIT];
		if (g_init_throws.load())
			throw 7;
		if (g_start_thread.load())
		{
			_thread = std::thread([this]() { loop(); });
			_started = true;
		}
		return g_init_ok.load() != 0;
	}

	void subscribe(const wtp::CodeSet&) override { ++g_counts[C_SUBSCRIBE]; }
	bool connect() override { ++g_counts[C_CONNECT]; return true; }
	void release() override { ++g_counts[C_RELEASE]; _legacy_released = true; }

	bool threadRunning() const { return _started && !_exited.load(); }
	bool onProviderThread() const { return _started && _thread.get_id() == std::this_thread::get_id(); }
	void requestStop() { _stop = true; }
	bool exited() const { return !_started || _exited.load(); }
	void join()
	{
		if (_thread.joinable() && _thread.get_id() != std::this_thread::get_id())
			_thread.join();
	}

private:
	void loop()
	{
		while (!_stop.load())
		{
			wtp::IParserSpi* spi = _spi.load();
			if (spi != nullptr)
			{
				if (_legacy_released.load())
					++g_counts[C_CALLBACKS_AFTER_RELEASE];
				if (g_quote_mode.load())
				{
					wtp::WTSTickStruct ts;
					std::memset(&ts, 0, sizeof(ts));
					std::strcpy(ts.exchg, "CFFEX");
					std::strcpy(ts.code, "IC2204");
					ts.action_date = 20240102;
					ts.trading_date = 20240102;
					ts.action_time = 93000000;
					ts.price = 5000.0;
					wtp::WTSTickData* tick = wtp::WTSTickData::create(ts);
					if (tick != nullptr)
					{
						spi->handleQuote(tick, 1);
						tick->release();
					}
				}
				else
					spi->handleParserLog(wtp::LL_DEBUG, "fake provider callback");
				++g_counts[C_CALLBACKS];
				void (*hook)(void*) = g_in_callback_hook.exchange(nullptr);
				if (hook != nullptr)
					hook(g_in_callback_ctx.load());
			}
			std::this_thread::sleep_for(std::chrono::microseconds(g_callback_interval_us.load()));
		}
		_exited = true;
	}

	std::atomic<wtp::IParserSpi*> _spi{nullptr};
	std::atomic<bool> _stop{false};
	std::atomic<bool> _exited{false};
	std::atomic<bool> _legacy_released{false};
	bool _started = false;
	std::thread _thread;
};

#ifndef FAKE_NO_V2
WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL preflight()
{
	++g_counts[C_PREFLIGHT];
	return WT_PARSER_LIFECYCLE_V2_OK;
}

WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL request_stop(void* api)
{
	++g_counts[C_REQUEST_STOP];
	static_cast<FakeParser*>(static_cast<wtp::IParserApi*>(api))->requestStop();
	return WT_PARSER_LIFECYCLE_V2_OK;
}

WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL can_wait(void* api)
{
	++g_counts[C_CAN_WAIT];
	FakeParser* p = static_cast<FakeParser*>(static_cast<wtp::IParserApi*>(api));
	return p->onProviderThread() ? WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK : WT_PARSER_LIFECYCLE_V2_OK;
}

WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL wait_drained(void* api, uint32_t timeout_ms)
{
	++g_counts[C_WAIT];
	FakeParser* p = static_cast<FakeParser*>(static_cast<wtp::IParserApi*>(api));
	if (g_wait_mode.load() == 2)
		return WT_PARSER_LIFECYCLE_V2_ERROR;
	if (g_wait_mode.load() == 1)
		return WT_PARSER_LIFECYCLE_V2_PENDING;
	if (p->onProviderThread())
		return WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (!p->exited())
	{
		if (std::chrono::steady_clock::now() >= deadline)
			return WT_PARSER_LIFECYCLE_V2_PENDING;
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}
	p->join();
	return WT_PARSER_LIFECYCLE_V2_DRAINED;
}

WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL destroy_api(void** api)
{
	++g_counts[C_DESTROY];
	if (g_destroy_mode.load() == 1)
		return WT_PARSER_LIFECYCLE_V2_ERROR;
	delete static_cast<FakeParser*>(static_cast<wtp::IParserApi*>(*api));
	*api = nullptr;
	return WT_PARSER_LIFECYCLE_V2_OK;
}
#endif
}

extern "C" WT_PARSER_LIFECYCLE_V2_EXPORT wtp::IParserApi* createParser()
{
	++g_counts[C_CREATE];
	return new FakeParser();
}

extern "C" WT_PARSER_LIFECYCLE_V2_EXPORT void deleteParser(wtp::IParserApi*& parser)
{
	FakeParser* p = static_cast<FakeParser*>(parser);
	if (p != nullptr && p->threadRunning())
	{
		// A real SDK object would be freed under a running callback thread.
		++g_counts[C_DELETE_UNSAFE];
		return;
	}
	++g_counts[C_DELETE_OK];
	delete p;
	parser = nullptr;
}

#ifndef FAKE_NO_V2
extern "C" WT_PARSER_LIFECYCLE_V2_EXPORT const WtParserLifecycleApiV2* WT_PARSER_LIFECYCLE_V2_CALL getParserLifecycleApiV2(uint32_t major)
{
	static const WtParserLifecycleApiV2 table = {
		sizeof(WtParserLifecycleApiV2), WT_PARSER_LIFECYCLE_V2_ABI_VERSION,
		WT_PARSER_LIFECYCLE_V2_REQUIRED_CAPABILITIES, {0, 0},
		preflight, request_stop, can_wait, wait_drained, destroy_api };
	return major == WT_PARSER_LIFECYCLE_V2_ABI_MAJOR ? &table : nullptr;
}
#endif

extern "C" WT_PARSER_LIFECYCLE_V2_EXPORT void fake_reset()
{
	for (auto& c : g_counts)
		c = 0;
	g_init_ok = 1;
	g_start_thread = 1;
	g_wait_mode = 0;
	g_destroy_mode = 0;
	g_init_throws = 0;
	g_callback_interval_us = 200;
	g_quote_mode = 0;
	g_in_callback_hook = nullptr;
	g_in_callback_ctx = nullptr;
}
extern "C" WT_PARSER_LIFECYCLE_V2_EXPORT int fake_count(int index) { return g_counts[index].load(); }
extern "C" WT_PARSER_LIFECYCLE_V2_EXPORT void fake_set(int key, int value)
{
	switch (key)
	{
	case 0: g_init_ok = value; break;
	case 1: g_start_thread = value; break;
	case 2: g_wait_mode = value; break;
	case 3: g_destroy_mode = value; break;
	case 4: g_init_throws = value; break;
	case 5: g_callback_interval_us = value; break;
	case 6: g_quote_mode = value; break;
	default: break;
	}
}
extern "C" WT_PARSER_LIFECYCLE_V2_EXPORT void fake_set_hook(void (*hook)(void*), void* ctx)
{
	g_in_callback_ctx.store(ctx);
	g_in_callback_hook.store(hook);
}
