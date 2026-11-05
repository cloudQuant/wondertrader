/*!
 * \file ParserCTP.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "ParserCTP.h"

#include "../Includes/WTSVersion.h"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/IBaseDataMgr.h"

#include "../Share/ModuleHelper.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/ActionDayNormalizer.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/Converter.hpp"

#include <exception>
#include <filesystem>
namespace fs = std::filesystem;

 //By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"
template<typename... Args>
inline void write_log(IParserSpi* sink, WTSLogLevel ll, const char* format, const Args&... args) noexcept
{
	if (sink == NULL)
		return;

	static thread_local char buffer[512] = { 0 };
	fmtutil::format_to(buffer, format, args...);

	sink->handleParserLog(ll, buffer);
}

extern "C"
{
	EXPORT_FLAG IParserApi* createParser()
	{
		try { return new ParserCTP(); } catch (...) { return NULL; }
	}

	/*
	 *	卡9 9b：仅当没有回调/接口重入、且本实例从未创建过 SDK 对象时才删除；
	 *	否则保留指针（不置空），由宿主隔离到进程退出。
	 */
	EXPORT_FLAG void deleteParser(IParserApi* &parser)
	{
		if (NULL != parser)
		{
			ParserCTP* ctp = dynamic_cast<ParserCTP*>(parser);
			if (ctp == NULL || !ctp->removableLegacy())
				return;
			delete parser;
			parser = NULL;
		}
	}

	WT_PARSER_LIFECYCLE_V2_EXPORT const WtParserLifecycleApiV2* WT_PARSER_LIFECYCLE_V2_CALL getParserLifecycleApiV2(uint32_t major)
	{
		return wt_vendor_md_v2::Table<ParserCTP>::get(major);
	}
};


// UpdateTime 为 HH:MM:SS；使用局部缓冲，避免多实例/多回调线程共享静态缓冲的数据竞争
inline uint32_t strToTime(const char* strTime) noexcept
{
	char digits[16] = { 0 };
	int idx = 0;
	for (const char* c = strTime; c != NULL && *c != '\0' && idx < 15; ++c)
	{
		if ('0' <= *c && *c <= '9')
			digits[idx++] = *c;
	}
	digits[idx] = '\0';

	return convert::to_uint32(digits);
}

constexpr inline double checkValid(double val) noexcept
{
	return (val == DBL_MAX || val == FLT_MAX) ? 0 : val;
}

ParserCTP::ParserCTP()
	:m_uTradingDate(0)
    ,m_pUserAPI(NULL)
	,m_bLocaltime(false)
	,m_iRequestID(0)
	,m_sink(NULL)
	,m_pBaseDataMgr(NULL)
	,m_hInstCTP(NULL)
	,m_funcCreator(NULL)
	{
}


ParserCTP::~ParserCTP()
{
	// 在自身回调或接口调用内直接析构无法做到安全（栈上仍有本对象帧）：fail-stop。
	if (wt_vendor_md_v2::Lifecycle::reentered(this))
		std::terminate();
	(void)shutdownLegacy();
}

/*
 *	Parser lifecycle V2（卡9 9a=A+C）
 *	CTP 头文件（API/CTP6.6.9/ThostFtdcMdApi.h:93-103、API/CTP6.3.15/ThostFtdcMdApi.h:90-100）
 *	对 Release() 只写「删除接口对象本身」、对 Join() 只写「等待接口线程结束运行」，
 *	未声明 Release 会 join 回调线程或返回后不再回调；RegisterSpi（6.6.9:127-129）也无注销语义。
 *	因此一旦创建过 SDK 对象，排空状态不可证明：wait_drained 释放一次 SDK 后返回 ERROR，宿主隔离。
 */
WtParserLifecycleV2Status ParserCTP::requestStopV2() noexcept
{
	return _lc.requestStop();
}

WtParserLifecycleV2Status ParserCTP::canWaitV2() const noexcept
{
	return wt_vendor_md_v2::Lifecycle::reentered(this) ? WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK : WT_PARSER_LIFECYCLE_V2_OK;
}

WtParserLifecycleV2Status ParserCTP::waitDrainedV2(uint32_t timeoutMs) noexcept
{
	return _lc.waitDrained(canWaitV2(), timeoutMs,
		[this]() { return sdkSeen(); },
		[this]() { releaseSdk(); },
		[this]() {
			std::lock_guard<std::mutex> lock(_sdk_mutex);
			m_sink = NULL;
			m_pBaseDataMgr = NULL;
		});
}

bool ParserCTP::destroyableV2() const noexcept
{
	return _lc.destroyable(canWaitV2()) && !sdkSeen();
}

bool ParserCTP::removableLegacy() noexcept
{
	return shutdownLegacy() && !sdkSeen();
}

bool ParserCTP::sdkSeen() const noexcept
{
	return _lc.sdk_created.load(std::memory_order_acquire) || m_pUserAPI != NULL;
}

bool ParserCTP::shutdownLegacy() noexcept
{
	return _lc.shutdownLegacy(this, false, [this]() { releaseSdk(); });
}

void ParserCTP::releaseSdk() noexcept
{
	// 调用方保证：两道闸门已关闭且已排空，当前线程不在本实例回调/接口帧内
	std::lock_guard<std::mutex> lock(_sdk_mutex);
	CThostFtdcMdApi* api = m_pUserAPI;
	m_pUserAPI = NULL;
	if (api == NULL)
		return;
	_lc.sdk_created.store(true, std::memory_order_release);
	try
	{
		api->RegisterSpi(NULL);
		api->Release();
	}
	catch (...)
	{
	}
}

bool ParserCTP::init(WTSVariant* config)
{
	auto call = _lc.call(this);
	// 卡9 9c：重复初始化拒绝并保留原实例；停止后、回调内或无配置均拒绝
	if (!call || config == NULL || _init_done || sdkSeen() || _lc.shutDown() || _lc.insideCallback(this))
		return false;

	m_strFrontAddr = config->getCString("front");
	m_strBroker = config->getCString("broker");
	m_strUserID = config->getCString("user");
	m_strPassword = config->getCString("pass");
	m_strFlowDir = config->getCString("flowdir", "CTPMDFlow");
    /*
     * By Wesley @ 2022.03.09
     * 这个参数主要是给非标准CTP环境用的
     * 如simnow全天候行情，openctp等环境
     * 如果为true，就用本地时间戳，默认为false
     */
    m_bLocaltime = config->getBoolean("localtime");
    m_iMidnightMode = (int)wt_actday::parseMode(config->getCString("midnight_actionday"));
	m_strFlowDir = StrUtil::standardisePath(m_strFlowDir);

	std::string path = fmtutil::format("{}{}/{}/", m_strFlowDir, m_strBroker, m_strUserID);
	if (!StdFile::exists(path.c_str()))
	{
		fs::create_directories(fs::path(path));
	}	
#ifdef __APPLE__
	m_funcCreator = CThostFtdcMdApi::CreateFtdcMdApi;
#else
	const char* module = config->getCString("ctpmodule", "thostmduserapi_se");
	std::string dllpath = getBinDir() + DLLHelper::wrap_module(module, "");
	m_hInstCTP = DLLHelper::load_library(dllpath.c_str());
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
#endif
	if (m_funcCreator == NULL)
	{
		write_log(m_sink, LL_ERROR, "[ParserCTP] Market data API creator was not found");
		return false;
	}
	CThostFtdcMdApi* api = m_funcCreator(path.c_str(), false, false);
	if (api == NULL)
	{
		write_log(m_sink, LL_ERROR, "[ParserCTP] Market data API creator returned null");
		return false;
	}
	// 从此刻起存在 SDK 对象：本实例的排空不再可证明（见 requestStopV2 上方说明）
	_lc.sdk_created.store(true, std::memory_order_release);
	m_pUserAPI = api;
	m_pUserAPI->RegisterSpi(this);
	m_pUserAPI->RegisterFront((char*)m_strFrontAddr.c_str());
	_init_done = true;

	return true;
}

void ParserCTP::release()
{
	(void)shutdownLegacy();
}

bool ParserCTP::connect()
{
	auto call = _lc.call(this);
	// 卡9 9d：回调内同步重入 connect 返回 busy(false)
	if (!call || _lc.shutDown() || _lc.insideCallback(this))
		return false;

	if(m_pUserAPI)
	{
		// SDK 的 Init 只允许调用一次；重复 connect 不再触发
		if (_connect_issued)
			return false;
		_connect_issued = true;
		m_pUserAPI->Init();
	}

	return true;
}

bool ParserCTP::disconnect()
{
	// 终止性关闭：回调/接口重入时只关闭准入并延后 SDK 释放，返回 false（卡9 9d）
	return shutdownLegacy();
}

void ParserCTP::OnRspError( CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool /*bIsLast*/ )
{
	auto cb = _lc.callback(this);
	if (!cb)
		return;
	IsErrorRspInfo(pRspInfo);
}

void ParserCTP::OnFrontConnected()
{
	auto cb = _lc.callback(this);
	if (!cb)
		return;
	if(m_sink)
	{
		write_log(m_sink, LL_INFO, "[ParserCTP] Market data server connected");
		m_sink->handleEvent(WPE_Connect, 0);
	}

	ReqUserLogin();
}

void ParserCTP::OnRspUserLogin( CThostFtdcRspUserLoginField */*pRspUserLogin*/, CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool bIsLast )
{
	auto cb = _lc.callback(this);
	if (!cb)
		return;
	if(bIsLast && !IsErrorRspInfo(pRspInfo))
	{
		uint32_t tradingDate = convert::to_uint32(m_pUserAPI->GetTradingDay());
        //By Wesley @ 2022.03.09
        //这里加一个判断，但是这样的交易日不准确，在夜盘会出错
        if(tradingDate == 0)
            tradingDate = TimeUtils::getCurDate();
		{
			std::lock_guard<std::recursive_mutex> lock(_data_mutex);
			m_uTradingDate = tradingDate;
		}
		
		write_log(m_sink, LL_INFO, "[ParserCTP] Market data server logined, {}", m_uTradingDate);

		if(m_sink)
		{
			m_sink->handleEvent(WPE_Login, 0);
		}

		//订阅行情数据
		DoSubscribeMD();
	}
}

void ParserCTP::OnRspUserLogout(CThostFtdcUserLogoutField */*pUserLogout*/, CThostFtdcRspInfoField */*pRspInfo*/, int /*nRequestID*/, bool /*bIsLast*/)
{
	auto cb = _lc.callback(this);
	if (!cb)
		return;
	if(m_sink)
	{
		m_sink->handleEvent(WPE_Logout, 0);
	}
}

void ParserCTP::OnFrontDisconnected( int nReason )
{
	auto cb = _lc.callback(this);
	if (!cb)
		return;
	if(m_sink)
	{
		write_log(m_sink, LL_ERROR, "[ParserCTP] Market data server disconnected: {}", nReason);
		m_sink->handleEvent(WPE_Close, 0);
	}
}

void ParserCTP::OnRspUnSubMarketData( CThostFtdcSpecificInstrumentField */*pSpecificInstrument*/, CThostFtdcRspInfoField */*pRspInfo*/, int /*nRequestID*/, bool /*bIsLast*/ )
{
	auto cb = _lc.callback(this);
	if (!cb)
		return;

}

void ParserCTP::OnRtnDepthMarketData( CThostFtdcDepthMarketDataField *pDepthMarketData )
{	
	auto cb = _lc.callback(this);
	if (!cb)
		return;
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
        actDate = convert::to_uint32(pDepthMarketData->ActionDay);
        actTime = strToTime(pDepthMarketData->UpdateTime) * 1000 + pDepthMarketData->UpdateMillisec;
        actHour = actTime / 10000000;

        //夜盘时发生日期不可能等于交易日,需按本地时钟纠偏（卡3 3b=C，默认 A，见 ActionDayNormalizer.hpp）
        if (actDate == m_uTradingDate && wt_actday::needsLocalClock(actHour, (wt_actday::MidnightMode)m_iMidnightMode)) {
            uint32_t curDate, curTime;
            wt_actday::localDateTime(curDate, curTime);
            if (!wt_actday::normalize(actDate, actHour, m_uTradingDate, curDate, curTime / 10000000,
                (wt_actday::MidnightMode)m_iMidnightMode))
                return;
        }
    }

	WTSCommodityInfo* pCommInfo = contract->getCommInfo();

	WTSTickData* tick = WTSTickData::create(contract->getCode());
	if (tick == NULL)
		return;
	tick->setContractInfo(contract);

	WTSTickStruct& quote = tick->getTickStruct();
	strcpy(quote.exchg, pCommInfo->getExchg());
	
	quote.action_date = actDate;
	quote.action_time = actTime;
	
	quote.price = checkValid(pDepthMarketData->LastPrice);
	quote.open = checkValid(pDepthMarketData->OpenPrice);
	quote.high = checkValid(pDepthMarketData->HighestPrice);
	quote.low = checkValid(pDepthMarketData->LowestPrice);
	quote.total_volume = (uint32_t)pDepthMarketData->Volume;
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

void ParserCTP::OnRspSubMarketData( CThostFtdcSpecificInstrumentField */*pSpecificInstrument*/, CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool /*bIsLast*/ )
{
	auto cb = _lc.callback(this);
	if (!cb)
		return;
	if(!IsErrorRspInfo(pRspInfo))
	{

	}
	else
	{

	}
}

void ParserCTP::OnHeartBeatWarning( int nTimeLapse )
{
	auto cb = _lc.callback(this);
	if (!cb)
		return;
	if(m_sink)
		write_log(m_sink, LL_INFO, "[ParserCTP] Heartbeating, elapse: {}", nTimeLapse);
}

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

void ParserCTP::DoSubscribeMD()
{
	std::lock_guard<std::recursive_mutex> lock(_data_mutex);
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

bool ParserCTP::IsErrorRspInfo(CThostFtdcRspInfoField *pRspInfo)
{
	// I03-NEW-132 (BUG-R105-004): same predicate as ParserCTPOpt/ParserFemas/ParserCTPMini;
	// a response is an error only when it carries info with a non-zero ErrorID.
	if (pRspInfo == NULL || pRspInfo->ErrorID == 0)
		return false;

	if (m_sink)
		write_log(m_sink, LL_ERROR, "[ParserCTP] API response failed with error id {}", pRspInfo->ErrorID);
	return true;
}

void ParserCTP::subscribe(const CodeSet &vecSymbols)
{
	auto call = _lc.call(this);
	if (!call)
		return;
	std::lock_guard<std::recursive_mutex> lock(_data_mutex);
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

void ParserCTP::unsubscribe(const CodeSet &/*vecSymbols*/)
{
}

bool ParserCTP::isConnected()
{
	auto call = _lc.call(this);
	return call && m_pUserAPI!=NULL;
}

void ParserCTP::registerSpi(IParserSpi* listener)
{
	auto call = _lc.call(this);
	if (!call)
		return;
	m_sink = listener;

	if(m_sink)
		m_pBaseDataMgr = m_sink->getBaseDataMgr();
}
