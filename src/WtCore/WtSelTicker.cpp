/*!
* \file WtSelTicker.cpp
* \project	WonderTrader
*
* \author Wesley
* \date 2020/03/30
*
* \brief 选股策略实时行情触发器实现
*/
#include "WtSelTicker.h"
#include "WtSelEngine.h"
#include "../Includes/IDataReader.h"

#include "../Share/TimeUtils.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/IHotMgr.h"
#include "../Share/CodeHelper.hpp"

#include "../WTSTools/WTSLogger.h"

USING_NS_WTP;


/**
 * @brief 选股策略实时行情触发器构造函数
 * @param engine 选股策略引擎指针
 * @details 初始化触发器的成员变量，设置引擎指针并将状态变量初始化为默认值
 */
WtSelRtTicker::WtSelRtTicker(WtSelEngine* engine)
	: _engine(engine)       // 设置策略引擎指针
	, _stopped(false)       // 初始化为未停止状态
	, _date(0)              // 初始化日期为0
	, _time(UINT_MAX)       // 初始化时间为最大值，表示未设置
	, _next_check_time(0)   // 初始化下次检查时间为0
	, _last_emit_pos(0)     // 初始化最后触发位置为0
	, _cur_pos(0)           // 初始化当前位置为0
{
	// 构造函数中没有其它逻辑，仅初始化列表
}


/**
 * @brief 选股策略实时行情触发器析构函数
 * @details 释放资源并清理内存
 */
WtSelRtTicker::~WtSelRtTicker()
{
	// 析构函数中没有逻辑，因为成员指针会自动清理
}

/**
 * @brief 初始化选股策略行情触发器
 * @param store 数据读取器指针，用于存储和读取数据
 * @param sessionID 交易时段ID，用于获取对应的交易时段信息
 * @details 初始化触发器，设置数据存储和交易时段信息，并获取当前的日期和时间
 */
void WtSelRtTicker::init(IDataReader* store, const char* sessionID)
{
	// 设置数据读取器
	_store = store;
	// 从选股引擎中获取指定的交易时段信息
	_s_info = _engine->get_session_info(sessionID);

	// 获取当前的日期和时间
	TimeUtils::getDateTime(_date, _time);
}

/**
 * @brief 触发行情处理
 * @param curTick 当前行情数据
 * @param hotFlag 主力合约标记，默认为0
 * @details 将行情数据转发给选股策略引擎，并处理主力合约的特殊逻辑
 * 如果合约不是平价合约，则还会生成主力合约的行情并转发
 */
void WtSelRtTicker::trigger_price(WTSTickData* curTick, uint32_t hotFlag /* = 0 */)
{
	// 检查引擎指针是否有效
	if (_engine)
	{
		// 获取标准合约代码
		std::string stdCode = curTick->code();
		// 转发行情到选股引擎
		_engine->on_tick(stdCode.c_str(), curTick);

		// 获取合约信息
		WTSContractInfo* cInfo = curTick->getContractInfo();
		// 如果不是平价合约，还需要处理主力合约行情
		if (!cInfo->isFlat())
		{
			// 创建一个新的行情数据对象，使用原始行情的数据结构
			WTSTickData* hotTick = WTSTickData::create(curTick->getTickStruct());
			// 获取主力合约代码
			const char* hotCode = cInfo->getHotCode();
			// 设置主力合约代码
			hotTick->setCode(hotCode);
			// 转发主力合约行情到选股引擎
			_engine->on_tick(hotCode, hotTick);
			// 释放临时创建的行情数据对象
			hotTick->release();
		}
	}
}

/**
 * @brief 处理实时行情数据
 * @param curTick 当前行情数据
 * @param hotFlag 主力合约标记，默认为0
 * @details 处理新到的行情数据，并根据时间检查是否需要触发分钟结束、交易时段结束等事件
 * 该方法主要完成以下工作：
 * 1. 检查行情时间是否合理
 * 2. 处理分钟结束事件
 * 3. 根据需要触发交易时段结束事件
 * 4. 更新时间信息并计算下次检查时间
 */
void WtSelRtTicker::on_tick(WTSTickData* curTick, uint32_t hotFlag /* = 0 */)
{
	// 如果监控线程未启动，直接触发行情处理
	if (_thrd == NULL)
	{
		trigger_price(curTick, hotFlag);
		return;
	}

	// 获取行情数据的日期和时间
	uint32_t uDate = curTick->actiondate();
	uint32_t uTime = curTick->actiontime();

	// 检查行情时间是否小于当前本地时间，如果是则直接处理行情
	if (_date != 0 && (uDate < _date || (uDate == _date && uTime < _time)))
	{
		//WTSLogger::info("行情时间{}小于本地时间{}", uTime, _time);
		trigger_price(curTick, hotFlag);
		return;
	}

	// 更新当前日期和时间
	_date = uDate;
	_time = uTime;

	// 处理分钟级别的时间信息
	// 将时间拆分为分钟和秒数部分
	uint32_t curMin = _time / 100000;
	uint32_t curSec = _time % 100000;
	// 将当前分钟时间转换为相对分钟数
	uint32_t minutes = _s_info->timeToMinutes(curMin);
	// 检查是否是交易时段的最后一分钟
	bool isSecEnd = _s_info->isLastOfSection(curMin);
	if (isSecEnd)
	{
		minutes--;
	}
	minutes++;
	uint32_t rawMin = curMin; // 保存原始分钟时间
	// 将相对分钟数转换回分钟时间格式
	curMin = _s_info->minuteToTime(minutes);

	// 如果当前位置是0，表示第一次设置
	if (_cur_pos == 0)
	{
		// 直接设置当前分钟位置
		_cur_pos = minutes;
	}
	// 如果新的分钟数大于当前分钟数，表示时间前进，需要触发旧分钟的闭合事件
	else if (_cur_pos < minutes)
	{
		// 如果已记录的分钟小于新的分钟，则需要触发闭合事件
		// 这个时候要先触发闭合，再修改平台时间和价格
		if (_last_emit_pos < _cur_pos)
		{
			// 触发数据回放模块
			StdUniqueLock lock(_mtx); // 加锁保护

			// 优先修改时间标记
			_last_emit_pos = _cur_pos;

			// 获取当前分钟的时间格式
			uint32_t thisMin = _s_info->minuteToTime(_cur_pos);

			// 记录日志，分钟线已经闭合
			WTSLogger::info("Minute Bar {}.{:04d} Closed by data", _date, thisMin);
			// 通知数据存储器分钟结束
			if (_store)
				_store->onMinuteEnd(_date, thisMin);

			// 触发选股引擎的分钟结束事件
			_engine->on_minute_end(_date, thisMin);

			// 计算时间偏移量，检查是否是交易时段的结束
			uint32_t offMin = _s_info->offsetTime(thisMin, true);
			if (offMin == _s_info->getCloseTime(true))
			{
				// 触发交易时段结束事件
				_engine->on_session_end();
			}
		}

		// 触发新行情的处理
		trigger_price(curTick, hotFlag);
		// 更新选股引擎的时间和交易日期
		if (_engine)
		{
			_engine->set_date_time(_date, curMin, curSec, rawMin);
			_engine->set_trading_date(curTick->tradingdate());
		}

		// 更新当前分钟位置
		_cur_pos = minutes;
	}
	else
	{
		// 如果分钟数还是一致的，则直接触发行情和时间即可
		trigger_price(curTick, hotFlag);
		if (_engine)
			_engine->set_date_time(_date, curMin, curSec, rawMin);
	}

	// 计算秒数和毫秒数
	uint32_t sec = curSec / 1000;
	uint32_t msec = curSec % 1000;
	// 计算距离下一分钟还有多少毫秒
	uint32_t left_ticks = (60 - sec) * 1000 - msec;
	// 设置下次检查时间
	_next_check_time = TimeUtils::getLocalTimeNow() + left_ticks;
}

/**
 * @brief 启动选股策略实时行情触发器
 * @details 创建并启动一个实时监控线程，监控时间变化并触发分钟结束等事件
 * 该方法主要执行以下操作：
 * 1. 计算当前交易日期并初始化选股引擎
 * 2. 触发交易时段开始事件
 * 3. 创建并启动监控线程，持续监控时间并触发相应事件
 */
void WtSelRtTicker::run()
{
	// 如果线程已经存在，直接返回
	if (_thrd)
		return;

	// 从基础数据管理器获取当前交易日期
	uint32_t curTDate = _engine->get_basedata_mgr()->calcTradingDate(_s_info->id(), _engine->get_date(), _engine->get_min_time(), true);
	// 设置选股引擎的交易日期
	_engine->set_trading_date(curTDate);

	// 触发选股引擎的初始化事件
	_engine->on_init();

	// 触发交易时段开始事件
	_engine->on_session_begin();

	// 先检查当前时间
	//uint32_t offTime = _s_info->offsetTime(_engine->get_min_time());

	// 创建并启动监控线程
	_thrd.reset(new StdThread([this](){
		// 循环运行直到触发器停止
		while (!_stopped)
		{
			// 如果当前时间有效并且在交易时间内
			if (_time != UINT_MAX && _s_info->isInTradingTime(_time / 100000, true))
			{
				// 等待10毫秒再检查
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				// 获取当前时间
				uint64_t now = TimeUtils::getLocalTimeNow();

				// 检查是否到达下次检查时间并且最近的分钟结束还未触发
				if (now >= _next_check_time && _last_emit_pos < _cur_pos)
				{
					// 触发数据回放模块
					StdUniqueLock lock(_mtx);

					// 优先修改时间标记
					_last_emit_pos = _cur_pos;

					// 获取当前分钟的时间格式
					uint32_t thisMin = _s_info->minuteToTime(_cur_pos);
					_time = thisMin;

					// 如果thisMin是0，表示时间进入下一天
					// 这里是本地计时导致的换日，需要自动将日期+1
					// 同时因为时间是235959xxx，所以也要手动置为0
					if (thisMin == 0)
					{
						// 记录旧日期
						uint32_t lastDate = _date;
						// 获取并设置下一天的日期
						_date = TimeUtils::getNextDate(_date);
						// 重置时间为0
						_time = 0;
						// 记录日期变更日志
						WTSLogger::info("Data automatically changed at time 00:00: {} -> {}", lastDate, _date);
					}

					// 记录分钟线自动闭合日志
					WTSLogger::info("Minute bar {}.{:04d} closed automatically", _date, thisMin);
					// 通知数据存储器分钟结束
					if (_store)
						_store->onMinuteEnd(_date, thisMin);

					// 触发选股引擎的分钟结束事件
					_engine->on_minute_end(_date, thisMin);

					// 计算时间偏移量，检查是否到达交易时段结束时间
					uint32_t offMin = _s_info->offsetTime(thisMin, true);
					if (offMin >= _s_info->getCloseTime(true))
					{
						// 触发交易时段结束事件
						_engine->on_session_end();
					}

					// 145959000 - 设置时间，秒数置为0
					if (_engine)
						_engine->set_date_time(_date, thisMin, 0);
				}
			}
			else
			{// 如果不在交易时间，则每隐10毫秒检查一次，如果分钟发生变化则触发
				// 等待10毫秒
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				// 获取当前分钟时间
				uint32_t curTime = TimeUtils::getCurMin();
				// 检查当前时间是否有效并且分钟发生变化
				if (_time != UINT_MAX && curTime != _time)
				{
					// 触发旧分钟的结束事件
					_engine->on_minute_end(_date, _time);
					// 如果当前时间小于旧时间，说明日期已经变化，需要将日期增加1天
					if (curTime < _time)
						_date = TimeUtils::getNextDate(_date);
					// 更新当前时间
					_time = curTime;
				}
			}
		}
	}));
}

/**
 * @brief 停止选股策略实时行情触发器
 * @details 设置停止标志并等待监控线程结束，实现安全停止
 */
void WtSelRtTicker::stop()
{
	// 设置停止标志，后台线程检测到该标志后会退出循环
	_stopped = true;
	// 如果线程存在，等待其结束
	if (_thrd)
		_thrd->join();
}
