/*!
 * @file WtExecRunner.cpp
 * @author wondertrader
 * @date 2022/05/01
 * 
 * @brief 执行监控系统运行器实现
 * @details 实现了执行监控系统的运行器，负责协调执行器、解析器和交易适配器的工作
 */

#include "WtExecRunner.h"

#include "../WtCore/WtHelper.h"
#include "../WtCore/WtDiffExecuter.h"
#include "../WtCore/WtDistExecuter.h"

#include "../WTSTools/WTSLogger.h"
#include "../WTSUtils/WTSCfgLoader.h"

#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Share/CodeHelper.hpp"
#include "../Share/ModuleHelper.hpp"
#include "../Share/TimeUtils.hpp"
#include "../WTSUtils/SignalHook.hpp"

#ifdef _MSC_VER
#include "../Common/mdump.h"
#include <boost/filesystem.hpp>
/**
 * @brief 获取模块名称
 * @details 获取当前模块的文件名，用于设置崩溃转储
 * @return 模块名称
 */
const char* getModuleName()
{
	static char MODULE_NAME[250] = { 0 };
	if (strlen(MODULE_NAME) == 0)
	{
		// 获取当前模块的完整路径
		GetModuleFileName(g_dllModule, MODULE_NAME, 250);
		// 从路径中提取文件名
		boost::filesystem::path p(MODULE_NAME);
		strcpy(MODULE_NAME, p.filename().string().c_str());
	}

	return MODULE_NAME;
}
#endif

/**
 * @brief 构造函数
 * @details 初始化执行监控系统运行器，安装信号处理钩子
 */
WtExecRunner::WtExecRunner()
{
	// 安装信号处理钩子，将信号错误输出到日志
	install_signal_hooks([](const char* message) {
		WTSLogger::error(message);
	});
}

/**
 * @brief 初始化运行器
 * @details 初始化日志系统和设置安装目录
 * @param logCfg 日志配置文件路径或内容，默认为"logcfgexec.json"
 * @param isFile 是否为文件路径，默认为true
 * @return 初始化是否成功
 */
bool WtExecRunner::init(const char* logCfg /* = "logcfgexec.json" */, bool isFile /* = true */)
{
#ifdef _MSC_VER
	// Windows平台下启用崩溃转储
	CMiniDumper::Enable(getModuleName(), true, WtHelper::getCWD().c_str());
#endif

	// 初始化日志系统
	if(isFile)
	{
		// 如果是文件路径，需要添加当前工作目录
		std::string path = WtHelper::getCWD() + logCfg;
		WTSLogger::init(path.c_str(), true);
	}
	else
	{
		// 如果是配置内容，直接使用
		WTSLogger::init(logCfg, false);
	}
	
	// 设置安装目录
	WtHelper::setInstDir(getBinDir());
	return true;
}

/**
 * @brief 加载配置
 * @details 加载执行监控系统的配置，包括基础数据、行情通道、交易通道和执行器配置
 * @param cfgFile 配置文件路径或内容
 * @param isFile 是否为文件路径，默认为true
 * @return 配置加载是否成功
 */
bool WtExecRunner::config(const char* cfgFile, bool isFile /* = true */)
{
	// 根据参数决定从文件加载还是从内容加载配置
	_config = isFile ? WTSCfgLoader::load_from_file(cfgFile) : WTSCfgLoader::load_from_content(cfgFile, false);
	if(_config == NULL)
	{
		WTSLogger::log_raw(LL_ERROR, "Loading config file failed");
		return false;
	}

	//基础数据文件
	WTSVariant* cfgBF = _config->get("basefiles");
	if (cfgBF->get("session"))
	{
		_bd_mgr.loadSessions(cfgBF->getCString("session"));
		WTSLogger::info("Trading sessions loaded");
	}

	WTSVariant* cfgItem = cfgBF->get("commodity");
	if (cfgItem)
	{
		if (cfgItem->type() == WTSVariant::VT_String)
		{
			_bd_mgr.loadCommodities(cfgItem->asCString());
		}
		else if (cfgItem->type() == WTSVariant::VT_Array)
		{
			for (uint32_t i = 0; i < cfgItem->size(); i++)
			{
				_bd_mgr.loadCommodities(cfgItem->get(i)->asCString());
			}
		}
	}

	cfgItem = cfgBF->get("contract");
	if (cfgItem)
	{
		if (cfgItem->type() == WTSVariant::VT_String)
		{
			_bd_mgr.loadContracts(cfgItem->asCString());
		}
		else if (cfgItem->type() == WTSVariant::VT_Array)
		{
			for (uint32_t i = 0; i < cfgItem->size(); i++)
			{
				_bd_mgr.loadContracts(cfgItem->get(i)->asCString());
			}
		}
	}

	if (cfgBF->get("holiday"))
	{
		_bd_mgr.loadHolidays(cfgBF->getCString("holiday"));
		WTSLogger::info("Holidays loaded");
	}


	//初始化数据管理
	initDataMgr();

	//初始化开平策略
	if (!initActionPolicy())
		return false;

	//初始化行情通道
	const char* cfgParser = _config->getCString("parsers");
	if (StdFile::exists(cfgParser))
	{
		WTSLogger::info("Reading parser config from {}...", cfgParser);
		WTSVariant* var = WTSCfgLoader::load_from_file(cfgParser);
		if (var)
		{
			if (!initParsers(var))
				WTSLogger::error("Loading parsers failed");
			var->release();
		}
		else
		{
			WTSLogger::error("Loading parser config {} failed", cfgParser);
		}
	}

	//初始化交易通道
	const char* cfgTraders = _config->getCString("traders");
	if (StdFile::exists(cfgTraders))
	{
		WTSLogger::info("Reading trader config from {}...", cfgTraders);
		WTSVariant* var = WTSCfgLoader::load_from_file(cfgTraders);
		if (var)
		{
			if (!initTraders(var))
				WTSLogger::error("Loading traders failed");
			var->release();
		}
		else
		{
			WTSLogger::error("Loading trader config {} failed", cfgTraders);
		}
	}

	const char* cfgExecuters = _config->getCString("executers");
	if (StdFile::exists(cfgExecuters))
	{
		WTSLogger::info("Reading executer config from {}...", cfgExecuters);
		WTSVariant* var = WTSCfgLoader::load_from_file(cfgExecuters);
		if (var)
		{
			if (!initExecuters(var))
				WTSLogger::error("Loading executers failed");
			var->release();
		}
		else
		{
			WTSLogger::error("Loading executer config {} failed", cfgExecuters);
		}
	}

	return true;
}

/**
 * @brief 运行执行监控系统
 * @details 启动所有的解析器和交易器，开始执行监控，并处理异常情况
 */
void WtExecRunner::run()
{
	try
	{
		// 启动行情解析器
		_parsers.run();
		// 启动交易通道
		_traders.run();
	}
	catch (...)
	{
		// 捕获并记录异常堆栈信息
		print_stack_trace([](const char* message) {
			WTSLogger::error(message);
		});
	}
}

/**
 * @brief 初始化解析器
 * @details 根据配置初始化行情解析器，包括创建和配置解析器适配器
 * @param cfgParser 解析器配置
 * @return 初始化是否成功
 */
bool WtExecRunner::initParsers(WTSVariant* cfgParser)
{
	// 获取解析器配置数组
	WTSVariant* cfg = cfgParser->get("parsers");
	if (cfg == NULL)
		return false;

	uint32_t count = 0;
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
			static uint32_t auto_parserid = 1000;
			realid = StrUtil::printf("auto_parser_%u", auto_parserid++);
		}

		ParserAdapterPtr adapter(new ParserAdapter);
		adapter->init(realid.c_str(), cfgItem, this, &_bd_mgr, &_hot_mgr);
		_parsers.addAdapter(realid.c_str(), adapter);

		count++;
	}

	WTSLogger::info("{} parsers loaded", count);

	return true;
}

/**
 * @brief 初始化执行器
 * @details 根据配置初始化交易执行器，包括加载执行器工厂和创建不同类型的执行器
 * @param cfgExecuter 执行器配置
 * @return 初始化是否成功
 */
bool WtExecRunner::initExecuters(WTSVariant* cfgExecuter)
{
	// 获取执行器配置数组
	WTSVariant* cfg = cfgExecuter->get("executers");
	if (cfg == NULL || cfg->type() != WTSVariant::VT_Array)
		return false;

	// 先加载自带的执行器工厂
	std::string path = WtHelper::getInstDir() + "executer//";
	_exe_factory.loadFactories(path.c_str());

	uint32_t count = 0;
	for (uint32_t idx = 0; idx < cfg->size(); idx++)
	{
		WTSVariant* cfgItem = cfg->get(idx);
		if (!cfgItem->getBoolean("active"))
			continue;

		const char* id = cfgItem->getCString("id");
		std::string name = cfgItem->getCString("name");	//local,diff,dist
		if (name.empty())
			name = "local";

		if (name == "local")
		{
			WtLocalExecuter* executer = new WtLocalExecuter(&_exe_factory, id, &_data_mgr);
			if (!executer->init(cfgItem))
				return false;

			const char* tid = cfgItem->getCString("trader");
			if (strlen(tid) == 0)
			{
				WTSLogger::error("No Trader configured for Executer {}", id);
			}
			else
			{
				TraderAdapterPtr trader = _traders.getAdapter(tid);
				if (trader)
				{
					executer->setTrader(trader.get());
					trader->addSink(executer);
				}
				else
				{
					WTSLogger::error("Trader {} not exists, cannot configured for executer %s", tid, id);
				}
			}

			executer->setStub(this);
			_exe_mgr.add_executer(ExecCmdPtr(executer));
		}
		else if (name == "diff")
		{
			WtDiffExecuter* executer = new WtDiffExecuter(&_exe_factory, id, &_data_mgr, &_bd_mgr);
			if (!executer->init(cfgItem))
				return false;

			const char* tid = cfgItem->getCString("trader");
			if (strlen(tid) == 0)
			{
				WTSLogger::error("No Trader configured for Executer {}", id);
			}
			else
			{
				TraderAdapterPtr trader = _traders.getAdapter(tid);
				if (trader)
				{
					executer->setTrader(trader.get());
					trader->addSink(executer);
				}
				else
				{
					WTSLogger::error("Trader {} not exists, cannot configured for executer %s", tid, id);
				}
			}

			executer->setStub(this);
			_exe_mgr.add_executer(ExecCmdPtr(executer));
		}
		else
		{
			WtDistExecuter* executer = new WtDistExecuter(id);
			if (!executer->init(cfgItem))
				return false;

			executer->setStub(this);
			_exe_mgr.add_executer(ExecCmdPtr(executer));
		}
		count++;
	}

	WTSLogger::info("{} executers loaded", count);

	return true;
}

/**
 * @brief 初始化交易器
 * @details 根据配置初始化交易适配器，包括创建和配置交易适配器
 * @param cfgTrader 交易器配置
 * @return 初始化是否成功
 */
bool WtExecRunner::initTraders(WTSVariant* cfgTrader)
{
	// 获取交易器配置数组
	WTSVariant* cfg = cfgTrader->get("traders");
	if (cfg == NULL || cfg->type() != WTSVariant::VT_Array)
		return false;

	uint32_t count = 0;
	for (uint32_t idx = 0; idx < cfg->size(); idx++)
	{
		WTSVariant* cfgItem = cfg->get(idx);
		// 跳过未激活的交易器
		if (!cfgItem->getBoolean("active"))
			continue;

		const char* id = cfgItem->getCString("id");

		// 创建并初始化交易适配器
		TraderAdapterPtr adapter(new TraderAdapter);
		adapter->init(id, cfgItem, &_bd_mgr, &_act_policy);

		// 添加到交易适配器管理器
		_traders.addAdapter(id, adapter);
		count++;
	}

	WTSLogger::info("{} traders loaded", count);

	return true;
}

/**
 * @brief 初始化数据管理器
 * @details 初始化简单数据管理器，用于管理行情数据
 * @return 初始化是否成功
 */
bool WtExecRunner::initDataMgr()
{
	// 获取数据配置
	WTSVariant* cfg = _config->get("data");
	if (cfg == NULL)
		return false;

	// 初始化数据管理器
	_data_mgr.init(cfg, this);

	WTSLogger::info("Data Manager initialized");
	return true;
}

/**
 * @brief 添加执行器工厂
 * @details 从指定文件夹加载执行器工厂
 * @param folder 工厂文件夹路径
 * @return 加载是否成功
 */
bool WtExecRunner::addExeFactories(const char* folder)
{
	return _exe_factory.loadFactories(folder);
}

/**
 * @brief 获取交易时段信息
 * @details 根据标准化合约代码或会话ID获取交易时段信息
 * @param sid 标准化合约代码或会话ID
 * @param isCode 是否为标准化合约代码，默认为true
 * @return 交易时段信息指针
 */
WTSSessionInfo* WtExecRunner::get_session_info(const char* sid, bool isCode /* = true */)
{
	// 如果不是合约代码，直接从基础数据管理器获取会话信息
	if (!isCode)
		return _bd_mgr.getSession(sid);

	// 从标准化合约代码中提取信息
	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(sid, NULL);
	// 获取品种信息
	WTSCommodityInfo* cInfo = _bd_mgr.getCommodity(codeInfo._exchg, codeInfo._product);
	if (cInfo == NULL)
		return NULL;

	// 从品种信息中获取会话信息
	return cInfo->getSessionInfo();
}

/**
 * @brief 处理实时主推行情
 * @details 实现IParserStub接口的方法，处理实时行情数据
 * @param quote 最新的tick数据
 */
void WtExecRunner::handle_push_quote(WTSTickData* quote)
{
	// 检查行情数据是否有效
	if (quote == NULL)
		return;

	// 提取行情数据中的时间信息
	uint32_t uDate = quote->actiondate();
	uint32_t uTime = quote->actiontime();
	uint32_t curMin = uTime / 100000;
	uint32_t curSec = uTime % 100000;
	// 设置当前时间和交易日
	WtHelper::setTime(uDate, curMin, curSec);
	WtHelper::setTDate(quote->tradingdate());

	// 将行情数据传递给数据管理器
	_data_mgr.handle_push_quote(quote->code(), quote);

	// 将行情数据传递给执行管理器
	_exe_mgr.handle_tick(quote->code(), quote);
}

/**
 * @brief 释放资源
 * @details 释放执行监控系统的资源，停止日志系统
 */
void WtExecRunner::release()
{
	// 停止日志系统
	WTSLogger::stop();
}

/**
 * @brief 设置目标仓位
 * @details 设置单个合约的目标仓位，添加到缓存中
 * @param stdCode 标准化合约代码
 * @param targetPos 目标仓位
 */
void WtExecRunner::setPosition(const char* stdCode, double targetPos)
{
	// 将目标仓位添加到缓存中
	_positions[stdCode] = targetPos;
}

/**
 * @brief 提交目标仓位
 * @details 提交缓存中的所有目标仓位，触发实际交易执行
 */
void WtExecRunner::commitPositions()
{
	// 将缓存中的目标仓位设置到执行管理器
	_exe_mgr.set_positions(_positions);
	// 清空缓存
	_positions.clear();
}

/**
 * @brief 初始化行为策略
 * @details 初始化开平仓行为策略，用于管理交易行为
 * @return 初始化是否成功
 */
bool WtExecRunner::initActionPolicy()
{
	// 获取行为策略配置文件路径
	const char* action_file = _config->getCString("bspolicy");
	if (strlen(action_file) <= 0)
		return false;

	// 初始化行为策略管理器
	bool ret = _act_policy.init(action_file);
	WTSLogger::info("Action policies initialized");
	return ret;
}

/**
 * @brief 获取当前实时时间
 * @details 实现IExecuterStub接口的方法，返回当前系统的实时时间
 * @return 当前时间，以毫秒为单位
 */
uint64_t WtExecRunner::get_real_time()
{
	// 根据数据管理器中的日期和时间信息生成完整的时间戳
	return TimeUtils::makeTime(_data_mgr.get_date(), _data_mgr.get_raw_time() * 100000 + _data_mgr.get_secs());
}

/**
 * @brief 获取品种信息
 * @details 实现IExecuterStub接口的方法，根据标准化合约代码获取品种信息
 * @param stdCode 标准化合约代码
 * @return 品种信息指针
 */
WTSCommodityInfo* WtExecRunner::get_comm_info(const char* stdCode)
{
	// 从标准化合约代码中提取信息
	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(stdCode, NULL);
	// 从基础数据管理器中获取品种信息
	return _bd_mgr.getCommodity(codeInfo._exchg, codeInfo._product);
}

/**
 * @brief 获取交易时段信息
 * @details 实现IExecuterStub接口的方法，根据标准化合约代码获取交易时段信息
 * @param stdCode 标准化合约代码
 * @return 交易时段信息指针
 */
WTSSessionInfo* WtExecRunner::get_sess_info(const char* stdCode)
{
	// 从标准化合约代码中提取信息
	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(stdCode, NULL);
	// 获取品种信息
	WTSCommodityInfo* cInfo = _bd_mgr.getCommodity(codeInfo._exchg, codeInfo._product);
	if (cInfo == NULL)
		return NULL;

	// 从品种信息中获取会话信息
	return cInfo->getSessionInfo();
}

/**
 * @brief 获取交易日
 * @details 实现IExecuterStub接口的方法，返回当前交易日
 * @return 交易日，格式YYYYMMDD
 */
uint32_t WtExecRunner::get_trading_day()
{
	// 从数据管理器获取当前交易日
	return _data_mgr.get_trading_day();
}