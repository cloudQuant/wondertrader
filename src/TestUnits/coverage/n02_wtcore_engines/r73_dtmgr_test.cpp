/**
 * r73_dtmgr_test.cpp — lane R73 / WtDtMgr 数据面补测
 *
 * 覆盖 src/WtCore/WtDtMgr.cpp 中 R42/R71 未覆盖的分支：
 *   - L2 切片委托与 K 线直读/重采样路径
 *   - on_bar 的 KP_Minute5 / KP_DAY 周期映射与通知队列
 *   - initStore 成功但库为空时的切片委托与强制缓存路径
 *   - 空库上 get_tick_slice 在 reader 内索引空环形缓冲区的缺陷（DISABLED）
 *
 * 使用构建产物里的 WtDataStorageAD 插件（只建立临时目录里的符号链接），
 * 数据根目录也在系统临时目录，reader 不起线程、不访问网络/柜台。
 */
#include "r73_fixture.h"

#include <WTSUtils/WTSCfgLoader.h>

using namespace wtp;

namespace {

class R73DtMgrTest : public ::testing::Test
{
protected:
    r73::TempDir _tmp{"dtmgr"};
    r73::BaseDataMgr _bd;
    r73::HotMgr _hot;
    r73::HftEngine _engine;

    void SetUp() override
    {
        _bd.regisSession("TRADING");
        WTSSessionInfo* sess = _bd.session("TRADING");
        ASSERT_NE(sess, nullptr);
        sess->addTradingSection(900, 1500);

        WTSCommodityInfo* ag = r73::makeComm("SHFE", "ag", "TRADING", TM_Both);
        ag->setSessionInfo(sess);
        _bd.regis(ag, r73::makeCt("SHFE", "ag", "ag2512"));

        _engine.attach(&_bd, &_hot, NULL);
    }

    std::string data_root()
    {
        std::string dir = _tmp.path("store_data");
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

    bool initMgr(WtDtMgr& mgr, bool force, const std::string& module = "WtDataStorageAD")
    {
        r73::PluginDir plug(module.c_str(), _tmp.path());
        if (!plug.ok())
            return false;

        const std::string oldInst = WtHelper::getInstDir();
        WtHelper::setInstDir((plug.dir() + "/").c_str());

        WTSVariant* cfg = makeCfg(module, true);
        if (cfg == NULL)
        {
            WtHelper::setInstDir(oldInst.c_str());
            return false;
        }
        bool ok = mgr.init(cfg, &_engine, force);
        cfg->release();
        WtHelper::setInstDir(oldInst.c_str());
        return ok;
    }
};

// 覆盖 WtDtMgr.cpp:68-86（reader 成功装载）、L255-275（复权因子/标志）、
// L378-400（L2 切片委托）、
// L421-448（times==1 且非强制缓存时直读 K 线）
TEST_F(R73DtMgrTest, ReaderInitAndSliceDelegation)
{
    WtDtMgr mgr;
    ASSERT_TRUE(initMgr(mgr, false));
    ASSERT_NE(mgr.reader(), nullptr);

    // 三个 L2 转发入口都落到插件；空库返回 NULL
    EXPECT_TRUE(mgr.get_order_queue_slice("SHFE.ag.2512", 5) == NULL);
    EXPECT_TRUE(mgr.get_order_detail_slice("SHFE.ag.2512", 5) == NULL);
    EXPECT_TRUE(mgr.get_transaction_slice("SHFE.ag.2512", 5) == NULL);

    // times==1 且未强制缓存：直接委托 reader
    EXPECT_TRUE(mgr.get_kline_slice("SHFE.ag.2512", KP_Minute1, 1, 3) == NULL);

    // 复权因子/标志走 reader 分支
    EXPECT_DOUBLE_EQ(1.0, mgr.get_adjusting_factor("SHFE.ag.2512", 20250110));
    (void)mgr.get_adjusting_flag();
}

// 覆盖 WtDtMgr.cpp:149-217（on_bar 的 Minute5/Day 周期映射、订阅命中、
// 非基础周期无缓存早退）与 L104-117（通知队列清空）
TEST_F(R73DtMgrTest, BarPeriodMappingNotifiesEngine)
{
    WtDtMgr mgr;
    ASSERT_TRUE(initMgr(mgr, false));

    // m5 直读失败但订阅键已登记；on_bar 命中后累计 times=5 的通知
    EXPECT_TRUE(mgr.get_kline_slice("SHFE.ag.2512", KP_Minute5, 1, 2) == NULL);
    WTSBarStruct bar;
    memset(&bar, 0, sizeof(bar));
    bar.close = 3500.0;
    mgr.on_bar("SHFE.ag.2512", KP_Minute5, &bar);
    EXPECT_EQ(_engine.bar_count, 0u);
    mgr.on_all_bar_updated(931);
    EXPECT_EQ(_engine.bar_count, 1u);

    // 日线键单独登记，on_bar 走 default 映射（times=1）
    EXPECT_TRUE(mgr.get_kline_slice("SHFE.ag.2512", KP_DAY, 1, 2) == NULL);
    mgr.on_bar("SHFE.ag.2512", KP_DAY, &bar);
    mgr.on_all_bar_updated(931);
    EXPECT_EQ(_engine.bar_count, 2u);

    // 未订阅的周期只推进、不通知
    mgr.on_bar("SHFE.ag.2512", KP_Minute1, &bar);
    mgr.on_all_bar_updated(932);
    EXPECT_EQ(_engine.bar_count, 2u);
}

// 覆盖 WtDtMgr.cpp:451-475（会话/缓存准备）、L460-521（强制缓存 + times=1/5
// 在空库上的 NULL 返回）、L180-181（_bars_cache 非空但无匹配键的早退）
TEST_F(R73DtMgrTest, ForcedCacheResampleOnEmptyStore)
{
    WtDtMgr mgr;
    ASSERT_TRUE(initMgr(mgr, true));

    // 强制缓存后 times=1 也走缓存分支
    EXPECT_TRUE(mgr.get_kline_slice("SHFE.ag.2512", KP_Minute1, 1, 4) == NULL);
    EXPECT_TRUE(mgr.get_kline_slice("SHFE.ag.2512", KP_Minute1, 5, 4) == NULL);
    EXPECT_TRUE(mgr.get_kline_slice("SHFE.ag.2512", KP_Minute5, 2, 4) == NULL);

    WTSBarStruct bar;
    memset(&bar, 0, sizeof(bar));
    bar.close = 3520.0;
    mgr.on_bar("SHFE.ag.2512", KP_Minute1, &bar);
    mgr.on_all_bar_updated(931);
    EXPECT_EQ(_engine.bar_count, 0u);
}

/*
 * DISABLED：空库上 get_tick_slice 无论是否后复权都会进入 WtDataStorageAD
 * 的 readTickSlice；该函数在 tick 缓存为空时直接用 `_ticks[0]` 取地址，
 * boost::circular_buffer 在 Debug 下断言 index < size()，公开 API 必崩。
 * 见 BUG-R73-001。
 */
TEST_F(R73DtMgrTest, DISABLED_EmptyStoreTickSliceCrashesInReader)
{
    WtDtMgr mgr;
    ASSERT_TRUE(initMgr(mgr, false));

    WTSTickSlice* slice = mgr.get_tick_slice("SHFE.ag.2512", 5);
    EXPECT_TRUE(slice == NULL);
}

}  // namespace
