/*!
 * @file WtHftStraDemo.cpp
 * @author wondertrader
 *
 * @brief 高频交易策略示例实现
 * @details 实现了一个基于理论价格和市场实际价格差异的简单高频交易策略
 */

#include "WtHftStraDemo.h"
#include "../Includes/IHftStraCtx.h"

#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/decimal.h"
#include "../Share/fmtlib.h"

/** 引用策略工厂名称外部变量 */
extern const char* FACT_NAME;

/**
 * @brief 构造函数
 * @param id 策略ID
 * @details 初始化策略实例并设置各成员变量的默认值
 */
WtHftStraDemo::WtHftStraDemo(const char* id)
	: HftStrategy(id)
	, _last_tick(NULL)
	, _last_entry_time(UINT64_MAX)
	, _channel_ready(false)
	, _last_calc_time(0)
	, _stock(false)
	, _unit(1)
	, _cancel_cnt(0)
	, _reserved(0)
{
}


/**
 * @brief 析构函数
 * @details 清理策略资源，释放保存的行情数据
 */
WtHftStraDemo::~WtHftStraDemo()
{
	if (_last_tick)
		_last_tick->release();
}

/**
 * @brief 获取策略名称
 * @return 策略名称字符串
 * @details 返回策略的唯一标识名称
 */
const char* WtHftStraDemo::getName()
{
	return "HftDemoStrategy";
}

/**
 * @brief 获取所属工厂名称
 * @return 工厂名称字符串
 * @details 返回创建当前策略的工厂名称
 */
const char* WtHftStraDemo::getFactName()
{
	return FACT_NAME;
}

/**
 * @brief 初始化策略
 * @param cfg 策略配置对象
 * @return 初始化是否成功
 * @details 从配置对象中加载策略参数，包括合约代码、超时秒数、交易频率等
 */
bool WtHftStraDemo::init(WTSVariant* cfg)
{
	//这里演示一下外部传入参数的获取
	_code = cfg->getCString("code");
	_secs = cfg->getUInt32("second");
	_freq = cfg->getUInt32("freq");
	_offset = cfg->getUInt32("offset");
	_reserved = cfg->getDouble("reserve");

	_stock = cfg->getBoolean("stock");
	_unit = _stock ? 100 : 1;

	return true;
}

/**
 * @brief 委托回报回调
 * @param localid 本地订单ID
 * @param bSuccess 委托是否成功
 * @param message 委托回报消息
 * @param userTag 用户标签
 * @details 委托发出后收到回报时调用，目前版本未实现具体处理逻辑
 */
void WtHftStraDemo::on_entrust(uint32_t localid, bool bSuccess, const char* message, const char* userTag)
{

}

/**
 * @brief 策略初始化回调
 * @param ctx 策略上下文
 * @details 在策略启动时调用，订阅行情数据并获取历史K线数据
 */
void WtHftStraDemo::on_init(IHftStraCtx* ctx)
{
	//WTSTickSlice* ticks = ctx->stra_get_ticks(_code.c_str(), _count);
	//if (ticks)
	//	ticks->release();

	WTSKlineSlice* kline = ctx->stra_get_bars(_code.c_str(), "m1", 30);
	if (kline)
		kline->release();

	ctx->stra_sub_ticks(_code.c_str());

	_ctx = ctx;
}

/**
 * @brief 执行策略计算
 * @param ctx 策略上下文
 * @details 根据策略逻辑计算交易信号并执行交易操作
 */
void WtHftStraDemo::do_calc(IHftStraCtx* ctx)
{
	const char* code = _code.c_str();

	//30秒内不重复计算
	uint64_t now = TimeUtils::makeTime(ctx->stra_get_date(), ctx->stra_get_time() * 100000 + ctx->stra_get_secs());//(uint64_t)ctx->stra_get_date()*1000000000 + (uint64_t)ctx->stra_get_time()*100000 + ctx->stra_get_secs();
	if (now - _last_entry_time <= _freq * 1000)
	{
		return;
	}

	WTSTickData* curTick = ctx->stra_get_last_tick(code);
	if (curTick == NULL)
		return;

	uint32_t curMin = curTick->actiontime() / 100000;	//actiontime是带毫秒的,要取得分钟,则需要除以10w
	if (curMin > _last_calc_time)
	{//如果spread上次计算的时候小于当前分钟,则重算spread
		//WTSKlineSlice* kline = ctx->stra_get_bars(code, "m5", 30);
		//if (kline)
		//	kline->release();

		//重算晚了以后,更新计算时间
		_last_calc_time = curMin;
	}

	int32_t signal = 0;
	double price = curTick->price();
	//计算部分
	double pxInThry = (curTick->bidprice(0)*curTick->askqty(0) + curTick->askprice(0)*curTick->bidqty(0)) / (curTick->bidqty(0) + curTick->askqty(0));

	//理论价格大于最新价
	if (pxInThry > price)
	{
		//正向信号
		signal = 1;
	}
	else if (pxInThry < price)
	{
		//反向信号
		signal = -1;
	}

	if (signal != 0)
	{
		double curPos = ctx->stra_get_position(code);
		curPos -= _reserved;

		WTSCommodityInfo* cInfo = ctx->stra_get_comminfo(code);

		if (signal > 0 && curPos <= 0)
		{//正向信号,且当前仓位小于等于0
			//最新价+2跳下单
			double targetPx = price + cInfo->getPriceTick() * _offset;
			auto ids = ctx->stra_buy(code, targetPx, _unit, "enterlong");

			_mtx_ords.lock();
			for (auto localid : ids)
			{
				_orders.insert(localid);
			}
			_mtx_ords.unlock();
			_last_entry_time = now;
		}
		else if (signal < 0 && (curPos > 0 || ((!_stock || !decimal::eq(_reserved, 0)) && curPos == 0)))
		{//反向信号,且当前仓位大于0,或者仓位为0但不是股票,或者仓位为0但是基础仓位有修正
			//最新价-2跳下单
			double targetPx = price - cInfo->getPriceTick()*_offset;
			auto ids = ctx->stra_sell(code, targetPx, _unit, "entershort");

			_mtx_ords.lock();
			for (auto localid : ids)
			{
				_orders.insert(localid);
			}
			_mtx_ords.unlock();
			_last_entry_time = now;
		}
	}

	curTick->release();
}

/**
 * @brief 行情数据回调
 * @param ctx 策略上下文
 * @param code 合约代码
 * @param newTick 新的行情数据
 * @details 当收到新的行情数据时，先检查待成交订单，然后执行策略计算
 */
void WtHftStraDemo::on_tick(IHftStraCtx* ctx, const char* code, WTSTickData* newTick)
{	
	if (_code.compare(code) != 0)
		return;

	if (!_orders.empty())
	{
		check_orders();
		return;
	}

	if (!_channel_ready)
		return;

	do_calc(ctx);
}

/**
 * @brief 检查订单状态
 * @details 检查当前未完成订单，如果超过指定时间未成交则撤单
 */
void WtHftStraDemo::check_orders()
{
	if (!_orders.empty() && _last_entry_time != UINT64_MAX)
	{
		uint64_t now = TimeUtils::makeTime(_ctx->stra_get_date(), _ctx->stra_get_time() * 100000 + _ctx->stra_get_secs());
		if (now - _last_entry_time >= _secs * 1000)	//如果超过一定时间没有成交完,则撤销
		{
			_mtx_ords.lock();
			for (auto localid : _orders)
			{
				_ctx->stra_cancel(localid);
				_cancel_cnt++;
				_ctx->stra_log_info(fmt::format("Order expired, cancelcnt updated to {}", _cancel_cnt).c_str());
			}
			_mtx_ords.unlock();
		}
	}
}

/**
 * @brief K线数据回调
 * @param ctx 策略上下文
 * @param code 合约代码
 * @param period 周期标识
 * @param times 周期倍数
 * @param newBar 新的K线数据
 * @details 当收到新的K线数据时调用，目前版本未实现具体处理逻辑
 */
void WtHftStraDemo::on_bar(IHftStraCtx* ctx, const char* code, const char* period, uint32_t times, WTSBarStruct* newBar)
{
	
}

/**
 * @brief 成交回调
 * @param ctx 策略上下文
 * @param localid 本地订单ID
 * @param stdCode 合约代码
 * @param isBuy 是否买入
 * @param qty 成交数量
 * @param price 成交价格
 * @param userTag 用户标签
 * @details 成交发生时调用，触发策略重新计算
 */
void WtHftStraDemo::on_trade(IHftStraCtx* ctx, uint32_t localid, const char* stdCode, bool isBuy, double qty, double price, const char* userTag)
{
	do_calc(ctx);
}

/**
 * @brief 持仓更新回调
 * @param ctx 策略上下文
 * @param stdCode 合约代码
 * @param isLong 是否多头仓位
 * @param prevol 前持仓量
 * @param preavail 前可用仓位
 * @param newvol 新持仓量
 * @param newavail 新可用仓位
 * @details 持仓量变化时调用，目前版本未实现具体处理逻辑
 */
void WtHftStraDemo::on_position(IHftStraCtx* ctx, const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail)
{
	
}

/**
 * @brief 订单状态回调
 * @param ctx 策略上下文
 * @param localid 本地订单ID
 * @param stdCode 合约代码
 * @param isBuy 是否买入
 * @param totalQty 总数量
 * @param leftQty 剩余数量
 * @param price 价格
 * @param isCanceled 是否已撤销
 * @param userTag 用户标签
 * @details 订单状态变化时调用，如果订单已完成或已撤销则从订单集合中移除
 */
void WtHftStraDemo::on_order(IHftStraCtx* ctx, uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled, const char* userTag)
{
	//如果不是我发出去的订单,我就不管了
	auto it = _orders.find(localid);
	if (it == _orders.end())
		return;

	//如果已撤销或者剩余数量为0,则清除掉原有的id记录
	if(isCanceled || leftQty == 0)
	{
		_mtx_ords.lock();
		_orders.erase(it);
		if (_cancel_cnt > 0)
		{
			_cancel_cnt--;
			_ctx->stra_log_info(fmt::format("cancelcnt -> {}", _cancel_cnt).c_str());
		}
		_mtx_ords.unlock();

		do_calc(ctx);
	}
}

/**
 * @brief 交易通道就绪回调
 * @param ctx 策略上下文
 * @details 当交易通道就绪可用时调用，标记通道为就绪状态
 */
void WtHftStraDemo::on_channel_ready(IHftStraCtx* ctx)
{
	double undone = _ctx->stra_get_undone(_code.c_str());
	if (!decimal::eq(undone, 0) && _orders.empty())
	{
		//这说明有未完成单不在监控之中,先撤掉
		_ctx->stra_log_info(fmt::format("{}有不在管理中的未完成单 {} 手,全部撤销", _code, undone).c_str());

		bool isBuy = (undone > 0);
		OrderIDs ids = _ctx->stra_cancel(_code.c_str(), isBuy, undone);
		for (auto localid : ids)
		{
			_orders.insert(localid);
		}
		_cancel_cnt += ids.size();

		_ctx->stra_log_info(fmt::format("cancelcnt -> {}", _cancel_cnt).c_str());
	}

	_channel_ready = true;
}

/**
 * @brief 交易通道断开回调
 * @param ctx 策略上下文
 * @details 当交易通道断开时调用，标记通道为断开状态
 */
void WtHftStraDemo::on_channel_lost(IHftStraCtx* ctx)
{
	_channel_ready = false;
}