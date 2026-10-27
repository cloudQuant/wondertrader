/*!
 * \file WtStraDualThrust.h
 * \brief DualThrust策略定义文件
 * 
 * DualThrust是一种经典的突破交易策略，通过计算上下轨道来确定入场和出场信号
 * 本文件定义了DualThrust策略的类和相关接口
 * 
 * \author Wesley
 */

#pragma once
#include "../Includes/CtaStrategyDefs.h"

/**
 * \brief DualThrust策略类
 * 
 * 基于CtaStrategy的DualThrust策略实现
 * DualThrust是一种经典的突破交易策略，通过计算上下轨道来确定入场和出场信号
 * 该策略使用开盘价加上一段时间内的波动区间乘以系数来计算上下轨道
 */
class WtStraDualThrust : public CtaStrategy
{
public:
	/**
	 * \brief 构造函数
	 * 
	 * \param id 策略ID，用于在策略工厂中标识该策略实例
	 */
	WtStraDualThrust(const char* id);

	/**
	 * \brief 析构函数
	 */
	virtual ~WtStraDualThrust();

public:
	/**
	 * \brief 获取策略工厂名称
	 * 
	 * \return 策略工厂名称
	 */
	virtual const char* getFactName() override;

	/**
	 * \brief 获取策略名称
	 * 
	 * \return 策略名称，返回"DualThrust"
	 */
	virtual const char* getName() override;

	/**
	 * \brief 策略初始化
	 * 
	 * 根据配置初始化策略参数
	 * 
	 * \param cfg 策略配置，包含策略所需的各种参数
	 * \return 初始化是否成功
	 */
	virtual bool init(WTSVariant* cfg) override;

	/**
	 * \brief 定时调度回调
	 * 
	 * 在策略定时器触发时调用，用于执行交易信号计算和交易操作
	 * 
	 * \param ctx 策略上下文
	 * \param curDate 当前日期，格式为YYYYMMDD
	 * \param curTime 当前时间，格式为HHMMSS或HHMMSS000
	 */
	virtual void on_schedule(ICtaStraCtx* ctx, uint32_t curDate, uint32_t curTime) override;

	/**
	 * \brief 策略初始化回调
	 * 
	 * 在策略实例创建后立即调用，用于订阅行情和初始化指标
	 * 
	 * \param ctx 策略上下文
	 */
	virtual void on_init(ICtaStraCtx* ctx) override;

	/**
	 * \brief Tick数据回调
	 * 
	 * 当有新的Tick数据到来时调用
	 * 
	 * \param ctx 策略上下文
	 * \param stdCode 合约代码
	 * \param newTick 新的Tick数据
	 */
	virtual void on_tick(ICtaStraCtx* ctx, const char* stdCode, WTSTickData* newTick) override;

	/**
	 * \brief 交易日开始回调
	 * 
	 * 在每个交易日开始时调用，用于处理主力合约换月等操作
	 * 
	 * \param ctx 策略上下文
	 * \param uTDate 交易日期，格式为YYYYMMDD
	 */
	virtual void on_session_begin(ICtaStraCtx* ctx, uint32_t uTDate) override;

private:
	//指标参数
	/**
	 * \brief 上轨道系数
	 * 
	 * 用于计算上轨道的乘数因子
	 */
	double		_k1;

	/**
	 * \brief 下轨道系数
	 * 
	 * 用于计算下轨道的乘数因子
	 */
	double		_k2;

	/**
	 * \brief 回看天数
	 * 
	 * 计算波动区间时使用的历史数据天数
	 */
	uint32_t	_days;

	/**
	 * \brief 当前交易的主力合约代码
	 * 
	 * 用于跟踪当前正在交易的主力合约，主要用于处理换月
	 */
	std::string _moncode;

	//数据周期
	/**
	 * \brief K线周期
	 * 
	 * 策略使用的K线周期，如"m1"/"m5"/"d1"等
	 */
	std::string _period;

	/**
	 * \brief 计算所需的K线条数
	 * 
	 * 策略计算所需的历史K线数量
	 */
	uint32_t	_count;

	//合约代码
	/**
	 * \brief 策略交易的合约代码
	 * 
	 * 可以是标准合约代码或者主力合约标记
	 */
	std::string _code;

	/**
	 * \brief 是否为股票标的
	 * 
	 * true表示交易股票，则交易单位为100股，并且不做空头
	 */
	bool		_isstk;
};

