// Local fake of the XTP 2.2.32.2 quote SDK. Login() is synchronous (as the
// vendored header documents) and starts an SDK-owned callback thread that
// Release() joins. No network or vendor code is involved.
#include "fake_sdk.hpp"

#include <cstring>

#include <API/XTP2.2.32.2/xtp_quote_api.h>

namespace
{
	class FakeQuoteApi;
	std::mutex g_mutex;
	FakeQuoteApi* g_current = nullptr;
	XTPRI g_error = {};

	void fillDepth(XTPMD& d)
	{
		std::memset(&d, 0, sizeof(d));
		d.exchange_id = XTP_EXCHANGE_SH;
		std::strncpy(d.ticker, "600000", sizeof(d.ticker) - 1);
		d.last_price = 10.5;
		d.data_time = 20261104100000000LL;
	}

	void deliverDepth(XTP::API::QuoteSpi* spi)
	{
		XTPMD d;
		fillDepth(d);
		int64_t bid[1] = { 0 }, ask[1] = { 0 };
		spi->OnDepthMarketData(&d, bid, 0, 0, ask, 0, 0);
	}

	class FakeQuoteApi final : public XTP::API::QuoteApi
	{
	public:
		~FakeQuoteApi()
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (g_current == this)
				g_current = nullptr;
		}
		void Release() override
		{
			fakesdk::counter(fakesdk::C_RELEASE)++;
			if (_thread.onThread())
			{
				fakesdk::counter(fakesdk::C_RELEASE_ON_SDK_THREAD)++;
				return;
			}
			if (fakesdk::releaseOverlapsCallback(_thread.busy()))
				fakesdk::counter(fakesdk::C_RELEASE_DURING_CALLBACK)++;
			XTP::API::QuoteSpi* stale = _lastSpi.load();
			if (fakesdk::option(fakesdk::OPT_LATE_CALLBACK).load() != 0 && stale != nullptr)
			{
				_thread.post([stale]() {
					fakesdk::counter(fakesdk::C_LATE_DISPATCHED)++;
					deliverDepth(stale);
				});
				_thread.waitIdle(2000);
			}
			_thread.stopJoin();
			delete this;
		}
		const char* GetTradingDay() override { return "20261104"; }
		const char* GetApiVersion() override { return "fake-xtp-v2-20261104"; }
		XTPRI* GetApiLastError() override { return &g_error; }
		void SetUDPBufferSize(uint32_t) override {}
		void RegisterSpi(XTP::API::QuoteSpi* spi) override
		{
			_spi.store(spi);
			if (spi != nullptr)
				_lastSpi.store(spi);
			else
				fakesdk::counter(fakesdk::C_SPI_NULL)++;
		}
		void SetHeartBeatInterval(uint32_t) override {}
		void SetUDPRecvThreadAffinity(int32_t) override {}
		void SetUDPRecvThreadAffinityArray(int32_t[], int32_t) override {}
		void SetUDPParseThreadAffinity(int32_t) override {}
		void SetUDPParseThreadAffinityArray(int32_t[], int32_t) override {}
		void SetUDPSeqLogOutPutFlag(bool) override {}
		int SubscribeMarketData(char*[], int, XTP_EXCHANGE_TYPE) override
		{
			fakesdk::counter(fakesdk::C_SUB_REQ)++;
			if (fakesdk::option(fakesdk::OPT_BLOCK_SUBSCRIBE).load() != 0)
				fakesdk::subscribeLatch().wait(5000);
			return 0;
		}
		int UnSubscribeMarketData(char*[], int, XTP_EXCHANGE_TYPE) override { return 0; }
		int SubscribeOrderBook(char*[], int, XTP_EXCHANGE_TYPE) override { return 0; }
		int UnSubscribeOrderBook(char*[], int, XTP_EXCHANGE_TYPE) override { return 0; }
		int SubscribeTickByTick(char*[], int, XTP_EXCHANGE_TYPE) override { return 0; }
		int UnSubscribeTickByTick(char*[], int, XTP_EXCHANGE_TYPE) override { return 0; }
		int SubscribeAllMarketData(XTP_EXCHANGE_TYPE) override { return 0; }
		int UnSubscribeAllMarketData(XTP_EXCHANGE_TYPE) override { return 0; }
		int SubscribeAllOrderBook(XTP_EXCHANGE_TYPE) override { return 0; }
		int UnSubscribeAllOrderBook(XTP_EXCHANGE_TYPE) override { return 0; }
		int SubscribeAllTickByTick(XTP_EXCHANGE_TYPE) override { return 0; }
		int UnSubscribeAllTickByTick(XTP_EXCHANGE_TYPE) override { return 0; }
		int Login(const char*, int, const char*, const char*, XTP_PROTOCOL_TYPE, const char*) override
		{
			fakesdk::counter(fakesdk::C_LOGIN_REQ)++;
			const int result = fakesdk::option(fakesdk::OPT_LOGIN_RESULT).load();
			if (result == 0)
			{
				fakesdk::counter(fakesdk::C_INIT)++;
				_thread.start();
			}
			return result;
		}
		int Logout() override
		{
			fakesdk::counter(fakesdk::C_LOGOUT)++;
			return 0;
		}
		int QueryAllTickers(XTP_EXCHANGE_TYPE) override { return 0; }
		int QueryTickersPriceInfo(char*[], int, XTP_EXCHANGE_TYPE) override { return 0; }
		int QueryAllTickersPriceInfo() override { return 0; }
		int SubscribeAllOptionMarketData(XTP_EXCHANGE_TYPE) override { return 0; }
		int UnSubscribeAllOptionMarketData(XTP_EXCHANGE_TYPE) override { return 0; }
		int SubscribeAllOptionOrderBook(XTP_EXCHANGE_TYPE) override { return 0; }
		int UnSubscribeAllOptionOrderBook(XTP_EXCHANGE_TYPE) override { return 0; }
		int SubscribeAllOptionTickByTick(XTP_EXCHANGE_TYPE) override { return 0; }
		int UnSubscribeAllOptionTickByTick(XTP_EXCHANGE_TYPE) override { return 0; }
		int QueryAllTickersFullInfo(XTP_EXCHANGE_TYPE) override { return 0; }

		int post(int kind)
		{
			return _thread.post([this, kind]() {
				XTP::API::QuoteSpi* spi = _spi.load();
				if (spi == nullptr)
					return;
				fakesdk::counter(fakesdk::C_DISPATCHED)++;
				switch (kind)
				{
				case fakesdk::K_DEPTH: deliverDepth(spi); break;
				case fakesdk::K_DISCONNECTED: spi->OnDisconnected(0x1001); break;
				default: break;
				}
			}) ? 1 : 0;
		}
		bool waitIdle(int ms) { return _thread.waitIdle(ms); }
		bool onThread() const { return _thread.onThread(); }

	private:
		fakesdk::SdkThread _thread;
		std::atomic<XTP::API::QuoteSpi*> _spi{nullptr};
		std::atomic<XTP::API::QuoteSpi*> _lastSpi{nullptr};
	};
}

XTP::API::QuoteApi* XTP::API::QuoteApi::CreateQuoteApi(uint8_t, const char*, XTP_LOG_LEVEL)
{
	if (fakesdk::option(fakesdk::OPT_CREATE_NULL).load() != 0)
		return nullptr;
	FakeQuoteApi* api = new FakeQuoteApi();
	fakesdk::counter(fakesdk::C_CREATED)++;
	std::lock_guard<std::mutex> lock(g_mutex);
	g_current = api;
	return api;
}

FAKE_MD_COMMON_CONTROL()

FAKE_MD_EXPORT int fake_md_post(int kind)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_current ? g_current->post(kind) : 0;
}

FAKE_MD_EXPORT int fake_md_wait_idle(int ms)
{
	FakeQuoteApi* api;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		api = g_current;
	}
	return api ? (api->waitIdle(ms) ? 1 : 0) : 1;
}

FAKE_MD_EXPORT int fake_md_on_sdk_thread()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_current && g_current->onThread() ? 1 : 0;
}
