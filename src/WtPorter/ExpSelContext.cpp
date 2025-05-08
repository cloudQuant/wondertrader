/*!
 * \file ExpSelContext.cpp
 * \project WonderTrader
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief SEL策略导出上下文实现
 * \details 实现了SEL(选股)策略的导出上下文，用于将内部SEL策略接口暴露给外部模块
 */

#include "ExpSelContext.h"
#include "WtRtRunner.h"

/**
 * @brief 获取运行器实例的外部函数声明
 * @return WtRtRunner& 运行器实例的引用
 */
extern WtRtRunner& getRunner();


/**
 * @brief 构造函数实现
 * @details 初始化SEL策略导出上下文对象
 * @param env SEL引擎指针
 * @param name 策略名称
 * @param slippage 滑点值
 */
ExpSelContext::ExpSelContext(WtSelEngine* env, const char* name, int32_t slippage)
	: SelStraBaseCtx(env, name, slippage)
{
	// 构造函数仅调用基类构造函数进行初始化
}


/**
 * @brief 析构函数实现
 * @details 清理SEL策略导出上下文对象的资源
 */
ExpSelContext::~ExpSelContext()
{
	// 析构函数当前无需要清理的资源
}

/**
 * @brief 初始化回调实现
 * @details 策略初始化时调用，先调用基类初始化方法，然后通知外部接口
 */
void ExpSelContext::on_init()
{
	// 调用基类的初始化方法
	SelStraBaseCtx::on_init();

	// 向外部接口回调初始化事件
	getRunner().ctx_on_init(_context_id, ET_SEL);
}

/**
 * @brief 交易日开始回调实现
 * @details 交易日开始时调用，先调用基类方法，然后通知外部接口
 * @param uDate 交易日日期，格式为YYYYMMDD
 */
void ExpSelContext::on_session_begin(uint32_t uDate)
{
	// 调用基类的交易日开始方法
	SelStraBaseCtx::on_session_begin(uDate);

	// 向外部接口回调交易日开始事件，true表示开始
	getRunner().ctx_on_session_event(_context_id, uDate, true, ET_SEL);
}

/**
 * @brief 交易日结束回调实现
 * @details 交易日结束时调用，先通知外部接口，然后调用基类方法
 * @param uDate 交易日日期，格式为YYYYMMDD
 */
void ExpSelContext::on_session_end(uint32_t uDate)
{
	// 向外部接口回调交易日结束事件，false表示结束
	getRunner().ctx_on_session_event(_context_id, uDate, false, ET_SEL);

	// 调用基类的交易日结束方法
	SelStraBaseCtx::on_session_end(uDate);
}

/**
 * @brief 策略调度回调实现
 * @details 在策略调度时点调用，通知外部接口
 * @param curDate 当前日期，格式为YYYYMMDD
 * @param curTime 当前时间，格式为HHMMSS或HHMMSS000
 */
void ExpSelContext::on_strategy_schedule(uint32_t curDate, uint32_t curTime)
{
	// 向外部接口回调计算事件
	getRunner().ctx_on_calc(_context_id, curDate, curTime, ET_SEL);
}

/**
 * @brief K线闭合回调实现
 * @details 收到K线闭合事件时调用，将K线数据转发给外部接口
 * @param stdCode 标准化合约代码
 * @param period K线周期，如m1/m5
 * @param newBar 新的K线数据
 */
void ExpSelContext::on_bar_close(const char* stdCode, const char* period, WTSBarStruct* newBar)
{
	// 将K线数据转发给外部接口
	getRunner().ctx_on_bar(_context_id, stdCode, period, newBar, ET_SEL);
}

/**
 * @brief Tick数据更新回调实现
 * @details 收到Tick数据时调用，检查是否订阅了该合约的Tick数据，
 *          如果是，则将Tick数据转发给外部接口
 * @param stdCode 标准化合约代码
 * @param newTick 新的Tick数据
 */
void ExpSelContext::on_tick_updated(const char* stdCode, WTSTickData* newTick)
{
	// 检查是否订阅了该合约的Tick数据
	auto it = _tick_subs.find(stdCode);
	if (it == _tick_subs.end())
		return;

	// 将Tick数据转发给外部接口
	getRunner().ctx_on_tick(_context_id, stdCode, newTick, ET_SEL);
}
