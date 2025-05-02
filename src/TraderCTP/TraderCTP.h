/*!
 * \file TraderCTP.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief CTP交易接口定义
 * \details 本文件定义了与上期所CTP交易接口对接的TraderCTP类
 *          实现了ITraderApi和CThostFtdcTraderSpi接口
 *          提供了实盘交易的操作功能，包括委托下单、撤单、查询等
 */
#pragma once

#include <string>
#include <queue>
#include <stdint.h>

#include "../Includes/WTSTypes.h"
#include "../Includes/ITraderApi.h"
#include "../Includes/WTSCollection.hpp"

#include "../API/CTP6.3.15/ThostFtdcTraderApi.h"

#include "../Share/StdUtils.hpp"
#include "../Share/DLLHelper.hpp"
#include "../Share/WtKVCache.hpp"

USING_NS_WTP;

/**
 * @brief CTP交易接口实现类
 * @details 此类拥有与上期所CTP交易接口的完整对接
 *          继承ITraderApi实现交易接口标准方法
 *          同时继承CThostFtdcTraderSpi实现CTP回调接口
 *          提供下单、撤单、查询账户、持仓、委托、成交等功能
 */
class TraderCTP : public ITraderApi, public CThostFtdcTraderSpi
{
public:
	TraderCTP();
	virtual ~TraderCTP();

public:
	/**
	 * @brief 交易接口状态枚举
	 * @details 用于记录CTP交易接口的当前状态
	 *          包括登录状态、结算单确认状态等
	 */
	typedef enum
	{
		WS_NOTLOGIN,		//未登录
		WS_LOGINING,		//正在登录
		WS_LOGINED,			//已登录
		WS_LOGINFAILED,		//登录失败
		WS_CONFIRM_QRYED,  //结算单已查询
		WS_CONFIRMED,		//已确认
		WS_ALLREADY			//全部就绪
	} WrapperState;


private:
	/**
	 * @brief 发送结算单确认
	 * @details 向服务器发送结算单确认请求
	 * @return int 请求结果代码，0表示成功
	 */
	int confirm();

	/**
	 * @brief 查询结算单确认状态
	 * @details 向服务器查询结算单确认状态
	 * @return int 请求结果代码，0表示成功
	 */
	int queryConfirm();

	/**
	 * @brief 发送用户验证请求
	 * @details 向服务器发送用户验证请求，需要使用AppID和授权码
	 * @return int 请求结果代码，0表示成功
	 */
	int authenticate();

	/**
	 * @brief 执行登录
	 * @details 向服务器发送登录请求
	 * @return int 登录结果代码，0表示成功
	 */
	int doLogin();

	//////////////////////////////////////////////////////////////////////////
	//ITraderApi接口
public:
	/**
	 * @brief 初始化交易接口
	 * @details 根据给定参数初始化CTP交易接口
	 *          包括设置服务器、用户信息、流通目录等
	 * 
	 * @param params 初始化参数
	 * @return bool 初始化是否成功
	 */
	virtual bool init(WTSVariant* params) override;

	/**
	 * @brief 释放交易接口资源
	 * @details 清理并释放交易接口相关资源
	 *          包括断开连接、释放内存等
	 */
	virtual void release() override;

	/**
	 * @brief 注册交易接口回调
	 * @details 注册交易接口的事件监听器
	 *          用于接收交易相关的回调事件
	 * 
	 * @param listener 交易接口事件监听器
	 */
	virtual void registerSpi(ITraderSpi *listener) override;

	/**
	 * @brief 生成委托编号
	 * @details 生成唯一的委托编号，用于下单标识
	 * 
	 * @param buffer 存储编号的缓冲区
	 * @param length 缓冲区长度
	 * @return bool 生成是否成功
	 */
	virtual bool makeEntrustID(char* buffer, int length) override;

	/**
	 * @brief 连接交易服务器
	 * @details 连接交易服务器并初始化交易环境
	 */
	virtual void connect() override;

	/**
	 * @brief 断开交易服务器连接
	 * @details 断开与交易服务器的连接
	 */
	virtual void disconnect() override;

	/**
	 * @brief 检查是否已连接
	 * @details 检查交易接口是否已连接到交易服务器
	 * @return bool 是否已连接，true表示已连接
	 */
	virtual bool isConnected() override;

	/**
	 * @brief 登录交易账户
	 * @details 使用给定的用户名和密码登录交易账户
	 * 
	 * @param user 用户名
	 * @param pass 密码
	 * @param productInfo 产品信息
	 * @return int 登录结果代码，0表示成功
	 */
	virtual int login(const char* user, const char* pass, const char* productInfo) override;

	/**
	 * @brief 登出交易账户
	 * @details 从交易系统登出当前账户
	 * @return int 登出结果代码，0表示成功
	 */
	virtual int logout() override;

	/**
	 * @brief 委托下单
	 * @details 向交易所发送委托下单请求
	 * 
	 * @param eutrust 委托下单请求对象，包含下单参数
	 * @return int 下单结果代码，0表示成功
	 */
	virtual int orderInsert(WTSEntrust* eutrust) override;

	/**
	 * @brief 委托撤单
	 * @details 向交易所发送委托撤单请求
	 * 
	 * @param action 委托撤单请求对象，包含要撤销的委托信息
	 * @return int 撤单结果代码，0表示成功
	 */
	virtual int orderAction(WTSEntrustAction* action) override;

	/**
	 * @brief 查询资金账户
	 * @details 向交易所发送资金账户查询请求
	 *          查询结果通过OnRspQryTradingAccount回调返回
	 * @return int 查询结果代码，0表示成功
	 */
	virtual int queryAccount() override;

	/**
	 * @brief 查询持仓信息
	 * @details 向交易所发送持仓查询请求
	 *          查询结果通过OnRspQryInvestorPosition回调返回
	 * @return int 查询结果代码，0表示成功
	 */
	virtual int queryPositions() override;

	/**
	 * @brief 查询委托单
	 * @details 向交易所发送委托单查询请求
	 *          查询结果通过OnRspQryOrder回调返回
	 * @return int 查询结果代码，0表示成功
	 */
	virtual int queryOrders() override;

	/**
	 * @brief 查询成交信息
	 * @details 向交易所发送成交信息查询请求
	 *          查询结果通过OnRspQryTrade回调返回
	 * @return int 查询结果代码，0表示成功
	 */
	virtual int queryTrades() override;

	/**
	 * @brief 查询结算单信息
	 * @details 向交易所发送指定交易日的结算单查询请求
	 *          查询结果通过OnRspQrySettlementInfo回调返回
	 * 
	 * @param uDate 结算日期，格式YYYYMMDD
	 * @return int 查询结果代码，0表示成功
	 */
	virtual int querySettlement(uint32_t uDate) override;


	//////////////////////////////////////////////////////////////////////////
	//CTP交易接口实现
public:
	/**
	 * @brief 前置机连接成功回调
	 * @details 当与CTP前置机连接成功时触发此回调
	 *          在此回调中通常进行登录、认证等操作
	 */
	virtual void OnFrontConnected() override;

	/**
	 * @brief 前置机断开连接回调
	 * @details 当与CTP前置机连接断开时触发此回调
	 * 
	 * @param nReason 断开原因代码
	 */
	virtual void OnFrontDisconnected(int nReason) override;

	/**
	 * @brief 心跳超时警告回调
	 * @details 当长时间没收到心跳包时触发此回调
	 * 
	 * @param nTimeLapse 超时时间（秒）
	 */
	virtual void OnHeartBeatWarning(int nTimeLapse) override;

	/**
	 * @brief 认证应答回调
	 * @details 用户认证请求的响应回调，展示认证成功或失败
	 * 
	 * @param pRspAuthenticateField 认证应答字段
	 * @param pRspInfo 错误信息
	 * @param nRequestID 请求ID
	 * @param bIsLast 是否为最后一条回复
	 */
	virtual void OnRspAuthenticate(CThostFtdcRspAuthenticateField *pRspAuthenticateField, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	/**
	 * @brief 登录应答回调
	 * @details 用户登录请求的响应回调，展示登录成功或失败
	 *          登录成功后能获取交易日、前置ID、会话ID等信息
	 * 
	 * @param pRspUserLogin 登录应答字段
	 * @param pRspInfo 错误信息
	 * @param nRequestID 请求ID
	 * @param bIsLast 是否为最后一条回复
	 */
	virtual void OnRspUserLogin(CThostFtdcRspUserLoginField *pRspUserLogin, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	/**
	 * @brief 登出应答回调
	 * @details 用户登出请求的响应回调，展示登出成功或失败
	 * 
	 * @param pUserLogout 登出信息
	 * @param pRspInfo 错误信息
	 * @param nRequestID 请求ID
	 * @param bIsLast 是否为最后一条回复
	 */
	virtual void OnRspUserLogout(CThostFtdcUserLogoutField *pUserLogout, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	/**
	 * @brief 结算单确认应答回调
	 * @details 确认结算单请求的响应回调，展示确认成功或失败
	 *          交易日开始前需要确认结算单
	 * 
	 * @param pSettlementInfoConfirm 结算单确认信息
	 * @param pRspInfo 错误信息
	 * @param nRequestID 请求ID
	 * @param bIsLast 是否为最后一条回复
	 */
	virtual void OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField *pSettlementInfoConfirm, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	/**
	 * @brief 查询结算单确认应答回调
	 * @details 查询结算单确认状态的响应回调
	 *          用于查询用户是否已确认结算单
	 * 
	 * @param pSettlementInfoConfirm 结算单确认信息
	 * @param pRspInfo 错误信息
	 * @param nRequestID 请求ID
	 * @param bIsLast 是否为最后一条回复
	 */
	virtual void OnRspQrySettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField *pSettlementInfoConfirm, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	/**
	 * @brief 资金账户查询应答回调
	 * @details 查询资金账户信息的响应回调
	 *          返回账户的资金信息，包括可用资金、冻结资金等
	 * 
	 * @param pTradingAccount 资金账户信息
	 * @param pRspInfo 错误信息
	 * @param nRequestID 请求ID
	 * @param bIsLast 是否为最后一条回复
	 */
	virtual void OnRspQryTradingAccount(CThostFtdcTradingAccountField *pTradingAccount, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	/**
	 * @brief 委托下单应答回调
	 * @details 委托下单请求的响应回调
	 *          如果下单请求失败，此处返回错误信息
	 *          注意：此回调仅在下单请求出错时返回错误，正常情况下通过OnRtnOrder返回委托状态
	 * 
	 * @param pInputOrder 委托输入信息
	 * @param pRspInfo 错误信息
	 * @param nRequestID 请求ID
	 * @param bIsLast 是否为最后一条回复
	 */
	virtual void OnRspOrderInsert(CThostFtdcInputOrderField *pInputOrder, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	/**
	 * @brief 委托撤单应答回调
	 * @details 委托撤单请求的响应回调
	 *          如果撤单请求失败，此处返回错误信息
	 *          注意：此回调仅在撤单请求出错时返回错误，正常情况下通过OnRtnOrder返回委托状态变化
	 * 
	 * @param pInputOrderAction 撤单操作信息
	 * @param pRspInfo 错误信息
	 * @param nRequestID 请求ID
	 * @param bIsLast 是否为最后一条回复
	 */
	virtual void OnRspOrderAction(CThostFtdcInputOrderActionField *pInputOrderAction, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	/**
	 * @brief 投资者持仓查询应答回调
	 * @details 查询投资者持仓信息的响应回调
	 *          返回号码分开方向的持仓明细信息
	 * 
	 * @param pInvestorPosition 投资者持仓信息
	 * @param pRspInfo 错误信息
	 * @param nRequestID 请求ID
	 * @param bIsLast 是否为最后一条回复
	 */
	virtual void OnRspQryInvestorPosition(CThostFtdcInvestorPositionField *pInvestorPosition, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	/**
	 * @brief 结算单信息查询应答回调
	 * @details 查询结算单信息的响应回调
	 *          返回指定交易日的结算单内容，可能分多个包返回
	 * 
	 * @param pSettlementInfo 结算单信息
	 * @param pRspInfo 错误信息
	 * @param nRequestID 请求ID
	 * @param bIsLast 是否为最后一条回复
	 */
	virtual void OnRspQrySettlementInfo(CThostFtdcSettlementInfoField *pSettlementInfo, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	/**
	 * @brief 成交信息查询应答回调
	 * @details 查询成交信息的响应回调
	 *          返回投资者当日成交明细
	 * 
	 * @param pTrade 成交信息
	 * @param pRspInfo 错误信息
	 * @param nRequestID 请求ID
	 * @param bIsLast 是否为最后一条回复
	 */
	virtual void OnRspQryTrade(CThostFtdcTradeField *pTrade, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	/**
	 * @brief 委托单查询应答回调
	 * @details 查询委托单的响应回调
	 *          返回当日委托单状态信息
	 * 
	 * @param pOrder 委托单信息
	 * @param pRspInfo 错误信息
	 * @param nRequestID 请求ID
	 * @param bIsLast 是否为最后一条回复
	 */
	virtual void OnRspQryOrder(CThostFtdcOrderField *pOrder, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	/**
	 * @brief 错误应答回调
	 * @details 当出现错误时触发此回调
	 *          可通过错误码和错误信息判断出错原因
	 * 
	 * @param pRspInfo 错误信息
	 * @param nRequestID 请求ID
	 * @param bIsLast 是否为最后一条回复
	 */
	virtual void OnRspError(CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

	/**
	 * @brief 委托单状态通知回调
	 * @details 当委托单状态发生变化时触发此回调
	 *          包括收到报单回报、撤单成功等状态变化
	 * 
	 * @param pOrder 委托单信息
	 */
	virtual void OnRtnOrder(CThostFtdcOrderField *pOrder) override;

	/**
	 * @brief 成交通知回调
	 * @details 当收到成交回报时触发此回调
	 *          包含成交价格、成交数量等信息
	 * 
	 * @param pTrade 成交信息
	 */
	virtual void OnRtnTrade(CThostFtdcTradeField *pTrade) override;

	/**
	 * @brief 报单录入错误回报
	 * @details 当报单在交易所端入库过程中出错时触发此回调
	 *          与普通下单回报的区别在于只会在错误情况下触发
	 * 
	 * @param pInputOrder 委托输入信息
	 * @param pRspInfo 错误信息
	 */
	virtual void OnErrRtnOrderInsert(CThostFtdcInputOrderField *pInputOrder, CThostFtdcRspInfoField *pRspInfo) override;

	/**
	 * @brief 合约状态通知回调
	 * @details 当合约交易状态发生变化时触发此回调
	 *          包括开盘、收盘、集合竞价等状态变化
	 * 
	 * @param pInstrumentStatus 合约状态信息
	 */
	virtual void OnRtnInstrumentStatus(CThostFtdcInstrumentStatusField *pInstrumentStatus) override;

private:
	/**
	 * @brief 检查错误信息
	 * @details 检查CTP响应信息中是否包含错误
	 *          当错误码不为0时表示有错误
	 * 
	 * @param pRspInfo CTP响应信息字段
	 * @return bool 是否存在错误，true表示有错误
	 */
	bool IsErrorRspInfo(CThostFtdcRspInfoField *pRspInfo);

	/**
	 * @brief 将WonderTrader价格类型转为CTP价格类型
	 * @details 将内部价格类型转换为CTP交易接口的价格类型
	 * 
	 * @param priceType WonderTrader内部价格类型
	 * @param isCFFEX 是否为中金所合约，默认为false
	 * @return int CTP价格类型代码
	 */
	int wrapPriceType(WTSPriceType priceType, bool isCFFEX = false);
	/**
	 * @brief 将WonderTrader交易方向转为CTP方向类型
	 * @details 将内部交易方向和开平类型转换为CTP交易接口的方向类型
	 *          CTP中方向和开平标志是分开的，而在WonderTrader中是组合的
	 * 
	 * @param dirType WonderTrader内部交易方向类型
	 * @param offType WonderTrader内部开平类型
	 * @return int CTP方向类型代码
	 */
	int wrapDirectionType(WTSDirectionType dirType, WTSOffsetType offType);
	/**
	 * @brief 将WonderTrader开平类型转为CTP开平标志
	 * @details 将内部开平类型转换为CTP交易接口的开平标志
	 *          包括开仓、平仓、平今等
	 * 
	 * @param offType WonderTrader内部开平类型
	 * @return int CTP开平标志代码
	 */
	int wrapOffsetType(WTSOffsetType offType);
	/**
	 * @brief 将WonderTrader时间条件转为CTP时间条件
	 * @details 将内部时间条件类型转换为CTP交易接口的时间条件
	 *          包括当日有效、立即执行等
	 * 
	 * @param timeCond WonderTrader内部时间条件类型
	 * @return int CTP时间条件代码
	 */
	int	wrapTimeCondition(WTSTimeCondition timeCond);

	/**
	 * @brief 将WonderTrader操作标志转为CTP操作标志
	 * @details 将内部操作标志类型转换为CTP交易接口的操作标志
	 *          主要用于撤单操作
	 * 
	 * @param actionFlag WonderTrader内部操作标志类型
	 * @return int CTP操作标志代码
	 */
	int wrapActionFlag(WTSActionFlag actionFlag);

	/**
	 * @brief 将CTP价格类型转为WonderTrader价格类型
	 * @details 将CTP交易接口的价格类型转换为内部价格类型
	 * 
	 * @param priceType CTP的价格类型
	 * @return WTSPriceType WonderTrader内部价格类型
	 */
	WTSPriceType		wrapPriceType(TThostFtdcOrderPriceTypeType priceType);

	/**
	 * @brief 将CTP方向和开平标志转为WonderTrader交易方向
	 * @details 将CTP交易接口的方向和开平标志转换为内部交易方向
	 *          CTP中方向和开平标志是分开的，而在WonderTrader中是组合的
	 * 
	 * @param dirType CTP的方向类型
	 * @param offType CTP的开平标志
	 * @return WTSDirectionType WonderTrader内部交易方向类型
	 */
	WTSDirectionType	wrapDirectionType(TThostFtdcDirectionType dirType, TThostFtdcOffsetFlagType offType);

	/**
	 * @brief 将CTP持仓方向转为WonderTrader方向类型
	 * @details 将CTP交易接口的持仓方向转换为内部方向类型
	 * 
	 * @param dirType CTP的持仓方向类型
	 * @return WTSDirectionType WonderTrader内部方向类型
	 */
	WTSDirectionType	wrapPosDirection(TThostFtdcPosiDirectionType dirType);

	/**
	 * @brief 将CTP开平标志转为WonderTrader开平类型
	 * @details 将CTP交易接口的开平标志转换为内部开平类型
	 * 
	 * @param offType CTP的开平标志
	 * @return WTSOffsetType WonderTrader内部开平类型
	 */
	WTSOffsetType		wrapOffsetType(TThostFtdcOffsetFlagType offType);

	/**
	 * @brief 将CTP时间条件转为WonderTrader时间条件
	 * @details 将CTP交易接口的时间条件转换为内部时间条件类型
	 * 
	 * @param timeCond CTP的时间条件
	 * @return WTSTimeCondition WonderTrader内部时间条件类型
	 */
	WTSTimeCondition	wrapTimeCondition(TThostFtdcTimeConditionType timeCond);

	/**
	 * @brief 将CTP委托状态转为WonderTrader委托状态
	 * @details 将CTP交易接口的委托状态转换为内部委托状态类型
	 * 
	 * @param orderState CTP的委托状态
	 * @return WTSOrderState WonderTrader内部委托状态
	 */
	WTSOrderState		wrapOrderState(TThostFtdcOrderStatusType orderState);

	/**
	 * @brief 将CTP委托对象转换为WonderTrader委托信息对象
	 * @details 将CTP交易接口的委托对象转换为内部委托信息对象
	 * 
	 * @param orderField CTP的委托对象
	 * @return WTSOrderInfo* WonderTrader内部委托信息对象指针
	 */
	WTSOrderInfo*	makeOrderInfo(CThostFtdcOrderField* orderField);

	/**
	 * @brief 将CTP委托输入对象转换为WonderTrader委托对象
	 * @details 将CTP交易接口的委托输入对象转换为内部委托对象
	 * 
	 * @param entrustField CTP的委托输入对象
	 * @return WTSEntrust* WonderTrader内部委托对象指针
	 */
	WTSEntrust*		makeEntrust(CThostFtdcInputOrderField *entrustField);

	/**
	 * @brief 将CTP委托操作输入对象转换为WonderTrader委托操作对象
	 * @details 将CTP交易接口的委托操作输入对象转换为内部委托操作对象
	 * 
	 * @param entrustField CTP的委托操作输入对象
	 * @return WTSEntrustAction* WonderTrader内部委托操作对象指针
	 */
	WTSEntrustAction*	makeAction(CThostFtdcInputOrderActionField *entrustField);
	/**
	 * @brief 根据CTP响应信息创建错误对象
	 * @details 将CTP交易接口的响应错误信息转换为内部错误对象
	 * 
	 * @param rspInfo CTP的响应信息对象
	 * @param ec 错误代码，默认为WEC_NONE
	 * @return WTSError* WonderTrader内部错误对象指针
	 */
	WTSError*		makeError(CThostFtdcRspInfoField* rspInfo, WTSErroCode ec = WEC_NONE);

	/**
	 * @brief 将CTP成交对象转换为WonderTrader成交信息对象
	 * @details 将CTP交易接口的成交对象转换为内部成交信息对象
	 * 
	 * @param tradeField CTP的成交对象
	 * @return WTSTradeInfo* WonderTrader内部成交信息对象指针
	 */
	WTSTradeInfo*	makeTradeInfo(CThostFtdcTradeField *tradeField);

	/**
	 * @brief 生成委托单编号
	 * @details 根据前置机ID、会话ID和报单引用生成唯一的委托单编号
	 * 
	 * @param buffer 存储生成编号的缓冲区
	 * @param frontid 前置机ID
	 * @param sessionid 会话ID
	 * @param orderRef 报单引用
	 */
	void			generateEntrustID(char* buffer, uint32_t frontid, uint32_t sessionid, uint32_t orderRef);

	/**
	 * @brief 解析委托单编号
	 * @details 从委托单编号中解析出前置机ID、会话ID和报单引用
	 * 
	 * @param entrustid 委托单编号
	 * @param frontid 输出参数，解析出的前置机ID
	 * @param sessionid 输出参数，解析出的会话ID
	 * @param orderRef 输出参数，解析出的报单引用
	 * @return bool 解析是否成功
	 */
	bool			extractEntrustID(const char* entrustid, uint32_t &frontid, uint32_t &sessionid, uint32_t &orderRef);

	/**
	 * @brief 生成请求ID
	 * @details 生成唯一的请求ID，用于识别不同的交易请求
	 *          每次调用都会自增，确保请求ID的唯一性
	 * 
	 * @return uint32_t 新生成的请求ID
	 */
	uint32_t		genRequestID();

protected:
	/// @brief 期货公司代码
	std::string		m_strBroker;
	/// @brief 交易前置机地址列表
	std::vector<std::string> m_strFront;

	/// @brief 交易账户用户名
	std::string		m_strUser;
	/// @brief 交易账户密码
	std::string		m_strPass;

	/// @brief CTP应用程ID，用于用户认证
	std::string		m_strAppID;
	/// @brief CTP应用授权码，用于用户认证
	std::string		m_strAuthCode;

	/// @brief 产品信息
	std::string		m_strProdInfo;

	/// @brief 是否使用快速启动模式，跳过结算单确认
	bool			m_bQuickStart;

	/// @brief 交易接口标签，用于识别当前交易接口
	std::string		m_strTag;

	/// @brief 结算单信息
	std::string		m_strSettleInfo;

	/// @brief 用户名称
	std::string		m_strUserName;
	/// @brief 流水等数据存储目录
	std::string		m_strFlowDir;

	/// @brief 交易接口回调接收器
	ITraderSpi*		m_sink;
	/// @brief 最后一次查询时间
	uint64_t		m_uLastQryTime;

	/// @brief 交易日期
	uint32_t					m_lDate;
	/// @brief 前置机编号，由CTP服务器分配
	uint32_t					m_frontID;		//前置编号
	/// @brief 会话编号，由CTP服务器分配
	uint32_t					m_sessionID;	//会话编号
	/// @brief 报单引用，自增值，用于唯一标识委托
	std::atomic<uint32_t>		m_orderRef;		//报单引用

	/// @brief 交易接口状态
	WrapperState				m_wrapperState;

	/// @brief CTP交易接口实例
	CThostFtdcTraderApi*		m_pUserAPI;
	/// @brief 请求ID，自增值，用于唯一标识请求
	std::atomic<uint32_t>		m_iRequestID;

	/// @brief 持仓映射类型定义，记录合约及其持仓情况
	typedef WTSHashMap<std::string> PositionMap;
	/// @brief 持仓映射表，用于存储所有合约的持仓情况
	PositionMap*				m_mapPosition;
	/// @brief 成交列表，存储所有成交记录
	WTSArray*				m_ayTrades;
	/// @brief 委托列表，存储所有委托记录
	WTSArray*				m_ayOrders;
	/// @brief 持仓明细列表，存储持仓明细信息
	WTSArray*				m_ayPosDetail;

	/// @brief 基础数据管理器，用于获取合约信息等
	IBaseDataMgr*				m_bdMgr;

	/// @brief 查询队列类型定义，用于处理批量查询
	typedef std::queue<CommonExecuter>	QueryQue;
	/// @brief 查询队列，存储待处理的查询请求
	QueryQue				m_queQuery;
	/// @brief 是否正在查询中的标志
	bool					m_bInQuery;
	/// @brief 查询队列互斥锁，保护查询队列的线程安全
	StdUniqueMutex			m_mtxQuery;
	/// @brief 最后一次查询时间
	uint64_t				m_lastQryTime;

	/// @brief 是否停止的标志
	bool					m_bStopped;
	/// @brief 工作线程，用于异步处理查询等操作
	StdThreadPtr			m_thrdWorker;

	/// @brief CTP模块路径
	std::string		m_strModule;
	/// @brief CTP动态库句柄
	DllHandle		m_hInstCTP;
	/// @brief CTP创建器函数指针类型
	typedef CThostFtdcTraderApi* (*CTPCreator)(const char *);
	/// @brief CTP创建器函数指针
	CTPCreator		m_funcCreator;

	/// @brief 委托单标记缓存器，用于缓存委托单编号与其他信息的关联
	WtKVCache		m_eidCache;
	/// @brief 订单标记缓存器，用于缓存订单编号与其他信息的关联
	WtKVCache		m_oidCache;
};
