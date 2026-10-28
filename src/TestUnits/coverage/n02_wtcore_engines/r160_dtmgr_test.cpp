/**
 * r160_dtmgr_test.cpp — lane R160 / wave24
 *
 * 目标：src/WtCore/WtDtMgr.cpp 的剩余行。WtDtMgr 的 _reader/_bars_cache/
 * _ticks_adjusted 都是 private 且只有 initStore 能装配，故本 TU 用一次
 * `#define private public` 只包住 WtDtMgr.h，随后立即 #undef（同 r105 的手法）。
 * 不修改任何生产头文件。
 *
 * 覆盖：
 *   - get_tick_slice 后复权缓存：:334-340（etime==0 用引擎时钟）、:358（比较器）、
 *     :367-370（定位点晚于目标时回退一根）
 *   - handle_push_quote 的 _ticks_adjusted 分支：:229-239（含 :233/:236 早退）
 *   - on_bar 非基础周期缓存：:185-191、:196-208（Minute5/Hour/Half 三种 period）
 *   - get_kline_slice 的三处异常捕获：:438-447、:468-475，并经由日志参数覆盖
 *     format_period 的全部 case（:403-419）
 */
#include "r160_common.h"

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <Includes/FasterDefs.h>
#include <Includes/IDataManager.h>
#include <Includes/IDataReader.h>
#include <Includes/WTSCollection.hpp>
#include <Includes/WTSDataDef.hpp>
#include <Share/CodeHelper.hpp>
#include <Share/TimeUtils.hpp>

#define private public
#include <WtCore/WtDtMgr.h>
#undef private

#include "r73_fixture.h"

namespace {

class R160FakeReader : public wtp::IDataReader
{
public:
    void onMinuteEnd(uint32_t, uint32_t, uint32_t = 0) override {}

    wtp::WTSTickSlice* readTickSlice(const char* stdCode, uint32_t count, uint64_t) override
    {
        if (throw_mode == 1)
            throw std::runtime_error("r160 tick failure");
        if (throw_mode == 2)
            throw 42;

        const uint32_t n = count < _ticks.size() ? count : static_cast<uint32_t>(_ticks.size());
        return wtp::WTSTickSlice::create(stdCode, _ticks.data(), n);
    }

    wtp::WTSKlineSlice* readKlineSlice(const char*, wtp::WTSKlinePeriod, uint32_t, uint64_t) override
    {
        if (throw_mode == 1)
            throw std::runtime_error("r160 kline failure");
        if (throw_mode == 2)
            throw 42;
        return NULL;
    }

    std::vector<wtp::WTSTickStruct> _ticks;
    int throw_mode = 0;
};

class R160DtMgrTest : public ::testing::Test
{
protected:
    R160DtMgrTest() { _tmp = NULL; }

    void SetUp() override
    {
        _tmp = new r160::TempDir("dtmgr");
        _bd.regisSession("TRADING");
        wtp::WTSSessionInfo* sess = _bd.session("TRADING");
        ASSERT_NE(sess, nullptr);
        sess->addTradingSection(900, 1500);

        wtp::WTSCommodityInfo* ag = r73::makeComm("SHFE", "ag", "TRADING", wtp::TM_Both);
        ag->setSessionInfo(sess);
        _bd.regis(ag, r73::makeCt("SHFE", "ag", "ag2512"));
        _engine.attach(&_bd, &_hot, NULL);
    }

    void TearDown() override
    {
        delete _tmp;
        _tmp = NULL;
    }

    static wtp::WTSBarStruct makeBar(uint32_t hhmm)
    {
        wtp::WTSBarStruct bar;
        std::memset(&bar, 0, sizeof(bar));
        bar.date = 20250110;
        bar.time = TimeUtils::timeToMinBar(20250110, hhmm);
        bar.open = 3500.0;
        bar.high = 3510.0;
        bar.low = 3490.0;
        bar.close = 3505.0;
        return bar;
    }

    r160::TempDir* _tmp = NULL;
    r73::BaseDataMgr _bd;
    r73::HotMgr _hot;
    r73::HftEngine _engine;
};

} // namespace

// 后复权 tick 缓存 + 引擎时钟 + 定位回退
// src/WtCore/WtDtMgr.cpp:297-375（重点 :334-340、:358、:367-370）、:219-253
TEST_F(R160DtMgrTest, R160HfqTickCacheUsesEngineClockAndStepsBackOneTick)
{
    wtp::WtDtMgr mgr;
    R160FakeReader reader;
    reader._ticks.resize(3);
    for (uint32_t i = 0; i < 3; i++)
    {
        std::memset(&reader._ticks[i], 0, sizeof(wtp::WTSTickStruct));
        reader._ticks[i].action_date = 20250110;
        reader._ticks[i].trading_date = 20250110;
        reader._ticks[i].action_time = 93000000 + i * 100000;
        reader._ticks[i].price = 3500.0 + i;
        reader._ticks[i].volume = 1.0;
        std::strcpy(reader._ticks[i].exchg, "SHFE");
        std::strcpy(reader._ticks[i].code, "SHFE.ag.2512");
    }

    mgr._reader = &reader;
    mgr._engine = &_engine;

    const std::string hfqCode = std::string("SHFE.ag.2512") + SUFFIX_HFQ;

    // etime==0：用引擎的 20250110 09:31 作为终点，取最近 2 根
    wtp::WTSTickSlice* first = mgr.get_tick_slice(hfqCode.c_str(), 2);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->size(), 2u);
    first->release();

    // 终点落在 09:30:30（两根 tick 之间）：lower_bound 定位到 09:31 后必须回退一根
    wtp::WTSTickSlice* second = mgr.get_tick_slice(hfqCode.c_str(), 2, 20250110093030000ull);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->size(), 1u);
    second->release();

    // 复权缓存已建立，handle_push_quote 走 tData->appendTick 分支
    wtp::WTSTickData* tick = wtp::WTSTickData::create(reader._ticks[2]);
    ASSERT_NE(tick, nullptr);
    tick->setContractInfo(_bd.getContract("ag2512", "SHFE"));
    mgr.handle_push_quote("SHFE.ag.2512", tick);
    tick->release();

    wtp::WTSHisTickData* his =
        (wtp::WTSHisTickData*)mgr._ticks_adjusted->get("SHFE.ag.2512");
    ASSERT_NE(his, nullptr);
    EXPECT_EQ(his->getDataRef().size(), 4u) << "3 根历史 + 1 根推送";

    wtp::WTSTickData* last = mgr.grab_last_tick("SHFE.ag.2512");
    ASSERT_NE(last, nullptr);
    EXPECT_DOUBLE_EQ(last->price(), 3502.0);
    last->release();
}

// on_bar 对非基础周期缓存的三种 period 分支 + 前缀不匹配的 continue
// src/WtCore/WtDtMgr.cpp:180-217（重点 :189、:196、:202-208）
TEST_F(R160DtMgrTest, R160OnBarClosesCachedMinute5HourAndHalfBars)
{
    wtp::WtDtMgr mgr;
    mgr._engine = &_engine;
    mgr._align_by_section = false;
    mgr._bars_cache = wtp::WtDtMgr::DataCacheMap::create();
    ASSERT_NE(mgr._bars_cache, nullptr);

    const wtp::WTSBarStruct bar930 = makeBar(930);
    auto addKline = [&mgr, bar930](const char* key, wtp::WTSKlinePeriod period, uint32_t times) {
        wtp::WTSKlineData* kline = wtp::WTSKlineData::create("SHFE.ag.2512", 1);
        kline->setPeriod(period, times);
        kline->setClosed(true);
        kline->getDataRef()[0] = bar930;
        mgr._bars_cache->add(key, kline, false);
    };

    addKline("SHFE.ag.2512-2-5", wtp::KP_Minute5, 5);
    addKline("SHFE.ag.2512-4-2", wtp::KP_Hour, 2);
    addKline("SHFE.ag.2512-5-2", wtp::KP_Half, 2);
    // 另一个合约的缓存键：memcmp 前缀不符时必须 continue，不能误更新
    addKline("SHFE.au.2512-2-5", wtp::KP_Minute5, 5);

    // 只有 min5 基础周期被订阅；hour/half 只走缓存更新、不发基础通知
    mgr._subed_basic_bars.insert("SHFE.ag.2512-2");

    wtp::WTSBarStruct bar = makeBar(935);
    mgr.on_bar("SHFE.ag.2512", wtp::KP_Minute5, &bar);
    mgr.on_all_bar_updated(935);
    EXPECT_EQ(_engine.bar_count, 1u);

    bar = makeBar(1000);
    mgr.on_bar("SHFE.ag.2512", wtp::KP_Hour, &bar);
    mgr.on_all_bar_updated(1000);
    EXPECT_EQ(_engine.bar_count, 2u);

    bar = makeBar(1000);
    mgr.on_bar("SHFE.ag.2512", wtp::KP_Half, &bar);
    mgr.on_all_bar_updated(1000);
    EXPECT_EQ(_engine.bar_count, 3u);
}

// 读取异常：std::exception 与 unknown 两个 catch；format_period 的 6 个 case
// src/WtCore/WtDtMgr.cpp:434-447、:464-475、:402-419
TEST_F(R160DtMgrTest, R160KlineReaderExceptionsAreCaughtAndPeriodsFormatted)
{
    wtp::WtDtMgr mgr;
    R160FakeReader reader;
    mgr._reader = &reader;
    mgr._engine = &_engine;

    const wtp::WTSKlinePeriod periods[] = {
        wtp::KP_DAY, wtp::KP_Minute1, wtp::KP_Minute5,
        wtp::KP_Half, wtp::KP_Hour, static_cast<wtp::WTSKlinePeriod>(99)
    };

    reader.throw_mode = 1;   // std::runtime_error -> catch (const std::exception&)
    for (wtp::WTSKlinePeriod period : periods)
    {
        EXPECT_EQ(mgr.get_kline_slice("SHFE.ag.2512", period, 1, 2), nullptr);
    }

    reader.throw_mode = 2;   // int -> catch (...)
    EXPECT_EQ(mgr.get_kline_slice("SHFE.ag.2512", wtp::KP_Minute1, 1, 2), nullptr);

    // 强制缓存 + times>1 的另一条读取链同样有两个 catch
    mgr._force_cache = true;
    reader.throw_mode = 1;
    EXPECT_EQ(mgr.get_kline_slice("SHFE.ag.2512", wtp::KP_Minute1, 5, 2), nullptr);
    reader.throw_mode = 2;
    EXPECT_EQ(mgr.get_kline_slice("SHFE.ag.2512", wtp::KP_Minute1, 5, 2), nullptr);
}
