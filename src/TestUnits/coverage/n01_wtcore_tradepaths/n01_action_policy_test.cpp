/**
 * n01_action_policy_test.cpp
 * 覆盖 src/WtCore/ActionPolicyMgr.{h,cpp}:
 *   - ActionPolicyMgr::init(const char*)：JSON 规则组解析、动作类型大小写不敏感映射、
 *     未知动作跳过分支、filters 品种映射分支
 *   - ActionPolicyMgr::getActionRules(const char*)：命中品种规则组 / 未命中回落 default /
 *     映射到不存在的组时二次回落 default 并打错误日志
 *   - ActionRule 默认构造（memset 归零）与 ActionType 枚举取值
 * 关联：TraderAdapter 的 doEntrust/doCancel 前置校验直接消费 getActionRules 结果。
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include <WtCore/ActionPolicyMgr.h>

USING_NS_WTP;

namespace {

/**
 * @brief 每个用例独占的唯一临时目录（一条用例一个进程，仍显式带 pid+tag 防撞名）
 */
class TempSandbox
{
public:
    explicit TempSandbox(const char* tag)
    {
        _dir = std::filesystem::temp_directory_path()
            / ("n01_action_policy_" + std::string(tag) + "_" + std::to_string(::getpid()));
        std::error_code ec;
        std::filesystem::remove_all(_dir, ec);
        std::filesystem::create_directories(_dir, ec);
    }

    ~TempSandbox()
    {
        std::error_code ec;
        std::filesystem::remove_all(_dir, ec);
    }

    TempSandbox(const TempSandbox&) = delete;
    TempSandbox& operator=(const TempSandbox&) = delete;

    std::filesystem::path file(const char* name) const { return _dir / name; }

    void write(const std::filesystem::path& p, const std::string& content) const
    {
        std::ofstream ofs(p.string(), std::ios::out | std::ios::trunc);
        ofs << content;
        ofs.flush();
    }

private:
    std::filesystem::path _dir;
};

/// 规则组的通用 5 条动作（open/close/closetoday/closeyestoday + 一条重复的 open）
const char* const kDefaultGroupJson = R"JSON(
{
    "default": {
        "order": [
            { "action": "open",         "limit": 100, "limit_l": 60, "limit_s": 40, "pure": false },
            { "action": "close",        "limit": 200, "limit_l": 90, "limit_s": 110, "pure": false },
            { "action": "closetoday",   "limit": 30,  "limit_l": 15, "limit_s": 15,  "pure": true },
            { "action": "closeyestoday","limit": 50,  "limit_l": 20, "limit_s": 30,  "pure": false }
        ]
    }
}
)JSON";

const ActionRule* findByType(const ActionRuleGroup& gp, ActionType at)
{
    for (const auto& r : gp)
    {
        if (r._atype == at)
            return &r;
    }
    return nullptr;
}

} // namespace

// ActionRule 构造函数用 memset 归零；ActionType 枚举顺序决定 AT_Close* 的取值
TEST(N01ActionPolicy, actionRule_defaults_are_zeroed)
{
    ActionRule rule;
    // memset(this, 0, ...) -> _atype 实际是 0，而不是 AT_Unknown(8888)
    EXPECT_EQ(rule._atype, static_cast<ActionType>(0));
    EXPECT_EQ(rule._limit, 0u);
    EXPECT_EQ(rule._limit_l, 0u);
    EXPECT_EQ(rule._limit_s, 0u);
    EXPECT_FALSE(rule._pure);

    // 枚举连续性：AT_Close/CloseToday/CloseYestoday 依次跟在 AT_Open 之后
    EXPECT_EQ(AT_Unknown, 8888);
    EXPECT_EQ(AT_Open, 9999);
    EXPECT_EQ(AT_Close, static_cast<ActionType>(AT_Open + 1));
    EXPECT_EQ(AT_CloseToday, static_cast<ActionType>(AT_Open + 2));
    EXPECT_EQ(AT_CloseYestoday, static_cast<ActionType>(AT_Open + 3));
}

TEST(N01ActionPolicy, init_parses_default_group_and_maps_every_action)
{
    TempSandbox sb("default");
    const auto path = sb.file("action.json");
    sb.write(path, kDefaultGroupJson);

    ActionPolicyMgr mgr;
    ASSERT_TRUE(mgr.init(path.string().c_str()));

    const ActionRuleGroup& gp = mgr.getActionRules("anything-not-mapped");
    ASSERT_EQ(gp.size(), 4u);
    EXPECT_NE(findByType(gp, AT_Open), nullptr);
    EXPECT_NE(findByType(gp, AT_Close), nullptr);
    EXPECT_NE(findByType(gp, AT_CloseToday), nullptr);
    EXPECT_NE(findByType(gp, AT_CloseYestoday), nullptr);

    const ActionRule* open = findByType(gp, AT_Open);
    EXPECT_EQ(open->_limit, 100u);
    EXPECT_EQ(open->_limit_l, 60u);
    EXPECT_EQ(open->_limit_s, 40u);
    EXPECT_FALSE(open->_pure);

    const ActionRule* closeToday = findByType(gp, AT_CloseToday);
    EXPECT_EQ(closeToday->_limit, 30u);
    EXPECT_TRUE(closeToday->_pure);

    const ActionRule* closeY = findByType(gp, AT_CloseYestoday);
    EXPECT_EQ(closeY->_limit_s, 30u);

    // 解析顺序即写入顺序
    EXPECT_EQ(gp[0]._atype, AT_Open);
    EXPECT_EQ(gp[3]._atype, AT_CloseYestoday);
}

// 动作名比较走 wt_stricmp：大小写、混合写法都必须落到同一个 ActionType
TEST(N01ActionPolicy, init_action_name_is_case_insensitive)
{
    TempSandbox sb("case");
    sb.write(sb.file("action.json"), R"JSON(
{
    "default": {
        "order": [
            { "action": "OPEN",         "limit": 1 },
            { "action": "Close",        "limit": 2 },
            { "action": "CLOSESTODAY",  "limit": 3 },
            { "action": "closeYeStoday","limit": 4 }
        ]
    }
}
)JSON");

    ActionPolicyMgr mgr;
    ASSERT_TRUE(mgr.init((sb.file("action.json")).string().c_str()));

    const ActionRuleGroup& gp = mgr.getActionRules("WHATEVER");
    ASSERT_EQ(gp.size(), 4u);
    EXPECT_EQ(gp[0]._atype, AT_Open);
    EXPECT_EQ(gp[0]._limit, 1u);
    EXPECT_EQ(gp[1]._atype, AT_Close);
    EXPECT_EQ(gp[2]._atype, AT_CloseToday);
    EXPECT_EQ(gp[3]._atype, AT_CloseYestoday);
}

// 未识别的动作名记录错误后 continue：该条被丢弃，其余规则不受影响
TEST(N01ActionPolicy, init_skips_unrecognized_action)
{
    TempSandbox sb("skip");
    sb.write(sb.file("action.json"), R"JSON(
{
    "default": {
        "order": [
            { "action": "open",       "limit": 5 },
            { "action": "nonsense",   "limit": 6 },
            { "action": "",           "limit": 7 },
            { "action": "close",      "limit": 8 }
        ]
    }
}
)JSON");

    ActionPolicyMgr mgr;
    ASSERT_TRUE(mgr.init((sb.file("action.json")).string().c_str()));

    const ActionRuleGroup& gp = mgr.getActionRules("X");
    ASSERT_EQ(gp.size(), 2u) << "两条非法动作必须被 continue 跳过";
    EXPECT_EQ(gp[0]._atype, AT_Open);
    EXPECT_EQ(gp[0]._limit, 5u);
    EXPECT_EQ(gp[1]._atype, AT_Close);
    EXPECT_EQ(gp[1]._limit, 8u);
}

// order 键缺失 / 不是数组：规则组仍然被创建，只是为空
TEST(N01ActionPolicy, init_tolerates_missing_or_non_array_order)
{
    TempSandbox sb("noorder");
    sb.write(sb.file("action.json"), R"JSON(
{
    "default": {
        "order": "not-an-array"
    },
    "noorder": {
        "filters": ["DCE.a"]
    }
}
)JSON");

    ActionPolicyMgr mgr;
    ASSERT_TRUE(mgr.init((sb.file("action.json")).string().c_str()));

    EXPECT_TRUE(mgr.getActionRules("DCE.a").empty());
    EXPECT_TRUE(mgr.getActionRules("unknown").empty());
}

// filters 把品种映射到规则组；未映射品种回落 default
TEST(N01ActionPolicy, filters_route_commodity_to_named_group)
{
    TempSandbox sb("route");
    sb.write(sb.file("action.json"), R"JSON(
{
    "default": {
        "order": [ { "action": "open", "limit": 11 } ]
    },
    "shfe_strict": {
        "order": [ { "action": "open", "limit": 22 }, { "action": "close", "limit": 33 } ],
        "filters": [ "SHFE.rb", "SHFE.cu", "SHFE.au" ]
    }
}
)JSON");

    ActionPolicyMgr mgr;
    ASSERT_TRUE(mgr.init((sb.file("action.json")).string().c_str()));

    const ActionRuleGroup& rb = mgr.getActionRules("SHFE.rb");
    ASSERT_EQ(rb.size(), 2u);
    EXPECT_EQ(rb[0]._limit, 22u);
    EXPECT_EQ(rb[1]._atype, AT_Close);
    EXPECT_EQ(rb[1]._limit, 33u);

    // 同一个映射表里的其它品种共享同一组规则对象
    EXPECT_EQ(mgr.getActionRules("SHFE.au").size(), 2u);
    EXPECT_EQ(mgr.getActionRules("SHFE.cu")[0]._limit, 22u);

    // 未登记品种 → default 组
    const ActionRuleGroup& dft = mgr.getActionRules("CFFEX.if");
    ASSERT_EQ(dft.size(), 1u);
    EXPECT_EQ(dft[0]._limit, 11u);
}

// 缺失 limit/limit_l/limit_s/pure 时全部按 0 / false 处理（getUInt32/getBoolean 的缺省语义）
TEST(N01ActionPolicy, init_absent_limit_fields_become_zero)
{
    TempSandbox sb("absent");
    sb.write(sb.file("action.json"), R"JSON(
{
    "default": {
        "order": [ { "action": "open" } ]
    }
}
)JSON");

    ActionPolicyMgr mgr;
    ASSERT_TRUE(mgr.init((sb.file("action.json")).string().c_str()));

    const ActionRuleGroup& gp = mgr.getActionRules("x");
    ASSERT_EQ(gp.size(), 1u);
    EXPECT_EQ(gp[0]._atype, AT_Open);
    EXPECT_EQ(gp[0]._limit, 0u);
    EXPECT_EQ(gp[0]._limit_l, 0u);
    EXPECT_EQ(gp[0]._limit_s, 0u);
    EXPECT_FALSE(gp[0]._pure);
}

// 配置文件不存在：load_from_file 返回 NULL，init 直接 false，且不改动已有规则表
TEST(N01ActionPolicy, init_returns_false_on_missing_file)
{
    TempSandbox sb("missing");
    ActionPolicyMgr mgr;
    EXPECT_FALSE(mgr.init((sb.file("does_not_exist.json")).string().c_str()));
    EXPECT_TRUE(mgr.getActionRules("anything").empty());
}

// 非 JSON 内容（语法非法）：加载失败返回 false
TEST(N01ActionPolicy, init_returns_false_on_broken_file)
{
    TempSandbox sb("broken");
    sb.write(sb.file("action.json"), "{ this is : not valid json ]]]");
    ActionPolicyMgr mgr;
    EXPECT_FALSE(mgr.init((sb.file("action.json")).string().c_str()));
}

// 首次加载缺少 default 必须失败且不留下部分规则；失败后的 lookup 也必须安全。
TEST(N01ActionPolicy, first_init_without_default_is_rejected_and_lookup_is_safe)
{
    TempSandbox sb("no_default");
    sb.write(sb.file("action.json"), R"JSON(
{
    "strict": {
        "order": [ { "action": "open", "limit": 99 } ],
        "filters": [ "SHFE.rb" ]
    }
}
)JSON");

    ActionPolicyMgr mgr;
    EXPECT_FALSE(mgr.init((sb.file("action.json")).string().c_str()));
    EXPECT_TRUE(mgr.getActionRules("whatever").empty());
    EXPECT_TRUE(mgr.getActionRules("SHFE.rb").empty());

    // 失败配置不能留下 strict 组或品种映射，之后有效 default 配置正常生效。
    sb.write(sb.file("action.json"), R"JSON(
{
    "default": {
        "order": [ { "action": "open", "limit": 11 } ]
    }
}
)JSON");
    ASSERT_TRUE(mgr.init((sb.file("action.json")).string().c_str()));

    const ActionRuleGroup& routed = mgr.getActionRules("SHFE.rb");
    ASSERT_EQ(routed.size(), 1u);
    EXPECT_EQ(routed[0]._limit, 11u);
}

// 重复初始化继续把新规则追加到已有组；缺少 default 的增量配置仍被接受。
TEST(N01ActionPolicy, repeated_init_remains_additive_without_requiring_default_again)
{
    TempSandbox sb("additive");
    const auto path = sb.file("action.json");
    sb.write(path, kDefaultGroupJson);

    ActionPolicyMgr mgr;
    ASSERT_TRUE(mgr.init(path.string().c_str()));

    sb.write(path, R"JSON(
{
    "default": {
        "order": [ { "action": "open", "limit": 200 } ]
    }
}
)JSON");
    ASSERT_TRUE(mgr.init(path.string().c_str()));

    sb.write(path, R"JSON(
{
    "strict": {
        "order": [ { "action": "close", "limit": 300 } ],
        "filters": [ "SHFE.rb" ]
    }
}
)JSON");
    ASSERT_TRUE(mgr.init(path.string().c_str()));

    const ActionRuleGroup& defaults = mgr.getActionRules("unknown");
    ASSERT_EQ(defaults.size(), 5u);
    EXPECT_EQ(defaults.back()._atype, AT_Open);
    EXPECT_EQ(defaults.back()._limit, 200u);

    const ActionRuleGroup& strict = mgr.getActionRules("SHFE.rb");
    ASSERT_EQ(strict.size(), 1u);
    EXPECT_EQ(strict[0]._atype, AT_Close);
    EXPECT_EQ(strict[0]._limit, 300u);
}
