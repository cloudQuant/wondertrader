/*!
 * \file WtSimpRiskMon.h
 *
 * \author Wesley
 * \date 2020/03/30
 *
 * \brief WonderTrader简单风险监控器头文件
 * \details 该文件定义了简单风险监控器WtSimpleRiskMon类，用于实现基础的交易风险控制
 *          包括日内和多日回撤控制、资金监控等功能
 */
#pragma once
#include <thread>
#include <memory>

#include "../Includes/RiskMonDefs.h"

USING_NS_WTP;

/**
 * @brief 简单风险监控器类
 * @details 继承自WtRiskMonitor基类，实现基础的交易风险监控功能
 *          支持日内和多日回撤控制检测，通过设置回撤阈值来限制交易
 */
class WtSimpleRiskMon : public WtRiskMonitor
{
public:
	/**
	 * @brief 构造函数
	 * @details 初始化监控器状态，关闭停止标记和限制标记
	 */
	WtSimpleRiskMon() :_stopped(false), _limited(false){}

public:
	/**
	 * @brief 获取风险监控器名称
	 * @return const char* 风险监控器名称
	 */
	virtual const char* getName() override;

	/**
	 * @brief 获取风险监控器工厂名称
	 * @return const char* 工厂名称
	 */
	virtual const char* getFactName() override;

	/**
	 * @brief 初始化风险监控器
	 * @details 从配置中读取风险控制参数，包括计算时间间隔、风险时间跨度、基础盈利率等
	 * @param ctx 投资组合上下文指针
	 * @param cfg 配置项指针
	 */
	virtual void init(WtPortContext* ctx, WTSVariant* cfg) override;

	/**
	 * @brief 启动风险监控器
	 * @details 创建并启动监控线程，定期检测风险状况并触发风控措施
	 */
	virtual void run() override;

	/**
	 * @brief 停止风险监控器
	 * @details 停止监控线程并释放相关资源
	 */
	virtual void stop() override;

private:
	/** @brief 线程指针类型定义 */
	typedef std::shared_ptr<std::thread> ThreadPtr;
	
	/** @brief 监控线程指针 */
	ThreadPtr		_thrd;
	
	/** @brief 监控器是否已停止标记 */
	bool			_stopped;
	
	/** @brief 交易是否已被限制标记 */
	bool			_limited;

	/** @brief 上次风控检查的时间戳 */
	uint64_t		_last_time;

	/** @brief 风控计算时间间隔，单位s */
	uint32_t		_calc_span;
	
	/** @brief 风险回撤比较的时间跨度 */
	uint32_t		_risk_span;
	
	/** @brief 基础盈利率，用于盈利打分计算 */
	double			_basic_ratio;
	
	/** @brief 风险控制系数，用于调整风控敏感度 */
	double			_risk_scale;
	
	/** @brief 日内高点回撤边界，单日回撤的阈值 */
	double			_inner_day_fd;
	
	/** @brief 日内风控是否启用 */
	bool			_inner_day_active;
	
	/** @brief 多日高点回撤边界，跨多日回撤的阈值 */
	double			_multi_day_fd;
	
	/** @brief 多日风控是否启用 */
	bool			_multi_day_active;
	
	/** @brief 基础资金规模，用于计算盈亏率 */
	double			_base_amount;
};