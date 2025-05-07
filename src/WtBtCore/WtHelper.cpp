/*!
 * \file WtHelper.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 回测平台辅助工具类实现文件
 * \details 实现了回测平台中用于路径管理和环境设置的辅助工具类
 */
#include "WtHelper.h"

#include "../Share/StrUtil.hpp"
#include <boost/filesystem.hpp>

#ifdef _MSC_VER
#include <direct.h>
#else	//UNIX
#include <unistd.h>
#endif

std::string WtHelper::_inst_dir;
std::string WtHelper::_out_dir = "./outputs_bt/";

/**
 * @brief 获取当前工作目录
 * @details 获取应用程序当前的工作目录路径，并进行标准化处理
 * 使用静态变量缓存结果，避免重复计算
 * @return std::string 返回标准化后的当前工作目录路径
 */
std::string WtHelper::getCWD()
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
	return _cwd;
}

/**
 * @brief 设置输出目录
 * @details 设置回测平台的输出目录路径，并使用StrUtil工具进行路径标准化
 * @param out_dir 输出目录路径
 */
void WtHelper::setOutputDir(const char* out_dir)
{
	_out_dir = StrUtil::standardisePath(std::string(out_dir));
}

/**
 * @brief 获取输出目录
 * @details 获取回测平台的输出目录路径，如果目录不存在则使用boost::filesystem创建
 * @return const char* 返回输出目录路径的C字符串
 */
const char* WtHelper::getOutputDir()
{
	if (!boost::filesystem::exists(_out_dir.c_str()))
        boost::filesystem::create_directories(_out_dir.c_str());
	return _out_dir.c_str();
}