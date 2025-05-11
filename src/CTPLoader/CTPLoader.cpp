/*!
 * \file CTPLoader.cpp
 * \brief CTP合约信息加载器主模块
 * 
 * 该文件实现了CTP合约信息加载器，用于从交易所获取合约信息
 * 并将其转换为WonderTrader框架所需的JSON格式
 * 主要功能包括：
 * 1. 读取配置文件并初始化CTP接口
 * 2. 加载合约映射文件，将交易所代码映射到中文名称
 * 3. 创建并运行交易API实例，获取合约信息
 * 4. 支持动态加载不同版本的CTP API
 * 
 * 使用CTP API v6.3.15版本
 * 
 * \author Wesley
 */

#include <string>
#include <map>
//v6.3.15
#include "../API/CTP6.3.15/ThostFtdcTraderApi.h"
#include "TraderSpi.h"

#include "../Share/IniHelper.hpp"
#include "../Share/ModuleHelper.hpp"
#include "../Share/DLLHelper.hpp"
#include "../Share/StdUtils.hpp"

#include "../Share/charconv.hpp"
#include "../Share/fmtlib.h"

#include "../WTSUtils/WTSCfgLoader.h"
#include "../Includes/WTSVariant.hpp"
USING_NS_WTP;

#include <boost/filesystem.hpp>

//! CTP交易API对象
CThostFtdcTraderApi* pUserApi;

//! 配置参数
//! 交易前置地址
std::string	FRONT_ADDR;
//! 经纪公司代码
std::string	BROKER_ID;
//! 投资者代码
std::string	INVESTOR_ID;
//! 用户密码
std::string	PASSWORD;
//! 数据保存路径
std::string SAVEPATH;
//! 应用程序ID，用于认证
std::string APPID;
//! 授权码，用于认证
std::string AUTHCODE;
//! 合约类型掩码，1-期货，2-期权，4-股票
int32_t	CLASSMASK;
//! 是否只处理配置文件中有的合约
bool		ONLYINCFG;

//! 输出的品种文件名
std::string COMM_FILE;
//! 输出的合约文件名
std::string CONT_FILE;

//! CTP API模块名称
std::string MODULE_NAME;

/**
 * \brief 字符串映射类型
 * 
 * 用于存储合约代码到名称的映射和合约代码到交易时段的映射
 */
typedef std::map<std::string, std::string>	SymbolMap;
//! 合约代码到名称的映射
SymbolMap	MAP_NAME;
//! 合约代码到交易时段的映射
SymbolMap	MAP_SESSION;

/**
 * \brief CTP API创建函数类型
 * 
 * 指向CTP API创建函数的函数指针类型
 * 
 * \param flowPath 流文件路径
 * \return 返回创建的CTP API对象
 */
typedef CThostFtdcTraderApi* (*CTPCreator)(const char *);
//! CTP API创建函数指针
CTPCreator		g_ctpCreator = NULL;

//! 请求编号，用于跟踪请求
int iRequestID = 0;

#ifdef _MSC_VER
#	define EXPORT_FLAG __declspec(dllexport)
#else
#	define EXPORT_FLAG __attribute__((__visibility__("default")))
#endif

#ifdef __cplusplus
extern "C"
{
#endif
	EXPORT_FLAG int run(const char* cfgfile, bool bAsync, bool isFile);
#ifdef __cplusplus
}
#endif

/**
 * \brief 运行CTP合约信息加载器
 * 
 * 该函数是合约信息加载器的主入口点，完成以下工作：
 * 1. 读取配置文件并初始化参数
 * 2. 加载合约映射文件
 * 3. 初始化并运行CTP API实例
 * 4. 获取合约信息并转换为WonderTrader框架格式
 * 
 * \param cfgfile 配置文件路径
 * \param bAsync 是否异步运行，如果为true则不等待API返回
 * \param isFile 是否为文件路径，如果为false则cfgfile为配置内容字符串
 * \return 成功返回0，失败返回非0值
 */
int run(const char* cfgfile, bool bAsync = false, bool isFile = true)
{
	std::string map_files;

	if(!isFile)
	{
		WTSVariant* root = WTSCfgLoader::load_from_content(cfgfile, true);
		if (root == NULL)
			return 0;

		WTSVariant* ctp = root->get("ctp");
		FRONT_ADDR = ctp->getCString("front");
		BROKER_ID = ctp->getCString("broker");
		INVESTOR_ID = ctp->getCString("user");
		PASSWORD = ctp->getCString("pass");
		APPID = ctp->getCString("appid");
		AUTHCODE = ctp->getCString("authcode");

		WTSVariant* cfg = root->get("config");
		SAVEPATH = cfg->getCString("path");
		CLASSMASK = cfg->getUInt32("mask"); //1-期货,2-期权,4-股票

		COMM_FILE = cfg->getCString("commfile");
		if (COMM_FILE.empty())
			COMM_FILE = "commodities.json";

		CONT_FILE = cfg->getCString("contfile");
		if (CONT_FILE.empty())
			CONT_FILE = "contracts.json";

		map_files = cfg->getCString("mapfiles");
		ONLYINCFG = ctp->getBoolean("onlyincfg");

		MODULE_NAME = ctp->getCString("module");
		if (MODULE_NAME.empty())
		{
#ifdef _WIN32
			MODULE_NAME = "./thosttraderapi_se.dll";
#else
			MODULE_NAME = "./thosttraderapi_se.so";
#endif
		}

		root->release();
	}
	else if(StrUtil::endsWith(cfgfile, ".ini"))
	{
		IniHelper ini;

		ini.load(cfgfile);

		FRONT_ADDR = ini.readString("ctp", "front", "");
		BROKER_ID = ini.readString("ctp", "broker", "");
		INVESTOR_ID = ini.readString("ctp", "user", "");
		PASSWORD = ini.readString("ctp", "pass", "");
		APPID = ini.readString("ctp", "appid", "");
		AUTHCODE = ini.readString("ctp", "authcode", "");

		SAVEPATH = ini.readString("config", "path", "");
		CLASSMASK = ini.readUInt("config", "mask", 1 | 2 | 4); //1-期货,2-期权,4-股票
		ONLYINCFG = wt_stricmp(ini.readString("config", "onlyincfg", "false").c_str(), "true") == 0;

		COMM_FILE = ini.readString("config", "commfile", "commodities.json");
		CONT_FILE = ini.readString("config", "contfile", "contracts.json");

		map_files = ini.readString("config", "mapfiles", "");

#ifdef _WIN32
		MODULE_NAME = ini.readString("ctp", "module", "./thosttraderapi_se.dll");
#else
		MODULE_NAME = ini.readString("ctp", "module", "./thosttraderapi_se.so");
#endif
	}
	else
	{
		WTSVariant* root = WTSCfgLoader::load_from_file(cfgfile);
		if (root == NULL)
			return 0;

		WTSVariant* ctp = root->get("ctp");
		FRONT_ADDR = ctp->getCString("front");
		BROKER_ID = ctp->getCString("broker");
		INVESTOR_ID = ctp->getCString("user");
		PASSWORD = ctp->getCString("pass");
		APPID = ctp->getCString("appid");
		AUTHCODE = ctp->getCString("authcode");

		WTSVariant* cfg = root->get("config");
		SAVEPATH = cfg->getCString("path"); 
		CLASSMASK = cfg->getUInt32("mask"); //1-期货,2-期权,4-股票

		COMM_FILE = cfg->getCString("commfile");
		if (COMM_FILE.empty())
			COMM_FILE = "commodities.json";

		CONT_FILE = cfg->getCString("contfile"); 
		if(CONT_FILE.empty())
			CONT_FILE = "contracts.json";

		map_files = cfg->getCString("mapfiles");
		ONLYINCFG = ctp->getBoolean("onlyincfg");

		MODULE_NAME = ctp->getCString("module");
		if(MODULE_NAME.empty())
		{
#ifdef _WIN32
			MODULE_NAME = "./thosttraderapi_se.dll";
#else
			MODULE_NAME = "./thosttraderapi_se.so";
#endif
		}
		root->release();
	}
	
	if(!StdFile::exists(MODULE_NAME.c_str()))
	{
		MODULE_NAME = StrUtil::printf("%straders/%s", getBinDir(), MODULE_NAME.c_str());
	}

	if(FRONT_ADDR.empty() || BROKER_ID.empty() || INVESTOR_ID.empty() || PASSWORD.empty() || SAVEPATH.empty())
	{
		return 0;
	}

	SAVEPATH = StrUtil::standardisePath(SAVEPATH);

	
	if(!map_files.empty())
	{
		StringVector ayFiles = StrUtil::split(map_files, ",");
		for(const std::string& fName:ayFiles)
		{
			printf("Reading mapping file %s...\r\n", fName.c_str());
			IniHelper iniMap;
			if(!StdFile::exists(fName.c_str()))
				continue;

			iniMap.load(fName.c_str());
			FieldArray ayKeys, ayVals;
			int cout = iniMap.readSecKeyValArray("Name", ayKeys, ayVals);
			for (int i = 0; i < cout; i++)
			{
				std::string pName = ayVals[i];
				bool isUTF8 = EncodingHelper::isUtf8((unsigned char*)pName.c_str(), pName.size());
				if (!isUTF8)
					pName = ChartoUTF8(ayVals[i]);
				//保存的时候全部转成UTF8
				MAP_NAME[ayKeys[i]] = pName;
#ifdef _WIN32
				printf("Commodity name mapping: %s - %s\r\n", ayKeys[i].c_str(), isUTF8 ? UTF8toChar(ayVals[i]).c_str() : ayVals[i].c_str());
#else
				printf("Commodity name mapping: %s - %s\r\n", ayKeys[i].c_str(), isUTF8 ? ayVals[i].c_str() : ChartoUTF8(ayVals[i]).c_str());
#endif
			}

			ayKeys.clear();
			ayVals.clear();
			cout = iniMap.readSecKeyValArray("Session", ayKeys, ayVals);
			for (int i = 0; i < cout; i++)
			{
				MAP_SESSION[ayKeys[i]] = ayVals[i];
				printf("Trading session mapping: %s - %s\r\n", ayKeys[i].c_str(), ayVals[i].c_str());
			}
		}
		
	}

	// 初始化UserApi
	DllHandle dllInst = DLLHelper::load_library(MODULE_NAME.c_str());
	if (dllInst == NULL)
		printf("Loading module %s failed\r\n", MODULE_NAME.c_str());
#ifdef _WIN32
#	ifdef _WIN64
	g_ctpCreator = (CTPCreator)DLLHelper::get_symbol(dllInst, "?CreateFtdcTraderApi@CThostFtdcTraderApi@@SAPEAV1@PEBD@Z");
#	else
	g_ctpCreator = (CTPCreator)DLLHelper::get_symbol(dllInst, "?CreateFtdcTraderApi@CThostFtdcTraderApi@@SAPAV1@PBD@Z");
#	endif
#else
	g_ctpCreator = (CTPCreator)DLLHelper::get_symbol(dllInst, "_ZN19CThostFtdcTraderApi19CreateFtdcTraderApiEPKc");
#endif
	if (g_ctpCreator == NULL)
		printf("Loading CreateFtdcTraderApi failed\r\n");

	std::string flowPath = fmtutil::format("./CTPFlow/{}/{}/", BROKER_ID, INVESTOR_ID);
	boost::filesystem::create_directories(flowPath.c_str());
	pUserApi = g_ctpCreator(flowPath.c_str());
	CTraderSpi* pUserSpi = new CTraderSpi();
	pUserApi->RegisterSpi((CThostFtdcTraderSpi*)pUserSpi);			// 注册事件类
	pUserApi->SubscribePublicTopic(THOST_TERT_QUICK);					// 注册公有流
	pUserApi->SubscribePrivateTopic(THOST_TERT_QUICK);					// 注册私有流
	pUserApi->RegisterFront((char*)FRONT_ADDR.c_str());				// connect
	pUserApi->Init();

    //如果不是异步，则等待API返回
    if(!bAsync)
	    pUserApi->Join();

	return 0;
}