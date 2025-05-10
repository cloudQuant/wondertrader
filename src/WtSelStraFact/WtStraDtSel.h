/**
 * @file WtStraDtSel.h
 * @brief 双突破(Dual Thrust)选股策略头文件
 * 
 * @details 该文件定义了基于双突破算法的选股策略类，用于实现自动化选股交易
 */

#pragma once
#include "../Includes/SelStrategyDefs.h"

#include <unordered_set>

USING_NS_WTP;

/**
 * @brief 双突破(Dual Thrust)选股策略类
 * 
 * @details 实现了基于双突破算法的选股策略，该算法通过设定上下边界，
 *          当价格突破上边界时做多，突破下边界时做空
 */
class WtStraDtSel : public SelStrategy
{
public:
	/**
	 * @brief 构造函数
	 * @param id 策略ID
	 */
	WtStraDtSel(const char* id);

	/**
	 * @brief 析构函数
	 */
	~WtStraDtSel();

public:
	/**
	 * @brief 获取策略名称
	 * @return 策略名称
	 */
	virtual const char* getName() override;

	/**
	 * @brief 获取策略所属工厂名称
	 * @return 工厂名称
	 */
	virtual const char* getFactName() override;

	/**
	 * @brief 初始化策略
	 * @param cfg 策略配置
	 * @return 初始化是否成功
	 */
	virtual bool init(WTSVariant* cfg) override;

	/**
	 * @brief 策略初始化回调
	 * @param ctx 策略上下文
	 */
	virtual void on_init(ISelStraCtx* ctx) override;

	/**
	 * @brief 定时调度回调
	 * @param ctx 策略上下文
	 * @param uDate 日期，YYYYMMDD格式
	 * @param uTime 时间，HHMMSS格式
	 */
	virtual void on_schedule(ISelStraCtx* ctx, uint32_t uDate, uint32_t uTime) override;

	/**
	 * @brief 行情数据Tick回调
	 * @param ctx 策略上下文
	 * @param stdCode 标准化合约代码
	 * @param newTick 最新的Tick数据
	 */
	virtual void on_tick(ISelStraCtx* ctx, const char* stdCode, WTSTickData* newTick) override;

	/**
	 * @brief K线数据回调
	 * @param ctx 策略上下文
	 * @param stdCode 标准化合约代码
	 * @param period 周期标识
	 * @param newBar 最新的K线数据
	 */
	virtual void on_bar(ISelStraCtx* ctx, const char* stdCode, const char* period, WTSBarStruct* newBar) override;

private:
	/// @brief 上边界系数
	double		_k1;
	/// @brief 下边界系数
	double		_k2;
	/// @brief 计算区间天数
	uint32_t	_days;

	/// @brief 数据周期
	std::string _period;
	/// @brief 加载K线条数
	uint32_t	_count;

	/// @brief 是否为股票
	bool		_isstk;

	/// @brief 合约代码集合
	std::unordered_set<std::string> _codes;

};

