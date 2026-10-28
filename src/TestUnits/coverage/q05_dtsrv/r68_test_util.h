/*!
 * \file r68_test_util.h
 * \brief R68 lane（第十三波 · WtDtCore 补测）自用的小工具，仅本 lane 的 r68_*.cpp 使用。
 *
 * 约定：
 *  - 临时目录只落在系统临时目录，名称带 r68_ 前缀 + pid + 序号；
 *  - 不建 socket、不建共享内存映射、不起线程、不读挂钟；
 *  - 指标工厂插件只做「按测试二进制相对位置查找」，找不到时由调用方 GTEST_SKIP。
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace r68 {

namespace fs = std::filesystem;

inline fs::path make_tmp_dir(const char* tag)
{
	static std::atomic<int> seq { 0 };
	fs::path p = fs::temp_directory_path()
		/ (std::string("r68_") + tag + "_" + std::to_string(::getpid()) + "_"
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

/*
 * 查找覆盖率构建产出的第一方指标工厂插件 libWtIndexFactory.dylib。
 * 只依赖测试二进制自身的相对位置（<bin>/TestUnits/CoverageTests_dtcore ->
 * <bin>/libWtIndexFactory.dylib），不做任何外部下载/安装。
 */
inline fs::path find_index_factory_plugin()
{
	std::vector<fs::path> cands;

#ifdef __APPLE__
	char buf[4096];
	uint32_t size = sizeof(buf);
	if (_NSGetExecutablePath(buf, &size) == 0)
	{
		std::error_code ec;
		fs::path exe = fs::weakly_canonical(fs::path(buf), ec);
		if (!ec)
		{
			// CoverageTests_dtcore 位于 <bin>/TestUnits/ 下
			cands.push_back(exe.parent_path().parent_path() / "libWtIndexFactory.dylib");
			cands.push_back(exe.parent_path() / "libWtIndexFactory.dylib");
		}
	}
#endif

	cands.push_back(fs::current_path() / "libWtIndexFactory.dylib");
	cands.push_back(fs::current_path() / "build-cov/build_x64/Debug/bin/libWtIndexFactory.dylib");

	for (const fs::path& c : cands)
	{
		std::error_code ec;
		if (fs::is_regular_file(c, ec))
			return fs::weakly_canonical(c, ec);
	}
	return fs::path();
}

}	// namespace r68
