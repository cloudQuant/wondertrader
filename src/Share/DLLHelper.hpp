/*!
 * \file DLLHelper.hpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 动态库辅助类,主要是把跨平台的差异封装起来,方便调用
 */
#pragma once
#include <string>

#ifdef _MSC_VER
#include <wtypes.h>
/**
 * @brief 动态库句柄类型（Windows平台）
 * @details 在Windows平台下，使用HMODULE表示动态库句柄
 */
typedef HMODULE		DllHandle;

/**
 * @brief 函数指针类型（Windows平台）
 * @details 在Windows平台下，使用void*表示函数指针
 */
typedef void*		ProcHandle;
#else
#include <dlfcn.h>
/**
 * @brief 动态库句柄类型（非Windows平台）
 * @details 在非Windows平台下，使用void*表示动态库句柄
 */
typedef void*		DllHandle;

/**
 * @brief 函数指针类型（非Windows平台）
 * @details 在非Windows平台下，使用void*表示函数指针
 */
typedef void*		ProcHandle;
#endif

/**
 * @brief 动态库操作辅助类
 * @details 封装了跨平台（Windows/Unix）动态库操作的差异，提供统一的接口
 *          包括动态库的加载、释放、符号查找等基本操作
 */
class DLLHelper
{
public:
	/**
	 * @brief 加载动态库
	 * @param filename 动态库文件名或路径
	 * @return DllHandle 成功返回动态库句柄，失败返回NULL
	 * @details 根据不同平台调用相应的库加载函数：
	 *          - Windows平台调用LoadLibrary
	 *          - 非Windows平台调用dlopen，并使用RTLD_NOW立即解析所有未定义的符号
	 *          函数内部捕获所有可能的异常，确保不会因动态库加载失败而导致程序崩溃
	 */
	static DllHandle load_library(const char *filename)
	{
		try
		{
#ifdef _MSC_VER
			return ::LoadLibrary(filename);
#else
			DllHandle ret = dlopen(filename, RTLD_NOW);
			if (ret == NULL)
				printf("%s\n", dlerror());
			return ret;
#endif
		}
		catch(...)
		{
			return NULL;
		}
	}

	/**
	 * @brief 释放动态库
	 * @param handle 要释放的动态库句柄
	 * @details 根据不同平台调用相应的库释放函数：
	 *          - Windows平台调用FreeLibrary
	 *          - 非Windows平台调用dlclose
	 *          如果传入的句柄为NULL，函数会直接返回，不执行任何操作
	 */
	static void free_library(DllHandle handle)
	{
		if (NULL == handle)
			return;

#ifdef _MSC_VER
		::FreeLibrary(handle);
#else
		dlclose(handle);
#endif
	}

	/**
	 * @brief 获取动态库中的符号（函数或变量）
	 * @param handle 动态库句柄
	 * @param name 符号名称
	 * @return ProcHandle 成功返回符号地址，失败返回NULL
	 * @details 根据不同平台调用相应的符号获取函数：
	 *          - Windows平台调用GetProcAddress
	 *          - 非Windows平台调用dlsym
	 *          如果传入的动态库句柄为NULL，函数会直接返回NULL
	 */
	static ProcHandle get_symbol(DllHandle handle, const char* name)
	{
		if (NULL == handle)
			return NULL;

#ifdef _MSC_VER
		return ::GetProcAddress(handle, name);
#else
		return dlsym(handle, name);
#endif
	}

	/**
	 * @brief 将模块名转换为平台相关的动态库文件名
	 * @param name 基本模块名
	 * @param unixPrefix 非Windows平台下的库文件前缀，默认为"lib"
	 * @return std::string 转换后的动态库文件名
	 * @details 根据不同平台生成相应格式的动态库文件名：
	 *          - Windows平台：直接添加".dll"后缀
	 *          - 非Windows平台：跳过名称前的非字母字符，添加前缀（默认"lib"）和".so"后缀
	 *            例如：传入"test"，返回"libtest.so"
	 *                  传入"_test"，返回"_libtest.so"
	 */
	static std::string wrap_module(const char* name, const char* unixPrefix = "lib")
	{
#ifdef _WIN32
		std::string ret = name;
		ret += ".dll";
		return std::move(ret);
#else
		std::size_t idx = 0;
		while (!isalpha(name[idx]))
			idx++;
		std::string ret(name, idx);
		ret.append(unixPrefix);
		ret.append(name + idx);
		ret += ".so";
		return std::move(ret);
#endif
	}
};