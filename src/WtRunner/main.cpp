/*!
 * @file main.cpp
 * @project	WonderTrader
 *
 * @author Wesley
 * @date 2020/03/30
 * 
 * @brief WonderTrader交易系统的主程序入口
 * @details 本文件实现了WonderTrader交易系统的主程序入口，负责解析命令行参数、初始化和运行WtRunner
 */

/**
 * @brief 包含WtRunner类的头文件
 * @details WtRunner是WonderTrader的主要运行器，负责管理交易引擎的初始化、配置和运行
 */
#include "WtRunner.h"

/**
 * @brief 包含日志工具类的头文件
 * @details WTSLogger用于系统日志的记录和管理
 */
#include "../WTSTools/WTSLogger.h"

/**
 * @brief Windows平台特定头文件
 * @details 在Windows平台下包含崩溃转储功能相关的头文件
 */
#ifdef _MSC_VER
#include "../Common/mdump.h"
#endif

/**
 * @brief 包含命令行参数解析工具的头文件
 * @details cppcli提供了命令行参数的解析功能
 */
#include "../Share/cppcli.hpp"

/**
 * @brief 内存泄漏检测工具头文件（已注释）
 * @details Visual Leak Detector用于检测内存泄漏，开发调试时可以启用
 */
//#include <vld.h>

/**
 * @brief 程序入口函数
 * @details 解析命令行参数，初始化日志系统和配置，创建WtRunner实例并运行
 * @param argc 命令行参数数量
 * @param argv 命令行参数数组
 * @return 程序返回值，0表示正常结束
 */
int main(int argc, char* argv[])
{
/**
 * @brief Windows平台崩溃转储功能
 * @details 在Windows平台下启用崩溃转储功能，当程序崩溃时生成dump文件便于分析
 */
#ifdef _MSC_VER
	CMiniDumper::Enable("WtRunner.exe", true);
#endif

	// 初始化命令行参数解析器
	cppcli::Option opt(argc, argv);

	/**
	 * @brief 配置文件路径参数
	 * @details -c或--config参数用于指定交易引擎的配置文件路径，默认为./config.yaml
	 */
	auto cParam = opt("-c", "--config", "configure filepath, dtcfg.yaml as default", false);

	/**
	 * @brief 日志配置文件路径参数
	 * @details -l或--logcfg参数用于指定日志系统的配置文件路径，默认为./logcfg.yaml
	 */
	auto lParam = opt("-l", "--logcfg", "logging configure filepath, logcfgbt.yaml as default", false);

	/**
	 * @brief 帮助文档参数
	 * @details -h或--help参数用于显示帮助信息
	 */
	auto hParam = opt("-h", "--help", "gain help doc", false)->asHelpParam();

	// 解析命令行参数
	opt.parse();

	// 如果指定了帮助参数，显示帮助信息后退出程序
	if (hParam->exists())
		return 0;

	// 处理日志配置文件路径
	std::string filename;
	if (lParam->exists())
		filename = lParam->get<std::string>();
	else
		filename = "./logcfg.yaml";

	/**
	 * @brief 创建WtRunner实例
	 * @details WtRunner是WonderTrader的主要运行器，负责管理交易引擎的初始化、配置和运行
	 */
	WtRunner runner;
	
	/**
	 * @brief 初始化日志系统
	 * @details 根据日志配置文件初始化日志系统，设置日志级别、输出方式等
	 */
	runner.init(filename);

	// 处理交易引擎配置文件路径
	if (cParam->exists())
		filename = cParam->get<std::string>();
	else
		filename = "./config.yaml";

	/**
	 * @brief 配置交易引擎
	 * @details 根据配置文件初始化交易引擎，加载基础数据、策略、执行器等
	 */
	runner.config(filename);

	/**
	 * @brief 运行交易引擎
	 * @details 启动交易引擎，开始处理行情数据和执行交易策略
	 * @param false 表示同步运行，程序会阻塞直到收到退出信号
	 */
	runner.run(false);
	return 0;
}

