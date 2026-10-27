/*!
* \file WtSelTicker.h
* \project	WonderTrader
*
* \author Wesley
* \date 2020/03/30
*
* \brief 选股策略实时行情触发器
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

class WtSelEngine;

/**
 * @brief 选股策略实时行情触发器
 * @details 负责接收实时行情数据，进行时间检测和触发选股策略引擎的相关事件。
 * 支持分钟结束、交易时段结束等事件的检测与触发。
 */
class WtSelRtTicker
{
public:
	/**
	 * @brief 构造函数
	 * @param engine 选股策略引擎指针
	 */
	WtSelRtTicker(WtSelEngine* engine);

	/**
	 * @brief 析构函数
	 */
	~WtSelRtTicker();

public:
	/**
	 * @brief 初始化触发器
	 * @param store 数据读取器指针，用于数据持久化
	 * @param sessionID 交易时段ID，用于获取对应的交易时段信息
	 * @details 初始化实时行情触发器，设置数据存储和交易时段信息，获取当前的日期和时间
	 */
	void	init(IDataReader* store, const char* sessionID);

	/**
	 * @brief 处理实时行情数据
	 * @param curTick 当前行情数据
	 * @param hotFlag 主力合约标记，默认为0
	 * @details 处理新到的行情数据，根据时间检查是否需要触发分钟结束、交易时段结束等事件
	 */
	void	on_tick(WTSTickData* curTick, uint32_t hotFlag = 0);

	/**
	 * @brief 启动触发器
	 * @details 创建并启动一个实时监控线程，监控时间变化并触发分钟结束等事件
	 */
	void	run();

	/**
	 * @brief 停止触发器
	 * @details 停止监控线程并等待其结束
	 */
	void	stop();

private:
	/**
	 * @brief 触发行情处理
	 * @param curTick 当前行情数据
	 * @param hotFlag 主力合约标记，默认为0
	 * @details 将行情数据转发给选股策略引擎，并处理主力合约的行情
	 */
	void	trigger_price(WTSTickData* curTick, uint32_t hotFlag = 0);

private:
	WTSSessionInfo*	_s_info;     ///< 交易时段信息
	WtSelEngine*	_engine;        ///< 选股策略引擎指针
	IDataReader*		_store;      ///< 数据读取器指针

	uint32_t	_date;              ///< 当前日期
	uint32_t	_time;              ///< 当前时间

	uint32_t	_cur_pos;           ///< 当前分钟位置

	StdUniqueMutex	_mtx;         ///< 互斥锁，用于线程同步
	std::atomic<uint64_t>	_next_check_time;  ///< 下次检查时间，原子类型
	std::atomic<uint32_t>	_last_emit_pos;    ///< 最后触发的分钟位置，原子类型

	bool			_stopped;           ///< 触发器是否已停止
	StdThreadPtr	_thrd;           ///< 监控线程指针
};

NS_WTP_END