/*!
 * \file WtHelper.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 路径常用辅助工具类头文件
 * \details 定义了WtHelper工具类，用于获取与设置当前工作目录和模块目录
 *          在模块初始化和路径操作的时候提供便捷工具
 */
#pragma once
#include <string>
#include <stdint.h>

/**
 * @brief 路径常用辅助工具类
 * @details 提供了获取当前工作目录和模块目录的静态方法
 *          在项目中用于路径相关的操作，简化路径管理
 */
class WtHelper
{
public:
	/**
	 * @brief 获取当前工作目录
	 * @details 返回应用程序当前的工作目录，返回结果会被缓存
	 * @return const char* 当前工作目录的C风格字符串
	 */
	static const char* get_cwd();

	/**
	 * @brief 获取模块目录
	 * @details 返回已设置的模块目录，需要先调用set_module_dir设置
	 * @return const char* 模块目录的C风格字符串
	 */
	static const char* get_module_dir(){ return _bin_dir.c_str(); }

	/**
	 * @brief 设置模块目录
	 * @details 存储模块目录路径，供后续使用
	 * @param mod_dir 模块目录路径
	 */
	static void set_module_dir(const char* mod_dir){ _bin_dir = mod_dir; }

private:
	static std::string	_bin_dir;  ///< 模块目录路径
};

