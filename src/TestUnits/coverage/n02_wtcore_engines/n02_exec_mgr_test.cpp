/**
 * n02_exec_mgr_test.cpp  (lane N02 / WtCore 后半)
 *
 * 覆盖目标: src/WtCore/WtExecMgr.cpp + WtExecMgr.h 内联接口
 *   WtExecuterMgr::add_executer / enum_executer / set_positions / handle_pos_change /
 *   handle_tick / add_target_to_cache / commit_cached_targets / clear_cached_targets /
 *   load_router_rules / get_route_by_strategy / set_filter_mgr
 *
 * 这里的"目标仓位 -> 执行器"路由与缓存组合逻辑是执行链路上最适合单测的一段:
 * 用记录型 IExecCommand 替身断言每个执行器实际收到的仓位表。
 *
 * 跳过: WtLocalExecuter / WtDiffExecuter 的下单撮合分支 —— 二者都要通过 TraderAdapter
 *       (真实 CTP/柜台连接)与 ExecuteUnit 插件 dylib 才能走到, 详见 manifest。
 */
#include <gtest/gtest.h>

#include "WtCore/WtExecMgr.h"
#include "WtCore/WtFilterMgr.h"
#include "Includes/WTSVariant.hpp"
#include "WTSUtils/WTSCfgLoader.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
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

private:
    fs::path _path;
};

//////////////////////////////////////////////////////////////////
// 记录型执行器替身
//////////////////////////////////////////////////////////////////

class RecExecuter : public IExecCommand
{
public:
    explicit RecExecuter(const char* name) : IExecCommand(name) {}
    // IExecCommand 的析构函数不是虚的（BUG-N02-003），
    // 替身必须自带虚析构，否则 shared_ptr 回收时 clang 判定为 UB。
    virtual ~RecExecuter() {}

    void set_position(const wt_hashmap<std::string, double>& targets) override
    {
        _sets.push_back(targets);
    }

    void on_position_changed(const char* stdCode, double diffPos) override
    {
        _changes.push_back(std::make_pair(std::string(stdCode), diffPos));
    }

    void on_tick(const char* stdCode, WTSTickData* newTick) override
    {
        _ticks.push_back(std::string(stdCode));
        if (newTick != NULL)
            ++_tickPtrs;
    }

    std::vector<wt_hashmap<std::string, double> > _sets;
    std::vector<std::pair<std::string, double> > _changes;
    std::vector<std::string> _ticks;
    uint32_t _tickPtrs = 0;
};

typedef std::shared_ptr<RecExecuter> RecPtr;

const char* const kFilterJson =
    "{\n"
    "  \"code_filters\": {\n"
    "    \"SSE.STK.600000\": { \"action\": \"ignore\" },\n"
    "    \"SSE.STK.600001\": { \"action\": \"redirect\", \"target\": 6 }\n"
    "  },\n"
    "  \"executer_filters\": {\n"
    "    \"exec_banned\": true\n"
    "  }\n"
    "}\n";

const char* const kRouterConfigJson =
    "{\n"
    "  \"router_rules\": [\n"
    "    { \"strategy\": \"stra_a\", \"executer\": [\"exec_1\", \"exec_2\"] },\n"
    "    { \"strategy\": \"stra_b\", \"executer\": \"exec_3\" }\n"
    "  ]\n"
    "}\n";

class N02ExecMgrTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        _root.reset(new TempRoot("execmgr"));
        _a.reset(new RecExecuter("exec_a"));
        _b.reset(new RecExecuter("exec_b"));
    }

    void TearDown() override
    {
        _b.reset();
        _a.reset();
        _root.reset();
    }

    TempRoot& root() { return *_root; }

    WtFilterMgr* loadFilters()
    {
        root().write("filters.json", kFilterJson);
        _filters.reset(new WtFilterMgr());
        _filters->load_filters(root().file("filters.json").c_str());
        return _filters.get();
    }

    WTSVariant* loadJson(const std::string& rel, const char* content)
    {
        root().write(rel, content);
        return WTSCfgLoader::load_from_file(root().file(rel).c_str());
    }

    WTSVariant* loadRouterRules(const std::string& rel, const char* content)
    {
        WTSVariant* config = loadJson(rel, content);
        if (config == NULL)
            return NULL;

        WTSVariant* rules = config->get("router_rules");
        if (rules != NULL)
            rules->retain();
        config->release();
        return rules;
    }

protected:
    std::unique_ptr<TempRoot> _root;
    std::unique_ptr<WtFilterMgr> _filters;
    RecPtr _a;
    RecPtr _b;
};

//////////////////////////////////////////////////////////////////
// 注册与遍历
//////////////////////////////////////////////////////////////////

TEST_F(N02ExecMgrTest, AddAndEnumExecuters)
{
    WtExecuterMgr mgr;
    std::vector<std::string> names;
    mgr.enum_executer([&names](ExecCmdPtr e) { names.push_back(e->name()); });
    EXPECT_TRUE(names.empty());

    mgr.add_executer(ExecCmdPtr(_a));
    mgr.add_executer(ExecCmdPtr(_b));
    // 同名重复注册只覆盖表项, 不增加数量
    mgr.add_executer(ExecCmdPtr(_a));

    mgr.enum_executer([&names](ExecCmdPtr e) { names.push_back(e->name()); });
    ASSERT_EQ(2u, names.size());
    EXPECT_EQ("exec_a", names.at(0));
    EXPECT_EQ("exec_b", names.at(1));
}

TEST_F(N02ExecMgrTest, HandleTickBroadcastsToEveryExecuter)
{
    WtExecuterMgr mgr;
    mgr.add_executer(ExecCmdPtr(_a));
    mgr.add_executer(ExecCmdPtr(_b));

    mgr.handle_tick("SHFE.rb2310", NULL);
    mgr.handle_tick("DCE.m2309", NULL);

    ASSERT_EQ(2u, _a->_ticks.size());
    ASSERT_EQ(2u, _b->_ticks.size());
    EXPECT_EQ("SHFE.rb2310", _a->_ticks.at(0));
    EXPECT_EQ("DCE.m2309", _b->_ticks.at(1));
    EXPECT_EQ(0u, _a->_tickPtrs);
}

//////////////////////////////////////////////////////////////////
// set_positions: 代码过滤 + 通道过滤
//////////////////////////////////////////////////////////////////

TEST_F(N02ExecMgrTest, SetPositionsWithoutFilterGivesEveryExecuterTheSameMap)
{
    WtExecuterMgr mgr;
    mgr.add_executer(ExecCmdPtr(_a));
    mgr.add_executer(ExecCmdPtr(_b));

    wt_hashmap<std::string, double> targets;
    targets["SHFE.rb2310"] = 5;
    targets["DCE.m2309"] = -2;
    mgr.set_positions(targets);

    ASSERT_EQ(1u, _a->_sets.size());
    ASSERT_EQ(1u, _b->_sets.size());
    EXPECT_EQ(2u, _a->_sets.at(0).size());
    EXPECT_DOUBLE_EQ(5, _a->_sets.at(0)["SHFE.rb2310"]);
    EXPECT_DOUBLE_EQ(-2, _b->_sets.at(0)["DCE.m2309"]);
}

TEST_F(N02ExecMgrTest, SetPositionsDropsIgnoredCodeAndAppliesRedirect)
{
    WtFilterMgr* filters = loadFilters();
    WtExecuterMgr mgr;
    mgr.set_filter_mgr(filters);
    mgr.add_executer(ExecCmdPtr(_a));

    wt_hashmap<std::string, double> targets;
    targets["SSE.STK.600000"] = 100;      // ignore -> 整条剔除
    targets["SSE.STK.600001"] = 3;        // redirect -> 改成 6
    targets["SHFE.rb2310"] = 7;           // 未配置 -> 原样
    mgr.set_positions(targets);

    ASSERT_EQ(1u, _a->_sets.size());
    const wt_hashmap<std::string, double>& sent = _a->_sets.at(0);
    EXPECT_EQ(2u, sent.size());
    EXPECT_EQ(sent.end(), sent.find("SSE.STK.600000"));
    ASSERT_NE(sent.end(), sent.find("SSE.STK.600001"));
    EXPECT_DOUBLE_EQ(6, sent.find("SSE.STK.600001")->second);
    ASSERT_NE(sent.end(), sent.find("SHFE.rb2310"));
    EXPECT_DOUBLE_EQ(7, sent.find("SHFE.rb2310")->second);
}

TEST_F(N02ExecMgrTest, SetPositionsSkipsExecuterBannedByFilter)
{
    WtFilterMgr* filters = loadFilters();
    WtExecuterMgr mgr;
    mgr.set_filter_mgr(filters);

    RecPtr banned(new RecExecuter("exec_banned"));
    mgr.add_executer(ExecCmdPtr(banned));
    mgr.add_executer(ExecCmdPtr(_a));

    wt_hashmap<std::string, double> targets;
    targets["SHFE.rb2310"] = 4;
    mgr.set_positions(targets);

    EXPECT_EQ(0u, banned->_sets.size());
    ASSERT_EQ(1u, _a->_sets.size());
    EXPECT_DOUBLE_EQ(4, _a->_sets.at(0).find("SHFE.rb2310")->second);
}

//////////////////////////////////////////////////////////////////
// handle_pos_change: 路由规则 + 过滤器
//////////////////////////////////////////////////////////////////

TEST_F(N02ExecMgrTest, HandlePosChangeWithAllReachesEveryUnroutedExecuter)
{
    WtExecuterMgr mgr;
    mgr.add_executer(ExecCmdPtr(_a));
    mgr.add_executer(ExecCmdPtr(_b));

    mgr.handle_pos_change("SHFE.rb2310", 5, 3);
    EXPECT_EQ(1u, _a->_changes.size());
    EXPECT_EQ(1u, _b->_changes.size());
    EXPECT_DOUBLE_EQ(3, _a->_changes.at(0).second);
    EXPECT_EQ("SHFE.rb2310", _b->_changes.at(0).first);
}

TEST_F(N02ExecMgrTest, HandlePosChangeSkipsRoutedExecutersWhenExecidIsAll)
{
    WTSVariant* rules = loadRouterRules("router.json", kRouterConfigJson);
    ASSERT_NE(nullptr, rules);

    WtExecuterMgr mgr;
    ASSERT_TRUE(mgr.load_router_rules(rules));
    rules->release();

    RecPtr routed(new RecExecuter("exec_1"));
    mgr.add_executer(ExecCmdPtr(routed));
    mgr.add_executer(ExecCmdPtr(_a));

    mgr.handle_pos_change("DCE.m2309", 2, 2, "ALL");
    EXPECT_EQ(0u, routed->_changes.size());     // 已绑定路由 -> "ALL" 不广播
    EXPECT_EQ(1u, _a->_changes.size());
}

TEST_F(N02ExecMgrTest, HandlePosChangeWithExplicitExecidHitsRoutedExecuter)
{
    WTSVariant* rules = loadRouterRules("router.json", kRouterConfigJson);
    ASSERT_NE(nullptr, rules);

    WtExecuterMgr mgr;
    ASSERT_TRUE(mgr.load_router_rules(rules));
    rules->release();

    RecPtr routed(new RecExecuter("exec_2"));
    RecPtr other(new RecExecuter("exec_3"));
    mgr.add_executer(ExecCmdPtr(routed));
    mgr.add_executer(ExecCmdPtr(other));

    mgr.handle_pos_change("CZCE.SR301", 4, 4, "exec_2");
    EXPECT_EQ(1u, routed->_changes.size());
    EXPECT_EQ(0u, other->_changes.size());
    EXPECT_DOUBLE_EQ(4, routed->_changes.at(0).second);
}

TEST_F(N02ExecMgrTest, HandlePosChangeHonoursCodeAndExecuterFilters)
{
    WtFilterMgr* filters = loadFilters();
    WtExecuterMgr mgr;
    mgr.set_filter_mgr(filters);
    mgr.add_executer(ExecCmdPtr(_a));

    // 被 ignore 的代码直接 return
    mgr.handle_pos_change("SSE.STK.600000", 9, 9);
    EXPECT_EQ(0u, _a->_changes.size());

    // 被 redirect 的代码: target 改成 6, diff 相应平移
    mgr.handle_pos_change("SSE.STK.600001", 3, 1);
    ASSERT_EQ(1u, _a->_changes.size());
    EXPECT_DOUBLE_EQ(4, _a->_changes.at(0).second);   // 1 + (6 - 3)

    // 未配置的代码原样透传
    mgr.handle_pos_change("SHFE.rb2310", 2, -2);
    ASSERT_EQ(2u, _a->_changes.size());
    EXPECT_DOUBLE_EQ(-2, _a->_changes.at(1).second);
}

//////////////////////////////////////////////////////////////////
// 目标缓存: 累加 / 缩放 / 分组 / 清理
//////////////////////////////////////////////////////////////////

TEST_F(N02ExecMgrTest, CacheAccumulatesAndCommitSendsAllGroup)
{
    WtExecuterMgr mgr;
    mgr.add_executer(ExecCmdPtr(_a));
    mgr.add_executer(ExecCmdPtr(_b));

    mgr.add_target_to_cache("SHFE.rb2310", 3, "ALL");
    mgr.add_target_to_cache("SHFE.rb2310", 4, "ALL");     // 同代码累加
    mgr.add_target_to_cache("DCE.m2309", -5, "ALL");
    mgr.commit_cached_targets();

    ASSERT_EQ(1u, _a->_sets.size());
    EXPECT_DOUBLE_EQ(7, _a->_sets.at(0).find("SHFE.rb2310")->second);
    EXPECT_DOUBLE_EQ(-5, _a->_sets.at(0).find("DCE.m2309")->second);
    ASSERT_EQ(1u, _b->_sets.size());
    EXPECT_DOUBLE_EQ(7, _b->_sets.at(0).find("SHFE.rb2310")->second);

    // 提交后缓存被清空, 再次提交不会重复下发
    mgr.commit_cached_targets();
    EXPECT_EQ(1u, _a->_sets.size());
}

TEST_F(N02ExecMgrTest, CommitScalesAndKeepsSignAndSkipsZero)
{
    WtExecuterMgr mgr;
    mgr.add_executer(ExecCmdPtr(_a));

    mgr.add_target_to_cache("SHFE.rb2310", 3, "ALL");
    mgr.add_target_to_cache("DCE.m2309", -4, "ALL");
    mgr.add_target_to_cache("CZCE.SR301", 0, "ALL");
    mgr.commit_cached_targets(2.0);

    ASSERT_EQ(1u, _a->_sets.size());
    const wt_hashmap<std::string, double>& sent = _a->_sets.at(0);
    EXPECT_DOUBLE_EQ(6, sent.find("SHFE.rb2310")->second);
    EXPECT_DOUBLE_EQ(-8, sent.find("DCE.m2309")->second);    // 符号保留
    EXPECT_DOUBLE_EQ(0, sent.find("CZCE.SR301")->second);    // 0 仓位跳过缩放
}

TEST_F(N02ExecMgrTest, CommitPrefersOwnGroupAndIgnoresUnmatchedGroups)
{
    WtExecuterMgr mgr;
    RecPtr own(new RecExecuter("exec_own"));
    RecPtr plain(new RecExecuter("exec_plain"));
    RecPtr orphan(new RecExecuter("exec_orphan"));
    mgr.add_executer(ExecCmdPtr(own));
    mgr.add_executer(ExecCmdPtr(plain));
    mgr.add_executer(ExecCmdPtr(orphan));

    mgr.add_target_to_cache("SHFE.rb2310", 2, "ALL");        // 全体组合
    mgr.add_target_to_cache("SHFE.rb2310", 9, "exec_own");   // 私有组合优先
    mgr.add_target_to_cache("DCE.m2309", 1, "exec_missing"); // 无人认领的组合
    mgr.commit_cached_targets();

    ASSERT_EQ(1u, own->_sets.size());
    const auto& ownTargets = own->_sets.at(0);
    auto ownTarget = ownTargets.find("SHFE.rb2310");
    ASSERT_NE(ownTargets.end(), ownTarget);
    EXPECT_DOUBLE_EQ(9, ownTarget->second);
    EXPECT_EQ(ownTargets.end(), ownTargets.find("DCE.m2309"));

    ASSERT_EQ(1u, plain->_sets.size());
    const auto& plainTargets = plain->_sets.at(0);
    auto plainTarget = plainTargets.find("SHFE.rb2310");
    ASSERT_NE(plainTargets.end(), plainTarget);
    EXPECT_DOUBLE_EQ(2, plainTarget->second);
    EXPECT_EQ(plainTargets.end(), plainTargets.find("DCE.m2309"));

    // 无私有目标的 executor 使用 ALL；exec_missing 的独有目标无人接收。
    ASSERT_EQ(1u, orphan->_sets.size());
    const auto& orphanTargets = orphan->_sets.at(0);
    auto orphanTarget = orphanTargets.find("SHFE.rb2310");
    ASSERT_NE(orphanTargets.end(), orphanTarget);
    EXPECT_DOUBLE_EQ(2, orphanTarget->second);
    EXPECT_EQ(orphanTargets.end(), orphanTargets.find("DCE.m2309"));
}

TEST_F(N02ExecMgrTest, CommitAppliesFiltersAndClearsCache)
{
    WtFilterMgr* filters = loadFilters();
    WtExecuterMgr mgr;
    mgr.set_filter_mgr(filters);

    RecPtr banned(new RecExecuter("exec_banned"));
    mgr.add_executer(ExecCmdPtr(banned));
    mgr.add_executer(ExecCmdPtr(_a));

    mgr.add_target_to_cache("SSE.STK.600000", 5, "ALL");     // ignore -> 剔除
    mgr.add_target_to_cache("SSE.STK.600001", 2, "ALL");     // redirect -> 6
    mgr.add_target_to_cache("SHFE.rb2310", 0, "ALL");        // 0 -> 不缩放
    mgr.commit_cached_targets();

    EXPECT_EQ(0u, banned->_sets.size());
    ASSERT_EQ(1u, _a->_sets.size());
    const wt_hashmap<std::string, double>& sent = _a->_sets.at(0);
    EXPECT_EQ(sent.end(), sent.find("SSE.STK.600000"));
    EXPECT_DOUBLE_EQ(6, sent.find("SSE.STK.600001")->second);

    // clear_cached_targets 之后提交应该什么都不发
    mgr.add_target_to_cache("SHFE.au2312", 3, "ALL");
    mgr.clear_cached_targets();
    mgr.commit_cached_targets();
    EXPECT_EQ(1u, _a->_sets.size());
}

//////////////////////////////////////////////////////////////////
// 路由规则本身
//////////////////////////////////////////////////////////////////

TEST_F(N02ExecMgrTest, LoadRouterRulesRejectsNullAndNonArray)
{
    WtExecuterMgr mgr;
    EXPECT_FALSE(mgr.load_router_rules(NULL));

    WTSVariant* obj = loadJson("router_obj.json", "{\n\"strategy\": \"stra_x\"\n}\n");
    ASSERT_NE(nullptr, obj);
    EXPECT_FALSE(mgr.load_router_rules(obj));
    obj->release();
}

TEST_F(N02ExecMgrTest, LoadRouterRulesAcceptsStringAndArrayExecuters)
{
    WTSVariant* rules = loadRouterRules("router.json", kRouterConfigJson);
    ASSERT_NE(nullptr, rules);

    WtExecuterMgr mgr;
    ASSERT_TRUE(mgr.load_router_rules(rules));

    const wt_hashset<std::string>& ra = mgr.get_route_by_strategy("stra_a");
    ASSERT_EQ(2u, ra.size());
    EXPECT_NE(ra.end(), ra.find("exec_1"));
    EXPECT_NE(ra.end(), ra.find("exec_2"));

    const wt_hashset<std::string>& rb = mgr.get_route_by_strategy("stra_b");
    ASSERT_EQ(1u, rb.size());
    EXPECT_NE(rb.end(), rb.find("exec_3"));

    rules->release();
}

TEST_F(N02ExecMgrTest, RouteOfUnknownStrategyFallsBackToAll)
{
    WtExecuterMgr mgr;
    // 未加载任何规则 -> 一律返回 {"ALL"}
    const wt_hashset<std::string>& pre = mgr.get_route_by_strategy("whatever");
    ASSERT_EQ(1u, pre.size());
    EXPECT_NE(pre.end(), pre.find("ALL"));

    WTSVariant* rules = loadRouterRules("router.json", kRouterConfigJson);
    ASSERT_NE(nullptr, rules);
    ASSERT_TRUE(mgr.load_router_rules(rules));
    rules->release();

    const wt_hashset<std::string>& post = mgr.get_route_by_strategy("not_registered");
    ASSERT_EQ(1u, post.size());
    EXPECT_NE(post.end(), post.find("ALL"));
}

//////////////////////////////////////////////////////////////////
// 缺陷类: 路由条目缺少 executer 节点时 itemExec 为 NULL 立刻解引用
// 见 BUG-N02-004; 仅将实际会触发空指针解引用的输入保留为 DISABLED。
//////////////////////////////////////////////////////////////////

TEST_F(N02ExecMgrTest, DISABLED_RouterRuleWithoutExecuterNodeCrashes)
{
    const char* const missingExecuterConfig =
        "{\"router_rules\":[{\"strategy\":\"s\"}]}\n";
    WTSVariant* rules = loadRouterRules("router_bad.json", missingExecuterConfig);
    ASSERT_NE(nullptr, rules);

    WtExecuterMgr mgr;
    mgr.load_router_rules(rules);       // itemExec == NULL, 当前实现会在此解引用
    rules->release();
}

TEST_F(N02ExecMgrTest, RouterRuleWithoutStrategyDoesNotDereferenceNull)
{
    const char* const missingStrategyConfig =
        "{\"router_rules\":[{\"executer\":\"e\"}]}\n";
    WTSVariant* rules = loadRouterRules("router_missing_strategy.json", missingStrategyConfig);
    ASSERT_NE(nullptr, rules);

    WtExecuterMgr mgr;
    // strategy 缺失时 getCString 返回默认空串；此用例只锚定不会在该处空指针解引用，
    // 不对缺失 strategy 的路由语义作额外约定。
    EXPECT_NO_THROW(mgr.load_router_rules(rules));
    rules->release();
}

}  // namespace
