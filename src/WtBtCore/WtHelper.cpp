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
#include <filesystem>
namespace fs = std::filesystem;

#ifdef _MSC_VER
#include <direct.h>
#else	//UNIX
#include <unistd.h>
#endif

std::string WtHelper::_inst_dir;
std::string WtHelper::_out_dir = "./outputs_bt/";

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
		_cwd = buffer;
		_cwd = StrUtil::standardisePath(_cwd);
	}	
	return _cwd;
}

void WtHelper::setOutputDir(const char* out_dir)
{
	_out_dir = StrUtil::standardisePath(std::string(out_dir));
}

const char* WtHelper::getOutputDir()
{
	if (!fs::exists(_out_dir.c_str()))
        fs::create_directories(_out_dir.c_str());
	return _out_dir.c_str();
}