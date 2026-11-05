/*!
 * \file ParserCTPOpt.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "ParserCTPOpt.h"

#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/WTSVersion.h"

#include "../Share/TimeUtils.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/ModuleHelper.hpp"
#include "../Share/Converter.hpp"

#include <exception>
#include <filesystem>
#include <limits>
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
		try { return new ParserCTPOpt(); } catch (...) { return NULL; }
	}

	/*
	 *	卡9 9b：仅当没有回调/接口重入、且本实例从未创建过 SDK 对象时才删除；
	 *	否则保留指针（不置空），由宿主隔离到进程退出。
	 */
	EXPORT_FLAG void deleteParser(IParserApi* &parser)
	{
		if (NULL != parser)
		{
			ParserCTPOpt* typed = dynamic_cast<ParserCTPOpt*>(parser);
			if (typed == NULL || !typed->removableLegacy())
				return;
			delete parser;
			parser = NULL;
		}
	}

	WT_PARSER_LIFECYCLE_V2_EXPORT const WtParserLifecycleApiV2* WT_PARSER_LIFECYCLE_V2_CALL getParserLifecycleApiV2(uint32_t major)
	{
		return wt_vendor_md_v2::Table<ParserCTPOpt>::get(major);
	}
};


constexpr inline uint32_t strToTime(const char* strTime) noexcept
{
	if (strTime == NULL)
		return 0;

	uint32_t result = 0;
	// CTPOpt's TThostFtdcTimeType is a fixed char[9] containing HH:MM:SS.
	// Bound the scan to the eight payload bytes and keep all state local so
	// concurrent parser instances cannot overwrite each other's conversion.
	for (std::size_t i = 0; i < 8 && strTime[i] != '\0'; ++i)
	{
		if ('0' <= strTime[i] && strTime[i] <= '9')
			result = result * 10 + static_cast<uint32_t>(strTime[i] - '0');
	}
	return result;
}

constexpr inline double checkValid(double val) noexcept
{
	return (val == DBL_MAX || val == FLT_MAX) ? 0 : val;
}

ParserCTPOpt::ParserCTPOpt()
	:m_uTradingDate(0)
	,m_loginState(LS_NOTLOGIN)
	,m_pUserAPI(NULL)
	,m_iRequestID(0)
	,m_sink(NULL)
	,m_pBaseDataMgr(NULL)
	,m_hInstCTP(NULL)
	,m_funcCreator(NULL)
	{
}


ParserCTPOpt::~ParserCTPOpt()
{
	// 在自身回调或接口调用内直接析构无法做到安全（栈上仍有本对象帧）：fail-stop。
	if (wt_vendor_md_v2::Lifecycle::reentered(this))
		std::terminate();
	(void)shutdownLegacy();
}

/*
 *	Parser lifecycle V2（卡9 9a=A+C）
 *	CTPOpt 头文件 API/CTPOpt3.5.8/ThostFtdcMdApi.h:92-102 对 Release() 只写「删除接口对象本身」、
 *	对 Join() 只写「等待接口线程结束运行」；RegisterSpi（:126-128）无注销语义，均未声明回调线程 join 或返回后不再回调。
 *	因此 SDK 释放后不再卸载厂商库（旧 release 中的 unloadApiLibrary 只保留给从未创建 SDK 对象的实例）。
 *	因此一旦创建过 SDK 对象，排空状态不可证明：wait_drained 释放一次 SDK 后返回 ERROR，宿主隔离到进程退出。
 */
WtParserLifecycleV2Status ParserCTPOpt::requestStopV2() noexcept
{
	return _lc.requestStop();
}

WtParserLifecycleV2Status ParserCTPOpt::canWaitV2() const noexcept
{
	return (wt_vendor_md_v2::Lifecycle::reentered(this)) ? WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK : WT_PARSER_LIFECYCLE_V2_OK;
}

WtParserLifecycleV2Status ParserCTPOpt::waitDrainedV2(uint32_t timeoutMs) noexcept
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

bool ParserCTPOpt::destroyableV2() const noexcept
{
	return _lc.destroyable(canWaitV2()) && !sdkSeen();
}

bool ParserCTPOpt::removableLegacy() noexcept
{
	return shutdownLegacy() && !sdkSeen();
}

bool ParserCTPOpt::sdkSeen() const noexcept
{
	return _lc.sdk_created.load(std::memory_order_acquire) || m_pUserAPI != NULL;
}

bool ParserCTPOpt::shutdownLegacy() noexcept
{
	return _lc.shutdownLegacy(this, false, [this]() { releaseSdk(); });
}

void ParserCTPOpt::releaseSdk() noexcept
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

bool ParserCTPOpt::init(WTSVariant* config)
{
	auto call = _lc.call(this);
	// 卡9 9c：重复初始化拒绝并保留原实例；停止后、回调内或无配置均拒绝
	if (!call || config == NULL || _init_done || sdkSeen() || _lc.shutDown() || _lc.insideCallback(this))
		return false;

	m_strFrontAddr = config->getCString("front");
	m_strBroker = config->getCString("broker");
	m_strUserID = config->getCString("user");
	m_strPassword = config->getCString("pass");
	m_strFlowDir = config->getCString("flowdir");

	if (m_strFlowDir.empty())
		m_strFlowDir = "CTPOptMDFlow";

	// T2-OPT-04 / 卡14 14b：SDK 未声明单次订阅上限，按可配置批次分批；非法配置 fail-closed。
	m_uSubBatch = DEFAULT_SUB_BATCH;
	if (config->has("subbatch"))
	{
		const int64_t batch = config->getInt64("subbatch");
		if (batch <= 0 || batch > static_cast<int64_t>((std::numeric_limits<int>::max)()))
		{
			write_log(m_sink, LL_ERROR, "[ParserCTPOpt] Init failed: subbatch must be a positive int, got {}", config->getCString("subbatch"));
			return false;
		}
		m_uSubBatch = static_cast<uint32_t>(batch);
	}

	m_strFlowDir = StrUtil::standardisePath(m_strFlowDir);

	std::string module = config->getCString("ctpmodule");
	if (module.empty())
		module = "soptthostmduserapi_se";
	const std::string moduleFile = DLLHelper::wrap_module(module.c_str(), "");
	const char* moduleDir = getBinDir();
	const bool noModuleDir = moduleDir == NULL || moduleDir[0] == '\0';
	if (noModuleDir && !fs::path(moduleFile).is_absolute())
	{
		write_log(m_sink, LL_ERROR, "[ParserCTPOpt] Failed to determine market data API module directory");
		return false;
	}
	std::string path = fmtutil::format("{}/{}/{}/", m_strFlowDir.c_str(), m_strBroker.c_str(), m_strUserID.c_str());
	if (!StdFile::exists(path.c_str()))
	{
		fs::create_directories(fs::path(path));
	}	
	std::string dllpath = noModuleDir ? moduleFile : moduleDir + moduleFile;
	m_hInstCTP = DLLHelper::load_library(dllpath.c_str());
	if (m_hInstCTP == NULL)
	{
		if (m_sink)
			write_log(m_sink, LL_ERROR, "[ParserCTPOpt] Failed to load market data API module");
		return false;
	}
#ifdef _WIN32
#	ifdef _WIN64
	const char* creatorName = "?CreateFtdcMdApi@CThostFtdcMdApi@ctp_sopt@@SAPEAV12@PEBD_N1@Z";
#	else
	const char* creatorName = "?CreateFtdcMdApi@CThostFtdcMdApi@ctp_sopt@@SAPAV12@PBD_N1@Z";
#	endif
#else
	const char* creatorName = "_ZN8ctp_sopt15CThostFtdcMdApi15CreateFtdcMdApiEPKcbb";
#endif
	m_funcCreator = (CTPCreator)DLLHelper::get_symbol(m_hInstCTP, creatorName);
	if (m_funcCreator == NULL)
	{
		if (m_sink)
			write_log(m_sink, LL_ERROR, "[ParserCTPOpt] Market data API creator symbol was not found");
		unloadApiLibrary();
		return false;
	}
	if (!bindApi(m_funcCreator, path.c_str()))
	{
		if (m_sink)
			write_log(m_sink, LL_ERROR, "[ParserCTPOpt] Market data API creator returned null");
		unloadApiLibrary();
		return false;
	}
	_init_done = true;

	return true;
}

bool ParserCTPOpt::bindApi(CTPCreator creator, const char* flowPath)
{
	if (creator == NULL)
		return false;

	m_pUserAPI = creator(flowPath, false, false);
	if (m_pUserAPI == NULL)
		return false;
	// 从此刻起存在 SDK 对象：本实例的排空不再可证明
	_lc.sdk_created.store(true, std::memory_order_release);

	m_pUserAPI->RegisterSpi(this);
	m_pUserAPI->RegisterFront((char*)m_strFrontAddr.c_str());
	return true;
}

void ParserCTPOpt::unloadApiLibrary()
{
	if (m_hInstCTP != NULL)
	{
		DLLHelper::free_library(m_hInstCTP);
		m_hInstCTP = NULL;
	}
	m_funcCreator = NULL;
}

void ParserCTPOpt::release()
{
	// 卡9 9a：曾存在 SDK 对象时保留厂商库到进程退出；从未创建时照旧卸载
	if (shutdownLegacy() && !sdkSeen())
		unloadApiLibrary();
}

bool ParserCTPOpt::connect()
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

bool ParserCTPOpt::disconnect()
{
	// 终止性关闭：回调/接口重入时只关闭准入并延后 SDK 释放，返回 false（卡9 9d）
	return shutdownLegacy();
}

void ParserCTPOpt::OnRspError( CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool /*bIsLast*/ )
{
	auto cb = _lc.callback(this);
	if (!cb)
		return;
	IsErrorRspInfo(pRspInfo);
}

void ParserCTPOpt::OnFrontConnected()
{
	auto cb = _lc.callback(this);
	if (!cb)
		return;
	if(m_sink)
	{
		write_log(m_sink, LL_INFO, "[ParserCTPOpt] Market data server connected");
		m_sink->handleEvent(WPE_Connect, 0);
	}

	ReqUserLogin();
}

void ParserCTPOpt::OnRspUserLogin( CThostFtdcRspUserLoginField */*pRspUserLogin*/, CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool bIsLast )
{
	auto cb = _lc.callback(this);
	if (!cb)
		return;
	if (!bIsLast)
		return;

	if (IsErrorRspInfo(pRspInfo))
		return;

	if (m_pUserAPI == NULL)
	{
		if (m_sink)
			write_log(m_sink, LL_ERROR, "[ParserCTPOpt] Login response received without an active market data API");
		return;
	}

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

void ParserCTPOpt::OnRspUserLogout(CThostFtdcUserLogoutField */*pUserLogout*/, CThostFtdcRspInfoField */*pRspInfo*/, int /*nRequestID*/, bool /*bIsLast*/)
{
	auto cb = _lc.callback(this);
	if (!cb)
		return;
	if(m_sink)
	{
		m_sink->handleEvent(WPE_Logout, 0);
	}
}

void ParserCTPOpt::OnFrontDisconnected( int nReason )
{
	auto cb = _lc.callback(this);
	if (!cb)
		return;
	if(m_sink)
	{
		write_log(m_sink, LL_ERROR, "[ParserCTPOpt] Market data server disconnected: {}", nReason);
		m_sink->handleEvent(WPE_Close, 0);
	}
}

void ParserCTPOpt::OnRspUnSubMarketData( CThostFtdcSpecificInstrumentField */*pSpecificInstrument*/, CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool /*bIsLast*/ )
{
	auto cb = _lc.callback(this);
	if (!cb)
		return;
	IsErrorRspInfo(pRspInfo);
}

void ParserCTPOpt::OnRtnDepthMarketData( CThostFtdcDepthMarketDataField *pDepthMarketData )
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
	/*
	 *	By Wesley @ 2024.08.29
	 *	ETF期权行情比较特别，时间戳是最后一笔成交时间
	 *	因此不活跃的合约的行情时间戳可能是较早时候的时间，甚至是0（如果一直没成交）
	 *	所以这里对行情时间戳小于已经收到的最大的时间戳的数据做一个处理
	 *	1、如果第一笔行情时间戳就是0，则直接读取本地时间
	 *	2、其他情况，则根据记录的最后一笔行情时间，以及当时的本地时间，计算一个最新的时间
	 */
	if (actTime == 0 || actTime < m_uLatestTime)
	{
		if (m_uLatestTime == 0)
			TimeUtils::getDateTime(actDate, actTime);
		else
		{
			uint32_t msecs = (uint32_t)(TimeUtils::getLocalTimeNow() - m_iLastTimestamp);
			actTime = m_uLatestTime + msecs;
		}
	}
	else
	{
		m_iLastTimestamp = TimeUtils::getLocalTimeNow();
		m_uLatestTime = actTime;
	}

	WTSContractInfo* cInfo = m_pBaseDataMgr->getContract(pDepthMarketData->InstrumentID, pDepthMarketData->ExchangeID);
	if (cInfo == NULL)
		return;

	WTSCommodityInfo* pCommInfo = cInfo->getCommInfo();

	WTSTickData* tick = WTSTickData::create(cInfo->getCode());
	if (tick == NULL)
		return;
	tick->setContractInfo(cInfo);
	WTSTickStruct& quote = tick->getTickStruct();
	strcpy(quote.exchg, pCommInfo->getExchg());
	
	quote.action_date = actDate != 0 ? actDate : m_uTradingDate;
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

void ParserCTPOpt::OnRspSubMarketData( CThostFtdcSpecificInstrumentField *pSpecificInstrument, CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool /*bIsLast*/ )
{
	auto cb = _lc.callback(this);
	if (!cb)
		return;
	if(!IsErrorRspInfo(pRspInfo))
	{
		// 卡14 14a："已确认"事件与发送分开记录；空 pSpecificInstrument 不被当作确认。
		if (pSpecificInstrument != NULL && pSpecificInstrument->InstrumentID[0] != '\0')
			write_log(m_sink, LL_DEBUG, "[ParserCTPOpt] Md subscription confirmed: {}", pSpecificInstrument->InstrumentID);

	}
	else
	{

	}
}

void ParserCTPOpt::OnHeartBeatWarning( int nTimeLapse )
{
	auto cb = _lc.callback(this);
	if (!cb)
		return;
	if (m_sink)
		write_log(m_sink, LL_INFO, "[ParserCTPOpt] Heartbeating, elapse: {}", nTimeLapse);
}

void ParserCTPOpt::ReqUserLogin()
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
			write_log(m_sink, LL_ERROR, "[ParserCTPOpt] Sending login request failed: {}", iResult);
	}
}

void ParserCTPOpt::SubscribeMarketData()
{
	std::lock_guard<std::recursive_mutex> lock(_data_mutex);
	CodeSet codeFilter = m_filterSubs;
	if(codeFilter.empty())
	{//如果订阅礼包只空的,则取出全部合约列表
		return;
	}

	sendSubscribeBatches(codeFilter);
}

void ParserCTPOpt::sendSubscribeBatches(const CodeSet& codes)
{
	if (m_pUserAPI == NULL || codes.empty())
		return;

	std::vector<char*> subscribe;
	subscribe.reserve(codes.size());
	for (auto& code : codes)
	{
		std::size_t pos = code.find('.');
		if (pos != std::string::npos)
			subscribe.push_back((char*)code.c_str() + pos + 1);
		else
			subscribe.push_back((char*)code.c_str());
	}

	const std::size_t limit = (m_uSubBatch == 0) ? DEFAULT_SUB_BATCH : m_uSubBatch;
	std::size_t sent = 0, failed = 0, batches = 0;
	for (std::size_t offset = 0; offset < subscribe.size(); offset += limit)
	{
		const std::size_t left = subscribe.size() - offset;
		const int nCount = static_cast<int>(left < limit ? left : limit);
		++batches;
		const int iResult = m_pUserAPI->SubscribeMarketData(subscribe.data() + offset, nCount);
		if (iResult != 0)
		{
			failed += static_cast<std::size_t>(nCount);
			write_log(m_sink, LL_ERROR, "[ParserCTPOpt] Sending md subscribe batch {} ({} contracts) failed: {}", batches, nCount, iResult);
		}
		else
		{
			sent += static_cast<std::size_t>(nCount);
		}
	}

	// 卡14 14a：这里只报告"已发送"，不把发送成功当作订阅成功。
	if (sent > 0)
		write_log(m_sink, LL_INFO, "[ParserCTPOpt] Md subscribe requests sent for {} contracts in {} batches (limit {}), awaiting confirmation", sent, batches, limit);
	if (failed > 0)
		write_log(m_sink, LL_ERROR, "[ParserCTPOpt] {} contracts were not sent for md subscription", failed);
}

bool ParserCTPOpt::IsErrorRspInfo(CThostFtdcRspInfoField *pRspInfo)
{
	if (pRspInfo == NULL || pRspInfo->ErrorID == 0)
		return false;

	if (m_sink)
		write_log(m_sink, LL_ERROR, "[ParserCTPOpt] API response failed with error id {}", pRspInfo->ErrorID);
	return true;
}

void ParserCTPOpt::subscribe(const CodeSet &vecSymbols)
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
		sendSubscribeBatches(vecSymbols);
	}
}

void ParserCTPOpt::unsubscribe(const CodeSet &/*vecSymbols*/)
{
}

bool ParserCTPOpt::isConnected()
{
	auto call = _lc.call(this);
	return call && m_pUserAPI!=NULL;
}

void ParserCTPOpt::registerSpi(IParserSpi* listener)
{
	auto call = _lc.call(this);
	if (!call)
		return;
	m_sink = listener;

	if(m_sink)
		m_pBaseDataMgr = m_sink->getBaseDataMgr();
}
