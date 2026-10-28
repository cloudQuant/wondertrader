// Lane R56 - 本 lane 自用的临时目录工具（只写系统临时目录，用例结束清理）
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <unistd.h>

namespace r56tmp {

inline std::filesystem::path unique_dir(const char* prefix)
{
	static uint32_t s_seq = 0;
	std::error_code ec;
	std::filesystem::path dir = std::filesystem::temp_directory_path() /
		(std::string(prefix) + "_" + std::to_string((long)::getpid()) + "_" + std::to_string(s_seq++));
	std::filesystem::remove_all(dir, ec);
	std::filesystem::create_directories(dir, ec);
	return dir;
}

inline std::string write_file(const std::filesystem::path& dir, const char* name, const std::string& body)
{
	std::filesystem::path p = dir / name;
	std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
	ofs << body;
	ofs.close();
	return p.string();
}

inline std::string read_file(const std::string& path)
{
	std::ifstream ifs(path, std::ios::binary);
	std::ostringstream oss;
	oss << ifs.rdbuf();
	return oss.str();
}

inline void clean_dir(const std::filesystem::path& dir)
{
	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
}

}	// namespace r56tmp
