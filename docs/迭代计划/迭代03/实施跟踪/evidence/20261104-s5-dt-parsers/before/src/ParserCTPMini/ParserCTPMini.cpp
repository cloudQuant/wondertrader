/*!
 * \file ParserCTPMini.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "ParserCTPMini.h"
#include "../Share/StrUtil.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/ActionDayNormalizer.hpp"
#include "../Share/ModuleHelper.hpp"
#include "../Share/Converter.hpp"

#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/WTSVersion.h"

#include <exception>
#include <mutex>
#include <filesystem>
#include <limits>
#include <system_error>
#include <vector>
namespace fs = std::filesystem;

 //By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"
template<typename... Args>
inline void write_log(IParserSpi* sink, WTSLogLevel ll, const char* format, const Args&... args)
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
		try { return new ParserCTPMini(); } catch (...) { return NULL; }
	}

	/*
	 *	卡9 9b：仅当没有回调/接口重入、且本实例从未创建过 SDK 对象时才删除；
	 *	否则保留指针（不置空），由宿主隔离到进程退出。
	 */
	EXPORT_FLAG void deleteParser(IParserApi* &parser)
	{
		if (NULL != parser)
		{
			ParserCTPMini* typed = dynamic_cast<ParserCTPMini*>(parser);
			if (typed == NULL || !typed->removableLegacy())
				return;
			delete parser;
			parser = NULL;
		}
	}

	WT_PARSER_LIFECYCLE_V2_EXPORT const WtParserLifecycleApiV2* WT_PARSER_LIFECYCLE_V2_CALL getParserLifecycleApiV2(uint32_t major)
	{
		return wt_vendor_md_v2::Table<ParserCTPMini>::get(major);
	}
};


uint32_t strToTime(const char* strTime)
{
	std::string str;
	const char *pos = strTime;
	while(strlen(pos) > 0)
	{
		if(pos[0] != ':')
		{
			str.append(pos, 1);
		}
		pos++;
	}

	return convert::to_uint32(str.c_str());
}

inline double checkValid(double val)
{
	if (val == DBL_MAX || val == FLT_MAX)
		return 0;

	return val;
}

ParserCTPMini::ParserCTPMini()
	:m_uTradingDate(0)
,m_pUserAPI(NULL)
	,m_iRequestID(0)
	,m_sink(NULL)
	,m_pBaseDataMgr(NULL)
	,m_hInstCTP(NULL)
	,m_funcCreator(NULL)
	{
}


ParserCTPMini::~ParserCTPMini()
{
	// 在自身回调或接口调用内直接析构无法做到安全（栈上仍有本对象帧）：fail-stop。
	if (wt_vendor_md_v2::Lifecycle::reentered(this))
		std::terminate();
	(void)shutdownLegacy();
}

/*
 *	Parser lifecycle V2（卡9 9a=A+C）
 *	CTPMini 头文件 API/CTPMini1.5.8/ThostFtdcMdApi.h:90-100 对 Release() 只写「删除接口对象本身」、
 *	对 Join() 只写「等待接口线程结束运行」；RegisterSpi（:113-115）无注销语义，均未声明回调线程 join 或返回后不再回调。
 *	因此一旦创建过 SDK 对象，排空状态不可证明：wait_drained 释放一次 SDK 后返回 ERROR，宿主隔离到进程退出。
 */
WtParserLifecycleV2Status ParserCTPMini::requestStopV2() noexcept
{
	return _lc.requestStop();
}

WtParserLifecycleV2Status ParserCTPMini::canWaitV2() const noexcept
{
	return (wt_vendor_md_v2::Lifecycle::reentered(this)) ? WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK : WT_PARSER_LIFECYCLE_V2_OK;
}

WtParserLifecycleV2Status ParserCTPMini::waitDrainedV2(uint32_t timeoutMs) noexcept
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

bool ParserCTPMini::destroyableV2() const noexcept
{
	return _lc.destroyable(canWaitV2()) && !sdkSeen();
}

bool ParserCTPMini::removableLegacy() noexcept
{
	return shutdownLegacy() && !sdkSeen();
}

bool ParserCTPMini::sdkSeen() const noexcept
{
	return _lc.sdk_created.load(std::memory_order_acquire) || m_pUserAPI != NULL;
}

bool ParserCTPMini::shutdownLegacy() noexcept
{
	return _lc.shutdownLegacy(this, false, [this]() { releaseSdk(); });
}

void ParserCTPMini::releaseSdk() noexcept
{
	// 调用方保证：两道闸门已关闭且已排空，当前线程不在本实例回调/接口帧内
	std::lock_guard<std::mutex> lock(_sdk_mutex);
	CThostFtdcMdApi* api = m_pUserAPI;
	m_pUserAPI = NULL;
	if (api == NULL)
		return;
	// 曾存在 SDK 对象：不卸载厂商库（无回调线程退出确认，卡9 9a）
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

bool ParserCTPMini::init(WTSVariant* config)
{
	auto call = _lc.call(this);
	// 卡9 9c：重复初始化拒绝并保留原实例；停止后或回调内拒绝
	if (!call || _init_done || sdkSeen() || _lc.shutDown() || _lc.insideCallback(this))
		return false;
	if (config == NULL)
	{
		write_log(m_sink, LL_ERROR, "[ParserCTPMini] Cannot initialize without configuration");
		return false;
	}
	// Any previously loaded module may still have vendor work in flight.
	// Keep its reference resident and do not retry on this parser instance.
	if (m_hInstCTP != NULL || m_pUserAPI != NULL)
	{
		write_log(m_sink, LL_ERROR, "[ParserCTPMini] Market data API module is already loaded; initialization disabled");
		return false;
	}

	m_strFrontAddr = config->getCString("front");
	m_iMidnightMode = (int)wt_actday::parseMode(config->getCString("midnight_actionday"));
	m_strBroker = config->getCString("broker");
	m_strUserID = config->getCString("user");
	m_strPassword = config->getCString("pass");
	m_strFlowDir = config->getCString("flowdir");

	if (m_strFlowDir.empty())
		m_strFlowDir = "CTPMiniMDFlow";

	m_strFlowDir = StrUtil::standardisePath(m_strFlowDir);

	std::string module = config->getCString("ctpmodule");
	if (module.empty())
		module = "thostmduserapi";
	const std::string moduleFile = DLLHelper::wrap_module(module.c_str(), "lib");
	const char* moduleDir = getBinDir();
	if ((moduleDir == NULL || *moduleDir == '\0') && !fs::path(moduleFile).is_absolute())
	{
		write_log(m_sink, LL_ERROR, "[ParserCTPMini] Cannot locate market data module directory");
		return false;
	}

	std::string path = fmtutil::format("{}/{}/{}/", m_strFlowDir.c_str(), m_strBroker.c_str(), m_strUserID.c_str());
	std::error_code pathError;
	fs::create_directories(fs::path(path), pathError);
	if (pathError)
	{
		if (m_sink)
			write_log(m_sink, LL_ERROR, "[ParserCTPMini] Failed to create flow directory '{}': {}", path, pathError.message());
		return false;
	}

	std::string dllpath = (moduleDir == NULL ? std::string() : std::string(moduleDir)) + moduleFile;
	const std::string logPath = dllpath.size() > 128 ? dllpath.substr(0, 128) + "..." : dllpath;
	DllHandle library = DLLHelper::load_library(dllpath.c_str());
	if (library == NULL)
	{
		write_log(m_sink, LL_ERROR, "[ParserCTPMini] Failed to load market data API module: {}", logPath);
		return false;
	}
	// Loading a library can already run vendor initialization. Keep its loader
	// reference even when symbol lookup or API creation fails: neither path has
	// a documented callback/thread quiescence guarantee.
	m_hInstCTP = library;

#ifdef _WIN32
#	ifdef _WIN64
	const char* creatorName = "?CreateFtdcMdApi@CThostFtdcMdApi@@SAPEAV1@PEBD_N1@Z";
#	else
	const char* creatorName = "?CreateFtdcMdApi@CThostFtdcMdApi@@SAPAV1@PBD_N1@Z";
#	endif
#else
	const char* creatorName = "_ZN15CThostFtdcMdApi15CreateFtdcMdApiEPKcbb";
#endif
	CTPCreator creator = (CTPCreator)DLLHelper::get_symbol(library, creatorName);
	if (creator == NULL)
	{
		write_log(m_sink, LL_ERROR, "[ParserCTPMini] Market data API creator symbol not found; module retained: {}", creatorName);
		return false;
	}
	CThostFtdcMdApi* api = creator(path.c_str(), false, false);
	if (api == NULL)
	{
		write_log(m_sink, LL_ERROR, "[ParserCTPMini] Market data API creator returned null; module retained: {}", logPath);
		return false;
	}
	// 从此刻起存在 SDK 对象：本实例的排空不再可证明
	_lc.sdk_created.store(true, std::memory_order_release);
	m_funcCreator = creator;
	m_pUserAPI = api;
	m_pUserAPI->RegisterSpi(this);
	m_pUserAPI->RegisterFront((char*)m_strFrontAddr.c_str());
	_init_done = true;

	return true;
}

void ParserCTPMini::release()
{
	(void)shutdownLegacy();
}

bool ParserCTPMini::connect()
{
	auto call = _lc.call(this);
	// 卡9 9d：回调内同步重入 connect 返回 busy(false)；SDK 的 Init 只调用一次
	if (!call || _lc.shutDown() || _lc.insideCallback(this) || m_pUserAPI == NULL || _connect_issued)
		return false;
	_connect_issued = true;
	m_pUserAPI->Init();
	return true;
}

bool ParserCTPMini::disconnect()
{
	// 终止性关闭：回调/接口重入时只关闭准入并延后 SDK 释放，返回 false（卡9 9d）
	return shutdownLegacy();
}

void ParserCTPMini::OnRspError( CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool /*bIsLast*/ )
{
	auto cb = _lc.callback(this);
	if (!cb)
		return;
	IsErrorRspInfo(pRspInfo);
}

void ParserCTPMini::OnFrontConnected()
{
	auto cb = _lc.callback(this);
	if (!cb)
		return;
	if(m_sink)
	{
		write_log(m_sink, LL_INFO, "[ParserCTPMini] Market data server connected");
		m_sink->handleEvent(WPE_Connect, 0);
	}

	ReqUserLogin();
}

void ParserCTPMini::OnRspUserLogin( CThostFtdcRspUserLoginField */*pRspUserLogin*/, CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool bIsLast )
{
	auto cb = _lc.callback(this);
	if (!cb)
		return;
	if(bIsLast && !IsErrorRspInfo(pRspInfo))
	{
		{
			std::lock_guard<std::recursive_mutex> lock(_data_mutex);
			m_uTradingDate = convert::to_uint32(m_pUserAPI->GetTradingDay());
		}
		
		if(m_sink)
		{
			m_sink->handleEvent(WPE_Login, 0);
		}

		//订阅行情数据
		SubscribeMarketData();
	}
}

void ParserCTPMini::OnRspUserLogout(CThostFtdcUserLogoutField */*pUserLogout*/, CThostFtdcRspInfoField */*pRspInfo*/, int /*nRequestID*/, bool /*bIsLast*/)
{
	auto cb = _lc.callback(this);
	if (!cb)
		return;
	if(m_sink)
	{
		m_sink->handleEvent(WPE_Logout, 0);
	}
}

void ParserCTPMini::OnFrontDisconnected( int nReason )
{
	auto cb = _lc.callback(this);
	if (!cb)
		return;
	if(m_sink)
	{
		write_log(m_sink, LL_ERROR, "[ParserCTPMini] Market data server disconnected: {}", nReason);
		m_sink->handleEvent(WPE_Close, 0);
	}
}

void ParserCTPMini::OnRspUnSubMarketData( CThostFtdcSpecificInstrumentField */*pSpecificInstrument*/, CThostFtdcRspInfoField */*pRspInfo*/, int /*nRequestID*/, bool /*bIsLast*/ )
{
	auto cb = _lc.callback(this);
	if (!cb)
		return;

}

void ParserCTPMini::OnRtnDepthMarketData( CThostFtdcDepthMarketDataField *pDepthMarketData )
{	
	auto cb = _lc.callback(this);
	if (!cb)
		return;
	if(m_pBaseDataMgr == NULL)
	{
		return;
	}

	uint32_t actDate = convert::to_uint32(pDepthMarketData->ActionDay);
	uint32_t actTime = strToTime(pDepthMarketData->UpdateTime) * 1000 + pDepthMarketData->UpdateMillisec;
	uint32_t actHour = actTime / 10000000;
	
	if (actDate == 0)
		actDate = m_uTradingDate;

	//夜盘时发生日期不可能等于交易日,需按本地时钟纠偏（卡3 3b=C，默认 A，见 ActionDayNormalizer.hpp）
	if (actDate == m_uTradingDate && wt_actday::needsLocalClock(actHour, (wt_actday::MidnightMode)m_iMidnightMode))
	{
		uint32_t curDate, curTime;
		wt_actday::localDateTime(curDate, curTime);
		if (!wt_actday::normalize(actDate, actHour, m_uTradingDate, curDate, curTime / 10000000,
			(wt_actday::MidnightMode)m_iMidnightMode))
			return;
	}

	WTSContractInfo* contract = m_pBaseDataMgr->getContract(pDepthMarketData->InstrumentID, pDepthMarketData->ExchangeID);
	if (contract == NULL)
		return;

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

	quote.open_interest = (uint32_t)pDepthMarketData->OpenInterest;

	quote.upper_limit = checkValid(pDepthMarketData->UpperLimitPrice);
	quote.lower_limit = checkValid(pDepthMarketData->LowerLimitPrice);

	quote.pre_close = checkValid(pDepthMarketData->PreClosePrice);
	quote.pre_settle = checkValid(pDepthMarketData->PreSettlementPrice);
	quote.pre_interest = (uint32_t)pDepthMarketData->PreOpenInterest;

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

void ParserCTPMini::OnRspSubMarketData( CThostFtdcSpecificInstrumentField */*pSpecificInstrument*/, CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool /*bIsLast*/ )
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

void ParserCTPMini::OnHeartBeatWarning( int nTimeLapse )
{
	auto cb = _lc.callback(this);
	if (!cb)
		return;
	if(m_sink)
		write_log(m_sink, LL_INFO, "[ParserCTPMini] Heartbeating, elapse: {}", nTimeLapse);
}

void ParserCTPMini::ReqUserLogin()
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
			write_log(m_sink, LL_ERROR, "[ParserCTPMini] Sending login request failed: {}", iResult);
	}
}

void ParserCTPMini::SubscribeMarketData()
{
	std::lock_guard<std::recursive_mutex> lock(_data_mutex);
	SubmitMarketDataSubscriptions(m_filterSubs);
}

bool ParserCTPMini::IsSubscriptionCountSupported(std::size_t count)
{
	return count <= static_cast<std::size_t>((std::numeric_limits<int>::max)());
}

void ParserCTPMini::SubmitMarketDataSubscriptions(const CodeSet &codes)
{
	if (codes.empty())
		return;

	if (!IsSubscriptionCountSupported(codes.size()))
	{
		if (m_sink)
			write_log(m_sink, LL_ERROR, "[ParserCTPMini] Too many contracts to subscribe: {}", codes.size());
		return;
	}

	if (m_pUserAPI == NULL)
		return;

	std::vector<char*> subscribe;
	try
	{
		subscribe.reserve(codes.size());
		for (const auto& code : codes)
		{
			const std::size_t pos = code.find('.');
			char* codePtr = const_cast<char*>(code.c_str());
			if (pos != std::string::npos)
				codePtr += pos + 1;
			subscribe.push_back(codePtr);
		}
	}
	catch (const std::exception& e)
	{
		if (m_sink)
			write_log(m_sink, LL_ERROR, "[ParserCTPMini] Preparing md subscribe request failed: {}", e.what());
		return;
	}

	const int nCount = static_cast<int>(subscribe.size());
	const int iResult = m_pUserAPI->SubscribeMarketData(subscribe.data(), nCount);
	if (iResult != 0)
	{
		if (m_sink)
			write_log(m_sink, LL_ERROR, "[ParserCTPMini] Sending md subscribe request failed: {}", iResult);
	}
	else if (m_sink)
	{
		write_log(m_sink, LL_INFO, "[ParserCTPMini] Market data of {} contracts subscribed in total", nCount);
	}
}

bool ParserCTPMini::IsErrorRspInfo(CThostFtdcRspInfoField */*pRspInfo*/)
{
	return false;
}

void ParserCTPMini::subscribe(const CodeSet &vecSymbols)
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
		SubmitMarketDataSubscriptions(m_filterSubs);
	}
}

void ParserCTPMini::unsubscribe(const CodeSet &/*vecSymbols*/)
{
}

bool ParserCTPMini::isConnected()
{
	auto call = _lc.call(this);
	return call && m_pUserAPI!=NULL;
}

void ParserCTPMini::registerSpi(IParserSpi* listener)
{
	auto call = _lc.call(this);
	if (!call)
		return;
	m_sink = listener;

	if(m_sink)
		m_pBaseDataMgr = m_sink->getBaseDataMgr();
}
