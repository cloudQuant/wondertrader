/*!
 * \file r19_shareblock_slave_test.cpp
 * \brief lane R19：ShareBlocks 的 slave 视角（同文件第二份映射）+ 刷新/释放语义
 *
 * 覆盖 src/WtShareHelper/ShareBlocks.cpp：
 * - L87-128  init_slave（文件不存在 L97-98、装载 section/key L110-123）
 * - L130-163 update_slave（L133-134 block 为空、L136-137 时间戳未变、L139-162 重装载）
 * - L165-184 release_slave（L168-169 未知域、L174-175 master 拒绝、L177-183 正常释放）
 * - L186-200 get_section_updatetime
 * - L202-250 commit_section / delete_section
 * - L252-345 make_valid 的「slave 不能建段/建 key」两条闸门（L266-271、L310-315）
 * - L407-433 get_keys（L416-419 段数不一致时强制刷新）
 */
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "r19_support.h"

#include "WtShareHelper/ShareBlocks.h"

using namespace shareblock;

namespace {
ShareBlocks& sb() { return ShareBlocks::one(); }
} // namespace

// 文件不存在 → false；master 写过的文件，slave 能读出同一份数据
TEST_F(R19TempDir, r19_init_slave_missing_file_false_and_reads_master_kv)
{
	r19::attach_logger();
	const std::string domM = r19::domain("b1m");
	const std::string domS = r19::domain("b1s");
	const std::string path = file("b1.blk");

	// L97-98：slave 侧文件必须已存在（slave 不建块）
	EXPECT_FALSE(sb().init_slave(domS.c_str(), file("never_created.blk").c_str()));

	ASSERT_TRUE(sb().init_master(domM.c_str(), path.c_str()));
	ASSERT_NE(nullptr, sb().allocate_int32(domM.c_str(), "kv", "i", 111));
	ASSERT_NE(nullptr, sb().allocate_string(domM.c_str(), "kv", "s", "hello"));
	ASSERT_NE(nullptr, sb().allocate_double(domM.c_str(), "kv", "d", 2.5));
	ASSERT_TRUE(sb().commit_section(domM.c_str(), "kv"));

	// L100-123：slave 映射同一份文件，按 _state==1 的段装载 key
	ASSERT_TRUE(sb().init_slave(domS.c_str(), path.c_str()));
	EXPECT_TRUE(r19::has_log("Initialize domain"));

	EXPECT_EQ(111, sb().get_int32(domS.c_str(), "kv", "i", -1));
	EXPECT_STREQ("hello", sb().get_string(domS.c_str(), "kv", "s", "?"));
	EXPECT_DOUBLE_EQ(2.5, sb().get_double(domS.c_str(), "kv", "d", 0.0));

	// 同一份映射区：master 改一下，slave 立刻能读到（不需要刷新缓存，值就在页里）
	ASSERT_TRUE(sb().set_int32(domM.c_str(), "kv", "i", 222));
	EXPECT_EQ(222, sb().get_int32(domS.c_str(), "kv", "i", -1));

	// 枚举：两侧都看到 _state==1 的 "kv"
	auto secsM = sb().get_sections(domM.c_str());
	auto secsS = sb().get_sections(domS.c_str());
	ASSERT_EQ(1u, secsM.size());
	EXPECT_EQ("kv", secsM[0]);
	ASSERT_EQ(1u, secsS.size());
	EXPECT_EQ("kv", secsS[0]);

	// commit_section 只写 section 级时间戳（L221），两侧读的是同一格
	const uint64_t tM = sb().get_section_updatetime(domM.c_str(), "kv");
	const uint64_t tS = sb().get_section_updatetime(domS.c_str(), "kv");
	EXPECT_EQ(tM, tS);
	EXPECT_GT(tM, 0u);
	// L193-195：slave 侧未知段 → 0
	EXPECT_EQ(0u, sb().get_section_updatetime(domS.c_str(), "nosection"));
	// L188-190：未知域 → 0
	EXPECT_EQ(0u, sb().get_section_updatetime(r19::domain("b1ghost").c_str(), "kv"));

	EXPECT_EQ(3u, sb().get_keys(domS.c_str(), "kv").size());
	// L421-423：段不在缓存里 → 空
	EXPECT_EQ(0u, sb().get_keys(domS.c_str(), "nosection").size());
}

// slave 只能读：建不了新段，也建不了新 key
TEST_F(R19TempDir, r19_slave_cannot_create_section_or_key)
{
	r19::attach_logger();
	const std::string domM = r19::domain("b2m");
	const std::string domS = r19::domain("b2s");
	const std::string path = file("b2.blk");

	ASSERT_TRUE(sb().init_master(domM.c_str(), path.c_str()));
	ASSERT_NE(nullptr, sb().allocate_int32(domM.c_str(), "exist", "old", 9));
	// 让 slave 的映射里带上一个已存在的段（_state==1）
	ASSERT_TRUE(sb().init_slave(domS.c_str(), path.c_str()));
	EXPECT_EQ(9, sb().get_int32(domS.c_str(), "exist", "old", -1));

	// L266-271：段不存在 + 非 master → 不建段
	r19::attach_logger();
	EXPECT_EQ(nullptr, sb().allocate_int32(domS.c_str(), "newsection", "k", 1));
	EXPECT_FALSE(sb().set_int32(domS.c_str(), "newsection", "k", 1));
	EXPECT_TRUE(r19::has_log("cannot add new section"));

	// L310-315：段存在但 key 不存在 + 非 master → 不建 key
	r19::attach_logger();
	EXPECT_EQ(nullptr, sb().allocate_int32(domS.c_str(), "exist", "fresh", 1));
	EXPECT_EQ(nullptr, sb().allocate_string(domS.c_str(), "exist", "fresh", "x"));
	EXPECT_FALSE(sb().set_int64(domS.c_str(), "exist", "fresh", 1));
	EXPECT_TRUE(r19::has_log("cannot create kv pair"));

	// 已有 key 仍然可读（check_valid 不看 master 标志）
	EXPECT_EQ(9, sb().get_int32(domS.c_str(), "exist", "old", -1));
	// 但写已有 key 会因为 make_valid 建不了新 key 而失败（key 名不存在时）
	EXPECT_FALSE(sb().set_int32(domS.c_str(), "exist", "another", 5));
	// master 侧同样一个 key 名可以建
	EXPECT_NE(nullptr, sb().allocate_int32(domM.c_str(), "exist", "fresh", 3));
}

// commit_section 不动 block 级 _updatetime → 非强制刷新永远为 false（缺陷 BUG-R19-001）
TEST_F(R19TempDir, r19_update_slave_ignores_commit_until_blocktime_changes)
{
	r19::attach_logger();
	const std::string domM = r19::domain("b3m");
	const std::string domS = r19::domain("b3s");
	const std::string path = file("b3.blk");

	ASSERT_TRUE(sb().init_master(domM.c_str(), path.c_str()));
	ASSERT_NE(nullptr, sb().allocate_int32(domM.c_str(), "sec", "k", 1));

	// init_slave：_blocktime = block->_updatetime（新建块 = 0）
	ASSERT_TRUE(sb().init_slave(domS.c_str(), path.c_str()));

	// master 提交了一次，block 级 _updatetime 仍然是 0（commit_section L221 只写段级）
	ASSERT_TRUE(sb().commit_section(domM.c_str(), "sec"));
	EXPECT_FALSE(sb().update_slave(domS.c_str(), false))
		<< "L136-137：block->_updatetime 没变，slave 认为无需刷新";
	// 强制刷新才会重装载（L139-162）
	r19::attach_logger();
	EXPECT_TRUE(sb().update_slave(domS.c_str(), true));
	EXPECT_TRUE(r19::has_log("reloaded"));
	// 再提交一次，block 级时间戳依旧没动 → 非强制刷新还是 false
	ASSERT_TRUE(sb().commit_section(domM.c_str(), "sec"));
	EXPECT_FALSE(sb().update_slave(domS.c_str(), false));
}

// 非强制刷新的真正触发条件：block->_updatetime 被 delete_section 改过
TEST_F(R19TempDir, r19_update_slave_reloads_after_blocktime_change_and_release_rules)
{
	r19::attach_logger();
	const std::string domM = r19::domain("b4m");
	const std::string domS = r19::domain("b4s");
	const std::string path = file("b4.blk");

	ASSERT_TRUE(sb().init_master(domM.c_str(), path.c_str()));
	ASSERT_NE(nullptr, sb().allocate_int32(domM.c_str(), "alive", "k1", 41));
	ASSERT_NE(nullptr, sb().allocate_int32(domM.c_str(), "doomed", "k2", 42));
	ASSERT_TRUE(sb().init_slave(domS.c_str(), path.c_str()));
	EXPECT_EQ(41, sb().get_int32(domS.c_str(), "alive", "k1", -1));

	// block->_updatetime 初值 0，slave 的 _blocktime 也是 0 → 非强制为 false
	EXPECT_FALSE(sb().update_slave(domS.c_str(), false));

	// master 删段 → block->_updatetime = now（L247）→ 非强制刷新为 true
	ASSERT_TRUE(sb().delete_section(domM.c_str(), "doomed"));
	const size_t before = r19::log_count();
	EXPECT_TRUE(sb().update_slave(domS.c_str(), false));
	EXPECT_GE(r19::log_count(), before);
	EXPECT_TRUE(r19::has_log("reloaded"));

	// 刷新后 block->_updatetime 被同步（L160）→ 再来一次非强制就是 false
	EXPECT_FALSE(sb().update_slave(domS.c_str(), false));

	// 缺陷 BUG-R19-002：L143-156 的重装载循环只看 _count==0，不看 _state，
	// 于是 master 已经 delete（_state=2）的段在 slave 侧被重新装回缓存
	EXPECT_EQ(42, sb().get_int32(domS.c_str(), "doomed", "k2", -1))
		<< "被删除的段在 slave 刷新后仍可读到，说明 slave 无法感知删除";
	auto secsS = sb().get_sections(domS.c_str());
	ASSERT_EQ(1u, secsS.size()) << "枚举侧看得到 _state!=1 被过滤掉";
	EXPECT_EQ("alive", secsS[0]);

	// L416-419：段数与 block->_count 不一致时 get_keys 内部会强制刷新
	ASSERT_NE(nullptr, sb().allocate_int32(domM.c_str(), "third", "k3", 43));
	EXPECT_EQ(1u, sb().get_keys(domS.c_str(), "third").size())
		<< "get_keys 检测到 size 不一致 → update_slave(domain,true) 后应能看到新段";
	EXPECT_EQ(43, sb().get_int32(domS.c_str(), "third", "k3", -1));

	// release_slave：master 域拒绝（L174-175），slave 域成功（L177-183），未知域返回 true（L168-169）
	EXPECT_FALSE(sb().release_slave(domM.c_str()));
	ASSERT_TRUE(sb().release_slave(domS.c_str()));
	EXPECT_FALSE(sb().update_slave(domS.c_str(), true)) << "L133-134：_block 已置空";
	EXPECT_EQ(-1, sb().get_int32(domS.c_str(), "alive", "k1", -1)) << "域已摘除，只能回落默认值";
	EXPECT_TRUE(sb().release_slave(r19::domain("b4ghost").c_str()));
	// 释放后同名再 init_slave 可以重新映射（_shm_blocks 里已经没有这条域）
	const std::string domS2 = domS;
	EXPECT_TRUE(sb().init_slave(domS2.c_str(), path.c_str()));
	EXPECT_EQ(41, sb().get_int32(domS2.c_str(), "alive", "k1", -1));
}
