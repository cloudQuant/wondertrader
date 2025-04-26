/*!
 * \file WtCtaTicker.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 * 
 * WtCtaTicker类的实现文件，实现了CTA策略引擎的定时器功能
 * 负责策略的定时触发和实时行情处理
 * 包括分钟线关闭、交易日切换、定时任务调度等功能
 */
#include "WtCtaTicker.h"
#include "WtCtaEngine.h"
#include "../Includes/IDataReader.h"

#include "../Share/CodeHelper.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/IHotMgr.h"
#include "../Includes/WTSContractInfo.hpp"

#include "../WTSTools/WTSLogger.h"

USING_NS_WTP;

//////////////////////////////////////////////////////////////////////////
// WtCtaRtTicker
/**
 * @brief 初始化定时器
 * @details 设置数据读取器和交易时间段ID，用于获取交易时间信息
 * 
 * @param store 数据读取器指针，用于读取历史数据和实时数据
 * @param sessionID 交易时间段ID，用于指定交易时间段
 */
void WtCtaRtTicker::init(IDataReader* store, const char* sessionID)
{
	// 设置数据读取器
	_store = store;
	// 从引擎中获取指定的交易时间段信息
	_s_info = _engine->get_session_info(sessionID);
	// 检查交易时间段是否有效
	if(_s_info == NULL)
		WTSLogger::fatal("Session {} is invalid, CtaTicker cannot run correctly", sessionID);
	else
		WTSLogger::info("CtaTicker will drive engine with session {}", sessionID);

	// 获取当前系统日期和时间
	TimeUtils::getDateTime(_date, _time);
}

/**
 * @brief 触发价格事件
 * @details 将实时行情传递给策略引擎，并处理主力合约的行情
 * 
 * @param curTick 当前实时行情数据
 */
void WtCtaRtTicker::trigger_price(WTSTickData* curTick)
{
	if (_engine )
	{
		// 获取合约信息
		WTSContractInfo* cInfo = curTick->getContractInfo();
		// 获取标准化的合约代码
		std::string stdCode = curTick->code();
		// 将行情传递给策略引擎处理
		_engine->on_tick(stdCode.c_str(), curTick);

		// 如果不是平价合约，还需要处理主力合约的行情
		if (!cInfo->isFlat())
		{
			// 创建一个新的行情数据对象，用于主力合约
			WTSTickData* hotTick = WTSTickData::create(curTick->getTickStruct());
			// 获取主力合约代码
			const char* hotCode = cInfo->getHotCode();
			// 设置主力合约代码
			hotTick->setCode(hotCode);
			// 将主力合约行情传递给策略引擎
			_engine->on_tick(hotCode, hotTick);
			// 释放临时创建的行情数据对象
			hotTick->release();
		}
	}
}

/**
 * @brief 处理实时行情
 * @details 处理新收到的实时行情数据，触发相应的策略计算和分钟线关闭事件
 * 
 * @param curTick 当前实时行情数据
 */
void WtCtaRtTicker::on_tick(WTSTickData* curTick)
{
	// 如果定时器线程未启动，直接触发价格事件
	if (_thrd == NULL)
	{
		trigger_price(curTick);
		return;
	}

	// 获取行情的日期和时间
	uint32_t uDate = curTick->actiondate();
	uint32_t uTime = curTick->actiontime();

	// 如果行情时间早于当前时间，只触发价格事件而不更新时间
	if (_date != 0 && (uDate < _date || (uDate == _date && uTime < _time)))
	{
		//WTSLogger::info("行情时间{}小于本地时间{}", uTime, _time);
		trigger_price(curTick);
		return;
	}

	// 更新当前日期和时间
	_date = uDate;
	_time = uTime;

	// 将时间分解为分钟和秒
	uint32_t curMin = _time / 100000;  // 当前分钟，格式为HHMM
	uint32_t curSec = _time % 100000;  // 当前秒数，格式为SS000


	// 静态变量用于记录上一次处理的分钟和相关信息
	static uint32_t prevMin = UINT_MAX;  // 上一次处理的分钟
	static uint32_t minutes = UINT_MAX;   // 转换后的分钟数
	static bool isSecEnd = false;         // 是否是交易时段的最后一分钟
	static uint32_t wrapMin = UINT_MAX;   // 包装后的分钟时间

	//By Wesley @ 2023.11.01
	//如果新的分钟和上一次处理的分钟数不同，才进行处理
	//否则就不用处理，减少一些开销
	if(prevMin != curMin)
	{
		// 将时间转换为从交易日开始的分钟数
		minutes = _s_info->timeToMinutes(curMin);
		// 检查是否是交易时段的最后一分钟
		isSecEnd = _s_info->isLastOfSection(curMin);
		prevMin = curMin;

		// 如果是交易时段的最后一分钟，需要特殊处理
		if (isSecEnd)
		{
			minutes--;
		}
		minutes++;

		// 将分钟数转换回时间格式
		wrapMin = _s_info->minuteToTime(minutes);
	}

	// 如果当前位置是0，说明是第一次收到行情
	if (_cur_pos == 0)
	{
		// 如果当前时间是0, 则直接赋值即可
		_cur_pos = minutes;
	}
	// 如果当前位置小于新的分钟数，说明分钟已经前进
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

			// 将当前位置转换为时间格式
			uint32_t thisMin = _s_info->minuteToTime(_cur_pos);
			
			// 检查是否是交易日结束
			bool bEndingTDate = false;
			uint32_t offMin = _s_info->offsetTime(thisMin, true);
			if (offMin == _s_info->getCloseTime(true))
				bEndingTDate = true;

			// 记录分钟线关闭信息
			WTSLogger::info("Minute Bar {}.{:04d} Closed by data", _date, thisMin);
			// 如果有数据存储器，通知分钟结束
			if (_store)
				_store->onMinuteEnd(_date, thisMin, bEndingTDate ? _engine->getTradingDate() : 0);

			// 触发引擎的定时任务调度
			_engine->on_schedule(_date, thisMin);

			// 如果是交易日结束，触发交易日结束事件
			if(bEndingTDate)
				_engine->on_session_end();
		}

		//By Wesley @ 2022.02.09
		//这里先修改时间，再调用trigger_price
		//无论分钟线是否切换，先修改时间都是对的
		if (_engine)
		{
			// 设置引擎的当前日期和时间
			_engine->set_date_time(_date, wrapMin, curSec, prevMin);
			// 设置引擎的交易日期
			_engine->set_trading_date(curTick->tradingdate());
		}
		// 触发价格事件
		trigger_price(curTick);

		// 更新当前位置
		_cur_pos = minutes;
	}
	else
	{
		// 如果分钟数还是一致的, 则直接触发行情和时间即可
		trigger_price(curTick);
		if (_engine)
			_engine->set_date_time(_date, wrapMin, curSec, prevMin);
	}

	// 计算下一次检查的时间
	uint32_t sec = curSec / 1000;    // 当前秒数
	uint32_t msec = curSec % 1000;   // 当前毫秒数
	// 计算到下一分钟的剩余毫秒数
	uint32_t left_ticks = (60 - sec) * 1000 - msec;
	// 设置下一次检查时间
	_next_check_time = TimeUtils::getLocalTimeNow() + left_ticks;
}

/**
 * @brief 启动定时器
 * @details 初始化并启动定时器线程，处理定时事件和分钟线关闭
 */
void WtCtaRtTicker::run()
{
	// 如果线程已经存在，直接返回
	if (_thrd)
		return;

	// 计算当前的交易日期
	uint32_t curTDate = _engine->get_basedata_mgr()->calcTradingDate(_s_info->id(), _engine->get_date(), _engine->get_min_time(), true);
	// 设置引擎的交易日期
	_engine->set_trading_date(curTDate);
	WTSLogger::info("Trading date confirmed: {}", curTDate);
	// 触发引擎的初始化事件
	_engine->on_init();
	// 触发交易日开始事件
	_engine->on_session_begin();

	// 先检查当前时间, 如果大于

	// 创建并启动定时器线程
	_thrd.reset(new StdThread([this](){
		while(!_stopped)
		{
			// 获取当前时间在交易时间段中的偏移
			uint32_t offTime = _s_info->offsetTime(_engine->get_min_time(), true);

			// 如果当前时间在交易时间内
			if (_time != UINT_MAX && _s_info->isInTradingTime(_time / 100000, true))
			{
				// 在交易时间内，短暂停以减少CPU占用
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				// 获取当前时间
				uint64_t now = TimeUtils::getLocalTimeNow();

				// 如果到了下一次检查时间且有未处理的分钟线
				if (now >= _next_check_time && _last_emit_pos < _cur_pos)
				{
					// 触发数据回放模块
					StdUniqueLock lock(_mtx);

					// 优先修改时间标记
					_last_emit_pos = _cur_pos;

					// 将当前位置转换为时间格式
					uint32_t thisMin = _s_info->minuteToTime(_cur_pos);
					_time = thisMin*100000;  // 这里要还原成毫秒为单位

					// 如果thisMin是0, 说明换日了
					// 这里是本地计时导致的换日, 说明日期其实还是老日期, 要自动+1
					// 同时因为时间是235959xxx, 所以也要手动置为0
					if (thisMin == 0)
					{
						// 记录旧日期并更新为新日期
						uint32_t lastDate = _date;
						_date = TimeUtils::getNextDate(_date);
						_time = 0;
						WTSLogger::info("Data automatically changed at time 00:00: {} -> {}", lastDate, _date);
					}

					// 检查是否是交易日结束
					bool bEndingTDate = false;
					uint32_t offMin = _s_info->offsetTime(thisMin, true);
					if (offMin == _s_info->getCloseTime(true))
						bEndingTDate = true;

					// 记录分钟线自动关闭信息
					WTSLogger::info("Minute bar {}.{:04d} closed automatically", _date, thisMin);
					// 如果有数据存储器，通知分钟结束
					if (_store)
						_store->onMinuteEnd(_date, thisMin, bEndingTDate ? _engine->getTradingDate() : 0);

					// 触发引擎的定时任务调度
					_engine->on_schedule(_date, thisMin);

					// 如果是交易日结束，触发交易日结束事件
					if (bEndingTDate)
						_engine->on_session_end();

					// 设置引擎的当前日期和时间
					if (_engine)
						_engine->set_date_time(_date, thisMin, 0);
				}
			}
			else //if(offTime >= _s_info->getOpenTime(true) && offTime <= _s_info->getCloseTime(true))
			{
				// 收盘以后，如果发现上次触发的位置不等于总的分钟数，说明少了最后一分钟的闭合逻辑
				uint32_t total_mins = _s_info->getTradingMins();
				if(_time != UINT_MAX && _last_emit_pos != 0 && _last_emit_pos < total_mins && offTime >= _s_info->getCloseTime(true))
				{
					// 记录强制结束交易日的警告信息
					WTSLogger::warn("Tradingday {} will be ended forcely, last_emit_pos: {}, time: {}", _engine->getTradingDate(), _last_emit_pos.fetch_add(0), _time);

					// 触发数据回放模块
					StdUniqueLock lock(_mtx);

					// 优先修改时间标记为总分钟数
					_last_emit_pos = total_mins;

					// 设置交易日结束标志
					bool bEndingTDate = true;
					// 获取收盘时间
					uint32_t thisMin = _s_info->getCloseTime(false);
					uint32_t offMin = _s_info->getCloseTime(true);

					// 记录分钟线自动关闭信息
					WTSLogger::info("Minute bar {}.{:04d} closed automatically", _date, thisMin);
					// 如果有数据存储器，通知分钟结束
					if (_store)
						_store->onMinuteEnd(_date, thisMin, _engine->getTradingDate());

					// 触发引擎的定时任务调度
					_engine->on_schedule(_date, thisMin);

					// 触发交易日结束事件
					_engine->on_session_end();

				}
				else
				{
					// 如果不在交易时间内且不需要强制结束，则较长时间暂停
					std::this_thread::sleep_for(std::chrono::seconds(10));
				}
			}
		}
	}));
}

/**
 * @brief 停止定时器
 * @details 停止定时器线程，终止定时触发功能
 */
void WtCtaRtTicker::stop()
{
	// 设置停止标志，通知线程退出
	_stopped = true;
	// 如果线程存在，等待其结束
	if (_thrd)
		_thrd->join();
}

/**
 * @brief 检查是否在交易时间内
 * @details 根据当前时间检查是否在交易时间段内
 * 
 * @return true 在交易时间内
 * @return false 不在交易时间内
 */
bool WtCtaRtTicker::is_in_trading() const 
{
	// 如果交易时间段信息不存在，返回false
	if (_s_info == NULL)
		return false;

	// 调用交易时间段对象的方法检查当前时间是否在交易时间内
	return _s_info->isInTradingTime(_time/100000, true);
}

/**
 * @brief 将时间转换为分钟数
 * @details 将HHMM格式的时间转换为从交易日开始的分钟数
 * 
 * @param uTime 时间，格式为HHMM
 * @return uint32_t 分钟数，从交易日开始的分钟数
 */
uint32_t WtCtaRtTicker::time_to_mins(uint32_t uTime) const
{
	// 如果交易时间段信息不存在，直接返回原始时间
	if (_s_info == NULL)
		return uTime;

	// 调用交易时间段对象的方法将时间转换为分钟数
	return _s_info->timeToMinutes(uTime, true);
}