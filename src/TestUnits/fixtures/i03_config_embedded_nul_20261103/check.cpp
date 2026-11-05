#include <gtest/gtest.h>
#include <fstream>
#include <string>
#include <unistd.h>
#include "WTSUtils/WTSCfgLoader.h"
#include "Includes/WTSVariant.hpp"

static std::string writeConfig(const std::string& content, const char* suffix)
{
    char path[] = "/tmp/wt-i03-nul-config-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return {};
    close(fd);
    std::string named = std::string(path) + suffix;
    if (rename(path, named.c_str()) != 0) return {};
    std::ofstream out(named, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return named;
}

TEST(I03ConfigNul, JsonKeysValuesAndNestedArraysRejectAtomically)
{
    for (const std::string& input : {
            R"({"good":1,"bad\u0000key":2})",
            R"({"good":1,"bad":"before\u0000after"})",
            R"({"groups":[{"values":["ok", "prefix\u0000suffix"]}]})",
            R"({"outer":[{"name\u0000x":"value"}]})"}) {
        EXPECT_EQ(nullptr, WTSCfgLoader::load_from_content(input));
        EXPECT_EQ(nullptr, WTSCfgLoader::load_from_normalized_content(input));
        auto path = writeConfig(input, ".json"); ASSERT_FALSE(path.empty());
        EXPECT_EQ(nullptr, WTSCfgLoader::load_from_file(path));
        unlink(path.c_str());
    }
}

TEST(I03ConfigNul, YamlKeysValuesAndNestedArraysRejectAtomically)
{
    for (const std::string& input : {
            "good: 1\n\"bad\\0key\": 2\n",
            "good: 1\nbad: \"before\\0after\"\n",
            "groups:\n  - values: [ok, \"prefix\\0suffix\"]\n",
            "outer:\n  - \"name\\0x\": value\n"}) {
        EXPECT_EQ(nullptr, WTSCfgLoader::load_from_content(input, true));
        EXPECT_EQ(nullptr, WTSCfgLoader::load_from_normalized_content(input, true));
        auto path = writeConfig(input, ".yaml"); ASSERT_FALSE(path.empty());
        EXPECT_EQ(nullptr, WTSCfgLoader::load_from_file(path));
        unlink(path.c_str());
    }
}

TEST(I03ConfigNul, RawNulInputDoesNotTruncate)
{
    for (bool yaml : {false, true}) {
        std::string input = yaml ? "good: value\n" : "{\"good\":1}";
        input.push_back('\0');
        input += "ignored";
        EXPECT_EQ(nullptr, WTSCfgLoader::load_from_content(input, yaml));
        EXPECT_EQ(nullptr, WTSCfgLoader::load_from_normalized_content(input, yaml));
        auto path = writeConfig(input, yaml ? ".yaml" : ".json"); ASSERT_FALSE(path.empty());
        EXPECT_EQ(nullptr, WTSCfgLoader::load_from_file(path));
        unlink(path.c_str());
    }
}

TEST(I03ConfigNul, OrdinaryUnicodeEscapesAndNullDroppingRetained)
{
    auto* json = WTSCfgLoader::load_from_content(R"({"中文":"你好\nworld","escaped":"\u4e2d","null":null,"items":["ok",null,"二"]})");
    ASSERT_NE(nullptr, json);
    EXPECT_EQ(std::string("你好\nworld"), json->getString("中文"));
    EXPECT_EQ(std::string("中"), json->getString("escaped"));
    EXPECT_FALSE(json->has("null"));
    ASSERT_NE(nullptr, json->get("items"));
    EXPECT_EQ(2u, json->get("items")->size());
    json->release();
    auto* yaml = WTSCfgLoader::load_from_content("中文: \"你好\\nworld\"\nescaped: \"\\u4e2d\"\nnothing: null\n", true);
    ASSERT_NE(nullptr, yaml);
    EXPECT_EQ(std::string("你好\nworld"), yaml->getString("中文"));
    EXPECT_EQ(std::string("中"), yaml->getString("escaped"));
    EXPECT_FALSE(yaml->has("nothing"));
    yaml->release();
}

TEST(I03ConfigNul, OrdinaryDuplicateKeysKeepExistingLastValue)
{
    auto* json = WTSCfgLoader::load_from_content(R"({"same":"first","same":"second"})");
    ASSERT_NE(nullptr, json);
    EXPECT_EQ("second", json->getString("same"));
    json->release();
    auto* yaml = WTSCfgLoader::load_from_content("same: first\nsame: second\n", true);
    ASSERT_NE(nullptr, yaml);
    EXPECT_EQ("second", yaml->getString("same"));
    yaml->release();
}
