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

#include <cstring>
#include <filesystem>
#include <string>
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

class R101DtMgrStore : public ::testing::Test
{
protected:
    r71::R71GenEnv env;
    r71::R71BaseDataMgr bd;
    r71::R71HotMgr hot;
    r71::R71HftEngine engine;

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

}  // namespace
