/*!
 * \file WtHelper.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "WtHelper.h"

#include "../Share/StrUtil.hpp"
#include "../Share/StdUtils.hpp"

#include <filesystem>
namespace fs = std::filesystem;

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


std::string WtHelper::getCWD()
{
	static std::string _cwd;
	if(_cwd.empty())
	{
		// I03-NEW-112: the old fixed 255-byte buffer ignored getcwd() failure (ERANGE once the
		// path reaches 255 bytes) and then read the uninitialized buffer. Use a larger buffer,
		// check the result, and fall back to "./" (the current directory) without caching it.
		char   buffer[4096] = { 0 };
#ifdef _MSC_VER
		if (_getcwd(buffer, (int)sizeof(buffer)) == NULL)
#else	//UNIX
		if (getcwd(buffer, sizeof(buffer)) == NULL)
#endif
			return "./";
		_cwd = StrUtil::standardisePath(buffer);
	}	
	return _cwd;
}

std::string WtHelper::getModulePath(const char* moduleName, const char* subDir, bool isCWD /* = true */)
{
	std::stringstream ss;
	ss << (isCWD?getCWD():getInstDir()) << subDir << "/" << moduleName;
	return ss.str();
}

const char* WtHelper::getStraDataDir()
{
	static std::string folder = StrUtil::standardisePath(_gen_dir) + "stradata/";
	if (!StdFile::exists(folder.c_str()))
		fs::create_directories(folder);
	return folder.c_str();
}

const char* WtHelper::getStraUsrDatDir()
{
	static std::string folder = StrUtil::standardisePath(_gen_dir) + "userdata/";
	if (!StdFile::exists(folder.c_str()))
		fs::create_directories(folder);
	return folder.c_str();
}

const char* WtHelper::getPortifolioDir()
{
	static std::string folder = StrUtil::standardisePath(_gen_dir) + "portfolio/";
	if (!StdFile::exists(folder.c_str()))
		fs::create_directories(folder);
	return folder.c_str();
}

const char* WtHelper::getOutputDir()
{
	static std::string folder = StrUtil::standardisePath(_gen_dir) + "outputs/";
	if (!StdFile::exists(folder.c_str()))
		fs::create_directories(folder);
	return folder.c_str();
}

const char* WtHelper::getBaseDir()
{
	static std::string folder = StrUtil::standardisePath(_gen_dir);
	if (!StdFile::exists(folder.c_str()))
		fs::create_directories(folder);
	return folder.c_str();
}