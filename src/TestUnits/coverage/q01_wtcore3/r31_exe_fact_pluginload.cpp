/**
 * r31_exe_fact_pluginload.cpp —— lane R31 / 插件装载「成功支路」：WtExecuterFactory
 *
 * 本文件只干一件事：把本轮主控实测已经存在的真实产品插件 libWtExeFact.dylib，
 * 用绝对路径喂给 src/WtCore/WtExecuterFactory.cpp::loadFactories，让
 *   L43 dlopen 成功 → L49 get_symbol("createExecFact") 非空
 *   → L56-61 ExeFactInfo 组包（_module_inst/_module_path/_creator/_remover）
 *   → L61 _creator() 真的实例化出 IExecuterFact
 *   → L63 _factories[getName()] 登记 → L65 日志
 * 这条「成功支路」第一次被走到，并把登记之后的六个创建口（三个 create* × 两个重载）
 * 的「工厂已注册」支路全部拿下来。
 *
 * 与既有用例的分工（都是失败支，所以成功支一直红着）：
 *   n02_executer_factory_test.cpp  目录不存在/空目录/跳过子目录与非 .dylib/普通文件传参抛异常
 *                                  + 三个 create* 的「工厂未注册」「名字不足两段」「三段取前两段」
 *   r15_mgr_executers.cpp          dlopen 失败 continue、缺 createExecFact ⇒ free_library
 *   R10 manifest 曾把这块记成「_factories 只能扫目录加载 dylib，无进程内注册口 ⇒ ~210 行不可回收」，
 *   这条判定是错的（见 接缝阻塞面-20261027.md 第 5 节），本文件是反证。
 *
 * 安全边界：
 *  - 只 dlopen libWtExeFact.dylib（产品插件，构造函数只做指针清零，不起线程、不建 socket、
 *    不碰共享内存；otool -L 显示它除系统库与 iconv 外没有其它依赖，拷到临时目录也能独立加载）。
 *  - 插件目录一律由 dladdr/__FILE__ 推出绝对路径，不写相对路径（case_runner 会先 cd 到运行目录）。
 *  - 产物机器上没有该 dylib 时 GTEST_SKIP，不伪造通过。
 *  - free_library 之后不再使用任何符号（本文件不主动 dlclose 成功登记的句柄，
 *    避免「释放镜像后仍持有 _fact 指针」这种真实缺陷被测试掩盖）。
 */
#include <gtest/gtest.h>

#include <dlfcn.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <WtCore/WtExecuterFactory.h>

namespace fs = std::filesystem;

namespace {

USING_NS_WTP;

/** 产品插件真实产物文件名（build-cov/build_x64/Debug/bin/ 下现成，非用例生成） */
const char* kExeFactLib = "libWtExeFact.dylib";
/** 「能 dlopen 但没有 createExecFact」的对照样本，用来在混合目录里压 continue 支 */
const char* kNoExecSymLib = "libWtIndexFactory.dylib";

/**
 * 定位 bin 目录（绝对路径）。三级兜底：
 *   1) 环境变量 WT_COV_PLUGIN_BIN
 *   2) dladdr —— 本函数所在镜像就是测试可执行体 .../bin/TestUnits/CoverageTests_wtcore
 *   3) __FILE__ 上溯到仓库根，再进 build-cov/build_x64/Debug/bin
 * 找不到就返回空路径，由用例 GTEST_SKIP。
 */
const fs::path& pluginBinDir()
{
    static fs::path cached;
    if (!cached.empty())
        return cached;

    if (const char* env = ::getenv("WT_COV_PLUGIN_BIN"))
    {
        fs::path p(env);
        std::error_code ec;
        if (!ec && fs::is_regular_file(p / kExeFactLib, ec))
        {
            cached = p;
            return cached;
        }
    }

    Dl_info info;
    if (::dladdr(reinterpret_cast<const void*>(&pluginBinDir), &info) && info.dli_fname != nullptr)
    {
        std::error_code ec;
        fs::path exe = fs::weakly_canonical(fs::path(info.dli_fname), ec);
        if (!ec && exe.parent_path().filename() == "TestUnits")
        {
            fs::path bin = exe.parent_path().parent_path();
            if (fs::is_regular_file(bin / kExeFactLib))
            {
                cached = bin;
                return cached;
            }
        }
    }

    fs::path up = fs::path(__FILE__).parent_path();
    for (int i = 0; i < 8 && !up.empty(); ++i)
    {
        fs::path cand = up / "build-cov/build_x64/Debug/bin";
        if (fs::is_regular_file(cand / kExeFactLib))
        {
            cached = cand;
            return cached;
        }
        up = up.parent_path();
    }
    return cached;
}

/**
 * 每条用例一个独立临时目录（lane_gate 一用例一进程，但仍按用例名区分，
 * 免得同进程内多条用例的固定文件名产物互相覆盖造出假回归）。
 */
class r31_exe_fact : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        _dir = fs::temp_directory_path() /
            ("r31exe_" + std::string(info->name()) + "_" + std::to_string(::getpid()));
        std::error_code ec;
        fs::remove_all(_dir, ec);
        fs::create_directories(_dir, ec);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(_dir, ec);
    }

    /** 把 bin 目录里的真实插件复制进临时目录；机器上缺产物则返回 false ⇒ 调用方 GTEST_SKIP */
    bool stage(const char* libName, const char* stagedName = nullptr)
    {
        const fs::path& bin = pluginBinDir();
        if (bin.empty() || !fs::is_regular_file(bin / libName))
            return false;
        std::error_code ec;
        fs::copy_file(bin / libName, _dir / (stagedName ? stagedName : libName),
            fs::copy_options::overwrite_existing, ec);
        return !ec;
    }

    void touch(const std::string& name, const std::string& content)
    {
        std::ofstream out((_dir / name).c_str(), std::ios::trunc);
        out << content;
    }

    /** 直接按绝对路径 dlopen + dlsym 的可证伪探针：证明产物里确实有这个导出符号 */
    static bool exportsSymbol(const char* libName, const char* sym)
    {
        const fs::path& bin = pluginBinDir();
        if (bin.empty())
            return false;
        DllHandle h = DLLHelper::load_library((bin / libName).string().c_str());
        if (h == nullptr)
            return false;
        bool ok = DLLHelper::get_symbol(h, sym) != nullptr;
        DLLHelper::free_library(h);      // 释放之后不再使用任何符号
        return ok;
    }

    fs::path _dir;
};

/* ------------------------------------------------------------------ *
 * 装载前置的事实核对：产物确实存在、导出符号确实叫 createExecFact。
 * 这条同时是「成功支路为什么现在可做」的可复现证据。
 * ------------------------------------------------------------------ */
TEST_F(r31_exe_fact, product_dylib_on_disk_exports_the_entry_symbols)
{
    ASSERT_TRUE(pluginBinDir().empty() == false) << "cannot locate build-cov bin dir";
    ASSERT_TRUE(fs::is_regular_file(pluginBinDir() / kExeFactLib))
        << pluginBinDir().string();
    EXPECT_TRUE(exportsSymbol(kExeFactLib, "createExecFact"));
    EXPECT_TRUE(exportsSymbol(kExeFactLib, "deleteExecFact"));
    // 对照样本必须是「能加载但没有执行器入口」，否则后面的混合目录用例失去意义
    EXPECT_TRUE(exportsSymbol(kNoExecSymLib, "create_index_factory"));
    EXPECT_FALSE(exportsSymbol(kNoExecSymLib, "createExecFact"));
}

/* ------------------------------------------------------------------ *
 * 成功支路主体：dlopen → creator 非空 → _creator() 实例化 → 登记 → 日志
 * ------------------------------------------------------------------ */
TEST_F(r31_exe_fact, loadfactories_with_real_dylib_reaches_creator_and_registers)
{
    ASSERT_TRUE(stage(kExeFactLib)) << "libWtExeFact.dylib missing on this machine";

    WtExecuterFactory factory;
    // L17 exists → L25 目录迭代 → L43 dlopen 成功 → L49 creator 非空
    // → L56-61 组包并真的调用 _creator() → L63 以 _fact->getName() 为键登记 → L65 日志
    ASSERT_TRUE(factory.loadFactories(_dir.string().c_str()));

    // 可证伪点：只有 L61 真的 new 出 WtExeFact、L63 真的登记成功，这里才拿得到单元。
    // 单元名 "WtTWapExeUnit" 是插件侧 WtExeFact::createExeUnit 唯一认得的普通名之一。
    ExecuteUnitPtr unit = factory.createExeUnit("WtExeFact", "WtTWapExeUnit");
    ASSERT_NE(unit.get(), nullptr) << "success branch never registered the real factory";
    ASSERT_NE(unit->self(), nullptr);
    EXPECT_STREQ(unit->self()->getFactName(), "WtExeFact");
    EXPECT_STREQ(unit->self()->getName(), "WtTWapExeUnit");
}

TEST_F(r31_exe_fact, registered_factory_creates_every_plain_unit_name)
{
    ASSERT_TRUE(stage(kExeFactLib));
    WtExecuterFactory factory;
    ASSERT_TRUE(factory.loadFactories(_dir.string().c_str()));

    // 工厂已登记之后，createExeUnit(fact,unit) 的 unit==NULL 支与成功支都要走到
    const char* plain[] = { "WtTWapExeUnit", "WtMinImpactExeUnit",
        "WtStockMinImpactExeUnit", "WtVWapExeUnit", "WtStockVWapExeUnit" };
    for (const char* nm : plain)
    {
        ExecuteUnitPtr u = factory.createExeUnit("WtExeFact", nm);
        ASSERT_NE(u.get(), nullptr) << nm;
        ASSERT_NE(u->self(), nullptr) << nm;
        EXPECT_STREQ(u->self()->getFactName(), "WtExeFact") << nm;
    }
    EXPECT_EQ(factory.createExeUnit("WtExeFact", "NoSuchUnit").get(), nullptr);
}

TEST_F(r31_exe_fact, registered_factory_creates_diff_unit_and_never_arbi)
{
    ASSERT_TRUE(stage(kExeFactLib));
    WtExecuterFactory factory;
    ASSERT_TRUE(factory.loadFactories(_dir.string().c_str()));

    // 差价口：真插件只认 WtDiffMinImpactExeUnit
    ExecuteUnitPtr diff = factory.createDiffExeUnit("WtExeFact", "WtDiffMinImpactExeUnit");
    ASSERT_NE(diff.get(), nullptr);
    EXPECT_STREQ(diff->self()->getName(), "WtDiffMinImpactExeUnit");
    EXPECT_EQ(factory.createDiffExeUnit("WtExeFact", "WtTWapExeUnit").get(), nullptr);

    // 套利口：WtExeFact::createArbiExeUnit 恒返 NULL ⇒ 走「已登记但造不出」的错误日志支
    EXPECT_EQ(factory.createArbiExeUnit("WtExeFact", "anything").get(), nullptr);
}

TEST_F(r31_exe_fact, dotted_name_overloads_hit_the_registered_factory)
{
    ASSERT_TRUE(stage(kExeFactLib));
    WtExecuterFactory factory;
    ASSERT_TRUE(factory.loadFactories(_dir.string().c_str()));

    // 单参重载：StrUtil::split(name,".") → 前两段查表 → 已登记 ⇒ 成功支
    ExecuteUnitPtr u = factory.createExeUnit("WtExeFact.WtTWapExeUnit");
    ASSERT_NE(u.get(), nullptr);
    EXPECT_STREQ(u->self()->getName(), "WtTWapExeUnit");

    ExecuteUnitPtr d = factory.createDiffExeUnit("WtExeFact.WtDiffMinImpactExeUnit");
    ASSERT_NE(d.get(), nullptr);

    // 三段名：只取前两段，第三段被忽略（n02 只在没有登记的情况下测过这个形状）
    ExecuteUnitPtr t = factory.createExeUnit("WtExeFact.WtVWapExeUnit.trailing");
    ASSERT_NE(t.get(), nullptr);
    EXPECT_STREQ(t->self()->getName(), "WtVWapExeUnit");

    // 已登记工厂 + 点号名 + 未知单元 ⇒ unit==NULL 错误日志支
    EXPECT_EQ(factory.createExeUnit("WtExeFact.NoSuchUnit").get(), nullptr);
    EXPECT_EQ(factory.createDiffExeUnit("WtExeFact.NoSuchUnit").get(), nullptr);
    EXPECT_EQ(factory.createArbiExeUnit("WtExeFact.Anything").get(), nullptr);

    // 已登记工厂 + 不足两段 ⇒ 仍然在 split 处早退
    EXPECT_EQ(factory.createExeUnit("WtExeFact").get(), nullptr);
    EXPECT_EQ(factory.createDiffExeUnit("WtExeFact").get(), nullptr);
    EXPECT_EQ(factory.createArbiExeUnit("WtExeFact").get(), nullptr);
}

TEST_F(r31_exe_fact, unknown_factory_name_still_misses_after_success)
{
    ASSERT_TRUE(stage(kExeFactLib));
    WtExecuterFactory factory;
    ASSERT_TRUE(factory.loadFactories(_dir.string().c_str()));

    // 登记进去的键是 "WtExeFact"，别的名字必须仍然查不到（可证伪：若键写错这里就红）
    EXPECT_EQ(factory.createExeUnit("libWtExeFact", "WtTWapExeUnit").get(), nullptr);
    EXPECT_EQ(factory.createExeUnit("wtExeFact", "WtTWapExeUnit").get(), nullptr);
    EXPECT_EQ(factory.createDiffExeUnit("other", "WtDiffMinImpactExeUnit").get(), nullptr);
    EXPECT_EQ(factory.createArbiExeUnit("other", "x").get(), nullptr);
    EXPECT_EQ(factory.createExeUnit("other.WtTWapExeUnit").get(), nullptr);
}

/* ------------------------------------------------------------------ *
 * 混合目录：成功支与三个 continue 支在同一次扫描里交错
 * ------------------------------------------------------------------ */
TEST_F(r31_exe_fact, mixed_directory_scans_success_and_skips_alongside)
{
    ASSERT_TRUE(stage(kExeFactLib, "libZExeFact.dylib"));
    ASSERT_TRUE(stage(kNoExecSymLib, "libANoFact.dylib"));
    fs::create_directories(_dir / "subdir.dylib");           // 目录名带 .dylib 也要跳过
    touch("notes.txt", "not a library");
    touch("libAfake.dylib", "garbage, not mach-o");

    WtExecuterFactory factory;
    ASSERT_TRUE(factory.loadFactories(_dir.string().c_str()));

    // 真插件那一项登记成功（按字典序 libA*/libANoFact/libZExeFact/subdir 交错走支）
    ExecuteUnitPtr u = factory.createExeUnit("WtExeFact", "WtTWapExeUnit");
    ASSERT_NE(u.get(), nullptr);
    // 无入口符号的那份走了 free_library + continue
    EXPECT_EQ(factory.createExeUnit("IndexFactory", "x").get(), nullptr);
}

TEST_F(r31_exe_fact, reloading_same_directory_overwrites_the_entry)
{
    ASSERT_TRUE(stage(kExeFactLib));
    WtExecuterFactory factory;
    ASSERT_TRUE(factory.loadFactories(_dir.string().c_str()));
    // 第二次扫描：dlopen 同一路径返回同一句柄（引用计数），L63 以同名键覆盖旧记录
    ASSERT_TRUE(factory.loadFactories(_dir.string().c_str()));

    ExecuteUnitPtr u = factory.createExeUnit("WtExeFact", "WtMinImpactExeUnit");
    ASSERT_NE(u.get(), nullptr);
    EXPECT_STREQ(u->self()->getFactName(), "WtExeFact");
}

/* ------------------------------------------------------------------ *
 * 装载成功之后才拿得到的生命周期面：ExeUnitWrapper 经插件工厂回收
 * ------------------------------------------------------------------ */
TEST_F(r31_exe_fact, wrapper_from_loaded_factory_releases_through_plugin)
{
    ASSERT_TRUE(stage(kExeFactLib));
    WtExecuterFactory factory;
    ASSERT_TRUE(factory.loadFactories(_dir.string().c_str()));

    ExecuteUnitPtr shared = factory.createExeUnit("WtExeFact", "WtVWapExeUnit");
    ASSERT_NE(shared.get(), nullptr);
    ExecuteUnit* raw = shared->self();
    ASSERT_NE(raw, nullptr);

    {
        ExecuteUnitPtr alias = shared;                 // shared_ptr 双引用
        EXPECT_EQ(alias->self(), raw);
        EXPECT_EQ(alias.use_count(), 2);
    }                                     // alias 析构：引用计数减到 1，不该触碰插件
    EXPECT_EQ(shared->self(), raw);
    shared.reset();                     // 最后一个引用析构 ⇒ ExeUnitWrapper::~ ⇒ 插件 deleteExeUnit
    EXPECT_EQ(shared.get(), nullptr);
}

TEST_F(r31_exe_fact, arbi_unit_ptr_stays_empty_and_wrapper_not_built)
{
    ASSERT_TRUE(stage(kExeFactLib));
    WtExecuterFactory factory;
    ASSERT_TRUE(factory.loadFactories(_dir.string().c_str()));

    // 已登记工厂但插件不产套利单元：返回的空 ExecuteUnitPtr 不得被解引用
    ExecuteUnitPtr u = factory.createArbiExeUnit("WtExeFact", "whatever");
    EXPECT_TRUE(u.get() == nullptr);
    EXPECT_EQ(u.use_count(), 0);
}

} // namespace
