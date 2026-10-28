/**
 * r160_hft_ticker_test.cpp — lane R160 / wave24
 *
 * 目标：src/WtCore/WtHftTicker.cpp 的 run() 线程体。
 *
 * 基线里 :172-248 全部未覆盖，因为已有用例要么不调 run()，要么运行时刻不在
 * 交易时段，线程只走 else 的 10 秒 sleep。本用例把会话设成 00:00-03:59，
 * 把 _time/_cur_pos/_last_emit_pos 预置到「03:58 行情已到、待闭合 03:59」，
 * run() 的线程首轮就会走闭合 + on_session_end + set_date_time 分支；
 * 闭合后 _time=0359，下一轮 isInTradingTime(0) 仍为真，stop() 最多等 10ms。
 *
 * _thrd/_time/_cur_pos 等是 private；沿用本仓 r105 的 `#define private public`
 * 手法，仅包住 WtHftTicker.h 并立即 #undef，不改生产代码。
 */
#include "r160_common.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

#include <Includes/WTSMarcos.h>
#include <Share/StdUtils.hpp>

#define private public
#include <WtCore/WtHftTicker.h>
#undef private

#include "r73_fixture.h"

namespace {

class R160HftEngine : public r73::HftEngine
{
public:
    void on_session_end() override { ++sessions; }

    uint32_t sessions = 0;
};

class R160HftTickerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        wtp::WTSSessionInfo* sess = _bd.regisSession("TRADING", 0, 359);
        ASSERT_NE(sess, nullptr);
        wtp::WTSCommodityInfo* ag = r73::makeComm("SHFE", "ag", "TRADING", wtp::TM_Both);
        ag->setSessionInfo(sess);
        _bd.regis(ag, r73::makeCt("SHFE", "ag", "ag2512"));

        _engine.attach(&_bd, &_hot, NULL, 20250110, 931);
    }

    r73::BaseDataMgr _bd;
    r73::HotMgr _hot;
    R160HftEngine _engine;
};

} // namespace

// run() 线程首轮闭合 03:59 并触发 on_session_end；第二次 run() 直接早退；
// stop() 的 join 分支
// src/WtCore/WtHftTicker.cpp:157-252（重点 :160、:172-217、:254-258）
TEST_F(R160HftTickerTest, R160RunThreadClosesLastMinuteAndStops)
{
    wtp::WtHftRtTicker ticker(&_engine);
    ticker.init(NULL, "TRADING");
    ASSERT_NE(ticker._s_info, nullptr);

    // 会话 00:00-03:59：03:58 的行情已处理，下一轮应闭合 03:59
    ticker._date = 20250110;
    ticker._time = 35800000;     // 03:58:00.000
    ticker._cur_pos = 239;       // 03:59 在会话内的分钟偏移
    ticker._last_emit_pos = 0;
    ticker._next_check_time = 0;

    ticker.run();

    for (int i = 0; i < 200 && _engine.get_min_time() != 359; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    EXPECT_EQ(_engine.get_min_time(), 359u) << "线程未执行分钟闭合";
    EXPECT_EQ(ticker._time, 359u);
    EXPECT_EQ(ticker._last_emit_pos.load(), 239u);
    EXPECT_EQ(_engine.sessions, 1u) << "03:59 收盘必须触发 on_session_end";

    // _thrd 已非空 -> run() 的首行早退
    ticker.run();

    ticker.stop();
}
