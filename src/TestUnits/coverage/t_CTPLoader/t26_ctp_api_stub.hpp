/*
 * t26_ctp_api_stub.hpp — CThostFtdcTraderApi 的可捕获替身
 *
 * 107 个纯虚逐条对过 src/API/CTP6.6.9/ThostFtdcTraderApi.h（与
 * t_TraderCTP/t2_ctp_api_stub.hpp 同源生成，形状一致），全部不起 socket/线程。
 * 与登录/查询流相关的 5 个入口（ReqAuthenticate / ReqUserLogin / ReqQryInstrument /
 * ReqQryInstrumentCommissionRate / ReqQryInstrumentMarginRate）会把请求结构体**拷贝**
 * 进成员，供用例断言字段换算与请求编号推进；GetTradingDay 返回可控字符串。
 *
 * 用法：`t26::T26CtpApiStub stub; pUserApi = &stub;`，用毕把 pUserApi 置回 NULL。
 * 注意 CTraderSpi::AppendQuery 的 worker 线程会持有 pUserApi，凡触发过队列的用例
 * 一律「不析构 spi、stub 用静态/堆上长命对象」——析构会挂死（见 bugs/T26 的 BUG-T26-001）。
 */
#pragma once

#include <cstring>
#include <string>
#include <vector>

#include "CTPLoader/TraderSpi.h"

class T26CtpApiStub : public CThostFtdcTraderApi
{
public:
	unsigned int	n_calls_;
	int				stub_ret_;
	std::string		trading_day_;

	// 被捕获的请求
	unsigned int							n_auth_;
	unsigned int							n_login_;
	unsigned int							n_qry_inst_;
	std::vector<CThostFtdcQryInstrumentCommissionRateField>	comm_reqs_;
	std::vector<CThostFtdcQryInstrumentMarginRateField>		margin_reqs_;
	std::vector<int>					auth_reqids_;
	std::vector<int>					login_reqids_;
	std::vector<int>					qry_inst_reqids_;
	CThostFtdcReqAuthenticateField		last_auth_;
	CThostFtdcReqUserLoginField			last_login_;

	T26CtpApiStub()
		: n_calls_(0), stub_ret_(0), trading_day_("20251027"),
		n_auth_(0), n_login_(0), n_qry_inst_(0)
	{
		std::memset(&last_auth_, 0, sizeof(last_auth_));
		std::memset(&last_login_, 0, sizeof(last_login_));
	}
	virtual ~T26CtpApiStub() {}

	virtual void Release() override { ++n_calls_; }
	virtual void Init() override { ++n_calls_; }
	virtual int Join() override { ++n_calls_; return stub_ret_; }
	virtual const char* GetTradingDay() override { return trading_day_.c_str(); }
	virtual void RegisterFront(char*) override { ++n_calls_; }
	virtual void RegisterNameServer(char*) override { ++n_calls_; }
	virtual void RegisterFensUserInfo(CThostFtdcFensUserInfoField*) override { ++n_calls_; }
	virtual void RegisterSpi(CThostFtdcTraderSpi*) override { ++n_calls_; }
	virtual void SubscribePrivateTopic(THOST_TE_RESUME_TYPE) override { ++n_calls_; }
	virtual void SubscribePublicTopic(THOST_TE_RESUME_TYPE) override { ++n_calls_; }

	virtual int ReqAuthenticate(CThostFtdcReqAuthenticateField* pField, int nRequestID) override
	{
		std::memcpy(&last_auth_, pField, sizeof(last_auth_));
		auth_reqids_.push_back(nRequestID);
		++n_auth_;
		++n_calls_;
		return stub_ret_;
	}

	virtual int ReqUserLogin(CThostFtdcReqUserLoginField* pField, int nRequestID) override
	{
		std::memcpy(&last_login_, pField, sizeof(last_login_));
		login_reqids_.push_back(nRequestID);
		++n_login_;
		++n_calls_;
		return stub_ret_;
	}

	virtual int ReqQryInstrument(CThostFtdcQryInstrumentField*, int nRequestID) override
	{
		++n_qry_inst_;
		qry_inst_reqids_.push_back(nRequestID);
		++n_calls_;
		return stub_ret_;
	}

	virtual int ReqQryInstrumentCommissionRate(CThostFtdcQryInstrumentCommissionRateField* pField, int) override
	{
		comm_reqs_.push_back(*pField);
		++n_calls_;
		return stub_ret_;
	}

	virtual int ReqQryInstrumentMarginRate(CThostFtdcQryInstrumentMarginRateField* pField, int) override
	{
		margin_reqs_.push_back(*pField);
		++n_calls_;
		return stub_ret_;
	}

	virtual int RegisterUserSystemInfo(CThostFtdcUserSystemInfoField*) override { ++n_calls_; return stub_ret_; }
	virtual int SubmitUserSystemInfo(CThostFtdcUserSystemInfoField*) override { ++n_calls_; return stub_ret_; }
	virtual int ReqUserLogout(CThostFtdcUserLogoutField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqUserPasswordUpdate(CThostFtdcUserPasswordUpdateField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqTradingAccountPasswordUpdate(CThostFtdcTradingAccountPasswordUpdateField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqUserAuthMethod(CThostFtdcReqUserAuthMethodField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqGenUserCaptcha(CThostFtdcReqGenUserCaptchaField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqGenUserText(CThostFtdcReqGenUserTextField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqUserLoginWithCaptcha(CThostFtdcReqUserLoginWithCaptchaField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqUserLoginWithText(CThostFtdcReqUserLoginWithTextField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqUserLoginWithOTP(CThostFtdcReqUserLoginWithOTPField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqOrderInsert(CThostFtdcInputOrderField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqParkedOrderInsert(CThostFtdcParkedOrderField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqParkedOrderAction(CThostFtdcParkedOrderActionField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqOrderAction(CThostFtdcInputOrderActionField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryMaxOrderVolume(CThostFtdcQryMaxOrderVolumeField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqRemoveParkedOrder(CThostFtdcRemoveParkedOrderField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqRemoveParkedOrderAction(CThostFtdcRemoveParkedOrderActionField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqExecOrderInsert(CThostFtdcInputExecOrderField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqExecOrderAction(CThostFtdcInputExecOrderActionField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqForQuoteInsert(CThostFtdcInputForQuoteField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQuoteInsert(CThostFtdcInputQuoteField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQuoteAction(CThostFtdcInputQuoteActionField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqBatchOrderAction(CThostFtdcInputBatchOrderActionField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqOptionSelfCloseInsert(CThostFtdcInputOptionSelfCloseField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqOptionSelfCloseAction(CThostFtdcInputOptionSelfCloseActionField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqCombActionInsert(CThostFtdcInputCombActionField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryOrder(CThostFtdcQryOrderField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryTrade(CThostFtdcQryTradeField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryInvestorPosition(CThostFtdcQryInvestorPositionField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryTradingAccount(CThostFtdcQryTradingAccountField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryInvestor(CThostFtdcQryInvestorField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryTradingCode(CThostFtdcQryTradingCodeField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryExchange(CThostFtdcQryExchangeField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryProduct(CThostFtdcQryProductField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryDepthMarketData(CThostFtdcQryDepthMarketDataField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryTraderOffer(CThostFtdcQryTraderOfferField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQrySettlementInfo(CThostFtdcQrySettlementInfoField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryTransferBank(CThostFtdcQryTransferBankField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryInvestorPositionDetail(CThostFtdcQryInvestorPositionDetailField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryNotice(CThostFtdcQryNoticeField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQrySettlementInfoConfirm(CThostFtdcQrySettlementInfoConfirmField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryInvestorPositionCombineDetail(CThostFtdcQryInvestorPositionCombineDetailField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryCFMMCTradingAccountKey(CThostFtdcQryCFMMCTradingAccountKeyField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryEWarrantOffset(CThostFtdcQryEWarrantOffsetField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryInvestorProductGroupMargin(CThostFtdcQryInvestorProductGroupMarginField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryExchangeMarginRate(CThostFtdcQryExchangeMarginRateField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryExchangeMarginRateAdjust(CThostFtdcQryExchangeMarginRateAdjustField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryExchangeRate(CThostFtdcQryExchangeRateField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQrySecAgentACIDMap(CThostFtdcQrySecAgentACIDMapField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryProductExchRate(CThostFtdcQryProductExchRateField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryProductGroup(CThostFtdcQryProductGroupField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryMMInstrumentCommissionRate(CThostFtdcQryMMInstrumentCommissionRateField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryMMOptionInstrCommRate(CThostFtdcQryMMOptionInstrCommRateField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryInstrumentOrderCommRate(CThostFtdcQryInstrumentOrderCommRateField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQrySecAgentTradingAccount(CThostFtdcQryTradingAccountField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQrySecAgentCheckMode(CThostFtdcQrySecAgentCheckModeField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQrySecAgentTradeInfo(CThostFtdcQrySecAgentTradeInfoField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryOptionInstrTradeCost(CThostFtdcQryOptionInstrTradeCostField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryOptionInstrCommRate(CThostFtdcQryOptionInstrCommRateField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryExecOrder(CThostFtdcQryExecOrderField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryForQuote(CThostFtdcQryForQuoteField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryQuote(CThostFtdcQryQuoteField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryOptionSelfClose(CThostFtdcQryOptionSelfCloseField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryInvestUnit(CThostFtdcQryInvestUnitField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryCombInstrumentGuard(CThostFtdcQryCombInstrumentGuardField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryCombAction(CThostFtdcQryCombActionField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryTransferSerial(CThostFtdcQryTransferSerialField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryAccountregister(CThostFtdcQryAccountregisterField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryContractBank(CThostFtdcQryContractBankField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryParkedOrder(CThostFtdcQryParkedOrderField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryParkedOrderAction(CThostFtdcQryParkedOrderActionField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryTradingNotice(CThostFtdcQryTradingNoticeField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryBrokerTradingParams(CThostFtdcQryBrokerTradingParamsField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryBrokerTradingAlgos(CThostFtdcQryBrokerTradingAlgosField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQueryCFMMCTradingAccountToken(CThostFtdcQueryCFMMCTradingAccountTokenField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqFromBankToFutureByFuture(CThostFtdcReqTransferField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqFromFutureToBankByFuture(CThostFtdcReqTransferField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQueryBankAccountMoneyByFuture(CThostFtdcReqQueryAccountField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryClassifiedInstrument(CThostFtdcQryClassifiedInstrumentField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryCombPromotionParam(CThostFtdcQryCombPromotionParamField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryRiskSettleInvstPosition(CThostFtdcQryRiskSettleInvstPositionField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryRiskSettleProductStatus(CThostFtdcQryRiskSettleProductStatusField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQrySPBMFutureParameter(CThostFtdcQrySPBMFutureParameterField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQrySPBMOptionParameter(CThostFtdcQrySPBMOptionParameterField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQrySPBMIntraParameter(CThostFtdcQrySPBMIntraParameterField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQrySPBMInterParameter(CThostFtdcQrySPBMInterParameterField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQrySPBMPortfDefinition(CThostFtdcQrySPBMPortfDefinitionField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQrySPBMInvestorPortfDef(CThostFtdcQrySPBMInvestorPortfDefField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryInvestorPortfMarginRatio(CThostFtdcQryInvestorPortfMarginRatioField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryInvestorProdSPBMDetail(CThostFtdcQryInvestorProdSPBMDetailField*, int) override { ++n_calls_; return stub_ret_; }
};
