/*!
 * @file WtUftRunner.cpp
 * @project	WonderTrader
 *
 * @author Wesley
 * @date 2020/03/30
 * 
 * @brief UFT策略引擎运行器实现文件
 * @details 实现了WtUftRunner类，用于管理UFT策略引擎的初始化和运行。
 *          包括配置加载、各模块初始化、引擎运行等功能。
 */
#include "WtUftRunner.h"
#include "../WtUftCore/ShareManager.h"

#include "../WtUftCore/WtHelper.h"
#include "../WtUftCore/UftStraContext.h"

#include "../Includes/WTSVariant.hpp"
#include "../WTSTools/WTSLogger.h"
#include "../WTSUtils/WTSCfgLoader.h"
#include "../WTSUtils/SignalHook.hpp"
#include "../Share/StrUtil.hpp"

/**
 * @brief 获取可执行文件所在目录
 * @return 可执行文件目录路径
 * @details 使用boost::filesystem获取程序运行时的初始路径，并进行标准化处理
 *          该函数使用静态存储确保路径只计算一次
 */
const char* getBinDir()
{
	static std::string basePath;
	if (basePath.empty())
	{
		// 获取程序运行时的初始路径
		basePath = boost::filesystem::initial_path<boost::filesystem::path>().string();

		// 将路径标准化（处理斜杠等问题）
		basePath = StrUtil::standardisePath(basePath);
	}

	return basePath.c_str();
}


/**
 * @brief WtUftRunner类的构造函数
 * @details 初始化运行器对象并安装信号处理钩子，用于处理程序退出信号
 */
WtUftRunner::WtUftRunner()
	:_to_exit(false)  // 初始化退出标志为否
{
	// 安装信号处理钩子，用于捕获并处理系统信号
	install_signal_hooks([](const char* message) {
		// 错误消息处理回调，输出错误日志
		WTSLogger::error(message);
	}, [this](bool bStopped) {
		// 停止信号处理回调，设置退出标志
		_to_exit = bStopped;
		WTSLogger::info("Exit flag is {}", _to_exit);
	});
}


/**
 * @brief WtUftRunner类的析构函数
 * @details 清理资源并释放内存，由于大多数成员变量是自动释放的，所以这里不需要额外的清理操作
 */
WtUftRunner::~WtUftRunner()
{
	// 由于引擎和各管理器都是对象成员变量，析构时会自动调用其析构函数
	// 无需手动释放资源
}

/**
 * @brief 初始化日志系统和安装目录
 * @param filename 日志配置文件路径
 * @details 使用指定的日志配置文件初始化日志系统，并设置程序的安装目录
 */
void WtUftRunner::init(const std::string& filename)
{
	// 使用配置文件初始化日志系统
	WTSLogger::init(filename.c_str());

	// 设置安装目录为当前可执行文件所在目录
	WtHelper::setInstDir(getBinDir());
}

/**
 * @brief 加载并应用配置
 * @param filename 主配置文件路径
 * @return 配置加载是否成功
 * @details 从指定文件加载主配置，并初始化基础数据、引擎和各种适配器
 *          包括交易时段、品种信息、合约信息、交易解析器等
 */
bool WtUftRunner::config(const std::string& filename)
{
	// 从文件加载配置
	_config = WTSCfgLoader::load_from_file(filename.c_str());
	if(_config == NULL)
	{
		WTSLogger::error("Loading config file {} failed", filename);
		return false;
	}

	//基础数据文件
	WTSVariant* cfgBF = _config->get("basefiles");
	// 加载交易时段配置
	if (cfgBF->get("session"))
		_bd_mgr.loadSessions(cfgBF->getCString("session"));

	// 加载品种信息
	WTSVariant* cfgItem = cfgBF->get("commodity");
	if (cfgItem)
	{
		if (cfgItem->type() == WTSVariant::VT_String)
		{
			// 如果是字符串类型，直接加载单个文件
			_bd_mgr.loadCommodities(cfgItem->asCString());
		}
		else if (cfgItem->type() == WTSVariant::VT_Array)
		{
			// 如果是数组类型，逐个加载多个文件
			for (uint32_t i = 0; i < cfgItem->size(); i++)
			{
				_bd_mgr.loadCommodities(cfgItem->get(i)->asCString());
			}
		}
	}

	// 加载合约信息
	cfgItem = cfgBF->get("contract");
	if (cfgItem)
	{
		if (cfgItem->type() == WTSVariant::VT_String)
		{
			// 如果是字符串类型，直接加载单个文件
			_bd_mgr.loadContracts(cfgItem->asCString());
		}
		else if (cfgItem->type() == WTSVariant::VT_Array)
		{
			// 如果是数组类型，逐个加载多个文件
			for (uint32_t i = 0; i < cfgItem->size(); i++)
			{
				_bd_mgr.loadContracts(cfgItem->get(i)->asCString());
			}
		}
	}

	// 加载节假日信息
	if (cfgBF->get("holiday"))
		_bd_mgr.loadHolidays(cfgBF->getCString("holiday"));

	//初始化运行环境
	initEngine();

	//初始化数据管理
	initDataMgr();

	// 如果存在共享域配置，初始化共享管理器
	if (_config->has("share_domain"))
	{
		WTSVariant* cfg = _config->get("share_domain");
		// 设置引擎到共享管理器
		ShareManager::self().set_engine(&_uft_engine);

		// 初始化共享管理器并设置域名
		ShareManager::self().initialize(cfg->getCString("module"));
		ShareManager::self().init_domain(cfg->getCString("name"));
	}

	// 初始化操作策略管理器
	if(!_act_policy.init(_config->getCString("bspolicy")))
	{
		WTSLogger::error("ActionPolicyMgr init failed, please check config");
	}

	//初始化行情通道
	WTSVariant* cfgParser = _config->get("parsers");
	if (cfgParser)
	{
		if (cfgParser->type() == WTSVariant::VT_String)
		{
			// 如果是字符串类型，表示指向单独的解析器配置文件
			const char* filename = cfgParser->asCString();
			if (StdFile::exists(filename))
			{
				WTSLogger::info("Reading parser config from {}...", filename);
				// 从文件加载解析器配置
				WTSVariant* var = WTSCfgLoader::load_from_file(filename);
				if(var)
				{
					// 初始化解析器
					if (!initParsers(var->get("parsers")))
						WTSLogger::error("Loading parsers failed");
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
			// 如果是数组类型，直接内嵌了解析器配置
			initParsers(cfgParser);
		}
	}

	//初始化交易通道
	WTSVariant* cfgTraders = _config->get("traders");
	if (cfgTraders)
	{
		if (cfgTraders->type() == WTSVariant::VT_String)
		{
			const char* filename = cfgTraders->asCString();
			if (StdFile::exists(filename))
			{
				WTSLogger::info("Reading trader config from {}...", filename);
				WTSVariant* var = WTSCfgLoader::load_from_file(filename);
				if (var)
				{
					if (!initTraders(var->get("traders")))
						WTSLogger::error("Loading traders failed");
					var->release();
				}
				else
				{
					WTSLogger::error("Loading trader config {} failed", filename);
				}
			}
			else
			{
				WTSLogger::error("Trader configuration {} not exists", filename);
			}
		}
		else if (cfgTraders->type() == WTSVariant::VT_Array)
		{
			initTraders(cfgTraders);
		}
	}

	initUftStrategies();
	
	return true;
}

/**
 * @brief 初始化UFT策略
 * @return 初始化是否成功
 * @details 加载策略工厂并初始化所有激活的UFT策略，为每个策略创建上下文并绑定交易适配器
 */
bool WtUftRunner::initUftStrategies()
{
	// 获取策略配置
	WTSVariant* cfg = _config->get("strategies");
	if (cfg == NULL || cfg->type() != WTSVariant::VT_Object)
		return false;

	// 获取UFT策略具体配置
	cfg = cfg->get("uft");
	if (cfg == NULL || cfg->type() != WTSVariant::VT_Array)
		return false;

	// 构建策略工厂路径并加载策略工厂
	std::string path = WtHelper::getCWD() + "uft/";
	_uft_stra_mgr.loadFactories(path.c_str());

	// 遍历所有策略配置
	for (uint32_t idx = 0; idx < cfg->size(); idx++)
	{
		WTSVariant* cfgItem = cfg->get(idx);
		// 跳过非激活的策略
		if(!cfgItem->getBoolean("active"))
			continue;
		// 获取策略ID和名称
		const char* id = cfgItem->getCString("id");
		const char* name = cfgItem->getCString("name");
		// 创建策略实例
		UftStrategyPtr stra = _uft_stra_mgr.createStrategy(name, id);
		if (stra == NULL)
		{
			WTSLogger::error("UFT Strategy {} create failed", name);
			continue;
		}
		else
		{
			WTSLogger::info("UFT Strategy {}({}) created", name, id);
		}

		// 使用配置参数初始化策略
		stra->self()->init(cfgItem->get("params"));
		// 创建策略上下文
		UftStraContext* ctx = new UftStraContext(&_uft_engine, id);
		ctx->set_strategy(stra->self());

		// 绑定交易适配器
		const char* traderid = cfgItem->getCString("trader");
		TraderAdapterPtr trader = _traders.getAdapter(traderid);
		if(trader)
		{
			// 设置交易适配器并添加回调接收器
			ctx->setTrader(trader.get());
			trader->addSink(ctx);
		}
		else
		{
			WTSLogger::error("Trader {} not exists, binding trader to UFT strategy failed", traderid);
		}

		// 将策略上下文添加到引擎
		_uft_engine.addContext(UftContextPtr(ctx));
	}

	return true;
}

/**
 * @brief 初始化UFT策略引擎
 * @return 初始化是否成功
 * @details 使用配置初始化UFT策略引擎，并设置各种管理器和适配器
 */
bool WtUftRunner::initEngine()
{
	// 获取引擎环境配置
	WTSVariant* cfg = _config->get("env");
	if (cfg == NULL)
		return false;

	WTSLogger::info("Trading enviroment initialzied with engine: UFT");
	// 使用配置和各管理器初始化引擎
	_uft_engine.init(cfg, &_bd_mgr, &_data_mgr, &_notifier);

	// 设置交易适配器管理器
	_uft_engine.set_adapter_mgr(&_traders);

	return true;
}


/**
 * @brief 初始化数据管理器
 * @return 初始化是否成功
 * @details 使用配置初始化数据管理器，并绑定引擎
 */
bool WtUftRunner::initDataMgr()
{
	// 获取数据管理器配置
	WTSVariant*cfg = _config->get("data");
	if (cfg == NULL)
		return false;

	// 使用配置和引擎初始化数据管理器
	_data_mgr.init(cfg, &_uft_engine);
	WTSLogger::info("Data manager initialized");

	return true;
}

/**
 * @brief 初始化行情解析器
 * @param cfgParser 行情解析器配置
 * @return 初始化是否成功
 * @details 根据配置创建并初始化所有激活的行情解析器，用于接收和处理市场数据
 */
bool WtUftRunner::initParsers(WTSVariant* cfgParser)
{
	// 检查配置是否有效
	if (cfgParser == NULL)
		return false;

	// 初始化计数器
	uint32_t count = 0;
	// 遍历所有解析器配置
	for (uint32_t idx = 0; idx < cfgParser->size(); idx++)
	{
		WTSVariant* cfgItem = cfgParser->get(idx);
		// 跳过非激活的解析器
		if(!cfgItem->getBoolean("active"))
			continue;

		// 获取解析器ID
		const char* id = cfgItem->getCString("id");
		// By Wesley @ 2021.12.14
		// 如果id为空，则生成自动id
		std::string realid = id;
		if (realid.empty())
		{
			// 使用递增的数字生成自动ID
			static uint32_t auto_parserid = 1000;
			realid = StrUtil::printf("auto_parser_%u", auto_parserid++);
		}

		// 创建并初始化解析器适配器
		ParserAdapterPtr adapter(new ParserAdapter);
		adapter->init(realid.c_str(), cfgItem, &_uft_engine, &_bd_mgr);
		// 将适配器添加到管理器中
		_parsers.addAdapter(realid.c_str(), adapter);

		count++;
	}

	// 输出已加载的解析器数量
	WTSLogger::info("{} parsers loaded", count);
	return true;
}

/**
 * @brief 初始化交易适配器
 * @param cfgTrader 交易适配器配置
 * @return 初始化是否成功
 * @details 根据配置创建并初始化所有激活的交易适配器，适配器用于连接实际交易接口
 */
bool WtUftRunner::initTraders(WTSVariant* cfgTrader)
{
	// 检查配置是否有效
	if (cfgTrader == NULL || cfgTrader->type() != WTSVariant::VT_Array)
		return false;
	
	// 初始化计数器
	uint32_t count = 0;
	// 遍历所有交易适配器配置
	for (uint32_t idx = 0; idx < cfgTrader->size(); idx++)
	{
		WTSVariant* cfgItem = cfgTrader->get(idx);
		// 跳过非激活的交易适配器
		if (!cfgItem->getBoolean("active"))
			continue;

		// 获取交易适配器ID
		const char* id = cfgItem->getCString("id");

		// 创建并初始化交易适配器
		TraderAdapterPtr adapter(new TraderAdapter());
		adapter->init(id, cfgItem, &_bd_mgr, &_act_policy);

		// 将适配器添加到管理器中
		_traders.addAdapter(id, adapter);

		count++;
	}

	// 输出已加载的交易适配器数量
	WTSLogger::info("{} traders loaded", count);

	return true;
}

/**
 * @brief 初始化事件通知器
 * @return 初始化是否成功
 * @details 使用配置初始化事件通知器，用于处理和分发系统事件
 */
bool WtUftRunner::initEvtNotifier()
{
	// 获取通知器配置
	WTSVariant* cfg = _config->get("notifier");
	// 检查配置是否有效
	if (cfg == NULL || cfg->type() != WTSVariant::VT_Object)
		return false;

	// 初始化事件通知器
	_notifier.init(cfg);

	return true;
}

/**
 * @brief 运行策略引擎
 * @param bAsync 是否以异步模式运行，默认为同步模式
 * @details 启动并运行策略引擎、行情解析器和交易适配器，并根据模式等待退出信号
 */
void WtUftRunner::run(bool bAsync /* = false */)
{
	try
	{
		// 启动UFT策略引擎
		_uft_engine.run();

		// 启动行情解析器
		_parsers.run();
		// 启动交易适配器
		_traders.run();

		// 启动共享监视（间隔为2秒）
		ShareManager::self().start_watching(2);

		// 如果是同步模式，等待退出信号
		while(!_to_exit)
		{
			// 休眠10毫秒，减少CPU占用
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}
	catch (...)
	{
		// 捕获并打印异常堆栈信息
		print_stack_trace([](const char* message) {
			WTSLogger::error(message);
		});
	}
}

/**
 * @brief 日志级别标签数组
 * @details 用于转换日志级别枚举至字符串表示
 */
const char* LOG_TAGS[] = {
	"all",      ///< 所有日志
	"debug",    ///< 调试级别
	"info",     ///< 信息级别
	"warn",     ///< 警告级别
	"error",    ///< 错误级别
	"fatal",    ///< 致命错误级别
	"none"      ///< 无日志
};

/**
 * @brief 处理日志追加回调
 * @param ll 日志级别
 * @param msg 日志消息
 * @details 实现ILogHandler接口中的日志处理方法，当前实现为空，可扩展实现自定义日志处理
 */
void WtUftRunner::handleLogAppend(WTSLogLevel ll, const char* msg)
{
	// 当前未实现任何日志处理逻辑，可根据需要扩展
}