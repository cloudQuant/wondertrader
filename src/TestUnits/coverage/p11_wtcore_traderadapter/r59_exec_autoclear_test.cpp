/**
 * r59_exec_autoclear_test.cpp — WtLocalExecuter / WtArbiExecuter / WtDiffExecuter
 * 在「执行单元无法创建（无插件 dylib）」前提下仍可达的分支
 *
 *   - Local/Arbi::on_position 的过期主力自动清理链（:917-993 / :821-889）：
 *     非分月早退、hot mgr 返回的上一期主力与当前不一致、排除列表命中、
 *     包含列表未命中、包含列表命中后 getUnit()==NULL 跳过 clear_all_position。
 *     用自造 IHotMgr 提供可控的 getPrevRawCode，不联网、不读墙钟。
 *   - 三个执行器 data_mgr==NULL 时的 getTicks/grabLastTick 护栏，以及
 *     poolsize>0 时线程池创建与析构 wait（:237-263, :49-53 等）。
 *   - WtDiffExecuter::load_data 的有效合约分支 + set_position 末尾的过期目标
 *     清理循环 + on_channel_ready 的差量恢复循环（:113-172, :597-639, :795-817）。
 *   - setTrader(已就绪通道) 后 buy/sell/cancel 真的转发到 TraderAdapter，
 *     再 on_channel_lost 后重新拒单。
 *
 * 普通清理与空数据用例不加载插件；过期目标测试会像 r31 一样把构建产物
 * libWtExeFact 临时复制后加载，以确保清理断言经过真实 diff 执行单元。
 */
#include "r59_trader_fixture.h"

#include <WtCore/WtLocalExecuter.h>
#include <WtCore/WtArbiExecuter.h>
#include <WtCore/WtDiffExecuter.h>
#include <WtCore/WtExecuterFactory.h>
#include <WtCore/WtHelper.h>
#include <Includes/IDataManager.h>
#include <Includes/IHotMgr.h>

#include <filesystem>
#include <iterator>
#include <cstdlib>

using namespace r59x;
using wtp::ActionPolicyMgr;
using wtp::TraderAdapter;
using wtp::WtArbiExecuter;
using wtp::WtDiffExecuter;
using wtp::WtLocalExecuter;

namespace {

namespace fs = std::filesystem;

#if defined(_WIN32)
const char* const kExeFactLib = "WtExeFact.dll";
#elif defined(__APPLE__)
const char* const kExeFactLib = "libWtExeFact.dylib";
#else
const char* const kExeFactLib = "libWtExeFact.so";
#endif

const fs::path& exeFactBinDir()
{
    static fs::path cached;
    if (!cached.empty())
        return cached;

    if (const char* env = std::getenv("WT_COV_PLUGIN_BIN"))
    {
        fs::path candidate(env);
        std::error_code ec;
        if (fs::is_regular_file(candidate / kExeFactLib, ec))
        {
            cached = candidate;
            return cached;
        }
    }

    fs::path up = fs::absolute(fs::path(__FILE__)).parent_path();
    for (int i = 0; i < 10 && !up.empty(); ++i)
    {
        for (const char* config : { "Debug", "Release" })
        {
            fs::path candidate = up / "build-cov" / "build_x64" / config / "bin";
            std::error_code ec;
            if (fs::is_regular_file(candidate / kExeFactLib, ec))
            {
                cached = candidate;
                return cached;
            }
        }
        up = up.parent_path();
    }
    return cached;
}

class R59Reader : public wtp::IDataManager
{
public:
    uint32_t slices_ = 0;

    virtual WTSTickSlice* get_tick_slice(const char*, uint32_t, uint64_t) override
    {
        ++slices_;
        return nullptr;
    }
    virtual WTSTickData* grab_last_tick(const char*) override { return nullptr; }
};

/// 可控主力合约管理器：只让 getPrevRawCode 返回预设值
class R59HotMgr : public wtp::IHotMgr
{
public:
    std::string prev_ = "ag2512";
    uint32_t n_prev_ = 0;

    virtual const char* getRawCode(const char*, const char*, uint32_t) override { return ""; }
    virtual const char* getPrevRawCode(const char*, const char*, uint32_t) override
    {
        ++n_prev_;
        return prev_.c_str();
    }
    virtual bool isHot(const char*, const char*, uint32_t) override { return false; }
    virtual bool splitHotSecions(const char*, const char*, uint32_t, uint32_t, HotSections&) override
    {
        return false;
    }
    virtual const char* getSecondRawCode(const char*, const char*, uint32_t) override { return ""; }
    virtual const char* getPrevSecondRawCode(const char*, const char*, uint32_t) override { return ""; }
    virtual bool isSecond(const char*, const char*, uint32_t) override { return false; }
    virtual bool splitSecondSecions(const char*, const char*, uint32_t, uint32_t, HotSections&) override
    {
        return false;
    }
    virtual const char* getCustomRawCode(const char*, const char*, uint32_t) override { return ""; }
    virtual const char* getPrevCustomRawCode(const char*, const char*, uint32_t) override { return ""; }
    virtual bool isCustomHot(const char*, const char*, uint32_t) override { return false; }
    virtual bool splitCustomSections(const char*, const char*, uint32_t, uint32_t, HotSections&) override
    {
        return false;
    }
    virtual const char* getRuleTag(const char*) override { return ""; }
    virtual double getRuleFactor(const char*, const char*, uint32_t) override { return 1.0; }
};

class R59Stub : public wtp::IExecuterStub
{
public:
    wtp::IHotMgr* hot_ = nullptr;

    virtual uint64_t get_real_time() override { return 20240510100000000ULL; }
    virtual WTSCommodityInfo* get_comm_info(const char*) override { return nullptr; }
    virtual WTSSessionInfo* get_sess_info(const char*) override { return nullptr; }
    virtual wtp::IHotMgr* get_hot_mon() override { return hot_; }
    virtual uint32_t get_trading_day() override { return 20240510; }
};

/// load_data 与 set_position 都能查到的合约表：标准码/原始码都映射到同一张合约
class R59DiffBd : public p11fix::FakeBaseDataMgr
{
public:
    WTSContractInfo* any_ = nullptr;
    std::vector<std::string> lookups_;

    virtual WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t uDate = 0) override
    {
        std::string key = code == nullptr ? "" : code;
        key += "|";
        key += exchg == nullptr ? "" : exchg;
        lookups_.push_back(key);
        if (any_ != nullptr && code != nullptr &&
            (strcmp(code, "SHFE.ag.2512") == 0 || strcmp(code, "ag2512") == 0))
            return any_;
        return p11fix::FakeBaseDataMgr::getContract(code, exchg, uDate);
    }
};

/// 严格匹配基础库公开约定：仅 (raw code, exchange) 能解析到合约。
class R59RawCodeDiffBd : public p11fix::FakeBaseDataMgr
{
public:
    WTSContractInfo* raw_contract_ = nullptr;
    std::vector<std::string> lookups_;

    virtual WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t uDate = 0) override
    {
        std::string key = code == nullptr ? "" : code;
        key += "|";
        key += exchg == nullptr ? "" : exchg;
        lookups_.push_back(key);
        if (raw_contract_ != nullptr && code != nullptr && strcmp(code, "ag2512") == 0 &&
            exchg != nullptr && strcmp(exchg, "SHFE") == 0)
            return raw_contract_;
        return p11fix::FakeBaseDataMgr::getContract(code, exchg, uDate);
    }
};

class R59Exec : public R59TraderTest
{
protected:
    wtp::WtExecuterFactory factory_;
    R59Reader reader_;
    R59HotMgr hot_;
    R59Stub stub_;

    void SetUp() override
    {
        R59TraderTest::SetUp();
        stub_.hot_ = &hot_;
    }

    wtp::WTSVariant* writeCfg(const char* name, const char* json)
    {
        auto path = tmp_ / name;
        std::ofstream out(path);
        out << json;
        out.close();
        return WTSCfgLoader::load_from_file(path.string().c_str());
    }

    std::string execFile(const char* name)
    {
        return std::string(WtHelper::getExecDataDir()) + name + ".json";
    }

    void TearDown() override
    {
        std::error_code ec;
        for (const char* n : { "r59_diff_load", "r59_diff_fwd", "r59_diff_null",
                "r59_diff_stale", "r59_diff_stale_strict" })
            std::filesystem::remove(execFile(n), ec);
        R59TraderTest::TearDown();
    }
};

const char* kLocalClearCfg = R"json(
{
    "scale": 1.0, "poolsize": 0,
    "clear": {"active": true, "includes": ["SHFE.ag"], "excludes": ["DCE.i"]},
    "policy": {"default": {"name": "r59.none"}}
}
)json";

const char* kArbiClearCfg = R"json(
{
    "scale": 1.0, "poolsize": 0,
    "clear": {"active": true, "includes": ["SHFE.ag"], "excludes": ["DCE.i"]},
    "policy": {"default": {"name": "r59.none"}}
}
)json";

const char* kPlainCfg = R"json(
{
    "scale": 1.0, "poolsize": 0,
    "policy": {"default": {"name": "r59.none"}}
}
)json";

const char* kDiffUnitCfg = R"json(
{
    "scale": 1.0, "poolsize": 0,
    "policy": {"default": {
        "name": "WtExeFact.WtDiffMinImpactExeUnit",
        "offset": 0, "expire": 0, "pricemode": 0, "span": 0,
        "byrate": false, "lots": 1.0, "rate": 0.0
    }}
}
)json";

// WtLocalExecuter.cpp:917-993（on_position 自动清理：hot 命中/不一致/排除/包含未命中）
TEST_F(R59Exec, local_auto_clear_checks_hot_code_and_filters)
{
    WtLocalExecuter ex(&factory_, "r59_local_ac", &reader_);
    ex.setStub(&stub_);
    wtp::WTSVariant* cfg = writeCfg("r59_local_ac.json", kLocalClearCfg);
    ASSERT_NE(nullptr, cfg);
    ASSERT_TRUE(ex.init(cfg));
    cfg->release();

    // 上一期主力一致 + 包含列表命中 -> 走到 clear_all_position 入口（unit 为空不崩）
    hot_.prev_ = "ag2512";
    ex.on_position("SHFE.ag.2512", true, 1, 1, 1, 1, 20240510);
    EXPECT_EQ(1u, hot_.n_prev_);

    // 上一期主力不一致 -> 提前返回
    hot_.prev_ = "ag2601";
    ex.on_position("SHFE.ag.2512", true, 1, 1, 1, 1, 20240510);
    EXPECT_EQ(2u, hot_.n_prev_);

    // 排除列表命中 -> 返回
    WtLocalExecuter excl(&factory_, "r59_local_ex", &reader_);
    excl.setStub(&stub_);
    wtp::WTSVariant* cfg2 = writeCfg("r59_local_ex.json", R"json(
    {"scale":1,"poolsize":0,
     "clear":{"active":true,"includes":[],"excludes":["SHFE.ag"]},
     "policy":{"default":{"name":"r59.none"}}})json");
    ASSERT_NE(nullptr, cfg2);
    ASSERT_TRUE(excl.init(cfg2));
    cfg2->release();
    hot_.prev_ = "ag2512";
    excl.on_position("SHFE.ag.2512", true, 1, 1, 1, 1, 20240510);
    EXPECT_EQ(3u, hot_.n_prev_);

    // 包含列表非空但未命中 -> 返回
    WtLocalExecuter miss(&factory_, "r59_local_miss", &reader_);
    miss.setStub(&stub_);
    wtp::WTSVariant* cfg3 = writeCfg("r59_local_miss.json", R"json(
    {"scale":1,"poolsize":0,
     "clear":{"active":true,"includes":["CFFEX.IF"],"excludes":[]},
     "policy":{"default":{"name":"r59.none"}}})json");
    ASSERT_NE(nullptr, cfg3);
    ASSERT_TRUE(miss.init(cfg3));
    cfg3->release();
    miss.on_position("SHFE.ag.2512", true, 1, 1, 1, 1, 20240510);
    EXPECT_EQ(4u, hot_.n_prev_);
}

// WtArbiExecuter.cpp:821-889（与 Local 同构的自动清理链）
TEST_F(R59Exec, arbi_auto_clear_checks_hot_code_and_filters)
{
    WtArbiExecuter ex(&factory_, "r59_arbi_ac", &reader_);
    ex.setStub(&stub_);
    wtp::WTSVariant* cfg = writeCfg("r59_arbi_ac.json", kArbiClearCfg);
    ASSERT_NE(nullptr, cfg);
    ASSERT_TRUE(ex.init(cfg));
    cfg->release();

    hot_.prev_ = "ag2512";
    ex.on_position("SHFE.ag.2512", true, 1, 1, 1, 1, 20240510);
    EXPECT_EQ(1u, hot_.n_prev_);

    hot_.prev_ = "ag2601";
    ex.on_position("SHFE.ag.2512", true, 1, 1, 1, 1, 20240510);
    EXPECT_EQ(2u, hot_.n_prev_);

    WtArbiExecuter excl(&factory_, "r59_arbi_ex", &reader_);
    excl.setStub(&stub_);
    wtp::WTSVariant* cfg2 = writeCfg("r59_arbi_ex.json", R"json(
    {"scale":1,"poolsize":0,
     "clear":{"active":true,"includes":[],"excludes":["SHFE.ag"]},
     "policy":{"default":{"name":"r59.none"}}})json");
    ASSERT_NE(nullptr, cfg2);
    ASSERT_TRUE(excl.init(cfg2));
    cfg2->release();
    excl.on_position("SHFE.ag.2512", true, 1, 1, 1, 1, 20240510);
    EXPECT_EQ(3u, hot_.n_prev_);

    WtArbiExecuter miss(&factory_, "r59_arbi_miss", &reader_);
    miss.setStub(&stub_);
    wtp::WTSVariant* cfg3 = writeCfg("r59_arbi_miss.json", R"json(
    {"scale":1,"poolsize":0,
     "clear":{"active":true,"includes":["CFFEX.IF"],"excludes":[]},
     "policy":{"default":{"name":"r59.none"}}})json");
    ASSERT_NE(nullptr, cfg3);
    ASSERT_TRUE(miss.init(cfg3));
    cfg3->release();
    miss.on_position("SHFE.ag.2512", true, 1, 1, 1, 1, 20240510);
    EXPECT_EQ(4u, hot_.n_prev_);
}

// WtLocalExecuter.cpp:237-263/49-53, WtArbiExecuter.cpp:217-237/49-53, WtDiffExecuter.cpp:304-324/57-61
TEST_F(R59Exec, null_data_manager_and_pool_lifecycle)
{
    wtp::WTSVariant* cfg1 = writeCfg("r59_pool.json",
        R"json({"scale":1,"poolsize":2,"policy":{"default":{"name":"r59.none"}}})json");
    ASSERT_NE(nullptr, cfg1);
    WtLocalExecuter local(&factory_, "r59_local_pool", nullptr);
    local.setStub(&stub_);
    ASSERT_TRUE(local.init(cfg1));
    cfg1->release();
    EXPECT_EQ(nullptr, local.getTicks("SHFE.ag.2512", 5));
    EXPECT_EQ(nullptr, local.grabLastTick("SHFE.ag.2512"));

    wtp::WTSVariant* cfg2 = writeCfg("r59_pool2.json",
        R"json({"scale":1,"poolsize":1,"policy":{"default":{"name":"r59.none"}}})json");
    ASSERT_NE(nullptr, cfg2);
    WtArbiExecuter arbi(&factory_, "r59_arbi_pool", nullptr);
    arbi.setStub(&stub_);
    ASSERT_TRUE(arbi.init(cfg2));
    cfg2->release();
    EXPECT_EQ(nullptr, arbi.getTicks("SHFE.ag.2512", 5));
    EXPECT_EQ(nullptr, arbi.grabLastTick("SHFE.ag.2512"));

    wtp::WTSVariant* cfg3 = writeCfg("r59_pool3.json",
        R"json({"scale":1,"poolsize":1,"policy":{"default":{"name":"r59.none"}}})json");
    ASSERT_NE(nullptr, cfg3);
    WtDiffExecuter diff(&factory_, "r59_diff_null", nullptr, &bd_);
    diff.setStub(&stub_);
    ASSERT_TRUE(diff.init(cfg3));
    cfg3->release();
    EXPECT_EQ(nullptr, diff.getTicks("SHFE.ag.2512", 5));
    EXPECT_EQ(nullptr, diff.grabLastTick("SHFE.ag.2512"));
    // 三个对象析构时各自等线程池清空（无任务，立即返回）
}

// WtDiffExecuter.cpp:113-172（有效合约装载）, 597-639（过期目标清理）, 795-817（通道恢复差量）
TEST_F(R59Exec, diff_loads_valid_targets_and_recovers_on_channel_ready)
{
    std::string file = execFile("r59_diff_load");
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(file).parent_path(), ec);
    {
        std::ofstream out(file);
        out << R"json({
            "targets": [
                {"code": "SHFE.ag.2512", "target": 10.0},
                {"code": "CFFEX.IF.2204", "target": -2.0}
            ],
            "diffs": [
                {"code": "SHFE.ag.2512", "diff": 3.0},
                {"code": "CFFEX.IF.2204", "diff": 2.0}
            ]
        })json";
    }

    R59DiffBd bd;
    bd.any_ = ct_;
    wtp::WTSVariant* cfg = writeCfg("r59_diff_load.json", kPlainCfg);
    ASSERT_NE(nullptr, cfg);
    WtDiffExecuter ex(&factory_, "r59_diff_load", nullptr, &bd);
    ex.setStub(&stub_);
    ASSERT_TRUE(ex.init(cfg));      // load_data：ag 有效装载，IF 无效跳过
    cfg->release();

    // 空目标集合 -> 进入「旧目标不在新目标中」的清理循环：查到合约、pos!=0、
    // getUnit()==NULL 跳过 set_position，最后 save_data 落盘
    wt_hashmap<std::string, double> empty;
    ex.set_position(empty);

    // 通道就绪 -> 遍历已装载的 diff，getUnit()==NULL 跳过恢复
    ex.on_channel_ready();

    std::ifstream in(file);
    std::string saved((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(std::string::npos, saved.find("\"targets\""));
    EXPECT_NE(std::string::npos, saved.find("\"diffs\""));
    EXPECT_NE(std::string::npos, saved.find("10"));
    EXPECT_EQ(0u, reader_.slices_); // 没有调用行情接口
}

// WtLocalExecuter.cpp:60-66/328-392（setTrader 读就绪状态，buy/sell/cancel 转发）；TraderAdapter.cpp:663-762
TEST_F(R59Exec, set_trader_forwards_orders_only_while_channel_ready)
{
    ActionPolicyMgr pm;
    ASSERT_TRUE(loadPolicy(pm, "r59_exec_open.json",
        R"json({"default":{"order":[{"action":"open"}]}})json"));

    TraderAdapter trader;
    build(trader, &pm);
    makeReady(trader);

    WtLocalExecuter local(&factory_, "r59_local_fwd", &reader_);
    WtArbiExecuter arbi(&factory_, "r59_arbi_fwd", &reader_);
    WtDiffExecuter diff(&factory_, "r59_diff_fwd", nullptr, &bd_);
    local.setStub(&stub_);
    arbi.setStub(&stub_);
    diff.setStub(&stub_);

    wtp::WTSVariant* c1 = writeCfg("r59_fwd1.json", kPlainCfg);
    wtp::WTSVariant* c2 = writeCfg("r59_fwd2.json", kPlainCfg);
    wtp::WTSVariant* c3 = writeCfg("r59_fwd3.json", kPlainCfg);
    ASSERT_NE(nullptr, c1);
    ASSERT_NE(nullptr, c2);
    ASSERT_NE(nullptr, c3);
    ASSERT_TRUE(local.init(c1));
    ASSERT_TRUE(arbi.init(c2));
    ASSERT_TRUE(diff.init(c3));
    c1->release();
    c2->release();
    c3->release();

    // setTrader 读到 AS_ALLREADY -> 三个执行器的 _channel_ready 都为 true
    local.setTrader(&trader);
    arbi.setTrader(&trader);
    diff.setTrader(&trader);

    EXPECT_EQ(1u, local.buy(kStdCode, 7000.0, 1.0).size());
    EXPECT_EQ(1u, local.sell(kStdCode, 7000.0, 1.0).size());
    EXPECT_EQ(1u, arbi.buy(kStdCode, 7000.0, 1.0).size());
    EXPECT_EQ(1u, arbi.sell(kStdCode, 7000.0, 1.0).size());
    EXPECT_EQ(1u, diff.buy(kStdCode, 7000.0, 1.0).size());
    EXPECT_EQ(1u, diff.sell(kStdCode, 7000.0, 1.0).size());
    EXPECT_EQ(6u, api2_.n_insert_);

    // 撤单同样转发；无订单表时返回 false
    EXPECT_FALSE(local.cancel(424242u));
    EXPECT_TRUE(local.cancel(kStdCode, true, 0.0).empty());

    // 通道断开后重新拒单
    local.on_channel_lost();
    EXPECT_TRUE(local.buy(kStdCode, 7000.0, 1.0).empty());
    local.on_channel_ready();
    EXPECT_EQ(1u, local.buy(kStdCode, 7000.0, 1.0).size());
}

// WtDiffExecuter.cpp:113-172, 597-641（真实 diff 单元下的过期目标清理）
TEST_F(R59Exec, diff_stale_target_uses_raw_code_lookup_and_clears_with_real_unit)
{
    const fs::path& pluginBin = exeFactBinDir();
    ASSERT_FALSE(pluginBin.empty()) << "cannot locate build-cov plugin bin";
    ASSERT_TRUE(fs::is_regular_file(pluginBin / kExeFactLib)) << pluginBin.string();

    const fs::path stagedPlugins = tmp_ / "r59_plugins";
    std::error_code ec;
    fs::create_directories(stagedPlugins, ec);
    ASSERT_FALSE(ec) << ec.message();
    fs::copy_file(pluginBin / kExeFactLib, stagedPlugins / kExeFactLib,
        fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();
    ASSERT_TRUE(factory_.loadFactories(stagedPlugins.string().c_str()));

    auto unitProbe = factory_.createDiffExeUnit("WtExeFact", "WtDiffMinImpactExeUnit");
    ASSERT_NE(nullptr, unitProbe.get());
    EXPECT_STREQ("WtDiffMinImpactExeUnit", unitProbe->self()->getName());
    unitProbe.reset();

    std::string file = execFile("r59_diff_stale");
    std::filesystem::create_directories(std::filesystem::path(file).parent_path(), ec);
    ASSERT_FALSE(ec) << ec.message();
    {
        std::ofstream out(file);
        ASSERT_TRUE(out.is_open()) << file;
        out << R"json({"targets":[{"code":"SHFE.ag.2512","target":10.0}],"diffs":[]})json";
        out.close();
        ASSERT_TRUE(out.good());
    }

    // 双格式替身让真实单元清理分支可达，同时记录查询参数，单独检查应使用原始码+交易所。
    R59DiffBd bd;
    bd.any_ = ct_;
    wtp::WTSVariant* cfg = writeCfg("r59_diff_stale.json", kDiffUnitCfg);
    ASSERT_NE(nullptr, cfg);
    WtDiffExecuter ex(&factory_, "r59_diff_stale", nullptr, &bd);
    ex.setStub(&stub_);
    ASSERT_TRUE(ex.init(cfg));
    cfg->release();

    wt_hashmap<std::string, double> empty;
    ex.set_position(empty);

    ASSERT_EQ(2u, bd.lookups_.size());
    EXPECT_EQ("ag2512|SHFE", bd.lookups_[0]); // load_data 的原始码查询
    EXPECT_EQ("ag2512|SHFE", bd.lookups_[1]); // 过期清理应遵循相同查询契约

    std::ifstream in(file);
    ASSERT_TRUE(in.is_open()) << file;
    std::string saved((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(std::string::npos, saved.find("\"target\": 0"));
}

// 真实 diff 单元 + 严格基础库：只接受 (raw code, exchange)，校验清理对外可见结果。
TEST_F(R59Exec, diff_stale_target_clears_with_raw_code_only_contract_lookup)
{
    const fs::path& pluginBin = exeFactBinDir();
    ASSERT_FALSE(pluginBin.empty()) << "cannot locate build-cov plugin bin";
    ASSERT_TRUE(fs::is_regular_file(pluginBin / kExeFactLib)) << pluginBin.string();

    const fs::path stagedPlugins = tmp_ / "r59_plugins_strict";
    std::error_code ec;
    fs::create_directories(stagedPlugins, ec);
    ASSERT_FALSE(ec) << ec.message();
    fs::copy_file(pluginBin / kExeFactLib, stagedPlugins / kExeFactLib,
        fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();
    ASSERT_TRUE(factory_.loadFactories(stagedPlugins.string().c_str()));

    auto unitProbe = factory_.createDiffExeUnit("WtExeFact", "WtDiffMinImpactExeUnit");
    ASSERT_NE(nullptr, unitProbe.get());
    EXPECT_STREQ("WtDiffMinImpactExeUnit", unitProbe->self()->getName());
    unitProbe.reset();

    const std::string file = execFile("r59_diff_stale_strict");
    fs::create_directories(fs::path(file).parent_path(), ec);
    ASSERT_FALSE(ec) << ec.message();
    {
        std::ofstream out(file);
        ASSERT_TRUE(out.is_open()) << file;
        out << R"json({"targets":[{"code":"SHFE.ag.2512","target":10.0}],"diffs":[]})json";
        out.close();
        ASSERT_TRUE(out.good());
    }

    R59RawCodeDiffBd bd;
    bd.raw_contract_ = ct_;
    wtp::WTSVariant* cfg = writeCfg("r59_diff_stale_strict.json", kDiffUnitCfg);
    ASSERT_NE(nullptr, cfg);
    WtDiffExecuter ex(&factory_, "r59_diff_stale_strict", nullptr, &bd);
    ex.setStub(&stub_);
    ASSERT_TRUE(ex.init(cfg));
    cfg->release();

    wt_hashmap<std::string, double> empty;
    ex.set_position(empty);

    ASSERT_EQ(2u, bd.lookups_.size());
    EXPECT_EQ("ag2512|SHFE", bd.lookups_[0]); // load_data 以原始码和交易所装载目标
    EXPECT_EQ("ag2512|SHFE", bd.lookups_[1]); // 清理也必须使用同一公开查询形式

    std::ifstream in(file);
    ASSERT_TRUE(in.is_open()) << file;
    std::string saved((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(std::string::npos, saved.find("\"target\": 0"));
}

} // namespace
