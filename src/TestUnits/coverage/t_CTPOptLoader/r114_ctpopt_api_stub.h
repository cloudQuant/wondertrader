/*
 * lane R114 —— CTPOptLoader 的厂商 API stub
 *
 * 只实现 ctp_sopt::CThostFtdcTraderApi 的纯虚接口，记录调用参数并返回可控值；
 * 不建 socket、不起线程、不 dlopen 厂商库。测试直接 new R114TraderApi() 后赋给
 * 生产 TU 的全局 pUserApi，从而在不触碰 run() 的模块加载尾段（BUG-T4-014）的前提下，
 * 覆盖 TraderSpi 的认证/登录/合约查询回报与 LoadFromJson。
 */
#pragma once

#include <string>

#include "CTPOptLoader/TraderSpi.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"

namespace r114_ctpopt {

struct StubState
{
	int releases = 0;
	int inits = 0;
	int joins = 0;
	int trading_day_calls = 0;
	int register_spis = 0;
	int register_fronts = 0;
	int auth_reqs = 0;
	int login_reqs = 0;
	int instrument_reqs = 0;
	int last_request_id = 0;
	int public_topic = -1;
	int private_topic = -1;
	std::string trading_day = "20261028";
	std::string flow_path;
	std::string front;
	std::string broker;
	std::string user;
	std::string password;
	std::string appid;
	std::string authcode;
	CThostFtdcTraderSpi* spi = nullptr;
};

inline StubState& state()
{
	static StubState s;
	return s;
}

inline void reset()
{
	state() = StubState();
}

}	// namespace r114_ctpopt

class R114TraderApi : public ctp_sopt::CThostFtdcTraderApi
{
public:
	~R114TraderApi() = default;

	void Release() override { ++r114_ctpopt::state().releases; }
	void Init() override { ++r114_ctpopt::state().inits; }
	int Join() override { ++r114_ctpopt::state().joins; return 0; }
	const char* GetTradingDay() override
	{
		++r114_ctpopt::state().trading_day_calls;
		return r114_ctpopt::state().trading_day.c_str();
	}
	void RegisterFront(char* pszFrontAddress) override
	{
		r114_ctpopt::state().front = pszFrontAddress ? pszFrontAddress : "";
		++r114_ctpopt::state().register_fronts;
	}
	void RegisterNameServer(char *pszNsAddress) override {}
	void RegisterFensUserInfo(CThostFtdcFensUserInfoField * pFensUserInfo) override {}
	void RegisterSpi(CThostFtdcTraderSpi* pSpi) override
	{
		r114_ctpopt::state().spi = pSpi;
		++r114_ctpopt::state().register_spis;
	}
	void SubscribePrivateTopic(THOST_TE_RESUME_TYPE nResumeType) override
	{
		r114_ctpopt::state().private_topic = (int)nResumeType;
	}
	void SubscribePublicTopic(THOST_TE_RESUME_TYPE nResumeType) override
	{
		r114_ctpopt::state().public_topic = (int)nResumeType;
	}
	int ReqAuthenticate(CThostFtdcReqAuthenticateField* p, int nRequestID) override
	{
		++r114_ctpopt::state().auth_reqs;
		r114_ctpopt::state().last_request_id = nRequestID;
		if (p)
		{
			r114_ctpopt::state().broker = p->BrokerID;
			r114_ctpopt::state().user = p->UserID;
			r114_ctpopt::state().appid = p->AppID;
			r114_ctpopt::state().authcode = p->AuthCode;
		}
		return 0;
	}
	int RegisterUserSystemInfo(CThostFtdcUserSystemInfoField *pUserSystemInfo) override { return 0; }
	int SubmitUserSystemInfo(CThostFtdcUserSystemInfoField *pUserSystemInfo) override { return 0; }
	int ReqUserLogin(CThostFtdcReqUserLoginField* p, int nRequestID) override
	{
		++r114_ctpopt::state().login_reqs;
		r114_ctpopt::state().last_request_id = nRequestID;
		if (p)
		{
			r114_ctpopt::state().broker = p->BrokerID;
			r114_ctpopt::state().user = p->UserID;
			r114_ctpopt::state().password = p->Password;
		}
		return 0;
	}
	int ReqUserLogout(CThostFtdcUserLogoutField *pUserLogout, int nRequestID) override { return 0; }
	int ReqUserPasswordUpdate(CThostFtdcUserPasswordUpdateField *pUserPasswordUpdate, int nRequestID) override { return 0; }
	int ReqTradingAccountPasswordUpdate(CThostFtdcTradingAccountPasswordUpdateField *pTradingAccountPasswordUpdate, int nRequestID) override { return 0; }
	int ReqOrderInsert(CThostFtdcInputOrderField *pInputOrder, int nRequestID) override { return 0; }
	int ReqParkedOrderInsert(CThostFtdcParkedOrderField *pParkedOrder, int nRequestID) override { return 0; }
	int ReqParkedOrderAction(CThostFtdcParkedOrderActionField *pParkedOrderAction, int nRequestID) override { return 0; }
	int ReqOrderAction(CThostFtdcInputOrderActionField *pInputOrderAction, int nRequestID) override { return 0; }
	int ReqQueryMaxOrderVolume(CThostFtdcQueryMaxOrderVolumeField *pQueryMaxOrderVolume, int nRequestID) override { return 0; }
	int ReqSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField *pSettlementInfoConfirm, int nRequestID) override { return 0; }
	int ReqRemoveParkedOrder(CThostFtdcRemoveParkedOrderField *pRemoveParkedOrder, int nRequestID) override { return 0; }
	int ReqRemoveParkedOrderAction(CThostFtdcRemoveParkedOrderActionField *pRemoveParkedOrderAction, int nRequestID) override { return 0; }
	int ReqExecOrderInsert(CThostFtdcInputExecOrderField *pInputExecOrder, int nRequestID) override { return 0; }
	int ReqExecOrderAction(CThostFtdcInputExecOrderActionField *pInputExecOrderAction, int nRequestID) override { return 0; }
	int ReqForQuoteInsert(CThostFtdcInputForQuoteField *pInputForQuote, int nRequestID) override { return 0; }
	int ReqQuoteInsert(CThostFtdcInputQuoteField *pInputQuote, int nRequestID) override { return 0; }
	int ReqQuoteAction(CThostFtdcInputQuoteActionField *pInputQuoteAction, int nRequestID) override { return 0; }
	int ReqLockInsert(CThostFtdcInputLockField *pInputLock, int nRequestID) override { return 0; }
	int ReqBatchOrderAction(CThostFtdcInputBatchOrderActionField *pInputBatchOrderAction, int nRequestID) override { return 0; }
	int ReqCombActionInsert(CThostFtdcInputCombActionField *pInputCombAction, int nRequestID) override { return 0; }
	int ReqQryOrder(CThostFtdcQryOrderField *pQryOrder, int nRequestID) override { return 0; }
	int ReqQryTrade(CThostFtdcQryTradeField *pQryTrade, int nRequestID) override { return 0; }
	int ReqQryInvestorPosition(CThostFtdcQryInvestorPositionField *pQryInvestorPosition, int nRequestID) override { return 0; }
	int ReqQryTradingAccount(CThostFtdcQryTradingAccountField *pQryTradingAccount, int nRequestID) override { return 0; }
	int ReqQryInvestor(CThostFtdcQryInvestorField *pQryInvestor, int nRequestID) override { return 0; }
	int ReqQryTradingCode(CThostFtdcQryTradingCodeField *pQryTradingCode, int nRequestID) override { return 0; }
	int ReqQryInstrumentMarginRate(CThostFtdcQryInstrumentMarginRateField *pQryInstrumentMarginRate, int nRequestID) override { return 0; }
	int ReqQryInstrumentCommissionRate(CThostFtdcQryInstrumentCommissionRateField *pQryInstrumentCommissionRate, int nRequestID) override { return 0; }
	int ReqQryExchange(CThostFtdcQryExchangeField *pQryExchange, int nRequestID) override { return 0; }
	int ReqQryProduct(CThostFtdcQryProductField *pQryProduct, int nRequestID) override { return 0; }
	int ReqQryInstrument(CThostFtdcQryInstrumentField* /*p*/, int nRequestID) override
	{
		++r114_ctpopt::state().instrument_reqs;
		r114_ctpopt::state().last_request_id = nRequestID;
		return 0;
	}
	int ReqQryDepthMarketData(CThostFtdcQryDepthMarketDataField *pQryDepthMarketData, int nRequestID) override { return 0; }
	int ReqQrySettlementInfo(CThostFtdcQrySettlementInfoField *pQrySettlementInfo, int nRequestID) override { return 0; }
	int ReqQryTransferBank(CThostFtdcQryTransferBankField *pQryTransferBank, int nRequestID) override { return 0; }
	int ReqQryInvestorPositionDetail(CThostFtdcQryInvestorPositionDetailField *pQryInvestorPositionDetail, int nRequestID) override { return 0; }
	int ReqQryNotice(CThostFtdcQryNoticeField *pQryNotice, int nRequestID) override { return 0; }
	int ReqQrySettlementInfoConfirm(CThostFtdcQrySettlementInfoConfirmField *pQrySettlementInfoConfirm, int nRequestID) override { return 0; }
	int ReqQryInvestorPositionCombineDetail(CThostFtdcQryInvestorPositionCombineDetailField *pQryInvestorPositionCombineDetail, int nRequestID) override { return 0; }
	int ReqQryCFMMCTradingAccountKey(CThostFtdcQryCFMMCTradingAccountKeyField *pQryCFMMCTradingAccountKey, int nRequestID) override { return 0; }
	int ReqQryEWarrantOffset(CThostFtdcQryEWarrantOffsetField *pQryEWarrantOffset, int nRequestID) override { return 0; }
	int ReqQryInvestorProductGroupMargin(CThostFtdcQryInvestorProductGroupMarginField *pQryInvestorProductGroupMargin, int nRequestID) override { return 0; }
	int ReqQryExchangeMarginRate(CThostFtdcQryExchangeMarginRateField *pQryExchangeMarginRate, int nRequestID) override { return 0; }
	int ReqQryExchangeMarginRateAdjust(CThostFtdcQryExchangeMarginRateAdjustField *pQryExchangeMarginRateAdjust, int nRequestID) override { return 0; }
	int ReqQryExchangeRate(CThostFtdcQryExchangeRateField *pQryExchangeRate, int nRequestID) override { return 0; }
	int ReqQrySecAgentACIDMap(CThostFtdcQrySecAgentACIDMapField *pQrySecAgentACIDMap, int nRequestID) override { return 0; }
	int ReqQryProductExchRate(CThostFtdcQryProductExchRateField *pQryProductExchRate, int nRequestID) override { return 0; }
	int ReqQryProductGroup(CThostFtdcQryProductGroupField *pQryProductGroup, int nRequestID) override { return 0; }
	int ReqQryMMInstrumentCommissionRate(CThostFtdcQryMMInstrumentCommissionRateField *pQryMMInstrumentCommissionRate, int nRequestID) override { return 0; }
	int ReqQryMMOptionInstrCommRate(CThostFtdcQryMMOptionInstrCommRateField *pQryMMOptionInstrCommRate, int nRequestID) override { return 0; }
	int ReqQryInstrumentOrderCommRate(CThostFtdcQryInstrumentOrderCommRateField *pQryInstrumentOrderCommRate, int nRequestID) override { return 0; }
	int ReqQryOptionInstrTradeCost(CThostFtdcQryOptionInstrTradeCostField *pQryOptionInstrTradeCost, int nRequestID) override { return 0; }
	int ReqQryOptionInstrCommRate(CThostFtdcQryOptionInstrCommRateField *pQryOptionInstrCommRate, int nRequestID) override { return 0; }
	int ReqQryExecOrder(CThostFtdcQryExecOrderField *pQryExecOrder, int nRequestID) override { return 0; }
	int ReqQryForQuote(CThostFtdcQryForQuoteField *pQryForQuote, int nRequestID) override { return 0; }
	int ReqQryQuote(CThostFtdcQryQuoteField *pQryQuote, int nRequestID) override { return 0; }
	int ReqQryLock(CThostFtdcQryLockField *pQryLock, int nRequestID) override { return 0; }
	int ReqQryLockPosition(CThostFtdcQryLockPositionField *pQryLockPosition, int nRequestID) override { return 0; }
	int ReqQryETFOptionInstrCommRate(CThostFtdcQryETFOptionInstrCommRateField *pQryETFOptionInstrCommRate, int nRequestID) override { return 0; }
	int ReqQryInvestorLevel(CThostFtdcQryInvestorLevelField *pQryInvestorLevel, int nRequestID) override { return 0; }
	int ReqQryExecFreeze(CThostFtdcQryExecFreezeField *pQryExecFreeze, int nRequestID) override { return 0; }
	int ReqQryCombInstrumentGuard(CThostFtdcQryCombInstrumentGuardField *pQryCombInstrumentGuard, int nRequestID) override { return 0; }
	int ReqQryCombAction(CThostFtdcQryCombActionField *pQryCombAction, int nRequestID) override { return 0; }
	int ReqQryTransferSerial(CThostFtdcQryTransferSerialField *pQryTransferSerial, int nRequestID) override { return 0; }
	int ReqQryAccountregister(CThostFtdcQryAccountregisterField *pQryAccountregister, int nRequestID) override { return 0; }
	int ReqQryContractBank(CThostFtdcQryContractBankField *pQryContractBank, int nRequestID) override { return 0; }
	int ReqQryParkedOrder(CThostFtdcQryParkedOrderField *pQryParkedOrder, int nRequestID) override { return 0; }
	int ReqQryParkedOrderAction(CThostFtdcQryParkedOrderActionField *pQryParkedOrderAction, int nRequestID) override { return 0; }
	int ReqQryTradingNotice(CThostFtdcQryTradingNoticeField *pQryTradingNotice, int nRequestID) override { return 0; }
	int ReqQryBrokerTradingParams(CThostFtdcQryBrokerTradingParamsField *pQryBrokerTradingParams, int nRequestID) override { return 0; }
	int ReqQryBrokerTradingAlgos(CThostFtdcQryBrokerTradingAlgosField *pQryBrokerTradingAlgos, int nRequestID) override { return 0; }
	int ReqQueryCFMMCTradingAccountToken(CThostFtdcQueryCFMMCTradingAccountTokenField *pQueryCFMMCTradingAccountToken, int nRequestID) override { return 0; }
	int ReqFromBankToFutureByFuture(CThostFtdcReqTransferField *pReqTransfer, int nRequestID) override { return 0; }
	int ReqFromFutureToBankByFuture(CThostFtdcReqTransferField *pReqTransfer, int nRequestID) override { return 0; }
	int ReqQueryBankAccountMoneyByFuture(CThostFtdcReqQueryAccountField *pReqQueryAccount, int nRequestID) override { return 0; }
	int ReqReserveOpenAccountTpdByFuture(CThostFtdcReqReserveOpenAccountTpdField *pReqReserveOpenAccountTpd, int nRequestID) override { return 0; }
	int ReqSecuritiesDepositInterestByFuture(CThostFtdcReqSecuritiesDepositInterestField *pReqSecuritiesDepositInterest, int nRequestID) override { return 0; }
	int ReqDayEndFileReadyByFuture(CThostFtdcReqDayEndFileReadyField *pReqDayEndFileReady, int nRequestID) override { return 0; }
	int ReqExecCombineOrderInsert(CThostFtdcInputExecCombineOrderField *pInputExecCombineOrder, int nRequestID) override { return 0; }
	int ReqExecCombineOrderAction(CThostFtdcInputExecCombineOrderActionField *pInputExecCombineOrderAction, int nRequestID) override { return 0; }
	int ReqQryExecCombineOrder(CThostFtdcQryExecCombineOrderField *pQryExecCombineOrder, int nRequestID) override { return 0; }
};

#pragma clang diagnostic pop
