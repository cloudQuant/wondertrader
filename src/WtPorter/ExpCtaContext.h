/*!
 * \file ExpCtaContext.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief CTA策略导出上下文定义
 * \details 定义了CTA策略导出的上下文类，用于将CTA策略引擎与外部接口连接
 */
#pragma once
#include "../WtCore/CtaStraBaseCtx.h"

USING_NS_WTP;

/**
 * @brief CTA策略导出上下文类
 * @details 继承自CtaStraBaseCtx，实现了CTA策略的各种回调函数，用于将内部事件转发给外部接口
 */
class ExpCtaContext : public CtaStraBaseCtx
{
public:
	/**
	 * @brief 构造函数
	 * @param env CTA策略引擎指针
	 * @param name 策略名称
	 * @param slippage 滑点设置
	 */
	ExpCtaContext(WtCtaEngine* env, const char* name, int32_t slippage);

	/**
	 * @brief 析构函数
	 */
	virtual ~ExpCtaContext();

public:
	/**
	 * @brief 策略初始化回调
	 * @details 在策略初始化时调用，将初始化事件转发给外部接口
	 */
	virtual void on_init() override;

	/**
	 * @brief 交易日开始回调
	 * @details 在每个交易日开始时调用，将交易日开始事件转发给外部接口
	 * @param uDate 交易日期，格式为YYYYMMDD
	 */
	virtual void on_session_begin(uint32_t uDate) override;

	/**
	 * @brief 交易日结束回调
	 * @details 在每个交易日结束时调用，将交易日结束事件转发给外部接口
	 * @param uDate 交易日期，格式为YYYYMMDD
	 */
	virtual void on_session_end(uint32_t uDate) override;

	/**
	 * @brief Tick数据更新回调
	 * @details 在收到新的Tick数据时调用，将Tick数据转发给外部接口
	 * @param stdCode 标准化合约代码
	 * @param newTick 新的Tick数据
	 */
	virtual void on_tick_updated(const char* stdCode, WTSTickData* newTick) override;

	/**
	 * @brief K线周期结束回调
	 * @details 在K线周期结束时调用，将新的K线数据转发给外部接口
	 * @param stdCode 标准化合约代码
	 * @param period K线周期标识，如m1/m5
	 * @param newBar 新的K线数据
	 */
	virtual void on_bar_close(const char* stdCode, const char* period, WTSBarStruct* newBar) override;

	/**
	 * @brief 计算回调
	 * @details 在每个计算周期调用，将计算事件转发给外部接口
	 * @param curDate 当前日期，格式为YYYYMMDD
	 * @param curTime 当前时间，格式为HHMMSS或HHMMSS000
	 */
	virtual void on_calculate(uint32_t curDate, uint32_t curTime) override;

	/**
	 * @brief 条件触发回调
	 * @details 在条件触发时调用，将条件触发事件转发给外部接口
	 * @param stdCode 标准化合约代码
	 * @param target 目标价格
	 * @param price 当前价格
	 * @param usertag 用户标签
	 */
	virtual void on_condition_triggered(const char* stdCode, double target, double price, const char* usertag) override;
};

