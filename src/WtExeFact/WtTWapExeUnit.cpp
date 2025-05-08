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

#include "../Share/TimeUtils.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Share/decimal.h"
#include "../Share/fmtlib.h"
/***---begin---23.5.18---zhaoyk***/
#include "../Includes/WTSSessionInfo.hpp"
/***---end---23.5.18---zhaoyk***/
#include <math.h>


extern const char* FACT_NAME;

/**
 * @brief TWAP执行单元的构造函数
 * @details 初始化所有成员变量为默认值
 */
WtTWapExeUnit::WtTWapExeUnit()
	: _last_tick(NULL)       // 行情数据指针
	, _comm_info(NULL)      // 品种信息
	, _ord_sticky(0)        // 挂单超时时间
	, _cancel_cnt(0)        // 撤单计数
	, _channel_ready(false) // 交易通道就绪标志
	, _last_fire_time(0)    // 上次发单时间
	, _fired_times(0)       // 已执行次数
	, _total_times(0)       // 总执行次数
	, _total_secs(0)        // 总执行时间
	, _price_mode(0)        // 价格模式
	, _price_offset(0)      // 价格偏移
	, _target_pos(0)        // 目标仓位
	, _cancel_times(0)      // 撤单次数
	, _begin_time(0)        // 开始时间
	, _end_time(0)          // 结束时间
	, _last_place_time(0)   // 上个下单时间
	, _last_tick_time(0)    // 上个tick时间
	, isCanCancel{ true }   // 订单是否可撤销
{
}


/**
 * @brief TWAP执行单元的析构函数
 * @details 释放对象所持有的资源，包括行情数据和品种信息
 */
WtTWapExeUnit::~WtTWapExeUnit()
{
	// 释放行情数据资源
	if (_last_tick)
		_last_tick->release();

	// 释放品种信息资源
	if (_comm_info)
		_comm_info->release();
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
	// 调用父类初始化
	ExecuteUnit::init(ctx, stdCode, cfg);

	// 获取并保存品种信息引用
	_comm_info = ctx->getCommodityInfo(stdCode);
	// 如果品种信息有效，增加引用计数
	if (_comm_info)
		_comm_info->retain();
	
	// 获取并保存交易时间模板信息
	_sess_info = ctx->getSessionInfo(stdCode);
	if (_sess_info)
		_sess_info->retain();
	
	// 从配置中读取各项参数
	_ord_sticky = cfg->getUInt32("ord_sticky");      // 挂单超时时间（秒）
	_begin_time= cfg->getUInt32("begin_time");      // 开始时间（HHMM格式）
	_end_time = cfg->getUInt32("end_time");         // 结束时间（HHMM格式）
	_total_secs = cfg->getUInt32("total_secs");     // 总执行时间（秒）
	_tail_secs = cfg->getUInt32("tail_secs");       // 收尾时间（秒）
	_total_times = cfg->getUInt32("total_times");   // 总执行次数
	_price_mode = cfg->getUInt32("price_mode");     // 价格模式（0-最新价,1-最优价,2-对手价）
	_price_offset = cfg->getUInt32("price_offset"); // 价格偏移
	_order_lots = cfg->getDouble("lots");           // 单次发单手数
	
	// 如果配置中有最小开仓数量参数，则读取
	if (cfg->has("minopenlots"))
		_min_open_lots = cfg->getDouble("minopenlots");
	
	// 计算执行总时间（秒）
	_total_secs = calTmSecs(_begin_time, _end_time);
	
	// 计算单次发单时间间隔（秒）
	// 去掉尾部时间后均匀分配，这样最后剩余的数量就有一个兵底发单的机制
	_fire_span = (_total_secs - _tail_secs) / _total_times;

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
void WtTWapExeUnit::on_order(uint32_t localid, const char* stdCode, bool isBuy, double leftover, double price, bool isCanceled)
{
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
		// 获取当前实际仓位
		double realPos = _ctx->getPosition(stdCode);
		
		// 如果实际仓位与本轮目标仓位不一致，需要重新发单
		if (!decimal::eq(realPos, _this_target))
		{
			/***---begin---23.5.22---zhaoyk***/
			// 记录订单撤销后重发日志
			_ctx->writeLog(fmtutil::format("Order {} of {} canceled, re_fire will be done", localid, stdCode));
			
			// 增加撤单次数，用于下次发单时的价格偏移
			_cancel_times++;
			
			// 撤单后重新发单，确保发单数量不小于最小开仓数量
			// fire_at_once(_this_target - realPos);
			fire_at_once(max(_min_open_lots, _this_target - realPos));
			/***---end---23.5.22---zhaoyk***/
		}
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
	// 标记交易通道为就绪状态
	_channel_ready = true;
	
	// 获取未完成的订单数量
	double undone = _ctx->getUndoneQty(_code.c_str());
	/***---begin---23.5.18---zhaoyk***/
	// 情况1: 如果有未完成订单但本地没有订单记录，需要清除未被管理的订单
	if (!decimal::eq(undone, 0) && !_orders_mon.has_order())
	//if (undone != 0 && !_orders_mon.has_order())
	/***---end---23.5.18---zhaoyk***/
	{
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


void WtTWapExeUnit::on_tick(WTSTickData* newTick)
{
	if (newTick == NULL || _code.compare(newTick->code()) != 0)
		return;

	bool isFirstTick = false;
	//如果原来的tick不为空,则要释放掉
	if (_last_tick)
	{
		_last_tick->release();
	}
	/***---begin---23.5.18---zhaoyk***/
	else
	{
		isFirstTick = true;
		//如果行情时间不在交易时间,这种情况一般是集合竞价的行情进来,下单会失败,所以直接过滤掉这笔行情
		if (_sess_info != NULL && !_sess_info->isInTradingTime(newTick->actiontime() / 100000))
			return;
	}
	/***---end---23.5.18---zhaoyk***/

	//新的tick数据,要保留
	_last_tick = newTick;
	_last_tick->retain();

	/*
	*	这里可以考虑一下
	*	如果写的上一次丢出去的单子不够达到目标仓位
	*	那么在新的行情数据进来的时候可以再次触发核心逻辑
	*/

	if (isFirstTick)	//如果是第一笔tick,则检查目标仓位,不符合则下单
	{
		double newVol = _target_pos;
		const char* stdCode = _code.c_str();
		double undone = _ctx->getUndoneQty(stdCode);
		double realPos = _ctx->getPosition(stdCode);
		if (!decimal::eq(newVol, undone + realPos)) //仓位变化要交易 
		{
			do_calc();
		}
	}
	else
	{
		uint64_t now = TimeUtils::getLocalTimeNow();
		bool hasCancel = false;
		if (_ord_sticky != 0 && _orders_mon.has_order()) 
		{			
			_orders_mon.check_orders(_ord_sticky, now, [this, &hasCancel](uint32_t localid) {
				if (_ctx->cancel(localid))
				{
					_cancel_cnt++;
					_ctx->writeLog(fmt::format("Order expired, cancelcnt updated to {}", _cancel_cnt).c_str());
					hasCancel = true;
				}
			});

		}

		if (!hasCancel && (now - _last_fire_time >= _fire_span * 1000)) 
		{
			do_calc();
		}
	}
}

void WtTWapExeUnit::on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price)
{
	//不用触发,这里在ontick里触发吧
}

void WtTWapExeUnit::on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message)
{
	if (!bSuccess)
	{
		//如果不是我发出去的订单,我就不管了
		if (!_orders_mon.has_order(localid))
			return;

		_orders_mon.erase_order(localid);

		do_calc();
	}
}

void WtTWapExeUnit::fire_at_once(double qty)
{
	if (decimal::eq(qty, 0))
		return;

	_last_tick->retain();
	WTSTickData* curTick = _last_tick;
	const char* code = _code.c_str();
	uint64_t now = TimeUtils::getLocalTimeNow();
	bool isBuy = decimal::gt(qty, 0);
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

	/***---begin---23.5.22---zhaoyk***/
	targetPx += _comm_info->getPriceTick() * _cancel_times* (isBuy ? 1 : -1);  
	/***---end---23.5.22---zhaoyk***/
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
	if (qty > 0)
		ids = _ctx->buy(code, targetPx, abs(qty));
	else
		ids = _ctx->sell(code, targetPx, abs(qty));

	_orders_mon.push_order(ids.data(), ids.size(), now, isCanCancel);

	curTick->release();
}

void WtTWapExeUnit::do_calc()
{
	CalcFlag flag(&_in_calc);
	if (flag)
		return;

	if (!_channel_ready)
		return;
//这里加一个锁，主要原因是实盘过程中发现
//在修改目标仓位的时候，会触发一次do_calc
//而ontick也会触发一次do_calc，两次调用是从两个线程分别触发的，所以会出现同时触发的情况
//如果不加锁，就会引起问题
//这种情况在原来的SimpleExecUnit没有出现，因为SimpleExecUnit只在set_position的时候触发
	StdUniqueLock lock(_mtx_calc);

	const char* code = _code.c_str();
	double undone = _ctx->getUndoneQty(code);
	double newVol = get_real_target(_target_pos);//真实目标价格
	double realPos = _ctx->getPosition(code);
	double diffQty = newVol - realPos;

	//有正在撤销的订单,则不能进行下一轮计算
	if (_cancel_cnt != 0)
	{
		_ctx->writeLog(fmt::format("{}尚有未完成撤单指令,暂时退出本轮执行", _code).c_str());
		return;
	}

	if (decimal::eq(diffQty, 0))
		return;
	//每一次发单要保障成交,所以如果有未完成单,说明上一轮没完成
	//有未完成订单，与实际仓位变动方向相反
	//则需要撤销现有订单
	if (decimal::lt(diffQty * undone, 0))
	{
		bool isBuy = decimal::gt(undone, 0);   
		OrderIDs ids = _ctx->cancel(code, isBuy);
		if (!ids.empty())
		{
			_orders_mon.push_order(ids.data(), ids.size(), _ctx->getCurTime());
			_cancel_cnt += ids.size();
			_ctx->writeLog(fmtutil::format("[{}@{}] live opposite order of {} canceled, cancelcnt -> {}", __FILE__, __LINE__, _code.c_str(), _cancel_cnt));//相反的订单已取消
		}
		return;
	}
	//因为是逐笔发单，所以如果有不需要撤销的未完成单，则暂不发单
	if (!decimal::eq(undone, 0))
	{
		_ctx->writeLog(fmt::format("{}上一轮有挂单未完成,暂时退出本轮执行", _code).c_str());
		return;
	}
	double curPos = realPos;
	if (_last_tick == NULL)
	{
		_ctx->writeLog(fmt::format("{}没有最新tick数据,退出执行逻辑", _code).c_str());
		return;
	}
	if (decimal::eq(curPos, newVol))
	{
		//当前仓位和最新仓位匹配时，如果不是全部清仓的需求，则直接退出计算了
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
	uint64_t curTickTime = (uint64_t)_last_tick->actiondate() * 1000000000 + _last_tick->actiontime();
	if (curTickTime <= _last_tick_time)
	{
		_ctx->writeLog(fmtutil::format("No tick of {} updated, {} <= {}, execute later", _code, curTickTime, _last_tick_time));
		return;
	}
	_last_tick_time = curTickTime;
	double this_qty = _order_lots; 	//单次发单手数
	/***---end---23.5.26---zhaoyk***/

	uint32_t leftTimes = _total_times - _fired_times;
	/***---begin---23.5.22---zhaoyk***/
	//_ctx->writeLog(fmt::format("第 {} 次发单", _fired_times).c_str());
	_ctx->writeLog(fmt::format("第 {} 次发单", _fired_times+1).c_str());

	/***---end---23.5.22---zhaoyk***/
	bool bNeedShowHand = false;
	//剩余次数为0,剩余数量不为0,说明要全部发出去了
	//剩余次数为0,说明已经到了兜底时间了,如果这个时候还有未完成数量,则需要发单
	/***---begin---23.5.22---zhaoyk***/
	//如果剩余此处为0 ,则需要全部下单
	//否则,取整(剩余数量/剩余次数)与1的最大值,即最小为_min_open_lots,但是要注意符号处理
	double curQty = 0;
	if (leftTimes == 0 && !decimal::eq(diffQty, 0))
	{
		bNeedShowHand = true;
		curQty = max(_min_open_lots, diffQty); 
	}
	else {
		curQty = std::max(_min_open_lots, round(abs(diffQty) / leftTimes)) * abs(diffQty) / diffQty;
	}
	
	/***---end---23.5.22---zhaoyk***/
	
	//设定本轮目标仓位
	_this_target = realPos + curQty;	//现仓位量+本轮下单量

	WTSTickData* curTick = _last_tick;
	uint64_t now = TimeUtils::getLocalTimeNow();
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
	OrderIDs ids;
	if (curQty > 0)
		ids = _ctx->buy(code, targetPx, abs(curQty));
	else
		ids = _ctx->sell(code, targetPx, abs(curQty));

	_orders_mon.push_order(ids.data(), ids.size(), now, isCanCancel);
	_last_fire_time = now;
	_fired_times += 1;

	curTick->release();
}

void WtTWapExeUnit::set_position(const char* stdCode, double newVol)
{
	if (_code.compare(stdCode) != 0)
		return;

	//double diff = newVol - _target_pos;
	if (decimal::eq(newVol, _target_pos))
		return;

	_target_pos = newVol;

	_fired_times = 0;

	do_calc();
}

