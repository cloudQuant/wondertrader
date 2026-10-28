/*!
 * \file r102_dtcore_util.h
 * \brief R102 lane（WtDtCore）自用小工具：临时目录、插件目录与插件符号探测
 *
 * 只做「按测试二进制相对位置查找构建产物」，找不到时调用方 GTEST_SKIP；
 * 不建 socket、不建共享内存、不起线程（AD writer 的 init 不建线程）。
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <fstream>
#include <string>
#include <unistd.h>

#include <dlfcn.h>
#include <filesystem>

namespace r102dt {

namespace fs = std::filesystem;

inline fs::path make_tmp_dir(const char* tag)
{
	static std::atomic<int> seq { 0 };
	fs::path p = fs::temp_directory_path()
		/ (std::string("r102dt_") + tag + "_" + std::to_string(::getpid()) + "_"
			+ std::to_string(seq++));
	fs::create_directories(p);
	return p;
}

inline void write_file(const fs::path& p, const std::string& content)
{
	fs::create_directories(p.parent_path());
	std::ofstream ofs(p.string(), std::ios::out | std::ios::trunc);
	ofs << content;
	ofs.close();
}

inline std::string with_slash(const fs::path& p)
{
	std::string s = p.string();
	if (!s.empty() && s.back() != '/')
		s.push_back('/');
	return s;
}

inline std::string bin_dir()
{
	Dl_info info;
	if (dladdr(reinterpret_cast<void*>(&bin_dir), &info) == 0 || info.dli_fname == nullptr)
		return std::string();

	fs::path bin = fs::path(info.dli_fname).parent_path().parent_path();
	return with_slash(bin);
}

inline bool plugin_has_symbols(const std::string& bin, const char* lib,
	const char* sym1, const char* sym2)
{
	if (bin.empty())
		return false;

	const std::string path = bin + lib;
	std::error_code ec;
	if (!fs::exists(path, ec))
		return false;

	void* h = dlopen(path.c_str(), RTLD_NOW);
	if (h == nullptr)
		return false;

	const bool ok = dlsym(h, sym1) != nullptr && dlsym(h, sym2) != nullptr;
	dlclose(h);
	return ok;
}

inline bool ad_writer_ready(const std::string& bin)
{
	return plugin_has_symbols(bin, "libWtDataStorageAD.dylib", "createWriter", "deleteWriter");
}

inline bool index_factory_ready(const std::string& bin)
{
	return plugin_has_symbols(bin, "libWtIndexFactory.dylib",
		"create_index_factory", "delete_index_factory");
}

}	// namespace r102dt
