/**
 * @file CtaStraContext.h
 * @project	WonderTrader
 *
 * @author Wesley
 * @date 2020/03/30
 * 
 * @brief CTA策略上下文实现
 * @details 本文件定义了CTA策略的具体上下文类，继承自CtaStraBaseCtx基类
 *          提供了策略实现类与策略引擎的桥接层，负责将引擎的事件
 *          转发到具体的策略实现类中
 */
#pragma once
#include "CtaStraBaseCtx.h"

#include "../Includes/WTSDataDef.hpp"

NS_WTP_BEGIN
class WtCtaEngine;
NS_WTP_END

USING_NS_WTP;

class CtaStrategy;

/**
 * @brief CTA策略上下文类
 * @details 该类继承自 CtaStraBaseCtx，实现了具体的CTA策略上下文
 *          作为策略引擎和策略实现类之间的桥梁，它将各种事件转发给策略实现类
 *          并维护策略运行的状态
 */
class CtaStraContext : public CtaStraBaseCtx
{
public:
	/**
	 * @brief 构造函数
	 * @param engine CTA策略引擎指针
	 * @param name 策略名称
	 * @param slippage 滚动点设置
	 */
	CtaStraContext(WtCtaEngine* engine, const char* name, int32_t slippage);
	
	/**
	 * @brief 析构函数
	 */
	virtual ~CtaStraContext();

	/**
	 * @brief 设置策略实现类
	 * @details 将策略实现类对象关联到当前上下文
	 * @param stra 策略实现类指针
	 */
	void set_strategy(CtaStrategy* stra){ _strategy = stra; }
	
	/**
	 * @brief 获取策略实现类
	 * @return 策略实现类指针
	 */
	CtaStrategy* get_stragety() { return _strategy; }

public:
	/**
	 * @brief 回调函数区域
	 * @details 以下函数都是从引擎调用的回调函数，在回调函数中会调用相应的策略实现函数
	 */

	/**
	 * @brief 初始化回调
	 * @details 在策略加载后首次运行时调用，用于初始化策略参数和状态
	 */
	virtual void on_init() override;

	/**
	 * @brief 交易日开始回调
	 * @param uTDate 交易日期
	 */
	virtual void on_session_begin(uint32_t uTDate) override;

	/**
	 * @brief 交易日结束回调
	 * @param uTDate 交易日期
	 */
	virtual void on_session_end(uint32_t uTDate) override;

	/**
	 * @brief Tick数据更新回调
	 * @param stdCode 标准合约代码
	 * @param newTick 新的Tick数据
	 */
	virtual void on_tick_updated(const char* stdCode, WTSTickData* newTick) override;

	/**
	 * @brief K线周期结束回调
	 * @param stdCode 标准合约代码
	 * @param period 周期类型
	 * @param newBar 新的K线数据
	 */
	virtual void on_bar_close(const char* stdCode, const char* period, WTSBarStruct* newBar) override;

	/**
	 * @brief 定时计算回调
	 * @param curDate 当前日期
	 * @param curTime 当前时间
	 */
	virtual void on_calculate(uint32_t curDate, uint32_t curTime) override;

	/**
	 * @brief 条件触发回调
	 * @param stdCode 标准合约代码
	 * @param target 目标价格或目标仓位
	 * @param price 触发时的实际价格
	 * @param usertag 用户自定义标签
	 */
	virtual void on_condition_triggered(const char* stdCode, double target, double price, const char* usertag) override;

private:
	/**
	 * @brief 策略实现类指针
	 * @details 指向具体策略实现类的指针，用于调用策略的各种方法
	 */
	CtaStrategy*		_strategy;
};


