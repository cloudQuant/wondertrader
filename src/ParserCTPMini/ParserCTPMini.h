/*!
 * \file ParserCTPMini.h
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
#include "../ParserCTP/VendorMdLifecycleV2.hpp"
#include <API/CTPMini1.5.8/ThostFtdcMdApi.h>
#include <cstddef>
#include <map>
#include <mutex>

NS_WTP_BEGIN
class WTSTickData;
NS_WTP_END

USING_NS_WTP;

class ParserCTPMini :	public IParserApi, public CThostFtdcMdSpi
{
public:
	ParserCTPMini();
	virtual ~ParserCTPMini();

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

//Parser lifecycle V2（迭代03 卡9，见 ../ParserCTP/VendorMdLifecycleV2.hpp）
public:
	WtParserLifecycleV2Status requestStopV2() noexcept;
	WtParserLifecycleV2Status canWaitV2() const noexcept;
	WtParserLifecycleV2Status waitDrainedV2(uint32_t timeoutMs) noexcept;
	bool destroyableV2() const noexcept;
	/*
	 *	deleteParser 的移除判定（卡9 9b）：
	 *	回调/接口重入时延后并保留；曾创建过 SDK 对象时也保留（无线程退出确认）
	 */
	bool removableLegacy() noexcept;

private:
	bool shutdownLegacy() noexcept;
	void releaseSdk() noexcept;
	bool sdkSeen() const noexcept;

private:
	/*
	 *	发送登录请求
	 */
	void ReqUserLogin();
	/*
	 *	订阅品种行情
	 */
	void SubscribeMarketData();
	void SubmitMarketDataSubscriptions(const CodeSet &codes);
	static bool IsSubscriptionCountSupported(std::size_t count);
	/*
	 *	检查错误信息
	 */
	bool IsErrorRspInfo(CThostFtdcRspInfoField *pRspInfo);


private:
	uint32_t			m_uTradingDate;
	int					m_iMidnightMode = 0;	//卡3 3b：0=默认 A（00 时行情按本地次日纠偏），1=旧行为 B；配置项 midnight_actionday
	[[maybe_unused]] LoginStatus			m_loginState;
	CThostFtdcMdApi*	m_pUserAPI;

	std::string			m_strFrontAddr;
	std::string			m_strBroker;
	std::string			m_strUserID;
	std::string			m_strPassword;
	std::string			m_strFlowDir;

	CodeSet				m_filterSubs;

	int					m_iRequestID;

	IParserSpi*	m_sink;
	IBaseDataMgr*		m_pBaseDataMgr;

	DllHandle		m_hInstCTP;
	typedef CThostFtdcMdApi* (*CTPCreator)(const char *, const bool, const bool);
	CTPCreator		m_funcCreator;

	wt_vendor_md_v2::Lifecycle	_lc;
	std::mutex					_sdk_mutex;		//串行化 SDK 释放
	std::recursive_mutex		_data_mutex;	//订阅缓存 / 交易日（宿主线程与 SDK 回调线程共享）
	bool						_init_done = false;
	bool						_connect_issued = false;
};

