/**
 * r202_wtcore_public_surface_test.cpp — lane R202 / wave28
 *
 * 目标：WtCore 剩余公开面里本轮可安全触达、且不依赖插件成功装载的部分
 *   - SelStraWrapper / HftStraWrapper 的空策略生命周期（两个头文件各 8 行）
 *   - WtCtaRtTicker 构造/析构（WtCtaTicker.h:42-48、54）
 *   - WtSelEngine 的 IExecuterStub inline 访问器与 addExecuter
 *     （WtSelEngine.h:75-76、85-88）
 *   - TraderAdapter 的公开查询面：isTradeEnabled / state
 *     （TraderAdapter.cpp:599-601；TraderAdapter.h:198）
 *   - WtHftRtTicker 数据驱动闭合时写 store（WtHftTicker.cpp:123）
 *
 * 另有一个 DISABLED_ 用例保留 TraderAdapter inline 弱符号问题的断言（BUG-R202-003），
 * 不参与覆盖率。
 *
 * 边界：只走公开 API；ticker 用 run()+on_tick()+stop()，行情由主线程同步送入，
 * 不等待墙钟；临时目录走系统 temp；不访问 private、不镜像类布局。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <string>

#include <unistd.h>

#include "q01_sel_env.h"

#include <Includes/IDataReader.h>
#include <Includes/ITraderApi.h>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <Share/TimeUtils.hpp>
#include <WTSUtils/WTSCfgLoader.h>
#include <WtCore/HftStrategyMgr.h>
#include <WtCore/SelStrategyMgr.h>
#include <WtCore/TraderAdapter.h>
#include <WtCore/WtCtaTicker.h>
#include <WtCore/WtDtMgr.h>
#include <WtCore/WtHftEngine.h>
#include <WtCore/WtHftTicker.h>
#include <WtCore/WtHelper.h>
#include <WtCore/WtSelEngine.h>

// TraderAdapter.h 把 getCommodify/isTradeEnabled 声明成 inline，定义却在 .cpp；
// 跨 TU 调用会触发 -Wundefined-inline，而本目标带 -Werror。这里只关掉该告警。
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wundefined-inline"
#endif

using namespace wtp;

namespace {

/** TraderAdapter::initExt 只要求 init() 成功，不需要真实柜台。 */
class R202Api : public ITraderApi
{
public:
    bool init(WTSVariant* /*params*/) override { return true; }
};

/** 记录 onMinuteEnd 的 IDataReader 替身。 */
class R202Reader : public IDataReader
{
public:
    void onMinuteEnd(uint32_t d, uint32_t t, uint32_t e = 0) override
    {
        last_date.store(d);
        last_time.store(t);
        last_end.store(e);
        count.fetch_add(1);
    }

    WTSTickSlice* readTickSlice(const char*, uint32_t, uint64_t = 0) override { return NULL; }
    WTSKlineSlice* readKlineSlice(const char*, WTSKlinePeriod, uint32_t, uint64_t = 0) override
    {
        return NULL;
    }

    std::atomic<uint32_t> count{0};
    std::atomic<uint32_t> last_date{0};
    std::atomic<uint32_t> last_time{0};
    std::atomic<uint32_t> last_end{0};
};

/** 24 小时交易时段 + ag2512 合约，供 HFT ticker 使用。 */
struct R202Market
{
    R202Market()
    {
        bd.regisSessionWithTime("TRADING", 0, 0);
        WTSCommodityInfo* comm = q01sel::makeComm("SHFE", "ag", "TRADING", TM_Both);
        comm->setSessionInfo(bd.session("TRADING"));
        bd.regis(comm, q01sel::makeCt("SHFE", "ag", "ag2512"));
    }

    WTSVariant* cfg() const
    {
        return WTSCfgLoader::load_from_content(
            "{\"product\":{\"session\":\"TRADING\"},\"fees\":\"\",\"filters\":\"\"}");
    }

    q01sel::BaseDataMgr bd;
    q01sel::HotMgr hot;
    WtDtMgr dm;
};

/** addExecuter 只需要一个 IExecCommand 实例；setStub 由引擎完成。 */
class R202Exec : public IExecCommand
{
public:
    R202Exec() : IExecCommand("r202") {}
    virtual ~R202Exec() {}
};

/* ------------------------------------------------------------------ *
 * 1. 两个策略包装器的空策略生命周期。
 *    SelStrategyMgr.h:22-31 / HftStrategyMgr.h:32-50
 * ------------------------------------------------------------------ */
TEST(R202StraWrapper, NullStrategyLifecycleIsSafe)
{
    SelStraWrapper sel(NULL, NULL);
    EXPECT_EQ(nullptr, sel.self());

    HftStraWrapper hft(NULL, NULL);
    EXPECT_EQ(nullptr, hft.self());
}

/* ------------------------------------------------------------------ *
 * 2. CTA ticker 的构造/析构（不 init、不 run，不触碰 _engine）。
 *    WtCtaTicker.h:42-48、54
 * ------------------------------------------------------------------ */
TEST(R202CtaTicker, ConstructAndStopWithoutEngine)
{
    WtCtaRtTicker ticker(NULL);
    ticker.stop();   // _thrd 为空，只置停止标记，安全
    SUCCEED();
}

/* ------------------------------------------------------------------ *
 * 3. WtSelEngine 的 inline 访问器 + addExecuter。
 *    WtSelEngine.h:75-76、85-88
 * ------------------------------------------------------------------ */
TEST(R202SelEngine, InlineStubAccessorsAndExecuterRegistration)
{
    q01sel::Q01TempEnv tmp("r202_selengine");
    R202Market mkt;
    WTSVariant* cfg = mkt.cfg();
    ASSERT_NE(nullptr, cfg);

    // BUG-R42-001：_tm_ticker 构造未初始化，析构 release 会读脏指针，堆分配且不回收
    WtSelEngine* engine = new WtSelEngine();
    engine->init(cfg, &mkt.bd, &mkt.dm, &mkt.hot, NULL);
    cfg->release();

    EXPECT_EQ(&mkt.hot, engine->get_hot_mon());
    engine->set_trading_date(20250110);
    EXPECT_EQ(20250110u, engine->get_trading_day());

    ExecCmdPtr exec = std::make_shared<R202Exec>();
    engine->addExecuter(exec);
    EXPECT_STREQ("r202", exec->name());
}

/* ------------------------------------------------------------------ *
 * 4. TraderAdapter 公开查询面：initExt 成功后的初始状态。
 *    state: TraderAdapter.h:198（1 行）
 *
 *    isTradeEnabled 本身无法在本 TU 调用：头文件把它声明成 inline，
 *    定义只在 TraderAdapter.cpp（BUG-R202-003/BUG-R215-003），
 *    目标可执行体内该符号是未定义弱符号，调用即跳空。断言保留下方
 *    的 DISABLED_ 用例，产品修复后去掉前缀即可复核 599-601。
 * ------------------------------------------------------------------ */
TEST(R202TraderAdapter, PublicStateAfterInitExt)
{
    q01sel::Q01TempEnv tmp("r202_trader");

    q01sel::BaseDataMgr bd;
    bd.regisSessionWithTime("TRADING", 0, 2359);
    WTSCommodityInfo* comm = q01sel::makeComm("SHFE", "ag", "TRADING", TM_Both);
    comm->setSessionInfo(bd.session("TRADING"));
    bd.regis(comm, q01sel::makeCt("SHFE", "ag", "ag2512"));

    R202Api api;
    TraderAdapter adapter;
    ASSERT_TRUE(adapter.initExt("r202_trader", &api, &bd, NULL));
    EXPECT_EQ(TraderAdapter::AS_NOTLOGIN, adapter.state());
}

/* BUG-R215-003（既有 BUG-R202-003）：isTradeEnabled 在头文件标 inline、
 * 定义只在 .cpp，WtCore 静态库不产出外部符号（nm 显示 `U`）。
 * 跨 TU 调用会跳到空地址。断言保持原样，修复定义可见性后再启用。 */
TEST(R202TraderAdapter, DISABLED_PublicTradeSwitchNeedsExternallyVisibleDefinition)
{
    q01sel::Q01TempEnv tmp("r202_trader_switch");

    q01sel::BaseDataMgr bd;
    bd.regisSessionWithTime("TRADING", 0, 2359);
    WTSCommodityInfo* comm = q01sel::makeComm("SHFE", "ag", "TRADING", TM_Both);
    comm->setSessionInfo(bd.session("TRADING"));
    bd.regis(comm, q01sel::makeCt("SHFE", "ag", "ag2512"));

    R202Api api;
    TraderAdapter adapter;
    ASSERT_TRUE(adapter.initExt("r202_trader_switch", &api, &bd, NULL));
    EXPECT_EQ(TraderAdapter::AS_NOTLOGIN, adapter.state());

    // 未启用风控：任何合约都允许交易
    EXPECT_TRUE(adapter.isTradeEnabled("SHFE.ag.2512"));
    EXPECT_TRUE(adapter.isTradeEnabled("SHFE.xx.9999"));
}

/* ------------------------------------------------------------------ *
 * 5. HFT ticker 数据驱动闭合时把分钟线写进 store。
 *    09:00 与 09:01 两笔行情让 09:00 分钟闭合，触发 :123。
 * ------------------------------------------------------------------ */
TEST(R202HftTicker, DataDrivenMinuteCloseReachesStore)
{
    q01sel::Q01TempEnv tmp("r202_hft");
    R202Market mkt;
    R202Reader reader;

    WtHftEngine engine;
    WTSVariant* cfg = mkt.cfg();
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &mkt.bd, &mkt.dm, &mkt.hot, NULL);
    cfg->release();
    engine.set_date_time(20250110, 930, 0);
    engine.set_trading_date(20250110);

    WtHftRtTicker ticker(&engine);
    ticker.init(&reader, "TRADING");
    ticker.run();

    const uint32_t tickDate = TimeUtils::getNextDate(TimeUtils::getCurDate(), 1);
    WTSTickData* tickA = q01sel::makeTick("SHFE", "ag2512", tickDate, tickDate, 90000000, 3500.0);
    tickA->setContractInfo(mkt.bd.getContract("ag2512", "SHFE"));
    ticker.on_tick(tickA);
    tickA->release();

    WTSTickData* tickB = q01sel::makeTick("SHFE", "ag2512", tickDate, tickDate, 90100000, 3501.0);
    tickB->setContractInfo(mkt.bd.getContract("ag2512", "SHFE"));
    ticker.on_tick(tickB);
    tickB->release();

    ticker.stop();

    // 闭合的是 _cur_pos 对应的 09:01 前一根，即 09:01 标签（正文按下一分钟记账）
    ASSERT_EQ(1u, reader.count.load());
    EXPECT_EQ(tickDate, reader.last_date.load());
    EXPECT_EQ(901u, reader.last_time.load());
}

/* ------------------------------------------------------------------ *
 * Full-day session metadata keeps the explicit midnight close label at 2400;
 * minute zero remains the separate 00:00 label.
 * ------------------------------------------------------------------ */
TEST(R202SelTicker, FullDaySessionKeepsExplicitMidnightCloseLabel)
{
    WTSSessionInfo* session = WTSSessionInfo::create("TRADING", "TRADING", 0);
    session->addTradingSection(0, 0);

    EXPECT_EQ(2400u, session->minuteToTime(1440));
    EXPECT_EQ(0u, session->minuteToTime(0));
    EXPECT_EQ(2400u, session->getCloseTime(true));
    session->release();
}

/* ------------------------------------------------------------------ *
 * Session close labels and clock offsets are distinct at midnight. This checks
 * session metadata only and does not reproduce a ticker on_session_end callback.
 * ------------------------------------------------------------------ */
TEST(R202HftTicker, FullDaySessionCloseLabelAndClockOffsetAreDistinct)
{
    WTSSessionInfo* session = WTSSessionInfo::create("TRADING", "TRADING", 0);
    session->addTradingSection(0, 0);

    EXPECT_EQ(2400u, session->minuteToTime(1440));
    EXPECT_EQ(0u, session->offsetTime(2400, true));
    EXPECT_EQ(2400u, session->getCloseTime(true));
    session->release();
}

}  // namespace
