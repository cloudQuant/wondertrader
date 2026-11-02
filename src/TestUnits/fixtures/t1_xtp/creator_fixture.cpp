#include <atomic>
#include <cstring>

#include <API/XTP2.2.32.2/xtp_quote_api.h>

namespace
{
	std::atomic<int> g_nonnullSpiRegistrations(0);
	std::atomic<int> g_nullSpiRegistrations(0);
	std::atomic<int> g_releaseCalls(0);
	XTPRI g_lastError = {};

	class T1XtpLoaderQuoteApi final : public XTP::API::QuoteApi
	{
	public:
		void Release() override
		{
			++g_releaseCalls;
			delete this;
		}
		const char* GetTradingDay() override { return "20260105"; }
		const char* GetApiVersion() override { return "t1-xtp-loader-stub"; }
		XTPRI* GetApiLastError() override { return &g_lastError; }
		void SetUDPBufferSize(uint32_t) override {}
		void RegisterSpi(XTP::API::QuoteSpi* spi) override
		{
			if (spi != nullptr)
				++g_nonnullSpiRegistrations;
			else
				++g_nullSpiRegistrations;
		}
		void SetHeartBeatInterval(uint32_t) override {}
		void SetUDPRecvThreadAffinity(int32_t) override {}
		void SetUDPRecvThreadAffinityArray(int32_t[], int32_t) override {}
		void SetUDPParseThreadAffinity(int32_t) override {}
		void SetUDPParseThreadAffinityArray(int32_t[], int32_t) override {}
		void SetUDPSeqLogOutPutFlag(bool) override {}
		int SubscribeMarketData(char*[], int, XTP_EXCHANGE_TYPE) override { return 0; }
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
		int Login(const char*, int, const char*, const char*, XTP_PROTOCOL_TYPE, const char*) override { return 0; }
		int Logout() override { return 0; }
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
	};
}

XTP::API::QuoteApi* XTP::API::QuoteApi::CreateQuoteApi(
	uint8_t, const char*, XTP_LOG_LEVEL)
{
#ifdef WT_T1_XTP_NULL_CREATOR
	return nullptr;
#else
	return new T1XtpLoaderQuoteApi();
#endif
}

extern "C" MD_API_EXPORT int wt_t1_xtp_nonnull_spi_registrations()
{
	return g_nonnullSpiRegistrations.load();
}

extern "C" MD_API_EXPORT int wt_t1_xtp_null_spi_registrations()
{
	return g_nullSpiRegistrations.load();
}

extern "C" MD_API_EXPORT int wt_t1_xtp_release_calls()
{
	return g_releaseCalls.load();
}
