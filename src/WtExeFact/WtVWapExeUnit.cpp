/*
23.5.23--zhaoyk--VWAP
*/
#include "WtVWapExeUnit.h"

#include <cmath>
#include <sstream>
#include <limits>

#include "../Share/TimeUtils.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Share/decimal.h"
#include "../Share/fmtlib.h"
#include "../Includes/WTSSessionInfo.hpp"
#include "VwapProfile.hpp"


extern const char* FACT_NAME;

WtVWapExeUnit::WtVWapExeUnit()
	:_last_tick(NULL)
	, _target_pos(0)
	,_channel_ready(false)
	,_channel_reconcile_pending(false)
	,_retry_pending(false)
	,_retry_cancel(false)
	,_valid_tick_dispatch(false)
	,_config_valid(false)
	,_init_succeeded(false)
	,_comm_info(NULL)//品种信息
	,_sess_info(NULL)
	, _cancel_times(0)
	,_begin_session_minute(0)
	,_end_session_minute(0)
	,_cancel_cnt(0)
	, _total_secs(0)
	, _total_times(0)
	,_tail_secs(0)
	,_ord_sticky(0)
	, _price_mode(0)
	, _price_offset(0)
	, _begin_time(0)
	, _end_time(0)
	,_min_open_lots(0)
	,_order_lots(0)
	,isCanCancel(true)
	,_fire_span(0)
	,_fired_times(0)
	, _last_fire_session_time_ms(0)
	, _last_fire_session_date(0)
	,_last_place_time(0)
	, _last_tick_time(0)
	,_last_valid_tick_time(0)
	,_tick_time_high_water(0)
	,_retry_after_tick_time(0)
	,_Vwap_prz(0)
	, _in_calc(false)
	{
}

WtVWapExeUnit::~WtVWapExeUnit()
{
	if (_last_tick)
		_last_tick->release();

	if (_comm_info)
		_comm_info->release();
	if (_sess_info)
		_sess_info->release();
}
inline double get_real_target(double _target) {
	if (_target == DBL_MAX)
		return 0;

	return _target;
}
inline bool is_clear(double target)
{
	return (target == DBL_MAX);
}

inline bool validSessionHHMM(uint32_t value)
{
	return value <= 2359 && value / 100 < 24 && value % 100 < 60;
}

inline bool validActionTime(uint32_t value)
{
	const uint32_t hhmmss = value / 1000;
	const uint32_t hhmm = hhmmss / 100;
	return hhmm / 100 < 24 && hhmm % 100 < 60 && hhmmss % 100 < 60;
}

inline bool validActionDate(uint32_t value)
{
	const uint32_t year = value / 10000;
	const uint32_t month = (value / 100) % 100;
	const uint32_t day = value % 100;
	if (year < 1900 || month == 0 || month > 12 || day == 0)
		return false;
	static const uint32_t monthDays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	uint32_t maxDay = monthDays[month - 1];
	if (month == 2 && (year % 400 == 0 || (year % 4 == 0 && year % 100 != 0)))
		maxDay = 29;
	return day <= maxDay;
}

inline bool validSessionDatePair(WTSSessionInfo* session, uint32_t actionDate,
	uint32_t tradingDate, uint32_t hhmm, uint32_t& sessionDate,
	std::string& diagnostic)
{
	if (session == NULL || !validActionDate(actionDate)
		|| (tradingDate != 0 && !validActionDate(tradingDate)))
	{
		diagnostic = "VWAP tick action/trading date is not a valid calendar date";
		return false;
	}
	sessionDate = session->getOffsetDate(actionDate, hhmm);
	if (!validActionDate(sessionDate))
	{
		diagnostic = "VWAP session-adjusted date cannot be verified";
		return false;
	}
	if (tradingDate == 0)
	{
		if (sessionDate != actionDate)
		{
			diagnostic = "VWAP overnight tick has no verifiable trading_date";
			return false;
		}
		return true;
	}
	// No exchange holiday calendar is available here. A trading date may skip
	// any number of natural days, so only reject an impossible earlier date.
	if (tradingDate < sessionDate)
	{
		diagnostic = "VWAP tick action_date and trading_date do not match the session offset; refusing ambiguous provider dates";
		return false;
	}
	diagnostic.clear();
	return true;
}

inline bool readTickSessionTime(WTSSessionInfo* session, WTSTickData* tick,
	size_t profileSize, size_t& bucket, uint32_t& sessionDate,
	uint64_t& sessionTimeMs, std::string& diagnostic)
{
	if (session == NULL || tick == NULL)
	{
		diagnostic = "VWAP requires a session and tick";
		return false;
	}
	if (!wtp::vwap_profile::mapActionTime(*session, tick->actiontime(), profileSize, bucket, diagnostic))
		return false;

	const uint32_t hhmmss = tick->actiontime() / 1000;
	const uint32_t hhmm = hhmmss / 100;
	const uint32_t second = hhmmss % 100;
	const uint32_t millisecond = tick->actiontime() % 1000;
	const uint64_t minute = static_cast<uint64_t>(bucket);
	if (minute > (std::numeric_limits<uint64_t>::max() - 59999) / 60000)
	{
		diagnostic = "VWAP session time overflows";
		return false;
	}
	sessionTimeMs = minute * 60000 + second * 1000 + millisecond;
	if (!validSessionDatePair(session, tick->actiondate(), tick->tradingdate(),
		hhmm, sessionDate, diagnostic))
		return false;
	diagnostic.clear();
	return true;
}

inline bool inConfiguredWindow(uint32_t bucket, uint32_t second, uint32_t millisecond,
	uint32_t beginMinute, uint32_t endMinute)
{
	const uint64_t elapsed = static_cast<uint64_t>(bucket) * 60000 + second * 1000 + millisecond;
	return elapsed >= static_cast<uint64_t>(beginMinute) * 60000
		&& elapsed <= static_cast<uint64_t>(endMinute) * 60000;
}

inline double calculateVwapQuantity(double aimQty, double realPos, double diffQty,
	double minOpenLots, bool showHand)
{
	if (!std::isfinite(aimQty) || !std::isfinite(realPos) || !std::isfinite(diffQty)
		|| !std::isfinite(minOpenLots) || decimal::eq(diffQty, 0))
		return 0;
	const double requested = showHand ? abs(diffQty) : max(aimQty - realPos, minOpenLots);
	const double quantity = min(abs(diffQty), max(0.0, requested));
	return decimal::lt(diffQty, 0) ? -quantity : quantity;
}

inline bool limitVwapQuantityByAvailability(ExecuteContext* ctx, const char* code,
	double realPos, double& quantity, std::string& diagnostic)
{
	if (ctx == NULL || code == NULL || !std::isfinite(realPos) || !std::isfinite(quantity))
	{
		diagnostic = "VWAP cannot verify current position availability";
		return false;
	}
	if (decimal::gt(quantity, 0) && decimal::lt(realPos, 0))
	{
		const double shortPosition = abs(ctx->getPosition(code, false, 2));
		const double availableShort = abs(ctx->getPosition(code, true, 2));
		if (!std::isfinite(shortPosition) || !std::isfinite(availableShort))
		{
			diagnostic = "VWAP short position availability is invalid";
			return false;
		}
		quantity = min(quantity, min(shortPosition, availableShort));
	}
	else if (decimal::lt(quantity, 0) && decimal::gt(realPos, 0))
	{
		const double longPosition = ctx->getPosition(code, false, 1);
		const double availableLong = ctx->getPosition(code, true, 1);
		if (!std::isfinite(longPosition) || !std::isfinite(availableLong)
			|| decimal::lt(longPosition, 0) || decimal::lt(availableLong, 0))
		{
			diagnostic = "VWAP long position availability is invalid";
			return false;
		}
		quantity = -min(abs(quantity), min(longPosition, availableLong));
	}
	diagnostic.clear();
	return true;
}

struct ScopedFlagReset
{
	explicit ScopedFlagReset(bool& flag) : _flag(flag) {}
	~ScopedFlagReset() { _flag = false; }
	bool& _flag;
};
const char * WtVWapExeUnit::getFactName()
{
	return FACT_NAME;
}

const char * WtVWapExeUnit::getName()
{
	return "WtVWapExeUnit";
}

void WtVWapExeUnit::init(ExecuteContext * ctx, const char * stdCode, WTSVariant * cfg)
{
	_init_succeeded = false;
	_config_valid = false;
	if (_comm_info)
	{
		_comm_info->release();
		_comm_info = NULL;
	}
	if (_sess_info)
	{
		_sess_info->release();
		_sess_info = NULL;
	}
	VwapAim.clear();
	if (ctx == NULL || stdCode == NULL || stdCode[0] == '\0' || cfg == NULL)
		return;

	ExecuteUnit::init(ctx, stdCode, cfg);
	_comm_info = ctx->getCommodityInfo(stdCode);
	if (_comm_info)
		_comm_info->retain();
	_sess_info = ctx->getSessionInfo(stdCode);
	if (_sess_info)
		_sess_info->retain();
	if (_comm_info == NULL || _sess_info == NULL || _sess_info->getSectionCount() == 0)
	{
		ctx->writeLog("WtVWapExeUnit initialization failed: commodity or trading session is unavailable");
		return;
	}

	_begin_time = cfg->getUInt32("begin_time");
	_end_time = cfg->getUInt32("end_time");
	_ord_sticky = cfg->getUInt32("ord_sticky");
	_tail_secs = cfg->getUInt32("tail_secs");
	_total_times = cfg->getUInt32("total_times");
	_price_mode = cfg->getUInt32("price_mode");
	_price_offset = cfg->getUInt32("price_offset");
	_order_lots = cfg->getDouble("lots");
	_min_open_lots = cfg->has("minopenlots") ? cfg->getDouble("minopenlots") : 0.0;
	if (!std::isfinite(_order_lots) || _order_lots <= 0
		|| !std::isfinite(_min_open_lots) || _min_open_lots < 0 || _price_mode > 2)
	{
		ctx->writeLog("WtVWapExeUnit initialization failed: lots or price_mode is invalid");
		return;
	}

	if (_total_times == 0)
	{
		ctx->writeLog(fmt::format("执行单元WtVWapExeUnit[{}] 配置无效: total_times 必须大于0", stdCode).c_str());
		return;
	}
	if (!validSessionHHMM(_begin_time) || !validSessionHHMM(_end_time)
		|| _sess_info->isInAuctionTime(_begin_time) || _sess_info->isInAuctionTime(_end_time))
	{
		ctx->writeLog("WtVWapExeUnit initialization failed: begin_time/end_time is not a valid trading time");
		return;
	}
	const uint32_t beginMinute = _sess_info->timeToMinutes(_begin_time, false);
	const uint32_t endMinute = _sess_info->timeToMinutes(_end_time, false);
	if (beginMinute == INVALID_UINT32 || endMinute == INVALID_UINT32 || endMinute <= beginMinute)
	{
		ctx->writeLog("WtVWapExeUnit initialization failed: execution window is not ordered within the trading session");
		return;
	}
	_begin_session_minute = beginMinute;
	_end_session_minute = endMinute;
	const uint64_t duration = static_cast<uint64_t>(endMinute - beginMinute) * 60;
	if (duration == 0 || duration > std::numeric_limits<uint32_t>::max())
	{
		ctx->writeLog("WtVWapExeUnit initialization failed: execution window duration is invalid");
		return;
	}
	_total_secs = static_cast<uint32_t>(duration);
	if (_tail_secs > _total_secs)
	{
		ctx->writeLog(fmt::format("执行单元WtVWapExeUnit[{}] 配置无效: tail_secs {} 超出执行时长 {} 秒", stdCode, _tail_secs, _total_secs).c_str());
		return;
	}
	_fire_span = (_total_secs - _tail_secs) / _total_times;

	const char* commodityName = _comm_info->getName();
	if (commodityName == NULL || commodityName[0] == '\0')
	{
		ctx->writeLog("WtVWapExeUnit initialization failed: commodity name is empty");
		return;
	}
	const std::string filename = std::string("Vwap_") + commodityName + ".txt";
	std::ifstream file(filename.c_str(), std::ios::in | std::ios::binary);
	if (!file.is_open())
	{
		ctx->writeLog(fmt::format("Vwap file {} cannot be opened; execution disabled.", filename).c_str());
		return;
	}
	std::ostringstream contents;
	contents << file.rdbuf();
	if (file.bad())
	{
		ctx->writeLog(fmt::format("Vwap file {} cannot be read; execution disabled.", filename).c_str());
		return;
	}
	std::vector<double> distribution;
	std::string diagnostic;
	if (!wtp::vwap_profile::loadProfile(contents.str(), *_sess_info, distribution, diagnostic))
	{
		ctx->writeLog(fmt::format("Vwap file {} is invalid for this session ({}); execution disabled.", filename, diagnostic).c_str());
		return;
	}

	VwapAim.swap(distribution);
	_config_valid = true;
	_init_succeeded = true;
	ctx->writeLog(fmt::format("执行单元WtVWapExeUnit[{}] 初始化完成,订单超时 {} 秒,执行时限 {} 秒,收尾时间 {} 秒,间隔时间 {} 秒", stdCode, _ord_sticky, _total_secs, _tail_secs, _fire_span).c_str());
} 

void WtVWapExeUnit::on_order(uint32_t localid, const char * stdCode, bool /*isBuy*/, double leftover, double /*price*/, bool isCanceled)
{
	if (!_orders_mon.has_order(localid))
		return;
	if (isCanceled || leftover == 0)
	{
		_orders_mon.erase_order(localid);
		if (_cancel_cnt > 0)
		{
			_cancel_cnt--;
			_ctx->writeLog(fmtutil::format("[{}@{}] Order of {} cancelling done, cancelcnt -> {}", __FILE__, __LINE__, _code.c_str(), _cancel_cnt));
		}
	}
	
	if (leftover == 0 && !isCanceled) {
		_cancel_times = 0;
		_ctx->writeLog(fmtutil::format("Order {} has filled", localid));
	}
	//如果全部订单已撤销,这个时候一般是遇到要超时撤单（挂单超时） 
	if (isCanceled && _cancel_cnt == 0)
	{
		const double realPos = _ctx->getPosition(stdCode, false);
		const double targetPos = get_real_target(_target_pos);
		if (!decimal::eq(realPos, targetPos))
		{
			_ctx->writeLog(fmtutil::format("Order {} of {} canceled; re_fire deferred until a fresh in-session tick", localid, stdCode));
			_cancel_times++;
			_retry_pending = true;
			_retry_cancel = true;
			_retry_after_tick_time = _last_valid_tick_time;
		}
	}

	if (!isCanceled&&_cancel_cnt != 0) {//一般出现问题，需要返回检查  触发撤单 cnt++,onorder响应处理才会--
		_ctx->writeLog(fmtutil::format("Order {} of {}  hasn't canceled, error will be return ", localid, stdCode));
		return;
	}
}

void WtVWapExeUnit::on_channel_ready()
{
	if (!_config_valid)
		return;

	_channel_ready = true;
	_channel_reconcile_pending = true;
	_ctx->writeLog("WtVWapExeUnit channel reconciliation deferred until the next fresh in-session tick");
}

void WtVWapExeUnit::reconcile_channel_ready()
{
	if (!_channel_reconcile_pending || !_channel_ready || !_valid_tick_dispatch)
		return;
	_channel_reconcile_pending = false;
	double undone = _ctx->getUndoneQty(_code.c_str());
	if (!decimal::eq(undone, 0) && !_orders_mon.has_order())
	{//未完成单不在监控中，撤单
		/*
		 *	如果未完成单不为0，而OMS没有订单
		 *	这说明有未完成单不在监控之中,全部撤销掉
		 *	因为这些订单没有本地订单号，无法直接进行管理
		 *	这种情况，就是刚启动的时候，上次的未完成单或者外部的挂单
		 */
		_ctx->writeLog(fmt::format("{} unmanaged orders of {},cancel all", undone, _code).c_str());
		
		bool isBuy = (undone > 0);
		OrderIDs ids =_ctx->cancel(_code.c_str(), isBuy);
		_orders_mon.push_order(ids.data(), ids.size(), _ctx->getCurTime());
		_cancel_cnt += ids.size();

		_ctx->writeLog(fmtutil::format("[{}@{}]cancelcnt -> {}", __FILE__, __LINE__, _cancel_cnt));
	}
	else if (decimal::eq(undone, 0) && _orders_mon.has_order())
	{	/*
		 *	By Wesey @ 2021.12.13
		 *	如果未完成单为0，但是OMS中是有订单的
		 *	说明OMS中是错单，需要清理掉，不然超时撤单就会出错
		 *	这种情况，一般是断线重连以后，之前下出去的订单，并没有真正发送到柜台
		 *	所以这里需要清理掉本地订单
		 */
		_ctx->writeLog(fmtutil::format("Local orders of {} not confirmed in trading channel, clear all", _code.c_str()));
		_orders_mon.clear_orders();
	}
	else
	{
		_ctx->writeLog(fmtutil::format("Unrecognized condition while channle ready, {:.2f} live orders of {} exists, local orders {}exist",
			undone, _code.c_str(), _orders_mon.has_order() ? "" : "not "));
	}
}


void WtVWapExeUnit::on_tick(WTSTickData * newTick)
{
	if (!_config_valid || newTick == NULL || _code.compare(newTick->code()) != 0)
		return;
	if (!validActionTime(newTick->actiontime()))
	{
		_ctx->writeLog(fmt::format("VWAP tick action time {} rejected: invalid HHMMSSmmm value", newTick->actiontime()).c_str());
		return;
	}
	if (!validActionDate(newTick->actiondate())
		|| (newTick->tradingdate() != 0 && !validActionDate(newTick->tradingdate())))
	{
		_ctx->writeLog(fmt::format("VWAP tick action/trading date {} / {} rejected: invalid calendar date",
			newTick->actiondate(), newTick->tradingdate()).c_str());
		return;
	}
	const int64_t tickTime = TimeUtils::makeTime(newTick->actiondate(), newTick->actiontime());
	if (tickTime <= 0)
	{
		_ctx->writeLog(fmt::format("VWAP tick date/time {} {} rejected: timestamp cannot be verified",
			newTick->actiondate(), newTick->actiontime()).c_str());
		return;
	}
	const uint64_t curTickTime = static_cast<uint64_t>(tickTime);
	if (curTickTime <= _tick_time_high_water)
	{
		_ctx->writeLog(fmt::format("VWAP ignored stale or out-of-order tick {} (high-water {})",
			curTickTime, _tick_time_high_water).c_str());
		return;
	}
	// Preserve event-time high-water even for a valid quote during a break or
	// after close, so a delayed in-session tick cannot become executable later.
	_tick_time_high_water = curTickTime;

	size_t bucket = 0;
	uint32_t sessionDate = 0;
	uint64_t sessionTimeMs = 0;
	std::string diagnostic;
	if (!readTickSessionTime(_sess_info, newTick, VwapAim.size(), bucket,
		sessionDate, sessionTimeMs, diagnostic))
	{
		_ctx->writeLog(fmt::format("VWAP tick action time {} rejected: {}", newTick->actiontime(), diagnostic).c_str());
		return;
	}
	const uint32_t hhmmss = newTick->actiontime() / 1000;
	if (!inConfiguredWindow(static_cast<uint32_t>(bucket), hhmmss % 100,
		newTick->actiontime() % 1000, _begin_session_minute, _end_session_minute))
	{
		_ctx->writeLog(fmt::format("VWAP tick action time {} is outside the configured trading window", newTick->actiontime()).c_str());
		return;
	}

	const bool isFirstTick = (_last_tick == NULL);
	if (_last_tick)
		_last_tick->release();
	_last_tick = newTick;
	_last_tick->retain();
	_last_valid_tick_time = curTickTime;
	_valid_tick_dispatch = true;
	ScopedFlagReset dispatchReset(_valid_tick_dispatch);

	// The new eligible tick is the execution-time proof. Engine getCurTime()
	// follows feed/replay time and can remain stationary while callbacks arrive.
	reconcile_channel_ready();
	if (_retry_pending)
	{
		if (!_channel_ready || curTickTime <= _retry_after_tick_time)
			return;
		if (_cancel_cnt != 0 || !decimal::eq(_ctx->getUndoneQty(_code.c_str()), 0))
		{
			_retry_after_tick_time = curTickTime;
			_ctx->writeLog(fmt::format("{} VWAP retry deferred while cancel or unfinished quantity remains", _code).c_str());
			return;
		}
		const bool retryCanceledOrder = _retry_cancel;
		_retry_pending = false;
		_retry_cancel = false;
		_retry_after_tick_time = 0;
		if (retryCanceledOrder)
		{
			// Rebuild the canceled slice from current target, total position,
			// profile, and available close quantity.
			const double realPos = _ctx->getPosition(_code.c_str(), false);
			double targetPos = get_real_target(_target_pos);
			double diffQty = targetPos - realPos;
			if (decimal::eq(diffQty, 0) && is_clear(_target_pos))
			{
				const double availableLong = _ctx->getPosition(_code.c_str(), true, 1);
				if (!std::isfinite(availableLong) || decimal::lt(availableLong, 0))
				{
					_ctx->writeLog(fmt::format("{} cannot size canceled clear retry from invalid available position", _code).c_str());
					return;
				}
				if (decimal::eq(availableLong, 0))
					return;
				targetPos = -min(availableLong, _order_lots);
				diffQty = targetPos - realPos;
			}
			if (!std::isfinite(realPos) || !std::isfinite(targetPos) || !std::isfinite(diffQty))
			{
				_ctx->writeLog(fmt::format("{} cannot size canceled retry from invalid target or position", _code).c_str());
				return;
			}
			const uint32_t leftTimes = _fired_times >= _total_times ? 0 : _total_times - _fired_times;
			const bool showHand = leftTimes == 0 && !decimal::eq(diffQty, 0);
			double retryQty = calculateVwapQuantity(VwapAim[bucket], realPos,
				diffQty, _min_open_lots, showHand);
			std::string availabilityDiagnostic;
			if (!limitVwapQuantityByAvailability(_ctx, _code.c_str(), realPos,
				retryQty, availabilityDiagnostic))
			{
				_ctx->writeLog(fmt::format("{} canceled retry skipped: {}", _code, availabilityDiagnostic).c_str());
				_retry_pending = true;
				_retry_cancel = true;
				_retry_after_tick_time = curTickTime;
				return;
			}
			if (decimal::eq(retryQty, 0))
			{
				_ctx->writeLog(fmt::format("{} canceled retry is no longer needed or has no available quantity", _code).c_str());
				if (!decimal::eq(diffQty, 0))
				{
					_retry_pending = true;
					_retry_cancel = true;
					_retry_after_tick_time = curTickTime;
				}
				return;
			}
			fire_at_once(retryQty);
		}
		else
		{
			// Rejected orders recalculate the scheduled slice from fresh state.
			do_calc();
		}
		return;
	}

	if (isFirstTick)//如果是第一笔tick,则检查目标仓位,不符合则下单
	{
		double newVol = _target_pos;
		const char* stdCode = _code.c_str();
		double undone = _ctx->getUndoneQty(stdCode);
		double realPos = _ctx->getPosition(stdCode, false);
		if (!decimal::eq(newVol, undone + realPos))
		{//如果是第一笔TICK，且目标量==未完成+仓位，退出 
			do_calc();
		}
	}
	else
	{
		bool intervalElapsed = _last_fire_session_date == 0 || sessionDate != _last_fire_session_date;
		if (!intervalElapsed && sessionTimeMs >= _last_fire_session_time_ms)
			intervalElapsed = sessionTimeMs - _last_fire_session_time_ms >= static_cast<uint64_t>(_fire_span) * 1000;
		uint64_t now = _ctx->getCurTime();
		if (_ord_sticky != 0 && _orders_mon.has_order())
		{
			_orders_mon.check_orders_with_generation(_ord_sticky, now, [this, now](uint32_t localid, uint64_t generation) {
				if (!_orders_mon.mark_unverified_timeout(localid, generation, _ord_sticky, now))
					return; // 终态或 localid 已复用；不得复活旧快照
				_ctx->writeLog(fmt::format("Expired order {} of {} cannot be auto-canceled: legacy localid is unverified; order remains monitored",
					localid, _code.c_str()).c_str());
			});
		}
		if (!_orders_mon.has_unverified_timeout() && intervalElapsed)
		{
			do_calc();
		}
	}
	
}

void WtVWapExeUnit::on_trade(uint32_t /*localid*/, const char * /*stdCode*/, bool /*isBuy*/, double /*vol*/, double /*price*/)
{//在ontick中触发
}
/*
下单结果回报
*/
void WtVWapExeUnit::on_entrust(uint32_t localid, const char * /*stdCode*/, bool bSuccess, const char * /*message*/)
{
	if (!bSuccess)
	{
		//如果不是我发出去的订单,我就不管了
		if (!_orders_mon.has_order(localid))
			return;
		
		_orders_mon.erase_order(localid);
		_retry_pending = true;
		_retry_cancel = false;
		_retry_after_tick_time = _last_valid_tick_time;
		_ctx->writeLog("VWAP rejected order retry deferred until a fresh in-session tick");
	}
}
void WtVWapExeUnit::do_calc()
{
	if (_orders_mon.has_unverified_timeout())
		return;
	if (!_config_valid || !_valid_tick_dispatch)
		return;

	CalcFlag flag(&_in_calc);
	if (flag)
		return;

	StdUniqueLock lock(_mtx_calc);
	if (_last_tick == NULL)
		return;
	size_t bucket = 0;
	uint32_t sessionDate = 0;
	uint64_t sessionTimeMs = 0;
	std::string diagnostic;
	if (!readTickSessionTime(_sess_info, _last_tick, VwapAim.size(), bucket,
		sessionDate, sessionTimeMs, diagnostic))
	{
		_ctx->writeLog(fmt::format("VWAP calculation skipped: {}", diagnostic).c_str());
		return;
	}
	const uint32_t hhmmss = _last_tick->actiontime() / 1000;
	const uint32_t second = hhmmss % 100;
	const uint32_t millisecond = _last_tick->actiontime() % 1000;
	if (!inConfiguredWindow(static_cast<uint32_t>(bucket), second, millisecond,
		_begin_session_minute, _end_session_minute))
	{
		_ctx->writeLog("VWAP calculation skipped: tick is outside the configured trading window");
		return;
	}
	const char* code = _code.c_str();
	double undone = _ctx->getUndoneQty(code);
	double newVol = get_real_target(_target_pos);
	double realPos = _ctx->getPosition(code, false);
	if (!std::isfinite(undone) || !std::isfinite(newVol) || !std::isfinite(realPos))
	{
		_ctx->writeLog(fmt::format("{} VWAP calculation skipped: target, position, or unfinished quantity is invalid", _code).c_str());
		return;
	}
	double diffQty = newVol - realPos;//目标差
	if (!_channel_ready)
		return;
	//有正在撤销的订单,则不能进行下一轮计算
	if (_cancel_cnt != 0)
	{
		_ctx->writeLog(fmt::format("{}尚有未完成的撤单指令，暂时退出本轮执行", _code).c_str());
		return;
	}
	if (decimal::eq(diffQty, 0))
		return;
	//每一次发单要保障成交,所以如果有未完成单,说明上一轮没完成
	//有未完成订单&&与实际仓位变动方向相反
	//则需要撤销现有订单
	if (decimal::lt(diffQty*undone, 0))
	{
		bool isBuy = decimal::gt(undone, 0);//undone>0,isbuy=1
		OrderIDs ids = _ctx->cancel(code, isBuy);
		if (!ids.empty())
		{
			_orders_mon.push_order(ids.data(), ids.size(), _ctx->getCurTime());
			_cancel_cnt += ids.size();
			_ctx->writeLog(fmtutil::format("[{}@{}] live opposite order of {} canceled, cancelcnt -> {}", __FILE__, __LINE__, _code.c_str(), _cancel_cnt));//相反的订单取消
		}
		return;
	}
	if (!decimal::eq(undone, 0))
	{
		_ctx->writeLog(fmt::format("{}上一轮有挂单未完成,暂时退出本轮执行", _code).c_str());
		return;
	}
	double curPos = realPos;

	if (decimal::eq(curPos, newVol))
	{
		//当前仓位和最新目标仓位匹配时，如果不是全部清仓的需求，则直接退出计算了
		if (!is_clear(_target_pos))
			return;

		//如果是清仓的需求，还要再进行对比
		//如果多头为0，说明已经全部清理掉了，则直接退出
		double lPos = _ctx->getPosition(code, true, 1);
		if (decimal::eq(lPos, 0))
			return;
		//如果还有多头仓位，则将目标仓位设置为非0，强制触发    
		newVol = -min(lPos, _order_lots);
		_ctx->writeLog(fmtutil::format("Clearing process triggered, target position of {} has been set to {}", _code.c_str(), newVol));
	}
	//如果相比上次没有更新的tick进来，则先不下单，防止开盘前集中下单导致通道被封
	uint64_t curTickTime = static_cast<uint64_t>(TimeUtils::makeTime(_last_tick->actiondate(), _last_tick->actiontime()));
	if (curTickTime <= _last_tick_time)
	{
		_ctx->writeLog(fmtutil::format("No tick of {} updated, {} <= {}, execute later", _code, curTickTime, _last_tick_time));
		return;
	}
	_last_tick_time = curTickTime;
	double aimQty = VwapAim[bucket];

	uint32_t leftTimes = _fired_times >= _total_times ? 0 : _total_times - _fired_times;
	_ctx->writeLog(fmt::format("第 {} 次发单", _fired_times+1).c_str());
	const bool bNeedShowHand = leftTimes == 0 && !decimal::eq(diffQty, 0);
	double curQty = calculateVwapQuantity(aimQty, curPos, diffQty,
		_min_open_lots, bNeedShowHand);
	std::string availabilityDiagnostic;
	if (!limitVwapQuantityByAvailability(_ctx, code, realPos, curQty, availabilityDiagnostic))
	{
		_ctx->writeLog(fmt::format("{} VWAP calculation skipped: {}", _code, availabilityDiagnostic).c_str());
		return;
	}
	if (decimal::eq(curQty, 0))
	{
		_ctx->writeLog(fmt::format("{} has no executable quantity after position availability checks", _code).c_str());
		return;
	}
	WTSTickData* curTick = _last_tick;
	uint64_t now = _ctx->getCurTime();
	bool isBuy = decimal::gt(diffQty, 0);
	double targetPx = 0;
	//根据价格模式设置,确定委托基准价格: 0-最新价,1-最优价,2-对手价
	if (_price_mode == 0)
	{
		targetPx = curTick->price();
	}
	else if (_price_mode == 1)
	{
		targetPx = isBuy ? curTick->bidprice(0) : curTick->askprice(0);
	}
	else
	{
		targetPx = isBuy ? curTick->askprice(0) : curTick->bidprice(0);
	}
	if (bNeedShowHand) {
		targetPx += _comm_info->getPriceTick() * 5 * (isBuy ? 1 : -1);
	}
	else if (_price_offset != 0)
	{
		targetPx += _comm_info->getPriceTick() * _price_offset * (isBuy ? 1 : -1);
	}

	// 如果最后价格为0，再做一个修正
	if (decimal::eq(targetPx, 0.0))
		targetPx = decimal::eq(_last_tick->price(), 0.0) ? _last_tick->preclose() : _last_tick->price();

	//检查涨跌停价
	isCanCancel = true;
	if (isBuy && !decimal::eq(_last_tick->upperlimit(), 0) && decimal::gt(targetPx, _last_tick->upperlimit()))
	{
		_ctx->writeLog(fmt::format("Buy price {} of {} modified to upper limit price", targetPx, _code.c_str(), _last_tick->upperlimit()).c_str());
		targetPx = _last_tick->upperlimit();
		isCanCancel = false;//如果价格被修正为涨跌停价，订单不可撤销
	}
	if (isBuy != 1 && !decimal::eq(_last_tick->lowerlimit(), 0) && decimal::lt(targetPx, _last_tick->lowerlimit()))
	{
		_ctx->writeLog(fmt::format("Sell price {} of {} modified to lower limit price", targetPx, _code.c_str(), _last_tick->lowerlimit()).c_str());
		targetPx = _last_tick->lowerlimit();
		isCanCancel = false;	//如果价格被修正为涨跌停价，订单不可撤销
	}
	OrderIDs ids;
	if (_orders_mon.has_unverified_timeout())
		return;
	if (curQty > 0)
	{
		ids = _ctx->buy(code, targetPx, abs(curQty));
	}
	else {
		ids = _ctx->sell(code, targetPx, abs(curQty));
	}
	_orders_mon.push_order(ids.data(), ids.size(), now, isCanCancel);
	_last_fire_session_time_ms = sessionTimeMs;
	_last_fire_session_date = sessionDate;
	_fired_times += 1;

}

void WtVWapExeUnit::fire_at_once(double qty)
{
	if (_orders_mon.has_unverified_timeout())
		return;
	if (!_config_valid || !_valid_tick_dispatch || _last_tick == NULL || decimal::eq(qty, 0))
		return;
	size_t bucket = 0;
	uint32_t sessionDate = 0;
	uint64_t sessionTimeMs = 0;
	std::string diagnostic;
	if (!readTickSessionTime(_sess_info, _last_tick, VwapAim.size(), bucket,
		sessionDate, sessionTimeMs, diagnostic))
		return;
	const uint32_t hhmmss = _last_tick->actiontime() / 1000;
	if (!inConfiguredWindow(static_cast<uint32_t>(bucket), hhmmss % 100,
		_last_tick->actiontime() % 1000, _begin_session_minute, _end_session_minute))
		return;
	_last_tick->retain();
	WTSTickData* curTick = _last_tick;
	const char* code = _code.c_str();
	uint64_t now = _ctx->getCurTime();
	bool isBuy = decimal::gt(qty, 0);
	double targetPx = 0;
	//根据价格模式设置,确定委托基准价格: 0-最新价,1-最优价,2-对手价
	if (_price_mode == 0) {
		targetPx = curTick->price();
	}
	else if (_price_mode == 1) {
		targetPx = isBuy ? curTick->askprice(0) : curTick->bidprice(0);
	}
	else // if(_price_mode == 2)
	{
		targetPx = isBuy ? curTick->askprice(0) : curTick->bidprice(0);//买入方向：卖价，卖出方向：买价
	}

	targetPx += _comm_info->getPriceTick()*_cancel_times *(isBuy ? 1 : -1);//增加价格偏移
	isCanCancel = true;
	if (isBuy && !decimal::eq(_last_tick->upperlimit(), 0) && decimal::gt(targetPx, _last_tick->upperlimit()))
	{
		_ctx->writeLog(fmt::format("Buy price {} of {} modified to upper limit price", targetPx, _code.c_str(), _last_tick->upperlimit()).c_str());
		targetPx = _last_tick->upperlimit();
		isCanCancel = false;//如果价格被修正为涨跌停价，订单不可撤销
	}
	if (isBuy != 1 && !decimal::eq(_last_tick->lowerlimit(), 0) && decimal::lt(targetPx, _last_tick->lowerlimit()))
	{
		_ctx->writeLog(fmt::format("Sell price {} of {} modified to lower limit price", targetPx, _code.c_str(), _last_tick->lowerlimit()).c_str());
		targetPx = _last_tick->lowerlimit();
		isCanCancel = false;	//如果价格被修正为涨跌停价，订单不可撤销
	}

	OrderIDs ids;
	if (_orders_mon.has_unverified_timeout())
	{
		curTick->release();
		return;
	}
	if (qty > 0)
		ids = _ctx->buy(code, targetPx, abs(qty));
	else
		ids = _ctx->sell(code, targetPx, abs(qty));

	_orders_mon.push_order(ids.data(), ids.size(), now, isCanCancel);
	_last_fire_session_time_ms = sessionTimeMs;
	_last_fire_session_date = sessionDate;

	curTick->release();
}

void WtVWapExeUnit::set_position(const char * stdCode, double newVol)
{
	if (_code.compare(stdCode) != 0)
		return;

	if (decimal::eq(newVol, _target_pos))
		return;
	
	_target_pos = newVol;
	
	_fired_times = 0;//已执行次数
	_last_fire_session_time_ms = 0;
	_last_fire_session_date = 0;
	_retry_pending = false;
	_retry_cancel = false;
	_retry_after_tick_time = 0;
}

void WtVWapExeUnit::on_channel_lost()
{
	_channel_ready = false;
}
