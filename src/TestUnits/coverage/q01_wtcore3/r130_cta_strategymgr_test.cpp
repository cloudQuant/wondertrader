/**
 * r130_cta_strategymgr_test.cpp — lane R130 / wave21
 *
 * 目标：src/WtCore/CtaStrategyMgr.cpp（基线 5/78 未覆盖）与
 *       CtaStrategyMgr.h 的 CtaStraWrapper。
 * 该管理器只由插件装配链调用，此前没有直接用例。
 *
 * 覆盖行段：
 *   loadFactories：目录不存在 :31-35、空目录 :37-91、真实工厂装载 :58-90
 *   createStrategy(factname,unitname,id)：未知工厂 :99-101、成功 :103-106
 *   createStrategy(name,id)：名称不含点 :111-112、未知工厂 :117-119、成功 :121-124
 *   getStrategy：命中/未命中 :129-137
 *   CtaStraWrapper：self :46、析构 deleteStrategy :35-40
 *
 * 只 dlopen 本机构建产物 `libWtCtaStraFact.dylib` 的软链，不联网、不起线程。
 * 临时目录由 r130::TempEnv 管理，析构整棵删除。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <string>

#include "r130_env.h"
#include "r90_env.h"

#include <Includes/CtaStrategyDefs.h>
#include <WtCore/CtaStrategyMgr.h>

using namespace wtp;

namespace {

namespace sfs = std::filesystem;

/// 临时目录里只放一个 CTA 策略工厂软链，避免 loadFactories 误加载其他插件
bool makeFactoryDir(const std::string& dir, std::string& outDir)
{
    std::error_code ec;
    sfs::create_directories(dir, ec);
    if (ec)
        return false;

    const std::string bin = r90::find_bin_dir();
    if (bin.empty())
        return false;

    sfs::path src = sfs::path(bin) / "libWtCtaStraFact.dylib";
    if (!sfs::exists(src, ec))
        return false;

    sfs::path dst = sfs::path(dir) / "libWtCtaStraFact.dylib";
    sfs::remove(dst, ec);
    sfs::create_symlink(src, dst, ec);
    if (ec)
        return false;

    outDir = dir;
    return true;
}

}  // namespace

//////////////////////////////////////////////////////////////////
// 目录不存在 -> false；空目录 -> true 且 0 个工厂
// src/WtCore/CtaStrategyMgr.cpp:31-91
//////////////////////////////////////////////////////////////////
TEST(R130CtaStrategyMgr, MissingDirectoryFailsAndEmptyDirectorySucceeds)
{
    r130::TempEnv env("r130stra");

    const std::string missing = env.dir() + "no_such_dir";
    CtaStrategyMgr mgr1;
    EXPECT_FALSE(mgr1.loadFactories(missing.c_str()));

    // TempEnv 刚建的空目录
    CtaStrategyMgr mgr2;
    EXPECT_TRUE(mgr2.loadFactories(env.dir().c_str()));
    EXPECT_EQ(nullptr, mgr2.getStrategy("nobody").get());
    EXPECT_EQ(nullptr, mgr2.createStrategy("WtCtaStraFact.DualThrust", "r130empty").get());
}

//////////////////////////////////////////////////////////////////
// 真实工厂装载 + 两个 createStrategy 重载 + getStrategy 命中
// src/WtCore/CtaStrategyMgr.cpp:58-90, 103-106, 121-124, 129-137
//////////////////////////////////////////////////////////////////
TEST(R130CtaStrategyMgr, LoadsFactoryAndCreatesStrategies)
{
    r130::TempEnv env("r130stra");
    std::string dir;
    if (!makeFactoryDir(env.dir() + "factories", dir))
        GTEST_SKIP() << "libWtCtaStraFact.dylib not found";

    CtaStrategyMgr mgr;
    ASSERT_TRUE(mgr.loadFactories(dir.c_str()));

    // 完整名 "工厂.策略"
    CtaStrategyPtr s1 = mgr.createStrategy("WtCtaStraFact.DualThrust", "r130cta1");
    ASSERT_NE(nullptr, s1.get());
    ASSERT_NE(nullptr, s1->self());
    EXPECT_STREQ("r130cta1", s1->self()->id());

    CtaStrategyPtr got = mgr.getStrategy("r130cta1");
    ASSERT_NE(nullptr, got.get());
    EXPECT_EQ(s1.get(), got.get());

    // 工厂名/策略名分开的版本
    CtaStrategyPtr s2 = mgr.createStrategy("WtCtaStraFact", "DualThrust", "r130cta2");
    ASSERT_NE(nullptr, s2.get());
    ASSERT_NE(nullptr, s2->self());
    EXPECT_STREQ("r130cta2", s2->self()->id());

    EXPECT_EQ(nullptr, mgr.getStrategy("r130cta_absent").get());
}

//////////////////////////////////////////////////////////////////
// 失败矩阵：名称无点、未知工厂、未知策略
// src/WtCore/CtaStrategyMgr.cpp:99-101, 111-112, 117-119, 121-124
//////////////////////////////////////////////////////////////////
TEST(R130CtaStrategyMgr, CreateStrategyFailureMatrix)
{
    r130::TempEnv env("r130stra");
    std::string dir;
    if (!makeFactoryDir(env.dir() + "factories", dir))
        GTEST_SKIP() << "libWtCtaStraFact.dylib not found";

    CtaStrategyMgr mgr;
    ASSERT_TRUE(mgr.loadFactories(dir.c_str()));

    // 不含 "."
    EXPECT_EQ(nullptr, mgr.createStrategy("WtCtaStraFactDualThrust", "r130bad1").get());

    // 工厂未注册
    EXPECT_EQ(nullptr, mgr.createStrategy("NoSuchFact.DualThrust", "r130bad2").get());
    EXPECT_EQ(nullptr, mgr.createStrategy("NoSuchFact", "DualThrust", "r130bad3").get());

    // 工厂存在但策略名不支持：包装器存在、内部 self() 为空
    CtaStrategyPtr bad = mgr.createStrategy("WtCtaStraFact", "NoSuchStrategy", "r130bad4");
    ASSERT_NE(nullptr, bad.get());
    EXPECT_EQ(nullptr, bad->self());

    // 失败结果同样进入 _strategies，可按 id 取回
    CtaStrategyPtr got = mgr.getStrategy("r130bad4");
    ASSERT_NE(nullptr, got.get());
    EXPECT_EQ(bad.get(), got.get());
}
