/**
 * r130_ticker_on_tick_test.cpp — lane R130 / wave21（R144 恢复并修复）
 *
 * 目标：三个实时 ticker 的 on_tick 分钟收盘驱动链
 *   - WtCtaRtTicker::on_tick  -> WtCtaEngine::on_schedule / on_session_end
 *   - WtSelRtTicker::on_tick  -> WtSelEngine::on_minute_end
 *   - WtHftRtTicker::on_tick  -> WtHftEngine::on_minute_end
 *
 * 原文件在第二十一波构建后丢失，本 lane 按 rc/目标文件残留的用例名恢复。
 * 与原文的差异仅两处夹具修复：
 *   1) Cta 用例补 TraderAdapterMgr：WtCtaEngine::on_schedule 的 push_task
 *      会无条件 `_adapter_mgr->refresh_funds()`，原夹具没有 adapter mgr，
 *      任务线程解引用 NULL（本波 CRASH 根因）。
 *   2) Cta 用例在 ticker.run() 前显式 init 引擎（WtCtaEngine::_cfg 由 init
 *      赋值，析构无条件 release，否则同样会读未初始化指针）。
 * 断言与原文一致：引擎日期/交易日等于 tick 日期，收盘后 min_time 落在
 * [930, 940] 区间。Sel/Hft 用例保持原文行为（Sel 引擎仍堆分配不回收，
 * 规避构造函数未初始化 _tm_ticker 的既有缺陷）。
 */

#include <gtest/gtest.h>

#include "q01_sel_env.h"
#include "r130_env.h"

#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>
#include <WtCore/TraderAdapter.h>
#include <WtCore/WtDtMgr.h>
#include <WtCore/WtCtaEngine.h>
#include <WtCore/WtCtaTicker.h>
#include <WtCore/WtHftEngine.h>
#include <WtCore/WtHftTicker.h>
#include <WtCore/WtSelEngine.h>
#include <WtCore/WtSelTicker.h>

using namespace wtp;

namespace {

/** 基础数据替身 + tick 工厂：品种 SHFE.ag，合约 ag2512，日期固定 20300110 */
struct R130TickEnv
{
    q01sel::BaseDataMgr bd;
    q01sel::HotMgr hot;
    WtDtMgr dm;

    R130TickEnv()
    {
        bd.regisSessionWithTime("TRADING", 0, 2359);
        WTSSessionInfo* sess = bd.session("TRADING");
        WTSCommodityInfo* ag = q01sel::makeComm("SHFE", "ag", "TRADING", TM_Both);
        ag->setSessionInfo(sess);
        bd.regis(ag, q01sel::makeCt("SHFE", "ag", "ag2512"));
    }

    WTSVariant* cfg() const
    {
        return WTSCfgLoader::load_from_content(
            "{\"product\":{\"session\":\"TRADING\"},\"fees\":\"\",\"filters\":\"\"}");
    }

    WTSTickData* tick(uint32_t atime, double price)
    {
        WTSTickData* t = q01sel::makeTick(
            "SHFE", "ag2512", 20300110, 20300110, atime, price);
        t->setContractInfo(bd.getContract("ag2512", "SHFE"));
        return t;
    }
};

}  // namespace

//////////////////////////////////////////////////////////////////
// Cta：930/931/931.5 三笔 tick，第二笔触发 930 分钟闭合
// src/WtCore/WtCtaTicker.cpp:127-166；WtCtaEngine.cpp:388-417
//////////////////////////////////////////////////////////////////
TEST(R130TickerOnTick, CtaTickerDrivesEngineThroughMinuteClose)
{
    r130::TempEnv env("r130ticker");
    R130TickEnv e;

    // adapter mgr 必须先于 engine 构造，保证 engine 析构 join 任务线程时仍有效
    TraderAdapterMgr adapterMgr;

    WtCtaEngine engine;
    WTSVariant* cfg = e.cfg();
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &e.bd, &e.dm, &e.hot, NULL);
    cfg->release();
    engine.set_adapter_mgr(&adapterMgr);

    WtCtaRtTicker ticker(&engine);
    ticker.init(e.dm.reader(), "TRADING");
    ticker.run();

    WTSTickData* t1 = e.tick(93000000, 3500.0);
    ticker.on_tick(t1);
    t1->release();

    WTSTickData* t2 = e.tick(93100000, 3504.0);
    ticker.on_tick(t2);
    t2->release();

    WTSTickData* t3 = e.tick(93130000, 3508.0);
    ticker.on_tick(t3);
    t3->release();

    ticker.stop();

    EXPECT_EQ(20300110u, engine.get_date());
    EXPECT_EQ(20300110u, engine.get_trading_date());
    EXPECT_GE(engine.get_min_time(), 930u);
    EXPECT_LE(engine.get_min_time(), 940u);
}

//////////////////////////////////////////////////////////////////
// Sel：同样的三笔 tick 驱动 on_minute_end
// src/WtCore/WtSelTicker.cpp:90-155
//////////////////////////////////////////////////////////////////
TEST(R130TickerOnTick, SelTickerDrivesEngineThroughMinuteClose)
{
    r130::TempEnv env("r130ticker");
    R130TickEnv e;

    // WtSelEngine 构造未初始化 _tm_ticker（BUG-R42-001），堆分配且不回收
    WtSelEngine& engine = *new WtSelEngine();
    WTSVariant* cfg = e.cfg();
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &e.bd, &e.dm, &e.hot, NULL);
    cfg->release();

    WtSelRtTicker ticker(&engine);
    ticker.init(e.dm.reader(), "TRADING");
    ticker.run();

    WTSTickData* t1 = e.tick(93000000, 3500.0);
    ticker.on_tick(t1, 0);
    t1->release();

    WTSTickData* t2 = e.tick(93100000, 3504.0);
    ticker.on_tick(t2, 0);
    t2->release();

    WTSTickData* t3 = e.tick(93130000, 3508.0);
    ticker.on_tick(t3, 0);
    t3->release();

    ticker.stop();

    EXPECT_EQ(20300110u, engine.get_date());
    EXPECT_EQ(20300110u, engine.get_trading_date());
    EXPECT_GE(engine.get_min_time(), 930u);
    EXPECT_LE(engine.get_min_time(), 940u);
}

//////////////////////////////////////////////////////////////////
// Hft：同样的三笔 tick 驱动 on_minute_end
// src/WtCore/WtHftTicker.cpp:120-157
//////////////////////////////////////////////////////////////////
TEST(R130TickerOnTick, HftTickerDrivesEngineThroughMinuteClose)
{
    r130::TempEnv env("r130ticker");
    R130TickEnv e;

    WtHftEngine engine;
    WTSVariant* cfg = e.cfg();
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &e.bd, &e.dm, &e.hot, NULL);
    cfg->release();

    WtHftRtTicker ticker(&engine);
    ticker.init(e.dm.reader(), "TRADING");
    ticker.run();

    WTSTickData* t1 = e.tick(93000000, 3500.0);
    ticker.on_tick(t1);
    t1->release();

    WTSTickData* t2 = e.tick(93100000, 3504.0);
    ticker.on_tick(t2);
    t2->release();

    WTSTickData* t3 = e.tick(93130000, 3508.0);
    ticker.on_tick(t3);
    t3->release();

    ticker.stop();

    EXPECT_EQ(20300110u, engine.get_date());
    EXPECT_EQ(20300110u, engine.get_trading_date());
    EXPECT_GE(engine.get_min_time(), 930u);
    EXPECT_LE(engine.get_min_time(), 940u);
}
