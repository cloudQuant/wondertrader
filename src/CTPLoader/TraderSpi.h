/*!
 * \file TraderSpi.h
 * \brief CTP交易接口回调类定义
 * 
 * 该文件定义了CTP交易接口的回调实现类，用于获取合约信息
 * 并将合约信息转换为WonderTrader框架所需的格式
 * 使用CTP API v6.3.15版本
 * 
 * \author Wesley
 */

#pragma once
//v6.3.15
#include "../API/CTP6.3.15/ThostFtdcTraderApi.h"

/**
 * \brief CTP交易接口回调实现类
 * 
 * 该类继承自CThostFtdcTraderSpi，实现了CTP交易接口的回调函数
 * 主要用于登录交易系统、查询合约信息并将其转换为WonderTrader框架格式
 */
class CTraderSpi : public CThostFtdcTraderSpi
{
public:
	/**
	 * \brief 前置连接回调
	 * 
	 * 当客户端与交易后台建立起通信连接时（还未登录前）调用
	 * 在该方法中进行认证请求
	 */
	virtual void OnFrontConnected();

	/**
	 * \brief 认证响应回调
	 * 
	 * 收到认证响应后调用，如果认证成功则进行登录
	 * 
	 * \param pRspAuthenticateField 认证响应数据
	 * \param pRspInfo 错误信息
	 * \param nRequestID 请求ID
	 * \param bIsLast 是否为最后一条消息
	 */
	virtual void OnRspAuthenticate(CThostFtdcRspAuthenticateField *pRspAuthenticateField, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	/**
	 * \brief 登录请求响应
	 * 
	 * 收到登录响应后调用，如果登录成功则查询合约信息
	 * 
	 * \param pRspUserLogin 登录响应数据
	 * \param pRspInfo 错误信息
	 * \param nRequestID 请求ID
	 * \param bIsLast 是否为最后一条消息
	 */
	virtual void OnRspUserLogin(CThostFtdcRspUserLoginField *pRspUserLogin,	CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);
	
	/**
	 * \brief 查询合约响应
	 * 
	 * 收到合约查询响应后调用，将合约信息转换为WonderTrader框架格式
	 * 并保存到指定文件
	 * 
	 * \param pInstrument 合约信息
	 * \param pRspInfo 错误信息
	 * \param nRequestID 请求ID
	 * \param bIsLast 是否为最后一条消息
	 */
	virtual void OnRspQryInstrument(CThostFtdcInstrumentField *pInstrument, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	/**
	 * \brief 错误应答
	 * 
	 * 收到错误应答时调用，输出错误信息
	 * 
	 * \param pRspInfo 错误信息
	 * \param nRequestID 请求ID
	 * \param bIsLast 是否为最后一条消息
	 */
	virtual void OnRspError(CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);
	
	/**
	 * \brief 前置断开连接回调
	 * 
	 * 当客户端与交易后台通信连接断开时调用
	 * 当发生这个情况后，API会自动重新连接，客户端可不做处理
	 * 
	 * \param nReason 断开原因
	 */
	virtual void OnFrontDisconnected(int nReason);

private:
	/**
	 * \brief 发送认证请求
	 * 
	 * 在连接建立后发送认证请求，验证用户身份
	 */
	void ReqAuth();

	/**
	 * \brief 发送用户登录请求
	 * 
	 * 在认证成功后发送登录请求
	 */
	void ReqUserLogin();

	/**
	 * \brief 发送查询合约请求
	 * 
	 * 在登录成功后发送查询合约请求，获取所有合约信息
	 */
	void ReqQryInstrument();

	/**
	 * \brief 检查响应是否有错误
	 * 
	 * 检查CTP响应中是否包含错误信息
	 * 
	 * \param pRspInfo 响应信息
	 * \return 如果有错误返回true，否则返回false
	 */
	bool IsErrorRspInfo(CThostFtdcRspInfoField *pRspInfo);

	/**
	 * \brief 将合约信息转储为JSON格式
	 * 
	 * 将已获取的合约和品种信息转换为JSON格式并保存到文件
	 */
	void DumpToJson();

	/**
	 * \brief 从本地JSON文件加载合约信息
	 * 
	 * 从指定的JSON文件中加载合约和品种信息
	 */
	void LoadFromJson();

protected:
	//! 交易日期
	int	m_lTradingDate;
	//! 请求计数器
	int m_ReqCount;
};