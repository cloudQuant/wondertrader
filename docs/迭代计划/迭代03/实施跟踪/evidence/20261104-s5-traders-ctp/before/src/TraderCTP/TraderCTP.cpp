/*!
 * \file TraderCTP.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "TraderCTP.h"
#include <boost/asio/post.hpp>
#include <climits>
#include <cmath>
#include <cstring>
#include <limits>

#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/WTSTradeDef.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/IBaseDataMgr.h"

#include "../Share/decimal.h"
#include "../Share/ModuleHelper.hpp"
#include "../Share/Converter.hpp"

#include <filesystem>
namespace fs = std::filesystem;

namespace {
bool parseDecimal(const char* begin, const char* end, uint32_t maximum, uint32_t& value)
{
	if (begin == end) return false;
	uint32_t parsed = 0;
	for (const char* p = begin; p != end; ++p)
	{
		if (*p < '0' || *p > '9') return false;
		const uint32_t digit = static_cast<uint32_t>(*p - '0');
		if (parsed > (maximum - digit) / 10) return false;
		parsed = parsed * 10 + digit;
	}
	value = parsed;
	return true;
}

bool parseBoundedDecimal(const char* value, size_t capacity, uint32_t maximum, uint32_t& result)
{
	if (!value) return false;
	const char* end = static_cast<const char*>(std::memchr(value, '\0', capacity));
	return end && parseDecimal(value, end, maximum, result);
}

bool matchesFixed(const char* value, size_t capacity, const std::string& expected)
{
	const char* end = static_cast<const char*>(std::memchr(value, '\0', capacity));
	return end != NULL && expected.size() == static_cast<size_t>(end - value) &&
		std::memcmp(value, expected.data(), expected.size()) == 0;
}
}

//By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"
template<typename... Args>
inline void write_log(ITraderSpi* sink, WTSLogLevel ll, const char* format, const Args&... args) noexcept
{
	if (sink == NULL)
		return;

	const char* buffer = fmtutil::format(format, args...);

	sink->handleTraderLog(ll, buffer);
}

uint32_t strToTime(const char* strTime) noexcept
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

	return convert::to_uint32(str.c_str());
}

constexpr static inline bool IsErrorRspInfo(CThostFtdcRspInfoField *pRspInfo) noexcept
{
	return (pRspInfo && pRspInfo->ErrorID != 0);
}

constexpr static inline int wrapDirectionType(WTSDirectionType dirType, WTSOffsetType offsetType) noexcept
{
	return (WDT_LONG == dirType) ? ((offsetType == WOT_OPEN) ? THOST_FTDC_D_Buy : THOST_FTDC_D_Sell) : ((offsetType == WOT_OPEN) ? THOST_FTDC_D_Sell : THOST_FTDC_D_Buy);
}

constexpr static inline WTSDirectionType wrapDirectionType(TThostFtdcDirectionType dirType, TThostFtdcOffsetFlagType offsetType) noexcept
{
	return (THOST_FTDC_D_Buy == dirType) ? ((offsetType == THOST_FTDC_OF_Open) ? WDT_LONG : WDT_SHORT) : ((offsetType == THOST_FTDC_OF_Open) ? WDT_SHORT : WDT_LONG);
}

constexpr static inline WTSDirectionType wrapPosDirection(TThostFtdcPosiDirectionType dirType) noexcept
{
	return (THOST_FTDC_PD_Long == dirType) ? WDT_LONG : WDT_SHORT;
}

constexpr static inline int wrapOffsetType(WTSOffsetType offType) noexcept
{
	switch(offType)
	{
	case WOT_OPEN:
		return THOST_FTDC_OF_Open;
	case WOT_CLOSE:
		return THOST_FTDC_OF_Close;
	case WOT_CLOSETODAY:
		return THOST_FTDC_OF_CloseToday;
	case WOT_CLOSEYESTERDAY:
		return THOST_FTDC_OF_Close;
	default:
		return THOST_FTDC_OF_ForceClose;
	}
}

constexpr static inline WTSOffsetType wrapOffsetType(TThostFtdcOffsetFlagType offType) noexcept
{
	switch (offType)
	{
	case THOST_FTDC_OF_Open:
		return WOT_OPEN;
	case THOST_FTDC_OF_Close:
		return WOT_CLOSE;
	case THOST_FTDC_OF_CloseYesterday:
		return WOT_CLOSEYESTERDAY;
	case THOST_FTDC_OF_CloseToday:
		return WOT_CLOSETODAY;
	default:
		return WOT_FORCECLOSE;
	}
}

constexpr static inline int wrapPriceType(WTSPriceType priceType, bool isCFFEX = false) noexcept
{
	switch (priceType)
	{
	case WPT_ANYPRICE:
		return isCFFEX ? THOST_FTDC_OPT_FiveLevelPrice : THOST_FTDC_OPT_AnyPrice;
	case WPT_LIMITPRICE:
		return THOST_FTDC_OPT_LimitPrice;
	case WPT_BESTPRICE:
		return THOST_FTDC_OPT_BestPrice;
	default:
		return THOST_FTDC_OPT_LastPrice;
	}
}

constexpr static inline WTSPriceType wrapPriceType(TThostFtdcOrderPriceTypeType priceType) noexcept
{
	switch (priceType)
	{
	case THOST_FTDC_OPT_AnyPrice:
	case THOST_FTDC_OPT_FiveLevelPrice:
		return WPT_ANYPRICE;
	case THOST_FTDC_OPT_LimitPrice:
		return WPT_LIMITPRICE;
	case THOST_FTDC_OPT_BestPrice:
		return WPT_BESTPRICE;
	default:
		return WPT_LASTPRICE;
	}
}

[[maybe_unused]] constexpr static inline int wrapTimeCondition(WTSTimeCondition timeCond) noexcept
{
	switch (timeCond)
	{
	case WTC_IOC:
		return THOST_FTDC_TC_IOC;
	case WTC_GFD:
		return THOST_FTDC_TC_GFD;
	default:
		return THOST_FTDC_TC_GFS;
	}
}

[[maybe_unused]] constexpr static inline WTSTimeCondition wrapTimeCondition(TThostFtdcTimeConditionType timeCond) noexcept
{
	switch(timeCond)
	{
	case THOST_FTDC_TC_IOC:
		return WTC_IOC;
	case THOST_FTDC_TC_GFD:
		return WTC_GFD;
	default:
		return WTC_GFS;
	}
}

constexpr static inline WTSOrderState wrapOrderState(TThostFtdcOrderStatusType orderState) noexcept
{
	switch (orderState)
	{
	case THOST_FTDC_OST_PartTradedNotQueueing:
		return WOS_Canceled;
	case THOST_FTDC_OST_Unknown:
		return WOS_Submitting;
	case THOST_FTDC_OST_NotTouched:
		return WOS_Nottouched;
	default:
		return (WTSOrderState)orderState;
	}
}

constexpr static inline int wrapActionFlag(WTSActionFlag actionFlag) noexcept
{
	return (WAF_CANCEL == actionFlag) ? THOST_FTDC_AF_Delete : THOST_FTDC_AF_Modify;
}

extern "C"
{
	EXPORT_FLAG ITraderApi* createTrader()
	{
		TraderCTP *instance = new TraderCTP();
		return instance;
	}

	EXPORT_FLAG void deleteTrader(ITraderApi* &trader)
	{
		if (NULL != trader)
		{
			delete trader;
			trader = NULL;
		}
	}
}

TraderCTP::TraderCTP()
	: m_bQuickStart(false)
	, m_sink(NULL)
	, m_uLastQryTime(0)
	, m_lDate(0)
	, m_frontID(0)
	, m_sessionID(0)
	, m_orderRef(0)
	, m_wrapperState(WS_NOTLOGIN)
	, m_pUserAPI(NULL)
	, m_iRequestID(0)
	, m_mapPosition(NULL)
	, m_ayTrades(NULL)
	, m_ayOrders(NULL)
	, m_ayPosDetail(NULL)
	, m_bdMgr(NULL)
	, m_bInQuery(false)
	, m_lastQryTime(0)
	, m_bStopped(false)
	, m_hInstCTP(NULL)
	, m_funcCreator(NULL)
	{
}


TraderCTP::~TraderCTP()
{
}

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
	m_strBroker = params->getCString("broker");
	m_strUser = params->getCString("user");
	m_strPass = params->getCString("pass");

	m_strAppID = params->getCString("appid");
	m_strAuthCode = params->getCString("authcode");
	m_strFlowDir = params->getCString("flowdir");

	if (m_strFlowDir.empty())
		m_strFlowDir = "CTPTDFlow";

	m_strFlowDir = StrUtil::standardisePath(m_strFlowDir);

#ifdef __APPLE__
	m_funcCreator = CThostFtdcTraderApi::CreateFtdcTraderApi;
#else
	std::string module = params->getCString("ctpmodule");
	if (module.empty())
		module = "thosttraderapi_se";

	m_strModule = getBinDir() + DLLHelper::wrap_module(module.c_str(), "");

	m_hInstCTP = DLLHelper::load_library(m_strModule.c_str());
	if (m_hInstCTP == NULL)
	{
		write_log(m_sink, LL_ERROR, "[TraderCTP] Failed to load CTP API module: {}", m_strModule.c_str());
		return false;
	}
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
	if (m_funcCreator == NULL)
	{
		write_log(m_sink, LL_ERROR, "[TraderCTP] CTP API creator symbol is missing: {}", creatorName);
		DLLHelper::free_library(m_hInstCTP);
		m_hInstCTP = NULL;
		return false;
	}
#endif
	if (m_funcCreator == NULL)
	{
		write_log(m_sink, LL_ERROR, "[TraderCTP] CTP API creator is unavailable");
		return false;
	}

	m_bQuickStart = params->getBoolean("quick");

	return true;
}

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

void TraderCTP::connect()
{
	if (m_funcCreator == NULL)
	{
		write_log(m_sink, LL_ERROR, "[TraderCTP] Cannot connect without a CTP API creator");
		return;
	}

	std::stringstream ss;
	ss << m_strFlowDir << "flows/" << m_strBroker << "/" << m_strUser << "/";
	fs::create_directories(ss.str().c_str());
	m_pUserAPI = m_funcCreator(ss.str().c_str());
	if (m_pUserAPI == NULL)
	{
		write_log(m_sink, LL_ERROR, "[TraderCTP] CTP API creator returned a null API instance");
#ifndef __APPLE__
		DLLHelper::free_library(m_hInstCTP);
		m_hInstCTP = NULL;
		m_funcCreator = NULL;
#endif
		return;
	}

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
		write_log(m_sink, LL_INFO, "registerFront: {}", front.c_str());
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
				if (m_bInQuery)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					continue;
				}

				CommonExecuter handler;
				bool waitForInterval = false;
				{
					StdUniqueLock lock(m_mtxQuery);
					if (!m_queQuery.empty())
					{
						uint64_t curTime = TimeUtils::getLocalTimeNow();
						if (curTime - m_lastQryTime < 1000)
						{
							waitForInterval = true;
						}
						else
						{
							handler = m_queQuery.front();
							m_queQuery.pop();
							m_bInQuery = true;
						}
					}
				}

				if (!handler)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(waitForInterval ? 50 : 1));
					continue;
				}

				handler();

				m_lastQryTime = TimeUtils::getLocalTimeNow();
			}
		}));
	}
}

void TraderCTP::disconnect()
{
	{
		StdUniqueLock lock(m_mtxQuery);
		m_queQuery.push([this]() {
			release();
		});
	}

	if (m_thrdWorker)
	{
		m_thrdWorker->join();
		m_thrdWorker = NULL;
	}
}

bool TraderCTP::reserveIdentityLocked(uint32_t& orderRef, std::string& canonical, size_t capacity)
{
	if (!m_identityReady || m_orderRef.load() >= static_cast<uint32_t>(INT_MAX))
		return false;
	const uint32_t candidate = m_orderRef.load() + 1;
	const std::string id = fmt::format("{:06d}#{:010d}#{:06d}", m_frontID, m_sessionID, candidate);
	if (id.size() + 1 > capacity)
		return false;
	OrderIdentity item;
	item.epoch = m_identityEpoch;
	item.front = m_frontID;
	item.session = m_sessionID;
	item.canonical = id;
	if (!m_identities.emplace(candidate, std::move(item)).second)
		return false;
	m_orderRef.store(candidate);
	orderRef = candidate;
	canonical = id;
	return true;
}

bool TraderCTP::makeEntrustID(char* buffer, int length)
{
	if (buffer == NULL || length <= 0) return false;
	buffer[0] = '\0';
	try
	{
		std::lock_guard<std::mutex> lock(m_identityMutex);
		const WrapperState state = m_wrapperState.load();
		if (state != WS_LOGINED && state != WS_CONFIRM_QRYED && state != WS_CONFIRMED && state != WS_ALLREADY)
			return false;
		uint32_t ref = 0;
		std::string canonical;
		if (!reserveIdentityLocked(ref, canonical, static_cast<size_t>(length))) return false;
		std::memcpy(buffer, canonical.c_str(), canonical.size() + 1);
		return true;
	}
	catch (...) { return false; }
}

void TraderCTP::registerSpi(ITraderSpi *listener)
{
	m_sink = listener;
	if (m_sink)
	{
		m_bdMgr = listener->getBaseDataMgr();
	}
}

uint32_t TraderCTP::genRequestID()
{
	return m_iRequestID.fetch_add(1) + 1;
}

int TraderCTP::login(const char* user, const char* pass, const char* productInfo)
{
	if (user == NULL || pass == NULL || productInfo == NULL || m_pUserAPI == NULL)
		return -1;
	uint64_t attempt = 0;
	{
		std::lock_guard<std::mutex> lock(m_identityMutex);
		if (m_inflightInserts != 0 || m_wrapperState == WS_LOGINING || m_loginAttempt == (std::numeric_limits<uint64_t>::max)())
			return -1;
		attempt = ++m_loginAttempt;
		m_identityReady = false;
		m_loginRequestID = 0;
		m_authRequestID = 0;
		m_confirmQueryRequestID = 0;
		m_confirmRequestID = 0;
		m_strUser = user;
		m_strPass = pass;
		m_strProdInfo = productInfo;
		m_wrapperState = WS_LOGINING;
	}
	int iResult = authenticate(attempt);
	if (iResult != 0)
	{
		write_log(m_sink, LL_ERROR, "[TraderCTP] Sending authenticate request failed: {}", iResult);
		{
			std::lock_guard<std::mutex> lock(m_identityMutex);
			if (m_loginAttempt != attempt) return -1;
			m_identityReady = false;
			m_wrapperState = WS_LOGINFAILED;
		}
		if (m_sink)
		{
			const std::string message = "CTP:认证请求发送失败: " + std::to_string(iResult);
			m_sink->onLoginResult(false, message.c_str(), 0);
		}
		return -1;
	}

	return 0;
}

int TraderCTP::doLogin(uint64_t attempt)
{
	CThostFtdcReqUserLoginField req;
	memset(&req, 0, sizeof(req));
	uint32_t requestID = 0;
	{
		std::lock_guard<std::mutex> lock(m_identityMutex);
		if (m_wrapperState != WS_LOGINING || m_loginAttempt != attempt) return -1;
		requestID = genRequestID();
		if (requestID == 0 || requestID > INT_MAX) return -1;
		m_loginRequestID = requestID;
		wt_strcpy(req.BrokerID, m_strBroker.c_str(), m_strBroker.size());
		wt_strcpy(req.UserID, m_strUser.c_str(), m_strUser.size());
		wt_strcpy(req.Password, m_strPass.c_str(), m_strPass.size());
		wt_strcpy(req.UserProductInfo, m_strProdInfo.c_str(), m_strProdInfo.size());
	}
	int iResult = m_pUserAPI->ReqUserLogin(&req, requestID);
	if (iResult != 0)
	{
		write_log(m_sink, LL_ERROR, "[TraderCTP] Sending login request failed: {}", iResult);
		{
			std::lock_guard<std::mutex> lock(m_identityMutex);
			if (m_loginAttempt != attempt) return -1;
			m_identityReady = false;
			m_wrapperState = WS_LOGINFAILED;
		}
		if (m_sink)
		{
			const std::string message = "CTP:登录请求发送失败: " + std::to_string(iResult);
			m_sink->onLoginResult(false, message.c_str(), 0);
		}
		return -1;
	}

	return 0;
}

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

int TraderCTP::orderInsert(WTSEntrust* entrust)
{
	if (m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY || entrust == NULL)
	{
		write_log(m_sink, LL_ERROR, "[TraderCTP] Trading channel not ready");
		return -1;
	}

	CThostFtdcInputOrderField req;
	memset(&req, 0, sizeof(req));
	const char* codeEnd = static_cast<const char*>(std::memchr(entrust->getCode(), '\0', sizeof(entrust->m_strCode)));
	const char* exchgEnd = static_cast<const char*>(std::memchr(entrust->getExchg(), '\0', sizeof(entrust->m_strExchg)));
	const char* idEnd = static_cast<const char*>(std::memchr(entrust->getEntrustID(), '\0', sizeof(entrust->m_strEntrustID)));
	const char* tagEnd = static_cast<const char*>(std::memchr(entrust->getUserTag(), '\0', sizeof(entrust->m_strUserTag)));
	const double volume = entrust->getVolume();
	if (!codeEnd || !exchgEnd || !idEnd || !tagEnd || codeEnd == entrust->getCode() || exchgEnd == entrust->getExchg() ||
		static_cast<size_t>(codeEnd - entrust->getCode()) >= sizeof(req.InstrumentID) ||
		static_cast<size_t>(exchgEnd - entrust->getExchg()) >= sizeof(req.ExchangeID) ||
		!std::isfinite(volume) || volume < 1 || volume > INT_MAX || std::floor(volume) != volume)
		return -1;
	wt_strcpy(req.InstrumentID, entrust->getCode());
	wt_strcpy(req.ExchangeID, entrust->getExchg());

	uint32_t orderref = 0;
	try
	{
		std::lock_guard<std::mutex> identityLock(m_identityMutex);
		if (!m_identityReady || m_wrapperState != WS_ALLREADY || m_inflightInserts == UINT32_MAX)
			return -1;
		if (m_strBroker.size() >= sizeof(req.BrokerID) || m_strUser.size() >= sizeof(req.InvestorID))
			return -1;
		wt_strcpy(req.BrokerID, m_strBroker.c_str());
		wt_strcpy(req.InvestorID, m_strUser.c_str());
		std::string canonical;
		if (idEnd == entrust->getEntrustID())
		{
			if (!reserveIdentityLocked(orderref, canonical, sizeof(entrust->m_strEntrustID))) return -1;
		}
		else
		{
			uint32_t front = 0, session = 0;
			if (!extractEntrustID(entrust->getEntrustID(), front, session, orderref) ||
				front != m_frontID || session != m_sessionID) return -1;
		}
		auto it = m_identities.find(orderref);
		if (it == m_identities.end() || it->second.submitted || it->second.epoch != m_identityEpoch ||
			it->second.front != m_frontID || it->second.session != m_sessionID) return -1;
		OrderIdentity& identity = it->second;
		identity.submitted = true; // Never recycle a possibly sent or failed identity.
		canonical = identity.canonical;
		const std::string tag = tagEnd == entrust->getUserTag() ? "ctp-auto:" + canonical :
			std::string(entrust->getUserTag(), static_cast<size_t>(tagEnd - entrust->getUserTag()));
		if (tag.size() >= sizeof(entrust->m_strUserTag)) return -1;
		identity.tag = tag;
		identity.broker = m_strBroker;
		identity.investor = m_strUser;
		identity.tradingDate = m_lDate;
		identity.exchange = entrust->getExchg();
		identity.instrument = entrust->getCode();
		{
			std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
			const uint32_t previousSize = m_eidCache.size();
			if (previousSize == UINT32_MAX || m_eidCache.has(canonical.c_str()) ||
				!m_eidCache.try_put_if_none(canonical.c_str(), tag.c_str()) ||
				m_eidCache.size() != previousSize + 1 || tag != m_eidCache.get(canonical.c_str())) return -1;
		}
		entrust->setEntrustID(canonical.c_str());
		entrust->setUserTag(tag.c_str());
		std::snprintf(req.OrderRef, sizeof(req.OrderRef), "%u", orderref);
		++m_inflightInserts;
	}
	catch (...) { return -1; }

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
	req.VolumeTotalOriginal = static_cast<int>(volume);

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

	struct InsertLease
	{
		std::mutex& mutex;
		uint32_t& count;
		~InsertLease() { std::lock_guard<std::mutex> lock(mutex); --count; }
	} lease{m_identityMutex, m_inflightInserts};
	int iResult = -1;
	const uint32_t requestID = genRequestID();
	if (requestID == 0 || requestID > INT_MAX) return -1;
	try { iResult = m_pUserAPI->ReqOrderInsert(&req, static_cast<int>(requestID)); }
	catch (...) { return -1; }
	if (iResult != 0)
	{
		write_log(m_sink, LL_ERROR, "[TraderCTP] Order inserting failed: {}", iResult);
		return -1;
	}

	return 0;
}

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
		return -1;
	}

	return 0;
}

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

void TraderCTP::OnFrontConnected()
{
	if (m_sink)
		m_sink->handleEvent(WTE_Connect, 0);
}

void TraderCTP::OnFrontDisconnected(int nReason)
{
	{
		std::lock_guard<std::mutex> lock(m_identityMutex);
		m_identityReady = false;
		m_wrapperState = WS_NOTLOGIN;
	}
	if (m_sink)
		m_sink->handleEvent(WTE_Close, nReason);
}

void TraderCTP::OnHeartBeatWarning(int /*nTimeLapse*/)
{
	write_log(m_sink, LL_DEBUG, "[TraderCTP][{}-{}] Heartbeating...", m_strBroker.c_str(), m_strUser.c_str());
}

void TraderCTP::OnRspAuthenticate(CThostFtdcRspAuthenticateField */*pRspAuthenticateField*/, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool /*bIsLast*/)
{
	uint64_t attempt = 0;
	{
		std::lock_guard<std::mutex> lock(m_identityMutex);
		if (m_wrapperState != WS_LOGINING || nRequestID <= 0 || static_cast<uint32_t>(nRequestID) != m_authRequestID) return;
		m_authRequestID = 0;
		attempt = m_loginAttempt;
	}
	if (!IsErrorRspInfo(pRspInfo))
	{
		doLogin(attempt);
	}
	else
	{
		write_log(m_sink, LL_ERROR, "[TraderCTP][{}-{}] Authentiation failed: {}", m_strBroker.c_str(), m_strUser.c_str(), pRspInfo->ErrorMsg);
		{
			std::lock_guard<std::mutex> lock(m_identityMutex);
			if (m_loginAttempt != attempt || m_wrapperState != WS_LOGINING) return;
			m_identityReady = false;
			m_wrapperState = WS_LOGINFAILED;
		}

		if (m_sink)
			m_sink->onLoginResult(false, pRspInfo->ErrorMsg, 0);
	}

}

void TraderCTP::OnRspUserLogin(CThostFtdcRspUserLoginField *pRspUserLogin, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool /*bIsLast*/)
{
	bool accepted = false;
	uint32_t serverRef = 0;
	uint32_t tradingDate = 0;
	const bool valid = !IsErrorRspInfo(pRspInfo) && pRspUserLogin != NULL &&
		pRspUserLogin->FrontID >= 0 && pRspUserLogin->SessionID >= 0 &&
		parseBoundedDecimal(pRspUserLogin->MaxOrderRef, sizeof(pRspUserLogin->MaxOrderRef), INT_MAX, serverRef) &&
		parseBoundedDecimal(pRspUserLogin->TradingDay, sizeof(pRspUserLogin->TradingDay), 99991231, tradingDate) && tradingDate != 0;
	try
	{
		std::lock_guard<std::mutex> identityLock(m_identityMutex);
		if (m_inflightInserts != 0)
		{
			// Never publish a session transition during an SDK submission. A
			// synchronous login callback must not wait for its own insert lease.
			m_loginRequestID = 0;
			m_identityReady = false;
			m_wrapperState = WS_LOGINFAILED;
		}
		else if (nRequestID <= 0 || static_cast<uint32_t>(nRequestID) != m_loginRequestID || m_wrapperState != WS_LOGINING)
			return;
		else
		{
		m_loginRequestID = 0;
		m_identityReady = false;
		if (valid && matchesFixed(pRspUserLogin->BrokerID, sizeof(pRspUserLogin->BrokerID), m_strBroker) &&
			matchesFixed(pRspUserLogin->UserID, sizeof(pRspUserLogin->UserID), m_strUser) &&
			m_identityEpoch != (std::numeric_limits<uint64_t>::max)())
		{
			const std::string directory = StrUtil::standardisePath(m_strFlowDir + "local/" + m_strBroker + "/");
			std::error_code ec;
			fs::create_directories(directory, ec);
			if (!ec)
			{
				std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
				// Cache diagnostics are reported after releasing identity/cache locks.
				accepted = m_eidCache.init((directory + m_strUser + "_eid.sc").c_str(), tradingDate) &&
					m_oidCache.init((directory + m_strUser + "_oid.sc").c_str(), tradingDate);
			}
			if (accepted)
			{
				m_frontID = static_cast<uint32_t>(pRspUserLogin->FrontID);
				m_sessionID = static_cast<uint32_t>(pRspUserLogin->SessionID);
				m_orderRef.store(std::max(m_orderRef.load(), serverRef));
				m_lDate = tradingDate;
				++m_identityEpoch;
				m_identityReady = true;
				m_wrapperState = WS_LOGINED;
			}
		}
		if (!accepted) m_wrapperState = WS_LOGINFAILED;
		}
	}
	catch (...)
	{
		std::lock_guard<std::mutex> lock(m_identityMutex);
		m_identityReady = false;
		m_wrapperState = WS_LOGINFAILED;
	}
	if (!accepted)
	{
		write_log(m_sink, LL_ERROR, "[TraderCTP] Login identity initialization failed or an insert is in flight");
		if (m_sink) m_sink->onLoginResult(false,
			IsErrorRspInfo(pRspInfo) ? pRspInfo->ErrorMsg : "CTP login identity initialization failed", 0);
		return;
	}
	write_log(m_sink, LL_INFO, "[TraderCTP][{}-{}] Login succeed, trading date: {}...", m_strBroker.c_str(), m_strUser.c_str(), tradingDate);
	queryConfirm();
}

void TraderCTP::OnRspUserLogout(CThostFtdcUserLogoutField */*pUserLogout*/, CThostFtdcRspInfoField */*pRspInfo*/, int /*nRequestID*/, bool /*bIsLast*/)
{
	{
		std::lock_guard<std::mutex> lock(m_identityMutex);
		m_identityReady = false;
		m_wrapperState = WS_NOTLOGIN;
	}
	if (m_sink)
		m_sink->handleEvent(WTE_Logout, 0);
}

void TraderCTP::OnRspQrySettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField *pSettlementInfoConfirm, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if (bIsLast)
	{
		m_bInQuery = false;
		//triggerQuery();
	}

	if (!IsErrorRspInfo(pRspInfo))
	{
		uint32_t tradingDate = 0;
		{
			std::lock_guard<std::mutex> lock(m_identityMutex);
			if (!m_identityReady || m_wrapperState != WS_LOGINED || nRequestID <= 0 ||
				static_cast<uint32_t>(nRequestID) != m_confirmQueryRequestID) return;
			if (bIsLast) m_confirmQueryRequestID = 0;
			tradingDate = m_lDate;
		}
		if (pSettlementInfoConfirm != NULL)
		{
			uint32_t uConfirmDate = convert::to_uint32(pSettlementInfoConfirm->ConfirmDate);
			if (uConfirmDate >= tradingDate)
			{
				{
					std::lock_guard<std::mutex> lock(m_identityMutex);
					if (!m_identityReady || m_wrapperState != WS_LOGINED) return;
					m_wrapperState = WS_ALLREADY;
				}

				write_log(m_sink, LL_INFO, "[TraderCTP][{}-{}] Trading channel initialized...", m_strBroker.c_str(), m_strUser.c_str());
				if (m_sink)
					m_sink->onLoginResult(true, "", tradingDate);
			}
			else
			{
				{
					std::lock_guard<std::mutex> lock(m_identityMutex);
					if (!m_identityReady || m_wrapperState != WS_LOGINED) return;
					m_wrapperState = WS_CONFIRM_QRYED;
				}

				write_log(m_sink, LL_INFO, "[TraderCTP][{}-{}] Confirming settlement data...", m_strBroker.c_str(), m_strUser.c_str());
				confirm();
			}
		}
		else
		{
			{
				std::lock_guard<std::mutex> lock(m_identityMutex);
				if (!m_identityReady || m_wrapperState != WS_LOGINED) return;
				m_wrapperState = WS_CONFIRM_QRYED;
			}
			confirm();
		}
	}

}

void TraderCTP::OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField *pSettlementInfoConfirm, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool /*bIsLast*/)
{
	if (!IsErrorRspInfo(pRspInfo) && pSettlementInfoConfirm != NULL)
	{
		bool ready = false;
		uint32_t tradingDate = 0;
		{
			std::lock_guard<std::mutex> lock(m_identityMutex);
			if (m_identityReady && m_wrapperState == WS_CONFIRM_QRYED && nRequestID > 0 &&
				static_cast<uint32_t>(nRequestID) == m_confirmRequestID)
			{
				m_confirmRequestID = 0;
				m_wrapperState = WS_ALLREADY;
				tradingDate = m_lDate;
				ready = true;
			}
		}
		if (ready)
		{

			write_log(m_sink, LL_INFO, "[TraderCTP][{}-{}] Trading channel initialized...", m_strBroker.c_str(), m_strUser.c_str());
			if (m_sink)
				m_sink->onLoginResult(true, "", tradingDate);
		}
	}
}

void TraderCTP::OnRspOrderInsert(CThostFtdcInputOrderField *pInputOrder, CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool /*bIsLast*/)
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

void TraderCTP::OnRspOrderAction(CThostFtdcInputOrderActionField *pInputOrderAction, CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool /*bIsLast*/)
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

void TraderCTP::OnRspQryTradingAccount(CThostFtdcTradingAccountField *pTradingAccount, CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool bIsLast)
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

void TraderCTP::OnRspQryInvestorPosition(CThostFtdcInvestorPositionField *pInvestorPosition, CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool bIsLast)
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

void TraderCTP::OnRspQrySettlementInfo(CThostFtdcSettlementInfoField *pSettlementInfo, CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool bIsLast)
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

void TraderCTP::OnRspQryTrade(CThostFtdcTradeField *pTrade, CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool bIsLast)
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

void TraderCTP::OnRspQryOrder(CThostFtdcOrderField *pOrder, CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool bIsLast)
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

void TraderCTP::OnRspError(CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool /*bIsLast*/)
{
	m_sink->handleTraderLog(LL_ERROR, fmtutil::format("{} rsp error: {} : {}", nRequestID, pRspInfo->ErrorID, pRspInfo->ErrorMsg));
}

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


WTSOrderInfo* TraderCTP::makeOrderInfo(CThostFtdcOrderField* orderField)
{
	if (orderField == NULL || m_bdMgr == NULL ||
		!std::memchr(orderField->BrokerID, '\0', sizeof(orderField->BrokerID)) ||
		!std::memchr(orderField->InvestorID, '\0', sizeof(orderField->InvestorID)) ||
		!std::memchr(orderField->InstrumentID, '\0', sizeof(orderField->InstrumentID)) ||
		!std::memchr(orderField->ExchangeID, '\0', sizeof(orderField->ExchangeID)) ||
		!std::memchr(orderField->OrderSysID, '\0', sizeof(orderField->OrderSysID))) return NULL;
	WTSContractInfo* cInfo = m_bdMgr->getContract(orderField->InstrumentID, orderField->ExchangeID);
	if (cInfo == NULL)
		return NULL;

	WTSOrderInfo* pRet = WTSOrderInfo::create();
	pRet->setContractInfo(cInfo);
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
	else
	{
		// WTSOrderFlag does not preserve GTD expiry or other time-condition details.
		pRet->setOrderFlag(WOF_NOR);
	}

	pRet->setVolTraded(orderField->VolumeTraded);
	pRet->setVolLeft(orderField->VolumeTotalOriginal - orderField->VolumeTraded);//要保证撤单的时候，剩余数量=挂单总数-已成交

	pRet->setCode(cInfo->getCode());
	pRet->setExchange(cInfo->getExchg());

	uint32_t uDate = convert::to_uint32(orderField->InsertDate);
	std::string strTime = orderField->InsertTime;
	StrUtil::replace(strTime, ":", "");
	uint32_t uTime = convert::to_uint32(strTime.c_str());
	uint32_t tradingDate = 0;
	{
		std::lock_guard<std::mutex> lock(m_identityMutex);
		tradingDate = m_lDate;
	}
	if (uTime >= 210000 && uDate == tradingDate)
	{
		uDate = TimeUtils::getNextDate(uDate, -1);
	}

	pRet->setOrderDate(uDate);
	pRet->setOrderTime(TimeUtils::makeTime(uDate, uTime * 1000));

	pRet->setOrderState(wrapOrderState(orderField->OrderStatus));
	if (orderField->OrderSubmitStatus >= THOST_FTDC_OSS_InsertRejected)
		pRet->setError(WOEF_Normal);

	uint32_t callbackRef = 0;
	uint32_t callbackTradingDate = 0;
	const bool validIdentity = orderField->FrontID >= 0 && orderField->SessionID >= 0 &&
		parseBoundedDecimal(orderField->OrderRef, sizeof(orderField->OrderRef), INT_MAX, callbackRef) && callbackRef != 0;
	const bool validTradingDate = parseBoundedDecimal(orderField->TradingDay, sizeof(orderField->TradingDay), 99991231, callbackTradingDate) && callbackTradingDate != 0;
	if (validIdentity)
		generateEntrustID(pRet->getEntrustID(), orderField->FrontID, orderField->SessionID, callbackRef);
	pRet->setOrderID(orderField->OrderSysID);

	pRet->setStateMsg(orderField->StatusMsg);


	std::string usertag;
	if (validIdentity && validTradingDate)
	{
		std::lock_guard<std::mutex> lock(m_identityMutex);
		auto it = m_identities.find(callbackRef);
		if (it != m_identities.end() && it->second.submitted &&
			it->second.front == static_cast<uint32_t>(orderField->FrontID) &&
			it->second.session == static_cast<uint32_t>(orderField->SessionID) &&
			it->second.tradingDate == callbackTradingDate &&
			matchesFixed(orderField->BrokerID, sizeof(orderField->BrokerID), it->second.broker) &&
			matchesFixed(orderField->InvestorID, sizeof(orderField->InvestorID), it->second.investor) &&
			it->second.exchange == orderField->ExchangeID && it->second.instrument == orderField->InstrumentID)
			usertag = it->second.tag;
	}
	if (usertag.empty())
	{
		pRet->setUserTag(pRet->getEntrustID());
	}
	else
	{
		pRet->setUserTag(usertag.c_str());

		if (strlen(pRet->getOrderID()) > 0)
		{
			const std::string key = std::string(orderField->BrokerID) + "#" + orderField->InvestorID + "#" + orderField->ExchangeID + "#" + orderField->InstrumentID + "#" +
				std::to_string(callbackTradingDate) + "#" + StrUtil::trim(pRet->getOrderID());
			bool conflict = false;
			if (key.size() < 64)
			{
				std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
				if (m_oidCache.has(key.c_str()))
					conflict = usertag != m_oidCache.get(key.c_str());
				else
					(void)m_oidCache.try_put_if_none(key.c_str(), usertag.c_str());
			}
			{
				std::lock_guard<std::mutex> lock(m_identityMutex);
				auto inserted = m_orderSysTags.emplace(key, conflict ? "" : usertag);
				if (conflict || (!inserted.second && inserted.first->second != usertag))
					inserted.first->second.clear();
			}
		}
	}

	return pRet;
}

WTSEntrust* TraderCTP::makeEntrust(CThostFtdcInputOrderField *entrustField)
{
	if (entrustField == NULL || m_bdMgr == NULL ||
		!std::memchr(entrustField->InstrumentID, '\0', sizeof(entrustField->InstrumentID)) ||
		!std::memchr(entrustField->ExchangeID, '\0', sizeof(entrustField->ExchangeID))) return NULL;
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
	else
	{
		// WTSOrderFlag does not preserve GTD expiry or other time-condition details.
		pRet->setOrderFlag(WOF_NOR);
	}

	uint32_t ref = 0;
	if (parseBoundedDecimal(entrustField->OrderRef, sizeof(entrustField->OrderRef), INT_MAX, ref) && ref != 0)
	{
		std::lock_guard<std::mutex> lock(m_identityMutex);
		auto it = m_identities.find(ref);
		if (it != m_identities.end() && it->second.submitted &&
			matchesFixed(entrustField->BrokerID, sizeof(entrustField->BrokerID), it->second.broker) &&
			matchesFixed(entrustField->InvestorID, sizeof(entrustField->InvestorID), it->second.investor) &&
			it->second.exchange == entrustField->ExchangeID && it->second.instrument == entrustField->InstrumentID)
		{
			pRet->setEntrustID(it->second.canonical.c_str());
			pRet->setUserTag(it->second.tag.c_str());
		}
	}

	return pRet;
}

WTSEntrustAction* TraderCTP::makeAction(CThostFtdcInputOrderActionField *actionField)
{
	WTSEntrustAction* pRet = WTSEntrustAction::create(actionField->InstrumentID, actionField->ExchangeID);
	pRet->setOrderID(actionField->OrderSysID);

	generateEntrustID(pRet->getEntrustID(), actionField->FrontID, actionField->SessionID, atoi(actionField->OrderRef));

	std::string usertag;
	uint32_t ref = 0;
	if (actionField->FrontID >= 0 && actionField->SessionID >= 0 &&
		parseBoundedDecimal(actionField->OrderRef, sizeof(actionField->OrderRef), INT_MAX, ref) && ref != 0)
	{
		std::lock_guard<std::mutex> lock(m_identityMutex);
		auto it = m_identities.find(ref);
		if (it != m_identities.end() && it->second.submitted &&
			it->second.front == static_cast<uint32_t>(actionField->FrontID) &&
			it->second.session == static_cast<uint32_t>(actionField->SessionID) &&
			matchesFixed(actionField->BrokerID, sizeof(actionField->BrokerID), it->second.broker) &&
			matchesFixed(actionField->InvestorID, sizeof(actionField->InvestorID), it->second.investor) &&
			it->second.exchange == actionField->ExchangeID && it->second.instrument == actionField->InstrumentID)
			usertag = it->second.tag;
	}
	if (!usertag.empty())
		pRet->setUserTag(usertag.c_str());

	return pRet;
}

WTSError* TraderCTP::makeError(CThostFtdcRspInfoField* rspInfo, WTSErroCode ec /* = WEC_NONE */)
{
	WTSError* pRet = WTSError::create(ec, fmtutil::format("{}({})", rspInfo->ErrorMsg, rspInfo->ErrorID));
	return pRet;
}

WTSTradeInfo* TraderCTP::makeTradeInfo(CThostFtdcTradeField *tradeField)
{
	if (tradeField == NULL || m_bdMgr == NULL ||
		!std::memchr(tradeField->BrokerID, '\0', sizeof(tradeField->BrokerID)) ||
		!std::memchr(tradeField->InvestorID, '\0', sizeof(tradeField->InvestorID)) ||
		!std::memchr(tradeField->InstrumentID, '\0', sizeof(tradeField->InstrumentID)) ||
		!std::memchr(tradeField->ExchangeID, '\0', sizeof(tradeField->ExchangeID)) ||
		!std::memchr(tradeField->OrderSysID, '\0', sizeof(tradeField->OrderSysID))) return NULL;
	WTSContractInfo* cInfo = m_bdMgr->getContract(tradeField->InstrumentID, tradeField->ExchangeID);
	if (cInfo == NULL)
		return NULL;

	WTSTradeInfo *pRet = WTSTradeInfo::create(cInfo->getCode(), cInfo->getExchg());
	pRet->setVolume(tradeField->Volume);
	pRet->setPrice(tradeField->Price);
	pRet->setTradeID(tradeField->TradeID);
	pRet->setContractInfo(cInfo);

	std::string strTime = tradeField->TradeTime;
	StrUtil::replace(strTime, ":", "");
	uint32_t uTime = convert::to_uint32(strTime.c_str());
	uint32_t uDate = convert::to_uint32(tradeField->TradeDate);
	
	//如果是夜盘时间，并且成交日期等于交易日，说明成交日期是需要修正
	//因为夜盘是提前的，成交日期必然小于交易日
	//但是这里只能做一个简单修正了
	uint32_t tradingDate = 0;
	{
		std::lock_guard<std::mutex> lock(m_identityMutex);
		tradingDate = m_lDate;
	}
	if(uTime >= 210000 && uDate == tradingDate)
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

	double amount = cInfo->getCommInfo()->getVolScale()*tradeField->Volume*pRet->getPrice();
	pRet->setAmount(amount);

	std::string usertag;
	uint32_t ref = 0;
	uint32_t tradeTradingDate = 0;
	const bool validTradingDate = parseBoundedDecimal(tradeField->TradingDay, sizeof(tradeField->TradingDay), 99991231, tradeTradingDate) && tradeTradingDate != 0;
	if (validTradingDate && parseBoundedDecimal(tradeField->OrderRef, sizeof(tradeField->OrderRef), INT_MAX, ref) && ref != 0)
	{
		std::lock_guard<std::mutex> lock(m_identityMutex);
		auto it = m_identities.find(ref);
		if (it != m_identities.end() && it->second.submitted &&
			it->second.tradingDate == tradeTradingDate &&
			matchesFixed(tradeField->BrokerID, sizeof(tradeField->BrokerID), it->second.broker) &&
			matchesFixed(tradeField->InvestorID, sizeof(tradeField->InvestorID), it->second.investor) &&
			it->second.exchange == tradeField->ExchangeID && it->second.instrument == tradeField->InstrumentID)
			usertag = it->second.tag;
	}
	if (usertag.empty() && validTradingDate)
	{
		const std::string key = std::string(tradeField->BrokerID) + "#" + tradeField->InvestorID + "#" + tradeField->ExchangeID + "#" + tradeField->InstrumentID + "#" +
			std::to_string(tradeTradingDate) + "#" + StrUtil::trim(pRet->getRefOrder());
		std::lock_guard<std::mutex> lock(m_identityMutex);
		auto it = m_orderSysTags.find(key);
		if (it != m_orderSysTags.end()) usertag = it->second;
	}
	if (!usertag.empty()) pRet->setUserTag(usertag.c_str());

	return pRet;
}

void TraderCTP::generateEntrustID(char* buffer, uint32_t frontid, uint32_t sessionid, uint32_t orderRef)
{
	fmtutil::format_to(buffer, "{:06d}#{:010d}#{:06d}", frontid, sessionid, orderRef);
}

bool TraderCTP::extractEntrustID(const char* entrustid, uint32_t &frontid, uint32_t &sessionid, uint32_t &orderRef)
{
	if (!entrustid) return false;
	const char* end = static_cast<const char*>(std::memchr(entrustid, '\0', 64));
	if (!end) return false;
	const char* first = static_cast<const char*>(std::memchr(entrustid, '#', end - entrustid));
	if (!first) return false;
	const char* second = static_cast<const char*>(std::memchr(first + 1, '#', end - first - 1));
	if (!second) return false;
	uint32_t front = 0, session = 0, ref = 0;
	if (!parseDecimal(entrustid, first, INT_MAX, front) ||
		!parseDecimal(first + 1, second, INT_MAX, session) ||
		!parseDecimal(second + 1, end, INT_MAX, ref) || ref == 0) return false;
	frontid = front; sessionid = session; orderRef = ref;
	return true;
}

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

void TraderCTP::OnRtnInstrumentStatus(CThostFtdcInstrumentStatusField *pInstrumentStatus)
{
	if (m_sink)
		m_sink->onPushInstrumentStatus(pInstrumentStatus->ExchangeID, pInstrumentStatus->InstrumentID, (WTSTradeStatus)pInstrumentStatus->InstrumentStatus);
}

bool TraderCTP::isConnected()
{
	return (m_wrapperState == WS_ALLREADY);
}

int TraderCTP::queryConfirm()
{
	uint64_t epoch = 0;
	{
		std::lock_guard<std::mutex> lock(m_identityMutex);
		if (m_pUserAPI == NULL || !m_identityReady || m_wrapperState != WS_LOGINED) return -1;
		epoch = m_identityEpoch;
	}

	{
		StdUniqueLock lock(m_mtxQuery);
		m_queQuery.push([this, epoch]() {
			CThostFtdcQrySettlementInfoConfirmField req;
			memset(&req, 0, sizeof(req));
			uint32_t request = 0;
			{
				std::lock_guard<std::mutex> lock(m_identityMutex);
				if (!m_identityReady || m_identityEpoch != epoch || m_wrapperState != WS_LOGINED) { m_bInQuery = false; return; }
				request = genRequestID();
				if (request == 0 || request > INT_MAX) { m_identityReady = false; m_wrapperState = WS_LOGINFAILED; return; }
				m_confirmQueryRequestID = request;
				wt_strcpy(req.BrokerID, m_strBroker.c_str());
				wt_strcpy(req.InvestorID, m_strUser.c_str());
			}
			int iResult = m_pUserAPI->ReqQrySettlementInfoConfirm(&req, static_cast<int>(request));
			if (iResult != 0)
			{
				m_bInQuery = false;
				write_log(m_sink, LL_ERROR, "[TraderCTP][{}-{}] Sending query of settlement data confirming state failed: {}", m_strBroker.c_str(), m_strUser.c_str(), iResult);
			}
		});
	}

	//triggerQuery();

	return 0;
}

int TraderCTP::confirm()
{
	//std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	CThostFtdcSettlementInfoConfirmField req;
	memset(&req, 0, sizeof(req));
	uint32_t request = 0;
	{
		std::lock_guard<std::mutex> lock(m_identityMutex);
		if (m_pUserAPI == NULL || !m_identityReady || m_wrapperState != WS_CONFIRM_QRYED) return -1;
		request = genRequestID();
		if (request == 0 || request > INT_MAX) { m_identityReady = false; m_wrapperState = WS_LOGINFAILED; return -1; }
		m_confirmRequestID = request;
		wt_strcpy(req.BrokerID, m_strBroker.c_str());
		wt_strcpy(req.InvestorID, m_strUser.c_str());
	}

	fmt::format_to(req.ConfirmDate, "{}", TimeUtils::getCurDate());
	memcpy(req.ConfirmTime, TimeUtils::getLocalTime().c_str(), 8);

	int iResult = m_pUserAPI->ReqSettlementInfoConfirm(&req, static_cast<int>(request));
	if (iResult != 0)
	{
		m_wrapperState = WS_LOGINFAILED;
		write_log(m_sink, LL_ERROR, "[TraderCTP][{}-{}] Sending confirming of settlement data failed: {}", m_strBroker.c_str(), m_strUser.c_str(), iResult);
		if (m_sink)
		{
			std::string message = fmtutil::format("Sending confirming of settlement data failed: {}", iResult);
			m_sink->onLoginResult(false, message.c_str(), 0);
		}
		return -1;
	}

	return 0;
}

int TraderCTP::authenticate(uint64_t attempt)
{
	CThostFtdcReqAuthenticateField req;
	memset(&req, 0, sizeof(req));
	uint32_t request = 0;
	{
		std::lock_guard<std::mutex> lock(m_identityMutex);
		if (m_wrapperState != WS_LOGINING || m_loginAttempt != attempt) return -1;
		request = genRequestID();
		if (request == 0 || request > INT_MAX) return -1;
		m_authRequestID = request;
		wt_strcpy(req.BrokerID, m_strBroker.c_str(), m_strBroker.size());
		wt_strcpy(req.UserID, m_strUser.c_str(), m_strUser.size());
		wt_strcpy(req.AuthCode, m_strAuthCode.c_str(), m_strAuthCode.size());
		wt_strcpy(req.AppID, m_strAppID.c_str(), m_strAppID.size());
	}
	return m_pUserAPI->ReqAuthenticate(&req, static_cast<int>(request));
}

/*
void TraderCTP::triggerQuery()
{
	boost::asio::post(*m_strandIO, [this](){
		if (m_queQuery.empty() || m_bInQuery)
			return;

		uint64_t curTime = TimeUtils::getLocalTimeNow();
		if (curTime - m_lastQryTime < 1000)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			boost::asio::post(*m_strandIO, [this](){
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
