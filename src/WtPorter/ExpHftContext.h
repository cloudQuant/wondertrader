/*!
 * \file ExpHftContext.h
 * \project WonderTrader
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief HFT策略导出上下文定义
 * \details 定义了HFT(高频交易)策略的导出上下文，用于将内部HFT策略接口暴露给外部模块
 */

#pragma once
#include "../WtCore/HftStraBaseCtx.h"

USING_NS_WTP;

/**
 * @brief HFT策略导出上下文类
 * @details 继承自HftStraBaseCtx基类，实现了HFT策略的导出接口，
 *          用于将内部HFT策略功能暴露给外部模块调用
 */
class ExpHftContext : public HftStraBaseCtx
{
public:
	/**
	 * @brief 构造函数
	 * @param engine HFT引擎指针
	 * @param name 策略名称
	 * @param bAgent 是否做为代理
	 * @param slippage 滑点值
	 */
	ExpHftContext(WtHftEngine* engine, const char* name, bool bAgent, int32_t slippage):HftStraBaseCtx(engine, name, bAgent, slippage){}
	
	/**
	 * @brief 析构函数
	 */
	virtual ~ExpHftContext(){}

public:
	/**
	 * @brief K线闭合回调
	 * @details 收到K线闭合事件时调用，将K线数据转发给外部接口
	 * @param code 合约代码
	 * @param period K线周期，如m1/m5
	 * @param times 周期倍数
	 * @param newBar 新的K线数据
	 */
	virtual void on_bar(const char* code, const char* period, uint32_t times, WTSBarStruct* newBar) override;

	/**
	 * @brief 交易通道丢失回调
	 * @details 交易通道连接丢失时调用，通知外部接口
	 */
	virtual void on_channel_lost() override;

	/**
	 * @brief 交易通道就绪回调
	 * @details 交易通道连接就绪时调用，通知外部接口
	 */
	virtual void on_channel_ready() override;

	/**
	 * @brief 委托回报回调
	 * @details 收到委托回报时调用，将委托状态转发给外部接口
	 * @param localid 本地委托ID
	 * @param stdCode 标准化合约代码
	 * @param bSuccess 是否成功
	 * @param message 回报消息
	 */
	virtual void on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message) override;

	/**
	 * @brief 初始化回调
	 * @details 策略初始化时调用，通知外部接口
	 */
	virtual void on_init() override;

	/**
	 * @brief 交易日开始回调
	 * @details 交易日开始时调用，通知外部接口
	 * @param uTDate 交易日日期，格式为YYYYMMDD
	 */
	virtual void on_session_begin(uint32_t uTDate) override;

	/**
	 * @brief 交易日结束回调
	 * @details 交易日结束时调用，通知外部接口
	 * @param uTDate 交易日日期，格式为YYYYMMDD
	 */
	virtual void on_session_end(uint32_t uTDate) override;

	/**
	 * @brief 订单回报回调
	 * @details 收到订单回报时调用，将订单状态转发给外部接口
	 * @param localid 本地订单ID
	 * @param stdCode 标准化合约代码
	 * @param isBuy 是否买入
	 * @param totalQty 委托总数量
	 * @param leftQty 剩余数量
	 * @param price 委托价格
	 * @param isCanceled 是否已撤销
	 */
	virtual void on_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled) override;

	/**
	 * @brief Tick数据回调
	 * @details 收到Tick数据时调用，更新动态收益并将Tick数据转发给外部接口
	 * @param code 合约代码
	 * @param newTick 新的Tick数据
	 */
	virtual void on_tick(const char* code, WTSTickData* newTick) override;

	/**
	 * @brief 委托队列数据回调
	 * @details 收到委托队列数据时调用，将数据转发给外部接口
	 * @param stdCode 标准化合约代码
	 * @param newOrdQue 新的委托队列数据
	 */
	virtual void on_order_queue(const char* stdCode, WTSOrdQueData* newOrdQue) override;

	/**
	 * @brief 委托明细数据回调
	 * @details 收到委托明细数据时调用，将数据转发给外部接口
	 * @param stdCode 标准化合约代码
	 * @param newOrdDtl 新的委托明细数据
	 */
	virtual void on_order_detail(const char* stdCode, WTSOrdDtlData* newOrdDtl) override;

	/**
	 * @brief 逐笔成交回调
	 * @details 收到逐笔成交数据时调用，将数据转发给外部接口
	 * @param stdCode 标准化合约代码
	 * @param newTrans 新的逐笔成交数据
	 */
	virtual void on_transaction(const char* stdCode, WTSTransData* newTrans) override;

	/**
	 * @brief 成交回报回调
	 * @details 收到成交回报时调用，将成交信息转发给外部接口
	 * @param localid 本地订单ID
	 * @param stdCode 标准化合约代码
	 * @param isBuy 是否买入
	 * @param vol 成交数量
	 * @param price 成交价格
	 */
	virtual void on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price) override;

	/**
	 * @brief 持仓回报回调
	 * @details 收到持仓回报时调用，将持仓信息转发给外部接口
	 * @param stdCode 标准化合约代码
	 * @param isLong 是否为多头持仓
	 * @param prevol 昨日持仓量
	 * @param preavail 昨日可用持仓量
	 * @param newvol 今日持仓量
	 * @param newavail 今日可用持仓量
	 * @param tradingday 交易日，格式为YYYYMMDD
	 */
	virtual void on_position(const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail, uint32_t tradingday) override;
};

