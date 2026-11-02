/**
 * @file WtStockVWapExeUnit.cpp
 * @author zhaoyk (23.6.2)
 * @brief 股票VWAP执行单元实现文件
 * @details 实现股票的成交量加权平均价(VWAP)执行单元的核心逻辑
 *          将大单拆分为多个小单并按时间分布执行，减少市场冲击
 */
#include "WtStockVWapExeUnit.h"
#include <cmath>
#include <fstream>
#include <iterator>




extern const char* FACT_NAME;

/**
 * @brief 构造函数
 * @details 初始化所有成员变量为默认值
 */
WtStockVWapExeUnit::WtStockVWapExeUnit()
	:_last_tick(NULL)       // 行情数据指针
	, _target_pos(0)        // 目标仓位
	, _channel_ready(false) // 交易通道就绪标志
	, _comm_info(NULL)      // 品种信息
	, _sess_info(NULL)      // 交易时段
	, _cancel_times(0)      // 撤单次数
	, _cancel_cnt(0)        // 撤单计数
	, _total_secs(0)        // 总执行时间
	, _total_times(0)       // 总执行次数
	, _ord_sticky(0)        // 挂单超时时间
	, _price_mode(0)        // 价格模式
	, _price_offset(0)      // 价格偏移
	, _begin_time(0)        // 开始时间
	, _end_time(0)          // 结束时间
	, _is_KC{ false }       // 科创板标志
	,isCanCancel{true}     // 订单是否可撤销
	, _is_clear{ false }    // 清仓标志
	, _fired_times(0)       // 已执行次数
	, _last_fire_time(0)    // 上次发单时间
	, _in_calc(false)
{
}

/**
 * @brief 析构函数
 * @details 清理分配的资源，包括行情数据和商品信息
 */
WtStockVWapExeUnit::~WtStockVWapExeUnit()
{
	// 释放行情数据
	if (_last_tick)
		_last_tick->release();

	// 释放商品信息
	if (_comm_info)
		_comm_info->release();

	if (_sess_info)
		_sess_info->release();
}
/**
 * @brief 获取实际目标仓位
 * @details 如果目标仓位是DBL_MAX（清仓标记），则返回0，否则返回原值
 * @param _target 原始目标仓位
 * @return 实际目标仓位
 */
inline double get_real_target(double _target) {
	if (_target == DBL_MAX)
		return 0;

	return _target;
}
/**
 * @brief 检查是否为清仓状态
 * @details 判断目标仓位是否等于DBL_MAX，该值被用作清仓标记
 * @param target 目标仓位
 * @return 如果为清仓状态返回true，否则返回false
 */
inline bool is_clear(double target)
{
	return (target == DBL_MAX);
}
/**
 * @brief 校验配置时间是否为有效 HHMM
 */
inline bool validHHMM(uint32_t time)
{
	if (time > 2400)
		return false;
	const uint32_t hour = time / 100;
	const uint32_t minute = time % 100;
	return hour <= 24 && minute < 60 && (hour < 24 || minute == 0);
}

inline uint32_t hhmmToActionTime(uint32_t hhmm)
{
	return hhmm * 100000;
}

inline bool validActionTime(uint32_t action_time)
{
	const uint32_t millisecond = action_time % 1000;
	const uint32_t hhmmss = action_time / 1000;
	const uint32_t second = hhmmss % 100;
	const uint32_t minute = (hhmmss / 100) % 100;
	const uint32_t hour = hhmmss / 10000;
	return hour < 24 && minute < 60 && second < 60 && millisecond < 1000;
}

inline bool validActionDate(uint32_t action_date)
{
	const uint32_t year = action_date / 10000;
	const uint32_t month = (action_date / 100) % 100;
	const uint32_t day = action_date % 100;
	if (year < 1900 || year > 9999 || month == 0 || month > 12 || day == 0)
		return false;
	static const uint32_t days_per_month[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	const bool leap_year = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
	const uint32_t max_day = month == 2 && leap_year ? 29 : days_per_month[month - 1];
	return day <= max_day;
}

inline bool tradingSecondsAt(WTSSessionInfo& session, uint32_t action_time,
	uint32_t& trading_seconds, std::string& diagnostic)
{
	if (!vwap_profile::validateTradingSections(session, diagnostic))
		return false;
	const uint32_t millisecond = action_time % 1000;
	const uint32_t hhmmss = action_time / 1000;
	const uint32_t second = hhmmss % 100;
	const uint32_t hhmm = hhmmss / 100;
	const uint32_t minute = hhmm % 100;
	const uint32_t hour = hhmm / 100;
	if (hour >= 24 || minute >= 60 || second >= 60 || millisecond >= 1000)
		return false;

	const uint32_t offset_hhmm = session.offsetTime(hhmm, true);
	const uint32_t point = (offset_hhmm / 100 * 60 + offset_hhmm % 100) * 60 + second;
	bool is_section_start = false;
	for (const WTSSessionInfo::TradingSection& section : session.getTradingSections())
	{
		const uint32_t start = (section.first / 100 * 60 + section.first % 100) * 60;
		is_section_start = is_section_start || point == start;
	}
	uint32_t elapsed = 0;
	uint32_t previous_end = 0;
	bool has_previous_section = false;
	for (const WTSSessionInfo::TradingSection& section : session.getTradingSections())
	{
		const uint32_t start = (section.first / 100 * 60 + section.first % 100) * 60;
		const uint32_t end = (section.second / 100 * 60 + section.second % 100) * 60;
		if (end <= start)
		{
			diagnostic = "session contains a zero-length or reversed trading section";
			return false;
		}
		if (has_previous_section && start < previous_end)
		{
			diagnostic = "session trading sections overlap or are out of order";
			return false;
		}
		if (point >= start && point <= end)
		{
			if (point == end && !is_section_start && (second != 0 || millisecond != 0))
			{
				diagnostic = "session close is inclusive only at HH:MM:00.000";
				return false;
			}
			trading_seconds = elapsed + point - start;
			diagnostic.clear();
			return true;
		}
		elapsed += end - start;
		previous_end = end;
		has_previous_section = true;
	}
	diagnostic = "time is outside this trading session";
		return false;
}
/**
 * @brief 获取工厂名称
 * @return 执行器工厂名称
 */
const char * WtStockVWapExeUnit::getFactName()
{
	return FACT_NAME;
}

/**
 * @brief 获取执行单元名称
 * @return 执行单元名称
 */
const char * WtStockVWapExeUnit::getName()
{
	return "WtStockVWapExeUnit";
}

/**
 * @brief 初始化执行单元
 * @details 从配置中读取参数并设置执行单元的各项配置，加载相关资源
 * @param ctx 执行上下文对象，用于访问交易环境
 * @param stdCode 标准化合约代码
 * @param cfg 配置项对象
 */
void WtStockVWapExeUnit::init(ExecuteContext * ctx, const char * stdCode, WTSVariant * cfg)
{
	_config_valid = false;
	_execution_state = ExecutionState::active;
	_pending_remainder = 0;
	_pending_action = PendingAction::none;
	_retry_not_before = 0;
	_target_pos = 0;
	_target_pos_valid = false;
	_this_target = 0;
	_target_generation = 0;
	_order_target_generations.clear();
	if (ctx == nullptr || stdCode == nullptr || stdCode[0] == '\0' || cfg == nullptr)
		return;

	ExecuteUnit::init(ctx, stdCode, cfg);
	auto fail_init = [this, stdCode](const std::string& reason) {
		_config_valid = false;
		if (_ctx != nullptr)
			_ctx->writeLog(fmt::format("{} StockVWap initialization failed: {}", stdCode, reason).c_str());
	};

	try
	{
		// Context getters return borrowed values; the unit owns one retained reference.
		_comm_info = ctx->getCommodityInfo(stdCode);
		if (_comm_info != nullptr)
			_comm_info->retain();
		_sess_info = ctx->getSessionInfo(stdCode);
		if (_sess_info != nullptr)
			_sess_info->retain();
		if (_comm_info == nullptr || _sess_info == nullptr)
		{
			fail_init(_comm_info == nullptr ? "commodity information is unavailable" : "trading session is unavailable");
			return;
		}

		const StringVector code_parts = StrUtil::split(stdCode, ".");
		if (code_parts.size() != 3 || code_parts[2].empty())
		{
			fail_init("standard code must contain exchange, product, and numeric code segments");
			return;
		}
		size_t parsed_digits = 0;
		const int code = std::stoi(code_parts[2], &parsed_digits);
		if (parsed_digits != code_parts[2].size() || code < 0)
		{
			fail_init("standard code numeric segment is invalid");
			return;
		}
		const char* product = _comm_info->getProduct();
		if (product == nullptr ||
			(strcmp(product, stockStr) != 0 && strcmp(product, cbondStr) != 0))
		{
			fail_init("commodity product must be STK or CBOND");
			return;
		}
		_is_KC = strcmp(product, stockStr) == 0 && code >= 688000;
		_min_hands = strcmp(product, cbondStr) == 0 ? 10.0 : (_is_KC ? 200.0 : 100.0);

		_begin_time = cfg->getUInt32("begin_time");
		_end_time = cfg->getUInt32("end_time");
		_ord_sticky = cfg->getUInt32("ord_sticky");
		_tail_secs = cfg->getUInt32("tail_secs");
		_total_times = cfg->getUInt32("total_times");
		_price_mode = cfg->getUInt32("price_mode");
		_price_offset = cfg->getUInt32("offset");
		_order_lots = cfg->getDouble("lots");
		const bool hasMinOpenLots = cfg->has("minopenlots");
		const double configuredMinOpenLots = hasMinOpenLots ? cfg->getDouble("minopenlots") : _min_hands;

		if (_total_times == 0)
		{
			fail_init("total_times must be greater than zero");
			return;
		}
		if (!validHHMM(_begin_time) || !validHHMM(_end_time))
		{
			fail_init("begin_time and end_time must be valid HHMM values");
			return;
		}
		if (_price_mode > 2)
		{
			fail_init("price_mode must be 0, 1, or 2");
			return;
		}
		if (!std::isfinite(_order_lots) || !decimal::gt(_order_lots, 0))
		{
			fail_init("lots must be a finite positive number");
			return;
		}
		if (!std::isfinite(configuredMinOpenLots) || !decimal::gt(configuredMinOpenLots, 0))
		{
			fail_init("minopenlots must be a finite positive number");
			return;
		}
		_min_open_lots = _is_KC ? max(configuredMinOpenLots, _min_hands) : min(configuredMinOpenLots, _min_hands);
		if (_comm_info->getTradingMode() == TradingMode::TM_Long)
			_is_t0 = true;

		std::string diagnostic;
		const uint32_t begin_action_time = hhmmToActionTime(_begin_time);
		const uint32_t end_action_time = hhmmToActionTime(_end_time);
		size_t begin_bucket = 0, end_bucket = 0;
		if (!vwap_profile::mapActionTime(*_sess_info, begin_action_time, 100000, begin_bucket, diagnostic) ||
			!tradingSecondsAt(*_sess_info, begin_action_time, _begin_trading_secs, diagnostic))
		{
			fail_init("begin_time is outside the trading session: " + diagnostic);
			return;
		}
		if (!vwap_profile::mapActionTime(*_sess_info, end_action_time, 100000, end_bucket, diagnostic) ||
			!tradingSecondsAt(*_sess_info, end_action_time, _end_trading_secs, diagnostic))
		{
			fail_init("end_time is outside the trading session: " + diagnostic);
			return;
		}
		(void)begin_bucket;
		(void)end_bucket;
		if (_end_trading_secs <= _begin_trading_secs)
		{
			fail_init("end_time must follow begin_time in session trading time");
			return;
		}
		_total_secs = _end_trading_secs - _begin_trading_secs;
		if (_tail_secs > _total_secs)
		{
			fail_init("tail_secs must not exceed the trading-time duration");
			return;
		}
		_fire_span = (_total_secs - _tail_secs) / _total_times;

		std::string filename = "Vwap_";
		const char* commodity_name = _comm_info->getName();
		if (commodity_name == nullptr || commodity_name[0] == '\0')
		{
			fail_init("commodity name is empty");
			return;
		}
		filename += commodity_name;
		filename += ".txt";
		std::ifstream file(filename.c_str(), std::ios::binary);
		if (!file.is_open())
		{
			fail_init("VWAP distribution file is missing: " + filename);
			return;
		}
		const std::string profile_text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		std::vector<double> profile;
		if (!vwap_profile::loadProfile(profile_text, *_sess_info, profile, diagnostic))
		{
			fail_init("invalid VWAP distribution file " + filename + ": " + diagnostic);
			return;
		}
		VwapAim.swap(profile);
		_config_valid = true;
		ctx->writeLog(fmt::format("执行单元WtStockVWapExeUnit[{}] 初始化完成,订单超时 {} 秒,执行时限 {} 秒,收尾时间 {} 秒",
			stdCode, _ord_sticky, _total_secs, _tail_secs).c_str());
	}
	catch (const std::exception& ex)
	{
		fail_init(std::string("invalid configuration or initialization exception: ") + ex.what());
	}
	catch (...)
	{
		fail_init("unknown initialization exception");
	}
}

/**
 * @brief 处理订单状态回调
 * @details 当订单状态变化时被调用，如成交、撤销等情况。更新订单监控器状态并处理相应的业务逻辑
 * @param localid 本地订单ID
 * @param stdCode 标准化合约代码
 * @param isBuy 是否为买入订单
 * @param leftover 剩余未成交数量
 * @param price 订单委托价格
 * @param isCanceled 订单是否已撤销
 */
void WtStockVWapExeUnit::on_order(uint32_t localid, const char * stdCode, bool /*isBuy*/, double leftover, double /*price*/, bool isCanceled)
{
	if (!_config_valid || _ctx == nullptr)
		return;
	// 检查订单是否在监控器中
	if (!_orders_mon.has_order(localid))
		return;
	bool orderTargetIsCurrent = false;
	
	// 处理订单完成或撤销的情况
	if (isCanceled || leftover == 0)
	{
		const auto generation = _order_target_generations.find(localid);
		orderTargetIsCurrent = generation != _order_target_generations.end() &&
			generation->second.generation == _target_generation && generation->second.retryable;
		if (generation != _order_target_generations.end())
			_order_target_generations.erase(generation);
		// 从监控器中移除订单
		_orders_mon.erase_order(localid);
		// 如果有撤单计数，减少并记录日志
		if (_cancel_cnt > 0)
		{
			_cancel_cnt--;
			_ctx->writeLog(fmtutil::format("[{}@{}] Order of {} cancelling done, cancelcnt -> {}", __FILE__, __LINE__, _code.c_str(), _cancel_cnt));
		}
	}

	// 如果订单全部成交（非撤销），重置撤单次数计数
	if (leftover == 0 && !isCanceled) {
		_cancel_times = 0;
		_ctx->writeLog(fmtutil::format("Order {} has filled", localid));
	}
	
	// 如果全部订单已撤销，这个时候一般是遇到要超时撤单（挂单超时） 
	if (isCanceled && _cancel_cnt == 0 && _config_valid)
	{
		_ctx->writeLog(fmtutil::format("Order {} of {} canceled, {} waits for the next fresh in-session tick",
			localid, stdCode, orderTargetIsCurrent ? "retry" : "target recalculation"));
		_cancel_times++;
		_pending_action = orderTargetIsCurrent ? PendingAction::retry : PendingAction::recalculate;
		_retry_not_before = 0;
	}

	// 处理异常情况：订单未撤销但撤单计数不为0
	if (!isCanceled && _cancel_cnt != 0) { // 一般出现问题，需要返回检查  触发撤单 cnt++,onorder响应处理才会--
		_ctx->writeLog(fmtutil::format("Order {} of {}  hasn't canceled, error will be return ", localid, stdCode));
		return;
	}
}

/**
 * @brief 处理交易通道就绪回调
 * @details 当交易通道准备就绪时调用，主要处理未完成订单的状态同步
 *          包含三种情况的处理：
 *          1. 有未完成订单但本地无订单记录：清除所有未被管理的订单
 *          2. 无未完成订单但本地有订单记录：清除本地错误订单
 *          3. 其他异常情况处理
 */
void WtStockVWapExeUnit::on_channel_ready()
{
	if (!_config_valid || _ctx == nullptr)
		return;
	// 标记交易通道为就绪状态
	_channel_ready = true;
	if (hasUnverifiedSubmission())
	{
		_ctx->writeLog(fmt::format("{} submission outcome unverified; channel reconciliation and automatic actions held", _code).c_str());
		return;
	}
	
	// 获取未完成的订单数量
	double undone = _ctx->getUndoneQty(_code.c_str());
	
	// 情况1: 如果有未完成订单但本地没有订单记录，需要清除未被管理的订单
	if (!decimal::eq(undone, 0) && !_orders_mon.has_order())
	{
		if (!wtRunIfNoUnverifiedTimeout(_orders_mon, [this, undone]() {
		/*
		 *	如果未完成单不为0，而OMS没有订单
		 *	这说明有未完成单不在监控之中,全部撤销掉
		 *	因为这些订单没有本地订单号，无法直接进行管理
		 *	这种情况，就是刚启动的时候，上次的未完成单或者外部的挂单
		 */
		_ctx->writeLog(fmt::format("{} unmanaged orders of {},cancel all", undone, _code).c_str());

		// 根据未完成订单的正负判断是买入还是卖出订单
		bool isBuy = (undone > 0);
		
		// 撤销对应方向的所有未完成订单
		if (hasUnverifiedSubmission())
			return;
		OrderIDs ids = _ctx->cancel(_code.c_str(), isBuy);
		
		// 将撤销的订单添加到监控器中进行跟踪
		_orders_mon.push_order(ids.data(), ids.size(), _ctx->getCurTime());
		
		// 更新撤单计数
		_cancel_cnt += ids.size();

		// 记录撤单日志
		_ctx->writeLog(fmtutil::format("[{}@{}]cancelcnt -> {}", __FILE__, __LINE__, _cancel_cnt));
		}))
			_ctx->writeLog(fmt::format("Unmanaged-order cancellation for {} suppressed after an unverified timeout", _code).c_str());
	}
	// 情况2: 如果没有未完成订单但本地有订单记录，需要清除本地错误订单
	else if (decimal::eq(undone, 0) && _orders_mon.has_order())
	{	/*
		 *	By Wesey @ 2021.12.13
		 *	如果未完成单为0，但是OMS中是有订单的
		 *	说明OMS中是错单，需要清理掉，不然超时撤单就会出错
		 *	这种情况，一般是断线重连以后，之前下出去的订单，并没有真正发送到柜台
		 *	所以这里需要清理掉本地订单
		 */
		// 记录日志并清除所有本地订单
		_ctx->writeLog(fmtutil::format("Local orders of {} not confirmed in trading channel, clear all", _code.c_str()));
		_orders_mon.clear_orders();
	}
	// 情况3: 其他异常情况处理
	else
	{
		// 记录日志，显示当前未完成订单和本地监控状态
		_ctx->writeLog(fmtutil::format("Unrecognized condition while channle ready, {:.2f} live orders of {} exists, local orders {}exist",
			undone, _code.c_str(), _orders_mon.has_order() ? "" : "not "));
	}
	
	// 通道就绪后触发一次计算，重新调整仓位
	if (_pending_action == PendingAction::none)
		do_calc();
	else
		_ctx->writeLog(fmt::format("{} order callback is pending; calculation waits for a fresh tick", _code).c_str());
}

bool WtStockVWapExeUnit::map_action_time(uint32_t action_time,
	size_t& bucket_index, std::string& diagnostic)
{
	if (!_sess_info)
	{
		diagnostic = "trading session is unavailable";
		return false;
	}
	if (!vwap_profile::mapActionTime(*_sess_info, action_time,
		VwapAim.size(), bucket_index, diagnostic))
		return false;
	uint32_t trading_seconds = 0;
	if (!tradingSecondsAt(*_sess_info, action_time, trading_seconds, diagnostic))
		return false;
	if (trading_seconds < _begin_trading_secs || trading_seconds > _end_trading_secs)
	{
		diagnostic = "tick time is outside the configured VWAP execution window";
		return false;
	}
	return true;
}

bool WtStockVWapExeUnit::uses_stock_lot_rules() const
{
	const char* product = _comm_info != nullptr ? _comm_info->getProduct() : nullptr;
	return product != nullptr && strcmp(product, stockStr) == 0;
}

void WtStockVWapExeUnit::mark_pending_remainder(double target_remainder,
	double executable_remainder, double sellable_position, const char* reason)
{
	_execution_state = ExecutionState::pending_remainder;
	_pending_remainder = target_remainder;
	if (_ctx != nullptr)
		_ctx->writeLog(fmt::format("{} target remains incomplete: signed remainder={} shares, currently executable={} shares, sellable={} shares, minimum stock lot={} shares; stock order not sent ({})",
			_code, target_remainder, executable_remainder, sellable_position, _min_hands,
			reason != nullptr ? reason : "below minimum stock lot").c_str());
}


/**
 * @brief 处理行情数据回调
 * @details 当收到新的行情数据时被调用，更新内部状态并检查超时订单，触发交易计算
 * @param newTick 新的行情数据
 */
void WtStockVWapExeUnit::on_tick(WTSTickData * newTick)
{
	// 检查行情数据是否有效且属于当前合约
	if (newTick == NULL || _code.compare(newTick->code()) != 0)
		return;
	if (hasUnverifiedSubmission())
		return;
	if (!_config_valid)
		return;
	if (_unsupported_date_contract)
		return;

	const uint64_t curTickTime = (uint64_t)newTick->actiondate() * 1000000000 + newTick->actiontime();
	if (!validActionDate(newTick->actiondate()))
	{
		_ctx->writeLog(fmt::format("{} invalid action date {}, skip VWAP order", _code,
			newTick->actiondate()).c_str());
		if (_last_tick)
		{
			_last_tick->release();
			_last_tick = nullptr;
		}
		return;
	}
	if (!validActionTime(newTick->actiontime()))
	{
		_ctx->writeLog(fmt::format("{} invalid action time {}, skip VWAP order", _code,
			newTick->actiontime()).c_str());
		if (_last_tick)
		{
			_last_tick->release();
			_last_tick = nullptr;
		}
		return;
	}

	const uint32_t trading_date = newTick->tradingdate();
	if (trading_date != 0 && !validActionDate(trading_date))
	{
		_ctx->writeLog(fmt::format("{} invalid trading_date {}, skip VWAP order", _code, trading_date).c_str());
		if (_last_tick)
		{
			_last_tick->release();
			_last_tick = nullptr;
		}
		return;
	}
	if (trading_date == 0 && _sess_info->getOffsetMins() != 0 && !_date_contract_warning_logged)
	{
		_date_contract_warning_logged = true;
		_ctx->writeLog(fmt::format("{} nonzero-offset session has no trading_date; natural action_date ordering is used, provider cross-midnight semantics are unverified",
			_code).c_str());
	}

	if (_high_water_action_date == newTick->actiondate() &&
		newTick->actiontime() < _high_water_action_time && _sess_info->getOffsetMins() != 0)
	{
		const uint32_t previous_offset_date = _sess_info->getOffsetDate(_high_water_action_date, _high_water_action_time / 100000);
		const uint32_t current_offset_date = _sess_info->getOffsetDate(newTick->actiondate(), newTick->actiontime() / 100000);
		if (previous_offset_date != current_offset_date)
		{
			_unsupported_date_contract = true;
			_ctx->writeLog(fmt::format("{} unsupported date rollover: action_date stayed {} while time moved backward across offset dates {} -> {}; provider natural-date semantics are not supported",
				_code, newTick->actiondate(), previous_offset_date, current_offset_date).c_str());
			if (_last_tick)
			{
				_last_tick->release();
				_last_tick = nullptr;
			}
			return;
		}
	}
	if (curTickTime <= _tick_time_high_water)
	{
		_ctx->writeLog(fmt::format("{} stale tick {} is not newer than observed high-water {}", _code,
			curTickTime, _tick_time_high_water).c_str());
		return;
	}

	if (trading_date != 0)
	{
		const uint32_t natural_offset_date = _sess_info->getOffsetDate(newTick->actiondate(), newTick->actiontime() / 100000);
		if (trading_date < natural_offset_date)
		{
			_unsupported_date_contract = true;
			_ctx->writeLog(fmt::format("{} unsupported action/trading date combination: action_date={} time={} natural offset date={} but trading_date={} precedes it; provider date semantics are not supported",
				_code, newTick->actiondate(), newTick->actiontime(), natural_offset_date, trading_date).c_str());
			if (_last_tick)
			{
				_last_tick->release();
				_last_tick = nullptr;
			}
			return;
		}
	}

	_tick_time_high_water = curTickTime;
	_high_water_action_date = newTick->actiondate();
	_high_water_action_time = newTick->actiontime();
	size_t bucket_index = 0;
	std::string time_diagnostic;
	if (!map_action_time(newTick->actiontime(), bucket_index, time_diagnostic))
	{
		_ctx->writeLog(fmt::format("{} invalid or out-of-session action time {}, skip VWAP order: {}",
			_code, newTick->actiontime(), time_diagnostic).c_str());
		// A later callback must not place an order against an old in-session tick.
		if (_last_tick)
		{
			_last_tick->release();
			_last_tick = nullptr;
		}
		return;
	}
	(void)bucket_index;

	// 标记是否为第一笔行情
	bool isFirstTick = false;
	
	// 释放旧的行情数据
	if (_last_tick) {
		_last_tick->release();
	}
	else {
		// 如果之前没有行情数据，这是第一笔行情
		isFirstTick = true;
	}
	// 保存新的行情数据并增加引用计数
	_last_tick = newTick;
	_last_tick->retain();
	if (_channel_ready && _pending_action != PendingAction::none && curTickTime > _last_tick_time)
	{
		const PendingAction action = _pending_action;
		_pending_action = PendingAction::none;
		if (action == PendingAction::retry)
		{
			if (!_target_pos_valid)
			{
				_ctx->writeLog(fmt::format("{} canceled-order retry skipped before a stock VWAP target was established", _code).c_str());
				return;
			}

			const int64_t tick_epoch = TimeUtils::makeTime(_last_tick->actiondate(), _last_tick->actiontime());
			if (_retry_not_before != 0 && tick_epoch < _retry_not_before)
			{
				_pending_action = PendingAction::retry;
				_ctx->writeLog(fmt::format("{} retry remains pending until configured retry time {} (new tick time {})",
					_code, _retry_not_before, tick_epoch).c_str());
				return;
			}
			_retry_not_before = 0;
			if (!decimal::eq(_ctx->getUndoneQty(_code.c_str()), 0))
			{
				_pending_action = PendingAction::retry;
				_ctx->writeLog(fmt::format("{} canceled-order retry deferred while another order remains open", _code).c_str());
				return;
			}
			const double retryQty = get_retry_quantity();
			if (!decimal::eq(retryQty, 0))
				fire_at_once(retryQty);
			else if (uses_stock_lot_rules())
			{
				const double total_position = _ctx->getPosition(_code.c_str(), false);
				const double target_remainder = _this_target - total_position;
				const double sellable_position = _is_t0 ? total_position : _ctx->getPosition(_code.c_str(), true, 1);
				const double executable_remainder = decimal::lt(target_remainder, 0)
					? -min(abs(target_remainder), sellable_position) : target_remainder;
				if (!decimal::eq(target_remainder, 0))
					mark_pending_remainder(target_remainder, executable_remainder, sellable_position,
						decimal::lt(abs(executable_remainder), _min_hands)
							? "canceled retry has less than one executable stock lot"
							: "canceled retry cannot be safely sized as a whole stock lot");
				else
					_ctx->writeLog(fmt::format("{} canceled-order retry is no longer needed", _code).c_str());
				_last_tick_time = curTickTime;
			}
			else
				_ctx->writeLog(fmt::format("{} canceled-order retry is no longer needed or cannot be safely sized", _code).c_str());
		}
		else
		{
			_ctx->writeLog(fmt::format("{} rejected-order recalculation uses fresh tick {}", _code, curTickTime).c_str());
			do_calc();
		}
		return;
	}

	// 如果是第一笔行情，需要检查目标仓位是否需要调整
	if (isFirstTick)
	{
		// 获取当前目标仓位、未完成订单和实际仓位
		double newVol = _target_pos;
		const char* stdCode = _code.c_str();
		double undone = _ctx->getUndoneQty(stdCode);
		double realPos = _ctx->getPosition(stdCode, false);
		
		// 如果目标仓位不等于未完成订单加实际仓位，需要调整
		if (!decimal::eq(newVol, undone + realPos))
		{
			// 触发计算逻辑进行仓位调整
			do_calc();
		}
	}
	else // 非第一笔行情处理
	{
		// 获取当前时间
		uint64_t now = _ctx->getCurTime();
		
		// 检查是否有超时订单需要撤销
		if (_ord_sticky != 0 && _orders_mon.has_order())
		{
			// 检查订单是否超时，如果超时则撤销
			_orders_mon.check_orders_with_generation(_ord_sticky, now, [this, now](uint32_t localid, uint64_t generation) {
				if (!_orders_mon.mark_unverified_timeout(localid, generation, _ord_sticky, now))
					return; // 终态或 localid 已复用；不得复活旧快照
				_ctx->writeLog(fmt::format("Expired order {} of {} cannot be auto-canceled: legacy localid is unverified; order remains monitored",
					localid, _code.c_str()).c_str());
			});
		}
		if (_orders_mon.has_unverified_timeout())
			return;
		
		// 如果没有撤单操作，且距离上次发单时间超过了发单间隔，则触发新的计算
		if (now - _last_fire_time >= _fire_span * 1000)
		{
			do_calc();
		}
	}
}

/**
 * @brief 处理成交回调
 * @details 当有成交发生时被调用，记录成交日志
 * @param localid 本地订单ID
 * @param stdCode 标准化合约代码
 * @param isBuy 是否为买入成交
 * @param vol 成交数量
 * @param price 成交价格
 */
void WtStockVWapExeUnit::on_trade(uint32_t localid, const char * stdCode, bool /*isBuy*/, double vol, double price)
{
	if (!_config_valid || _ctx == nullptr)
		return;
	// 记录成交日志
	_ctx->writeLog(fmtutil::format("Order {} of {} traded: {} @ {}", localid, stdCode, vol, price));
}
/*
下单结果回报
*/
void WtStockVWapExeUnit::on_entrust(uint32_t localid, const char * /*stdCode*/, bool bSuccess, const char * /*message*/)
{
	if (!_config_valid)
		return;
	if (!bSuccess)
	{
		//如果不是我发出去的订单,我就不管了
		if (!_orders_mon.has_order(localid))
			return;

		_orders_mon.erase_order(localid);
		_pending_action = PendingAction::recalculate;
		_retry_not_before = 0;
		_ctx->writeLog(fmt::format("{} rejected order deferred until the next fresh in-session tick", _code).c_str());
	}
}
/**
 * @brief VWAP执行单元的核心计算逻辑
 * @details 根据当前市场状况、目标仓位和实际仓位计算下一步操作
 *          包括判断是否需要发单、发单数量、价格策略等
 *          实现了根据VWAP分布序列分批次执行大单的核心逻辑
 */
void WtStockVWapExeUnit::do_calc()
{
	if (hasUnverifiedSubmission() || _orders_mon.has_unverified_timeout())
		return;
	if (!_config_valid || !_target_pos_valid || _ctx == nullptr || _last_tick == nullptr)
		return;

	// 防止重入计算
	CalcFlag flag(&_in_calc);
	if (flag)
		return;

	// 加锁保护共享数据
	StdUniqueLock lock(_mtx_calc);
	size_t bucketIndex = 0;
	std::string timeDiagnostic;
	if (!map_action_time(_last_tick->actiontime(), bucketIndex, timeDiagnostic))
	{
		_ctx->writeLog(fmtutil::format("{} last tick is outside the VWAP session/window: {}", _code, timeDiagnostic));
		return;
	}
	const uint64_t curTickTime = (uint64_t)_last_tick->actiondate() * 1000000000 + _last_tick->actiontime();
	if (curTickTime <= _last_tick_time)
	{
		_ctx->writeLog(fmtutil::format("No tick of {} updated, {} <= {}, execute later", _code, curTickTime, _last_tick_time));
		return;
	}
	if (!_channel_ready)
		return;
	const char* code = _code.c_str();
	double undone = _ctx->getUndoneQty(code);
	// TraderAdapter returns a map snapshot, but its WTSOrderInfo values are shared
	// objects and this call is not atomic with getUndoneQty(). Only use snapshot
	// IDs here; reading order fields would require a synchronization contract the
	// ExecuteContext API does not provide. A null map means the snapshot is
	// unavailable (for example, before adapter initialization or in ExecMocker),
	// so it cannot safely be treated as an empty order set.
	OrderMap* grossOrders = _ctx->getOrders(code);
	if (grossOrders == nullptr)
	{
		_last_tick_time = curTickTime;
		_ctx->writeLog(fmt::format("{} getOrders returned null; gross same-code order state is unavailable, failing closed until a reliable order snapshot is provided",
			_code).c_str());
		return;
	}
	std::vector<uint32_t> grossOrderIds;
	bool invalidOrderSnapshot = false;
	for (auto it = grossOrders->begin(); it != grossOrders->end(); ++it)
	{
		if (it->second == nullptr)
		{
			invalidOrderSnapshot = true;
			break;
		}
		grossOrderIds.push_back(it->first);
	}
	grossOrders->release();
	if (invalidOrderSnapshot)
	{
		_last_tick_time = curTickTime;
		_ctx->writeLog(fmt::format("{} getOrders snapshot contains an invalid order entry; gross exposure cannot be checked, failing closed",
			_code).c_str());
		return;
	}
	auto holdForGrossOrders = [this, &grossOrderIds](const char* reason) {
		// getOrders is account-wide and exposes no execution-unit owner. Do not
		// claim these IDs in this unit's monitor; owner-scoped handling is pending.
		_ctx->writeLog(fmt::format("{} found {} gross same-code order(s) without verifiable owner; no localid cancellation or ownership registration, execution held: {}",
			_code, grossOrderIds.size(), reason).c_str());
	};
	// Consume each valid quote at most once, including no-order/pending outcomes.
	_last_tick_time = curTickTime;
	if (_cancel_cnt != 0)
	{
		_ctx->writeLog(fmt::format("{} waits for outstanding cancellation reports before recalculation", _code).c_str());
		return;
	}
	const bool netZeroWithGrossOrders = decimal::eq(undone, 0) && !grossOrderIds.empty();
	if (netZeroWithGrossOrders)
	{
		holdForGrossOrders(fmt::format("gross same-code orders exist while net undone is {}", undone).c_str());
		return;
	}
	const double requestedTarget = get_real_target(_target_pos);
	double newVol = requestedTarget;
	// ExecuteContext 合同：validOnly=true 返回可用仓，false 返回总仓。
	double realPos = _ctx->getPosition(code, false);//总仓位
	double vailyPos = _ctx->getPosition(code, true, 1);//可用多头仓位，可卖量
	const double requestedRemainder = requestedTarget - realPos;
	if (!grossOrderIds.empty() && decimal::eq(requestedRemainder, 0))
	{
		holdForGrossOrders("gross same-code orders remain after the requested target was reached");
		return;
	}
	if (_execution_state == ExecutionState::complete)
		return;
	_execution_state = ExecutionState::active;
	_pending_remainder = 0;
	double diffQty = requestedRemainder;//目标差
	if (_is_t0)
		vailyPos = realPos;
	double target_pos = max(realPos - vailyPos, newVol);
	bool targetLimitedBySellablePosition = false;
	if (!decimal::eq(target_pos, newVol))
	{
		targetLimitedBySellablePosition = true;
		_ctx->writeLog(fmtutil::format("{} can sell hold pos not enough, target adjust {}->{}", code, newVol, target_pos));
		newVol = target_pos;
		diffQty = newVol - realPos;
	}
	if (_last_tick == NULL)
	{
		_ctx->writeLog(fmt::format("{}没有最新的tick数据，退出执行逻辑", _code).c_str());
		return;
	}
	if (decimal::ge(_start_price, 0)) {//补一次
		_start_price = _last_tick->price();
	}
	//每一次发单要保障成交,所以如果有未完成单,说明上一轮没完成
	//有未完成订单&&与实际仓位变动方向相反
	//则需要撤销现有订单
	bool undoneIsBuy = decimal::gt(undone, 0);//undone>0,isbuy=1
	double orderIntentDiff = decimal::eq(diffQty, 0) ? requestedRemainder : diffQty;
	// If the exact target is already reached, any remaining live order would move
	// the account away from it. This is especially important for clear_all_position
	// when the current position is zero but an earlier buy is still open.
	if (decimal::eq(diffQty, 0) && decimal::eq(requestedRemainder, 0) &&
		!decimal::eq(undone, 0))
		orderIntentDiff = -undone;
	const bool oppositeLiveOrder = decimal::lt(orderIntentDiff * undone, 0);
	// An order from the prior target can still point in the right direction but
	// exceed the account's remaining total target gap (for example, buy 100 left
	// open after the requested buy target shrinks to 50). Cancel it before waiting
	// on undone. For T+1 sells, requestedRemainder is the total-position budget;
	// diffQty is only the currently executable increment and may be zero while an
	// already-open sell correctly reserves the otherwise unavailable shares.
	const bool oversizedSameDirectionOrder = decimal::gt(orderIntentDiff * undone, 0) &&
		decimal::gt(abs(undone), abs(requestedRemainder));
	if (oppositeLiveOrder || oversizedSameDirectionOrder)
	{
		if (hasUnverifiedSubmission())
			return;
		OrderIDs ids = _ctx->cancel(code, undoneIsBuy);
		if (!ids.empty())
		{
			_orders_mon.push_order(ids.data(), ids.size(), _ctx->getCurTime());
			_cancel_cnt += ids.size();
			if (oversizedSameDirectionOrder)
				_ctx->writeLog(fmtutil::format("[{}@{}] live order {} exceeds current total target remainder {} (effective gap {}), cancelcnt -> {}",
					__FILE__, __LINE__, undone, requestedRemainder, diffQty, _cancel_cnt));
			else
				_ctx->writeLog(fmtutil::format("[{}@{}] live opposite order of {} canceled, cancelcnt -> {}", __FILE__, __LINE__, _code.c_str(), _cancel_cnt));//相反的订单取消
		}
		return;
	}
	if (!decimal::eq(undone, 0))
	{
		_ctx->writeLog(fmt::format("{}上一轮有挂单未完成,暂时退出本轮执行", _code).c_str());
		return;
	}
	if (decimal::eq(diffQty, 0))
	{
		if (uses_stock_lot_rules() &&
			(targetLimitedBySellablePosition || !decimal::eq(requestedRemainder, 0)))
		{
			mark_pending_remainder(requestedRemainder, diffQty, vailyPos,
				"T+1 sellable position currently prevents reaching the requested target");
		}
		else if (decimal::eq(requestedRemainder, 0))
		{
			_execution_state = ExecutionState::complete;
			_pending_remainder = 0;
			_ctx->writeLog(fmtutil::format("{} target position {} reached exactly; execution complete", _code, requestedTarget));
		}
		return;
	}
	if (uses_stock_lot_rules() && decimal::lt(abs(diffQty), _min_hands))
	{
		const char* reason = targetLimitedBySellablePosition
			? "T+1 sellable quantity is below one stock lot"
			: (decimal::gt(diffQty, 0) ? "buy remainder is below one stock lot" : "sell remainder is below one stock lot");
		mark_pending_remainder(requestedRemainder, diffQty, vailyPos, reason);
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
	double aimQty = VwapAim[bucketIndex];//取到对应时刻的目标vwapaim （递增）

	uint32_t leftTimes = _total_times - _fired_times;
	_ctx->writeLog(fmt::format("第 {} 次发单", _fired_times + 1).c_str());
	_Vwap_vol = aimQty - curPos;//若在本分钟发单，对应的VWapVol
	bool bNeedShowHand = false;
	double curQty = 0;
	bool isBuy = decimal::gt(diffQty, 0);
	double qtyMagnitude = 0;
	if (leftTimes == 0 && !decimal::eq(diffQty, 0))
	{
		bNeedShowHand = true;
		qtyMagnitude = abs(diffQty);
	}
	else {
		qtyMagnitude = max(_Vwap_vol, _min_open_lots);//VWAP 分布给出的本轮切片量
	}
	qtyMagnitude = max(0.0, (double)round_hands(qtyMagnitude, _min_open_lots));
	qtyMagnitude = min(qtyMagnitude, abs(diffQty));
	if (isBuy)
	{
		// 股票买入按最小手数下单，且不超过剩余目标量。
		curQty = std::floor(qtyMagnitude / _min_hands) * _min_hands;
	}
	else if (!uses_stock_lot_rules() && decimal::lt(vailyPos, _min_hands))
	{
		// Preserve the existing non-stock rule; stock odd-lot order legality is not assumed.
		if (decimal::ge(abs(diffQty), vailyPos))
			curQty = -vailyPos;
	}
	else
	{
		// 减仓量不得超过可卖仓；截断后向下取整，避免超仓或非整手订单。
		double sellQty = min(qtyMagnitude, vailyPos);
		// Keep CBOND's existing bounded tail-sell behavior. The stock lot contract
		// below this branch must not enlarge a final bond sell to the full target gap.
		if (!uses_stock_lot_rules())
			sellQty = min(sellQty, _min_open_lots);
		curQty = -std::floor(sellQty / _min_hands) * _min_hands;
	}
	if (decimal::eq(curQty, 0))
	{
		if (uses_stock_lot_rules())
			mark_pending_remainder(requestedRemainder, diffQty, vailyPos,
				"VWAP slice cannot produce one legal stock lot");
		else
			_ctx->writeLog(fmtutil::format("{} remaining target {} is below a valid order quantity", code, diffQty));
		return;
	}

	//设定本轮目标仓位
	_this_target = realPos + curQty;

	WTSTickData* curTick = _last_tick;
	uint64_t now = _ctx->getCurTime();
	isBuy = decimal::gt(diffQty, 0);
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
	if (hasUnverifiedSubmission() || _orders_mon.has_unverified_timeout())
		return;
	if (curQty > 0)
	{
		ids = _ctx->buy(code, targetPx, abs(curQty));
	}
	else {
		ids = _ctx->sell(code, targetPx, abs(curQty));
	}
	if (ids.empty())
	{
		_unverified_submission.store(true);
		_pending_action = PendingAction::none;
		_retry_not_before = 0;
		_ctx->writeLog(fmt::format("{} order returned no IDs; submission outcome unverified, automatic actions held", _code).c_str());
		return;
	}
	_orders_mon.push_order(ids.data(), ids.size(), now, isCanCancel);
	for (uint32_t localid : ids)
		_order_target_generations[localid] = { _target_generation, true };
	_last_fire_time = now;
	_fired_times += 1;
}

double WtStockVWapExeUnit::get_retry_quantity()
{
	if (!_ctx || !_config_valid || !std::isfinite(_min_hands) || !decimal::gt(_min_hands, 0))
		return 0;

	const double total_position = _ctx->getPosition(_code.c_str(), false);
	// Retry the most recent VWAP slice target. A user target update clears the
	// pending action, so _this_target remains the latest applicable target here.
	const double target_position = _this_target;
	if (!std::isfinite(total_position) || !std::isfinite(target_position))
	{
		_ctx->writeLog(fmt::format("{} cannot size retry from a non-finite target or position", _code).c_str());
		return 0;
	}
	const double target_diff = target_position - total_position;
	if (decimal::eq(target_diff, 0))
		return 0;

	if (decimal::gt(target_diff, 0))
		return std::floor(target_diff / _min_hands) * _min_hands;

	const double sellable_position = _is_t0 ? total_position : _ctx->getPosition(_code.c_str(), true, 1);
	if (!std::isfinite(sellable_position) || decimal::lt(sellable_position, 0))
	{
		_ctx->writeLog(fmt::format("{} cannot size retry from invalid sellable position", _code).c_str());
		return 0;
	}
	const double sell_quantity = min(abs(target_diff), sellable_position);
	if (!uses_stock_lot_rules() && decimal::lt(sellable_position, _min_hands) && decimal::ge(abs(target_diff), sellable_position))
		return -sellable_position;
	return -std::floor(sell_quantity / _min_hands) * _min_hands;
}

/**
 * @brief 立即发送订单
 * @details 根据指定的数量立即发送买入或卖出订单
 *          计算目标价格基于当前价格模式设置（最新价、最优价或对手价）
 *          并处理涅跌停板限制和订单监控
 * @param qty 待下单数量，正数表示买入，负数表示卖出
 */
void WtStockVWapExeUnit::fire_at_once(double qty)
{
	if (hasUnverifiedSubmission() || _orders_mon.has_unverified_timeout())
		return;
	// 如果数量为0，直接返回
	if (decimal::eq(qty, 0) || !_config_valid || _last_tick == nullptr || _comm_info == nullptr)
		return;
	size_t bucket_index = 0;
	std::string time_diagnostic;
	if (!map_action_time(_last_tick->actiontime(), bucket_index, time_diagnostic))
		return;
	(void)bucket_index;
	const uint64_t curTickTime = (uint64_t)_last_tick->actiondate() * 1000000000 + _last_tick->actiontime();
	if (curTickTime <= _last_tick_time)
	{
		_pending_action = PendingAction::retry;
		_retry_not_before = 0;
		_ctx->writeLog(fmt::format("{} retry waits for a fresh tick; current tick {} is not newer than {}",
			_code, curTickTime, _last_tick_time).c_str());
		return;
	}
	if (uses_stock_lot_rules())
	{
		const double absolute_quantity = abs(qty);
		const double whole_lot_quantity = std::floor(absolute_quantity / _min_hands) * _min_hands;
		if (decimal::lt(absolute_quantity, _min_hands) || !decimal::eq(absolute_quantity, whole_lot_quantity))
		{
			const double total_position = _ctx->getPosition(_code.c_str(), false);
			const double target_remainder = _this_target - total_position;
			const double sellable_position = _is_t0 ? total_position : _ctx->getPosition(_code.c_str(), true, 1);
			const double executable_remainder = decimal::lt(target_remainder, 0)
				? -min(abs(target_remainder), sellable_position) : target_remainder;
			mark_pending_remainder(target_remainder, executable_remainder, sellable_position,
				"retry quantity is not a legal whole stock lot");
			_last_tick_time = curTickTime;
			return;
		}
	}
	_execution_state = ExecutionState::active;
	_pending_remainder = 0;
	// 增加行情数据的引用计数
	_last_tick->retain();
	WTSTickData* curTick = _last_tick;
	
	// 获取合约代码、当前时间
	const char* code = _code.c_str();
	uint64_t now = _ctx->getCurTime();
	
	// 根据数量正负判断是买入还是卖出
	bool isBuy = decimal::gt(qty, 0);
	double targetPx = 0;

	// 根据价格模式设置确定委托基准价格: 
	// 0-最新价, 1-最优价, 2-对手价
	if (_price_mode == 0) {
		// 使用最新成交价格
		targetPx = curTick->price();
	}
	else if (_price_mode == 1) {
		// 使用本方最优价：买入时用买一价，卖出时用卖一价
		targetPx = isBuy ? curTick->bidprice(0) : curTick->askprice(0);
	}
	else // if(_price_mode == 2)
	{
		// 使用对手价：买入时用卖一价，卖出时用买一价
		targetPx = isBuy ? curTick->askprice(0) : curTick->bidprice(0);
	}

	// 根据撤单次数增加价格偏移，买入时价格上移，卖出时价格下移
	targetPx += _comm_info->getPriceTick()*_cancel_times *(isBuy ? 1 : -1);
	
	// 检查并处理涨跌停限制
	isCanCancel = true;  // 默认订单可撤销
	
	// 如果是买入订单，检查是否超过涨停价
	if (isBuy && !decimal::eq(_last_tick->upperlimit(), 0) && decimal::gt(targetPx, _last_tick->upperlimit()))
	{
		// 记录日志并将价格修正为涨停价
		_ctx->writeLog(fmt::format("Buy price {} of {} modified to upper limit price", targetPx, _code.c_str(), _last_tick->upperlimit()).c_str());
		targetPx = _last_tick->upperlimit();
		isCanCancel = false;  // 涨停价订单不可撤销
	}
	
	// 如果是卖出订单，检查是否低于跌停价
	if (isBuy != 1 && !decimal::eq(_last_tick->lowerlimit(), 0) && decimal::lt(targetPx, _last_tick->lowerlimit()))
	{
		// 记录日志并将价格修正为跌停价
		_ctx->writeLog(fmt::format("Sell price {} of {} modified to lower limit price", targetPx, _code.c_str(), _last_tick->lowerlimit()).c_str());
		targetPx = _last_tick->lowerlimit();
		isCanCancel = false;  // 跌停价订单不可撤销
	}
	
	// 创建订单ID容器
	OrderIDs ids;
	
	// 根据数量正负发送买入或卖出订单
	if (hasUnverifiedSubmission() || _orders_mon.has_unverified_timeout())
	{
		curTick->release();
		return;
	}
	if (qty > 0)
		// 发送买入订单
		ids = _ctx->buy(code, targetPx, abs(qty));
	else
		// 发送卖出订单
		ids = _ctx->sell(code, targetPx, abs(qty));
	if (ids.empty())
	{
		_unverified_submission.store(true);
		_pending_action = PendingAction::none;
		_last_tick_time = curTickTime;
		_retry_not_before = 0;
		curTick->release();
		_ctx->writeLog(fmt::format("{} retry order returned no IDs; submission outcome unverified, automatic actions held", _code).c_str());
		return;
	}

	// 将订单添加到监控器中
	_orders_mon.push_order(ids.data(), ids.size(), now, isCanCancel);
	for (uint32_t localid : ids)
		_order_target_generations[localid] = { _target_generation, true };
	_last_tick_time = curTickTime;
	_last_fire_time = now;
	_retry_not_before = 0;

	// 释放行情数据
	curTick->release();
}

/**
 * @brief 设置合约的目标仓位
 * @details 设置指定合约的目标仓位，并触发VWAP执行计算
 *          如果目标仓位与当前仓位相同或为负数，则不进行操作
 *          设置目标仓位时，会重置执行状态并记录当前时间和价格
 * @param stdCode 合约代码
 * @param newVol 新的目标仓位量
 */
void WtStockVWapExeUnit::set_position(const char * stdCode, double newVol)
{
	// 检查合约是否与当前管理的合约一致
	if (_code.compare(stdCode) != 0)
		return;
	if (!_config_valid)
		return;

	// 如果目标仓位与当前设置的一样，则不需要重复设置
	if (_target_pos_valid && decimal::eq(newVol, _target_pos))
		return;
	
	// 验证目标仓位是否合法（不能为负数）
	if (decimal::lt(newVol, 0))
	{
		_ctx->writeLog(fmt::format("{} is an error stock target position", newVol).c_str());
		return;
	}
	_target_pos_valid = true;
	_pending_action = PendingAction::none;
	_retry_not_before = 0;
	++_target_generation;
	
	// 设置新的目标仓位
	_target_pos = newVol;

	// 将目标模式设置为股票数量模式
	_target_mode = TargetMode::stocks;
	
	// 重置执行状态
	_execution_state = ExecutionState::active;
	_pending_remainder = 0;
	
	// 记录当前时间作为开始时间
	_start_time = TimeUtils::getLocalTimeNow();
	
	// 获取当前合约的最新行情并记录开始价格
	WTSTickData* tick = _ctx->grabLastTick(_code.c_str());
	if (tick) {
		_start_price = tick->price();
		tick->release();
	}
	
	// 重置已执行次数
	_fired_times = 0;

	// 触发计算逻辑
	do_calc();
}

/**
 * @brief 处理交易通道断开回调
 * @details 当交易通道断开时被调用，当前为空实现
 *          可以在此处添加必要的断开后处理逻辑，如标记订单状态、清除本地订单等
 */
void WtStockVWapExeUnit::on_channel_lost()
{
	_channel_ready = false;
}
/**
 * @brief 清除指定合约的所有仓位
 * @details 将指定合约的目标仓位和目标数量设置为0，并标记为清仓状态
 *          如果输入的合约代码与执行单元当前管理的合约不符，则不进行操作
 * @param stdCode 合约代码
 */
void WtStockVWapExeUnit::clear_all_position(const char* stdCode) {
	// 检查传入的合约代码是否与当前管理的合约一致
	if (_code.compare(stdCode) != 0)
		return;
	if (!_config_valid)
		return;
	_pending_action = PendingAction::none;
	_retry_not_before = 0;
	++_target_generation;
	_execution_state = ExecutionState::active;
	_pending_remainder = 0;
	
	// 标记为清仓状态
	_is_clear = true;
	
	// 将目标仓位和目标数量设为0
	_target_pos = 0;
	_target_pos_valid = true;
	_target_amount = 0;
	
	// 触发计算逻辑执行清仓
	do_calc();
}
