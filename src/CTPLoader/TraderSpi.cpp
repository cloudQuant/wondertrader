/*!
 * \file TraderSpi.cpp
 * \brief CTP交易接口回调类实现
 * 
 * 该文件实现了CTP交易接口的回调函数，用于获取合约信息
 * 并将合约信息转换为WonderTrader框架所需的JSON格式
 * 主要实现功能包括：
 * 1. 连接CTP交易系统并进行认证登录
 * 2. 查询并获取合约信息
 * 3. 将合约信息转换为WonderTrader框架格式
 * 4. 将转换后的信息保存为JSON文件
 * 
 * \author Wesley
 */

#include <iostream>
#include <set>
#include <stdint.h>
#include <fstream>
#include <exception>

#include "../Share/StrUtil.hpp"
#include "../Share/fmtlib.h"
#include "../Share/charconv.hpp"
#include "../Includes/LoaderDef.hpp"
#include "../Includes/WTSVariant.hpp"

#include "../WTSUtils/WTSCfgLoader.h"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
namespace rj = rapidjson;

#include "TraderSpi.h"

/**
 * \brief 编码文本为UTF-8格式
 * 
 * 在非Windows平台上将文本转换为UTF-8编码
 * 在Windows平台上直接返回原文本
 * 
 * \param s 输入文本
 * \return 转换后的文本
 */
inline const char* encode_text(const char* s)
{
#ifdef _MSC_VER
	return s;
#else
	static std::string ret;
	ret = ChartoUTF8(s);
	return ret.c_str();
#endif
}



USING_NS_WTP;

extern std::map<std::string, std::string>	MAP_NAME;
extern std::map<std::string, std::string>	MAP_SESSION;

#pragma warning(disable : 4996)

// USER_API参数
extern CThostFtdcTraderApi* pUserApi;

// 配置参数
extern std::string	FRONT_ADDR;	// 前置地址
extern std::string	BROKER_ID;	// 经纪公司代码
extern std::string	INVESTOR_ID;// 投资者代码
extern std::string	PASSWORD;	// 用户密码
extern std::string	SAVEPATH;	//保存位置
extern std::string	APPID;
extern std::string	AUTHCODE;
extern uint32_t		CLASSMASK;
extern bool			ONLYINCFG;

extern std::string COMM_FILE;		//输出的品种文件名
extern std::string CONT_FILE;		//输出的合约文件名

// 请求编号
extern int iRequestID;

// 会话参数
TThostFtdcFrontIDType	FRONT_ID;	//前置编号
TThostFtdcSessionIDType	SESSION_ID;	//会话编号
TThostFtdcOrderRefType	ORDER_REF;	//报单引用

CommodityMap _commodities;
ContractMap _contracts;


/**
 * \brief 从合约代码中提取品种代码
 * 
 * 从合约代码中提取字母部分作为品种代码
 * 例如：从"cu2301"中提取"cu"
 * 
 * \param instrument 合约代码
 * \return 品种代码
 */
std::string extractProductID(const char* instrument)
{
	std::string strRet;
	int nLen = 0;
	// 提取字母部分作为品种代码
	while ('A' <= instrument[nLen] && instrument[nLen] <= 'z')
	{
		strRet += instrument[nLen];
		nLen++;
	}

	return strRet;
}

/**
 * \brief 从合约名称中提取品种名称
 * 
 * 从合约名称中去除数字部分，保留品种名称
 * 例如：从"沃尔沃铜2301"中提取"沃尔沃铜"
 * 
 * \param cname 合约名称
 * \return 品种名称
 */
std::string extractProductName(const char* cname)
{
	std::string strRet;
	// 从名称的末尾开始向前查找非数字字符
	auto idx = strlen(cname) - 1;
	while (isdigit(cname[idx]) && idx > 0)
	{
		idx--;
	}

	// 复制非数字部分作为品种名称
	strRet.append(cname, idx + 1);
	return strRet;
}

std::set<std::string>	prod_set;

/**
 * \brief 检查浮点数值是否有效
 * 
 * 检查浮点数值是否为最大值，如果是则返回0
 * 用于处理CTP接口返回的无效浮点数值
 * 
 * \param val 输入浮点数值
 * \return 如果输入为最大值则返回0，否则返回原值
 */
inline double checkValid(double val)
{
	if (val == DBL_MAX || val == FLT_MAX)
		return 0;

	return val;
}


/**
 * \brief 前置连接成功回调函数
 * 
 * 当交易前置连接成功建立后调用此函数
 * 输出连接成功日志，并发起认证请求
 */
void CTraderSpi::OnFrontConnected()
{
	std::cerr << "--->>> " << "OnFrontConnected" << std::endl;
	// 发起认证请求
	ReqAuth();
}

/**
 * \brief 发送认证请求
 * 
 * 在连接建立后发送认证请求，用于验证交易用户身份
 * 填充经纪商ID、用户ID、应用ID和授权码等认证信息
 */
void CTraderSpi::ReqAuth()
{
	// 创建并初始化认证请求结构
	CThostFtdcReqAuthenticateField req;
	memset(&req, 0, sizeof(req));
	// 填充认证信息
	strcpy(req.BrokerID, BROKER_ID.c_str());
	strcpy(req.UserID, INVESTOR_ID.c_str());
	strcpy(req.AppID, APPID.c_str());
	strcpy(req.AuthCode, AUTHCODE.c_str());

	// 发送认证请求
	int iResult = pUserApi->ReqAuthenticate(&req, ++iRequestID);
	std::cerr << "--->>> Requesting authentication: " << ((iResult == 0) ? "succeed" : "failed") << std::endl;
}

/**
 * \brief 认证响应回调函数
 * 
 * 当收到认证请求的响应时调用此函数
 * 如果认证成功，则发起用户登录请求
 * 
 * \param pRspAuthenticateField 认证响应信息
 * \param pRspInfo 错误信息，如果为NULL表示成功
 * \param nRequestID 请求ID
 * \param bIsLast 是否为最后一条消息
 */
void CTraderSpi::OnRspAuthenticate(CThostFtdcRspAuthenticateField *pRspAuthenticateField, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	std::cerr << "--->>> " << "OnRspAuthenticate" << std::endl;
	if (bIsLast && !IsErrorRspInfo(pRspInfo))
	{
		ReqUserLogin();
	}
}

/**
 * \brief 发送用户登录请求
 * 
 * 在认证成功后发送登录请求，用于登录交易系统
 * 填充经纪商ID、用户ID和密码等登录信息
 */
void CTraderSpi::ReqUserLogin()
{
	// 创建并初始化登录请求结构
	CThostFtdcReqUserLoginField req;
	memset(&req, 0, sizeof(req));
	// 填充登录信息
	strcpy(req.BrokerID, BROKER_ID.c_str());
	strcpy(req.UserID, INVESTOR_ID.c_str());
	strcpy(req.Password, PASSWORD.c_str());
	// 发送登录请求
	int iResult = pUserApi->ReqUserLogin(&req, ++iRequestID);
	std::cerr << "--->>> Requesting user login: " << ((iResult == 0) ? "succeed" : "failed") << std::endl;
}

/**
 * \brief 登录响应回调函数
 * 
 * 当收到登录请求的响应时调用此函数
 * 如果登录成功，则保存会话参数并发起合约查询请求
 * 
 * \param pRspUserLogin 登录响应信息
 * \param pRspInfo 错误信息，如果为NULL表示成功
 * \param nRequestID 请求ID
 * \param bIsLast 是否为最后一条消息
 */
void CTraderSpi::OnRspUserLogin(CThostFtdcRspUserLoginField *pRspUserLogin,
	CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	std::cerr << "--->>> " << "OnRspUserLogin" << std::endl;
	if (bIsLast && !IsErrorRspInfo(pRspInfo))
	{
		// 保存会话参数
		FRONT_ID = pRspUserLogin->FrontID;
		SESSION_ID = pRspUserLogin->SessionID;
		int iNextOrderRef = atoi(pRspUserLogin->MaxOrderRef);
		iNextOrderRef++;
		fmtutil::format_to(ORDER_REF, "{}", iNextOrderRef);
		// 获取当前交易日
		m_lTradingDate = atoi(pUserApi->GetTradingDay());

		// 尝试从本地JSON文件加载合约信息
		LoadFromJson();

		// 发送合约查询请求
		ReqQryInstrument();
	}
}

/**
 * \brief 发送合约查询请求
 * 
 * 在登录成功后发送查询合约请求，获取所有可交易合约的信息
 * 该查询不设置过滤条件，将获取所有合约
 */
void CTraderSpi::ReqQryInstrument()
{
	// 创建并初始化合约查询结构
	CThostFtdcQryInstrumentField req;
	memset(&req, 0, sizeof(req));
	// 发送合约查询请求，不设置过滤条件将查询所有合约
	int iResult = pUserApi->ReqQryInstrument(&req, ++iRequestID);
	std::cerr << "--->>> Quering instruments: " << ((iResult == 0) ? "succeed" : "failed") << std::endl;
}

/**
 * \brief 检查产品类型是否为期权
 * 
 * 检查CTP产品类型是否为期权或现货期权
 * 
 * \param pClass CTP产品类型
 * \return 如果是期权则返回true，否则返回false
 */
inline bool isOption(TThostFtdcProductClassType pClass)
{
	if (pClass == THOST_FTDC_PC_Options || pClass == THOST_FTDC_PC_SpotOption)
		return true;

	return false;
}

/**
 * \brief 检查产品类型是否为期货
 * 
 * 检查CTP产品类型是否为期货
 * 
 * \param pClass CTP产品类型
 * \return 如果是期货则返回true，否则返回false
 */
inline bool isFuture(TThostFtdcProductClassType pClass)
{
	return pClass == THOST_FTDC_PC_Futures;
}

/**
 * \brief 将CTP产品类型转换为WonderTrader合约类型
 * 
 * 将CTP接口的产品类型转换为WonderTrader框架中的合约类型
 * 
 * \param cType CTP产品类型
 * \return WonderTrader合约类型
 * \throw std::runtime_error 如果产品类型不支持则抛出异常
 */
inline ContractCategory wrapCategory(TThostFtdcProductClassType cType)
{
	switch (cType)
	{
	case THOST_FTDC_PC_Futures: return CC_Future;      // 期货
	case THOST_FTDC_PC_Options: return CC_FutOption;    // 期权
	case THOST_FTDC_PC_Combination: return CC_Combination; // 组合
	case THOST_FTDC_PC_Spot: return CC_Spot;           // 现货
	case THOST_FTDC_PC_EFP: return CC_EFP;             // 期货期权组合
	case THOST_FTDC_PC_SpotOption: return CC_SpotOption;  // 现货期权
	default:
		throw std::runtime_error("non implemented category");
	}
}


/**
 * \brief 合约查询响应回调函数
 * 
 * 当收到合约查询的响应时调用此函数
 * 将CTP合约信息转换为WonderTrader框架所需的格式
 * 并处理合约的交易时段、手续费等信息
 * 当所有合约信息接收完成后，将合约信息保存为JSON格式
 * 
 * \param pInstrument 合约信息结构
 * \param pRspInfo 错误信息，如果为NULL表示成功
 * \param nRequestID 请求ID
 * \param bIsLast 是否为最后一条消息
 */
void CTraderSpi::OnRspQryInstrument(CThostFtdcInstrumentField *pInstrument, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if (!IsErrorRspInfo(pRspInfo))
	{
		if (pInstrument)
		{
			std::string fullInstId = StrUtil::printf("%s.%s", pInstrument->ExchangeID, pInstrument->InstrumentID);
			auto it = _contracts.find(fullInstId);
			if (it != _contracts.end())
			{
				std::cerr << "--->>> " << pInstrument->ExchangeID << "." << pInstrument->InstrumentID << " already exists, skipped" << std::endl;
				return;
			}

			bool bOption = isOption(pInstrument->ProductClass);
			bool bFuture = isFuture(pInstrument->ProductClass);

			bool isGranted = false;
			if (bOption && (CLASSMASK & 2) != 0)
				isGranted = true;
			else if (bFuture && (CLASSMASK & 1) != 0)
				isGranted = true;

			do 
			{
				if (isGranted)
				{
					std::string pname = MAP_NAME[pInstrument->ProductID];
					if (pname.empty())
					{
						std::stringstream ss;
						ss << pInstrument->ExchangeID << "." << pInstrument->ProductID;
						pname = MAP_NAME[ss.str()];
					}

					std::string cname = "";
					if (pname.empty())
					{
						if (ONLYINCFG)
						{
							std::cerr << "--->>> OnRspQryInstrument: " << pInstrument->ExchangeID << "." << pInstrument->InstrumentID << std::endl;
							break;
						}

						cname = pInstrument->InstrumentName;
						pname = bFuture ? extractProductName(pInstrument->InstrumentName) : pInstrument->InstrumentName;

						std::cerr << "--->>> Name confirmed automatically: " << pInstrument->ExchangeID << "." << pInstrument->InstrumentID << std::endl;
					}
					else
					{
						if (bFuture)
						{
							std::string month = pInstrument->InstrumentID;
							month = month.substr(strlen(pInstrument->ProductID));
							cname = pname + month;
						}
						else if (bOption)
						{
							std::string underlyPID = extractProductID(pInstrument->UnderlyingInstrID);
							std::string month = pInstrument->InstrumentID;
							month = month.substr(underlyPID.size());
							cname = pname + month;
						}
						else
						{
							cname = pInstrument->InstrumentName;
						}

					}

					//合约名称转成UTF8
					cname = StrUtil::trim(cname.c_str());
					if (!EncodingHelper::isUtf8((unsigned char*)cname.c_str(), cname.size()))
						cname = ChartoUTF8(cname);

					Contract contract;
					contract.m_strCode = pInstrument->InstrumentID;
					contract.m_strExchg = pInstrument->ExchangeID;
					contract.m_strName = cname;
					contract.m_strProduct = pInstrument->ProductID;

					contract.m_maxMktQty = pInstrument->MaxMarketOrderVolume;
					contract.m_maxLmtQty = pInstrument->MaxLimitOrderVolume;
					contract.m_minMktQty = pInstrument->MinMarketOrderVolume;
					contract.m_minLmtQty = pInstrument->MinLimitOrderVolume;

					contract.m_optType = bOption ? (OptionType)pInstrument->OptionsType : OT_None;
					contract.m_strUnderlying = pInstrument->UnderlyingInstrID;
					contract.m_strikePrice = pInstrument->StrikePrice;
					contract.m_dUnderlyingScale = pInstrument->UnderlyingMultiple;

					contract.m_uOpenDate = strtoul(pInstrument->OpenDate, NULL, 10);
					contract.m_uExpireDate = strtoul(pInstrument->ExpireDate, NULL, 10);

					contract.m_dLongMarginRatio = checkValid(pInstrument->LongMarginRatio);
					contract.m_dShortMarginRatio = checkValid(pInstrument->ShortMarginRatio);

					std::string fullPid = StrUtil::printf("%s.%s", pInstrument->ExchangeID, pInstrument->ProductID);
					auto it = _commodities.find(fullPid);
					if (it == _commodities.end())
					{
						//品种名称也转成UTF8
						pname = StrUtil::trim(pname.c_str());
						if (!EncodingHelper::isUtf8((unsigned char*)pname.c_str(), pname.size()))
							pname = ChartoUTF8(pname);

						Commodity commInfo;
						commInfo.m_strProduct = pInstrument->ProductID;
						commInfo.m_strName = pname;
						commInfo.m_strExchg = pInstrument->ExchangeID;
						commInfo.m_strCurrency = "CNY";

						commInfo.m_strSession = MAP_SESSION[fullPid];
						commInfo.m_ccCategory = wrapCategory(pInstrument->ProductClass);

						commInfo.m_uVolScale = (pInstrument->VolumeMultiple == 0 ? 1 : pInstrument->VolumeMultiple);
						commInfo.m_fPriceTick = pInstrument->PriceTick;

						CoverMode cm = CM_OpenCover;
						/*
						 *	By Wesley @ 2023.05.04
						 *	有用户反馈上期所和上能所的期权合约也区分平昨平今
						 *	把这个bFuture的判断去掉
						 */
						//if (bFuture)
						{
							if (strcmp(pInstrument->ExchangeID, "SHFE") == 0 || strcmp(pInstrument->ExchangeID, "INE") == 0)
								cm = CM_CoverToday;
							//上期所的就是平今,非上期所的就是开平
						}

						commInfo.m_coverMode = cm;

						PriceMode pm = PM_Both;
						/*
						 *	By Wesley @ 2023.05.04
						 *	有用户反馈上期所和上能所的期权合约也区分平昨平今
						 *	把这个bFuture的判断去掉
						 */
						//if (bFuture)
						{
							if (strcmp(pInstrument->ExchangeID, "SHFE") == 0 || strcmp(pInstrument->ExchangeID, "INE") == 0)
								pm = PM_Limit;
						}
						commInfo.m_priceMode = pm;
						commInfo.m_tradeMode = TM_Both;

						if (pInstrument->PriceTick < 0.001)
							commInfo.m_uPrecision = 4;
						else if (pInstrument->PriceTick < 0.01)
							commInfo.m_uPrecision = 3;
						else if (pInstrument->PriceTick < 0.1)
							commInfo.m_uPrecision = 2;
						else if (pInstrument->PriceTick < 1)
							commInfo.m_uPrecision = 1;
						else
							commInfo.m_uPrecision = 0;

						_commodities[fullPid] = commInfo;
					}

					_contracts[fullInstId] = contract;
				}
			} while (false);
			
		}
	}

	if (bIsLast)
	{
		DumpToJson();
		exit(0);
	}
}

/**
 * \brief 从本地JSON文件加载合约信息
 * 
 * 尝试从指定的JSON文件中加载合约和品种信息
 * 如果文件存在且格式正确，则读取其中的信息并填充到内存中
 * 这样可以避免每次都需要从交易所获取全部合约信息
 */
void CTraderSpi::LoadFromJson()
{
	std::string path = SAVEPATH;
	path += COMM_FILE;
	WTSVariant* root = WTSCfgLoader::load_from_file(path);
	if(root)
	{
		for(const auto& exchg : root->memberNames())
		{
			WTSVariant* jExchg = root->get(exchg.c_str());
			for(const auto& pid : jExchg->memberNames())
			{
				WTSVariant* pComm = jExchg->get(pid.c_str());
				std::string key = fmt::format("{}.{}", exchg, pid);

				Commodity& commInfo = _commodities[key];
				commInfo.m_strProduct = pid;
				commInfo.m_strName = pComm->getCString("name");
				commInfo.m_strExchg = exchg;
				commInfo.m_strCurrency = "CNY";

				commInfo.m_strSession = MAP_SESSION[key];
				commInfo.m_ccCategory = (ContractCategory)pComm->getUInt32("category");

				commInfo.m_uVolScale = pComm->getUInt32("volscale");;
				commInfo.m_fPriceTick = pComm->getDouble("pricetick");

				commInfo.m_coverMode = (CoverMode)pComm->getUInt32("covermode");

				commInfo.m_priceMode = (PriceMode)pComm->getUInt32("pricemode");;
				commInfo.m_tradeMode = (TradingMode)pComm->getUInt32("trademode");

				commInfo.m_uPrecision = pComm->getUInt32("precision");
			}
		}

		root->release();
	}

	path = SAVEPATH;
	path += CONT_FILE;
	root = WTSCfgLoader::load_from_file(path);
	if (root)
	{
		WTSVariant* root = WTSCfgLoader::load_from_file(path);
		for (const auto& exchg_id : root->memberNames())
		{
			WTSVariant* jExchg = root->get(exchg_id.c_str());
			for (const auto& inst_id : jExchg->memberNames())
			{
				WTSVariant* pCont = jExchg->get(inst_id.c_str());
				std::string key = fmt::format("{}.{}", exchg_id, inst_id);

				Contract& contract = _contracts[key];
				contract.m_strCode = inst_id;
				contract.m_strExchg = exchg_id;
				contract.m_strName = pCont->getCString("name");
				contract.m_strProduct = pCont->getCString("product");

				contract.m_maxMktQty = pCont->getUInt32("maxmarketqty");
				contract.m_maxLmtQty = pCont->getUInt32("maxlimitqty");
				contract.m_minMktQty = pCont->getUInt32("minmarketqty");
				contract.m_minLmtQty = pCont->getUInt32("minlimitqty");

				if(pCont->has("option"))
				{
					contract.m_optType = (OptionType)pCont->getUInt32("optiontype");
					contract.m_strUnderlying = pCont->getCString("underlying");
					contract.m_strikePrice = pCont->getDouble("product");
					contract.m_dUnderlyingScale = pCont->getDouble("underlyingscale");
				}
				

				contract.m_uOpenDate = pCont->getUInt32("opendate");
				contract.m_uExpireDate = pCont->getUInt32("expiredate");

				contract.m_dLongMarginRatio = pCont->getDouble("longmarginratio");
				contract.m_dShortMarginRatio = pCont->getDouble("shortmarginratio");
			}
		}

		root->release();
	}
	std::cerr << "--->>> " << "LoadFromJson" << std::endl;
}

/**
 * \brief 将合约信息转储为JSON格式
 * 
 * 将已获取的合约信息和品种信息转换为JSON格式
 * 并分别保存为两个文件：
 * 1. 品种文件(commodities.json)：包含品种的交易时段、手续费等信息
 * 2. 合约文件(contracts.json)：包含合约的到期日、合约乘数等信息
 */
void CTraderSpi::DumpToJson()
{
	//两个文件,一个contracts.json,一个commodities.json
	//Json::Value jComms(Json::objectValue);
	rj::Document jComms(rj::kObjectType);
	{
		rj::Document::AllocatorType &allocator = jComms.GetAllocator();
		for (auto it = _commodities.begin(); it != _commodities.end(); it++)
		{
			const Commodity& commInfo = it->second;
			if (!jComms.HasMember(commInfo.m_strExchg.c_str()))
			{
				//jComms[commInfo.m_strExchg] = Json::Value(Json::objectValue);
				jComms.AddMember(rj::Value(commInfo.m_strExchg.c_str(), allocator), rj::Value(rj::kObjectType), allocator);
			}

			rj::Value jComm(rj::kObjectType);
			jComm.AddMember("covermode", (uint32_t)commInfo.m_coverMode, allocator);
			jComm.AddMember("pricemode", (uint32_t)commInfo.m_priceMode, allocator);
			jComm.AddMember("category", (uint32_t)commInfo.m_ccCategory, allocator);
			jComm.AddMember("trademode", (uint32_t)commInfo.m_tradeMode, allocator);
			jComm.AddMember("precision", commInfo.m_uPrecision, allocator);
			jComm.AddMember("pricetick", commInfo.m_fPriceTick, allocator);
			jComm.AddMember("volscale", commInfo.m_uVolScale, allocator);

			jComm.AddMember("name", rj::Value(commInfo.m_strName.c_str(), allocator), allocator);
			jComm.AddMember("exchg", rj::Value(commInfo.m_strExchg.c_str(), allocator), allocator);
			jComm.AddMember("session", rj::Value(commInfo.m_strSession.c_str(), allocator), allocator);
			jComm.AddMember("holiday", rj::Value("CHINA", allocator), allocator);

			//jComms[commInfo.m_strExchg][commInfo.m_strProduct] = jComm;
			jComms[commInfo.m_strExchg.c_str()].AddMember(rj::Value(commInfo.m_strProduct.c_str(), allocator), jComm, allocator);
		}
	}

	//Json::Value jContracts(Json::objectValue);
	rj::Document jContracts(rj::kObjectType);
	{
		rj::Document::AllocatorType &allocator = jContracts.GetAllocator();
		for (auto it = _contracts.begin(); it != _contracts.end(); it++)
		{
			const Contract& cInfo = it->second;
			if (!jContracts.HasMember(cInfo.m_strExchg.c_str()))
			{
				//jComms[commInfo.m_strExchg] = Json::Value(Json::objectValue);
				jContracts.AddMember(rj::Value(cInfo.m_strExchg.c_str(), allocator), rj::Value(rj::kObjectType), allocator);
			}

			rj::Value jcInfo(rj::kObjectType);

			jcInfo.AddMember("name", rj::Value(cInfo.m_strName.c_str(), allocator), allocator);
			jcInfo.AddMember("code", rj::Value(cInfo.m_strCode.c_str(), allocator), allocator);
			jcInfo.AddMember("exchg", rj::Value(cInfo.m_strExchg.c_str(), allocator), allocator);
			jcInfo.AddMember("product", rj::Value(cInfo.m_strProduct.c_str(), allocator), allocator);

			jcInfo.AddMember("maxlimitqty", cInfo.m_maxLmtQty, allocator);
			jcInfo.AddMember("maxmarketqty", cInfo.m_maxMktQty, allocator);
			jcInfo.AddMember("minlimitqty", cInfo.m_minLmtQty, allocator);
			jcInfo.AddMember("minmarketqty", cInfo.m_minMktQty, allocator);

			jcInfo.AddMember("opendate", cInfo.m_uOpenDate, allocator);
			jcInfo.AddMember("expiredate", cInfo.m_uExpireDate, allocator);

			jcInfo.AddMember("longmarginratio", cInfo.m_dLongMarginRatio, allocator);
			jcInfo.AddMember("shortmarginratio", cInfo.m_dShortMarginRatio, allocator);

			if (cInfo.m_optType != OT_None)
			{
				//OptionType	m_optType;
				//std::string m_strUnderlying;
				//double		m_strikePrice;
				//double		m_dUnderlyingScale;
				rj::Value optInfo(rj::kObjectType);
				optInfo.AddMember("optiontype", (uint32_t)cInfo.m_optType, allocator);
				optInfo.AddMember("underlying", rj::Value(cInfo.m_strUnderlying.c_str(), allocator), allocator);
				optInfo.AddMember("strikeprice", cInfo.m_strikePrice, allocator);
				optInfo.AddMember("underlyingscale", cInfo.m_dUnderlyingScale, allocator);

				jcInfo.AddMember("option", optInfo, allocator);
			}

			jContracts[cInfo.m_strExchg.c_str()].AddMember(rj::Value(cInfo.m_strCode.c_str(), allocator), jcInfo, allocator);
		}
	}

	std::ofstream ofs;
	std::string path = SAVEPATH;
	path += COMM_FILE;
	ofs.open(path);
	{
		rj::StringBuffer sb;
		rj::PrettyWriter<rj::StringBuffer> writer(sb);
		jComms.Accept(writer);
		ofs << sb.GetString();
	}
	ofs.close();
	std::cerr << "--->>> " << _commodities.size() << " commodities dumped into : " <<  path << std::endl;

	path = SAVEPATH;
	path += CONT_FILE;
	ofs.open(path);
	{
		rj::StringBuffer sb;
		rj::PrettyWriter<rj::StringBuffer> writer(sb);
		jContracts.Accept(writer);
		ofs << sb.GetString();
	}
	ofs.close();
	std::cerr << "--->>> " << _contracts.size() << " contracts dumped into : " << path << std::endl;
}


/**
 * \brief 前置断开连接回调函数
 * 
 * 当交易前置连接断开时调用此函数
 * 输出断开连接的原因代码，CTP API会自动重连
 * 
 * \param nReason 断开原因代码
 */
void CTraderSpi::OnFrontDisconnected(int nReason)
{
	std::cerr << "--->>> " << "OnFrontDisconnected" << std::endl;
	std::cerr << "--->>> Reason = " << nReason << std::endl;
}


/**
 * \brief 错误响应回调函数
 * 
 * 当收到错误响应时调用此函数
 * 输出错误信息，帮助识别和跟踪问题
 * 
 * \param pRspInfo 错误信息结构
 * \param nRequestID 请求ID
 * \param bIsLast 是否为最后一条消息
 */
void CTraderSpi::OnRspError(CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	std::cerr << "--->>> " << "OnRspError" << std::endl;
	IsErrorRspInfo(pRspInfo);
}

/**
 * \brief 检查响应信息是否包含错误
 * 
 * 检查CTP响应信息中是否包含错误信息
 * 如果错误代码不为0，则表示有错误，并输出错误信息
 * 
 * \param pRspInfo CTP响应信息结构
 * \return 如果有错误返回true，否则返回false
 */
bool CTraderSpi::IsErrorRspInfo(CThostFtdcRspInfoField *pRspInfo)
{
	// 如果ErrorID != 0, 说明收到了错误的响应
	bool bResult = ((pRspInfo) && (pRspInfo->ErrorID != 0));
	if (bResult)
		std::cerr << "--->>> ErrorID=" << pRspInfo->ErrorID << ", ErrorMsg=" << encode_text(pRspInfo->ErrorMsg) << std::endl;
	return bResult;
}