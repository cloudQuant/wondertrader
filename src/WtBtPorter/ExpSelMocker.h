/*!
 * \file ExpSelMocker.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 选股策略回测模拟器对外封装定义
 * \details 定义了对外提供的选股策略回测模拟器类，用于封装WtBtCore中的SelMocker，
 *          便于在回测引擎中实现选股策略的模拟测试。
 */
#pragma once
#include "../WtBtCore/SelMocker.h"

/**
 * @brief 选股策略回测模拟器对外封装类
 * @details 继承自SelMocker，用于在回测系统中模拟选股策略的行为和表现
 *          重写了父类的各个虚函数，实现对不同事件的响应处理
 *          该类主要用于对外接口封装，方便与其他系统集成
 */
class ExpSelMocker : public SelMocker
{
public:
	/**
	 * @brief 构造函数
	 * @param replayer 历史数据回放器指针
	 * @param name 策略名称
	 * @param slippage 滑点设置，默认为0
	 * @param isRatioSlp 是否为百分比滑点，默认为false
	 * @details 创建选股策略回测模拟器实例，并传入相关参数
	 */
	ExpSelMocker(HisDataReplayer* replayer, const char* name, int32_t slippage = 0, bool isRatioSlp = false);
	
	/**
	 * @brief 析构函数
	 * @details 清理选股策略回测模拟器资源
	 */
	virtual ~ExpSelMocker();

public:
	/**
	 * @brief 策略初始化回调
	 * @details 在回测引擎初始化策略时调用，用于实现选股策略的初始化操作
	 */
	virtual void on_init() override;

	/**
	 * @brief 交易日开始回调
	 * @param uDate 交易日期，格式为YYYYMMDD
	 * @details 在每个交易日开始时调用，用于处理交易日开始时的特定业务
	 */
	virtual void on_session_begin(uint32_t uDate) override;

	/**
	 * @brief 交易日结束回调
	 * @param uDate 交易日期，格式为YYYYMMDD
	 * @details 在每个交易日结束时调用，用于处理交易日结束时的收盈统计和仓位调整等操作
	 */
	virtual void on_session_end(uint32_t uDate) override;

	/**
	 * @brief 回测结束回调
	 * @details 在回测结束时调用，用于清理资源和生成回测报告
	 */
	virtual void on_bactest_end() override;

	/**
	 * @brief Tick数据更新回调
	 * @param stdCode 标准化合约代码
	 * @param newTick 新的Tick数据
	 * @details 在收到新的Tick数据时调用，用于处理最新的市场行情
	 */
	virtual void on_tick_updated(const char* stdCode, WTSTickData* newTick) override;

	/**
	 * @brief K线周期结束回调
	 * @param stdCode 标准化合约代码
	 * @param period K线周期，如"m1"、"d1"等
	 * @param newBar 新的K线数据
	 * @details 在新的K线周期结束时调用，用于处理K线周期结束时的策略计算和信号生成
	 */
	virtual void on_bar_close(const char* stdCode, const char* period, WTSBarStruct* newBar) override;

	/**
	 * @brief 策略定时调度回调
	 * @param curDate 当前日期，格式为YYYYMMDD
	 * @param curTime 当前时间，格式为HHMMSS或HHMMSS000
	 * @details 在回测引擎按照调度规则调度策略时调用，用于处理定时调度任务
	 */
	virtual void on_strategy_schedule(uint32_t curDate, uint32_t curTime) override;
};

