/**
 * n02_filter_mgr_test.cpp  (lane N02 / WtCore 后半)
 *
 * 覆盖目标: src/WtCore/WtFilterMgr.cpp
 *   load_filters            : 空文件名早退 / 文件不存在 / 首次加载 / 时间戳未变化跳过 / 时间戳前进后重载
 *   strategy_filters        : ignore / redirect / 未识别 action 跳过
 *   code_filters            : 精确合约代码命中 / 品种(交易所.品种)代码命中 / redirect 改写 / 未识别 action
 *   executer_filters        : true 屏蔽 / false 放行 / 未配置放行
 *   is_filtered_by_strategy : 增量仓位直接丢弃 / 无过滤器放行
 *
 * 跳过:
 *   - _notifier != NULL 的通知分支（EventNotifier 需要 ZMQ 端点，属外部依赖）。
 *
 * 注意: load_filters 对 WTSCfgLoader::load_from_file 的返回值没有判空,
 *       非法配置会得到 NULL 并立刻 cfg->get(...)  ->  空指针解引用。
 *       该缺陷类的全部输入集中在唯一一个 DISABLED_ 用例里, 启用用例只喂合法 JSON。
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
// 缺陷类: 非法配置文件 -> load_from_file 返回 NULL 后未判空
// 见 bugs/n02_wtcore_engines.md  BUG-N02-001
// 该类全部输入集中在本用例内, 不放进任何启用用例。
//////////////////////////////////////////////////////////////////

TEST(N02FilterMgrDisabled, DISABLED_MalformedConfigFilesCrashLoadFilters)
{
    const char* const malformed[] = {
        "",                                     // 空文件
        "{",                                    // 截断对象
        "[1,2,3]",                              // 顶层是数组不是对象
        "not json at all",                      // 完全非法
        "{\"strategy_filters\": null}",         // 节点为 null
    };

    TempRoot root("fltbad");
    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i)
    {
        root.write("bad_filters.json", malformed[i]);
        WtFilterMgr mgr;
        // 期望: 记录错误并返回; 实际: cfg 为 NULL 时 cfg->get(...) 解引用空指针
        mgr.load_filters(root.file("bad_filters.json").c_str());
    }
}

}  // namespace
