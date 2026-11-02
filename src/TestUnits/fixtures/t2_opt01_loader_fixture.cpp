#include "API/CTPOpt3.5.8/ThostFtdcMdApi.h"

#ifndef WT_R105_CTP_OPT_NULL_CREATOR
namespace {

class R105LoaderMdApi final : public ctp_sopt::CThostFtdcMdApi
{
public:
	void Release() override { delete this; }
	void Init() override {}
	int Join() override { return 0; }
	const char* GetTradingDay() override { return "20261030"; }
	void RegisterFront(char*) override {}
	void RegisterNameServer(char*) override {}
	void RegisterFensUserInfo(ctp_sopt::CThostFtdcFensUserInfoField*) override {}
	void RegisterSpi(ctp_sopt::CThostFtdcMdSpi*) override {}
	int SubscribeMarketData(char**, int) override { return 0; }
	int UnSubscribeMarketData(char**, int) override { return 0; }
	int SubscribeForQuoteRsp(char**, int) override { return 0; }
	int UnSubscribeForQuoteRsp(char**, int) override { return 0; }
	int ReqUserLogin(ctp_sopt::CThostFtdcReqUserLoginField*, int) override { return 0; }
	int ReqUserLogout(ctp_sopt::CThostFtdcUserLogoutField*, int) override { return 0; }
};

} // namespace
#endif

ctp_sopt::CThostFtdcMdApi* ctp_sopt::CThostFtdcMdApi::CreateFtdcMdApi(
	const char*, const bool, const bool)
{
#ifdef WT_R105_CTP_OPT_NULL_CREATOR
	return nullptr;
#else
	return new R105LoaderMdApi();
#endif
}
