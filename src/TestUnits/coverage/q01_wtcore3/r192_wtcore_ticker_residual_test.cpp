/**
 * r192_wtcore_ticker_residual_test.cpp — lane R192 / wave27
 *
 * 目标：WtCore 剩余公开面中仍可安全触达的自动闭合分支
 *   - WtCtaTicker.cpp:213-219（跨日 0 点）、:221-234（收盘分钟 + session_end）、
 *     :245-268（错过最后一分钟的强制收盘）
 *   - WtSelTicker.cpp:199-204（跨日 0 点）、:213-217（收盘分钟 + session_end）
 *   - WtHftTicker.cpp:195-200（跨日 0 点）、:219-241（强制收盘）
 * 以及一个 WtSelEngine 的公开面缺陷复现（DISABLED）。
 *
 * 方法：ticker 只走公开 init/run/on_tick/stop。测试 stub 的 IDataReader
 * onMinuteEnd 在计数后短暂 sleep（100ms），主线程看到计数后立即 stop()：
 * 闭合分支已经在执行，线程随后检查 _stopped 退出，不会被 10 秒休市 sleep 拖住。
 * 不访问任何 private 成员，不启长线程，临时目录只用系统临时目录。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include <unistd.h>

#include "q01_sel_env.h"

#include <Includes/IDataReader.h>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <Share/TimeUtils.hpp>
#include <WTSUtils/WTSCfgLoader.h>
#include <WtCore/WtCtaEngine.h>
#include <WtCore/WtCtaTicker.h>
#include <WtCore/WtDtMgr.h>
#include <WtCore/WtHftEngine.h>
#include <WtCore/WtHftTicker.h>
#include <WtCore/WtSelEngine.h>
#include <WtCore/WtSelTicker.h>
#include <WtCore/WtHelper.h>

using namespace wtp;

namespace {

namespace fs = std::filesystem;

/** 系统临时目录下的独立生成目录，析构删除。 */
class R192TempEnv
{
public:
    explicit R192TempEnv(const char* tag)
    {
        static std::string root;
        if (root.empty())
        {
            root = (fs::temp_directory_path()
                / ("r192_wtcore_" + std::string(tag) + "_"
                    + std::to_string(static_cast<long long>(::getpid()))))
                       .string()
                + "/";
        }
        std::error_code ec;
        fs::remove_all(fs::path(root), ec);
        fs::create_directories(fs::path(root), ec);
        WtHelper::setGenerateDir(root.c_str());
        _dir = root;
    }

    ~R192TempEnv()
    {
        std::error_code ec;
        fs::remove_all(fs::path(_dir), ec);
    }

private:
    std::string _dir;
};

/**
 * 记录 onMinuteEnd 的 IDataReader 替身。计数后睡 100ms，让主线程在
 * 闭合分支内部看到计数并 stop()，从而避开 ticker 的 10 秒休市等待。
 */
class R192Reader : public IDataReader
{
public:
    void onMinuteEnd(uint32_t d, uint32_t t, uint32_t e = 0) override
    {
        last_date.store(d);
        last_time.store(t);
        last_end.store(e);
        count.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
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

/** 24 小时或指定区间的交易时段 + ag2512 合约。 */
struct R192Market
{
    R192Market(uint32_t start, uint32_t end)
    {
        bd.regisSessionWithTime("TRADING", start, end);
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

/** 只对 on_session_end 计数：避免 WtEngine 结算写文件，同时验证 ticker 的调用。 */
class R192CtaEngine : public WtCtaEngine
{
public:
    void on_session_end() override { ++session_ends; }
    uint32_t session_ends = 0;
};

class R192HftEngine : public WtHftEngine
{
public:
    void on_session_end() override { ++session_ends; }
    uint32_t session_ends = 0;
};

/** 次日行情日期：晚于 init() 取到的本地日期，避免 on_tick 早退。 */
inline uint32_t r192_next_date()
{
    return TimeUtils::getNextDate(TimeUtils::getCurDate(), 1);
}

inline WTSTickData* r192_make_tick(R192Market& mkt, uint32_t date, uint32_t atime,
    double price = 3500.0)
{
    WTSTickData* tick = q01sel::makeTick("SHFE", "ag2512", date, date, atime, price);
    tick->setContractInfo(mkt.bd.getContract("ag2512", "SHFE"));
    return tick;
}

inline bool r192_wait_count(const R192Reader& reader, uint32_t n, int loops = 400)
{
    for (int i = 0; i < loops && reader.count.load() < n; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    return reader.count.load() >= n;
}

//////////////////////////////////////////////////////////////
// CTA：24h 时段，23:59:60 的 tick 令 _cur_pos=1440，
// run() 线程自动闭合 00:00 并跨日。
// src/WtCore/WtCtaTicker.cpp:213-219
//////////////////////////////////////////////////////////////
TEST(R192CtaTicker, RunRollsDateAtMidnight)
{
    R192TempEnv tmp("cta_roll");
    R192Market mkt(0, 0);
    R192Reader reader;

    R192CtaEngine engine;
    WTSVariant* cfg = mkt.cfg();
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &mkt.bd, &mkt.dm, &mkt.hot, NULL);
    cfg->release();
    engine.set_date_time(20250110, 930, 0);
    engine.set_trading_date(20250110);

    WtCtaRtTicker ticker(&engine);
    ticker.init(&reader, "TRADING");
    ticker.run();

    const uint32_t tickDate = r192_next_date();
    WTSTickData* tick = r192_make_tick(mkt, tickDate, 235960000);
    ticker.on_tick(tick);
    tick->release();

    const bool closed = r192_wait_count(reader, 1);
    ticker.stop();

    EXPECT_TRUE(closed) << "CTA ticker did not close the 00:00 minute";
    EXPECT_EQ(1u, reader.count.load());
    EXPECT_EQ(tickDate, reader.last_date.load());
    EXPECT_EQ(0u, reader.last_time.load());
    EXPECT_EQ(TimeUtils::getNextDate(tickDate, 1), engine.get_date());
    EXPECT_EQ(0u, engine.get_min_time());
}

//////////////////////////////////////////////////////////////
// CTA：0:00-23:59 时段，23:58:60 令 _cur_pos=1439，
// run() 自动闭合 23:59 并触发 on_session_end。
// src/WtCore/WtCtaTicker.cpp:221-238
//////////////////////////////////////////////////////////////
TEST(R192CtaTicker, RunEndsSessionAtLastMinute)
{
    if (TimeUtils::getCurMin() / 100 >= 2359)
        GTEST_SKIP() << "wall clock is in the 23:59 minute";

    R192TempEnv tmp("cta_end");
    R192Market mkt(0, 2359);
    R192Reader reader;

    R192CtaEngine engine;
    WTSVariant* cfg = mkt.cfg();
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &mkt.bd, &mkt.dm, &mkt.hot, NULL);
    cfg->release();
    engine.set_date_time(20250110, 930, 0);
    engine.set_trading_date(20250110);

    WtCtaRtTicker ticker(&engine);
    ticker.init(&reader, "TRADING");
    ticker.run();

    const uint32_t tickDate = r192_next_date();
    WTSTickData* tick = r192_make_tick(mkt, tickDate, 235860000);
    ticker.on_tick(tick);
    tick->release();

    const bool closed = r192_wait_count(reader, 1);
    ticker.stop();

    EXPECT_TRUE(closed) << "CTA ticker did not close 23:59";
    EXPECT_EQ(2359u, reader.last_time.load());
    EXPECT_EQ(tickDate, reader.last_end.load());
    EXPECT_EQ(2359u, engine.get_min_time());
    EXPECT_EQ(1u, engine.session_ends);
}

//////////////////////////////////////////////////////////////
// CTA：先由 23:57 行情闭合 23:58，再由 23:59 行情把状态留在盘外，
// run() 走「错过最后一分钟」的强制收盘分支。
// src/WtCore/WtCtaTicker.cpp:245-268
//////////////////////////////////////////////////////////////
TEST(R192CtaTicker, RunForcedCloseAfterMissedLastMinute)
{
    if (TimeUtils::getCurMin() / 100 >= 2359)
        GTEST_SKIP() << "wall clock is in the 23:59 minute";

    R192TempEnv tmp("cta_forced");
    R192Market mkt(0, 2359);
    R192Reader reader;

    R192CtaEngine engine;
    WTSVariant* cfg = mkt.cfg();
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &mkt.bd, &mkt.dm, &mkt.hot, NULL);
    cfg->release();
    // run() 读一次 get_min_time() 作为 offTime，需 >= close(2359)
    engine.set_date_time(20250110, 2359, 0);
    engine.set_trading_date(20250110);

    WtCtaRtTicker ticker(&engine);
    ticker.init(&reader, "TRADING");
    ticker.run();

    const uint32_t tickDate = r192_next_date();
    WTSTickData* tickA = r192_make_tick(mkt, tickDate, 235760000);
    ticker.on_tick(tickA);
    tickA->release();

    const bool first = r192_wait_count(reader, 1);
    WTSTickData* tickB = r192_make_tick(mkt, tickDate, 235900000);
    ticker.on_tick(tickB);
    tickB->release();

    const bool forced = r192_wait_count(reader, 2);
    ticker.stop();

    EXPECT_TRUE(first) << "CTA ticker did not close 23:58";
    EXPECT_TRUE(forced) << "CTA ticker did not force-close the trading day";
    EXPECT_EQ(2u, reader.count.load());
    EXPECT_EQ(2359u, reader.last_time.load());
    EXPECT_EQ(tickDate, reader.last_end.load());
    EXPECT_EQ(1u, engine.session_ends);
}

//////////////////////////////////////////////////////////////
// SEL：24h 时段，23:59:60 令 _cur_pos=1440，run() 自动闭合 00:00 并跨日。
// src/WtCore/WtSelTicker.cpp:199-204、:207-221
//////////////////////////////////////////////////////////////
TEST(R192SelTicker, RunRollsDateAtMidnight)
{
    R192TempEnv tmp("sel_roll");
    R192Market mkt(0, 0);
    R192Reader reader;

    // BUG-R42-001：_tm_ticker 构造未初始化，析构 release 会读脏指针，堆分配且不回收
    WtSelEngine& engine = *new WtSelEngine();
    WTSVariant* cfg = mkt.cfg();
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &mkt.bd, &mkt.dm, &mkt.hot, NULL);
    cfg->release();
    engine.set_date_time(20250110, 930, 0);
    engine.set_trading_date(20250110);

    WtSelRtTicker ticker(&engine);
    ticker.init(&reader, "TRADING");
    ticker.run();

    const uint32_t tickDate = r192_next_date();
    WTSTickData* tick = r192_make_tick(mkt, tickDate, 235960000);
    ticker.on_tick(tick);
    tick->release();

    const bool closed = r192_wait_count(reader, 1);
    ticker.stop();

    EXPECT_TRUE(closed) << "SEL ticker did not close the 00:00 minute";
    EXPECT_EQ(1u, reader.count.load());
    EXPECT_EQ(0u, reader.last_time.load());
    EXPECT_EQ(TimeUtils::getNextDate(tickDate, 1), engine.get_date());
    EXPECT_EQ(0u, engine.get_min_time());
}

//////////////////////////////////////////////////////////////
// SEL：0:00-23:59 时段，23:58:60 令 _cur_pos=1439，
// run() 自动闭合 23:59 并触发 on_session_end。
// src/WtCore/WtSelTicker.cpp:213-217
//////////////////////////////////////////////////////////////
TEST(R192SelTicker, RunEndsSessionAtLastMinute)
{
    R192TempEnv tmp("sel_end");
    R192Market mkt(0, 2359);
    R192Reader reader;

    WtSelEngine& engine = *new WtSelEngine();  // BUG-R42-001
    WTSVariant* cfg = mkt.cfg();
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &mkt.bd, &mkt.dm, &mkt.hot, NULL);
    cfg->release();
    engine.set_date_time(20250110, 930, 0);
    engine.set_trading_date(20250110);

    WtSelRtTicker ticker(&engine);
    ticker.init(&reader, "TRADING");
    ticker.run();

    const uint32_t tickDate = r192_next_date();
    WTSTickData* tick = r192_make_tick(mkt, tickDate, 235860000);
    ticker.on_tick(tick);
    tick->release();

    const bool closed = r192_wait_count(reader, 1);
    ticker.stop();

    EXPECT_TRUE(closed) << "SEL ticker did not close 23:59";
    EXPECT_EQ(2359u, reader.last_time.load());
    EXPECT_EQ(2359u, engine.get_min_time());
}

//////////////////////////////////////////////////////////////
// HFT：24h 时段，23:59:60 令 _cur_pos=1440，run() 自动闭合 00:00 并跨日。
// src/WtCore/WtHftTicker.cpp:194-200
//////////////////////////////////////////////////////////////
TEST(R192HftTicker, RunRollsDateAtMidnight)
{
    R192TempEnv tmp("hft_roll");
    R192Market mkt(0, 0);
    R192Reader reader;

    R192HftEngine engine;
    WTSVariant* cfg = mkt.cfg();
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &mkt.bd, &mkt.dm, &mkt.hot, NULL);
    cfg->release();
    engine.set_date_time(20250110, 930, 0);
    engine.set_trading_date(20250110);

    WtHftRtTicker ticker(&engine);
    ticker.init(&reader, "TRADING");
    ticker.run();

    const uint32_t tickDate = r192_next_date();
    WTSTickData* tick = r192_make_tick(mkt, tickDate, 235960000);
    ticker.on_tick(tick);
    tick->release();

    const bool closed = r192_wait_count(reader, 1);
    ticker.stop();

    EXPECT_TRUE(closed) << "HFT ticker did not close the 00:00 minute";
    EXPECT_EQ(1u, reader.count.load());
    EXPECT_EQ(0u, reader.last_time.load());
    EXPECT_EQ(TimeUtils::getNextDate(tickDate, 1), engine.get_date());
    EXPECT_EQ(0u, engine.get_min_time());
}

//////////////////////////////////////////////////////////////
// HFT：先由 23:57 行情闭合 23:58，再由 23:59 行情把 _time 留在盘外，
// run() 走强制收盘分支（:219-241）。
// src/WtCore/WtHftTicker.cpp:219-241
//////////////////////////////////////////////////////////////
TEST(R192HftTicker, RunForcedCloseAfterMissedLastMinute)
{
    if (TimeUtils::getCurMin() / 100 >= 2359)
        GTEST_SKIP() << "wall clock is in the 23:59 minute";

    R192TempEnv tmp("hft_forced");
    R192Market mkt(0, 2359);
    R192Reader reader;

    R192HftEngine engine;
    WTSVariant* cfg = mkt.cfg();
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &mkt.bd, &mkt.dm, &mkt.hot, NULL);
    cfg->release();
    engine.set_date_time(20250110, 2359, 0);
    engine.set_trading_date(20250110);

    WtHftRtTicker ticker(&engine);
    ticker.init(&reader, "TRADING");
    ticker.run();

    const uint32_t tickDate = r192_next_date();
    WTSTickData* tickA = r192_make_tick(mkt, tickDate, 235760000);
    ticker.on_tick(tickA);
    tickA->release();

    const bool first = r192_wait_count(reader, 1);
    WTSTickData* tickB = r192_make_tick(mkt, tickDate, 235900000);
    ticker.on_tick(tickB);
    tickB->release();

    const bool forced = r192_wait_count(reader, 2);
    ticker.stop();

    EXPECT_TRUE(first) << "HFT ticker did not close 23:58";
    EXPECT_TRUE(forced) << "HFT ticker did not force-close the trading day";
    EXPECT_EQ(2u, reader.count.load());
    EXPECT_EQ(2359u, reader.last_time.load());
    EXPECT_EQ(tickDate, reader.last_end.load());
    EXPECT_EQ(1u, engine.session_ends);
}

//////////////////////////////////////////////////////////////////
// 公开面缺陷复现（BUG-R192-001）：
// WtEngine::get_adjusting_flag() 丢弃 _data_mgr->get_adjusting_flag() 的返回值，
// 恒返回 0，后复权 tick 的量/额/持仓复权分支（WtSelEngine.cpp:189-206）不可达。
// 期望：flag=7 时 total_volume/volume/open_interest 除以 factor，
//       total_turnover/turn_over 乘以 factor。
// 当前实际：量额持仓都不变，仅 OHLC 乘以 factor，故本用例 DISABLED。
//////////////////////////////////////////////////////////////////
class R192FlagDtMgr : public WtDtMgr
{
public:
    uint32_t get_adjusting_flag() override { return 7; }
};

class R192FactorHot : public q01sel::HotMgr
{
public:
    const char* getRuleTag(const char* stdCode) override
    {
        return (stdCode != NULL && strcmp(stdCode, "SHFE.ag.2512") == 0) ? "HOT" : "";
    }

    double getRuleFactor(const char* /*tag*/, const char* /*fullPid*/, uint32_t = 0) override
    {
        return 2.0;
    }
};

class R192TickCtx : public q01sel::SelCtx
{
public:
    R192TickCtx(WtSelEngine* engine, const char* name) : q01sel::SelCtx(engine, name) {}

    void on_tick_updated(const char* stdCode, WTSTickData* tick) override
    {
        q01sel::SelCtx::on_tick_updated(stdCode, tick);
        last_struct = tick->getTickStruct();
    }

    WTSTickStruct last_struct{};
};

class R192ProbeEngine : public WtSelEngine
{
public:
    void subscribe(const char* stdCode, uint32_t sid, uint32_t opt)
    {
        _tick_sub_map[stdCode][sid] = std::make_pair(sid, opt);
    }

    void mark_ready() { _ready = true; }
};

TEST(R192SelEngine, DISABLED_HfqFlagBitsScaleVolumeTurnoverAndInterest)
{
    R192TempEnv tmp("sel_flag");
    R192Market mkt(0, 2359);
    R192FlagDtMgr dm;
    R192FactorHot hot;

    R192ProbeEngine& engine = *new R192ProbeEngine();  // BUG-R42-001
    WTSVariant* cfg = mkt.cfg();
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &mkt.bd, &dm, &hot, NULL);
    cfg->release();
    engine.set_date_time(20250110, 931, 0);
    engine.set_trading_date(20250110);

    SelContextPtr ctx(new R192TickCtx(&engine, "r192flag"));
    R192TickCtx* probe = static_cast<R192TickCtx*>(ctx.get());
    engine.addContext(ctx, 0, 0, TPT_None);
    engine.subscribe("SHFE.ag.2512", ctx->id(), 2);
    engine.mark_ready();

    WTSTickData* tick = q01sel::makeTick("SHFE", "ag2512", 20250110, 20250110,
        93100000, 3500.0);
    WTSTickStruct& st = tick->getTickStruct();
    st.open = 3400.0;
    st.high = 3600.0;
    st.low = 3300.0;
    st.volume = 10;
    st.total_volume = 100;
    st.turn_over = 35000.0;
    st.total_turnover = 350000.0;
    st.open_interest = 500.0;
    st.diff_interest = 20.0;
    st.pre_interest = 480.0;
    tick->setContractInfo(mkt.bd.getContract("ag2512", "SHFE"));

    engine.on_tick("SHFE.ag.2512", tick);
    tick->release();

    EXPECT_EQ(1u, probe->tick_updates);
    // factor=2.0，flag=7：量字段 /2、额字段 *2、持仓字段 /2
    EXPECT_DOUBLE_EQ(50.0, probe->last_struct.total_volume);
    EXPECT_DOUBLE_EQ(5.0, probe->last_struct.volume);
    EXPECT_DOUBLE_EQ(700000.0, probe->last_struct.total_turnover);
    EXPECT_DOUBLE_EQ(70000.0, probe->last_struct.turn_over);
    EXPECT_DOUBLE_EQ(250.0, probe->last_struct.open_interest);
    EXPECT_DOUBLE_EQ(10.0, probe->last_struct.diff_interest);
    EXPECT_DOUBLE_EQ(240.0, probe->last_struct.pre_interest);
}

}  // namespace
