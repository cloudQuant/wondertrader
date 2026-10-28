/**
 * r31_stra_mgr_pluginload.cpp —— lane R31 / 插件装载「成功支路」：CtaStrategyMgr + SelStrategyMgr
 *
 * 覆盖面（对照 docs/迭代计划/迭代01/manifests/api-brief-R15.md 里同名面的失败支）：
 *   src/WtCore/CtaStrategyMgr.cpp
 *     L57-92  真 dylib ⇒ dlopen 成功 → get_symbol("createStrategyFact") 非空
 *             → creator() 真的 new 出 WtStraFact → L71-78 以 fact->getName() 为键登记
 *             → L90 count 日志 → L92 return true
 *     L101-104 / L120-123  工厂已登记后的两个 createStrategy 重载成功支（含 _strategies 登记）
 *     L126-133  getStrategy 命中支
 *   src/WtCore/SelStrategyMgr.cpp
 *     L57-66 / L84-86  SEL 入口符号名 createMfStrategyFact 在全仓库无出处 ⇒
 *              任何可 dlopen 的插件都走 creator==NULL → free_library → continue
 *     L90-92、L97-99、L107-118、L126-133  空表各支
 *
 * 为什么这里只把 SelStrategyMgr 拿下失败面：见 bugs/R31_pluginload.md（BUG-R31-001，
 * SelStrategyMgr 找的导出符号与 WtSelStraFact 实际导出不一致，SEL 插件在本仓库任何一份
 * 产物里都装载不进来）。HftStrategyMgr 的「成功支」只能用跨插件同名符号 createStrategyFact
 * 把 CTA 工厂当 IHftStrategyFact 用（类型双关），属主控提示的符号复用面，收进 DISABLED_。
 *
 * 安全边界同 r31_exe_fact_pluginload.cpp：绝对路径、只 dlopen 产品插件、
 * 插件构造函数无副作用（指针清零）、free 之后不再使用符号、缺产物则 GTEST_SKIP。
 */
#include <gtest/gtest.h>

#include <dlfcn.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <WtCore/CtaStrategyMgr.h>
#include <WtCore/SelStrategyMgr.h>
#include <WtCore/HftStrategyMgr.h>

namespace fs = std::filesystem;

namespace {

USING_NS_WTP;

/** bin 目录下的真实产品插件 */
const char* kCtaFactLib = "libWtCtaStraFact.dylib";      // 导出 createStrategyFact，工厂名 WtCtaStraFact
const char* kTwoMaLib = "libWtCtaTwoMaFact.dylib";       // 导出 createStrategyFact，工厂名 WtCtaTwoMaFact
const char* kExeFactLib = "libWtExeFact.dylib";          // 只有 createExecFact
const char* kIdxFactLib = "libWtIndexFactory.dylib";     // 只有 create_index_factory

/** 本机全部产品插件（含子目录形态）；用于「createMfStrategyFact 无出处」的可证伪核对 */
const char* kAllProductLibs[] = {
    "libWtCtaStraFact.dylib", "libWtCtaTwoMaFact.dylib", "libWtExeFact.dylib",
    "libWtRiskMonFact.dylib", "libWtIndexFactory.dylib", "libWtDataStorage.dylib",
    "libWtDataStorageAD.dylib", "libWtMsgQue.dylib", "libWtShareHelper.dylib",
    "WtUftRunner/libWtUftStraFact.dylib", "WtPorter/libWtRiskMonFact.dylib",
    "WtPorter/executer/libWtExeFact.dylib"
};

const fs::path& pluginBinDir()
{
    static fs::path cached;
    if (!cached.empty())
        return cached;

    if (const char* env = ::getenv("WT_COV_PLUGIN_BIN"))
    {
        fs::path p(env);
        if (fs::is_regular_file(p / kCtaFactLib))
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
            if (fs::is_regular_file(bin / kCtaFactLib))
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
        if (fs::is_regular_file(cand / kCtaFactLib))
        {
            cached = cand;
            return cached;
        }
        up = up.parent_path();
    }
    return cached;
}

class r31_stra_mgr : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        _dir = fs::temp_directory_path() /
            ("r31stra_" + std::string(info->name()) + "_" + std::to_string(::getpid()));
        std::error_code ec;
        fs::remove_all(_dir, ec);
        fs::create_directories(_dir, ec);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(_dir, ec);
    }

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

    static bool exportsSymbol(const std::string& relName, const char* sym)
    {
        const fs::path& bin = pluginBinDir();
        if (bin.empty())
            return false;
        fs::path p = bin / relName;
        std::error_code ec;
        if (!fs::is_regular_file(p, ec))
            return false;
        DllHandle h = DLLHelper::load_library(p.string().c_str());
        if (h == nullptr)
            return false;
        bool ok = DLLHelper::get_symbol(h, sym) != nullptr;
        DLLHelper::free_library(h);
        return ok;
    }

    fs::path _dir;
};

/* ------------------------------------------------------------------ *
 * 事实核对：两份 CTA 插件各自导出 createStrategyFact，且没有任何产物导出
 * SelStrategyMgr 要找的 createMfStrategyFact。
 * ------------------------------------------------------------------ */
TEST_F(r31_stra_mgr, cta_plugins_export_create_strategy_fact_entry)
{
    ASSERT_FALSE(pluginBinDir().empty()) << "cannot locate build-cov bin dir";
    EXPECT_TRUE(exportsSymbol(kCtaFactLib, "createStrategyFact"));
    EXPECT_TRUE(exportsSymbol(kCtaFactLib, "deleteStrategyFact"));
    EXPECT_TRUE(exportsSymbol(kTwoMaLib, "createStrategyFact"));
    EXPECT_TRUE(exportsSymbol(kTwoMaLib, "deleteStrategyFact"));
    // 反例：执行器/指标插件没有策略入口
    EXPECT_FALSE(exportsSymbol(kExeFactLib, "createStrategyFact"));
    EXPECT_FALSE(exportsSymbol(kIdxFactLib, "createStrategyFact"));
}

TEST_F(r31_stra_mgr, no_product_plugin_exports_the_sel_entry_symbol)
{
    ASSERT_FALSE(pluginBinDir().empty()) << "cannot locate build-cov bin dir";
    size_t seen = 0;
    for (const char* rel : kAllProductLibs)
    {
        fs::path p = pluginBinDir() / rel;
        std::error_code ec;
        if (!fs::is_regular_file(p, ec))
            continue;           // 该机器上没产出这一份，跳过而不是判失败
        ++seen;
        // SelStrategyMgr.cpp:61 只认这个名字；本用例证明它在本机任何产物里都不存在
        EXPECT_FALSE(exportsSymbol(rel, "createMfStrategyFact")) << rel;
    }
    EXPECT_GE(seen, 6u) << "too few product plugins on this machine to make the claim";
}

/* ------------------------------------------------------------------ *
 * CtaStrategyMgr 成功支路
 * ------------------------------------------------------------------ */
TEST_F(r31_stra_mgr, cta_load_reaches_creator_and_registers_real_factory)
{
    ASSERT_TRUE(stage(kCtaFactLib));

    CtaStrategyMgr mgr;
    // L57 dlopen 成功 → L61 creator 非空 → L68 creator() 真的 new 出 WtStraFact
    // → L71-76 登记（键为 fact->getName()）→ L78 日志 → L90/L92
    ASSERT_TRUE(mgr.loadFactories(_dir.string().c_str()));

    CtaStrategyPtr stra = mgr.createStrategy("WtCtaStraFact", "DualThrust", "r31_cta_1");
    ASSERT_NE(stra.get(), nullptr) << "success branch never registered the CTA factory";
    ASSERT_NE(stra->self(), nullptr);
    EXPECT_STREQ(stra->self()->getFactName(), "WtCtaStraFact");
    EXPECT_STREQ(stra->self()->getName(), "DualThrust");
}

TEST_F(r31_stra_mgr, cta_two_plugins_in_one_dir_register_under_their_own_names)
{
    ASSERT_TRUE(stage(kCtaFactLib, "libAOne.dylib"));
    ASSERT_TRUE(stage(kTwoMaLib, "libATwo.dylib"));

    CtaStrategyMgr mgr;
    ASSERT_TRUE(mgr.loadFactories(_dir.string().c_str()));

    // 两份插件都导出同名的 createStrategyFact。若 get_symbol 发生跨镜像符号复用，
    // 下面两个工厂名只会剩下一个 —— 这条断言就是用来证伪「符号复用」的。
    CtaStrategyPtr dt = mgr.createStrategy("WtCtaStraFact", "DualThrust", "id_dt");
    ASSERT_NE(dt.get(), nullptr) << "WtCtaStraFact missing => symbol reuse across images";
    ASSERT_NE(dt->self(), nullptr);
    EXPECT_STREQ(dt->self()->getFactName(), "WtCtaStraFact");

    CtaStrategyPtr ma = mgr.createStrategy("WtCtaTwoMaFact", "TwoMa", "id_ma");
    ASSERT_NE(ma.get(), nullptr) << "WtCtaTwoMaFact missing => symbol reuse across images";
    ASSERT_NE(ma->self(), nullptr);
    EXPECT_STREQ(ma->self()->getFactName(), "WtCtaTwoMaFact");
    EXPECT_STREQ(ma->self()->getName(), "TwoMa");
}

TEST_F(r31_stra_mgr, cta_dotted_name_overload_creates_and_registers)
{
    ASSERT_TRUE(stage(kTwoMaLib));

    CtaStrategyMgr mgr;
    ASSERT_TRUE(mgr.loadFactories(_dir.string().c_str()));

    CtaStrategyPtr stra = mgr.createStrategy("WtCtaTwoMaFact.TwoMa", "r31_dotted");
    ASSERT_NE(stra.get(), nullptr);
    ASSERT_NE(stra->self(), nullptr);

    // L122 _strategies[id] 登记后，getStrategy 必须命中同一个包装对象
    CtaStrategyPtr got = mgr.getStrategy("r31_dotted");
    ASSERT_NE(got.get(), nullptr);
    EXPECT_EQ(got.get(), stra.get());
    EXPECT_EQ(got->self(), stra->self());

    // 三段名只取前两段
    CtaStrategyPtr three = mgr.createStrategy("WtCtaTwoMaFact.TwoMa.tail", "r31_three");
    ASSERT_NE(three.get(), nullptr);

    // 不足两段 ⇒ 在 split 处早退，不进登记表
    EXPECT_EQ(mgr.createStrategy("WtCtaTwoMaFact", "r31_nodot").get(), nullptr);
    EXPECT_EQ(mgr.getStrategy("r31_nodot").get(), nullptr);
}

TEST_F(r31_stra_mgr, cta_registered_factory_with_unknown_unit_wraps_null)
{
    ASSERT_TRUE(stage(kCtaFactLib));

    CtaStrategyMgr mgr;
    ASSERT_TRUE(mgr.loadFactories(_dir.string().c_str()));

    // 工厂已登记，但单元名不认识 ⇒ WtStraFact::createStrategy 返 NULL
    // ⇒ CtaStraWrapper 包一个空指针（L101-104 仍然执行，_strategies 也仍然登记）
    CtaStrategyPtr nullStra = mgr.createStrategy("WtCtaStraFact", "NoSuchStrategy", "r31_null");
    ASSERT_NE(nullStra.get(), nullptr);
    EXPECT_EQ(nullStra->self(), nullptr);
    // 登记表里确实留下了这个 id（析构时 CtaStraWrapper 不会去调用 deleteStrategy）
    EXPECT_EQ(mgr.getStrategy("r31_null").get(), nullStra.get());

    EXPECT_EQ(mgr.getStrategy("r31_never_used_id").get(), nullptr);
}

TEST_F(r31_stra_mgr, cta_mixed_dir_interleaves_success_with_three_continue_branches)
{
    ASSERT_TRUE(stage(kCtaFactLib, "libZCta.dylib"));
    ASSERT_TRUE(stage(kExeFactLib, "libAExe.dylib"));      // 可加载，无 createStrategyFact ⇒ free_library
    ASSERT_TRUE(stage(kIdxFactLib, "libAIdx.dylib"));      // 同上
    fs::create_directories(_dir / "subdir.dylib");
    touch("readme.txt", "not a library");
    touch("libAfake.dylib", "garbage, not mach-o");

    CtaStrategyMgr mgr;
    ASSERT_TRUE(mgr.loadFactories(_dir.string().c_str()));

    CtaStrategyPtr stra = mgr.createStrategy("WtCtaStraFact", "DualThrust", "mix_1");
    ASSERT_NE(stra.get(), nullptr);
    ASSERT_NE(stra->self(), nullptr);
    // 执行器/指标插件没有登记成策略工厂
    EXPECT_EQ(mgr.createStrategy("WtExeFact", "WtTWapExeUnit", "mix_2").get(), nullptr);
    EXPECT_EQ(mgr.createStrategy("IndexFactory", "x", "mix_3").get(), nullptr);
}

TEST_F(r31_stra_mgr, cta_reload_same_dir_overwrites_the_factory_entry)
{
    ASSERT_TRUE(stage(kTwoMaLib));

    CtaStrategyMgr mgr;
    ASSERT_TRUE(mgr.loadFactories(_dir.string().c_str()));
    // 第二次扫描：同一目录、同一键名 ⇒ L71 _factories[getName()] 命中已有项并覆盖各字段
    ASSERT_TRUE(mgr.loadFactories(_dir.string().c_str()));

    CtaStrategyPtr stra = mgr.createStrategy("WtCtaTwoMaFact", "TwoMa", "reload_1");
    ASSERT_NE(stra.get(), nullptr);
    EXPECT_STREQ(stra->self()->getName(), "TwoMa");
    EXPECT_EQ(mgr.createStrategy("WtCtaTwoMaFact", "TwoMa", "reload_1").get(),
        mgr.getStrategy("reload_1").get());
}

/* ------------------------------------------------------------------ *
 * SelStrategyMgr：装载循环真跑，但入口符号在本仓库无出处
 * ------------------------------------------------------------------ */
TEST_F(r31_stra_mgr, sel_load_of_plugin_like_dir_always_misses_its_entry_symbol)
{
    // 目录里放的是「能 dlopen、且导出别的工厂入口」的真实插件
    ASSERT_TRUE(stage(kCtaFactLib));
    ASSERT_TRUE(stage(kTwoMaLib));
    ASSERT_TRUE(stage(kExeFactLib));
    ASSERT_TRUE(stage(kIdxFactLib));

    SelStrategyMgr mgr;
    // L57 每次 dlopen 都成功 → L61 get_symbol("createMfStrategyFact") 恒为 NULL
    // → L64 free_library + continue → L90（count=0）→ L92 return true
    ASSERT_TRUE(mgr.loadFactories(_dir.string().c_str()));

    // 可证伪点：SEL 插件在本仓库根本装载不进来 ⇒ 任何名字、任何形态都造不出策略
    EXPECT_EQ(mgr.createStrategy("WtCtaStraFact", "DualThrust", "sel_1").get(), nullptr);
    EXPECT_EQ(mgr.createStrategy("WtSelStraFact.SelDemo", "sel_2").get(), nullptr);
    EXPECT_EQ(mgr.getStrategy("sel_1").get(), nullptr);
    EXPECT_EQ(mgr.createStrategy("nodotsatall", "sel_3").get(), nullptr);
}

TEST_F(r31_stra_mgr, sel_mixed_dir_with_unloadable_and_text_files_still_returns_true)
{
    ASSERT_TRUE(stage(kCtaFactLib, "libZcta.dylib"));
    touch("libZfake.dylib", "not mach-o at all");
    touch("notes.txt", "ignored by extension filter");
    fs::create_directories(_dir / "nested.dylib");

    SelStrategyMgr mgr;
    ASSERT_TRUE(mgr.loadFactories(_dir.string().c_str()));
    EXPECT_EQ(mgr.createStrategy("WtCtaStraFact", "DualThrust", "sel_m").get(), nullptr);
    EXPECT_EQ(mgr.getStrategy("sel_m").get(), nullptr);
}

/* ------------------------------------------------------------------ *
 * HftStrategyMgr：成功支只能靠跨插件同名符号（类型双关），不硬闯
 * ------------------------------------------------------------------ *
 * DISABLED_: 观察到的现象 —— 不要放开这条用例（实测 3/3 必崩，不是偶发）。
 * HftStrategyMgr.cpp:61 找的入口符号名与 CTA 插件导出的完全同名（createStrategyFact），
 * 于是目录里的 libWtCtaStraFact.dylib 会被 HFT 管理器当成 IHftStrategyFact 装载：
 * creator 非空 → creator() 返回的其实是 ICtaStrategyFact* → L71 _factories[pFact->getName()]
 * 的登记路径里直接 SIGSEGV（rc=139/signal 11；崩在 [ RUN ] 之后、[ OK ] 之前，
 * createStrategy 根本没被执行到；只放一份 CTA 插件也崩，与表扩容无关）。
 * 崩溃栈顶是跳到 0x0，上一帧是插件导出的 deleteStrategyFact，再往下是
 * HftStrategyMgr::_StraFactInfo::~_StraFactInfo() 与 ankerl 哈希表的插入路径
 * ——即「按 IHftStrategyFact 的形状去用 ICtaStrategyFact 对象」的跨镜像同名符号复用。
 * 主控提示的正是这一面，所以不硬闯：HftStrategyMgr.cpp 的 L68-81 留在空白面
 * （见 manifests/R31_pluginload.md），现象本身作为缺陷登记在 bugs/R31_pluginload.md（BUG-R31-002）。
 * 对照事实：同样两份插件喂给 CtaStrategyMgr（本文件上面 6 条用例）完全正常，
 * 差别只在管理器的接口类型 —— 说明缺的是产品侧的插件类型标识，不是用例写法。
 */
TEST_F(r31_stra_mgr, DISABLED_hft_loads_cta_plugin_via_shared_symbol_name)
{
    ASSERT_TRUE(stage(kCtaFactLib));

    HftStrategyMgr mgr;
    ASSERT_TRUE(mgr.loadFactories(_dir.string().c_str()));
    HftStrategyPtr stra = mgr.createStrategy("WtCtaStraFact", "DualThrust", "hft_1");
    EXPECT_NE(stra.get(), nullptr);
}

} // namespace
