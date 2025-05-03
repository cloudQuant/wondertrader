/*!
 * \file WtDtRunner.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 数据服务运行器实现
 * 
 * \details 本文件实现了WtDtRunner类，该类是数据服务模块的核心组件，
 * 负责数据的加载、管理、查询和订阅功能。
 * 它封装了底层的数据存储和解析器管理，为上层应用提供统一的数据访问接口。
 */
#include "WtDtRunner.h"

#include "../WtDtCore/WtHelper.h"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSContractInfo.hpp"

#include "../WTSUtils/SignalHook.hpp"
#include "../WTSUtils/WTSCfgLoader.h"
#include "../WTSTools/WTSLogger.h"

#include "../Share/StrUtil.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/CodeHelper.hpp"

USING_NS_WTP;

/**
 * @brief 构造函数
 * 
 * @details 初始化数据服务运行器对象，设置初始状态并安装信号处理钩子。
 * 初始化时将数据存储对象指针设置为空，初始化标志设置为否。
 * 安装的信号处理钩子用于捕获系统信号并记录错误日志。
 */
WtDtRunner::WtDtRunner()
	: _data_store(NULL)
	, _is_inited(false)
{
	install_signal_hooks([](const char* message) {
		WTSLogger::error(message);
	});
}


/**
 * @brief 析构函数
 * 
 * @details 清理数据服务运行器对象的资源。
 * 目前该析构函数为空实现，因为对象的成员变量会自动析构。
 */
WtDtRunner::~WtDtRunner()
{
}
#ifdef _MSC_VER
#include "../Common/mdump.h"
extern const char* getModuleName();
#endif

/**
 * @brief 初始化数据服务运行器
 * @param cfgFile 配置文件路径或配置内容字符串
 * @param isFile 是否为文件路径，如果为true则cfgFile为文件路径，否则为配置内容字符串
 * @param modDir 模块目录，用于加载解析器模块
 * @param logCfg 日志配置文件路径
 * @param cbTick Tick数据回调函数，用于接收实时Tick数据
 * @param cbBar K线数据回调函数，用于接收实时K线数据
 * 
 * @details 该函数完成数据服务运行器的全面初始化工作，包括：
 * 1. 初始化日志系统
 * 2. 加载配置文件
 * 3. 加载交易时段、商品、合约、假日等基础数据
 * 4. 加载主力合约和次主力合约规则
 * 5. 初始化数据管理器
 * 6. 加载并初始化解析器
 * 7. 启动数据服务
 * 
 * 该函数是使用WtDtRunner的第一步，必须在调用其他方法前调用。
 */
void WtDtRunner::initialize(const char* cfgFile, bool isFile /* = true */, const char* modDir /* = "" */, const char* logCfg /* = "logcfg.yaml" */, 
			FuncOnTickCallback cbTick /* = NULL */, FuncOnBarCallback cbBar /* = NULL */)
{
	if(_is_inited)
	{
		WTSLogger::error("WtDtServo has already been initialized");
		return;
	}

	_cb_tick = cbTick;
	_cb_bar = cbBar;

	WTSLogger::init(logCfg);
	WtHelper::set_module_dir(modDir);

	WTSVariant* config = isFile ? WTSCfgLoader::load_from_file(cfgFile) : WTSCfgLoader::load_from_content(cfgFile, false);
	if(config == NULL)
	{
		WTSLogger::error("Loading config failed");
		WTSLogger::log_raw(LL_INFO, cfgFile);
		return;
	}

	if(!config->getBoolean("disable_dump"))
	{
#ifdef _MSC_VER
		CMiniDumper::Enable(getModuleName(), true, WtHelper::get_cwd());
#endif
	}
	//基础数据文件
	WTSVariant* cfgBF = config->get("basefiles");
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

	if (cfgBF->get("hot"))
	{
		_hot_mgr.loadHots(cfgBF->getCString("hot"));
		WTSLogger::info("Hot rules loaded");
	}

	if (cfgBF->get("second"))
	{
		_hot_mgr.loadSeconds(cfgBF->getCString("second"));
		WTSLogger::info("Second rules loaded");
	}

	WTSArray* ayContracts = _bd_mgr.getContracts();
	for (auto it = ayContracts->begin(); it != ayContracts->end(); it++)
	{
		WTSContractInfo* cInfo = (WTSContractInfo*)(*it);
		bool isHot = _hot_mgr.isHot(cInfo->getExchg(), cInfo->getCode());
		bool isSecond = _hot_mgr.isSecond(cInfo->getExchg(), cInfo->getCode());

		std::string hotCode = cInfo->getFullPid();
		if (isHot)
			hotCode += ".HOT";
		else if (isSecond)
			hotCode += ".2ND";
		else
			hotCode = "";

		cInfo->setHotFlag(isHot ? 1 : (isSecond ? 2 : 0), hotCode.c_str());
	}
	ayContracts->release();

	initDataMgr(config->get("data"));

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
	else
		WTSLogger::log_raw(LL_WARN, "No parsers config, skipped loading parsers");

	config->release();

	start();

	_is_inited = true;
}

/**
 * @brief 初始化数据管理器
 * @param config 数据管理器配置对象指针
 * 
 * @details 该函数根据配置初始化数据管理器，包括数据存储和数据缓存等组件。
 * 如果配置对象为空，则直接返回不进行初始化。
 * 初始化完成后会记录日志信息。
 * 该函数通常在initialize函数中被调用。
 */
void WtDtRunner::initDataMgr(WTSVariant* config)
{
	if (config == NULL)
		return;

	_data_mgr.init(config, this);

	WTSLogger::info("Data manager initialized");
}

/**
 * @brief 根据时间范围获取K线数据
 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
 * @param period K线周期，如"m1"/"d1"
 * @param beginTime 开始时间，格式为YYYYMMDDHHmmss
 * @param endTime 结束时间，格式为YYYYMMDDHHmmss，如果为0则表示当前时间
 * @return WTSKlineSlice* K线数据切片指针，使用后需要调用release方法释放
 * 
 * @details 该函数获取指定时间范围内的K线数据。
 * 首先检查运行器是否已经初始化，如果没有则返回空指针。
 * 然后解析周期字符串，将其转换为内部的周期类型和倍数。
 * 最后调用数据管理器的方法获取数据并返回。
 * 返回的数据切片需要用户调用release方法释放内存。
 */
WTSKlineSlice* WtDtRunner::get_bars_by_range(const char* stdCode, const char* period, uint64_t beginTime, uint64_t endTime /* = 0 */)
{
	if(!_is_inited)
	{
		WTSLogger::error("WtDtServo not initialized");
		return NULL;
	}

	thread_local static char basePeriod[2] = { 0 };
	basePeriod[0] = period[0];
	uint32_t times = 1;
	if (strlen(period) > 1)
		times = strtoul(period + 1, NULL, 10);

	WTSKlinePeriod kp;
	uint32_t realTimes = times;
	if (basePeriod[0] == 'm')
	{
		if (times % 5 == 0)
		{
			kp = KP_Minute5;
			realTimes /= 5;
		}
		else
		{
			kp = KP_Minute1;
		}
	}
	else
		kp = KP_DAY;

	if (endTime == 0)
	{
		uint32_t curDate = TimeUtils::getCurDate();
		endTime = (uint64_t)curDate * 10000 + 2359;
	}

	return _data_mgr.get_kline_slice_by_range(stdCode, kp, realTimes, beginTime, endTime);
}

/**
 * @brief 根据日期获取K线数据
 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
 * @param period K线周期，如"m1"/"d1"
 * @param uDate 交易日期，格式为YYYYMMDD，如果为0则表示当前交易日
 * @return WTSKlineSlice* K线数据切片指针，使用后需要调用release方法释放
 * 
 * @details 该函数获取指定交易日的K线数据。
 * 首先检查运行器是否已经初始化，如果没有则返回空指针。
 * 如果交易日期为0，则获取当前交易日的数据。
 * 然后解析周期字符串，将其转换为内部的周期类型和倍数。
 * 注意该函数只支持分钟周期。
 * 最后调用数据管理器的方法获取数据并返回。
 * 返回的数据切片需要用户调用release方法释放内存。
 */
WTSKlineSlice* WtDtRunner::get_bars_by_date(const char* stdCode, const char* period, uint32_t uDate /* = 0 */)
{
	if (!_is_inited)
	{
		WTSLogger::error("WtDtServo not initialized");
		return NULL;
	}

	thread_local static char basePeriod[2] = { 0 };
	basePeriod[0] = period[0];
	uint32_t times = 1;
	if (strlen(period) > 1)
		times = strtoul(period + 1, NULL, 10);

	WTSKlinePeriod kp;
	uint32_t realTimes = times;
	if (basePeriod[0] == 'm')
	{
		if (times % 5 == 0)
		{
			kp = KP_Minute5;
			realTimes /= 5;
		}
		else
		{
			kp = KP_Minute1;
		}
	}
	else
	{
		WTSLogger::log_raw(LL_ERROR, "get_bars_by_date only supports minute period");
		return NULL;
	}

	if (uDate == 0)
	{
		uDate = TimeUtils::getCurDate();
	}

	return _data_mgr.get_kline_slice_by_date(stdCode, kp, realTimes, uDate);
}

/**
 * @brief 根据时间范围获取Tick数据
 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
 * @param beginTime 开始时间，格式为YYYYMMDDHHmmss
 * @param endTime 结束时间，格式为YYYYMMDDHHmmss，如果为0则表示当前时间
 * @return WTSTickSlice* Tick数据切片指针，使用后需要调用release方法释放
 * 
 * @details 该函数获取指定时间范围内的Tick数据。
 * 首先检查运行器是否已经初始化，如果没有则返回空指针。
 * 如果结束时间为0，则使用当前日期的最后时间点作为结束时间。
 * 最后调用数据管理器的get_tick_slices_by_range方法获取数据并返回。
 * 返回的数据切片需要用户调用release方法释放内存。
 */
WTSTickSlice* WtDtRunner::get_ticks_by_range(const char* stdCode, uint64_t beginTime, uint64_t endTime /* = 0 */)
{
	if (!_is_inited)
	{
		WTSLogger::error("WtDtServo not initialized");
		return NULL;
	}

	if(endTime == 0)
	{
		uint32_t curDate = TimeUtils::getCurDate();
		endTime = (uint64_t)curDate * 10000 + 2359;
	}
	return _data_mgr.get_tick_slices_by_range(stdCode, beginTime, endTime);
}

/**
 * @brief 根据日期获取Tick数据
 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
 * @param uDate 交易日期，格式为YYYYMMDD，如果为0则表示当前交易日
 * @return WTSTickSlice* Tick数据切片指针，使用后需要调用release方法释放
 * 
 * @details 该函数获取指定交易日的Tick数据。
 * 首先检查运行器是否已经初始化，如果没有则返回空指针。
 * 然后直接调用数据管理器的get_tick_slice_by_date方法获取数据并返回。
 * 返回的数据切片需要用户调用release方法释放内存。
 */
WTSTickSlice* WtDtRunner::get_ticks_by_date(const char* stdCode, uint32_t uDate /* = 0 */)
{
	if (!_is_inited)
	{
		WTSLogger::error("WtDtServo not initialized");
		return NULL;
	}

	return _data_mgr.get_tick_slice_by_date(stdCode, uDate);
}

/**
 * @brief 根据数量获取K线数据
 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
 * @param period K线周期，如"m1"/"d1"
 * @param count 要获取的K线数量
 * @param endTime 结束时间，格式为YYYYMMDDHHmmss，如果为0则表示当前时间
 * @return WTSKlineSlice* K线数据切片指针，使用后需要调用release方法释放
 * 
 * @details 该函数获取指定数量的K线数据，从结束时间往前计算。
 * 首先检查运行器是否已经初始化，如果没有则返回空指针。
 * 然后解析周期字符串，将其转换为内部的周期类型和倍数。
 * 如果结束时间为0，则使用当前日期的最后时间点作为结束时间。
 * 最后调用数据管理器的get_kline_slice_by_count方法获取数据并返回。
 * 返回的数据切片需要用户调用release方法释放内存。
 */
WTSKlineSlice* WtDtRunner::get_bars_by_count(const char* stdCode, const char* period, uint32_t count, uint64_t endTime /* = 0 */)
{
	if (!_is_inited)
	{
		WTSLogger::error("WtDtServo not initialized");
		return NULL;
	}

	thread_local static char basePeriod[2] = { 0 };
	basePeriod[0] = period[0];
	uint32_t times = 1;
	if (strlen(period) > 1)
		times = strtoul(period + 1, NULL, 10);

	WTSKlinePeriod kp;
	uint32_t realTimes = times;
	if (basePeriod[0] == 'm')
	{
		if (times % 5 == 0)
		{
			kp = KP_Minute5;
			realTimes /= 5;
		}
		else
		{
			kp = KP_Minute1;
		}
	}
	else
		kp = KP_DAY;

	if (endTime == 0)
	{
		uint32_t curDate = TimeUtils::getCurDate();
		endTime = (uint64_t)curDate * 10000 + 2359;
	}

	return _data_mgr.get_kline_slice_by_count(stdCode, kp, realTimes, count, endTime);
}

/**
 * @brief 根据数量获取Tick数据
 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
 * @param count 要获取的Tick数量
 * @param endTime 结束时间，格式为YYYYMMDDHHmmss，如果为0则表示当前时间
 * @return WTSTickSlice* Tick数据切片指针，使用后需要调用release方法释放
 * 
 * @details 该函数获取指定数量的Tick数据，从结束时间往前计算。
 * 首先检查运行器是否已经初始化，如果没有则返回空指针。
 * 如果结束时间为0，则使用当前日期的最后时间点作为结束时间。
 * 最后调用数据管理器的get_tick_slice_by_count方法获取数据并返回。
 * 返回的数据切片需要用户调用release方法释放内存。
 */
WTSTickSlice* WtDtRunner::get_ticks_by_count(const char* stdCode, uint32_t count, uint64_t endTime /* = 0 */)
{
	if (!_is_inited)
	{
		WTSLogger::error("WtDtServo not initialized");
		return NULL;
	}

	if (endTime == 0)
	{
		uint32_t curDate = TimeUtils::getCurDate();
		endTime = (uint64_t)curDate * 10000 + 2359;
	}
	return _data_mgr.get_tick_slice_by_count(stdCode, count, endTime);
}

/**
 * @brief 根据日期获取秒线K线数据
 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
 * @param secs 秒线周期，单位为秒
 * @param uDate 交易日期，格式为YYYYMMDD，如果为0则表示当前交易日
 * @return WTSKlineSlice* K线数据切片指针，使用后需要调用release方法释放
 * 
 * @details 该函数获取指定交易日的秒线K线数据。
 * 首先检查运行器是否已经初始化，如果没有则返回空指针。
 * 然后直接调用数据管理器的get_skline_slice_by_date方法获取数据并返回。
 * 返回的数据切片需要用户调用release方法释放内存。
 */
WTSKlineSlice* WtDtRunner::get_sbars_by_date(const char* stdCode, uint32_t secs, uint32_t uDate /* = 0 */)
{
	if (!_is_inited)
	{
		WTSLogger::error("WtDtServo not initialized");
		return NULL;
	}

	return _data_mgr.get_skline_slice_by_date(stdCode, secs, uDate);
}

/**
 * @brief 初始化数据解析器
 * @param cfg 解析器配置数组
 * 
 * @details 该函数根据配置初始化所有数据解析器。
 * 首先遍历所有解析器配置项，对于每一个激活的解析器：
 * 1. 获取解析器ID，如果ID为空则自动生成一个唯一ID
 * 2. 创建解析器适配器对象
 * 3. 初始化适配器
 * 4. 将适配器添加到解析器管理器中
 * 
 * 最后记录已加载的解析器数量。
 * 该函数通常在initialize函数中被调用。
 */
void WtDtRunner::initParsers(WTSVariant* cfg)
{
	for (uint32_t idx = 0; idx < cfg->size(); idx++)
	{
		WTSVariant* cfgItem = cfg->get(idx);
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

		ParserAdapterPtr adapter(new ParserAdapter(&_bd_mgr, this));
		adapter->init(realid.c_str(), cfgItem);
		_parsers.addAdapter(realid.c_str(), adapter);
	}

	WTSLogger::info("{} market data parsers loaded in total", _parsers.size());
}

/**
 * @brief 启动数据服务
 * 
 * @details 该函数启动所有数据解析器，开始接收并处理实时行情数据。
 * 它会调用解析器管理器的run方法，使所有解析器开始工作。
 * 该函数应在initialize函数之后调用，以确保所有解析器已经正确初始化。
 */
void WtDtRunner::start()
{
	_parsers.run();
}

/**
 * @brief 处理Tick数据
 * @param curTick 当前Tick数据对象指针
 * 
 * @details 该函数处理接收到的Tick数据，包括：
 * 1. 检查合约信息是否存在
 * 2. 将原始合约代码转换为标准化合约代码
 * 3. 触发Tick数据回调
 * 4. 如果是主力合约，还会生成主力合约的Tick数据并触发相应回调
 * 
 * 该函数通常由解析器在接收到新的Tick数据时调用。
 */
void WtDtRunner::proc_tick(WTSTickData* curTick)
{
	WTSContractInfo* cInfo = curTick->getContractInfo();
	if (cInfo == NULL)
	{
		cInfo = _bd_mgr.getContract(curTick->code(), curTick->exchg());
		curTick->setContractInfo(cInfo);
	}

	if (cInfo == NULL)
		return;

	WTSCommodityInfo* commInfo = cInfo->getCommInfo();
	WTSSessionInfo* sInfo = commInfo->getSessionInfo();

	uint32_t hotflag = 0;

	std::string stdCode;
	if (commInfo->getCategoty() == CC_FutOption)
	{
		stdCode = CodeHelper::rawFutOptCodeToStdCode(cInfo->getCode(), cInfo->getExchg());
	}
	else if (CodeHelper::isMonthlyCode(curTick->code()))
	{
		//如果是分月合约，则进行主力和次主力的判断
		stdCode = CodeHelper::rawMonthCodeToStdCode(cInfo->getCode(), cInfo->getExchg());
	}
	else
	{
		stdCode = CodeHelper::rawFlatCodeToStdCode(cInfo->getCode(), cInfo->getExchg(), cInfo->getProduct());
	}
	curTick->setCode(stdCode.c_str());

	trigger_tick(stdCode.c_str(), curTick);

	if (!cInfo->isFlat())
	{
		const char* hotCode = cInfo->getHotCode();
		WTSTickData* hotTick = WTSTickData::create(curTick->getTickStruct());
		hotTick->setCode(hotCode);
		hotTick->setContractInfo(curTick->getContractInfo());

		trigger_tick(hotCode, hotTick);

		hotTick->release();
	}
	//else if (hotflag == 2)
	//{
	//	std::string scndCode = CodeHelper::stdCodeToStd2ndCode(stdCode.c_str());
	//	WTSTickData* scndTick = WTSTickData::create(curTick->getTickStruct());
	//	scndTick->setCode(scndCode.c_str());
	//	scndTick->setContractInfo(curTick->getContractInfo());

	//	trigger_tick(scndCode.c_str(), scndTick);

	//	scndTick->release();
	//}
}

/**
 * @brief 触发Tick数据处理
 * @param stdCode 标准化合约代码
 * @param curTick 当前Tick数据对象指针
 * 
 * @details 该函数将Tick数据分发给外部订阅者和内部订阅者。
 * 对于外部订阅者，通过回调函数将Tick数据传递出去。
 * 对于内部订阅者，调用数据管理器的update_bars方法更新K线数据。
 * 这里还处理了前复权和后复权的情况，根据复权因子对价格等数据进行调整。
 * 
 * 该函数由proc_tick函数调用。
 */
void WtDtRunner::trigger_tick(const char* stdCode, WTSTickData* curTick)
{
	if (_cb_tick != NULL)
	{
		StdUniqueLock lock(_mtx_subs);
		auto sit = _tick_sub_map.find(stdCode);
		if (sit != _tick_sub_map.end())
		{
			SubFlags flags = sit->second;
			for (uint32_t flag : flags)
			{
				if (flag == 0)
				{
					_cb_tick(stdCode, &curTick->getTickStruct());
				}
				else
				{
					std::string wCode = fmtutil::format("{}{}", stdCode, (flag == 1) ? SUFFIX_QFQ : SUFFIX_HFQ);
					if (flag == 1)
					{
						_cb_tick(wCode.c_str(), &curTick->getTickStruct());
					}
					else //(flag == 2)
					{
						WTSTickData* newTick = WTSTickData::create(curTick->getTickStruct());
						WTSTickStruct& newTS = newTick->getTickStruct();
						newTick->setContractInfo(curTick->getContractInfo());

						//这里做一个复权因子的处理
						double factor = _data_mgr.get_exright_factor(stdCode, curTick->getContractInfo()->getCommInfo());
						newTS.open *= factor;
						newTS.high *= factor;
						newTS.low *= factor;
						newTS.price *= factor;

						newTS.settle_price *= factor;

						newTS.pre_close *= factor;
						newTS.pre_settle *= factor;

						_cb_tick(wCode.c_str(), &newTS);
						newTick->release();
					}
				}
			}

		}
	}

	{
		StdUniqueLock lock(_mtx_innersubs);
		auto sit = _tick_innersub_map.find(stdCode);
		if (sit == _tick_innersub_map.end())
			return;

		SubFlags flags = sit->second;
		for (uint32_t flag : flags)
		{
			if (flag == 0)
			{
				_data_mgr.update_bars(stdCode, curTick);
			}
			else
			{
				std::string wCode = fmtutil::format("{}{}", stdCode, (flag == 1) ? SUFFIX_QFQ : SUFFIX_HFQ);
				curTick->setCode(wCode.c_str());
				if (flag == 1)
				{
					_data_mgr.update_bars(wCode.c_str(), curTick);
				}
				else //(flag == 2)
				{
					WTSTickData* newTick = WTSTickData::create(curTick->getTickStruct());
					WTSTickStruct& newTS = newTick->getTickStruct();
					newTick->setContractInfo(curTick->getContractInfo());

					//这里做一个复权因子的处理
					double factor = _data_mgr.get_exright_factor(stdCode, curTick->getContractInfo()->getCommInfo());
					newTS.open *= factor;
					newTS.high *= factor;
					newTS.low *= factor;
					newTS.price *= factor;

					newTS.settle_price *= factor;

					newTS.pre_close *= factor;
					newTS.pre_settle *= factor;

					_data_mgr.update_bars(wCode.c_str(), newTick);
					newTick->release();
				}
			}
		}
	}
}

/**
 * @brief 订阅Tick数据
 * @param codes 合约代码，可以是单个代码或用逗号分隔的多个代码
 * @param bReplace 是否替换现有订阅，如果为true则清除所有现有订阅
 * @param bInner 是否为内部订阅，内部订阅用于生成K线数据
 * 
 * @details 该函数实现对Tick数据的订阅功能。
 * 如果bInner为true，表示这是一个内部订阅，用于生成K线数据，订阅信息存储在_tick_innersub_map中。
 * 如果bInner为false，表示这是一个外部订阅，用于将Tick数据通过回调函数传递给外部程序，订阅信息存储在_tick_sub_map中。
 * 对于每个订阅的合约代码，还会检查是否有复权标记（前复权或后复权），并存储相应的复权标志。
 * 
 * 该函数可以由用户直接调用，也可以由sub_bar函数调用。
 */
void WtDtRunner::sub_tick(const char* codes, bool bReplace, bool bInner /* = false */)
{
	if(bInner)
	{
		StdUniqueLock lock(_mtx_innersubs);
		if (bReplace)
			_tick_innersub_map.clear();

		const char* stdCode = codes;
		std::size_t length = strlen(stdCode);
		uint32_t flag = 0;
		if (stdCode[length - 1] == SUFFIX_QFQ || stdCode[length - 1] == SUFFIX_HFQ)
		{
			length--;

			flag = (stdCode[length] == SUFFIX_QFQ) ? 1 : 2;
		}

		SubFlags& flags = _tick_innersub_map[std::string(stdCode, length)];
		flags.insert(flag);
		WTSLogger::info("Tick dada of {} subscribed with flag {} for inner use", stdCode, flag);
	}
	else
	{
		StdUniqueLock lock(_mtx_subs);
		if (bReplace)
			_tick_sub_map.clear();

		StringVector ayCodes = StrUtil::split(codes, ",");
		for (const std::string& code : ayCodes)
		{
			//如果是主力合约代码, 如SHFE.ag.HOT, 那么要转换成原合约代码, SHFE.ag.1912
			//因为执行器只识别原合约代码
			const char* stdCode = code.c_str();
			std::size_t length = strlen(stdCode);
			uint32_t flag = 0;
			if (stdCode[length - 1] == SUFFIX_QFQ || stdCode[length - 1] == SUFFIX_HFQ)
			{
				length--;

				flag = (stdCode[length] == SUFFIX_QFQ) ? 1 : 2;
			}

			SubFlags& flags = _tick_sub_map[std::string(stdCode, length)];
			flags.insert(flag);
			WTSLogger::info("Tick dada of {} subscribed with flag {}", stdCode, flag);
		}
	}
}

/**
 * @brief 订阅K线数据
 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
 * @param period K线周期，如"m1"/"d1"
 * 
 * @details 该函数实现对K线数据的订阅功能。
 * 首先解析周期字符串，将其转换为内部的周期类型和倍数。
 * 然后清除数据管理器中的现有订阅，并添加新的订阅。
 * 最后调用sub_tick函数订阅相应的Tick数据，因为K线数据是由Tick数据生成的。
 * 
 * 该函数通常由用户调用，用于订阅实时K线数据。
 */
void WtDtRunner::sub_bar(const char* stdCode, const char* period)
{
	thread_local static char basePeriod[2] = { 0 };
	basePeriod[0] = period[0];
	uint32_t times = 1;
	if (strlen(period) > 1)
		times = strtoul(period + 1, NULL, 10);

	WTSKlinePeriod kp;
	uint32_t realTimes = times;
	if (basePeriod[0] == 'm')
	{
		if (times % 5 == 0)
		{
			kp = KP_Minute5;
			realTimes /= 5;
		}
		else
		{
			kp = KP_Minute1;
		}
	}
	else
		kp = KP_DAY;

	_data_mgr.clear_subbed_bars();
	_data_mgr.subscribe_bar(stdCode, kp, realTimes);
	sub_tick(stdCode, true, true);
}

/**
 * @brief 触发K线数据回调
 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
 * @param period K线周期，如"m1"/"d1"
 * @param lastBar 最新的K线数据结构指针
 * 
 * @details 该函数将最新的K线数据通过回调函数传递给外部订阅者。
 * 如果没有设置K线数据回调函数（_cb_bar为NULL），则直接返回。
 * 该函数通常由数据管理器在生成新的K线数据时调用。
 */
void WtDtRunner::trigger_bar(const char* stdCode, const char* period, WTSBarStruct* lastBar)
{
	if (_cb_bar == NULL)
		return;

	_cb_bar(stdCode, period, lastBar);
}

/**
 * @brief 清除数据缓存
 * 
 * @details 该函数清除数据管理器中的所有缓存数据。
 * 它会调用数据管理器的clear_cache方法来实现。
 * 这在重新加载数据或重置系统状态时非常有用。
 * 
 * 该函数通常由用户调用，用于释放内存或重置系统状态。
 */
void WtDtRunner::clear_cache()
{
	_data_mgr.clear_cache();
}