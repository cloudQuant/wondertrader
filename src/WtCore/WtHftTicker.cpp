/*!
 * \file WtHftTicker.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 高频交易行情定时器实现文件
 * \details 实现了高频交易行情定时器类，包括行情处理、分钟线间隔触发等功能
 */
#include "WtHftTicker.h"
#include "WtHftEngine.h"
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
 * @brief 高频交易实时行情定时器构造函数
 * @param engine 高频交易引擎指针
 */
WtHftRtTicker::WtHftRtTicker(WtHftEngine* engine)
	: _engine(engine)     // 初始化引擎指针
	, _stopped(false)     // 初始化停止标志为否
	, _date(0)            // 初始化日期为0
	, _time(UINT_MAX)     // 初始化时间为最大值
	, _next_check_time(0)  // 初始化下次检查时间为0
	, _last_emit_pos(0)    // 初始化最后触发位置为0
	, _cur_pos(0)         // 初始化当前位置为0
{
}


/**
 * @brief 高频交易实时行情定时器析构函数
 */
WtHftRtTicker::~WtHftRtTicker()
{
}

/**
 * @brief 初始化定时器
 * @param store 数据读取器指针
 * @param sessionID 交易时段ID
 */
void WtHftRtTicker::init(IDataReader* store, const char* sessionID)
{
	// 保存数据读取器指针
	_store = store;
	// 从引擎中获取交易时段信息
	_s_info = _engine->get_session_info(sessionID);

	// 获取当前日期和时间
	TimeUtils::getDateTime(_date, _time);
}

/**
 * @brief 触发行情处理
 * @param curTick 当前行情数据指针
 * @details 将行情数据传递给高频交易引擎处理，并处理主力合约的行情
 */
void WtHftRtTicker::trigger_price(WTSTickData* curTick)
{
	if (_engine)
	{
		// 获取合约信息
		WTSContractInfo* cInfo = curTick->getContractInfo();
		// 获取标准化的合约代码
		std::string stdCode = curTick->code();
		// 将行情传递给引擎处理
		_engine->on_tick(stdCode.c_str(), curTick);

		// 如果不是平台合约，还需要处理主力合约的行情
		if (!cInfo->isFlat())
		{
			// 创建主力合约的行情数据
			WTSTickData* hotTick = WTSTickData::create(curTick->getTickStruct());
			// 获取主力合约代码
			const char* hotCode = cInfo->getHotCode();
			// 设置主力合约代码
			hotTick->setCode(hotCode);
			// 将主力合约行情传递给引擎处理
			_engine->on_tick(hotCode, hotTick);
			// 释放主力合约行情数据
			hotTick->release();
		}
	}
}

/**
 * @brief 处理实时行情数据
 * @param curTick 当前行情数据指针
 * @details 处理实时行情数据，包括时间检查、分钟线间隔判断和触发等
 */
void WtHftRtTicker::on_tick(WTSTickData* curTick)
{
	// 如果定时器线程未启动，直接触发行情处理
	if (_thrd == NULL)
	{
		trigger_price(curTick);
		return;
	}

	// 获取行情的日期和时间
	uint32_t uDate = curTick->actiondate();
	uint32_t uTime = curTick->actiontime();

	// 如果行情时间早于当前时间，直接触发行情处理
	if (_date != 0 && (uDate < _date || (uDate == _date && uTime < _time)))
	{
		//WTSLogger::info("行情时间{}小于本地时间{}", uTime, _time);
		trigger_price(curTick);
		return;
	}

	// 更新当前日期和时间
	_date = uDate;
	_time = uTime;

	// 分离分钟和秒数
	uint32_t curMin = _time / 100000;
	uint32_t curSec = _time % 100000;
	// 将时间转换为分钟序号
	uint32_t minutes = _s_info->timeToMinutes(curMin);
	// 判断是否是交易时段的最后一分钟
	bool isSecEnd = _s_info->isLastOfSection(curMin);
	if (isSecEnd)
	{
		minutes--;
	}
	minutes++;
	uint32_t rawMin = curMin;
	// 将分钟序号转换回时间格式
	curMin = _s_info->minuteToTime(minutes);

	if (_cur_pos == 0)
	{
		// 如果当前时间是0, 则直接赋值即可
		_cur_pos = minutes;
	}
	else if (_cur_pos < minutes)
	{
		// 如果已记录的分钟小于新的分钟, 则需要触发闭合事件
		// 这个时候要先触发闭合, 再修改平台时间和价格
		if (_last_emit_pos < _cur_pos)
		{
			// 触发数据回放模块
			StdUniqueLock lock(_mtx);

			// 优先修改时间标记
			_last_emit_pos = _cur_pos;

			uint32_t thisMin = _s_info->minuteToTime(_cur_pos);

			WTSLogger::info("Minute Bar {}.{:04d} Closed by data", _date, thisMin);
			// 通知数据存储器分钟线结束
			if (_store)
				_store->onMinuteEnd(_date, thisMin);

			// 通知引擎分钟线结束
			_engine->on_minute_end(_date, thisMin);

			// 判断是否是交易时段结束
			uint32_t offMin = _s_info->offsetTime(thisMin, true);
			if (offMin == _s_info->getCloseTime(true))
			{
				// 触发交易时段结束事件
				_engine->on_session_end();
			}
		}

		// 触发行情处理
		trigger_price(curTick);
		if (_engine)
		{
			// 设置引擎当前日期时间
			_engine->set_date_time(_date, curMin, curSec, rawMin);
			// 设置引擎交易日期
			_engine->set_trading_date(curTick->tradingdate());
		}

		// 更新当前分钟位置
		_cur_pos = minutes;
	}
	else
	{
		// 如果分钟数还是一致的, 则直接触发行情和时间即可
		trigger_price(curTick);
		if (_engine)
			_engine->set_date_time(_date, curMin, curSec, rawMin);
	}

	// 计算距离下一分钟的时间
	uint32_t sec = curSec / 1000;
	uint32_t msec = curSec % 1000;
	uint32_t left_ticks = (60 - sec) * 1000 - msec;
	// 设置下次检查时间
	_next_check_time = TimeUtils::getLocalTimeNow() + left_ticks;
}

/**
 * @brief 启动定时器
 * @details 创建并启动定时器线程，处理定时事件和分钟线间隔触发
 */
void WtHftRtTicker::run()
{
	// 如果线程已存在，直接返回
	if (_thrd)
		return;

	// 计算当前交易日期
	uint32_t curTDate = _engine->get_basedata_mgr()->calcTradingDate(_s_info->id(), _engine->get_date(), _engine->get_min_time(), true);
	// 设置引擎的交易日期
	_engine->set_trading_date(curTDate);

	// 触发引擎初始化事件
	_engine->on_init();

	// 触发交易时段开始事件
	_engine->on_session_begin();

	// 先检查当前时间
	uint32_t offTime = _s_info->offsetTime(_engine->get_min_time(), true);

	// 创建并启动定时器线程
	_thrd.reset(new StdThread([this, offTime](){
		while (!_stopped)
		{
			// 如果当前时间在交易时间内
			if (_time != UINT_MAX && _s_info->isInTradingTime(_time / 100000, true))
			{
				// 短暂停以减少CPU占用
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				// 获取当前时间
				uint64_t now = TimeUtils::getLocalTimeNow();

				// 如果到达下次检查时间且最后触发位置小于当前位置
				if (now >= _next_check_time && _last_emit_pos < _cur_pos)
				{
					// 触发数据回放模块
					StdUniqueLock lock(_mtx);

					// 优先修改时间标记
					_last_emit_pos = _cur_pos;

					// 获取当前分钟时间
					uint32_t thisMin = _s_info->minuteToTime(_cur_pos);
					_time = thisMin;

					// 如果thisMin是0, 说明换日了
					// 这里是本地计时导致的换日, 说明日期其实还是老日期, 要自动+1
					// 同时因为时间是235959xxx, 所以也要手动置为0
					if (thisMin == 0)
					{
						uint32_t lastDate = _date;
						// 获取下一日日期
						_date = TimeUtils::getNextDate(_date);
						_time = 0;
						WTSLogger::info("Data automatically changed at time 00:00: {} -> {}", lastDate, _date);
					}

					WTSLogger::info("Minute bar {}.{:04d} closed automatically", _date, thisMin);
					// 通知数据存储器分钟线结束
					if (_store)
						_store->onMinuteEnd(_date, thisMin);

					// 通知引擎分钟线结束
					_engine->on_minute_end(_date, thisMin);

					// 判断是否到达交易时段结束时间
					uint32_t offMin = _s_info->offsetTime(thisMin, true);
					if (offMin >= _s_info->getCloseTime(true))
					{
						// 触发交易时段结束事件
						_engine->on_session_end();
					}

					// 设置引擎当前日期时间
					if (_engine)
						_engine->set_date_time(_date, thisMin, 0);
				}
			}
			else // 非交易时间处理
			{
				// 收盘以后，如果发现上次触发的位置不等于总的分钟数，说明少了最后一分钟的闭合逻辑
				uint32_t total_mins = _s_info->getTradingMins();
				if (_time != UINT_MAX && _last_emit_pos != 0 && _last_emit_pos < total_mins && offTime >= _s_info->getCloseTime(true))
				{
					WTSLogger::warn("Tradingday {} will be ended forcely, last_emit_pos: {}, time: {}", _engine->getTradingDate(), _last_emit_pos.fetch_add(0), _time);

					// 触发数据回放模块
					StdUniqueLock lock(_mtx);

					// 优先修改时间标记
					_last_emit_pos = total_mins;

					bool bEndingTDate = true;
					// 获取收盘时间
					uint32_t thisMin = _s_info->getCloseTime(false);
					uint32_t offMin = _s_info->getCloseTime(true);

					WTSLogger::info("Minute bar {}.{:04d} closed automatically", _date, thisMin);
					// 通知数据存储器分钟线结束
					if (_store)
						_store->onMinuteEnd(_date, thisMin, _engine->getTradingDate());

					// 触发交易时段结束事件
					_engine->on_session_end();

				}
				else
				{
					// 非交易时间内休眠时间更长
					std::this_thread::sleep_for(std::chrono::seconds(10));
				}
			}
			
		}
	}));
}

/**
 * @brief 停止定时器
 * @details 停止定时器线程并等待其结束
 */
void WtHftRtTicker::stop()
{
	// 设置停止标志
	_stopped = true;
	// 如果线程存在，等待其结束
	if (_thrd)
		_thrd->join();
}
