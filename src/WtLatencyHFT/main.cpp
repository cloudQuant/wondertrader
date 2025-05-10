/*!
 * @file main.cpp
 * @project	WonderTrader
 *
 * @author Wesley
 * @date 2020/03/30
 * 
 * @brief 高频交易延迟测试主程序
 * @details 本文件实现了HFT(高频交易)延迟测试程序的主入口函数，用于测试WonderTrader框架下HFT模块的内部延迟性能
 */

#include "../WTSTools/WTSLogger.h"

/**
 * @brief 测试HFT模块延迟的外部函数声明
 * @details 该函数在HftLatencyTool.cpp中实现，主要用于执行高频交易延迟测试流程
 */
extern void test_hft();

/**
 * @brief 程序入口函数
 * @return 程序退出码，0表示正常退出
 * @details 本函数作为程序入口点，调用test_hft()函数执行高频交易延迟测试
 */
int main()
{
	// 执行高频交易延迟测试
	test_hft();
	// 提示用户按任意键退出
	printf("press enter key to exit\r\n");
	getchar();
	return 0;
}

