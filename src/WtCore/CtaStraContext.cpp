/**
 * @file CtaStraContext.cpp
 * @project	WonderTrader
 *
 * @author Wesley
 * @date 2020/03/30
 * 
 * @brief CTA策略上下文实现
 * @details 本文件实现了CTA策略的具体上下文类（CtaStraContext）
 *          作为策略引擎和策略实现之间的桥梁，它负责将引擎事件
 *          转发到策略实现类中并维护策略运行状态
 */
#include "CtaStraContext.h"
#include "WtCtaEngine.h"
#include "../Includes/CtaStrategyDefs.h"

#include <exception>

#include "../Includes/WTSContractInfo.hpp"


/**
 * @brief 构造函数
 * @details 初始化CTA策略上下文，调用基类构造函数初始化基本参数
 * @param engine CTA策略引擎指针
 * @param name 策略名称
 * @param slippage 滚动点设置
 */
CtaStraContext::CtaStraContext(WtCtaEngine* engine, const char* name, int32_t slippage)
	: CtaStraBaseCtx(engine, name, slippage)
{
}


/**
 * @brief 析构函数
 * @details 清理CTA策略上下文资源
 */
CtaStraContext::~CtaStraContext()
{
}

//////////////////////////////////////////////////////////////////////////
/**
 * @brief K线周期结束回调
 * @details 当一个K线周期结束时调用此函数，将新完成的K线转发给具体策略实现类
 * @param code 标准合约代码
 * @param period 周期类型，如"day"、"min1"等
 * @param newBar 新的K线数据
 */
void CtaStraContext::on_bar_close(const char* code, const char* period, WTSBarStruct* newBar)
{
	if (_strategy)
		_strategy->on_bar(this, code, period, newBar);
}

/**
 * @brief 初始化回调
 * @details 策略加载后首次运行时调用此函数
 *          1. 调用基类初始化方法
 *          2. 调用策略实现类的初始化方法
 *          3. 导出图表信息
 */
void CtaStraContext::on_init()
{
	CtaStraBaseCtx::on_init();

	if (_strategy)
		_strategy->on_init(this);

	dump_chart_info();
}

/**
 * @brief 交易日开始回调
 * @details 每个交易日开始时调用此函数
 *          1. 调用基类的交易日开始处理
 *          2. 调用策略实现类的交易日开始处理
 * @param uTDate 交易日期，格式YYYYMMDD
 */
void CtaStraContext::on_session_begin(uint32_t uTDate)
{
	CtaStraBaseCtx::on_session_begin(uTDate);

	if (_strategy)
		_strategy->on_session_begin(this, uTDate);
}

/**
 * @brief 交易日结束回调
 * @details 每个交易日结束时调用此函数
 *          1. 调用策略实现类的交易日结束处理
 *          2. 调用基类的交易日结束处理
 * @param uTDate 交易日期，格式YYYYMMDD
 */
void CtaStraContext::on_session_end(uint32_t uTDate)
{
	if (_strategy)
		_strategy->on_session_end(this, uTDate);

	CtaStraBaseCtx::on_session_end(uTDate);
}

/**
 * @brief Tick数据更新回调
 * @details 收到新的Tick数据时调用此函数
 *          1. 检查是否订阅了该合约的Tick数据
 *          2. 如果已订阅，调用策略实现类的Tick处理方法
 * @param code 标准合约代码
 * @param newTick 新的Tick数据
 */
void CtaStraContext::on_tick_updated(const char* code, WTSTickData* newTick)
{
	auto it = _tick_subs.find(code);
	if (it == _tick_subs.end())
		return;

	if (_strategy)
		_strategy->on_tick(this, code, newTick);
}

/**
 * @brief 定时计算回调
 * @details 在定时任务执行时调用此函数，将事件转发给策略实现类
 * @param curDate 当前日期，格式YYYYMMDD
 * @param curTime 当前时间，格式HHMMSS
 */
void CtaStraContext::on_calculate(uint32_t curDate, uint32_t curTime)
{
	if (_strategy)
		_strategy->on_schedule(this, curDate, curTime);
}

/**
 * @brief 条件触发回调
 * @details 当条件交易触发时调用此函数，将触发事件转发给策略实现类
 * @param stdCode 标准合约代码
 * @param target 目标价格或目标仓位
 * @param price 触发时的实际价格
 * @param usertag 用户自定义标签
 */
void CtaStraContext::on_condition_triggered(const char* stdCode, double target, double price, const char* usertag)
{
	if (_strategy)
		_strategy->on_condition_triggered(this, stdCode, target, price, usertag);
}

