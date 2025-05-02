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

	virtual void OnRtnInstrumentStatus(CThostFtdcInstrumentStatusField *pInstrumentStatus) override;

private:
	/*
	*	检查错误信息
	*/
	bool IsErrorRspInfo(CThostFtdcRspInfoField *pRspInfo);

	int wrapPriceType(WTSPriceType priceType, bool isCFFEX = false);
	int wrapDirectionType(WTSDirectionType dirType, WTSOffsetType offType);
	int wrapOffsetType(WTSOffsetType offType);
	int	wrapTimeCondition(WTSTimeCondition timeCond);
	int wrapActionFlag(WTSActionFlag actionFlag);

	WTSPriceType		wrapPriceType(TThostFtdcOrderPriceTypeType priceType);
	WTSDirectionType	wrapDirectionType(TThostFtdcDirectionType dirType, TThostFtdcOffsetFlagType offType);
	WTSDirectionType	wrapPosDirection(TThostFtdcPosiDirectionType dirType);
	WTSOffsetType		wrapOffsetType(TThostFtdcOffsetFlagType offType);
	WTSTimeCondition	wrapTimeCondition(TThostFtdcTimeConditionType timeCond);
	WTSOrderState		wrapOrderState(TThostFtdcOrderStatusType orderState);

	WTSOrderInfo*	makeOrderInfo(CThostFtdcOrderField* orderField);
	WTSEntrust*		makeEntrust(CThostFtdcInputOrderField *entrustField);
	WTSEntrustAction*	makeAction(CThostFtdcInputOrderActionField *entrustField);
	WTSError*		makeError(CThostFtdcRspInfoField* rspInfo, WTSErroCode ec = WEC_NONE);
	WTSTradeInfo*	makeTradeInfo(CThostFtdcTradeField *tradeField);

	void			generateEntrustID(char* buffer, uint32_t frontid, uint32_t sessionid, uint32_t orderRef);
	bool			extractEntrustID(const char* entrustid, uint32_t &frontid, uint32_t &sessionid, uint32_t &orderRef);

	uint32_t		genRequestID();

protected:
	std::string		m_strBroker;
	std::vector<std::string> m_strFront;

	std::string		m_strUser;
	std::string		m_strPass;

	std::string		m_strAppID;
	std::string		m_strAuthCode;

	std::string		m_strProdInfo;

	bool			m_bQuickStart;

	std::string		m_strTag;

	std::string		m_strSettleInfo;

	std::string		m_strUserName;
	std::string		m_strFlowDir;

	ITraderSpi*		m_sink;
	uint64_t		m_uLastQryTime;

	uint32_t					m_lDate;
	uint32_t					m_frontID;		//前置编号
	uint32_t					m_sessionID;	//会话编号
	std::atomic<uint32_t>		m_orderRef;		//报单引用

	WrapperState				m_wrapperState;

	CThostFtdcTraderApi*		m_pUserAPI;
	std::atomic<uint32_t>		m_iRequestID;

	typedef WTSHashMap<std::string> PositionMap;
	PositionMap*				m_mapPosition;
	WTSArray*					m_ayTrades;
	WTSArray*					m_ayOrders;
	WTSArray*					m_ayPosDetail;

	IBaseDataMgr*				m_bdMgr;

	typedef std::queue<CommonExecuter>	QueryQue;
	QueryQue				m_queQuery;
	bool					m_bInQuery;
	StdUniqueMutex			m_mtxQuery;
	uint64_t				m_lastQryTime;

	bool					m_bStopped;
	StdThreadPtr			m_thrdWorker;

	std::string		m_strModule;
	DllHandle		m_hInstCTP;
	typedef CThostFtdcTraderApi* (*CTPCreator)(const char *);
	CTPCreator		m_funcCreator;

	//委托单标记缓存器
	WtKVCache		m_eidCache;
	//订单标记缓存器
	WtKVCache		m_oidCache;
};

