/*!
 * \file WtDataManager.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 数据管理器实现文件
 * 
 * 本文件实现了WtDataManager类，负责数据的加载、缓存、查询和订阅等功能。
 * 主要功能包括：
 * 1. 初始化数据存储模块
 * 2. 获取不同类型的市场数据（Tick、K线、订单队列、成交明细等）
 * 3. 管理数据订阅和实时更新
 * 4. 处理复权因子
 */
#include "WtDataManager.h"
#include "WtDtRunner.h"
#include "WtHelper.h"

#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSContractInfo.hpp"

#include "../Share/StrUtil.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/CodeHelper.hpp"
#include "../Share/DLLHelper.hpp"

#include "../WTSTools/WTSLogger.h"
#include "../WTSTools/WTSDataFactory.h"


WTSDataFactory g_dataFact;

/**
 * @brief 构造函数
 * 
 * @details 初始化数据管理器对象，将所有指针成员变量初始化为NULL
 * 包括基础数据管理器、主力合约管理器、运行器、数据读取器和实时K线映射
 */
WtDataManager::WtDataManager()
	: _bd_mgr(NULL)
	, _hot_mgr(NULL)
	, _runner(NULL)
	, _reader(NULL)
	, _rt_bars(NULL)
{
}


/**
 * @brief 析构函数
 * 
 * @details 清理数据管理器对象的资源，释放内存
 * 遍历K线缓存映射，释放每个K线数据对象的内存，然后清空缓存映射
 */
WtDataManager::~WtDataManager()
{
	for(auto& m : _bars_cache)
	{
		if (m.second._bars != NULL)
			m.second._bars->release();
	}
	_bars_cache.clear();
}

/**
 * @brief 初始化数据存储模块
 * @param cfg 存储模块配置对象指针
 * @return 初始化是否成功
 * 
 * @details 根据配置加载数据存储模块并初始化数据读取器。
 * 步骤包括：
 * 1. 从配置中获取模块名称，默认为"WtDataStorage"
 * 2. 加载指定的模块动态库
 * 3. 获取创建和删除数据读取器的函数指针
 * 4. 创建数据读取器并初始化
 */
bool WtDataManager::initStore(WTSVariant* cfg)
{
	if (cfg == NULL)
		return false;

	std::string module = cfg->getCString("module");
	if (module.empty())
		module = "WtDataStorage";

	module = WtHelper::get_module_dir() + DLLHelper::wrap_module(module.c_str());
	DllHandle libParser = DLLHelper::load_library(module.c_str());
	if (libParser)
	{
		FuncCreateRdmDtReader pFuncCreateReader = (FuncCreateRdmDtReader)DLLHelper::get_symbol(libParser, "createRdmDtReader");
		if (pFuncCreateReader == NULL)
		{
			WTSLogger::error("Initializing of random data reader failed: function createRdmDtReader not found...");
		}

		FuncDeleteRdmDtReader pFuncDeleteReader = (FuncDeleteRdmDtReader)DLLHelper::get_symbol(libParser, "deleteRdmDtReader");
		if (pFuncDeleteReader == NULL)
		{
			WTSLogger::error("Initializing of random data reader failed: function deleteRdmDtReader not found...");
		}

		if (pFuncCreateReader && pFuncDeleteReader)
		{
			_reader = pFuncCreateReader();
			_remover = pFuncDeleteReader;
		}

	}
	else
	{
		WTSLogger::error("Initializing of random data reader failed: loading module {} failed...", module);

	}

	_reader->init(cfg, this);
	return true;
}

/**
 * @brief 初始化数据管理器
 * @param cfg 配置对象指针
 * @param runner 数据服务运行器指针
 * @return 初始化是否成功
 * 
 * @details 初始化数据管理器，设置相关参数和引用。
 * 步骤包括：
 * 1. 设置运行器引用
 * 2. 从运行器获取基础数据管理器和主力合约管理器
 * 3. 从配置中获取是否按交易时段对齐K线数据的设置
 * 4. 调用initStore初始化数据存储模块
 */
bool WtDataManager::init(WTSVariant* cfg, WtDtRunner* runner)
{
	_runner = runner;
	if (_runner)
	{
		_bd_mgr = &_runner->getBaseDataMgr();
		_hot_mgr = &_runner->getHotMgr();
	}

	_align_by_section = cfg->getBoolean("align_by_section");

	WTSLogger::info("Resampled bars will be aligned by section: {}", _align_by_section ? "yes" : " no");

	return initStore(cfg->get("store"));
}

/**
 * @brief 输出数据读取模块的日志
 * @param ll 日志级别
 * @param message 日志消息
 * 
 * @details 实现IRdmDtReaderSink接口的reader_log方法，将数据读取模块的日志输出到日志系统。
 * 该方法简单地将消息转发给WTSLogger的log_raw方法。
 */
void WtDataManager::reader_log(WTSLogLevel ll, const char* message)
{
	WTSLogger::log_raw(ll, message);
}

/**
 * @brief 根据时间范围获取Tick数据切片
 * @param stdCode 标准化合约代码
 * @param stime 开始时间，格式为YYYYMMDDHHMMSS
 * @param etime 结束时间，格式为YYYYMMDDHHMMSS，默认为0（表示当前时间）
 * @return Tick数据切片指针
 * 
 * @details 根据时间范围获取指定合约的Tick数据切片。
 * 时间参数需要进行转换（乘以100000），以适应内部数据存储格式。
 * 返回的数据切片需要用户调用release方法释放内存。
 */
WTSTickSlice* WtDataManager::get_tick_slices_by_range(const char* stdCode,uint64_t stime, uint64_t etime /* = 0 */)
{
	stime = stime * 100000;
	etime = etime * 100000;
	return _reader->readTickSliceByRange(stdCode, stime, etime);
}

/**
 * @brief 根据日期获取Tick数据切片
 * @param stdCode 标准化合约代码
 * @param uDate 交易日期，格式为YYYYMMDD，默认为0（表示当前日期）
 * @return Tick数据切片指针
 * 
 * @details 根据日期获取指定合约的Tick数据切片。
 * 如果日期为0，则使用当前日期。
 * 返回的数据切片需要用户调用release方法释放内存。
 */
WTSTickSlice* WtDataManager::get_tick_slice_by_date(const char* stdCode, uint32_t uDate /* = 0 */)
{
	return _reader->readTickSliceByDate(stdCode, uDate);
}

/**
 * @brief 根据时间范围获取订单队列数据切片
 * @param stdCode 标准化合约代码
 * @param stime 开始时间，格式为YYYYMMDDHHMMSS
 * @param etime 结束时间，格式为YYYYMMDDHHMMSS，默认为0（表示当前时间）
 * @return 订单队列数据切片指针
 * 
 * @details 根据时间范围获取指定合约的订单队列数据切片。
 * 时间参数需要进行转换（乘以100000），以适应内部数据存储格式。
 * 返回的数据切片需要用户调用release方法释放内存。
 */
WTSOrdQueSlice* WtDataManager::get_order_queue_slice(const char* stdCode,uint64_t stime, uint64_t etime /* = 0 */)
{
	stime = stime * 100000;
	etime = etime * 100000;
	return _reader->readOrdQueSliceByRange(stdCode, stime, etime);
}

/**
 * @brief 根据时间范围获取订单明细数据切片
 * @param stdCode 标准化合约代码
 * @param stime 开始时间，格式为YYYYMMDDHHMMSS
 * @param etime 结束时间，格式为YYYYMMDDHHMMSS，默认为0（表示当前时间）
 * @return 订单明细数据切片指针
 * 
 * @details 根据时间范围获取指定合约的订单明细数据切片。
 * 时间参数需要进行转换（乘以100000），以适应内部数据存储格式。
 * 返回的数据切片需要用户调用release方法释放内存。
 */
WTSOrdDtlSlice* WtDataManager::get_order_detail_slice(const char* stdCode,uint64_t stime, uint64_t etime /* = 0 */)
{
	stime = stime * 100000;
	etime = etime * 100000;
	return _reader->readOrdDtlSliceByRange(stdCode, stime, etime);
}

/**
 * @brief 根据时间范围获取成交明细数据切片
 * @param stdCode 标准化合约代码
 * @param stime 开始时间，格式为YYYYMMDDHHMMSS
 * @param etime 结束时间，格式为YYYYMMDDHHMMSS，默认为0（表示当前时间）
 * @return 成交明细数据切片指针
 * 
 * @details 根据时间范围获取指定合约的成交明细数据切片。
 * 时间参数需要进行转换（乘以100000），以适应内部数据存储格式。
 * 返回的数据切片需要用户调用release方法释放内存。
 */
WTSTransSlice* WtDataManager::get_transaction_slice(const char* stdCode,uint64_t stime, uint64_t etime /* = 0 */)
{
	stime = stime * 100000;
	etime = etime * 100000;
	return _reader->readTransSliceByRange(stdCode, stime, etime);
}

/**
 * @brief 获取交易时段信息
 * @param sid 交易时段ID或合约代码
 * @param isCode 是否为合约代码，默认为false
 * @return 交易时段信息对象指针
 * 
 * @details 根据时段ID或合约代码获取交易时段信息。
 * 如果isCode为false，直接从基础数据管理器中获取时段信息。
 * 如果isCode为true，则需要先解析合约代码，获取其对应的品种信息，然后再获取时段信息。
 * 该方法在生成K线数据和对齐数据时非常有用。
 */
WTSSessionInfo* WtDataManager::get_session_info(const char* sid, bool isCode /* = false */)
{
	if (!isCode)
		return _bd_mgr->getSession(sid);

	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(sid, _hot_mgr);
	WTSCommodityInfo* cInfo = _bd_mgr->getCommodity(codeInfo._exchg, codeInfo._product);
	if (cInfo == NULL)
		return NULL;

	return cInfo->getSessionInfo();
}

/**
 * @brief 根据日期获取秒线K线数据切片
 * @param stdCode 标准化合约代码
 * @param secs 秒线周期，单位为秒
 * @param uDate 交易日期，格式为YYYYMMDD，默认为0（表示当前日期）
 * @return 秒线K线数据切片指针
 * 
 * @details 根据日期获取指定合约的秒线K线数据切片。
 * 该方法的工作流程如下：
 * 1. 根据合约代码、日期和秒线周期生成缓存键
 * 2. 获取合约的交易时段信息
 * 3. 检查缓存中是否已有数据，如果没有，则从存储中读取Tick数据并生成秒线K线
 * 4. 创建并返回K线数据切片
 * 
 * 秒线K线是从原始Tick数据生成的，而不是直接从存储中读取。
 * 生成的数据会缓存在内存中，以便于后续快速访问。
 * 返回的数据切片需要用户调用release方法释放内存。
 */
WTSKlineSlice* WtDataManager::get_skline_slice_by_date(const char* stdCode, uint32_t secs, uint32_t uDate /* = 0 */)
{
	std::string key = StrUtil::printf("%s-%u-s%u", stdCode, uDate, secs);

	//只有非基础周期的会进到下面的步骤
	WTSSessionInfo* sInfo = get_session_info(stdCode, true);
	BarCache& barCache = _bars_cache[key];
	barCache._period = KP_Tick;
	barCache._times = secs;
	if (barCache._bars == NULL)
	{
		//第一次将全部数据缓存到内存中
		WTSTickSlice* ticks = _reader->readTickSliceByDate(stdCode, uDate);
		if (ticks != NULL)
		{
			WTSKlineData* kData = g_dataFact.extractKlineData(ticks, secs, sInfo, true);
			barCache._bars = kData;		
			ticks->release();
		}
		else
		{
			return NULL;
		}
	}
	
	if (barCache._bars == NULL)
		return NULL;

	WTSBarStruct* rtHead = barCache._bars->at(0);
	WTSKlineSlice* slice = WTSKlineSlice::create(stdCode, KP_Tick, secs, rtHead, barCache._bars->size());
	return slice;
}

/**
 * @brief 根据日期获取K线数据切片
 * @param stdCode 标准化合约代码
 * @param period K线周期类型（分钟、日线等）
 * @param times 周期倍数，如当5分钟线时为5
 * @param uDate 交易日期，格式为YYYYMMDD，默认为0（表示当前日期）
 * @return K线数据切片指针
 * 
 * @details 根据日期获取指定合约的K线数据切片。
 * 该方法的工作流程如下：
 * 1. 解析合约代码，获取标准化的品种ID
 * 2. 根据品种ID和日期获取交易时段的边界时间（开始时间和结束时间）
 * 3. 调用get_kline_slice_by_range方法获取指定时间范围内的K线数据
 * 
 * 这个方法实际上是将按日期获取转换为按时间范围获取，因为内部存储是基于时间范围的。
 * 返回的数据切片需要用户调用release方法释放内存。
 */
WTSKlineSlice* WtDataManager::get_kline_slice_by_date(const char* stdCode, WTSKlinePeriod period, uint32_t times, uint32_t uDate /* = 0 */)
{
	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	uint64_t stime = _bd_mgr->getBoundaryTime(codeInfo.stdCommID(), uDate, false, true);
	uint64_t etime = _bd_mgr->getBoundaryTime(codeInfo.stdCommID(), uDate, false, false);
	return get_kline_slice_by_range(stdCode, period, times, stime, etime);
}

/**
 * @brief 根据时间范围获取K线数据切片
 * @param stdCode 标准化合约代码
 * @param period K线周期类型（分钟、日线等）
 * @param times 周期倍数，如当5分钟线时为5
 * @param stime 开始时间，格式为YYYYMMDDHHMMSS
 * @param etime 结束时间，格式为YYYYMMDDHHMMSS，默认为0（表示当前时间）
 * @return K线数据切片指针
 * 
 * @details 根据时间范围获取指定合约的K线数据切片。
 * 该方法的处理逻辑如下：
 * 1. 如果周期倍数为1（基础周期），直接从数据读取器中读取数据
 * 2. 如果周期倍数大于1（非基础周期），需要先读取基础周期数据，然后进行重采样
 * 3. 对于非基础周期，会先检查缓存中是否已有数据，如果有，则直接使用缓存数据
 * 4. 如果缓存中没有数据，则从数据读取器中读取基础周期数据，并进行重采样
 * 5. 根据请求的时间范围，从重采样后的数据中提取相应的数据切片
 * 
 * 返回的数据切片需要用户调用release方法释放内存。
 */
WTSKlineSlice* WtDataManager::get_kline_slice_by_range(const char* stdCode, WTSKlinePeriod period, uint32_t times,uint64_t stime, uint64_t etime /* = 0 */)
{
	if (times == 1)
	{
		return _reader->readKlineSliceByRange(stdCode, period, stime, etime);
	}

	//只有非基础周期的会进到下面的步骤
	WTSSessionInfo* sInfo = get_session_info(stdCode, true);
	std::string key = StrUtil::printf("%s-%u-%u", stdCode, period, times);
	BarCache& barCache = _bars_cache[key];
	barCache._period = period;
	barCache._times = times;
	if(barCache._bars == NULL)
	{
		//第一次将全部数据缓存到内存中
		WTSKlineSlice* rawData = _reader->readKlineSliceByCount(stdCode, period, UINT_MAX, 0);
		if (rawData != NULL)
		{
			WTSKlineData* kData = g_dataFact.extractKlineData(rawData, period, times, sInfo, false);
			barCache._bars = kData;

			//不管如何，都删除最后一条K线
			//不能通过闭合标记判断，因为读取的基础周期可能本身没有闭合
			if (barCache._bars->size() > 0)
			{
				auto& bars = barCache._bars->getDataRef();
				bars.erase(bars.begin() + bars.size() - 1, bars.end());
			}

			if (period == KP_DAY)
				barCache._last_bartime = kData->date(-1);
			else
			{
				uint64_t lasttime = kData->time(-1);
				barCache._last_bartime = 199000000000 + lasttime;
			}

			rawData->release();
		}
		else
		{
			return NULL;
		}
	}
	else
	{
		//后面则增量更新
		WTSKlineSlice* rawData = _reader->readKlineSliceByRange(stdCode, period, barCache._last_bartime, 0);
		if (rawData != NULL)
		{
			for(int32_t idx = 0; idx < rawData->size(); idx ++)
			{
				uint64_t barTime = 0;
				if (period == KP_DAY)
					barTime = rawData->at(0)->date;
				else
					barTime = 199000000000 + rawData->at(0)->time;
				
				//只有时间上次记录的最后一条时间，才可以用于更新K线
				if(barTime <= barCache._last_bartime)
					continue;

				g_dataFact.updateKlineData(barCache._bars, rawData->at(idx), sInfo, _align_by_section);
			}

			//不管如何，都删除最后一条K线
			//不能通过闭合标记判断，因为读取的基础周期可能本身没有闭合
			if(barCache._bars->size() > 0)
			{
				auto& bars = barCache._bars->getDataRef();
				bars.erase(bars.begin() + bars.size() - 1, bars.end());
			}

			if (period == KP_DAY)
				barCache._last_bartime = barCache._bars->date(-1);
			else
			{
				uint64_t lasttime = barCache._bars->time(-1);
				barCache._last_bartime = 199000000000 + lasttime;
			}
			

			rawData->release();
		}
	}

	//最后到缓存中定位
	bool isDay = period == KP_DAY;
	uint32_t rDate, rTime, lDate, lTime;
	rDate = (uint32_t)(etime / 10000);
	rTime = (uint32_t)(etime % 10000);
	lDate = (uint32_t)(stime / 10000);
	lTime = (uint32_t)(stime % 10000);

	WTSBarStruct eBar;
	eBar.date = rDate;
	eBar.time = (rDate - 19900000) * 10000 + rTime;

	WTSBarStruct sBar;
	sBar.date = lDate;
	sBar.time = (lDate - 19900000) * 10000 + lTime;

	uint32_t eIdx, sIdx;
	auto& bars = barCache._bars->getDataRef();
	auto eit = std::lower_bound(bars.begin(), bars.end(), eBar, [isDay](const WTSBarStruct& a, const WTSBarStruct& b) {
		if (isDay)
			return a.date < b.date;
		else
			return a.time < b.time;
	});


	if (eit == bars.end())
		eIdx = bars.size() - 1;
	else
	{
		if ((isDay && eit->date > eBar.date) || (!isDay && eit->time > eBar.time))
		{
			eit--;
		}

		eIdx = eit - bars.begin();
	}

	auto sit = std::lower_bound(bars.begin(), eit, sBar, [isDay](const WTSBarStruct& a, const WTSBarStruct& b) {
		if (isDay)
			return a.date < b.date;
		else
			return a.time < b.time;
	});
	sIdx = sit - bars.begin();
	uint32_t rtCnt = eIdx - sIdx + 1;
	WTSBarStruct* rtHead = barCache._bars->at(sIdx);
	WTSKlineSlice* slice = WTSKlineSlice::create(stdCode, period, times, rtHead, rtCnt);
	return slice;
}

/**
 * @brief 根据数量获取K线数据切片
 * @param stdCode 标准化合约代码
 * @param period K线周期类型（分钟、日线等）
 * @param times 周期倍数，如当5分钟线时为5
 * @param count 要获取的K线数量
 * @param etime 结束时间，格式为YYYYMMDDHHMMSS，默认为0（表示当前时间）
 * @return K线数据切片指针
 * 
 * @details 根据数量获取指定合约的K线数据切片。
 * 该方法的处理逻辑与get_kline_slice_by_range类似，不同之处在于：
 * 1. 对于基础周期（times=1），直接从数据读取器中读取指定数量的数据
 * 2. 对于非基础周期，需要先读取足够的基础周期数据，然后进行重采样
 * 3. 根据结束时间和请求的数量，从重采样后的数据中提取相应的数据切片
 * 
 * 返回的数据是从结束时间往前数的count条数据。
 * 返回的数据切片需要用户调用release方法释放内存。
 */
WTSKlineSlice* WtDataManager::get_kline_slice_by_count(const char* stdCode, WTSKlinePeriod period, uint32_t times, uint32_t count, uint64_t etime /* = 0 */)
{
	if (times == 1)
	{
		return _reader->readKlineSliceByCount(stdCode, period, count, etime);
	}

	//只有非基础周期的会进到下面的步骤
	WTSSessionInfo* sInfo = get_session_info(stdCode, true);
	std::string key = StrUtil::printf("%s-%u-%u", stdCode, period, times);
	BarCache& barCache = _bars_cache[key];
	barCache._period = period;
	barCache._times = times;

	const char* tag = PERIOD_NAME[period-KP_Tick];

	if (barCache._bars == NULL)
	{
		//第一次将全部数据缓存到内存中
		WTSLogger::info("Caching all {} bars of {}...", tag, stdCode);
		WTSKlineSlice* rawData = _reader->readKlineSliceByCount(stdCode, period, UINT_MAX, 0);
		if (rawData != NULL)
		{
			WTSLogger::info("Resampling {} {} bars by {}-TO-1 of {}...", rawData->size(), tag, times, stdCode);
			WTSKlineData* kData = g_dataFact.extractKlineData(rawData, period, times, sInfo, true);
			barCache._bars = kData;

			//如果不是日线，要考虑最后一条K线是否闭合的情况
			//这里采用保守的方案，如果本地时间大于最后一条K线的时间，则认为真正闭合了
			if (period != KP_DAY)
			{
				uint64_t last_bartime = 0;
				last_bartime = 199000000000 + kData->time(-1);

				uint64_t now = TimeUtils::getYYYYMMDDhhmmss() / 100;
				if (now <= last_bartime && barCache._bars->size() > 0)
				{
					auto& bars = barCache._bars->getDataRef();
					bars.erase(bars.begin() + bars.size() - 1, bars.end());
				}
			}


			if (period == KP_DAY)
				barCache._last_bartime = kData->date(-1);
			else
			{
				uint64_t lasttime = kData->time(-1);
				barCache._last_bartime = 199000000000 + lasttime;
			}

			rawData->release();
		}
		else
		{
			return NULL;
		}
	}
	else
	{
		//后面则增量更新
		WTSKlineSlice* rawData = _reader->readKlineSliceByRange(stdCode, period, barCache._last_bartime, 0);
		if (rawData != NULL)
		{
			WTSLogger::info("{} {} bars of {} updated, adding to cache...", rawData->size(), tag, stdCode);
			for (int32_t idx = 0; idx < rawData->size(); idx++)
			{
				uint64_t barTime = 0;
				if (period == KP_DAY)
					barTime = rawData->at(0)->date;
				else
					barTime = 199000000000 + rawData->at(0)->time;

				//只有时间上次记录的最后一条时间，才可以用于更新K线
				if (barTime <= barCache._last_bartime)
					continue;

				g_dataFact.updateKlineData(barCache._bars, rawData->at(idx), sInfo, _align_by_section);
			}

			//如果不是日线，要考虑最后一条K线是否闭合的情况
			//这里采用保守的方案，如果本地时间大于最后一条K线的时间，则认为真正闭合了
			if (period != KP_DAY)
			{
				uint64_t last_bartime = 0;
				last_bartime = 199000000000 + barCache._bars->time(-1);

				uint64_t now = TimeUtils::getYYYYMMDDhhmmss() / 100;
				if (now <= last_bartime && barCache._bars->size() > 0)
				{
					auto& bars = barCache._bars->getDataRef();
					bars.erase(bars.begin() + bars.size() - 1, bars.end());
				}
			}

			if (period == KP_DAY)
				barCache._last_bartime = barCache._bars->date(-1);
			else
			{
				uint64_t lasttime = barCache._bars->time(-1);
				barCache._last_bartime = 199000000000 + lasttime;
			}


			rawData->release();
		}
	}

	//最后到缓存中定位
	bool isDay = period == KP_DAY;
	uint32_t rDate, rTime;
	rDate = (uint32_t)(etime / 10000);
	rTime = (uint32_t)(etime % 10000);

	WTSBarStruct eBar;
	eBar.date = rDate;
	eBar.time = (rDate - 19900000) * 10000 + rTime;

	uint32_t eIdx, sIdx;
	auto& bars = barCache._bars->getDataRef();
	auto eit = std::lower_bound(bars.begin(), bars.end(), eBar, [isDay](const WTSBarStruct& a, const WTSBarStruct& b) {
		if (isDay)
			return a.date < b.date;
		else
			return a.time < b.time;
	});


	if (eit == bars.end())
		eIdx = bars.size() - 1;
	else
	{
		if ((isDay && eit->date > eBar.date) || (!isDay && eit->time > eBar.time))
		{
			eit--;
		}

		eIdx = eit - bars.begin();
	}

	sIdx = (eIdx + 1 >= count) ? (eIdx + 1 - count) : 0;
	uint32_t rtCnt = eIdx - sIdx + 1;
	WTSBarStruct* rtHead = barCache._bars->at(sIdx);
	WTSKlineSlice* slice = WTSKlineSlice::create(stdCode, period, times, rtHead, rtCnt);
	return slice;
}

/**
 * @brief 根据数量获取Tick数据切片
 * @param stdCode 标准化合约代码
 * @param count 要获取的Tick数量
 * @param etime 结束时间，格式为YYYYMMDDHHMMSS，默认为0（表示当前时间）
 * @return Tick数据切片指针
 * 
 * @details 根据数量获取指定合约的Tick数据切片。
 * 时间参数需要进行转换（乘以100000），以适应内部数据存储格式。
 * 返回的数据是从结束时间往前数的count条数据。
 * 返回的数据切片需要用户调用release方法释放内存。
 */
WTSTickSlice* WtDataManager::get_tick_slice_by_count(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */)
{
	etime = etime * 100000;
	return _reader->readTickSliceByCount(stdCode, count, etime);
}

/**
 * @brief 获取复权因子
 * @param stdCode 标准化合约代码
 * @param commInfo 品种信息对象指针，默认为NULL
 * @return 复权因子，默认为1.0（不复权）
 * 
 * @details 根据合约代码和品种信息获取复权因子。
 * 如果品种信息为NULL，直接返回1.0（不复权）。
 * 如果是股票，从数据读取器中获取除权因子。
 * 如果是期货，且是主力合约，从主力合约管理器中获取规则因子。
 * 复权因子用于调整历史数据，使其与当前数据可比。
 */
double WtDataManager::get_exright_factor(const char* stdCode, WTSCommodityInfo* commInfo /* = NULL */)
{
	if (commInfo == NULL)
		return 1.0;

	if (commInfo->isStock())
		return _reader->getAdjFactorByDate(stdCode, 0);
	else
	{
		const char* ruleTag = _hot_mgr->getRuleTag(stdCode);
		if (strlen(ruleTag) > 0)
			return _hot_mgr->getRuleFactor(ruleTag, commInfo->getFullPid(), 0);
	}

	return 1.0;
}

/**
 * @brief 订阅K线数据
 * @param stdCode 标准化合约代码
 * @param period K线周期类型（分钟、日线等）
 * @param times 周期倍数，如当5分钟线时为5
 * 
 * @details 订阅指定合约的K线数据，并将数据缓存到内存中以便于实时更新。
 * 该方法的处理逻辑如下：
 * 1. 根据合约代码、周期类型和周期倍数生成缓存键
 * 2. 如果是基础周期（times=1），直接从数据读取器中读取数据并缓存
 * 3. 如果是非基础周期，需要先读取基础周期数据，然后进行重采样生成目标周期数据
 * 4. 将生成的数据添加到实时K线映射中
 * 
 * 该方法通常由WtDtRunner的sub_bar方法调用。
 */
void WtDataManager::subscribe_bar(const char* stdCode, WTSKlinePeriod period, uint32_t times)
{
	std::string key = fmtutil::format("{}-{}-{}", stdCode, (uint32_t)period, times);

	uint32_t curDate = TimeUtils::getCurDate();
	uint64_t etime = (uint64_t)curDate * 10000 + 2359;

	if (times == 1)
	{
		WTSKlineSlice* slice = _reader->readKlineSliceByCount(stdCode, period, 10, etime);
		if (slice == NULL)
			return;

		WTSKlineData* kline = WTSKlineData::create(stdCode, slice->size());
		kline->setPeriod(period);
		uint32_t offset = 0;
		for(uint32_t blkIdx = 0; blkIdx < slice->get_block_counts(); blkIdx++)
		{
			memcpy(kline->getDataRef().data() + offset, slice->get_block_addr(blkIdx), sizeof(WTSBarStruct)*slice->get_block_size(blkIdx));
			offset += slice->get_block_size(blkIdx);
		}
		
		{
			StdUniqueLock lock(_mtx_rtbars);
			if (_rt_bars == NULL)
				_rt_bars = RtBarMap::create();

			_rt_bars->add(key, kline, false);
		}

		slice->release();
	}
	else
	{
		//只有非基础周期的会进到下面的步骤
		WTSSessionInfo* sInfo = get_session_info(stdCode, true);
		WTSKlineSlice* rawData = _reader->readKlineSliceByCount(stdCode, period, 10*times, 0);
		if (rawData != NULL)
		{
			WTSKlineData* kData = g_dataFact.extractKlineData(rawData, period, times, sInfo, true);
			{
				StdUniqueLock lock(_mtx_rtbars);
				if (_rt_bars == NULL)
					_rt_bars = RtBarMap::create();
				_rt_bars->add(key, kData, false);
			}
			rawData->release();
		}
	}

	WTSLogger::info("Realtime bar {} has subscribed", key);
}

void WtDataManager::clear_subbed_bars()
{
	StdUniqueLock lock(_mtx_rtbars);
	if (_rt_bars)
		_rt_bars->clear();
}

void WtDataManager::update_bars(const char* stdCode, WTSTickData* newTick)
{
	if (_rt_bars == NULL)
		return;

	StdUniqueLock lock(_mtx_rtbars);
	auto it = _rt_bars->begin();
	for(; it != _rt_bars->end(); it++)
	{
		WTSKlineData* kData = (WTSKlineData*)it->second;
		if (strcmp(kData->code(), stdCode) != 0)
			continue;

		WTSSessionInfo* sInfo = NULL;
		if (newTick->getContractInfo())
			sInfo = newTick->getContractInfo()->getCommInfo()->getSessionInfo();
		else
			sInfo = get_session_info(kData->code(), true);
		g_dataFact.updateKlineData(kData, newTick, sInfo, _align_by_section);
		WTSBarStruct* lastBar = kData->at(-1);

		std::string speriod;
		uint32_t times = kData->times();
		switch (kData->period())
		{
		case KP_Minute1:
			speriod = fmtutil::format("m{}", times);
			break;
		case KP_Minute5:
			speriod = fmtutil::format("m{}", times*5);
			break;
		default:
			speriod = fmtutil::format("d{}", times);
			break;
		}

		_runner->trigger_bar(stdCode, speriod.c_str(), lastBar);
	}
}

void WtDataManager::clear_cache()
{
	if (_reader == NULL)
	{
		WTSLogger::warn("DataReader not initialized, clearing canceled");
		return;
	}

	_reader->clearCache();
	WTSLogger::warn("All cache cleared");
}