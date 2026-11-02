class FakeParserApi : public HuaXParserApi
{
public:
    void Release() override { ++releaseCalls; }
    void Init() override { ++initCalls; }
    int Join() override { return 0; }
    void RegisterFront(char *pszFrontAddress) override { ++frontCalls; }
    void RegisterNameServer(char *pszNsAddress) override {  }
    void RegisterFensUserInfo(CTORATstpFensUserInfoField *pFensUserInfoField) override {  }
    void RegisterMulticast(char *pszMulticastAddress, char *pszInterfaceIP, char *pszSourceIp) override {  }
    void RegisterDeriveServer(char *pszDeriveAddress) override {  }
    void RegisterDeriveMulticast(char * pszMulticastAddress, char * pszInterfaceIP, char * pszSourceIp) override {  }
    void RegisterSpi(CTORATstpXMdSpi *pSpi) override { ++spiCalls; }
    int ReqGetConnectionInfo(int nRequestID) override { return 0; }
    int ReqUserLogin(CTORATstpReqUserLoginField *pReqUserLoginField, int nRequestID) override { return 0; }
    int ReqUserLogout(CTORATstpUserLogoutField *pUserLogoutField, int nRequestID) override { return 0; }
    int SubscribeMarketData(char **ppSecurityID, int nCount, TTORATstpExchangeIDType ExchangeID) override { return 0; }
    int UnSubscribeMarketData(char **ppSecurityID, int nCount, TTORATstpExchangeIDType ExchangeID) override { return 0; }
    int SubscribePHMarketData(char **ppSecurityID, int nCount, TTORATstpExchangeIDType ExchangeID) override { return 0; }
    int UnSubscribePHMarketData(char **ppSecurityID, int nCount, TTORATstpExchangeIDType ExchangeID) override { return 0; }
    int SubscribeSpecialMarketData(char **ppSecurityID, int nCount, TTORATstpExchangeIDType ExchangeID) override { return 0; }
    int UnSubscribeSpecialMarketData(char **ppSecurityID, int nCount, TTORATstpExchangeIDType ExchangeID) override { return 0; }
    int SubscribeSimplifyMarketData(char **ppSecurityID, int nCount, TTORATstpExchangeIDType ExchangeID) override { return 0; }
    int UnSubscribeSimplifyMarketData(char **ppSecurityID, int nCount, TTORATstpExchangeIDType ExchangeID) override { return 0; }
    int SubscribeSecurityStatus(char **ppSecurityID, int nCount, TTORATstpExchangeIDType ExchangeID) override { return 0; }
    int UnSubscribeSecurityStatus(char **ppSecurityID, int nCount, TTORATstpExchangeIDType ExchangeID) override { return 0; }
    int SubscribeMarketStatus(TTORATstpMarketIDType MarketID) override { return 0; }
    int UnSubscribeMarketStatus(TTORATstpMarketIDType MarketID) override { return 0; }
    int SubscribeImcParams(TTORATstpMarketIDType MarketID) override { return 0; }
    int UnSubscribeImcParams(TTORATstpMarketIDType MarketID) override { return 0; }
    int ReqInquiryMarketDataMirror(CTORATstpInquiryMarketDataField *pInquiryMarketDataField, int nRequestID) override { return 0; }
    int ReqInquiryPHMarketDataMirror(CTORATstpInquiryMarketDataField *pInquiryMarketDataField, int nRequestID) override { return 0; }
    int ReqInquirySpecialMarketDataMirror(CTORATstpInquirySpecialMarketDataField *pInquirySpecialMarketDataField, int nRequestID) override { return 0; }
    int SubscribeSPMarketData(char **ppSecurityID, int nCount, TTORATstpExchangeIDType ExchangeID) override { return 0; }
    int UnSubscribeSPMarketData(char **ppSecurityID, int nCount, TTORATstpExchangeIDType ExchangeID) override { return 0; }
    int SubscribeSPSimplifyMarketData(char **ppSecurityID, int nCount, TTORATstpExchangeIDType ExchangeID) override { return 0; }
    int UnSubscribeSPSimplifyMarketData(char **ppSecurityID, int nCount, TTORATstpExchangeIDType ExchangeID) override { return 0; }
    int SubscribeSPSecurityStatus(char **ppSecurityID, int nCount, TTORATstpExchangeIDType ExchangeID) override { return 0; }
    int UnSubscribeSPSecurityStatus(char **ppSecurityID, int nCount, TTORATstpExchangeIDType ExchangeID) override { return 0; }
    int SubscribeSPMarketStatus(TTORATstpMarketIDType MarketID) override { return 0; }
    int UnSubscribeSPMarketStatus(TTORATstpMarketIDType MarketID) override { return 0; }
    int ReqInquirySPMarketDataMirror(CTORATstpInquiryMarketDataField *pInquiryMarketDataField, int nRequestID) override { return 0; }
    int SubscribeRapidMarketData(char **ppSecurityID, int nCount, TTORATstpExchangeIDType ExchangeID) override { return 0; }
    int UnSubscribeRapidMarketData(char **ppSecurityID, int nCount, TTORATstpExchangeIDType ExchangeID) override { return 0; }
    int SubscribeFundsFlowMarketData(char **ppSecurityID, int nCount, TTORATstpExchangeIDType ExchangeID) override { return 0; }
    int UnSubscribeFundsFlowMarketData(char **ppSecurityID, int nCount, TTORATstpExchangeIDType ExchangeID) override { return 0; }
};
