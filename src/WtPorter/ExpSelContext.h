/*!
 * \file ExpSelContext.h
 * \project WonderTrader
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief SEL策略导出上下文定义
 * \details 定义了SEL(选股)策略的导出上下文，用于将内部SEL策略接口暴露给外部模块
 */

#pragma once
#include "../WtCore/SelStraBaseCtx.h"

USING_NS_WTP;

/**
 * @brief SEL策略导出上下文类
 * @details 继承自SelStraBaseCtx基类，实现了SEL策略的导出接口，
 *          用于将内部SEL策略功能暴露给外部模块调用
 */
class ExpSelContext : public SelStraBaseCtx
{
public:
	/**
	 * @brief 构造函数
	 * @param env SEL引擎指针
	 * @param name 策略名称
	 * @param slippage 滑点值
	 */
	ExpSelContext(WtSelEngine* env, const char* name, int32_t slippage);
	
	/**
	 * @brief 析构函数
	 */
	virtual ~ExpSelContext();

public:
	/**
	 * @brief 初始化回调
	 * @details 策略初始化时调用，通知外部接口
	 */
	virtual void on_init() override;

	/**
	 * @brief 交易日开始回调
	 * @details 交易日开始时调用，通知外部接口
	 * @param uDate 交易日日期，格式为YYYYMMDD
	 */
	virtual void on_session_begin(uint32_t uDate) override;

	/**
	 * @brief 交易日结束回调
	 * @details 交易日结束时调用，通知外部接口
	 * @param uDate 交易日日期，格式为YYYYMMDD
	 */
	virtual void on_session_end(uint32_t uDate) override;

	/**
	 * @brief 策略调度回调
	 * @details 在策略调度时点调用，通知外部接口
	 * @param curDate 当前日期，格式为YYYYMMDD
	 * @param curTime 当前时间，格式为HHMMSS或HHMMSS000
	 */
	virtual void on_strategy_schedule(uint32_t curDate, uint32_t curTime) override;

	/**
	 * @brief K线闭合回调
	 * @details 收到K线闭合事件时调用，将K线数据转发给外部接口
	 * @param stdCode 标准化合约代码
	 * @param period K线周期，如m1/m5
	 * @param newBar 新的K线数据
	 */
	virtual void on_bar_close(const char* stdCode, const char* period, WTSBarStruct* newBar) override;

	/**
	 * @brief Tick数据更新回调
	 * @details 收到Tick数据时调用，将Tick数据转发给外部接口
	 * @param stdCode 标准化合约代码
	 * @param newTick 新的Tick数据
	 */
	virtual void on_tick_updated(const char* stdCode, WTSTickData* newTick) override;

};

