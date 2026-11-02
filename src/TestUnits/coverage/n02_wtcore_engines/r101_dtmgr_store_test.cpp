/**
 * r101_dtmgr_store_test.cpp — lane R101 / wave18
 *
 * 目标：src/WtCore/WtDtMgr.cpp 的真实 reader 命中路径。
 *
 * 做法：测试侧用公开的 WtLMDB（WTSUtils）按 WtDataStorageAD 的落盘布局
 * （ticks/{exchg}/{code}、min1/{exchg}）预置 LMDB，再由 WtDtMgr::init 通过
 * dlopen libWtDataStorageAD 装载 reader 读取，不启动线程、不连网。
 *
 * 覆盖行段：
 *   - handle_push_quote 的 _ticks_adjusted 非空追加：:229-239
 *   - get_tick_slice 的普通直读与 HFQ 缓存生成/命中：:287-376
 *   - get_kline_slice 的重采样、缓存写入、闭合修正：:451-541
 *   - on_bar 的 bars_cache 命中（times==1 与 times!=1）与 on_all_bar_updated：:180-217
 */
#include <TestUnits/coverage/n01_wtcore_tradepaths/r71_wtcore_env.h>

#include <WtDataStorageAD/LMDBKeys.h>
#include <WTSUtils/WtLMDB.hpp>
#include <WTSUtils/WTSCfgLoader.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

const char* const kCode = "SHFE.ag.2512";
const char* const kRawCode = "ag2512";

std::string store_root()
{
    std::string dir = r71::temp_root() + "r101_store/";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

uint32_t bar_time(uint32_t minute)
{
    // WtDataStorageAD 的分钟线时间：(date - 19900000) * 10000 + HHMM
    return (20250110u - 19900000u) * 10000u + minute;
}

void write_ticks(const std::string& root)
{
    const std::string dir = root + "ticks/SHFE/" + kRawCode + "/";
    std::error_code ec;
    fs::create_directories(dir, ec);

    wtp::WtLMDB db(false);
    ASSERT_TRUE(db.open(dir.c_str(), 4 * 1024 * 1024));
    wtp::WtLMDBQuery query(db);
    for (int i = 0; i < 3; i++)
    {
        WTSTickStruct tick;
        memset(&tick, 0, sizeof(tick));
        strcpy(tick.exchg, "SHFE");
        strcpy(tick.code, kRawCode);
        tick.price = 3500.0 + i;
        tick.open = 3490.0;
        tick.high = 3510.0;
        tick.low = 3480.0;
        tick.total_volume = 100 + i;
        tick.volume = 1 + i;
        tick.open_interest = 500;
        tick.trading_date = 20250110;
        tick.action_date = 20250110;
        tick.action_time = 93100000 + i * 1000;

        LMDBHftKey key("SHFE", kRawCode, tick.trading_date, tick.action_time);
        ASSERT_TRUE(query.put(&key, sizeof(key), &tick, sizeof(tick)));
    }
    query.commit();
}

void write_min1_bars(const std::string& root)
{
    const std::string dir = root + "min1/SHFE/";
    std::error_code ec;
    fs::create_directories(dir, ec);

    wtp::WtLMDB db(false);
    ASSERT_TRUE(db.open(dir.c_str(), 4 * 1024 * 1024));
    wtp::WtLMDBQuery query(db);
    for (uint32_t i = 0; i < 15; i++)
    {
        WTSBarStruct bar;
        memset(&bar, 0, sizeof(bar));
        bar.date = 20250110;
        bar.time = bar_time(917 + i);
        bar.open = 3000.0 + i;
        bar.high = 3010.0 + i;
        bar.low = 2990.0 + i;
        bar.close = 3005.0 + i;
        bar.vol = 10 + i;

        LMDBBarKey key("SHFE", kRawCode, (uint32_t)bar.time);
        ASSERT_TRUE(query.put(&key, sizeof(key), &bar, sizeof(bar)));
    }
    query.commit();
}

WTSVariant* make_cfg(const std::string& root, bool align)
{
    const std::string json = "{\"store\":{\"module\":\"WtDataStorageAD\",\"path\":\"" + root +
        "\"},\"align_by_section\":" + (align ? "true" : "false") + "}";
    return WTSCfgLoader::load_from_content(json.c_str());
}

struct R101BarSnapshot
{
    uint32_t date;
    uint64_t time;
    double close;
    double vol;
};

class R101RecordingHftEngine : public r71::R71HftEngine
{
public:
    void on_bar(const char* code, const char* period, uint32_t times,
        wtp::WTSBarStruct* newBar) override
    {
        r71::R71HftEngine::on_bar(code, period, times, newBar);
        if (newBar != nullptr)
        {
            const R101BarSnapshot snapshot = { newBar->date, newBar->time, newBar->close, newBar->vol };
            delivered_bars.push_back(snapshot);
            if (on_bar_hook)
                on_bar_hook(snapshot);
        }
    }

    std::vector<R101BarSnapshot> delivered_bars;
    std::function<void(const R101BarSnapshot&)> on_bar_hook;
};

class R101DtMgrStore : public ::testing::Test
{
protected:
    r71::R71GenEnv env;
    r71::R71BaseDataMgr bd;
    r71::R71HotMgr hot;
    R101RecordingHftEngine engine;

    void SetUp() override
    {
        bd.regisSession("TRADING");
        WTSSessionInfo* sess = bd.session("TRADING");
        ASSERT_NE(sess, nullptr);
        sess->addTradingSection(900, 1500);

        WTSCommodityInfo* ag = r71::makeComm("SHFE", "ag", "TRADING", TM_Both);
        ag->setSessionInfo(sess);
        bd.regis(ag, r71::makeCt("SHFE", "ag", "ag2512"));
        engine.attach(&bd, &hot, NULL, 20250110, 931);
    }
};

//////////////////////////////////////////////////////////////////
// HFQ tick 缓存 + handle_push_quote 追加
// src/WtCore/WtDtMgr.cpp:229-239, 287-376
//////////////////////////////////////////////////////////////////
TEST_F(R101DtMgrStore, HfqTickCacheGeneratedThenAppended)
{
    r71::R71PluginDir plug("WtDataStorageAD");
    ASSERT_TRUE(plug.ok());
    const std::string oldInst = WtHelper::getInstDir();
    WtHelper::setInstDir((plug.dir() + "/").c_str());

    const std::string root = store_root();
    write_ticks(root);

    WtDtMgr mgr;
    WTSVariant* cfg = make_cfg(root, false);
    ASSERT_NE(cfg, nullptr);
    ASSERT_TRUE(mgr.init(cfg, &engine, false));
    cfg->release();

    // 非复权直读：走 reader 直读分支
    WTSTickSlice* plain = mgr.get_tick_slice(kCode, 2, 20250110093103000ull);
    ASSERT_NE(plain, nullptr);
    EXPECT_EQ(2u, plain->size());
    plain->release();

    // 后复权：首次查询生成 _ticks_adjusted 缓存
    WTSTickSlice* hfq = mgr.get_tick_slice("SHFE.ag.2512+", 2, 20250110093103000ull);
    ASSERT_NE(hfq, nullptr);
    EXPECT_EQ(2u, hfq->size());
    hfq->release();

    // 二次查询走缓存命中（不重新读 LMDB）
    WTSTickSlice* again = mgr.get_tick_slice("SHFE.ag.2512+", 3, 20250110093103000ull);
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(3u, again->size());
    again->release();

    // _ticks_adjusted 非空：推送行情追加到复权 tick 缓存
    WTSTickData* tick = r71::makeTick("SHFE", kCode, 20250110, 20250110, 93103000, 3600.0);
    ASSERT_NE(tick, nullptr);
    tick->getTickStruct().volume = 0;
    mgr.handle_push_quote(kCode, tick);
    tick->release();

    WTSTickSlice* appended = mgr.get_tick_slice("SHFE.ag.2512+", 4, 20250110093103000ull);
    ASSERT_NE(appended, nullptr);
    EXPECT_EQ(4u, appended->size());
    appended->release();

    WtHelper::setInstDir(oldInst.c_str());
}

//////////////////////////////////////////////////////////////////
// K 线重采样缓存 + on_bar 通知
// src/WtCore/WtDtMgr.cpp:180-217, 451-541
//////////////////////////////////////////////////////////////////
TEST_F(R101DtMgrStore, KlineResampleCacheAndBarNotifications)
{
    r71::R71PluginDir plug("WtDataStorageAD");
    ASSERT_TRUE(plug.ok());
    const std::string oldInst = WtHelper::getInstDir();
    WtHelper::setInstDir((plug.dir() + "/").c_str());

    const std::string root = store_root();
    write_min1_bars(root);

    // 非强制缓存：times==1 直接读并登记基础周期订阅；times==5 走重采样
    {
        WtDtMgr mgr;
        WTSVariant* cfg = make_cfg(root, false);
        ASSERT_NE(cfg, nullptr);
        ASSERT_TRUE(mgr.init(cfg, &engine, false));
        cfg->release();

        WTSKlineSlice* base = mgr.get_kline_slice(kCode, KP_Minute1, 1, 3);
        ASSERT_NE(base, nullptr);
        EXPECT_EQ(3u, base->size());
        base->release();

        WTSKlineSlice* resampled = mgr.get_kline_slice(kCode, KP_Minute1, 5, 2);
        ASSERT_NE(resampled, nullptr);
        EXPECT_EQ(2u, resampled->size());
        resampled->release();

        WTSBarStruct next;
        memset(&next, 0, sizeof(next));
        next.date = 20250110;
        next.time = bar_time(932);
        next.close = 3100.0;
        mgr.on_bar(kCode, KP_Minute1, &next);

        engine.bar_count = 0;
        mgr.on_all_bar_updated(932);
        EXPECT_GE(engine.bar_count, 1u);
    }

    // 强制缓存：times==1 也进 _bars_cache，on_bar 命中 times==1 的追加分支
    {
        WtDtMgr mgr;
        WTSVariant* cfg = make_cfg(root, true);
        ASSERT_NE(cfg, nullptr);
        ASSERT_TRUE(mgr.init(cfg, &engine, true));
        cfg->release();

        WTSKlineSlice* first = mgr.get_kline_slice(kCode, KP_Minute1, 1, 3);
        ASSERT_NE(first, nullptr);
        EXPECT_EQ(3u, first->size());
        first->release();

        WTSKlineSlice* five = mgr.get_kline_slice(kCode, KP_Minute1, 5, 2);
        ASSERT_NE(five, nullptr);
        EXPECT_EQ(2u, five->size());
        five->release();

        // 缓存条数足够，第二次请求不再读库
        WTSKlineSlice* hit = mgr.get_kline_slice(kCode, KP_Minute1, 1, 3);
        ASSERT_NE(hit, nullptr);
        EXPECT_EQ(3u, hit->size());
        hit->release();

        WTSBarStruct next;
        memset(&next, 0, sizeof(next));
        next.date = 20250110;
        next.time = bar_time(932);
        next.close = 3200.0;
        mgr.on_bar(kCode, KP_Minute1, &next);

        engine.bar_count = 0;
        mgr.on_all_bar_updated(932);
        EXPECT_GE(engine.bar_count, 1u);
    }

    WtHelper::setInstDir(oldInst.c_str());
}

//////////////////////////////////////////////////////////////////
// Delayed notifications must preserve each queued bar's value snapshot
//////////////////////////////////////////////////////////////////
TEST_F(R101DtMgrStore, DelayedBarNotificationsKeepMutableSlotSnapshots)
{
    r71::R71PluginDir plug("WtDataStorageAD");
    ASSERT_TRUE(plug.ok());
    const std::string oldInst = WtHelper::getInstDir();
    WtHelper::setInstDir((plug.dir() + "/").c_str());

    const std::string root = store_root();
    write_min1_bars(root);

    WtDtMgr mgr;
    WTSVariant* cfg = make_cfg(root, false);
    ASSERT_NE(cfg, nullptr);
    ASSERT_TRUE(mgr.init(cfg, &engine, false));
    cfg->release();

    // 用真实 AD reader 建立基础周期订阅；之后用同一个可变 bar 地址
    // 连续模拟 reader 回调，直到统一 on_all_bar_updated 才允许引擎收到通知。
    WTSKlineSlice* subscribed = mgr.get_kline_slice(kCode, KP_Minute1, 1, 3);
    ASSERT_NE(subscribed, nullptr);
    subscribed->release();

    engine.delivered_bars.clear();
    engine.bar_count = 0;
    WTSBarStruct mutableSlot;
    const uint32_t minutes[] = { 932, 933, 934 };
    const double closes[] = { 3211.0, 3222.0, 3233.0 };
    const double volumes[] = { 11.0, 22.0, 33.0 };
    for (std::size_t i = 0; i < 3; ++i)
    {
        mutableSlot.date = 20250110;
        mutableSlot.time = bar_time(minutes[i]);
        mutableSlot.close = closes[i];
        mutableSlot.vol = volumes[i];
        mgr.on_bar(kCode, KP_Minute1, &mutableSlot);
    }

    mgr.on_all_bar_updated(934);

    ASSERT_EQ(3u, engine.delivered_bars.size());
    EXPECT_EQ(3u, engine.bar_count);
    for (std::size_t i = 0; i < 3; ++i)
    {
        EXPECT_EQ(20250110u, engine.delivered_bars[i].date);
        EXPECT_EQ(bar_time(minutes[i]), engine.delivered_bars[i].time);
        EXPECT_DOUBLE_EQ(closes[i], engine.delivered_bars[i].close);
        EXPECT_DOUBLE_EQ(volumes[i], engine.delivered_bars[i].vol);
    }

    WtHelper::setInstDir(oldInst.c_str());
}

TEST_F(R101DtMgrStore, DelayedForcedCacheBarNotificationsKeepMutableSlotSnapshots)
{
    r71::R71PluginDir plug("WtDataStorageAD");
    ASSERT_TRUE(plug.ok());
    const std::string oldInst = WtHelper::getInstDir();
    WtHelper::setInstDir((plug.dir() + "/").c_str());

    const std::string root = store_root();
    write_min1_bars(root);

    WtDtMgr mgr;
    WTSVariant* cfg = make_cfg(root, true);
    ASSERT_NE(cfg, nullptr);
    ASSERT_TRUE(mgr.init(cfg, &engine, true));
    cfg->release();

    // 强制缓存的一倍周期不登记基础周期订阅；通知来自 bars_cache 分支。
    WTSKlineSlice* cached = mgr.get_kline_slice(kCode, KP_Minute1, 1, 3);
    ASSERT_NE(cached, nullptr);
    cached->release();

    WTSBarStruct mutableSlot;
    const uint32_t minutes[] = { 935, 936, 937 };
    const double closes[] = { 3311.0, 3322.0, 3333.0 };
    const double volumes[] = { 41.0, 42.0, 43.0 };
    for (std::size_t i = 0; i < 3; ++i)
    {
        mutableSlot.date = 20250110;
        mutableSlot.time = bar_time(minutes[i]);
        mutableSlot.close = closes[i];
        mutableSlot.vol = volumes[i];
        mgr.on_bar(kCode, KP_Minute1, &mutableSlot);
    }

    mgr.on_all_bar_updated(937);

    ASSERT_EQ(3u, engine.delivered_bars.size());
    for (std::size_t i = 0; i < 3; ++i)
    {
        EXPECT_EQ(20250110u, engine.delivered_bars[i].date);
        EXPECT_EQ(bar_time(minutes[i]), engine.delivered_bars[i].time);
        EXPECT_DOUBLE_EQ(closes[i], engine.delivered_bars[i].close);
        EXPECT_DOUBLE_EQ(volumes[i], engine.delivered_bars[i].vol);
    }

    WtHelper::setInstDir(oldInst.c_str());
}

TEST_F(R101DtMgrStore, DelayedResampledNotificationsKeepUpdatedBarSnapshots)
{
    r71::R71PluginDir plug("WtDataStorageAD");
    ASSERT_TRUE(plug.ok());
    const std::string oldInst = WtHelper::getInstDir();
    WtHelper::setInstDir((plug.dir() + "/").c_str());

    const std::string root = store_root();
    write_min1_bars(root);

    WtDtMgr mgr;
    WTSVariant* cfg = make_cfg(root, false);
    ASSERT_NE(cfg, nullptr);
    ASSERT_TRUE(mgr.init(cfg, &engine, false));
    cfg->release();

    // times=5 的缓存保留了当前未闭合的内部聚合 bar。将同一终点的
    // 修订连续送入 on_bar，会多次把同一内部 bar 指针排入通知队列。
    WTSKlineSlice* resampled = mgr.get_kline_slice(kCode, KP_Minute1, 5, 2);
    ASSERT_NE(resampled, nullptr);
    ASSERT_EQ(2u, resampled->size());
    resampled->release();

    const double closes[] = { 3411.0, 3422.0, 3433.0 };
    const double volumes[] = { 1.0, 2.0, 3.0 };
    for (std::size_t i = 0; i < 3; ++i)
    {
        WTSBarStruct revision;
        revision.date = 20250110;
        revision.time = bar_time(935);
        revision.close = closes[i];
        revision.vol = volumes[i];
        mgr.on_bar(kCode, KP_Minute1, &revision);
    }

    mgr.on_all_bar_updated(935);

    ASSERT_EQ(3u, engine.delivered_bars.size());
    for (std::size_t i = 0; i < 3; ++i)
    {
        EXPECT_EQ(20250110u, engine.delivered_bars[i].date);
        EXPECT_EQ(bar_time(935), engine.delivered_bars[i].time);
        EXPECT_DOUBLE_EQ(closes[i], engine.delivered_bars[i].close);
        if (i > 0)
            EXPECT_DOUBLE_EQ(volumes[i], engine.delivered_bars[i].vol - engine.delivered_bars[i - 1].vol);
    }

    WtHelper::setInstDir(oldInst.c_str());
}

TEST_F(R101DtMgrStore, ReentrantDrainDefersNestedCallbacksButDrainsRequestedBatch)
{
    r71::R71PluginDir plug("WtDataStorageAD");
    ASSERT_TRUE(plug.ok());
    const std::string oldInst = WtHelper::getInstDir();
    WtHelper::setInstDir((plug.dir() + "/").c_str());

    const std::string root = store_root();
    write_min1_bars(root);

    WtDtMgr mgr;
    WTSVariant* cfg = make_cfg(root, false);
    ASSERT_NE(cfg, nullptr);
    ASSERT_TRUE(mgr.init(cfg, &engine, false));
    cfg->release();

    WTSKlineSlice* subscribed = mgr.get_kline_slice(kCode, KP_Minute1, 1, 3);
    ASSERT_NE(subscribed, nullptr);
    subscribed->release();

    bool reentered = false;
    std::size_t deliveredInsideA = 0;
    engine.on_bar_hook = [&](const R101BarSnapshot&) {
        if (reentered)
            return;
        reentered = true;

        WTSBarStruct queuedDuringCallback;
        queuedDuringCallback.date = 20250110;
        queuedDuringCallback.time = bar_time(938);
        queuedDuringCallback.close = 3533.0;
        queuedDuringCallback.vol = 33.0;
        mgr.on_bar(kCode, KP_Minute1, &queuedDuringCallback);
        mgr.on_all_bar_updated(938);
        deliveredInsideA = engine.delivered_bars.size();
    };

    WTSBarStruct first;
    first.date = 20250110;
    first.time = bar_time(936);
    first.close = 3511.0;
    first.vol = 11.0;
    mgr.on_bar(kCode, KP_Minute1, &first);

    mgr.on_all_bar_updated(937);

    ASSERT_TRUE(reentered);
    EXPECT_EQ(1u, deliveredInsideA);
    ASSERT_EQ(2u, engine.delivered_bars.size());
    EXPECT_EQ(bar_time(936), engine.delivered_bars[0].time);
    EXPECT_DOUBLE_EQ(3511.0, engine.delivered_bars[0].close);
    EXPECT_EQ(bar_time(938), engine.delivered_bars[1].time);
    EXPECT_DOUBLE_EQ(3533.0, engine.delivered_bars[1].close);

    WtHelper::setInstDir(oldInst.c_str());
}

TEST_F(R101DtMgrStore, ThrowingBarCallbackRetainsFailedAndRemainingNotifications)
{
    r71::R71PluginDir plug("WtDataStorageAD");
    ASSERT_TRUE(plug.ok());
    const std::string oldInst = WtHelper::getInstDir();
    WtHelper::setInstDir((plug.dir() + "/").c_str());

    const std::string root = store_root();
    write_min1_bars(root);

    WtDtMgr mgr;
    WTSVariant* cfg = make_cfg(root, false);
    ASSERT_NE(cfg, nullptr);
    ASSERT_TRUE(mgr.init(cfg, &engine, false));
    cfg->release();

    WTSKlineSlice* subscribed = mgr.get_kline_slice(kCode, KP_Minute1, 1, 3);
    ASSERT_NE(subscribed, nullptr);
    subscribed->release();

    bool shouldThrow = true;
    engine.on_bar_hook = [&shouldThrow](const R101BarSnapshot& snapshot) {
        if (shouldThrow && snapshot.close == 3622.0)
        {
            shouldThrow = false;
            throw std::runtime_error("injected on_bar failure after callback entry");
        }
    };

    const double closes[] = { 3611.0, 3622.0, 3633.0 };
    for (std::size_t i = 0; i < 3; ++i)
    {
        WTSBarStruct bar;
        bar.date = 20250110;
        bar.time = bar_time(941 + static_cast<uint32_t>(i));
        bar.close = closes[i];
        bar.vol = 51.0 + i;
        mgr.on_bar(kCode, KP_Minute1, &bar);
    }

    EXPECT_THROW(mgr.on_all_bar_updated(943), std::runtime_error);
    ASSERT_EQ(2u, engine.delivered_bars.size());
    EXPECT_DOUBLE_EQ(closes[0], engine.delivered_bars[0].close);
    EXPECT_DOUBLE_EQ(closes[1], engine.delivered_bars[1].close);

    // 回调已经进入后才抛错，外部副作用无法判定是否完成；只验证失败项可重试、
    // 后续项不丢失，以及已成功前缀不重发，不承诺失败项 exactly-once。
    engine.on_bar_hook = nullptr;
    mgr.on_all_bar_updated(943);

    ASSERT_EQ(4u, engine.delivered_bars.size());
    EXPECT_DOUBLE_EQ(closes[0], engine.delivered_bars[0].close);
    EXPECT_DOUBLE_EQ(closes[1], engine.delivered_bars[1].close);
    EXPECT_DOUBLE_EQ(closes[1], engine.delivered_bars[2].close);
    EXPECT_DOUBLE_EQ(closes[2], engine.delivered_bars[3].close);

    WtHelper::setInstDir(oldInst.c_str());
}

TEST_F(R101DtMgrStore, WorkerThreadCanEnqueueDuringEngineCallback)
{
    r71::R71PluginDir plug("WtDataStorageAD");
    ASSERT_TRUE(plug.ok());
    const std::string oldInst = WtHelper::getInstDir();
    WtHelper::setInstDir((plug.dir() + "/").c_str());

    const std::string root = store_root();
    write_min1_bars(root);

    WtDtMgr mgr;
    WTSVariant* cfg = make_cfg(root, false);
    ASSERT_NE(cfg, nullptr);
    ASSERT_TRUE(mgr.init(cfg, &engine, false));
    cfg->release();

    WTSKlineSlice* subscribed = mgr.get_kline_slice(kCode, KP_Minute1, 1, 3);
    ASSERT_NE(subscribed, nullptr);
    subscribed->release();

    // 此时订阅集合只读且 bars_cache 为空；工作线程只测试通知队列入队，
    // 不与 reader/缓存构建或其它策略管理器状态并发。
    EXPECT_EXIT({
        std::atomic<bool> finished(false);
        std::thread watchdog([&finished]() {
            for (int i = 0; i < 500; ++i)
            {
                if (finished.load(std::memory_order_acquire))
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!finished.load(std::memory_order_acquire))
                std::_Exit(124);
        });

        engine.on_bar_hook = [&mgr](const R101BarSnapshot&) {
            std::thread worker([&mgr]() {
                WTSBarStruct queuedByWorker;
                queuedByWorker.date = 20250110;
                queuedByWorker.time = bar_time(952);
                queuedByWorker.close = 3733.0;
                queuedByWorker.vol = 73.0;
                mgr.on_bar(kCode, KP_Minute1, &queuedByWorker);
            });
            worker.join();
        };

        WTSBarStruct currentBatch;
        currentBatch.date = 20250110;
        currentBatch.time = bar_time(951);
        currentBatch.close = 3711.0;
        currentBatch.vol = 71.0;
        mgr.on_bar(kCode, KP_Minute1, &currentBatch);
        mgr.on_all_bar_updated(951);

        if (engine.delivered_bars.size() != 1 ||
            engine.delivered_bars[0].time != bar_time(951) ||
            engine.delivered_bars[0].close != 3711.0)
            std::_Exit(3);

        engine.on_bar_hook = nullptr;
        mgr.on_all_bar_updated(952);
        if (engine.delivered_bars.size() != 2 ||
            engine.delivered_bars[1].time != bar_time(952) ||
            engine.delivered_bars[1].close != 3733.0)
            std::_Exit(4);

        finished.store(true, std::memory_order_release);
        watchdog.join();
        std::_Exit(0);
    }, ::testing::ExitedWithCode(0), "");

    WtHelper::setInstDir(oldInst.c_str());
}

TEST_F(R101DtMgrStore, WorkerThreadDrainRequestDuringBatchIsNotLost)
{
    r71::R71PluginDir plug("WtDataStorageAD");
    ASSERT_TRUE(plug.ok());
    const std::string oldInst = WtHelper::getInstDir();
    WtHelper::setInstDir((plug.dir() + "/").c_str());

    const std::string root = store_root();
    write_min1_bars(root);

    WtDtMgr mgr;
    WTSVariant* cfg = make_cfg(root, false);
    ASSERT_NE(cfg, nullptr);
    ASSERT_TRUE(mgr.init(cfg, &engine, false));
    cfg->release();

    WTSKlineSlice* subscribed = mgr.get_kline_slice(kCode, KP_Minute1, 1, 3);
    ASSERT_NE(subscribed, nullptr);
    subscribed->release();

    // 订阅已建立，且 bars_cache 为空；工作线程只入队并提交显式 drain 请求。
    EXPECT_EXIT({
        std::atomic<bool> finished(false);
        bool redrainRequested = false;
        std::thread watchdog([&finished]() {
            for (int i = 0; i < 500; ++i)
            {
                if (finished.load(std::memory_order_acquire))
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!finished.load(std::memory_order_acquire))
                std::_Exit(124);
        });

        engine.on_bar_hook = [&](const R101BarSnapshot&) {
            if (redrainRequested)
                return;
            redrainRequested = true;

            std::thread worker([&mgr]() {
                WTSBarStruct queuedByWorker;
                queuedByWorker.date = 20250110;
                queuedByWorker.time = bar_time(954);
                queuedByWorker.close = 3822.0;
                queuedByWorker.vol = 82.0;
                mgr.on_bar(kCode, KP_Minute1, &queuedByWorker);
                mgr.on_all_bar_updated(954);
            });
            worker.join();
        };

        WTSBarStruct currentBatch;
        currentBatch.date = 20250110;
        currentBatch.time = bar_time(953);
        currentBatch.close = 3811.0;
        currentBatch.vol = 81.0;
        mgr.on_bar(kCode, KP_Minute1, &currentBatch);
        mgr.on_all_bar_updated(953);

        if (engine.delivered_bars.size() != 2 ||
            engine.delivered_bars[0].time != bar_time(953) ||
            engine.delivered_bars[0].close != 3811.0 ||
            engine.delivered_bars[1].time != bar_time(954) ||
            engine.delivered_bars[1].close != 3822.0)
            std::_Exit(3);

        finished.store(true, std::memory_order_release);
        watchdog.join();
        std::_Exit(0);
    }, ::testing::ExitedWithCode(0), "");

    WtHelper::setInstDir(oldInst.c_str());
}

}  // namespace
