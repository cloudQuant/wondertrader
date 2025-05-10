/*!
 * \file LoaderRunner.cpp
 * \brief 加载器运行器主模块
 * 
 * 加载器运行器(LoaderRunner)是一个用于加载和运行各种数据加载模块的工具
 * 主要功能包括：
 * 1. 动态加载指定的加载器模块（如CTPLoader）
 * 2. 根据配置文件运行加载器
 * 3. 支持命令行参数指定模块和配置文件
 * 
 * \author Wesley
 */

#include <string>

#include "../Share/DLLHelper.hpp"
#include "../Share/cppcli.hpp"
#include "../Share/fmtlib.h"
#include "../Share/StdUtils.hpp"

/**
 * \brief 加载器运行函数类型定义
 * 
 * 定义了加载器模块必须实现的函数类型
 * 
 * \param cfgFile 配置文件路径
 * \param bDate 是否按日期加载
 * \param bTime 是否按时间加载
 * \return 返回加载器运行结果码，0表示成功
 */
typedef int (*LoaderRunner)(const char*, bool, bool);

/**
 * \brief 程序入口函数
 * 
 * 加载器运行器的主函数，负责解析命令行参数、加载指定的模块并调用其运行函数
 * 
 * \param argc 命令行参数数量
 * \param argv 命令行参数数组
 * \return 程序退出码，0表示正常退出
 */
int main(int argc, char *argv[])
{
	// 初始化命令行参数解析器
	cppcli::Option opt(argc, argv);

	// 定义命令行参数
	// -m/--module: 指定加载器模块路径
	auto mParam = opt("-m", "--module", "loader module filepath, CTPLoader.dll for win and libCTPLoader.so for linux as default", false);
	// -c/--config: 指定配置文件路径
	auto cParam = opt("-c", "--config", "configure filepath, config.ini as default", false);

	// -h/--help: 显示帮助信息
	auto hParam = opt("-h", "--help", "gain help doc", false)->asHelpParam();

	// 解析命令行参数
	opt.parse();

	// 如果指定了帮助参数，显示帮助信息并退出
	if (hParam->exists())
		return 0;

	// 初始化模块路径和动态库句柄
	std::string module;
	DllHandle handle = NULL;
	
	// 如果没有指定模块路径，使用默认的CTPLoader
	if(!mParam->exists())
	{
		// 构建默认模块路径，在当前目录下
		module = "./";
		// 根据平台自动添加适当的扩展名（Windows下为.dll，Linux下为.so）
		module += DLLHelper::wrap_module("CTPLoader").c_str();
	}
	else
	{
		// 使用用户指定的模块路径
		module = mParam->get<std::string>();
	}
	
	// 加载指定的动态库
	handle = DLLHelper::load_library(module.c_str());
	if (handle == NULL)
	{
		// 如果加载失败，输出错误信息
		fmt::print("module {} not found\n", module);
	}
	else
	{
		// 从库中获取run函数符号
		LoaderRunner runner = (LoaderRunner)DLLHelper::get_symbol(handle, "run");
		if(runner == NULL)
		{
			// 如果找不到run函数，输出错误信息
			fmt::print("module {} is invalid\n", module);
		}
		else
		{
			// 确定配置文件路径，如果没有指定则使用默认的config.ini
			std::string cfgfile = cParam->exists() ? cParam->get<std::string>() : "config.ini";
			// 检查配置文件是否存在
			if(!StdFile::exists(cfgfile.c_str()))
			{
				// 如果配置文件不存在，输出错误信息并退出
				fmt::print("configure {} not found\n", cfgfile);
				return 0;
			}
			// 调用加载器的run函数，传入配置文件路径
			// 参数false表示不按日期加载，true表示按时间加载
			return runner(cfgfile.c_str(), false, true);
		}
	}

	// 如果没有成功调用加载器的run函数，返回0表示正常退出
	return 0;
}