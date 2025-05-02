/*!
 * \file TraderCTP.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief CTP交易接口实现
 * \details 本文件实现了与上期所CTP交易接口对接的TraderCTP类
 *          实现了连接、认证、登录、下单、撤单、查询等功能
 */
#include "TraderCTP.h"

#include "../Includes/WTSError.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/WTSTradeDef.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/IBaseDataMgr.h"

#include "../Share/decimal.h"
#include "../Share/ModuleHelper.hpp"

#include <boost/filesystem.hpp>

//By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"
/**
 * @brief 格式化写入日志
 * @details 将格式化的日志信息写入到交易接口回调中
 * @tparam Args 格式化参数类型
 * @param sink 交易接口回调对象
 * @param ll 日志级别
 * @param format 格式化字符串
 * @param args 参数列表
 */
template<typename... Args>
inline void write_log(ITraderSpi* sink, WTSLogLevel ll, const char* format, const Args&... args)
{
	if (sink == NULL)
		return;

	const char* buffer = fmtutil::format(format, args...);

	sink->handleTraderLog(ll, buffer);
}

/**
 * @brief 将时间格式字符串转换为无格式整数
 * @details 将含有分隔符“:”的时间格式字符串转换为整数
 *          例如将“09:30:15”转换为“93015”
 * @param strTime 要转换的时间字符串
 * @return uint32_t 转换后的整数时间
 */
uint32_t strToTime(const char* strTime)
{
	std::string str;
	const char *pos = strTime;
	while (strlen(pos) > 0)
	{
		if (pos[0] != ':')
		{
			str.append(pos, 1);
		}
		pos++;
	}

	return strtoul(str.c_str(), NULL, 10);
}

/**
 * @brief DLL导出函数声明
 * @details 声明供动态链接库调用的C接口函数
 */
extern "C"
{
	/**
	 * @brief 创建交易接口实例
	 * @details 创建一个TraderCTP实例并返回接口指针
	 * @return ITraderApi* 新创建的交易接口实例
	 */
	EXPORT_FLAG ITraderApi* createTrader()
	{
		TraderCTP *instance = new TraderCTP();
		return instance;
	}

	/**
	 * @brief 删除交易接口实例
	 * @details 删除指定的交易接口实例并将指针置空
	 * @param trader 要删除的交易接口实例指针引用
	 */
	EXPORT_FLAG void deleteTrader(ITraderApi* &trader)
	{
		if (NULL != trader)
		{
			delete trader;
			trader = NULL;
		}
	}
}

/**
 * @brief 构造函数
 * @details 初始化TraderCTP对象的成员变量
 *          将指针和状态设置为初始值
 */
TraderCTP::TraderCTP()
	: m_pUserAPI(NULL)
	, m_mapPosition(NULL)
	, m_ayOrders(NULL)
	, m_ayTrades(NULL)
	, m_ayPosDetail(NULL)
	, m_wrapperState(WS_NOTLOGIN)
	, m_uLastQryTime(0)
	, m_iRequestID(0)
	, m_bQuickStart(false)
	, m_bInQuery(false)
	, m_bStopped(false)
	, m_lastQryTime(0)
{
}

/**
 * @brief 析构函数
 * @details 清理TraderCTP对象资源
 *          实际资源释放在release方法中进行
 */
TraderCTP::~TraderCTP()
{
}

/**
 * @brief 初始化交易接口
 * @details 根据传入的参数设置交易接口的各种配置
 *          包括前置机地址、经纪商代码、用户名密码、授权信息等
 *          加载CTP交易接口动态库并获取创建函数
 * 
 * @param params 配置参数集合
 * @return bool 初始化是否成功
 */
bool TraderCTP::init(WTSVariant* params)
{
	auto fontItem = params->get("front");
	if (fontItem)
	{
		if (fontItem->type() == WTSVariant::VT_String)
		{
			m_strFront.push_back(fontItem->asCString());
		}
		else if (fontItem->type() == WTSVariant::VT_Array)
		{
			for (uint32_t i = 0; i < fontItem->size(); i++)
			{
				m_strFront.push_back(fontItem->get(i)->asCString());
			}
		}
	}
	m_strBroker = params->get("broker")->asCString();
	m_strUser = params->get("user")->asCString();
	m_strPass = params->get("pass")->asCString();

	m_strAppID = params->getCString("appid");
	m_strAuthCode = params->getCString("authcode");
	m_strFlowDir = params->getCString("flowdir");

	if (m_strFlowDir.empty())
		m_strFlowDir = "CTPTDFlow";

	m_strFlowDir = StrUtil::standardisePath(m_strFlowDir);

	std::string module = params->getCString("ctpmodule");
	if (module.empty())
		module = "thosttraderapi_se";

	m_strModule = getBinDir() + DLLHelper::wrap_module(module.c_str(), "");

	m_hInstCTP = DLLHelper::load_library(m_strModule.c_str());
#ifdef _WIN32
#	ifdef _WIN64
	const char* creatorName = "?CreateFtdcTraderApi@CThostFtdcTraderApi@@SAPEAV1@PEBD@Z";
#	else
	const char* creatorName = "?CreateFtdcTraderApi@CThostFtdcTraderApi@@SAPAV1@PBD@Z";
#	endif
#else
	const char* creatorName = "_ZN19CThostFtdcTraderApi19CreateFtdcTraderApiEPKc";
#endif
	m_funcCreator = (CTPCreator)DLLHelper::get_symbol(m_hInstCTP, creatorName);

	m_bQuickStart = params->getBoolean("quick");

	return true;
}

/**
 * @brief 释放交易接口资源
 * @details 释放交易接口的所有资源
 *          包括释放CTP交易接口实例
 *          清空委托、成交、持仓等数据集合
 */
void TraderCTP::release()
{
	m_bStopped = true;

	if (m_pUserAPI)
	{
		//m_pUserAPI->RegisterSpi(NULL);
		m_pUserAPI->Release();
		m_pUserAPI = NULL;
	}

	if (m_ayOrders)
		m_ayOrders->clear();

	if (m_ayPosDetail)
		m_ayPosDetail->clear();

	if (m_mapPosition)
		m_mapPosition->clear();

	if (m_ayTrades)
		m_ayTrades->clear();
}

/**
 * @brief 连接交易服务器
 * @details 初始化CTP交易接口并连接服务器
 *          创建流文件目录、注册回调接口
 *          根据配置决定是否使用快速订阅模式
 *          注册前置机地址并初始化数据集合
 */
void TraderCTP::connect()
{
	std::stringstream ss;
	ss << m_strFlowDir << "flows/" << m_strBroker << "/" << m_strUser << "/";
	boost::filesystem::create_directories(ss.str().c_str());
	m_pUserAPI = m_funcCreator(ss.str().c_str());
	m_pUserAPI->RegisterSpi(this);
	if (m_bQuickStart)
	{
		m_pUserAPI->SubscribePublicTopic(THOST_TERT_QUICK);			// 注册公有流
		m_pUserAPI->SubscribePrivateTopic(THOST_TERT_QUICK);		// 注册私有流
	}
	else
	{
		m_pUserAPI->SubscribePublicTopic(THOST_TERT_RESUME);		// 注册公有流
		m_pUserAPI->SubscribePrivateTopic(THOST_TERT_RESUME);		// 注册私有流
	}

	for (std::string front : m_strFront)
	{
		m_pUserAPI->RegisterFront((char*)front.c_str());
		m_sink->handleTraderLog(LL_INFO, fmtutil::format("registerFront: {}", front));
	}

	if (m_pUserAPI)
	{
		m_pUserAPI->Init();
	}

	if (m_thrdWorker == NULL)
	{
		m_thrdWorker.reset(new StdThread([this](){
			while (!m_bStopped)
			{
				if(m_queQuery.empty() || m_bInQuery)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					continue;
				}

				uint64_t curTime = TimeUtils::getLocalTimeNow();
				if (curTime - m_lastQryTime < 1000)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(50));
					continue;
				}


				m_bInQuery = true;
				CommonExecuter& handler = m_queQuery.front();
				handler();

				{
					StdUniqueLock lock(m_mtxQuery);
					m_queQuery.pop();
				}

				m_lastQryTime = TimeUtils::getLocalTimeNow();
			}
		}));
	}
}

/**
 * @brief 断开交易服务器连接
 * @details 将断开连接请求添加到查询队列中
 *          等待工作线程完成后合并线程
 *          释放相关资源
 */
void TraderCTP::disconnect()
{
	m_queQuery.push([this]() {
		release();
	});

	if (m_thrdWorker)
	{
		m_thrdWorker->join();
		m_thrdWorker = NULL;
	}
}

/**
 * @brief 生成委托编号
 * @details 创建格式化的委托编号字符串
 *          由前置机编号、会话编号和递增的委托引用组成
 *          格式为：前置机编号#会话编号#委托引用
 * @param buffer 接收生成的委托编号的缓冲区
 * @param length 缓冲区长度
 * @return bool 是否成功生成委托编号
 */
bool TraderCTP::makeEntrustID(char* buffer, int length)
{
	if (buffer == NULL || length == 0)
		return false;

	try
	{
		memset(buffer, 0, length);
		uint32_t orderref = m_orderRef.fetch_add(1) + 1;
		fmt::format_to(buffer, "{:06d}#{:010d}#{:06d}", m_frontID, (uint32_t)m_sessionID, orderref);
		return true;
	}
	catch (...)
	{

	}

	return false;
}

/**
 * @brief 注册交易回调接口
 * @details 注册用于接收交易事件通知的回调接口
 *          并从接口中获取基础数据管理器
 * @param listener 交易回调接口实例
 */
void TraderCTP::registerSpi(ITraderSpi *listener)
{
	m_sink = listener;
	if (m_sink)
	{
		m_bdMgr = listener->getBaseDataMgr();
	}
}

/**
 * @brief 生成请求编号
 * @details 生成唯一的请求编号用于CTP API调用
 *          使用原子操作保证线程安全
 * @return uint32_t 新生成的请求编号
 */
uint32_t TraderCTP::genRequestID()
{
	return m_iRequestID.fetch_add(1) + 1;
}

/**
 * @brief 登录交易账户
 * @details 使用提供的用户名和密码登录交易账户
 *          设置内部状态并启动认证流程
 * @param user 用户名
 * @param pass 密码
 * @param productInfo 产品信息
 * @return int 成功返回0，失败返回负值
 */
int TraderCTP::login(const char* user, const char* pass, const char* productInfo)
{
	m_strUser = user;
	m_strPass = pass;
	m_strProdInfo = productInfo;

	if (m_pUserAPI == NULL)
	{
		return -1;
	}

	m_wrapperState = WS_LOGINING;
	authenticate();

	return 0;
}

/**
 * @brief 执行实际登录操作
 * @details 创建并填充CTP登录请求字段
 *          包括经纪商代码、用户名、密码、产品信息等
 *          然后向CTP服务器发送登录请求
 * @return int 总是返回0，实际登录结果通过回调函数获知
 */
int TraderCTP::doLogin()
{
	CThostFtdcReqUserLoginField req;
	memset(&req, 0, sizeof(req));
	wt_strcpy(req.BrokerID, m_strBroker.c_str(), m_strBroker.size());
	wt_strcpy(req.UserID, m_strUser.c_str(), m_strUser.size());
	wt_strcpy(req.Password, m_strPass.c_str(), m_strPass.size());
	wt_strcpy(req.UserProductInfo, m_strProdInfo.c_str(), m_strProdInfo.size());
	int iResult = m_pUserAPI->ReqUserLogin(&req, genRequestID());
	if (iResult != 0)
	{
		write_log(m_sink, LL_ERROR, "[TraderCTP] Sending login request failed: {}", iResult);
	}

	return 0;
}

/**
 * @brief 登出交易账户
 * @details 创建并填充CTP登出请求字段
 *          包括经纪商代码、用户名等信息
 *          然后向CTP服务器发送登出请求
 * @return int 如果接口未初始化返回-1，否则总是返回0，实际登出结果通过回调函数获知
 */
int TraderCTP::logout()
{
	if (m_pUserAPI == NULL)
	{
		return -1;
	}

	CThostFtdcUserLogoutField req;
	memset(&req, 0, sizeof(req));
	wt_strcpy(req.BrokerID, m_strBroker.c_str(), m_strBroker.size());
	wt_strcpy(req.UserID, m_strUser.c_str(), m_strUser.size());
	int iResult = m_pUserAPI->ReqUserLogout(&req, genRequestID());
	if (iResult != 0)
	{
		write_log(m_sink, LL_ERROR, "[TraderCTP] Sending logout request failed: {}", iResult);
	}

	return 0;
}

/**
 * @brief 委托下单
 * @details 将WTSEntrust对象的委托信息转换成CTP的订单请求格式
 *          并且填充各种必要的字段，如经纪商代码、投资者ID、合约代码、交易所代码
 *          价格类型、买卖方向、开平标志、数量、价格等
 *          同时设置不同委托标志（普通/FAK/FOK）对应的有效期和成交量条件
 * @param entrust 要发送的委托对象
 * @return int 如果交易通道未就绪返回-1，否则总是返回0，实际委托结果通过回调函数获知
 */
int TraderCTP::orderInsert(WTSEntrust* entrust)
{
	if (m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		write_log(m_sink, LL_ERROR, "[TraderCTP] Trading channel not ready");
		return -1;
	}

	CThostFtdcInputOrderField req;
	memset(&req, 0, sizeof(req));
	wt_strcpy(req.BrokerID, m_strBroker.c_str(), m_strBroker.size());
	wt_strcpy(req.InvestorID, m_strUser.c_str(), m_strUser.size());

	wt_strcpy(req.InstrumentID, entrust->getCode());
	wt_strcpy(req.ExchangeID, entrust->getExchg());

	if (strlen(entrust->getUserTag()) == 0)
	{
		///报单引用
		fmt::format_to(req.OrderRef, "{}", m_orderRef.fetch_add(0));
	}
	else
	{
		uint32_t fid, sid, orderref;
		extractEntrustID(entrust->getEntrustID(), fid, sid, orderref);
		///报单引用
		fmt::format_to(req.OrderRef, "{}", orderref);
	}

	if (strlen(entrust->getUserTag()) > 0)
	{
		m_eidCache.put(entrust->getEntrustID(), entrust->getUserTag(), 0, [this](const char* message) {
			write_log(m_sink, LL_WARN, message);
		});
	}

	///报单价格条件: 限价
	req.OrderPriceType = wrapPriceType(entrust->getPriceType(), strcmp(entrust->getExchg(), "CFFEX") == 0);
	///买卖方向: 
	req.Direction = wrapDirectionType(entrust->getDirection(), entrust->getOffsetType());
	///组合开平标志: 开仓
	req.CombOffsetFlag[0] = wrapOffsetType(entrust->getOffsetType());
	///组合投机套保标志
	req.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;
	///价格
	req.LimitPrice = entrust->getPrice();
	///数量: 1
	req.VolumeTotalOriginal = (int)entrust->getVolume();

	if(entrust->getOrderFlag() == WOF_NOR)
	{
		req.TimeCondition = THOST_FTDC_TC_GFD;
		req.VolumeCondition = THOST_FTDC_VC_AV;
	}
	else if (entrust->getOrderFlag() == WOF_FAK)
	{
		req.TimeCondition = THOST_FTDC_TC_IOC;
		req.VolumeCondition = THOST_FTDC_VC_AV;
	}
	else if (entrust->getOrderFlag() == WOF_FOK)
	{
		req.TimeCondition = THOST_FTDC_TC_IOC;
		req.VolumeCondition = THOST_FTDC_VC_CV;
	}
	//req.MinVolume = 1;
	
	///触发条件: 立即
	req.ContingentCondition = THOST_FTDC_CC_Immediately;
	///强平原因: 非强平
	req.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;
	///自动挂起标志: 否
	req.IsAutoSuspend = 0;
	///用户强评标志: 否
	req.UserForceClose = 0;

	int iResult = m_pUserAPI->ReqOrderInsert(&req, genRequestID());
	if (iResult != 0)
	{
		write_log(m_sink, LL_ERROR, "[TraderCTP] Order inserting failed: {}", iResult);
	}

	return 0;
}

/**
 * @brief 委托操作（撤单）
 * @details 将WTSEntrustAction对象的撤单信息转换成CTP的撤单请求格式
 *          首先通过委托ID提取前置机编号、会话编号和委托引用
 *          然后填充经纪商代码、投资者ID、操作标志、合约代码、交易所等信息
 *          最后发送撤单请求给CTP交易服务器
 * @param action 撤单操作对象
 * @return int 如果交易通道未就绪或提取委托ID失败返回-1，否则总是返回0，实际撤单结果通过回调函数获知
 */
int TraderCTP::orderAction(WTSEntrustAction* action)
{
	if (m_wrapperState != WS_ALLREADY)
		return -1;

	uint32_t frontid, sessionid, orderref;
	if (!extractEntrustID(action->getEntrustID(), frontid, sessionid, orderref))
		return -1;

	CThostFtdcInputOrderActionField req;
	memset(&req, 0, sizeof(req));
	wt_strcpy(req.BrokerID, m_strBroker.c_str(), m_strBroker.size());
	wt_strcpy(req.InvestorID, m_strUser.c_str(), m_strUser.size());

	///报单引用
	fmt::format_to(req.OrderRef, "{}", orderref);
	///请求编号
	///前置编号
	req.FrontID = frontid;
	///会话编号
	req.SessionID = sessionid;
	///操作标志
	req.ActionFlag = wrapActionFlag(action->getActionFlag());
	///合约代码
	wt_strcpy(req.InstrumentID, action->getCode());

	wt_strcpy(req.OrderSysID, action->getOrderID());
	wt_strcpy(req.ExchangeID, action->getExchg());

	int iResult = m_pUserAPI->ReqOrderAction(&req, genRequestID());
	if (iResult != 0)
	{
		write_log(m_sink, LL_ERROR, "[TraderCTP] Sending cancel request failed: {}", iResult);
	}

	return 0;
}

/**
 * @brief 查询资金账户
 * @details 将资金账户查询请求添加到查询队列中
 *          创建查询资金请求并填充经纪商代码和投资者ID
 *          然后将该请求异步提交给工作线程处理
 *          结果将在对应的回调函数中处理
 * @return int 如果交易接口未初始化或未准备就绪返回-1，否则返回0
 */
int TraderCTP::queryAccount()
{
	if (m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	{
		StdUniqueLock lock(m_mtxQuery);
		m_queQuery.push([this]() {
			CThostFtdcQryTradingAccountField req;
			memset(&req, 0, sizeof(req));
			wt_strcpy(req.BrokerID, m_strBroker.c_str(), m_strBroker.size());
			wt_strcpy(req.InvestorID, m_strUser.c_str(), m_strUser.size());
			m_pUserAPI->ReqQryTradingAccount(&req, genRequestID());
		});
	}

	//triggerQuery();

	return 0;
}

/**
 * @brief 查询持仓信息
 * @details 将持仓查询请求添加到查询队列中
 *          创建查询持仓请求并填充经纪商代码和投资者ID
 *          然后将该请求异步提交给工作线程处理
 *          结果将在对应的回调函数中处理
 * @return int 如果交易接口未初始化或未准备就绪返回-1，否则返回0
 */
int TraderCTP::queryPositions()
{
	if (m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	{
		StdUniqueLock lock(m_mtxQuery);
		m_queQuery.push([this]() {
			CThostFtdcQryInvestorPositionField req;
			memset(&req, 0, sizeof(req));
			wt_strcpy(req.BrokerID, m_strBroker.c_str(), m_strBroker.size());
			wt_strcpy(req.InvestorID, m_strUser.c_str(), m_strUser.size());
			m_pUserAPI->ReqQryInvestorPosition(&req, genRequestID());
		});
	}

	//triggerQuery();

	return 0;
}

/**
 * @brief 查询委托单
 * @details 将查询委托请求添加到查询队列中
 *          创建查询委托请求并填充经纪商代码和投资者ID
 *          然后将该请求异步提交给工作线程处理
 *          返回的所有委托单将在对应的回调函数中处理
 * @return int 如果交易接口未初始化或未准备就绪返回-1，否则返回0
 */
int TraderCTP::queryOrders()
{
	if (m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	{
		StdUniqueLock lock(m_mtxQuery);
		m_queQuery.push([this]() {
			CThostFtdcQryOrderField req;
			memset(&req, 0, sizeof(req));
			wt_strcpy(req.BrokerID, m_strBroker.c_str(), m_strBroker.size());
			wt_strcpy(req.InvestorID, m_strUser.c_str(), m_strUser.size());

			m_pUserAPI->ReqQryOrder(&req, genRequestID());
		});

		//triggerQuery();
	}

	return 0;
}

/**
 * @brief 查询成交记录
 * @details 将成交查询请求添加到查询队列中
 *          创建查询成交请求并填充经纪商代码和投资者ID
 *          然后将该请求异步提交给工作线程处理
 *          返回的所有成交记录将在对应的回调函数中处理
 * @return int 如果交易接口未初始化或未准备就绪返回-1，否则返回0
 */
int TraderCTP::queryTrades()
{
	if (m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	{
		StdUniqueLock lock(m_mtxQuery);
		m_queQuery.push([this]() {
			CThostFtdcQryTradeField req;
			memset(&req, 0, sizeof(req));
			wt_strcpy(req.BrokerID, m_strBroker.c_str(), m_strBroker.size());
			wt_strcpy(req.InvestorID, m_strUser.c_str(), m_strUser.size());

			m_pUserAPI->ReqQryTrade(&req, genRequestID());
		});
	}

	//triggerQuery();

	return 0;
}

/**
 * @brief 查询结算单信息
 * @details 根据指定的交易日期查询结算单信息
 *          首先清空之前的结算单字符串
 *          然后将查询请求添加到查询队列中
 *          创建结算单查询请求并填充经纪商代码和投资者ID
 * @param uDate 交易日期(YYYYMMDD格式)
 * @return int 如果交易接口未初始化或未准备就绪返回-1，否则返回0
 */
int TraderCTP::querySettlement(uint32_t uDate)
{
	if (m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	m_strSettleInfo.clear();
	StdUniqueLock lock(m_mtxQuery);
	m_queQuery.push([this, uDate]() {
		CThostFtdcQrySettlementInfoField req;
		memset(&req, 0, sizeof(req));
		wt_strcpy(req.BrokerID, m_strBroker.c_str(), m_strBroker.size());
		wt_strcpy(req.InvestorID, m_strUser.c_str(), m_strUser.size());
		fmt::format_to(req.TradingDay, "{}", uDate);

		m_pUserAPI->ReqQrySettlementInfo(&req, genRequestID());
	});

	//triggerQuery();

	return 0;
}

/**
 * @brief 前置机连接成功回调
 * @details 当与CTP前置机建立连接成功后系统自动调用此函数
 *          通过回调接口将连接成功事件通知给上层应用
 */
void TraderCTP::OnFrontConnected()
{
	if (m_sink)
		m_sink->handleEvent(WTE_Connect, 0);
}

/**
 * @brief 前置机断开连接回调
 * @details 当与CTP前置机连接断开后系统自动调用此函数
 *          将包装器状态设置为未登录状态
 *          通过回调接口将断开连接事件及原因通知给上层应用
 * @param nReason 断开原因代码
 */
void TraderCTP::OnFrontDisconnected(int nReason)
{
	m_wrapperState = WS_NOTLOGIN;
	if (m_sink)
		m_sink->handleEvent(WTE_Close, nReason);
}

/**
 * @brief 心跳警告回调
 * @details 当与CTP前置机连接建立后，如果在一定时间内没有收到心跳包，系统会自动调用此函数
 *          通过回调接口将心跳警告事件通知给上层应用
 * @param nTimeLapse 心跳间隔时间（秒）
 */
void TraderCTP::OnHeartBeatWarning(int nTimeLapse)
{
	write_log(m_sink, LL_DEBUG, "[TraderCTP][{}-{}] Heartbeating...", m_strBroker.c_str(), m_strUser.c_str());
}


/**
 * @brief 认证请求响应回调
 * @details 当调用ReqAuthenticate发送认证请求后收到的响应
 *          如果认证成功，继续调用doLogin方法进行登录
 *          如果认证失败，记录错误日志并设置登录失败状态
 * @param pRspAuthenticateField 认证响应数据
 * @param pRspInfo 响应信息，包含错误代码和错误信息
 * @param nRequestID 请求ID
 * @param bIsLast 是否为最后一个响应
 */
void TraderCTP::OnRspAuthenticate(CThostFtdcRspAuthenticateField *pRspAuthenticateField, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if (!IsErrorRspInfo(pRspInfo))
	{
		doLogin();
	}
	else
	{
		write_log(m_sink, LL_ERROR, "[TraderCTP][{}-{}] Authentiation failed: {}", m_strBroker.c_str(), m_strUser.c_str(), pRspInfo->ErrorMsg);
		m_wrapperState = WS_LOGINFAILED;

		if (m_sink)
			m_sink->onLoginResult(false, pRspInfo->ErrorMsg, 0);
	}

}

/**
 * @brief 登录请求响应回调
 * @details 当调用ReqUserLogin发送登录请求后收到的响应
 *          如果登录成功，设置包装器状态为已登录
 *          获取交易日、前置机编号、会话编号等信息
 *          如果登录失败，设置到登录失败状态
 * @param pRspUserLogin 登录响应数据
 * @param pRspInfo 响应信息，包含错误代码和错误信息
 * @param nRequestID 请求ID
 * @param bIsLast 是否为最后一个响应
 */
void TraderCTP::OnRspUserLogin(CThostFtdcRspUserLoginField *pRspUserLogin, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if (!IsErrorRspInfo(pRspInfo))
	{
		m_wrapperState = WS_LOGINED;

		// 保存会话参数
		m_frontID = pRspUserLogin->FrontID;
		m_sessionID = pRspUserLogin->SessionID;
		m_orderRef = atoi(pRspUserLogin->MaxOrderRef);
		///获取当前交易日
		m_lDate = atoi(m_pUserAPI->GetTradingDay());

		write_log(m_sink, LL_INFO, "[TraderCTP][{}-{}] Login succeed, AppID: {}, Sessionid: {}, login time: {}...",
			m_strBroker.c_str(), m_strUser.c_str(), m_strAppID.c_str(), m_sessionID, pRspUserLogin->LoginTime);

		{
			//初始化委托单缓存器
			std::stringstream ss;
			ss << m_strFlowDir << "local/" << m_strBroker << "/";
			std::string path = StrUtil::standardisePath(ss.str());
			if (!StdFile::exists(path.c_str()))
				boost::filesystem::create_directories(path.c_str());
			ss << m_strUser << "_eid.sc";
			m_eidCache.init(ss.str().c_str(), m_lDate, [this](const char* message) {
				write_log(m_sink, LL_WARN, message);
			});
		}

		{
			//初始化订单标记缓存器
			std::stringstream ss;
			ss << m_strFlowDir << "local/" << m_strBroker << "/";
			std::string path = StrUtil::standardisePath(ss.str());
			if (!StdFile::exists(path.c_str()))
				boost::filesystem::create_directories(path.c_str());
			ss << m_strUser << "_oid.sc";
			m_oidCache.init(ss.str().c_str(), m_lDate, [this](const char* message) {
				write_log(m_sink, LL_WARN, message);
			});
		}

		write_log(m_sink, LL_INFO, "[TraderCTP][{}-{}] Login succeed, trading date: {}...", m_strBroker.c_str(), m_strUser.c_str(), m_lDate);

		write_log(m_sink, LL_INFO, "[TraderCTP][{}-{}] Querying confirming state of settlement data...", m_strBroker.c_str(), m_strUser.c_str());
		queryConfirm();
	}
	else
	{
		write_log(m_sink, LL_ERROR, "[TraderCTP][{}-{}] Login failed: {}", m_strBroker.c_str(), m_strUser.c_str(), pRspInfo->ErrorMsg);
		m_wrapperState = WS_LOGINFAILED;

		if (m_sink)
			m_sink->onLoginResult(false, pRspInfo->ErrorMsg, 0);
	}
}

/**
 * @brief 登出请求响应回调
 * @details 当调用ReqUserLogout发送登出请求后收到的响应
 *          将包装器状态设置为未登录状态
 *          通过回调接口将登出事件通知给上层应用
 * @param pUserLogout 登出响应数据
 * @param pRspInfo 响应信息，包含错误代码和错误信息
 * @param nRequestID 请求ID
 * @param bIsLast 是否为最后一个响应
 */
void TraderCTP::OnRspUserLogout(CThostFtdcUserLogoutField *pUserLogout, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	m_wrapperState = WS_NOTLOGIN;
	if (m_sink)
		m_sink->handleEvent(WTE_Logout, 0);
}


/**
 * @brief 查询结算单确认响应回调
 * @details 当调用ReqQrySettlementInfoConfirm查询结算单确认情况后收到的响应
 *          根据回调中获取的确认日期信息判断是否需要重新确认结算单
 *          如果确认日期大于等于交易日，则设置为已准备就绪状态
 *          否则调用confirm方法进行结算单确认
 * @param pSettlementInfoConfirm 结算单确认信息
 * @param pRspInfo 响应信息，包含错误代码和错误信息
 * @param nRequestID 请求ID
 * @param bIsLast 是否为最后一个响应
 */
void TraderCTP::OnRspQrySettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField *pSettlementInfoConfirm, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if (bIsLast)
	{
		m_bInQuery = false;
		//triggerQuery();
	}

	if (!IsErrorRspInfo(pRspInfo))
	{
		if (pSettlementInfoConfirm != NULL)
		{
			uint32_t uConfirmDate = strtoul(pSettlementInfoConfirm->ConfirmDate, NULL, 10);
			if (uConfirmDate >= m_lDate)
			{
				m_wrapperState = WS_CONFIRMED;

				write_log(m_sink, LL_INFO, "[TraderCTP][{}-{}] Trading channel initialized...", m_strBroker.c_str(), m_strUser.c_str());
				m_wrapperState = WS_ALLREADY;
				if (m_sink)
					m_sink->onLoginResult(true, "", m_lDate);
			}
			else
			{
				m_wrapperState = WS_CONFIRM_QRYED;

				write_log(m_sink, LL_INFO, "[TraderCTP][{}-{}] Confirming settlement data...", m_strBroker.c_str(), m_strUser.c_str());
				confirm();
			}
		}
		else
		{
			m_wrapperState = WS_CONFIRM_QRYED;
			confirm();
		}
	}

}

/**
 * @brief 结算单确认请求响应回调
 * @details 当调用ReqSettlementInfoConfirm发送结算单确认请求后收到的响应
 *          如果结算单确认成功，将包装器状态更新为已就绪状态
 *          并通过回调接口将登录成功信息通知给上层应用
 * @param pSettlementInfoConfirm 结算单确认信息
 * @param pRspInfo 响应信息，包含错误代码和错误信息
 * @param nRequestID 请求ID
 * @param bIsLast 是否为最后一个响应
 */
void TraderCTP::OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField *pSettlementInfoConfirm, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if (!IsErrorRspInfo(pRspInfo) && pSettlementInfoConfirm != NULL)
	{
		if (m_wrapperState == WS_CONFIRM_QRYED)
		{
			m_wrapperState = WS_CONFIRMED;

			write_log(m_sink, LL_INFO, "[TraderCTP][{}-{}] Trading channel initialized...", m_strBroker.c_str(), m_strUser.c_str());
			m_wrapperState = WS_ALLREADY;
			if (m_sink)
				m_sink->onLoginResult(true, "", m_lDate);
		}
	}
}

/**
 * @brief 订单插入失败响应回调
 * @details 当订单提交被CTP交易服务器拒绝时触发此回调
 *          将订单字段和错误信息转换为内部错误对象
 *          然后通过回调接口将错误信息推送给上层应用
 * @param pInputOrder CTP订单插入字段指针
 * @param pRspInfo 响应信息，包含错误代码和错误信息
 * @param nRequestID 请求ID
 * @param bIsLast 是否为最后一个响应
 */
void TraderCTP::OnRspOrderInsert(CThostFtdcInputOrderField *pInputOrder, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	WTSEntrust* entrust = makeEntrust(pInputOrder);
	if (entrust)
	{
		WTSError *err = makeError(pRspInfo, WEC_ORDERINSERT);
		//g_orderMgr.onRspEntrust(entrust, err);
		if (m_sink)
			m_sink->onRspEntrust(entrust, err);
		entrust->release();
		err->release();
	}
	else if(IsErrorRspInfo(pRspInfo))
	{
		WTSError *err = makeError(pRspInfo, WEC_ORDERINSERT);
		if (m_sink)
			m_sink->onTraderError(err);
		err->release();
	}
}

/**
 * @brief 撤单操作失败响应回调
 * @details 当撤单操作被CTP交易服务器拒绝时触发此回调
 *          将撤单操作字段和错误信息转换为内部错误对象和操作对象
 *          然后通过回调接口将错误信息推送给上层应用
 * @param pInputOrderAction CTP撤单操作字段指针
 * @param pRspInfo 响应信息，包含错误代码和错误信息
 * @param nRequestID 请求ID
 * @param bIsLast 是否为最后一个响应
 */
void TraderCTP::OnRspOrderAction(CThostFtdcInputOrderActionField *pInputOrderAction, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if (IsErrorRspInfo(pRspInfo))
	{
		WTSError* error = WTSError::create(WEC_ORDERCANCEL, pRspInfo->ErrorMsg);
		WTSEntrustAction* action = makeAction(pInputOrderAction);
		if (m_sink)
			m_sink->onTraderError(error, action);

		if (error)
			error->release();

		if (action)
			action->release();
	}
}

/**
 * @brief 查询资金账户响应回调
 * @details 当调用ReqQryTradingAccount查询资金账户后收到的响应
 *          将CTP账户字段转换为内部WTSAccountInfo对象
 *          包括保证金、可用资金、手续费、平仓盈亏、入金、出金等信息
 *          然后通过回调接口将账户信息推送给上层应用
 * @param pTradingAccount CTP资金账户字段指针
 * @param pRspInfo 响应信息，包含错误代码和错误信息
 * @param nRequestID 请求ID
 * @param bIsLast 是否为最后一个响应
 */
void TraderCTP::OnRspQryTradingAccount(CThostFtdcTradingAccountField *pTradingAccount, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if (bIsLast)
	{
		m_bInQuery = false;
		//triggerQuery();
	}

	if (bIsLast && !IsErrorRspInfo(pRspInfo))
	{
		WTSAccountInfo* accountInfo = WTSAccountInfo::create();
		accountInfo->setPreBalance(pTradingAccount->PreBalance);
		accountInfo->setCloseProfit(pTradingAccount->CloseProfit);
		accountInfo->setDynProfit(pTradingAccount->PositionProfit);
		accountInfo->setMargin(pTradingAccount->CurrMargin);
		accountInfo->setAvailable(pTradingAccount->Available);
		accountInfo->setCommission(pTradingAccount->Commission);
		accountInfo->setFrozenMargin(pTradingAccount->FrozenMargin);
		accountInfo->setFrozenCommission(pTradingAccount->FrozenCommission);
		accountInfo->setDeposit(pTradingAccount->Deposit);
		accountInfo->setWithdraw(pTradingAccount->Withdraw);
		accountInfo->setBalance(accountInfo->getPreBalance() + accountInfo->getCloseProfit() - accountInfo->getCommission() + accountInfo->getDeposit() - accountInfo->getWithdraw());
		accountInfo->setCurrency("CNY");

		WTSArray * ay = WTSArray::create();
		ay->append(accountInfo, false);
		if (m_sink)
			m_sink->onRspAccount(ay);

		ay->release();
	}
}

/**
 * @brief 查询投资者持仓响应回调
 * @details 当调用ReqQryInvestorPosition查询持仓后收到的响应
 *          将CTP持仓字段转换为内部WTSPositionItem对象
 *          根据交易所和合约代码划分持仓，并区分多空方向
 *          处理昨仓和今仓的持仓数量、均价、浮动盈亏等信息
 *          最后将所有持仓信息通过回调接口推送给上层应用
 * @param pInvestorPosition CTP持仓字段指针
 * @param pRspInfo 响应信息，包含错误代码和错误信息
 * @param nRequestID 请求ID
 * @param bIsLast 是否为最后一个响应
 */
void TraderCTP::OnRspQryInvestorPosition(CThostFtdcInvestorPositionField *pInvestorPosition, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if (bIsLast)
	{
		m_bInQuery = false;
		//triggerQuery();
	}

	if (!IsErrorRspInfo(pRspInfo) && pInvestorPosition)
	{
		if (NULL == m_mapPosition)
			m_mapPosition = PositionMap::create();

		WTSContractInfo* contract = m_bdMgr->getContract(pInvestorPosition->InstrumentID, pInvestorPosition->ExchangeID);
		if (contract)
		{
			WTSCommodityInfo* commInfo = contract->getCommInfo();
			std::string key = fmt::format("{}-{}", pInvestorPosition->InstrumentID, pInvestorPosition->PosiDirection);
			WTSPositionItem* pos = (WTSPositionItem*)m_mapPosition->get(key);
			if(pos == NULL)
			{
				pos = WTSPositionItem::create(pInvestorPosition->InstrumentID, commInfo->getCurrency(), commInfo->getExchg());
				pos->setContractInfo(contract);
				m_mapPosition->add(key, pos, false);
			}
			pos->setDirection(wrapPosDirection(pInvestorPosition->PosiDirection));
			if(commInfo->getCoverMode() == CM_CoverToday)
			{
				if (pInvestorPosition->PositionDate == THOST_FTDC_PSD_Today)
					pos->setNewPosition(pInvestorPosition->Position);
				else
					pos->setPrePosition(pInvestorPosition->Position);
			}
			else
			{
				pos->setNewPosition(pInvestorPosition->TodayPosition);
				pos->setPrePosition(pInvestorPosition->Position - pInvestorPosition->TodayPosition);
			}

			pos->setMargin(pos->getMargin() + pInvestorPosition->UseMargin);
			pos->setDynProfit(pos->getDynProfit() + pInvestorPosition->PositionProfit);
			pos->setPositionCost(pos->getPositionCost() + pInvestorPosition->PositionCost);

			if (pos->getTotalPosition() != 0)
			{
				pos->setAvgPrice(pos->getPositionCost() / pos->getTotalPosition() / commInfo->getVolScale());
			}
			else
			{
				pos->setAvgPrice(0);
			}

			if (commInfo->getCategoty() != CC_Combination)
			{
				if (commInfo->getCoverMode() == CM_CoverToday)
				{
					if (pInvestorPosition->PositionDate == THOST_FTDC_PSD_Today)
					{
						int availNew = pInvestorPosition->Position;
						if (pInvestorPosition->PosiDirection == THOST_FTDC_PD_Long)
						{
							availNew -= pInvestorPosition->ShortFrozen;
						}
						else
						{
							availNew -= pInvestorPosition->LongFrozen;
						}
						if (availNew < 0)
							availNew = 0;
						pos->setAvailNewPos(availNew);
					}
					else
					{
						int availPre = pInvestorPosition->Position;
						if (pInvestorPosition->PosiDirection == THOST_FTDC_PD_Long)
						{
							availPre -= pInvestorPosition->ShortFrozen;
						}
						else
						{
							availPre -= pInvestorPosition->LongFrozen;
						}
						if (availPre < 0)
							availPre = 0;
						pos->setAvailPrePos(availPre);
					}
				}
				else
				{
					int availNew = pInvestorPosition->TodayPosition;
					if (pInvestorPosition->PosiDirection == THOST_FTDC_PD_Long)
					{
						availNew -= pInvestorPosition->ShortFrozen;
					}
					else
					{
						availNew -= pInvestorPosition->LongFrozen;
					}
					if (availNew < 0)
						availNew = 0;
					pos->setAvailNewPos(availNew);

					double availPre = pos->getNewPosition() + pos->getPrePosition()
						- pInvestorPosition->LongFrozen - pInvestorPosition->ShortFrozen
						- pos->getAvailNewPos();
					pos->setAvailPrePos(availPre);
				}
			}
			else
			{

			}

			if (decimal::lt(pos->getTotalPosition(), 0.0) && decimal::eq(pos->getMargin(), 0.0))
			{
				//有仓位,但是保证金为0,则说明是套利合约,单个合约的可用持仓全部置为0
				pos->setAvailNewPos(0);
				pos->setAvailPrePos(0);
			}
		}
	}

	if (bIsLast)
	{

		WTSArray* ayPos = WTSArray::create();

		if(m_mapPosition && m_mapPosition->size() > 0)
		{
			for (auto it = m_mapPosition->begin(); it != m_mapPosition->end(); it++)
			{
				ayPos->append(it->second, true);
			}
		}

		if (m_sink)
			m_sink->onRspPosition(ayPos);

		if (m_mapPosition)
		{
			m_mapPosition->release();
			m_mapPosition = NULL;
		}

		ayPos->release();
	}
}

/**
 * @brief 查询结算单响应回调
 * @details 当调用ReqQrySettlementInfo查询结算单后收到的响应
 *          收集结算单内容并存储到类成员变量m_strSettleInfo中
 *          当收到所有结算单内容后，通过回调接口将结算单推送给上层应用
 * @param pSettlementInfo CTP结算单字段指针
 * @param pRspInfo 响应信息，包含错误代码和错误信息
 * @param nRequestID 请求ID
 * @param bIsLast 是否为最后一个响应
 */
void TraderCTP::OnRspQrySettlementInfo(CThostFtdcSettlementInfoField *pSettlementInfo, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if (bIsLast)
	{
		m_bInQuery = false;
		//triggerQuery();
	}

	if (!IsErrorRspInfo(pRspInfo) && pSettlementInfo)
	{
		m_strSettleInfo += pSettlementInfo->Content;
	}

	if (bIsLast && !m_strSettleInfo.empty())
	{
		m_sink->onRspSettlementInfo(atoi(pSettlementInfo->TradingDay), m_strSettleInfo.c_str());
	}
}

/**
 * @brief 查询成交响应回调
 * @details 当调用ReqQryTrade查询成交记录后收到的响应
 *          将CTP成交字段转换为内部WTSTradeInfo对象
 *          并添加到成交数组中
 *          收集完所有成交数据后，通过回调接口将成交信息推送给上层应用
 * @param pTrade CTP成交字段指针
 * @param pRspInfo 响应信息，包含错误代码和错误信息
 * @param nRequestID 请求ID
 * @param bIsLast 是否为最后一个响应
 */
void TraderCTP::OnRspQryTrade(CThostFtdcTradeField *pTrade, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if (bIsLast)
	{
		m_bInQuery = false;
		//triggerQuery();
	}

	if (!IsErrorRspInfo(pRspInfo) && pTrade)
	{
		if (NULL == m_ayTrades)
			m_ayTrades = WTSArray::create();

		WTSTradeInfo* trade = makeTradeInfo(pTrade);
		if (trade)
		{
			m_ayTrades->append(trade, false);
		}
	}

	if (bIsLast)
	{
		if (m_sink)
			m_sink->onRspTrades(m_ayTrades);

		if (NULL != m_ayTrades)
			m_ayTrades->clear();
	}
}

/**
 * @brief 查询订单响应回调
 * @details 当调用ReqQryOrder查询订单后收到的响应
 *          将CTP订单字段转换为内部WTSOrderInfo对象
 *          并添加到订单数组中
 *          收集完所有订单数据后，通过回调接口将订单信息推送给上层应用
 * @param pOrder CTP订单字段指针
 * @param pRspInfo 响应信息，包含错误代码和错误信息
 * @param nRequestID 请求ID
 * @param bIsLast 是否为最后一个响应
 */
void TraderCTP::OnRspQryOrder(CThostFtdcOrderField *pOrder, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if (bIsLast)
	{
		m_bInQuery = false;
		//triggerQuery();
	}

	if (!IsErrorRspInfo(pRspInfo) && pOrder)
	{
		if (NULL == m_ayOrders)
			m_ayOrders = WTSArray::create();

		WTSOrderInfo* orderInfo = makeOrderInfo(pOrder);
		if (orderInfo)
		{
			m_ayOrders->append(orderInfo, false);
		}
	}

	if (bIsLast)
	{
		if (m_sink)
			m_sink->onRspOrders(m_ayOrders);

		if (m_ayOrders)
			m_ayOrders->clear();
	}
}

void TraderCTP::OnRspError(CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	m_sink->handleTraderLog(LL_ERROR, fmtutil::format("{} rsp error: {} : {}", nRequestID, pRspInfo->ErrorID, pRspInfo->ErrorMsg));
}

/**
 * @brief 订单状态回报回调
 * @details 当订单状态发生变化时，CTP交易服务器主动推送该回调
 *          将CTP订单字段转换为内部WTSOrderInfo对象
 *          然后通过回调接口将订单状态推送给上层应用
 * @param pOrder CTP订单字段指针
 */
void TraderCTP::OnRtnOrder(CThostFtdcOrderField *pOrder)
{
	WTSOrderInfo *orderInfo = makeOrderInfo(pOrder);
	if (orderInfo)
	{
		if (m_sink)
			m_sink->onPushOrder(orderInfo);

		orderInfo->release();
	}

	//ReqQryTradingAccount();
}

/**
 * @brief 成交回报回调
 * @details 当有新成交发生时，CTP交易服务器主动推送该回调
 *          将CTP成交字段转换为内部WTSTradeInfo对象
 *          然后通过回调接口将成交信息推送给上层应用
 * @param pTrade CTP成交字段指针
 */
void TraderCTP::OnRtnTrade(CThostFtdcTradeField *pTrade)
{
	WTSTradeInfo *tRecord = makeTradeInfo(pTrade);
	if (tRecord)
	{
		if (m_sink)
			m_sink->onPushTrade(tRecord);

		tRecord->release();
	}
}

/**
 * @brief 将内部方向类型和开平类型转换为CTP方向类型
 * @details 在中国期货市场中，交易方向需要结合开平标志来确定买卖方向
 *          例如：多头开仓是买入，多头平仓是卖出
 *                空头开仓是卖出，空头平仓是买入
 * @param dirType 内部交易方向类型（多/空）
 * @param offsetType 内部开平类型（开仓/平仓/平今/平昨）
 * @return int CTP方向类型定义，对应THOST_FTDC_D_Buy或THOST_FTDC_D_Sell
 */
int TraderCTP::wrapDirectionType(WTSDirectionType dirType, WTSOffsetType offsetType)
{
	if (WDT_LONG == dirType)
		if (offsetType == WOT_OPEN)
			return THOST_FTDC_D_Buy;
		else
			return THOST_FTDC_D_Sell;
	else
		if (offsetType == WOT_OPEN)
			return THOST_FTDC_D_Sell;
		else
			return THOST_FTDC_D_Buy;
}

/**
 * @brief 将CTP方向类型和开平类型转换为内部方向类型
 * @details 与第一个wrapDirectionType方法相反，该方法将CTP的买卖方向和开平标志转换为内部使用的多空方向
 *          例如：买入开仓是多头，买入平仓是空头
 *                卖出开仓是空头，卖出平仓是多头
 * @param dirType CTP交易方向类型（买/卖）
 * @param offsetType CTP开平类型（开仓/平仓/平今/平昨）
 * @return WTSDirectionType 内部方向类型定义，对应WDT_LONG或WDT_SHORT
 */
WTSDirectionType TraderCTP::wrapDirectionType(TThostFtdcDirectionType dirType, TThostFtdcOffsetFlagType offsetType)
{
	if (THOST_FTDC_D_Buy == dirType)
		if (offsetType == THOST_FTDC_OF_Open)
			return WDT_LONG;
		else
			return WDT_SHORT;
	else
		if (offsetType == THOST_FTDC_OF_Open)
			return WDT_SHORT;
		else
			return WDT_LONG;
}

WTSDirectionType TraderCTP::wrapPosDirection(TThostFtdcPosiDirectionType dirType)
{
	if (THOST_FTDC_PD_Long == dirType)
		return WDT_LONG;
	else
		return WDT_SHORT;
}

/**
 * @brief 将内部开平类型转换为CTP开平类型
 * @details 转换内部定义的开平类型为CTP接口使用的开平类型
 *          注意在CTP中平昨仓使用的是普通平仓标志THOST_FTDC_OF_Close
 * @param offType 内部开平类型定义（WOT_OPEN/WOT_CLOSE/WOT_CLOSETODAY/WOT_CLOSEYESTERDAY/WOT_FORCECLOSE）
 * @return int CTP开平类型定义，对应THOST_FTDC_OF_*系列常量
 */
int TraderCTP::wrapOffsetType(WTSOffsetType offType)
{
	if (WOT_OPEN == offType)
		return THOST_FTDC_OF_Open;
	else if (WOT_CLOSE == offType)
		return THOST_FTDC_OF_Close;
	else if (WOT_CLOSETODAY == offType)
		return THOST_FTDC_OF_CloseToday;
	else if (WOT_CLOSEYESTERDAY == offType)
		return THOST_FTDC_OF_Close;
	else
		return THOST_FTDC_OF_ForceClose;
}

/**
 * @brief 将CTP开平类型转换为内部开平类型
 * @details 与第一个wrapOffsetType方法相反，该方法将CTP接口的开平类型转换为内部使用的开平类型
 *          注意在CTP中普通平仓标志THOST_FTDC_OF_Close会转换为WOT_CLOSE
 *          这会导致无法区分是平仓还是平昨仓，需要结合其他信息判断
 * @param offType CTP开平类型定义（THOST_FTDC_OF_*系列常量）
 * @return WTSOffsetType 内部开平类型定义（WOT_OPEN/WOT_CLOSE/WOT_CLOSETODAY/WOT_FORCECLOSE）
 */
WTSOffsetType TraderCTP::wrapOffsetType(TThostFtdcOffsetFlagType offType)
{
	if (THOST_FTDC_OF_Open == offType)
		return WOT_OPEN;
	else if (THOST_FTDC_OF_Close == offType)
		return WOT_CLOSE;
	else if (THOST_FTDC_OF_CloseToday == offType)
		return WOT_CLOSETODAY;
	else
		return WOT_FORCECLOSE;
}

/**
 * @brief 将内部价格类型转换为CTP价格类型
 * @details 转换内部价格类型为CTP接口使用的价格类型
 *          特别地，对于中金所（CFFEX）的市价单需要使用五档行情报价
 *          其他交易所可以使用普通市价单
 * @param priceType 内部价格类型定义（WPT_ANYPRICE/WPT_LIMITPRICE/WPT_BESTPRICE/WPT_LASTPRICE）
 * @param isCFFEX 是否为中金所合约，默认为false
 * @return int CTP价格类型定义，对应THOST_FTDC_OPT_*系列常量
 */
int TraderCTP::wrapPriceType(WTSPriceType priceType, bool isCFFEX /* = false */)
{
	if (WPT_ANYPRICE == priceType)
		return isCFFEX ? THOST_FTDC_OPT_FiveLevelPrice : THOST_FTDC_OPT_AnyPrice;
	else if (WPT_LIMITPRICE == priceType)
		return THOST_FTDC_OPT_LimitPrice;
	else if (WPT_BESTPRICE == priceType)
		return THOST_FTDC_OPT_BestPrice;
	else
		return THOST_FTDC_OPT_LastPrice;
}

/**
 * @brief 将CTP价格类型转换为内部价格类型
 * @details 与第一个wrapPriceType方法相反，该方法将CTP接口的价格类型转换为内部使用的价格类型
 *          注意市价单和五档行情报价类型都会转换为内部的WPT_ANYPRICE类型
 * @param priceType CTP价格类型定义（THOST_FTDC_OPT_*系列常量）
 * @return WTSPriceType 内部价格类型定义（WPT_ANYPRICE/WPT_LIMITPRICE/WPT_BESTPRICE/WPT_LASTPRICE）
 */
WTSPriceType TraderCTP::wrapPriceType(TThostFtdcOrderPriceTypeType priceType)
{
	if (THOST_FTDC_OPT_AnyPrice == priceType || THOST_FTDC_OPT_FiveLevelPrice == priceType)
		return WPT_ANYPRICE;
	else if (THOST_FTDC_OPT_LimitPrice == priceType)
		return WPT_LIMITPRICE;
	else if (THOST_FTDC_OPT_BestPrice == priceType)
		return WPT_BESTPRICE;
	else
		return WPT_LASTPRICE;
}

int TraderCTP::wrapTimeCondition(WTSTimeCondition timeCond)
{
	if (WTC_IOC == timeCond)
		return THOST_FTDC_TC_IOC;
	else if (WTC_GFD == timeCond)
		return THOST_FTDC_TC_GFD;
	else
		return THOST_FTDC_TC_GFS;
}

WTSTimeCondition TraderCTP::wrapTimeCondition(TThostFtdcTimeConditionType timeCond)
{
	if (THOST_FTDC_TC_IOC == timeCond)
		return WTC_IOC;
	else if (THOST_FTDC_TC_GFD == timeCond)
		return WTC_GFD;
	else
		return WTC_GFS;
}

WTSOrderState TraderCTP::wrapOrderState(TThostFtdcOrderStatusType orderState)
{
	if (orderState == THOST_FTDC_OST_PartTradedNotQueueing)
		return WOS_Canceled;
	else if (orderState == THOST_FTDC_OST_Unknown)
		return WOS_Submitting;
	else
		return (WTSOrderState)orderState;
}

int TraderCTP::wrapActionFlag(WTSActionFlag actionFlag)
{
	if (WAF_CANCEL == actionFlag)
		return THOST_FTDC_AF_Delete;
	else
		return THOST_FTDC_AF_Modify;
}


/**
 * @brief 创建订单信息对象
 * @details 将CTP订单字段转换为内部使用的WTSOrderInfo对象
 *          包括合约信息、价格、数量、方向、开平标志、订单状态等
 *          同时维护订单ID与用户标签之间的映射关系
 * @param orderField CTP订单字段指针
 * @return WTSOrderInfo* 创建的订单信息对象指针，如果合约不存在则返回NULL
 */
WTSOrderInfo* TraderCTP::makeOrderInfo(CThostFtdcOrderField* orderField)
{
	WTSContractInfo* contract = m_bdMgr->getContract(orderField->InstrumentID, orderField->ExchangeID);
	if (contract == NULL)
		return NULL;

	WTSOrderInfo* pRet = WTSOrderInfo::create();
	pRet->setContractInfo(contract);
	pRet->setPrice(orderField->LimitPrice);
	pRet->setVolume(orderField->VolumeTotalOriginal);
	pRet->setDirection(wrapDirectionType(orderField->Direction, orderField->CombOffsetFlag[0]));
	pRet->setPriceType(wrapPriceType(orderField->OrderPriceType));
	pRet->setOffsetType(wrapOffsetType(orderField->CombOffsetFlag[0]));

	if (orderField->TimeCondition == THOST_FTDC_TC_GFD)
	{
		pRet->setOrderFlag(WOF_NOR);
	}
	else if (orderField->TimeCondition == THOST_FTDC_TC_IOC)
	{
		if (orderField->VolumeCondition == THOST_FTDC_VC_AV || orderField->VolumeCondition == THOST_FTDC_VC_MV)
			pRet->setOrderFlag(WOF_FAK);
		else
			pRet->setOrderFlag(WOF_FOK);
	}

	pRet->setVolTraded(orderField->VolumeTraded);
	pRet->setVolLeft(orderField->VolumeTotal);

	pRet->setCode(orderField->InstrumentID);
	pRet->setExchange(contract->getExchg());

	uint32_t uDate = strtoul(orderField->InsertDate, NULL, 10);
	std::string strTime = orderField->InsertTime;
	StrUtil::replace(strTime, ":", "");
	uint32_t uTime = strtoul(strTime.c_str(), NULL, 10);
	if (uTime >= 210000 && uDate == m_lDate)
	{
		uDate = TimeUtils::getNextDate(uDate, -1);
	}

	pRet->setOrderDate(uDate);
	pRet->setOrderTime(TimeUtils::makeTime(uDate, uTime * 1000));

	pRet->setOrderState(wrapOrderState(orderField->OrderStatus));
	if (orderField->OrderSubmitStatus >= THOST_FTDC_OSS_InsertRejected)
		pRet->setError(true);		

	generateEntrustID(pRet->getEntrustID(), orderField->FrontID, orderField->SessionID, atoi(orderField->OrderRef));
	pRet->setOrderID(orderField->OrderSysID);

	pRet->setStateMsg(orderField->StatusMsg);


	const char* usertag = m_eidCache.get(pRet->getEntrustID());
	if(strlen(usertag) == 0)
	{
		pRet->setUserTag(pRet->getEntrustID());
	}
	else
	{
		pRet->setUserTag(usertag);

		if (strlen(pRet->getOrderID()) > 0)
		{
			m_oidCache.put(StrUtil::trim(pRet->getOrderID()).c_str(), usertag, 0, [this](const char* message) {
				write_log(m_sink, LL_ERROR, message);
			});
		}
	}

	return pRet;
}

/**
 * @brief 创建委托对象
 * @details 将CTP订单插入字段转换为内部使用的WTSEntrust对象
 *          包括合约信息、价格、数量、交易所、交易方向、开平标志等
 *          同时生成委托ID并关联用户标签
 * @param entrustField CTP订单插入字段指针
 * @return WTSEntrust* 创建的委托对象指针，如果合约不存在则返回NULL
 */
WTSEntrust* TraderCTP::makeEntrust(CThostFtdcInputOrderField *entrustField)
{
	WTSContractInfo* ct = m_bdMgr->getContract(entrustField->InstrumentID, entrustField->ExchangeID);
	if (ct == NULL)
		return NULL;

	WTSEntrust* pRet = WTSEntrust::create(
		entrustField->InstrumentID,
		entrustField->VolumeTotalOriginal,
		entrustField->LimitPrice,
		ct->getExchg());

	pRet->setContractInfo(ct);

	pRet->setDirection(wrapDirectionType(entrustField->Direction, entrustField->CombOffsetFlag[0]));
	pRet->setPriceType(wrapPriceType(entrustField->OrderPriceType));
	pRet->setOffsetType(wrapOffsetType(entrustField->CombOffsetFlag[0]));
	
	if (entrustField->TimeCondition == THOST_FTDC_TC_GFD)
	{
		pRet->setOrderFlag(WOF_NOR);
	}
	else if (entrustField->TimeCondition == THOST_FTDC_TC_IOC)
	{
		if (entrustField->VolumeCondition == THOST_FTDC_VC_AV || entrustField->VolumeCondition == THOST_FTDC_VC_MV)
			pRet->setOrderFlag(WOF_FAK);
		else
			pRet->setOrderFlag(WOF_FOK);
	}

	//pRet->setEntrustID(generateEntrustID(m_frontID, m_sessionID, atoi(entrustField->OrderRef)).c_str());
	generateEntrustID(pRet->getEntrustID(), m_frontID, m_sessionID, atoi(entrustField->OrderRef));

	const char* usertag = m_eidCache.get(pRet->getEntrustID());
	if (strlen(usertag) > 0)
		pRet->setUserTag(usertag);

	return pRet;
}

/**
 * @brief 创建撤单操作对象
 * @details 将CTP撤单操作字段转换为内部使用的WTSEntrustAction对象
 *          包括合约代码、交易所、系统单号等信息
 *          同时生成委托ID并关联用户标签
 * @param actionField CTP撤单操作字段指针
 * @return WTSEntrustAction* 创建的撤单操作对象指针
 */
WTSEntrustAction* TraderCTP::makeAction(CThostFtdcInputOrderActionField *actionField)
{
	WTSEntrustAction* pRet = WTSEntrustAction::create(actionField->InstrumentID, actionField->ExchangeID);
	pRet->setOrderID(actionField->OrderSysID);

	generateEntrustID(pRet->getEntrustID(), actionField->FrontID, actionField->SessionID, atoi(actionField->OrderRef));

	const char* usertag = m_eidCache.get(pRet->getEntrustID());
	if (strlen(usertag) > 0)
		pRet->setUserTag(usertag);

	return pRet;
}

/**
 * @brief 创建错误信息对象
 * @details 将CTP响应信息字段转换为内部使用的WTSError对象
 *          格式化错误信息为错误消息和错误代码的组合
 * @param rspInfo CTP响应信息字段指针
 * @param ec 错误代码，默认为WEC_NONE
 * @return WTSError* 创建的错误信息对象指针
 */
WTSError* TraderCTP::makeError(CThostFtdcRspInfoField* rspInfo, WTSErroCode ec /* = WEC_NONE */)
{
	WTSError* pRet = WTSError::create(ec, fmtutil::format("{}({})", rspInfo->ErrorMsg, rspInfo->ErrorID));
	return pRet;
}

/**
 * @brief 创建成交信息对象
 * @details 将CTP成交字段转换为内部使用的WTSTradeInfo对象
 *          包括合约信息、成交价格、成交量、成交时间、成交ID等
 *          同时根据方向和开平标志确定交易类型，计算成交金额
 *          并从缓存中获取对应的用户标签
 * @param tradeField CTP成交字段指针
 * @return WTSTradeInfo* 创建的成交信息对象指针，如果合约不存在则返回NULL
 */
WTSTradeInfo* TraderCTP::makeTradeInfo(CThostFtdcTradeField *tradeField)
{
	WTSContractInfo* contract = m_bdMgr->getContract(tradeField->InstrumentID, tradeField->ExchangeID);
	if (contract == NULL)
		return NULL;

	WTSTradeInfo *pRet = WTSTradeInfo::create(tradeField->InstrumentID, contract->getExchg());
	pRet->setVolume(tradeField->Volume);
	pRet->setPrice(tradeField->Price);
	pRet->setTradeID(tradeField->TradeID);
	pRet->setContractInfo(contract);

	std::string strTime = tradeField->TradeTime;
	StrUtil::replace(strTime, ":", "");
	uint32_t uTime = strtoul(strTime.c_str(), NULL, 10);
	uint32_t uDate = strtoul(tradeField->TradeDate, NULL, 10);
	
	//如果是夜盘时间，并且成交日期等于交易日，说明成交日期是需要修正
	//因为夜盘是提前的，成交日期必然小于交易日
	//但是这里只能做一个简单修正了
	if(uTime >= 210000 && uDate == m_lDate)
	{
		uDate = TimeUtils::getNextDate(uDate, -1);
	}

	pRet->setTradeDate(uDate);
	pRet->setTradeTime(TimeUtils::makeTime(uDate, uTime * 1000));

	WTSDirectionType dType = wrapDirectionType(tradeField->Direction, tradeField->OffsetFlag);

	pRet->setDirection(dType);
	pRet->setOffsetType(wrapOffsetType(tradeField->OffsetFlag));
	pRet->setRefOrder(tradeField->OrderSysID);
	pRet->setTradeType((WTSTradeType)tradeField->TradeType);

	double amount = contract->getCommInfo()->getVolScale()*tradeField->Volume*pRet->getPrice();
	pRet->setAmount(amount);

	const char* usertag = m_oidCache.get(StrUtil::trim(pRet->getRefOrder()).c_str());
	if (strlen(usertag))
		pRet->setUserTag(usertag);

	return pRet;
}

/**
 * @brief 生成完整的委托ID
 * @details 根据前置机编号、会话编号和委托引用生成格式化的委托ID
 *          格式为“000001#0000000100#000123”，即“前置机编号#会话编号#委托引用”
 * @param buffer 接收生成的委托ID的缓冲区
 * @param frontid 前置机编号
 * @param sessionid 会话编号
 * @param orderRef 委托引用
 */
void TraderCTP::generateEntrustID(char* buffer, uint32_t frontid, uint32_t sessionid, uint32_t orderRef)
{
	fmtutil::format_to(buffer, "{:06d}#{:010d}#{:06d}", frontid, sessionid, orderRef);
}

/**
 * @brief 从委托ID中提取组件
 * @details 解析形如“000001#0000000100#000123”的委托ID字符串
 *          按照“前置机编号#会话编号#委托引用”的格式提取各部分
 *          并将提取的值保存到相应的输出参数中
 * @param entrustid 要解析的委托ID字符串
 * @param frontid 输出参数，存放提取的前置机编号
 * @param sessionid 输出参数，存放提取的会话编号
 * @param orderRef 输出参数，存放提取的委托引用
 * @return bool 如果解析成功返回true，否则返回false
 */
bool TraderCTP::extractEntrustID(const char* entrustid, uint32_t &frontid, uint32_t &sessionid, uint32_t &orderRef)
{
	thread_local static char buffer[64];
	wt_strcpy(buffer, entrustid);
	char* s = buffer;
	auto idx = StrUtil::findFirst(s, '#');
	if (idx == std::string::npos)
		return false;
	s[idx] = '\0';
	frontid = strtoul(s, NULL, 10);
	s += idx + 1;

	idx = StrUtil::findFirst(s, '#');
	if (idx == std::string::npos)
		return false;
	s[idx] = '\0';
	sessionid = strtoul(s, NULL, 10);
	s += idx + 1;

	orderRef = strtoul(s, NULL, 10);

	return true;
}

/**
 * @brief 判断响应信息是否包含错误
 * @details 检查CTP响应信息字段中的错误代码是否非零
 *          如果错误代码非零，则表示响应中包含错误
 * @param pRspInfo CTP响应信息字段指针
 * @return bool 如果包含错误返回true，否则返回false
 */
bool TraderCTP::IsErrorRspInfo(CThostFtdcRspInfoField *pRspInfo)
{
	if (pRspInfo && pRspInfo->ErrorID != 0)
		return true;

	return false;
}

/**
 * @brief 下单错误通知回调
 * @details 当报单在进入交易所前发生错误时，CTP交易服务器主动推送该回调
 *          将原始输入的订单字段和错误信息转换为内部委托对象和错误对象
 *          然后通过回调接口将错误信息推送给上层应用
 * @param pInputOrder CTP输入订单字段指针
 * @param pRspInfo 响应信息，包含错误代码和错误信息
 */
void TraderCTP::OnErrRtnOrderInsert(CThostFtdcInputOrderField *pInputOrder, CThostFtdcRspInfoField *pRspInfo)
{
	WTSEntrust* entrust = makeEntrust(pInputOrder);
	if (entrust)
	{
		WTSError *err = makeError(pRspInfo, WEC_ORDERINSERT);
		//g_orderMgr.onRspEntrust(entrust, err);
		if (m_sink)
			m_sink->onRspEntrust(entrust, err);
		entrust->release();
		err->release();
	}
}

/**
 * @brief 合约状态变化通知回调
 * @details 当合约交易状态发生变化时，CTP交易服务器主动推送该回调
 *          将合约交易所、代码和状态信息通过回调接口推送给上层应用
 *          状态包括开盘前、非交易、连续交易、集合竞价、集合竞价结束等
 * @param pInstrumentStatus CTP合约状态字段指针
 */
void TraderCTP::OnRtnInstrumentStatus(CThostFtdcInstrumentStatusField *pInstrumentStatus)
{
	if (m_sink)
		m_sink->onPushInstrumentStatus(pInstrumentStatus->ExchangeID, pInstrumentStatus->InstrumentID, (WTSTradeStatus)pInstrumentStatus->InstrumentStatus);
}

/**
 * @brief 检查交易接口连接状态
 * @details 判断当前交易接口是否处于全部就绪状态
 *          只有在当前状态为WS_ALLREADY时才认为连接成功
 * @return bool 如果已连接并就绪返回true，否则返回false
 */
bool TraderCTP::isConnected()
{
	return (m_wrapperState == WS_ALLREADY);
}

/**
 * @brief 查询结算单确认状态
 * @details 向CTP服务器发送查询结算单确认状态请求
 *          将查询操作加入查询队列，待触发执行
 *          只有在当前状态为WS_LOGINED(已登录)时才能执行
 * @return int 成功返回0，失败返回-1
 */
int TraderCTP::queryConfirm()
{
	if (m_pUserAPI == NULL || m_wrapperState != WS_LOGINED)
	{
		return -1;
	}

	{
		StdUniqueLock lock(m_mtxQuery);
		m_queQuery.push([this]() {
			CThostFtdcQrySettlementInfoConfirmField req;
			memset(&req, 0, sizeof(req));
			wt_strcpy(req.BrokerID, m_strBroker.c_str(), m_strBroker.size());
			wt_strcpy(req.InvestorID, m_strUser.c_str(), m_strUser.size());

			int iResult = m_pUserAPI->ReqQrySettlementInfoConfirm(&req, genRequestID());
			if (iResult != 0)
			{
				write_log(m_sink, LL_ERROR, "[TraderCTP][{}-{}] Sending query of settlement data confirming state failed: {}", m_strBroker.c_str(), m_strUser.c_str(), iResult);
			}
		});
	}

	//triggerQuery();

	return 0;
}

/**
 * @brief 确认结算单
 * @details 向CTP服务器发送结算单确认请求
 *          填充经纪商代码、投资者ID、当前日期和时间
 *          只有在包装器状态为WS_CONFIRM_QRYED(已查询结算单确认)时才能执行
 * @return int 成功返回0，失败返回-1
 */
int TraderCTP::confirm()
{
	if (m_pUserAPI == NULL || m_wrapperState != WS_CONFIRM_QRYED)
	{
		return -1;
	}

	//std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	CThostFtdcSettlementInfoConfirmField req;
	memset(&req, 0, sizeof(req));
	wt_strcpy(req.BrokerID, m_strBroker.c_str(), m_strBroker.size());
	wt_strcpy(req.InvestorID, m_strUser.c_str(), m_strUser.size());

	fmt::format_to(req.ConfirmDate, "{}", TimeUtils::getCurDate());
	memcpy(req.ConfirmTime, TimeUtils::getLocalTime().c_str(), 8);

	int iResult = m_pUserAPI->ReqSettlementInfoConfirm(&req, genRequestID());
	if (iResult != 0)
	{
		write_log(m_sink, LL_ERROR, "[TraderCTP][{}-{}] Sending confirming of settlement data failed: {}", m_strBroker.c_str(), m_strUser.c_str(), iResult);
		return -1;
	}

	return 0;
}

/**
 * @brief 进行交易账户认证
 * @details 在登录之前先进行账户认证
 *          创建认证请求并填充经纪商代码、用户名、授权码、应用ID等信息
 *          然后向CTP服务器发送认证请求
 * @return int 总是返回0，实际认证结果通过回调函数获知
 */
int TraderCTP::authenticate()
{
	CThostFtdcReqAuthenticateField req;
	memset(&req, 0, sizeof(req));
	wt_strcpy(req.BrokerID, m_strBroker.c_str(), m_strBroker.size());
	wt_strcpy(req.UserID, m_strUser.c_str(), m_strUser.size());
	//strcpy(req.UserProductInfo, m_strProdInfo.c_str());
	wt_strcpy(req.AuthCode, m_strAuthCode.c_str(), m_strAuthCode.size());
	wt_strcpy(req.AppID, m_strAppID.c_str(), m_strAppID.size());
	m_pUserAPI->ReqAuthenticate(&req, genRequestID());

	return 0;
}

/*
void TraderCTP::triggerQuery()
{
	m_strandIO->post([this](){
		if (m_queQuery.empty() || m_bInQuery)
			return;

		uint64_t curTime = TimeUtils::getLocalTimeNow();
		if (curTime - m_lastQryTime < 1000)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			m_strandIO->post([this](){
				triggerQuery();
			});
			return;
		}


		m_bInQuery = true;
		CommonExecuter& handler = m_queQuery.front();
		handler();

		{
			StdUniqueLock lock(m_mtxQuery);
			m_queQuery.pop();
		}

		m_lastQryTime = TimeUtils::getLocalTimeNow();
	});
}
*/