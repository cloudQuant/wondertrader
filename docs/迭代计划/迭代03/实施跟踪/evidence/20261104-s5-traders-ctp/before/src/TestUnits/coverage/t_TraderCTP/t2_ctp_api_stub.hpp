/*
 * t2_ctp_api_stub.hpp — CThostFtdcTraderApi 的机械空实现
 *
 * 107 个纯虚函数逐条来自 src/API/CTP6.6.9/ThostFtdcTraderApi.h（awk/perl 抽取签名后生成），
 * 全部只记次数并返回可控的 stub_ret_，不触碰任何 socket/线程。
 * 用例用它替身 TraderCTP::m_pUserAPI（protected 成员，经派生 seam 注入），
 * 因此 login/orderInsert/orderAction/OnRspUserLogin 等「必须有 API 指针」的分支可以真实走到。
 */
#pragma once

#include <cstring>

#include "TraderCTP/TraderCTP.h"

class CtpApiStub : public CThostFtdcTraderApi
{
public:
	unsigned int	n_calls_;
	int				stub_ret_;

	CtpApiStub() : n_calls_(0), stub_ret_(0) {}
	virtual ~CtpApiStub() {}

	virtual void Release() override { ++n_calls_; }
	virtual void Init() override { ++n_calls_; }
	virtual int Join() override { ++n_calls_; return stub_ret_; }
	virtual const char * GetTradingDay() override { return NULL; }
	virtual void RegisterFront(char *) override { ++n_calls_; }
	virtual void RegisterNameServer(char *) override { ++n_calls_; }
	virtual void RegisterFensUserInfo(CThostFtdcFensUserInfoField *) override { ++n_calls_; }
	virtual void RegisterSpi(CThostFtdcTraderSpi *) override { ++n_calls_; }
	virtual void SubscribePrivateTopic(THOST_TE_RESUME_TYPE) override { ++n_calls_; }
	virtual void SubscribePublicTopic(THOST_TE_RESUME_TYPE) override { ++n_calls_; }
	virtual int ReqAuthenticate(CThostFtdcReqAuthenticateField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int RegisterUserSystemInfo(CThostFtdcUserSystemInfoField *) override { ++n_calls_; return stub_ret_; }
	virtual int SubmitUserSystemInfo(CThostFtdcUserSystemInfoField *) override { ++n_calls_; return stub_ret_; }
	virtual int ReqUserLogin(CThostFtdcReqUserLoginField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqUserLogout(CThostFtdcUserLogoutField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqUserPasswordUpdate(CThostFtdcUserPasswordUpdateField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqTradingAccountPasswordUpdate(CThostFtdcTradingAccountPasswordUpdateField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqUserAuthMethod(CThostFtdcReqUserAuthMethodField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqGenUserCaptcha(CThostFtdcReqGenUserCaptchaField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqGenUserText(CThostFtdcReqGenUserTextField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqUserLoginWithCaptcha(CThostFtdcReqUserLoginWithCaptchaField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqUserLoginWithText(CThostFtdcReqUserLoginWithTextField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqUserLoginWithOTP(CThostFtdcReqUserLoginWithOTPField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqOrderInsert(CThostFtdcInputOrderField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqParkedOrderInsert(CThostFtdcParkedOrderField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqParkedOrderAction(CThostFtdcParkedOrderActionField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqOrderAction(CThostFtdcInputOrderActionField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryMaxOrderVolume(CThostFtdcQryMaxOrderVolumeField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqRemoveParkedOrder(CThostFtdcRemoveParkedOrderField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqRemoveParkedOrderAction(CThostFtdcRemoveParkedOrderActionField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqExecOrderInsert(CThostFtdcInputExecOrderField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqExecOrderAction(CThostFtdcInputExecOrderActionField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqForQuoteInsert(CThostFtdcInputForQuoteField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQuoteInsert(CThostFtdcInputQuoteField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQuoteAction(CThostFtdcInputQuoteActionField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqBatchOrderAction(CThostFtdcInputBatchOrderActionField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqOptionSelfCloseInsert(CThostFtdcInputOptionSelfCloseField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqOptionSelfCloseAction(CThostFtdcInputOptionSelfCloseActionField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqCombActionInsert(CThostFtdcInputCombActionField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryOrder(CThostFtdcQryOrderField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryTrade(CThostFtdcQryTradeField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryInvestorPosition(CThostFtdcQryInvestorPositionField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryTradingAccount(CThostFtdcQryTradingAccountField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryInvestor(CThostFtdcQryInvestorField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryTradingCode(CThostFtdcQryTradingCodeField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryInstrumentMarginRate(CThostFtdcQryInstrumentMarginRateField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryInstrumentCommissionRate(CThostFtdcQryInstrumentCommissionRateField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryExchange(CThostFtdcQryExchangeField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryProduct(CThostFtdcQryProductField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryInstrument(CThostFtdcQryInstrumentField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryDepthMarketData(CThostFtdcQryDepthMarketDataField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryTraderOffer(CThostFtdcQryTraderOfferField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQrySettlementInfo(CThostFtdcQrySettlementInfoField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryTransferBank(CThostFtdcQryTransferBankField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryInvestorPositionDetail(CThostFtdcQryInvestorPositionDetailField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryNotice(CThostFtdcQryNoticeField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQrySettlementInfoConfirm(CThostFtdcQrySettlementInfoConfirmField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryInvestorPositionCombineDetail(CThostFtdcQryInvestorPositionCombineDetailField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryCFMMCTradingAccountKey(CThostFtdcQryCFMMCTradingAccountKeyField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryEWarrantOffset(CThostFtdcQryEWarrantOffsetField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryInvestorProductGroupMargin(CThostFtdcQryInvestorProductGroupMarginField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryExchangeMarginRate(CThostFtdcQryExchangeMarginRateField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryExchangeMarginRateAdjust(CThostFtdcQryExchangeMarginRateAdjustField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryExchangeRate(CThostFtdcQryExchangeRateField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQrySecAgentACIDMap(CThostFtdcQrySecAgentACIDMapField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryProductExchRate(CThostFtdcQryProductExchRateField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryProductGroup(CThostFtdcQryProductGroupField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryMMInstrumentCommissionRate(CThostFtdcQryMMInstrumentCommissionRateField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryMMOptionInstrCommRate(CThostFtdcQryMMOptionInstrCommRateField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryInstrumentOrderCommRate(CThostFtdcQryInstrumentOrderCommRateField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQrySecAgentTradingAccount(CThostFtdcQryTradingAccountField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQrySecAgentCheckMode(CThostFtdcQrySecAgentCheckModeField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQrySecAgentTradeInfo(CThostFtdcQrySecAgentTradeInfoField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryOptionInstrTradeCost(CThostFtdcQryOptionInstrTradeCostField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryOptionInstrCommRate(CThostFtdcQryOptionInstrCommRateField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryExecOrder(CThostFtdcQryExecOrderField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryForQuote(CThostFtdcQryForQuoteField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryQuote(CThostFtdcQryQuoteField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryOptionSelfClose(CThostFtdcQryOptionSelfCloseField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryInvestUnit(CThostFtdcQryInvestUnitField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryCombInstrumentGuard(CThostFtdcQryCombInstrumentGuardField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryCombAction(CThostFtdcQryCombActionField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryTransferSerial(CThostFtdcQryTransferSerialField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryAccountregister(CThostFtdcQryAccountregisterField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryContractBank(CThostFtdcQryContractBankField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryParkedOrder(CThostFtdcQryParkedOrderField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryParkedOrderAction(CThostFtdcQryParkedOrderActionField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryTradingNotice(CThostFtdcQryTradingNoticeField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryBrokerTradingParams(CThostFtdcQryBrokerTradingParamsField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryBrokerTradingAlgos(CThostFtdcQryBrokerTradingAlgosField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQueryCFMMCTradingAccountToken(CThostFtdcQueryCFMMCTradingAccountTokenField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqFromBankToFutureByFuture(CThostFtdcReqTransferField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqFromFutureToBankByFuture(CThostFtdcReqTransferField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQueryBankAccountMoneyByFuture(CThostFtdcReqQueryAccountField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryClassifiedInstrument(CThostFtdcQryClassifiedInstrumentField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryCombPromotionParam(CThostFtdcQryCombPromotionParamField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryRiskSettleInvstPosition(CThostFtdcQryRiskSettleInvstPositionField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryRiskSettleProductStatus(CThostFtdcQryRiskSettleProductStatusField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQrySPBMFutureParameter(CThostFtdcQrySPBMFutureParameterField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQrySPBMOptionParameter(CThostFtdcQrySPBMOptionParameterField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQrySPBMIntraParameter(CThostFtdcQrySPBMIntraParameterField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQrySPBMInterParameter(CThostFtdcQrySPBMInterParameterField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQrySPBMPortfDefinition(CThostFtdcQrySPBMPortfDefinitionField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQrySPBMInvestorPortfDef(CThostFtdcQrySPBMInvestorPortfDefField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryInvestorPortfMarginRatio(CThostFtdcQryInvestorPortfMarginRatioField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryInvestorProdSPBMDetail(CThostFtdcQryInvestorProdSPBMDetailField *, int) override { ++n_calls_; return stub_ret_; }
};
