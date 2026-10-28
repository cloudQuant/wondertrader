/**
 * n03_action_policy_test.cpp
 *
 * 覆盖 src/WtUftCore/ActionPolicyMgr.cpp:
 *   ActionPolicyMgr::init(const char*)       —— 规则组解析、动作类型映射、品种过滤器映射
 *   ActionPolicyMgr::getActionRules(pid)     —— 品种->规则组->默认组回退
 * 以及 ActionRule 默认构造 / ActionType 枚举取值.
 *
 * 配置文件写在 temp_directory_path() 下的唯一目录里, 用例结束即删除.
 * 一条用例一个进程, 不共享任何静态状态.
 */
#include <gtest/gtest.h>

#include "WtUftCore/ActionPolicyMgr.h"

#include <filesystem>
#include <fstream>
#include <string>

USING_NS_WTP;

namespace {

// 每个用例一个唯一临时目录, 避免并行运行互相覆盖
class N03TempDir
{
public:
	N03TempDir(const char* tag)
	{
		_dir = std::filesystem::temp_directory_path()
			/ ("n03_" + std::string(tag) + "_" + std::to_string(::getpid()));
		std::error_code ec;
		std::filesystem::remove_all(_dir, ec);
		std::filesystem::create_directories(_dir, ec);
	}

	~N03TempDir()
	{
		std::error_code ec;
		std::filesystem::remove_all(_dir, ec);
	}

	std::string file(const char* name) const { return (_dir / name).string(); }

	void write(const std::string& path, const std::string& content) const
	{
		std::ofstream ofs(path);
		ofs << content;
		ofs.close();
	}

private:
	std::filesystem::path _dir;
};

const char* POLICY_JSON = R"JSON({
	"default": {
		"order": [
			{"action":"open",        "limit":500, "limit_s":0,   "limit_l":0,   "pure":false},
			{"action":"close",       "limit":400, "limit_s":200, "limit_l":300, "pure":true},
			{"action":"closetoday",  "limit":300, "limit_s":10,  "limit_l":20,  "pure":true},
			{"action":"closeyestoday","limit":200,"limit_s":30,  "limit_l":40,  "pure":false},
			{"action":"notanaction", "limit":1,   "limit_s":0,   "limit_l":0,   "pure":false}
		],
		"filters": ["SHFE.rb", "DCE.m", "CZCE.TA"]
	},
	"strict": {
		"order": [
			{"action":"OPEN",  "limit":10, "limit_s":5, "limit_l":6, "pure":true},
			{"action":"Close", "limit":9,  "limit_s":7, "limit_l":8, "pure":false}
		],
		"filters": ["CFFEX.IF"]
	},
	"noorder": {
		"filters": []
	},
	"justscalar": true
})JSON";

}

//////////////////////////////////////////////////////////////////////////
TEST(n03_action_policy, action_rule_defaults_and_enum_values)
{
	ActionRule rule;
	// memset(0) 构造: 三个限额与 pure 都为 0
	EXPECT_EQ(0u, rule._limit);
	EXPECT_EQ(0u, rule._limit_s);
	EXPECT_EQ(0u, rule._limit_l);
	EXPECT_FALSE(rule._pure);

	// 枚举取值是风控比较的基准, 固定下来防止被无意改动
	EXPECT_EQ(8888, AT_Unknown);
	EXPECT_EQ(9999, AT_Open);
	EXPECT_EQ(10000, AT_Close);
	EXPECT_EQ(10001, AT_CloseToday);
	EXPECT_EQ(10002, AT_CloseYestoday);

	ActionRuleGroup empty;
	EXPECT_TRUE(empty.empty());
}

TEST(n03_action_policy, init_fails_on_missing_file)
{
	N03TempDir tmp("nofile");
	ActionPolicyMgr mgr;
	EXPECT_FALSE(mgr.init(tmp.file("not_exists.json").c_str()));
}

TEST(n03_action_policy, init_loads_groups_and_filters)
{
	N03TempDir tmp("policy_ok");
	const std::string path = tmp.file("action_policy.json");
	tmp.write(path, POLICY_JSON);

	ActionPolicyMgr mgr;
	ASSERT_TRUE(mgr.init(path.c_str()));

	// 显式映射到 default 组的品种
	const ActionRuleGroup& def = mgr.getActionRules("SHFE.rb");
	ASSERT_EQ(4u, def.size()) << "未识别的 notanaction 应当被跳过";

	EXPECT_EQ(AT_Open, def[0]._atype);
	EXPECT_EQ(500u, def[0]._limit);
	EXPECT_EQ(0u, def[0]._limit_s);
	EXPECT_EQ(0u, def[0]._limit_l);
	EXPECT_FALSE(def[0]._pure);

	EXPECT_EQ(AT_Close, def[1]._atype);
	EXPECT_EQ(400u, def[1]._limit);
	EXPECT_EQ(200u, def[1]._limit_s);
	EXPECT_EQ(300u, def[1]._limit_l);
	EXPECT_TRUE(def[1]._pure);

	EXPECT_EQ(AT_CloseToday, def[2]._atype);
	EXPECT_EQ(300u, def[2]._limit);
	EXPECT_TRUE(def[2]._pure);

	EXPECT_EQ(AT_CloseYestoday, def[3]._atype);
	EXPECT_EQ(200u, def[3]._limit);
	EXPECT_FALSE(def[3]._pure);

	// 另一个映射组, 动作名大小写不敏感
	const ActionRuleGroup& strict = mgr.getActionRules("CFFEX.IF");
	ASSERT_EQ(2u, strict.size());
	EXPECT_EQ(AT_Open, strict[0]._atype);
	EXPECT_EQ(10u, strict[0]._limit);
	EXPECT_EQ(5u, strict[0]._limit_s);
	EXPECT_EQ(6u, strict[0]._limit_l);
	EXPECT_TRUE(strict[0]._pure);
	EXPECT_EQ(AT_Close, strict[1]._atype);
	EXPECT_EQ(9u, strict[1]._limit);

	// 未配置 filters 的品种回退到 default 组
	const ActionRuleGroup& fallback = mgr.getActionRules("SSE.600000");
	EXPECT_EQ(def.size(), fallback.size());
	EXPECT_EQ(AT_CloseToday, fallback[2]._atype);

	// 同品种二次查询稳定, 且回退路径的空 filters 组也落在 default
	const ActionRuleGroup& emptyGrp = mgr.getActionRules("DCE.m");
	EXPECT_EQ(4u, emptyGrp.size());
}

TEST(n03_action_policy, init_with_minimal_and_empty_rules)
{
	N03TempDir tmp("policy_min");
	const std::string path = tmp.file("min.json");
	// 只有一个空 default 组: 既没有 order 也没有 filters
	tmp.write(path, R"JSON({ "default": {} })JSON");

	ActionPolicyMgr mgr;
	ASSERT_TRUE(mgr.init(path.c_str()));
	EXPECT_TRUE(mgr.getActionRules("ANYTHING").empty());
}

TEST(n03_action_policy, init_only_filters_no_order)
{
	N03TempDir tmp("policy_nofilter");
	const std::string path = tmp.file("nofilter.json");
	// filters 缺失, order 存在
	tmp.write(path, R"JSON({
		"default": { "order": [ {"action":"close","limit":7,"limit_s":7,"limit_l":7,"pure":true} ] },
		"other":   { "order": [ {"action":"open","limit":3} ] }
	})JSON");

	ActionPolicyMgr mgr;
	ASSERT_TRUE(mgr.init(path.c_str()));
	const ActionRuleGroup& def = mgr.getActionRules("whatever");
	ASSERT_EQ(1u, def.size());
	EXPECT_EQ(AT_Close, def[0]._atype);
	EXPECT_EQ(7u, def[0]._limit);
	EXPECT_TRUE(def[0]._pure);
}

TEST(n03_action_policy, init_twice_is_additive)
{
	N03TempDir tmp("policy_twice");
	const std::string p1 = tmp.file("first.json");
	const std::string p2 = tmp.file("second.json");
	tmp.write(p1, R"JSON({ "default": { "order":[{"action":"open","limit":1}], "filters":["SHFE.rb"] } })JSON");
	tmp.write(p2, R"JSON({ "default": { "order":[{"action":"open","limit":2},{"action":"close","limit":3}], "filters":["DCE.m"] } })JSON");

	ActionPolicyMgr mgr;
	ASSERT_TRUE(mgr.init(p1.c_str()));
	EXPECT_EQ(1u, mgr.getActionRules("SHFE.rb").size());

	// _rules 是 map, 二次 init 追加而不是重建, default 组规则累加
	ASSERT_TRUE(mgr.init(p2.c_str()));
	const ActionRuleGroup& def = mgr.getActionRules("SHFE.rb");
	ASSERT_EQ(3u, def.size());
	EXPECT_EQ(1u, def[0]._limit);
	EXPECT_EQ(2u, def[1]._limit);
	EXPECT_EQ(3u, def[2]._limit);
	// 第二份配置的 filters 也生效
	EXPECT_EQ(2u, mgr.getActionRules("DCE.m").size());
}

TEST(n03_action_policy, unrecognized_action_types_are_skipped)
{
	N03TempDir tmp("policy_badaction");
	const std::string path = tmp.file("bad.json");
	tmp.write(path, R"JSON({
		"default": { "order": [
			{"action":"",        "limit":1},
			{"action":"opne",    "limit":2},
			{"action":"closetod","limit":3},
			{"action":"NULL",    "limit":4},
			{"action":"open",    "limit":5}
		] }
	})JSON");

	ActionPolicyMgr mgr;
	ASSERT_TRUE(mgr.init(path.c_str()));
	const ActionRuleGroup& def = mgr.getActionRules("SSE.600000");
	// 5 条里只有最后一条合法
	ASSERT_EQ(1u, def.size());
	EXPECT_EQ(AT_Open, def[0]._atype);
	EXPECT_EQ(5u, def[0]._limit);
}

//////////////////////////////////////////////////////////////////////////
// 缺陷类整体隔离: 配置里没有任何名为 "default" 的规则组时,
// getActionRules 先 find(gpName) 失败, 再 find("default") 仍然失败,
// 随后的 it->second 是对 end() 迭代器解引用( NDEBUG 下 assert 被裁掉就是野指针),
// 属于崩溃类. 与"pid 未映射""pid 为空串"是同一个缺陷类, 因此整组放进本 DISABLED_ 用例.
// 详见 docs/迭代计划/迭代01/bugs/n03_uftcore_index.md
TEST(DISABLED_n03_action_policy, no_default_group_deref_end_class)
{
	N03TempDir tmp("policy_nodefault");
	const std::string path = tmp.file("nodefault.json");
	tmp.write(path, R"JSON({ "onlygroup": { "order":[{"action":"open","limit":1}], "filters":["SHFE.rb"] } })JSON");

	ActionPolicyMgr mgr;
	ASSERT_TRUE(mgr.init(path.c_str()));
	// 三类输入命中同一缺陷: 显式映射组不存在 / 未映射回退 default / 空串
	mgr.getActionRules("SHFE.rb");
	mgr.getActionRules("SSE.600000");
	mgr.getActionRules("");
}
