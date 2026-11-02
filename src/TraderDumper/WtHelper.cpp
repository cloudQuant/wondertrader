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
#include <system_error>

std::string WtHelper::_bin_dir;

const char* WtHelper::get_cwd()
{
	static std::string _cwd;
	if (_cwd.empty())
	{
		std::error_code ec;
		const std::filesystem::path cwd = std::filesystem::current_path(ec);
		if (ec)
			return "";

		_cwd = StrUtil::standardisePath(cwd.string());
	}	
	return _cwd.c_str();
}
