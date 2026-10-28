/**
 * r71_dtmgr_test.cpp — lane R71 / WtDtMgr reader 成功路径补测
 *
 * 覆盖 src/WtCore/WtDtMgr.cpp：
 *   initStore 成功（dlopen WtDataStorageAD 插件）/ init 读配置
 *   get_adjusting_factor / get_adjusting_flag 的 reader 分支
 *   get_order_queue_slice / get_order_detail_slice / get_transaction_slice 委托
 *   get_kline_slice 的直读与重采样缓存两条路径（空库 -> NULL）
 *   on_bar 订阅命中 -> 通知队列，on_all_bar_updated -> engine->on_bar
 *   handle_push_quote / grab_last_tick 实时缓存，析构释放缓存
 *
 * 插件用临时目录里的单文件符号链接，数据根目录也在临时目录；
 * WtDataStorageAD 的 reader 不起后台线程，也不访问网络/真实柜台。
 */
#include "r71_wtcore_env.h"

#include <WTSUtils/WTSCfgLoader.h>

using namespace wtp;

namespace {

class R71DtMgrTest : public ::testing::Test
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

        engine.attach(&bd, &hot, NULL);
    }

    std::string data_root()
    {
        std::string dir = r71::temp_root() + "store_data/";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    WTSVariant* makeCfg(const std::string& module, bool align)
    {
        std::string json = "{\"store\":{\"module\":\"" + module + "\",\"path\":\""
            + data_root() + "\"},\"align_by_section\":"
            + (align ? "true" : "false") + "}";
        return WTSCfgLoader::load_from_content(json.c_str());
    }
};

// 覆盖 WtDtMgr.cpp:68-86 initStore 成功、L89-101 init、L255-275 复权、
// L378-400 三类 L2 切片委托、L426-448 直读 K 线（空库返回 NULL）
TEST_F(R71DtMgrTest, ReaderInitAdjustingAndKlineDelegation)
{
    r71::R71PluginDir plug("WtDataStorageAD");
    ASSERT_TRUE(plug.ok()) << "缺少 libWtDataStorageAD，无法驱动 reader 成功路径";

    const std::string oldInst = WtHelper::getInstDir();
    WtHelper::setInstDir((plug.dir() + "/").c_str());

    WtDtMgr mgr;
    WTSVariant* cfg = makeCfg("WtDataStorageAD", false);
    ASSERT_NE(cfg, nullptr);
    ASSERT_TRUE(mgr.init(cfg, &engine, false));
    cfg->release();

    ASSERT_NE(mgr.reader(), nullptr);
    EXPECT_TRUE(mgr.loader() == NULL);
    EXPECT_DOUBLE_EQ(mgr.get_adjusting_factor("SHFE.ag.2512", 20250110), 1.0);
    EXPECT_EQ(mgr.get_adjusting_flag(), 0u);

    EXPECT_TRUE(mgr.get_order_queue_slice("SHFE.ag.2512", 5) == NULL);
    EXPECT_TRUE(mgr.get_order_detail_slice("SHFE.ag.2512", 5) == NULL);
    EXPECT_TRUE(mgr.get_transaction_slice("SHFE.ag.2512", 5) == NULL);

    // 空数据根目录：reader 读不到 K 线，返回 NULL，但基础周期订阅已登记
    EXPECT_TRUE(mgr.get_kline_slice("SHFE.ag.2512", KP_Minute1, 1, 3) == NULL);

    WTSBarStruct bar;
    memset(&bar, 0, sizeof(bar));
    bar.close = 3500.0;
    mgr.on_bar("SHFE.ag.2512", KP_Minute1, &bar);
    EXPECT_EQ(engine.bar_count, 0u);

    mgr.on_all_bar_updated(931);
    EXPECT_EQ(engine.bar_count, 1u);
    EXPECT_EQ(engine.last_bar_code, "SHFE.ag.2512");
    EXPECT_EQ(engine.last_bar_times, 1u);

    mgr.on_all_bar_updated(932);
    EXPECT_EQ(engine.bar_count, 1u);

    WtHelper::setInstDir(oldInst.c_str());
}

// 覆盖 WtDtMgr.cpp:219-253 实时 tick 缓存、L104-117 空/非空通知队列、L38-48 析构
TEST_F(R71DtMgrTest, RealtimeTickCacheAndNotifyQueueLifecycle)
{
    r71::R71PluginDir plug("WtDataStorageAD");
    ASSERT_TRUE(plug.ok());

    const std::string oldInst = WtHelper::getInstDir();
    WtHelper::setInstDir((plug.dir() + "/").c_str());

    WtDtMgr mgr;
    WTSVariant* cfg = makeCfg("WtDataStorageAD", false);
    ASSERT_NE(cfg, nullptr);
    ASSERT_TRUE(mgr.init(cfg, &engine));
    cfg->release();

    mgr.handle_push_quote("SHFE.ag.2512", NULL);
    EXPECT_TRUE(mgr.grab_last_tick("SHFE.ag.2512") == NULL);

    WTSTickData* tick = r71::makeTick("SHFE", "SHFE.ag.2512", 20250110, 20250110, 93100000, 3500.0);
    ASSERT_NE(tick, nullptr);
    mgr.handle_push_quote("SHFE.ag.2512", tick);
    tick->release();

    WTSTickData* got = mgr.grab_last_tick("SHFE.ag.2512");
    ASSERT_NE(got, nullptr);
    EXPECT_DOUBLE_EQ(got->price(), 3500.0);
    got->release();
    EXPECT_TRUE(mgr.grab_last_tick("SHFE.au.2212") == NULL);

    // 订阅基础 K 线后，on_bar 把通知压入队列
    EXPECT_TRUE(mgr.get_kline_slice("SHFE.ag.2512", KP_Minute1, 1, 2) == NULL);
    WTSBarStruct bar;
    memset(&bar, 0, sizeof(bar));
    bar.close = 3510.0;
    mgr.on_bar("SHFE.ag.2512", KP_Minute1, &bar);
    EXPECT_EQ(engine.bar_count, 0u);
    mgr.on_all_bar_updated(931);
    EXPECT_EQ(engine.bar_count, 1u);

    // mgr 析构时释放 _rt_tick_map；插件目录随后删除
    WtHelper::setInstDir(oldInst.c_str());
}

// 覆盖 WtDtMgr.cpp:451-475 会话/缓存准备、L460-513 空库重采样、
// L180-181 on_bar 的 _bars_cache 空队列早退、L532-541 缓存切片返回前的守卫
TEST_F(R71DtMgrTest, ResampleCachePathWithEmptyStoreReturnsNull)
{
    r71::R71PluginDir plug("WtDataStorageAD");
    ASSERT_TRUE(plug.ok());

    const std::string oldInst = WtHelper::getInstDir();
    WtHelper::setInstDir((plug.dir() + "/").c_str());

    WtDtMgr mgr;
    WTSVariant* cfg = makeCfg("WtDataStorageAD", true);
    ASSERT_NE(cfg, nullptr);
    ASSERT_TRUE(mgr.init(cfg, &engine, true));
    cfg->release();

    // 强制缓存 + times=1 也走缓存分支；读不到原始数据则返回 NULL
    EXPECT_TRUE(mgr.get_kline_slice("SHFE.ag.2512", KP_Minute1, 1, 4) == NULL);
    EXPECT_TRUE(mgr.get_kline_slice("SHFE.ag.2512", KP_Minute1, 5, 4) == NULL);

    WTSBarStruct bar;
    memset(&bar, 0, sizeof(bar));
    bar.close = 3520.0;
    mgr.on_bar("SHFE.ag.2512", KP_Minute1, &bar); // _bars_cache 里没有多周期项
    mgr.on_all_bar_updated(931);
    EXPECT_EQ(engine.bar_count, 0u);

    WtHelper::setInstDir(oldInst.c_str());
}

/*
 * DISABLED：WtDtMgr::init 没有判空 cfg，L93 直接 cfg->getBoolean 会段错误。
 * 入口 init(NULL, engine) 是公开 API，调用方传入空配置即崩。BUG-R71-001。
 */
TEST_F(R71DtMgrTest, DISABLED_InitNullCfgCrashes)
{
    WtDtMgr mgr;
    EXPECT_FALSE(mgr.init(NULL, &engine));
}

} // namespace
