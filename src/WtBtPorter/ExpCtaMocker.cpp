/*!
 * \file ExpCtaMocker.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief CTA策略回测模拟器对外封装实现
 * \details 实现了对外提供的CTA策略回测模拟器类，用于在回测引擎中实现CTA策略的模拟测试。
 *          CTA(Commodity Trading Advisor)策略是一种基于股票、期货等金融工具进行交易的策略类型。
 */
#include "ExpCtaMocker.h"
#include "WtBtRunner.h"

/**
 * @brief 获取全局回测引擎实例
 * @return WtBtRunner& 全局回测引擎实例的引用
 * @details 外部声明的函数，用于获取全局唯一的回测引擎实例
 *          所有的策略模拟器都通过该函数与外部回测引擎进行交互
 */
extern WtBtRunner& getRunner();

/**
 * @brief 构造函数
 * @param replayer 历史数据回放器指针，用于提供回测所需的历史数据
 * @param name 策略名称，用于唯一标识策略实例
 * @param slippage 滑点数量，模拟交易时的滑点损失，默认为0
 * @param persistData 是否持久化数据，默认为true
 * @param notifier 事件通知器指针，用于实现事件通知功能，默认为NULL
 * @param isRatioSlp 是否为百分比滑点，默认为false（固定点数滑点）
 * @details 初始化CTA策略回测模拟器，通过调用父类 CtaMocker 构造函数来初始化
 *          父类负责大部分策略模拟器的基础功能
 *          ExpCtaMocker主要添加了对外部接口的调用和处理
 */
ExpCtaMocker::ExpCtaMocker(HisDataReplayer* replayer, const char* name, int32_t slippage /* = 0 */, bool persistData /* = true */, EventNotifier* notifier /* = NULL */, bool isRatioSlp /* = false */)
	: CtaMocker(replayer, name, slippage, persistData, notifier, isRatioSlp)
{
}

/**
 * @brief 析构函数
 * @details 清理CTA策略回测模拟器资源
 *          当前实现为空，因为大部分资源清理由父类的析构函数处理
 *          如果有特殊的资源需要释放，应在此函数中实现
 */
ExpCtaMocker::~ExpCtaMocker()
{
}

/**
 * @brief 策略初始化回调函数
 * @details 当策略被创建后需要进行初始化时触发此回调
 *          1. 首先调用父类的on_init函数进行基本的初始化设置
 *          2. 然后通过getRunner调用外部引擎的ctx_on_init接口，将策略初始化事件通知给上下文
 *          3. 最后调用全局初始化事件处理函数on_initialize_event
 *          在CTA策略中，初始化过程包括设置策略参数、订阅数据、初始化指标等
 */
void ExpCtaMocker::on_init()
{
	CtaMocker::on_init();

	//向外部回调
	getRunner().ctx_on_init(_context_id, ET_CTA);

	getRunner().on_initialize_event();
}

/**
 * @brief 交易日开始回调函数
 * @param uCurDate 当前交易日日期，格式为YYYYMMDD
 * @details 每个交易日开始时触发此回调
 *          1. 首先调用父类的on_session_begin函数进行基本的交易日开始处理
 *          2. 然后通过getRunner调用外部引擎的ctx_on_session_event接口，将交易日开始事件通知给上下文
 *          3. 最后调用全局交易日事件处理函数on_session_event
 *          在CTA策略中，交易日开始处理包括准备每日数据、重置状态、清理过期缓存等
 */
void ExpCtaMocker::on_session_begin(uint32_t uCurDate)
{
	CtaMocker::on_session_begin(uCurDate);

	getRunner().ctx_on_session_event(_context_id, uCurDate, true, ET_CTA);
	getRunner().on_session_event(uCurDate, true);
}

/**
 * @brief 交易日结束回调函数
 * @param uCurDate 当前交易日日期，格式为YYYYMMDD
 * @details 每个交易日结束时触发此回调
 *          1. 首先通过getRunner调用外部引擎的ctx_on_session_event接口，将交易日结束事件通知给上下文
 *          2. 然后调用全局交易日事件处理函数on_session_event
 *          3. 最后调用父类的on_session_end函数进行基本的交易日结束处理
 *          注意这里与on_session_begin不同，先通知外部系统，再调用父类方法
 *          在CTA策略中，交易日结束处理包括结算当日交易、记录统计数据、保存状态等
 */
void ExpCtaMocker::on_session_end(uint32_t uCurDate)
{
	getRunner().ctx_on_session_event(_context_id, uCurDate, false, ET_CTA);
	getRunner().on_session_event(uCurDate, false);

	CtaMocker::on_session_end(uCurDate);
}

/**
 * @brief 实时行情数据更新回调函数
 * @param stdCode 标准化合约代码
 * @param newTick 新的Tick行情数据
 * @details 当收到新的Tick数据时触发此回调
 *          1. 首先检查合约是否已订阅(_tick_subs映射中是否存在)，如果未订阅则直接返回
 *          2. 然后调用父类的on_tick_updated函数进行基本的Tick数据处理
 *          3. 最后通过getRunner调用外部引擎的ctx_on_tick接口，将Tick数据通知给上下文
 *          在CTA策略中，Tick数据用于实时性较高的交易信号生成和快速市场反应
 */
void ExpCtaMocker::on_tick_updated(const char* stdCode, WTSTickData* newTick)
{
	auto it = _tick_subs.find(stdCode);
	if (it == _tick_subs.end())
		return;

	CtaMocker::on_tick_updated(stdCode, newTick);
	getRunner().ctx_on_tick(_context_id, stdCode, newTick, ET_CTA);
}

/**
 * @brief K线周期结束回调函数
 * @param code 标准化合约代码
 * @param period K线周期，如"m1"、"d1"等
 * @param newBar 新的K线数据
 * @details 当收到新的K线数据时触发此回调
 *          1. 首先调用父类的on_bar_close函数进行基本的K线数据处理
 *          2. 然后通过getRunner调用外部引擎的ctx_on_bar接口，将K线数据通知给上下文
 *          在CTA策略中，K线数据是最常用的数据类型，用于技术分析、信号生成和交易决策
 */
void ExpCtaMocker::on_bar_close(const char* code, const char* period, WTSBarStruct* newBar)
{
	CtaMocker::on_bar_close(code, period, newBar);

	//要向外部回调
	getRunner().ctx_on_bar(_context_id, code, period, newBar, ET_CTA);
}

/**
 * @brief 策略计算回调函数
 * @param curDate 当前日期，格式为YYYYMMDD
 * @param curTime 当前时间，格式为HHMMSS
 * @details 在回测引擎进行定时计算时触发此回调
 *          1. 首先调用父类的on_calculate函数进行基本的策略计算
 *          2. 然后通过getRunner调用外部引擎的ctx_on_calc接口，将定时计算事件通知给上下文
 *          在CTA策略中，该函数用于定时执行策略计算逻辑，不依赖于具体的行情数据更新
 *          常用于定时再平衡、定时发送交易信号等基于时间的策略逻辑
 */
void ExpCtaMocker::on_calculate(uint32_t curDate, uint32_t curTime)
{
	CtaMocker::on_calculate(curDate, curTime);
	getRunner().ctx_on_calc(_context_id, curDate, curTime, ET_CTA);
}

/**
 * @brief 策略计算完成回调函数
 * @param curDate 当前日期，格式为YYYYMMDD
 * @param curTime 当前时间，格式为HHMMSS
 * @details 在每个计算周期的策略计算完成后触发此回调
 *          1. 首先调用父类的on_calculate_done函数执行基本的计算完成处理
 *          2. 然后通过getRunner调用外部引擎的ctx_on_calc_done接口，将计算完成事件通知给上下文
 *          3. 最后调用全局调度事件处理函数on_schedule_event
 *          在CTA策略中，该函数用于确保所有策略计算都已完成，然后执行后续操作
 *          通常在这里进行汇总分析或执行最终的交易决策
 */
void ExpCtaMocker::on_calculate_done(uint32_t curDate, uint32_t curTime)
{
	CtaMocker::on_calculate_done(curDate, curTime);
	getRunner().ctx_on_calc_done(_context_id, curDate, curTime, ET_CTA);

	getRunner().on_schedule_event(curDate, curTime);
}

/**
 * @brief 回测结束回调函数
 * @details 当整个回测过程结束时触发此回调
 *          直接通过getRunner调用外部引擎的on_backtest_end接口，通知外部系统回测结束
 *          与其他函数不同，这里没有调用父类的相关函数，而是直接处理全局的回测结束事件
 *          在CTA策略中，可以在回测结束时生成完整的回测报告、统计和数据分析
 *          这是策略生命周期中的最后一个调用的函数
 */
void ExpCtaMocker::on_bactest_end()
{
	getRunner().on_backtest_end();
}

/**
 * @brief 条件触发回调函数
 * @param stdCode 标准化合约代码
 * @param target 目标价格或条件值
 * @param price 当前实际价格
 * @param usertag 用户自定义标签，用于识别不同的触发条件
 * @details 当设定的条件被触发时触发此回调
 *          直接通过getRunner调用外部引擎的ctx_on_cond_triggered接口，将条件触发事件通知给上下文
 *          这里没有调用父类的相关函数，将条件触发通知直接传递给外部引擎
 *          在CTA策略中，条件触发通常用于止盈、止损或特定价格条件的交易信号
 *          根据usertag可以实现不同类型的条件触发处理
 */
void ExpCtaMocker::on_condition_triggered(const char* stdCode, double target, double price, const char* usertag)
{
	getRunner().ctx_on_cond_triggered(_context_id, stdCode, target, price, usertag, ET_CTA);
}