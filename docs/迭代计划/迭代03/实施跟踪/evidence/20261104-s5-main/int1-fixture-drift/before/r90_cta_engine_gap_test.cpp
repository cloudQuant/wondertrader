/**
 * r90_cta_engine_gap_test.cpp — lane R90 / wave17
 *
 * 目标：src/WtCore/WtCtaEngine.cpp 残余公开面。
 *
 * 覆盖行段：
 *   - on_init/on_schedule 的策略过滤 redirect/ignore 与风控缩放：:159-188, 282-314, 323-354, 359-379
 *   - handle_pos_change 过滤早退：:429-437
 *   - 有线程池的 tick/bar 分发、后复权订阅：:543-566, 613-634, 652-668
 *   - run() 的 marker.json 导出 + ticker 生命周期 + isInTrading/transTimeToMin：:49-114, 673-681
 *   - 非活跃 notifier 的图表通知守卫：:704-719
 *
 * 用 r73 的确定性引擎/数据替身；run() 用 24h 内 09:31 的固定时间，ticker 线程
 * 首轮立即被 release 停止，不等待 10s 墙钟。
 */
#include "../n02_wtcore_engines/r73_fixture.h"
#include "r90_env.h"

#include <WtCore/WtCtaEngine.h>
#include <WtCore/WtEngine.h>
#include <WtCore/IExecCommand.h>
#include <WtCore/EventNotifier.h>
#include <WtCore/TraderAdapter.h>
#include <WTSUtils/WTSCfgLoader.h>

#include <fstream>

using namespace wtp;

namespace {

::testing::Environment* const g_r90_env =
    ::testing::AddGlobalTestEnvironment(new r90::GenEnv);

const char* const kCode = "SHFE.ag.2512";

class RecExecuter : public IExecCommand
{
public:
    explicit RecExecuter(const char* name) : IExecCommand(name) {}
    virtual ~RecExecuter() {}

    void set_position(const wt_hashmap<std::string, double>& targets) override
    {
        last = targets;
        ++sets;
    }

    wt_hashmap<std::string, double> last;
    uint32_t sets = 0;
};

class R90Ctx : public CtaStraBaseCtx
{
public:
    R90Ctx(WtCtaEngine* engine, const char* name)
        : CtaStraBaseCtx(engine, name, 0) {}

    void on_init() override { ++inits; }
    void on_session_begin(uint32_t) override { ++sessions; }
    void on_session_end(uint32_t) override {}
    bool on_schedule(uint32_t, uint32_t) override { ++scheds; return true; }

    void on_tick(const char* stdCode, WTSTickData*, bool) override
    {
        ++ticks;
        last_tick_code = stdCode;
    }

    void on_bar(const char* stdCode, const char* period, uint32_t, WTSBarStruct*) override
    {
        ++bars;
        last_bar_key = std::string(stdCode) + "-" + period;
    }

    void on_calculate(uint32_t, uint32_t) override {}
    void on_bar_close(const char*, const char*, WTSBarStruct*) override {}

    void enum_position(FuncEnumCtaPosCallBack cb, bool) override
    {
        for (const auto& p : positions)
            cb(p.first.c_str(), p.second);
    }

    std::vector<std::pair<std::string, double>> positions;
    uint32_t inits = 0;
    uint32_t sessions = 0;
    uint32_t scheds = 0;
    uint32_t ticks = 0;
    uint32_t bars = 0;
    std::string last_tick_code;
    std::string last_bar_key;
};

class R90CtaEngine : public ::testing::Test
{
protected:
    r73::BaseDataMgr bd;
    r73::HotMgr hot;
    r73::DataMgr dm;
    TraderAdapterMgr adapterMgr;
    WTSSessionInfo* sess = NULL;

    void SetUp() override
    {
        WtHelper::setGenerateDir(r90::temp_root().c_str());
        WtHelper::setTime(20250110, 931, 0);

        sess = bd.regisSession("TRADING");
        ASSERT_NE(nullptr, sess);
        WTSCommodityInfo* comm = r73::makeComm("SHFE", "ag", "TRADING", TM_Both);
        comm->setSessionInfo(sess);
        bd.regis(comm, r73::makeCt("SHFE", "ag", "ag2512"));
        // 第二品种，供过滤/多上下文用例使用
        WTSCommodityInfo* au = r73::makeComm("SHFE", "au", "TRADING", TM_Both);
        au->setSessionInfo(sess);
        bd.regis(au, r73::makeCt("SHFE", "au", "au2212"));
    }

    WTSVariant* makeCfg(uint32_t poolsize, const std::string& filters)
    {
        std::string json = "{\"product\":{\"session\":\"TRADING\"},\"poolsize\":"
            + std::to_string(poolsize) + ",\"fees\":\"\",\"filters\":\"" + filters + "\"}";
        return WTSCfgLoader::load_from_content(json.c_str());
    }

    void initEngine(WtCtaEngine& engine, uint32_t poolsize, const std::string& filters,
        EventNotifier* notifier = NULL)
    {
        WTSVariant* cfg = makeCfg(poolsize, filters);
        ASSERT_NE(nullptr, cfg);
        engine.set_adapter_mgr(&adapterMgr);
        engine.init(cfg, &bd, &dm, &hot, notifier);
        cfg->release();
        engine.set_date_time(20250110, 931, 0);
        engine.set_trading_date(20250110);
    }

    WTSTickData* makeTick(double price)
    {
        WTSTickData* tick = WTSTickData::create(kCode);
        WTSTickStruct& ts = tick->getTickStruct();
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
        tick->setContractInfo(bd.getContract("ag2512", "SHFE"));
        return tick;
    }
};

//////////////////////////////////////////////////////////////////
// 过滤器 redirect/ignore + 风控缩放；on_init 与 on_schedule 两处
// src/WtCore/WtCtaEngine.cpp:159-188, 282-314, 323-354, 359-379
//////////////////////////////////////////////////////////////////
TEST_F(R90CtaEngine, FilterRedirectIgnoreAndRiskScale)
{
    std::string filterPath = r90::write_file("filters/cta_filters.json",
        "{\"strategy_filters\":{"
        "\"r90redir\":{\"action\":\"redirect\",\"target\":5},"
        "\"r90ignore\":{\"action\":\"ignore\"}}}");

    WtCtaEngine engine;
    initEngine(engine, 0, filterPath);
    engine.setVolScale(1.0);

    std::shared_ptr<RecExecuter> rawExec(new RecExecuter("r90exec"));
    engine.addExecuter(ExecCmdPtr(rawExec));

    CtaContextPtr redir(new R90Ctx(&engine, "r90redir"));
    CtaContextPtr ignore(new R90Ctx(&engine, "r90ignore"));
    static_cast<R90Ctx*>(redir.get())->positions = { { kCode, 3.0 } };
    static_cast<R90Ctx*>(ignore.get())->positions = { { kCode, 2.0 } };
    engine.addContext(redir);
    engine.addContext(ignore);

    // on_init：redirect 把 3 改成 5（打 reset 日志），ignore 整条丢弃
    engine.on_init();
    ASSERT_EQ(1u, rawExec->sets);
    ASSERT_NE(rawExec->last.end(), rawExec->last.find(kCode));
    EXPECT_DOUBLE_EQ(5.0, rawExec->last.find(kCode)->second);

    // on_schedule 无池分支：同样的过滤 + 组合理论仓位风控缩放 2x
    engine.setVolScale(2.0);
    engine.on_schedule(20250110, 931);
    EXPECT_EQ(1u, static_cast<R90Ctx*>(redir.get())->scheds);
    ASSERT_EQ(2u, rawExec->sets);
    EXPECT_DOUBLE_EQ(10.0, rawExec->last.find(kCode)->second);

    // handle_pos_change：ignore 策略的增量在入口被过滤
    engine.handle_pos_change("r90ignore", kCode, 1.0);
    // 未过滤策略的增量 -> 组合仓位 + 执行器路由（无路由时只落 _pos_map）
    engine.handle_pos_change("r90redir", kCode, 1.0);
    EXPECT_EQ(2u, rawExec->sets);
}

//////////////////////////////////////////////////////////////////
// 有线程池：tick/bar 分发（原始 + 后复权订阅）
// src/WtCore/WtCtaEngine.cpp:543-566, 613-634, 652-668
//////////////////////////////////////////////////////////////////
TEST_F(R90CtaEngine, PooledTickAndBarDispatch)
{
    WtCtaEngine engine;
    initEngine(engine, 2, "");

    CtaContextPtr raw(new R90Ctx(&engine, "r90raw"));
    CtaContextPtr hfq(new R90Ctx(&engine, "r90hfq"));
    engine.addContext(raw);
    engine.addContext(hfq);

    engine.on_session_begin();
    engine.sub_tick(raw->id(), kCode);
    engine.sub_tick(hfq->id(), std::string(std::string(kCode) + "+").c_str());

    // 注册 bar 订阅（get_kline_slice 会写 _bar_sub_map）
    WTSKlineSlice* slice = engine.get_kline_slice(raw->id(), kCode, "d", 10);
    ASSERT_NE(nullptr, slice);
    slice->release();

    WTSTickData* tick = makeTick(3500.0);
    engine.on_tick(kCode, tick);
    tick->release();

    WTSBarStruct bar;
    memset(&bar, 0, sizeof(bar));
    bar.date = 20250110;
    bar.open = 3500;
    bar.high = 3510;
    bar.low = 3490;
    bar.close = 3505;
    engine.on_bar(kCode, "d", 1, &bar);

    EXPECT_EQ(1u, static_cast<R90Ctx*>(raw.get())->ticks);
    EXPECT_EQ(1u, static_cast<R90Ctx*>(hfq.get())->ticks);
    EXPECT_EQ(std::string(kCode), static_cast<R90Ctx*>(raw.get())->last_tick_code);
    EXPECT_EQ(std::string(std::string(kCode) + "+"),
        static_cast<R90Ctx*>(hfq.get())->last_tick_code);
    EXPECT_EQ(1u, static_cast<R90Ctx*>(raw.get())->bars);
    EXPECT_EQ(std::string(std::string(kCode) + "-d"),
        static_cast<R90Ctx*>(raw.get())->last_bar_key);
}

//////////////////////////////////////////////////////////////////
// run()：marker.json 导出、ticker 起停、isInTrading/transTimeToMin、通知守卫
// src/WtCore/WtCtaEngine.cpp:49-114, 673-681, 704-719
//////////////////////////////////////////////////////////////////
TEST_F(R90CtaEngine, RunExportsMarkerAndTickerWrappers)
{
    std::string filterPath = r90::write_file("filters/cta_empty.json", "{}");
    WtCtaEngine engine;
    EventNotifier notifier;  // 未 init，notify 全部短路
    initEngine(engine, 0, filterPath, &notifier);

    CtaContextPtr ctx(new R90Ctx(&engine, "r90run"));
    engine.addContext(ctx);

    // ticker 线程按本机墙钟判断是否在交易时段；加一段 0-2400 使线程首轮即进入
    // 10ms 短睡而非 10s 长睡，release() 可立即 join（不依赖具体运行时点）。
    sess->addTradingSection(0, 2400);

    engine.notify_chart_marker(20250110093100000ULL, "r90run", 3500.0, "icon", "tag");
    engine.notify_chart_index(20250110093100000ULL, "r90run", "idx", "line", 1.0);
    engine.notify_trade("r90run", kCode, true, true, 20250110093100000ULL, 3500.0, "tag");

    engine.run();
    // ticker 已创建：交易时段内，时间转换走 session
    EXPECT_TRUE(engine.isInTrading());
    EXPECT_GT(engine.transTimeToMin(931), 0u);

    std::string marker = std::string(WtHelper::getBaseDir()) + "marker.json";
    std::string body = r90::read_file(marker);
    EXPECT_NE(std::string::npos, body.find("\"engine\""));
    EXPECT_NE(std::string::npos, body.find("CTA"));
    EXPECT_NE(std::string::npos, body.find("r90run"));

    // ticker 已创建：handle_push_quote 转发一笔行情
    WTSTickData* rt = makeTick(3500.0);
    engine.handle_push_quote(rt);
    rt->release();
    EXPECT_EQ(1u, static_cast<R90Ctx*>(ctx.get())->ticks);

    engine.release();
}

}  // namespace
