// Local fake of the Femas 3.02 market-data SDK with an SDK-owned callback
// thread started by Init() and joined by Release(). No network or vendor code.
#include "fake_sdk.hpp"

#include <cstring>

#include <API/Femas3.02/USTPFtdcMduserApi.h>

namespace
{
	class FakeMdApi;
	std::mutex g_mutex;
	FakeMdApi* g_current = nullptr;

	void fillDepth(CUstpFtdcDepthMarketDataField& d)
	{
		std::memset(&d, 0, sizeof(d));
		std::strncpy(d.InstrumentID, "ag2512", sizeof(d.InstrumentID) - 1);
		std::strncpy(d.ActionDay, "20261104", sizeof(d.ActionDay) - 1);
		std::strncpy(d.TradingDay, "20261104", sizeof(d.TradingDay) - 1);
		std::strncpy(d.UpdateTime, "10:00:00", sizeof(d.UpdateTime) - 1);
		d.LastPrice = 7011.0;
		d.Volume = 1;
	}

	class FakeMdApi final : public CUstpFtdcMduserApi
	{
	public:
		~FakeMdApi()
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
			CUstpFtdcMduserSpi* stale = _lastSpi.load();
			if (fakesdk::option(fakesdk::OPT_LATE_CALLBACK).load() != 0 && stale != nullptr)
			{
				_thread.post([stale]() {
					fakesdk::counter(fakesdk::C_LATE_DISPATCHED)++;
					CUstpFtdcDepthMarketDataField d;
					fillDepth(d);
					stale->OnRtnDepthMarketData(&d);
				});
				_thread.waitIdle(2000);
			}
			_thread.stopJoin();
			delete this;
		}
		void Init() override
		{
			fakesdk::counter(fakesdk::C_INIT)++;
			_thread.start();
		}
		int Join() override { return 0; }
		void SetUseMultiChannel(bool) override {}
		void RegisterMultiChannel(const char*) override {}
		const char* GetTradingDay() override { return "20261104"; }
		void RegisterFront(char*) override {}
		void RegisterNameServer(char*) override {}
		void RegisterSpi(CUstpFtdcMduserSpi* spi) override
		{
			_spi.store(spi);
			if (spi != nullptr)
				_lastSpi.store(spi);
			else
				fakesdk::counter(fakesdk::C_SPI_NULL)++;
		}
		int RegisterCertificateFile(const char*, const char*, const char*, const char*) override { return 0; }
		void SubscribeMarketDataTopic(int, USTP_TE_RESUME_TYPE) override {}
		int SubMarketData(char*[], int) override
		{
			fakesdk::counter(fakesdk::C_SUB_REQ)++;
			if (fakesdk::option(fakesdk::OPT_BLOCK_SUBSCRIBE).load() != 0)
				fakesdk::subscribeLatch().wait(5000);
			return 0;
		}
		int UnSubMarketData(char*[], int) override { return 0; }
		int GetMarketTopic(char*) override { return 0; }
		int GetMarketData(char*, char*) override { return 0; }
		void SetHeartbeatTimeout(unsigned int) override {}
		int ReqUserLogin(CUstpFtdcReqUserLoginField*, int) override
		{
			fakesdk::counter(fakesdk::C_LOGIN_REQ)++;
			return 0;
		}
		int ReqUserLogout(CUstpFtdcReqUserLogoutField*, int) override
		{
			fakesdk::counter(fakesdk::C_LOGOUT)++;
			return 0;
		}

		int post(int kind)
		{
			return _thread.post([this, kind]() {
				CUstpFtdcMduserSpi* spi = _spi.load();
				if (spi == nullptr)
					return;
				fakesdk::counter(fakesdk::C_DISPATCHED)++;
				switch (kind)
				{
				case fakesdk::K_FRONT_CONNECTED: spi->OnFrontConnected(); break;
				case fakesdk::K_LOGIN: spi->OnRspUserLogin(nullptr, nullptr, 1, true); break;
				case fakesdk::K_DEPTH: { CUstpFtdcDepthMarketDataField d; fillDepth(d); spi->OnRtnDepthMarketData(&d); break; }
				case fakesdk::K_DISCONNECTED: spi->OnFrontDisconnected(0x1001); break;
				default: break;
				}
			}) ? 1 : 0;
		}
		bool waitIdle(int ms) { return _thread.waitIdle(ms); }
		bool onThread() const { return _thread.onThread(); }

	private:
		fakesdk::SdkThread _thread;
		std::atomic<CUstpFtdcMduserSpi*> _spi{nullptr};
		std::atomic<CUstpFtdcMduserSpi*> _lastSpi{nullptr};
	};
}

CUstpFtdcMduserApi* CUstpFtdcMduserApi::CreateFtdcMduserApi(const char*)
{
	if (fakesdk::option(fakesdk::OPT_CREATE_NULL).load() != 0)
		return nullptr;
	FakeMdApi* api = new FakeMdApi();
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
	FakeMdApi* api;
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
