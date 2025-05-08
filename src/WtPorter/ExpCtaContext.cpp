/*!
 * \file ExpCtaContext.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief CTA策略导出上下文实现
 * \details 实现了CTA策略导出的上下文类，用于将CTA策略引擎与外部接口连接
 */
#include "ExpCtaContext.h"
#include "WtRtRunner.h"

#include "../Share/StrUtil.hpp"

/**
 * @brief 获取全局运行器实例
 * @return WtRtRunner实例的引用
 */
extern WtRtRunner& getRunner();

/**
 * @brief 构造函数实现
 * @details 初始化基类并设置策略环境、名称和滑点
 * @param env CTA策略引擎指针
 * @param name 策略名称
 * @param slippage 滑点设置
 */
ExpCtaContext::ExpCtaContext(WtCtaEngine* env, const char* name, int32_t slippage)
	: CtaStraBaseCtx(env, name, slippage)
{
}


/**
 * @brief 析构函数实现
 * @details 清理资源，目前为空实现
 */
ExpCtaContext::~ExpCtaContext()
{
}

/**
 * @brief 策略初始化回调实现
 * @details 在策略初始化时调用，首先调用基类的初始化方法，
 *          然后将初始化事件转发给外部接口，最后输出图表信息
 */
void ExpCtaContext::on_init()
{
	// 调用基类的初始化方法
	CtaStraBaseCtx::on_init();

	// 向外部回调初始化事件
	getRunner().ctx_on_init(_context_id, ET_CTA);

	// 输出图表信息
	dump_chart_info();
}

/**
 * @brief 交易日开始回调实现
 * @details 在每个交易日开始时调用，首先调用基类的交易日开始方法，
 *          然后将交易日开始事件转发给外部接口
 * @param uDate 交易日期，格式为YYYYMMDD
 */
void ExpCtaContext::on_session_begin(uint32_t uDate)
{
	// 调用基类的交易日开始方法
	CtaStraBaseCtx::on_session_begin(uDate);

	// 向外部回调交易日开始事件，true表示开始
	getRunner().ctx_on_session_event(_context_id, uDate, true, ET_CTA);
}

/**
 * @brief 交易日结束回调实现
 * @details 在每个交易日结束时调用，首先将交易日结束事件转发给外部接口，
 *          然后调用基类的交易日结束方法
 * @param uDate 交易日期，格式为YYYYMMDD
 */
void ExpCtaContext::on_session_end(uint32_t uDate)
{
	// 向外部回调交易日结束事件，false表示结束
	getRunner().ctx_on_session_event(_context_id, uDate, false, ET_CTA);

	// 调用基类的交易日结束方法
	CtaStraBaseCtx::on_session_end(uDate);
}

/**
 * @brief Tick数据更新回调实现
 * @details 在收到新的Tick数据时调用，首先检查是否订阅了该合约的Tick数据，
 *          如果是，则将Tick数据转发给外部接口
 * @param stdCode 标准化合约代码
 * @param newTick 新的Tick数据
 */
void ExpCtaContext::on_tick_updated(const char* stdCode, WTSTickData* newTick)
{
	// 检查是否订阅了该合约的Tick数据
	auto it = _tick_subs.find(stdCode);
	if (it == _tick_subs.end())
		return;

	// 将Tick数据转发给外部接口
	getRunner().ctx_on_tick(_context_id, stdCode, newTick, ET_CTA);
}

/**
 * @brief K线周期结束回调实现
 * @details 在K线周期结束时调用，将新的K线数据转发给外部接口
 * @param stdCode 标准化合约代码
 * @param period K线周期标识，如m1/m5
 * @param newBar 新的K线数据
 */
void ExpCtaContext::on_bar_close(const char* stdCode, const char* period, WTSBarStruct* newBar)
{
	// 将新的K线数据转发给外部接口
	getRunner().ctx_on_bar(_context_id, stdCode, period, newBar, ET_CTA);
}

/**
 * @brief 计算回调实现
 * @details 在每个计算周期调用，将计算事件转发给外部接口
 * @param curDate 当前日期，格式为YYYYMMDD
 * @param curTime 当前时间，格式为HHMMSS或HHMMSS000
 */
void ExpCtaContext::on_calculate(uint32_t curDate, uint32_t curTime)
{
	// 将计算事件转发给外部接口
	getRunner().ctx_on_calc(_context_id, curDate, curTime, ET_CTA);
}

/**
 * @brief 条件触发回调实现
 * @details 在条件触发时调用，将条件触发事件转发给外部接口
 * @param stdCode 标准化合约代码
 * @param target 目标价格
 * @param price 当前价格
 * @param usertag 用户标签
 */
void ExpCtaContext::on_condition_triggered(const char* stdCode, double target, double price, const char* usertag)
{
	// 将条件触发事件转发给外部接口
	getRunner().ctx_on_cond_triggered(_context_id, stdCode, target, price, usertag, ET_CTA);
}
