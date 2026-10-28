/**
 * r170_ticker_run_test.cpp — lane R170 / wave25
 *
 * 目标：WtCtaRtTicker / WtSelRtTicker 的 run() 自动闭合线程体。
 * 已有用例要么没 run()，要么运行时刻不在交易时段，线程只走 10 秒休市 sleep；
 * 本用例把会话设成 00:00-04:00，并预置「03:58 已处理、待闭合 03:59」，
 * 线程首轮即闭合 03:59；闭合后 _time=0359 仍在交易时段，后续只 10ms 轮询，
 * stop() 可立即 join。
 *
 * _date/_time/_cur_pos/_last_emit_pos/_next_check_time 为 private；沿用本仓
 * r105/r160 的 `#define private public` 手法，只包住两个 ticker 头并立即 #undef。
 *
 * 覆盖行段：
 *   - WtCtaTicker.cpp:195-232、:236-238（非收盘分钟的自动闭合 + set_date_time）
 *   - WtSelTicker.cpp:179-218、:220-223（同上）
 */
#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "r170_env.h"

#define private public
#include <WtCore/WtCtaTicker.h>
#include <WtCore/WtSelTicker.h>
#undef private

#include <Includes/WTSVariant.hpp>
#include <WtCore/TraderAdapter.h>
#include <WtCore/WtCtaEngine.h>
#include <WtCore/WtDtMgr.h>
#include <WtCore/WtSelEngine.h>
#include <WTSUtils/WTSCfgLoader.h>

using namespace wtp;

//////////////////////////////////////////////////////////////////
// CTA：00:00-04:00 会话，预置待闭合分钟 03:59
// src/WtCore/WtCtaTicker.cpp:189-238
//////////////////////////////////////////////////////////////////
TEST(R170TickerRun, CtaAutoCloseAdvancesMinute)
{
    r170::TempEnv tmp("cta_run");
    q01sel::BaseDataMgr bd;
    q01sel::HotMgr hot;
    r170::makeAg(bd, 400);

    WtDtMgr dm;
    TraderAdapterMgr adapterMgr;

    WtCtaEngine engine;
    WTSVariant* cfg = WTSCfgLoader::load_from_content(
        "{\"product\":{\"session\":\"TRADING\"},\"fees\":\"\",\"filters\":\"\"}");
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &bd, &dm, &hot, NULL);
    cfg->release();
    engine.set_adapter_mgr(&adapterMgr);
    engine.set_date_time(20250110, 358, 0);
    engine.set_trading_date(20250110);

    WtCtaRtTicker ticker(&engine);
    ticker.init(NULL, "TRADING");
    ASSERT_NE(nullptr, ticker._s_info);

    ticker._date = 20250110;
    ticker._time = 35800000;
    ticker._cur_pos = 239;          // minuteToTime(239)=03:59
    ticker._last_emit_pos = 238;
    ticker._next_check_time = 0;

    ticker.run();

    bool closed = false;
    for (int i = 0; i < 200 && !closed; ++i)
    {
        closed = (engine.get_min_time() == 359);
        if (!closed)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    EXPECT_TRUE(closed) << "CTA ticker thread did not close 03:59";
    EXPECT_EQ(239u, ticker._last_emit_pos.load());
    EXPECT_EQ(35900000u, ticker._time);

    ticker.stop();
}

//////////////////////////////////////////////////////////////////
// SEL：同一预置，验证 on_minute_end/set_date_time 自动闭合
// src/WtCore/WtSelTicker.cpp:175-223
//////////////////////////////////////////////////////////////////
TEST(R170TickerRun, SelAutoCloseAdvancesMinute)
{
    r170::TempEnv tmp("sel_run");
    q01sel::BaseDataMgr bd;
    q01sel::HotMgr hot;
    r170::makeAg(bd, 400);

    WtDtMgr dm;

    // BUG-R42-001：WtSelEngine::_tm_ticker 构造未初始化，析构 release 会读脏指针，
    // 因此引擎堆分配且不回收（与 r110/r130 同处理）。
    WtSelEngine& engine = *new WtSelEngine();
    WTSVariant* cfg = WTSCfgLoader::load_from_content(
        "{\"product\":{\"session\":\"TRADING\"},\"fees\":\"\",\"filters\":\"\"}");
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &bd, &dm, &hot, NULL);
    cfg->release();
    engine.set_date_time(20250110, 358, 0);
    engine.set_trading_date(20250110);

    WtSelRtTicker ticker(&engine);
    ticker.init(NULL, "TRADING");
    ASSERT_NE(nullptr, ticker._s_info);

    ticker._date = 20250110;
    ticker._time = 35800000;
    ticker._cur_pos = 239;
    ticker._last_emit_pos = 238;
    ticker._next_check_time = 0;

    ticker.run();

    bool closed = false;
    for (int i = 0; i < 200 && !closed; ++i)
    {
        closed = (engine.get_min_time() == 359);
        if (!closed)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    EXPECT_TRUE(closed) << "SEL ticker thread did not close 03:59";
    EXPECT_EQ(239u, ticker._last_emit_pos.load());
    EXPECT_EQ(35900000u, ticker._time);

    ticker.stop();
}
