/*!
 * \file ExpCtaMocker.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief CTA策略回测模拟器对外封装定义
 * \details 定义了对外提供的CTA策略回测模拟器类，用于封装WtBtCore中的CtaMocker，
 *          便于在回测引擎中实现CTA策略的模拟测试。
 *          CTA(Commodity Trading Advisor)策略是一种基于股票、期货等金融工具进行交易的策略类型。
 */
#pragma once
#include "../WtBtCore/CtaMocker.h"

USING_NS_WTP;

/**
 * @brief CTA策略回测模拟器对外封装类
 * @details 继承自CtaMocker，用于在回测系统中模拟CTA策略的行为和表现
 *          重写了父类的各个虚函数，实现对不同事件的响应处理
 *          该类主要用于对外接口封装，方便与其他系统集成
 *          包含处理策略初始化、交易日管理、行情数据更新、计算周期和条件触发等功能
 */
class ExpCtaMocker : public CtaMocker
{
public:
	/**
	 * @brief 构造函数
	 * @param replayer 历史数据回放器指针，用于提供回测所需的历史数据
	 * @param name 策略名称，用于唯一标识策略实例
	 * @param slippage 滑点数量，模拟交易时的滑点损失，默认为0
	 * @param persistData 是否持久化数据，默认为true
	 * @param notifier 事件通知器指针，用于实现事件通知功能，默认为NULL
	 * @param isRatioSlp 是否为百分比滑点，默认为false（固定点数滑点）
	 * @details 初始化CTA策略回测模拟器，通过调用父类构造函数来初始化
	 *          基类 CtaMocker负责大部分策略模拟器的基础功能
	 *          ExpCtaMocker主要添加了对外部接口的调用和处理
	 */
	ExpCtaMocker(HisDataReplayer* replayer, const char* name, int32_t slippage = 0, bool persistData = true, EventNotifier* notifier = NULL, bool isRatioSlp = false);
	
	/**
	 * @brief 析构函数
	 * @details 清理CTA策略回测模拟器资源
	 *          由于ExpCtaMocker对象通常在整个回测过程中只创建一次，
	 *          所以析构函数也只会在回测结束时被调用一次
	 */
	virtual ~ExpCtaMocker();

public:
	/**
	 * @brief 策略初始化回调函数
	 * @details 当策略被创建后需要进行初始化时触发此回调
	 *          完成策略参数设置、数据订阅、变量初始化等操作
	 *          通常在回测开始时被触发一次
	 *          这是策略生命周期中的第一个回调函数
	 */
	virtual void on_init() override;

	/**
	 * @brief 交易日开始回调函数
	 * @param uCurDate 当前交易日日期，格式为YYYYMMDD
	 * @details 每个交易日开始时触发此回调
	 *          可以在此函数中进行交易日开始前的准备工作
	 *          例如重置交易状态、准备每日数据等
	 *          在CTA策略中通常用于处理日间行情数据的准备工作
	 */
	virtual void on_session_begin(uint32_t uCurDate) override;

	/**
	 * @brief 交易日结束回调函数
	 * @param uCurDate 当前交易日日期，格式为YYYYMMDD
	 * @details 每个交易日结束时触发此回调
	 *          可以在此函数中进行交易日结束后的清理工作
	 *          例如结算当日交易、记录统计数据、保存状态等
	 *          在CTA策略中通常用于总结当日表现并为下一交易日做准备
	 */
	virtual void on_session_end(uint32_t uCurDate) override;

	/**
	 * @brief 实时行情数据更新回调函数
	 * @param stdCode 标准化合约代码
	 * @param newTick 新的Tick行情数据
	 * @details 当收到新的Tick数据时触发此回调
	 *          在CTA策略中，此函数用于处理实时行情并更新策略状态
	 *          可以在收到新券行情时判断是否满足交易条件并发出交易信号
	 *          Tick数据包含最新成交价、买卖相关信息等
	 */
	virtual void on_tick_updated(const char* stdCode, WTSTickData* newTick) override;

	/**
	 * @brief K线周期结束回调函数
	 * @param stdCode 标准化合约代码
	 * @param period K线周期，如"m1"、"d1"等
	 * @param newBar 新的K线数据
	 * @details 当收到新的K线数据时触发此回调
	 *          在CTA策略中，此函数是策略计算和决策的主要入口
	 *          通常基于K线数据进行技术指标计算、模型验证并生成交易信号
	 *          CTA策略主要依靠K线数据进行分析决策
	 */
	virtual void on_bar_close(const char* stdCode, const char* period, WTSBarStruct* newBar) override;

	/**
	 * @brief 策略计算回调函数
	 * @param curDate 当前日期，格式为YYYYMMDD
	 * @param curTime 当前时间，格式为HHMMSS
	 * @details 在回测引擎进行定时计算时触发此回调
	 *          用于定时执行策略计算逻辑，不依赖于特定K线周期
	 *          可以在这里实现基于时间的策略逻辑，如定时再平衡、定时信号生成等
	 *          在CTA策略中用于实现非行情驱动的算法逻辑
	 */
	virtual void on_calculate(uint32_t curDate, uint32_t curTime) override;

	/**
	 * @brief 策略计算完成回调函数
	 * @param curDate 当前日期，格式为YYYYMMDD
	 * @param curTime 当前时间，格式为HHMMSS
	 * @details 在每个计算周期的策略计算完成后触发此回调
	 *          所有定时计算以及事件驱动的计算完成后都会触发此回调
	 *          通常用于在全部计算完成后进行一些汇总工作或执行交易决策
	 *          在CTA策略中用于确保所有分析都完成后再执行交易操作
	 */
	virtual void on_calculate_done(uint32_t curDate, uint32_t curTime) override;

	/**
	 * @brief 回测结束回调函数
	 * @details 当整个回测过程结束时触发此回调
	 *          用于处理回测结束时的事件，如数据汇总、结果输出、资源释放等
	 *          在CTA策略中通常用于生成最终的策略报告和统计分析
	 *          这是策略生命周期中的最后一个回调函数
	 */
	virtual void on_bactest_end() override;

	/**
	 * @brief 条件触发回调函数
	 * @param stdCode 标准化合约代码
	 * @param target 目标价格或条件值
	 * @param price 当前实际价格
	 * @param usertag 用户自定义标签，用于识别不同的触发条件
	 * @details 当设定的条件被触发时调用此函数
	 *          在CTA策略中通常用于收到条件单触发事件，如止损、止盈等
	 *          可以基于不同的usertag实现不同类型的条件触发处理
	 *          对于风险管理和复杂交易策略非常重要
	 */
	virtual void on_condition_triggered(const char* stdCode, double target, double price, const char* usertag) override;
};

