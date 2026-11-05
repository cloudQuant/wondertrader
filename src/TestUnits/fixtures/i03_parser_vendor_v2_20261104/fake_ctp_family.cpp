// Local fake of the CTP-family market-data SDK (CTP 6.6.9, CTPMini 1.5.8,
// CTPOpt 3.5.8), selected with FAKE_FAMILY_CTP / FAKE_FAMILY_CTPMINI /
// FAKE_FAMILY_CTPOPT. Init() starts an SDK-owned callback thread; Release()
// joins it (refusing a self-join, which a real SDK would deadlock on) and
// deletes the object. No network or vendor code is involved.
#include "fake_sdk.hpp"

#include <cstring>

#if defined(FAKE_FAMILY_CTP)
#include <API/CTP6.6.9/ThostFtdcMdApi.h>
#define FAKE_NS
#elif defined(FAKE_FAMILY_CTPMINI)
#define MD_API_EXPORT
#include <API/CTPMini1.5.8/ThostFtdcMdApi.h>
#define FAKE_NS
#elif defined(FAKE_FAMILY_CTPOPT)
#include <API/CTPOpt3.5.8/ThostFtdcMdApi.h>
#define FAKE_NS ctp_sopt::
#else
#error "select a fake family"
#endif

namespace
{
	using MdApi = FAKE_NS CThostFtdcMdApi;
	using MdSpi = FAKE_NS CThostFtdcMdSpi;
	using Depth = FAKE_NS CThostFtdcDepthMarketDataField;
	using LoginReq = FAKE_NS CThostFtdcReqUserLoginField;
	using Logout = FAKE_NS CThostFtdcUserLogoutField;

	class FakeMdApi;
	std::mutex g_mutex;
	FakeMdApi* g_current = nullptr;

	void fillDepth(Depth& d)
	{
		std::memset(&d, 0, sizeof(d));
		std::strncpy(d.InstrumentID, "ag2512", sizeof(d.InstrumentID) - 1);
		std::strncpy(d.ExchangeID, "SHFE", sizeof(d.ExchangeID) - 1);
		std::strncpy(d.ActionDay, "20261104", sizeof(d.ActionDay) - 1);
		std::strncpy(d.TradingDay, "20261104", sizeof(d.TradingDay) - 1);
		std::strncpy(d.UpdateTime, "10:00:00", sizeof(d.UpdateTime) - 1);
		d.LastPrice = 7011.0;
		d.Volume = 1;
	}

	class FakeMdApi final : public MdApi
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
				// A real SDK would join its own thread here and deadlock.
				fakesdk::counter(fakesdk::C_RELEASE_ON_SDK_THREAD)++;
				return;
			}
			if (fakesdk::releaseOverlapsCallback(_thread.busy()))
				fakesdk::counter(fakesdk::C_RELEASE_DURING_CALLBACK)++;
			MdSpi* stale = _lastSpi.load();
			if (fakesdk::option(fakesdk::OPT_LATE_CALLBACK).load() != 0 && stale != nullptr)
			{
				// Model an SDK that still delivers one queued callback through the
				// SPI pointer it captured earlier, after RegisterSpi(NULL).
				_thread.post([stale]() {
					fakesdk::counter(fakesdk::C_LATE_DISPATCHED)++;
					Depth d;
					fillDepth(d);
					stale->OnRtnDepthMarketData(&d);
				});
				_thread.waitIdle(2000);
			}
			_thread.stopJoin();
			delete this;
		}
#if defined(FAKE_FAMILY_CTPMINI)
		void Init(bool) override
#else
		void Init() override
#endif
		{
			fakesdk::counter(fakesdk::C_INIT)++;
			_thread.start();
		}
		int Join() override { return 0; }
		const char* GetTradingDay() override { return "20261104"; }
		void RegisterFront(char*) override {}
#if !defined(FAKE_FAMILY_CTPMINI)
		void RegisterNameServer(char*) override {}
		void RegisterFensUserInfo(FAKE_NS CThostFtdcFensUserInfoField*) override {}
#endif
		void RegisterSpi(MdSpi* spi) override
		{
			_spi.store(spi);
			if (spi != nullptr)
				_lastSpi.store(spi);
			else
				fakesdk::counter(fakesdk::C_SPI_NULL)++;
		}
		int SubscribeMarketData(char*[], int) override
		{
			fakesdk::counter(fakesdk::C_SUB_REQ)++;
			if (fakesdk::option(fakesdk::OPT_BLOCK_SUBSCRIBE).load() != 0)
				fakesdk::subscribeLatch().wait(5000);
			return 0;
		}
		int UnSubscribeMarketData(char*[], int) override { return 0; }
		int SubscribeForQuoteRsp(char*[], int) override { return 0; }
		int UnSubscribeForQuoteRsp(char*[], int) override { return 0; }
		int ReqUserLogin(LoginReq*, int) override
		{
			fakesdk::counter(fakesdk::C_LOGIN_REQ)++;
			return 0;
		}
		int ReqUserLogout(Logout*, int) override
		{
			fakesdk::counter(fakesdk::C_LOGOUT)++;
			return 0;
		}
#if defined(FAKE_FAMILY_CTP)
		int ReqQryMulticastInstrument(CThostFtdcQryMulticastInstrumentField*, int) override { return 0; }
#endif

		int post(int kind)
		{
			return _thread.post([this, kind]() {
				MdSpi* spi = _spi.load();
				if (spi == nullptr)
					return;
				fakesdk::counter(fakesdk::C_DISPATCHED)++;
				switch (kind)
				{
				case fakesdk::K_FRONT_CONNECTED: spi->OnFrontConnected(); break;
				case fakesdk::K_LOGIN: spi->OnRspUserLogin(nullptr, nullptr, 1, true); break;
				case fakesdk::K_DEPTH: { Depth d; fillDepth(d); spi->OnRtnDepthMarketData(&d); break; }
				case fakesdk::K_DISCONNECTED: spi->OnFrontDisconnected(0x1001); break;
				default: break;
				}
			}) ? 1 : 0;
		}
		bool waitIdle(int ms) { return _thread.waitIdle(ms); }
		bool onThread() const { return _thread.onThread(); }

	private:
		fakesdk::SdkThread _thread;
		std::atomic<MdSpi*> _spi{nullptr};
		std::atomic<MdSpi*> _lastSpi{nullptr};
	};
}

MdApi* MdApi::CreateFtdcMdApi(const char*, const bool, const bool)
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
