/*!
 * \file ExpExecuter.cpp
 * \project WonderTrader
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 执行器导出模块实现
 * \details 实现了执行器的导出接口，用于将内部执行器接口暴露给外部模块
 */

#include "ExpExecuter.h"
#include "WtRtRunner.h"

/**
 * @brief 获取运行器实例的外部函数声明
 * @return WtRtRunner& 运行器实例的引用
 */
extern WtRtRunner& getRunner();

/**
 * @brief 初始化执行器
 * @details 调用运行器的执行器初始化接口，传入当前执行器的名称
 */
void ExpExecuter::init()
{
	// 调用运行器的执行器初始化接口
	getRunner().executer_init(name());
}

/**
 * @brief 设置持仓目标
 * @details 批量设置多个合约的目标仓位，逐个调用运行器的执行器设置仓位接口
 * @param targets 目标仓位映射表，key为合约代码，value为目标仓位
 */
void ExpExecuter::set_position(const wt_hashmap<std::string, double>& targets)
{
	// 遍历目标仓位映射表
	for(auto& v : targets)
	{
		// 逐个调用运行器的执行器设置仓位接口
		getRunner().executer_set_position(name(), v.first.c_str(), v.second);
	}
}

/**
 * @brief 持仓变更回调
 * @details 当单个合约的目标仓位发生变化时调用，调用运行器的执行器设置仓位接口
 * @param stdCode 标准化合约代码
 * @param targetPos 目标仓位
 */
void ExpExecuter::on_position_changed(const char* stdCode, double targetPos)
{
	// 调用运行器的执行器设置仓位接口
	getRunner().executer_set_position(name(), stdCode, targetPos);
}
