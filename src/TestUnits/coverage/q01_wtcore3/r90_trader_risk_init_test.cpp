/**
 * r90_trader_risk_init_test.cpp — lane R90 / wave17
 *
 * 目标：src/WtCore/TraderAdapter.cpp 的风控频率窗口与 init() 装配失败分支。
 *
 * 覆盖行段：
 *   - checkCancelLimits 撤单频率窗口（去重/越界封禁/历史裁剪）：:566-593
 *   - checkOrderLimits 下单频率窗口：:631-658
 *   - init() 的 savedata/无默认风控规则告警、模块加载失败/createTrader 缺失、
 *     重复 init 拒绝：:105-207
 *
 * 频率窗口用「交易所原始代码」直接调用检查函数，绕开 P11-003 记录的
 * 原始码/标准码键不一致（该缺陷已在 bugs/p11 登记，本 lane 不重复开号）。
 */
#include "../p11_wtcore_traderadapter/r59_trader_fixture.h"
#include "r90_env.h"

#include <WtCore/EventNotifier.h>
#include <WtCore/WtDtMgr.h>

#include <fstream>

using namespace p11fix;
using wtp::TraderAdapter;

namespace {

::testing::Environment* const g_r90_env =
    ::testing::AddGlobalTestEnvironment(new r90::GenEnv);

std::string writeCfgFile(const char* name, const std::string& text)
{
    return r90::write_file(std::string("cfg/") + name, text);
}

WTSVariant* loadCfg(const char* name, const std::string& text)
{
    std::string path = writeCfgFile(name, text);
    return WTSCfgLoader::load_from_file(path.c_str());
}

class R90TraderRisk : public P11TraderTest
{
protected:
    void SetUp() override
    {
        P11TraderTest::SetUp();
        id_ = "r90risk";
    }

    void attach(wtp::TraderAdapter& trader)
    {
        ASSERT_TRUE(trader.initExt(id_.c_str(), &api_, &bd_, NULL));
        ASSERT_TRUE(trader.run());
    }
};

//////////////////////////////////////////////////////////////////
// 下单/撤单频率窗口：连续 3 次、边界 1 -> 第 3 次检查封禁
// src/WtCore/TraderAdapter.cpp:566-593, 631-658
//////////////////////////////////////////////////////////////////
TEST_F(R90TraderRisk, FrequencyWindowsExcludeRawCode)
{
    const char* cfgText =
        "{\"savedata\":false,\"riskmon\":{\"active\":true,\"policy\":{"
        "\"default\":{\"order_times_boundary\":1,\"order_stat_timespan\":60,"
        "\"order_total_limits\":0,\"cancel_times_boundary\":1,"
        "\"cancel_stat_timespan\":60,\"cancel_total_limits\":0}}}}";

    // 下单频率：doEntrust 按原始代码 ag2512 写缓存
    {
        WTSVariant* cfg = loadCfg("r90_order_freq.json", cfgText);
        ASSERT_NE(nullptr, cfg);
        TraderAdapter trader;
        attach(trader);
        EXPECT_FALSE(trader.init(id_.c_str(), cfg, &bd_, NULL));
        cfg->release();
        ASSERT_TRUE(trader.run());

        for (uint32_t i = 0; i < 3; ++i)
            EXPECT_NE(UINT_MAX, trader.openLong("SHFE.ag.2512", 7000.0, 1, 0));
        EXPECT_EQ(3u, api_.n_insert_);

        // 标准代码查不到原始码缓存 -> 放行（P11-003 现状）
        EXPECT_TRUE(trader.checkOrderLimits("SHFE.ag.2512"));
        // 直接用缓存键检查 -> 命中频率窗口并封禁
        EXPECT_FALSE(trader.checkOrderLimits("ag2512"));
        // 排除队列短路
        EXPECT_FALSE(trader.checkOrderLimits("ag2512"));
    }

    // 撤单频率：cancel() 按原始代码写 _cancel_time_cache
    {
        WTSVariant* cfg = loadCfg("r90_cancel_freq.json", cfgText);
        ASSERT_NE(nullptr, cfg);
        TraderAdapter trader;
        attach(trader);
        EXPECT_FALSE(trader.init(id_.c_str(), cfg, &bd_, NULL));
        cfg->release();
        ASSERT_TRUE(trader.run());

        trader.onRspOrders(r59x::makeArray({
            r59x::makeOrder(ct_, WDT_LONG, WOT_OPEN, WOS_NotTraded_Queuing, 1, 0,
                "FQ1", "otp.r90risk.1001"),
            r59x::makeOrder(ct_, WDT_LONG, WOT_OPEN, WOS_NotTraded_Queuing, 1, 0,
                "FQ2", "otp.r90risk.1002"),
            r59x::makeOrder(ct_, WDT_LONG, WOT_OPEN, WOS_NotTraded_Queuing, 1, 0,
                "FQ3", "otp.r90risk.1003") }));

        EXPECT_TRUE(trader.cancel(1001u));
        EXPECT_TRUE(trader.cancel(1002u));
        EXPECT_TRUE(trader.cancel(1003u));
        EXPECT_EQ(3u, api_.n_action_);

        EXPECT_TRUE(trader.checkCancelLimits("SHFE.ag.2512"));
        EXPECT_FALSE(trader.checkCancelLimits("ag2512"));
        EXPECT_FALSE(trader.checkCancelLimits("ag2512"));
    }
}

//////////////////////////////////////////////////////////////////
// init()：savedata 建表、无 default 风控告警、模块可加载但无 createTrader、
// 重复 init 拒绝
// src/WtCore/TraderAdapter.cpp:105-207
//////////////////////////////////////////////////////////////////
TEST_F(R90TraderRisk, ModuleWithoutCreateTraderAndRepeatedInit)
{
    const std::string binDir = r90::find_bin_dir();
    if (binDir.empty())
        GTEST_SKIP() << "build bin dir with libWtMsgQue not found";

    namespace fs = std::filesystem;
    const std::string instRoot = r90::temp_root() + "inst/";
    ASSERT_TRUE(fs::create_directories(fs::path(instRoot) / "traders"));
    std::error_code ec;
    fs::create_symlink(fs::path(binDir) / "libWtMsgQue.dylib",
        fs::path(instRoot) / "traders" / "libWtMsgQue.dylib", ec);
    if (ec)
        GTEST_SKIP() << "cannot create module symlink: " << ec.message();
    fs::create_symlink(fs::path(binDir) / "libWtMsgQue.dylib",
        fs::path(instRoot) / "libWtMsgQue.dylib", ec);
    ASSERT_FALSE(ec);

    std::string oldInst = WtHelper::getInstDir();
    WtHelper::setInstDir(instRoot.c_str());

    WTSVariant* cfg = loadCfg("r90_module_missing.json",
        "{\"savedata\":true,\"ignore_selfmatch\":true,"
        "\"riskmon\":{\"active\":true,\"policy\":{"
        "\"SHFE.ag\":{\"order_times_boundary\":9,\"order_stat_timespan\":60,"
        "\"order_total_limits\":9,\"cancel_times_boundary\":9,"
        "\"cancel_stat_timespan\":60,\"cancel_total_limits\":9}}},"
        "\"module\":\"WtMsgQue\"}");
    ASSERT_NE(nullptr, cfg);

    TraderAdapter trader;
    // 模块能 dlopen，但没有 createTrader -> false
    EXPECT_FALSE(trader.init(id_.c_str(), cfg, &bd_, NULL));
    // _cfg 已落地 -> 第二次 init 在入口拒绝
    EXPECT_FALSE(trader.init(id_.c_str(), cfg, &bd_, NULL));
    cfg->release();

    // savedata 在建 cfg 之后已创建 traders/<id> 目录与两张表
    std::string folder = std::string(WtHelper::getBaseDir()) + "traders/" + id_ + "/";
    EXPECT_TRUE(fs::exists(fs::path(folder) / "trades.csv"));
    EXPECT_TRUE(fs::exists(fs::path(folder) / "orders.csv"));

    // 同一模块配置给 WtDtMgr：能 dlopen 但没有 createDataReader -> initStore false
    {
        WTSVariant* storeCfg = loadCfg("r90_dt_store.json",
            "{\"align_by_section\":true,\"store\":{\"module\":\"WtMsgQue\"}}");
        ASSERT_NE(nullptr, storeCfg);
        WtDtMgr dt;
        EXPECT_FALSE(dt.init(storeCfg, NULL, false));
        storeCfg->release();
    }

    // EventNotifier 的 creator 缺失分支：把同名模块指向不含 create_server 的库
    {
        const std::string badRoot = r90::temp_root() + "inst_bad/";
        ASSERT_TRUE(fs::create_directories(fs::path(badRoot)));
        fs::create_symlink(fs::path(binDir) / "libWtDataStorage.dylib",
            fs::path(badRoot) / "libWtMsgQue.dylib", ec);
        if (!ec)
        {
            WtHelper::setInstDir(badRoot.c_str());
            WTSVariant* ntfCfg = loadCfg("r90_ntf_cfg.json",
                "{\"active\":true,\"url\":\"inproc://r90_bad\"}");
            ASSERT_NE(nullptr, ntfCfg);
            EventNotifier notifier;
            EXPECT_FALSE(notifier.init(ntfCfg));
            ntfCfg->release();
            WtHelper::setInstDir(instRoot.c_str());
        }
    }

    WtHelper::setInstDir(oldInst.c_str());
}

}  // namespace
