/*!
 * \file WtHelper.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 路径常用辅助工具类实现
 * \details 实现了WtHelper工具类，用于获取与设置当前工作目录和模块目录
 *          包含跨平台的目录获取方法实现，支持Windows和Unix/Linux系统
 */
#include "WtHelper.h"

#include "../Share/StrUtil.hpp"

#ifdef _MSC_VER
#include <direct.h>
#else	//UNIX
#include <unistd.h>
#endif

/**
 * @brief 模块目录路径的静态存储变量
 * @details 默认为空字符串，需要通过set_module_dir方法显式设置
 */
std::string WtHelper::_bin_dir;

/**
 * @brief 获取当前工作目录的实现
 * @details 返回应用程序的当前工作目录，使用静态变量缓存结果以减少重复调用系统函数
 * @return 当前工作目录的C风格字符串
 */
const char* WtHelper::get_cwd()
{
	// 使用静态变量保存工作目录，避免重复获取
	static std::string _cwd;
	// 只有首次调用时才获取当前工作目录
	if(_cwd.empty())
	{
		char buffer[255]; // 用于存储当前目录的缓冲区
#ifdef _MSC_VER
		// Windows系统下使用_getcwd获取当前目录
		_getcwd(buffer, 255);
#else	//UNIX
		// Unix/Linux系统下使用getcwd获取当前目录
		getcwd(buffer, 255);
#endif
		_cwd = buffer; // 将C风格字符串转换为string
		// 标准化路径，确保路径分隔符正确
		_cwd = StrUtil::standardisePath(_cwd);
	}	
	// 返回字符串的C风格表示
	return _cwd.c_str();
}