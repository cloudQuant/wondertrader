// Lane R195 - 头文件内联面：由生产配置加载链驱动 WTSVariant / WTSCollection。
//
// 关键口径：这些用例不直接“调 header”，而是调用生产入口
//   WTSCfgLoader::load_from_content  -> json_to_variant / yaml_to_variant
//   WTSBaseDataMgr::loadSessions     -> WTSSessionInfo / WTSVariant 读取
// 只有这两条生产 TU（WTSCfgLoader.cpp、WTSBaseDataMgr.cpp）里实例化的 header 内联函数
// 才会计入覆盖率报表；测试 TU 自己调用不算。
//
// 覆盖目标（行号取自 src/Includes/WTSVariant.hpp 当前 HEAD）：
//   - 406-408/423-425/.../508-510  对象 append(name, 标量) 的“map 为空则建”防御体
//   - 520-532                    对象 append(name, 子对象)  —— json/yaml 嵌套对象
//   - 569-634                    数组 append(uint32/int64/uint64/double) —— JSON 数组数字类型
//   - 654-666                    数组 append(子对象) —— 数组里的对象/数组
//   - 169-170/187-188/241-242/259-260  asInt32/asUInt32/asDouble/asString 的 VT_Null 分支
//   - WTSCollection.hpp 155-160  WTSArray::append(WTSObject*, bool)（容器内联）
// 另覆盖 charconv.hpp 的 EncodingHelper::isUtf8 / ChartoUTF8 非 ASCII 路径：
//   load_from_content 对 GBK 内容先做编码识别与转换（WTSCfgLoader.cpp:266）。
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include "Includes/WTSVariant.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "WTSTools/WTSBaseDataMgr.h"
#include "WTSUtils/WTSCfgLoader.h"

USING_NS_WTP;

namespace {

std::string make_temp_text(const char* tag, const std::string& content)
{
	std::filesystem::path dir = std::filesystem::temp_directory_path()
		/ (std::string("r195_cfg_") + tag + "_" + std::to_string((long)::getpid()));
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	std::filesystem::path file = dir / "cfg.json";
	std::ofstream ofs(file.c_str(), std::ios::binary | std::ios::trunc);
	ofs.write(content.data(), (std::streamsize)content.size());
	ofs.close();
	return file.string();
}

}	// namespace

// JSON 全类型：rapidjson 的 IsInt/IsUint/IsInt64/IsUint64/IsDouble 五条分支
// 分别调用 WTSVariant 的五个 append 重载；嵌套对象/数组再走 append(WTSVariant*)。
TEST(r195_cfg_variant, json_all_number_types_and_nested_containers)
{
	const char* json = R"({
		"i32": -2147483647,
		"u32": 4294967295,
		"i64": -9223372036854775807,
		"u64": 18446744073709551615,
		"real": 3.25,
		"str": "text",
		"flagT": true,
		"flagF": false,
		"emptyObj": {},
		"emptyArr": [],
		"obj": {"x": 1, "sub": [1, 2.5, true, "s", {"n": 7}, [8, 9]]},
		"arr": [2147483647, 2147483648, -2147483649, 9223372036854775807,
				9223372036854775808, 2.5, false, "x", {"k": 2}, [3, 4]]
	})";

	WTSVariant* cfg = WTSCfgLoader::load_from_content(json, false);
	ASSERT_NE(cfg, nullptr);
	ASSERT_TRUE(cfg->isObject());

	EXPECT_EQ(cfg->get("i32")->asInt32(), -2147483647);
	EXPECT_EQ(cfg->get("u32")->asUInt32(), 4294967295u);
	EXPECT_EQ(cfg->get("i64")->asInt64(), -9223372036854775807LL);
	EXPECT_EQ(cfg->get("u64")->asUInt64(), 18446744073709551615ULL);
	EXPECT_DOUBLE_EQ(cfg->get("real")->asDouble(), 3.25);
	EXPECT_EQ(cfg->get("str")->asString(), std::string("text"));
	EXPECT_TRUE(cfg->get("flagT")->asBoolean());
	EXPECT_FALSE(cfg->get("flagF")->asBoolean());
	EXPECT_EQ(cfg->get("emptyObj")->size(), 0u);
	EXPECT_EQ(cfg->get("emptyArr")->size(), 0u);

	WTSVariant* obj = cfg->get("obj");
	ASSERT_NE(obj, nullptr);
	ASSERT_TRUE(obj->isObject());
	EXPECT_EQ(obj->get("x")->asInt32(), 1);
	WTSVariant* sub = obj->get("sub");
	ASSERT_NE(sub, nullptr);
	EXPECT_EQ(sub->size(), 6u);
	EXPECT_EQ(sub->get((uint32_t)4)->get("n")->asInt32(), 7);
	EXPECT_EQ(sub->get((uint32_t)5)->get((uint32_t)1)->asInt32(), 9);

	WTSVariant* arr = cfg->get("arr");
	ASSERT_NE(arr, nullptr);
	ASSERT_TRUE(arr->isArray());
	ASSERT_EQ(arr->size(), 10u);
	EXPECT_EQ(arr->get((uint32_t)0)->asInt32(), 2147483647);
	EXPECT_EQ(arr->get((uint32_t)1)->asUInt32(), 2147483648u);
	EXPECT_EQ(arr->get((uint32_t)2)->asInt64(), -2147483649LL);
	EXPECT_EQ(arr->get((uint32_t)3)->asInt64(), 9223372036854775807LL);
	EXPECT_EQ(arr->get((uint32_t)4)->asUInt64(), 9223372036854775808ULL);
	EXPECT_DOUBLE_EQ(arr->get((uint32_t)5)->asDouble(), 2.5);
	EXPECT_FALSE(arr->get((uint32_t)6)->asBoolean());
	EXPECT_EQ(arr->get((uint32_t)7)->asString(), std::string("x"));
	EXPECT_EQ(arr->get((uint32_t)8)->get("k")->asInt32(), 2);
	EXPECT_EQ(arr->get((uint32_t)9)->get((uint32_t)0)->asInt32(), 3);
	cfg->release();
}

// YAML 嵌套 map/sequence：yaml_to_variant 对子节点走 append(name, WTSVariant*)
// 与 append(WTSVariant*)，标量一律按字符串写入。
TEST(r195_cfg_variant, yaml_nested_map_and_sequence)
{
	const char* yaml =
		"root:\n"
		"  name: alpha\n"
		"  n: 5\n"
		"  child:\n"
		"    x: 1\n"
		"    list:\n"
		"      - 1\n"
		"      - two\n"
		"      - k: v\n"
		"        arr: [1, 2]\n"
		"  seq:\n"
		"    - a\n"
		"    - b: c\n";

	WTSVariant* cfg = WTSCfgLoader::load_from_content(yaml, true);
	ASSERT_NE(cfg, nullptr);
	WTSVariant* root = cfg->get("root");
	ASSERT_NE(root, nullptr);
	EXPECT_EQ(root->get("name")->asString(), std::string("alpha"));
	EXPECT_EQ(root->get("n")->asString(), std::string("5"));

	WTSVariant* child = root->get("child");
	ASSERT_NE(child, nullptr);
	EXPECT_EQ(child->get("x")->asString(), std::string("1"));
	WTSVariant* list = child->get("list");
	ASSERT_NE(list, nullptr);
	ASSERT_EQ(list->size(), 3u);
	EXPECT_EQ(list->get((uint32_t)1)->asString(), std::string("two"));
	EXPECT_EQ(list->get((uint32_t)2)->get("k")->asString(), std::string("v"));
	EXPECT_EQ(list->get((uint32_t)2)->get("arr")->get((uint32_t)1)->asString(), std::string("2"));

	WTSVariant* seq = root->get("seq");
	ASSERT_NE(seq, nullptr);
	ASSERT_EQ(seq->size(), 2u);
	EXPECT_EQ(seq->get((uint32_t)0)->asString(), std::string("a"));
	EXPECT_EQ(seq->get((uint32_t)1)->get("b")->asString(), std::string("c"));
	cfg->release();
}

// GBK 内容：WTSCfgLoader::load_from_content 先判 isUtf8=false，再走 ChartoUTF8
// (iconv gb2312->utf-8) 转换，随后 JSON 解析成功。ASCII 与空/坏内容走其它分支。
TEST(r195_cfg_variant, gbk_content_is_converted_before_json_parse)
{
	std::string gbk = "{\"name\":\"";
	gbk += "\xD6\xD0\xCE\xC4";	// GBK: 中文
	gbk += "\"}";

	WTSVariant* cfg = WTSCfgLoader::load_from_content(gbk, false);
	ASSERT_NE(cfg, nullptr);
	EXPECT_EQ(cfg->getString("name"), std::string("\xE4\xB8\xAD\xE6\x96\x87"));
	cfg->release();

	// 空内容 / 语法错误：load_from_content 返回 NULL
	EXPECT_EQ(WTSCfgLoader::load_from_content("", false), nullptr);
	EXPECT_EQ(WTSCfgLoader::load_from_content("{bad", false), nullptr);
	// 根是数组但目标容器是对象：json_to_variant 的容器类型守卫返回 false
	EXPECT_EQ(WTSCfgLoader::load_from_content("[]", false), nullptr);
	// 根是 sequence 但目标容器是对象：yaml_to_variant 的容器类型守卫返回 false
	EXPECT_EQ(WTSCfgLoader::load_from_content("- a\n- b\n", true), nullptr);
}

// 生产入口 WTSBaseDataMgr::loadSessions：JSON 里的 null 标量会让
// getCString/getInt32/getUInt32 落到 WTSVariant 的 VT_Null 分支；
// 同时走 WTSSessionInfo::create/addTradingSection 的真实构造路径。
TEST(r195_cfg_variant, base_data_mgr_sessions_null_scalars)
{
	const char* json = R"({
		"N1": {"name": null, "offset": null, "sections": [{"from": null, "to": 1130}]},
		"N2": {"name": "night", "offset": 180, "sections": [{"from": 2100, "to": 2300}]}
	})";
	const std::string file = make_temp_text("sess", json);

	WTSBaseDataMgr mgr;
	ASSERT_TRUE(mgr.loadSessions(file.c_str()));

	WTSSessionInfo* s1 = mgr.getSession("N1");
	ASSERT_NE(s1, nullptr);
	EXPECT_EQ(s1->getOffsetMins(), 0);
	ASSERT_EQ(s1->getSectionCount(), 1u);
	EXPECT_EQ(s1->getTradingSections()[0].first_raw, 0u);
	EXPECT_EQ(s1->getTradingSections()[0].second_raw, 1130u);

	WTSSessionInfo* s2 = mgr.getSession("N2");
	ASSERT_NE(s2, nullptr);
	EXPECT_EQ(s2->getOffsetMins(), 180);
	EXPECT_EQ(s2->getOpenTime(), 2100u);
	EXPECT_EQ(s2->getCloseTime(), 2300u);

	std::error_code ec;
	std::filesystem::remove_all(std::filesystem::path(file).parent_path(), ec);
}
