/*
 * WS05 - WTSUtils 测试：WTSCfgLoader（JSON/YAML/编码/文件加载）
 *
 * 只读生产代码；本文件仅覆盖公开 API 及无副作用的内部转换函数。
 * 缺陷记录见 docs/迭代计划/迭代01/bugs/ws05_wtsutils.md。
 */
#include <gtest/gtest.h>

#include "WTSUtils/WTSCfgLoader.h"
#include "Includes/WTSVariant.hpp"

#include <rapidjson/document.h>
#include "WTSUtils/yamlcpp/yaml.h"

#include <filesystem>
#include <fstream>
#include <string>

// WTSCfgLoader.cpp 中定义的内部转换函数（外部链接）。
// 直接从测试调用它们，用于覆盖 load_from_* 无法触发的“根节点类型不匹配”分支。
bool json_to_variant(const rapidjson::Value& root, WTSVariant* params);
bool yaml_to_variant(const YAML::Node& root, WTSVariant* params);

namespace {

std::filesystem::path ws05_make_temp_dir(const char* tag)
{
	static int s_seq = 0;
	std::filesystem::path p = std::filesystem::temp_directory_path()
		/ (std::string("ws05_cfg_") + tag + "_" + std::to_string(++s_seq));
	std::error_code ec;
	std::filesystem::remove_all(p, ec);
	std::filesystem::create_directories(p, ec);
	return p;
}

void ws05_write_file(const std::filesystem::path& p, const std::string& content)
{
	std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(ofs.is_open());
	ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
	ofs.close();
}

// GB2312 编码的“中文”：D6 D0 CE C4
std::string ws05_gbk_cn()
{
	std::string s;
	s.push_back(static_cast<char>(0xD6));
	s.push_back(static_cast<char>(0xD0));
	s.push_back(static_cast<char>(0xCE));
	s.push_back(static_cast<char>(0xC4));
	return s;
}

// GBK 编码的 JSON 片段 {"name":"中文"}
std::string ws05_gbk_json()
{
	return std::string("{\"name\":\"") + ws05_gbk_cn() + "\"}";
}

// GBK 编码的 YAML 片段 name: 中文
std::string ws05_gbk_yaml()
{
	return std::string("name: ") + ws05_gbk_cn() + "\n";
}

const char* const kCn = "\xE4\xB8\xAD\xE6\x96\x87";	// “中文”的 UTF-8 字节

} // namespace

//////////////////////////////////////////////////////////////////////////
// JSON

TEST(ws05_cfgloader, ParseJsonScalarTypes)
{
	// 覆盖 rapidjson 的 int/uint/int64/uint64/double/string/bool/null 分支
	const char* content =
		"{"
		"\"i32\": -12,"
		"\"u32\": 3000000000,"
		"\"i64\": -3000000000,"
		"\"u64\": 18446744073709551615,"
		"\"real\": 1.25,"
		"\"str\": \"hello\","
		"\"btrue\": true,"
		"\"bfalse\": false,"
		"\"nul\": null"
		"}";

	WTSVariant* doc = WTSCfgLoader::load_from_content(content);
	ASSERT_NE(doc, nullptr);
	ASSERT_EQ(doc->type(), WTSVariant::VT_Object);

	EXPECT_EQ(doc->getInt32("i32"), -12);
	EXPECT_EQ(doc->getUInt32("u32"), 3000000000u);
	EXPECT_EQ(doc->getInt64("i64"), -3000000000LL);
	EXPECT_EQ(doc->getUInt64("u64"), 18446744073709551615ull);
	EXPECT_DOUBLE_EQ(doc->getDouble("real"), 1.25);
	EXPECT_EQ(doc->getString("str"), "hello");
	EXPECT_TRUE(doc->getBoolean("btrue"));
	EXPECT_FALSE(doc->getBoolean("bfalse"));
	// null 值被跳过，键不存在
	EXPECT_FALSE(doc->has("nul"));

	doc->release();
}

TEST(ws05_cfgloader, ParseJsonNestedObjectAndArray)
{
	const char* content =
		"{"
		"\"obj\": {\"a\": 1, \"sub\": {\"b\": 2}},"
		"\"arr\": [1, \"two\", true, null, [2, 3], {\"b\": 3}]"
		"}";

	WTSVariant* doc = WTSCfgLoader::load_from_content(content);
	ASSERT_NE(doc, nullptr);

	WTSVariant* obj = doc->get("obj");
	ASSERT_NE(obj, nullptr);
	EXPECT_EQ(obj->type(), WTSVariant::VT_Object);
	EXPECT_EQ(obj->getInt32("a"), 1);
	WTSVariant* sub = obj->get("sub");
	ASSERT_NE(sub, nullptr);
	EXPECT_EQ(sub->getInt32("b"), 2);

	WTSVariant* arr = doc->get("arr");
	ASSERT_NE(arr, nullptr);
	EXPECT_EQ(arr->type(), WTSVariant::VT_Array);
	// null 元素被跳过，共 5 个元素
	ASSERT_EQ(arr->size(), 5u);
	EXPECT_EQ(arr->get(0u)->asInt32(), 1);
	EXPECT_EQ(arr->get(1u)->asString(), "two");
	EXPECT_TRUE(arr->get(2u)->asBoolean());
	WTSVariant* inner = arr->get(3u);
	ASSERT_NE(inner, nullptr);
	EXPECT_EQ(inner->type(), WTSVariant::VT_Array);
	ASSERT_EQ(inner->size(), 2u);
	EXPECT_EQ(inner->get(0u)->asInt32(), 2);
	EXPECT_EQ(inner->get(1u)->asInt32(), 3);
	WTSVariant* last = arr->get(4u);
	ASSERT_NE(last, nullptr);
	EXPECT_EQ(last->type(), WTSVariant::VT_Object);
	EXPECT_EQ(last->getInt32("b"), 3);

	doc->release();
}

TEST(ws05_cfgloader, RejectJsonTopLevelArray)
{
	// load_from_json 固定创建 Object，数组根节点直接判定失败
	WTSVariant* doc = WTSCfgLoader::load_from_content("[1, 2, 3]");
	EXPECT_EQ(doc, nullptr);
}

TEST(ws05_cfgloader, RejectJsonInvalidAndEmpty)
{
	EXPECT_EQ(WTSCfgLoader::load_from_content("{ invalid json"), nullptr);
	EXPECT_EQ(WTSCfgLoader::load_from_content(""), nullptr);
	EXPECT_EQ(WTSCfgLoader::load_from_content("{} trailing"), nullptr);
}

TEST(ws05_cfgloader, InternalJsonToVariantTypeMismatch)
{
	// 对象根节点写入数组容器 -> false（覆盖 json_to_variant 第 36-37 行）
	rapidjson::Document objDoc;
	objDoc.Parse("{\"a\": 1}");
	ASSERT_FALSE(objDoc.HasParseError());

	WTSVariant* arr = WTSVariant::createArray();
	EXPECT_FALSE(json_to_variant(objDoc, arr));
	arr->release();

	// 数组根节点写入对象容器 -> false（覆盖 json_to_variant 第 39-40 行）
	rapidjson::Document arrDoc;
	arrDoc.Parse("[1]");
	ASSERT_FALSE(arrDoc.HasParseError());

	WTSVariant* obj = WTSVariant::createObject();
	EXPECT_FALSE(json_to_variant(arrDoc, obj));
	obj->release();

	// 匹配的容器类型返回 true
	rapidjson::Document okDoc;
	okDoc.Parse("{\"a\": 1}");
	WTSVariant* okObj = WTSVariant::createObject();
	EXPECT_TRUE(json_to_variant(okDoc, okObj));
	EXPECT_EQ(okObj->getInt32("a"), 1);
	okObj->release();
}

//////////////////////////////////////////////////////////////////////////
// YAML

TEST(ws05_cfgloader, ParseYamlMapAndSequence)
{
	const char* content =
		"name: wt\n"
		"level: 3\n"
		"flag: true\n"
		"nothing: null\n"
		"nested:\n"
		"  a: 1\n"
		"  b:\n"
		"    - 10\n"
		"    - 20\n"
		"list:\n"
		"  - one\n"
		"  - two\n";

	WTSVariant* doc = WTSCfgLoader::load_from_content(content, true);
	ASSERT_NE(doc, nullptr);
	ASSERT_EQ(doc->type(), WTSVariant::VT_Object);

	EXPECT_EQ(doc->getString("name"), "wt");
	EXPECT_EQ(doc->getInt32("level"), 3);
	EXPECT_TRUE(doc->getBoolean("flag"));
	// null 节点被跳过
	EXPECT_FALSE(doc->has("nothing"));

	WTSVariant* nested = doc->get("nested");
	ASSERT_NE(nested, nullptr);
	EXPECT_EQ(nested->getInt32("a"), 1);
	WTSVariant* b = nested->get("b");
	ASSERT_NE(b, nullptr);
	ASSERT_EQ(b->type(), WTSVariant::VT_Array);
	ASSERT_EQ(b->size(), 2u);
	EXPECT_EQ(b->get(0u)->asString(), "10");
	EXPECT_EQ(b->get(1u)->asString(), "20");

	WTSVariant* list = doc->get("list");
	ASSERT_NE(list, nullptr);
	ASSERT_EQ(list->type(), WTSVariant::VT_Array);
	ASSERT_EQ(list->size(), 2u);
	EXPECT_EQ(list->get(0u)->asString(), "one");
	EXPECT_EQ(list->get(1u)->asString(), "two");

	doc->release();
}

TEST(ws05_cfgloader, RejectYamlTopLevelSequence)
{
	WTSVariant* doc = WTSCfgLoader::load_from_content("- a\n- b\n", true);
	EXPECT_EQ(doc, nullptr);
}

TEST(ws05_cfgloader, RejectYamlEmptyAndNull)
{
	EXPECT_EQ(WTSCfgLoader::load_from_content("", true), nullptr);
	EXPECT_EQ(WTSCfgLoader::load_from_content("---\n", true), nullptr);
}

TEST(ws05_cfgloader, InternalYamlToVariantNullRoot)
{
	// Null 根节点写入数组容器 -> false（覆盖 yaml_to_variant 第 175-176 行）
	YAML::Node nullNode(YAML::NodeType::Null);
	WTSVariant* arr = WTSVariant::createArray();
	EXPECT_FALSE(yaml_to_variant(nullNode, arr));
	arr->release();

	// Null 根节点写入对象容器 -> 空对象，返回 true（Null 节点不可迭代）
	WTSVariant* obj = WTSVariant::createObject();
	EXPECT_TRUE(yaml_to_variant(nullNode, obj));
	EXPECT_EQ(obj->size(), 0u);
	obj->release();
}

TEST(ws05_cfgloader, YamlSyntaxErrorThrows)
{
	// 语法错误时 yaml-cpp 抛出异常，load_from_yaml 不做捕获
	EXPECT_THROW(WTSCfgLoader::load_from_content("a: [1, 2\n", true), YAML::ParserException);
}

//////////////////////////////////////////////////////////////////////////
// 编码

TEST(ws05_cfgloader, ContentUtf8Chinese)
{
	std::string content = std::string("{\"name\":\"") + kCn + "\"}";
	WTSVariant* doc = WTSCfgLoader::load_from_content(content);
	ASSERT_NE(doc, nullptr);
	EXPECT_EQ(doc->getString("name"), kCn);
	doc->release();

	std::string yaml = std::string("name: ") + kCn + "\n";
	WTSVariant* ydoc = WTSCfgLoader::load_from_content(yaml, true);
	ASSERT_NE(ydoc, nullptr);
	EXPECT_EQ(ydoc->getString("name"), kCn);
	ydoc->release();
}

TEST(ws05_cfgloader, ContentGbkChineseConverted)
{
	// 非 UTF-8 内容会在 POSIX 平台走 ChartoUTF8 分支
	std::string content = ws05_gbk_json();
	WTSVariant* doc = WTSCfgLoader::load_from_content(content);
	ASSERT_NE(doc, nullptr);
	EXPECT_EQ(doc->getString("name"), kCn);
	doc->release();

	std::string yaml = ws05_gbk_yaml();
	WTSVariant* ydoc = WTSCfgLoader::load_from_content(yaml, true);
	ASSERT_NE(ydoc, nullptr);
	EXPECT_EQ(ydoc->getString("name"), kCn);
	ydoc->release();
}

//////////////////////////////////////////////////////////////////////////
// 文件加载

class ws05_CfgLoaderFile : public ::testing::Test
{
protected:
	void SetUp() override { _dir = ws05_make_temp_dir("file"); }
	void TearDown() override
	{
		std::error_code ec;
		std::filesystem::remove_all(_dir, ec);
	}

	std::filesystem::path _dir;
};

TEST_F(ws05_CfgLoaderFile, MissingFileReturnsNull)
{
	EXPECT_EQ(WTSCfgLoader::load_from_file((_dir / "not_exists.json").string()), nullptr);
	EXPECT_EQ(WTSCfgLoader::load_from_file(std::string("/no/such/dir/ws05.json")), nullptr);
}

TEST_F(ws05_CfgLoaderFile, EmptyFileReturnsNull)
{
	const std::filesystem::path p = _dir / "empty.json";
	ws05_write_file(p, "");
	EXPECT_EQ(WTSCfgLoader::load_from_file(p.string()), nullptr);
}

TEST_F(ws05_CfgLoaderFile, LoadJsonFile)
{
	const std::filesystem::path p = _dir / "conf.json";
	ws05_write_file(p, "{\"level\": 7, \"name\": \"a\"}");

	WTSVariant* doc = WTSCfgLoader::load_from_file(p.string());
	ASSERT_NE(doc, nullptr);
	EXPECT_EQ(doc->getInt32("level"), 7);
	EXPECT_EQ(doc->getString("name"), "a");
	doc->release();

	// .JSON 扩展名（endsWith 默认忽略大小写）
	const std::filesystem::path up = _dir / "conf2.JSON";
	ws05_write_file(up, "{\"level\": 8}");
	WTSVariant* updoc = WTSCfgLoader::load_from_file(up.string());
	ASSERT_NE(updoc, nullptr);
	EXPECT_EQ(updoc->getInt32("level"), 8);
	updoc->release();
}

TEST_F(ws05_CfgLoaderFile, LoadYamlAndYmlFiles)
{
	const std::filesystem::path yaml = _dir / "conf.yaml";
	ws05_write_file(yaml, "name: wt\nlevel: 9\n");
	WTSVariant* doc = WTSCfgLoader::load_from_file(yaml.string());
	ASSERT_NE(doc, nullptr);
	EXPECT_EQ(doc->getString("name"), "wt");
	EXPECT_EQ(doc->getInt32("level"), 9);
	doc->release();

	const std::filesystem::path yml = _dir / "conf.yml";
	ws05_write_file(yml, "name: wt2\n");
	WTSVariant* doc2 = WTSCfgLoader::load_from_file(yml.string());
	ASSERT_NE(doc2, nullptr);
	EXPECT_EQ(doc2->getString("name"), "wt2");
	doc2->release();
}

TEST_F(ws05_CfgLoaderFile, UnknownExtensionReturnsNull)
{
	const std::filesystem::path p = _dir / "conf.txt";
	ws05_write_file(p, "{\"level\": 1}");
	EXPECT_EQ(WTSCfgLoader::load_from_file(p.string()), nullptr);
}

TEST_F(ws05_CfgLoaderFile, GbkEncodedFilesAreConverted)
{
	const std::filesystem::path json = _dir / "gbk.json";
	ws05_write_file(json, ws05_gbk_json());
	WTSVariant* doc = WTSCfgLoader::load_from_file(json.string());
	ASSERT_NE(doc, nullptr);
	EXPECT_EQ(doc->getString("name"), kCn);
	doc->release();

	const std::filesystem::path yaml = _dir / "gbk.yml";
	ws05_write_file(yaml, ws05_gbk_yaml());
	WTSVariant* ydoc = WTSCfgLoader::load_from_file(yaml.string());
	ASSERT_NE(ydoc, nullptr);
	EXPECT_EQ(ydoc->getString("name"), kCn);
	ydoc->release();
}
