/*!
 * \file ParserCTP.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief CTP行情解析器头文件
 * \details 本文件定义了与上期所CTP行情接口对接的ParserCTP类
 *          实现了登录、订阅、行情数据解析等功能
 */
#pragma once
#include "../Includes/IParserApi.h"
#include "../Share/DLLHelper.hpp"
#include "../API/CTP6.3.15/ThostFtdcMdApi.h"
#include <map>

NS_WTP_BEGIN
class WTSTickData;
NS_WTP_END

USING_NS_WTP;

/**
 * @brief CTP行情解析器类
 * @details 实现了IParserApi接口和CThostFtdcMdSpi回调接口
 *          负责与Shanghai Futures Exchange(SFE)的CTP行情服务器建立连接
 *          订阅行情并将接收到的行情数据转换为内部定义的数据格式
 *          然后通过回调接口推送给上层应用
 */
class ParserCTP :	public IParserApi, public CThostFtdcMdSpi
{
public:
	/**
	 * @brief 构造函数
	 * @details 初始化ParserCTP对象的成员变量
	 *          将指针和状态设置为初始值
	 */
	ParserCTP();

	/**
	 * @brief 析构函数
	 * @details 清理ParserCTP对象资源
	 *          实际资源释放在release方法中进行
	 */
	virtual ~ParserCTP();

public:
	/**
	 * @brief 登录状态枚举
	 * @details 定义了行情解析器的不同登录状态
	 */
	enum LoginStatus
	{
		LS_NOTLOGIN,     ///< 未登录状态
		LS_LOGINING,     ///< 登录中状态
		LS_LOGINED       ///< 已登录状态
	};

//IQuoteParser 接口
public:
	/**
	 * @brief 初始化行情解析器
	 * @details 根据传入的配置参数设置行情解析器的各种配置
	 *          包括前置机地址、经纪商代码、用户名密码等
	 *          加载CTP行情接口动态库并获取创建函数
	 * @param config 配置参数集合
	 * @return bool 初始化是否成功
	 */
	virtual bool init(WTSVariant* config) override;

	/**
	 * @brief 释放行情解析器资源
	 * @details 释放行情解析器的所有资源
	 *          包括释放CTP行情接口实例
	 *          清空各种数据缓存
	 */
	virtual void release() override;

	/**
	 * @brief 连接行情服务器
	 * @details 初始化CTP行情接口并连接服务器
	 *          创建流文件目录、注册回调接口
	 *          注册前置机地址并初始化数据集合
	 * @return bool 连接是否成功
	 */
	virtual bool connect() override;

	/**
	 * @brief 断开行情服务器连接
	 * @details 释放CTP行情接口并清理相关资源
	 * @return bool 断开连接是否成功
	 */
	virtual bool disconnect() override;

	/**
	 * @brief 检查行情接口连接状态
	 * @details 判断当前行情接口是否处于已登录状态
	 * @return bool 如果已连接并登录返回true，否则返回false
	 */
	virtual bool isConnected() override;

	/**
	 * @brief 订阅行情
	 * @details 向CTP服务器发送订阅请求，订阅指定合约的行情
	 * @param vecSymbols 要订阅的合约代码集合
	 */
	virtual void subscribe(const CodeSet &vecSymbols) override;

	/**
	 * @brief 取消订阅行情
	 * @details 向CTP服务器发送取消订阅请求，取消指定合约的行情订阅
	 * @param vecSymbols 要取消订阅的合约代码集合
	 */
	virtual void unsubscribe(const CodeSet &vecSymbols) override;

	/**
	 * @brief 注册行情回调接口
	 * @details 注册用于接收行情数据和事件通知的回调接口
	 *          并从接口中获取基础数据管理器
	 * @param listener 行情回调接口实例
	 */
	virtual void registerSpi(IParserSpi* listener) override;


//CThostFtdcMdSpi 接口
public:
	/**
	 * @brief 响应错误回调
	 * @details 当CTP行情服务器返回错误响应时触发该回调
	 *          将错误信息转换为内部错误对象并输出日志
	 * @param pRspInfo 错误信息字段指针
	 * @param nRequestID 请求ID
	 * @param bIsLast 是否为最后一个响应
	 */
	virtual void OnRspError( CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast );

	/**
	 * @brief 前置机连接成功回调
	 * @details 当与CTP前置机建立连接成功时触发该回调
	 *          在该回调中发送登录请求
	 */
	virtual void OnFrontConnected();

	/**
	 * @brief 用户登录响应回调
	 * @details 当发送登录请求后收到的响应
	 *          如果登录成功，更新交易日并调用订阅方法
	 *          如果失败，输出错误日志
	 * @param pRspUserLogin 登录响应字段指针
	 * @param pRspInfo 响应信息字段指针
	 * @param nRequestID 请求ID
	 * @param bIsLast 是否为最后一个响应
	 */
	virtual void OnRspUserLogin( CThostFtdcRspUserLoginField *pRspUserLogin, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast );

	/**
	 * @brief 登出请求响应回调
	 * @details 当发送登出请求后收到的响应
	 *          清理现有的资源并更新状态
	 * @param pUserLogout 登出响应字段指针
	 * @param pRspInfo 响应信息字段指针
	 * @param nRequestID 请求ID
	 * @param bIsLast 是否为最后一个响应
	 */
	virtual void OnRspUserLogout(CThostFtdcUserLogoutField *pUserLogout, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	/**
	 * @brief 前置机断开连接回调
	 * @details 当与CTP前置机断开连接时触发该回调
	 *          输出断开原因并通过回调接口通知上层应用
	 * @param nReason 断开原因
	 */
	virtual void OnFrontDisconnected( int nReason );

	/**
	 * @brief 取消订阅行情响应回调
	 * @details 当发送取消订阅请求后收到的响应
	 *          处理取消订阅结果并输出日志
	 * @param pSpecificInstrument 合约信息字段指针
	 * @param pRspInfo 响应信息字段指针
	 * @param nRequestID 请求ID
	 * @param bIsLast 是否为最后一个响应
	 */
	virtual void OnRspUnSubMarketData( CThostFtdcSpecificInstrumentField *pSpecificInstrument, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast );

	/**
	 * @brief 市场行情推送回调
	 * @details 当收到订阅合约的新行情时触发该回调
	 *          将CTP行情数据转换为内部WTSTickData对象
	 *          然后通过回调接口推送给上层应用
	 * @param pDepthMarketData 深度市场行情数据字段指针
	 */
	virtual void OnRtnDepthMarketData( CThostFtdcDepthMarketDataField *pDepthMarketData );

	/**
	 * @brief 订阅行情响应回调
	 * @details 当发送订阅请求后收到的响应
	 *          处理订阅结果并输出日志
	 * @param pSpecificInstrument 合约信息字段指针
	 * @param pRspInfo 响应信息字段指针
	 * @param nRequestID 请求ID
	 * @param bIsLast 是否为最后一个响应
	 */
	virtual void OnRspSubMarketData( CThostFtdcSpecificInstrumentField *pSpecificInstrument, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast );

	/**
	 * @brief 心跳超时警告回调
	 * @details 当与CTP服务器心跳检测超时时触发该回调
	 *          输出警告日志并可能重新初始化连接
	 * @param nTimeLapse 超时时间间隔（秒）
	 */
	virtual void OnHeartBeatWarning( int nTimeLapse );

private:
	/**
	 * @brief 发送登录请求
	 * @details 创建并填充CTP登录请求字段
	 *          包括经纪商代码、用户名、密码等信息
	 *          然后向CTP服务器发送登录请求
	 */
	void ReqUserLogin();

	/**
	 * @brief 订阅品种行情
	 * @details 将缓存的订阅列表中的所有合约代码发送给CTP服务器
	 *          批量订阅这些合约的行情数据
	 */
	void DoSubscribeMD();

	/**
	 * @brief 检查错误信息
	 * @details 检查CTP响应信息字段中的错误代码是否非零
	 *          如果错误代码非零，则表示响应中包含错误
	 * @param pRspInfo CTP响应信息字段指针
	 * @return bool 如果包含错误返回true，否则返回false
	 */
	bool IsErrorRspInfo(CThostFtdcRspInfoField *pRspInfo);


private:
	uint32_t			m_uTradingDate;      ///< 交易日，格式为YYYYMMDD
	CThostFtdcMdApi*	m_pUserAPI;          ///< CTP行情API接口实例指针

	std::string			m_strFrontAddr;      ///< 前置机地址，格式为"tcp://ip:port"
	std::string			m_strBroker;         ///< 经纪商代码
	std::string			m_strUserID;         ///< 用户名/投资者代码
	std::string			m_strPassword;       ///< 用户密码
	std::string			m_strFlowDir;        ///< 流文件存储目录
	bool 				m_bLocaltime;         ///< 是否使用本地时间戳，true表示使用本地时间，false表示使用交易所时间

	CodeSet				m_filterSubs;         ///< 订阅合约代码集合

	int					m_iRequestID;         ///< 请求编号，用于标识不同的请求

	IParserSpi*			m_sink;              ///< 行情回调接口实例指针
	IBaseDataMgr*		m_pBaseDataMgr;       ///< 基础数据管理器实例指针

	DllHandle		m_hInstCTP;           ///< CTP动态库句柄
	typedef CThostFtdcMdApi* (*CTPCreator)(const char *, const bool, const bool);
	CTPCreator		m_funcCreator;        ///< CTP API创建函数指针
};

