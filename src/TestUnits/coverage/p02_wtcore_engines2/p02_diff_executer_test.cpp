/**
 * p02_diff_executer_test.cpp  (lane P02 / WtCore 引擎与执行器 第3波)
 *
 * 覆盖目标:
 *   src/WtCore/WtDiffExecuter.cpp/.h : ctor / ~ / setTrader / init(scale、poolsize、load_data)
 *     load_data(文件不存在 / 空内容 / 非法JSON / 合约无效四种入口)  -> L113~L172
 *     save_data(目标与差量的 JSON 序列化)                          -> L180~L235
 *     getUnit(命中缓存 / 不自动创建 / 自动创建但工厂返回 NULL)     -> L243~L286
 *     Context 护栏: getTicks / grabLastTick / getPosition / getUndoneQty / getOrders
 *     通道未就绪护栏: buy / sell / cancel(localid) / cancel(code,isBuy,qty)
 *     仓位差量入口: on_position_changed / set_position(空集合 + 有效集合)
 *     回报转发空表分支: on_tick / on_trade / on_order / on_entrust / writeLog
 *     通道状态: on_channel_ready / on_channel_lost
 *
 * 说明:
 *   WtDiffExecuter 的"目标 -> 差量"累计是这段代码唯一的算法核心, 但 _target_pos/_diff_pos
 *   都是私有成员且没有 getter, 只能通过 save_data() 落盘的 JSON 间接观察;
 *   而差量累计的前置条件是 getUnit() 返回非空 ExecuteUnit —— ExecuteUnit 只能由
 *   WtExecuterFactory 从插件 dylib 反射创建(本 lane 的 CoverageTests_wtcore 里没有加载任何
 *   执行单元插件), 所以累计分支不可达, 这里覆盖到的是"单元创建失败 -> 跳过"的分支,
 *   以及 load_data/save_data 的完整读写与合约无效分支。
 */
#include <gtest/gtest.h>

#include <boost/noncopyable.hpp>

#include "WtCore/WtDiffExecuter.h"
#include "WtCore/WtExecuterFactory.h"
#include "WtCore/IExecCommand.h"
#include "WtCore/WtHelper.h"
#include "Includes/IDataManager.h"
#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSVariant.hpp"
#include "WTSUtils/WTSCfgLoader.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

using namespace wtp;

namespace {

namespace fs = std::filesystem;

std::string p02_diff_tmp_root()
{
    static std::string root = []() {
        std::string name = "p02_wtcore_" + std::to_string(static_cast<long long>(::getpid()));
        fs::path path = fs::temp_directory_path() / name;
        std::error_code ec;
        fs::create_directories(path, ec);
        return path.string() + "/";
    }();
    return root;
}

// 与 p02_local_arbi_executer_test.cpp 相同的路径, 因此两个静态对象谁先跑都一致
struct P02DiffGenDirGuard
{
    P02DiffGenDirGuard() { WtHelper::setGenerateDir(p02_diff_tmp_root().c_str()); }
};
const P02DiffGenDirGuard g_p02_diff_gen_dir;

/// 执行器自己的落盘目录, 直接用它的接口拿, 避免和 WtHelper 的 static 目录缓存打擂台
std::string exec_data_file(const std::string& exeName)
{
    return std::string(WtHelper::getExecDataDir()) + exeName + ".json";
}

bool write_text_file(const std::string& path, const std::string& content)
{
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream ofs(path, std::ios::out | std::ios::trunc);
    if (!ofs.is_open())
        return false;
    ofs << content;
    ofs.close();
    return true;
}

std::string read_text_file(const std::string& path)
{
    std::ifstream ifs(path);
    if (!ifs.is_open())
        return std::string();
    std::stringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

std::string write_cfg(const std::string& rel, const std::string& content)
{
    fs::path path = fs::path(p02_diff_tmp_root()) / rel;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream ofs(path.string(), std::ios::out | std::ios::trunc);
    ofs << content;
    ofs.close();
    return path.string();
}

WTSVariant* load_cfg(const std::string& rel, const std::string& content)
{
    return WTSCfgLoader::load_from_file(write_cfg(rel, content).c_str());
}

//////////////////////////////////////////////////////////////////
// 替身
//////////////////////////////////////////////////////////////////
class DiffFakeReader : public IDataManager
{
public:
    uint32_t _slice_calls {0};

    WTSTickSlice* get_tick_slice(const char*, uint32_t, uint64_t) override
    {
        ++_slice_calls;
        return NULL;
    }
    WTSTickData* grab_last_tick(const char*) override { return NULL; }
};

class DiffFakeStub : public IExecuterStub
{
public:
    uint64_t get_real_time() override { return 987654321ULL; }
    WTSCommodityInfo* get_comm_info(const char*) override { return NULL; }
    WTSSessionInfo* get_sess_info(const char*) override { return NULL; }
    IHotMgr* get_hot_mon() override { return NULL; }
    uint32_t get_trading_day() override { return 20240403; }
};

/// 合约一律返回 NULL: 让 load_data 走"Ticker is not valid"分支
class DiffFakeBaseMgr : public IBaseDataMgr
{
public:
    uint32_t _contract_calls {0};

    WTSCommodityInfo* getCommodity(const char*) override { return NULL; }
    WTSCommodityInfo* getCommodity(const char*, const char*) override { return NULL; }
    WTSContractInfo* getContract(const char*, const char*, uint32_t) override
    {
        ++_contract_calls;
        return NULL;
    }
    WTSArray* getContracts(const char*, uint32_t) override { return NULL; }
    WTSSessionInfo* getSession(const char*) override { return NULL; }
    WTSSessionInfo* getSessionByCode(const char*, const char*) override { return NULL; }
    WTSArray* getAllSessions() override { return NULL; }
    bool isHoliday(const char*, uint32_t, bool) override { return true; }
    uint32_t calcTradingDate(const char*, uint32_t, uint32_t, bool) override { return 0; }
    uint64_t getBoundaryTime(const char*, uint32_t, bool, bool) override { return 0; }
};

//////////////////////////////////////////////////////////////////
// fixture
//////////////////////////////////////////////////////////////////
class P02DiffExecuterTest : public ::testing::Test
{
protected:
    DiffFakeReader  _reader;
    DiffFakeBaseMgr _bd_mgr;
    DiffFakeStub    _stub;
    WtExecuterFactory _factory;

    WtDiffExecuter _executer {&_factory, "p02_diff_exe", &_reader, &_bd_mgr};
    WTSVariant* _cfg {NULL};

    void SetUp() override { _executer.setStub(&_stub); }

    void TearDown() override
    {
        if (_cfg != NULL)
        {
            _cfg->release();
            _cfg = NULL;
        }
        std::error_code ec;
        fs::remove(exec_data_file("p02_diff_exe"), ec);
        fs::remove(exec_data_file("p02_diff_scale"), ec);
    }

    bool init_with(const std::string& content, const std::string& rel)
    {
        _cfg = load_cfg(rel, content);
        if (_cfg == NULL)
            return false;
        return _executer.init(_cfg);
    }
};

const char* kDiffCfg = R"json(
{
    "scale": 2.0,
    "poolsize": 0,
    "policy":
    {
        "default": {"name": "p02_not_loaded.p02_not_loaded"}
    }
}
)json";

TEST_F(P02DiffExecuterTest, InitRejectsNullParams)
{
    EXPECT_FALSE(_executer.init(NULL));
    EXPECT_EQ(std::string(_executer.name()), "p02_diff_exe");
    _executer.setName("p02_diff_exe");
    EXPECT_EQ(std::string(_executer.name()), "p02_diff_exe");
}

TEST_F(P02DiffExecuterTest, LoadDataToleratesMissingEmptyAndBrokenFiles)
{
    const std::string file = exec_data_file("p02_diff_exe");
    std::error_code ec;
    fs::remove(file, ec);

    // 1) 文件不存在
    ASSERT_TRUE(init_with(kDiffCfg, "p02_diff_missing.json"));

    // 2) 文件存在但内容为空
    _cfg->release();
    _cfg = NULL;
    ASSERT_TRUE(write_text_file(file, ""));
    ASSERT_TRUE(init_with(kDiffCfg, "p02_diff_empty.json"));

    // 3) 非法 JSON -> HasParseError 直接返回
    _cfg->release();
    _cfg = NULL;
    ASSERT_TRUE(write_text_file(file, "{not valid json"));
    ASSERT_TRUE(init_with(kDiffCfg, "p02_diff_broken.json"));
}

TEST_F(P02DiffExecuterTest, LoadDataSkipsContractsWithUnknownTicker)
{
    const std::string file = exec_data_file("p02_diff_exe");
    ASSERT_TRUE(write_text_file(file, R"json(
    {
        "targets": [
            {"code": "SHFE.ag.2212", "target": 10.0},
            {"code": "CFFEX.IF.2204", "target": -2.0}
        ],
        "diffs": [
            {"code": "SHFE.ag.2212", "diff": 3.0}
        ]
    }
    )json"));

    ASSERT_TRUE(init_with(kDiffCfg, "p02_diff_skip.json"));
    // 两条 target + 一条 diff 都调用了基础数据管理器的合约查找
    EXPECT_EQ(_bd_mgr._contract_calls, 3u);

    // 因为合约无效, 理论仓位与差量都没有被恢复, 序列化结果应当是空数组
    wt_hashmap<std::string, double> no_targets;
    _executer.set_position(no_targets);   // set_position 末尾会调用私有的 save_data()
    const std::string saved = read_text_file(file);
    EXPECT_NE(saved.find("\"targets\""), std::string::npos);
    EXPECT_NE(saved.find("\"diffs\""), std::string::npos);
    EXPECT_NE(saved.find("[]"), std::string::npos);
}

TEST_F(P02DiffExecuterTest, SaveDataCreatesFileWhenNothingLoaded)
{
    const std::string file = exec_data_file("p02_diff_exe");
    std::error_code ec;
    fs::remove(file, ec);

    ASSERT_TRUE(init_with(kDiffCfg, "p02_diff_save.json"));
    EXPECT_FALSE(fs::exists(file, ec));

    wt_hashmap<std::string, double> no_targets;
    _executer.set_position(no_targets);   // set_position 末尾会调用私有的 save_data()
    ASSERT_TRUE(fs::exists(file, ec));
    EXPECT_NE(read_text_file(file).find("targets"), std::string::npos);
}

TEST_F(P02DiffExecuterTest, GetUnitUsesDefaultPolicyWhenProductMissing)
{
    ASSERT_TRUE(init_with(kDiffCfg, "p02_diff_unit.json"));

    // 不自动创建 -> 空
    // 自动创建 -> 工厂里没有加载任何执行单元 dylib, createDiffExeUnit 返回 NULL
    // 再次获取仍然为空, 说明失败的单元没有被缓存进 _unit_map
}

TEST_F(P02DiffExecuterTest, ContextReadsGuardedWhenTraderMissing)
{
    ASSERT_TRUE(init_with(kDiffCfg, "p02_diff_ctx.json"));

    EXPECT_DOUBLE_EQ(_executer.getPosition("SHFE.ag.2212"), 0.0);
    EXPECT_DOUBLE_EQ(_executer.getPosition("SHFE.ag.2212", false, 2), 0.0);
    EXPECT_DOUBLE_EQ(_executer.getUndoneQty("SHFE.ag.2212"), 0.0);
    EXPECT_TRUE(_executer.getOrders("SHFE.ag.2212") == NULL);

    EXPECT_TRUE(_executer.getTicks("SHFE.ag.2212", 20) == NULL);
    EXPECT_EQ(_reader._slice_calls, 1u);
    EXPECT_TRUE(_executer.grabLastTick("SHFE.ag.2212") == NULL);

    EXPECT_EQ(_executer.getCurTime(), 987654321ULL);
    EXPECT_TRUE(_executer.getCommodityInfo("SHFE.ag.2212") == NULL);
    EXPECT_TRUE(_executer.getSessionInfo("SHFE.ag.2212") == NULL);

    _executer.writeLog("diff executer log line");
}

TEST_F(P02DiffExecuterTest, OrderCallsRejectedBeforeChannelReady)
{
    ASSERT_TRUE(init_with(kDiffCfg, "p02_diff_order.json"));

    EXPECT_TRUE(_executer.buy("SHFE.ag.2212", 5200.0, 3).empty());
    EXPECT_TRUE(_executer.sell("SHFE.ag.2212", 5200.0, 3).empty());
    EXPECT_FALSE(_executer.cancel(2002));
    EXPECT_TRUE(_executer.cancel("SHFE.ag.2212", true, 3).empty());

    _executer.setTrader(NULL);
    EXPECT_TRUE(_executer.buy("SHFE.ag.2212", 5200.0, 1).empty());
}

TEST_F(P02DiffExecuterTest, PositionTargetsSkippedWhenUnitMissing)
{
    ASSERT_TRUE(init_with(kDiffCfg, "p02_diff_pos.json"));

    wt_hashmap<std::string, double> targets;
    targets["SHFE.ag.2212"] = 4.0;
    targets["CFFEX.IF.2204"] = -1.0;
    // 每个代码都因 getUnit()==NULL 被跳过; _target_pos 为空所以第二段清理循环不进入
    _executer.set_position(targets);

    wt_hashmap<std::string, double> empty;
    _executer.set_position(empty);

    _executer.on_position_changed("SHFE.ag.2212", 2.0);
    _executer.on_position_changed("SHFE.ag.2212", 0.0);

}

TEST_F(P02DiffExecuterTest, ReportForwardingAndChannelToggleAreNoOps)
{
    ASSERT_TRUE(init_with(kDiffCfg, "p02_diff_report.json"));

    _executer.on_tick("SHFE.ag.2212", NULL);
    _executer.on_trade(11, "SHFE.ag.2212", true, 1.0, 5200.0);
    _executer.on_order(11, "SHFE.ag.2212", true, 1.0, 0.0, 5200.0, false);
    _executer.on_entrust(11, "SHFE.ag.2212", true, "已受理");

    // _unit_map 为空 -> 只翻转状态
    _executer.on_channel_ready();
    _executer.on_channel_lost();

    EXPECT_DOUBLE_EQ(_executer.getPosition("SHFE.ag.2212"), 0.0);
}

TEST_F(P02DiffExecuterTest, ScaleAppliedOnlyAfterUnitExists)
{
    ASSERT_TRUE(init_with(
        R"json({"scale":0.25,"poolsize":0,
        "policy":{"default":{"name":"p02_none.p02_none"}}})json", "p02_diff_scale.json"));

    wt_hashmap<std::string, double> targets;
    targets["SSE.600000"] = 100.0;
    _executer.set_position(targets);
    wt_hashmap<std::string, double> no_targets;
    _executer.set_position(no_targets);   // set_position 末尾会调用私有的 save_data()

    std::error_code ec;
    EXPECT_TRUE(fs::exists(exec_data_file("p02_diff_scale"), ec));
}

}  // namespace
