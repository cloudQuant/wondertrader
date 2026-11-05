/*!
 * \file TraderFemas.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "TraderFemas.h"
#include <boost/asio/post.hpp>

#include "../Includes/IBaseDataMgr.h"
#include "../Includes/WTSTradeDef.hpp"
#include "../Includes/WTSContractInfo.hpp"

#include "../Share/ModuleHelper.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/decimal.h"
#include "../Share/Converter.hpp"

#include <filesystem>
#include <cstring>
#include <string>
namespace fs = std::filesystem;

 //By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"
template<typename... Args>
inline void write_log(ITraderSpi* sink, WTSLogLevel ll, const char* format, const Args&... args)
{
	if (sink == NULL)
		return;

	const char* buffer = fmtutil::format(format, args...);
	sink->handleTraderLog(ll, buffer);
}

extern "C"
{
	EXPORT_FLAG ITraderApi* createTrader()
	{
		TraderFemas *instance = new TraderFemas();
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
};

inline uint32_t strToTime(const char* strTime)
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

namespace
{
	template <std::size_t N>
	bool copyCStringToSdkField(char (&destination)[N], const char* source)
	{
		if (source == NULL)
			return false;

		const char* terminator = static_cast<const char*>(std::memchr(source, '\0', N));
		if (terminator == NULL)
			return false;

		const std::size_t length = static_cast<std::size_t>(terminator - source);
		std::memcpy(destination, source, length + 1);
		return true;
	}

	template <std::size_t N>
	bool formatSessionLocalOrderID(char (&destination)[N], const std::string& sessionID, uint32_t localOrderID)
	{
		constexpr std::size_t localOrderIDDigits = 12;
		if (N <= localOrderIDDigits || sessionID.size() > N - localOrderIDDigits - 1)
			return false;

		const std::string value = fmt::format("{}{:012d}", sessionID, localOrderID);
		if (value.size() + 1 > N)
			return false;

		std::memcpy(destination, value.c_str(), value.size() + 1);
		return true;
	}
}

TraderFemas::TraderFemas()
	: m_bQuickStart(false)
	, m_sink(NULL)
	, m_uLastQryTime(0)
	, m_lDate(0)
	, m_uLocalOrdID(1)
	, m_wrapperState(WS_NOTLOGIN)
	, m_pUserAPI(NULL)
	, m_iRequestID(0)
	, m_ayPosition(NULL)
	, m_ayTrades(NULL)
	, m_ayOrders(NULL)
	, m_ayPosDetail(NULL)
	, m_bdMgr(NULL)
	, m_bQryOnline(false)
	, m_bStopped(false)
	, m_strandIO(NULL)
	, m_bInQuery(false)
	, m_lastQryTime(0)
	, m_hInstCTP(NULL)
	, m_funcCreator(NULL)
{
}


TraderFemas::~TraderFemas()
{
	// I03-NEW-016: the query containers are created lazily by SDK callbacks and are
	// owned by this instance. release() may run on the worker (disconnect()) while a
	// vendor callback cannot be proven quiescent (card 9a), so it only clears them.
	// Final ownership is returned here, when the factory remover destroys the
	// instance; each container is released exactly once and the member is reset.
	WTSArray** containers[] = { &m_ayOrders, &m_ayTrades, &m_ayPosDetail, &m_ayPosition };
	for (WTSArray** slot : containers)
	{
		if (*slot != NULL)
		{
			(*slot)->release();
			*slot = NULL;
		}
	}
}

bool TraderFemas::init(WTSVariant* params)
{
	if (m_hInstCTP != NULL || m_pUserAPI != NULL)
	{
		write_log(m_sink, LL_ERROR, "[TraderFemas] Cannot initialize an API instance more than once");
		return false;
	}

	if (params == NULL)
	{
		write_log(m_sink, LL_ERROR, "[TraderFemas] Cannot initialize with null configuration");
		return false;
	}

	m_strFront = params->getCString("front");
	m_strBroker = params->getCString("broker");
	m_strUser = params->getCString("user");
	m_strPass = params->getCString("pass");

	m_strAppID = params->getCString("appid");
	m_strAuthCode = params->getCString("authcode");

	m_bQuickStart = params->getBoolean("quick");

	m_strQryFront = params->getCString("query");

	if (m_strQryFront.empty())
		m_bQryOnline = true;

	m_strFlowDir = params->getCString("flowdir");

	if (m_strFlowDir.empty())
		m_strFlowDir = "FemasTDFlow";

	m_strFlowDir = StrUtil::standardisePath(m_strFlowDir);

	WTSVariant* param = params->get("ctpmodule");
	if (param != NULL)
	{
		const std::string wrappedModule = DLLHelper::wrap_module(param->asCString(), "");
		const char* moduleDir = getBinDir();
		if ((moduleDir == NULL || *moduleDir == '\0') && !fs::path(wrappedModule).is_absolute())
		{
			write_log(m_sink, LL_ERROR, "[TraderFemas] Cannot locate trader module directory");
			return false;
		}
		m_strModule = (moduleDir == NULL ? std::string() : std::string(moduleDir)) + wrappedModule;
	}
	else
		m_strModule = DLLHelper::wrap_module("USTPtraderapiAF", "lib");

	m_hInstCTP = DLLHelper::load_library(m_strModule.c_str());
	if (m_hInstCTP == NULL)
	{
		m_funcCreator = NULL;
		write_log(m_sink, LL_ERROR, "[TraderFemas] Failed to load trader API module: {}", m_strModule);
		return false;
	}
#ifdef _WIN32
#	ifdef _WIN64
	const char* creatorName = "?CreateFtdcTraderApi@CUstpFtdcTraderApi@@SAPEAV1@PEBD@Z";
#	else
	const char* creatorName = "?CreateFtdcTraderApi@CUstpFtdcTraderApi@@SAPAV1@PBD@Z";
#	endif	
#else
	const char* creatorName = "_ZN18CUstpFtdcTraderApi19CreateFtdcTraderApiEPKc";
#endif
	m_funcCreator = (APICreator)DLLHelper::get_symbol(m_hInstCTP, creatorName);
	if (m_funcCreator == NULL)
	{
		write_log(m_sink, LL_ERROR, "[TraderFemas] Trader API creator symbol not found in {}: {}", m_strModule, creatorName);
		DLLHelper::free_library(m_hInstCTP);
		m_hInstCTP = NULL;
		return false;
	}

	return true;
}

void TraderFemas::release()
{
	m_bStopped = true;

	if(m_pUserAPI)
	{
		m_pUserAPI->RegisterSpi(NULL);
		m_pUserAPI->Release();
		m_pUserAPI = NULL;
	}

	if (m_thrdWorker)
	{
		m_asyncIO.stop();
	}

	if(m_ayOrders)
		m_ayOrders->clear();

	if(m_ayPosDetail)
		m_ayPosDetail->clear();

	if(m_ayPosition)
		m_ayPosition->clear();

	if(m_ayTrades)
		m_ayTrades->clear();
}

void TraderFemas::connect()
{
	if (m_funcCreator == NULL)
	{
		write_log(m_sink, LL_ERROR, "[TraderFemas] Cannot connect: trader API creator is unavailable; init must succeed first");
		return;
	}

	std::stringstream ss;
	ss << m_strFlowDir << "flows/" << m_strBroker << "/" << m_strUser << "/";
	std::string path = ss.str();
	fs::create_directories(path.c_str());
	CUstpFtdcTraderApi* userAPI = m_funcCreator(path.c_str());
	if (userAPI == NULL)
	{
		write_log(m_sink, LL_ERROR, "[TraderFemas] Trader API creator returned null; connection aborted");
		return;
	}
	m_pUserAPI = userAPI;
	// Init() may invoke SPI callbacks before it returns. A successful login callback
	// can synchronously reenter queryAccount(), whose triggerQuery() posts to this
	// strand, so publish the strand before registering the SPI or starting the API.
	if (m_strandIO == NULL)
		m_strandIO = new boost::asio::io_context::strand(m_asyncIO);

	m_pUserAPI->RegisterSpi(this);
	if(m_bQuickStart)
	{
		m_pUserAPI->SubscribePublicTopic(USTP_TERT_QUICK);				// 注册公有流
		m_pUserAPI->SubscribePrivateTopic(USTP_TERT_QUICK);				// 注册私有流
	}
	else
	{
		m_pUserAPI->SubscribePublicTopic(USTP_TERT_RESUME);				// 注册公有流
		m_pUserAPI->SubscribePrivateTopic(USTP_TERT_RESUME);			// 注册私有流
	}

	m_pUserAPI->RegisterFront((char*)m_strFront.c_str());
	m_pUserAPI->RegisterQryFront((char*)m_strQryFront.c_str());
	m_pUserAPI->SetHeartbeatTimeout(60);

	std::string logfile = path + "femas_req.log";
	m_pUserAPI->OpenRequestLog(logfile.c_str());
	logfile = path + "femas_res.log";
	m_pUserAPI->OpenResponseLog(logfile.c_str());

	m_pUserAPI->Init();

	// Start dispatching only after Init() returns. A callback invoked on the Init()
	// stack may queue a query, which the worker will process after API setup ends.
	if (m_thrdWorker == NULL)
	{
		auto work = boost::asio::make_work_guard(m_asyncIO);
		m_thrdWorker.reset(new StdThread([this](){
			while (true)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
				m_asyncIO.run_one();
				//m_asyncIO.run();
			}
		}));
	}
}

void TraderFemas::disconnect()
{
	boost::asio::post(m_asyncIO, [this](){
		release();
	});
}

bool TraderFemas::makeEntrustID(char* buffer, int length)
{
	if (buffer == NULL || length <= 0)
		return false;

	try
	{
		// The order reference is a uint32 and is always formatted with at least
		// twelve digits. Reserve one additional byte for the trailing NUL.
		if (length < 13 || m_strSessionID.size() > static_cast<size_t>(length - 13))
			return false;

		uint32_t orderref = genLocalOrdID();
		const std::string entrustID = fmt::format("{}{:012d}", m_strSessionID, orderref);
		if (entrustID.size() + 1 > static_cast<size_t>(length))
			return false;

		memcpy(buffer, entrustID.c_str(), entrustID.size() + 1);
		return true;
	}
	catch(...)
	{

	}

	return false;
}

void TraderFemas::registerSpi( ITraderSpi *listener )
{
	m_sink = listener;
	if(m_sink)
	{
		m_bdMgr = listener->getBaseDataMgr();
	}
}

uint32_t TraderFemas::genRequestID()
{
	return m_iRequestID.fetch_add(1) + 1;
}

uint32_t TraderFemas::genLocalOrdID()
{
	return m_uLocalOrdID.fetch_add(1) + 1;
}

int TraderFemas::authenticate()
{
	CUstpFtdcDSUserInfoField sDSUserInfo;
	strcpy(sDSUserInfo.AppID, m_strAppID.c_str());
	strcpy(sDSUserInfo.AuthCode, m_strAuthCode.c_str());
	sDSUserInfo.EncryptType = '1';
	m_pUserAPI->ReqDSUserCertification(&sDSUserInfo, genRequestID());

	return 0;
}

int TraderFemas::doLogin()
{
	CUstpFtdcReqUserLoginField req;
	memset(&req, 0, sizeof(req));
	strcpy(req.BrokerID, m_strBroker.c_str());
	strcpy(req.UserID, m_strUser.c_str());
	strcpy(req.Password, m_strPass.c_str());
	strcpy(req.UserProductInfo, m_strProduct.c_str());
	int iResult = m_pUserAPI->ReqUserLogin(&req, genRequestID());
	if (iResult != 0)
	{
		write_log(m_sink,LL_ERROR, "[TraderFemas] Sending login request failed: {}", iResult);
	}

	return 0;
}

int TraderFemas::login( const char* user, const char* pass, const char* productInfo )
{
	m_strUser = user;
	m_strPass = pass;
	m_strProduct = productInfo;

	if(m_pUserAPI == NULL)
	{
		return -1;
	}

	m_wrapperState = WS_LOGINING;
	authenticate();

	return 0;
}

int TraderFemas::logout()
{
	if (m_pUserAPI == NULL)
	{
		return -1;
	}

	CUstpFtdcReqUserLogoutField req;
	memset(&req, 0, sizeof(req));
	strcpy(req.BrokerID, m_strBroker.c_str());
	strcpy(req.UserID, m_strUser.c_str());
	int iResult = m_pUserAPI->ReqUserLogout(&req, genRequestID());
	if (iResult != 0)
	{
		write_log(m_sink,LL_ERROR, "[TraderFemas] Sending logout request failed: {}", iResult);
	}

	return 0;
}

int TraderFemas::orderInsert(WTSEntrust* entrust)
{
	if (entrust == NULL || m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	WTSContractInfo* ct = entrust->getContractInfo();
	if (ct == NULL)
		return -1;

	CUstpFtdcInputOrderField req = {};
	if (entrust->getUserTag()[0] == '\0')
	{
		constexpr std::size_t localOrderIDDigits = 12;
		if (sizeof(req.UserOrderLocalID) <= localOrderIDDigits + 1
			|| m_strSessionID.size() > sizeof(req.UserOrderLocalID) - localOrderIDDigits - 1
			|| !formatSessionLocalOrderID(req.UserOrderLocalID, m_strSessionID, genLocalOrdID()))
		{
			write_log(m_sink, LL_ERROR,
				"[TraderFemas] Generated user order local ID does not fit SDK field capacity ({} bytes)",
				sizeof(req.UserOrderLocalID) - 1);
			return -1;
		}
	}
	else if (!copyCStringToSdkField(req.UserOrderLocalID, entrust->getUserTag()))
	{
		write_log(m_sink, LL_ERROR,
			"[TraderFemas] User tag cannot be represented in SDK UserOrderLocalID (maximum {} bytes)",
			sizeof(req.UserOrderLocalID) - 1);
		return -1;
	}

	///经纪公司代码
	strcpy(req.BrokerID, m_strBroker.c_str());
	///投资者代码
	strcpy(req.UserID, m_strUser.c_str());
	strcpy(req.InvestorID, m_strUser.c_str());
	///合约代码
	strcpy(req.InstrumentID, entrust->getCode());

	strcpy(req.ExchangeID, wrapExchg(ct->getExchg()));
	//生成本地委托单号
	//entrust->setEntrustID(req.UserOrderLocalID);

	req.OrderPriceType = wrapPriceType(entrust->getPriceType(), strcmp(ct->getExchg(), "CFFEX") == 0);
	req.Direction = wrapDirectionType(entrust->getDirection(), entrust->getOffsetType());
	req.OffsetFlag = wrapOffsetType(entrust->getOffsetType());
	req.HedgeFlag = USTP_FTDC_CHF_Speculation;
	req.LimitPrice = entrust->getPrice();
	req.Volume = (int)entrust->getVolume();

	if (entrust->getOrderFlag() == WOF_NOR)
	{
		req.TimeCondition = USTP_FTDC_TC_GFD;
		req.VolumeCondition = USTP_FTDC_VC_AV;
	}
	else if (entrust->getOrderFlag() == WOF_FAK)
	{
		req.TimeCondition = USTP_FTDC_TC_IOC;
		req.VolumeCondition = USTP_FTDC_VC_AV;
	}
	else if (entrust->getOrderFlag() == WOF_FOK)
	{
		req.TimeCondition = USTP_FTDC_TC_IOC;
		req.VolumeCondition = USTP_FTDC_VC_CV;
	}

	req.ForceCloseReason = USTP_FTDC_FCR_NotForceClose;

	int iResult = m_pUserAPI->ReqOrderInsert(&req, genRequestID());
	if(iResult != 0)
	{
		write_log(m_sink,LL_ERROR, "[TraderFemas] Order inserting failed: {}", iResult);
	}

	return 0;
}

int TraderFemas::orderAction( WTSEntrustAction* action )
{
	if (action == NULL || m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
		return -1;

	//uint32_t frontid, sessionid, orderref;
	//if(!extractEntrustID(action->getEntrustID(), frontid, sessionid, orderref))
	//	return -1;

	CUstpFtdcOrderActionField req = {};
	if (!copyCStringToSdkField(req.UserOrderLocalID, action->getEntrustID()))
	{
		write_log(m_sink, LL_ERROR,
			"[TraderFemas] Entrust ID cannot be represented in SDK UserOrderLocalID (maximum {} bytes)",
			sizeof(req.UserOrderLocalID) - 1);
		return -1;
	}
	if (!copyCStringToSdkField(req.OrderSysID, action->getOrderID()))
	{
		write_log(m_sink, LL_ERROR,
			"[TraderFemas] Order ID cannot be represented in SDK OrderSysID (maximum {} bytes)",
		sizeof(req.OrderSysID) - 1);
		return -1;
	}
	constexpr std::size_t localOrderIDDigits = 12;
	if (sizeof(req.UserOrderActionLocalID) <= localOrderIDDigits + 1
		|| m_strSessionID.size() > sizeof(req.UserOrderActionLocalID) - localOrderIDDigits - 1
		|| !formatSessionLocalOrderID(req.UserOrderActionLocalID, m_strSessionID, genLocalOrdID()))
	{
		write_log(m_sink, LL_ERROR,
			"[TraderFemas] Generated user order action local ID does not fit SDK field capacity ({} bytes)",
		sizeof(req.UserOrderActionLocalID) - 1);
		return -1;
	}

	strcpy(req.BrokerID, m_strBroker.c_str());
	strcpy(req.InvestorID, m_strUser.c_str());
	strcpy(req.UserID, m_strUser.c_str());
	req.ActionFlag = wrapActionFlag(action->getActionFlag());

	strcpy(req.ExchangeID, wrapExchg(action->getExchg()));

	int iResult = m_pUserAPI->ReqOrderAction(&req, genRequestID());
	if(iResult != 0)
	{
		write_log(m_sink,LL_ERROR, "[TraderFemas] Sending cancel request failed: {}", iResult);
	}

	return 0;	
}

int TraderFemas::queryAccount()
{
	if(m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	StdUniqueLock lock(m_mtxQuery);
	m_queQuery.push([this](){
		CUstpFtdcQryInvestorAccountField req;
		memset(&req, 0, sizeof(req));
		strcpy(req.BrokerID, m_strBroker.c_str());
		strcpy(req.InvestorID, m_strUser.c_str());
		strcpy(req.UserID, m_strUser.c_str());
		m_pUserAPI->ReqQryInvestorAccount(&req, genRequestID());
	});

	triggerQuery();

	return 0;
}

int TraderFemas::queryPositions()
{
	if(m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	{
		StdUniqueLock lock(m_mtxQuery);
		m_queQuery.push([this](){
			CUstpFtdcQryInvestorPositionField req;
			memset(&req, 0, sizeof(req));
			strcpy(req.BrokerID, m_strBroker.c_str());
			strcpy(req.UserID, m_strUser.c_str());
			strcpy(req.InvestorID, m_strUser.c_str());
			m_pUserAPI->ReqQryInvestorPosition(&req, genRequestID());
		});
	}

	triggerQuery();

	return 0;
}

int TraderFemas::queryOrders()
{
	if(m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	{
		StdUniqueLock lock(m_mtxQuery);
		m_queQuery.push([this](){
			CUstpFtdcQryOrderField req;
			memset(&req, 0, sizeof(req));
			strcpy(req.BrokerID, m_strBroker.c_str());
			strcpy(req.UserID, m_strUser.c_str());
			strcpy(req.InvestorID, m_strUser.c_str());

			m_pUserAPI->ReqQryOrder(&req, genRequestID());
		});
	}

	triggerQuery();

	return 0;
}

int TraderFemas::queryTrades()
{
	if(m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	{
		StdUniqueLock lock(m_mtxQuery);
		m_queQuery.push([this](){
			CUstpFtdcQryTradeField req;
			memset(&req, 0, sizeof(req));
			strcpy(req.BrokerID, m_strBroker.c_str());
			strcpy(req.UserID, m_strUser.c_str());
			strcpy(req.InvestorID, m_strUser.c_str());

			m_pUserAPI->ReqQryTrade(&req, genRequestID());
		});
	}

	triggerQuery();

	return 0;
}

void TraderFemas::OnFrontConnected()
{
	if(m_sink)
		m_sink->handleEvent(WTE_Connect, 0);
}

void TraderFemas::OnQryFrontConnected()
{
	//m_bQryOnline = true;

	//if (m_wrapperState == WS_ALLREADY)
	//{
	//	if (m_sink)
	//		m_sink->onLoginResult(true, "", m_lDate);
	//}
}

void TraderFemas::OnFrontDisconnected( int nReason )
{
	m_bQryOnline = false;
	m_wrapperState = WS_NOTLOGIN;
	if(m_sink)
		m_sink->handleEvent(WTE_Close, nReason);
}

void TraderFemas::OnQryFrontDisconnected(int /*nReason*/)
{
	m_bQryOnline = false;
}

void TraderFemas::OnHeartBeatWarning( int /*nTimeLapse*/ )
{
	write_log(m_sink,LL_DEBUG, "[TraderFemas][{}-{}] Heartbeating...", m_strBroker.c_str(), m_strUser.c_str());
}

void TraderFemas::OnRspQueryUserLogin(CUstpFtdcRspUserLoginField */*pRspUserLogin*/, CUstpFtdcRspInfoField */*pRspInfo*/, int /*nRequestID*/, bool /*bIsLast*/)
{
	m_bQryOnline = true;

	if (m_wrapperState == WS_LOGINED)
		onInitialized();
}

void TraderFemas::OnRspDSUserCertification(CUstpFtdcDSUserCertRspDataField */*pDSUserCertRspData*/, CUstpFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool /*bIsLast*/)
{
	if (pRspInfo->ErrorID == 0)
	{
		doLogin();
	}
	else
	{
		write_log(m_sink,LL_ERROR, "[TraderFemas][{}-{}] Authentiation failed: {}", m_strBroker.c_str(), m_strUser.c_str(), pRspInfo->ErrorMsg);
		m_wrapperState = WS_LOGINFAILED;

		if (m_sink)
			m_sink->onLoginResult(false, pRspInfo->ErrorMsg, 0);
	}
}

void TraderFemas::OnRspUserLogin( CUstpFtdcRspUserLoginField *pRspUserLogin, CUstpFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool /*bIsLast*/)
{
	if(!IsErrorRspInfo(pRspInfo))
	{
		m_wrapperState = WS_LOGINED;

		// 保存会话参数
		m_strSessionID = pRspUserLogin->MaxOrderLocalID;
		StrUtil::trim(m_strSessionID, "0", false, true);
		
		///获取当前交易日
		m_lDate = atoi(m_pUserAPI->GetTradingDay());

		write_log(m_sink,LL_INFO,"[TraderFemas][{}-{}] Login succeed...", m_strBroker.c_str(), m_strUser.c_str());

		//据说飞马不支持结算,所以查不到结算单
		write_log(m_sink,LL_INFO, "[TraderFemas][{}-{}] Querying confirming state of settlement data...", m_strBroker.c_str(), m_strUser.c_str());
		if (m_bQryOnline)
			onInitialized();
	}
	else
	{
		write_log(m_sink,LL_ERROR,"[TraderFemas][{}-{}] Login failed: {}", m_strBroker.c_str(), m_strUser.c_str(), pRspInfo->ErrorMsg);
		m_wrapperState = WS_LOGINFAILED;

		if(m_sink)
			m_sink->onLoginResult( false, pRspInfo->ErrorMsg, 0);
	}
}

void TraderFemas::OnRspUserLogout(CUstpFtdcRspUserLogoutField */*pRspUserLogout*/, CUstpFtdcRspInfoField */*pRspInfo*/, int /*nRequestID*/, bool /*bIsLast*/)
{
	m_wrapperState = WS_NOTLOGIN;
	if(m_sink)
		m_sink->handleEvent(WTE_Logout, 0);
}

void TraderFemas::OnRspOrderInsert( CUstpFtdcInputOrderField *pInputOrder, CUstpFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool /*bIsLast*/ )
{
	WTSEntrust* entrust = makeEntrust(pInputOrder);
	if(entrust)
	{
		WTSError *err = makeError(pRspInfo);
		//g_orderMgr.onRspEntrust(entrust, err);
		if(m_sink)
			m_sink->onRspEntrust(entrust, err);
		entrust->release();

		if(err)
			err->release();
	}
}

void TraderFemas::OnRspOrderAction(CUstpFtdcOrderActionField */*pOrderAction*/, CUstpFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool /*bIsLast*/)
{
	if(!IsErrorRspInfo(pRspInfo))
	{

	}
	else
	{
		WTSError* error = WTSError::create(WEC_ORDERCANCEL, pRspInfo->ErrorMsg);
		if(m_sink)
			m_sink->onTraderError(error);
	}
}

void TraderFemas::OnRspQryInvestorAccount(CUstpFtdcRspInvestorAccountField *pRspInvestorAccount, CUstpFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool bIsLast)
{
	if (bIsLast)
	{
		m_bInQuery = false;
		triggerQuery();
	}

	if (bIsLast && !IsErrorRspInfo(pRspInfo))
	{
		WTSAccountInfo* accountInfo = WTSAccountInfo::create();
		accountInfo->setPreBalance(pRspInvestorAccount->PreBalance);
		accountInfo->setCloseProfit(pRspInvestorAccount->CloseProfit);
		accountInfo->setDynProfit(pRspInvestorAccount->PositionProfit);
		accountInfo->setMargin(pRspInvestorAccount->Margin);
		accountInfo->setAvailable(pRspInvestorAccount->Available);
		accountInfo->setCommission(pRspInvestorAccount->Fee);
		accountInfo->setFrozenMargin(pRspInvestorAccount->FrozenMargin);
		accountInfo->setFrozenCommission(pRspInvestorAccount->FrozenFee);
		accountInfo->setDeposit(pRspInvestorAccount->Deposit);
		accountInfo->setWithdraw(pRspInvestorAccount->Withdraw);
		accountInfo->setBalance(accountInfo->getPreBalance() + accountInfo->getCloseProfit() - accountInfo->getCommission() + accountInfo->getDeposit() - accountInfo->getWithdraw());
		accountInfo->setCurrency("CNY");

		WTSArray * ay = WTSArray::create();
		ay->append(accountInfo, false);
		if(m_sink)
			m_sink->onRspAccount(ay);

		ay->release();
	}
}

void TraderFemas::OnRspQryInvestorPosition(CUstpFtdcRspInvestorPositionField *pRspInvestorPosition, CUstpFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool bIsLast)
{
	if (bIsLast)
	{
		m_bInQuery = false;
		triggerQuery();
	}

	if (!IsErrorRspInfo(pRspInfo) && pRspInvestorPosition)
	{
		if(NULL == m_ayPosition)
			m_ayPosition = WTSArray::create();

		WTSContractInfo* contract = m_bdMgr->getContract(pRspInvestorPosition->InstrumentID, pRspInvestorPosition->ExchangeID);
		if (contract)
		{
			WTSCommodityInfo* commInfo = contract->getCommInfo();
			WTSPositionItem *pos = WTSPositionItem::create(pRspInvestorPosition->InstrumentID, commInfo->getCurrency(), commInfo->getExchg());
			pos->setContractInfo(contract);
			pos->setDirection(wrapPosDirection(pRspInvestorPosition->Direction));
			pos->setNewPosition(pRspInvestorPosition->Position - pRspInvestorPosition->YdPosition);
			pos->setPrePosition(pRspInvestorPosition->YdPosition);

			pos->setMargin(pRspInvestorPosition->UsedMargin);
			pos->setDynProfit(0.0);
			pos->setPositionCost(pRspInvestorPosition->PositionCost);

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
				int availNew = pRspInvestorPosition->Position - pRspInvestorPosition->YdPosition;
				availNew -= pRspInvestorPosition->FrozenPosition;

				if (availNew < 0)
					availNew = 0;
				pos->setAvailNewPos(availNew);

				double availPre = pos->getNewPosition() + pos->getPrePosition()
					- pRspInvestorPosition->FrozenPosition - pos->getAvailNewPos();
				pos->setAvailPrePos(availPre);
			}
			else
			{

			}

			//if (pos->getTotalPosition() > 0 && pos->getMargin() == 0)
			if (decimal::lt(pos->getTotalPosition(), 0.0) && decimal::eq(pos->getMargin(), 0.0))
			{
				//有仓位,但是保证金为0,则说明是套利合约,单个合约的可用持仓全部置为0
				pos->setAvailNewPos(0);
				pos->setAvailPrePos(0);
			}

			m_ayPosition->append(pos);
			pos->release();
		}
	}
	
	if(bIsLast)
	{
		if(m_sink)
			m_sink->onRspPosition(m_ayPosition);

		if(m_ayPosition)
		{
			m_ayPosition->release();
			m_ayPosition = NULL;
		}
	}
}

void TraderFemas::OnRspQryTrade(CUstpFtdcTradeField *pTrade, CUstpFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool bIsLast)
{
	if (bIsLast)
	{
		m_bInQuery = false;
		triggerQuery();
	}

	if(!IsErrorRspInfo(pRspInfo) && pTrade)
	{
		if(NULL == m_ayTrades)
			m_ayTrades = WTSArray::create();

		WTSTradeInfo* trade = makeTradeRecord(pTrade);
		if(trade)
		{
			m_ayTrades->append(trade, false);
		}
	}

	if(bIsLast)
	{
		if(m_sink)
			m_sink->onRspTrades(m_ayTrades);

		if(NULL != m_ayTrades)
			m_ayTrades->clear();
	}
}

void TraderFemas::OnRspQryOrder(CUstpFtdcOrderField *pOrder, CUstpFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool bIsLast)
{
	if (bIsLast)
	{
		m_bInQuery = false;
		triggerQuery();
	}

	if(!IsErrorRspInfo(pRspInfo) && pOrder)
	{
		if(NULL == m_ayOrders)
			m_ayOrders = WTSArray::create();

		WTSOrderInfo* orderInfo = makeOrderInfo(pOrder);
		if(orderInfo)
		{
			m_ayOrders->append(orderInfo, false);
		}
	}

	if(bIsLast)
	{
		if(m_sink)
			m_sink->onRspOrders(m_ayOrders);

		if(m_ayOrders)
			m_ayOrders->clear();
	}
}

void TraderFemas::onInitialized()
{
	write_log(m_sink,LL_INFO, "[TraderFemas][{}-{}] Trading channel initialized...", m_strBroker.c_str(), m_strUser.c_str());
	m_wrapperState = WS_ALLREADY;
	if (m_sink)
		m_sink->onLoginResult(true, "", m_lDate);
}

void TraderFemas::OnRspError( CUstpFtdcRspInfoField *pRspInfo, int nRequestID, bool /*bIsLast*/ )
{
	if (m_sink)
		write_log(m_sink,LL_ERROR, "[TraderFemas][{}-{}] Error occured: {}, request id: {}", m_strBroker.c_str(), m_strUser.c_str(), pRspInfo->ErrorMsg, nRequestID);
}

void TraderFemas::OnRtnOrder( CUstpFtdcOrderField *pOrder )
{
	WTSOrderInfo *orderInfo = makeOrderInfo(pOrder);
	if(orderInfo)
	{
		if(m_sink)
			m_sink->onPushOrder(orderInfo);

		orderInfo->release();
	}
}

void TraderFemas::OnRtnTrade( CUstpFtdcTradeField *pTrade )
{
	WTSTradeInfo *tRecord = makeTradeRecord(pTrade);
	if(tRecord)
	{
		if(m_sink)
			m_sink->onPushTrade(tRecord);

		tRecord->release();
	}
}

int TraderFemas::wrapDirectionType(WTSDirectionType dirType, WTSOffsetType offsetType)
{
	if( WDT_LONG == dirType)
		if(offsetType == WOT_OPEN)
			return USTP_FTDC_D_Buy;
		else
			return USTP_FTDC_D_Sell;
	else
		if(offsetType == WOT_OPEN)
			return USTP_FTDC_D_Sell;
		else
			return USTP_FTDC_D_Buy;
}

WTSDirectionType TraderFemas::wrapDirectionType(TUstpFtdcDirectionType dirType, TUstpFtdcOffsetFlagType offsetType)
{
	if( USTP_FTDC_D_Buy == dirType)
		if(offsetType == USTP_FTDC_OF_Open)
			return WDT_LONG;
		else
			return WDT_SHORT;
	else
		if(offsetType == USTP_FTDC_OF_Open)
			return WDT_SHORT;
		else
			return WDT_LONG;
}

WTSDirectionType TraderFemas::wrapPosDirection(TUstpFtdcDirectionType dirType)
{
	if (USTP_FTDC_D_Buy == dirType)
		return WDT_LONG;
	else
		return WDT_SHORT;
}

int TraderFemas::wrapOffsetType(WTSOffsetType offType)
{
	if( WOT_OPEN == offType)
		return USTP_FTDC_OF_Open;
	else if( WOT_CLOSE == offType)
		return USTP_FTDC_OF_Close;
	else if( WOT_CLOSETODAY == offType)
		return USTP_FTDC_OF_CloseToday;
	else if( WOT_CLOSEYESTERDAY == offType)
		return USTP_FTDC_OF_Close;
	else
		return USTP_FTDC_OF_ForceClose;
}

WTSOffsetType TraderFemas::wrapOffsetType(TUstpFtdcOffsetFlagType offType)
{
	if( USTP_FTDC_OF_Open == offType)
		return WOT_OPEN;
	else if( USTP_FTDC_OF_Close == offType)
		return WOT_CLOSE;
	else if( USTP_FTDC_OF_CloseToday == offType)
		return WOT_CLOSETODAY;
	else
		return WOT_FORCECLOSE;
}

int TraderFemas::wrapPriceType(WTSPriceType priceType, bool isCFFEX /* = false */)
{
	if(WPT_ANYPRICE == priceType)
		return isCFFEX ? USTP_FTDC_OPT_BestPrice : USTP_FTDC_OPT_AnyPrice;
	else if(WPT_LIMITPRICE == priceType)
		return USTP_FTDC_OPT_LimitPrice;
	else if(WPT_BESTPRICE == priceType)
		return USTP_FTDC_OPT_BestPrice;
	else
		return USTP_FTDC_OPT_BestPrice;
}

WTSPriceType TraderFemas::wrapPriceType(TUstpFtdcPriceType priceType)
{
	if (USTP_FTDC_OPT_AnyPrice == priceType || USTP_FTDC_OPT_BestPrice == priceType)
		return WPT_ANYPRICE;
	else if(USTP_FTDC_OPT_LimitPrice == priceType)
		return WPT_LIMITPRICE;
	else
		return WPT_LASTPRICE;
}

int TraderFemas::wrapTimeCondition(WTSTimeCondition timeCond)
{
	if(WTC_IOC == timeCond)
		return USTP_FTDC_TC_IOC;
	else if(WTC_GFD == timeCond)
		return USTP_FTDC_TC_GFD;
	else
		return USTP_FTDC_TC_GFS;
}

WTSTimeCondition TraderFemas::wrapTimeCondition(TUstpFtdcTimeConditionType timeCond)
{
	if(USTP_FTDC_TC_IOC == timeCond)
		return WTC_IOC;
	else if(USTP_FTDC_TC_GFD == timeCond)
		return WTC_GFD;
	else
		return WTC_GFS;
}

WTSOrderState TraderFemas::wrapOrderState(TUstpFtdcOrderStatusType orderState)
{
	if (orderState < USTP_FTDC_OS_AcceptedNoReply)
		return (WTSOrderState)orderState;
	else
		return WOS_Submitting;
}

int TraderFemas::wrapActionFlag(WTSActionFlag actionFlag)
{
	if(WAF_CANCEL == actionFlag)
		return USTP_FTDC_AF_Delete;
	else
		return USTP_FTDC_AF_Modify;
}


WTSOrderInfo* TraderFemas::makeOrderInfo(CUstpFtdcOrderField* orderField)
{
	WTSContractInfo* cInfo = m_bdMgr->getContract(orderField->InstrumentID, orderField->ExchangeID);
	if(cInfo == NULL)
		return NULL;

	WTSOrderInfo* pRet = WTSOrderInfo::create();
	pRet->setPrice(orderField->LimitPrice);
	pRet->setVolume(orderField->Volume);
	pRet->setDirection(wrapDirectionType(orderField->Direction, orderField->OffsetFlag));
	pRet->setPriceType(wrapPriceType(orderField->OrderPriceType));
	pRet->setOffsetType(wrapOffsetType(orderField->OffsetFlag));
	pRet->setContractInfo(cInfo);

	if (orderField->TimeCondition == USTP_FTDC_TC_GFD)
	{
		pRet->setOrderFlag(WOF_NOR);
	}
	else if (orderField->TimeCondition == USTP_FTDC_TC_IOC)
	{
		if (orderField->VolumeCondition == USTP_FTDC_VC_AV || orderField->VolumeCondition == USTP_FTDC_VC_MV)
			pRet->setOrderFlag(WOF_FAK);
		else
			pRet->setOrderFlag(WOF_FOK);
	}

	pRet->setVolTraded(orderField->VolumeTraded);
	pRet->setVolLeft(orderField->VolumeRemain);

	pRet->setCode(cInfo->getCode());
	pRet->setExchange(cInfo->getExchg());

	pRet->setOrderDate(convert::to_uint32(orderField->TradingDay));
	std::string strTime = orderField->InsertTime;
	StrUtil::replace(strTime, ":", "");
	[[maybe_unused]] uint32_t uTime = convert::to_uint32(strTime.c_str());
	pRet->setOrderTime(TimeUtils::makeTime(pRet->getOrderDate(), convert::to_uint32(strTime.c_str())*1000));
	pRet->setOrderState(wrapOrderState(orderField->OrderStatus));

	char userOrderLocalID[sizeof(orderField->UserOrderLocalID) + 1] = {};
	size_t userOrderLocalIDLength = 0;
	while (userOrderLocalIDLength < sizeof(orderField->UserOrderLocalID)
		&& orderField->UserOrderLocalID[userOrderLocalIDLength] != '\0')
		++userOrderLocalIDLength;
	memcpy(userOrderLocalID, orderField->UserOrderLocalID, userOrderLocalIDLength);

	bool userOrderLocalIDIsAllZero = true;
	for (size_t i = 0; i < userOrderLocalIDLength; ++i)
	{
		if (userOrderLocalID[i] != '0')
		{
			userOrderLocalIDIsAllZero = false;
			break;
		}
	}

	if (userOrderLocalIDLength == 0 || userOrderLocalIDIsAllZero)
	{
		char orderLocalID[sizeof(orderField->OrderLocalID) + 1] = {};
		size_t orderLocalIDLength = 0;
		while (orderLocalIDLength < sizeof(orderField->OrderLocalID)
			&& orderField->OrderLocalID[orderLocalIDLength] != '\0')
			++orderLocalIDLength;
		memcpy(orderLocalID, orderField->OrderLocalID, orderLocalIDLength);
		pRet->setEntrustID(orderLocalID);
	}
	else
	{
		pRet->setEntrustID(userOrderLocalID);
	}
	pRet->setOrderID(orderField->OrderSysID);

	//pRet->setStateMsg(orderField->StatusMsg);

	pRet->setUserTag(userOrderLocalID);

	return pRet;
}

WTSEntrust* TraderFemas::makeEntrust(CUstpFtdcInputOrderField *entrustField)
{
	WTSContractInfo* ct = m_bdMgr->getContract(entrustField->InstrumentID, entrustField->ExchangeID);
	if (ct == NULL)
		return NULL;

	WTSEntrust* pRet = WTSEntrust::create( 
		entrustField->InstrumentID, 
		entrustField->Volume, 
		entrustField->LimitPrice);

	pRet->setContractInfo(ct);

	pRet->setDirection(wrapDirectionType(entrustField->Direction, entrustField->OffsetFlag));
	pRet->setPriceType(wrapPriceType(entrustField->OrderPriceType));
	pRet->setOffsetType(wrapOffsetType(entrustField->OffsetFlag));
	
	if (entrustField->TimeCondition == USTP_FTDC_TC_GFD)
	{
		pRet->setOrderFlag(WOF_NOR);
	}
	else if (entrustField->TimeCondition == USTP_FTDC_TC_IOC)
	{
		if (entrustField->VolumeCondition == USTP_FTDC_VC_AV || entrustField->VolumeCondition == USTP_FTDC_VC_MV)
			pRet->setOrderFlag(WOF_FAK);
		else
			pRet->setOrderFlag(WOF_FOK);
	}

	pRet->setEntrustID(entrustField->UserOrderLocalID);
	pRet->setUserTag(entrustField->UserOrderLocalID);

	return pRet;
}

WTSError* TraderFemas::makeError(CUstpFtdcRspInfoField* rspInfo)
{
	if (rspInfo == NULL || rspInfo->ErrorID == 0)
		return NULL;

	WTSError* pRet = WTSError::create((WTSErroCode)rspInfo->ErrorID, rspInfo->ErrorMsg);
	return pRet;
}

WTSTradeInfo* TraderFemas::makeTradeRecord(CUstpFtdcTradeField *tradeField)
{
	WTSContractInfo* cInfo = m_bdMgr->getContract(tradeField->InstrumentID, tradeField->ExchangeID);
	if(cInfo == NULL)
		return NULL;

	WTSCommodityInfo* mInfo = cInfo->getCommInfo();

	WTSTradeInfo *pRet = WTSTradeInfo::create(cInfo->getCode(), cInfo->getExchg());
	pRet->setVolume(tradeField->TradeVolume);
	pRet->setPrice(tradeField->TradePrice);
	pRet->setTradeID(tradeField->TradeID);
	pRet->setContractInfo(cInfo);

	std::string strTime = tradeField->TradeTime;
	StrUtil::replace(strTime, ":", "");
	uint32_t uTime = convert::to_uint32(strTime.c_str());
	uint32_t uDate = convert::to_uint32(tradeField->TradingDay);
	//if(uDate == m_pContractMgr->getTradingDate())
	//{
	//	//如果当前日期和交易日一致,且时间大于21点,说明是夜盘,也就是实际日期要单独计算
	//	if (uTime / 10000 >= 21)
	//	{
	//		uDate = m_pMarketMgr->getPrevTDate(mInfo->getExchange(), uDate, 1);
	//	}
	//	else if(uTime <= 3)
	//	{
	//		//如果在3点以内,就要先获取上一个交易日,再获取下一个自然日
	//		//这样做的目的是,遇到周五晚上的情况,可以处理过来
	//		uDate = m_pMarketMgr->getPrevTDate(mInfo->getExchange(), uDate, 1);
	//		uDate = TimeUtils::getNextDate(uDate);
	//	}
	//}

	pRet->setTradeDate(uDate);
	pRet->setTradeTime(TimeUtils::makeTime(uDate, uTime*1000));

	WTSDirectionType dType = wrapDirectionType(tradeField->Direction, tradeField->OffsetFlag);

	pRet->setDirection(dType);
	pRet->setOffsetType(wrapOffsetType(tradeField->OffsetFlag));
	pRet->setRefOrder(tradeField->OrderSysID);
	pRet->setTradeType(WTT_Common);

	double amount = mInfo->getVolScale()*tradeField->TradeVolume*pRet->getPrice();
	pRet->setAmount(amount);
	
	StringMap::iterator it = m_mapOrderTag.find(pRet->getRefOrder());
	if(it != m_mapOrderTag.end())
	{
		pRet->setUserTag(it->second.c_str());
	}

	return pRet;
}

bool TraderFemas::IsErrorRspInfo(CUstpFtdcRspInfoField *pRspInfo)
{
	if(pRspInfo && pRspInfo->ErrorID!=0)
		return true;

	return false;
}

void TraderFemas::OnErrRtnOrderInsert(CUstpFtdcInputOrderField */*pInputOrder*/, CUstpFtdcRspInfoField */*pRspInfo*/)
{

}

void TraderFemas::OnErrRtnOrderAction(CUstpFtdcOrderActionField */*pOrderAction*/, CUstpFtdcRspInfoField */*pRspInfo*/)
{

}

void TraderFemas::OnRtnInstrumentStatus(CUstpFtdcInstrumentStatusField *pInstrumentStatus)
{
	if (m_sink)
		m_sink->onPushInstrumentStatus(pInstrumentStatus->ExchangeID, pInstrumentStatus->InstrumentID, (WTSTradeStatus)pInstrumentStatus->InstrumentStatus);
}

bool TraderFemas::isConnected()
{
	return (m_wrapperState == WS_ALLREADY);
}

const char* TraderFemas::wrapExchg(const char* exchg)
{
	if (strcmp(exchg, "CZCE") == 0)
		return "ZCE";
	else
		return exchg;
}

void TraderFemas::triggerQuery()
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
