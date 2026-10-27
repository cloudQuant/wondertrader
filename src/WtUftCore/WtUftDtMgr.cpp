/*!
 * \file WtUftDtMgr.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief UFT数据管理器实现文件
 * \details 实现了WtUftDtMgr类的各种方法，包括数据管理、缓存和访问等功能
 */
#include "WtUftDtMgr.h"
#include "WtUftEngine.h"
#include "WtHelper.h"

#include "../Share/StrUtil.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSVariant.hpp"

#include "../WTSTools/WTSLogger.h"
#include "../WTSTools/WTSDataFactory.h"


/**
 * @brief 全局数据工厂对象
 * @details 用于创建和管理数据对象
 */
WTSDataFactory g_dataFact;

/**
 * @brief 构造函数
 * @details 初始化所有成员变量为Null
 */
WtUftDtMgr::WtUftDtMgr()
	: _engine(NULL)
	, _bars_cache(NULL)
	, _ticks_cache(NULL)
	, _rt_tick_map(NULL)
{
}


/**
 * @brief 析构函数
 * @details 释放所有缓存相关的资源
 */
WtUftDtMgr::~WtUftDtMgr()
{
	if (_bars_cache)
		_bars_cache->release();

	if (_ticks_cache)
		_ticks_cache->release();

	if (_rt_tick_map)
		_rt_tick_map->release();
}

/**
 * @brief 初始化数据管理器
 * @param cfg 配置项变量集
 * @param engine UFT引擎指针
 * @return 初始化是否成功
 * @details 存储引擎指针并完成初始化设置
 */
bool WtUftDtMgr::init(WTSVariant* cfg, WtUftEngine* engine)
{
	_engine = engine;

	return true;
}

/**
 * @brief 处理实时推送的行情数据
 * @param stdCode 标准化的合约代码
 * @param newTick 新的Tick数据指针
 * @details 将新的Tick数据添加到实时缓存中，并在必要时更新历史数据
 */
void WtUftDtMgr::handle_push_quote(const char* stdCode, WTSTickData* newTick)
{
	if (newTick == NULL)
		return;

	if (_rt_tick_map == NULL)
		_rt_tick_map = DataCacheMap::create();

	_rt_tick_map->add(stdCode, newTick, true);

	if(_ticks_cache != NULL)
	{
		WTSHisTickData* tData = (WTSHisTickData*)_ticks_cache->get(stdCode);
		if (tData == NULL)
			return;

		// 如果只需要有效数据且当前成交量为0，则跳过
		if (tData->isValidOnly() && newTick->volume() == 0)
			return;

		// 将新的Tick数据追加到历史数据中
		tData->appendTick(newTick->getTickStruct());
	}
}

/**
 * @brief 获取最新的Tick数据
 * @param code 合约代码
 * @return 最新的Tick数据指针，如果没有则返回NULL
 * @details 从实时tick缓存中获取指定合约的最新行情数据
 */
WTSTickData* WtUftDtMgr::grab_last_tick(const char* code)
{
	if (_rt_tick_map == NULL)
		return NULL;

	WTSTickData* curTick = (WTSTickData*)_rt_tick_map->grab(code);
	if (curTick == NULL)
		return NULL;

	return curTick;
}


/**
 * @brief 获取Tick切片数据
 * @param stdCode 标准化的合约代码
 * @param count 请求的数据条数
 * @param etime 结束时间，默认为0（当前时间）
 * @return Tick切片数据指针，当前实现返回NULL
 * @details 当前实现为空，需要在后续版本中实现
 */
WTSTickSlice* WtUftDtMgr::get_tick_slice(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */)
{
	return NULL;
}

/**
 * @brief 获取委托队列切片数据
 * @param stdCode 标准化的合约代码
 * @param count 请求的数据条数
 * @param etime 结束时间，默认为0（当前时间）
 * @return 委托队列切片数据指针，当前实现返回NULL
 * @details 当前实现为空，需要在后续版本中实现
 */
WTSOrdQueSlice* WtUftDtMgr::get_order_queue_slice(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */)
{
	return NULL;
}

/**
 * @brief 获取委托明细切片数据
 * @param stdCode 标准化的合约代码
 * @param count 请求的数据条数
 * @param etime 结束时间，默认为0（当前时间）
 * @return 委托明细切片数据指针，当前实现返回NULL
 * @details 当前实现为空，需要在后续版本中实现
 */
WTSOrdDtlSlice* WtUftDtMgr::get_order_detail_slice(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */)
{
	return NULL;
}

/**
 * @brief 获取成交切片数据
 * @param stdCode 标准化的合约代码
 * @param count 请求的数据条数
 * @param etime 结束时间，默认为0（当前时间）
 * @return 成交切片数据指针，当前实现返回NULL
 * @details 当前实现为空，需要在后续版本中实现
 */
WTSTransSlice* WtUftDtMgr::get_transaction_slice(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */)
{
	return NULL;
}

/**
 * @brief 获取K线切片数据
 * @param stdCode 标准化的合约代码
 * @param period K线周期
 * @param times 周期倍数
 * @param count 请求的数据条数
 * @param etime 结束时间，默认为0（当前时间）
 * @return K线切片数据指针，当前实现返回NULL
 * @details 当前实现为空，需要在后续版本中实现
 */
WTSKlineSlice* WtUftDtMgr::get_kline_slice(const char* stdCode, WTSKlinePeriod period, uint32_t times, uint32_t count, uint64_t etime /* = 0 */)
{
	return NULL;
}
