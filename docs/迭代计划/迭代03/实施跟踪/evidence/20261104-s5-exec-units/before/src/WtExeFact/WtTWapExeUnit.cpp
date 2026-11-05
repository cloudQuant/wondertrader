/*!
 * @file WtTWapExeUnit.cpp
 * @author wondertrader
 * @date 2023/05/18
 * @brief 基于时间加权平均价格(TWAP)的订单执行单元实现文件
 * 
 * 该文件实现了TWAP执行策略的核心逻辑，包括订单生成、发送和管理。
 * TWAP策略将大单按时间均匀拆分为多个小单，在指定时间内均匀执行，以降低市场冲击
 */

#include "WtTWapExeUnit.h"
#include "../Includes/IExecPluginV2.h"

#include "../Share/TimeUtils.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Share/decimal.h"
#include "../Share/fmtlib.h"
/***---begin---23.5.18---zhaoyk***/
#include "../Includes/WTSSessionInfo.hpp"
/***---end---23.5.18---zhaoyk***/
#include <math.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>


extern const char* FACT_NAME;

/**
 * @brief TWAP执行单元的构造函数
 * @details 初始化所有成员变量为默认值
 */
WtTWapExeUnit::WtTWapExeUnit()
	: _last_tick(NULL)       // 行情数据指针
	, _target_pos(0)        // 目标仓位
	, _target_pos_valid(false)
	, _channel_ready(false) // 交易通道就绪标志
	, _config_valid(false)  // 执行配置尚未通过校验
	, _comm_info(NULL)      // 品种信息
	, _sess_info(NULL)      // 交易时间模板
	, _cancel_times(0)      // 撤单次数
	, _cancel_cnt(0)        // 撤单计数
	, _total_secs(0)        // 总执行时间
	, _total_times(0)       // 总执行次数
	, _tail_secs(0)         // 收尾时间
	, _ord_sticky(0)        // 挂单超时时间
	, _price_mode(0)        // 价格模式
	, _price_offset(0)      // 价格偏移
	, _begin_time(0)        // 开始时间
	, _end_time(0)          // 结束时间
	, _min_open_lots(0.0)
	, _order_lots(0.0)
	, isCanCancel{ true }   // 订单是否可撤销
	, _fired_times(0)       // 已执行次数
	, _last_fire_time(0)    // 上次发单时间
	, _last_place_time(0)   // 上个下单时间
	, _last_tick_time(0)    // 上个tick时间
	, _in_calc(false)
{
}


/**
 * @brief TWAP执行单元的析构函数
 * @details 释放对象所持有的行情数据、品种信息和交易时间模板引用
 */
WtTWapExeUnit::~WtTWapExeUnit()
{
	// 释放行情数据资源
	if (_last_tick)
		_last_tick->release();

	// 释放品种信息资源
	if (_comm_info)
		_comm_info->release();

	// 释放交易时间模板资源
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
 * @brief 计算时间区间的秒数
 * @details 将开始时间和结束时间（以HHMM格式表示）转换为总秒数
 * @param begintime 开始时间，格式为HHMM，如1030表示10:30
 * @param endtime 结束时间，格式为HHMM，如1100表示11:00
 * @return 开始时间和结束时间之间的秒数
 */
inline uint32_t calTmSecs(uint32_t begintime, uint32_t endtime) 
{
	// 将HHMM格式转换为总秒数：小时*3600 + 分钟*60
	return   ((endtime / 100) * 3600 + (endtime % 100) * 60) - ((begintime / 100) * 3600 + (begintime % 100) * 60);

}

inline bool isValidHHMM(uint32_t time)
{
	return (time / 100) < 24 && (time % 100) < 60;
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

struct TwapPositionSnapshot
{
	double total;
	double longTotal;
	double shortTotal;
	double longAvailable;
	double shortAvailable;
	double netAvailable;
};

// ExecuteContext has no atomic position snapshot or error status. Verify the
// individual fields and reject values that change across two reads; this does
// not detect ABA changes or distinguish a missing position from a real zero.
static bool checkedTwapPosition(ExecuteContext* ctx, const char* code, TwapPositionSnapshot& out)
{
	auto read = [ctx, code]() {
		return TwapPositionSnapshot{ctx->getPosition(code, false, 3),
			ctx->getPosition(code, false, 1), ctx->getPosition(code, false, 2),
			ctx->getPosition(code, true, 1), ctx->getPosition(code, true, 2),
			ctx->getPosition(code, true, 3)};
	};
	const TwapPositionSnapshot first = read();
	const TwapPositionSnapshot second = read();
	if (!std::isfinite(first.total) || !std::isfinite(first.longTotal) || !std::isfinite(first.shortTotal) ||
		!std::isfinite(first.longAvailable) || !std::isfinite(first.shortAvailable) || !std::isfinite(first.netAvailable) ||
		first.total != second.total || first.longTotal != second.longTotal || first.shortTotal != second.shortTotal ||
		first.longAvailable != second.longAvailable || first.shortAvailable != second.shortAvailable || first.netAvailable != second.netAvailable ||
		first.longTotal < 0.0 || first.shortTotal > 0.0 || first.longAvailable < 0.0 || first.shortAvailable > 0.0 ||
		first.longAvailable > first.longTotal || first.shortAvailable < first.shortTotal ||
		first.total != first.longTotal + first.shortTotal || first.netAvailable != first.longAvailable + first.shortAvailable)
		return false;
	out = first;
	return true;
}

// Preserve the order direction and never exceed either the target difference
// or the canceled order's reported unfilled quantity. Commodity lot metadata
// is not read here: legacy commodity objects have no initialization/ready
// contract, and the provider's legal quantity rules require separate checks.
static double boundedTwapQuantity(double delta, bool closing, double available, double desired, double maximum,
	double minOpenLots)
{
	if (!std::isfinite(delta) || !std::isfinite(available) || !std::isfinite(desired) || !std::isfinite(maximum) ||
		!std::isfinite(minOpenLots) || delta == 0.0 || desired <= 0.0 || maximum <= 0.0 || minOpenLots < 0.0)
		return 0.0;
	double limit = std::min(std::fabs(delta), maximum);
	if (closing)
		limit = std::min(limit, available);
	const double quantity = std::min(desired, limit);
	if (quantity <= 0.0 || (!closing && quantity < minOpenLots))
		return 0.0;
	return std::copysign(quantity, delta);
}
/**
 * @brief 获取所属执行器工厂名称
 * @details 返回当前执行单元所属的工厂名称，用于执行器的工厂注册和搜寻
 * @return 执行器工厂名称
 */
const char* WtTWapExeUnit::getFactName()
{
	return FACT_NAME;
}

/**
 * @brief 获取执行单元名称
 * @details 返回当前执行单元的名称，用于标识和日志记录
 * @return 执行单元名称
 */
const char* WtTWapExeUnit::getName()
{
	return "WtTWapExeUnit";
}

/**
 * @brief 初始化执行单元
 * @details 从配置中读取参数并设置执行单元的各项配置，加载相关资源
 * @param ctx 执行上下文对象，用于访问交易环境
 * @param stdCode 标准化合约代码
 * @param cfg 配置项对象
 */
void WtTWapExeUnit::init(ExecuteContext* ctx, const char* stdCode, WTSVariant* cfg)
{
	// Reserved host key (IExecPluginV2.h): automatic timeout cancels only with host-verified identity.
	_host_order_identity = cfg != NULL && cfg->getUInt32(WT_EXEC_PLUGIN_V2_HOST_ORDER_IDENTITY_KEY) == 1;
	_config_valid = false;
	_target_pos = 0.0;
	_target_pos_valid = false;
	_this_target = 0.0;
	_this_target_valid = false;

	if (ctx == NULL)
		return;

	if (stdCode == NULL || stdCode[0] == '\0')
	{
		ctx->writeLog("TWAP init rejected: standard code is empty");
		return;
	}

	// 调用父类初始化前先验证 code，避免空指针进入基类字符串字段。
	ExecuteUnit::init(ctx, stdCode, cfg);

	if (cfg == NULL)
	{
		ctx->writeLog(fmt::format("TWAP init rejected for {}: configuration is null", stdCode).c_str());
		return;
	}

	// 获取并保存品种信息引用
	_comm_info = ctx->getCommodityInfo(stdCode);
	// 如果品种信息有效，增加引用计数
	if (_comm_info)
		_comm_info->retain();
	
	// 获取并保存交易时间模板信息
	_sess_info = ctx->getSessionInfo(stdCode);
	if (_sess_info)
		_sess_info->retain();

	if (_comm_info == NULL || _sess_info == NULL || _sess_info->getSectionCount() == 0)
	{
		ctx->writeLog(fmt::format("TWAP init rejected for {}: commodity or trading session is unavailable", stdCode).c_str());
		return;
	}
	
	// 从配置中读取各项参数
	_ord_sticky = cfg->getUInt32("ord_sticky");      // 挂单超时时间（秒）
	_begin_time= cfg->getUInt32("begin_time");      // 开始时间（HHMM格式）
	_end_time = cfg->getUInt32("end_time");         // 结束时间（HHMM格式）
	_total_secs = cfg->getUInt32("total_secs");     // 总执行时间（秒）
	_tail_secs = cfg->getUInt32("tail_secs");       // 收尾时间（秒）
	// WTSVariant::asUInt32 converts via floating point; out-of-range and
	// negative inputs must be rejected before that conversion can occur.
	WTSVariant* totalTimesValue = cfg->get("total_times");
	const double totalTimes = totalTimesValue ? totalTimesValue->asDouble() : 0.0;
	if (!std::isfinite(totalTimes) || totalTimes < 1.0 ||
		totalTimes > static_cast<double>((std::numeric_limits<uint32_t>::max)()))
	{
		ctx->writeLog(fmt::format("TWAP init rejected for {}: total_times must be greater than zero and fit in uint32", stdCode).c_str());
		return;
	}
	_total_times = static_cast<uint32_t>(totalTimes); // 总执行次数
	_price_mode = cfg->getUInt32("price_mode");     // 价格模式（0-最新价,1-最优价,2-对手价）
	_price_offset = cfg->getUInt32("price_offset"); // 价格偏移
	_order_lots = cfg->getDouble("lots");           // 单次发单手数
	
	// 如果配置中有最小开仓数量参数，则读取
	if (cfg->has("minopenlots"))
		_min_open_lots = cfg->getDouble("minopenlots");
	
	if (_total_times == 0)
	{
		ctx->writeLog(fmt::format("TWAP init rejected for {}: total_times must be greater than zero", stdCode).c_str());
		return;
	}

	if (!cfg->has("begin_time") || !cfg->has("end_time")
		|| !isValidHHMM(_begin_time) || !isValidHHMM(_end_time))
	{
		ctx->writeLog(fmt::format("TWAP init rejected for {}: begin_time/end_time must be valid HHMM values", stdCode).c_str());
		return;
	}

	if (_end_time < _begin_time)
	{
		ctx->writeLog(fmt::format("TWAP init blocked for {}: overnight schedules are unsupported pending explicit session semantics", stdCode).c_str());
		return;
	}

	// 计算执行总时间（秒）；同一开始/结束时间代表空执行窗口，不能继续下单
	_total_secs = calTmSecs(_begin_time, _end_time);
	if (_total_secs == 0)
	{
		ctx->writeLog(fmt::format("TWAP init rejected for {}: execution duration must be greater than zero", stdCode).c_str());
		return;
	}

	if (_tail_secs > _total_secs)
	{
		ctx->writeLog(fmt::format("TWAP init rejected for {}: tail_secs exceeds execution duration", stdCode).c_str());
		return;
	}

	// 计算单次发单时间间隔（秒）
	// 去掉尾部时间后均匀分配，这样最后剩余的数量就有一个兵底发单的机制
	_fire_span = (_total_secs - _tail_secs) / _total_times;
	_config_valid = true;

	// 记录执行单元初始化完成日志
	ctx->writeLog(fmt::format("执行单元WtTWapExeUnit[{}] 初始化完成,订单超时 {} 秒,执行时限 {} 秒,收尾时间 {} 秒,间隔时间 {} 秒", stdCode, _ord_sticky, _total_secs, _tail_secs, _fire_span).c_str());
}

/**
 * @brief 处理订单回报
 * @details 处理订单状态变化，包括成交、撤销等情况，并更新内部订单管理状态
 *          如果订单被撤销且目标仓位未达到，则会重新发送订单
 * @param localid 本地订单ID
 * @param stdCode 标准化合约代码
 * @param isBuy 是否为买入订单
 * @param leftover 剩余未成交数量
 * @param price 委托价格
 * @param isCanceled 是否已撤销
 */
void WtTWapExeUnit::on_order(uint32_t localid, const char* stdCode, bool isBuy, double leftover, double /*price*/, bool isCanceled)
{
	// A provider may synchronously report before its IDs are returned and
	// registered. The legacy callback does not carry a submission token.
	if (_submission_in_progress.load(std::memory_order_acquire))
	{
		_submission_unverified.store(true, std::memory_order_release);
		return;
	}
	if (stdCode == NULL || _code != stdCode)
		return;
	// 如果订单不在监控中，则忽略此回报
	if (!_orders_mon.has_order(localid))
		return;

	// 如果订单已撤销或已完全成交，从订单监控器中移除
	if (isCanceled || leftover == 0)  
	{
		// 从订单监控器中删除该订单
		_orders_mon.erase_order(localid);

		// 减少撤单计数
		if (_cancel_cnt > 0)
			_cancel_cnt--;
		
		// 记录日志
		_ctx->writeLog(fmt::format("Order {} updated cancelcnt -> {}", localid, _cancel_cnt).c_str());
	}
	/***---begin---23.5.19---zhaoyk***/
	// 如果订单完全成交且不是因为撤单，重置撤单次数
	if (leftover == 0 && !isCanceled)
	{
		// 订单成交后重置撤单次数
		_cancel_times = 0;
		// 记录订单成交日志
		_ctx->writeLog(fmtutil::format("Order {} has filled", localid));
	}
	/***---end---23.5.19---zhaoyk***/
	// 如果所有订单已撤销（撤单计数为0），处理超时撤单情况
	if (isCanceled && _cancel_cnt == 0)
	{
		if (!_this_target_valid)
		{
			_ctx->writeLog(fmt::format("Order {} of {} canceled before a TWAP slice target was established; retry skipped",
				localid, stdCode).c_str());
			return;
		}

		TwapPositionSnapshot position;
		if (!checkedTwapPosition(_ctx, stdCode, position))
		{
			_ctx->writeLog(fmt::format("Order {} of {} canceled; position snapshot is inconsistent, retry held", localid, stdCode).c_str());
			return;
		}
		
		// The cancellation's leftover is an upper bound, not proof that all
		// fills have reached the position view. Never reverse the canceled order
		// or enlarge its unfilled quantity to satisfy an opening minimum.
		const double delta = _this_target - position.total;
		const bool closing = delta > 0.0 ? position.shortTotal < 0.0 : position.longTotal > 0.0;
		const double available = delta > 0.0 ? -position.shortAvailable : position.longAvailable;
		const double retryQty = _comm_info && _last_tick && std::isfinite(leftover) &&
			((isBuy && delta > 0.0) || (!isBuy && delta < 0.0))
			? boundedTwapQuantity(delta, closing, available, leftover, leftover, _min_open_lots) : 0.0;
		if (retryQty != 0.0)
		{
			/***---begin---23.5.22---zhaoyk***/
			// 记录订单撤销后重发日志
			_ctx->writeLog(fmtutil::format("Order {} of {} canceled, re_fire will be done", localid, stdCode));
			
			// 增加撤单次数，用于下次发单时的价格偏移
			_cancel_times++;
			
			fire_at_once(retryQty, closing);
			/***---end---23.5.22---zhaoyk***/
		}
		else
			_ctx->writeLog(fmt::format("Order {} of {} canceled; no bounded remaining slice to retry", localid, stdCode).c_str());
	}
	/***---begin---23.5.22---zhaoyk***/
	// 处理异常情况：订单已撤销但撤单计数不为0，这可能是一个错误状态
	if (isCanceled && _cancel_cnt != 0)
	{
		// 记录错误日志并返回，等待后续检查
		_ctx->writeLog(fmtutil::format("Order {} of {}  hasn't canceled, error will be return ", localid, stdCode));
		return;
	}
	/***---end---23.5.22---zhaoyk***/
} 

/**
 * @brief 处理交易通道就绪回调
 * @details 当交易通道准备就绪时调用，主要处理未完成订单的状态同步
 *          包含三种情况的处理：
 *          1. 有未完成订单但本地无订单记录：清除所有未被管理的订单
 *          2. 无未完成订单但本地有订单记录：清除本地错误订单
 *          3. 其他异常情况处理
 */
void WtTWapExeUnit::on_channel_ready()
{
	if (!_config_valid)
		return;

	// 标记交易通道为就绪状态
	_channel_ready = true;
	if (_submission_unverified.load(std::memory_order_acquire))
	{
		_ctx->writeLog(fmt::format("TWAP order outcome for {} is unverified; automatic actions remain held", _code).c_str());
		return;
	}
	
	// 获取未完成的订单数量
	double undone = _ctx->getUndoneQty(_code.c_str());
	/***---begin---23.5.18---zhaoyk***/
	// 情况1: 如果有未完成订单但本地没有订单记录，需要清除未被管理的订单
	if (!decimal::eq(undone, 0) && !_orders_mon.has_order())
	//if (undone != 0 && !_orders_mon.has_order())
	/***---end---23.5.18---zhaoyk***/
	{
		if (!wtRunIfNoUnverifiedTimeout(_orders_mon, [this, undone]() {
		// 这说明有未完成订单不在监控中，需要先撤销
		_ctx->writeLog(fmt::format("{} unmanaged orders of {}, cancel all", undone, _code).c_str());

		// 根据未完成订单的正负判断是买入还是卖出订单
		bool isBuy = (undone > 0);
		
		// 撤销对应方向的所有未完成订单
		OrderIDs ids = _ctx->cancel(_code.c_str(), isBuy);
		
		// 将撤销的订单添加到监控器中进行跟踪
		_orders_mon.push_order(ids.data(), ids.size(), _ctx->getCurTime());
		
		// 更新撤单计数
		_cancel_cnt += ids.size();

		// 记录撤单日志
		_ctx->writeLog(fmt::format("Unmanaged order updated cancelcnt to {}", _cancel_cnt).c_str());
		}))
			_ctx->writeLog(fmt::format("Unmanaged-order cancellation for {} suppressed after an unverified timeout", _code).c_str());
	}
	/***---begin---23.5.18---zhaoyk***/
	// 情况2: 如果没有未完成订单但本地有订单记录，需要清除本地错误订单
	else if (decimal::eq(undone, 0) && _orders_mon.has_order()) 
	{
		/*
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
	else // 参考Minimpactunit实现
	{
		// 记录日志，显示当前未完成订单和本地监控状态
		_ctx->writeLog(fmtutil::format("Unrecognized condition while channle ready,{:.2f} live orders of {} exists,local orders {}exist", undone, _code.c_str(), _orders_mon.has_order() ? "" : "not"));
	}
	/***---end---23.5.18---zhaoyk***/
	
	// 通道就绪后触发一次计算，重新调整仓位
	do_calc();
}

/**
 * @brief 处理交易通道断开回调
 * @details 当交易通道断开时被调用，当前为空实现
 *          可以在此处添加必要的断开后处理逻辑，如标记订单状态、清除本地订单等
 */
void WtTWapExeUnit::on_channel_lost()
{
	// 在交易通道断开时执行的处理逻辑
	// 当前为空实现
}


/**
 * @brief 处理行情数据回调
 * @details 当收到新的行情数据时调用，更新内部行情缓存并触发相关的交易逻辑
 *          包括首次行情处理、交易时间校验、计算目标仓位等
 * @param newTick 新的行情数据指针
 */
void WtTWapExeUnit::on_tick(WTSTickData* newTick)
{
	if (!_config_valid)
		return;

	// 如果行情数据为空或者合约代码不匹配，则直接返回
	if (newTick == NULL || _code.compare(newTick->code()) != 0)
		return;

	// 标记是否为首次收到行情
	bool isFirstTick = false;
	
	// 如果已有缓存的行情数据，需要先释放内存
	if (_last_tick)
	{
		_last_tick->release();
	}
	/***---begin---23.5.18---zhaoyk***/
	else
	{
		// 标记为首次收到行情
		isFirstTick = true;
		
		// 首次行情需要检查是否在交易时间内
		// 如果行情时间不在交易时间内（如集合竞价时段），则过滤掉该行情
		if (_sess_info != NULL && !_sess_info->isInTradingTime(newTick->actiontime() / 100000))
			return;
	}
	/***---end---23.5.18---zhaoyk***/

	// 保存新的行情数据并增加引用计数
	_last_tick = newTick;
	_last_tick->retain();

	/*
	 * 这里可以考虑一下
	 * 如果写的上一次丢出去的单子不够达到目标仓位
	 * 那么在新的行情数据进来的时候可以再次触发核心逻辑
	 */

	// 如果是首次收到行情，需要检查目标仓位与实际仓位是否一致
	if (isFirstTick)
	{
		// 获取目标仓位
		double newVol = _target_pos;
		const char* stdCode = _code.c_str();
		
		// 获取未完成订单数量和实际仓位
		double undone = _ctx->getUndoneQty(stdCode);
		double realPos = _ctx->getPosition(stdCode, false);
		
		// 如果目标仓位与当前仓位加未完成订单不一致，需要调整仓位
		if (!decimal::eq(newVol, undone + realPos))
		{
			// 触发仓位计算和调整
			do_calc();
		}
	}
	// 非首次行情处理，主要检查订单超时和定时发单
	else
	{
		// 获取当前时间
		uint64_t now = _ctx->getCurTime();
		
		// 扫描裸 localid 超时；mark 会在锁内校验记录仍然存活且 generation 未变化。
		// 如果设置了订单超时时间且有未完成订单，检查是否有订单需要撤销
		if (_ord_sticky != 0 && _orders_mon.has_order()) 
		{			
			// 没有端到端订单身份令牌时，不通过旧 localid 接口发起延迟自动撤单。
			_orders_mon.check_orders_with_generation(_ord_sticky, now, [this, now](uint32_t localid, uint64_t generation) {
				if (_host_order_identity)
				{
					// 迭代03 卡5 / N05-003：宿主按 owner 与不可复用身份核验后才撤单；核验失败即隔离，不回退裸 ID。
					const int verified = _orders_mon.cancel_timeout_verified(localid, generation, _ord_sticky, now,
						[this](uint32_t id) { return _ctx->cancel(id); });
					if (verified > 0)
					{
					_cancel_cnt++;
						_ctx->writeLog((std::string("Expired order ") + std::to_string(localid) + " of " + _code + " canceled with host-verified identity").c_str());
					}
					else if (verified < 0)
						_ctx->writeLog((std::string("Expired order ") + std::to_string(localid) + " of " + _code + " cannot be auto-canceled: host could not verify its identity; order remains monitored").c_str());
					return;
				}
				if (!_orders_mon.mark_unverified_timeout(localid, generation, _ord_sticky, now))
					return; // 终态或 localid 已复用；不得复活旧快照
				_ctx->writeLog(fmt::format("Expired order {} of {} cannot be auto-canceled: legacy localid is unverified; order remains monitored",
					localid, _code.c_str()).c_str());
			});

		}

		// 超时记录仍在监控；本轮等待明确的订单状态回报，不重新计算发单。
		if (!_orders_mon.has_unverified_timeout() && (now - _last_fire_time >= _fire_span * 1000)) 
		{
			// 执行仓位计算和发单操作
			do_calc();
		}
	}
}

/**
 * @brief 处理成交回报
 * @details 当收到成交回报时调用，当前实现中不在此处触发交易逻辑，而是在on_tick中处理
 * @param localid 本地订单ID
 * @param stdCode 标准化合约代码
 * @param isBuy 是否为买入成交
 * @param vol 成交数量
 * @param price 成交价格
 */
void WtTWapExeUnit::on_trade(uint32_t /*localid*/, const char* /*stdCode*/, bool /*isBuy*/, double /*vol*/, double /*price*/)
{
	if (_submission_in_progress.load(std::memory_order_acquire))
	{
		_submission_unverified.store(true, std::memory_order_release);
		return;
	}
}

/**
 * @brief 处理委托回报
 * @details 当收到委托回报时调用，主要处理委托失败的情况
 * @param localid 本地订单ID
 * @param stdCode 标准化合约代码
 * @param bSuccess 委托是否成功
 * @param message 委托回报消息
 */
void WtTWapExeUnit::on_entrust(uint32_t localid, const char* /*stdCode*/, bool bSuccess, const char* /*message*/)
{
	if (_submission_in_progress.load(std::memory_order_acquire))
	{
		_submission_unverified.store(true, std::memory_order_release);
		return;
	}
	// 如果委托失败，需要处理
	if (!bSuccess)
	{
		// 如果不是由本执行单元发出的订单，则忽略
		if (!_orders_mon.has_order(localid))
			return;

		// 从订单监控器中移除失败的订单
		_orders_mon.erase_order(localid);

		// 重新计算并发送新的订单
		do_calc();
	}
}

/**
 * @brief 立即发单
 * @details 根据给定的数量立即发送交易订单，包括计算委托价格、确定交易方向等
 *          当发生撤单后重发或者需要立即执行交易时使用
 * @param qty 目标交易数量，正数表示买入，负数表示卖出
 */
bool WtTWapExeUnit::rememberOrders(const OrderIDs& ids, uint64_t now)
{
	std::vector<uint32_t> validIds;
	bool verified = !ids.empty();
	for (uint32_t id : ids)
	{
		if (id == 0 || id == (std::numeric_limits<uint32_t>::max)() ||
			std::find(validIds.begin(), validIds.end(), id) != validIds.end() || _orders_mon.has_order(id))
		{
			verified = false;
			continue;
		}
		validIds.push_back(id);
	}
	if (!verified)
		_submission_unverified.store(true, std::memory_order_release);
	if (!validIds.empty())
		_orders_mon.push_order(validIds.data(), static_cast<uint32_t>(validIds.size()), now, isCanCancel);
	if (!verified)
		_ctx->writeLog(fmt::format("TWAP order outcome for {} is unverified; automatic actions held", _code).c_str());
	return verified;
}

bool WtTWapExeUnit::submitOrder(bool isBuy, double price, double qty, bool forceClose, uint64_t now,
	bool newSlice, double sliceTarget)
{
	if (_submission_unverified.load(std::memory_order_acquire) || _orders_mon.has_unverified_timeout())
		return false;
	bool expected = false;
	if (!_submission_in_progress.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
		return false;
	if (_submission_unverified.load(std::memory_order_acquire) || _orders_mon.has_unverified_timeout())
	{
		_submission_in_progress.store(false, std::memory_order_release);
		return false;
	}
	try
	{
		const OrderIDs ids = isBuy ? _ctx->buy(_code.c_str(), price, qty, forceClose) :
			_ctx->sell(_code.c_str(), price, qty, forceClose);
		const bool verified = rememberOrders(ids, now);
		if (verified && !_submission_unverified.load(std::memory_order_acquire) && newSlice)
		{
			_this_target = sliceTarget;
			_this_target_valid = true;
			_last_fire_time = now;
			_fired_times += 1;
		}
		_submission_in_progress.store(false, std::memory_order_release);
		if (_submission_unverified.load(std::memory_order_acquire))
		{
			_ctx->writeLog(fmt::format("TWAP order outcome for {} is unverified; automatic actions held", _code).c_str());
			return false;
		}
		return verified;
	}
	catch (...)
	{
		_submission_unverified.store(true, std::memory_order_release);
		_submission_in_progress.store(false, std::memory_order_release);
		_ctx->writeLog(fmt::format("TWAP submission for {} threw; order outcome is unverified", _code).c_str());
		return false;
	}
}

void WtTWapExeUnit::fire_at_once(double qty, bool forceClose)
{
	if (!_this_target_valid)
		return;

	if (_submission_unverified.load(std::memory_order_acquire) || _orders_mon.has_unverified_timeout())
		return;

	if (!_config_valid)
		return;

	// 如果数量为0，则直接返回
	if (decimal::eq(qty, 0))
		return;

	// 增加行情数据引用计数，防止在使用过程中被释放
	_last_tick->retain();
	WTSTickData* curTick = _last_tick;
	
	// 获取当前时间
	uint64_t now = _ctx->getCurTime();
	
	// 根据数量正负确定交易方向，正数为买入，负数为卖出
	bool isBuy = decimal::gt(qty, 0);
	
	// 初始化目标价格
	double targetPx = 0;
	
	// 根据价格模式设置确定委托基准价格
	// 0-最新价, 1-最优价(买入用买一价，卖出用卖一价), 2-对手价(买入用卖一价，卖出用买一价)
	if (_price_mode == 0)
	{
		// 模式0: 使用最新成交价
		targetPx = curTick->price();
	}
	else if (_price_mode == 1)
	{
		// 模式1: 使用最优价（买入用买一价，卖出用卖一价）
		targetPx = isBuy ? curTick->bidprice(0) : curTick->askprice(0);
	}
	else if (_price_mode == 2)
	{
		// 模式2: 使用对手价（买入用卖一价，卖出用买一价）
		targetPx = isBuy ? curTick->askprice(0) : curTick->bidprice(0);
	}

	/***---begin---23.5.22---zhaoyk***/
	// 根据撤单次数调整目标价格
	targetPx += _comm_info->getPriceTick() * _cancel_times* (isBuy ? 1 : -1);  
	/***---end---23.5.22---zhaoyk***/

	// 检查涨跌停价
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

	if (_submission_unverified.load(std::memory_order_acquire) || _orders_mon.has_unverified_timeout())
	{
		curTick->release();
		return;
	}
	(void)submitOrder(qty > 0, targetPx, abs(qty), forceClose, now);

	curTick->release();
}

/**
 * @brief 执行仓位计算和发单操作
 * @details 根据目标仓位和当前实际仓位计算需要发送的订单数量和价格
 *          包含订单撤销、价格计算、数量分配等逻辑
 *          该方法是执行单元的核心逻辑，在多个地方被触发
 */
void WtTWapExeUnit::do_calc()
{
	if (_submission_unverified.load(std::memory_order_acquire) || _orders_mon.has_unverified_timeout())
		return;

	if (!_config_valid)
		return;
	if (!_target_pos_valid)
		return;

	// 使用标记类防止重入，确保同一时间只有一个线程在执行计算
	CalcFlag flag(&_in_calc);
	if (flag)
		return;

	// 如果交易通道未就绪，则直接返回
	if (!_channel_ready)
		return;

	// 这里加一个锁，主要原因是实盘过程中发现
	// 在修改目标仓位的时候，会触发一次do_calc
	// 而ontick也会触发一次do_calc，两次调用是从两个线程分别触发的，所以会出现同时触发的情况
	// 如果不加锁，就会引起问题
	// 这种情况在原来的SimpleExecUnit没有出现，因为SimpleExecUnit只在set_position的时候触发
	StdUniqueLock lock(_mtx_calc);

	// 获取合约代码
	const char* code = _code.c_str();
	
	// 获取未完成订单数量
	double undone = _ctx->getUndoneQty(code);
	
	// 获取真实目标仓位（处理清仓标记DBL_MAX的情况）
	double newVol = get_real_target(_target_pos);
	
	TwapPositionSnapshot position;
	if (!checkedTwapPosition(_ctx, code, position))
	{
		_ctx->writeLog(fmt::format("TWAP position snapshot for {} is inconsistent; order held", _code).c_str());
		return;
	}
	// The target denotes total net exposure, not merely currently available lots.
	double realPos = position.total;
	
	// 计算目标仓位与实际仓位的差值，即需要交易的数量
	double diffQty = newVol - realPos;

	// 检查是否有正在撤销的订单，如果有则不能进行下一轮计算
	if (_cancel_cnt != 0)
	{
		// 记录日志并退出本轮执行
		_ctx->writeLog(fmt::format("{}尚有未完成撤单指令,暂时退出本轮执行", _code).c_str());
		return;
	}

	// 如果目标仓位与实际仓位相等，则无需发单，直接返回
	if (decimal::eq(diffQty, 0))
		return;
	
	// 检查是否有与目标仓位变动方向相反的未完成订单
	// 每一次发单要保障成交，如果有与目标相反的未完成单，需要先撤销
	if (decimal::lt(diffQty * undone, 0))
	{
		// 确定未完成订单的方向（正数为买入，负数为卖出）
		bool isBuy = decimal::gt(undone, 0);   
		
		// 撤销相应方向的所有订单
		OrderIDs ids = _ctx->cancel(code, isBuy);
		
		// 如果成功撤销了订单，需要更新订单监控状态
		if (!ids.empty())
		{
			// 将撤销的订单添加到监控器中
			_orders_mon.push_order(ids.data(), ids.size(), _ctx->getCurTime());
			
			// 增加撤单计数
			_cancel_cnt += ids.size();
			
			// 记录撤单日志
			_ctx->writeLog(fmtutil::format("[{}@{}] live opposite order of {} canceled, cancelcnt -> {}", __FILE__, __LINE__, _code.c_str(), _cancel_cnt));
		}
		
		// 撤单后退出本轮执行，等待撤单回报
		return;
	}
	// 因为采用逐笔发单策略，如果有同向的未完成订单，则暂不发新单
	if (!decimal::eq(undone, 0))
	{
		// 记录日志并退出本轮执行
		_ctx->writeLog(fmt::format("{}上一轮有挂单未完成,暂时退出本轮执行", _code).c_str());
		return;
	}
	
	// 保存当前仓位
	double curPos = realPos;
	
	// 检查是否有最新的行情数据
	if (_last_tick == NULL)
	{
		// 没有行情数据则无法计算价格，退出执行
		_ctx->writeLog(fmt::format("{}没有最新tick数据,退出执行逻辑", _code).c_str());
		return;
	}
	// 检查当前仓位是否已经达到目标仓位
	if (decimal::eq(curPos, newVol))
	{
		// 如果当前仓位和目标仓位匹配，且不是清仓指令，则直接退出
		if (!is_clear(_target_pos))
			return;

		// 如果是清仓指令，需要进一步检查多头仓位
		// 获取多头仓位
		double lPos = _ctx->getPosition(code, true, 1); 
		
		// 如果多头仓位已经为0，说明已经完全清仓，直接退出
		if (decimal::eq(lPos, 0))
			return;

		// 如果还有多头仓位，则将目标仓位设置为卖出指令，强制触发清仓                      
		newVol = -min(lPos, _order_lots);
		
		// 记录清仓过程触发日志
		_ctx->writeLog(fmtutil::format("Clearing process triggered, target position of {} has been set to {}", _code.c_str(), newVol));
	}

	// 检查行情是否有更新，如果相比上次没有更新的tick，则先不下单
	// 这是为了防止开盘前集中下单导致交易通道被封
	uint64_t curTickTime = (uint64_t)_last_tick->actiondate() * 1000000000 + _last_tick->actiontime();
	
	// 如果当前行情时间小于等于上次行情时间，说明行情未更新
	if (curTickTime <= _last_tick_time)
	{
		// 记录日志并等待下次执行
		_ctx->writeLog(fmtutil::format("No tick of {} updated, {} <= {}, execute later", _code, curTickTime, _last_tick_time));
		return;
	}
	
	// 更新最新行情时间
	_last_tick_time = curTickTime;
	
	// 设置单次发单手数
	[[maybe_unused]] double this_qty = _order_lots;
	/***---end---23.5.26---zhaoyk***/

	// 计算剩余的发单次数
	uint32_t leftTimes = _fired_times >= _total_times ? 0 : _total_times - _fired_times;
	
	/***---begin---23.5.22---zhaoyk***/
	// 记录当前发单次数日志
	//_ctx->writeLog(fmt::format("第 {} 次发单", _fired_times).c_str());
	_ctx->writeLog(fmt::format("第 {} 次发单", _fired_times+1).c_str());
	/***---end---23.5.22---zhaoyk***/
	
	// 标记是否需要全部发单（即“打板”）
	bool bNeedShowHand = false;
	
	// 计算本次发单数量
	// 如果剩余发单次数为0且还有未完成的仓位差异，则需要全部发单
	// 否则将剩余数量平均分配到剩余发单次数中
	/***---begin---23.5.22---zhaoyk***/
	// 计算本次发单数量
	double curQty = 0;
	
	// Preserve the existing TWAP slice sizing while capping the result to the
	// target difference. Legal exchange increments require separate validation.
	bNeedShowHand = leftTimes == 0;
	const double desired = leftTimes == 0 ? std::fabs(diffQty) :
		std::max(_min_open_lots, std::round(std::fabs(diffQty) / leftTimes));
	const bool closing = diffQty > 0.0 ? position.shortTotal < 0.0 : position.longTotal > 0.0;
	const double available = diffQty > 0.0 ? -position.shortAvailable : position.longAvailable;
	curQty = boundedTwapQuantity(diffQty, closing, available, desired, std::fabs(diffQty), _min_open_lots);
	if (curQty == 0.0)
	{
		_ctx->writeLog(fmt::format("No bounded TWAP slice for {}: target difference {} remains outstanding", _code, diffQty).c_str());
		return;
	}
	/***---end---23.5.22---zhaoyk***/
	/***---end---23.5.22---zhaoyk***/
	
	// Only publish this slice target once the host returns at least one order ID.
	const double nextSliceTarget = realPos + curQty;

	// Borrowed from _last_tick; the member owns this reference until the next tick or destruction.
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
	else // if(_price_mode == 2)
	{
		targetPx = isBuy ? curTick->askprice(0) : curTick->bidprice(0);
	}

	//如果需要全部发单,则价格偏移5跳,以保障执行
	if (bNeedShowHand) //  last showhand time
	{
		targetPx += _comm_info->getPriceTick() * 5 * (isBuy ? 1 : -1);
	}
	else if (_price_offset != 0)	//如果设置了价格偏移,也要处理一下
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
	//最后发出指令
	if (_submission_unverified.load(std::memory_order_acquire) || _orders_mon.has_unverified_timeout())
		return;
	(void)submitOrder(curQty > 0, targetPx, abs(curQty), closing, now, true, nextSliceTarget);

}

/**
 * @brief 设置目标仓位
 * @details 设置执行单元的目标仓位，并触发仓位计算和发单操作
 *          当目标仓位与当前目标仓位不同时，重置发单次数并触发do_calc方法
 * @param stdCode 标准化合约代码
 * @param newVol 新的目标仓位
 */
void WtTWapExeUnit::set_position(const char* stdCode, double newVol)
{
	// 检查合约代码是否匹配，如果不匹配则直接返回
	if (_code.compare(stdCode) != 0)
		return;

	// 检查新的目标仓位是否与当前目标仓位相同，相同则直接返回
	//double diff = newVol - _target_pos;
	if (_target_pos_valid && decimal::eq(newVol, _target_pos))
		return;

	// 更新目标仓位
	_this_target_valid = false;
	_target_pos = newVol;
	_target_pos_valid = true;

	// 重置发单次数计数，开始新一轮的仓位调整
	_fired_times = 0;

	// 触发仓位计算和发单操作
	do_calc();
}

