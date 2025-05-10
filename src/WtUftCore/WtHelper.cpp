/*!
 * \file WtHelper.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief WonderTrader辅助工具类实现
 * \details 实现了WtHelper类的各种辅助函数，主要包括路径管理和时间处理功能
 */
#include "WtHelper.h"

#include "../Share/StrUtil.hpp"
#include "../Share/StdUtils.hpp"

#include <boost/filesystem.hpp>

#ifdef _MSC_VER
#include <direct.h>
#else	//UNIX
#include <unistd.h>
#endif

uint32_t WtHelper::_cur_date = 0;
uint32_t WtHelper::_cur_time = 0;
uint32_t WtHelper::_cur_secs = 0;
uint32_t WtHelper::_cur_tdate = 0;
std::string WtHelper::_inst_dir;
std::string WtHelper::_gen_dir = "./generated/";


/**
 * @brief 获取当前工作目录
 * @details 使用静态变量缓存当前工作目录，避免重复获取
 * @return 当前工作目录的字符串表示，保证路径格式标准化
 */
std::string WtHelper::getCWD()
{
	static std::string _cwd;
	if(_cwd.empty())
	{
		char   buffer[256];
#ifdef _MSC_VER
		// _getcwd(buffer, 255);
		if (_getcwd(buffer, sizeof(buffer)) == nullptr) {
			throw std::runtime_error("Failed to get current working directory on Windows");
		}
#else	//UNIX
		// getcwd(buffer, 255);
		if (getcwd(buffer, sizeof(buffer)) == nullptr) {
			throw std::runtime_error("Failed to get current working directory on UNIX");
		}
#endif
		_cwd = StrUtil::standardisePath(buffer);
	}	
	return _cwd;
}

/**
 * @brief 获取模块路径
 * @details 根据模块名称、子目录和基础目录组合生成完整路径
 * @param moduleName 模块名称
 * @param subDir 子目录
 * @param isCWD 是否基于当前工作目录，默认为true
 * @return 完整的模块路径字符串
 */
std::string WtHelper::getModulePath(const char* moduleName, const char* subDir, bool isCWD /* = true */)
{
	std::stringstream ss;
	ss << (isCWD?getCWD():getInstDir()) << subDir << "/" << moduleName;
	return ss.str();
}

/**
 * @brief 获取策略数据目录
 * @details 根据生成目录路径及"stradata/"子目录组合生成策略数据路径，并确保该目录存在
 * @return 策略数据目录的字符串表示
 */
const char* WtHelper::getStraDataDir()
{
	static std::string folder = StrUtil::standardisePath(_gen_dir) + "stradata/";
	if (!StdFile::exists(folder.c_str()))
		boost::filesystem::create_directories(folder);
	return folder.c_str();
}

/**
 * @brief 获取策略用户数据目录
 * @details 根据生成目录路径及"userdata/"子目录组合生成用户数据路径，并确保该目录存在
 * @return 策略用户数据目录的字符串表示
 */
const char* WtHelper::getStraUsrDatDir()
{
	static std::string folder = StrUtil::standardisePath(_gen_dir) + "userdata/";
	if (!StdFile::exists(folder.c_str()))
		boost::filesystem::create_directories(folder);
	return folder.c_str();
}

/**
 * @brief 获取组合策略目录
 * @details 根据生成目录路径及"portfolio/"子目录组合生成组合策略路径，并确保该目录存在
 * @return 组合策略目录的字符串表示
 */
const char* WtHelper::getPortifolioDir()
{
	static std::string folder = StrUtil::standardisePath(_gen_dir) + "portfolio/";
	if (!StdFile::exists(folder.c_str()))
		boost::filesystem::create_directories(folder);
	return folder.c_str();
}

/**
 * @brief 获取输出目录
 * @details 根据生成目录路径及"outputs/"子目录组合生成输出目录路径，并确保该目录存在
 * @return 输出目录的字符串表示
 */
const char* WtHelper::getOutputDir()
{
	static std::string folder = StrUtil::standardisePath(_gen_dir) + "outputs/";
	if (!StdFile::exists(folder.c_str()))
		boost::filesystem::create_directories(folder);
	return folder.c_str();
}

/**
 * @brief 获取基础目录
 * @details 返回标准化的生成目录路径，并确保该目录存在
 * @return 基础目录的字符串表示
 */
const char* WtHelper::getBaseDir()
{
	static std::string folder = StrUtil::standardisePath(_gen_dir);
	if (!StdFile::exists(folder.c_str()))
		boost::filesystem::create_directories(folder);
	return folder.c_str();
}