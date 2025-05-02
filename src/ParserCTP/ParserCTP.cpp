/*!
 * \file ParserCTP.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief CTP行情解析器实现文件
 * \details 本文件实现了与上期所CTP行情接口对接的ParserCTP类
 *          实现了连接、登录、订阅、行情数据解析等功能
 *          将CTP行情数据转换为内部使用的数据格式
 */
#include "ParserCTP.h"

#include "../Includes/WTSVersion.h"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/IBaseDataMgr.h"

#include "../Share/ModuleHelper.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/StdUtils.hpp"

#include <boost/filesystem.hpp>

 //By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"
/**
 * @brief 格式化写入日志
 * @details 将格式化的日志信息写入到行情解析器回调中
 * @tparam Args 格式化参数类型
 * @param sink 行情解析器回调对象
 * @param ll 日志级别
 * @param format 格式化字符串
 * @param args 参数列表
 */
template<typename... Args>
inline void write_log(IParserSpi* sink, WTSLogLevel ll, const char* format, const Args&... args)
{
	if (sink == NULL)
		return;

	static thread_local char buffer[512] = { 0 };
	fmtutil::format_to(buffer, format, args...);

	sink->handleParserLog(ll, buffer);
}

/**
 * @brief DLL导出函数声明
 * @details 声明供动态链接库调用的C接口函数
 */
extern "C"
{
	/**
	 * @brief 创建行情解析器实例
	 * @details 创建一个ParserCTP实例并返回接口指针
	 * @return IParserApi* 新创建的行情接口实例
	 */
	EXPORT_FLAG IParserApi* createParser()
	{
		ParserCTP* parser = new ParserCTP();
		return parser;
	}

	/**
	 * @brief 删除行情解析器实例
	 * @details 删除指定的行情解析器实例并将指针置空
	 * @param parser 要删除的行情解析器实例指针引用
	 */
	EXPORT_FLAG void deleteParser(IParserApi* &parser)
	{
		if (NULL != parser)
		{
			delete parser;
			parser = NULL;
		}
	}
};


/**
 * @brief 将时间格式字符串转换为无格式整数
 * @details 将含有分隔符":"的时间格式字符串转换为整数
 *          例如将"09:30:15"转换为"93015"
 * @param strTime 要转换的时间字符串
 * @return uint32_t 转换后的整数时间
 */
inline uint32_t strToTime(const char* strTime)
{
	static char str[10] = { 0 };
	const char *pos = strTime;
	int idx = 0;
	auto len = strlen(strTime);
	for(std::size_t i = 0; i < len; i++)
	{
		if(strTime[i] != ':')
		{
			str[idx] = strTime[i];
			idx++;
		}
	}
	str[idx] = '\0';

	return strtoul(str, NULL, 10);
}

/**
 * @brief 检查并处理无效的数值
 * @details 检查输入的浮点数是否为特定的无效值（DBL_MAX或FLT_MAX）
 *          如果是无效值，则返回0，否则返回原值
 *          在行情处理中用于过滤CTP接口返回的异常数值
 * @param val 要检查的浮点数值
 * @return double 处理后的有效数值
 */
inline double checkValid(double val)
{
	if (val == DBL_MAX || val == FLT_MAX)
		return 0;

	return val;
}

/**
 * @brief 构造函数
 * @details 初始化ParserCTP对象的成员变量
 *          将API指针、请求ID、交易日等置为初始值
 */
ParserCTP::ParserCTP()
	:m_pUserAPI(NULL)
	,m_iRequestID(0)
	,m_uTradingDate(0)
    ,m_bLocaltime(false)
{
}


/**
 * @brief 析构函数
 * @details 清理ParserCTP对象资源
 *          将API指针置空，实际资源释放在release方法中进行
 */
ParserCTP::~ParserCTP()
{
	m_pUserAPI = NULL;
}

/**
 * @brief 初始化行情解析器
 * @details 从配置中读取各种参数并初始化行情解析器
 *          加载CTP行情接口动态链接库
 *          创建CTP行情API实例
 *          注册回调接口和前置机地址
 * @param config 包含初始化参数的变量集合
 * @return bool 初始化是否成功，总是返回true
 */
bool ParserCTP::init(WTSVariant* config)
{
	m_strFrontAddr = config->getCString("front");
	m_strBroker = config->getCString("broker");
	m_strUserID = config->getCString("user");
	m_strPassword = config->getCString("pass");
	m_strFlowDir = config->getCString("flowdir");
    /*
     * By Wesley @ 2022.03.09
     * 这个参数主要是给非标准CTP环境用的
     * 如simnow全天候行情，openctp等环境
     * 如果为true，就用本地时间戳，默认为false
     */
    m_bLocaltime = config->getBoolean("localtime");

	if (m_strFlowDir.empty())
		m_strFlowDir = "CTPMDFlow";

	m_strFlowDir = StrUtil::standardisePath(m_strFlowDir);

	std::string module = config->getCString("ctpmodule");
	if (module.empty())
		module = "thostmduserapi_se";

	std::string dllpath = getBinDir() + DLLHelper::wrap_module(module.c_str(), "");
	m_hInstCTP = DLLHelper::load_library(dllpath.c_str());
	std::string path = StrUtil::printf("%s%s/%s/", m_strFlowDir.c_str(), m_strBroker.c_str(), m_strUserID.c_str());
	if (!StdFile::exists(path.c_str()))
	{
		boost::filesystem::create_directories(boost::filesystem::path(path));
	}	
#ifdef _WIN32
#	ifdef _WIN64
	const char* creatorName = "?CreateFtdcMdApi@CThostFtdcMdApi@@SAPEAV1@PEBD_N1@Z";
#	else
	const char* creatorName = "?CreateFtdcMdApi@CThostFtdcMdApi@@SAPAV1@PBD_N1@Z";
#	endif
#else
	const char* creatorName = "_ZN15CThostFtdcMdApi15CreateFtdcMdApiEPKcbb";
#endif
	m_funcCreator = (CTPCreator)DLLHelper::get_symbol(m_hInstCTP, creatorName);
	m_pUserAPI = m_funcCreator(path.c_str(), false, false);
	m_pUserAPI->RegisterSpi(this);
	m_pUserAPI->RegisterFront((char*)m_strFrontAddr.c_str());

	return true;
}

/**
 * @brief 释放行情解析器资源
 * @details 调用disconnect方法断开与CTP服务器的连接并释放相关资源
 */
void ParserCTP::release()
{
	disconnect();
}

/**
 * @brief 连接到CTP行情服务器
 * @details 如果CTP API实例已创建，则初始化该实例
 *          这将触发异步连接过程
 *          连接成功后会回调OnFrontConnected方法
 * @return bool 连接请求是否成功发送，总是返回true
 */
bool ParserCTP::connect()
{
	if(m_pUserAPI)
	{
		m_pUserAPI->Init();
	}

	return true;
}

/**
 * @brief 断开与CTP行情服务器的连接
 * @details 如果CTP API实例存在，则取消回调注册并释放API资源
 *          将API指针置空以防止重复释放
 * @return bool 断开连接是否成功，总是返回true
 */
bool ParserCTP::disconnect()
{
	if(m_pUserAPI)
	{
		m_pUserAPI->RegisterSpi(NULL);
		m_pUserAPI->Release();
		m_pUserAPI = NULL;
	}

	return true;
}

/**
 * @brief 响应错误回调
 * @details 当CTP行情接口返回错误时触发该回调
 *          调用IsErrorRspInfo方法处理错误信息
 * @param pRspInfo 错误信息字段指针
 * @param nRequestID 对应的请求ID
 * @param bIsLast 是否为最后一个响应
 */
void ParserCTP::OnRspError( CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast )
{
	IsErrorRspInfo(pRspInfo);
}

/**
 * @brief 前置机连接成功回调
 * @details 当成功连接到CTP前置机时触发该回调
 *          记录连接成功日志，并通过回调接口通知上层应用
 *          然后自动发送登录请求
 */
void ParserCTP::OnFrontConnected()
{
	if(m_sink)
	{
		write_log(m_sink, LL_INFO, "[ParserCTP] Market data server connected");
		m_sink->handleEvent(WPE_Connect, 0);
	}

	ReqUserLogin();
}
/**
 * @brief 用户登录响应回调
 * @details 当发送登录请求后收到的响应
 *          如果登录成功，获取交易日并处理无效交易日的情况
 *          记录登录成功日志并通知上层应用
 *          然后自动执行行情订阅
 * @param pRspUserLogin 登录响应字段指针
 * @param pRspInfo 响应信息字段指针
 * @param nRequestID 请求ID
 * @param bIsLast 是否为最后一个响应
 */
void ParserCTP::OnRspUserLogin( CThostFtdcRspUserLoginField *pRspUserLogin, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast )
{
	if(bIsLast && !IsErrorRspInfo(pRspInfo))
	{
		m_uTradingDate = strtoul(m_pUserAPI->GetTradingDay(), NULL, 10);
        //By Wesley @ 2022.03.09
        //这里加一个判断，但是这样的交易日不准确，在夜盘会出错
        if(m_uTradingDate == 0)
            m_uTradingDate = TimeUtils::getCurDate();
		
		write_log(m_sink, LL_INFO, "[ParserCTP] Market data server logined, {}", m_uTradingDate);

		if(m_sink)
		{
			m_sink->handleEvent(WPE_Login, 0);
		}

		//订阅行情数据
		DoSubscribeMD();
	}
}

/**
 * @brief 用户登出响应回调
 * @details 当发送登出请求后收到的响应
 *          通过回调接口将登出事件通知上层应用
 * @param pUserLogout 登出响应字段指针
 * @param pRspInfo 响应信息字段指针
 * @param nRequestID 请求ID
 * @param bIsLast 是否为最后一个响应
 */
void ParserCTP::OnRspUserLogout(CThostFtdcUserLogoutField *pUserLogout, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if(m_sink)
	{
		m_sink->handleEvent(WPE_Logout, 0);
	}
}

/**
 * @brief 前置机断开连接回调
 * @details 当与CTP前置机断开连接时触发该回调
 *          记录断开连接错误日志，包含断开原因
 *          通过回调接口将断开事件通知上层应用
 * @param nReason 断开连接的原因代码
 */
void ParserCTP::OnFrontDisconnected( int nReason )
{
	if(m_sink)
	{
		write_log(m_sink, LL_ERROR, "[ParserCTP] Market data server disconnected: {}", nReason);
		m_sink->handleEvent(WPE_Close, 0);
	}
}

/**
 * @brief 取消订阅行情响应回调
 * @details 当发送取消订阅行情请求后收到的响应
 *          当前实现中未处理此响应
 * @param pSpecificInstrument 合约信息字段指针
 * @param pRspInfo 响应信息字段指针
 * @param nRequestID 请求ID
 * @param bIsLast 是否为最后一个响应
 */
void ParserCTP::OnRspUnSubMarketData( CThostFtdcSpecificInstrumentField *pSpecificInstrument, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast )
{

}

/**
 * @brief 行情数据推送回调
 * @details 当收到CTP推送的实时行情数据时触发该回调
 *          处理流程包括：
 *          1. 验证合约是否有效
 *          2. 根据配置决定使用本地时间或CTP提供的时间
 *          3. 将CTP行情数据转换为内部行情数据结构
 *          4. 调用回调接口将行情数据推送给上层应用
 * @param pDepthMarketData CTP行情数据字段指针
 */
void ParserCTP::OnRtnDepthMarketData( CThostFtdcDepthMarketDataField *pDepthMarketData )
{	
	if(m_pBaseDataMgr == NULL)
	{
		return;
	}

    WTSContractInfo* contract = m_pBaseDataMgr->getContract(pDepthMarketData->InstrumentID, pDepthMarketData->ExchangeID);
    if (contract == NULL)
        return;

    uint32_t actDate, actTime, actHour;

    if(m_bLocaltime)
    {
        TimeUtils::getDateTime(actDate, actTime);
        actHour = actTime / 10000000;
    }
    else
    {
        actDate = strtoul(pDepthMarketData->ActionDay, NULL, 10);
        actTime = strToTime(pDepthMarketData->UpdateTime) * 1000 + pDepthMarketData->UpdateMillisec;
        actHour = actTime / 10000000;

        if (actDate == m_uTradingDate && actHour >= 20) {
            //这样的时间是有问题,因为夜盘时发生日期不可能等于交易日
            //这就需要手动设置一下
            uint32_t curDate, curTime;
            TimeUtils::getDateTime(curDate, curTime);
            uint32_t curHour = curTime / 10000000;

            //早上启动以后,会收到昨晚12点以前收盘的行情,这个时候可能会有发生日期=交易日的情况出现
            //这笔数据直接丢掉
            if (curHour >= 3 && curHour < 9)
                return;

            actDate = curDate;

            if (actHour == 23 && curHour == 0) {
                //行情时间慢于系统时间
                actDate = TimeUtils::getNextDate(curDate, -1);
            } else if (actHour == 0 && curHour == 23) {
                //系统时间慢于行情时间
                actDate = TimeUtils::getNextDate(curDate, 1);
            }
        }
    }

	WTSCommodityInfo* pCommInfo = contract->getCommInfo();

	WTSTickData* tick = WTSTickData::create(pDepthMarketData->InstrumentID);
	tick->setContractInfo(contract);

	WTSTickStruct& quote = tick->getTickStruct();
	strcpy(quote.exchg, pCommInfo->getExchg());
	
	quote.action_date = actDate;
	quote.action_time = actTime;
	
	quote.price = checkValid(pDepthMarketData->LastPrice);
	quote.open = checkValid(pDepthMarketData->OpenPrice);
	quote.high = checkValid(pDepthMarketData->HighestPrice);
	quote.low = checkValid(pDepthMarketData->LowestPrice);
	quote.total_volume = pDepthMarketData->Volume;
	quote.trading_date = m_uTradingDate;
	if(pDepthMarketData->SettlementPrice != DBL_MAX)
		quote.settle_price = checkValid(pDepthMarketData->SettlementPrice);
	if(strcmp(quote.exchg, "CZCE") == 0)
	{
		quote.total_turnover = pDepthMarketData->Turnover*pCommInfo->getVolScale();
	}
	else
	{
		if(pDepthMarketData->Turnover != DBL_MAX)
			quote.total_turnover = pDepthMarketData->Turnover;
	}

	quote.open_interest = pDepthMarketData->OpenInterest;

	quote.upper_limit = checkValid(pDepthMarketData->UpperLimitPrice);
	quote.lower_limit = checkValid(pDepthMarketData->LowerLimitPrice);

	quote.pre_close = checkValid(pDepthMarketData->PreClosePrice);
	quote.pre_settle = checkValid(pDepthMarketData->PreSettlementPrice);
	quote.pre_interest = pDepthMarketData->PreOpenInterest;

	//委卖价格
	quote.ask_prices[0] = checkValid(pDepthMarketData->AskPrice1);
	quote.ask_prices[1] = checkValid(pDepthMarketData->AskPrice2);
	quote.ask_prices[2] = checkValid(pDepthMarketData->AskPrice3);
	quote.ask_prices[3] = checkValid(pDepthMarketData->AskPrice4);
	quote.ask_prices[4] = checkValid(pDepthMarketData->AskPrice5);

	//委买价格
	quote.bid_prices[0] = checkValid(pDepthMarketData->BidPrice1);
	quote.bid_prices[1] = checkValid(pDepthMarketData->BidPrice2);
	quote.bid_prices[2] = checkValid(pDepthMarketData->BidPrice3);
	quote.bid_prices[3] = checkValid(pDepthMarketData->BidPrice4);
	quote.bid_prices[4] = checkValid(pDepthMarketData->BidPrice5);

	//委卖量
	quote.ask_qty[0] = pDepthMarketData->AskVolume1;
	quote.ask_qty[1] = pDepthMarketData->AskVolume2;
	quote.ask_qty[2] = pDepthMarketData->AskVolume3;
	quote.ask_qty[3] = pDepthMarketData->AskVolume4;
	quote.ask_qty[4] = pDepthMarketData->AskVolume5;

	//委买量
	quote.bid_qty[0] = pDepthMarketData->BidVolume1;
	quote.bid_qty[1] = pDepthMarketData->BidVolume2;
	quote.bid_qty[2] = pDepthMarketData->BidVolume3;
	quote.bid_qty[3] = pDepthMarketData->BidVolume4;
	quote.bid_qty[4] = pDepthMarketData->BidVolume5;

	if(m_sink)
		m_sink->handleQuote(tick, 1);

	tick->release();
}

/**
 * @brief 订阅行情响应回调
 * @details 当发送订阅行情请求后收到的响应
 *          检查响应是否包含错误信息
 *          当前实现中空处理，可用于日志或状态跟踪
 * @param pSpecificInstrument 合约信息字段指针
 * @param pRspInfo 响应信息字段指针
 * @param nRequestID 请求ID
 * @param bIsLast 是否为最后一个响应
 */
void ParserCTP::OnRspSubMarketData( CThostFtdcSpecificInstrumentField *pSpecificInstrument, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast )
{
	if(!IsErrorRspInfo(pRspInfo))
	{

	}
	else
	{

	}
}

/**
 * @brief 心跳超时警告回调
 * @details 当CTP行情服务器心跳超时时触发该回调
 *          记录心跳超时日志，包含超时的时间间隔
 * @param nTimeLapse 心跳超时的时间间隔（单位：秒）
 */
void ParserCTP::OnHeartBeatWarning( int nTimeLapse )
{
	if(m_sink)
		write_log(m_sink, LL_INFO, "[ParserCTP] Heartbeating, elapse: {}", nTimeLapse);
}

/**
 * @brief 发送登录请求
 * @details 向CTP行情服务器发送登录请求
 *          填充登录所需的各个字段：经纪ID、用户ID、密码等
 *          如果用户API对象为空或请求发送失败，记录相应错误日志
 *          登录结果将在OnRspUserLogin回调中处理
 */
void ParserCTP::ReqUserLogin()
{
	if(m_pUserAPI == NULL)
	{
		return;
	}

	CThostFtdcReqUserLoginField req;
	memset(&req, 0, sizeof(req));
	strcpy(req.BrokerID, m_strBroker.c_str());
	strcpy(req.UserID, m_strUserID.c_str());
	strcpy(req.Password, m_strPassword.c_str());
	strcpy(req.UserProductInfo, WT_PRODUCT);
	int iResult = m_pUserAPI->ReqUserLogin(&req, ++m_iRequestID);
	if(iResult != 0)
	{
		if(m_sink)
			write_log(m_sink, LL_ERROR, "[ParserCTP] Sending login request failed: {}", iResult);
	}
}

/**
 * @brief 执行市场行情订阅
 * @details 根据过滤集合发送行情订阅请求
 *          处理流程包括：
 *          1. 检查是否有合约要订阅
 *          2. 处理合约代码格式，去除交易所前缀
 *          3. 调用CTP API发送订阅请求
 *          4. 记录订阅结果日志
 */
void ParserCTP::DoSubscribeMD()
{
	CodeSet codeFilter = m_filterSubs;
	if(codeFilter.empty())
	{//如果订阅礼包只空的,则取出全部合约列表
		return;
	}

	char ** subscribe = new char*[codeFilter.size()];
	int nCount = 0;
	for(auto& code : codeFilter)
	{
		std::size_t pos = code.find('.');
		if (pos != std::string::npos)
			subscribe[nCount++] = (char*)code.c_str() + pos + 1;
		else
			subscribe[nCount++] = (char*)code.c_str();
	}

	if(m_pUserAPI && nCount > 0)
	{
		int iResult = m_pUserAPI->SubscribeMarketData(subscribe, nCount);
		if(iResult != 0)
		{
			if(m_sink)
				write_log(m_sink, LL_ERROR, "[ParserCTP] Sending md subscribe request failed: {}", iResult);
		}
		else
		{
			if(m_sink)
				write_log(m_sink, LL_INFO, "[ParserCTP] Market data of {} contracts subscribed totally", nCount);
		}
	}
	codeFilter.clear();
	delete[] subscribe;
}

/**
 * @brief 检查响应信息是否包含错误
 * @details 当前实现总是返回false，表示无错误
 *          在实际应用中可能需要完善实现，检查pRspInfo是否为空
 *          以及ErrorID是否为0来判断是否发生错误
 * @param pRspInfo CTP响应信息字段指针
 * @return bool 如果有错误返回true，无错误返回false
 */
bool ParserCTP::IsErrorRspInfo(CThostFtdcRspInfoField *pRspInfo)
{
	return false;
}

/**
 * @brief 订阅行情
 * @details 接受上层要订阅的合约代码集合
 *          如果已登录（交易日已设置）则立即执行订阅
 *          否则仅保存订阅列表，等待登录成功后自动订阅
 * @param vecSymbols 要订阅的合约代码集合
 */
void ParserCTP::subscribe(const CodeSet &vecSymbols)
{
	if(m_uTradingDate == 0)
	{
		m_filterSubs = vecSymbols;
	}
	else
	{
		m_filterSubs = vecSymbols;
		DoSubscribeMD();
	}
}

/**
 * @brief 取消行情订阅
 * @details 取消指定合约的行情订阅
 *          当前实现中未实现该功能
 * @param vecSymbols 要取消订阅的合约代码集合
 */
void ParserCTP::unsubscribe(const CodeSet &vecSymbols)
{
}

/**
 * @brief 检查是否已连接
 * @details 检查CTP行情API实例是否已创建
 *          注意这只检查API实例是否存在，不能决定连接是否活跃
 * @return bool 如果API实例已创建则返回true，否则返回false
 */
bool ParserCTP::isConnected()
{
	return m_pUserAPI!=NULL;
}

/**
 * @brief 注册行情回调接口
 * @details 设置接收行情数据和事件通知的回调接口
 *          同时从回调接口获取基础数据管理器
 *          基础数据管理器用于获取合约信息
 * @param listener 实现了IParserSpi接口的回调对象指针
 */
void ParserCTP::registerSpi(IParserSpi* listener)
{
	m_sink = listener;

	if(m_sink)
		m_pBaseDataMgr = m_sink->getBaseDataMgr();
}