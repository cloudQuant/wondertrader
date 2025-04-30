/*!
 * @file WtExecPorter.h
 * @author wondertrader
 * @date 2022/05/01
 * 
 * @brief 执行监控系统对外接口定义
 * @details 提供了执行监控系统的C接口，包括初始化、配置、运行、仓位管理等功能
 */

#pragma once
#include <stdint.h>
#include "../Includes/WTSMarcos.h"

/**
 * @brief 字符串类型别名
 * @details 将const char*类型定义为WtString，用于接口中的字符串参数
 */
typedef const char*			WtString;

#ifdef __cplusplus
extern "C"
{
#endif

	/**
	 * @brief 初始化执行监控系统
	 * @details 初始化执行监控系统，加载日志配置
	 * @param logCfg 日志配置，可以是配置文件路径或者配置内容字符串
	 * @param isFile 是否为文件路径，默认为true
	 */
	EXPORT_FLAG	void		init_exec(WtString logCfg, bool isFile = true);

	/**
	 * @brief 配置执行监控系统
	 * @details 加载执行监控系统的配置
	 * @param cfgfile 配置文件路径或者配置内容字符串
	 * @param isFile 是否为文件路径，默认为true
	 */
	EXPORT_FLAG	void		config_exec(WtString cfgfile, bool isFile = true);

	/**
	 * @brief 运行执行监控系统
	 * @details 启动执行监控系统，开始监控和管理交易执行
	 */
	EXPORT_FLAG	void		run_exec();

	/**
	 * @brief 写入日志
	 * @details 向执行监控系统的日志系统写入日志
	 * @param level 日志级别
	 * @param message 日志内容
	 * @param catName 日志分类名称
	 */
	EXPORT_FLAG	void		write_log(unsigned int level, WtString message, WtString catName);

	/**
	 * @brief 获取版本信息
	 * @details 获取执行监控系统的版本信息
	 * @return 版本信息字符串
	 */
	EXPORT_FLAG	WtString	get_version();

	/**
	 * @brief 释放执行监控系统
	 * @details 释放执行监控系统的资源，在程序结束前调用
	 */
	EXPORT_FLAG	void		release_exec();

	/**
	 * @brief 设置目标仓位
	 * @details 设置单个合约的目标仓位，添加到缓存中
	 * @param stdCode 标准化合约代码
	 * @param targetPos 目标仓位
	 */
	EXPORT_FLAG	void		set_position(WtString stdCode, double targetPos);

	/**
	 * @brief 提交目标仓位
	 * @details 提交缓存中的所有目标仓位，触发实际交易执行
	 */
	EXPORT_FLAG	void		commit_positions();

#ifdef __cplusplus
}
#endif