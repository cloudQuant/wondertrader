/**
 * n02_wtcore_engines/ticker_slicing_test.cpp  (lane N02 / WtCore 后半)
 *
 * 覆盖目标:
 *   src/WtCore/WtCtaTicker.h/.cpp : WtCtaRtTicker ctor / on_tick -> trigger_price / stop / dtor
 *   src/WtCore/WtSelTicker.cpp    : WtSelRtTicker ctor / dtor / on_tick / trigger_price / stop
 *   src/WtCore/WtHftTicker.cpp    : WtHftRtTicker ctor / dtor / on_tick / trigger_price / stop
 *
 * 为什么只测这么多:
 *   bar 切片推进(on_tick 里的分钟位 _cur_pos / _last_emit_pos / run() 定时线程)完全依赖
 *   WTSSessionInfo 与 Wt*Engine 实例:
 *     - init(store, sessionID) 第一步就是 _engine->get_session_info(sessionID), 传 NULL 引擎即空指针解引用;
 *     - on_tick 的切片分支要求 _thrd != NULL, 而 _thrd 只能由 run() 创建, run() 又会起后台线程并
 *       依赖 _engine->get_basedata_mgr() / 真实交易日推算 -> 属"需要真实服务端"的分支, 本 lane 跳过。
 *   BUG-N02-002 仅确认 CTA ticker 的公开 session 查询在构造后、init 前读取未初始化
 *   的 _s_info；该路径由独立启用用例覆盖。SEL/HFT 的预初始化生命周期不在本缺陷范围。
 */
#include <gtest/gtest.h>

#include "WtCore/WtCtaTicker.h"
#include "WtCore/WtSelTicker.h"
#include "WtCore/WtHftTicker.h"

#include <memory>
#include <string>

USING_NS_WTP;

namespace {

//////////////////////////////////////////////////////////////////
// WtCtaRtTicker
//////////////////////////////////////////////////////////////////

TEST(N02CtaTickerTest, ConstructsWithoutAnyEngine)
{
    // 头文件内联构造: _date=0, _time=UINT_MAX, _cur_pos=0, _last_emit_pos=0, _stopped=false
    WtCtaRtTicker ticker(NULL);
    // 未 run() -> _thrd 为空 -> on_tick 直接落到 trigger_price
    // trigger_price 首行判 _engine, 引擎为空时不触碰 tick 指针
    ticker.on_tick(NULL);
    ticker.stop();
}

TEST(N02CtaTickerTest, StopBeforeRunIsIdempotent)
{
    WtCtaRtTicker ticker(NULL);
    ticker.stop();
    ticker.stop();
    ticker.on_tick(NULL);
}

TEST(N02CtaTickerTest, PreInitQueriesUseFallback)
{
    WtCtaRtTicker ticker(NULL);

    EXPECT_FALSE(ticker.is_in_trading());
    EXPECT_EQ(93000u, ticker.time_to_mins(93000));
}

//////////////////////////////////////////////////////////////////
// WtSelRtTicker
//////////////////////////////////////////////////////////////////

TEST(N02SelTickerTest, ConstructsWithoutAnyEngine)
{
    WtSelRtTicker ticker(NULL);
    ticker.on_tick(NULL);
    ticker.on_tick(NULL, 1);       // hotFlag 分支同样被 _engine 判空短路
    ticker.stop();
}

TEST(N02SelTickerTest, StopBeforeRunIsIdempotent)
{
    WtSelRtTicker ticker(NULL);
    ticker.stop();
    ticker.stop();
    ticker.on_tick(NULL, 2);
}

//////////////////////////////////////////////////////////////////
// WtHftRtTicker
//////////////////////////////////////////////////////////////////

TEST(N02HftTickerTest, ConstructsWithoutAnyEngine)
{
    WtHftRtTicker ticker(NULL);
    ticker.on_tick(NULL);
    ticker.stop();
}

TEST(N02HftTickerTest, StopBeforeRunIsIdempotent)
{
    WtHftRtTicker ticker(NULL);
    ticker.stop();
    ticker.on_tick(NULL);
    ticker.stop();
}

//////////////////////////////////////////////////////////////////
// SEL/HFT 预初始化生命周期与 BUG-N02-002 无关，本轮不作结论。
//////////////////////////////////////////////////////////////////

TEST(N02TickerDisabled, DISABLED_UnverifiedSelHftPreInitPaths)
{
    {
        WtSelRtTicker sel(NULL);
        sel.on_tick(NULL);
        sel.stop();
    }
    {
        WtHftRtTicker hft(NULL);
        hft.on_tick(NULL);
        hft.stop();
    }
}

}  // namespace
