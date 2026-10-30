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
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

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
	 * @details 请求停止并尝试等待；从回调线程调用时会立即返回，不能据此认定停止已完成。
	 *          需要确认完成时使用 request_stop()/wait_stopped()。
	 */
	void	stop();

	/**
	 * @brief 请求停止工作线程；可从策略回调中安全调用
	 */
	void	request_stop();

	/**
	 * @brief 等待请求的停止完成
	 * @return 当前线程是该 ticker 的回调/启动线程时返回 false，否则 join 后返回 true
	 */
	bool	wait_stopped();

private:
	friend class WtUftEngine;
	friend struct UftTickerShutdownTestAccess;
	friend struct UftFirstTickAutoCloseTestAccess;

	/**
	 * @brief 若当前线程正在该 engine 的 ticker 回调中，请求停止并返回 true
	 */
	static bool request_stop_for_current_callback(WtUftEngine* engine);
	uint64_t capture_wake_generation();
	void notify_worker();
	bool wait_for_worker_wakeup(uint64_t generation, std::chrono::milliseconds timeout);

	WTSSessionInfo*	_s_info;			//!< 交易时段信息
	WtUftEngine*	_engine;			//!< UFT引擎指针
	bool			_init_attempted;	//!< 是否已尝试初始化交易时段

	uint32_t	_date;					//!< 当前日期
	uint32_t	_time;					//!< 当前时间

	uint32_t	_cur_pos;				//!< 当前分钟位置

	StdUniqueMutex	_mtx;				//!< 互斥锁
	std::atomic<uint64_t>	_next_check_time;	//!< 下次检查时间
	std::atomic<uint32_t>	_last_emit_pos;	//!< 上次触发位置

	std::atomic<bool>	_stopped;			//!< 停止标志
	StdThreadPtr	_thrd;				//!< 工作线程
	std::atomic<bool>	_worker_started;	//!< 工作线程已启动
	std::mutex		_state_mtx;			//!< run/worker 线程身份和启动状态
	std::condition_variable	_state_cv;	//!< 启动流程结束通知
	std::mutex		_join_mtx;			//!< 串行化多个控制线程的 join
	std::mutex		_callback_mtx;		//!< 排空已经进入的外部 on_tick 回调
	std::thread::id	_run_thread_id;		//!< 同步启动回调所属线程
	std::thread::id	_worker_thread_id;	//!< 后台工作线程
	bool			_run_active;			//!< run 启动回调仍在执行
};

NS_WTP_END
