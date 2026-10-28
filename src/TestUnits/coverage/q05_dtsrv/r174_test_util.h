/*!
 * \file r174_test_util.h
 * \brief R174 lane（第二十五波 · StateMonitor.h 查询面）自用小工具。
 *
 * 约束：
 *  - 只落系统临时目录，目录名带 r174_ + pid + 序号，fixture 负责删除；
 *  - 不建 socket、不建 shm、不起监控线程；本 lane 只做纯内存查询与配置装配；
 *  - 不依赖墙钟：initialize 的会话没有关联品种，不走 getCurDate/getCurMin 分支。
 */
#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>

namespace r174 {

namespace fs = std::filesystem;

inline fs::path make_tmp_dir(const char* tag)
{
	static std::atomic<int> seq { 0 };
	fs::path p = fs::temp_directory_path()
		/ (std::string("r174_") + tag + "_" + std::to_string(::getpid()) + "_"
			+ std::to_string(seq++));
	fs::create_directories(p);
	return p;
}

inline void write_file(const fs::path& p, const std::string& content)
{
	std::ofstream ofs(p.string(), std::ios::out | std::ios::trunc);
	ofs << content;
	ofs.close();
}

inline WTSVariant* load_cfg(const fs::path& dir, const char* name, const std::string& body)
{
	fs::path p = dir / name;
	write_file(p, body);
	return WTSCfgLoader::load_from_file(p.string().c_str());
}

}	// namespace r174
