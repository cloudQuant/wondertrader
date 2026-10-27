/*!
 * \file WtHftTicker.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 高频交易行情定时器头文件
 * \details 定义了高频交易行情定时器类，用于处理高频交易的行情数据和时间触发
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

class WtHftEngine;

/**
 * @brief 高频交易实时行情定时器类
 * @details 负责处理高频交易的实时行情数据，包括行情处理、分钟线间隔触发等
 */
class WtHftRtTicker
{
public:
	/**
	 * @brief 构造函数
	 * @param engine 高频交易引擎指针
	 */
	WtHftRtTicker(WtHftEngine* engine);

	/**
	 * @brief 析构函数
	 */
	~WtHftRtTicker();

public:
	/**
	 * @brief 初始化定时器
	 * @param store 数据读取器指针
	 * @param sessionID 交易时段ID
	 */
	void	init(IDataReader* store, const char* sessionID);

	/**
	 * @brief 处理实时行情数据
	 * @param curTick 当前行情数据指针
	 */
	void	on_tick(WTSTickData* curTick);

	/**
	 * @brief 启动定时器
	 * @details 创建并启动定时器线程，处理定时事件
	 */
	void	run();

	/**
	 * @brief 停止定时器
	 * @details 停止定时器线程并等待其结束
	 */
	void	stop();

private:
	/**
	 * @brief 触发行情处理
	 * @param curTick 当前行情数据指针
	 * @details 将行情数据传递给高频交易引擎处理
	 */
	void	trigger_price(WTSTickData* curTick);

private:
	/**
	 * @brief 交易时段信息指针
	 */
	WTSSessionInfo*	_s_info;

	/**
	 * @brief 高频交易引擎指针
	 */
	WtHftEngine*	_engine;

	/**
	 * @brief 数据读取器指针
	 */
	IDataReader*		_store;

	/**
	 * @brief 当前日期，格式YYYYMMDD
	 */
	uint32_t	_date;

	/**
	 * @brief 当前时间，格式HHMMSSmmm
	 */
	uint32_t	_time;

	/**
	 * @brief 当前分钟位置，以分钟序号表示
	 */
	uint32_t	_cur_pos;

	/**
	 * @brief 互斥锁，用于线程同步
	 */
	StdUniqueMutex	_mtx;

	/**
	 * @brief 下次检查时间，原子变量
	 */
	std::atomic<uint64_t>	_next_check_time;

	/**
	 * @brief 最后触发的分钟位置，原子变量
	 */
	std::atomic<uint32_t>	_last_emit_pos;

	/**
	 * @brief 是否停止标志
	 */
	bool			_stopped;

	/**
	 * @brief 定时器线程指针
	 */
	StdThreadPtr	_thrd;
};

NS_WTP_END