/*!
 * \file ExpSelMocker.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 选股策略回测模拟器对外封装实现
 * \details 实现了选股策略回测模拟器对外封装类，负责与外部系统集成时的事件回调和数据传递
 *          主要功能是重写父类的各个虚函数，且在每个函数实现中调用getRunner()将事件向外部回调
 */
#include "ExpSelMocker.h"
#include "WtBtRunner.h"

/**
 * @brief 获取全局回测引擎实例
 * @return WtBtRunner的引用
 * @details 这是一个外部函数声明，用于获取全局的回测引擎实例，在回调到外部时使用
 */
extern WtBtRunner& getRunner();


/**
 * @brief 选股策略回测模拟器构造函数
 * @param replayer 历史数据回放器指针
 * @param name 策略名称
 * @param slippage 滑点设置，默认为0
 * @param isRatioSlp 是否为百分比滑点，默认为false
 * @details 初始化选股策略回测模拟器实例，调用父类构造函数完成初始化
 */
ExpSelMocker::ExpSelMocker(HisDataReplayer* replayer, const char* name, int32_t slippage /* = 0 */, bool isRatioSlp/* = false*/)
	: SelMocker(replayer, name, slippage, isRatioSlp)
{
}


/**
 * @brief 选股策略回测模拟器析构函数
 * @details 释放选股策略回测模拟器实例相关资源，父类析构函数会自动调用
 */
ExpSelMocker::~ExpSelMocker()
{
}

/**
 * @brief 策略初始化回调函数
 * @details 选股策略初始化时的处理逻辑
 *          1. 首先调用父类的on_init函数进行基础初始化
 *          2. 然后通过getRunner向外部回调策略初始化事件
 *          3. 最后触发全局初始化事件
 */
void ExpSelMocker::on_init()
{
	SelMocker::on_init();

	//向外部回调
	getRunner().ctx_on_init(_context_id, ET_SEL);

	getRunner().on_initialize_event();
}

/**
 * @brief 交易日开始回调函数
 * @param uDate 交易日期，格式为YYYYMMDD
 * @details 处理交易日开始时的逻辑
 *          1. 首先调用父类的on_session_begin函数执行基础处理
 *          2. 然后通过getRunner向外部回调特定策略上下文的交易日开始事件
 *          3. 最后触发全局交易日开始事件
 */
void ExpSelMocker::on_session_begin(uint32_t uDate)
{
	SelMocker::on_session_begin(uDate);

	getRunner().ctx_on_session_event(_context_id, uDate, true, ET_SEL);
	getRunner().on_session_event(uDate, true);
}

/**
 * @brief 交易日结束回调函数
 * @param uDate 交易日期，格式为YYYYMMDD
 * @details 处理交易日结束时的逻辑
 *          1. 首先调用父类的on_session_end函数执行基础处理
 *          2. 然后通过getRunner向外部回调特定策略上下文的交易日结束事件
 *          3. 最后触发全局交易日结束事件
 */
void ExpSelMocker::on_session_end(uint32_t uDate)
{
	SelMocker::on_session_end(uDate);

	getRunner().ctx_on_session_event(_context_id, uDate, false, ET_SEL);
	getRunner().on_session_event(uDate, false);
}

/**
 * @brief Tick数据更新回调函数
 * @param stdCode 标准化合约代码
 * @param newTick 新的Tick数据
 * @details 处理新到达Ticke数据的逻辑
 *          1. 首先检查当前合约是否已订阅，如果未订阅则直接返回
 *          2. 使用getRunner向外部回调Tick事件，传递对应的策略上下文ID、合约代码和数据
 *          注意这里不调用父类的on_tick_updated函数
 */
void ExpSelMocker::on_tick_updated(const char* stdCode, WTSTickData* newTick)
{
	auto it = _tick_subs.find(stdCode);
	if (it == _tick_subs.end())
		return;

	//向外部回调
	getRunner().ctx_on_tick(_context_id, stdCode, newTick, ET_SEL);
}

/**
 * @brief K线周期结束回调函数
 * @param stdCode 标准化合约代码
 * @param period K线周期，如"m1"、"d1"等
 * @param newBar 新的K线数据
 * @details 处理K线周期结束时的逻辑
 *          1. 首先调用父类的on_bar_close函数进行基础处理
 *          2. 然后使用getRunner向外部回调K线周期结束事件，传递策略上下文ID、合约代码、周期和数据
 */
void ExpSelMocker::on_bar_close(const char* stdCode, const char* period, WTSBarStruct* newBar)
{
	SelMocker::on_bar_close(stdCode, period, newBar);
	//要向外部回调
	getRunner().ctx_on_bar(_context_id, stdCode, period, newBar, ET_SEL);
}

/**
 * @brief 策略定时调度回调函数
 * @param curDate 当前日期，格式为YYYYMMDD
 * @param curTime 当前时间，格式为HHMMSS或HHMMSS000
 * @details 处理策略定时调度时的逻辑
 *          1. 首先调用父类的on_strategy_schedule函数执行基础的策略调度处理
 *          2. 然后使用getRunner向外部回调策略计算事件，传递策略上下文ID、当前日期和时间
 *          3. 最后触发全局策略调度事件
 */
void ExpSelMocker::on_strategy_schedule(uint32_t curDate, uint32_t curTime)
{
	SelMocker::on_strategy_schedule(curDate, curTime);

	//向外部回调
	getRunner().ctx_on_calc(_context_id, curDate, curTime, ET_SEL);

	getRunner().on_schedule_event(curDate, curTime);
}

/**
 * @brief 回测结束回调函数
 * @details 处理回测结束时的逻辑
 *          调用getRunner的on_backtest_end函数，触发全局回测结束事件
 *          注意这里没有调用父类的on_bactest_end函数，因为它只关注全局的回测结束处理
 */
void ExpSelMocker::on_bactest_end()
{
	getRunner().on_backtest_end();
}