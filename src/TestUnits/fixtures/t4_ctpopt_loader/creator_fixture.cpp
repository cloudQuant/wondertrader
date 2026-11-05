#include <fstream>
#include <filesystem>

#include "r114_ctpopt_api_stub.h"

#ifndef WT_T4_CTP_OPT_UNLOAD_MARKER
#define WT_T4_CTP_OPT_UNLOAD_MARKER "ctpopt_creator_unloaded"
#endif

#if defined(_WIN32)
#define T4_CTP_OPT_EXPORT __declspec(dllexport)
#else
#define T4_CTP_OPT_EXPORT __attribute__((visibility("default")))
#endif

namespace
{
class T4CtpOptLoaderApi final : public R114TraderApi
{
public:
	~T4CtpOptLoaderApi()
	{
		delete _spi;
	}

	void RegisterSpi(CThostFtdcTraderSpi* spi) override
	{
		_spi = static_cast<CTraderSpi*>(spi);
		R114TraderApi::RegisterSpi(spi);
	}

	void Release() override
	{
		R114TraderApi::Release();
		delete _spi;
		_spi = nullptr;
	}

	// K02=B (2026-11-04): run() now waits for a terminal result, so the fake drives
	// a successful final instrument packet as soon as the API is initialized.
	void Init() override
	{
		R114TraderApi::Init();
		if (_spi != nullptr)
			_spi->OnRspQryInstrument(nullptr, nullptr, 1, true);
	}

private:
	CTraderSpi* _spi = nullptr;
};

T4CtpOptLoaderApi g_api;

struct UnloadMarker
{
	~UnloadMarker()
	{
		std::ofstream marker(WT_T4_CTP_OPT_UNLOAD_MARKER);
		marker << "unloaded\n";
	}
};

UnloadMarker g_unloadMarker;
}

namespace ctp_sopt
{
T4_CTP_OPT_EXPORT CThostFtdcTraderApi* CThostFtdcTraderApi::CreateFtdcTraderApi(const char* flowPath)
{
	const bool flowPathExists = flowPath != nullptr && std::filesystem::exists(flowPath);
	std::ofstream flowPathProbe(flowPathExists
		? "ctpopt_creator_saw_flow_path"
		: "ctpopt_creator_flow_path_missing");
	flowPathProbe << (flowPathExists ? "exists\n" : "missing\n");
#if defined(WT_T4_CTP_OPT_NULL_CREATOR)
	return nullptr;
#else
	return &g_api;
#endif
}
}
