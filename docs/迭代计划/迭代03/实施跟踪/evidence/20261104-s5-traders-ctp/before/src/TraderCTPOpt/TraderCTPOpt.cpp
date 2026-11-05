/*!
 * \file TraderCTPOpt.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "TraderCTPOpt.h"
#include <boost/asio/post.hpp>
#include <cstdio>

#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/WTSTradeDef.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/IBaseDataMgr.h"

#include "../Share/ModuleHelper.hpp"
#include "../Share/decimal.h"
#include "../Share/Converter.hpp"

#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;

const char* ENTRUST_SECTION = "entrusts";
const char* ORDER_SECTION = "orders";

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

	return convert::to_uint32(str.c_str());
}

extern "C"
{
	EXPORT_FLAG ITraderApi* createTrader()
	{
		TraderCTPOpt *instance = new TraderCTPOpt();
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

inline int wrapDirectionType(WTSDirectionType dirType, WTSOffsetType offsetType)
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

inline int wrapPosDirType(WTSDirectionType dirType)
{
	if (WDT_LONG == dirType)
		return THOST_FTDC_PD_Long;
	else if (WDT_SHORT == dirType)
		return THOST_FTDC_PD_Short;
	else
		return THOST_FTDC_PD_Net;
}

inline WTSDirectionType wrapPosDirType(TThostFtdcPosiDirectionType dirType)
{
	if (THOST_FTDC_PD_Long == dirType)
		return WDT_LONG;
	else if (THOST_FTDC_PD_Short == dirType)
		return WDT_SHORT;
	else
		return WDT_NET;
}


inline WTSDirectionType wrapDirectionType(TThostFtdcDirectionType dirType, TThostFtdcOffsetFlagType offsetType)
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

inline WTSDirectionType wrapPosDirection(TThostFtdcPosiDirectionType dirType)
{
	if (THOST_FTDC_PD_Long == dirType)
		return WDT_LONG;
	else
		return WDT_SHORT;
}

inline int wrapOffsetType(WTSOffsetType offType)
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

inline WTSOffsetType wrapOffsetType(TThostFtdcOffsetFlagType offType)
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

inline int wrapPriceType(WTSPriceType priceType, bool isCFFEX /* = false */)
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

inline WTSPriceType wrapPriceType(TThostFtdcOrderPriceTypeType priceType)
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

inline int wrapTimeCondition(WTSTimeCondition timeCond)
{
	if (WTC_IOC == timeCond)
		return THOST_FTDC_TC_IOC;
	else if (WTC_GFD == timeCond)
		return THOST_FTDC_TC_GFD;
	else
		return THOST_FTDC_TC_GFS;
}

inline WTSTimeCondition wrapTimeCondition(TThostFtdcTimeConditionType timeCond)
{
	if (THOST_FTDC_TC_IOC == timeCond)
		return WTC_IOC;
	else if (THOST_FTDC_TC_GFD == timeCond)
		return WTC_GFD;
	else
		return WTC_GFS;
}

inline WTSOrderState wrapOrderState(TThostFtdcOrderStatusType orderState)
{
	if (orderState != THOST_FTDC_OST_Unknown)
		return (WTSOrderState)orderState;
	else
		return WOS_Submitting;
}

inline int wrapActionFlag(WTSActionFlag actionFlag)
{
	if (WAF_CANCEL == actionFlag)
		return THOST_FTDC_AF_Delete;
	else
		return THOST_FTDC_AF_Modify;
}

TraderCTPOpt::TraderCTPOpt()
	: m_bQuickStart(false)
	, m_bscSink(NULL)
	, m_optSink(NULL)
	, m_uLastQryTime(0)
	, m_wrapperState(WS_NOTLOGIN)
	, m_pUserAPI(NULL)
	, m_iRequestID(0)
	, m_mapPosition(NULL)
	, m_bPositionQueryIncomplete(false)
	, m_iPosQryReqID(0)
	, m_iPosAggReqID(0)
	, m_ayTrades(NULL)
	, m_ayOrders(NULL)
	, m_ayExecOrds(NULL)
	, m_ayPosDetail(NULL)
	, m_bInQuery(false)
	, m_lastQryTime(0)
	, m_bStopped(false)
	, m_hInstCTP(NULL)
	, m_funcCreator(NULL)
	{
}


TraderCTPOpt::~TraderCTPOpt()
{
}

bool TraderCTPOpt::init(WTSVariant* params)
{
	if (params == NULL)
	{
		write_log(m_bscSink, LL_ERROR, "[TraderCTPOpt] Init failed: missing configuration");
		return false;
	}

	if (m_hInstCTP != NULL || m_pUserAPI != NULL)
	{
		write_log(m_bscSink, LL_ERROR, "[TraderCTPOpt] Init failed: module or API is already active");
		return false;
	}

	m_strFront = params->getCString("front");
	m_strBroker = params->getCString("broker");
	m_strUser = params->getCString("user");
	m_strPass = params->getCString("pass");

	m_strAppID = params->getCString("appid");
	m_strAuthCode = params->getCString("authcode");

	m_strFlowDir = params->getCString("flowdir");

	if (m_strFlowDir.empty())
		m_strFlowDir = "CTPOptTDFlow";

	m_strFlowDir = StrUtil::standardisePath(m_strFlowDir);

	std::string module = params->getCString("ctpmodule");
	if (module.empty())
		module = "soptthosttraderapi_se";

	const std::string moduleFile = DLLHelper::wrap_module(module.c_str(), "");
	const char* moduleDir = getBinDir();
	const bool noModuleDir = moduleDir == NULL || moduleDir[0] == '\0';
	if (noModuleDir && !fs::path(moduleFile).is_absolute())
	{
		write_log(m_bscSink, LL_ERROR, "[TraderCTPOpt] Failed to determine CTPOpt API module directory");
		return false;
	}

	m_strModule = noModuleDir ? moduleFile : moduleDir + moduleFile;

	m_hInstCTP = DLLHelper::load_library(m_strModule.c_str());
	if (m_hInstCTP == NULL)
	{
		m_funcCreator = NULL;
		write_log(m_bscSink, LL_ERROR, "[TraderCTPOpt] Failed to load CTPOpt API module: {}", m_strModule);
		return false;
	}
#ifdef _WIN32
#	ifdef _WIN64
	const char* creatorName = "?CreateFtdcTraderApi@CThostFtdcTraderApi@ctp_sopt@@SAPEAV12@PEBD@Z";
#	else
	const char* creatorName = "?CreateFtdcTraderApi@CThostFtdcTraderApi@ctp_sopt@@SAPAV12@PBD@Z";
#	endif
#else
	const char* creatorName = "_ZN8ctp_sopt19CThostFtdcTraderApi19CreateFtdcTraderApiEPKc";
#endif
	m_funcCreator = (CTPCreator)DLLHelper::get_symbol(m_hInstCTP, creatorName);
	if (m_funcCreator == NULL)
	{
		write_log(m_bscSink, LL_ERROR, "[TraderCTPOpt] CTPOpt API creator symbol is missing: {}", m_strModule);
		DLLHelper::free_library(m_hInstCTP);
		m_hInstCTP = NULL;
		return false;
	}

	m_bQuickStart = params->getBoolean("quick");

	return true;
}

void TraderCTPOpt::release()
{
	if (m_pUserAPI)
	{
		CThostFtdcTraderApi* api = m_pUserAPI;
		m_pUserAPI = NULL;
		//m_pUserAPI->RegisterSpi(NULL);
		api->Release();
	}

	if (m_ayOrders)
		m_ayOrders->clear();

	if (m_ayPosDetail)
		m_ayPosDetail->clear();

	if (m_mapPosition)
		m_mapPosition->clear();
	m_bPositionQueryIncomplete = false;
	m_iPosQryReqID.store(0);
	m_iPosAggReqID = 0;

	if (m_ayTrades)
		m_ayTrades->clear();

	if (m_ayExecOrds)
		m_ayExecOrds->clear();
}

void TraderCTPOpt::connect()
{
	if (m_funcCreator == NULL)
	{
		write_log(m_bscSink, LL_ERROR, "[TraderCTPOpt] Connect failed: CTPOpt API creator is unavailable");
		if (m_bscSink)
			m_bscSink->handleEvent(WTE_Connect, -1);
		return;
	}

	std::stringstream ss;
	ss << m_strFlowDir << "flows/" << m_strBroker << "/" << m_strUser << "/";
	if(!StdFile::exists(ss.str().c_str()))
		fs::create_directories(ss.str().c_str());
	CThostFtdcTraderApi* api = m_funcCreator(ss.str().c_str());
	if (api == NULL)
	{
		write_log(m_bscSink, LL_ERROR, "[TraderCTPOpt] Connect failed: CTPOpt API creator returned NULL");
		if (m_bscSink)
			m_bscSink->handleEvent(WTE_Connect, -1);
		return;
	}

	m_pUserAPI = api;
	m_wrapperState = WS_NOTLOGIN;
	m_bStopped.store(false);
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

	m_pUserAPI->RegisterFront((char*)m_strFront.c_str());

	if (m_pUserAPI)
	{
		m_pUserAPI->Init();
	}

	if (m_thrdWorker == NULL)
	{
		m_thrdWorker.reset(new StdThread([this]() {
			while (!m_bStopped.load())
			{
				if (m_queQuery.empty() || m_bInQuery)
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

void TraderCTPOpt::disconnect()
{
	m_bStopped.store(true);
	if (m_thrdWorker)
	{
		m_thrdWorker->join();
		m_thrdWorker = NULL;
	}
	release();
}

bool TraderCTPOpt::makeEntrustID(char* buffer, int length)
{
	if (buffer == NULL || length == 0)
		return false;

	try
	{
		uint32_t orderref = m_orderRef.fetch_add(1) + 1;
		fmtutil::format_to(buffer, "{:06d}#{:010d}#{:06d}", m_frontID, (uint32_t)m_sessionID, orderref);
		return true;
	}
	catch (...)
	{

	}

	return false;
}

void TraderCTPOpt::registerSpi(ITraderSpi *listener)
{
	m_bscSink = listener;
	if (m_bscSink)
	{
		m_bdMgr = listener->getBaseDataMgr();

		m_optSink = listener->getOptSpi();
	}
}

uint32_t TraderCTPOpt::genRequestID()
{
	return m_iRequestID.fetch_add(1) + 1;
}

int TraderCTPOpt::login(const char* user, const char* pass, const char* productInfo)
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

int TraderCTPOpt::doLogin()
{
	CThostFtdcReqUserLoginField req;
	memset(&req, 0, sizeof(req));
	strcpy(req.BrokerID, m_strBroker.c_str());
	strcpy(req.UserID, m_strUser.c_str());
	strcpy(req.Password, m_strPass.c_str());
	strcpy(req.UserProductInfo, m_strProdInfo.c_str());
	int iResult = m_pUserAPI->ReqUserLogin(&req, genRequestID());
	if (iResult != 0)
	{
		write_log(m_bscSink, LL_ERROR, "[TraderCTPOpt] Sending login request failed: {}", iResult);
	}

	return 0;
}

int TraderCTPOpt::logout()
{
	if (m_pUserAPI == NULL)
	{
		return -1;
	}

	CThostFtdcUserLogoutField req;
	memset(&req, 0, sizeof(req));
	strcpy(req.BrokerID, m_strBroker.c_str());
	strcpy(req.UserID, m_strUser.c_str());
	int iResult = m_pUserAPI->ReqUserLogout(&req, genRequestID());
	if (iResult != 0)
	{
		write_log(m_bscSink, LL_ERROR, "[TraderCTPOpt] Sending logout request failed: {}", iResult);
	}

	return 0;
}

int TraderCTPOpt::orderInsertOpt(WTSEntrust* entrust)
{
	if (m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	if (entrust == NULL)
		return -1;

	if(entrust->getBusinessType() != BT_EXECUTE)
	{
		if(m_bscSink) write_log(m_bscSink, LL_ERROR, "[TraderCTPOpt] Bz type not supported except option execution");
		return -1;
	}

	CThostFtdcInputExecOrderField req;
	memset(&req, 0, sizeof(req));
	///经纪公司代码
	strcpy(req.BrokerID, m_strBroker.c_str());
	///投资者代码
	strcpy(req.InvestorID, m_strUser.c_str());
	///合约代码
	strcpy(req.InstrumentID, entrust->getCode());

	strcpy(req.ExchangeID, entrust->getExchg());

	req.ActionType = THOST_FTDC_ACTP_Exec;

	if (strlen(entrust->getUserTag()) == 0)
	{
		///报单引用
		fmt::format_to(req.ExecOrderRef, "{}", m_orderRef.fetch_add(0));

	}
	else
	{
		uint32_t fid, sid, orderref;
		extractEntrustID(entrust->getEntrustID(), fid, sid, orderref);
		///报单引用
		fmt::format_to(req.ExecOrderRef, "{}", orderref);
	}

	if (strlen(entrust->getUserTag()) > 0)
	{
		m_iniHelper.writeString(ENTRUST_SECTION, entrust->getEntrustID(), entrust->getUserTag());
		m_iniHelper.save();
	}

	req.PosiDirection = wrapPosDirType(entrust->getDirection());
	req.OffsetFlag = wrapOffsetType(entrust->getOffsetType());
	req.HedgeFlag = THOST_FTDC_HF_Speculation;
	req.Volume = (int)entrust->getVolume();
	req.CloseFlag = THOST_FTDC_EOCF_AutoClose;

	int iResult = m_pUserAPI->ReqExecOrderInsert(&req, genRequestID());
	if (iResult != 0)
	{
		write_log(m_bscSink, LL_ERROR, "[TraderCTPOpt] Option execution order inserting failed: {}", iResult);
	}

	return 0;
}

int TraderCTPOpt::orderActionOpt(WTSEntrustAction* action)
{
	if (action == NULL || m_wrapperState != WS_ALLREADY)
		return -1;

	if (action->getBusinessType() != BT_EXECUTE)
	{
		if (m_bscSink) write_log(m_bscSink, LL_ERROR, "[TraderCTPOpt] Bz type not supported except option execution");
		return -1;
	}

	uint32_t frontid, sessionid, orderref;
	if (!extractEntrustID(action->getEntrustID(), frontid, sessionid, orderref))
		return -1;

	CThostFtdcInputExecOrderActionField req;
	memset(&req, 0, sizeof(req));
	///经纪公司代码
	strcpy(req.BrokerID, m_strBroker.c_str());
	///投资者代码
	strcpy(req.InvestorID, m_strUser.c_str());
	///报单引用
	fmt::format_to(req.ExecOrderRef, "{}", orderref);
	///请求编号
	///前置编号
	req.FrontID = frontid;
	///会话编号
	req.SessionID = sessionid;
	///操作标志
	req.ActionFlag = wrapActionFlag(action->getActionFlag());
	///合约代码
	strcpy(req.InstrumentID, action->getCode());

	strcpy(req.ExecOrderSysID, action->getOrderID());
	strcpy(req.ExchangeID, action->getExchg());

	if (m_pUserAPI == NULL)
		return -1;

	int iResult = m_pUserAPI->ReqExecOrderAction(&req, genRequestID());
	if (iResult != 0)
	{
		write_log(m_bscSink, LL_ERROR, "[TraderCTPOpt] Sending cancel request of option execution order failed: {}", iResult);
	}

	return 0;
}

int TraderCTPOpt::queryOrdersOpt(WTSBusinessType bType)
{
	if (m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	if (bType != BT_EXECUTE)
	{
		if (m_bscSink) write_log(m_bscSink, LL_ERROR, "[TraderCTPOpt] Bz type not supported except option execution");
		return -1;
	}

	StdUniqueLock lock(m_mtxQuery);
	m_queQuery.push([this]() {
		CThostFtdcQryExecOrderField req;
		memset(&req, 0, sizeof(req));
		strcpy(req.BrokerID, m_strBroker.c_str());
		strcpy(req.InvestorID, m_strUser.c_str());

		m_pUserAPI->ReqQryExecOrder(&req, genRequestID());
	});

	//triggerQuery();

	return 0;
}

int TraderCTPOpt::orderInsert(WTSEntrust* entrust)
{
	if (m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	WTSContractInfo* ct = entrust->getContractInfo();
	if (ct == NULL)
		return -1;

	CThostFtdcInputOrderField req;
	memset(&req, 0, sizeof(req));
	///经纪公司代码
	strcpy(req.BrokerID, m_strBroker.c_str());
	///投资者代码
	strcpy(req.InvestorID, m_strUser.c_str());
	///合约代码
	strcpy(req.InstrumentID, entrust->getCode());

	strcpy(req.ExchangeID, entrust->getExchg());

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
		//m_mapEntrustTag[entrust->getEntrustID()] = entrust->getUserTag();
		m_iniHelper.writeString(ENTRUST_SECTION, entrust->getEntrustID(), entrust->getUserTag());
		m_iniHelper.save();
	}

	WTSCommodityInfo* commInfo = ct->getCommInfo();

	///用户代码
	//	TThostFtdcUserIDType	UserID;
	///报单价格条件: 限价
	req.OrderPriceType = wrapPriceType(entrust->getPriceType(), strcmp(commInfo->getExchg(), "CFFEX") == 0);
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

	if (entrust->getOrderFlag() == WOF_NOR)
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

	///触发条件: 立即
	req.ContingentCondition = THOST_FTDC_CC_Immediately;
	///止损价
	//	TThostFtdcPriceType	StopPrice;
	///强平原因: 非强平
	req.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;
	///自动挂起标志: 否
	req.IsAutoSuspend = 0;
	///业务单元
	//	TThostFtdcBusinessUnitType	BusinessUnit;
	///请求编号
	//	TThostFtdcRequestIDType	RequestID;
	///用户强评标志: 否
	req.UserForceClose = 0;

	int iResult = m_pUserAPI->ReqOrderInsert(&req, genRequestID());
	if (iResult != 0)
	{
		write_log(m_bscSink, LL_ERROR, "[TraderCTPOpt] Order inserting failed: {}", iResult);
	}

	return 0;
}

int TraderCTPOpt::orderAction(WTSEntrustAction* action)
{
	if (action == NULL || m_wrapperState != WS_ALLREADY)
		return -1;

	uint32_t frontid, sessionid, orderref;
	if (!extractEntrustID(action->getEntrustID(), frontid, sessionid, orderref))
		return -1;

	CThostFtdcInputOrderActionField req;
	memset(&req, 0, sizeof(req));
	///经纪公司代码
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
	strcpy(req.InstrumentID, action->getCode());

	strcpy(req.OrderSysID, action->getOrderID());
	strcpy(req.ExchangeID, action->getExchg());

	if (m_pUserAPI == NULL)
		return -1;

	int iResult = m_pUserAPI->ReqOrderAction(&req, genRequestID());
	if (iResult != 0)
	{
		write_log(m_bscSink, LL_ERROR, "[TraderCTPOpt] Sending cancel request failed: {}", iResult);
	}

	return 0;
}

int TraderCTPOpt::queryAccount()
{
	if (m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	StdUniqueLock lock(m_mtxQuery);
	m_queQuery.push([this](){
		CThostFtdcQryTradingAccountField req;
		memset(&req, 0, sizeof(req));
		strcpy(req.BrokerID, m_strBroker.c_str());
		strcpy(req.InvestorID, m_strUser.c_str());
		m_pUserAPI->ReqQryTradingAccount(&req, genRequestID());
	});

	//triggerQuery();

	return 0;
}

int TraderCTPOpt::queryPositions()
{
	if (m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	StdUniqueLock lock(m_mtxQuery);
	m_queQuery.push([this](){
		CThostFtdcQryInvestorPositionField req;
		memset(&req, 0, sizeof(req));
		strcpy(req.BrokerID, m_strBroker.c_str());
		strcpy(req.InvestorID, m_strUser.c_str());
		// BUG-R65-002 / 卡14 14a：调用 SDK 前登记在途请求号，同步回调也只按该号匹配。
		// 同步返回 0 仅表示"已发送"，持仓快照只在该请求号末帧（"已确认"）时发布。
		const int requestId = static_cast<int>(genRequestID());
		m_iPosQryReqID.store(requestId);
		const int iResult = m_pUserAPI->ReqQryInvestorPosition(&req, requestId);
		if (iResult != 0)
		{
			// 头文件未声明非零返回后是否仍有迟到回调：撤销在途登记，迟到回报一律忽略。
			int expected = requestId;
			if (m_iPosQryReqID.compare_exchange_strong(expected, 0))
				m_bInQuery = false;
			write_log(m_bscSink, LL_ERROR, "[TraderCTPOpt] Position query request {} send failed: {}; no position snapshot is published for it", requestId, iResult);
		}
		else
		{
			write_log(m_bscSink, LL_DEBUG, "[TraderCTPOpt] Position query request {} sent, awaiting confirmation", requestId);
		}
	});

	//triggerQuery();

	return 0;
}

int TraderCTPOpt::queryOrders()
{
	if (m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	StdUniqueLock lock(m_mtxQuery);
	m_queQuery.push([this](){
		CThostFtdcQryOrderField req;
		memset(&req, 0, sizeof(req));
		strcpy(req.BrokerID, m_strBroker.c_str());
		strcpy(req.InvestorID, m_strUser.c_str());

		m_pUserAPI->ReqQryOrder(&req, genRequestID());
	});

	//triggerQuery();

	return 0;
}

int TraderCTPOpt::queryTrades()
{
	if (m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	StdUniqueLock lock(m_mtxQuery);
	m_queQuery.push([this](){
		CThostFtdcQryTradeField req;
		memset(&req, 0, sizeof(req));
		strcpy(req.BrokerID, m_strBroker.c_str());
		strcpy(req.InvestorID, m_strUser.c_str());

		m_pUserAPI->ReqQryTrade(&req, genRequestID());
	});

	//triggerQuery();

	return 0;
}

int TraderCTPOpt::querySettlement(uint32_t uDate)
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
		strcpy(req.BrokerID, m_strBroker.c_str());
		strcpy(req.InvestorID, m_strUser.c_str());
		fmt::format_to(req.TradingDay, "{}", uDate);

		m_pUserAPI->ReqQrySettlementInfo(&req, genRequestID());
	});

	//triggerQuery();

	return 0;
}

void TraderCTPOpt::OnFrontConnected()
{
	if (m_bscSink)
		m_bscSink->handleEvent(WTE_Connect, 0);
}

void TraderCTPOpt::OnFrontDisconnected(int nReason)
{
	m_wrapperState = WS_NOTLOGIN;
	if (m_bscSink)
		m_bscSink->handleEvent(WTE_Close, nReason);
}

void TraderCTPOpt::OnHeartBeatWarning(int /*nTimeLapse*/)
{
	write_log(m_bscSink, LL_DEBUG, "[TraderCTPOpt][{}-{}] Heartbeating...", m_strBroker.c_str(), m_strUser.c_str());
}

void TraderCTPOpt::OnRspAuthenticate(CThostFtdcRspAuthenticateField */*pRspAuthenticateField*/, CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool /*bIsLast*/)
{
	if (!IsErrorRspInfo(pRspInfo))
	{
		doLogin();
	}
	else
	{
		write_log(m_bscSink, LL_ERROR, "[TraderCTPOpt][{}-{}] Authentiation failed: {}", m_strBroker.c_str(), m_strUser.c_str(), pRspInfo->ErrorMsg);
		m_wrapperState = WS_LOGINFAILED;

		if (m_bscSink)
			m_bscSink->onLoginResult(false, pRspInfo->ErrorMsg, 0);
	}

}

void TraderCTPOpt::OnRspUserLogin(CThostFtdcRspUserLoginField *pRspUserLogin, CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool /*bIsLast*/)
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

		write_log(m_bscSink, LL_INFO, "[TraderCTPOpt][{}-{}] Login succeed, AppID: {}, Sessionid: {}, login time: {}...",
			m_strBroker.c_str(), m_strUser.c_str(), m_strAppID.c_str(), m_sessionID, pRspUserLogin->LoginTime);

		std::stringstream ss;
		ss << m_strFlowDir << "local/" << m_strBroker << "/";
		std::string path = StrUtil::standardisePath(ss.str());
		if (!StdFile::exists(path.c_str()))
			fs::create_directories(path.c_str());
		ss << m_strUser << ".dat";

		m_iniHelper.load(ss.str().c_str());
		uint32_t lastDate = m_iniHelper.readUInt("marker", "date", 0);
		if(lastDate != m_lDate)
		{
			//交易日不同,清理掉原来的数据
			m_iniHelper.removeSection(ENTRUST_SECTION);
			m_iniHelper.removeSection(ORDER_SECTION);
			m_iniHelper.writeUInt("marker", "date", m_lDate);
			m_iniHelper.save();

			write_log(m_bscSink, LL_INFO, "[TraderCTPOpt][{}-{}] Trading date changed [{} -> {}], local cache cleared...", m_strBroker.c_str(), m_strUser.c_str(), lastDate, m_lDate);
		}

		write_log(m_bscSink, LL_INFO, "[TraderCTPOpt][{}-{}] Login succeed, trading date: {}...", m_strBroker.c_str(), m_strUser.c_str(), m_lDate);

		write_log(m_bscSink, LL_INFO, "[TraderCTPOpt][{}-{}] Querying confirming state of settlement data...", m_strBroker.c_str(), m_strUser.c_str());
		queryConfirm();
	}
	else
	{
		write_log(m_bscSink, LL_ERROR, "[TraderCTPOpt][{}-{}] Login failed: {}", m_strBroker.c_str(), m_strUser.c_str(), pRspInfo->ErrorMsg);
		m_wrapperState = WS_LOGINFAILED;

		if (m_bscSink)
			m_bscSink->onLoginResult(false, pRspInfo->ErrorMsg, 0);
	}
}

void TraderCTPOpt::OnRspUserLogout(CThostFtdcUserLogoutField */*pUserLogout*/, CThostFtdcRspInfoField */*pRspInfo*/, int /*nRequestID*/, bool /*bIsLast*/)
{
	m_wrapperState = WS_NOTLOGIN;
	if (m_bscSink)
		m_bscSink->handleEvent(WTE_Logout, 0);
}

void TraderCTPOpt::OnRspQrySettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField *pSettlementInfoConfirm, CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool bIsLast)
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
			uint32_t uConfirmDate = convert::to_uint32(pSettlementInfoConfirm->ConfirmDate);
			if (uConfirmDate >= m_lDate)
			{
				m_wrapperState = WS_CONFIRMED;

				write_log(m_bscSink, LL_INFO, "[TraderCTPOpt][{}-{}] Trading channel initialized...", m_strBroker.c_str(), m_strUser.c_str());
				m_wrapperState = WS_ALLREADY;
				if (m_bscSink)
					m_bscSink->onLoginResult(true, "", m_lDate);
			}
			else
			{
				m_wrapperState = WS_CONFIRM_QRYED;

				write_log(m_bscSink, LL_INFO, "[TraderCTPOpt][{}-{}] Confirming settlement data...", m_strBroker.c_str(), m_strUser.c_str());
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

void TraderCTPOpt::OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField *pSettlementInfoConfirm, CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool /*bIsLast*/)
{
	if (!IsErrorRspInfo(pRspInfo) && pSettlementInfoConfirm != NULL)
	{
		if (m_wrapperState == WS_CONFIRM_QRYED)
		{
			m_wrapperState = WS_CONFIRMED;

			write_log(m_bscSink, LL_INFO, "[TraderCTPOpt][{}-{}] Trading channel initialized...", m_strBroker.c_str(), m_strUser.c_str());
			m_wrapperState = WS_ALLREADY;
			if (m_bscSink)
				m_bscSink->onLoginResult(true, "", m_lDate);
		}
	}
}

void TraderCTPOpt::OnRspOrderInsert(CThostFtdcInputOrderField *pInputOrder, CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool /*bIsLast*/)
{
	WTSEntrust* entrust = makeEntrust(pInputOrder);
	if (entrust)
	{
		WTSError *err = makeError(pRspInfo);
		//g_orderMgr.onRspEntrust(entrust, err);
		if (m_bscSink)
			m_bscSink->onRspEntrust(entrust, err);
		entrust->release();
		err->release();
	}
}

void TraderCTPOpt::OnRspExecOrderInsert(CThostFtdcInputExecOrderField *pInputExecOrder, CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool /*bIsLast*/)
{
	WTSEntrust* entrust = makeEntrust(pInputExecOrder);
	if (entrust)
	{
		WTSError *err = makeError(pRspInfo);
		//g_orderMgr.onRspEntrust(entrust, err);
		if (m_optSink)
			m_optSink->onRspEntrustOpt(entrust, err);
		entrust->release();
		err->release();
	}
}

void TraderCTPOpt::OnRspExecOrderAction(CThostFtdcInputExecOrderActionField */*pInputExecOrderAction*/, CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool /*bIsLast*/)
{
	if (IsErrorRspInfo(pRspInfo))
	{
		WTSError* error = WTSError::create(WEC_EXECCANCEL, pRspInfo->ErrorMsg);
		if (m_bscSink)
			m_bscSink->onTraderError(error);
		error->release();
	}
}

void TraderCTPOpt::OnRtnExecOrder(CThostFtdcExecOrderField *pExecOrder)
{
	WTSOrderInfo *orderInfo = makeOrderInfo(pExecOrder);
	if (orderInfo)
	{
		if (m_optSink)
			m_optSink->onPushOrderOpt(orderInfo);

		orderInfo->release();
	}
}

void TraderCTPOpt::OnErrRtnExecOrderInsert(CThostFtdcInputExecOrderField *pInputExecOrder, CThostFtdcRspInfoField *pRspInfo)
{
	WTSEntrust* entrust = makeEntrust(pInputExecOrder);
	if (entrust)
	{
		WTSError *err = makeError(pRspInfo);
		//g_orderMgr.onRspEntrust(entrust, err);
		if (m_optSink)
			m_optSink->onRspEntrustOpt(entrust, err);
		entrust->release();
		err->release();
	}
}

void TraderCTPOpt::OnRspQryExecOrder(CThostFtdcExecOrderField *pExecOrder, CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool bIsLast)
{
	if (bIsLast)
	{
		m_bInQuery = false;
		//triggerQuery();
	}

	if (!IsErrorRspInfo(pRspInfo) && pExecOrder)
	{
		if (NULL == m_ayExecOrds)
			m_ayExecOrds = WTSArray::create();

		WTSOrderInfo* orderInfo = makeOrderInfo(pExecOrder);
		if (orderInfo)
		{
			m_ayExecOrds->append(orderInfo, false);
		}
	}

	if (bIsLast)
	{
		if (m_optSink)
			m_optSink->onRspOrdersOpt(m_ayExecOrds);

		if (m_ayExecOrds)
			m_ayExecOrds->clear();
	}
}

void TraderCTPOpt::OnRspOrderAction(CThostFtdcInputOrderActionField */*pInputOrderAction*/, CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool /*bIsLast*/)
{
	if (IsErrorRspInfo(pRspInfo))
	{
		WTSError* error = WTSError::create(WEC_ORDERCANCEL, pRspInfo->ErrorMsg);
		if (m_bscSink)
			m_bscSink->onTraderError(error);
		error->release();
	}
}

void TraderCTPOpt::OnRspQryTradingAccount(CThostFtdcTradingAccountField *pTradingAccount, CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool bIsLast)
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
		accountInfo->setCloseProfit(pTradingAccount->CloseProfit + pTradingAccount->CashIn);
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
		if (m_bscSink)
			m_bscSink->onRspAccount(ay);

		ay->release();
	}
}

void TraderCTPOpt::OnRspQrySettlementInfo(CThostFtdcSettlementInfoField *pSettlementInfo, CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool bIsLast)
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
		m_bscSink->onRspSettlementInfo(atoi(pSettlementInfo->TradingDay), m_strSettleInfo.c_str());
	}
}

void TraderCTPOpt::discardPositionBatch()
{
	if (m_mapPosition)
	{
		m_mapPosition->release();
		m_mapPosition = NULL;
	}
	m_bPositionQueryIncomplete = false;
	m_iPosAggReqID = 0;
}

void TraderCTPOpt::OnRspQryInvestorPosition(CThostFtdcInvestorPositionField *pInvestorPosition, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	// BUG-R65-002：只接受当前在途持仓请求的回报；失败、已完成或未知请求号的迟到回报不得污染新批次。
	if (nRequestID == 0 || nRequestID != m_iPosQryReqID.load())
	{
		write_log(m_bscSink, LL_WARN, "[TraderCTPOpt] Ignored position response for request {} that is not in flight", nRequestID);
		return;
	}
	if (m_iPosAggReqID != nRequestID)
	{
		// 新请求的第一帧：丢弃之前未收尾请求遗留的聚合状态。
		discardPositionBatch();
		m_iPosAggReqID = nRequestID;
	}
	if (bIsLast)
	{
		// 末帧即该请求的终态；之后同号重复帧会被上面的在途检查忽略。
		int expected = nRequestID;
		m_iPosQryReqID.compare_exchange_strong(expected, 0);
		m_bInQuery = false;
		//triggerQuery();
	}

	const bool hasError = IsErrorRspInfo(pRspInfo);
	if (hasError)
	{
		m_bPositionQueryIncomplete = true;
		write_log(m_bscSink, LL_ERROR, "[TraderCTPOpt] Position query response failed: {} : {}", pRspInfo->ErrorID, pRspInfo->ErrorMsg);
	}

	if (!hasError && pInvestorPosition)
	{
		if (NULL == m_mapPosition)
			m_mapPosition = PositionMap::create();

		WTSContractInfo* contract = m_bdMgr->getContract(pInvestorPosition->InstrumentID, pInvestorPosition->ExchangeID);
		if (contract == NULL)
		{
			m_bPositionQueryIncomplete = true;
			write_log(m_bscSink, LL_ERROR, "[TraderCTPOpt] Unknown contract in position query: {}@{}", pInvestorPosition->InstrumentID, pInvestorPosition->ExchangeID);
			if (bIsLast)
				discardPositionBatch();
			return;
		}

		WTSCommodityInfo* commInfo = contract->getCommInfo();
		if (contract)
		{
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
		if (m_bPositionQueryIncomplete)
		{
			discardPositionBatch();
			return;
		}

		WTSArray* ayPos = WTSArray::create();

		if(m_mapPosition && m_mapPosition->size() > 0)
		{
			for (auto it = m_mapPosition->begin(); it != m_mapPosition->end(); it++)
			{
				ayPos->append(it->second, true);
			}
		}

		if (m_bscSink)
			m_bscSink->onRspPosition(ayPos);

		discardPositionBatch();

		ayPos->release();
	}
}

void TraderCTPOpt::OnRspQryTrade(CThostFtdcTradeField *pTrade, CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool bIsLast)
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

		WTSTradeInfo* trade = makeTradeRecord(pTrade);
		if (trade)
		{
			m_ayTrades->append(trade, false);
		}
	}

	if (bIsLast)
	{
		if (m_bscSink)
			m_bscSink->onRspTrades(m_ayTrades);

		if (NULL != m_ayTrades)
			m_ayTrades->clear();
	}
}

void TraderCTPOpt::OnRspQryOrder(CThostFtdcOrderField *pOrder, CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool bIsLast)
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
		if (m_bscSink)
			m_bscSink->onRspOrders(m_ayOrders);

		if (m_ayOrders)
			m_ayOrders->clear();
	}
}

void TraderCTPOpt::OnRspError(CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool /*bIsLast*/)
{
	// T1-CTPOpt-07 / 卡14 14c：SDK 头文件未声明 OnRspError 空 pRspInfo 的含义，
	// 无契约前按失败处理（fail-closed），不再静默忽略；ErrorID==0 维持成功静默。
	const bool emptyInfo = (pRspInfo == NULL);
	if (!emptyInfo && pRspInfo->ErrorID == 0)
		return;

	// BUG-R65-002：错误应答命中在途持仓请求即为该请求的失败终态，丢弃部分聚合。
	int expected = nRequestID;
	if (nRequestID != 0 && m_iPosQryReqID.compare_exchange_strong(expected, 0))
	{
		discardPositionBatch();
		m_bInQuery = false;
		write_log(m_bscSink, LL_ERROR, "[TraderCTPOpt] Position query request {} failed by error response; no position snapshot is published for it", nRequestID);
	}

	if (m_bscSink == NULL)
		return;

	if (emptyInfo)
		m_bscSink->handleTraderLog(LL_ERROR, fmtutil::format("{} rsp error: empty response info without SDK contract, treated as failure", nRequestID));
	else
		m_bscSink->handleTraderLog(LL_ERROR, fmtutil::format("{} rsp error: {} : {}", nRequestID, pRspInfo->ErrorID, pRspInfo->ErrorMsg));
}

void TraderCTPOpt::OnRtnOrder(CThostFtdcOrderField *pOrder)
{
	WTSOrderInfo *orderInfo = makeOrderInfo(pOrder);
	if (orderInfo)
	{
		if (m_bscSink)
			m_bscSink->onPushOrder(orderInfo);

		orderInfo->release();
	}

	//ReqQryTradingAccount();
}

void TraderCTPOpt::OnRtnTrade(CThostFtdcTradeField *pTrade)
{
	WTSTradeInfo *tRecord = makeTradeRecord(pTrade);
	if (tRecord)
	{
		if (m_bscSink)
			m_bscSink->onPushTrade(tRecord);

		tRecord->release();
	}
}


WTSOrderInfo* TraderCTPOpt::makeOrderInfo(CThostFtdcOrderField* orderField)
{
	WTSContractInfo* contract = m_bdMgr->getContract(orderField->InstrumentID, orderField->ExchangeID);
	if (contract == NULL)
		return NULL;

	WTSOrderInfo* pRet = WTSOrderInfo::create();
	pRet->setPrice(orderField->LimitPrice);
	pRet->setVolume(orderField->VolumeTotalOriginal);
	pRet->setDirection(wrapDirectionType(orderField->Direction, orderField->CombOffsetFlag[0]));
	pRet->setPriceType(wrapPriceType(orderField->OrderPriceType));
	pRet->setOffsetType(wrapOffsetType(orderField->CombOffsetFlag[0]));
	pRet->setContractInfo(contract);

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
	pRet->setVolLeft(orderField->VolumeTotalOriginal - orderField->VolumeTraded);//要保证撤单的时候，剩余数量=挂单总数-已成交

	pRet->setCode(orderField->InstrumentID);
	pRet->setExchange(contract->getExchg());

	pRet->setOrderDate(convert::to_uint32(orderField->InsertDate));
	std::string strTime = orderField->InsertTime;
	StrUtil::replace(strTime, ":", "");
	[[maybe_unused]] uint32_t uTime = convert::to_uint32(strTime.c_str());
	pRet->setOrderTime(TimeUtils::makeTime(pRet->getOrderDate(), convert::to_uint32(strTime.c_str()) * 1000));

	pRet->setOrderState(wrapOrderState(orderField->OrderStatus));
	if (orderField->OrderSubmitStatus >= THOST_FTDC_OSS_InsertRejected)
		pRet->setError(WOEF_Normal);

	//pRet->setEntrustID(generateEntrustID(orderField->FrontID, orderField->SessionID, atoi(orderField->OrderRef)).c_str());
	generateEntrustID(pRet->getEntrustID(), orderField->FrontID, orderField->SessionID, atoi(orderField->OrderRef));
	pRet->setOrderID(orderField->OrderSysID);

	pRet->setStateMsg(orderField->StatusMsg);


	std::string usertag = m_iniHelper.readString(ENTRUST_SECTION, pRet->getEntrustID(), "");
	if(usertag.empty())
	{
		pRet->setUserTag(pRet->getEntrustID());
	}
	else
	{
		pRet->setUserTag(usertag.c_str());

		if (strlen(pRet->getOrderID()) > 0)
		{
			m_iniHelper.writeString(ORDER_SECTION, StrUtil::trim(pRet->getOrderID()).c_str(), usertag.c_str());
			m_iniHelper.save();
		}
	}

	return pRet;
}

WTSEntrust* TraderCTPOpt::makeEntrust(CThostFtdcInputOrderField *entrustField)
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

	//StringMap::iterator it = m_mapEntrustTag.find(pRet->getEntrustID());
	//if (it != m_mapEntrustTag.end())
	//{
	//	pRet->setUserTag(it->second.c_str());
	//}
	std::string usertag = m_iniHelper.readString(ENTRUST_SECTION, pRet->getEntrustID());
	if (!usertag.empty())
		pRet->setUserTag(usertag.c_str());

	return pRet;
}

WTSOrderInfo* TraderCTPOpt::makeOrderInfo(CThostFtdcExecOrderField* orderField)
{
	WTSContractInfo* contract = m_bdMgr->getContract(orderField->InstrumentID, orderField->ExchangeID);
	if (contract == NULL)
		return NULL;

	WTSOrderInfo* pRet = WTSOrderInfo::create();
	pRet->setContractInfo(contract);
	pRet->setPrice(0);
	pRet->setBusinessType(BT_EXECUTE);
	pRet->setVolume(orderField->Volume);
	pRet->setDirection(wrapPosDirType(orderField->PosiDirection));
	pRet->setOffsetType(wrapOffsetType(orderField->OffsetFlag));

	pRet->setCode(orderField->InstrumentID);
	pRet->setExchange(contract->getExchg());

	pRet->setOrderDate(convert::to_uint32(orderField->InsertDate));
	std::string strTime = orderField->InsertTime;
	StrUtil::replace(strTime, ":", "");
	[[maybe_unused]] uint32_t uTime = convert::to_uint32(strTime.c_str());
	pRet->setOrderTime(TimeUtils::makeTime(pRet->getOrderDate(), convert::to_uint32(strTime.c_str()) * 1000));

	pRet->setOrderState(WOS_Nottouched);
	if (orderField->OrderSubmitStatus >= THOST_FTDC_OSS_InsertRejected)
	{
		pRet->setError(WOEF_Normal);
		pRet->setOrderState(WOS_Canceled);
	}

	//pRet->setEntrustID(generateEntrustID(orderField->FrontID, orderField->SessionID, atoi(orderField->ExecOrderRef)).c_str());
	generateEntrustID(pRet->getEntrustID(), orderField->FrontID, orderField->SessionID, atoi(orderField->ExecOrderRef));
	pRet->setOrderID(orderField->ExecOrderSysID);

	pRet->setStateMsg(orderField->StatusMsg);


	std::string usertag = m_iniHelper.readString(ENTRUST_SECTION, pRet->getEntrustID(), "");
	if (usertag.empty())
	{
		pRet->setUserTag(pRet->getEntrustID());
	}
	else
	{
		pRet->setUserTag(usertag.c_str());

		if (strlen(pRet->getOrderID()) > 0)
		{
			m_iniHelper.writeString(ORDER_SECTION, StrUtil::trim(pRet->getOrderID()).c_str(), usertag.c_str());
			m_iniHelper.save();
		}
	}

	return pRet;
}

WTSEntrust* TraderCTPOpt::makeEntrust(CThostFtdcInputExecOrderField *entrustField)
{
	WTSContractInfo* ct = m_bdMgr->getContract(entrustField->InstrumentID, entrustField->ExchangeID);
	if (ct == NULL)
		return NULL;

	WTSEntrust* pRet = WTSEntrust::create(entrustField->InstrumentID, entrustField->Volume, 0, ct->getExchg(), BT_EXECUTE);
	pRet->setContractInfo(ct);

	pRet->setDirection(wrapPosDirType(entrustField->PosiDirection));
	pRet->setOffsetType(wrapOffsetType(entrustField->OffsetFlag));

	//pRet->setEntrustID(generateEntrustID(m_frontID, m_sessionID, atoi(entrustField->ExecOrderRef)).c_str());
	generateEntrustID(pRet->getEntrustID(), m_frontID, m_sessionID, atoi(entrustField->ExecOrderRef));

	std::string usertag = m_iniHelper.readString(ENTRUST_SECTION, pRet->getEntrustID());
	if (!usertag.empty())
		pRet->setUserTag(usertag.c_str());

	return pRet;
}


WTSError* TraderCTPOpt::makeError(CThostFtdcRspInfoField* rspInfo)
{
	WTSError* pRet = WTSError::create((WTSErroCode)rspInfo->ErrorID, rspInfo->ErrorMsg);
	return pRet;
}

WTSTradeInfo* TraderCTPOpt::makeTradeRecord(CThostFtdcTradeField *tradeField)
{
	WTSContractInfo* contract = m_bdMgr->getContract(tradeField->InstrumentID, tradeField->ExchangeID);
	if (contract == NULL)
		return NULL;

	WTSCommodityInfo* commInfo = contract->getCommInfo();

	WTSTradeInfo *pRet = WTSTradeInfo::create(tradeField->InstrumentID, commInfo->getExchg());
	pRet->setVolume(tradeField->Volume);
	pRet->setPrice(tradeField->Price);
	pRet->setTradeID(tradeField->TradeID);
	pRet->setContractInfo(contract);

	std::string strTime = tradeField->TradeTime;
	StrUtil::replace(strTime, ":", "");
	uint32_t uTime = convert::to_uint32(strTime.c_str());
	uint32_t uDate = convert::to_uint32(tradeField->TradeDate);

	//if(uDate == m_pContractMgr->getTradingDate())
	//{
	//	//如果当前日期和交易日一致,且时间大于21点,说明是夜盘,也就是实际日期要单独计算
	//	if (uTime / 10000 >= 21)
	//	{
	//		uDate = m_pMarketMgr->getPrevTDate(commInfo->getExchg(), uDate, 1);
	//	}
	//	else if(uTime <= 3)
	//	{
	//		//如果在3点以内,就要先获取上一个交易日,再获取下一个自然日
	//		//这样做的目的是,遇到周五晚上的情况,可以处理过来
	//		uDate = m_pMarketMgr->getPrevTDate(commInfo->getExchg(), uDate, 1);
	//		uDate = TimeUtils::getNextDate(uDate);
	//	}
	//}

	pRet->setTradeDate(uDate);
	pRet->setTradeTime(TimeUtils::makeTime(uDate, uTime * 1000));

	WTSDirectionType dType = wrapDirectionType(tradeField->Direction, tradeField->OffsetFlag);

	pRet->setDirection(dType);
	pRet->setOffsetType(wrapOffsetType(tradeField->OffsetFlag));
	pRet->setRefOrder(tradeField->OrderSysID);
	pRet->setTradeType((WTSTradeType)tradeField->TradeType);

	double amount = commInfo->getVolScale()*tradeField->Volume*pRet->getPrice();
	pRet->setAmount(amount);

	//StringMap::iterator it = m_mapOrderTag.find(pRet->getRefOrder());
	//if (it != m_mapOrderTag.end())
	//{
	//	pRet->setUserTag(it->second.c_str());
	//}
	std::string usertag = m_iniHelper.readString(ORDER_SECTION, StrUtil::trim(pRet->getRefOrder()).c_str());
	if (!usertag.empty())
		pRet->setUserTag(usertag.c_str());

	return pRet;
}

void TraderCTPOpt::generateEntrustID(char* buffer, uint32_t frontid, uint32_t sessionid, uint32_t orderRef)
{
	fmtutil::format_to(buffer, "{:06d}#{:010d}#{:06d}", frontid, sessionid, orderRef);
}

bool TraderCTPOpt::extractEntrustID(const char* entrustid, uint32_t &frontid, uint32_t &sessionid, uint32_t &orderRef)
{
	//Market.FrontID.SessionID.OrderRef
	thread_local static char buffer[64];
	wt_strcpy(buffer, entrustid);
	char* s = buffer;
	auto idx = StrUtil::findFirst(s, '#');
	if (idx == std::string::npos)
		return false;
	s[idx] = '\0';
	frontid = convert::to_uint32(s);
	s += idx + 1;

	idx = StrUtil::findFirst(s, '#');
	if (idx == std::string::npos)
		return false;
	s[idx] = '\0';
	sessionid = convert::to_uint32(s);
	s += idx + 1;

	orderRef = convert::to_uint32(s);

	return true;
}

bool TraderCTPOpt::IsErrorRspInfo(CThostFtdcRspInfoField *pRspInfo)
{
	if (pRspInfo && pRspInfo->ErrorID != 0)
		return true;

	return false;
}

void TraderCTPOpt::OnErrRtnOrderInsert(CThostFtdcInputOrderField *pInputOrder, CThostFtdcRspInfoField *pRspInfo)
{
	WTSEntrust* entrust = makeEntrust(pInputOrder);
	if (entrust)
	{
		WTSError *err = makeError(pRspInfo);
		//g_orderMgr.onRspEntrust(entrust, err);
		if (m_bscSink)
			m_bscSink->onRspEntrust(entrust, err);
		entrust->release();
		err->release();
	}
}

void TraderCTPOpt::OnRtnInstrumentStatus(CThostFtdcInstrumentStatusField *pInstrumentStatus)
{
	if (m_bscSink)
	{
		write_log(m_bscSink, LL_INFO, "NewStatus, exchg: {}, code: {}, status: {}, reason: {}, entertime: {}",
			pInstrumentStatus->ExchangeID, pInstrumentStatus->InstrumentID, (int)pInstrumentStatus->InstrumentStatus, (int)pInstrumentStatus->EnterReason, pInstrumentStatus->EnterTime);
		m_bscSink->onPushInstrumentStatus(pInstrumentStatus->ExchangeID, pInstrumentStatus->InstrumentID, (WTSTradeStatus)pInstrumentStatus->InstrumentStatus);			
	}
}

bool TraderCTPOpt::isConnected()
{
	return (m_wrapperState == WS_ALLREADY);
}

int TraderCTPOpt::queryConfirm()
{
	if (m_pUserAPI == NULL || m_wrapperState != WS_LOGINED)
	{
		return -1;
	}

	StdUniqueLock lock(m_mtxQuery);
	m_queQuery.push([this](){
		CThostFtdcQrySettlementInfoConfirmField req;
		memset(&req, 0, sizeof(req));
		strcpy(req.BrokerID, m_strBroker.c_str());
		strcpy(req.InvestorID, m_strUser.c_str());

		int iResult = m_pUserAPI->ReqQrySettlementInfoConfirm(&req, genRequestID());
		if (iResult != 0)
		{
			write_log(m_bscSink, LL_ERROR, "[TraderCTPOpt][{}-{}] Sending query of settlement data confirming state failed: {}", m_strBroker.c_str(), m_strUser.c_str(), iResult);
		}
	});

	//triggerQuery();

	return 0;
}

int TraderCTPOpt::confirm()
{
	if (m_pUserAPI == NULL || m_wrapperState != WS_CONFIRM_QRYED)
	{
		return -1;
	}
	uint32_t date = 0, time = 0;
	if (!TimeUtils::tryGetDateTime(date, time))
	{
		write_log(m_bscSink, LL_ERROR, "[TraderCTPOpt][{}-{}] Cannot confirm settlement: local time unavailable", m_strBroker.c_str(), m_strUser.c_str());
		return -1;
	}

	//std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	CThostFtdcSettlementInfoConfirmField req;
	memset(&req, 0, sizeof(req));
	strcpy(req.BrokerID, m_strBroker.c_str());
	strcpy(req.InvestorID, m_strUser.c_str());

	std::snprintf(req.ConfirmDate, sizeof(req.ConfirmDate), "%08u", static_cast<unsigned>(date));
	const uint32_t seconds = time / 1000;
	std::snprintf(req.ConfirmTime, sizeof(req.ConfirmTime), "%02u:%02u:%02u",
		static_cast<unsigned>(seconds / 10000), static_cast<unsigned>((seconds / 100) % 100),
		static_cast<unsigned>(seconds % 100));

	int iResult = m_pUserAPI->ReqSettlementInfoConfirm(&req, genRequestID());
	if (iResult != 0)
	{
		write_log(m_bscSink, LL_ERROR, "[TraderCTPOpt][{}-{}] Sending confirming of settlement data failed: {}", m_strBroker.c_str(), m_strUser.c_str(), iResult);
		return -1;
	}

	return 0;
}

int TraderCTPOpt::authenticate()
{
	CThostFtdcReqAuthenticateField req;
	memset(&req, 0, sizeof(req));
	strcpy(req.BrokerID, m_strBroker.c_str());
	strcpy(req.UserID, m_strUser.c_str());
	//strcpy(req.UserProductInfo, m_strProdInfo.c_str());
	strcpy(req.AuthCode, m_strAuthCode.c_str());
	strcpy(req.AppID, m_strAppID.c_str());
	m_pUserAPI->ReqAuthenticate(&req, genRequestID());

	return 0;
}

/*
void TraderCTPOpt::triggerQuery()
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
