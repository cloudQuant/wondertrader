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

class CpuHelper
{
public:
    // 获取 CPU 核心数量
    static uint32_t get_cpu_cores()
    {
        static uint32_t cores = std::thread::hardware_concurrency();
        return cores;
    }

    // 绑定线程到指定核心
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
