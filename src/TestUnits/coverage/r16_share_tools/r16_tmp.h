/*!
 * \file r16_tmp.h
 * \brief R16 用例专用的临时文件路径助手（用例之间按 pid + 用例名隔离，可并行跑）
 *        只碰普通磁盘文件，不涉及共享内存 / 线程 / socket。
 */
#pragma once
#include <string>
#include <fstream>
#include <unistd.h>
#include <filesystem>

namespace r16tmp
{
	inline std::string dir(const char* tag)
	{
		std::string p = std::filesystem::temp_directory_path().string()
			+ "/wondertrader_r16_" + tag + "_" + std::to_string((long)getpid());
		std::filesystem::create_directories(p);
		return p;
	}

	inline std::string file(const char* tag, const char* name)
	{
		return dir(tag) + "/" + name;
	}

	inline void clean(const char* tag)
	{
		std::error_code ec;
		std::filesystem::remove_all(dir(tag), ec);
		// 清完立刻把空目录建回来，这样「先算路径再 clean」的用例仍然可写
		std::filesystem::create_directories(dir(tag), ec);
	}
}
