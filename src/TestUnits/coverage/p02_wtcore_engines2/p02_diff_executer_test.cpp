/**
 * p02_diff_executer_test.cpp  (lane P02 / WtCore 引擎与执行器 第3波)
 *
 * 覆盖目标:
 *   src/WtCore/WtDiffExecuter.cpp/.h : ctor / ~ / setTrader / init(scale、poolsize、load_data)
 *     load_data(文件缺失 / 新旧快照格式 / 损坏结构拒绝 / 合约无效)   -> L113~L172
 *     save_data(快照写回、失败保留 last-good、重新加载往返)          -> L180~L235
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
#include "p02_test_environment.h"

#include <boost/noncopyable.hpp>

#include "WtCore/WtDiffExecuter.h"
#include "WtCore/WtExecuterFactory.h"
#include "WtCore/IExecCommand.h"
#include "WtCore/WtHelper.h"
#include "Includes/IDataManager.h"
#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSVariant.hpp"
#include "WTSUtils/WTSCfgLoader.h"
#include <rapidjson/document.h>

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

using namespace wtp;

namespace wtp
{
class P02DiffExecuterTestAccess
{
public:
	static bool load(WtDiffExecuter& executor) { return executor.load_data(); }
	static bool save(WtDiffExecuter& executor) { return executor.save_data(); }

	static void setSnapshot(WtDiffExecuter& executor, const std::string& code,
		double target, double diff)
	{
		std::lock_guard<std::mutex> lock(executor._position_mutex);
		executor._target_pos.clear();
		executor._diff_pos.clear();
		executor._target_versions.clear();
		executor._target_pos[code] = target;
		executor._diff_pos[code] = diff;
		executor._target_versions[code] = ++executor._target_update_generation;
		++executor._position_revision;
	}

	static double target(const WtDiffExecuter& executor, const std::string& code)
	{
		std::lock_guard<std::mutex> lock(executor._position_mutex);
		auto it = executor._target_pos.find(code);
		return it == executor._target_pos.end() ? -999999.0 : it->second;
	}

	static double diff(const WtDiffExecuter& executor, const std::string& code)
	{
		std::lock_guard<std::mutex> lock(executor._position_mutex);
		auto it = executor._diff_pos.find(code);
		return it == executor._diff_pos.end() ? -999999.0 : it->second;
	}

	static uint64_t savedRevision(WtDiffExecuter& executor)
	{
		std::lock_guard<std::mutex> lock(executor._save_mutex);
		return executor._saved_position_revision;
	}

	static uint64_t positionRevision(const WtDiffExecuter& executor)
	{
		std::lock_guard<std::mutex> lock(executor._position_mutex);
		return executor._position_revision;
	}

	static std::size_t partialWrite(void* output, const void* data, std::size_t size)
	{
		return std::fwrite(data, 1, size / 2, static_cast<std::FILE*>(output));
	}

	static int flushAndFail(void* output)
	{
		std::fflush(static_cast<std::FILE*>(output));
		return EOF;
	}

	static int closeAndFail(void* output)
	{
		std::fclose(static_cast<std::FILE*>(output));
		return EOF;
	}

	static void failPartialWrite()
	{
		WtDiffExecuter::_save_write_test_hook = &partialWrite;
	}

	static void failFlush()
	{
		WtDiffExecuter::_save_flush_test_hook = &flushAndFail;
	}

	static void failClose()
	{
		WtDiffExecuter::_save_close_test_hook = &closeAndFail;
	}

	static void clearSaveIoHooks()
	{
		WtDiffExecuter::_save_write_test_hook = NULL;
		WtDiffExecuter::_save_flush_test_hook = NULL;
		WtDiffExecuter::_save_close_test_hook = NULL;
	}
};
}

namespace {

::testing::Environment* const g_p02_generate_dir_environment =
	::testing::AddGlobalTestEnvironment(new p02test::GenerateDirEnvironment);

namespace fs = std::filesystem;
namespace rj = rapidjson;

std::string p02_diff_tmp_root()
{
    return p02test::temp_root();
}

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

/// 默认返回 NULL 测试失效合约；兼容性快照用例可启用一个合法合约替身。
class DiffFakeBaseMgr : public IBaseDataMgr
{
public:
    uint32_t _contract_calls {0};
    WTSContractInfo* _contract {NULL};
    bool _resolve_contracts {false};

    ~DiffFakeBaseMgr()
    {
        if (_contract != NULL)
            _contract->release();
    }

    void enableContract()
    {
        if (_contract == NULL)
            _contract = WTSContractInfo::create("ag2212", "ag2212", "SHFE", "ag");
        _resolve_contracts = _contract != NULL;
    }

    WTSCommodityInfo* getCommodity(const char*) override { return NULL; }
    WTSCommodityInfo* getCommodity(const char*, const char*) override { return NULL; }
    WTSContractInfo* getContract(const char*, const char*, uint32_t) override
    {
        ++_contract_calls;
        return _resolve_contracts ? _contract : NULL;
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
        P02DiffExecuterTestAccess::clearSaveIoHooks();
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

TEST_F(P02DiffExecuterTest, InitSucceedsWhenSnapshotIsMissing)
{
    const std::string file = exec_data_file("p02_diff_exe");
    std::error_code ec;
    fs::remove(file, ec);

    ASSERT_TRUE(init_with(kDiffCfg, "p02_diff_missing.json"));
}

TEST_F(P02DiffExecuterTest, InitRejectsMalformedSnapshot)
{
    ASSERT_TRUE(write_text_file(exec_data_file("p02_diff_exe"), R"json({"targets":[]})json"));
    EXPECT_FALSE(init_with(kDiffCfg, "p02_diff_bad_schema.json"));
}

TEST_F(P02DiffExecuterTest, LoadDataRejectsMalformedSnapshotsWithoutPartialRestore)
{
    const std::string code = "SHFE.ag.2212";
    const std::vector<std::string> invalidSnapshots = {
        "",
        "{not valid json",
        "[]",
        R"json({"targets":[]})json",
        R"json({"targets":[],"diffs":{}})json",
        R"json({"targets":"bad","diffs":"bad"})json",
        R"json({"targets":[{"code":"SHFE.ag.2212","target":7},{"code":3,"target":1}],"diffs":[]})json",
        R"json({"targets":[{"target":7}],"diffs":[]})json",
        R"json({"targets":[],"diffs":[{"code":"SHFE.ag.2212"}]})json",
        R"json({"targets":[],"diffs":[],"blocked_units":[{}]})json"
    };

    _bd_mgr.enableContract();
    P02DiffExecuterTestAccess::setSnapshot(_executer, code, 19.0, -4.0);
    for (const std::string& content : invalidSnapshots)
    {
        ASSERT_TRUE(write_text_file(exec_data_file("p02_diff_exe"), content));
        EXPECT_FALSE(P02DiffExecuterTestAccess::load(_executer)) << content;
        EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::target(_executer, code), 19.0) << content;
        EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::diff(_executer, code), -4.0) << content;
    }
    EXPECT_EQ(_bd_mgr._contract_calls, 0u);
}

TEST_F(P02DiffExecuterTest, LoadDataAcceptsCurrentArrayAndHistoricalObjectMap)
{
    const std::string currentCode = "CFFEX.IF.2204";
    const std::string historicalCode = "SHFE.ag.2212";
    _bd_mgr.enableContract();

    ASSERT_TRUE(write_text_file(exec_data_file("p02_diff_exe"), R"json(
    {"targets":[{"code":"CFFEX.IF.2204","target":7.5}],
     "diffs":[{"code":"CFFEX.IF.2204","diff":2.25}],
     "blocked_units":[]}
    )json"));
    ASSERT_TRUE(P02DiffExecuterTestAccess::load(_executer));
    EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::target(_executer, currentCode), 7.5);
    EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::diff(_executer, currentCode), 2.25);

    // fb9350f2 used object maps before switching to array records in 2022.
    ASSERT_TRUE(write_text_file(exec_data_file("p02_diff_exe"), R"json(
    {"targets":{"SHFE.ag.2212":-3.0},"diffs":{"SHFE.ag.2212":8.5}}
    )json"));
    ASSERT_TRUE(P02DiffExecuterTestAccess::load(_executer));
    EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::target(_executer, historicalCode), -3.0);
    EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::diff(_executer, historicalCode), 8.5);
    EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::target(_executer, currentCode), -999999.0);
}

TEST_F(P02DiffExecuterTest, PartialWriteFlushAndCloseFailuresPreserveLastGoodAndRetry)
{
    const std::string file = exec_data_file("p02_diff_exe");
    std::error_code ec;
    fs::remove(file, ec);

    P02DiffExecuterTestAccess::setSnapshot(_executer, "SHFE.ag.2212", 3.0, 1.0);
    ASSERT_TRUE(P02DiffExecuterTestAccess::save(_executer));
    const std::string lastGood = read_text_file(file);
    ASSERT_FALSE(lastGood.empty());
    uint64_t committedRevision = P02DiffExecuterTestAccess::savedRevision(_executer);
    std::string committedBytes = lastGood;
    const std::string tempPrefix = fs::path(file).filename().string() + ".tmp.";

    auto hasNoTempSnapshot = [&]() {
        for (const auto& item : fs::directory_iterator(fs::path(file).parent_path()))
        {
            const std::string name = item.path().filename().string();
            if (name.find(tempPrefix) == 0)
                return false;
        }
        return true;
    };

    auto failurePreservesLastGood = [&](void (*injectFailure)(), double target, double diff) {
        P02DiffExecuterTestAccess::setSnapshot(_executer, "SHFE.ag.2212", target, diff);
        injectFailure();
        const bool saveFailed = !P02DiffExecuterTestAccess::save(_executer);
        const bool bytesPreserved = read_text_file(file) == committedBytes;
        const bool revisionPreserved =
            P02DiffExecuterTestAccess::savedRevision(_executer) == committedRevision;
        const bool tempCleaned = hasNoTempSnapshot();
        P02DiffExecuterTestAccess::clearSaveIoHooks();
        return saveFailed && bytesPreserved && revisionPreserved && tempCleaned;
    };

    EXPECT_TRUE(failurePreservesLastGood(&P02DiffExecuterTestAccess::failPartialWrite, 9.0, 7.0));
    ASSERT_TRUE(P02DiffExecuterTestAccess::save(_executer));
    committedBytes = read_text_file(file);
    committedRevision = P02DiffExecuterTestAccess::savedRevision(_executer);

    EXPECT_TRUE(failurePreservesLastGood(&P02DiffExecuterTestAccess::failFlush, 10.0, 8.0));
    ASSERT_TRUE(P02DiffExecuterTestAccess::save(_executer));
    committedBytes = read_text_file(file);
    committedRevision = P02DiffExecuterTestAccess::savedRevision(_executer);

    EXPECT_TRUE(failurePreservesLastGood(&P02DiffExecuterTestAccess::failClose, 11.0, 9.0));
    ASSERT_TRUE(P02DiffExecuterTestAccess::save(_executer));
    rj::Document saved;
    const std::string updated = read_text_file(file);
    saved.Parse(updated.c_str());
    ASSERT_FALSE(saved.HasParseError());
    ASSERT_EQ(saved["targets"].Size(), 1u);
    ASSERT_EQ(saved["diffs"].Size(), 1u);
    EXPECT_DOUBLE_EQ(saved["targets"][0]["target"].GetDouble(), 11.0);
    EXPECT_DOUBLE_EQ(saved["diffs"][0]["diff"].GetDouble(), 9.0);

    _bd_mgr.enableContract();
    WtDiffExecuter reloaded(&_factory, "p02_diff_exe", &_reader, &_bd_mgr);
    ASSERT_TRUE(P02DiffExecuterTestAccess::load(reloaded));
    EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::target(reloaded, "SHFE.ag.2212"), 11.0);
    EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::diff(reloaded, "SHFE.ag.2212"), 9.0);
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
    // 合约无效时两张仓位表都不恢复；空 set_position 是无操作，不承担持久化职责。
    EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::target(_executer, "SHFE.ag.2212"), -999999.0);
    EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::diff(_executer, "SHFE.ag.2212"), -999999.0);
}

TEST_F(P02DiffExecuterTest, SaveDataCreatesFileWhenNothingLoaded)
{
    const std::string file = exec_data_file("p02_diff_exe");
    std::error_code ec;
    fs::remove(file, ec);

    ASSERT_TRUE(init_with(kDiffCfg, "p02_diff_save.json"));
    EXPECT_FALSE(fs::exists(file, ec));

    ASSERT_TRUE(P02DiffExecuterTestAccess::save(_executer));
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
    EXPECT_TRUE(fs::exists(exec_data_file("p02_diff_exe"), ec));
}

}  // namespace
