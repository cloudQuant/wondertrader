/*!
 * \file ExpHftContext.cpp
 * \project WonderTrader
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief HFT策略导出上下文实现
 * \details 实现了HFT(高频交易)策略的导出上下文，用于将内部HFT策略接口暴露给外部模块
 */

#include "ExpHftContext.h"
#include "WtRtRunner.h"
#include "../Share/StrUtil.hpp"

/**
 * @brief 获取运行器实例的外部函数声明
 * @return WtRtRunner& 运行器实例的引用
 */
extern WtRtRunner& getRunner();

/**
 * @brief K线闭合回调实现
 * @details 收到K线闭合事件时调用，将K线数据转发给外部接口
 * @param code 合约代码
 * @param period K线周期，如m1/m5
 * @param times 周期倍数
 * @param newBar 新的K线数据
 */
void ExpHftContext::on_bar(const char* code, const char* period, uint32_t times, WTSBarStruct* newBar)
{
	// 如果K线数据为空，直接返回
	if (newBar == NULL)
		return;

	// 线程本地静态变量，用于存储真实周期（周期+倍数）
	thread_local static char realPeriod[8] = { 0 };
	fmtutil::format_to(realPeriod, "{}{}", period, times);

	// 将K线数据转发给外部接口
	getRunner().ctx_on_bar(_context_id, code, realPeriod, newBar, ET_HFT);

	// 调用基类的K线闭合回调
	HftStraBaseCtx::on_bar(code, period, times, newBar);
}

/**
 * @brief 交易通道丢失回调实现
 * @details 交易通道连接丢失时调用，通知外部接口
 */
void ExpHftContext::on_channel_lost()
{
	// 将交易通道丢失事件转发给外部接口
	getRunner().hft_on_channel_lost(_context_id, _trader->id());

	// 调用基类的交易通道丢失回调
	HftStraBaseCtx::on_channel_lost();
}

/**
 * @brief 交易通道就绪回调实现
 * @details 交易通道连接就绪时调用，通知外部接口
 */
void ExpHftContext::on_channel_ready()
{
	// 将交易通道就绪事件转发给外部接口
	getRunner().hft_on_channel_ready(_context_id, _trader->id());

	// 调用基类的交易通道就绪回调
	HftStraBaseCtx::on_channel_ready();
}

/**
 * @brief 委托回报回调实现
 * @details 收到委托回报时调用，将委托状态转发给外部接口
 * @param localid 本地委托ID
 * @param stdCode 标准化合约代码
 * @param bSuccess 是否成功
 * @param message 回报消息
 */
void ExpHftContext::on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message)
{
	// 将委托回报转发给外部接口，并包含订单标签
	getRunner().hft_on_entrust(_context_id, localid, stdCode, bSuccess, message, getOrderTag(localid));

	// 调用基类的委托回报回调
	HftStraBaseCtx::on_entrust(localid, stdCode, bSuccess, message);
}

/**
 * @brief 初始化回调实现
 * @details 策略初始化时调用，先调用基类初始化方法，然后通知外部接口
 */
void ExpHftContext::on_init()
{
	// 调用基类的初始化方法
	HftStraBaseCtx::on_init();

	// 向外部接口回调初始化事件
	getRunner().ctx_on_init(_context_id, ET_HFT);
}

/**
 * @brief 交易日开始回调实现
 * @details 交易日开始时调用，先调用基类方法，然后通知外部接口
 * @param uTDate 交易日日期，格式为YYYYMMDD
 */
void ExpHftContext::on_session_begin(uint32_t uTDate)
{
	// 调用基类的交易日开始方法
	HftStraBaseCtx::on_session_begin(uTDate);

	// 向外部接口回调交易日开始事件，true表示开始
	getRunner().ctx_on_session_event(_context_id, uTDate, true, ET_HFT);
}

/**
 * @brief 交易日结束回调实现
 * @details 交易日结束时调用，先通知外部接口，然后调用基类方法
 * @param uTDate 交易日日期，格式为YYYYMMDD
 */
void ExpHftContext::on_session_end(uint32_t uTDate)
{
	// 向外部接口回调交易日结束事件，false表示结束
	getRunner().ctx_on_session_event(_context_id, uTDate, false, ET_HFT);

	// 调用基类的交易日结束方法
	HftStraBaseCtx::on_session_end(uTDate);
}

/**
 * @brief 订单回报回调实现
 * @details 收到订单回报时调用，将订单状态转发给外部接口
 * @param localid 本地订单ID
 * @param stdCode 标准化合约代码
 * @param isBuy 是否买入
 * @param totalQty 委托总数量
 * @param leftQty 剩余数量
 * @param price 委托价格
 * @param isCanceled 是否已撤销
 */
void ExpHftContext::on_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled)
{
	// 将订单回报转发给外部接口，并包含订单标签
	getRunner().hft_on_order(_context_id, localid, stdCode, isBuy, totalQty, leftQty, price, isCanceled, getOrderTag(localid));

	// 调用基类的订单回报回调
	HftStraBaseCtx::on_order(localid, stdCode, isBuy, totalQty, leftQty, price, isCanceled);
}

/**
 * @brief 持仓回报回调实现
 * @details 收到持仓回报时调用，将持仓信息转发给外部接口
 * @param stdCode 标准化合约代码
 * @param isLong 是否为多头持仓
 * @param prevol 昨日持仓量
 * @param preavail 昨日可用持仓量
 * @param newvol 今日持仓量
 * @param newavail 今日可用持仓量
 * @param tradingday 交易日，格式为YYYYMMDD
 */
void ExpHftContext::on_position(const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail, uint32_t tradingday)
{
	// 将持仓回报转发给外部接口
	getRunner().hft_on_position(_context_id, stdCode, isLong, prevol, preavail, newvol, newavail);
}

/**
 * @brief Tick数据回调实现
 * @details 收到Tick数据时调用，更新动态收益并将Tick数据转发给外部接口
 * @param code 合约代码
 * @param newTick 新的Tick数据
 */
void ExpHftContext::on_tick(const char* code, WTSTickData* newTick)
{
	// 更新动态收益
	update_dyn_profit(code, newTick);

	// 检查是否订阅了该合约的Tick数据
	auto it = _tick_subs.find(code);
	if (it != _tick_subs.end())
	{
		// 将Tick数据转发给外部接口
		getRunner().ctx_on_tick(_context_id, code, newTick, ET_HFT);
	}

	// 调用基类的Tick数据回调
	HftStraBaseCtx::on_tick(code, newTick);
}

/**
 * @brief 委托队列数据回调实现
 * @details 收到委托队列数据时调用，将数据转发给外部接口
 * @param stdCode 标准化合约代码
 * @param newOrdQue 新的委托队列数据
 */
void ExpHftContext::on_order_queue(const char* stdCode, WTSOrdQueData* newOrdQue)
{
	// 将委托队列数据转发给外部接口
	getRunner().hft_on_order_queue(_context_id, stdCode, newOrdQue);
}

/**
 * @brief 委托明细数据回调实现
 * @details 收到委托明细数据时调用，将数据转发给外部接口
 * @param stdCode 标准化合约代码
 * @param newOrdDtl 新的委托明细数据
 */
void ExpHftContext::on_order_detail(const char* stdCode, WTSOrdDtlData* newOrdDtl)
{
	// 将委托明细数据转发给外部接口
	getRunner().hft_on_order_detail(_context_id, stdCode, newOrdDtl);
}

/**
 * @brief 逐笔成交回调实现
 * @details 收到逐笔成交数据时调用，将数据转发给外部接口
 * @param stdCode 标准化合约代码
 * @param newTrans 新的逐笔成交数据
 */
void ExpHftContext::on_transaction(const char* stdCode, WTSTransData* newTrans)
{
	// 将逐笔成交数据转发给外部接口
	getRunner().hft_on_transaction(_context_id, stdCode, newTrans);
}

/**
 * @brief 成交回报回调实现
 * @details 收到成交回报时调用，将成交信息转发给外部接口
 * @param localid 本地订单ID
 * @param stdCode 标准化合约代码
 * @param isBuy 是否买入
 * @param vol 成交数量
 * @param price 成交价格
 */
void ExpHftContext::on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price)
{
	// 将成交回报转发给外部接口，并包含订单标签
	getRunner().hft_on_trade(_context_id, localid, stdCode, isBuy, vol, price, getOrderTag(localid));

	// 调用基类的成交回报回调
	HftStraBaseCtx::on_trade(localid, stdCode, isBuy, vol, price);
}