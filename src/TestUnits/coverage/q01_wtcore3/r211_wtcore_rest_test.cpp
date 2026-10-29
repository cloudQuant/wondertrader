/**
 * r211_wtcore_rest_test.cpp — lane R211 / wave29
 *
 * 目标：WtCore 命名残余面里「可执行」与「已被既有缺陷锁死」的两类入口分开放：
 *   - WtEngine::init_riskmon 装入合法 dylib 但缺 createRiskMonFact 时的拒绝支
 *     （WtEngine.cpp:1165-1171，5 行）；
 *   - WtExecuterMgr::handle_pos_change 的执行器过滤与显式路由
 *     （WtExecMgr.cpp:96-98、104-105，5 行）；
 *   - WtSelEngine 复权 flag 被引擎丢弃（BUG-R192-001，charaterization，不新增行）；
 *   - SelStrategyMgr / HftStrategyMgr 对「可 dlopen 但无工厂入口」目录的跳过
 *     （既有行，锁行为）；
 *   - WtCtaRtTicker 数据驱动闭合在有限时段最后一分钟不触发 on_session_end
 *     （BUG-R202-002 的有限时段复核，characterization）；
 *   - 两个 DISABLED_ 用例分别固化「到期任务 abort」与「CTA 插件类型双关 SIGSEGV」，
 *     不参与覆盖率，也不放宽断言。
 *
 * 边界：只走公开 API；不启动长线程、不等待墙钟；ticker 不调用 run()，行情由
 * 主线程同步 on_tick() 送入；临时目录只用系统 temp；不加载策略实现、不连柜台。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <mach-o/dyld.h>
#include <unistd.h>

#include "q01_sel_env.h"

#include <Includes/IDataReader.h>
#include <Includes/IHotMgr.h>
#include <Includes/IBaseDataMgr.h>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>
#include <WtCore/HftStrategyMgr.h>
#include <WtCore/IExecCommand.h>
#include <WtCore/SelStrategyMgr.h>
#include <WtCore/WtCtaEngine.h>
#include <WtCore/WtCtaTicker.h>
#include <WtCore/WtDtMgr.h>
#include <WtCore/WtExecMgr.h>
#include <WtCore/WtFilterMgr.h>
#include <WtCore/WtHelper.h>
#include <WtCore/WtSelEngine.h>

using namespace wtp;

namespace {

namespace fs = std::filesystem;

/** 每个用例独立的生成目录，避免 q01sel::Q01TempEnv 的进程级静态缓存互相删除。 */
class R211Temp
{
public:
    explicit R211Temp(const char* tag)
    {
        _dir = fs::temp_directory_path()
            / ("r211_" + std::string(tag) + "_" + std::to_string((long)::getpid()));
        std::error_code ec;
        fs::remove_all(_dir, ec);
        fs::create_directories(_dir, ec);
        WtHelper::setGenerateDir((_dir.string() + "/").c_str());
    }

    ~R211Temp()
    {
        std::error_code ec;
        fs::remove_all(_dir, ec);
    }

    std::string dir() const { return _dir.string() + "/"; }

private:
    fs::path _dir;
};

/** bin 目录：<build>/bin/TestUnits/CoverageTests_wtcore -> <build>/bin */
fs::path binDir()
{
    char exe[4096] = { 0 };
    uint32_t size = sizeof(exe);
    if (_NSGetExecutablePath(exe, &size) != 0)
        return fs::path();

    std::error_code ec;
    fs::path exePath = fs::canonical(exe, ec);
    if (ec)
        return fs::path();
    return exePath.parent_path().parent_path();
}

/** setInstDir() 与 init_riskmon() 的目录拼接要求目录值保留末尾分隔符。 */
class R211InstDirGuard
{
public:
    explicit R211InstDirGuard(const fs::path& dir) : _old(WtHelper::getInstDir())
    {
        _value = dir.string();
        if (!_value.empty() && _value.back() != fs::path::preferred_separator)
            _value.push_back(fs::path::preferred_separator);
        WtHelper::setInstDir(_value.c_str());
    }

    ~R211InstDirGuard() { WtHelper::setInstDir(_old.c_str()); }

    const std::string& value() const { return _value; }

private:
    std::string _old;
    std::string _value;
};

/** 数据管理器替身：复权标志按位组合返回 7，供引擎转发断言使用。 */
class R211FlagDtMgr : public WtDtMgr
{
public:
    uint32_t get_adjusting_flag() override { return 7; }
};

/** 只接基础数据/主力/数据管理器的裸 SEL 引擎。 */
class R211SelEngine : public WtSelEngine
{
public:
    void attach(IBaseDataMgr* bd, IHotMgr* hot, WtDtMgr* dm)
    {
        _base_data_mgr = bd;
        _hot_mgr = hot;
        _data_mgr = dm;
        set_date_time(20250110, 931, 0);
        set_trading_date(20250110);
    }

    bool has_riskmon() const { return _risk_mon != nullptr; }
};

/** 记录分钟闭合写入的 IDataReader 替身。 */
class R211Reader : public IDataReader
{
public:
    void onMinuteEnd(uint32_t d, uint32_t t, uint32_t e = 0) override
    {
        last_date = d;
        last_time = t;
        last_end = e;
        ++count;
    }

    WTSTickSlice* readTickSlice(const char*, uint32_t, uint64_t = 0) override { return NULL; }
    WTSKlineSlice* readKlineSlice(const char*, WTSKlinePeriod, uint32_t, uint64_t = 0) override
    {
        return NULL;
    }

    uint32_t count = 0;
    uint32_t last_date = 0;
    uint32_t last_time = 0;
    uint32_t last_end = 0;
};

/** CTA 引擎替身：只观察会话结束，行情入口不再拉起基类的任务线程。 */
class R211CtaEngine : public WtCtaEngine
{
public:
    void attach(IBaseDataMgr* bd)
    {
        _base_data_mgr = bd;
        set_date_time(20250110, 1028, 0);
        set_trading_date(20250110);
    }

    void on_tick(const char*, WTSTickData*) override {}

    void on_session_end() override { ++session_ends; }

    int session_ends = 0;
};

/** 执行器替身：只记录 on_position_changed 次数。 */
class R211Exec : public IExecCommand
{
public:
    explicit R211Exec(const char* name) : IExecCommand(name) {}
    virtual ~R211Exec() {}

    void on_position_changed(const char*, double) override { ++changes; }

    int changes = 0;
};

/* ------------------------------------------------------------------ *
 * 1. WtEngine::init_riskmon：合法 dylib 但没有 createRiskMonFact 时
 *    必须 free_library 并返回 false（WtEngine.cpp:1165-1171）。
 * ------------------------------------------------------------------ */
TEST(R211Engine, IncompatibleRiskMonModuleIsRejected)
{
    R211Temp tmp("risk");
    const R211InstDirGuard instDir(binDir());
    const std::string moduleName = DLLHelper::wrap_module("WtExeFact");
    const fs::path modulePath = binDir() / moduleName;

    // 前置条件：这是可加载的模块，但其导出接口是 ExecuterFact 而非 RiskMonFact。
    ASSERT_TRUE(fs::is_regular_file(modulePath)) << modulePath.string();
    ASSERT_FALSE(fs::exists(fs::path(WtHelper::getCWD()) / moduleName))
        << "测试必须走 getInstDir() 的回退路径";
    ASSERT_EQ(modulePath.string(), instDir.value() + moduleName);
    DllHandle module = DLLHelper::load_library(modulePath.string().c_str());
    ASSERT_NE(nullptr, module);
    const bool hasRiskMonFactory = DLLHelper::get_symbol(module, "createRiskMonFact") != nullptr;
    DLLHelper::free_library(module);
    ASSERT_FALSE(hasRiskMonFactory);

    q01sel::BaseDataMgr bd;
    q01sel::HotMgr hot;
    WtDtMgr dm;

    WTSVariant* cfg = WTSCfgLoader::load_from_content(
        "{\"product\":{\"session\":\"TRADING\"},\"fees\":\"\",\"filters\":\"\","
        "\"riskmon\":{\"active\":true,\"module\":\"WtExeFact\",\"name\":\"r211\"}}");
    ASSERT_NE(nullptr, cfg);

    // BUG-R42-001：WtSelEngine 析构会读未初始化的 _tm_ticker，堆分配且不回收
    R211SelEngine* engine = new R211SelEngine();
    engine->init(cfg, &bd, &dm, &hot, NULL);
    cfg->release();

    // 有库且无 createRiskMonFact 时，init_riskmon 应拒绝模块并保持无风控包装器状态。
    EXPECT_FALSE(engine->has_riskmon());
    // BUG-R211-001：init() 丢弃 init_riskmon 的 false，active 的风控配置会被静默忽略
}

/* ------------------------------------------------------------------ *
 * 2. WtExecuterMgr::handle_pos_change：被过滤的执行器收不到信号，
 *    显式路由（execid==执行器名）走 104-105。
 * ------------------------------------------------------------------ */
TEST(R211ExecMgr, ExecuterFilterAndExplicitRouteDispatch)
{
    R211Temp tmp("exec");
    const std::string fltFile = tmp.dir() + "filters.json";
    {
        std::ofstream f(fltFile.c_str());
        ASSERT_TRUE(f.good());
        f << "{\"executer_filters\":{\"EX1\":true}}";
    }

    WtFilterMgr flt;
    flt.load_filters(fltFile.c_str());
    ASSERT_TRUE(flt.is_filtered_by_executer("EX1"));
    ASSERT_FALSE(flt.is_filtered_by_executer("EX2"));

    WtExecuterMgr mgr;
    mgr.set_filter_mgr(&flt);
    std::shared_ptr<R211Exec> ex1(new R211Exec("EX1"));
    std::shared_ptr<R211Exec> ex2(new R211Exec("EX2"));
    mgr.add_executer(ex1);
    mgr.add_executer(ex2);

    // "ALL" 路由：EX1 在 96-98 被 continue，EX2 走 102-103
    mgr.handle_pos_change("SHFE.ag.2512", 10.0, 10.0, "ALL");
    EXPECT_EQ(0, ex1->changes);
    EXPECT_EQ(1, ex2->changes);

    // 显式路由到 EX2：102 的 ALL 判定为假，104-105 命中
    mgr.handle_pos_change("SHFE.ag.2512", 12.0, 2.0, "EX2");
    EXPECT_EQ(0, ex1->changes);
    EXPECT_EQ(2, ex2->changes);
}

/* ------------------------------------------------------------------ *
 * 3. BUG-P02-003 regression：引擎公开 accessor 应转发 data manager 的复权位。
 *    WtDataReader 将该值定义为按位组合（1/2/4），不能在引擎层固定成 0。
 * ------------------------------------------------------------------ */
TEST(R211SelEngine, AdjustingFlagPropagatesDataManagerValue)
{
    R211Temp tmp("flag");
    q01sel::BaseDataMgr bd;
    q01sel::HotMgr hot;
    R211FlagDtMgr dm;

    EXPECT_EQ(7u, dm.get_adjusting_flag());

    // BUG-R42-001：同样不回收
    R211SelEngine* engine = new R211SelEngine();
    engine->attach(&bd, &hot, &dm);

    // 无 data manager 时公开 accessor 的默认值是 0；这是单独的空依赖控制。
    R211SelEngine* engineWithoutDataMgr = new R211SelEngine();
    engineWithoutDataMgr->attach(&bd, &hot, NULL);
    EXPECT_EQ(0u, engineWithoutDataMgr->get_adjusting_flag());

    // 回归断言：公开引擎接口应原样转发数据管理器的 flag。
    EXPECT_EQ(7u, engine->get_adjusting_flag());
}

/* ------------------------------------------------------------------ *
 * 4. SelStrategyMgr / HftStrategyMgr：可 dlopen 但无各自工厂入口的目录
 *    只跳过、整体返回 true；两个 createStrategy 重载与 getStrategy 落空。
 * ------------------------------------------------------------------ */
TEST(R211StrategyMgr, NonPluginDirectoryLoadsNothing)
{
    const fs::path bin = binDir();
    ASSERT_TRUE(fs::exists(bin / "libWtShareHelper.dylib")) << bin;

    fs::path dir = fs::temp_directory_path()
        / ("r211_mgr_" + std::to_string((long)::getpid()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    ASSERT_FALSE(ec) << ec.message();

    fs::create_symlink(bin / "libWtShareHelper.dylib", dir / "libnotastrategy.dylib", ec);
    ASSERT_FALSE(ec) << ec.message();

    SelStrategyMgr sel;
    EXPECT_TRUE(sel.loadFactories(dir.string().c_str()));
    EXPECT_EQ(nullptr, sel.createStrategy("NoFact.Unit", "r211_sel"));
    EXPECT_EQ(nullptr, sel.createStrategy("NoFact", "Unit", "r211_sel2"));
    EXPECT_EQ(nullptr, sel.getStrategy("r211_sel"));

    HftStrategyMgr hft;
    EXPECT_TRUE(hft.loadFactories(dir.string().c_str()));
    EXPECT_EQ(nullptr, hft.createStrategy("NoFact.Unit", "r211_hft"));
    EXPECT_EQ(nullptr, hft.getStrategy("r211_hft"));

    fs::remove_all(dir, ec);
}

/* ------------------------------------------------------------------ *
 * 5. BUG-R202-002 有限时段复核：数据驱动只闭合到 10:29，
 *    最后一分钟 10:30 不会闭合，因此 on_session_end 永不触发。
 * ------------------------------------------------------------------ */
TEST(R211CtaTicker, LastMinuteOfFiniteSessionIsNotClosedByData)
{
    R211Temp tmp("cta_ticker");
    q01sel::BaseDataMgr bd;
    bd.regisSessionWithTime("TRADING", 900, 1030);
    WTSCommodityInfo* comm = q01sel::makeComm("SHFE", "ag", "TRADING", TM_Both);
    comm->setSessionInfo(bd.session("TRADING"));
    bd.regis(comm, q01sel::makeCt("SHFE", "ag", "ag2512"));

    R211CtaEngine engine;
    engine.attach(&bd);
    R211Reader reader;

    WtCtaRtTicker ticker(&engine);
    ticker.init(&reader, "TRADING");

    auto feed = [&](uint32_t t, double px) {
        WTSTickData* tk = q01sel::makeTick("SHFE", "ag2512", 20250110, 20250110, t, px);
        ASSERT_NE(nullptr, tk);
        ticker.on_tick(tk);
        tk->release();
    };

    feed(102800000, 7100.0);
    EXPECT_EQ(0u, reader.count);

    feed(102900000, 7101.0);   // 闭合 10:29
    EXPECT_EQ(1u, reader.count);
    EXPECT_EQ(1029u, reader.last_time);

    feed(103000000, 7102.0);   // 10:30 是最后一分钟，数据侧不再闭合
    EXPECT_EQ(1u, reader.count);
    EXPECT_EQ(0, engine.session_ends);
}

/* ------------------------------------------------------------------ *
 * 6. BUG-R192-002/003 固化：到期任务创建局部 std::thread，析构即 abort；
 *    即便不 abort，任务 id 与上下文 id 也是两套序列。DISABLED_，不参与覆盖率。
 * ------------------------------------------------------------------ */
TEST(R211SelEngine, DISABLED_ExpiredTaskTerminatesBeforeSchedule)
{
    R211Temp tmp("task");
    q01sel::BaseDataMgr bd;
    q01sel::HotMgr hot;

    R211SelEngine* engine = new R211SelEngine();
    engine->attach(&bd, &hot, NULL);

    std::shared_ptr<q01sel::SelCtx> ctx(new q01sel::SelCtx(engine, "r211_task"));
    engine->addContext(ctx, 20250110, 931, TPT_Daily);

    engine->on_minute_end(20250110, 930);

    EXPECT_EQ(1u, ctx->schedule_fires);
}

/* ------------------------------------------------------------------ *
 * 7. BUG-R31-002 固化：HFT 装载器与 CTA 插件共用 createStrategyFact，
 *    类型双关后在登记路径 SIGSEGV。DISABLED_，不参与覆盖率。
 * ------------------------------------------------------------------ */
TEST(R211HftStrategyMgr, DISABLED_CtaPluginTypePunCrashesOnRegistration)
{
    const fs::path bin = binDir();
    ASSERT_TRUE(fs::exists(bin / "libWtCtaStraFact.dylib")) << bin;

    fs::path dir = fs::temp_directory_path()
        / ("r211_hft_" + std::to_string((long)::getpid()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    ASSERT_FALSE(ec) << ec.message();

    fs::create_symlink(bin / "libWtCtaStraFact.dylib", dir / "libctafact.dylib", ec);
    ASSERT_FALSE(ec) << ec.message();

    HftStrategyMgr mgr;
    EXPECT_TRUE(mgr.loadFactories(dir.string().c_str()));

    fs::remove_all(dir, ec);
}

} // namespace
