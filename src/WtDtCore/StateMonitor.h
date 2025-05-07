/*!
 * \file StateMonitor.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 交易状态监控器定义
 * \details 该文件定义了交易状态监控器，用于跟踪和管理不同交易时间段的状态
 *          包括初始化、交易中、休息、收盘等状态，并根据时间自动处理状态转换
 *          监控器通过后台线程定期检查各个交易时间段的状态，执行相应的操作
 */
#pragma once
#include <vector>
#include "../Share/StdUtils.hpp"
#include "../Includes/FasterDefs.h"
#include "../Includes/WTSMarcos.h"

NS_WTP_BEGIN
class WTSSessionInfo;
NS_WTP_END

USING_NS_WTP;

/**
 * @brief 交易状态枚举
 * @details 定义交易时间段的各种状态，用于跟踪交易时间段当前的运行状态
 */
typedef enum tagSimpleState
{
	SS_ORIGINAL,		///< 未初始化状态，初始状态
	SS_INITIALIZED,		///< 已初始化状态，数据准备就绪
	SS_RECEIVING,		///< 交易中状态，正在接收数据
	SS_PAUSED,			///< 休息中状态，交易暂停
	SS_CLOSED,			///< 已收盘状态，交易结束
	SS_PROCING,			///< 收盘作业中状态，正在处理收盘数据
	SS_PROCED,			///< 盘后已处理状态，收盘作业完成
	SS_Holiday	= 99	///< 节假日状态，不进行交易
} SimpleState;

/**
 * @brief 交易状态信息结构体
 * @details 用于存储单个交易时间段的相关信息，包括其ID、各种关键时间点、当前状态及交易时间段设置
 */
typedef struct _StateInfo
{
	char		_session[16];     ///< 交易时间段ID
	uint32_t	_init_time;     ///< 初始化时间，格式HHMM，该时间之后才开始接收数据
	uint32_t	_close_time;    ///< 收盘时间，格式HHMM，该时间之后不再接收数据
	uint32_t	_proc_time;     ///< 盘后处理时间，格式HHMM，该时间进行收盘数据处理
	SimpleState	_state;       ///< 当前交易状态
	WTSSessionInfo*	_sInfo;  ///< 交易时间段信息指针

	/**
	 * @brief 交易时间区间结构体
	 * @details 定义交易时间段内的具体交易时间区间，一个交易时间段可能包含多个交易时间区间
	 */
	typedef struct _Section
	{
		uint32_t _from;  ///< 区间开始时间，格式HHMM
		uint32_t _end;   ///< 区间结束时间，格式HHMM
	} Section;
	std::vector<Section> _sections;  ///< 当前交易时间段的所有交易时间区间集合

	/**
	 * @brief 检查给定时间是否在交易时间区间内
	 * @param curTime 当前时间，格式HHMM
	 * @return 如果当前时间在任一交易时间区间内返回true，否则返回false
	 */
	inline bool isInSections(uint32_t curTime)
	{
		// 遍历所有交易时间区间检查是否包含当前时间
		for (auto it = _sections.begin(); it != _sections.end(); it++)
		{
			const Section& sec = *it;
			// 如果当前时间在区间[开始时间, 结束时间)内
			if (sec._from <= curTime && curTime < sec._end)
				return true;
		}
		return false;
	}

	/**
	 * @brief 结构体构造函数
	 * @details 初始化交易状态信息各成员为默认值
	 */
	_StateInfo()
	{
		_session[0] = '\0';     // 初始化交易时间段ID为空字符串
		_init_time = 0;         // 初始化时间设置为0
		_close_time = 0;        // 收盘时间设置为0
		_proc_time = 0;         // 盘后处理时间设置为0
		_state = SS_ORIGINAL;    // 初始状态设置为未初始化
		_sInfo = nullptr;       // 交易时间段信息指针设置为空
	}
} StateInfo;

/**
 * @brief 交易状态信息的智能指针类型
 * @details 使用智能指针管理StateInfo对象，避免内存泄漏
 */
typedef std::shared_ptr<StateInfo> StatePtr;

/**
 * @brief 交易状态信息映射表类型
 * @details 以交易时间段ID为键，存储对应的交易状态信息
 */
typedef wtp::wt_hashmap<std::string, StatePtr>	StateMap;

// 前向声明
/**
 * @brief 基础数据管理器类前向声明
 */
class WTSBaseDataMgr;

/**
 * @brief 数据管理器类前向声明
 */
class DataManager;

/**
 * @brief 交易状态监控器类
 * @details 用于监控和管理不同交易时间段的状态变化
 *          定期检查交易时间段并根据当前时间自动处理状态转换
 *          提供查询方法检查特定状态的交易时间段
 */
class StateMonitor
{
public:
	/**
	 * @brief 构造函数
	 * @details 初始化监控器并设置默认参数
	 */
	StateMonitor();
	
	/**
	 * @brief 析构函数
	 * @details 清理监控器资源
	 */
	~StateMonitor();

public:
	/**
	 * @brief 初始化交易状态监控器
	 * @param filename 配置文件路径，包含交易时间段的定义和状态设置
	 * @param bdMgr 基础数据管理器指针，提供交易时间段信息
	 * @param dtMgr 数据管理器指针，用于处理盘后数据
	 * @return 初始化是否成功
	 */
	bool initialize(const char* filename, WTSBaseDataMgr* bdMgr, DataManager* dtMgr);
	
	/**
	 * @brief 启动交易状态监控器
	 * @details 创建并运行一个后台线程，定期检查各交易时间段的状态并处理状态转换
	 */
	void run();
	
	/**
	 * @brief 停止交易状态监控器
	 * @details 设置停止标志并等待监控线程结束
	 */
	void stop();

	/**
	 * @brief 检查是否有任意交易时间段处于指定状态
	 * @param ss 要检查的状态
	 * @return 如果至少有一个交易时间段处于指定状态则返回true，否则返回false
	 */
	inline bool	isAnyInState(SimpleState ss) const
	{
		// 遍历所有交易时间段
		auto it = _map.begin();
		for (; it != _map.end(); it++)
		{
			const StatePtr& sInfo = it->second;
			// 如果某个交易时间段的当前状态与目标状态相同
			if (sInfo->_state == ss)
				return true;
		}

		return false;
	}

	/**
	 * @brief 检查是否所有非节假日交易时间段都处于指定状态
	 * @param ss 要检查的状态
	 * @return 如果所有非节假日交易时间段都处于指定状态则返回true，否则返回false
	 */
	inline bool	isAllInState(SimpleState ss) const
	{
		// 遍历所有交易时间段
		auto it = _map.begin();
		for (; it != _map.end(); it++)
		{
			const StatePtr& sInfo = it->second;
			// 忽略节假日状态的交易时间段
			// 如果某个非节假日交易时间段不处于指定状态，则返回false
			if (sInfo->_state != SS_Holiday && sInfo->_state != ss)
				return false;
		}

		return true;
	}

	/**
	 * @brief 检查指定交易时间段是否处于特定状态
	 * @param sid 交易时间段ID，用于标识特定的交易时间段
	 * @param ss 要检查的状态
	 * @return 如果指定交易时间段存在且处于指定状态，则返回true，否则返回false
	 */
	inline bool	isInState(const char* sid, SimpleState ss) const
	{
		// 在映射表中查找指定交易时间段
		auto it = _map.find(sid);
		// 如果找不到该交易时间段，返回false
		if (it == _map.end())
			return false;

		// 检查交易时间段的当前状态是否与目标状态相同
		const StatePtr& sInfo = it->second;
		return sInfo->_state == ss;
	}

private:
	StateMap		_map;        ///< 交易状态信息映射表，以交易时间段ID为键存储所有交易时间段的状态信息
	WTSBaseDataMgr*	_bd_mgr;    ///< 基础数据管理器指针，提供交易时间段信息和交易日历
	DataManager*	_dt_mgr;    ///< 数据管理器指针，用于处理盘后数据转换

	StdThreadPtr	_thrd;      ///< 监控线程智能指针，用于定期检查状态

	bool			_stopped;    ///< 停止标志，用于控制监控线程的结束
};
