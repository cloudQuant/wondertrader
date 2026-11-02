class FakeTraderApi : public HuaXTraderApi
{
public:
    void Release() override { ++releaseCalls; }
    void Init() override { ++initCalls; }
    int Join() override { return 0; }
    void RegisterFront(char *pszFrontAddress) override { ++frontCalls; }
    void RegisterNameServer(char *pszNsAddress) override {  }
    void RegisterFensUserInfo(CTORATstpFensUserInfoField *pFensUserInfoField) override {  }
    void RegisterSpi(CTORATstpTraderSpi *pSpi) override { ++spiCalls; }
    void SubscribePrivateTopic(TORA_TE_RESUME_TYPE nResumeType) override {  }
    void SubscribePublicTopic(TORA_TE_RESUME_TYPE nResumeType) override {  }
    int ReqGetConnectionInfo(int nRequestID) override { return 0; }
    int ReqUserLogin(CTORATstpReqUserLoginField *pReqUserLoginField, int nRequestID) override { return 0; }
    int ReqUserLogout(CTORATstpUserLogoutField *pUserLogoutField, int nRequestID) override { return 0; }
    int ReqUserPasswordUpdate(CTORATstpUserPasswordUpdateField *pUserPasswordUpdateField, int nRequestID) override { return 0; }
    int ReqInputDeviceSerial(CTORATstpReqInputDeviceSerialField *pReqInputDeviceSerialField, int nRequestID) override { return 0; }
    int ReqOrderInsert(CTORATstpInputOrderField *pInputOrderField, int nRequestID) override { return 0; }
    int ReqOrderAction(CTORATstpInputOrderActionField *pInputOrderActionField, int nRequestID) override { return 0; }
    int ReqCondOrderInsert(CTORATstpInputCondOrderField *pInputCondOrderField, int nRequestID) override { return 0; }
    int ReqCondOrderAction(CTORATstpInputCondOrderActionField *pInputCondOrderActionField, int nRequestID) override { return 0; }
    int ReqNegoOrderInsert(CTORATstpInputNegoOrderField *pInputNegoOrderField, int nRequestID) override { return 0; }
    int ReqNegoOrderAction(CTORATstpInputNegoOrderActionField *pInputNegoOrderActionField, int nRequestID) override { return 0; }
    int ReqTransferFund(CTORATstpInputTransferFundField *pInputTransferFundField, int nRequestID) override { return 0; }
    int ReqTransferPosition(CTORATstpInputTransferPositionField *pInputTransferPositionField, int nRequestID) override { return 0; }
    int ReqInquiryJZFund(CTORATstpReqInquiryJZFundField *pReqInquiryJZFundField, int nRequestID) override { return 0; }
    int ReqInquiryBankAccountFund(CTORATstpReqInquiryBankAccountFundField *pReqInquiryBankAccountFundField, int nRequestID) override { return 0; }
    int ReqInquiryMaxOrderVolume(CTORATstpReqInquiryMaxOrderVolumeField *pReqInquiryMaxOrderVolumeField, int nRequestID) override { return 0; }
    int ReqInquiryTradeConcentration(CTORATstpInquiryTradeConcentrationField *pInquiryTradeConcentrationField, int nRequestID) override { return 0; }
    int ReqModifyOpenPosCost(CTORATstpReqModifyOpenPosCostField *pReqModifyOpenPosCostField, int nRequestID) override { return 0; }
    int ReqInputNodeFundAssignment(CTORATstpInputNodeFundAssignmentField *pInputNodeFundAssignmentField, int nRequestID) override { return 0; }
    int ReqInquiryNodeFundAssignment(CTORATstpReqInquiryNodeFundAssignmentField *pReqInquiryNodeFundAssignmentField, int nRequestID) override { return 0; }
    int ReqQryExchange(CTORATstpQryExchangeField *pQryExchangeField, int nRequestID) override { return 0; }
    int ReqQrySecurity(CTORATstpQrySecurityField *pQrySecurityField, int nRequestID) override { return 0; }
    int ReqQryIPOInfo(CTORATstpQryIPOInfoField *pQryIPOInfoField, int nRequestID) override { return 0; }
    int ReqQryUser(CTORATstpQryUserField *pQryUserField, int nRequestID) override { return 0; }
    int ReqQryInvestor(CTORATstpQryInvestorField *pQryInvestorField, int nRequestID) override { return 0; }
    int ReqQryShareholderAccount(CTORATstpQryShareholderAccountField *pQryShareholderAccountField, int nRequestID) override { return 0; }
    int ReqQryRationalInfo(CTORATstpQryRationalInfoField *pQryRationalInfoField, int nRequestID) override { return 0; }
    int ReqQryOrder(CTORATstpQryOrderField *pQryOrderField, int nRequestID) override { return 0; }
    int ReqQryOrderAction(CTORATstpQryOrderActionField *pQryOrderActionField, int nRequestID) override { return 0; }
    int ReqQryTrade(CTORATstpQryTradeField *pQryTradeField, int nRequestID) override { return 0; }
    int ReqQryTradingAccount(CTORATstpQryTradingAccountField *pQryTradingAccountField, int nRequestID) override { return 0; }
    int ReqQryPosition(CTORATstpQryPositionField *pQryPositionField, int nRequestID) override { return 0; }
    int ReqQryTradingFee(CTORATstpQryTradingFeeField *pQryTradingFeeField, int nRequestID) override { return 0; }
    int ReqQryInvestorTradingFee(CTORATstpQryInvestorTradingFeeField *pQryInvestorTradingFeeField, int nRequestID) override { return 0; }
    int ReqQryIPOQuota(CTORATstpQryIPOQuotaField *pQryIPOQuotaField, int nRequestID) override { return 0; }
    int ReqQryOrderFundDetail(CTORATstpQryOrderFundDetailField *pQryOrderFundDetailField, int nRequestID) override { return 0; }
    int ReqQryFundTransferDetail(CTORATstpQryFundTransferDetailField *pQryFundTransferDetailField, int nRequestID) override { return 0; }
    int ReqQryPositionTransferDetail(CTORATstpQryPositionTransferDetailField *pQryPositionTransferDetailField, int nRequestID) override { return 0; }
    int ReqQryPeripheryPositionTransferDetail(CTORATstpQryPeripheryPositionTransferDetailField *pQryPeripheryPositionTransferDetailField, int nRequestID) override { return 0; }
    int ReqQryPeripheryFundTransferDetail(CTORATstpQryPeripheryFundTransferDetailField *pQryPeripheryFundTransferDetailField, int nRequestID) override { return 0; }
    int ReqQryBondConversionInfo(CTORATstpQryBondConversionInfoField *pQryBondConversionInfoField, int nRequestID) override { return 0; }
    int ReqQryBondPutbackInfo(CTORATstpQryBondPutbackInfoField *pQryBondPutbackInfoField, int nRequestID) override { return 0; }
    int ReqQryInvestorCondOrderLimitParam(CTORATstpQryInvestorCondOrderLimitParamField *pQryInvestorCondOrderLimitParamField, int nRequestID) override { return 0; }
    int ReqQryConditionOrder(CTORATstpQryConditionOrderField *pQryConditionOrderField, int nRequestID) override { return 0; }
    int ReqQryCondOrderAction(CTORATstpQryCondOrderActionField *pQryCondOrderActionField, int nRequestID) override { return 0; }
    int ReqQryTradingNotice(CTORATstpQryTradingNoticeField *pQryTradingNoticeField, int nRequestID) override { return 0; }
    int ReqQryIPONumberResult(CTORATstpQryIPONumberResultField *pQryIPONumberResultField, int nRequestID) override { return 0; }
    int ReqQryIPOMatchNumberResult(CTORATstpQryIPOMatchNumberResultField *pQryIPOMatchNumberResultField, int nRequestID) override { return 0; }
    int ReqQryShareholderSpecPrivilege(CTORATstpQryShareholderSpecPrivilegeField *pQryShareholderSpecPrivilegeField, int nRequestID) override { return 0; }
    int ReqQryMarket(CTORATstpQryMarketField *pQryMarketField, int nRequestID) override { return 0; }
    int ReqQryETFFile(CTORATstpQryETFFileField *pQryETFFileField, int nRequestID) override { return 0; }
    int ReqQryETFBasket(CTORATstpQryETFBasketField *pQryETFBasketField, int nRequestID) override { return 0; }
    int ReqQryInvestorPositionLimit(CTORATstpQryInvestorPositionLimitField *pQryInvestorPositionLimitField, int nRequestID) override { return 0; }
    int ReqQrySZSEImcParams(CTORATstpQrySZSEImcParamsField *pQrySZSEImcParamsField, int nRequestID) override { return 0; }
    int ReqQrySZSEImcExchangeRate(CTORATstpQrySZSEImcExchangeRateField *pQrySZSEImcExchangeRateField, int nRequestID) override { return 0; }
    int ReqQrySZSEHKPriceTickInfo(CTORATstpQrySZSEHKPriceTickInfoField *pQrySZSEHKPriceTickInfoField, int nRequestID) override { return 0; }
    int ReqQryLofFundInfo(CTORATstpQryLofFundInfoField *pQryLofFundInfoField, int nRequestID) override { return 0; }
    int ReqQryPledgePosition(CTORATstpQryPledgePositionField *pQryPledgePositionField, int nRequestID) override { return 0; }
    int ReqQryPledgeInfo(CTORATstpQryPledgeInfoField *pQryPledgeInfoField, int nRequestID) override { return 0; }
    int ReqQrySystemNodeInfo(CTORATstpQrySystemNodeInfoField *pQrySystemNodeInfoField, int nRequestID) override { return 0; }
    int ReqQryStandardBondPosition(CTORATstpQryStandardBondPositionField *pQryStandardBondPositionField, int nRequestID) override { return 0; }
    int ReqQryPrematurityRepoOrder(CTORATstpQryPrematurityRepoOrderField *pQryPrematurityRepoOrderField, int nRequestID) override { return 0; }
    int ReqQryNegoOrder(CTORATstpQryNegoOrderField *pQryNegoOrderField, int nRequestID) override { return 0; }
    int ReqQryNegoOrderAction(CTORATstpQryNegoOrderActionField *pQryNegoOrderActionField, int nRequestID) override { return 0; }
    int ReqQryNegoTrade(CTORATstpQryNegoTradeField *pQryNegoTradeField, int nRequestID) override { return 0; }
    int ReqQryNegotiationParam(CTORATstpQryNegotiationParamField *pQryNegotiationParamField, int nRequestID) override { return 0; }
};
