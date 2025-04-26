/*!
 * \file WtCtaTicker.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 * 
 * WtCtaTicker类的头文件，定义了CTA策略引擎的定时器
 * 负责策略的定时触发和实时行情处理
 * 是CTA策略引擎的核心组件之一
 */
#pragma once
#include <stdint.h>
#include <atomic>

#include "../Includes/WTSMarcos.h"
#include "../Share/StdUtils.hpp"

NS_WTP_BEGIN
class WTSSessionInfo;
class IDataReader;
class WTSTickData;

class WtCtaEngine;
//////////////////////////////////////////////////////////////////////////
/**
 * @brief CTA策略定时器类
 * @details 负责CTA策略的定时触发和实时行情处理，是策略引擎的核心组件之一
 */
class WtCtaRtTicker
{
public:
	/**
	 * @brief 构造函数
	 * @details 初始化定时器对象，设置引擎指针和初始状态
	 * 
	 * @param engine CTA策略引擎指针
	 */
	WtCtaRtTicker(WtCtaEngine* engine) 
		: _engine(engine)      // 策略引擎指针
		, _stopped(false)      // 停止标志，初始化为未停止
		, _date(0)             // 当前日期，初始化为0
		, _time(UINT_MAX)      // 当前时间，初始化为最大值
		, _next_check_time(0)  // 下次检查时间，初始化为0
		, _last_emit_pos(0)    // 上次发布位置，初始化为0
		, _cur_pos(0){}        // 当前位置，初始化为0
	
	/**
	 * @brief 析构函数
	 * @details 清理定时器对象的资源
	 */
	~WtCtaRtTicker(){}

public:
	/**
	 * @brief 初始化定时器
	 * @details 设置数据读取器和交易时间段ID，用于获取交易时间信息
	 * 
	 * @param store 数据读取器指针
	 * @param sessionID 交易时间段ID
	 */
	void	init(IDataReader* store, const char* sessionID);
	
	//void	set_time(uint32_t uDate, uint32_t uTime);
	
	/**
	 * @brief 处理实时行情
	 * @details 处理新收到的实时行情数据，触发相应的策略计算
	 * 
	 * @param curTick 当前实时行情数据
	 */
	void	on_tick(WTSTickData* curTick);

	/**
	 * @brief 启动定时器
	 * @details 启动定时器线程，开始定时触发策略计算
	 */
	void	run();
	
	/**
	 * @brief 停止定时器
	 * @details 停止定时器线程，终止定时触发
	 */
	void	stop();

	/**
	 * @brief 检查是否在交易时间内
	 * @details 根据当前时间检查是否在交易时间段内
	 * 
	 * @return true 在交易时间内
	 * @return false 不在交易时间内
	 */
	bool		is_in_trading() const;
	
	/**
	 * @brief 将时间转换为分钟数
	 * @details 将HHMMSS格式的时间转换为从当日开始的分钟数
	 * 
	 * @param uTime 时间，格式为HHMMSS
	 * @return uint32_t 分钟数
	 */
	uint32_t	time_to_mins(uint32_t uTime) const;

private:
	/**
	 * @brief 触发价格事件
	 * @details 根据实时行情触发策略的价格事件处理
	 * 
	 * @param curTick 当前实时行情数据
	 */
	void	trigger_price(WTSTickData* curTick);

private:
	WTSSessionInfo*	_s_info;    ///< 交易时间段信息对象
	WtCtaEngine*	_engine;    ///< CTA策略引擎指针
	IDataReader*	_store;     ///< 数据读取器指针

	uint32_t	_date;      ///< 当前日期，格式为YYYYMMDD
	uint32_t	_time;      ///< 当前时间，格式为HHMMSS

	uint32_t	_cur_pos;   ///< 当前定时器位置

	StdUniqueMutex	_mtx;       ///< 互斥锁，用于线程同步
	std::atomic<uint64_t>	_next_check_time;  ///< 下次检查时间，原子变量
	std::atomic<uint32_t>	_last_emit_pos;    ///< 上次发布位置，原子变量

	bool			_stopped;   ///< 停止标志，用于控制定时器线程
	StdThreadPtr	_thrd;      ///< 定时器线程指针

};
NS_WTP_END