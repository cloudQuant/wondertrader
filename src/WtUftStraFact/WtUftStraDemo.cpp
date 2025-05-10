/**
 * @file WtUftStraDemo.cpp
 * @brief UFT策略演示类实现
 * @author Wesley
 * @date 未指定
 * 
 * @details 实现了WtUftStraDemo类的所有方法，包括策略初始化、数据处理、交易信号生成等功能
 */

#include "WtUftStraDemo.h"
#include "../Includes/IUftStraCtx.h"

#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/decimal.h"
#include "../Share/fmtlib.h"

/// @brief 外部声明的工厂名称
/// @note 在WtUftStraFact.cpp中定义
extern const char* FACT_NAME;

/**
 * @brief 构造函数
 * @param id 策略实例ID
 * @details 初始化策略实例的各个成员变量并设置初始值
 */
WtUftStraDemo::WtUftStraDemo(const char* id)
	: UftStrategy(id)              // 调用父类构造函数
	, _last_tick(NULL)             // 初始化最后一个Tick为空
	, _last_entry_time(UINT64_MAX) // 最后入场时间初始化为最大值，确保首次可以正常入场
	, _channel_ready(false)        // 初始化交易通道为未就绪状态
	, _last_calc_time(0)           // 最后计算时间初始化为0
	, _lots(1)                     // 交易手数默认为1手
	, _cancel_cnt(0)               // 撤单计数器初始化为0
{
}


/**
 * @brief 析构函数
 * @details 释放策略实例所持有的资源，主要是最后一个Tick数据
 */
WtUftStraDemo::~WtUftStraDemo()
{
	// 释放最后一个Tick数据的内存，避免内存泄漏
	if (_last_tick)
		_last_tick->release();
}

/**
 * @brief 获取策略名称
 * @return 策略名称字符串
 * @details 返回策略的唯一标识名称"UftDemoStrategy"
 */
const char* WtUftStraDemo::getName()
{
	return "UftDemoStrategy";
}

/**
 * @brief 获取策略所属工厂名称
 * @return 工厂名称字符串
 * @details 返回创建该策略的工厂名称，用于策略与工厂的关联
 */
const char* WtUftStraDemo::getFactName()
{
	return FACT_NAME;
}

/**
 * @brief 初始化策略
 * @param cfg 策略配置
 * @return 初始化是否成功
 * @details 从配置中读取策略所需的参数，包括交易品种代码、交易时间参数和交易数量等
 */
bool WtUftStraDemo::init(WTSVariant* cfg)
{
	//这里演示一下外部传入参数的获取
	_code = cfg->getCString("code");      // 获取交易品种代码
	_secs = cfg->getUInt32("second");     // 获取订单超时时间（秒）
	_freq = cfg->getUInt32("freq");       // 获取交易频率控制参数
	_offset = cfg->getUInt32("offset");   // 获取下单价格偏移跳数

	_lots = cfg->getDouble("lots");       // 获取交易手数

	return true;
}

/**
 * @brief 委托回调
 * @param localid 本地订单ID
 * @param bSuccess 委托是否成功
 * @param message 委托消息
 * @details 在委托发送后回调，如果委托失败则从内部订单集合中移除该委托
 */
void WtUftStraDemo::on_entrust(uint32_t localid, bool bSuccess, const char* message)
{
	// 委托失败的处理
	if(!bSuccess)
	{
		// 在订单集合中查找该ID
		auto it = _orders.find(localid);
		// 如果找到则删除该订单
		if(it != _orders.end())
			_orders.erase(it);
	}
}

/**
 * @brief 策略初始化回调
 * @param ctx 策略上下文
 * @details 在策略引擎启动时调用，用于设置参数监视、订阅数据等初始化操作
 */
void WtUftStraDemo::on_init(IUftStraCtx* ctx)
{
	// 设置需要监视的参数，这些参数可以在运行时动态更新
	ctx->watch_param("second", _secs);   // 监视超时时间参数
	ctx->watch_param("freq", _freq);     // 监视交易频率参数
	ctx->watch_param("offset", _offset); // 监视价格偏移参数
	ctx->watch_param("lots", _lots);     // 监视交易手数参数
	ctx->commit_param_watcher();          // 提交参数监视器

	// 获取历史K线数据，这里为测试目的获取1分钟线数30条
	WTSKlineSlice* kline = ctx->stra_get_bars(_code.c_str(), "m1", 30);
	if (kline)
		kline->release();  // 释放获取的K线数据内存

	// 订阅交易品种的Tick数据
	ctx->stra_sub_ticks(_code.c_str());

	// 保存策略上下文指针供后续使用
	_ctx = ctx;
}

/**
 * @brief Tick数据回调
 * @param ctx 策略上下文
 * @param code 合约代码
 * @param newTick 最新Tick数据
 * @details 在收到新的Tick数据时调用，是策略交易逻辑的核心实现部分
 */
void WtUftStraDemo::on_tick(IUftStraCtx* ctx, const char* code, WTSTickData* newTick)
{	
	// 如果不是监控的品种，则直接返回
	if (_code.compare(code) != 0)
		return;

	// 如果有待成交的订单，先检查是否超时
	if (!_orders.empty())
	{
		check_orders();
		return;
	}

	// 如果交易通道未就绪，则等待
	if (!_channel_ready)
		return;

	// 获取当前品种的最新Tick，这里只是为了测试接口
	WTSTickData* curTick = ctx->stra_get_last_tick(code);
	if (curTick)
		curTick->release();  // 释放内存

	// 提取Tick数据的分钟时间
	uint32_t curMin = newTick->actiontime() / 100000;	//actiontime是带毫秒的,要取得分钟,则需要除以10w
	if (curMin > _last_calc_time)
	{//如果spread上次计算的时候小于当前分钟,则重算spread
		//WTSKlineSlice* kline = ctx->stra_get_bars(code, "m5", 30);
		//if (kline)
		//	kline->release();

		//重算晚了以后,更新计算时间
		_last_calc_time = curMin;
	}

	// 频率控制，要求相邻入场时间间隔至少为_freq毫秒
	uint64_t now = TimeUtils::makeTime(ctx->stra_get_date(), ctx->stra_get_time() * 100000 + ctx->stra_get_secs());
	if(now - _last_entry_time <= _freq * 1000)
	{
		return;
	}

	// 交易信号，1为看多，-1为看空，0为没有信号
	int32_t signal = 0;
	// 获取当前最新价
	double price = newTick->price();
	// 计算理论价格，这里采用加权平均价格计算方法
	double pxInThry = (newTick->bidprice(0)*newTick->askqty(0) + newTick->askprice(0)*newTick->bidqty(0)) / (newTick->bidqty(0) + newTick->askqty(0));

	// 当理论价格大于实际价格时，生成做多信号
	if (pxInThry > price)
	{
		// 正向信号（做多）
		signal = 1;
	}
	// 当理论价格小于实际价格时，生成做空信号
	else if (pxInThry < price)
	{
		// 反向信号（做空）
		signal = -1;
	}

	// 如果有交易信号生成则处理
	if (signal != 0)
	{
		// 获取当前品种的持仓
		double curPos = ctx->stra_get_position(code);

		// 获取品种信息，用于查询价格步长等数据
		WTSCommodityInfo* cInfo = ctx->stra_get_comminfo(code);

		// 处理做多信号
		if(signal > 0  && decimal::le(curPos, 0))
		{//正向信号,且当前仓位小于等于0
			// 计算目标下单价格，在最新价基础上加上偏移跳数
			double targetPx = price + cInfo->getPriceTick() * _offset;

			// 发送买入指令
			auto ids = ctx->stra_buy(code, targetPx, _lots, UFT_OrderFlag_Nor);

			{
				// 将新生成的订单ID加入监控列表
				_mtx_ords.lock();
				for (uint32_t localid : ids)
					_orders.insert(localid);
				_mtx_ords.unlock();
				// 更新最后入场时间
				_last_entry_time = now;
			}
			
		}
		// 处理做空信号
		else if (signal < 0 && decimal::ge(curPos, 0))
		{//反向信号,且当前仓位大于0,或者仓位为0但不是股票,或者仓位为0但是基础仓位有修正
			// 计算目标下单价格，在最新价基础上减去偏移跳数
			double targetPx = price - cInfo->getPriceTick()*_offset;

			// 发送卖出指令
			auto ids = ctx->stra_sell(code, targetPx, _lots, UFT_OrderFlag_Nor);

			{
				// 将新生成的订单ID加入监控列表
				_mtx_ords.lock();
				for (uint32_t localid : ids)
					_orders.insert(localid);
				_mtx_ords.unlock();
				// 更新最后入场时间
				_last_entry_time = now;
			}
		}
	}
}

/**
 * @brief 检查订单状态
 * @details 检查订单是否超时，如果超过设定时间未成交，则自动撤销
 */
void WtUftStraDemo::check_orders()
{
	// 如果有待成交的订单且最后入场时间有效
	if (!_orders.empty() && _last_entry_time != UINT64_MAX)
	{
		// 获取当前时间
		uint64_t now = TimeUtils::makeTime(_ctx->stra_get_date(), _ctx->stra_get_time() * 100000 + _ctx->stra_get_secs());
		// 如果超过设定的超时时间（_secs秒），则撤销所有订单
		if (now - _last_entry_time >= _secs * 1000)	//如果超过一定时间没有成交完,则撤销
		{
			// 加锁以保护并发安全
			_mtx_ords.lock();
			// 遍历并撤销所有订单
			for (auto localid : _orders)
			{
				_ctx->stra_cancel(localid);  // 撤销订单
				_cancel_cnt++;               // 撤单计数器增加
				// 输出日志，记录撤单信息
				_ctx->stra_log_info(fmt::format("Order expired, cancelcnt updated to {}", _cancel_cnt).c_str());
			}
			// 释放锁
			_mtx_ords.unlock();
		}
	}
}

/**
 * @brief K线数据回调
 * @param ctx 策略上下文
 * @param code 合约代码
 * @param period 周期标识符
 * @param times 周期倦数
 * @param newBar 最新K线数据
 * @details 在收到新的K线数据时调用，当前示例策略中未实现具体逻辑
 */
void WtUftStraDemo::on_bar(IUftStraCtx* ctx, const char* code, const char* period, uint32_t times, WTSBarStruct* newBar)
{
	// 当前示例策略中不使用K线数据进行交易决策
}

/**
 * @brief 成交回调
 * @param ctx 策略上下文
 * @param localid 本地订单ID
 * @param stdCode 合约代码
 * @param isLong 是否为多头方向
 * @param offset 开平仓标记
 * @param qty 成交数量
 * @param price 成交价格
 * @details 在有成交发生时调用，当前示例策略中未实现具体逻辑
 */
void WtUftStraDemo::on_trade(IUftStraCtx* ctx, uint32_t localid, const char* stdCode, bool isLong, uint32_t offset, double qty, double price)
{
	// 当前示例策略中不处理成交回调
}

/**
 * @brief 持仓变更回调
 * @param ctx 策略上下文
 * @param stdCode 合约代码
 * @param isLong 是否为多头方向
 * @param prevol 前期总持仓
 * @param preavail 前期可用持仓
 * @param newvol 新的总持仓
 * @param newavail 新的可用持仓
 * @details 在持仓变更时调用，记录前一交易日持仓并输出日志
 */
void WtUftStraDemo::on_position(IUftStraCtx* ctx, const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail)
{
	// 如果不是监控的品种，则直接返回
	if (_code != stdCode)
		return;

	// 保存前一交易日持仓值
	_prev = prevol;
	// 输出日志，记录前一交易日持仓情况
	_ctx->stra_log_info(fmt::format("There are {} of {} before today", _prev, stdCode).c_str());
}

/**
 * @brief 订单状态回调
 * @param ctx 策略上下文
 * @param localid 本地订单ID
 * @param stdCode 合约代码
 * @param isLong 是否为多头方向
 * @param offset 开平仓标记
 * @param totalQty 总数量
 * @param leftQty 剩余数量
 * @param price 价格
 * @param isCanceled 是否已撤销
 * @details 在订单状态变更时调用，用于管理内部订单集合，当订单完成或撤销时从集合中移除
 */
void WtUftStraDemo::on_order(IUftStraCtx* ctx, uint32_t localid, const char* stdCode, bool isLong, uint32_t offset, double totalQty, double leftQty, double price, bool isCanceled)
{
	//如果不是我发出去的订单,我就不管了
	auto it = _orders.find(localid);
	if (it == _orders.end())
		return;

	//如果已撤销或者剩余数量为0,则清除掉原有的id记录
	if(isCanceled || leftQty == 0)
	{
		// 加锁以保护并发安全
		_mtx_ords.lock();
		// 从订单集合中移除该订单
		_orders.erase(it);
		// 如果有撤单计数，则减少计数并输出日志
		if (_cancel_cnt > 0)
		{
			_cancel_cnt--;
			_ctx->stra_log_info(fmt::format("cancelcnt -> {}", _cancel_cnt).c_str());
		}
		// 释放锁
		_mtx_ords.unlock();
	}
}

/**
 * @brief 交易通道就绪回调
 * @param ctx 策略上下文
 * @details 在交易通道准备就绪时调用，处理未完成订单并标记通道就绪状态
 */
void WtUftStraDemo::on_channel_ready(IUftStraCtx* ctx)
{
	// 获取当前品种的未完成订单数量
	double undone = _ctx->stra_get_undone(_code.c_str());
	// 如果有未完成订单但不在当前监控列表中，则需要处理
	if (!decimal::eq(undone, 0) && _orders.empty())
	{
		//这说明有未完成单不在监控之中,先撤掉
		_ctx->stra_log_info(fmt::format("{}有不在管理中的未完成单 {} 手,全部撤销", _code, undone).c_str());

		// 撤销所有未完成订单并获取撤单ID列表
		OrderIDs ids = _ctx->stra_cancel_all(_code.c_str());
		// 将撤单ID添加到监控列表中
		for (auto localid : ids)
		{
			_orders.insert(localid);
		}
		// 更新撤单计数器
		_cancel_cnt += ids.size();

		// 输出日志记录撤单信息
		_ctx->stra_log_info(fmt::format("cancelcnt -> {}", _cancel_cnt).c_str());
	}

	// 标记交易通道为就绪状态
	_channel_ready = true;
}

/**
 * @brief 交易通道中断回调
 * @param ctx 策略上下文
 * @details 在交易通道中断时调用，标记通道为非就绪状态
 */
void WtUftStraDemo::on_channel_lost(IUftStraCtx* ctx)
{
	// 标记交易通道为非就绪状态，暂停交易活动
	_channel_ready = false;
}

/**
 * @brief 策略参数更新回调
 * @details 在策略参数变更时调用，从上下文读取最新的参数值并应用到策略中
 */
void WtUftStraDemo::on_params_updated()
{
	// 以下是对应于init中设置的监视参数
	//ctx->watch_param("second", _secs);
	//ctx->watch_param("freq", _freq);
	//ctx->watch_param("offset", _offset);
	//ctx->watch_param("lots", _lots);

	// 从上下文中读取更新后的参数值
	_secs = _ctx->read_param("second", _secs);    // 读取超时时间参数
	_freq = _ctx->read_param("freq", _freq);      // 读取交易频率参数
	_offset = _ctx->read_param("offset", _offset); // 读取价格偏移参数
	_lots = _ctx->read_param("lots", _lots);       // 读取交易手数参数

	// 输出日志记录参数更新情况
	_ctx->stra_log_info(fmtutil::format("[{}] Params updated, second: {}, freq: {}, offset: {}, lots: {}", _id.c_str(), _secs, _freq, _offset, _lots));
}