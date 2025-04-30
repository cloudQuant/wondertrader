/*!
 * @file WtExecPorter.cpp
 * @author wondertrader
 * @date 2022/05/01
 * 
 * @brief 执行监控系统对外接口实现
 * @details 实现了执行监控系统的C接口，包括初始化、配置、运行、仓位管理等功能
 */

#include "WtExecPorter.h"
#include "WtExecRunner.h"

#include "../WtCore/WtHelper.h"
#include "../WTSTools/WTSLogger.h"
#include "../Includes/WTSVersion.h"

/**
 * @brief 平台名称定义
 * @details 根据编译平台定义不同的平台名称，用于版本信息显示
 */
#ifdef _WIN32
#   ifdef _WIN64
    char PLATFORM_NAME[] = "X64";
#   else
    char PLATFORM_NAME[] = "X86";
#   endif
#else
    char PLATFORM_NAME[] = "UNIX";
#endif

/**
 * @brief 获取执行运行器单例
 * @details 使用单例模式获取WtExecRunner对象，确保整个程序中只有一个运行器实例
 * @return WtExecRunner对象的引用
 */
WtExecRunner& getRunner()
{
	static WtExecRunner runner;
	return runner;
}

/**
 * @brief 初始化执行监控系统
 * @details 初始化执行监控系统，加载日志配置，确保只初始化一次
 * @param logCfg 日志配置，可以是配置文件路径或者配置内容字符串
 * @param isFile 是否为文件路径，默认为true
 */
void init_exec(WtString logCfg, bool isFile /*= true*/)
{
	// 使用静态变量记录初始化状态，确保只初始化一次
	static bool inited = false;

	if (inited)
		return;

	// 调用运行器的初始化方法
	getRunner().init(logCfg);

	inited = true;
}

/**
 * @brief 配置执行监控系统
 * @details 加载执行监控系统的配置，如果没有指定配置文件，则使用默认的"cfgexec.json"
 * @param cfgfile 配置文件路径或者配置内容字符串
 * @param isFile 是否为文件路径，默认为true
 */
void config_exec(WtString cfgfile, bool isFile /*= true*/)
{
	// 如果配置文件路径为空，则使用默认的"cfgexec.json"
	if (strlen(cfgfile) == 0)
		getRunner().config("cfgexec.json");
	else
		getRunner().config(cfgfile);
}

/**
 * @brief 运行执行监控系统
 * @details 启动执行监控系统，开始监控和管理交易执行
 */
void run_exec()
{
	// 调用运行器的运行方法
	getRunner().run();
}

/**
 * @brief 释放执行监控系统
 * @details 释放执行监控系统的资源，在程序结束前调用
 */
void release_exec()
{
	// 调用运行器的释放方法
	getRunner().release();
}

/**
 * @brief 获取版本信息
 * @details 获取执行监控系统的版本信息，包括平台、版本号和编译时间
 * @return 版本信息字符串
 */
WtString get_version()
{
	// 使用静态变量缓存版本信息字符串，避免重复构建
	static std::string _ver;
	if (_ver.empty())
	{
		// 构建版本信息字符串，包括平台、版本号和编译时间
		_ver = PLATFORM_NAME;
		_ver += " ";
		_ver += WT_VERSION;
		_ver += " Build@";
		_ver += __DATE__;
		_ver += " ";
		_ver += __TIME__;
	}
	return _ver.c_str();
}

/**
 * @brief 写入日志
 * @details 向执行监控系统的日志系统写入日志，可以指定日志分类
 * @param level 日志级别
 * @param message 日志内容
 * @param catName 日志分类名称
 */
void write_log(unsigned int level, WtString message, WtString catName)
{
	// 如果指定了日志分类，则使用分类日志函数
	if (strlen(catName) > 0)
	{
		WTSLogger::log_raw_by_cat(catName, (WTSLogLevel)level, message);
	}
	else
	{
		// 否则使用默认日志函数
		WTSLogger::log_raw((WTSLogLevel)level, message);
	}
}

/**
 * @brief 设置目标仓位
 * @details 设置单个合约的目标仓位，添加到缓存中
 * @param stdCode 标准化合约代码
 * @param targetPos 目标仓位
 */
void set_position(WtString stdCode, double targetPos)
{
	// 调用运行器的设置仓位方法
	getRunner().setPosition(stdCode, targetPos);
}

/**
 * @brief 提交目标仓位
 * @details 提交缓存中的所有目标仓位，触发实际交易执行
 */
void commit_positions()
{
	// 调用运行器的提交仓位方法
	getRunner().commitPositions();
}