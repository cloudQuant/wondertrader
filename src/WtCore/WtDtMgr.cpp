/*!
 * \file WtDtMgr.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 数据管理器实现文件
 * 
 * 本文件实现了WonderTrader的数据管理器，负责管理历史数据和实时行情数据
 * 包括K线、Tick、委托队列、成交明细等数据的获取和缓存
 * 是WonderTrader中数据访问的核心组件
 */
#include "WtDtMgr.h"
#include "WtEngine.h"
#include "WtHelper.h"

#include "../Share/StrUtil.hpp"
#include "../Share/CodeHelper.hpp"

#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSVariant.hpp"

#include "../WTSTools/WTSLogger.h"
#include "../WTSTools/WTSDataFactory.h"


WTSDataFactory g_dataFact;

/**
 * @brief 构造函数
 * @details 初始化数据管理器对象，将所有指针成员初始化为NULL，并设置默认的缓存标志
 */
WtDtMgr::WtDtMgr()
	: _reader(NULL)
	, _engine(NULL)
	, _loader(NULL)
	, _bars_cache(NULL)
	, _ticks_adjusted(NULL)
	, _rt_tick_map(NULL)
	, _force_cache(false)
{
	// 构造函数中不需要额外的初始化操作，所有初始化已在初始化列表中完成
}


/**
 * @brief 析构函数
 * @details 清理数据管理器对象的资源，释放缓存的数据对象
 * 注意：此处只释放了缓存对象，但没有释放_reader和_loader，可能是因为这些对象的所有权在外部
 */
WtDtMgr::~WtDtMgr()
{
	// 释放K线缓存
	if (_bars_cache)
		_bars_cache->release();

	// 释放复权Tick缓存
	if (_ticks_adjusted)
		_ticks_adjusted->release();

	// 释放实时Tick缓存
	if (_rt_tick_map)
		_rt_tick_map->release();

	// 注意：这里没有释放_reader和_loader，可能是因为这些对象的所有权在外部
}

/**
 * @brief 初始化数据存储模块
 * @param cfg 配置项
 * @return bool 初始化是否成功
 * @details 根据配置加载数据存储模块，并创建数据读取器实例
 */
bool WtDtMgr::initStore(WTSVariant* cfg)
{
	// 检查配置是否为空
	if (cfg == NULL)
		return false;

	// 获取数据存储模块名称
	std::string module = cfg->getCString("module");
	// 如果模块名称为空，使用默认的WtDataStorage
	if (module.empty())
		module = WtHelper::getInstDir() + DLLHelper::wrap_module("WtDataStorage");
	else
		module = WtHelper::getInstDir() + DLLHelper::wrap_module(module.c_str());

	// 加载数据存储模块动态库
	DllHandle hInst = DLLHelper::load_library(module.c_str());
	if(hInst == NULL)
	{
		WTSLogger::error("Loading data reader module {} failed", module.c_str());
		return false;
	}

	// 获取创建数据读取器的函数指针
	FuncCreateDataReader funcCreator = (FuncCreateDataReader)DLLHelper::get_symbol(hInst, "createDataReader");
	if(funcCreator == NULL)
	{
		WTSLogger::error("Loading data reader module {} failed, entrance function createDataReader not found", module.c_str());
		DLLHelper::free_library(hInst);
		return false;
	}

	// 创建数据读取器实例
	_reader = funcCreator();
	if(_reader == NULL)
	{
		WTSLogger::error("Creating instance of data reader module {} failed", module.c_str());
		DLLHelper::free_library(hInst);
		return false;
	}

	// 初始化数据读取器，传入配置、当前对象作为回调接收器和历史数据加载器
	_reader->init(cfg, this, _loader);

	return true;
}

/**
 * @brief 初始化数据管理器
 * @param cfg 配置项
 * @param engine 交易引擎指针
 * @param bForceCache 是否强制缓存数据，默认为false
 * @return bool 初始化是否成功
 * @details 根据配置初始化数据管理器，设置引擎关联和数据对齐选项
 */
bool WtDtMgr::init(WTSVariant* cfg, WtEngine* engine, bool bForceCache /* = false */)
{
	// 设置交易引擎指针
	_engine = engine;

	// 从配置中读取是否按小节对齐K线
	_align_by_section = cfg->getBoolean("align_by_section");

	// 设置是否强制缓存K线数据
	_force_cache = bForceCache;

	// 输出小节对齐设置的日志
	WTSLogger::info("Resampled bars will be aligned by section: {}", _align_by_section?"yes":" no");

	// 输出强制缓存设置的日志
	WTSLogger::info("Force to cache bars: {}", _force_cache ? "yes" : " no");

	// 初始化数据存储模块
	return initStore(cfg->get("store"));
}

/**
 * @brief 所有K线数据更新完成回调
 * @param updateTime 更新时间
 * @details 当所有K线数据更新完成时，触发所有等待的K线通知
 * 这个方法将所有等待的K线更新通知传递给交易引擎，然后清空通知列表
 */
void WtDtMgr::on_all_bar_updated(uint32_t updateTime)
{
	// 如果没有等待的通知，直接返回
	if (_bar_notifies.empty())
		return;

	// 输出调试日志
	WTSLogger::debug("All bars updated, on_bar will be triggered");

	// 遍历所有等待的K线通知项，触发交易引擎的on_bar回调
	for (const NotifyItem& item : _bar_notifies)
	{
		_engine->on_bar(item._code, item._period, item._times, item._newBar);
	}

	// 清空通知列表，准备下一轮更新
	_bar_notifies.clear();
}

/**
 * @brief 获取基础数据管理器
 * @return IBaseDataMgr* 基础数据管理器指针
 * @details 代理方法，返回交易引擎的基础数据管理器
 */
IBaseDataMgr* WtDtMgr::get_basedata_mgr()
{ 
	// 直接调用交易引擎的方法获取基础数据管理器
	return _engine->get_basedata_mgr(); 
}

/**
 * @brief 获取主力合约管理器
 * @return IHotMgr* 主力合约管理器指针
 * @details 代理方法，返回交易引擎的主力合约管理器
 */
IHotMgr* WtDtMgr::get_hot_mgr() 
{ 
	// 直接调用交易引擎的方法获取主力合约管理器
	return _engine->get_hot_mgr(); 
}

/**
 * @brief 获取当前交易日期
 * @return uint32_t 交易日期，格式YYYYMMDD
 * @details 代理方法，返回交易引擎的当前交易日期
 */
uint32_t WtDtMgr::get_date() 
{ 
	// 直接调用交易引擎的方法获取当前交易日期
	return _engine->get_date(); 
}

/**
 * @brief 获取当前交易分钟时间
 * @return uint32_t 分钟时间，格式HHMM
 * @details 代理方法，返回交易引擎的当前交易分钟时间
 */
uint32_t WtDtMgr::get_min_time()
{ 
	// 直接调用交易引擎的方法获取当前交易分钟时间
	return _engine->get_min_time(); 
}

/**
 * @brief 获取当前交易秒数
 * @return uint32_t 秒数，从0点开始的秒数
 * @details 代理方法，返回交易引擎的当前交易秒数
 */
uint32_t WtDtMgr::get_secs() 
{ 
	// 直接调用交易引擎的方法获取当前交易秒数
	return _engine->get_secs(); 
}

/**
 * @brief 记录数据读取器日志
 * @param ll 日志级别
 * @param message 日志消息
 * @details 由数据读取器调用，用于记录数据读取过程中的日志
 * 将数据读取器的日志转发到系统日志器
 */
void WtDtMgr::reader_log(WTSLogLevel ll, const char* message)
{
	// 将消息转发到系统日志器
	WTSLogger::log_raw(ll, message);
}

/**
 * @brief K线数据回调
 * @param code 合约代码
 * @param period K线周期
 * @param newBar 新的K线数据
 * @details 当有新的K线数据生成时，由数据读取器调用此回调函数
 * 处理基础周期和非基础周期的K线数据，并将通知添加到列表中
 */
void WtDtMgr::on_bar(const char* code, WTSKlinePeriod period, WTSBarStruct* newBar)
{
    // 生成合约和周期的键值模式
    std::string key_pattern = fmt::format("{}-{}", code, period);

    // 根据周期类型转换为字符周期和倍数
    char speriod;
    uint32_t times = 1;
    switch (period)
    {
    case KP_Minute1:
        speriod = 'm';
        times = 1;
        break;
    case KP_Minute5:
        speriod = 'm';
        times = 5;
        break;
    default:
        speriod = 'd';
        times = 1;
        break;
    }

    // 如果是订阅的基础周期，则添加到通知列表中
    if(_subed_basic_bars.find(key_pattern) != _subed_basic_bars.end())
    {
        //如果是基础周期, 直接触发on_bar事件
        //_engine->on_bar(code, speriod.c_str(), times, newBar);
        //更新完K线以后, 统一通知交易引擎
        _bar_notifies.emplace_back(NotifyItem(code, speriod, times, newBar));
    }

    //然后再处理非基础周期
    if (_bars_cache == NULL || _bars_cache->size() == 0)
        return;
    
    WTSSessionInfo* sInfo = _engine->get_session_info(code, true);

    for (auto it = _bars_cache->begin(); it != _bars_cache->end(); it++)
    {
        const char* key = it->first.c_str();
        if(memcmp(key, key_pattern.c_str(), key_pattern.size()) != 0)
            continue;

        WTSKlineData* kData = (WTSKlineData*)it->second;
        if(kData->times() != 1)
        {
            g_dataFact.updateKlineData(kData, newBar, sInfo, _align_by_section);
            if (kData->isClosed())
            {
                //如果基础周期K线的时间和自定义周期K线的时间一致, 说明K线关闭了
                //这里也要触发on_bar事件
                WTSBarStruct* lastBar = kData->at(-1);
                //_engine->on_bar(code, speriod.c_str(), times, lastBar);
                //更新完K线以后, 统一通知交易引擎
                _bar_notifies.emplace_back(NotifyItem(code, speriod, times*kData->times(), lastBar));
            }
        }
        else
        {
            //如果是强制缓存的一倍周期，直接压到缓存队列里
            kData->getDataRef().emplace_back(*newBar);
            _bar_notifies.emplace_back(NotifyItem(code, speriod, times, newBar));
        }
    }
}

/**
 * @brief 处理推送的行情数据
 * @param stdCode 标准化合约代码
 * @param newTick 新的Tick数据
 * @details 处理实时推送的行情数据，更新内部缓存并进行必要的数据处理
 * 将新的Tick数据添加到实时缓存和复权缓存中
 */
void WtDtMgr::handle_push_quote(const char* stdCode, WTSTickData* newTick)
{
	// 检查新Tick数据是否有效
	if (newTick == NULL)
		return;

	// 如果实时Tick缓存不存在，创建一个新的
	if (_rt_tick_map == NULL)
		_rt_tick_map = DataCacheMap::create();

	// 将新的Tick数据添加到实时缓存中，并设置为自动释放
	_rt_tick_map->add(stdCode, newTick, true);

	// 如果复权Tick缓存存在，则处理复权数据
	if(_ticks_adjusted != NULL)
	{
		// 获取合约的复权历史数据
		WTSHisTickData* tData = (WTSHisTickData*)_ticks_adjusted->get(stdCode);
		if (tData == NULL)
			return;

		// 如果只处理有效数据且当前数据成交量为0，则跳过
		if (tData->isValidOnly() && newTick->volume() == 0)
			return;

		// 将新的Tick数据添加到复权历史数据中
		tData->appendTick(newTick->getTickStruct());
	}
}

/**
 * @brief 获取实时Tick数据
 * @param code 合约代码
 * @return WTSTickData* Tick数据指针，调用者需要负责释放
 * @details 获取指定合约的实时Tick数据，如果缓存不存在则返回NULL
 */
WTSTickData* WtDtMgr::grab_last_tick(const char* code)
{
	if (_rt_tick_map == NULL)
		return NULL;

	WTSTickData* curTick = (WTSTickData*)_rt_tick_map->get(code);
	if (curTick == NULL)
		return NULL;

	curTick->retain();
	return curTick;
}

/**
 * @brief 获取复权因子
 * @param stdCode 标准化合约代码
 * @param uDate 交易日期
 * @return double 复权因子
 * @details 获取指定合约在指定日期的复权因子，如果数据读取器不存在则返回1.0
 */
double WtDtMgr::get_adjusting_factor(const char* stdCode, uint32_t uDate)
{
	if (_reader)
		return _reader->getAdjFactorByDate(stdCode, uDate);

	return 1.0;
}

/**
 * @brief 获取复权标志
 * @return uint32_t 复权标志
 * @details 获取复权标志，如果数据读取器不存在则返回0
 */
uint32_t WtDtMgr::get_adjusting_flag()
{
	static uint32_t flag = UINT_MAX;
	if(flag == UINT_MAX)
	{
		if (_reader)
			flag = _reader->getAdjustingFlag();
		else
			flag = 0;
	}

	return flag;
}

/**
 * @brief 获取指定合约的实时Tick数据切片
 * @param stdCode 标准化合约代码
 * @param count 需要获取的Tick数量
 * @param etime 事件时间戳
 * @return WTSTickSlice* Tick数据切片指针，调用者需要负责释放
 * @details 获取指定合约在指定时间范围内的实时Tick数据切片
 */
WTSTickSlice* WtDtMgr::get_tick_slice(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */)
{
	if (_reader == NULL)
		return NULL;

	/*
	 *	By Wesley @ 2022.02.11
	 *	这里要重新处理一下
	 *	如果是不复权或者前复权，则直接读取底层的实时缓存即可
	 */
	auto len = strlen(stdCode);
	bool isHFQ = (stdCode[len - 1] == SUFFIX_HFQ);

	//不是后复权，缓存直接用底层缓存
	if(!isHFQ)
		return _reader->readTickSlice(stdCode, count, etime);

	//先转成不带+的标准代码
	std::string pureStdCode(stdCode, len - 1);

	if (_ticks_adjusted == NULL)
		_ticks_adjusted = DataCacheMap::create();

	//如果缓存没有，先重新生成一下缓存
	auto it = _ticks_adjusted->find(pureStdCode);
	if (it == _ticks_adjusted->end())
	{
		//先读取全部tick数据
		double factor = _engine->get_exright_factor(stdCode, NULL);
		WTSTickSlice* slice = _reader->readTickSlice(pureStdCode.c_str(), 999999, etime);
		std::vector<WTSTickStruct> ayTicks;
		ayTicks.resize(slice->size());
		std::size_t offset = 0;
		for (std::size_t bIdx = 0; bIdx < slice->get_block_counts(); bIdx++)
		{
			memcpy(&ayTicks[0] + offset, slice->get_block_addr(bIdx), slice->get_block_size(bIdx) * sizeof(WTSTickStruct));
			offset += slice->get_block_size(bIdx);
		}

		//缓存的数据做一个复权处理
		for (WTSTickStruct& tick : ayTicks)
		{
			tick.price *= factor;
			tick.open *= factor;
			tick.high *= factor;
			tick.low *= factor;
		}

		//添加到缓存中
		WTSHisTickData* hisTick = WTSHisTickData::create(stdCode, false, factor);
		hisTick->getDataRef().swap(ayTicks);
		_ticks_adjusted->add(pureStdCode, hisTick, false);
	}

	WTSHisTickData* hisTick = (WTSHisTickData*)_ticks_adjusted->get(pureStdCode);
	uint32_t curDate, curTime, curSecs;
	if (etime == 0)
	{
		curDate = get_date();
		curTime = get_min_time();
		curSecs = get_secs();

		etime = (uint64_t)curDate * 1000000000 + curTime * 100000 + curSecs;
	}
	else
	{
		//20190807124533900
		curDate = (uint32_t)(etime / 1000000000);
		curTime = (uint32_t)(etime % 1000000000) / 100000;
		curSecs = (uint32_t)(etime % 100000);
	}

	//比较时间的对象
	WTSTickStruct eTick;
	eTick.action_date = curDate;
	eTick.action_time = curTime * 100000 + curSecs;

	auto& ticks = hisTick->getDataRef();

	WTSTickStruct* pTick = std::lower_bound(&ticks.front(), &ticks.back(), eTick, [](const WTSTickStruct& a, const WTSTickStruct& b) {
		if (a.action_date != b.action_date)
			return a.action_date < b.action_date;
		else
			return a.action_time < b.action_time;
	});

	uint32_t eIdx = pTick - &ticks.front();

	//如果光标定位的tick时间比目标时间打, 则全部回退一个
	if (pTick->action_date > eTick.action_date || pTick->action_time > eTick.action_time)
	{
		pTick--;
		eIdx--;
	}

	uint32_t cnt = min(eIdx + 1, count);
	uint32_t sIdx = eIdx + 1 - cnt;
	WTSTickSlice* slice = WTSTickSlice::create(stdCode, &ticks.front() + sIdx, cnt);
	return slice;
}

/**
 * @brief 获取指定合约的实时订单队列数据切片
 * @param stdCode 标准化合约代码
 * @param count 需要获取的订单队列数量
 * @param etime 事件时间戳
 * @return WTSOrdQueSlice* 订单队列数据切片指针，调用者需要负责释放
 * @details 获取指定合约在指定时间范围内的实时订单队列数据切片
 */
WTSOrdQueSlice* WtDtMgr::get_order_queue_slice(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */)
{
	if (_reader == NULL)
		return NULL;

	return _reader->readOrdQueSlice(stdCode, count, etime);
}

/**
 * @brief 获取指定合约的实时订单明细数据切片
 * @param stdCode 标准化合约代码
 * @param count 需要获取的订单明细数量
 * @param etime 事件时间戳
 * @return WTSOrdDtlSlice* 订单明细数据切片指针，调用者需要负责释放
 * @details 获取指定合约在指定时间范围内的实时订单明细数据切片
 */
WTSOrdDtlSlice* WtDtMgr::get_order_detail_slice(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */)
{
	if (_reader == NULL)
		return NULL;

	return _reader->readOrdDtlSlice(stdCode, count, etime);
}

/**
 * @brief 获取指定合约的实时成交数据切片
 * @param stdCode 标准化合约代码
 * @param count 需要获取的成交数量
 * @param etime 事件时间戳
 * @return WTSTransSlice* 成交数据切片指针，调用者需要负责释放
 * @details 获取指定合约在指定时间范围内的实时成交数据切片
 */
WTSTransSlice* WtDtMgr::get_transaction_slice(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */)
{
	if (_reader == NULL)
		return NULL;

	return _reader->readTransSlice(stdCode, count, etime);
}

/**
 * @brief 获取K线数据切片
 * @param stdCode 标准化合约代码
 * @param period K线周期
 * @param times 周期倍数
 * @param count 请求的数据条数
 * @param etime 结束时间，默认为0表示当前时间
 * @return WTSKlineSlice* K线数据切片指针，由调用者负责释放
 * @details 获取指定合约的K线历史数据，包含指定数量的最新数据
 * 支持多周期重采样和缓存机制，可以根据配置决定是否强制缓存
 */
WTSKlineSlice* WtDtMgr::get_kline_slice(const char* stdCode, WTSKlinePeriod period, uint32_t times, uint32_t count, uint64_t etime /* = 0 */)
{
	// 如果数据读取器不存在，返回NULL
	if (_reader == NULL)
		return NULL;

	// 使用线程本地存储创建缓存键值
	thread_local static char key[64] = { 0 };
	fmtutil::format_to(key, "{}-{}", stdCode, (uint32_t)period);

	// 如果不强制缓存，并且重采样倍数为1，则直接读取slice返回
	if (times == 1 && !_force_cache)
	{
		// 将当前合约周期添加到订阅的基础周期集合中
		_subed_basic_bars.insert(key);

		// 直接调用数据读取器获取K线切片
		return _reader->readKlineSlice(stdCode, period, count, etime);
	}

	// 只有非基础周期的会进到下面的步骤
	// 获取合约的交易时段信息
	WTSSessionInfo* sInfo = _engine->get_session_info(stdCode, true);

	// 如果K线缓存不存在，创建一个新的
	if (_bars_cache == NULL)
		_bars_cache = DataCacheMap::create();

	// 生成缓存键，包含合约代码、周期和倍数
	fmtutil::format_to(key, "{}-{}-{}", stdCode, (uint32_t)period, times);

	// 从缓存中获取K线数据
	WTSKlineData* kData = (WTSKlineData*)_bars_cache->get(key);
	// 如果缓存中没有数据或数据不足，需要重新读取
	if (kData == NULL || kData->size() < count)
	{
		// 计算需要读取的原始K线数量
		uint32_t realCount = times==1 ? count: (count*times + times);
		// 读取原始K线数据
		WTSKlineSlice* rawData = _reader->readKlineSlice(stdCode, period, realCount, etime);
		if (rawData != NULL && rawData->size() > 0)
		{
			if(times != 1) // 如果是多周期，需要进行重采样
			{
				// 使用数据工厂提取多周期K线数据
				kData = g_dataFact.extractKlineData(rawData, period, times, sInfo, true, _align_by_section);
			}
			else // 如果是单周期，直接复制数据
			{
				// 创建K线数据对象
				kData = WTSKlineData::create(stdCode, rawData->size());
				kData->setPeriod(period, 1);
				kData->setClosed(true);
				// 复制原始K线数据
				WTSBarStruct* pBar = kData->getDataRef().data();
				for(uint32_t bIdx = 0; bIdx < rawData->get_block_counts(); bIdx++ )
				{
					memcpy(pBar, rawData->get_block_addr(bIdx), sizeof(WTSBarStruct)*rawData->get_block_size(bIdx));
					pBar += rawData->get_block_size(bIdx);
				}
			}
			
			// 释放原始K线数据
			rawData->release();
		}
		else
		{
			return NULL;
		}

		// 如果K线数据创建成功，添加到缓存中
		if (kData)
		{
			_bars_cache->add(key, kData, false);
			// 如果是多周期，输出重采样日志
			if(times != 1)
				WTSLogger::debug("{} bars of {} resampled every {} bars: {} -> {}", 
					PERIOD_NAME[period], stdCode, times, realCount, kData->size());
		}
	}

	/*
	 *	By Wesley @ 2023.03.03
	 *	当多周期K线跨越小节时，如果重启了组合
	 *	这个时候就会在启动的时候拉到一条未闭合的K线
	 *	但是未闭合的K线等一下还会重新推一遍
	 *	所以这里必须要做一个修正
	 *	只处理已经闭合的K线
	 */
	// 计算已闭合的K线数量
	uint32_t closedSz = kData->size();
	if (closedSz > 0 && !kData->isClosed())
		closedSz--; // 如果最后一条未闭合，则不计入

	// 计算起始索引和返回的K线数量
	int32_t sIdx = 0;
	uint32_t rtCnt = min(closedSz, count); // 取请求数量和可用数量的最小值
	sIdx = closedSz - rtCnt; // 计算起始索引
	// 获取起始位置的K线指针
	WTSBarStruct* rtHead = kData->at(sIdx);
	// 创建K线切片并返回
	WTSKlineSlice* slice = WTSKlineSlice::create(stdCode, period, times, rtHead, rtCnt);
	return slice;
}
