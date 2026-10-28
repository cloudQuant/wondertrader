/**
 * r110_cta_rule_run_test.cpp — lane R110 / wave19
 *
 * 目标：src/WtCore/WtCtaEngine.cpp 的自定义规则标签、过滤器与 run() 装配残余。
 *
 * 覆盖行段：
 *   - run()：通道列表/执行器列表导出 + ticker 生命周期：:61-114
 *   - on_init 的 HOT 规则换月：:172-181
 *   - on_schedule 非池/池两分支的 HOT 规则与过滤器忽略：:282-314, 323-354
 *   - handle_pos_change 的 HOT 规则换月：:439-446
 *   - on_tick 前复权（opt=1）的池调度分支：:543-566
 *
 * run() 启动的 ticker 线程在用例末尾 release() 时 stop/join（10ms 轮询）。
 */
#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <vector>

#include "../n02_wtcore_engines/r73_fixture.h"
#include "r90_env.h"

#include <WtCore/IExecCommand.h>
#include <WtCore/TraderAdapter.h>
#include <WtCore/WtCtaEngine.h>
#include <WTSUtils/WTSCfgLoader.h>

using namespace wtp;

namespace {

const char* const kCode = "SHFE.ag.2512";

class RecExec : public IExecCommand
{
public:
    explicit RecExec(const char* name) : IExecCommand(name) {}
    virtual ~RecExec() {}

    void set_position(const wt_hashmap<std::string, double>& targets) override
    {
        last = targets;
        ++sets;
    }

    wt_hashmap<std::string, double> last;
    uint32_t sets = 0;
};

class R110Ctx : public CtaStraBaseCtx
{
public:
    R110Ctx(WtCtaEngine* engine, const char* name)
        : CtaStraBaseCtx(engine, name, 0) {}

    void on_init() override { ++inits; }
    void on_session_begin(uint32_t) override {}
    void on_session_end(uint32_t) override {}
    bool on_schedule(uint32_t, uint32_t) override { ++scheds; return true; }

    void on_tick(const char* stdCode, WTSTickData*, bool) override
    {
        ++ticks;
        last_tick = stdCode;
    }

    void on_bar(const char*, const char*, uint32_t, WTSBarStruct*) override {}
    void on_calculate(uint32_t, uint32_t) override {}
    void on_bar_close(const char*, const char*, WTSBarStruct*) override {}

    void enum_position(FuncEnumCtaPosCallBack cb, bool) override
    {
        for (const auto& p : positions)
            cb(p.first.c_str(), p.second);
    }

    std::vector<std::pair<std::string, double>> positions;
    uint32_t inits = 0;
    uint32_t scheds = 0;
    uint32_t ticks = 0;
    std::string last_tick;
};

class R110CtaBase
{
public:
    R110CtaBase()
    {
        WtHelper::setGenerateDir(r90::temp_root().c_str());
        WtHelper::setTime(20250110, 931, 0);
        sess = bd.regisSession("TRADING");
        sess->addTradingSection(900, 1500);

        WTSCommodityInfo* ag = r73::makeComm("SHFE", "ag", "TRADING", TM_Both);
        ag->setSessionInfo(sess);
        bd.regis(ag, r73::makeCt("SHFE", "ag", "ag2512"));

        WTSCommodityInfo* au = r73::makeComm("SHFE", "au", "TRADING", TM_Both);
        au->setSessionInfo(sess);
        bd.regis(au, r73::makeCt("SHFE", "au", "au2212"));

        hot.rule_tag = "HOT";
        hot.prev_custom = "ag2512";
    }

    WTSVariant* cfg(uint32_t poolsize, const std::string& filters)
    {
        const std::string json = "{\"product\":{\"session\":\"TRADING\"},\"poolsize\":"
            + std::to_string(poolsize) + ",\"fees\":\"\",\"filters\":\"" + filters + "\"}";
        return WTSCfgLoader::load_from_content(json.c_str());
    }

    void init(WtCtaEngine& engine, uint32_t poolsize, const std::string& filters)
    {
        WTSVariant* c = cfg(poolsize, filters);
        ASSERT_NE(nullptr, c);
        engine.set_adapter_mgr(&adapterMgr);
        engine.init(c, &bd, &dm, &hot, NULL);
        c->release();
        engine.set_date_time(20250110, 931, 0);
        engine.set_trading_date(20250110);
    }

    WTSTickData* tick(double price)
    {
        WTSTickData* t = WTSTickData::create(kCode);
        WTSTickStruct& ts = t->getTickStruct();
        ts.price = price;
        ts.open = price;
        ts.high = price;
        ts.low = price;
        ts.pre_close = price;
        ts.volume = 1;
        ts.total_volume = 1;
        ts.action_date = 20250110;
        ts.action_time = 93100000;
        ts.trading_date = 20250110;
        t->setContractInfo(bd.getContract("ag2512", "SHFE"));
        return t;
    }

    r73::BaseDataMgr   bd;
    r73::HotMgr        hot;
    r73::DataMgr       dm;
    TraderAdapterMgr   adapterMgr;
    WTSSessionInfo*    sess = NULL;
};

//////////////////////////////////////////////////////////////////
// 非池：HOT 规则换月 + 策略过滤器忽略（on_init/on_schedule/handle_pos_change）
// src/WtCore/WtCtaEngine.cpp:172-181, 282-314, 323-354, 439-446
//////////////////////////////////////////////////////////////////
TEST(R110CtaRule, RulesAndFiltersOnInitScheduleAndPosChange)
{
    R110CtaBase env;
    const std::string filterPath = r90::write_file("r110_filters/cta.json",
        "{\"strategy_filters\":{\"r110ignore\":{\"action\":\"ignore\"}}}");

    WtCtaEngine engine;
    env.init(engine, 0, filterPath);

    std::shared_ptr<RecExec> exec(new RecExec("r110exec"));
    engine.addExecuter(ExecCmdPtr(exec));

    CtaContextPtr rule(new R110Ctx(&engine, "r110rule"));
    CtaContextPtr ignore(new R110Ctx(&engine, "r110ignore"));
    static_cast<R110Ctx*>(rule.get())->positions = { { "SHFE.ag.HOT", 3.0 } };
    static_cast<R110Ctx*>(ignore.get())->positions = { { kCode, 2.0 } };
    engine.addContext(rule);
    engine.addContext(ignore);

    // on_init：HOT 规则还原为 SHFE.ag.2512 后进入执行器；ignore 整条丢弃
    engine.on_init();
    ASSERT_EQ(1u, exec->sets);
    ASSERT_NE(exec->last.end(), exec->last.find(kCode));
    EXPECT_DOUBLE_EQ(3.0, exec->last.find(kCode)->second);

    // on_schedule：非池分支同样的规则/过滤，再叠加 2x 风控缩放
    engine.setVolScale(2.0);
    engine.on_schedule(20250110, 931);
    EXPECT_EQ(1u, static_cast<R110Ctx*>(rule.get())->scheds);
    EXPECT_EQ(2u, exec->sets);
    EXPECT_DOUBLE_EQ(6.0, exec->last.find(kCode)->second);

    // handle_pos_change：HOT 规则换月后写入信号表（价格未命中时挂起）
    engine.handle_pos_change("r110rule", "SHFE.ag.HOT", 1.0);
}

//////////////////////////////////////////////////////////////////
// run()：通道/执行器列表导出、ticker 启动；池内前复权 tick 分发与
// on_schedule 的 HOT 规则分支
// src/WtCore/WtCtaEngine.cpp:61-114, 339-342, 543-566
//////////////////////////////////////////////////////////////////
TEST(R110CtaRule, RunExportsListsAndPoolForwardsQfqTick)
{
    R110CtaBase env;

    WtCtaEngine engine;
    env.init(engine, 2, "");

    std::shared_ptr<RecExec> exec(new RecExec("r110runexec"));
    engine.addExecuter(ExecCmdPtr(exec));

    TraderAdapterPtr chn(new TraderAdapter());
    ASSERT_TRUE(env.adapterMgr.addAdapter("r110chn", chn));

    CtaContextPtr ctx(new R110Ctx(&engine, "r110pool"));
    static_cast<R110Ctx*>(ctx.get())->positions = { { "SHFE.ag.HOT", 2.0 } };
    engine.addContext(ctx);

    engine.run();

    const std::string marker = std::string(WtHelper::getBaseDir()) + "marker.json";
    EXPECT_TRUE(std::filesystem::exists(marker));

    // 前复权订阅：opt=1 走池调度 lambda
    engine.sub_tick(static_cast<R110Ctx*>(ctx.get())->id(), "SHFE.ag.2512-");
    WTSTickData* t = env.tick(3500.0);
    engine.on_tick(kCode, t);
    t->release();

    // 池内 on_schedule（wait 会等待前面的 tick 任务完成），并覆盖池分支的 HOT 规则
    engine.on_schedule(20250110, 931);
    EXPECT_EQ(1u, static_cast<R110Ctx*>(ctx.get())->scheds);
    EXPECT_EQ(1u, static_cast<R110Ctx*>(ctx.get())->ticks);
    EXPECT_EQ("SHFE.ag.2512-", static_cast<R110Ctx*>(ctx.get())->last_tick);
    EXPECT_EQ(1u, exec->sets);

    engine.release();
}

}  // namespace
