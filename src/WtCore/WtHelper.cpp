/*!
 * \file WtHelper.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 帮助工具类实现文件
 * \details 实现了WtHelper帮助工具类的各种方法，包括路径管理、时间管理等
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

/**
 * @brief 当前日期，格式为YYYYMMDD
 */
uint32_t WtHelper::_cur_date = 0;

/**
 * @brief 当前时间，格式为HHMM
 */
uint32_t WtHelper::_cur_time = 0;

/**
 * @brief 当前秒数，格式为HHMMSS
 */
uint32_t WtHelper::_cur_secs = 0;

/**
 * @brief 当前交易日期，格式为YYYYMMDD
 */
uint32_t WtHelper::_cur_tdate = 0;

/**
 * @brief 实例目录路径
 */
std::string WtHelper::_inst_dir;

/**
 * @brief 生成目录路径，默认为"./generated/"
 */
std::string WtHelper::_gen_dir = "./generated/";


/**
 * @brief 获取当前工作目录
 * @return 当前工作目录的标准路径
 */
std::string WtHelper::getCWD()
{
	// 使用静态变量缓存工作目录，避免重复获取
	static std::string _cwd;
	if(_cwd.empty())
	{
		char   buffer[256];
#ifdef _MSC_VER
		_getcwd(buffer, 255); // Windows系统下获取当前目录
#else	//UNIX
		getcwd(buffer, 255);  // Unix/Linux系统下获取当前目录
#endif
		_cwd = StrUtil::standardisePath(buffer); // 标准化路径格式
	}	
	return _cwd;
}

/**
 * @brief 获取模块路径
 * @param moduleName 模块名称
 * @param subDir 子目录
 * @param isCWD 是否使用当前工作目录作为基准，默认为true
 * @return 模块的完整路径
 */
std::string WtHelper::getModulePath(const char* moduleName, const char* subDir, bool isCWD /* = true */)
{
	// 构造模块完整路径：基础目录 + 子目录 + 模块名称
	std::stringstream ss;
	ss << (isCWD?getCWD():getInstDir()) << subDir << "/" << moduleName;
	return ss.str();
}

/**
 * @brief 获取策略数据目录
 * @return 策略数据目录路径，如果不存在则创建
 */
const char* WtHelper::getStraDataDir()
{
	// 构造策略数据目录路径：生成目录 + "stradata/"
	static std::string folder = StrUtil::standardisePath(_gen_dir) + "stradata/";
	// 如果目录不存在，则创建目录
	if (!StdFile::exists(folder.c_str()))
		boost::filesystem::create_directories(folder);
	return folder.c_str();
}

/**
 * @brief 获取执行数据目录
 * @return 执行数据目录路径，如果不存在则创建
 */
const char* WtHelper::getExecDataDir()
{
	// 构造执行数据目录路径：生成目录 + "execdata/"
	static std::string folder = StrUtil::standardisePath(_gen_dir) + "execdata/";
	// 如果目录不存在，则创建目录
	if (!StdFile::exists(folder.c_str()))
		boost::filesystem::create_directories(folder);
	return folder.c_str();
}

/**
 * @brief 获取策略用户数据目录
 * @return 策略用户数据目录路径，如果不存在则创建
 */
const char* WtHelper::getStraUsrDatDir()
{
	// 构造策略用户数据目录路径：生成目录 + "userdata/"
	static std::string folder = StrUtil::standardisePath(_gen_dir) + "userdata/";
	// 如果目录不存在，则创建目录
	if (!StdFile::exists(folder.c_str()))
		boost::filesystem::create_directories(folder);
	return folder.c_str();
}

/**
 * @brief 获取组合目录
 * @return 组合目录路径，如果不存在则创建
 */
const char* WtHelper::getPortifolioDir()
{
	// 构造组合目录路径：生成目录 + "portfolio/"
	static std::string folder = StrUtil::standardisePath(_gen_dir) + "portfolio/";
	// 如果目录不存在，则创建目录
	if (!StdFile::exists(folder.c_str()))
		boost::filesystem::create_directories(folder);
	return folder.c_str();
}

/**
 * @brief 获取输出目录
 * @return 输出目录路径，如果不存在则创建
 */
const char* WtHelper::getOutputDir()
{
	// 构造输出目录路径：生成目录 + "outputs/"
	static std::string folder = StrUtil::standardisePath(_gen_dir) + "outputs/";
	// 如果目录不存在，则创建目录
	if (!StdFile::exists(folder.c_str()))
		boost::filesystem::create_directories(folder);
	return folder.c_str();
}

/**
 * @brief 获取基础目录
 * @return 基础目录路径，如果不存在则创建
 */
const char* WtHelper::getBaseDir()
{
	// 获取标准化的生成目录路径
	static std::string folder = StrUtil::standardisePath(_gen_dir);
	// 如果目录不存在，则创建目录
	if (!StdFile::exists(folder.c_str()))
		boost::filesystem::create_directories(folder);
	return folder.c_str();
}