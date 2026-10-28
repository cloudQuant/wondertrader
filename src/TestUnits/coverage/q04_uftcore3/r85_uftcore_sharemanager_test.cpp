/*!
 * \file r85_uftcore_sharemanager_test.cpp
 * \brief 迭代01 第十六波 R85 - ShareManager 模块存在但不可装载时的初始化尾段
 *
 * 覆盖（只读生产代码）:
 *  - ShareManager.cpp:31-66 initialize 的「文件存在 -> load_library 失败 ->
 *    _inited=false -> 15 个 get_symbol 全部返回 NULL -> 返回 false」路径
 *    （R11 只覆盖了文件不存在的早退，这里补文件存在但非共享库的形态）
 *
 * ShareManager 是进程级单例，_inited 只能由成功装载 WtShareHelper 置真；
 * 本用例用「存在的普通文本文件」保证装载失败，单例仍停留在未初始化态，
 * 不会影响 r11 对未初始化守卫的断言，也不创建任何共享内存段。
 */
#include "WtUftCore/ShareManager.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <atomic>
#include <fstream>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path makeTextModule()
{
	static std::atomic<int> seq { 0 };
	const fs::path p = fs::temp_directory_path()
		/ ("r85_sharemgrmod_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
	std::ofstream ofs(p.string().c_str(), std::ios::out | std::ios::trunc);
	ofs << "not a dynamic library";
	ofs.close();
	return p;
}

}	// namespace

TEST(R85UftShareManager, existing_unloadable_module_still_returns_false)
{
	const fs::path module = makeTextModule();
	std::error_code ec;
	ASSERT_TRUE(fs::exists(module, ec));

	// 文件存在 -> dlopen 失败 -> _inited 保持 false -> 全部符号指针为空 -> false
	EXPECT_FALSE(ShareManager::self().initialize(module.string().c_str()));

	// 未初始化态下读写仍回到默认值，证明单例没有被带进半初始化态
	EXPECT_DOUBLE_EQ(3.5, ShareManager::self().get_value("sec", "k", 3.5));
	EXPECT_EQ(nullptr, ShareManager::self().allocate_value("sec", "k", 1.0));

	fs::remove(module, ec);
}
