/*!
 * @file WtSimpDataMgr.cpp
 * @project WonderTrader
 *
 * @author Wesley
 * @date 2020/03/30
 *
 * @brief 简单数据管理器实现
 * @details 实现了简单数据管理器类，用于管理行情数据，包括实时行情和历史数据
 */
#include "WtSimpDataMgr.h"
#include "WtExecRunner.h"
#include "../WtCore/WtHelper.h"

#include "../Share/StrUtil.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Share/DLLHelper.hpp"
#include "../Includes/WTSSessionInfo.hpp"

#include "../WTSTools/WTSLogger.h"
#include "../WTSTools/WTSDataFactory.h"

USING_NS_WTP;


/**
 * @brief 全局数据工厂实例
 * @details 用于创建和处理数据对象
 */
WTSDataFactory g_dataFact;

/**
 * @brief 简单数据管理器构造函数
 * @details 初始化成员变量为空
 */
WtSimpDataMgr::WtSimpDataMgr()
	: _reader(NULL)      // 数据读取器
	, _runner(NULL)      // 执行器运行器
	, _bars_cache(NULL)  // K线缓存
	, _rt_tick_map(NULL) // 实时tick缓存
{
	// 构造函数初始化成员变量为空
}


/**
 * @brief 简单数据管理器析构函数
 * @details 释放实时tick缓存资源
 */
WtSimpDataMgr::~WtSimpDataMgr()
{
	// 释放实时tick缓存资源
	if (_rt_tick_map)
		_rt_tick_map->release();
}

/**
 * @brief 初始化数据存储模块
 * @details 加载数据存储模块并创建数据读取器
 * @param cfg 数据存储配置
 * @return 初始化是否成功
 */
bool WtSimpDataMgr::initStore(WTSVariant* cfg)
{
	// 检查配置是否为空
	if (cfg == NULL)
		return false;

	// 获取数据存储模块名称
	std::string module = cfg->getCString("module");
	if (module.empty())
		// 如果模块名称为空，使用默认的WtDataStorage
		module = WtHelper::getInstDir() + DLLHelper::wrap_module("WtDataStorage");
	else
		// 否则使用指定的模块
		module = WtHelper::getInstDir() + DLLHelper::wrap_module(module.c_str());

	// 加载数据存储模块动态库
	DllHandle hInst = DLLHelper::load_library(module.c_str());
	if (hInst == NULL)
	{
		WTSLogger::error("Data reader {} loading failed", module.c_str());
		return false;
	}

	// 获取创建数据读取器的函数
	FuncCreateDataReader funcCreator = (FuncCreateDataReader)DLLHelper::get_symbol(hInst, "createDataReader");
	if (funcCreator == NULL)
	{
		WTSLogger::error("Data reader {} loading failed: entrance function createDataReader not found", module.c_str());
		DLLHelper::free_library(hInst);
		return false;
	}

	// 创建数据读取器
	_reader = funcCreator();
	if (_reader == NULL)
	{
		WTSLogger::error("Data reader {} creating api failed", module.c_str());
		DLLHelper::free_library(hInst);
		return false;
	}

	// 初始化数据读取器
	_reader->init(cfg, this);

	// 获取交易时段信息
	_s_info = _runner->get_session_info(cfg->getCString("session"), false);

	return true;
}

/**
 * @brief 初始化数据管理器
 * @details 设置执行器运行器并初始化数据存储
 * @param cfg 配置项
 * @param runner 执行器运行器
 * @return 初始化是否成功
 */
bool WtSimpDataMgr::init(WTSVariant* cfg, WtExecRunner* runner)
{
	// 设置执行器运行器
	_runner = runner;
	// 初始化数据存储模块
	return initStore(cfg->get("store"));
}

/**
 * @brief 所有K线数据更新完成回调
 * @details 实现IDataReaderSink接口的方法，当所有K线数据更新完成时调用
 * @param updateTime 更新时间
 */
void WtSimpDataMgr::on_all_bar_updated(uint32_t updateTime)
{
	// 注意: 当前实现为空，可以在这里添加更新完成后的处理逻辑
}

/**
 * @brief 获取基础数据管理器
 * @details 实现IDataReaderSink接口的方法，返回执行器运行器中的基础数据管理器
 * @return 基础数据管理器指针
 */
IBaseDataMgr* WtSimpDataMgr::get_basedata_mgr()
{
	// 返回执行器运行器中的基础数据管理器
	return _runner->get_bd_mgr();
}

/**
 * @brief 获取主力合约管理器
 * @details 实现IDataReaderSink接口的方法，返回执行器运行器中的主力合约管理器
 * @return 主力合约管理器指针
 */
IHotMgr* WtSimpDataMgr::get_hot_mgr()
{
	// 返回执行器运行器中的主力合约管理器
	return _runner->get_hot_mgr();
}

/**
 * @brief 获取当前日期
 * @details 实现IDataReaderSink接口的方法，返回当前日期
 * @return 当前日期，格式为YYYYMMDD
 */
uint32_t WtSimpDataMgr::get_date()
{
	// 返回当前日期
	return _cur_date;
}

/**
 * @brief 获取当前分钟时间
 * @details 实现IDataReaderSink接口的方法，返回当前分钟时间
 * @return 当前分钟时间，格式为HHMM
 */
uint32_t WtSimpDataMgr::get_min_time()
{
	// 返回当前分钟时间
	return _cur_min_time;
}

/**
 * @brief 获取当前秒数
 * @details 实现IDataReaderSink接口的方法，返回当前秒数
 * @return 当前秒数，格式为SSMMM
 */
uint32_t WtSimpDataMgr::get_secs()
{
	// 返回当前秒数
	return _cur_secs;
}

/**
 * @brief 数据读取器日志回调
 * @details 实现IDataReaderSink接口的方法，将数据读取器的日志输出到系统日志
 * @param ll 日志级别
 * @param message 日志消息
 */
void WtSimpDataMgr::reader_log(WTSLogLevel ll, const char* message)
{
	// 将数据读取器的日志输出到系统日志
	WTSLogger::log_raw(ll, message);
}

/**
 * @brief 收到K线数据回调
 * @details 实现IDataReaderSink接口的方法，当收到新的K线数据时调用
 * @param code 合约代码
 * @param period K线周期
 * @param newBar 新的K线数据
 */
void WtSimpDataMgr::on_bar(const char* code, WTSKlinePeriod period, WTSBarStruct* newBar)
{
	// 注意: 当前实现为空，可以在这里添加处理新K线数据的逻辑
}

/**
 * @brief 处理推送的行情数据
 * @details 处理新收到的Tick数据，更新实时行情缓存和当前时间信息
 * @param stdCode 标准化合约代码
 * @param curTick 当前的Tick数据
 */
void WtSimpDataMgr::handle_push_quote(const char* stdCode, WTSTickData* curTick)
{
	// 检查Tick数据是否有效
	if (curTick == NULL)
		return;

	// 如果实时Tick缓存不存在，则创建
	if (_rt_tick_map == NULL)
		_rt_tick_map = DataCacheMap::create();

	// 将新的Tick数据添加到缓存中，true表示如果已存在则覆盖
	_rt_tick_map->add(stdCode, curTick, true);

	// 获取Tick数据中的日期和时间
	uint32_t uDate = curTick->actiondate();
	uint32_t uTime = curTick->actiontime();

	// 如果新收到的数据时间早于当前已存储的时间，则忽略
	if (_cur_date != 0 && (uDate < _cur_date || (uDate == _cur_date && uTime < _cur_act_time)))
	{
		return;
	}

	// 更新当前日期和完整时间
	_cur_date = uDate;
	_cur_act_time = uTime;

	// 计算当前原始时间和秒数
	uint32_t _cur_raw_time = _cur_act_time / 100000; // 提取小时分钟部分
	uint32_t _cur_secs = _cur_act_time % 100000;      // 提取秒数部分

	// 将时间转换为分钟数
	uint32_t minutes = _s_info->timeToMinutes(_cur_raw_time);
	// 检查是否是交易时段的最后一分钟
	bool isSecEnd = _s_info->isLastOfSection(_cur_raw_time);
	if (isSecEnd)
	{
		// 如果是交易时段的最后一分钟，则减去1分钟
		minutes--;
	}
	// 增加1分钟，确保分钟数正确
	minutes++;
	// 将分钟数转换回时间格式
	_cur_min_time = _s_info->minuteToTime(minutes);
	// 设置当前交易日
	_cur_tdate = curTick->tradingdate();
}

/**
 * @brief 获取最新的Tick数据
 * @details 实现IDataManager接口的方法，从实时缓存中获取指定合约的最新Tick数据
 * @param code 合约代码
 * @return 最新的Tick数据指针，如果不存在则返回NULL
 */
WTSTickData* WtSimpDataMgr::grab_last_tick(const char* code)
{
	// 检查实时Tick缓存是否存在
	if (_rt_tick_map == NULL)
		return NULL;

	// 从缓存中获取指定合约的Tick数据
	WTSTickData* curTick = (WTSTickData*)_rt_tick_map->get(code);
	if (curTick == NULL)
		return NULL;

	// 增加引用计数，防止被意外释放
	curTick->retain();
	return curTick;
}


/**
 * @brief 获取Tick切片数据
 * @details 实现IDataManager接口的方法，从数据读取器中获取指定合约的Tick切片数据
 * @param code 合约代码
 * @param count 获取的数据条数
 * @param etime 结束时间，默认为0表示当前
 * @return Tick切片数据指针，如果数据读取器不存在则返回NULL
 */
WTSTickSlice* WtSimpDataMgr::get_tick_slice(const char* code, uint32_t count, uint64_t etime /*= 0*/)
{
	// 检查数据读取器是否存在
	if (_reader == NULL)
		return NULL;

	// 从数据读取器中读取Tick切片数据
	return _reader->readTickSlice(code, count, etime);
}


/**
 * @brief 获取K线切片数据
 * @details 实现IDataManager接口的方法，从数据读取器中获取指定合约的K线切片数据
 * @param stdCode 标准化合约代码
 * @param period K线周期
 * @param times 周期倍数
 * @param count 获取的数据条数
 * @param etime 结束时间，默认为0表示当前
 * @return K线切片数据指针，如果数据读取器不存在则返回NULL
 */
WTSKlineSlice* WtSimpDataMgr::get_kline_slice(const char* stdCode, WTSKlinePeriod period, uint32_t times, uint32_t count, uint64_t etime /*= 0*/)
{
	// 检查数据读取器是否存在
	if (_reader == NULL)
		return NULL;

	// 生成缓存键
	std::string key = StrUtil::printf("%s-%u", stdCode, period);

	// 如果是基础周期（倍数为1），直接从数据读取器中读取
	if (times == 1)
	{
		return _reader->readKlineSlice(stdCode, period, count, etime);
	}

	// 只有非基础周期的会进到下面的步骤
	// 获取交易时段信息
	WTSSessionInfo* sInfo = _runner->get_session_info(stdCode, true);

	// 如果K线缓存不存在，则创建
	if (_bars_cache == NULL)
		_bars_cache = DataCacheMap::create();

	// 生成包含周期倍数的缓存键
	key = StrUtil::printf("%s-%u-%u", stdCode, period, times);

	// 从缓存中获取K线数据
	WTSKlineData* kData = (WTSKlineData*)_bars_cache->get(key);
	// 如果缓存中的数据不存在或条数不足，需要重新读取
	if (kData == NULL || kData->size() < count)
	{
		// 计算需要读取的实际条数，考虑周期倍数
		uint32_t realCount = count * times + times;
		// 读取原始K线数据
		WTSKlineSlice* rawData = _reader->readKlineSlice(stdCode, period, realCount, etime);
		if (rawData != NULL)
		{
			// 从原始K线数据中提取指定周期倍数的K线数据
			kData = g_dataFact.extractKlineData(rawData, period, times, sInfo, true);
			// 释放原始K线数据
			rawData->release();
		}
		else
		{
			return NULL;
		}

		// 将提取的K线数据添加到缓存中，false表示不覆盖已存在的数据
		if (kData)
			_bars_cache->add(key, kData, false);
	}

	// 计算返回数据的起始索引和实际条数
	int32_t sIdx = 0;
	// 实际返回条数为请求条数和缓存条数的最小值
	uint32_t rtCnt = min(kData->size(), count);
	// 计算起始索引，保证返回最新的数据
	sIdx = kData->size() - rtCnt;
	// 获取数据头指针
	WTSBarStruct* rtHead = kData->at(sIdx);
	// 创建并返回K线切片
	WTSKlineSlice* slice = WTSKlineSlice::create(stdCode, period, times, rtHead, rtCnt);
	return slice;
}
