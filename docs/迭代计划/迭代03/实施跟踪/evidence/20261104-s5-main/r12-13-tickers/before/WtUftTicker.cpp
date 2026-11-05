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

#include <exception>

USING_NS_WTP;

namespace
{
	struct TickerCallbackFrame
	{
		WtUftRtTicker* ticker;
		TickerCallbackFrame* previous;
	};
	thread_local TickerCallbackFrame* active_ticker_callback = nullptr;

	class TickerCallbackScope
	{
	public:
		explicit TickerCallbackScope(WtUftRtTicker* ticker)
			: _frame{ ticker, active_ticker_callback }
		{
			active_ticker_callback = &_frame;
		}

		~TickerCallbackScope()
		{
			active_ticker_callback = _frame.previous;
		}

	private:
		TickerCallbackFrame _frame;
	};
}


/**
 * @brief 构造函数
 * @param engine UFT引擎指针
 * @details 初始化实时时间控制器对象，设置初始状态和参数
 */
WtUftRtTicker::WtUftRtTicker(WtUftEngine* engine)
	: _s_info(NULL)
	, _engine(engine)
	, _init_attempted(false)
	, _date(0)
	, _time(UINT_MAX)
	, _cur_pos(0)
	, _next_check_time(0)
	, _last_emit_pos(0)
	, _stopped(false)
	, _worker_started(false)
	, _run_thread_id()
	, _worker_thread_id()
	, _run_active(false)
	{
}


/**
 * @brief 析构函数
 * @details 清理实时时间控制器资源
 */
WtUftRtTicker::~WtUftRtTicker()
{
	request_stop();
	if (!wait_stopped())
		std::terminate();
}

/**
 * @brief 初始化时间控制器
 * @param sessionID 交易时段ID
 * @details 获取指定交易时段的信息并设置当前日期时间
 */
void WtUftRtTicker::init(const char* sessionID)
{
	_init_attempted = true;
	_s_info = NULL;
	if (_engine == NULL || sessionID == NULL || sessionID[0] == '\0')
	{
		WTSLogger::error("UFT ticker cannot initialize with an empty session ID");
		return;
	}

	// 从引擎中获取指定交易时段的信息
	_s_info = _engine->get_session_info(sessionID);
	if (_s_info == NULL)
	{
		WTSLogger::error("UFT ticker session '{}' was not found", sessionID);
		return;
	}

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
	std::lock_guard<std::mutex> callback_lock(_callback_mtx);
	if (_stopped.load(std::memory_order_acquire)
		|| _engine == NULL || _engine->_stop_requested.load(std::memory_order_seq_cst))
		return;

	// 已尝试初始化但交易时段无效时，拒绝行情，避免在无时间上下文下触发策略。
	if (_init_attempted && _s_info == NULL)
		return;

	TickerCallbackScope callback_scope(this);

	// 如果线程未初始化，直接将Tick数据转发给引擎并返回
	if (!_worker_started.load(std::memory_order_acquire))
	{
		if (_engine)
			_engine->on_tick(curTick->code(), curTick);
		return;
	}

	// 获取Tick数据的日期和时间
	uint32_t uDate = curTick->actiondate();
	uint32_t uTime = curTick->actiontime();

	// 启动时本地时钟可能已晚于同日第一条行情；尚未建立分钟位置时以
	// 该行情初始化时钟。分钟位置建立后，迟到行情只转发，不得回拨时钟。
	const bool first_quote_before_startup_clock = _cur_pos == 0 && _date == uDate;
	if (!first_quote_before_startup_clock && _date != 0
		&& (uDate < _date || (uDate == _date && uTime < _time)))
	{
		//WTSLogger::info("行情时间{}小于本地时间{}", uTime, _time);
		if (_engine)
			_engine->on_tick(curTick->code(), curTick);
		return;
	}

	// 计算当前分钟和秒数
	uint32_t curMin = uTime / 100000;
	uint32_t curSec = uTime % 100000;
	// 将当前时间转换为交易时段中的分钟位置
	uint32_t minutes = _s_info->timeToMinutes(curMin);
	if (minutes == INVALID_UINT32)
	{
		// 休市行情仍向引擎/策略转发，但不能更改时钟或分钟闭合基线。
		if (_engine)
			_engine->on_tick(curTick->code(), curTick);
		return;
	}

	// 只有合法的交易时段映射才能推进 ticker 时钟和分钟位置。
	_date = uDate;
	_time = uTime;

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

	// 首笔有效行情同时初始化分钟位置并送入引擎；不能只用于建立时钟基线。
	if (_cur_pos == 0)
	{
		//如果当前时间是0, 则直接赋值即可
		_cur_pos = minutes;
		if (_engine)
		{
			_engine->set_date_time(_date, curMin, curSec, rawMin);
			_engine->set_trading_date(curTick->tradingdate());
			_engine->on_tick(curTick->code(), curTick);
		}
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
	notify_worker();
}

/**
 * @brief 启动时间控制器
 * @details 初始化并启动一个工作线程，负责在交易时间内自动检查并触发分钟线结束事件
 */
void WtUftRtTicker::run()
{
	if (_engine == NULL || _s_info == NULL
		|| _engine->_stop_requested.load(std::memory_order_seq_cst))
	{
		WTSLogger::error("UFT ticker cannot run without a valid session");
		return;
	}

	{
		std::lock_guard<std::mutex> lock(_state_mtx);
		if (_stopped.load(std::memory_order_acquire) || _run_active || (_thrd && _thrd->joinable()))
			return;
		_run_active = true;
		_run_thread_id = std::this_thread::get_id();
	}

	auto finish_run = [this]() {
		std::lock_guard<std::mutex> lock(_state_mtx);
		_run_active = false;
		_run_thread_id = std::thread::id();
		_state_cv.notify_all();
	};

	try
	{
		// Synchronous strategy callbacks can also reenter engine shutdown.
		{
			TickerCallbackScope callback_scope(this);
			_engine->on_init();
		}

		if (!_stopped.load(std::memory_order_acquire)
			&& !_engine->_stop_requested.load(std::memory_order_seq_cst))
		{
			uint32_t curTDate = _engine->get_basedata_mgr()->calcTradingDate(
				_s_info->id(), _engine->get_date(), _engine->get_min_time(), true);
			_engine->set_trading_date(curTDate);

			{
				TickerCallbackScope callback_scope(this);
				_engine->on_session_begin();
			}
		}

		if (!_stopped.load(std::memory_order_acquire)
			&& !_engine->_stop_requested.load(std::memory_order_seq_cst))
		{
			std::lock_guard<std::mutex> lock(_state_mtx);
			if (!_stopped.load(std::memory_order_acquire))
			{
				_worker_started.store(true, std::memory_order_release);
				_thrd.reset(new StdThread([this]() {
					{
						std::lock_guard<std::mutex> state_lock(_state_mtx);
						_worker_thread_id = std::this_thread::get_id();
					}
					_state_cv.notify_all();

					while (!_stopped.load(std::memory_order_acquire)
						&& !_engine->_stop_requested.load(std::memory_order_seq_cst))
					{
						const uint64_t wake_generation = capture_wake_generation();
						bool in_trading_time = false;
						{
							std::lock_guard<std::mutex> callback_lock(_callback_mtx);
							if (_stopped.load(std::memory_order_acquire)
								|| _engine->_stop_requested.load(std::memory_order_seq_cst))
								break;
							in_trading_time = _time != UINT_MAX
								&& _s_info->isInTradingTime(_time / 100000, true);
						}

						const std::chrono::milliseconds interval = in_trading_time
							? std::chrono::milliseconds(10) : std::chrono::milliseconds(10000);
						wait_for_worker_wakeup(wake_generation, interval);

						if (_stopped.load(std::memory_order_acquire)
							|| _engine->_stop_requested.load(std::memory_order_seq_cst))
							break;

						if (in_trading_time)
						{
							uint64_t now = TimeUtils::getLocalTimeNow();
							std::lock_guard<std::mutex> callback_lock(_callback_mtx);
							if (_stopped.load(std::memory_order_acquire)
								|| _engine->_stop_requested.load(std::memory_order_seq_cst))
								break;
							if (_time == UINT_MAX || !_s_info->isInTradingTime(_time / 100000, true)
								|| now < _next_check_time || _last_emit_pos >= _cur_pos)
								continue;
							{
								StdUniqueLock state_lock(_mtx);
								if (now < _next_check_time || _last_emit_pos >= _cur_pos)
									continue;

								_last_emit_pos = _cur_pos;
								uint32_t thisMin = _s_info->minuteToTime(_cur_pos);
								_time = thisMin;
								if (thisMin == 0)
								{
									uint32_t lastDate = _date;
									_date = TimeUtils::getNextDate(_date);
									_time = 0;
									WTSLogger::info("Data automatically changed at time 00:00: {} -> {}", lastDate, _date);
								}

								WTSLogger::info("Minute bar {}.{:04d} closed automatically", _date, thisMin);
								{
									TickerCallbackScope callback_scope(this);
									_engine->on_minute_end(_date, thisMin);

									uint32_t offMin = _s_info->offsetTime(thisMin, true);
									if (offMin >= _s_info->getCloseTime(true))
										_engine->on_session_end();
								}

								_engine->set_date_time(_date, thisMin, 0);
							}
						}
					}

					{
						std::lock_guard<std::mutex> state_lock(_state_mtx);
						_worker_thread_id = std::thread::id();
					}
					_state_cv.notify_all();
				}));
			}
		}
	}
	catch (...)
	{
		_worker_started.store(false, std::memory_order_release);
		finish_run();
		throw;
	}

	finish_run();
}

/**
 * @brief 停止时间控制器
 * @details 停止工作线程并等待其完成
 */
void WtUftRtTicker::stop()
{
	request_stop();
	wait_stopped();
}

void WtUftRtTicker::request_stop()
{
	_stopped.store(true, std::memory_order_release);
	notify_worker();
}

uint64_t WtUftRtTicker::capture_wake_generation()
{
	if (_engine == NULL)
		return 0;
	std::lock_guard<std::mutex> lock(_engine->_ticker_wake_mtx);
	return _engine->_ticker_wake_generation;
}

void WtUftRtTicker::notify_worker()
{
	if (_engine == NULL)
		return;
	{
		std::lock_guard<std::mutex> lock(_engine->_ticker_wake_mtx);
		++_engine->_ticker_wake_generation;
	}
	_engine->_ticker_wake_cv.notify_all();
}

bool WtUftRtTicker::wait_for_worker_wakeup(uint64_t generation, std::chrono::milliseconds timeout)
{
	if (_engine == NULL)
		return false;
	std::unique_lock<std::mutex> lock(_engine->_ticker_wake_mtx);
	_engine->_ticker_worker_waiting = true;
	_engine->_ticker_wake_cv.notify_all();
	const bool woke = _engine->_ticker_wake_cv.wait_for(lock, timeout, [this, generation]() {
		return _stopped.load(std::memory_order_acquire)
			|| _engine->_stop_requested.load(std::memory_order_seq_cst)
			|| _engine->_ticker_wake_generation != generation;
	});
	_engine->_ticker_worker_waiting = false;
	_engine->_ticker_wake_cv.notify_all();
	return woke;
}

bool WtUftRtTicker::wait_stopped()
{
	const std::thread::id caller = std::this_thread::get_id();
	for (TickerCallbackFrame* frame = active_ticker_callback; frame != nullptr; frame = frame->previous)
	{
		if (frame->ticker == this)
			return false;
	}

	{
		std::lock_guard<std::mutex> state_lock(_state_mtx);
		if ((_run_active && _run_thread_id == caller) || _worker_thread_id == caller)
			return false;
	}

	std::lock_guard<std::mutex> join_lock(_join_mtx);
	std::shared_ptr<StdThread> thread;
	{
		std::unique_lock<std::mutex> state_lock(_state_mtx);
		if ((_run_active && _run_thread_id == caller) || _worker_thread_id == caller)
			return false;
		_state_cv.wait(state_lock, [this]() { return !_run_active; });
		thread = _thrd;
	}

	if (thread && thread->joinable())
	{
		if (thread->get_id() == caller)
			return false;
		thread->join();
	}

	// Joining the worker does not serialize a concurrently entered quote callback.
	// request_stop prevents new callbacks; this lock drains any one already inside.
	std::lock_guard<std::mutex> callback_lock(_callback_mtx);
	return true;
}

bool WtUftRtTicker::request_stop_for_current_callback(WtUftEngine* engine)
{
	for (TickerCallbackFrame* frame = active_ticker_callback; frame != nullptr; frame = frame->previous)
	{
		if (frame->ticker->_engine == engine)
		{
			frame->ticker->request_stop();
			return true;
		}
	}
	return false;
}
