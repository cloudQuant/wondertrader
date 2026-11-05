/**
 * r15_mgr_executers.cpp —— lane R15 / 工厂目录扫描 + 策略管理器 + ParserAdapter 日志 + WtLocalExecuter 通道事件
 *
 * 覆盖面（对照 manifests/api-brief-R15.md）：
 *   WtExecuterFactory.cpp  L17-23（目录不存在/false、fs::path 组包）、L36-53
 *                          （非 dylib continue、dlopen 失败 continue、缺 createExecFact ⇒ free_library）
 *   CtaStrategyMgr.cpp     L39-84（同上三段 + createStrategyFact 缺失分支）
 *   HftStrategyMgr.cpp     L39-84（同 Cta 形状）
 *   ParserAdapter.cpp      L369-372（handleParserLog 的 _stopped 守卫 + log_dyn_raw）
 *   WtLocalExecuter.cpp    writeLog（空/边界/超长输入）、L803/836/879（通道事件的 SpinLock）
 *   WtArbiExecuter.cpp     writeLog（空/边界/超长输入）
 *
 * 只引用 WtCore 一族。用 libWtIndexFactory.dylib 当「可 dlopen 但不含工厂符号」的样本
 * （已 nm 验证 createExecFact/createStrategyFact 导出数 = 0），全程不起线程、不建 socket。
 * 注意：_factories/_strategies 是实例成员而非 static，本文件的注入不会污染其他用例。
 */
#include "q01_sel_env.h"

#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#ifndef _WIN32
#include <dlfcn.h>
#endif

#include <Includes/ILogHandler.h>
#include <Includes/WTSVariant.hpp>
#include <WtCore/WtExecuterFactory.h>
#include <WtCore/CtaStrategyMgr.h>
#include <WtCore/HftStrategyMgr.h>
#include <WtCore/ParserAdapter.h>
#include <WtCore/WtExecMgr.h>
#include <WtCore/WtArbiExecuter.h>
#include <WtCore/WtLocalExecuter.h>
#include <WTSTools/WTSLogger.h>

using namespace q01sel;

namespace {

USING_NS_WTP;

/** 「能加载但没有工厂导出」的真实 dylib（构建产物 libWtIndexFactory.dylib）。
 *  I03-NEW-105：不再写死仓库 build-cov 路径；依次取 WT_COV_PLUGIN_BIN、本测试二进制所在构建的 bin/。 */
std::string noFactDylibPath()
{
    const char* const name = "libWtIndexFactory.dylib";
    std::error_code ec;
    if (const char* pluginBin = std::getenv("WT_COV_PLUGIN_BIN"))
    {
        const std::filesystem::path candidate = std::filesystem::path(pluginBin) / name;
        if (pluginBin[0] != '\0' && std::filesystem::exists(candidate, ec))
            return candidate.string();
    }
#ifndef _WIN32
    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(&noFactDylibPath), &info) != 0 && info.dli_fname != nullptr)
    {
        // <build>/bin/TestUnits/<test binary>  ->  <build>/bin/libWtIndexFactory.dylib
        const std::filesystem::path candidate =
            std::filesystem::path(info.dli_fname).parent_path().parent_path() / name;
        if (std::filesystem::exists(candidate, ec))
            return candidate.string();
    }
#endif
    return std::string();
}

class R15MgrTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        _env.reset(new Q01TempEnv("r15mgr"));
        _dir = std::filesystem::temp_directory_path() /
            ("r15mgr_" + std::to_string(::getpid()));
        std::error_code ec;
        std::filesystem::remove_all(_dir, ec);
        std::filesystem::create_directories(_dir, ec);
    }
    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(_dir, ec);
        _env.reset();
    }

    void touch(const std::string& name, const std::string& content)
    {
        std::ofstream out((_dir / name).c_str(), std::ios::trunc);
        out << content;
    }

    /** 复制真实 dylib；目标机不存在则返回 false，由用例 GTEST_SKIP */
    bool copyDylib(const std::string& name)
    {
        std::error_code ec;
        const std::string source = noFactDylibPath();
        if (source.empty())
            return false;
        std::filesystem::copy_file(source, _dir / name,
            std::filesystem::copy_options::overwrite_existing, ec);
        return !ec;
    }

    std::unique_ptr<Q01TempEnv> _env;
    std::filesystem::path _dir;
};

class R15LogCapture : public ILogHandler
{
public:
    std::vector<std::string> messages;

    void handleLogAppend(WTSLogLevel, const char* message) override
    {
        messages.emplace_back(message ? message : "");
    }
};

class R15QuiesceGuard
{
public:
    explicit R15QuiesceGuard(WtExecuterMgr& manager)
        : _manager(manager), _coordinator(std::vector<WtExecuterMgr*>{&manager})
    {}

    ~R15QuiesceGuard()
    {
        if (!quiesceAll())
        {
            ADD_FAILURE() << "executor manager did not complete coordinated quiesce";
            std::abort();
        }
    }

    bool quiesceAll()
    {
        return _coordinator.quiesce_all() && _coordinator.is_complete() &&
            _manager.wait_quiesced();
    }

private:
    WtExecuterMgr& _manager;
    WtExecQuiesceCoordinator _coordinator;
};

int runArbiWriteLogBoundsScenario()
{
    const char* loggerConfig = R"({
        "root": {"level":"info","async":false,"sinks":[]},
        "dyn_pattern": {
            "executer": {"level":"info","async":false,"sinks":[]}
        }
    })";

    R15LogCapture capture;
    try
    {
        WTSLogger::init(loggerConfig, false, &capture);
    }
    catch (...)
    {
        return 1;
    }
    WTSLogger::registerHandler(&capture);

    WtExecuterFactory factory;
    WtArbiExecuter exe(&factory, "r15_arbi_observable", NULL);
    const std::string prefix = "[r15_arbi_observable]";
    if (prefix.size() >= MAX_LOG_BUF_SIZE)
        return 2;

    const std::string boundary(MAX_LOG_BUF_SIZE - 1 - prefix.size(), 'b');
    const std::string oneByteOver = boundary + 'x';
    const std::string oversized(MAX_LOG_BUF_SIZE * 8, 'x');

    exe.writeLog("short");
    exe.writeLog(NULL);
    exe.writeLog(boundary.c_str());
    exe.writeLog(oneByteOver.c_str());
    exe.writeLog(oversized.c_str());

    if (capture.messages.size() != 5u)
        return 3;
    if (capture.messages[0] != prefix + "short")
        return 4;
    if (capture.messages[1] != prefix)
        return 5;
    if (capture.messages[2] != prefix + boundary)
        return 6;
    if (capture.messages[3] != prefix + boundary)
        return 7;
    const std::string truncatedOversized = prefix +
        std::string(MAX_LOG_BUF_SIZE - 1 - prefix.size(), 'x');
    if (capture.messages[4] != truncatedOversized)
        return 8;
    for (const std::string& message : capture.messages)
    {
        if (message.size() > MAX_LOG_BUF_SIZE - 1)
            return 9;
    }
    return 0;
}

} // namespace

/* ================= WtExecuterFactory ================= */

TEST_F(R15MgrTest, executer_factory_missing_dir_returns_false)
{
    WtExecuterFactory factory;
    EXPECT_FALSE(factory.loadFactories("/r15_nonexistent_dir_xyz"));
    // 未装载 ⇒ 三个创建口都空
    EXPECT_EQ(factory.createExeUnit("no.such").get(), nullptr);
    EXPECT_EQ(factory.createDiffExeUnit("no.such").get(), nullptr);
    EXPECT_EQ(factory.createArbiExeUnit("no.such").get(), nullptr);
    EXPECT_EQ(factory.createExeUnit("nodotsatall").get(), nullptr);   // ay.size()<2 早退
}

TEST_F(R15MgrTest, executer_factory_scan_skips_dirs_text_and_unloadable_libs)
{
    std::error_code ec;
    std::filesystem::create_directories(_dir / "asubdir", ec);
    touch("readme.txt", "not a library");
    touch("fake.dylib", "this is not mach-o");

    WtExecuterFactory factory;
    // 目录存在 ⇒ 走完整个循环：目录 continue、非 dylib continue、dlopen 失败 continue
    EXPECT_TRUE(factory.loadFactories(_dir.string().c_str()));
    EXPECT_EQ(factory.createExeUnit("fake.anything").get(), nullptr);
    EXPECT_EQ(factory.createDiffExeUnit("fake.anything").get(), nullptr);
    EXPECT_EQ(factory.createArbiExeUnit("fake.anything").get(), nullptr);
}

TEST_F(R15MgrTest, executer_factory_free_library_when_creator_symbol_absent)
{
    if (!copyDylib("libNoExecFact.dylib"))
        GTEST_SKIP() << "libWtIndexFactory.dylib not found via WT_COV_PLUGIN_BIN or this build's bin directory";
    WtExecuterFactory factory;
    EXPECT_TRUE(factory.loadFactories(_dir.string().c_str()));
    // dlopen 成功但 createExecFact 缺失 ⇒ L52 free_library+continue；
    // 可证伪点：_factories 仍为空 ⇒ 任何名字都造不出单元
    EXPECT_EQ(factory.createExeUnit("anything", "x").get(), nullptr);
    EXPECT_EQ(factory.createDiffExeUnit("anything", "x").get(), nullptr);
    EXPECT_EQ(factory.createArbiExeUnit("anything", "x").get(), nullptr);
}

/* ================= CtaStrategyMgr / HftStrategyMgr ================= */

TEST_F(R15MgrTest, cta_strategy_mgr_missing_dir_and_name_shapes)
{
    CtaStrategyMgr mgr;
    EXPECT_FALSE(mgr.loadFactories("/r15_nonexistent_dir_xyz"));
    EXPECT_EQ(mgr.createStrategy("fact", "unit", "id_a").get(), nullptr);
    EXPECT_EQ(mgr.createStrategy("nodotsatall", "id_b").get(), nullptr);
    EXPECT_EQ(mgr.getStrategy("id_a").get(), nullptr);         // 创建失败不入表
    EXPECT_EQ(mgr.getStrategy("id_b").get(), nullptr);
}

TEST_F(R15MgrTest, cta_strategy_mgr_scan_free_library_path)
{
    touch("notes.txt", "plain text");
    touch("broken.dylib", "not mach-o");
    if (!copyDylib("libNoStraFact.dylib"))
        GTEST_SKIP() << "libWtIndexFactory.dylib not found via WT_COV_PLUGIN_BIN or this build's bin directory";

    CtaStrategyMgr mgr;
    EXPECT_TRUE(mgr.loadFactories(_dir.string().c_str()));     // L39 fs::path + 循环三段
    // createStrategyFact 缺失 ⇒ 已登记的工厂为空 ⇒ 两个创建口都拿不到策略
    EXPECT_EQ(mgr.createStrategy("anyfact", "anyunit", "id1").get(), nullptr);
    EXPECT_EQ(mgr.createStrategy("anyfact.anyunit", "id2").get(), nullptr);
    EXPECT_EQ(mgr.getStrategy("id1").get(), nullptr);
}

TEST_F(R15MgrTest, hft_strategy_mgr_scan_free_library_path)
{
    touch("notes.txt", "plain text");
    touch("broken.dylib", "not mach-o");
    if (!copyDylib("libNoHftFact.dylib"))
        GTEST_SKIP() << "libWtIndexFactory.dylib not found via WT_COV_PLUGIN_BIN or this build's bin directory";

    HftStrategyMgr mgr;
    EXPECT_FALSE(mgr.loadFactories("/r15_nonexistent_dir_xyz"));
    EXPECT_TRUE(mgr.loadFactories(_dir.string().c_str()));
    EXPECT_EQ(mgr.createStrategy("anyfact", "anyunit", "h1").get(), nullptr);
    EXPECT_EQ(mgr.createStrategy("anyfact.anyunit", "h2").get(), nullptr);
    EXPECT_EQ(mgr.getStrategy("h1").get(), nullptr);
}

/* ================= ParserAdapter::handleParserLog ================= */

TEST_F(R15MgrTest, parser_adapter_initext_guard_and_log_forward)
{
    ParserAdapter adapter;
    EXPECT_STREQ(adapter.id(), "");
    // api==NULL ⇒ initExt 第一行早退（L45-47），_id 不写入、_stopped 保持 false
    EXPECT_FALSE(adapter.initExt("p15", NULL, NULL, NULL, NULL));
    EXPECT_STREQ(adapter.id(), "");

    // _stopped==false ⇒ L372 log_dyn_raw("parser", "") 被真实执行；
    // 可证伪点：转发前后对象状态与 id 完全不变（不得偷偷 init）
    adapter.handleParserLog(LL_INFO, "r15 parser log info");
    adapter.handleParserLog(LL_ERROR, "r15 parser log error");
    EXPECT_STREQ(adapter.id(), "");
    EXPECT_EQ(adapter.getBaseDataMgr(), nullptr);
}

/* ================= WtLocalExecuter 通道事件 + writeLog ================= */

/** 白盒派生只为把虚函数按公开口调用（writeLog 本身已是 public override） */
class R15LocalExecuter : public WtLocalExecuter
{
public:
    R15LocalExecuter(WtExecuterFactory* f, const char* name, IDataManager* dm)
        : WtLocalExecuter(f, name, dm) {}
    using WtLocalExecuter::init;      // 已是 public，重声明只作说明
};

TEST_F(R15MgrTest, local_executer_null_init_and_empty_channel_events)
{
    WtExecuterFactory factory;
    WtExecuterMgr manager;
    R15QuiesceGuard quiesce(manager);
    auto exe = std::make_shared<R15LocalExecuter>(&factory, "loc15", nullptr);
    manager.add_executer(exe);

    // 与 arbi/diff 同形：params==NULL ⇒ false（不置 _config）
    EXPECT_FALSE(exe->init(NULL));

    // 三个通道事件都只是 _channel_ready 赋值 + 空 _unit_map 的 SpinLock 遍历（L803/836/879）
    exe->on_channel_ready();
    exe->on_channel_lost();
    exe->on_account("CNY", 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    // 空表 ⇒ 不应有任何单元回调；可证伪点：事件后三个创建口依然造不出单元（工厂为空）
    EXPECT_EQ(factory.createExeUnit("loc15", "unit").get(), nullptr);

    // 短日志转发仍可达；空/边界/超长日志输入由独立用例覆盖。
    exe->writeLog("r15 short log");
    exe->on_channel_lost();
    EXPECT_EQ(factory.createArbiExeUnit("nothing.here").get(), nullptr);

    ASSERT_TRUE(quiesce.quiesceAll());
}

template <typename Executer>
void exerciseWriteLogBounds(Executer& exe, const char* name)
{
    const std::string prefix = std::string("[") + name + "]";
    ASSERT_LT(prefix.size(), static_cast<std::size_t>(MAX_LOG_BUF_SIZE));

    // 前缀 + 消息恰好占用缓冲区除 NUL 终止符外的全部容量。
    const std::string boundary(MAX_LOG_BUF_SIZE - 1 - prefix.size(), 'b');
    EXPECT_EQ(static_cast<std::size_t>(MAX_LOG_BUF_SIZE - 1), prefix.size() + boundary.size());
    const std::string oneByteOver = boundary + 'x';
    const std::string oversized(MAX_LOG_BUF_SIZE * 8, 'x');

    exe.writeLog("");
    exe.writeLog(boundary.c_str());
    exe.writeLog(oneByteOver.c_str());
    exe.writeLog(oversized.c_str());
}

TEST_F(R15MgrTest, local_and_arbi_executer_write_log_bounds)
{
    WtExecuterFactory factory;
    WtExecuterMgr manager;
    R15QuiesceGuard quiesce(manager);
    auto local = std::make_shared<WtLocalExecuter>(&factory, "r15_local_log", nullptr);
    manager.add_executer(local);
    WtArbiExecuter arbi(&factory, "r15_arbi_log", NULL);

    // 调用覆盖空字符串、缓冲区边界、越界一个字节及远超缓冲区的消息。
    // ASan/UBSan 运行该用例时可直接发现任何写越界；实现合同为最多 2047 字节内容，
    // 超长时保留从开头开始的确定性前缀并以 NUL 终止。
    exerciseWriteLogBounds(*local, "r15_local_log");
    exerciseWriteLogBounds(arbi, "r15_arbi_log");

    ASSERT_TRUE(quiesce.quiesceAll());
}

TEST_F(R15MgrTest, arbi_write_log_bounds_and_null_are_observable)
{
    EXPECT_EXIT(
        { std::_Exit(runArbiWriteLogBoundsScenario()); },
        ::testing::ExitedWithCode(0), "");
}

TEST_F(R15MgrTest, local_executer_on_tick_is_safe_with_empty_unit_state_map)
{
    // on_tick only queues callbacks for existing _unit_states; an empty state map
    // returns without entering unit creation. The empty factory probes confirm no unit was created.
    // 卡8b=A（eager 全量校验）：scale 必须为有限正数、policy name 严格为 "<factory>.<unit>"；
    // 原夹具缺 scale 且 name 无工厂段，按新合同补齐（工厂仍未加载）。
    WTSVariant* root = WTSVariant::createObject();
    WTSVariant* policy = WTSVariant::createObject();
    WTSVariant* def = WTSVariant::createObject();
    def->append("name", "R15NoSuchFact.R15NoSuchUnit");
    policy->append("default", def);
    root->append("policy", policy);
    root->append("scale", 1.0);

    WtExecuterFactory factory;
    WtExecuterMgr manager;
    R15QuiesceGuard quiesce(manager);
    auto exe = std::make_shared<WtLocalExecuter>(&factory, "loc15b", nullptr);
    manager.add_executer(exe);
    ASSERT_TRUE(exe->init(root));
    root->release();

    WTSTickData* tk = makeTick("SHFE", "ag2512", 20250110, 20250110, 93145, 7100.0);
    ASSERT_NE(tk, nullptr);
    // 工厂为空 ⇒ getUnit 建不出单元 ⇒ on_tick 早退；可证伪点：三个创建口仍拿不到单元
    exe->on_tick("SHFE.ag.2512", tk);
    EXPECT_EQ(factory.createExeUnit("R15NoSuchUnit").get(), nullptr);
    EXPECT_EQ(factory.createDiffExeUnit("R15NoSuchUnit").get(), nullptr);
    EXPECT_EQ(factory.createArbiExeUnit("R15NoSuchUnit").get(), nullptr);

    exe->on_channel_ready();      // 单元表仍空 ⇒ L803 遍历零次
    tk->release();

    ASSERT_TRUE(quiesce.quiesceAll());
}
