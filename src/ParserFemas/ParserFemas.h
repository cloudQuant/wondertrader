/*!
 * \file ParserFemas.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#pragma once
#include "../Includes/IParserApi.h"
#include "../Share/DLLHelper.hpp"
#include <API/Femas3.02/USTPFtdcMduserApi.h>
#include <map>

NS_WTP_BEGIN
class WTSTickData;
NS_WTP_END

USING_NS_WTP;

class ParserFemas : public IParserApi, public CUstpFtdcMduserSpi
{
public:
	ParserFemas();
	virtual ~ParserFemas();

public:
	enum LoginStatus
	{
		LS_NOTLOGIN,
		LS_LOGINING,
		LS_LOGINED
	};

//IQuoteParser 接口
public:
	virtual bool init(WTSVariant* config) override;

	virtual void release() override;

	virtual bool connect() override;

	virtual bool disconnect() override;

	virtual bool isConnected() override;

	virtual void subscribe(const CodeSet &vecSymbols) override;
	virtual void unsubscribe(const CodeSet &vecSymbols) override;

	virtual void registerSpi(IParserSpi* listener) override;


//CUstpFtdcMdSpi 接口
public:
	virtual void OnRspError( CUstpFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast ) override;

	virtual void OnFrontConnected() override;

	virtual void OnRspUserLogin( CUstpFtdcRspUserLoginField *pRspUserLogin, CUstpFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast ) override;

	///登出请求响应
	virtual void OnRspUserLogout(CUstpFtdcRspUserLogoutField *pUserLogout, CUstpFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	virtual void OnFrontDisconnected( int nReason ) override;

	virtual void OnRspUnSubMarketData( CUstpFtdcSpecificInstrumentField *pSpecificInstrument, CUstpFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast ) override;

	virtual void OnRtnDepthMarketData( CUstpFtdcDepthMarketDataField *pDepthMarketData ) override;

	virtual void OnRspSubMarketData( CUstpFtdcSpecificInstrumentField *pSpecificInstrument, CUstpFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast ) override;

	virtual void OnHeartBeatWarning( int nTimeLapse ) override;

private:
	/*
	 *	发送登录请求
	 */
	void ReqUserLogin();
	/*
	 *	订阅品种行情
	 */
	void SubscribeMarketData();
	/*
	 *	检查错误信息
	 */
	bool IsErrorRspInfo(CUstpFtdcRspInfoField *pRspInfo);


private:
	uint32_t			m_uTradingDate;
	[[maybe_unused]] LoginStatus			m_loginState;
	CUstpFtdcMduserApi*	m_pUserAPI;

	std::string			m_strFrontAddr;
	std::string			m_strBroker;
	std::string			m_strUserID;
	std::string			m_strPassword;
	std::string			m_strFlowDir;

	CodeSet				m_filterSubs;

	int					m_iRequestID;

	IParserSpi*	m_sink;
	IBaseDataMgr*		m_pBaseDataMgr;

	DllHandle		m_hInst;
	typedef CUstpFtdcMduserApi* (*FemasCreator)(const char *);
	FemasCreator		m_funcCreator;
};

