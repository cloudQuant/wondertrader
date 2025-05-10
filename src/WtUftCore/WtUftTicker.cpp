/*!
 * \file WtUftTicker.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief UFT引擎的实时时间控制器实现文件
 * \details 实现了WtUftRtTicker类，负责处理超高频交易引擎的实时时间控制和行情数据处理
 */
#include "WtUftTicker.h"
#include "WtUftEngine.h"
#include "../Includes/IDataReader.h"

#include "../Share/TimeUtils.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/IBaseDataMgr.h"

#include "../WTSTools/WTSLogger.h"

USING_NS_WTP;


/**
 * @brief 构造函数
 * @param engine UFT引擎指针
 * @details 初始化实时时间控制器对象，设置初始状态和参数
 */
WtUftRtTicker::WtUftRtTicker(WtUftEngine* engine)
	: _engine(engine)
	, _stopped(false)
	, _date(0)
	, _time(UINT_MAX)
	, _next_check_time(0)
	, _last_emit_pos(0)
	, _cur_pos(0)
{
}


/**
 * @brief 析构函数
 * @details 清理实时时间控制器资源
 */
WtUftRtTicker::~WtUftRtTicker()
{
}

/**
 * @brief 初始化时间控制器
 * @param sessionID 交易时段ID
 * @details 获取指定交易时段的信息并设置当前日期时间
 */
void WtUftRtTicker::init(const char* sessionID)
{
	// 从引擎中获取指定交易时段的信息
	_s_info = _engine->get_session_info(sessionID);

	// 获取当前系统日期和时间
	TimeUtils::getDateTime(_date, _time);
}

/**
 * @brief 处理Tick数据
 * @param curTick 当前Tick数据指针
 * @details 处理行情数据，设置当前时间，触发分钟线结束事件，并计算下次检查时间
 */
void WtUftRtTicker::on_tick(WTSTickData* curTick)
{
	// 如果线程未初始化，直接将Tick数据转发给引擎并返回
	if (_thrd == NULL)
	{
		if (_engine)
			_engine->on_tick(curTick->code(), curTick);
		return;
	}

	// 获取Tick数据的日期和时间
	uint32_t uDate = curTick->actiondate();
	uint32_t uTime = curTick->actiontime();

	// 如果行情时间早于当前时间，直接处理并返回
	if (_date != 0 && (uDate < _date || (uDate == _date && uTime < _time)))
	{
		//WTSLogger::info("行情时间{}小于本地时间{}", uTime, _time);
		if (_engine)
			_engine->on_tick(curTick->code(), curTick);
		return;
	}

	// 更新当前日期和时间
	_date = uDate;
	_time = uTime;

	// 计算当前分钟和秒数
	uint32_t curMin = _time / 100000;
	uint32_t curSec = _time % 100000;
	// 将当前时间转换为交易时段中的分钟位置
	uint32_t minutes = _s_info->timeToMinutes(curMin);
	// 判断是否是交易时段的最后一分钟
	bool isSecEnd = _s_info->isLastOfSection(curMin);
	if (isSecEnd)
	{
		minutes--;
	}
	minutes++;
	// 保存原始分钟时间并将交易时段分钟位置转回实际时间
	uint32_t rawMin = curMin;
	curMin = _s_info->minuteToTime(minutes);

	// 如果是首次设置当前分钟位置
	if (_cur_pos == 0)
	{
		//如果当前时间是0, 则直接赋值即可
		_cur_pos = minutes;
	}
	// 如果时间向前跃进，需要触发分钟线结束事件
	else if (_cur_pos < minutes)
	{
		//如果已记录的分钟小于新的分钟, 则需要触发闭合事件
		//这个时候要先触发闭合, 再修改平台时间和价格
		if (_last_emit_pos < _cur_pos)
		{
			//触发数据回放模块
			StdUniqueLock lock(_mtx);

			//优先修改时间标记
			_last_emit_pos = _cur_pos;

			// 获取当前分钟位置对应的实际时间
			uint32_t thisMin = _s_info->minuteToTime(_cur_pos);

			// 输出日志并触发分钟线结束事件
			WTSLogger::info("Minute Bar {}.{:04d} Closed by data", _date, thisMin);
			_engine->on_minute_end(_date, thisMin);
		}
			
		// 更新引擎时间并触发Tick数据
		if (_engine)
		{
			_engine->on_tick(curTick->code(), curTick);
			_engine->set_date_time(_date, curMin, curSec, rawMin);
			_engine->set_trading_date(curTick->tradingdate());
		}

		// 更新当前分钟位置
		_cur_pos = minutes;
	}
	// 如果在同一分钟内，直接处理Tick数据
	else
	{
		//如果分钟数还是一致的, 则直接触发行情和时间即可
		if (_engine)
		{
			_engine->on_tick(curTick->code(), curTick);
			_engine->set_date_time(_date, curMin, curSec, rawMin);
		}
	}

	// 计算距离下一分钟的剩余时间（毫秒）
	uint32_t sec = curSec / 1000;
	uint32_t msec = curSec % 1000;
	uint32_t left_ticks = (60 - sec) * 1000 - msec;
	// 设置下次检查时间
	_next_check_time = TimeUtils::getLocalTimeNow() + left_ticks;
}

/**
 * @brief 启动时间控制器
 * @details 初始化并启动一个工作线程，负责在交易时间内自动检查并触发分钟线结束事件
 */
void WtUftRtTicker::run()
{
	// 如果线程已存在，直接返回
	if (_thrd)
		return;

	// 执行引擎初始化
	_engine->on_init();

	// 计算当前交易日期并设置到引擎中
	uint32_t curTDate = _engine->get_basedata_mgr()->calcTradingDate(_s_info->id(), _engine->get_date(), _engine->get_min_time(), true);
	_engine->set_trading_date(curTDate);

	// 触发交易日开始事件
	_engine->on_session_begin();

	// 获取当前时间在交易时段中的偏移值
	uint32_t offTime = _s_info->offsetTime(_engine->get_min_time(), true);

	// 创建并启动工作线程
	_thrd.reset(new StdThread([this, offTime](){
		// 循环运行直到被停止
		while (!_stopped)
		{
			// 如果当前时间在交易时间内
			if (_time != UINT_MAX && _s_info->isInTradingTime(_time / 100000, true))
			{
				// 在交易时间内每10毫秒检查一次
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				uint64_t now = TimeUtils::getLocalTimeNow();

				// 如果到达下次检查时间且当前分钟位置尚未触发结束事件
				if (now >= _next_check_time && _last_emit_pos < _cur_pos)
				{
					// 触发数据回放模块，加锁保护
					StdUniqueLock lock(_mtx);

					// 更新上次触发位置
					_last_emit_pos = _cur_pos;

					// 获取当前分钟位置对应的实际时间
					uint32_t thisMin = _s_info->minuteToTime(_cur_pos);
					_time = thisMin;

					// 处理日期切换（如果时间是0点）
					if (thisMin == 0)
					{
						uint32_t lastDate = _date;
						_date = TimeUtils::getNextDate(_date);
						_time = 0;
						WTSLogger::info("Data automatically changed at time 00:00: {} -> {}", lastDate, _date);
					}

					// 输出日志并触发分钟线结束事件
					WTSLogger::info("Minute bar {}.{:04d} closed automatically", _date, thisMin);
					//if (_store)
					//	_store->onMinuteEnd(_date, thisMin);

					_engine->on_minute_end(_date, thisMin);

					// 如果当前时间是交易时段的收盤时间，触发交易日结束事件
					uint32_t offMin = _s_info->offsetTime(thisMin, true);
					if (offMin >= _s_info->getCloseTime(true))
					{
						_engine->on_session_end();
					}

					// 更新引擎时间
					if (_engine)
						_engine->set_date_time(_date, thisMin, 0);
				}
			}
			else //if (offTime >= _s_info->getOpenTime(true) && offTime <= _s_info->getCloseTime(true))
			{
				// 不在交易时间，则休息10s再进行检查
				// 因为这个逻辑是处理分钟线的，所以休盤时间休息10s，不会引起数据踏空的问题
				std::this_thread::sleep_for(std::chrono::seconds(10));
			}
			
		}
	}));
}

/**
 * @brief 停止时间控制器
 * @details 停止工作线程并等待其完成
 */
void WtUftRtTicker::stop()
{
	// 设置停止标志
	_stopped = true;
	// 如果线程存在，等待其结束
	if (_thrd)
		_thrd->join();
}
