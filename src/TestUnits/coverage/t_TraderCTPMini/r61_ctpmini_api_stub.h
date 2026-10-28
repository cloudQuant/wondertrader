/*
 * r61_ctpmini_api_stub.h — CThostFtdcTraderApi（CTPMini1.5.8）的机械空实现
 *
 * 49 个纯虚函数逐条来自 src/API/CTPMini1.5.8/ThostFtdcTraderApi.h，
 * 只记调用次数并返回可控的 stub_ret_，不碰 socket、不加载厂商库、不起线程。
 * 用例通过 TraderCTPMini 的派生探针把 m_pUserAPI 指向它，
 * 从而真实进入 login/orderInsert/query* 的正向分支（lane R61）。
 */
#pragma once

#include "TraderCTPMini/TraderCTPMini.h"

class R61CtpMiniApiStub : public CThostFtdcTraderApi
{
public:
	unsigned int	n_calls_ = 0;
	int				stub_ret_ = 0;
	const char*		trading_day_ = "20260105";

	R61CtpMiniApiStub() {}
	virtual ~R61CtpMiniApiStub() {}

	virtual void Release() override { ++n_calls_; }
	virtual void Init(bool) override { ++n_calls_; }
	virtual int Join() override { ++n_calls_; return stub_ret_; }
	virtual const char *GetTradingDay() override { ++n_calls_; return trading_day_; }
	virtual void RegisterFront(char *) override { ++n_calls_; }
	virtual void RegisterSpi(CThostFtdcTraderSpi *) override { ++n_calls_; }
	virtual void SubscribePrivateTopic(THOST_TE_RESUME_TYPE) override { ++n_calls_; }
	virtual void SubscribePublicTopic(THOST_TE_RESUME_TYPE) override { ++n_calls_; }
	virtual int ReqAuthenticate(CThostFtdcReqAuthenticateField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqUserLogin(CThostFtdcReqUserLoginField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqUserLoginEncrypt(CThostFtdcReqUserLoginField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqUserLogout(CThostFtdcUserLogoutField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqOrderInsert(CThostFtdcInputOrderField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqOrderAction(CThostFtdcInputOrderActionField *, int) override { ++n_calls_; return stub_ret_; }
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
	virtual int ReqQryCombInstrument(CThostFtdcQryCombInstrumentField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryInvestorPositionForComb(CThostFtdcQryInvestorPositionForCombField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryCombAction(CThostFtdcQryCombActionField*, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryDepthMarketData(CThostFtdcQryDepthMarketDataField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryOptionSelfClose(CThostFtdcQryOptionSelfCloseField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryInstrumentStatus(CThostFtdcQryInstrumentStatusField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryInvestorPositionDetail(CThostFtdcQryInvestorPositionDetailField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryInvestorPositionCombineDetail(CThostFtdcQryInvestorPositionCombineDetailField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryExchangeMarginRate(CThostFtdcQryExchangeMarginRateField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryExchangeMarginRateAdjust(CThostFtdcQryExchangeMarginRateAdjustField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryOptionInstrTradeCost(CThostFtdcQryOptionInstrTradeCostField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryOptionInstrCommRate(CThostFtdcQryOptionInstrCommRateField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryExecOrder(CThostFtdcQryExecOrderField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryForQuote(CThostFtdcQryForQuoteField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryQuote(CThostFtdcQryQuoteField *, int) override { ++n_calls_; return stub_ret_; }
	virtual int ReqQryInstrumentOrderCommRate(CThostFtdcQryInstrumentOrderCommRateField *, int) override { ++n_calls_; return stub_ret_; }
};
