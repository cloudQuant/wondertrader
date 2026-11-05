/*!
 * \file WtCtaTicker.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "WtCtaTicker.h"
#include "TickerDayBoundary.h"
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
#include "../Includes/DateMappingClient.hpp"

USING_NS_WTP;

namespace
{
	bool isSessionCloseTime(WTSSessionInfo* session, uint32_t minute)
	{
		const uint32_t closeTime = session->getCloseTime(true);
		return session->offsetTime(minute, true) == closeTime ||
			(minute == 2400 && closeTime == 2400);
	}
}

//////////////////////////////////////////////////////////////////////////
//WtTimeTicker
void WtCtaRtTicker::init(IDataReader* store, const char* sessionID)
{
	_store = store;
	_s_info = _engine->get_session_info(sessionID);
	if(_s_info == NULL)
		WTSLogger::fatal("Session {} is invalid, CtaTicker cannot run correctly", sessionID);
	else
		WTSLogger::info("CtaTicker will drive engine with session {}", sessionID);

	uint32_t curDate = 0, curTime = 0;
	TimeUtils::getDateTime(curDate, curTime);
	_date = curDate;
	_time = curTime;
}

void WtCtaRtTicker::trigger_price(WTSTickData* curTick)
{
	if (_engine )
	{
		WTSContractInfo* cInfo = curTick->getContractInfo();
		std::string stdCode = curTick->code();
		_engine->on_tick(stdCode.c_str(), curTick);

		// I03-NEW-094: a tick without contract metadata has no hot-code fan-out.
		if (cInfo != NULL && !cInfo->isFlat())
		{
			auto hotCodes = cInfo->getHotCodes();
			for(const std::string& hotCode : hotCodes)
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
}

void WtCtaRtTicker::on_tick(WTSTickData* curTick)
{
	if (curTick == NULL)
		return;

	//卡9 9d=A：owner 线程内（策略/引擎/数据回调中）的同步重入排队，在当前事件返回后的安全点投递
	if (_strand.enterEvent(curTick, 0) == wt_ticker::EventStrand::Entry::Queued)
		return;

	wt_ticker::runAndDrain(_strand, [this, curTick]() { handle_tick(curTick); },
		[this](wtp::WTSObject* obj, uint32_t) { handle_tick(static_cast<WTSTickData*>(obj)); });
}

void WtCtaRtTicker::handle_tick(WTSTickData* curTick)
{
	if (!_running.load(std::memory_order_acquire))
	{
		trigger_price(curTick);
		return;
	}

	uint32_t uDate = curTick->actiondate();
	uint32_t uTime = curTick->actiontime();

	//卡3 3a=A：23:59:60 等非法时刻是非法输入，拒绝且不推进 ticker 状态
	if (!wt_ticker::isValidTickTime(uTime))
	{
		WTSLogger::error("Tick of {} with invalid action time {}.{} rejected", curTick->code(), uDate, uTime);
		return;
	}

	if (_date != 0 && (uDate < _date || (uDate == _date && uTime < _time)))
	{
		//WTSLogger::info("行情时间{}小于本地时间{}", uTime, _time);
		trigger_price(curTick);
		return;
	}

	//卡3 3a=A：新交易日首 tick，先闭合旧日未发布的分钟（标签保留 2400），再把分钟位置归零
	const uint32_t tickTDate = curTick->tradingdate();
	const bool bNewTDate = _engine != NULL && wt_ticker::isNewTradingDate(_cur_pos, tickTDate, _engine->getTradingDate());
	if (bNewTDate)
	{
		if (_last_emit_pos < _cur_pos)
		{
			_last_emit_pos = _cur_pos;
			uint32_t thisMin = _s_info->minuteToTime(_cur_pos);
			bool bEndingTDate = isSessionCloseTime(_s_info, thisMin);
			WTSLogger::info("Minute Bar {}.{:04d} Closed by new tradingday data", _date.load(), thisMin);
			if (_store)
				_store->onMinuteEnd(_date, thisMin, bEndingTDate ? _engine->getTradingDate() : 0);
			_engine->on_schedule(_date, thisMin);
			if (bEndingTDate)
				_engine->on_session_end();
		}
		WTSLogger::info("Tradingday {} -> {} by data at {}.{}, ticker position reset", _engine->getTradingDate(), tickTDate, uDate, uTime);
		_cur_pos = 0;
		_last_emit_pos = 0;
	}

	_date = uDate;
	_time = uTime;

	uint32_t curMin = _time / 100000;
	uint32_t curSec = _time % 100000;


	// Keep the minute mapping local to this ticker/session. Function-static cache
	// state can be reused by another ticker with a different session definition.
	uint32_t minutes = _s_info->timeToMinutes(curMin);
	//卡3 3a=A：全天时段 00:00 同时等于收盘 2400 的取模值；位置 0 是新日开始，不能回退（防 uint 下溢）
	if (_s_info->isLastOfSection(curMin) && minutes > 0)
		minutes--;
	minutes++;
	uint32_t wrapMin = _s_info->minuteToTime(minutes);

	if (_cur_pos == 0)
	{
		//如果当前时间是0, 则直接赋值即可
		//卡3 3a=A：首 tick（含新交易日首 tick）同样推进引擎时间并交给引擎，不丢弃
		if (_engine)
		{
			_engine->set_date_time(_date, wrapMin, curSec, curMin);
			if (tickTDate != 0 && tickTDate > _engine->getTradingDate())
				_engine->set_trading_date(tickTDate);
		}
		trigger_price(curTick);
		_cur_pos = minutes;
	}
	else if (_cur_pos < minutes)
	{
		//如果已记录的分钟小于新的分钟, 则需要触发闭合事件
		//这个时候要先触发闭合, 再修改平台时间和价格
		if (_last_emit_pos < _cur_pos)
		{
			//触发数据回放模块（已在 _strand owner 内串行）
			//优先修改时间标记
			_last_emit_pos = _cur_pos;

			uint32_t thisMin = _s_info->minuteToTime(_cur_pos);
			
			bool bEndingTDate = false;
			if (isSessionCloseTime(_s_info, thisMin))
				bEndingTDate = true;

			WTSLogger::info("Minute Bar {}.{:04d} Closed by data", _date.load(), thisMin);
			if (_store)
				_store->onMinuteEnd(_date, thisMin, bEndingTDate ? _engine->getTradingDate() : 0);

			//任务调度
			_engine->on_schedule(_date, thisMin);

			if(bEndingTDate)
				_engine->on_session_end();
		}

		//By Wesley @ 2022.02.09
		//这里先修改时间，再调用trigger_price
		//无论分钟线是否切换，先修改时间都是对的
		if (_engine)
		{
			_engine->set_date_time(_date, wrapMin, curSec, curMin);
			_engine->set_trading_date(curTick->tradingdate());
		}
		trigger_price(curTick);

		_cur_pos = minutes;
	}
	else
	{
		//如果分钟数还是一致的, 则直接触发行情和时间即可
		trigger_price(curTick);
		if (_engine)
			_engine->set_date_time(_date, wrapMin, curSec, curMin);
	}

	uint32_t sec = curSec / 1000;
	uint32_t msec = curSec % 1000;
	uint32_t left_ticks = (60 - sec) * 1000 - msec;
	_next_check_time = TimeUtils::getLocalTimeNow() + left_ticks;
}

bool WtCtaRtTicker::run()
{
	if (_thrd)
		return true;

	// I03-NEW-075: without a trading session the ticker cannot compute the
	// trading date or drive minutes; refuse before initializing the engine.
	if (_s_info == NULL || _engine == NULL || _engine->get_basedata_mgr() == NULL)
	{
		WTSLogger::error("CtaTicker cannot run: trading session or base data is not available");
		return false;
	}

	/*
	 *	By Wesley @ 2022.12.06
	 *	这里一定要在初始化之前把交易日确定下来
	 *	不然如果策略在on_init的时候调用一些依赖交易日的接口就会出错
	 */
	// BUG-R12-13 (DateMappingV1, card 1 A+C): an unmappable trading date is an explicit
	// failure; the ticker refuses to start instead of driving the engine with a sentinel.
	wtp::DateMappingClient dateMapping(_engine->get_basedata_mgr());
	const DateMappingResult32 mappedTDate = dateMapping.calcTradingDate(_s_info->id(), _engine->get_date(), _engine->get_min_time(), true);
	if (mappedTDate.status != DateMappingStatus::Ok)
	{
		WTSLogger::error("CtaTicker cannot run: trading date of session {} cannot be mapped ({})",
			_s_info->id(), wtp::dateMappingStatusName(mappedTDate.status));
		return false;
	}
	uint32_t curTDate = mappedTDate.value;
	_engine->set_trading_date(curTDate);
	WTSLogger::info("Trading date confirmed: {}", curTDate);
	_engine->on_init();
	_engine->on_session_begin();

	_running.store(true, std::memory_order_release);
	_thrd.reset(new StdThread([this](){
		while (!_stopped.load(std::memory_order_acquire))
		{
			//卡9 9d=A：worker 与 on_tick 串行；worker 回调内的同步重入同样排队并在本轮结束前投递
			if (!_strand.enterWorker(_stopped))
				break;

			bool busyPolling = true;
			wt_ticker::runAndDrain(_strand, [this, &busyPolling]() { busyPolling = worker_pass(); },
				[this](wtp::WTSObject* obj, uint32_t) { handle_tick(static_cast<WTSTickData*>(obj)); });

			_strand.sleepFor(busyPolling ? std::chrono::milliseconds(10) : std::chrono::milliseconds(10000), _stopped);
		}
	}));
	return true;
}

bool WtCtaRtTicker::worker_pass()
{
	uint32_t offTime = _s_info->offsetTime(_engine->get_min_time(), true);

	if (_time != UINT_MAX && _s_info->isInTradingTime(_time / 100000, true))
	{
		uint64_t now = TimeUtils::getLocalTimeNow();

		if (now >= _next_check_time && _last_emit_pos < _cur_pos)
		{
			//优先修改时间标记
			_last_emit_pos = _cur_pos;

			uint32_t thisMin = _s_info->minuteToTime(_cur_pos);
			_time = thisMin*100000;//这里要还原成毫秒为单位

			//如果thisMin是0, 说明换日了
			//这里是本地计时导致的换日, 说明日期其实还是老日期, 要自动+1
			//同时因为时间是235959xxx, 所以也要手动置为0
			if (thisMin == 0)
			{
				uint32_t lastDate = _date;
				_date = TimeUtils::getNextDate(lastDate);
				_time = 0;
				WTSLogger::info("Data automatically changed at time 00:00: {} -> {}", lastDate, _date.load());
			}

			bool bEndingTDate = false;
			if (isSessionCloseTime(_s_info, thisMin))
				bEndingTDate = true;

			WTSLogger::info("Minute bar {}.{:04d} closed automatically", _date.load(), thisMin);
			if (_store)
				_store->onMinuteEnd(_date, thisMin, bEndingTDate ? _engine->getTradingDate() : 0);

			//任务调度
			_engine->on_schedule(_date, thisMin);

			if (bEndingTDate)
				_engine->on_session_end();

			//145959000
			if (_engine)
				_engine->set_date_time(_date, thisMin, 0);
		}
		return true;
	}

	//收盘以后，如果发现上次触发的位置不等于总的分钟数，说明少了最后一分钟的闭合逻辑
	uint32_t total_mins = _s_info->getTradingMins();
	if(_time != UINT_MAX && _last_emit_pos != 0 && _last_emit_pos < total_mins && offTime >= _s_info->getCloseTime(true))
	{
		WTSLogger::warn("Tradingday {} will be ended forcely, last_emit_pos: {}, time: {}", _engine->getTradingDate(), _last_emit_pos.load(), _time.load());

		//优先修改时间标记
		_last_emit_pos = total_mins;

		uint32_t thisMin = _s_info->getCloseTime(false);

		WTSLogger::info("Minute bar {}.{:04d} closed automatically", _date.load(), thisMin);
		if (_store)
			_store->onMinuteEnd(_date, thisMin, _engine->getTradingDate());

		//任务调度
		_engine->on_schedule(_date, thisMin);

		_engine->on_session_end();
		return true;
	}

	return false;
}

void WtCtaRtTicker::stop()
{
	_stopped.store(true, std::memory_order_release);
	_strand.notifyAll();

	StdThreadPtr worker = _thrd;
	if (!worker)
		return;

	//卡9 9d=A：worker（回调内）调用 stop 只发停止请求，不自 join；由 owner 之外的 stop/析构负责 join
	if (worker->get_id() == std::this_thread::get_id())
		return;

	//非 owner 调用方：有界等待当前事件与排队事件排空
	if (!_strand.isOwner() && !_strand.waitIdle(wt_ticker::STOP_DRAIN_TIMEOUT))
		WTSLogger::warn("CtaTicker stop: in-flight event or queued ticks not drained within {} ms, joining worker",
			(long long)wt_ticker::STOP_DRAIN_TIMEOUT.count());

	StdUniqueLock lock(_stop_mtx);
	if (worker->joinable())
		worker->join();
}

bool WtCtaRtTicker::is_in_trading() const 
{
	if (_s_info == NULL)
		return false;

	return _s_info->isInTradingTime(_time/100000, true);
}

uint32_t WtCtaRtTicker::time_to_mins(uint32_t uTime) const
{
	if (_s_info == NULL)
		return uTime;

	return _s_info->timeToMinutes(uTime, true);
}
