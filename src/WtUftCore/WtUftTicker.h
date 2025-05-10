/*!
 * \file WtUftTicker.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief UFT引擎的实时时间控制器头文件
 * \details 定义了WtUftRtTicker类，负责处理超高频交易引擎的实时时间控制和行情数据处理
 */
#pragma once

#include <stdint.h>
#include <atomic>

#include "../Includes/WTSMarcos.h"
#include "../Share/StdUtils.hpp"

NS_WTP_BEGIN
class WTSSessionInfo;
class WTSTickData;

class WtUftEngine;

/**
 * @brief UFT引擎的实时时间控制器
 * @details 用于管理超高频交易引擎的时间控制和数据处理。
 *          负责处理Tick数据、触发分钟线结束事件、计算交易日切换和管理交易时段会话。
 */
class WtUftRtTicker
{
public:
	/**
	 * @brief 构造函数
	 * @param engine UFT引擎指针
	 */
	WtUftRtTicker(WtUftEngine* engine);

	/**
	 * @brief 析构函数
	 */
	~WtUftRtTicker();

public:
	/**
	 * @brief 初始化时间控制器
	 * @param sessionID 交易时段ID
	 */
	void	init(const char* sessionID);

	/**
	 * @brief 处理Tick数据
	 * @param curTick 当前Tick数据指针
	 */
	void	on_tick(WTSTickData* curTick);

	/**
	 * @brief 启动时间控制器
	 */
	void	run();

	/**
	 * @brief 停止时间控制器
	 */
	void	stop();

private:
	WTSSessionInfo*	_s_info;			//!< 交易时段信息
	WtUftEngine*	_engine;			//!< UFT引擎指针

	uint32_t	_date;					//!< 当前日期
	uint32_t	_time;					//!< 当前时间

	uint32_t	_cur_pos;				//!< 当前分钟位置

	StdUniqueMutex	_mtx;				//!< 互斥锁
	std::atomic<uint64_t>	_next_check_time;	//!< 下次检查时间
	std::atomic<uint32_t>	_last_emit_pos;	//!< 上次触发位置

	bool			_stopped;			//!< 停止标志
	StdThreadPtr	_thrd;				//!< 工作线程
};

NS_WTP_END