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

#include <string>

#include "q01_sel_env.h"
#include "r130_env.h"
#include "r90_env.h"

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

    ParserAdapter adapter;
    EXPECT_FALSE(adapter.init("r130_missing_parser", cfg, &stub, &bd, &hot));
    cfg->release();

    WtHelper::setInstDir(oldInst.c_str());
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
