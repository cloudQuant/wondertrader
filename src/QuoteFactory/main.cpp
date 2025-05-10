/*!
 * \file main.cpp
 * \brief 行情工厂主模块
 * 
 * 行情工厂(QuoteFactory)是一个用于接收、处理和分发行情数据的组件
 * 主要功能包括：
 * 1. 加载并初始化各种行情解析器
 * 2. 收集和处理各种市场数据
 * 3. 通过UDP和共享内存分发行情数据
 * 4. 支持全天候模式和交易时段模式
 * 
 * \author Wesley
 */

#include "../WtDtCore/ParserAdapter.h"
#include "../WtDtCore/DataManager.h"
#include "../WtDtCore/StateMonitor.h"
#include "../WtDtCore/UDPCaster.h"
#include "../WtDtCore/ShmCaster.h"
#include "../WtDtCore/WtHelper.h"
#include "../WtDtCore/IndexFactory.h"

#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/WTSVariant.hpp"

#include "../WTSTools/WTSHotMgr.h"
#include "../WTSTools/WTSBaseDataMgr.h"
#include "../WTSTools/WTSLogger.h"
#include "../WTSUtils/WTSCfgLoader.h"
#include "../Share/StrUtil.hpp"
#include "../Share/cppcli.hpp"

#include "../WTSUtils/SignalHook.hpp"

//! 基础数据管理器，管理交易时段、合约、品种等基础数据
WTSBaseDataMgr	g_baseDataMgr;
//! 主力合约管理器，管理主力合约和次主力合约规则
WTSHotMgr		g_hotMgr;
//! 状态监控器，管理交易时段状态和监控交易时段切换
StateMonitor	g_stateMon;
//! UDP广播器，通过UDP分发行情数据
UDPCaster		g_udpCaster;
//! 共享内存广播器，通过共享内存分发行情数据
ShmCaster		g_shmCaster;
//! 数据管理器，管理行情数据的存储和分发
DataManager		g_dataMgr;
//! 解析器适配器管理器，管理所有的行情解析器
ParserAdapterMgr g_parsers;
//! 指数工厂，用于计算和管理各种指数
IndexFactory	g_idxFactory;

#ifdef _MSC_VER
#include "../Common/mdump.h"
DWORD g_dwMainThreadId = 0;
BOOL WINAPI ConsoleCtrlhandler(DWORD dwCtrlType)
{
	switch (dwCtrlType)
	{
	case CTRL_CLOSE_EVENT:
	{
		g_dataMgr.release();

		PostThreadMessage(g_dwMainThreadId, WM_QUIT, 0, 0);
	}
	break;
	}

	return TRUE;
}
#endif

/**
 * \brief 获取当前可执行文件所在目录
 * 
 * 该函数使用静态存储技术，只在第一次调用时计算路径
 * 后续调用直接返回缓存的路径
 * 
 * \return 返回当前可执行文件所在目录的标准路径，以字符串形式
 */
const char* getBinDir()
{
	// 使用静态变量缓存路径，避免重复计算
	static std::string basePath;
	if (basePath.empty())
	{
		// 获取当前工作目录
		basePath = boost::filesystem::initial_path<boost::filesystem::path>().string();

		// 标准化路径，确保路径格式统一
		basePath = StrUtil::standardisePath(basePath);
	}

	return basePath.c_str();
}


/**
 * \brief 初始化数据管理器
 * 
 * 根据配置和运行模式初始化数据管理器
 * 如果是全天候模式，则不使用状态监控器
 * 
 * \param config 数据管理器配置对象
 * \param bAlldayMode 是否为全天候模式，默认为false
 */
void initDataMgr(WTSVariant* config, bool bAlldayMode = false)
{
	//如果是全天模式，则不传递状态机给DataManager
	g_dataMgr.init(config, &g_baseDataMgr, bAlldayMode ? NULL : &g_stateMon);
}

/**
 * \brief 初始化行情解析器
 * 
 * 根据配置初始化所有活跃的行情解析器
 * 如果解析器没有指定ID，会自动生成一个唯一ID
 * 
 * \param cfg 解析器配置数组
 */
void initParsers(WTSVariant* cfg)
{
	for (uint32_t idx = 0; idx < cfg->size(); idx++)
	{
		WTSVariant* cfgItem = cfg->get(idx);
		// 跳过未激活的解析器
		if (!cfgItem->getBoolean("active"))
			continue;

		const char* id = cfgItem->getCString("id");
		// By Wesley @ 2021.12.14
		// 如果id为空，则生成自动id
		std::string realid = id;
		if (realid.empty())
		{
			// 使用静态变量生成自增的解析器ID
			static uint32_t auto_parserid = 1000;
			realid = StrUtil::printf("auto_parser_%u", auto_parserid++);
		}

		// 创建解析器适配器并初始化
		ParserAdapterPtr adapter(new ParserAdapter(&g_baseDataMgr, &g_dataMgr, &g_idxFactory));
		adapter->init(realid.c_str(), cfgItem);
		// 将适配器添加到管理器中
		g_parsers.addAdapter(realid.c_str(), adapter);
	}

	// 输出已加载的解析器数量
	WTSLogger::info("{} market data parsers loaded in total", g_parsers.size());
}

/**
 * \brief 初始化行情工厂
 * 
 * 根据配置文件初始化行情工厂的各个组件
 * 包括加载基础数据、初始化广播器、状态监控器和解析器等
 * 
 * \param filename 配置文件路径
 */
void initialize(const std::string& filename)
{
	// 设置模块目录
	WtHelper::set_module_dir(getBinDir());

	// 加载配置文件
	WTSVariant* config = WTSCfgLoader::load_from_file(filename.c_str());
	if(config == NULL)
	{
		WTSLogger::error("Loading config file {} failed", filename);
		return;
	}

	//加载市场信息
	WTSVariant* cfgBF = config->get("basefiles");
	if (cfgBF->get("session"))
	{
		g_baseDataMgr.loadSessions(cfgBF->getCString("session"));
		WTSLogger::info("Trading sessions loaded");
	}

	WTSVariant* cfgItem = cfgBF->get("commodity");
	if (cfgItem)
	{
		if (cfgItem->type() == WTSVariant::VT_String)
		{
			g_baseDataMgr.loadCommodities(cfgItem->asCString());
		}
		else if (cfgItem->type() == WTSVariant::VT_Array)
		{
			for (uint32_t i = 0; i < cfgItem->size(); i++)
			{
				g_baseDataMgr.loadCommodities(cfgItem->get(i)->asCString());
			}
		}
	}

	cfgItem = cfgBF->get("contract");
	if (cfgItem)
	{
		if (cfgItem->type() == WTSVariant::VT_String)
		{
			g_baseDataMgr.loadContracts(cfgItem->asCString());
		}
		else if (cfgItem->type() == WTSVariant::VT_Array)
		{
			for (uint32_t i = 0; i < cfgItem->size(); i++)
			{
				g_baseDataMgr.loadContracts(cfgItem->get(i)->asCString());
			}
		}
	}

	if (cfgBF->get("holiday"))
	{
		g_baseDataMgr.loadHolidays(cfgBF->getCString("holiday"));
		WTSLogger::info("Holidays loaded");
	}
	if (cfgBF->get("hot"))
	{
		g_hotMgr.loadHots(cfgBF->getCString("hot"));
		WTSLogger::log_raw(LL_INFO, "Hot rules loaded");
	}

	if (cfgBF->get("second"))
	{
		g_hotMgr.loadSeconds(cfgBF->getCString("second"));
		WTSLogger::log_raw(LL_INFO, "Second rules loaded");
	}

	if (cfgBF->has("rules"))
	{
		auto cfgRules = cfgBF->get("rules");
		auto tags = cfgRules->memberNames();
		for (const std::string& ruleTag : tags)
		{
			g_hotMgr.loadCustomRules(ruleTag.c_str(), cfgRules->getCString(ruleTag.c_str()));
			WTSLogger::info("{} rules loaded from {}", ruleTag, cfgRules->getCString(ruleTag.c_str()));
		}
	}

	if (config->has("shmcaster"))
	{
		g_shmCaster.init(config->get("shmcaster"));
		g_dataMgr.add_caster(&g_shmCaster);
	}

	if (config->has("broadcaster"))
	{
		g_udpCaster.init(config->get("broadcaster"), &g_baseDataMgr, &g_dataMgr);
		g_dataMgr.add_caster(&g_udpCaster);
	}


	//By Wesley @ 2021.12.27
	//全天候模式，不需要再使用状态机
	bool bAlldayMode = config->getBoolean("allday");
	if (!bAlldayMode)
	{
		g_stateMon.initialize(config->getCString("statemonitor"), &g_baseDataMgr, &g_dataMgr);
	}
	else
	{
		WTSLogger::info("QuoteFactory will run in allday mode");
	}
	initDataMgr(config->get("writer"), bAlldayMode);

	if(config->has("index"))
	{
		//如果存在指数模块要，配置指数
		const char* filename = config->getCString("index");
		WTSLogger::info("Reading index config from {}...", filename);
		WTSVariant* var = WTSCfgLoader::load_from_file(filename);
		if (var)
		{
			g_idxFactory.init(var, &g_hotMgr, &g_baseDataMgr, &g_dataMgr);
			var->release();
		}
		else
		{
			WTSLogger::error("Loading index config {} failed", filename);
		}		
	}

	WTSVariant* cfgParser = config->get("parsers");
	if (cfgParser)
	{
		if (cfgParser->type() == WTSVariant::VT_String)
		{
			const char* filename = cfgParser->asCString();
			if (StdFile::exists(filename))
			{
				WTSLogger::info("Reading parser config from {}...", filename);
				WTSVariant* var = WTSCfgLoader::load_from_file(filename);
				if (var)
				{
					initParsers(var->get("parsers"));
					var->release();
				}
				else
				{
					WTSLogger::error("Loading parser config {} failed", filename);
				}
			}
			else
			{
				WTSLogger::error("Parser configuration {} not exists", filename);
			}
		}
		else if (cfgParser->type() == WTSVariant::VT_Array)
		{
			initParsers(cfgParser);
		}
	}

	config->release();

	g_parsers.run();

	//全天候模式，不启动状态机
	if(!bAlldayMode)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		g_stateMon.run();
	}
}

/**
 * \brief 程序入口函数
 * 
 * 行情工厂的主函数，负责解析命令行参数、初始化日志系统、
 * 安装信号处理器、初始化行情工厂并进入主循环
 * 
 * \param argc 命令行参数数量
 * \param argv 命令行参数数组
 * \return 程序退出码，0表示正常退出
 */
int main(int argc, char* argv[])
{
	// 初始化命令行参数解析器
	cppcli::Option opt(argc, argv);

	// 定义命令行参数
	auto cParam = opt("-c", "--config", "configure filepath, dtcfg.yaml as default", false);
	auto lParam = opt("-l", "--logcfg", "logging configure filepath, logcfgdt.yaml as default", false);

	auto hParam = opt("-h", "--help", "gain help doc", false)->asHelpParam();

	// 解析命令行参数
	opt.parse();

	// 如果指定了帮助参数，则显示帮助信息并退出
	if (hParam->exists())
		return 0;

	// 处理日志配置文件路径
	std::string filename;
	if (lParam->exists())
		filename = lParam->get<std::string>();
	else
		filename = "./logcfgdt.yaml"; // 使用默认日志配置文件
	// 初始化日志系统
	WTSLogger::init(filename.c_str());

#ifdef _MSC_VER
	_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_DEBUG);
	_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG);
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);

	_set_error_mode(_OUT_TO_STDERR);
	_set_abort_behavior(0, _WRITE_ABORT_MSG);

	g_dwMainThreadId = GetCurrentThreadId();
	SetConsoleCtrlHandler(ConsoleCtrlhandler, TRUE);

	CMiniDumper::Enable("QuoteFactory.exe", true);
#endif

	// 初始化退出标志
	bool bExit = false;
	// 安装信号处理钩子，用于处理系统信号（如Ctrl+C）
	install_signal_hooks([&bExit](const char* message) {
		// 错误信息处理回调
		if(!bExit)
			WTSLogger::error(message);
	}, [&bExit](bool toExit) {
		// 退出信号处理回调
		if (bExit)
			return;

		// 设置退出标志并记录日志
		bExit = toExit;
		WTSLogger::info("Exit flag is {}", bExit);
	});

	// 处理主配置文件路径
	if (cParam->exists())
		filename = cParam->get<std::string>();
	else
		filename = "./dtcfg.yaml"; // 使用默认配置文件

	// 检查配置文件是否存在
	if(!StdFile::exists(filename.c_str()))
	{
		fmt::print("confiture {} not exists", filename);
		return 0;
	}

	// 初始化行情工厂
	initialize(filename);

	// 主循环，等待退出信号
	while (!bExit)
	{
		// 小休眠减少CPU占用
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	
	// 正常退出
	return 0;
}

