/*
 * r93_femas_api_stub.h — CUstpFtdcTraderApi（Femas3.02）的机械空实现
 *
 * 纯虚函数逐条取自 src/API/Femas3.02/USTPFtdcTraderApi.h，
 * 只记录调用与最近一次请求结构，不碰 socket、不加载厂商库、不起线程。
 * 用例把 TraderFemas 探针的 m_pUserAPI 指向本类，从而进入 connect/login/
 * orderInsert/orderAction/logout/query* 的正向组装分支（lane R93）。
 */
#pragma once

#include <cstring>
#include <string>

#include "TraderFemas/TraderFemas.h"

class R93FemasApiStub : public CUstpFtdcTraderApi
{
public:
	int		n_release_ = 0;
	int		n_init_ = 0;
	int		n_join_ = 0;
	int		n_login_ = 0;
	int		n_logout_ = 0;
	int		n_order_insert_ = 0;
	int		n_order_action_ = 0;
	int		n_auth_ = 0;
	int		n_qry_account_ = 0;
	int		n_qry_position_ = 0;
	int		n_qry_order_ = 0;
	int		n_qry_trade_ = 0;

	int		stub_ret_ = 0;
	const char* trading_day_ = "20260105";
	const char* version_ = "femas-stub-3.02";

	CUstpFtdcTraderSpi*	spi_ = NULL;
	std::string			front_;
	std::string			qry_front_;
	std::string			req_log_;
	std::string			rsp_log_;
	unsigned int		heartbeat_ = 0;
	USTP_TE_RESUME_TYPE	private_topic_ = USTP_TERT_RESUME;
	USTP_TE_RESUME_TYPE	public_topic_ = USTP_TERT_RESUME;

	CUstpFtdcDSUserInfoField		auth_req_;
	CUstpFtdcReqUserLoginField		login_req_;
	CUstpFtdcReqUserLogoutField		logout_req_;
	CUstpFtdcInputOrderField		order_req_;
	CUstpFtdcOrderActionField		action_req_;
	CUstpFtdcQryInvestorAccountField	qry_account_req_;
	CUstpFtdcQryInvestorPositionField	qry_position_req_;
	CUstpFtdcQryOrderField			qry_order_req_;
	CUstpFtdcQryTradeField			qry_trade_req_;

	R93FemasApiStub()
	{
		memset(&auth_req_, 0, sizeof(auth_req_));
		memset(&login_req_, 0, sizeof(login_req_));
		memset(&logout_req_, 0, sizeof(logout_req_));
		memset(&order_req_, 0, sizeof(order_req_));
		memset(&action_req_, 0, sizeof(action_req_));
		memset(&qry_account_req_, 0, sizeof(qry_account_req_));
		memset(&qry_position_req_, 0, sizeof(qry_position_req_));
		memset(&qry_order_req_, 0, sizeof(qry_order_req_));
		memset(&qry_trade_req_, 0, sizeof(qry_trade_req_));
	}
	virtual ~R93FemasApiStub() {}

	virtual void Release() override { ++n_release_; }
	virtual void Init() override { ++n_init_; }
	virtual int Join() override { ++n_join_; return stub_ret_; }
	virtual const char* GetTradingDay() override { return trading_day_; }
	virtual void RegisterFront(char* pszFrontAddress) override { front_ = pszFrontAddress ? pszFrontAddress : ""; }
	virtual void RegisterQryFront(char* pszFrontAddress) override { qry_front_ = pszFrontAddress ? pszFrontAddress : ""; }
	virtual void RegisterNameServer(char*) override {}
	virtual void RegisterSpi(CUstpFtdcTraderSpi* pSpi) override { spi_ = pSpi; }
	virtual int RegisterCertificateFile(const char*, const char*, const char*, const char*) override { return stub_ret_; }
	virtual void SubscribePrivateTopic(USTP_TE_RESUME_TYPE nResumeType) override { private_topic_ = nResumeType; }
	virtual void SubscribePublicTopic(USTP_TE_RESUME_TYPE nResumeType) override { public_topic_ = nResumeType; }
	virtual void SubscribeUserTopic(USTP_TE_RESUME_TYPE) override {}
	virtual void SubscribeForQuote(USTP_TE_RESUME_TYPE) override {}
	virtual void SetHeartbeatTimeout(unsigned int timeout) override { heartbeat_ = timeout; }
	virtual int OpenRequestLog(const char* pszReqLogFileName) override
	{
		req_log_ = pszReqLogFileName ? pszReqLogFileName : "";
		return stub_ret_;
	}
	virtual int OpenResponseLog(const char* pszRspLogFileName) override
	{
		rsp_log_ = pszRspLogFileName ? pszRspLogFileName : "";
		return stub_ret_;
	}
	virtual int RegisterDSProxyUserCert(CUstpFtdcDSProxyCheckUserInfoField*, CUstpFtdcDSProxyUserCertInField*, CUstpFtdcDSProxyUserCertOutField*, int) override { return stub_ret_; }
	virtual int ReqUserLogin(CUstpFtdcReqUserLoginField* pReqUserLogin, int) override
	{
		++n_login_;
		if (pReqUserLogin)
			login_req_ = *pReqUserLogin;
		return stub_ret_;
	}
	virtual int ReqUserLogout(CUstpFtdcReqUserLogoutField* pReqUserLogout, int) override
	{
		++n_logout_;
		if (pReqUserLogout)
			logout_req_ = *pReqUserLogout;
		return stub_ret_;
	}
	virtual int ReqUserPasswordUpdate(CUstpFtdcUserPasswordUpdateField*, int) override { return stub_ret_; }
	virtual int ReqOrderInsert(CUstpFtdcInputOrderField* pInputOrder, int) override
	{
		++n_order_insert_;
		if (pInputOrder)
			order_req_ = *pInputOrder;
		return stub_ret_;
	}
	virtual int ReqOrderAction(CUstpFtdcOrderActionField* pOrderAction, int) override
	{
		++n_order_action_;
		if (pOrderAction)
			action_req_ = *pOrderAction;
		return stub_ret_;
	}
	virtual int ReqQuoteInsert(CUstpFtdcInputQuoteField*, int) override { return stub_ret_; }
	virtual int ReqQuoteAction(CUstpFtdcQuoteActionField*, int) override { return stub_ret_; }
	virtual int ReqForQuote(CUstpFtdcReqForQuoteField*, int) override { return stub_ret_; }
	virtual int ReqMarginCombAction(CUstpFtdcInputMarginCombActionField*, int) override { return stub_ret_; }
	virtual int ReqUserDeposit(CUstpFtdcstpUserDepositField*, int) override { return stub_ret_; }
	virtual int ReqQryOrder(CUstpFtdcQryOrderField* pQryOrder, int) override
	{
		++n_qry_order_;
		if (pQryOrder)
			qry_order_req_ = *pQryOrder;
		return stub_ret_;
	}
	virtual int ReqQryTrade(CUstpFtdcQryTradeField* pQryTrade, int) override
	{
		++n_qry_trade_;
		if (pQryTrade)
			qry_trade_req_ = *pQryTrade;
		return stub_ret_;
	}
	virtual int ReqQryUserInvestor(CUstpFtdcQryUserInvestorField*, int) override { return stub_ret_; }
	virtual int ReqQryTradingCode(CUstpFtdcQryTradingCodeField*, int) override { return stub_ret_; }
	virtual int ReqQryInvestorAccount(CUstpFtdcQryInvestorAccountField* pQryInvestorAccount, int) override
	{
		++n_qry_account_;
		if (pQryInvestorAccount)
			qry_account_req_ = *pQryInvestorAccount;
		return stub_ret_;
	}
	virtual int ReqQryInstrument(CUstpFtdcQryInstrumentField*, int) override { return stub_ret_; }
	virtual int ReqQryExchange(CUstpFtdcQryExchangeField*, int) override { return stub_ret_; }
	virtual int ReqQryInvestorPosition(CUstpFtdcQryInvestorPositionField* pQryInvestorPosition, int) override
	{
		++n_qry_position_;
		if (pQryInvestorPosition)
			qry_position_req_ = *pQryInvestorPosition;
		return stub_ret_;
	}
	virtual int ReqQryComplianceParam(CUstpFtdcQryComplianceParamField*, int) override { return stub_ret_; }
	virtual int ReqQryInvestorFee(CUstpFtdcQryInvestorFeeField*, int) override { return stub_ret_; }
	virtual int ReqQryInvestorMargin(CUstpFtdcQryInvestorMarginField*, int) override { return stub_ret_; }
	virtual int ReqQryInvestorCombPosition(CUstpFtdcQryInvestorCombPositionField*, int) override { return stub_ret_; }
	virtual int ReqQryInvestorLegPosition(CUstpFtdcQryInvestorLegPositionField*, int) override { return stub_ret_; }
	virtual int ReqQryInstrumentGroup(CUstpFtdcQryUstpInstrumentGroupField*, int) override { return stub_ret_; }
	virtual int ReqQryClientMarginCombType(CUstpFtdcQryClientMarginCombTypeField*, int) override { return stub_ret_; }
	virtual int ReqExecOrderInsert(CUstpFtdcInputExecOrderField*, int) override { return stub_ret_; }
	virtual int ReqExecOrderAction(CUstpFtdcInputExecOrderActionField*, int) override { return stub_ret_; }
	virtual int ReqQrySystemTime(CUstpFtdcReqQrySystemTimeField*, int) override { return stub_ret_; }
	virtual int ReqQryMarginPrefParam(CUstpFtdcReqQryMarginPrefParamField*, int) override { return stub_ret_; }
	virtual int ReqDSUserCertification(CUstpFtdcDSUserInfoField* pDSUserInfo, int) override
	{
		++n_auth_;
		if (pDSUserInfo)
			auth_req_ = *pDSUserInfo;
		return stub_ret_;
	}
	virtual int ReqDSProxySubmitInfo(CUstpFtdcDSProxySubmitDataField*, int) override { return stub_ret_; }
};
