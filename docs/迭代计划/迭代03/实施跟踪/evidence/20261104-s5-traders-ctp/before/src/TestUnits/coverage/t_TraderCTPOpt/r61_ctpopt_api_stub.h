/*
 * r61_ctpopt_api_stub.h — CThostFtdcTraderApi（CTPOpt3.5.8）的机械空实现
 *
 * 93 个纯虚函数逐条来自 src/API/CTPOpt3.5.8/ThostFtdcTraderApi.h，
 * 只记调用次数并返回可控的 stub_ret_，不碰 socket、不加载厂商库、不起线程。
 * 用例通过 TraderCTPOpt 的派生探针把 m_pUserAPI 指向它，
 * 从而真实进入 login/orderInsert/query* 的正向分支（lane R61）。
 */
#pragma once

#include "TraderCTPOpt/TraderCTPOpt.h"

class R61CtpOptApiStub : public ctp_sopt::CThostFtdcTraderApi
{
public:
	unsigned int	n_calls_ = 0;
	int				stub_ret_ = 0;
	const char*		trading_day_ = "20260105";

	R61CtpOptApiStub() {}
	virtual ~R61CtpOptApiStub() {}

	virtual void Release() override { ++n_calls_; }
	virtual void Init() override { ++n_calls_; }
	virtual int Join() override { ++n_calls_; return stub_ret_; }
	virtual const char *GetTradingDay() override { ++n_calls_; return trading_day_; }
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
	virtual int ReqOrderInsert(CThostFtdcInputOrderField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqParkedOrderInsert(CThostFtdcParkedOrderField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqParkedOrderAction(CThostFtdcParkedOrderActionField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqOrderAction(CThostFtdcInputOrderActionField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQueryMaxOrderVolume(CThostFtdcQueryMaxOrderVolumeField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqRemoveParkedOrder(CThostFtdcRemoveParkedOrderField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqRemoveParkedOrderAction(CThostFtdcRemoveParkedOrderActionField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqExecOrderInsert(CThostFtdcInputExecOrderField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqExecOrderAction(CThostFtdcInputExecOrderActionField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqForQuoteInsert(CThostFtdcInputForQuoteField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQuoteInsert(CThostFtdcInputQuoteField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQuoteAction(CThostFtdcInputQuoteActionField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqLockInsert(CThostFtdcInputLockField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqBatchOrderAction(CThostFtdcInputBatchOrderActionField *, int) override { ++n_calls_; return stub_ret_; }
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
	virtual int ReqQryOptionInstrTradeCost(CThostFtdcQryOptionInstrTradeCostField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryOptionInstrCommRate(CThostFtdcQryOptionInstrCommRateField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryExecOrder(CThostFtdcQryExecOrderField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryForQuote(CThostFtdcQryForQuoteField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryQuote(CThostFtdcQryQuoteField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryLock(CThostFtdcQryLockField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryLockPosition(CThostFtdcQryLockPositionField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryETFOptionInstrCommRate(CThostFtdcQryETFOptionInstrCommRateField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryInvestorLevel(CThostFtdcQryInvestorLevelField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryExecFreeze(CThostFtdcQryExecFreezeField *, int) override { ++n_calls_; return stub_ret_; }
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
	virtual int ReqReserveOpenAccountTpdByFuture(CThostFtdcReqReserveOpenAccountTpdField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqSecuritiesDepositInterestByFuture(CThostFtdcReqSecuritiesDepositInterestField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqDayEndFileReadyByFuture(CThostFtdcReqDayEndFileReadyField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqExecCombineOrderInsert(CThostFtdcInputExecCombineOrderField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqExecCombineOrderAction(CThostFtdcInputExecCombineOrderActionField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryExecCombineOrder(CThostFtdcQryExecCombineOrderField *, int) override { ++n_calls_; return stub_ret_; }
};
