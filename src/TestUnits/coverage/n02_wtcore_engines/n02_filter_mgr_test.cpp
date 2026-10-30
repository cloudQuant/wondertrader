/**
 * n02_filter_mgr_test.cpp  (lane N02 / WtCore 后半)
 *
 * 覆盖目标: src/WtCore/WtFilterMgr.cpp
 *   load_filters            : 空文件名早退 / 文件不存在 / 首次加载 / 时间戳未变化跳过 / 时间戳前进后重载 / 解析失败后重试
 *   strategy_filters        : ignore / redirect / 未识别 action 跳过
 *   code_filters            : 精确合约代码命中 / 品种(交易所.品种)代码命中 / redirect 改写 / 未识别 action
 *   executer_filters        : true 屏蔽 / false 放行 / 未配置放行
 *   is_filtered_by_strategy : 增量仓位直接丢弃 / 无过滤器放行
 *
 * 跳过:
 *   - _notifier != NULL 的通知分支（EventNotifier 需要 ZMQ 端点，属外部依赖）。
 *
 * 注意: strategy_filters:null 会被 JSON 转换器忽略，根对象仍有效；坏文件解析失败时
 *       应保留已生效规则和成功加载时间戳，以便修正文件后重新尝试加载。
 */
#include <gtest/gtest.h>

#include "WtCore/WtFilterMgr.h"
#include "Share/StdUtils.hpp"

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

USING_NS_WTP;
namespace {

namespace fs = std::filesystem;

class TempRoot
{
public:
    explicit TempRoot(const std::string& tag)
    {
        std::string name = "n02_" + tag + "_" + std::to_string(static_cast<long long>(::getpid()));
        _path = fs::temp_directory_path() / name;
        std::error_code ec;
        fs::remove_all(_path, ec);
        fs::create_directories(_path, ec);
    }

    ~TempRoot()
    {
        std::error_code ec;
        fs::remove_all(_path, ec);
    }

    TempRoot(const TempRoot&) = delete;
    TempRoot& operator=(const TempRoot&) = delete;

    std::string file(const std::string& rel) const { return (_path / rel).string(); }

    void write(const std::string& rel, const std::string& content) const
    {
        std::ofstream ofs((_path / rel).string().c_str(), std::ios::binary | std::ios::trunc);
        ofs << content;
        ofs.flush();
    }

    void touchForward(const std::string& rel, int seconds) const
    {
        const fs::path p = _path / rel;
        fs::last_write_time(p, fs::last_write_time(p) + std::chrono::seconds(seconds));
    }

    fs::file_time_type lastWriteTime(const std::string& rel) const
    {
        return fs::last_write_time(_path / rel);
    }

    void setLastWriteTime(const std::string& rel, fs::file_time_type value) const
    {
        fs::last_write_time(_path / rel, value);
    }

private:
    fs::path _path;
};

const char* const kFullFilters =
    "{\n"
    "  \"strategy_filters\": {\n"
    "    \"S_IGNORE\":   { \"action\": \"ignore\" },\n"
    "    \"S_REDIRECT\": { \"action\": \"redirect\", \"target\": 7.5 },\n"
    "    \"S_UNKNOWN\":  { \"action\": \"whatever\" },\n"
    "    \"S_NOACTION\": { \"target\": 1 }\n"
    "  },\n"
    "  \"code_filters\": {\n"
    "    \"SSE.STK.600000\": { \"action\": \"IGNORE\" },\n"
    "    \"SSE.STK\":        { \"action\": \"Redirect\", \"target\": 5 },\n"
    "    \"SSE.STK.600519\": { \"action\": \"not_a_real_action\" }\n"
    "  },\n"
    "  \"executer_filters\": {\n"
    "    \"exec_off\": true,\n"
    "    \"exec_on\":  false\n"
    "  }\n"
    "}\n";

class N02FilterMgrTest : public ::testing::Test
{
protected:
    void SetUp() override { _root.reset(new TempRoot("filtermgr")); }
    void TearDown() override { _root.reset(); }

    std::string writeFilters(const std::string& content)
    {
        _root->write("filters.json", content);
        return _root->file("filters.json");
    }

    TempRoot& root() { return *_root; }

private:
    std::unique_ptr<TempRoot> _root;
};

//////////////////////////////////////////////////////////////////
// 早退与容错
//////////////////////////////////////////////////////////////////

TEST_F(N02FilterMgrTest, LoadWithoutAnyFileReturnsImmediately)
{
    WtFilterMgr mgr;
    mgr.load_filters("");
    // 没有任何过滤器被装配, 所有查询都应放行
    double pos = 12;
    EXPECT_FALSE(mgr.is_filtered_by_strategy("S_IGNORE", pos));
    EXPECT_FALSE(mgr.is_filtered_by_code("SSE.STK.600000", pos));
    EXPECT_FALSE(mgr.is_filtered_by_executer("exec_off"));
    EXPECT_DOUBLE_EQ(12, pos);
}

TEST_F(N02FilterMgrTest, LoadMissingFileIsLoggedAndSkipped)
{
    WtFilterMgr mgr;
    const std::string missing = root().file("definitely_not_here.json");
    ASSERT_FALSE(StdFile::exists(missing.c_str()));
    mgr.load_filters(missing.c_str());

    double pos = 3;
    EXPECT_FALSE(mgr.is_filtered_by_strategy("S_IGNORE", pos));
    EXPECT_FALSE(mgr.is_filtered_by_executer("exec_off"));
}

TEST_F(N02FilterMgrTest, NullStrategyFiltersSectionIsIgnored)
{
    WtFilterMgr mgr;
    mgr.load_filters(writeFilters("{\"strategy_filters\": null}\n").c_str());

    double pos = 12;
    EXPECT_FALSE(mgr.is_filtered_by_strategy("S_IGNORE", pos));
    EXPECT_DOUBLE_EQ(12, pos);
}

//////////////////////////////////////////////////////////////////
// 策略过滤器
//////////////////////////////////////////////////////////////////

TEST_F(N02FilterMgrTest, StrategyIgnoreFilterDropsPosition)
{
    WtFilterMgr mgr;
    mgr.load_filters(writeFilters(kFullFilters).c_str());

    double pos = 20;
    EXPECT_TRUE(mgr.is_filtered_by_strategy("S_IGNORE", pos));
    EXPECT_DOUBLE_EQ(20, pos);      // ignore 不改写数值, 只丢弃
}

TEST_F(N02FilterMgrTest, StrategyRedirectFilterRewritesPosition)
{
    WtFilterMgr mgr;
    mgr.load_filters(writeFilters(kFullFilters).c_str());

    double pos = 20;
    EXPECT_FALSE(mgr.is_filtered_by_strategy("S_REDIRECT", pos));
    EXPECT_DOUBLE_EQ(7.5, pos);     // 重定向到配置里的 target
}

TEST_F(N02FilterMgrTest, StrategyFilterDropsAnyDiffPosition)
{
    WtFilterMgr mgr;
    mgr.load_filters(writeFilters(kFullFilters).c_str());

    double pos = 20;
    // 增量仓位无论 ignore 还是 redirect 都被直接丢弃
    EXPECT_TRUE(mgr.is_filtered_by_strategy("S_IGNORE", pos, true));
    EXPECT_TRUE(mgr.is_filtered_by_strategy("S_REDIRECT", pos, true));
    EXPECT_DOUBLE_EQ(20, pos);
}

TEST_F(N02FilterMgrTest, UnrecognizedAndMissingStrategyPassThrough)
{
    WtFilterMgr mgr;
    mgr.load_filters(writeFilters(kFullFilters).c_str());

    double pos = 20;
    EXPECT_FALSE(mgr.is_filtered_by_strategy("S_UNKNOWN", pos));    // action 未识别 -> 不装配
    EXPECT_FALSE(mgr.is_filtered_by_strategy("S_NOACTION", pos));   // 缺 action -> 不装配
    EXPECT_FALSE(mgr.is_filtered_by_strategy("NOT_IN_CONFIG", pos));
    EXPECT_DOUBLE_EQ(20, pos);
}

//////////////////////////////////////////////////////////////////
// 代码过滤器
//////////////////////////////////////////////////////////////////

TEST_F(N02FilterMgrTest, CodeFilterMatchesExactStdCode)
{
    WtFilterMgr mgr;
    mgr.load_filters(writeFilters(kFullFilters).c_str());

    double pos = 10;
    EXPECT_TRUE(mgr.is_filtered_by_code("SSE.STK.600000", pos));    // 精确命中, 大小写不敏感
    EXPECT_DOUBLE_EQ(10, pos);
}

TEST_F(N02FilterMgrTest, CodeFilterFallsBackToCommodityId)
{
    WtFilterMgr mgr;
    mgr.load_filters(writeFilters(kFullFilters).c_str());

    // 600002 没有精确条目, 落到品种 ID SSE.STK 上被重定向
    double pos = 33;
    EXPECT_FALSE(mgr.is_filtered_by_code("SSE.STK.600002", pos));
    EXPECT_DOUBLE_EQ(5, pos);

    // 600519 配了非法 action, 不装配, 于是仍然走品种 ID 分支
    double pos2 = 8;
    EXPECT_FALSE(mgr.is_filtered_by_code("SSE.STK.600519", pos2));
    EXPECT_DOUBLE_EQ(5, pos2);
}

TEST_F(N02FilterMgrTest, CodeFilterLeavesUnconfiguredCodesAlone)
{
    WtFilterMgr mgr;
    mgr.load_filters(writeFilters(kFullFilters).c_str());

    double pos = 11;
    EXPECT_FALSE(mgr.is_filtered_by_code("SHFE.rb2310", pos));      // 两段码: 品种ID等于代码本身
    EXPECT_FALSE(mgr.is_filtered_by_code("DCE.m2309", pos));
    EXPECT_DOUBLE_EQ(11, pos);
}

//////////////////////////////////////////////////////////////////
// 执行通道过滤器
//////////////////////////////////////////////////////////////////

TEST_F(N02FilterMgrTest, ExecuterFilterHonoursBooleanFlag)
{
    WtFilterMgr mgr;
    mgr.load_filters(writeFilters(kFullFilters).c_str());

    EXPECT_TRUE(mgr.is_filtered_by_executer("exec_off"));
    EXPECT_FALSE(mgr.is_filtered_by_executer("exec_on"));
    EXPECT_FALSE(mgr.is_filtered_by_executer("never_configured"));
}

//////////////////////////////////////////////////////////////////
// 重载与时间戳
//////////////////////////////////////////////////////////////////

TEST_F(N02FilterMgrTest, SecondLoadWithSameTimestampIsSkipped)
{
    WtFilterMgr mgr;
    const std::string path = writeFilters(kFullFilters);
    mgr.load_filters(path.c_str());

    // 文件未被改动 -> lastModTime <= _filter_timestamp, 直接 return, 不重建三张表
    mgr.load_filters(path.c_str());
    mgr.load_filters("");        // _filter_file 已记录, 走同一文件, 仍然跳过

    double pos = 20;
    EXPECT_TRUE(mgr.is_filtered_by_strategy("S_IGNORE", pos));
    double pos2 = 20;
    EXPECT_FALSE(mgr.is_filtered_by_strategy("S_REDIRECT", pos2));
    EXPECT_DOUBLE_EQ(7.5, pos2);
}

TEST_F(N02FilterMgrTest, ReloadPicksUpNewContentAfterFileTimeAdvances)
{
    WtFilterMgr mgr;
    const std::string path = writeFilters(kFullFilters);
    mgr.load_filters(path.c_str());

    double before = 20;
    EXPECT_FALSE(mgr.is_filtered_by_strategy("S_REDIRECT", before));
    EXPECT_DOUBLE_EQ(7.5, before);

    // 改写内容并显式把 mtime 推后 5 秒（不依赖墙钟与本地时区）
    root().write("filters.json",
        "{\n"
        "  \"strategy_filters\": {\n"
        "    \"S_REDIRECT\": { \"action\": \"redirect\", \"target\": 42 }\n"
        "  }\n"
        "}\n");
    root().touchForward("filters.json", 5);
    mgr.load_filters("");

    double after = 20;
    EXPECT_FALSE(mgr.is_filtered_by_strategy("S_REDIRECT", after));
    EXPECT_DOUBLE_EQ(42, after);                                   // 新 target 生效
    EXPECT_FALSE(mgr.is_filtered_by_strategy("S_IGNORE", after));   // 旧 ignore 已被清空
    EXPECT_FALSE(mgr.is_filtered_by_executer("exec_off"));          // 旧通道过滤器同样被清空
}

//////////////////////////////////////////////////////////////////
// 坏配置重载失败安全 (BUG-N02-001)
//////////////////////////////////////////////////////////////////

TEST_F(N02FilterMgrTest, FailedReloadPreservesRulesAndAllowsRetryAtSameTimestamp)
{
    WtFilterMgr mgr;
    const std::string path = writeFilters(kFullFilters);
    mgr.load_filters(path.c_str());

    root().write("filters.json", "{\n"); // 语法损坏，loader 返回 NULL
    root().touchForward("filters.json", 5);
    const fs::file_time_type failedTime = root().lastWriteTime("filters.json");
    mgr.load_filters("");

    double strategyPos = 20;
    EXPECT_TRUE(mgr.is_filtered_by_strategy("S_IGNORE", strategyPos));
    double redirectPos = 20;
    EXPECT_FALSE(mgr.is_filtered_by_strategy("S_REDIRECT", redirectPos));
    EXPECT_DOUBLE_EQ(7.5, redirectPos);
    double codePos = 10;
    EXPECT_TRUE(mgr.is_filtered_by_code("SSE.STK.600000", codePos));
    EXPECT_TRUE(mgr.is_filtered_by_executer("exec_off"));

    // 将修复后的内容设回失败文件的 mtime：若失败时错误推进了成功时间戳，
    // 此次重试会被跳过，旧的 7.5 规则就不会替换为 42。
    root().write("filters.json",
        "{\n"
        "  \"strategy_filters\": {\n"
        "    \"S_REDIRECT\": { \"action\": \"redirect\", \"target\": 42 }\n"
        "  }\n"
        "}\n");
    root().setLastWriteTime("filters.json", failedTime);
    mgr.load_filters("");

    double retriedPos = 20;
    EXPECT_FALSE(mgr.is_filtered_by_strategy("S_REDIRECT", retriedPos));
    EXPECT_DOUBLE_EQ(42, retriedPos);
    EXPECT_FALSE(mgr.is_filtered_by_strategy("S_IGNORE", retriedPos));
    EXPECT_FALSE(mgr.is_filtered_by_code("SSE.STK.600000", codePos));
    EXPECT_FALSE(mgr.is_filtered_by_executer("exec_off"));
}

TEST_F(N02FilterMgrTest, FailedFirstLoadDoesNotBlockValidRetryAtSameTimestamp)
{
    WtFilterMgr mgr;
    root().write("filters.json", ""); // 空文件由 loader 直接作为失败返回
    const fs::file_time_type failedTime = root().lastWriteTime("filters.json");
    const std::string path = root().file("filters.json");
    mgr.load_filters(path.c_str());

    double beforeRetry = 12;
    EXPECT_FALSE(mgr.is_filtered_by_strategy("S_FIRST_RETRY", beforeRetry));
    EXPECT_FALSE(mgr.is_filtered_by_executer("exec_off"));

    root().write("filters.json",
        "{\"strategy_filters\":{\"S_FIRST_RETRY\":{\"action\":\"redirect\",\"target\":19}}}\n");
    root().setLastWriteTime("filters.json", failedTime);
    mgr.load_filters("");

    double afterRetry = 12;
    EXPECT_FALSE(mgr.is_filtered_by_strategy("S_FIRST_RETRY", afterRetry));
    EXPECT_DOUBLE_EQ(19, afterRetry);
}

TEST_F(N02FilterMgrTest, NonObjectJsonRootsPreserveRulesAndDoNotAdvanceTimestamp)
{
    WtFilterMgr mgr;
    const std::string path = writeFilters(kFullFilters);
    mgr.load_filters(path.c_str());

    const std::vector<std::string> invalidRoots = { "null", "42", "[]", "[1,2,3]" };
    const fs::file_time_type initialTime = root().lastWriteTime("filters.json");
    fs::file_time_type lastBadTime = initialTime;
    for (size_t i = 0; i < invalidRoots.size(); ++i)
    {
        root().write("filters.json", invalidRoots[i]);
        lastBadTime = initialTime + std::chrono::seconds(5 * static_cast<int>(i + 1));
        root().setLastWriteTime("filters.json", lastBadTime);
        mgr.load_filters("");

        double strategyPos = 20;
        EXPECT_TRUE(mgr.is_filtered_by_strategy("S_IGNORE", strategyPos));
        double redirectPos = 20;
        EXPECT_FALSE(mgr.is_filtered_by_strategy("S_REDIRECT", redirectPos));
        EXPECT_DOUBLE_EQ(7.5, redirectPos);
        double codePos = 10;
        EXPECT_TRUE(mgr.is_filtered_by_code("SSE.STK.600000", codePos));
        EXPECT_TRUE(mgr.is_filtered_by_executer("exec_off"));
    }

    // A valid replacement at exactly the last failed mtime must still be retried.
    root().write("filters.json",
        "{\"strategy_filters\":{\"S_REDIRECT\":{\"action\":\"redirect\",\"target\":42}}}\n");
    root().setLastWriteTime("filters.json", lastBadTime);
    mgr.load_filters("");

    double retriedPos = 20;
    EXPECT_FALSE(mgr.is_filtered_by_strategy("S_REDIRECT", retriedPos));
    EXPECT_DOUBLE_EQ(42, retriedPos);
    EXPECT_FALSE(mgr.is_filtered_by_strategy("S_IGNORE", retriedPos));
}

TEST_F(N02FilterMgrTest, InvalidSectionAndItemTypesPreserveRulesUntilValidRetry)
{
    WtFilterMgr mgr;
    const std::string path = writeFilters(kFullFilters);
    mgr.load_filters(path.c_str());

    const std::vector<std::string> invalidStructures = {
        "{\"strategy_filters\":[]}",
        "{\"strategy_filters\":{\"BROKEN\":[]}}",
        "{\"code_filters\":false}",
        "{\"code_filters\":{\"BROKEN\":\"ignore\"}}",
        "{\"executer_filters\":[]}",
        "{\"executer_filters\":{\"BROKEN\":[]}}",
    };
    const fs::file_time_type initialTime = root().lastWriteTime("filters.json");
    fs::file_time_type lastBadTime = initialTime;
    for (size_t i = 0; i < invalidStructures.size(); ++i)
    {
        root().write("filters.json", invalidStructures[i]);
        lastBadTime = initialTime + std::chrono::seconds(5 * static_cast<int>(i + 1));
        root().setLastWriteTime("filters.json", lastBadTime);
        mgr.load_filters("");

        double strategyPos = 20;
        EXPECT_TRUE(mgr.is_filtered_by_strategy("S_IGNORE", strategyPos));
        double codePos = 10;
        EXPECT_TRUE(mgr.is_filtered_by_code("SSE.STK.600000", codePos));
        EXPECT_TRUE(mgr.is_filtered_by_executer("exec_off"));
    }

    root().write("filters.json",
        "{\"strategy_filters\":{\"S_REDIRECT\":{\"action\":\"redirect\",\"target\":42}}}\n");
    root().setLastWriteTime("filters.json", lastBadTime);
    mgr.load_filters("");

    double retriedPos = 20;
    EXPECT_FALSE(mgr.is_filtered_by_strategy("S_REDIRECT", retriedPos));
    EXPECT_DOUBLE_EQ(42, retriedPos);
    EXPECT_FALSE(mgr.is_filtered_by_strategy("S_IGNORE", retriedPos));
    EXPECT_FALSE(mgr.is_filtered_by_code("SSE.STK.600000", retriedPos));
    EXPECT_FALSE(mgr.is_filtered_by_executer("exec_off"));
}

TEST_F(N02FilterMgrTest, MalformedYamlReloadIsSafeAndStringScalarsRemainCompatible)
{
    WtFilterMgr mgr;
    const std::string path = root().file("filters.yaml");
    root().write("filters.yaml",
        "strategy_filters:\n"
        "  YAML_REDIRECT:\n"
        "    action: redirect\n"
        "    target: 3.5\n"
        "code_filters:\n"
        "  SSE.STK.600000:\n"
        "    action: ignore\n"
        "executer_filters:\n"
        "  exec_yaml: true\n");
    mgr.load_filters(path.c_str());

    double initialPos = 10;
    EXPECT_FALSE(mgr.is_filtered_by_strategy("YAML_REDIRECT", initialPos));
    EXPECT_DOUBLE_EQ(3.5, initialPos);
    double codePos = 10;
    EXPECT_TRUE(mgr.is_filtered_by_code("SSE.STK.600000", codePos));
    EXPECT_TRUE(mgr.is_filtered_by_executer("exec_yaml"));

    root().write("filters.yaml", "strategy_filters:\n  - action: [\n");
    root().touchForward("filters.yaml", 5);
    const fs::file_time_type failedTime = root().lastWriteTime("filters.yaml");
    mgr.load_filters("");

    double afterFailurePos = 10;
    EXPECT_FALSE(mgr.is_filtered_by_strategy("YAML_REDIRECT", afterFailurePos));
    EXPECT_DOUBLE_EQ(3.5, afterFailurePos);
    EXPECT_TRUE(mgr.is_filtered_by_code("SSE.STK.600000", codePos));
    EXPECT_TRUE(mgr.is_filtered_by_executer("exec_yaml"));

    root().write("filters.yaml",
        "strategy_filters:\n"
        "  YAML_REDIRECT:\n"
        "    action: redirect\n"
        "    target: 19\n");
    root().setLastWriteTime("filters.yaml", failedTime);
    mgr.load_filters("");

    double afterRetryPos = 10;
    EXPECT_FALSE(mgr.is_filtered_by_strategy("YAML_REDIRECT", afterRetryPos));
    EXPECT_DOUBLE_EQ(19, afterRetryPos);
    EXPECT_FALSE(mgr.is_filtered_by_code("SSE.STK.600000", codePos));
    EXPECT_FALSE(mgr.is_filtered_by_executer("exec_yaml"));
}

TEST_F(N02FilterMgrTest, ScalarYamlRootReloadPreservesRulesAndAllowsRetry)
{
    WtFilterMgr mgr;
    const std::string path = root().file("filters.yaml");
    root().write("filters.yaml",
        "strategy_filters:\n"
        "  YAML_REDIRECT:\n"
        "    action: redirect\n"
        "    target: 3.5\n"
        "executer_filters:\n"
        "  exec_yaml: true\n");
    mgr.load_filters(path.c_str());

    double initialPos = 10;
    EXPECT_FALSE(mgr.is_filtered_by_strategy("YAML_REDIRECT", initialPos));
    EXPECT_DOUBLE_EQ(3.5, initialPos);
    EXPECT_TRUE(mgr.is_filtered_by_executer("exec_yaml"));

    root().write("filters.yaml", "plain-string-root\n");
    root().touchForward("filters.yaml", 5);
    const fs::file_time_type failedTime = root().lastWriteTime("filters.yaml");
    mgr.load_filters("");

    double afterFailurePos = 10;
    EXPECT_FALSE(mgr.is_filtered_by_strategy("YAML_REDIRECT", afterFailurePos));
    EXPECT_DOUBLE_EQ(3.5, afterFailurePos);
    EXPECT_TRUE(mgr.is_filtered_by_executer("exec_yaml"));

    root().write("filters.yaml",
        "strategy_filters:\n"
        "  YAML_REDIRECT:\n"
        "    action: redirect\n"
        "    target: 19\n");
    root().setLastWriteTime("filters.yaml", failedTime);
    mgr.load_filters("");

    double afterRetryPos = 10;
    EXPECT_FALSE(mgr.is_filtered_by_strategy("YAML_REDIRECT", afterRetryPos));
    EXPECT_DOUBLE_EQ(19, afterRetryPos);
    EXPECT_FALSE(mgr.is_filtered_by_executer("exec_yaml"));
}

}  // namespace
