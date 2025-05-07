/*!
 * \file WtHelper.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 回测平台辅助工具类头文件
 * \details 定义了回测平台中用于路径管理和环境设置的辅助工具类
 */
#pragma once
#include <string>
#include <stdint.h>

/**
 * @brief 回测平台辅助工具类
 * @details 提供了一系列静态方法，用于获取和设置路径、目录等环境信息
 */
class WtHelper
{
public:
	/**
	 * @brief 获取当前工作目录
	 * @details 获取应用程序当前的工作目录路径，并进行标准化处理
	 * @return std::string 返回标准化后的当前工作目录路径
	 */
	static std::string getCWD();

	/**
	 * @brief 获取输出目录
	 * @details 获取回测平台的输出目录路径，如果目录不存在则创建
	 * @return const char* 返回输出目录路径的C字符串
	 */
	static const char* getOutputDir();

	/**
	 * @brief 获取实例目录
	 * @details 获取应用程序实例的安装目录
	 * @return const std::string& 返回实例目录路径的字符串引用
	 */
	static const std::string& getInstDir() { return _inst_dir; }

	/**
	 * @brief 设置实例目录
	 * @details 设置应用程序实例的安装目录
	 * @param inst_dir 实例目录路径
	 */
	static void setInstDir(const char* inst_dir) { _inst_dir = inst_dir; }
	/**
	 * @brief 设置输出目录
	 * @details 设置回测平台的输出目录路径，并进行标准化处理
	 * @param out_dir 输出目录路径
	 */
	static void setOutputDir(const char* out_dir);

private:
	static std::string	_inst_dir;	///< 实例所在目录，存储应用程序实例的安装路径
	static std::string	_out_dir;	///< 输出目录，存储回测结果和日志等输出文件的路径
};

