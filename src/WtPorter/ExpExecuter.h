/*!
 * \file ExpExecuter.h
 * \project WonderTrader
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 执行器导出模块定义
 * \details 定义了执行器的导出接口，用于将内部执行器接口暴露给外部模块
 */

#pragma once
#include "../WtCore/IExecCommand.h"

USING_NS_WTP;

/**
 * @brief 执行器导出类
 * @details 继承自IExecCommand基类，实现了执行器的导出接口，
 *          用于将内部执行器功能暴露给外部模块调用
 */
class ExpExecuter : public IExecCommand
{
public:
	/**
	 * @brief 构造函数
	 * @param name 执行器名称
	 */
	ExpExecuter(const char* name):IExecCommand(name){}

	/**
	 * @brief 初始化执行器
	 * @details 调用运行器的执行器初始化接口
	 */
	void init();

	/**
	 * @brief 设置持仓目标
	 * @details 批量设置多个合约的目标仓位
	 * @param targets 目标仓位映射表，key为合约代码，value为目标仓位
	 */
	virtual void set_position(const wt_hashmap<std::string, double>& targets) override;

	/**
	 * @brief 持仓变更回调
	 * @details 当单个合约的目标仓位发生变化时调用
	 * @param stdCode 标准化合约代码
	 * @param targetPos 目标仓位
	 */
	virtual void on_position_changed(const char* stdCode, double targetPos) override;

};

