/*!
 * \file r173_uftcore_fact_test.cpp
 * \brief R173 · UftStrategyMgr 对“有效 dylib 但不含 createStrategyFact”的跳过分支
 *
 * 生产行段：WtUftCore/UftStrategyMgr.cpp:82-86
 *   loadFactories 枚举目录内的 .dylib，dlopen 成功后 dlsym("createStrategyFact") 为空，
 *   必须 dlclose 并 continue；随后整个目录仍返回 true（跳过不是失败）。
 *
 * 边界：临时目录里只放一个指向已构建 libWtShareHelper.dylib 的符号链接，
 *      该库确实可 dlopen，但不导出策略工厂入口；不装载任何策略实现、不起线程。
 */
#include <gtest/gtest.h>

#include "WtUftCore/UftStrategyMgr.h"

#include <mach-o/dyld.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>

namespace fs = std::filesystem;

/*
 * 生产: UftStrategyMgr.cpp:82-86
 */
TEST(R173UftStrategyMgr, ValidDylibWithoutStrategyFactoryIsSkipped)
{
	char exe[4096] = { 0 };
	uint32_t size = sizeof(exe);
	ASSERT_EQ(0, _NSGetExecutablePath(exe, &size));

	const fs::path exePath = fs::canonical(exe);
	// <build>/bin/TestUnits/CoverageTests_uftcore -> <build>/bin/libWtShareHelper.dylib
	const fs::path lib = exePath.parent_path().parent_path() / "libWtShareHelper.dylib";
	ASSERT_TRUE(fs::exists(lib)) << lib;

	const fs::path dir = fs::temp_directory_path()
		/ ("r173_uft_fact_" + std::to_string((long)::getpid()));
	std::error_code ec;
	fs::remove_all(dir, ec);
	fs::create_directories(dir, ec);
	ASSERT_FALSE(ec) << ec.message();

	// 扩展名必须是 .dylib，内容只需是一个能 dlopen 的合法 Mach-O 动态库
	fs::create_symlink(lib, dir / "libnotastrategy.dylib", ec);
	ASSERT_FALSE(ec) << ec.message();

	UftStrategyMgr mgr;
	EXPECT_TRUE(mgr.loadFactories(dir.string().c_str()));
	EXPECT_EQ(nullptr, mgr.createStrategy("nofact.unit", "r173"));

	fs::remove_all(dir, ec);
}
