/*
 * t25_ctp_api_stub.hpp — lane R25 自带柜台替身（与 lane T2 的 t2_ctp_api_stub.hpp 同形、独立命名空间）
 *
 * 为什么 lane R25 不复用 T2 的头：本轮要跑真实 worker 线程（connect()/disconnect()），
 * 替身计数器必须是原子的，且要能观察 RegisterFront/Subscribe*Topic/Release/Init 的次数与入参。
 * 逐条签名照抄 src/API/CTP6.6.9/ThostFtdcTraderApi.h 的 107 个纯虚函数（与 T2 同源、同集合），
 * 目的是让 TraderCTP::m_pUserAPI（protected 成员，经 t25ctp::Probe 派生 seam 注入）
 * 指向一个**不起线程、不开 socket、不碰厂商实现**的空壳。
 *
 * 注意：StubApi 里没有任何真实网络调用。connect() 用到的 m_funcCreator 也被换成
 * 测试自己的 creator，因此厂商的 CreateFtdcTraderApi 在用例里一个字节都不会被执行。
 */
#pragma once

#include <atomic>
#include <cstring>
#include <string>

#include "TraderCTP/TraderCTP.h"

namespace t25ctp {

class StubApi : public CThostFtdcTraderApi
{
public:
    std::atomic<unsigned int>   n_other_{0};    ///< 未被细化的其余调用
    std::atomic<int>            ret_{0};        ///< 所有 Req* 的统一返回值（用例可编程）
    std::atomic<int>  n_release_{0}, n_init_{0}, n_register_spi_{0}, n_register_front_{0};
    std::atomic<int>  n_sub_public_{0}, n_sub_private_{0};
    std::atomic<int>  n_auth_{0}, n_login_{0}, n_logout_{0}, n_insert_{0}, n_action_{0};
    std::atomic<int>  n_confirm_{0}, n_qry_confirm_{0}, n_qry_acct_{0}, n_qry_pos_{0};
    std::atomic<int>  n_qry_order_{0}, n_qry_trade_{0}, n_qry_settle_{0};
    int               last_sub_public_ = 0, last_sub_private_ = 0;
    int               last_req_id_ = 0;
    CThostFtdcTraderSpi* ptr_last_spi_ = NULL;
    std::string       str_last_front_;
    char              trading_day_[16] = "20240115";

    CThostFtdcReqAuthenticateField        auth_;
    CThostFtdcReqUserLoginField           login_;
    CThostFtdcUserLogoutField             logout_;
    CThostFtdcInputOrderField             order_;
    CThostFtdcInputOrderActionField       action_;
    CThostFtdcSettlementInfoConfirmField  confirm_;
    CThostFtdcQrySettlementInfoConfirmField qry_confirm_;
    CThostFtdcQryTradingAccountField      qry_acct_;
    CThostFtdcQryInvestorPositionField    qry_pos_;
    CThostFtdcQryOrderField               qry_order_;
    CThostFtdcQryTradeField               qry_trade_;
    CThostFtdcQrySettlementInfoField      qry_settle_;

    void resetCaptured()
    {
        memset(&auth_, 0, sizeof(auth_));
        memset(&login_, 0, sizeof(login_));
        memset(&logout_, 0, sizeof(logout_));
        memset(&order_, 0, sizeof(order_));
        memset(&action_, 0, sizeof(action_));
        memset(&confirm_, 0, sizeof(confirm_));
        memset(&qry_confirm_, 0, sizeof(qry_confirm_));
        memset(&qry_acct_, 0, sizeof(qry_acct_));
        memset(&qry_pos_, 0, sizeof(qry_pos_));
        memset(&qry_order_, 0, sizeof(qry_order_));
        memset(&qry_trade_, 0, sizeof(qry_trade_));
        memset(&qry_settle_, 0, sizeof(qry_settle_));
    }

StubApi() : n_other_(0), ret_(0) {}
	virtual ~StubApi() {}

	virtual void Release() override { ++n_release_; }
	virtual void Init() override { ++n_init_; }
	virtual int Join() override { ++n_other_; return ret_; }
	virtual const char * GetTradingDay() override { return trading_day_; }
	virtual void RegisterFront(char *pFront) override { ++n_register_front_; if (pFront) str_last_front_ = pFront; }
	virtual void RegisterNameServer(char *) override { ++n_other_; }
	virtual void RegisterFensUserInfo(CThostFtdcFensUserInfoField *) override { ++n_other_; }
	virtual void RegisterSpi(CThostFtdcTraderSpi *pSpi) override { ++n_register_spi_; ptr_last_spi_ = pSpi; }
	virtual void SubscribePrivateTopic(THOST_TE_RESUME_TYPE e) override { ++n_sub_private_; last_sub_private_ = (int)e; }
	virtual void SubscribePublicTopic(THOST_TE_RESUME_TYPE e) override { ++n_sub_public_; last_sub_public_ = (int)e; }
	virtual int ReqAuthenticate(CThostFtdcReqAuthenticateField *pReq, int nRequestID) override { ++n_auth_; last_req_id_ = nRequestID; if (pReq) memcpy(&auth_, pReq, sizeof(auth_)); return ret_; }
	virtual int RegisterUserSystemInfo(CThostFtdcUserSystemInfoField *) override { ++n_other_; return ret_; }
	virtual int SubmitUserSystemInfo(CThostFtdcUserSystemInfoField *) override { ++n_other_; return ret_; }
	virtual int ReqUserLogin(CThostFtdcReqUserLoginField *pReq, int nRequestID) override { ++n_login_; last_req_id_ = nRequestID; if (pReq) memcpy(&login_, pReq, sizeof(login_)); return ret_; }
	virtual int ReqUserLogout(CThostFtdcUserLogoutField *pReq, int nRequestID) override { ++n_logout_; last_req_id_ = nRequestID; if (pReq) memcpy(&logout_, pReq, sizeof(logout_)); return ret_; }
	virtual int ReqUserPasswordUpdate(CThostFtdcUserPasswordUpdateField *, int) override { ++n_other_; return ret_; }
	virtual int ReqTradingAccountPasswordUpdate(CThostFtdcTradingAccountPasswordUpdateField *, int) override { ++n_other_; return ret_; }
	virtual int ReqUserAuthMethod(CThostFtdcReqUserAuthMethodField *, int) override { ++n_other_; return ret_; }
	virtual int ReqGenUserCaptcha(CThostFtdcReqGenUserCaptchaField *, int) override { ++n_other_; return ret_; }
	virtual int ReqGenUserText(CThostFtdcReqGenUserTextField *, int) override { ++n_other_; return ret_; }
	virtual int ReqUserLoginWithCaptcha(CThostFtdcReqUserLoginWithCaptchaField *, int) override { ++n_other_; return ret_; }
	virtual int ReqUserLoginWithText(CThostFtdcReqUserLoginWithTextField *, int) override { ++n_other_; return ret_; }
	virtual int ReqUserLoginWithOTP(CThostFtdcReqUserLoginWithOTPField *, int) override { ++n_other_; return ret_; }
	virtual int ReqOrderInsert(CThostFtdcInputOrderField *pReq, int nRequestID) override { ++n_insert_; last_req_id_ = nRequestID; if (pReq) memcpy(&order_, pReq, sizeof(order_)); return ret_; }
	virtual int ReqParkedOrderInsert(CThostFtdcParkedOrderField *, int) override { ++n_other_; return ret_; }
	virtual int ReqParkedOrderAction(CThostFtdcParkedOrderActionField *, int) override { ++n_other_; return ret_; }
	virtual int ReqOrderAction(CThostFtdcInputOrderActionField *pReq, int nRequestID) override { ++n_action_; last_req_id_ = nRequestID; if (pReq) memcpy(&action_, pReq, sizeof(action_)); return ret_; }
	virtual int ReqQryMaxOrderVolume(CThostFtdcQryMaxOrderVolumeField *, int) override { ++n_other_; return ret_; }
	virtual int ReqSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField *pReq, int nRequestID) override { ++n_confirm_; last_req_id_ = nRequestID; if (pReq) memcpy(&confirm_, pReq, sizeof(confirm_)); return ret_; }
	virtual int ReqRemoveParkedOrder(CThostFtdcRemoveParkedOrderField *, int) override { ++n_other_; return ret_; }
	virtual int ReqRemoveParkedOrderAction(CThostFtdcRemoveParkedOrderActionField *, int) override { ++n_other_; return ret_; }
	virtual int ReqExecOrderInsert(CThostFtdcInputExecOrderField *, int) override { ++n_other_; return ret_; }
	virtual int ReqExecOrderAction(CThostFtdcInputExecOrderActionField *, int) override { ++n_other_; return ret_; }
	virtual int ReqForQuoteInsert(CThostFtdcInputForQuoteField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQuoteInsert(CThostFtdcInputQuoteField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQuoteAction(CThostFtdcInputQuoteActionField *, int) override { ++n_other_; return ret_; }
	virtual int ReqBatchOrderAction(CThostFtdcInputBatchOrderActionField *, int) override { ++n_other_; return ret_; }
	virtual int ReqOptionSelfCloseInsert(CThostFtdcInputOptionSelfCloseField *, int) override { ++n_other_; return ret_; }
	virtual int ReqOptionSelfCloseAction(CThostFtdcInputOptionSelfCloseActionField *, int) override { ++n_other_; return ret_; }
	virtual int ReqCombActionInsert(CThostFtdcInputCombActionField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryOrder(CThostFtdcQryOrderField *pReq, int nRequestID) override { ++n_qry_order_; last_req_id_ = nRequestID; if (pReq) memcpy(&qry_order_, pReq, sizeof(qry_order_)); return ret_; }
	virtual int ReqQryTrade(CThostFtdcQryTradeField *pReq, int nRequestID) override { ++n_qry_trade_; last_req_id_ = nRequestID; if (pReq) memcpy(&qry_trade_, pReq, sizeof(qry_trade_)); return ret_; }
	virtual int ReqQryInvestorPosition(CThostFtdcQryInvestorPositionField *pReq, int nRequestID) override { ++n_qry_pos_; last_req_id_ = nRequestID; if (pReq) memcpy(&qry_pos_, pReq, sizeof(qry_pos_)); return ret_; }
	virtual int ReqQryTradingAccount(CThostFtdcQryTradingAccountField *pReq, int nRequestID) override { ++n_qry_acct_; last_req_id_ = nRequestID; if (pReq) memcpy(&qry_acct_, pReq, sizeof(qry_acct_)); return ret_; }
	virtual int ReqQryInvestor(CThostFtdcQryInvestorField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryTradingCode(CThostFtdcQryTradingCodeField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryInstrumentMarginRate(CThostFtdcQryInstrumentMarginRateField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryInstrumentCommissionRate(CThostFtdcQryInstrumentCommissionRateField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryExchange(CThostFtdcQryExchangeField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryProduct(CThostFtdcQryProductField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryInstrument(CThostFtdcQryInstrumentField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryDepthMarketData(CThostFtdcQryDepthMarketDataField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryTraderOffer(CThostFtdcQryTraderOfferField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQrySettlementInfo(CThostFtdcQrySettlementInfoField *pReq, int nRequestID) override { ++n_qry_settle_; last_req_id_ = nRequestID; if (pReq) memcpy(&qry_settle_, pReq, sizeof(qry_settle_)); return ret_; }
	virtual int ReqQryTransferBank(CThostFtdcQryTransferBankField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryInvestorPositionDetail(CThostFtdcQryInvestorPositionDetailField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryNotice(CThostFtdcQryNoticeField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQrySettlementInfoConfirm(CThostFtdcQrySettlementInfoConfirmField *pReq, int nRequestID) override { ++n_qry_confirm_; last_req_id_ = nRequestID; if (pReq) memcpy(&qry_confirm_, pReq, sizeof(qry_confirm_)); return ret_; }
	virtual int ReqQryInvestorPositionCombineDetail(CThostFtdcQryInvestorPositionCombineDetailField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryCFMMCTradingAccountKey(CThostFtdcQryCFMMCTradingAccountKeyField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryEWarrantOffset(CThostFtdcQryEWarrantOffsetField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryInvestorProductGroupMargin(CThostFtdcQryInvestorProductGroupMarginField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryExchangeMarginRate(CThostFtdcQryExchangeMarginRateField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryExchangeMarginRateAdjust(CThostFtdcQryExchangeMarginRateAdjustField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryExchangeRate(CThostFtdcQryExchangeRateField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQrySecAgentACIDMap(CThostFtdcQrySecAgentACIDMapField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryProductExchRate(CThostFtdcQryProductExchRateField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryProductGroup(CThostFtdcQryProductGroupField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryMMInstrumentCommissionRate(CThostFtdcQryMMInstrumentCommissionRateField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryMMOptionInstrCommRate(CThostFtdcQryMMOptionInstrCommRateField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryInstrumentOrderCommRate(CThostFtdcQryInstrumentOrderCommRateField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQrySecAgentTradingAccount(CThostFtdcQryTradingAccountField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQrySecAgentCheckMode(CThostFtdcQrySecAgentCheckModeField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQrySecAgentTradeInfo(CThostFtdcQrySecAgentTradeInfoField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryOptionInstrTradeCost(CThostFtdcQryOptionInstrTradeCostField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryOptionInstrCommRate(CThostFtdcQryOptionInstrCommRateField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryExecOrder(CThostFtdcQryExecOrderField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryForQuote(CThostFtdcQryForQuoteField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryQuote(CThostFtdcQryQuoteField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryOptionSelfClose(CThostFtdcQryOptionSelfCloseField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryInvestUnit(CThostFtdcQryInvestUnitField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryCombInstrumentGuard(CThostFtdcQryCombInstrumentGuardField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryCombAction(CThostFtdcQryCombActionField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryTransferSerial(CThostFtdcQryTransferSerialField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryAccountregister(CThostFtdcQryAccountregisterField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryContractBank(CThostFtdcQryContractBankField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryParkedOrder(CThostFtdcQryParkedOrderField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryParkedOrderAction(CThostFtdcQryParkedOrderActionField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryTradingNotice(CThostFtdcQryTradingNoticeField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryBrokerTradingParams(CThostFtdcQryBrokerTradingParamsField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryBrokerTradingAlgos(CThostFtdcQryBrokerTradingAlgosField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQueryCFMMCTradingAccountToken(CThostFtdcQueryCFMMCTradingAccountTokenField *, int) override { ++n_other_; return ret_; }
	virtual int ReqFromBankToFutureByFuture(CThostFtdcReqTransferField *, int) override { ++n_other_; return ret_; }
	virtual int ReqFromFutureToBankByFuture(CThostFtdcReqTransferField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQueryBankAccountMoneyByFuture(CThostFtdcReqQueryAccountField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryClassifiedInstrument(CThostFtdcQryClassifiedInstrumentField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryCombPromotionParam(CThostFtdcQryCombPromotionParamField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryRiskSettleInvstPosition(CThostFtdcQryRiskSettleInvstPositionField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryRiskSettleProductStatus(CThostFtdcQryRiskSettleProductStatusField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQrySPBMFutureParameter(CThostFtdcQrySPBMFutureParameterField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQrySPBMOptionParameter(CThostFtdcQrySPBMOptionParameterField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQrySPBMIntraParameter(CThostFtdcQrySPBMIntraParameterField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQrySPBMInterParameter(CThostFtdcQrySPBMInterParameterField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQrySPBMPortfDefinition(CThostFtdcQrySPBMPortfDefinitionField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQrySPBMInvestorPortfDef(CThostFtdcQrySPBMInvestorPortfDefField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryInvestorPortfMarginRatio(CThostFtdcQryInvestorPortfMarginRatioField *, int) override { ++n_other_; return ret_; }
	virtual int ReqQryInvestorProdSPBMDetail(CThostFtdcQryInvestorProdSPBMDetailField *, int) override { ++n_other_; return ret_; }
};


}// namespace t25ctp
