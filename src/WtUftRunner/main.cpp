/*!
 * @file main.cpp
 * @project	WonderTrader
 *
 * @author Wesley
 * @date 2020/03/30
 * 
 * @brief UFT策略引擎的主程序入口
 * @details 该文件实现了WtUftRunner的命令行参数解析和启动过程，
 *          用于加载和运行高频UFT策略。
 */

#include "WtUftRunner.h"

#include "../WTSTools/WTSLogger.h"

#ifdef _MSC_VER
#include "../Common/mdump.h"
#endif

#include <Share/cppcli.hpp>

#include <chrono>
#include <memory>
#include <thread>

namespace
{
// I03-NEW-058 / 卡9a：离开 main 前在控制线程完成关闭；只因无回调排空证明的 parser owner
// 而无法完成时，按合同把 runner 及其借用图保留到进程退出（诊断 + 刻意不销毁，不 abort）；
// 其他无法完成的关闭仍经析构 fail-stop。有限重试以等待 PENDING 的排空。
int finish_runner(std::unique_ptr<WtUftRunner>& runner, int exitCode)
{
	for (int attempt = 0; attempt < 30; ++attempt)
	{
		if (runner->shutdown())
		{
			runner.reset();
			return exitCode;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	if (runner->parserOwnersRetained())
	{
		WTSLogger::error("WtUftRunner exit: parser owners without callback-drain proof are retained until process exit; runner intentionally not destroyed");
		(void)runner.release(); // deliberate process-lifetime retention
		return exitCode;
	}
	runner.reset(); // unrecoverable teardown failure: the destructor fails stop
	return exitCode;
}
}
//#include <vld.h>

/**
 * @brief 主函数入口
 * @param argc 命令行参数数量
 * @param argv 命令行参数数组
 * @return 程序退出码，0表示正常退出
 * @details 解析命令行参数，初始化并启动UFT策略引擎
 */
int main(int argc, char* argv[])
{
#ifdef _MSC_VER
	// 在Windows下启用崩溃转储功能，可生成dump文件
	CMiniDumper::Enable("WtUftRunner.exe", true);
#endif

	// 初始化命令行参数解析器
	cppcli::Option opt(argc, argv);

	// 定义参数选项: 配置文件路径
	auto cParam = opt("-c", "--config", "configure filepath, dtcfg.yaml as default", false);
	// 定义参数选项: 日志配置文件路径
	auto lParam = opt("-l", "--logcfg", "logging configure filepath, logcfgbt.yaml as default", false);

	// 定义帮助选项
	auto hParam = opt("-h", "--help", "gain help doc", false)->asHelpParam();

	// 解析命令行参数
	opt.parse();

	// 如果指定了帮助选项，则显示帮助信息并退出
	if (hParam->exists())
		return 0;

	// 初始化文件名变量
	std::string filename;
	// 第一步: 获取日志配置文件路径，如果命令行参数中没有指定，则使用默认路径
	if (lParam->exists())
		filename = lParam->get<std::string>();
	else
		filename = "./logcfg.yaml";

	// 创建UFT运行器实例
	std::unique_ptr<WtUftRunner> runner(new WtUftRunner());
	// 使用日志配置文件初始化日志系统
	runner->init(filename);

	// 读取配置文件路径，如果命令行参数中没有指定，则使用默认路径
	if (cParam->exists())
		filename = cParam->get<std::string>();
	else
		filename = "./config.yaml";
	// 加载并应用配置
	if (!runner->config(filename))
		return finish_runner(runner, 1);

	// 启动UFT策略引擎（非异步模式）
	runner->run(false);
	return finish_runner(runner, 0);
}
