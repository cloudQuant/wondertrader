#include <string>
#include <map>

#include <API/CTPOpt3.5.8/ThostFtdcTraderApi.h>
#include "TraderSpi.h"

#include "../Share/IniHelper.hpp"
#include "../Share/ModuleHelper.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/DLLHelper.hpp"
#include "../Share/fmtlib.h"

#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;

#include "../WTSUtils/WTSCfgLoader.h"
#include "../Includes/WTSVariant.hpp"
USING_NS_WTP;

#include "../Share/charconv.hpp"

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

std::string COMM_FILE;		//输出的品种文件名
std::string CONT_FILE;		//输出的合约文件名

std::string MODULE_NAME;	//外部模块名

typedef std::map<std::string, std::string>	SymbolMap;
SymbolMap	MAP_NAME;
SymbolMap	MAP_SESSION;

typedef CThostFtdcTraderApi* (*CTPCreator)(const char *);
CTPCreator		g_ctpCreator = NULL;

// 请求编号
int iRequestID = 0;

namespace
{
	bool hasOnlyScalarValues(const WTSVariant* section, const char* const* fields, size_t count)
	{
		for (size_t i = 0; i < count; ++i)
		{
			WTSVariant* value = section->get(fields[i]);
			if (value != NULL && (value->isObject() || value->isArray()))
				return false;
		}

		return true;
	}

	bool applyVariantConfig(WTSVariant* root, std::string& map_files)
	{
		if (root == NULL || !root->isObject())
			return false;

		WTSVariant* ctp = root->get("ctp");
		WTSVariant* cfg = root->get("config");
		if (ctp == NULL || !ctp->isObject() || cfg == NULL || !cfg->isObject())
			return false;

		// Container values are not scalar configuration values. In particular,
		// WTSVariant::asBoolean() assumes scalar storage and is unsafe for maps/arrays.
		const char* const ctpFields[] = { "front", "broker", "user", "pass", "appid", "authcode", "onlyincfg", "incremental" };
		const char* const configFields[] = { "path", "mask", "commfile", "contfile", "mapfiles", "module" };
		if (!hasOnlyScalarValues(ctp, ctpFields, sizeof(ctpFields) / sizeof(ctpFields[0])) ||
			!hasOnlyScalarValues(cfg, configFields, sizeof(configFields) / sizeof(configFields[0])))
			return false;

		// Parse all values into locals first. A malformed root/section/value must
		// not leave the process-wide loader configuration partially overwritten.
		std::string frontAddr = ctp->getCString("front");
		std::string brokerId = ctp->getCString("broker");
		std::string investorId = ctp->getCString("user");
		std::string password = ctp->getCString("pass");
		std::string appId = ctp->getCString("appid");
		std::string authCode = ctp->getCString("authcode");
		std::string savePath = cfg->getCString("path");
		uint32_t classMask = cfg->getUInt32("mask"); //1-期货,2-期权,4-股票

		std::string commFile = cfg->getCString("commfile");
		if (commFile.empty())
			commFile = "commodities.json";

		std::string contFile = cfg->getCString("contfile");
		if (contFile.empty())
			contFile = "contracts.json";

		std::string parsedMapFiles = cfg->getCString("mapfiles");
		bool onlyInCfg = ctp->getBoolean("onlyincfg");
		bool incremental = ctp->getBoolean("incremental");

		std::string moduleName = cfg->getCString("module");
		if (moduleName.empty())
		{
#ifdef _WIN32
			moduleName = "./soptthosttraderapi_se.dll";
#else
			moduleName = "./soptthosttraderapi_se.so";
#endif
		}

		FRONT_ADDR = frontAddr;
		BROKER_ID = brokerId;
		INVESTOR_ID = investorId;
		PASSWORD = password;
		APPID = appId;
		AUTHCODE = authCode;
		SAVEPATH = savePath;
		CLASSMASK = classMask;
		COMM_FILE = commFile;
		CONT_FILE = contFile;
		map_files = parsedMapFiles;
		ONLYINCFG = onlyInCfg;
		INCREMENTAL = incremental;
		MODULE_NAME = moduleName;
		return true;
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
	// I03-NEW-091 / K02=B: run() reports a result code in every mode; a missing,
	// unparsable or incomplete configuration returns 1 (as the module failures do).
	if (cfgfile == NULL)
		return 1;

	if (!isFile)
	{
		WTSVariant* root = WTSCfgLoader::load_from_content(cfgfile, true);
		if (root == NULL)
			return 1;
		if (!applyVariantConfig(root, map_files))
		{
			root->release();
			return 1;
		}
		root->release();
	}
	else if (StrUtil::endsWith(cfgfile, ".ini"))
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

		COMM_FILE = ini.readString("config", "commfile", "commodities.json");
		CONT_FILE = ini.readString("config", "contfile", "contracts.json");

		map_files = ini.readString("config", "mapfiles", "");
		ONLYINCFG = wt_stricmp(ini.readString("config", "onlyincfg", "false").c_str(), "true") == 0;
		INCREMENTAL = wt_stricmp(ini.readString("config", "incremental", "false").c_str(), "true") == 0;

#ifdef _WIN32
		MODULE_NAME = ini.readString("config", "module", "./soptthosttraderapi_se.dll");
#else
		MODULE_NAME = ini.readString("config", "module", "./soptthosttraderapi_se.so");
#endif
	}
	else
	{
		WTSVariant* root = WTSCfgLoader::load_from_file(cfgfile);
		if (root == NULL)
			return 1;
		if (!applyVariantConfig(root, map_files))
		{
			root->release();
			return 1;
		}
		root->release();
	}

	if(!StdFile::exists(MODULE_NAME.c_str()))
	{
		MODULE_NAME = getBinDir();
#ifdef _WIN32
		MODULE_NAME += "traders/soptthosttraderapi_se.dll";
#else
		MODULE_NAME += "traders/soptthosttraderapi_se.so";
#endif
	}

	if(FRONT_ADDR.empty() || BROKER_ID.empty() || INVESTOR_ID.empty() || PASSWORD.empty() || SAVEPATH.empty())
	{
		return 1;
	}

	SAVEPATH = StrUtil::standardisePath(SAVEPATH);

	if (!map_files.empty())
	{
		StringVector ayFiles = StrUtil::split(map_files, ",");
		for (const std::string& fName : ayFiles)
		{
			printf("Reading mapping file %s...\r\n", fName.c_str());
			IniHelper iniMap;
			if (!StdFile::exists(fName.c_str()))
				continue;

			iniMap.load(fName.c_str());
			FieldArray ayKeys, ayVals;
			int cout = iniMap.readSecKeyValArray("Name", ayKeys, ayVals);
			for (int i = 0; i < cout; i++)
			{
				std::string pName = ayVals[i];
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
	DllHandle dllInst = DLLHelper::load_library(MODULE_NAME.c_str());
	if (dllInst == NULL)
	{
		printf("Loading module %s failed\r\n", MODULE_NAME.c_str());
		g_ctpCreator = NULL;
		pUserApi = NULL;
		return 1;
	}
#ifdef _WIN32
#	ifdef _WIN64
	g_ctpCreator = (CTPCreator)DLLHelper::get_symbol(dllInst, "?CreateFtdcTraderApi@CThostFtdcTraderApi@ctp_sopt@@SAPEAV12@PEBD@Z");
#	else
	g_ctpCreator = (CTPCreator)DLLHelper::get_symbol(dllInst, "?CreateFtdcTraderApi@CThostFtdcTraderApi@ctp_sopt@@SAPAV12@PBD@Z");
#	endif
#else
	g_ctpCreator = (CTPCreator)DLLHelper::get_symbol(dllInst, "_ZN8ctp_sopt19CThostFtdcTraderApi19CreateFtdcTraderApiEPKc");
#endif
	if (g_ctpCreator == NULL)
	{
		printf("Loading CreateFtdcTraderApi failed\r\n");
		DLLHelper::free_library(dllInst);
		pUserApi = NULL;
		return 1;
	}

	std::string flowPath = fmtutil::format("./CTPFlow/{}/{}/", BROKER_ID, INVESTOR_ID);
	fs::create_directories(flowPath.c_str());
	loader_completion::reset();
	pUserApi = g_ctpCreator(flowPath.c_str());
	if (pUserApi == NULL)
	{
		printf("CreateFtdcTraderApi returned NULL\r\n");
		g_ctpCreator = NULL;
		DLLHelper::free_library(dllInst);
		return 1;
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
	delete pUserSpi;
	return result;
}
