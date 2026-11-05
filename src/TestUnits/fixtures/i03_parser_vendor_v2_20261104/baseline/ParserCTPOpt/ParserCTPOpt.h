/*!
 * \file ParserCTPOpt.h
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
#include <API/CTPOpt3.5.8/ThostFtdcMdApi.h>
#include <map>

NS_WTP_BEGIN
class WTSTickData;
NS_WTP_END

USING_NS_WTP;

class ParserCTPOpt :	public IParserApi, public CThostFtdcMdSpi
{
public:
	ParserCTPOpt();
	virtual ~ParserCTPOpt();

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


//CThostFtdcMdSpi 接口
public:
	virtual void OnRspError( CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast ) override;

	virtual void OnFrontConnected() override;

	virtual void OnRspUserLogin( CThostFtdcRspUserLoginField *pRspUserLogin, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast ) override;

	///登出请求响应
	virtual void OnRspUserLogout(CThostFtdcUserLogoutField *pUserLogout, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	virtual void OnFrontDisconnected( int nReason ) override;

	virtual void OnRspUnSubMarketData( CThostFtdcSpecificInstrumentField *pSpecificInstrument, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast ) override;

	virtual void OnRtnDepthMarketData( CThostFtdcDepthMarketDataField *pDepthMarketData ) override;

	virtual void OnRspSubMarketData( CThostFtdcSpecificInstrumentField *pSpecificInstrument, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast ) override;

	virtual void OnHeartBeatWarning( int nTimeLapse ) override;

private:
	typedef CThostFtdcMdApi* (*CTPCreator)(const char *, const bool, const bool);
	/*
	 *	发送登录请求
	 */
	void ReqUserLogin();
	bool bindApi(CTPCreator creator, const char* flowPath);
	void unloadApiLibrary();
	/*
	 *	订阅品种行情
	 */
	void SubscribeMarketData();
	/*
	 *	按批次上限发送订阅请求（T2-OPT-04 / 迭代03 卡14 14b）
	 *	SDK 返回 0 只表示"已发送"，订阅确认来自 OnRspSubMarketData。
	 */
	void sendSubscribeBatches(const CodeSet& codes);
	/*
	 *	检查错误信息
	 */
	bool IsErrorRspInfo(CThostFtdcRspInfoField *pRspInfo);


private:
	uint32_t			m_uTradingDate;
	[[maybe_unused]] LoginStatus			m_loginState;
	CThostFtdcMdApi*	m_pUserAPI;

	std::string			m_strFrontAddr;
	std::string			m_strBroker;
	std::string			m_strUserID;
	std::string			m_strPassword;
	std::string			m_strFlowDir;

	CodeSet				m_filterSubs;

	// 单次 SubscribeMarketData 的合约数上限（配置项 subbatch）。
	// CTPOpt3.5.8 头文件未声明上限；默认 500 沿用本模块历史固定缓冲容量，不是厂商声明值。
	static constexpr uint32_t DEFAULT_SUB_BATCH = 500;
	uint32_t			m_uSubBatch = DEFAULT_SUB_BATCH;

	int					m_iRequestID;

	IParserSpi*	m_sink;
	IBaseDataMgr*		m_pBaseDataMgr;

	DllHandle		m_hInstCTP;
	CTPCreator		m_funcCreator;

	uint32_t		m_uLatestTime = 0;
	int64_t			m_iLastTimestamp = 0;
};

