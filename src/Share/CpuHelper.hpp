//#pragma once
//#include <thread>
//
//class CpuHelper
//{
//public:
//	static uint32_t get_cpu_cores()
//	{
//		static uint32_t cores = std::thread::hardware_concurrency();
//		return cores;
//	}
//
//#ifdef _WIN32
//#include <thread>
//	static bool bind_core(uint32_t i)
//	{
//		uint32_t cores = get_cpu_cores();
//		if (i >= cores)
//			return false;
//
//		HANDLE hThread = GetCurrentThread();
//		DWORD_PTR mask = SetThreadAffinityMask(hThread, (DWORD_PTR)(1 << i));
//		return (mask != 0);
//	}
//#else
//#include <pthread.h>
//#include <sched.h>
//#include <unistd.h>
//#include <string.h>
//	static bool bind_core(uint32_t i)
//	{
//		int cores = get_cpu_cores();
//		if (i >= cores)
//			return false;
//
//		cpu_set_t mask;
//		CPU_ZERO(&mask);
//		CPU_SET(i, &mask);
//		return (pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) >= 0);
//	}
//#endif
//};

/**
 * @file CpuHelper.hpp
 * @brief CPU相关操作的辅助工具类
 * @details 提供获取CPU核心数量和将线程绑定到特定CPU核心的功能
 * @author wondertrader
 */

#pragma once

#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <cstring>
#endif

/**
 * @brief CPU辅助工具类
 * @details 提供CPU相关的实用功能，如获取CPU核心数量和将线程绑定到特定CPU核心
 * @note 所有方法都是静态的，可以直接通过类名调用
 */
class CpuHelper
{
public:
    /**
     * @brief 获取系统CPU核心数量
     * @return uint32_t 返回系统可用的CPU核心数量
     * @details 使用std::thread::hardware_concurrency()函数获取系统支持的并发线程数，
     *          通常与物理CPU核心数相匹配
     * @note 结果被缓存为静态变量，多次调用不会重复计算
     */
    static uint32_t get_cpu_cores()
    {
        static uint32_t cores = std::thread::hardware_concurrency();
        return cores;
    }

    /**
     * @brief 将当前线程绑定到指定的CPU核心
     * @param i 指定的CPU核心索引，从0开始计数
     * @return bool 绑定成功返回true，失败返回false
     * @details 此函数会尝试将调用线程绑定到指定索引的CPU核心上执行
     *          如果指定的核心索引超出系统可用核心数，将返回false
     *          不同操作系统下使用不同的API实现绑定功能
     */
    static bool bind_core(uint32_t i)
    {
        uint32_t cores = get_cpu_cores();
        if (i >= cores)
            return false;

#ifdef _WIN32
        HANDLE hThread = GetCurrentThread();
        DWORD_PTR mask = SetThreadAffinityMask(hThread, (DWORD_PTR)(1 << i));
        return (mask != 0);
#elif __APPLE__
        // macOS 不支持线程绑定到特定核心
        return false;
#else
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(i, &mask);
        return (pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) == 0);
#endif
    }
};
