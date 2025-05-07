/*!
 * \file IndexFactory.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 指数工厂定义
 * \details 该文件定义了指数计算的工厂类IndexFactory，负责管理多个指数计算器，并协调它们处理实时行情数据
 */
#pragma once
#include "IndexWorker.h"
#include "../Share/threadpool.hpp"
#include <vector>

class DataManager;

/**
 * @brief 指数工厂类
 * @details 管理多个指数计算器（IndexWorker），并协调它们处理实时行情数据
 * 支持用线程池并行处理多个指数的计算，提高处理效率
 */
class IndexFactory
{
public:
	/**
	 * @brief 构造函数
	 * @details 初始化指数工厂对象，将热点合约管理器和基础数据管理器指针初始化为NULL
	 */
	IndexFactory():_hot_mgr(NULL), _bd_mgr(NULL){}

public:
	/**
	 * @brief 初始化指数工厂
	 * @details 根据配置初始化指数工厂，包括创建线程池和所有指数计算器
	 * @param config 配置项，包含线程池大小和指数配置
	 * @param hotMgr 热点合约管理器指针
	 * @param bdMgr 基础数据管理器指针
	 * @param dataMgr 数据管理器指针
	 * @return bool 初始化是否成功
	 */
	bool	init(WTSVariant* config, IHotMgr* hotMgr, IBaseDataMgr* bdMgr, DataManager* dataMgr);
	/**
	 * @brief 处理行情数据
	 * @details 接收新的Tick数据，并将其分发给所有关注该合约的指数计算器进行处理
	 * @param newTick 新的Tick数据指针
	 */
	void	handle_quote(WTSTickData* newTick);

public:
	/**
	 * @brief 获取热点合约管理器
	 * @return IHotMgr* 热点合约管理器指针
	 */
	inline IHotMgr*			get_hot_mgr() { return _hot_mgr; }

	/**
	 * @brief 获取基础数据管理器
	 * @return IBaseDataMgr* 基础数据管理器指针
	 */
	inline IBaseDataMgr*	get_bd_mgr() { return _bd_mgr; }

	/**
	 * @brief 订阅合约的Tick数据
	 * @details 将指定合约代码添加到订阅列表中，并返回当前最新的Tick数据
	 * @param fullCode 合约完整代码，格式为“交易所.代码”
	 * @return WTSTickData* 当前合约的最新Tick数据指针，如果没有则返回NULL
	 */
	WTSTickData*	sub_ticks(const char* fullCode);

	/**
	 * @brief 推送Tick数据
	 * @details 将新的Tick数据推送到数据管理器进行存储
	 * @param newTick 新的Tick数据指针
	 */
	void			push_tick(WTSTickData* newTick);

private:
	/// @brief 指数计算器列表类型定义
	typedef std::vector<IndexWorkerPtr>	IndexWorkers;

	/// @brief 指数计算器列表，存储所有指数计算器实例
	IndexWorkers	_workers;

	/// @brief 热点合约管理器指针，用于获取热点合约信息
	IHotMgr*		_hot_mgr;

	/// @brief 基础数据管理器指针，用于获取基础数据信息
	IBaseDataMgr*	_bd_mgr;

	/// @brief 数据管理器指针，用于存储和读取数据
	DataManager*	_data_mgr;

	/// @brief 线程池指针类型定义
	typedef std::shared_ptr<boost::threadpool::pool> ThreadPoolPtr;

	/// @brief 线程池指针，用于并行处理指数计算任务
	ThreadPoolPtr	_pool;

	/// @brief 已订阅合约代码集合，存储所有已订阅的合约完整代码
	wt_hashset<std::string>	_subbed;
};

