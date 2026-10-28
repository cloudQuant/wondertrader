/*!
 * \file r19_shareblock_capi_test.cpp
 * \brief lane R19：WtShareHelper 的 extern "C" 包装层（src/WtShareHelper/WtShareHelper.cpp）
 *
 * 覆盖 L6-29（init_master/init_slave/update_slave/release_slave/register_logger）、
 * L31-47（get_sections/get_keys 的回调遍历与计数）、L49-62（updatetime/commit/delete）、
 * L64-92（六个 allocate_*）、L94-122（六个 set_*）、L124-152（六个 get_*）、
 * L154-167（init_cmder/add_cmd/get_cmd）。
 * 这些包装只是转发到 ShareBlocks::one()，所以断言重点放在
 * 「返回值/回调次数与入参」是否和喂进去的输入算得一致。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "r19_support.h"

#include "WtShareHelper/ShareBlocks.h"
#include "WtShareHelper/WtShareHelper.h"

using namespace shareblock;

namespace {

std::vector<std::string> g_sections;
std::vector<std::string> g_keys;
std::vector<uint64_t> g_key_types;

void PORTER_FLAG collect_section(const char* name)
{
	g_sections.push_back(name == NULL ? std::string("<null>") : std::string(name));
}

void PORTER_FLAG collect_key(const char* key, uint64_t type)
{
	g_keys.push_back(key == NULL ? std::string("<null>") : std::string(key));
	g_key_types.push_back(type);
}

} // namespace

TEST_F(R19TempDir, r19_capi_master_slave_and_all_type_wrappers)
{
	r19::attach_logger();
	const std::string dom = r19::domain("f1m");
	const std::string domS = r19::domain("f1s");
	const std::string ghost = r19::domain("f1ghost");
	const std::string path = file("f1.blk");

	EXPECT_TRUE(::init_master(dom.c_str(), path.c_str()));
	EXPECT_FALSE(::init_slave(domS.c_str(), file("nope.blk").c_str()))
		<< "L11-14：slave 侧文件不存在必须返回 false";

	// 六个 allocate_*（L64-92）
	EXPECT_EQ(-5, *::allocate_int32(dom.c_str(), "s", "i32", -5));
	EXPECT_EQ(-6000000000LL, *::allocate_int64(dom.c_str(), "s", "i64", -6000000000LL));
	EXPECT_EQ(4000000000u, *::allocate_uint32(dom.c_str(), "s", "u32", 4000000000u));
	EXPECT_EQ(18446744073709551614ULL, *::allocate_uint64(dom.c_str(), "s", "u64", 18446744073709551614ULL));
	EXPECT_DOUBLE_EQ(-7.25, *::allocate_double(dom.c_str(), "s", "dbl", -7.25));
	EXPECT_STREQ("capi", ::allocate_string(dom.c_str(), "s", "str", "capi"));

	// 六个 set_*（L94-122）
	EXPECT_TRUE(::set_int32(dom.c_str(), "s", "i32", 1));
	EXPECT_TRUE(::set_int64(dom.c_str(), "s", "i64", 2));
	EXPECT_TRUE(::set_uint32(dom.c_str(), "s", "u32", 3u));
	EXPECT_TRUE(::set_uint64(dom.c_str(), "s", "u64", 4ULL));
	EXPECT_TRUE(::set_double(dom.c_str(), "s", "dbl", 5.5));
	EXPECT_TRUE(::set_string(dom.c_str(), "s", "str", "six"));

	// 六个 get_*（L124-152）
	EXPECT_EQ(1, ::get_int32(dom.c_str(), "s", "i32", -99));
	EXPECT_EQ(2, ::get_int64(dom.c_str(), "s", "i64", -99));
	EXPECT_EQ(3u, ::get_uint32(dom.c_str(), "s", "u32", 99u));
	EXPECT_EQ(4ULL, ::get_uint64(dom.c_str(), "s", "u64", 99ULL));
	EXPECT_DOUBLE_EQ(5.5, ::get_double(dom.c_str(), "s", "dbl", -1.0));
	EXPECT_STREQ("six", ::get_string(dom.c_str(), "s", "str", "?"));

	// slave 名字映射同一份文件 → 读到的是同一批值
	ASSERT_TRUE(::init_slave(domS.c_str(), path.c_str()));
	EXPECT_EQ(1, ::get_int32(domS.c_str(), "s", "i32", -99));
	EXPECT_STREQ("six", ::get_string(domS.c_str(), "s", "str", "?"));

	// 未知域：get_* 一律回落调用方给的默认值
	EXPECT_EQ(-77, ::get_int32(ghost.c_str(), "s", "i32", -77));
	EXPECT_EQ(0u, ::get_uint32(ghost.c_str(), "s", "u32", 0u));
	EXPECT_STREQ("dd", ::get_string(ghost.c_str(), "s", "str", "dd"));
	EXPECT_FALSE(::set_int32(ghost.c_str(), "s", "i32", 1));
	EXPECT_EQ(nullptr, ::allocate_int32(ghost.c_str(), "s", "nx", 1));

	// L16-19 update_slave：master 域的 _blocktime 未被 init_master 赋过值
	// （_ShmPair 构造没初始化它，见 BUG-R19-009），所以只断言强制刷新这条确定路径
	EXPECT_TRUE(::update_slave(dom.c_str(), true));
	EXPECT_FALSE(::update_slave(ghost.c_str(), true)) << "L133-134：_block 为空";

	// L21-24 release_slave：slave 域可释放，master 域拒绝
	EXPECT_TRUE(::release_slave(domS.c_str()));
	EXPECT_FALSE(::release_slave(dom.c_str()));
	EXPECT_TRUE(::release_slave(ghost.c_str())) << "L168-169：未知域直接 true";

	// L49-62：时间戳/提交/删除
	EXPECT_EQ(0u, ::get_section_updatetime(dom.c_str(), "nosection"));
	EXPECT_TRUE(::commit_section(dom.c_str(), "s"));
	EXPECT_GT(::get_section_updatetime(dom.c_str(), "s"), 0u);
	EXPECT_FALSE(::commit_section(ghost.c_str(), "s"));
	EXPECT_TRUE(::delete_section(dom.c_str(), "s"));
	EXPECT_TRUE(::delete_section(dom.c_str(), "already_gone"));
	EXPECT_FALSE(::delete_section(ghost.c_str(), "s"));

	// L26-29 register_logger(NULL) → write_log 的空 logger 早退分支，且不得崩溃
	const size_t seen = r19::log_count();
	::register_logger(NULL);
	EXPECT_FALSE(::commit_section(ghost.c_str(), "s"));
	EXPECT_FALSE(::set_int32(ghost.c_str(), "s", "k", 1));
	EXPECT_EQ(seen, r19::log_count()) << "logger 为空时 write_log 必须静默（ShareBlocks.h L127-130）";
	::register_logger(r19::sb_logger);
	EXPECT_FALSE(::commit_section(ghost.c_str(), "s"));
	EXPECT_TRUE(r19::has_log("not initialized"));
}

TEST_F(R19TempDir, r19_capi_enum_callbacks_count_and_args)
{
	r19::attach_logger();
	const std::string dom = r19::domain("f2");
	const std::string ghost = r19::domain("f2ghost");
	const std::string path = file("f2.blk");
	ASSERT_TRUE(::init_master(dom.c_str(), path.c_str()));

	// 两个段，各自 2 个 key，类型故意混着放
	ASSERT_NE(nullptr, ::allocate_int32(dom.c_str(), "secA", "a1", 10));
	ASSERT_NE(nullptr, ::allocate_string(dom.c_str(), "secA", "a2", "aa2"));
	ASSERT_NE(nullptr, ::allocate_uint64(dom.c_str(), "secB", "b1", 20ULL));
	ASSERT_NE(nullptr, ::allocate_double(dom.c_str(), "secB", "b2", 1.5));

	g_sections.clear();
	const uint32_t nsec = ::get_sections(dom.c_str(), collect_section);
	EXPECT_EQ(2u, nsec) << "L31-38：返回值就是段数";
	ASSERT_EQ(2u, g_sections.size());
	EXPECT_EQ("secA", g_sections[0]);
	EXPECT_EQ("secB", g_sections[1]) << "段按块内槽位顺序回调";

	g_keys.clear(); g_key_types.clear();
	const uint32_t nkey = ::get_keys(dom.c_str(), "secA", collect_key);
	EXPECT_EQ(2u, nkey);
	ASSERT_EQ(2u, g_keys.size());
	// wt_hashmap 的遍历顺序不保证，按内容校验
	for (size_t i = 0; i < g_keys.size(); i++)
	{
		if (g_keys[i] == "a1")
			EXPECT_EQ((uint64_t)SMVT_INT32, g_key_types[i]);
		else if (g_keys[i] == "a2")
			EXPECT_EQ((uint64_t)SMVT_STRING, g_key_types[i]);
		else
			FAIL() << "意外的 key 名：" << g_keys[i];
	}
	if (g_keys.size() == 2u)
	{
		EXPECT_TRUE(g_keys[0] == "a1" || g_keys[0] == "a2");
		EXPECT_NE(g_keys[0], g_keys[1]) << "两个 key 必须各回调一次";
	}

	g_keys.clear(); g_key_types.clear();
	EXPECT_EQ(2u, ::get_keys(dom.c_str(), "secB", collect_key));
	ASSERT_EQ(2u, g_key_types.size());
	for (size_t i = 0; i < g_keys.size(); i++)
	{
		if (g_keys[i] == "b1")
			EXPECT_EQ((uint64_t)SMVT_UINT64, g_key_types[i]);
		else
			EXPECT_EQ((uint64_t)SMVT_DOUBLE, g_key_types[i]);
	}

	// 域不存在 / 段不存在：回调 0 次、计数 0（ShareBlocks.cpp L388-392、L421-423）
	g_sections.clear();
	EXPECT_EQ(0u, ::get_sections(ghost.c_str(), collect_section));
	EXPECT_TRUE(g_sections.empty());
	g_keys.clear();
	EXPECT_EQ(0u, ::get_keys(dom.c_str(), "nosection", collect_key));
	EXPECT_TRUE(g_keys.empty());
	EXPECT_EQ(0u, ::get_keys(ghost.c_str(), "secA", collect_key));
}

TEST_F(R19TempDir, r19_capi_cmd_wrappers_forward_to_shareblocks)
{
	const std::string cmder = r19::domain("f3c");
	const std::string worker = r19::domain("f3w");
	const std::string ghost = r19::domain("f3ghost");
	const std::string path = file("f3.blk");

	// L154-157 init_cmder
	ASSERT_TRUE(::init_cmder(cmder.c_str(), true, path.c_str()));
	// 未 init 的名字：L159-162 add_cmd 必须失败
	EXPECT_FALSE(::add_cmd(ghost.c_str(), "nope"));
	uint32_t idx = 7u;
	EXPECT_STREQ("", ::get_cmd(ghost.c_str(), idx)) << "L164-167 转发到 ShareBlocks::get_cmd";
	EXPECT_EQ(7u, idx);

	// 下达者写命令
	ASSERT_TRUE(::add_cmd(cmder.c_str(), "ping"));

	// 消费者（同文件、非 cmder）用 999999 哨兵接上第一条
	ASSERT_TRUE(::init_cmder(worker.c_str(), false, path.c_str()));
	idx = 999999u;
	EXPECT_STREQ("ping", ::get_cmd(worker.c_str(), idx));
	EXPECT_EQ(0u, idx);
	EXPECT_STREQ("", ::get_cmd(worker.c_str(), idx)) << "已经追平 readable";

	// 重复 init_cmder 同名 → 幂等 true（ShareBlocks.cpp L678-679）
	EXPECT_TRUE(::init_cmder(worker.c_str(), false, path.c_str()));

	// 第二条
	ASSERT_TRUE(::add_cmd(cmder.c_str(), "pong"));
	EXPECT_STREQ("pong", ::get_cmd(worker.c_str(), idx));
	EXPECT_EQ(1u, idx);
}
