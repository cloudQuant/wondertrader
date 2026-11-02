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
 * 频率窗口用公开标准码检查由成功下单/撤单产生的缓存；夹具合约的 alt code
 * 是交易所原始码，用于核对标准码检查能否命中相应的缓存记录。
 */
#include "../p11_wtcore_traderadapter/r59_trader_fixture.h"
#include "r90_env.h"

#include <WtCore/EventNotifier.h>
#include <WtCore/WtDtMgr.h>

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>

#ifndef WT_P11_TRADERMOCKER_PATH
#error "WT_P11_TRADERMOCKER_PATH must point to the built TraderMocker plugin"
#endif

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

namespace fs = std::filesystem;

class ScopedR90MockerEnvironment
{
public:
    explicit ScopedR90MockerEnvironment(const fs::path& root)
        : _old_cwd(fs::current_path())
        , _old_inst_dir(WtHelper::getInstDir())
    {
        const fs::path cached_cwd(WtHelper::getCWD());
        std::error_code ec;
        if (!fs::equivalent(cached_cwd, _old_cwd, ec) || ec)
        {
            _error = "WtHelper CWD cache differs from the process CWD";
            if (ec)
                _error += ": " + ec.message();
            return;
        }

        const fs::path source(WT_P11_TRADERMOCKER_PATH);
        if (!fs::is_regular_file(source) || fs::is_symlink(source))
        {
            _error = "TraderMocker plugin is missing or is not a regular file: " + source.string();
            return;
        }
        if (fs::is_symlink(root))
        {
            _error = "temporary mocker root is a symlink: " + root.string();
            return;
        }

        const fs::path cwd_module = cached_cwd / "traders" / "libTraderMocker.dylib";
        if (fs::exists(cwd_module))
        {
            _error = "CWD already contains a TraderMocker plugin: " + cwd_module.string();
            return;
        }

        const fs::path traders_dir = root / "traders";
        fs::create_directories(traders_dir, ec);
        if (ec)
        {
            _error = "cannot create temporary traders directory: " + ec.message();
            return;
        }
        if (!fs::is_directory(traders_dir) || fs::is_symlink(traders_dir))
        {
            _error = "temporary traders path is not a regular directory: " + traders_dir.string();
            return;
        }

        const fs::path staged_module = traders_dir / source.filename();
        fs::copy_file(source, staged_module, fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            _error = "cannot copy TraderMocker plugin: " + ec.message();
            return;
        }
        if (!fs::is_regular_file(staged_module) || fs::is_symlink(staged_module))
        {
            _error = "staged TraderMocker plugin is not a regular file: " + staged_module.string();
            return;
        }

        fs::current_path(root, ec);
        if (ec)
        {
            _error = "cannot switch to temporary mocker CWD: " + ec.message();
            return;
        }
        _cwd_changed = true;

        _new_inst_dir = root.string() + "/";
        WtHelper::setInstDir(_new_inst_dir.c_str());
        _inst_dir_changed = true;
        _ok = true;
    }

    ~ScopedR90MockerEnvironment()
    {
        if (_inst_dir_changed)
            WtHelper::setInstDir(_old_inst_dir.c_str());
        if (_cwd_changed)
        {
            std::error_code ec;
            fs::current_path(_old_cwd, ec);
        }
    }

    bool ok() const { return _ok; }
    const std::string& error() const { return _error; }

private:
    fs::path _old_cwd;
    std::string _old_inst_dir;
    std::string _new_inst_dir;
    std::string _error;
    bool _cwd_changed = false;
    bool _inst_dir_changed = false;
    bool _ok = false;
};

class R90RiskMockerSink : public RecordingSink
{
public:
    void on_channel_ready() override
    {
        RecordingSink::on_channel_ready();
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _ready = true;
        }
        _changed.notify_all();
    }

    void on_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty,
        double leftQty, double price, bool isCanceled = false) override
    {
        RecordingSink::on_order(localid, stdCode, isBuy, totalQty, leftQty, price, isCanceled);
        {
            std::lock_guard<std::mutex> lock(_mutex);
            ++_order_events;
            if (isCanceled)
                ++_cancel_events;
        }
        _changed.notify_all();
    }

    bool waitReady()
    {
        std::unique_lock<std::mutex> lock(_mutex);
        return _changed.wait_for(lock, std::chrono::seconds(5), [this] { return _ready; });
    }

    bool waitOrders(uint32_t count)
    {
        std::unique_lock<std::mutex> lock(_mutex);
        return _changed.wait_for(lock, std::chrono::seconds(5),
            [this, count] { return _order_events >= count; });
    }

    bool waitCancels(uint32_t count)
    {
        std::unique_lock<std::mutex> lock(_mutex);
        return _changed.wait_for(lock, std::chrono::seconds(5),
            [this, count] { return _cancel_events >= count; });
    }

private:
    std::mutex _mutex;
    std::condition_variable _changed;
    bool _ready = false;
    uint32_t _order_events = 0;
    uint32_t _cancel_events = 0;
};

struct R90VariantRelease
{
    void operator()(WTSVariant* value) const
    {
        if (value)
            value->release();
    }
};

std::string activeMockerRiskConfig(uint32_t mockerId, uint32_t orderBoundary,
    uint32_t cancelBoundary)
{
    std::ostringstream json;
    json << "{\"module\":\"TraderMocker\",\"mockerid\":" << mockerId
        << ",\"span\":0,\"udp_port\":0,\"savedata\":false,\"riskmon\":{\"active\":true,\"policy\":{"
        << "\"default\":{\"order_times_boundary\":" << orderBoundary
        << ",\"order_stat_timespan\":60,\"order_total_limits\":0"
        << ",\"cancel_times_boundary\":" << cancelBoundary
        << ",\"cancel_stat_timespan\":60,\"cancel_total_limits\":0}}}}";
    return json.str();
}

class R90TraderRisk : public P11TraderTest
{
protected:
    void SetUp() override
    {
        P11TraderTest::SetUp();
        id_ = "r90risk";
    }
};

//////////////////////////////////////////////////////////////////
// 下单/撤单频率窗口：连续 3 次、边界 1 -> 第 3 次检查封禁
// src/WtCore/TraderAdapter.cpp:566-593, 631-658
//////////////////////////////////////////////////////////////////
TEST_F(R90TraderRisk, FrequencyWindowsRejectStandardCodeAfterRawCodeEvents)
{
    const fs::path plugin(WT_P11_TRADERMOCKER_PATH);
    if (!fs::is_regular_file(plugin) || fs::is_symlink(plugin))
        GTEST_SKIP() << "regular TraderMocker plugin not found: " << plugin.string();

    // 风控必须经 TraderAdapter::init 的完整模块路径解析并启用；initExt
    // 没有 riskmon 参数，不能用它验证风控频率缓存。
    {
        const uint32_t mockerId = static_cast<uint32_t>(::getpid()) * 100u + 901u;
        const fs::path root = fs::path(r90::temp_root()) / ("freq_order_" + std::to_string(mockerId));
        ScopedR90MockerEnvironment mockerEnv(root);
        ASSERT_TRUE(mockerEnv.ok()) << mockerEnv.error();

        std::unique_ptr<WTSVariant, R90VariantRelease> cfg(
            loadCfg("r90_order_freq.json", activeMockerRiskConfig(mockerId, 1, 9)));
        ASSERT_NE(nullptr, cfg.get());

        R90RiskMockerSink sink;
        TraderAdapter trader;
        ASSERT_TRUE(trader.init(id_.c_str(), cfg.get(), &bd_, NULL));
        cfg.reset();
        trader.addSink(&sink);
        ASSERT_TRUE(trader.run());
        ASSERT_TRUE(sink.waitReady()) << "TraderMocker did not become ready";

        // 完整 init 后风控先放行；每次 API 提交都由 TraderMocker 接受，
        // 并在回报中保留交易所原始代码 ag2512。
        ASSERT_TRUE(trader.checkOrderLimits("SHFE.ag.2512"));
        for (uint32_t i = 0; i < 3; ++i)
        {
            ASSERT_NE(UINT_MAX, trader.openLong("SHFE.ag.2512", 7000.0, 1, 0));
            ASSERT_TRUE(sink.waitOrders(i + 1)) << "TraderMocker did not acknowledge order " << (i + 1);
        }

        OrderMap* orders = trader.getOrders("SHFE.ag.2512");
        ASSERT_NE(nullptr, orders);
        ASSERT_EQ(3u, orders->size());
        for (auto it = orders->begin(); it != orders->end(); ++it)
            EXPECT_STREQ("ag2512", static_cast<WTSOrderInfo*>(it->second)->getCode());
        orders->release();

        // API 回报携带原始代码，但公开风控查询使用标准码；该标准码必须
        // 命中三个已成功提交事件的频率缓存。
        EXPECT_FALSE(trader.checkOrderLimits("SHFE.ag.2512"));
        trader.release();
    }

    // 撤单频率也用成功 init 的 TraderMocker，并等待真实 mock 取消回报。
    {
        const uint32_t mockerId = static_cast<uint32_t>(::getpid()) * 100u + 902u;
        const fs::path root = fs::path(r90::temp_root()) / ("freq_cancel_" + std::to_string(mockerId));
        ScopedR90MockerEnvironment mockerEnv(root);
        ASSERT_TRUE(mockerEnv.ok()) << mockerEnv.error();

        std::unique_ptr<WTSVariant, R90VariantRelease> cfg(
            loadCfg("r90_cancel_freq.json", activeMockerRiskConfig(mockerId, 9, 1)));
        ASSERT_NE(nullptr, cfg.get());

        R90RiskMockerSink sink;
        TraderAdapter trader;
        ASSERT_TRUE(trader.init(id_.c_str(), cfg.get(), &bd_, NULL));
        cfg.reset();
        trader.addSink(&sink);
        ASSERT_TRUE(trader.run());
        ASSERT_TRUE(sink.waitReady()) << "TraderMocker did not become ready";

        ASSERT_TRUE(trader.checkCancelLimits("SHFE.ag.2512"));
        std::vector<uint32_t> localIds;
        for (uint32_t i = 0; i < 3; ++i)
        {
            const uint32_t localId = trader.openLong("SHFE.ag.2512", 7000.0, 1, 0);
            ASSERT_NE(UINT_MAX, localId);
            localIds.push_back(localId);
            ASSERT_TRUE(sink.waitOrders(i + 1)) << "TraderMocker did not acknowledge order " << (i + 1);
        }

        OrderMap* orders = trader.getOrders("SHFE.ag.2512");
        ASSERT_NE(nullptr, orders);
        ASSERT_EQ(3u, orders->size());
        for (auto it = orders->begin(); it != orders->end(); ++it)
            EXPECT_STREQ("ag2512", static_cast<WTSOrderInfo*>(it->second)->getCode());
        orders->release();

        for (uint32_t localId : localIds)
            ASSERT_TRUE(trader.cancel(localId));
        ASSERT_TRUE(sink.waitCancels(3)) << "TraderMocker did not acknowledge the cancellations";

        // 三次 mock 撤单成功回报带原始合约代码；公开标准码查询应命中频率缓存。
        EXPECT_FALSE(trader.checkCancelLimits("SHFE.ag.2512"));
        trader.release();
    }
}

//////////////////////////////////////////////////////////////////
// init()：合法风控配置下 savedata 建表、模块可加载但无 createTrader、
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
    fs::copy_file(fs::path(binDir) / "libWtMsgQue.dylib",
        fs::path(instRoot) / "traders" / "libWtMsgQue.dylib",
        fs::copy_options::overwrite_existing, ec);
    if (ec)
        GTEST_SKIP() << "cannot copy module into traders dir: " << ec.message();
    fs::copy_file(fs::path(binDir) / "libWtMsgQue.dylib",
        fs::path(instRoot) / "libWtMsgQue.dylib",
        fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();

    std::string oldInst = WtHelper::getInstDir();
    WtHelper::setInstDir(instRoot.c_str());

    WTSVariant* cfg = loadCfg("r90_module_missing.json",
        "{\"savedata\":true,\"ignore_selfmatch\":true,"
        "\"riskmon\":{\"active\":true,\"policy\":{"
        "\"default\":{\"order_times_boundary\":9,\"order_stat_timespan\":60,"
        "\"order_total_limits\":9,\"cancel_times_boundary\":9,"
        "\"cancel_stat_timespan\":60,\"cancel_total_limits\":9},"
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
        std::error_code badRootEc;
        fs::create_directories(fs::path(badRoot), badRootEc);
        ASSERT_FALSE(badRootEc) << badRootEc.message();
        ASSERT_TRUE(fs::is_directory(fs::path(badRoot)));
        ASSERT_FALSE(fs::is_symlink(fs::path(badRoot)));
        fs::copy_file(fs::path(binDir) / "libWtDataStorage.dylib",
            fs::path(badRoot) / "libWtMsgQue.dylib",
            fs::copy_options::overwrite_existing, ec);
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
