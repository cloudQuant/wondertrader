#include "WtSelEngine.h"
#include "WtDtMgr.h"
#include "WtSelTicker.h"
#include "TraderAdapter.h"
#include "WtHelper.h"

#include "../WTSTools/WTSLogger.h"
#include "../Share/TimeUtils.hpp"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/IHotMgr.h"
#include "../Share/StrUtil.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Share/CodeHelper.hpp"
#include "../Share/decimal.h"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
namespace rj = rapidjson;

#include <atomic>

USING_NS_WTP;

inline uint32_t makeTaskId()
{
	static std::atomic<uint32_t> _auto_task_id{ 1 };
	return _auto_task_id.fetch_add(1);
}


WtSelEngine::WtSelEngine()
	: _terminated(false)
	, _tm_ticker(NULL)
	, _cfg(NULL)
{
}


WtSelEngine::~WtSelEngine()
{
	release();

	if (_cfg)
	{
		_cfg->release();
		_cfg = NULL;
	}
}

void WtSelEngine::release()
{
	_terminated = true;

	if (_tm_ticker)
	{
		_tm_ticker->stop();
		delete _tm_ticker;
		_tm_ticker = NULL;
	}

	// The ticker no longer dispatches; wait for tasks already started.
	join_schedule_workers(false);

	WtEngine::release();
}

void WtSelEngine::join_schedule_workers(bool onlyFinished)
{
	std::vector<ScheduleWorker> workers;
	{
		std::lock_guard<std::mutex> lock(_sched_mtx);
		if (!onlyFinished)
		{
			workers.swap(_sched_workers);
		}
		else
		{
			auto it = _sched_workers.begin();
			while (it != _sched_workers.end())
			{
				if (it->_done->load(std::memory_order_acquire))
				{
					workers.push_back(*it);
					it = _sched_workers.erase(it);
				}
				else
				{
					++it;
				}
			}
		}
	}

	for (ScheduleWorker& worker : workers)
	{
		if (!worker._thread || !worker._thread->joinable())
			continue;
		if (worker._thread->get_id() == std::this_thread::get_id())
		{
			// release() called from a task's own on_schedule: it cannot join
			// itself. The thread owns its context reference; let it finish.
			WTSLogger::error("SEL engine released from its own schedule task; that task is detached");
			worker._thread->detach();
			continue;
		}
		worker._thread->join();
	}
}

void WtSelEngine::on_session_end()
{
	if (_evt_listener)
		_evt_listener->on_session_event(_cur_tdate, false);
}

void WtSelEngine::on_session_begin()
{
	if (_evt_listener)
		_evt_listener->on_session_event(_cur_tdate, true);

	_ready = true;
}

void WtSelEngine::on_init()
{
	// Every registered SEL context receives on_init when the ticker starts
	// (repeated engine run() is a separate open item, BUG-R10-004), before the initialize event and before any schedule/tick
	// dispatch (mirrors WtCtaEngine::on_init). Contexts are only registered after
	// the runner has created and initialized the whole strategy batch.
	for (auto it = _ctx_map.begin(); it != _ctx_map.end(); it++)
	{
		SelContextPtr& ctx = (SelContextPtr&)it->second;
		ctx->on_init();
	}

	if (_evt_listener)
		_evt_listener->on_initialize_event();
}

void WtSelEngine::handle_push_quote(WTSTickData* curTick)
{
	if (_tm_ticker)
		_tm_ticker->on_tick(curTick);
}

void WtSelEngine::on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar)
{
	// I03-NEW-039: a legal stdCode plus period and times can exceed 64 bytes.
	const std::string key = fmt::format("{}-{}-{}", stdCode, period, times);

	const SubList& sids = _bar_sub_map[key];
	for (auto it = sids.begin(); it != sids.end(); it++)
	{
		uint32_t sid = it->first;
		auto cit = _ctx_map.find(sid);
		if (cit != _ctx_map.end())
		{
			SelContextPtr& ctx = (SelContextPtr&)cit->second;
			ctx->on_bar(stdCode, period, times, newBar);
		}
	}

	WTSLogger::info("KBar [{}] @ {} closed", key, period[0] == 'd' ? newBar->date : newBar->time);
}

void WtSelEngine::on_tick(const char* stdCode, WTSTickData* curTick)
{
	WtEngine::on_tick(stdCode, curTick);

	_data_mgr->handle_push_quote(stdCode, curTick);

	//如果是真实代码, 则要传递给执行器
	{
		_exec_mgr.handle_tick(stdCode, curTick);
	}

	/*
	auto sit = _tick_sub_map.find(stdCode);
	if (sit != _tick_sub_map.end())
	{
		const SubList& sids = sit->second;
		for (auto it = sids.begin(); it != sids.end(); it++)
		{
			uint32_t sid = *it;
			auto cit = _ctx_map.find(sid);
			if (cit != _ctx_map.end() && curTick->volume())
			{
				SelContextPtr& ctx = (SelContextPtr&)cit->second;
				ctx->on_tick(stdCode, curTick);
			}
		}
	}
	*/

	/*
	 *	By Wesley @ 2022.02.07
	 *	这里做了一个彻底的调整
	 *	第一，检查订阅标记，如果标记为0，即无复权模式，则直接按照原始代码触发ontick
	 *	第二，如果标记为1，即前复权模式，则将代码转成xxxx-，再触发ontick
	 *	第三，如果标记为2，即后复权模式，则将代码转成xxxx+，再把tick数据做一个修正，再触发ontick
	 */
	if(_ready)
	{
		auto sit = _tick_sub_map.find(stdCode);
		if (sit != _tick_sub_map.end())
		{
			uint32_t flag = get_adjusting_flag();

			const SubList& sids = sit->second;
			for (auto it = sids.begin(); it != sids.end(); it++)
			{
				uint32_t sid = it->first;


				auto cit = _ctx_map.find(sid);
				if (cit != _ctx_map.end())
				{
					SelContextPtr& ctx = (SelContextPtr&)cit->second;
					uint32_t opt = it->second.second;

					// BUG-R10-004 / K25=A (slice1 final gaps): a strategy whose tick handler
					// throws is isolated and reported; the other subscribers still get the tick
					// and the exception never reaches the ticker strand or the market-data thread.
					try
					{
					if (opt == 0)
					{
						ctx->on_tick(stdCode, curTick);
					}
					else
					{
						std::string wCode = stdCode;
						wCode = fmt::format("{}{}", stdCode, opt == 1 ? SUFFIX_QFQ : SUFFIX_HFQ);
						if (opt == 1)
						{
							ctx->on_tick(wCode.c_str(), curTick);
						}
						else //(opt == 2)
						{
							WTSTickData* newTick = WTSTickData::create(curTick->getTickStruct());
							struct TickRelease { WTSTickData* tick; ~TickRelease() { if (tick) tick->release(); } } releaseNewTick{ newTick };
							WTSTickStruct& newTS = newTick->getTickStruct();
							newTick->setContractInfo(curTick->getContractInfo());

							//这里做一个复权因子的处理
							double factor = get_exright_factor(stdCode);
							newTS.open *= factor;
							newTS.high *= factor;
							newTS.low *= factor;
							newTS.price *= factor;

							/*
							 *	By Wesley @ 2022.08.15
							 *	这里对tick的复权做一个完善
							 */
							if (flag & 1)
							{
								newTS.total_volume /= factor;
								newTS.volume /= factor;
							}

							if (flag & 2)
							{
								newTS.total_turnover *= factor;
								newTS.turn_over *= factor;
							}

							if (flag & 4)
							{
								newTS.open_interest /= factor;
								newTS.diff_interest /= factor;
								newTS.pre_interest /= factor;
							}

							_price_map[wCode] = newTS.price;

							ctx->on_tick(wCode.c_str(), newTick);
						}
					}
					}
					catch (const std::exception& e)
					{
						WTSLogger::error("Tick handler of strategy {} threw on {}: {}", ctx->name(), stdCode, e.what());
					}
					catch (...)
					{
						WTSLogger::error("Tick handler of strategy {} threw an unknown exception on {}", ctx->name(), stdCode);
					}

				}


			}

		}
	}
}

void WtSelEngine::on_minute_end(uint32_t curDate, uint32_t curTime)
{
	//要比较下一分钟的时间
	uint32_t nextTime = TimeUtils::getNextMinute(curTime, 1);
	if (nextTime < curTime)
		curDate = TimeUtils::getNextDate(curDate);

	uint32_t weekDay = TimeUtils::getWeekDay(curDate);

	// I03-NEW-066: reap dispatch threads of earlier tasks that have finished.
	join_schedule_workers(true);

	for (auto& v : _tasks)
	{
		TaskInfoPtr& tInfo = (TaskInfoPtr&)v.second;
		// Decision K27=A: d/w/m/y fire at the HHMM time; for the min period
		// _time is the interval in minutes, matched below like the backtest.
		if (tInfo->_period != TPT_Minute && tInfo->_time != nextTime)
			continue;

		uint64_t now = (uint64_t)curDate * 10000 + nextTime;
		if (tInfo->_last_exe_time >= now)
			continue;

		if (_base_data_mgr->isHoliday(tInfo->_trdtpl, curDate, true))
			continue;

		//获取上一个交易日的日期
		uint32_t preTDate = TimeUtils::getNextDate(_cur_date, -1);
		bool bHasHoliday = false;
		uint32_t days = 1;
		while (_base_data_mgr->isHoliday(tInfo->_trdtpl, preTDate, true))
		{
			bHasHoliday = true;
			preTDate = TimeUtils::getNextDate(preTDate, -1);
			days++;
		}
		uint32_t preWD = TimeUtils::getWeekDay(preTDate);

		WTSSessionInfo* sInfo = get_session_info(tInfo->_session, false);

		bool bIgnore = true;
		switch (tInfo->_period)
		{
		case TPT_None:
			break;
		case TPT_Daily:
			bIgnore = false;
			break;
		case TPT_Minute:
			if (sInfo != NULL && tInfo->_time != 0)
			{
				uint32_t minutes = sInfo->timeToMinutes(curTime);	//先将时间转换成分钟数
				if(minutes != 0 && (minutes%tInfo->_time == 0))		//如果分钟数能被整除,且不为0,则可以触发
				{
					bIgnore = false;
				}
			}
			break;
		case TPT_Monthly:
			//if (preTDate % 1000000 < _task->_day && _cur_date % 1000000 >= _task->_day)
			//	fired = true;
			// Decision K27=A: monthly date is the day of month (1..31), as in the backtest.
			if (_cur_date % 100 == tInfo->_day)
				bIgnore = false;
			else if (bHasHoliday)
			{
				//上一个交易日在上个月,且当前日期大于触发日期
				//说明这个月的开始日期在节假日内,顺延到今天
				if ((preTDate % 10000 / 100 < _cur_date % 10000 / 100) && _cur_date % 100 > tInfo->_day)
				{
					bIgnore = false;
				}
				else if (preTDate % 100 < tInfo->_day && _cur_date % 100 > tInfo->_day)
				{
					//上一个交易日在同一个月,且小于触发日期,但是今天大于触发日期,说明正确触发日期到节假日内,顺延到今天
					bIgnore = false;
				}
			}
			break;
		case TPT_Weekly:
			//if (preWD < _task->_day && weekDay >= _task->_day)
			//	fired = true;
			if (weekDay == tInfo->_day)
				bIgnore = false;
			else if (bHasHoliday)
			{
				if (days >= 7 && weekDay > tInfo->_day)
				{
					bIgnore = false;
				}
				else if (preWD > weekDay && weekDay > tInfo->_day)
				{
					//上一个交易日的星期大于今天的星期,说明换了一周了
					bIgnore = false;
				}
				else if (preWD < tInfo->_day && weekDay > tInfo->_day)
				{
					bIgnore = false;
				}
			}
			break;
		case TPT_Yearly:
			if (preTDate % 10000 < tInfo->_day && _cur_date % 10000 >= tInfo->_day)
				bIgnore = false;
			break;
		}

		if (bIgnore)
			continue;

		//TODO: 回调任务
		// I03-NEW-066: tasks are registered under the context id (the map key),
		// not under the task sequence number.
		SelContextPtr ctx = getContext(v.first);
		tInfo->_last_exe_time = now;
		if (!ctx)
		{
			WTSLogger::error("Schedule task {} has no context {}", tInfo->_name, v.first);
			continue;
		}

		// Decision K25=A (2026-11-04): the schedule runs synchronously on the
		// ticker strand that delivers on_minute_end, so schedule and tick
		// callbacks of a context never overlap; re-entrant ticker events are
		// queued by the strand. Exceptions are isolated per strategy.
		try
		{
			ctx->on_schedule(curDate, curTime, nextTime);
		}
		catch (const std::exception& e)
		{
			WTSLogger::error("Schedule of strategy {} threw: {}", ctx->name(), e.what());
		}
		catch (...)
		{
			WTSLogger::error("Schedule of strategy {} threw an unknown exception", ctx->name());
		}
	}
}

void WtSelEngine::run()
{
	// BUG-R10-004 (slice1 final gaps): a repeated run() is rejected. Creating a
	// second ticker would overwrite _tm_ticker and leak the running ticker and
	// its worker, which would keep driving this engine.
	if (_tm_ticker != NULL)
	{
		WTSLogger::error("SEL engine is already running; repeated run() rejected");
		return;
	}

	if (!fee_config_valid())
	{
		WTSLogger::error("SEL engine start rejected: fee template configuration is invalid");
		return;
	}

	WTSVariant* cfgProd = _cfg->get("product");
	_tm_ticker = new WtSelRtTicker(this);
	_tm_ticker->init(_data_mgr->reader(), cfgProd->getCString("session"));

	//启动之前,先把运行中的策略落地
	{
		rj::Document root(rj::kObjectType);
		rj::Document::AllocatorType &allocator = root.GetAllocator();

		rj::Value jStraList(rj::kArrayType);
		for (auto& m : _ctx_map)
		{
			const SelContextPtr& ctx = m.second;
			jStraList.PushBack(rj::Value(ctx->name(), allocator), allocator);
		}

		root.AddMember("marks", jStraList, allocator);

		rj::Value jChnlList(rj::kArrayType);
		for (auto& m : _adapter_mgr->getAdapters())
		{
			const TraderAdapterPtr& adapter = m.second;
			jChnlList.PushBack(rj::Value(adapter->id(), allocator), allocator);
		}

		root.AddMember("channels", jChnlList, allocator);

		root.AddMember("engine", rj::Value("SEL", allocator), allocator);

		std::string filename = WtHelper::getBaseDir();
		filename += "marker.json";

		rj::StringBuffer sb;
		rj::PrettyWriter<rj::StringBuffer> writer(sb);
		root.Accept(writer);
		StdFile::write_file_content(filename.c_str(), sb.GetString());
	}

	if (!_tm_ticker->run())
		WTSLogger::error("SEL engine start failed: time ticker could not start (trading session unavailable)");
}

void WtSelEngine::init(WTSVariant* cfg, IBaseDataMgr* bdMgr, WtDtMgr* dataMgr, IHotMgr* hotMgr, EventNotifier* notifier /* = NULL */)
{
	WtEngine::init(cfg, bdMgr, dataMgr, hotMgr, notifier);

	_cfg = cfg;
	_cfg->retain();
}

void WtSelEngine::addContext(SelContextPtr ctx, uint32_t date, uint32_t time, TaskPeriodType period, bool bStrict /* = true */, const char* trdtpl /* = "CHINA" */, const char* sessionID/* ="TRADING" */)
{
	if (ctx == NULL)
		return;

	auto it = _tasks.find(ctx->id());
	if(it != _tasks.end())
	{
		WTSLogger::error("Task registration failed: task id {} already registered", ctx->id());
		return;
	}

	TaskInfoPtr tInfo(new TaskInfo);
	wt_strcpy(tInfo->_name, ctx->name());
	wt_strcpy(tInfo->_trdtpl, trdtpl);
	wt_strcpy(tInfo->_session, sessionID);
	tInfo->_day = date;
	tInfo->_time = time;
	tInfo->_period = period;
	tInfo->_strict_time = bStrict;
	tInfo->_id = makeTaskId();

	_tasks[ctx->id()] = tInfo;

	uint32_t sid = ctx->id();
	_ctx_map[sid] = ctx;
}

SelContextPtr WtSelEngine::getContext(uint32_t id)
{
	auto it = _ctx_map.find(id);
	if (it == _ctx_map.end())
		return SelContextPtr();

	return it->second;
}

void WtSelEngine::handle_pos_change(const char* straName, const char* stdCode, double diffQty)
{
	//这里是持仓增量,所以不用处理未过滤的情况,因为增量情况下,不会改变目标diffQty
	if (_filter_mgr.is_filtered_by_strategy(straName, diffQty, true))
	{
		//输出日志
		WTSLogger::info("[Filters] Target position of {} of strategy {} ignored by strategy filter", stdCode, straName);
		return;
	}

	std::string realCode = stdCode;
	//const char* ruleTag = _hot_mgr->getRuleTag(stdCode);
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	if (strlen(cInfo._ruletag) > 0)
	{
		std::string code = _hot_mgr->getCustomRawCode(cInfo._ruletag, cInfo.stdCommID(), _cur_tdate);
		realCode = CodeHelper::rawMonthCodeToStdCode(code.c_str(), cInfo._exchg);
	}
	//else if (CodeHelper::isStdFutHotCode(stdCode))
	//{
	//	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode);
	//	std::string code = _hot_mgr->getRawCode(cInfo._exchg, cInfo._product, _cur_tdate);
	//	realCode = CodeHelper::rawMonthCodeToStdCode(code.c_str(), cInfo._exchg);
	//}
	//else if (CodeHelper::isStdFut2ndCode(stdCode))
	//{
	//	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode);
	//	std::string code = _hot_mgr->getSecondRawCode(cInfo._exchg, cInfo._product, _cur_tdate);
	//	realCode = CodeHelper::rawMonthCodeToStdCode(code.c_str(), cInfo._exchg);
	//}

	// I03-NEW-115: copy the owner under _pos_mtx; a reference into the map
	// dangles once another insert relocates the map storage.
	PosInfoPtr pInfo = get_or_create_position(realCode.c_str());
	double curVolume = 0;
	{
		SpinLock posLock(pInfo->_mtx);
		curVolume = pInfo->_volume;
	}

	bool bRiskEnabled = false;
	if (!decimal::eq(_risk_volscale, 1.0) && _risk_date == _cur_tdate)
	{
		WTSLogger::log_by_cat("risk", LL_INFO, "Risk scale of portfolio is {:.2f}", _risk_volscale);
		bRiskEnabled = true;
	}
	if (bRiskEnabled && diffQty != 0)
	{
		double symbol = diffQty / abs(diffQty);
		diffQty = decimal::rnd(abs(diffQty)*_risk_volscale)*symbol;
	}
	// K26=B1: accumulate onto a pending target (runs on the ticker strand, K25).
	double targetPos = pending_target_or(realCode, curVolume) + diffQty;

	append_signal(realCode.c_str(), targetPos, false);
	save_datas();

	const auto& exec_ids = _exec_mgr.get_route_by_strategy(straName);
	for (auto& execid : exec_ids)
		_exec_mgr.handle_pos_change(realCode.c_str(), targetPos, diffQty, execid.c_str());
}

ExecUnitInitStatus WtSelEngine::queryExecUnitInitStatus(const char* executorId, const char* stdCode) const
{
	return _exec_mgr.query_exec_unit_init_status(executorId, stdCode);
}

ExecUnitInitClearResult WtSelEngine::clearFailedExecUnitInitStatus(const char* executorId, const char* stdCode)
{
	return _exec_mgr.clear_failed_exec_unit_init_status(executorId, stdCode);
}

WTSCommodityInfo* WtSelEngine::get_comm_info(const char* stdCode)
{
	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	return _base_data_mgr->getCommodity(codeInfo._exchg, codeInfo._product);
}

WTSSessionInfo* WtSelEngine::get_sess_info(const char* stdCode)
{
	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	WTSCommodityInfo* cInfo = _base_data_mgr->getCommodity(codeInfo._exchg, codeInfo._product);
	if (cInfo == NULL)
		return NULL;

	return cInfo->getSessionInfo();
}

uint64_t WtSelEngine::get_real_time()
{
	return TimeUtils::makeTime(_cur_date, _cur_raw_time * 100000 + _cur_secs);
}
