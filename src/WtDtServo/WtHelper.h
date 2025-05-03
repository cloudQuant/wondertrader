/*!
 * \file WtHelper.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 辅助工具类头文件
 * 
 * 本文件定义了WtHelper类，提供了一系列系统相关的辅助函数，
 * 如获取当前工作目录、模块目录等功能。
 */
#pragma once
#include <string>
#include <stdint.h>

/**
 * @brief 系统辅助工具类
 * 
 * @details WtHelper类提供了一系列与系统路径相关的辅助函数，
 * 包括获取当前工作目录、设置和获取模块目录等功能。
 * 这些函数都是静态的，可以直接通过类名调用，无需创建类的实例。
 */
class WtHelper
{
public:
	/**
	 * @brief 获取当前工作目录
	 * 
	 * @return const char* 当前工作目录的字符串指针
	 * 
	 * @details 该方法返回当前程序的工作目录路径。
	 * 第一次调用时会获取并缓存路径，后续调用直接返回缓存的路径。
	 * 返回的路径已经过标准化处理，确保路径分隔符的一致性。
	 */
	static const char* get_cwd();

	/**
	 * @brief 获取模块目录
	 * 
	 * @return const char* 模块目录的字符串指针
	 * 
	 * @details 该方法返回当前模块的目录路径。
	 * 返回的是静态成员变量_bin_dir的内容，该变量可以通过set_module_dir方法设置。
	 * 如果未调用set_module_dir设置，则返回空字符串。
	 */
	static const char* get_module_dir(){ return _bin_dir.c_str(); }

	/**
	 * @brief 设置模块目录
	 * 
	 * @param mod_dir 要设置的模块目录路径
	 * 
	 * @details 该方法用于设置当前模块的目录路径。
	 * 设置后的路径可以通过get_module_dir方法获取。
	 * 通常在程序初始化时调用该方法设置模块目录。
	 */
	static void set_module_dir(const char* mod_dir){ _bin_dir = mod_dir; }

private:
	/**
	 * @brief 模块目录路径
	 * 
	 * @details 存储当前模块的目录路径。
	 * 该变量是静态的，由set_module_dir方法设置，由get_module_dir方法获取。
	 * 默认为空字符串。
	 */
	static std::string	_bin_dir;
};

