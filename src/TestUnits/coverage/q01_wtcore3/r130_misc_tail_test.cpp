/**
 * r130_misc_tail_test.cpp — lane R130 / wave21
 *
 * 目标：WtCore 三处独立的残余公开分支。
 *
 *   1) ParserAdapter 模块装载失败 :117-121
 *      （cfg.module 指向不存在的插件，instDir 指向空临时目录）
 *   2) WtSelEngine::on_minute_end 的“当天是节假日”跳过 :239-241
 *      （任务时间命中且未执行过，但 isHoliday(curDate) 为 true）
 *   3) WtCtaEngine::run 的风控模块启动 :111-112
 *      （setRiskMonitor 后 run() 必须调用 risk_mon->self()->run()）
 *
 * 全部用例不起长线程：Sel 分支不触发任务线程；Cta run 的 ticker 在 release()
 * 时立即 stop/join（会话 0-2359，线程轮询间隔 10ms）。临时文件只在系统临时目录。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include "q01_sel_env.h"
#include "r130_env.h"
#include "r90_env.h"
#include "../p02_wtcore_engines2/r42_core_fakes.h"

#include <WTSUtils/WTSCfgLoader.h>

#include <Includes/IParserApi.h>
#include <Includes/RiskMonDefs.h>
#include <Includes/WTSVariant.hpp>
#include <WtCore/ParserAdapter.h>
#include <WtCore/WtDtMgr.h>
#include <WtCore/TraderAdapter.h>
#include <WtCore/WtCtaEngine.h>
#include <WtCore/WtSelEngine.h>

using namespace wtp;

namespace {

//////////////////////////////////////////////////////////////////
// ParserAdapter 用例替身
//////////////////////////////////////////////////////////////////
class R130ParserStub : public IParserStub
{
public:
    uint32_t quotes = 0;
    void handle_push_quote(WTSTickData*) override { ++quotes; }
};

/** isHoliday 只为指定日期返回 true */
class R130HolidayBase : public q01sel::BaseDataMgr
{
public:
    uint32_t holiday = 0;

    bool isHoliday(const char* /*pid*/, uint32_t uDate, bool /*isTpl*/ = false) override
    {
        return holiday != 0 && uDate == holiday;
    }
};

//////////////////////////////////////////////////////////////////
// 风控模块替身
//////////////////////////////////////////////////////////////////
class R130RiskMon : public WtRiskMonitor
{
public:
    const char* getName() override { return "r130risk"; }
    const char* getFactName() override { return "r130fact"; }
    void run() override { ++runs; }

    uint32_t runs = 0;
};

class R130RiskFact : public IRiskMonitorFact
{
public:
    const char* getName() override { return "r130fact"; }
    void enumRiskMonitors(FuncEnumRiskMonCallback) override {}
    WtRiskMonitor* createRiskMonotor(const char*) override { return NULL; }
    bool deleteRiskMonotor(WtRiskMonitor*) override { return true; }
};

WTSVariant* sessionCfg()
{
    return WTSCfgLoader::load_from_content(
        "{\"product\":{\"session\":\"TRADING\"},\"fees\":\"\",\"filters\":\"\"}");
}

class R130InstDirGuard
{
public:
    explicit R130InstDirGuard(const std::string& path)
        : _old(WtHelper::getInstDir())
    {
        WtHelper::setInstDir(path.c_str());
    }

    ~R130InstDirGuard() { WtHelper::setInstDir(_old.c_str()); }

private:
    std::string _old;
};

class R130ParserCounters
{
public:
    using ResetFn = void (*)();
    using CountFn = uint32_t (*)();
	using QuiesceFn = void (*)();

    explicit R130ParserCounters(const std::filesystem::path& modulePath)
        : _module(DLLHelper::load_library(modulePath.c_str()))
    {
        if (_module == nullptr)
            return;
        _reset = reinterpret_cast<ResetFn>(DLLHelper::get_symbol(_module, "r130_parser_reset_counts"));
		_proveQuiescent = reinterpret_cast<QuiesceFn>(DLLHelper::get_symbol(_module, "r130_parser_prove_quiescent"));
        _create = reinterpret_cast<CountFn>(DLLHelper::get_symbol(_module, "r130_parser_create_count"));
		_init = reinterpret_cast<CountFn>(DLLHelper::get_symbol(_module, "r130_parser_init_count"));
		_register = reinterpret_cast<CountFn>(DLLHelper::get_symbol(_module, "r130_parser_register_count"));
		_subscribe = reinterpret_cast<CountFn>(DLLHelper::get_symbol(_module, "r130_parser_subscribe_count"));
		_subscribeSize = reinterpret_cast<CountFn>(DLLHelper::get_symbol(_module, "r130_parser_last_subscribe_size"));
		_connect = reinterpret_cast<CountFn>(DLLHelper::get_symbol(_module, "r130_parser_connect_count"));
        _release = reinterpret_cast<CountFn>(DLLHelper::get_symbol(_module, "r130_parser_release_count"));
        _delete = reinterpret_cast<CountFn>(DLLHelper::get_symbol(_module, "r130_parser_delete_count"));
		_unsafeRelease = reinterpret_cast<CountFn>(DLLHelper::get_symbol(_module, "r130_parser_unsafe_release_count"));
    }

    ~R130ParserCounters()
    {
        if (_module != nullptr)
            DLLHelper::free_library(_module);
    }

    bool valid() const
    {
		return _module != nullptr && _reset != nullptr && _proveQuiescent != nullptr
			&& _create != nullptr && _init != nullptr && _register != nullptr
			&& _subscribe != nullptr && _subscribeSize != nullptr && _connect != nullptr && _release != nullptr
			&& _delete != nullptr && _unsafeRelease != nullptr;
    }

    void reset() const { _reset(); }
	void proveQuiescent() const { _proveQuiescent(); }
    uint32_t createCount() const { return _create(); }
	uint32_t initCount() const { return _init(); }
	uint32_t registerCount() const { return _register(); }
	uint32_t subscribeCount() const { return _subscribe(); }
	uint32_t lastSubscribeSize() const { return _subscribeSize(); }
	uint32_t connectCount() const { return _connect(); }
    uint32_t releaseCount() const { return _release(); }
    uint32_t deleteCount() const { return _delete(); }
	uint32_t unsafeReleaseCount() const { return _unsafeRelease(); }

private:
    DllHandle _module = nullptr;
    ResetFn _reset = nullptr;
	QuiesceFn _proveQuiescent = nullptr;
    CountFn _create = nullptr;
	CountFn _init = nullptr;
	CountFn _register = nullptr;
	CountFn _subscribe = nullptr;
	CountFn _subscribeSize = nullptr;
	CountFn _connect = nullptr;
    CountFn _release = nullptr;
    CountFn _delete = nullptr;
	CountFn _unsafeRelease = nullptr;
};

}  // namespace

//////////////////////////////////////////////////////////////////
// 模块不存在：init 返回 false 且不构造 api
// src/WtCore/ParserAdapter.cpp:105-121
//////////////////////////////////////////////////////////////////
TEST(R130MiscTail, ParserInitFailsWhenModuleCannotBeLoaded)
{
    r130::TempEnv env("r130misc");

    const std::string oldInst = WtHelper::getInstDir();
    WtHelper::setInstDir(env.dir().c_str());

    q01sel::BaseDataMgr bd;
    bd.regisSessionWithTime("TRADING", 0, 2359);
    q01sel::HotMgr hot;
    R130ParserStub stub;

    WTSVariant* cfg = WTSCfgLoader::load_from_content(
        "{\"module\":\"R130NoSuchParserModule\",\"host\":\"127.0.0.1\"}");
    ASSERT_NE(nullptr, cfg);
    const uint32_t cfgRetainBaseline = cfg->retainCount();

    ParserAdapter adapter;
    EXPECT_FALSE(adapter.init("r130_missing_parser", cfg, &stub, &bd, &hot));
    EXPECT_FALSE(adapter.hasApi());
    EXPECT_EQ(cfgRetainBaseline, cfg->retainCount());
    adapter.release();
    EXPECT_EQ(cfgRetainBaseline, cfg->retainCount());
    cfg->release();

    WtHelper::setInstDir(oldInst.c_str());
}

//////////////////////////////////////////////////////////////////
// 成功 init 后由受控 fake module 释放 cfg；重复 release 不重复释放引用
// src/WtCore/ParserAdapter.cpp:237-388, 391-488
//////////////////////////////////////////////////////////////////
TEST(R130MiscTail, ParserSuccessfulInitReleaseAndRepeatReleaseBalanceCfgRetain)
{
    namespace fs = std::filesystem;

    r130::TempEnv env("r130parser");
    std::error_code ec;
    const fs::path parserDir = fs::path(env.dir()) / "parsers";
    fs::create_directories(parserDir, ec);
    ASSERT_FALSE(ec) << ec.message();

    const fs::path pluginPath = parserDir / DLLHelper::wrap_module("R130LifecycleParser");
    fs::copy_file(WT_R130_PARSER_PLUGIN_PATH, pluginPath, fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();

    R130InstDirGuard instDir(env.dir());
    R130ParserCounters counters(pluginPath);
    ASSERT_TRUE(counters.valid()) << "R130 lifecycle fake parser exports are unavailable";
    counters.reset();

    WTSVariant* cfg = WTSCfgLoader::load_from_content(
        "{\"module\":\"R130LifecycleParser\",\"host\":\"127.0.0.1\"}");
    ASSERT_NE(nullptr, cfg);
    const uint32_t cfgRetainBaseline = cfg->retainCount();

    r42fakes::R42BaseDataMgr bd;
    r42fakes::R42HotMgr hot;
    R130ParserStub stub;
    ParserAdapter adapter;
    EXPECT_TRUE(adapter.init("r130_lifecycle_parser", cfg, &stub, &bd, &hot));
    EXPECT_TRUE(adapter.hasApi());
    EXPECT_EQ(1u, counters.createCount());
    EXPECT_EQ(1u, counters.initCount());
    EXPECT_EQ(1u, counters.registerCount());
    EXPECT_EQ(1u, counters.subscribeCount());
    EXPECT_EQ(cfgRetainBaseline + 1u, cfg->retainCount());

    // The controlled fake has no asynchronous producer; the explicit provider
    // signal keeps this teardown test aligned with the adapter's drain contract.
    counters.proveQuiescent();
    adapter.release();
    EXPECT_FALSE(adapter.hasApi());
    EXPECT_EQ(1u, counters.releaseCount());
    EXPECT_EQ(1u, counters.deleteCount());
    EXPECT_EQ(cfgRetainBaseline, cfg->retainCount());

    adapter.release();
    EXPECT_EQ(1u, counters.releaseCount());
    EXPECT_EQ(1u, counters.deleteCount());
    EXPECT_EQ(cfgRetainBaseline, cfg->retainCount());

    cfg->release();
}

//////////////////////////////////////////////////////////////////
// ordinary init 的真实动态模块路径：init=false 不订阅/连接，API owner 留存到显式静默证明
//////////////////////////////////////////////////////////////////
TEST(R130MiscTail, ParserOrdinaryInitFalsePropagatesAndRetainsApiUntilQuiescent)
{
    namespace fs = std::filesystem;

    r130::TempEnv env("r130parser_init_false");
    std::error_code ec;
    const fs::path parserDir = fs::path(env.dir()) / "parsers";
    fs::create_directories(parserDir, ec);
    ASSERT_FALSE(ec) << ec.message();

    const fs::path pluginPath = parserDir / DLLHelper::wrap_module("R130LifecycleParser");
    fs::copy_file(WT_R130_PARSER_PLUGIN_PATH, pluginPath, fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();

    R130InstDirGuard instDir(env.dir());
    R130ParserCounters counters(pluginPath);
    ASSERT_TRUE(counters.valid()) << "R130 lifecycle fake parser exports are unavailable";
    counters.reset();

    WTSVariant* cfg = WTSCfgLoader::load_from_content(
        "{\"module\":\"R130LifecycleParser\",\"r130_init_ok\":false}");
    ASSERT_NE(nullptr, cfg);
    const uint32_t cfgRetainBaseline = cfg->retainCount();

    q01sel::BaseDataMgr bd;
    q01sel::HotMgr hot;
    R130ParserStub stub;
    ParserAdapter adapter;
    EXPECT_FALSE(adapter.init("r130_init_false", cfg, &stub, &bd, &hot));
    EXPECT_FALSE(adapter.isInitialized());
    EXPECT_TRUE(adapter.hasApi());
    EXPECT_FALSE(adapter.run());
    EXPECT_EQ(1u, counters.createCount());
    EXPECT_EQ(1u, counters.registerCount());
    EXPECT_EQ(1u, counters.initCount());
    EXPECT_EQ(0u, counters.subscribeCount());
    EXPECT_EQ(0u, counters.connectCount());
    EXPECT_EQ(0u, counters.releaseCount());
    EXPECT_EQ(0u, counters.deleteCount());
    EXPECT_EQ(0u, counters.unsafeReleaseCount());
    EXPECT_EQ(cfgRetainBaseline + 1u, cfg->retainCount());

    counters.proveQuiescent();
    adapter.release();
    EXPECT_FALSE(adapter.hasApi());
    EXPECT_EQ(1u, counters.releaseCount());
    EXPECT_EQ(1u, counters.deleteCount());
    EXPECT_EQ(0u, counters.unsafeReleaseCount());
    EXPECT_EQ(cfgRetainBaseline, cfg->retainCount());
    cfg->release();
}

//////////////////////////////////////////////////////////////////
// ordinary init 正控：有效 manager 的合同集照常订阅，成功 API 可以 connect
//////////////////////////////////////////////////////////////////
TEST(R130MiscTail, ParserOrdinaryInitSuccessSubscribesContractsAndConnects)
{
    namespace fs = std::filesystem;

    r130::TempEnv env("r130parser_init_success");
    std::error_code ec;
    const fs::path parserDir = fs::path(env.dir()) / "parsers";
    fs::create_directories(parserDir, ec);
    ASSERT_FALSE(ec) << ec.message();

    const fs::path pluginPath = parserDir / DLLHelper::wrap_module("R130LifecycleParser");
    fs::copy_file(WT_R130_PARSER_PLUGIN_PATH, pluginPath, fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();

    R130InstDirGuard instDir(env.dir());
    R130ParserCounters counters(pluginPath);
    ASSERT_TRUE(counters.valid()) << "R130 lifecycle fake parser exports are unavailable";
    counters.reset();

    r42fakes::R42BaseDataMgr bd;
    bd.regisSession("TRADING");
    bd.regis(r42fakes::makeComm("SHFE", "rb", "TRADING", TM_Both),
        r42fakes::makeCt("SHFE", "rb", "rb2510"));
    r42fakes::R42HotMgr hot;
    R130ParserStub stub;

    WTSVariant* cfg = WTSCfgLoader::load_from_content(
        "{\"module\":\"R130LifecycleParser\",\"r130_init_ok\":true}");
    ASSERT_NE(nullptr, cfg);
    ParserAdapter adapter;
    EXPECT_TRUE(adapter.init("r130_init_success", cfg, &stub, &bd, &hot));
    EXPECT_TRUE(adapter.isInitialized());
    EXPECT_EQ(1u, counters.registerCount());
    EXPECT_EQ(1u, counters.initCount());
    EXPECT_EQ(1u, counters.subscribeCount());
    EXPECT_EQ(1u, counters.lastSubscribeSize());
    EXPECT_TRUE(adapter.run());
    EXPECT_EQ(1u, counters.connectCount());

    counters.proveQuiescent();
    adapter.release();
    EXPECT_EQ(1u, counters.releaseCount());
    EXPECT_EQ(1u, counters.deleteCount());
    EXPECT_EQ(0u, counters.unsafeReleaseCount());
    cfg->release();
}

//////////////////////////////////////////////////////////////////
// 普通 init 缺少必需的基础数据 manager：在加载/创建 provider 前拒绝
//////////////////////////////////////////////////////////////////
TEST(R130MiscTail, ParserOrdinaryInitRejectsNullBaseDataManagerBeforeProviderWork)
{
    namespace fs = std::filesystem;

    r130::TempEnv env("r130parser_null_manager");
    std::error_code ec;
    const fs::path parserDir = fs::path(env.dir()) / "parsers";
    fs::create_directories(parserDir, ec);
    ASSERT_FALSE(ec) << ec.message();

    const fs::path pluginPath = parserDir / DLLHelper::wrap_module("R130LifecycleParser");
    fs::copy_file(WT_R130_PARSER_PLUGIN_PATH, pluginPath, fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();

    R130InstDirGuard instDir(env.dir());
    R130ParserCounters counters(pluginPath);
    ASSERT_TRUE(counters.valid()) << "R130 lifecycle fake parser exports are unavailable";
    counters.reset();

    WTSVariant* cfg = WTSCfgLoader::load_from_content(
        "{\"module\":\"R130LifecycleParser\",\"r130_init_ok\":true}");
    ASSERT_NE(nullptr, cfg);
    const uint32_t cfgRetainBaseline = cfg->retainCount();
    R130ParserStub stub;
    r42fakes::R42HotMgr hot;

    ParserAdapter adapter;
    EXPECT_FALSE(adapter.init("r130_null_manager", cfg, &stub, nullptr, &hot));
    EXPECT_FALSE(adapter.isInitialized());
    EXPECT_FALSE(adapter.hasApi());
    EXPECT_FALSE(adapter.run());
    EXPECT_EQ(0u, counters.createCount());
    EXPECT_EQ(0u, counters.registerCount());
    EXPECT_EQ(0u, counters.initCount());
    EXPECT_EQ(0u, counters.subscribeCount());
    EXPECT_EQ(0u, counters.connectCount());
    EXPECT_EQ(cfgRetainBaseline, cfg->retainCount());

    adapter.release();
    EXPECT_EQ(0u, counters.releaseCount());
    EXPECT_EQ(0u, counters.deleteCount());
    EXPECT_EQ(cfgRetainBaseline, cfg->retainCount());
    cfg->release();
}

//////////////////////////////////////////////////////////////////
// 当天为节假日：任务时间命中也不触发（不创建调度线程）
// src/WtCore/WtSelEngine.cpp:233-244
//////////////////////////////////////////////////////////////////
TEST(R130MiscTail, SelMinuteEndSkipsTaskOnHoliday)
{
    r130::TempEnv env("r130misc");

    R130HolidayBase bd;
    bd.regisSessionWithTime("TRADING", 0, 2359);
    WTSSessionInfo* sess = bd.session("TRADING");
    WTSCommodityInfo* ag = q01sel::makeComm("SHFE", "ag", "TRADING", TM_Both);
    ag->setSessionInfo(sess);
    bd.regis(ag, q01sel::makeCt("SHFE", "ag", "ag2512"));
    bd.holiday = 20250110;

    q01sel::HotMgr hot;
    WtDtMgr dm;

    // WtSelEngine 构造未初始化 _tm_ticker（BUG-R42-001），堆分配且不回收
    WtSelEngine& engine = *new WtSelEngine();
    WTSVariant* cfg = sessionCfg();
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &bd, &dm, &hot, NULL);
    cfg->release();
    engine.set_date_time(20250110, 930, 0);
    engine.set_trading_date(20250110);

    SelContextPtr ctx(new q01sel::SelCtx(&engine, "r130holiday"));
    engine.addContext(ctx, 0, 931, TPT_Daily);

    engine.on_minute_end(20250110, 930);

    EXPECT_EQ(0u, static_cast<q01sel::SelCtx*>(ctx.get())->schedule_fires);
}

//////////////////////////////////////////////////////////////////
// run() 启动风控模块
// src/WtCore/WtCtaEngine.cpp:102-112
//////////////////////////////////////////////////////////////////
TEST(R130MiscTail, CtaRunStartsRiskMonitor)
{
    r130::TempEnv env("r130misc");

    q01sel::BaseDataMgr bd;
    bd.regisSessionWithTime("TRADING", 0, 2359);
    WTSSessionInfo* sess = bd.session("TRADING");
    WTSCommodityInfo* ag = q01sel::makeComm("SHFE", "ag", "TRADING", TM_Both);
    ag->setSessionInfo(sess);
    bd.regis(ag, q01sel::makeCt("SHFE", "ag", "ag2512"));

    q01sel::HotMgr hot;
    WtDtMgr dm;
    TraderAdapterMgr adapterMgr;

    WtCtaEngine engine;
    engine.set_adapter_mgr(&adapterMgr);
    WTSVariant* cfg = sessionCfg();
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &bd, &dm, &hot, NULL);
    cfg->release();

    R130RiskMon mon;
    R130RiskFact fact;
    WtRiskMonPtr risk(new WtRiskMonWrapper(&mon, &fact));
    engine.setRiskMonitor(risk);

    engine.run();

    EXPECT_EQ(1u, mon.runs);

    engine.release();
}
