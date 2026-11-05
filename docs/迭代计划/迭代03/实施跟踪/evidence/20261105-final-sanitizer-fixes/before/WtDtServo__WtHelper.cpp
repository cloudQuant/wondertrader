/*!
 * \file WtHelper.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 辅助工具类实现文件
 * 
 * 本文件实现了WtHelper类的方法，提供了与系统路径相关的辅助功能，
 * 如获取当前工作目录等。
 */
#include "WtHelper.h"

#include "../Share/StrUtil.hpp"

#ifdef _MSC_VER
#include <direct.h>
#else	//UNIX
#include <unistd.h>
#endif

std::string WtHelper::_bin_dir;

/**
 * @brief 获取当前工作目录
 * 
 * @return const char* 当前工作目录的字符串指针
 * 
 * @details 该方法返回当前程序的工作目录路径。
 * 实现过程如下：
 * 1. 使用静态局部变量_cwd缓存工作目录路径
 * 2. 首次调用时，检查_cwd是否为空，如果为空则获取当前工作目录
 * 3. 根据不同的操作系统调用相应的API获取工作目录
 * 4. 使用StrUtil::standardisePath对路径进行标准化处理
 * 5. 返回缓存的工作目录路径
 * 
 * 该方法第一次调用时会获取并缓存路径，后续调用直接返回缓存的路径。
 */
const char* WtHelper::get_cwd()
{
	static std::string _cwd;
	if(_cwd.empty())
	{
		char   buffer[255];
#ifdef _MSC_VER
		_getcwd(buffer, 255);
#else	//UNIX
		getcwd(buffer, 255);
#endif
		_cwd = buffer;
		_cwd = StrUtil::standardisePath(_cwd);
	}	
	return _cwd.c_str();
}