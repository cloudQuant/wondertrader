#include <string>
#include <map>
#include <set>
#ifdef __APPLE__
#include <API/CTP6.6.9/ThostFtdcTraderApi.h>
#else
//v6.3.15
#include <API/CTP6.3.15/ThostFtdcTraderApi.h>
#endif
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

// UserApi对象
CThostFtdcTraderApi* pUserApi;

// 配置参数
std::string	FRONT_ADDR;	// 前置地址
std::string	BROKER_ID;	// 经纪公司代码
std::string	INVESTOR_ID;// 投资者代码
std::string	PASSWORD;	// 用户密码
std::string SAVEPATH;	//保存位置
std::string APPID;
std::string AUTHCODE;
uint32_t	CLASSMASK;	//期权
bool		ONLYINCFG;	//只落地配置文件有的
bool		INCREMENTAL;//是否增量拉取，默认false
bool		QRYFEES;	//查询费率

std::string COMM_FILE;		//输出的品种文件名
std::string CONT_FILE;		//输出的合约文件名
std::string FEES_FILE;		//输出的费率文件名

std::string MODULE_NAME;	//外部模块名

typedef std::map<std::string, std::string>	SymbolMap;
SymbolMap	MAP_NAME;
SymbolMap	MAP_SESSION;

std::string FEE_FILTERS;
std::set<std::string>	SET_FILTERS;

typedef CThostFtdcTraderApi* (*CTPCreator)(const char *);
CTPCreator		g_ctpCreator = NULL;

// 请求编号
int iRequestID = 0;

namespace
{
	struct CTPConfig
	{
		std::string front;
		std::string broker;
		std::string investor;
		std::string password;
		std::string savepath;
		std::string appid;
		std::string authcode;
		uint32_t classmask;
		bool onlyincfg;
		bool incremental;
		bool qryfees;
		std::string commFile;
		std::string contFile;
		std::string feesFile;
		std::string module;
		std::string feeFilters;
		std::string mapFiles;

		CTPConfig()
			: classmask(0), onlyincfg(false), incremental(false), qryfees(false),
			  commFile("commodities.json"), contFile("contracts.json"), feesFile("fees.json")
		{}
	};

	std::string defaultModuleName()
	{
#ifdef _WIN32
		return "./thosttraderapi_se.dll";
#else
		return "./thosttraderapi_se.so";
#endif
	}

	bool readVariantConfig(WTSVariant* root, CTPConfig& config)
	{
		if (root == NULL || !root->isObject())
			return false;

		WTSVariant* ctp = root->get("ctp");
		WTSVariant* cfg = root->get("config");
		if (ctp == NULL || !ctp->isObject() || cfg == NULL || !cfg->isObject())
			return false;

		config.front = ctp->getCString("front");
		config.broker = ctp->getCString("broker");
		config.investor = ctp->getCString("user");
		config.password = ctp->getCString("pass");
		config.appid = ctp->getCString("appid");
		config.authcode = ctp->getCString("authcode");
		config.savepath = cfg->getCString("path");
		config.classmask = cfg->getUInt32("mask");
		config.commFile = cfg->getCString("commfile", "commodities.json");
		config.contFile = cfg->getCString("contfile", "contracts.json");
		config.feesFile = cfg->getCString("feesfile", "fees.json");
		config.mapFiles = cfg->getCString("mapfiles");
		config.onlyincfg = cfg->getBoolean("onlyincfg");
		config.incremental = cfg->getBoolean("incremental");
		config.qryfees = cfg->getBoolean("qryfees");
		config.feeFilters = cfg->getCString("feefilter");
		config.module = ctp->getCString("module");
		if (config.module.empty())
			config.module = defaultModuleName();
		return true;
	}

	bool hasIniSection(IniHelper& ini, const char* section)
	{
		FieldArray sections;
		ini.readSections(sections);
		for (const std::string& name : sections)
		{
			if (name == section)
				return true;
		}
		return false;
	}

	bool readIniConfig(const char* path, CTPConfig& config)
	{
		IniHelper ini;
		ini.load(path);
		if (!ini.isLoaded() || !hasIniSection(ini, "ctp") || !hasIniSection(ini, "config"))
			return false;

		config.front = ini.readString("ctp", "front", "");
		config.broker = ini.readString("ctp", "broker", "");
		config.investor = ini.readString("ctp", "user", "");
		config.password = ini.readString("ctp", "pass", "");
		config.appid = ini.readString("ctp", "appid", "");
		config.authcode = ini.readString("ctp", "authcode", "");
		config.savepath = ini.readString("config", "path", "");
		config.classmask = ini.readUInt("config", "mask", 1 | 2 | 4);
		config.onlyincfg = wt_stricmp(ini.readString("config", "onlyincfg", "false").c_str(), "true") == 0;
		config.incremental = wt_stricmp(ini.readString("config", "incremental", "false").c_str(), "true") == 0;
		config.qryfees = wt_stricmp(ini.readString("config", "qryfees", "false").c_str(), "true") == 0;
		config.feeFilters = ini.readString("config", "feefilter", "");
		config.commFile = ini.readString("config", "commfile", "commodities.json");
		config.contFile = ini.readString("config", "contfile", "contracts.json");
		config.feesFile = ini.readString("config", "feesfile", "fees.json");
		config.mapFiles = ini.readString("config", "mapfiles", "");
		config.module = ini.readString("ctp", "module", defaultModuleName().c_str());
		return true;
	}

	void applyConfig(const CTPConfig& config)
	{
		FRONT_ADDR = config.front;
		BROKER_ID = config.broker;
		INVESTOR_ID = config.investor;
		PASSWORD = config.password;
		APPID = config.appid;
		AUTHCODE = config.authcode;
		SAVEPATH = config.savepath;
		CLASSMASK = config.classmask;
		ONLYINCFG = config.onlyincfg;
		INCREMENTAL = config.incremental;
		QRYFEES = config.qryfees;
		FEE_FILTERS = config.feeFilters;
		COMM_FILE = config.commFile;
		CONT_FILE = config.contFile;
		FEES_FILE = config.feesFile;
		MODULE_NAME = config.module;
	}
}

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

int run(const char* cfgfile, bool bAsync = false, bool isFile = true)
{
#ifdef _WIN32
	SetConsoleOutputCP(CP_UTF8);
#endif // _WIN32

	std::string map_files;
	if (cfgfile == NULL)
		return 0;

	CTPConfig config;

	if(!isFile)
	{
		WTSVariant* root = NULL;
		try
		{
			root = WTSCfgLoader::load_from_content(cfgfile, true);
		}
		catch (...)
		{
			return 0;
		}
		if (root == NULL)
			return 0;
		bool valid = readVariantConfig(root, config);
		root->release();
		if (!valid)
			return 0;
	}
	else if(StrUtil::endsWith(cfgfile, ".ini"))
	{
		if (!readIniConfig(cfgfile, config))
			return 0;
	}
	else
	{
		WTSVariant* root = NULL;
		try
		{
			root = WTSCfgLoader::load_from_file(cfgfile);
		}
		catch (...)
		{
			return 0;
		}
		if (root == NULL)
			return 0;
		bool valid = readVariantConfig(root, config);
		root->release();
		if (!valid)
			return 0;
	}

	if (config.front.empty() || config.broker.empty() || config.investor.empty() ||
		config.password.empty() || config.savepath.empty())
	{
		return 0;
	}

	map_files = config.mapFiles;
	applyConfig(config);

	if(!StdFile::exists(MODULE_NAME.c_str()))
	{
		MODULE_NAME = fmtutil::format("{}traders/{}", getBinDir(), MODULE_NAME.c_str());
	}

	SAVEPATH = StrUtil::standardisePath(SAVEPATH);

	if(!FEE_FILTERS.empty())
	{
		auto ay = StrUtil::split(FEE_FILTERS, ",");
		for (auto& s : ay)
		{
			if (s.empty())
				continue;

			SET_FILTERS.insert(s);
		}
	}
	
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
				printf("Commodity name mapping: %s - %s\r\n", ayKeys[i].c_str(), ayVals[i].c_str());
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
#ifdef __APPLE__
	g_ctpCreator = CThostFtdcTraderApi::CreateFtdcTraderApi;
#else
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
#endif

	std::string flowPath = fmtutil::format("./CTPFlow/{}/{}/", BROKER_ID, INVESTOR_ID);
	boost::filesystem::create_directories(flowPath.c_str());
	if (g_ctpCreator == NULL)
		return EXIT_FAILURE;
	loader_completion::reset();
	pUserApi = g_ctpCreator(flowPath.c_str());
	if (pUserApi == NULL)
	{
		printf("CreateFtdcTraderApi returned NULL\r\n");
		return EXIT_FAILURE;
	}
	CTraderSpi* pUserSpi = new CTraderSpi();
	pUserApi->RegisterSpi((CThostFtdcTraderSpi*)pUserSpi);			// 注册事件类
	pUserApi->SubscribePublicTopic(THOST_TERT_QUICK);					// 注册公有流
	pUserApi->SubscribePrivateTopic(THOST_TERT_QUICK);					// 注册私有流
	pUserApi->RegisterFront((char*)FRONT_ADDR.c_str());				// connect
	pUserApi->Init();

	// K02=B (2026-11-04): every mode waits for the terminal result on this
	// (non-callback) thread, then releases the API and returns the result code;
	// bAsync is kept only for signature compatibility.
	(void)bAsync;
	const int result = loader_completion::wait();
	pUserApi->RegisterSpi(NULL);
	pUserApi->Release();
	pUserApi = NULL;
	// Card 9a: a worker still blocked in an SDK call cannot be joined safely here;
	// such an SPI is retained until process exit instead of being destroyed.
	if (pUserSpi->WaitWorkersStopped(std::chrono::seconds(5)))
		delete pUserSpi;
	else
		printf("Loader worker still active, SPI retained until process exit\r\n");
	return result;
}
