/*!
 * \file r11_share_manager_test.cpp
 * \brief 覆盖率迭代01 第七波 R11(WtUftCore) - ShareManager 未初始化态的入参拒绝
 *
 * 覆盖: ShareManager.cpp 里 initialize() 的模块缺失分支, 以及 _inited==false 时
 * 19 个公开读写/分配入口的提前返回(set_value x6 / get_value x6 / allocate_value x6
 * + init_domain / commit_param_watcher / start_watching)。
 *
 * 不接真实共享内存: 本用例只喂不存在的路径, _inited 恒为 false,
 * 因此不会走到 _init_master/_set_double 等真实 shm 函数指针。
 * 已初始化后的真实读写面需要接缝, 见 docs/迭代计划/迭代01/bugs/R11_wtuftcore.md。
 */
#include <boost/noncopyable.hpp>

#include "WtUftCore/ShareManager.h"

#include "Includes/WTSVariant.hpp"
#include "Includes/WTSMarcos.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>

using namespace wtp;

namespace {

//本用例唯一依赖的"确定不存在"的模块路径: 目录本身存在, 文件名带 pid 保证不撞车
std::string missingModule()
{
	std::filesystem::path dir = std::filesystem::temp_directory_path();
	return (dir / ("r11_no_such_share_module_" + std::to_string((long)::getpid()) + ".dylib")).string();
}

//ShareManager 是 Meyers 单例, 全进程只有一个实例; 本用例只读它的未初始化态
ShareManager& shm() { return ShareManager::self(); }

//////////////////////////////////////////////////////////////////////////
// initialize: 模块不存在 -> 记 warn 并返回 false, _inited 保持 false
TEST(R11ShareManager, initializeMissingModuleReturnsFalse)
{
	const std::string mod = missingModule();
	std::error_code ec;
	ASSERT_FALSE(std::filesystem::exists(mod, ec));

	EXPECT_FALSE(shm().initialize(mod.c_str()));
	//重复调用仍然是 false(因为 _inited 从未置真, 不会命中 `if(_inited) return true`)
	EXPECT_FALSE(shm().initialize(mod.c_str()));

	//未初始化 -> 所有真实读写入口必须拒绝, 而不是调用空函数指针
	EXPECT_FALSE(shm().start_watching(0));
	EXPECT_FALSE(shm().init_domain("r11domain"));
	EXPECT_FALSE(shm().commit_param_watcher("r11section"));
}

//////////////////////////////////////////////////////////////////////////
// set_value 六个重载: 未初始化全部返回 false
TEST(R11ShareManager, setValueRejectedWhenNotInited)
{
	EXPECT_FALSE(shm().set_value("sec", "k_str", "hello"));
	EXPECT_FALSE(shm().set_value("sec", "k_i32", (int32_t)-2147483648));
	EXPECT_FALSE(shm().set_value("sec", "k_i64", (int64_t)-9223372036854775807LL - 1));
	EXPECT_FALSE(shm().set_value("sec", "k_u32", (uint32_t)4294967295U));
	EXPECT_FALSE(shm().set_value("sec", "k_u64", (uint64_t)18446744073709551615ULL));
	EXPECT_FALSE(shm().set_value("sec", "k_dbl", 3.25));
}

//////////////////////////////////////////////////////////////////////////
// get_value 六个重载: 未初始化原样回吐调用方给的默认值
TEST(R11ShareManager, getValueFallsBackToDefaultWhenNotInited)
{
	EXPECT_STREQ("fallback", shm().get_value("sec", "k_str", "fallback"));
	//缺省默认值: 字符串重载的 defVal 默认是 ""
	EXPECT_STREQ("", shm().get_value("sec", "k_str_empty", (const char*)nullptr == nullptr ? "" : ""));

	EXPECT_EQ((int32_t)-7, shm().get_value("sec", "k_i32", (int32_t)-7));
	EXPECT_EQ((int64_t)-1234567890123LL, shm().get_value("sec", "k_i64", (int64_t)-1234567890123LL));
	EXPECT_EQ((uint32_t)4000000007U, shm().get_value("sec", "k_u32", (uint32_t)4000000007U));
	EXPECT_EQ((uint64_t)18446744073709551615ULL, shm().get_value("sec", "k_u64", (uint64_t)18446744073709551615ULL));
	EXPECT_DOUBLE_EQ(1.5, shm().get_value("sec", "k_dbl", 1.5));

	//数值重载的缺省默认值是 0
	EXPECT_EQ((int32_t)0, shm().get_value("sec", "k_i32_zero", (int32_t)0));
	EXPECT_EQ((uint32_t)0, shm().get_value("sec", "k_u32_zero", (uint32_t)0));
}

//////////////////////////////////////////////////////////////////////////
// allocate_value 六个重载: 未初始化返回 nullptr(不是崩溃, 也不是野指针)
TEST(R11ShareManager, allocateValueReturnsNullWhenNotInited)
{
	EXPECT_EQ(nullptr, shm().allocate_value("sec", "a_str", "init", false, false));
	EXPECT_EQ(nullptr, shm().allocate_value("sec", "a_i32", (int32_t)11, false, false));
	EXPECT_EQ(nullptr, shm().allocate_value("sec", "a_i64", (int64_t)12, false, false));
	EXPECT_EQ(nullptr, shm().allocate_value("sec", "a_u32", (uint32_t)13, false, false));
	EXPECT_EQ(nullptr, shm().allocate_value("sec", "a_u64", (uint64_t)14, false, false));
	EXPECT_EQ(nullptr, shm().allocate_value("sec", "a_dbl", 15.5, false, false));

	//isExchg=true 走交换区分支, 未初始化同样拒绝
	EXPECT_EQ(nullptr, shm().allocate_value("sec", "x_str", "init", true, true));
	EXPECT_EQ(nullptr, shm().allocate_value("sec", "x_dbl", 1.0, true, true));
	EXPECT_EQ(nullptr, shm().allocate_value("sec", "x_u64", (uint64_t)9, true, true));
}

//////////////////////////////////////////////////////////////////////////
// 单例身份: self() 两次必须同一个对象(否则上面的"未初始化态"结论不成立)
TEST(R11ShareManager, selfIsStableSingleton)
{
	ShareManager& a = ShareManager::self();
	ShareManager& b = ShareManager::self();
	EXPECT_EQ(&a, &b);

	//set_engine 只是存指针, 不改变任何拒绝行为
	WtUftEngine* dummy = nullptr;
	a.set_engine(dummy);
	EXPECT_FALSE(a.init_domain("r11domain2"));
	EXPECT_FALSE(a.commit_param_watcher("r11section2"));
	EXPECT_FALSE(a.set_value("sec2", "k", (int32_t)1));
}

} // namespace
