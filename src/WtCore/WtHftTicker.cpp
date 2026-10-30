/*!
 * \file WtHftTicker.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
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

#include <exception>

USING_NS_WTP;

namespace
{
	bool isSessionCloseTime(WTSSessionInfo* session, uint32_t minute)
	{
		const uint32_t closeTime = session->getCloseTime(true);
		return session->offsetTime(minute, true) == closeTime ||
			(minute == 2400 && closeTime == 2400);
	}

	thread_local WtHftRtTicker* s_hft_worker_ticker = nullptr;
}


WtHftRtTicker::WtHftRtTicker(WtHftEngine* engine)
	: _engine(engine)
	, _date(0)
	, _time(UINT_MAX)
	, _cur_pos(0)
, _next_check_time(0)
	, _last_emit_pos(0)
	, _stopped(false)
	, _in_session_pass_count(0)
	, _lifecycle_state(LifecycleState::Idle)
	, _join_waiter_count(0)
	{
}


WtHftRtTicker::~WtHftRtTicker()
{
	if (s_hft_worker_ticker != nullptr)
		std::terminate();

	{
		std::lock_guard<std::mutex> lock(_lifecycle_mtx);
		if (_lifecycle_state == LifecycleState::Starting &&
			_run_owner_id == std::this_thread::get_id())
			std::terminate();
	}

	stop();
}

void WtHftRtTicker::init(IDataReader* store, const char* sessionID)
{
	_store = store;
	_s_info = _engine->get_session_info(sessionID);

	TimeUtils::getDateTime(_date, _time);
}

void WtHftRtTicker::trigger_price(WTSTickData* curTick)
{
	if (_engine)
	{
		WTSContractInfo* cInfo = curTick->getContractInfo();
		std::string stdCode = curTick->code();
		_engine->on_tick(stdCode.c_str(), curTick);

		auto hotCodes = cInfo->getHotCodes();
		for (const std::string& hotCode : hotCodes)
		{
			WTSTickData* hotTick = WTSTickData::create(curTick->getTickStruct());
			if (hotTick == NULL)
				continue;
			if (!hotTick->trySetCode(hotCode.c_str()))
			{
				hotTick->release();
				continue;
			}
			_engine->on_tick(hotCode.c_str(), hotTick);
			hotTick->release();
		}
	}
}

void WtHftRtTicker::on_tick(WTSTickData* curTick)
{
	bool hasWorkerThread = false;
	{
		std::lock_guard<std::mutex> lock(_lifecycle_mtx);
		hasWorkerThread = _thrd != NULL;
	}
	if (!hasWorkerThread)
	{
		trigger_price(curTick);
		return;
	}

	uint32_t uDate = curTick->actiondate();
	uint32_t uTime = curTick->actiontime();

	if (_date != 0 && (uDate < _date || (uDate == _date && uTime < _time)))
	{
		//WTSLogger::info("行情时间{}小于本地时间{}", uTime, _time);
		trigger_price(curTick);
		return;
	}

	_date = uDate;
	_time = uTime;

	uint32_t curMin = _time / 100000;
	uint32_t curSec = _time % 100000;
	uint32_t minutes = _s_info->timeToMinutes(curMin);
	bool isSecEnd = _s_info->isLastOfSection(curMin);
	if (isSecEnd)
	{
		minutes--;
	}
	minutes++;
	uint32_t rawMin = curMin;
	curMin = _s_info->minuteToTime(minutes);

	if (_cur_pos == 0)
	{
		//如果当前时间是0, 则直接赋值即可
		_cur_pos = minutes;
	}
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

			uint32_t thisMin = _s_info->minuteToTime(_cur_pos);

			WTSLogger::info("Minute Bar {}.{:04d} Closed by data", _date, thisMin);
			if (_store)
				_store->onMinuteEnd(_date, thisMin);

			_engine->on_minute_end(_date, thisMin);

			if (isSessionCloseTime(_s_info, thisMin))
			{
				_engine->on_session_end();
			}
		}

		trigger_price(curTick);
		if (_engine)
		{
			_engine->set_date_time(_date, curMin, curSec, rawMin);
			_engine->set_trading_date(curTick->tradingdate());
		}

		_cur_pos = minutes;
	}
	else
	{
		//如果分钟数还是一致的, 则直接触发行情和时间即可
		trigger_price(curTick);
		if (_engine)
			_engine->set_date_time(_date, curMin, curSec, rawMin);
	}

	uint32_t sec = curSec / 1000;
	uint32_t msec = curSec % 1000;
	uint32_t left_ticks = (60 - sec) * 1000 - msec;
	_next_check_time = TimeUtils::getLocalTimeNow() + left_ticks;
}

void WtHftRtTicker::run()
{
	{
		std::lock_guard<std::mutex> lock(_lifecycle_mtx);
		if (_lifecycle_state != LifecycleState::Idle)
			return;
		if (_stopped.load(std::memory_order_acquire))
		{
			_lifecycle_state = LifecycleState::Joined;
			_lifecycle_cv.notify_all();
			return;
		}

		_lifecycle_state = LifecycleState::Starting;
		_run_owner_id = std::this_thread::get_id();
	}

	auto finishStoppedStartup = [this]() {
		std::lock_guard<std::mutex> lock(_lifecycle_mtx);
		_run_owner_id = std::thread::id();
		_lifecycle_state = LifecycleState::Joined;
		_lifecycle_cv.notify_all();
	};

	try
	{
		uint32_t curTDate = _engine->get_basedata_mgr()->calcTradingDate(_s_info->id(), _engine->get_date(), _engine->get_min_time(), true);
		_engine->set_trading_date(curTDate);
		if (_stopped.load(std::memory_order_acquire))
		{
			finishStoppedStartup();
			return;
		}

		_engine->on_init();
		if (_stopped.load(std::memory_order_acquire))
		{
			finishStoppedStartup();
			return;
		}

		_engine->on_session_begin();

		uint32_t offTime = _s_info->offsetTime(_engine->get_min_time(), true);

		std::unique_lock<std::mutex> lock(_lifecycle_mtx);
		_run_owner_id = std::thread::id();
		if (_stopped.load(std::memory_order_acquire))
		{
			_lifecycle_state = LifecycleState::Joined;
		}
		else
		{
			_thrd = std::make_shared<StdThread>([this, offTime]() {
				s_hft_worker_ticker = this;
				while (!_stopped.load(std::memory_order_acquire))
				{
					if (_time != UINT_MAX && _s_info->isInTradingTime(_time / 100000, true))
					{
						uint8_t passCount = _in_session_pass_count.load(std::memory_order_relaxed);
						if (passCount < 2)
							_in_session_pass_count.store(passCount + 1, std::memory_order_release);

						std::this_thread::sleep_for(std::chrono::milliseconds(10));
						uint64_t now = TimeUtils::getLocalTimeNow();

						if (now >= _next_check_time && _last_emit_pos < _cur_pos)
						{
							StdUniqueLock dataLock(_mtx);
							_last_emit_pos = _cur_pos;

							uint32_t thisMin = _s_info->minuteToTime(_cur_pos);
							// _time uses HHMMSSmmm throughout the ticker. Keep the
							// auto-close value in the same unit used by on_tick() and
							// isInTradingTime(_time / 100000).
							_time = thisMin * 100000;

							if (thisMin == 0)
							{
								uint32_t lastDate = _date;
								_date = TimeUtils::getNextDate(_date);
								_time = 0;
								WTSLogger::info("Data automatically changed at time 00:00: {} -> {}", lastDate, _date);
							}

							WTSLogger::info("Minute bar {}.{:04d} closed automatically", _date, thisMin);
							if (_store)
								_store->onMinuteEnd(_date, thisMin);

							_engine->on_minute_end(_date, thisMin);

							uint32_t offMin = _s_info->offsetTime(thisMin, true);
							if (offMin >= _s_info->getCloseTime(true) ||
								isSessionCloseTime(_s_info, thisMin))
								_engine->on_session_end();

							if (_engine)
								_engine->set_date_time(_date, thisMin, 0);
						}
					}
					else
					{
						uint32_t total_mins = _s_info->getTradingMins();
						if (_time != UINT_MAX && _last_emit_pos != 0 && _last_emit_pos < total_mins && offTime >= _s_info->getCloseTime(true))
						{
							WTSLogger::warn("Tradingday {} will be ended forcely, last_emit_pos: {}, time: {}", _engine->getTradingDate(), _last_emit_pos.fetch_add(0), _time);
							StdUniqueLock dataLock(_mtx);
							_last_emit_pos = total_mins;

							[[maybe_unused]] bool bEndingTDate = true;
							uint32_t thisMin = _s_info->getCloseTime(false);
							[[maybe_unused]] uint32_t offMin = _s_info->getCloseTime(true);

							WTSLogger::info("Minute bar {}.{:04d} closed automatically", _date, thisMin);
							if (_store)
								_store->onMinuteEnd(_date, thisMin, _engine->getTradingDate());

							_engine->on_session_end();
						}
						else
						{
							std::this_thread::sleep_for(std::chrono::seconds(10));
						}
					}
				}
				s_hft_worker_ticker = nullptr;
			});
			_lifecycle_state = LifecycleState::Running;
		}

		_lifecycle_cv.notify_all();
	}
	catch (...)
	{
		std::lock_guard<std::mutex> lock(_lifecycle_mtx);
		if (_lifecycle_state == LifecycleState::Starting)
		{
			_run_owner_id = std::thread::id();
			_lifecycle_state = LifecycleState::Joined;
		}
		_lifecycle_cv.notify_all();
		throw;
	}
}

void WtHftRtTicker::stop()
{
	_stopped.store(true, std::memory_order_release);

	if (s_hft_worker_ticker != nullptr)
		return;

	std::unique_lock<std::mutex> lock(_lifecycle_mtx);
	if (_lifecycle_state == LifecycleState::Starting &&
		_run_owner_id == std::this_thread::get_id())
		return;

	_lifecycle_cv.wait(lock, [this]() {
		return _lifecycle_state != LifecycleState::Starting;
	});

	if (_lifecycle_state == LifecycleState::Idle)
	{
		_lifecycle_state = LifecycleState::Joined;
		_lifecycle_cv.notify_all();
		return;
	}

	if (_lifecycle_state == LifecycleState::Joined)
	{
		_lifecycle_cv.wait(lock, [this]() {
			return _join_waiter_count == 0;
		});
		return;
	}

	if (_lifecycle_state == LifecycleState::Joining)
	{
		++_join_waiter_count;
		_lifecycle_cv.wait(lock, [this]() {
			return _lifecycle_state == LifecycleState::Joined;
		});
		--_join_waiter_count;
		_lifecycle_cv.notify_all();
		return;
	}

	_lifecycle_state = LifecycleState::Joining;
	StdThreadPtr worker = _thrd;
	lock.unlock();

	try
	{
		if (worker && worker->joinable())
			worker->join();
	}
	catch (...)
	{
		std::terminate();
	}

	lock.lock();
	_lifecycle_state = LifecycleState::Joined;
	_lifecycle_cv.notify_all();
}
