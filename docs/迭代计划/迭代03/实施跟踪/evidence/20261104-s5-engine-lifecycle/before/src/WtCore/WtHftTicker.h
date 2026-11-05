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
#include <condition_variable>
#include <mutex>
#include <thread>

#include "../Includes/WTSMarcos.h"
#include "../Share/StdUtils.hpp"
#include "TickerDayBoundary.h"

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
	 * 当前实例为单次启动；停止后请创建新实例，不会在 run/init 中重置停止标志。
	 */
	void	run();

	/**
	 * @brief 停止定时器
	 * @details 停止定时器线程并等待其结束
	 * 任意 HFT worker 调用（包括跨 ticker）只发停止请求，不阻塞；owner 后续 stop/析构负责 join。
	 * 禁止从任何 HFT worker 线程或本实例 run 初始化回调内析构；该路径会 fail-stop。
	 */
	void	stop();

private:
	enum class LifecycleState : uint8_t
	{
		Idle,
		Starting,
		Running,
		Joining,
		Joined
	};

	/**
	 * @brief 触发行情处理
	 * @param curTick 当前行情数据指针
	 * @details 将行情数据传递给高频交易引擎处理
	 */
	void	trigger_price(WTSTickData* curTick);

	/**
	 * @brief 处理一笔行情（调用方必须是 _strand 的 owner）
	 */
	void	handle_tick(WTSTickData* curTick);

	/**
	 * @brief worker 的一轮检查（调用方必须是 _strand 的 owner）
	 * @param offTime 启动时的偏移时间
	 * @return 是否处于需要短间隔轮询的状态
	 */
	bool	worker_pass(uint32_t offTime);

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
	std::atomic<uint32_t>	_date;

	/**
	 * @brief 当前时间，格式HHMMSSmmm
	 */
	std::atomic<uint32_t>	_time;

	/**
	 * @brief 当前分钟位置，以分钟序号表示
	 */
	uint32_t	_cur_pos;

	/**
	 * @brief 互斥锁，用于线程同步
	 */
	wt_ticker::EventStrand	_strand;	///< 卡9 9d=A：on_tick 与 worker 的串行化/重入排队

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
	std::atomic<bool>	_stopped;

	/**
	 * @brief 活跃交易时段循环计数，饱和到2供线程测试确定观察下一轮判时
	 */
	std::atomic<uint8_t>	_in_session_pass_count;

	/**
	 * @brief worker 生命周期同步；锁保护 run 初始化和 join owner 选举
	 * @details join owner 在锁外 join；并发 stop 调用等待 Joined 后返回
	 */
	std::mutex		_lifecycle_mtx;
	std::condition_variable	_lifecycle_cv;
	LifecycleState	_lifecycle_state;
	std::thread::id	_run_owner_id;
	uint32_t		_join_waiter_count;

	/**
	 * @brief 定时器线程指针
	 */
	StdThreadPtr	_thrd;
};

NS_WTP_END
