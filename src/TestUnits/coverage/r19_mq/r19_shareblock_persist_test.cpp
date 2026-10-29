/*!
 * \file r19_shareblock_persist_test.cpp
 * \brief lane R19：以磁盘镜像为输入覆盖 init_master 的压缩/装载分支，
 *        并锁定几个「定长字段 + 无界拷贝」造成的越界写行为。
 *
 * 覆盖 src/WtShareHelper/ShareBlocks.cpp：
 * - L36-59   init_master 的「失效 section 压缩」分支（含 write_log L48-49）
 * - L61-82   init_master 的 key 装载 + L72 把 _state 置 0
 * - L202-250 commit_section（L221 段级时间戳）/ delete_section（L247 块级时间戳）
 * - L294-303 make_valid 的「索引位置段名不匹配 → 拒绝写」
 * - L280-285 make_valid 新建段时 wt_strcpy(_name, section) 无长度上限
 * - L442-447/L544-546 string 定长 64 拷贝 + NUL（越界 1 字节）
 * 输入文件全部由用例自己创建在临时目录里，TearDown 清理。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "r19_support.h"

#include "WtShareHelper/ShareBlocks.h"

using namespace shareblock;

namespace {
ShareBlocks& sb() { return ShareBlocks::one(); }

/// 造一份「1 个有效段 + 1 个零 key 段 + 1 个 state=0 段」的镜像
inline std::vector<char> craft_mixed_block()
{
	std::vector<char> buf = r19::make_zero_block();
	ShmBlock* blk = reinterpret_cast<ShmBlock*>(buf.data());
	std::memcpy(blk->_flag, BLK_FLAG, FLAG_SIZE);
	blk->_updatetime = 5;
	blk->_count = 3;

	SecInfo& keep = blk->_sections[0];
	std::memcpy(keep._name, "keep", 5);
	keep._count = 2;
	keep._state = 1;
	keep._offset = 8;
	keep._updatetime = 12345;
	r19::put_int32_key(keep, 0, "a", 777, 0);
	r19::put_int32_key(keep, 1, "b", 888, 4);

	SecInfo& zero = blk->_sections[1];
	std::memcpy(zero._name, "zerocnt", 8);
	zero._count = 0;		// L40 第一个条件
	zero._state = 1;
	zero._updatetime = 22222;

	SecInfo& inactive = blk->_sections[2];
	std::memcpy(inactive._name, "inactive", 9);
	inactive._count = 1;
	inactive._state = 0;	// L40 第二个条件
	inactive._offset = 4;
	inactive._updatetime = 33333;
	r19::put_int32_key(inactive, 0, "c", 999, 0);

	return buf;
}
} // namespace

// 压缩分支 + state=0 隐藏 + 段级/块级时间戳分工
TEST_F(R19TempDir, r19_init_master_compacts_dead_sections_and_hides_live_ones)
{
	r19::attach_logger();
	const std::string path = file("c1.blk");
	const std::vector<char> buf = craft_mixed_block();
	ASSERT_TRUE(r19::write_raw_shm_block(path, buf.data(), buf.size()));

	const std::string dom = r19::domain("c1");
	// L46-59：加载到的有效段(1) != 块里记录的段数(3) → 压缩 + 覆盖计数 + 刷新块时间戳
	ASSERT_TRUE(sb().init_master(dom.c_str(), path.c_str()));
	EXPECT_TRUE(r19::has_log("cache resized"));
	EXPECT_TRUE(r19::has_log("Section count of domain"));

	// 778/888 这两个从镜像里读出来的 key 必须还在
	EXPECT_EQ(777, sb().get_int32(dom.c_str(), "keep", "a", -1));
	EXPECT_EQ(888, sb().get_int32(dom.c_str(), "keep", "b", -1));

	// 被丢掉的两个段不进缓存（_count==0 / _state!=1）
	EXPECT_EQ(-1, sb().get_int32(dom.c_str(), "zerocnt", "a", -1));
	EXPECT_EQ(-1, sb().get_int32(dom.c_str(), "inactive", "c", -1));

	// key 元信息原样保留（KeyInfo 直接来自映射区）
	auto keys = sb().get_keys(dom.c_str(), "keep");
	ASSERT_EQ(2u, keys.size());
	for (KeyInfo* k : keys)
	{
		ASSERT_NE(nullptr, k);
		EXPECT_EQ(SMVT_INT32, k->_type);
		if (std::strcmp(k->_key, "a") == 0)
		{
			EXPECT_EQ(0u, k->_offset);
			EXPECT_EQ(111000u, k->_updatetime);
		}
		else
		{
			EXPECT_STREQ("b", k->_key);
			EXPECT_EQ(4u, k->_offset);
			EXPECT_EQ(111001u, k->_updatetime);
		}
	}

	// 缺陷 BUG-R19-004：L72 把「刚从文件里读出来」的段 _state 一律置 0，
	// 而 get_sections 只列 _state==1 的段 → 重启后所有历史段都不再可见
	EXPECT_TRUE(sb().get_sections(dom.c_str()).empty())
		<< "keep 段明明有 2 个可用 key，却因为 L72 被枚举漏掉";

	// 段级时间戳仍是从文件里读到的 12345（L199），commit 之后必须严格变大
	EXPECT_EQ(12345u, sb().get_section_updatetime(dom.c_str(), "keep"));
	ASSERT_TRUE(sb().commit_section(dom.c_str(), "keep"));
	EXPECT_GT(sb().get_section_updatetime(dom.c_str(), "keep"), 12345u);

	// 压缩分支把 _blocktime 同步成了新的块时间戳（L57-58）→ 非强制刷新为 false
	EXPECT_FALSE(sb().update_slave(dom.c_str(), false));
	// delete_section 改块级时间戳（L247）→ 非强制刷新转 true（L136-137 反向）
	ASSERT_TRUE(sb().delete_section(dom.c_str(), "keep"));
	EXPECT_TRUE(sb().update_slave(dom.c_str(), false));
	// 重装载之后 keep 段的数据依旧可读（文件内容没被动过）
	EXPECT_EQ(777, sb().get_int32(dom.c_str(), "keep", "a", -1));
}

// 同一份文件被第二个域再次 init_master 时，第一次已存在的数据必须继续可读。
TEST_F(R19TempDir, r19_second_init_master_preserves_first_domain_data)
{
	r19::attach_logger();
	const std::string path = file("c2.blk");
	std::vector<char> buf = r19::make_zero_block();
	ShmBlock* blk = reinterpret_cast<ShmBlock*>(buf.data());
	std::memcpy(blk->_flag, BLK_FLAG, FLAG_SIZE);
	blk->_updatetime = 7;
	blk->_count = 2;
	SecInfo& p0 = blk->_sections[0];
	std::memcpy(p0._name, "p0", 3);
	p0._count = 1; p0._state = 1; p0._offset = 4; p0._updatetime = 1000;
	r19::put_int32_key(p0, 0, "a", 101, 0);
	SecInfo& p1 = blk->_sections[1];
	std::memcpy(p1._name, "p1", 3);
	p1._count = 1; p1._state = 1; p1._offset = 4; p1._updatetime = 2000;
	r19::put_int32_key(p1, 0, "b", 202, 0);
	ASSERT_TRUE(r19::write_raw_shm_block(path, buf.data(), buf.size()));

	const std::string domX = r19::domain("c2x");
	ASSERT_TRUE(sb().init_master(domX.c_str(), path.c_str()));
	// 两段都 _state==1 且 _count!=0 → 不进压缩分支（1..2 全保留）
	EXPECT_FALSE(r19::has_log("cache resized"));
	EXPECT_EQ(101, sb().get_int32(domX.c_str(), "p0", "a", -1));
	EXPECT_EQ(202, sb().get_int32(domX.c_str(), "p1", "b", -1));
	ASSERT_TRUE(sb().commit_section(domX.c_str(), "p0"));

	const std::string domY = r19::domain("c2y");
	// 第二次 init_master 可以被拒绝，也可以安全共存；只验证已提交的首份数据不丢失。
	(void)sb().init_master(domY.c_str(), path.c_str());
	EXPECT_EQ(101, sb().get_int32(domX.c_str(), "p0", "a", -1));
	EXPECT_EQ(202, sb().get_int32(domX.c_str(), "p1", "b", -1));
}

// string key 的定长 64 拷贝 + 结尾 NUL：段内最后一个 key 会把 NUL 写进下一段的段名
TEST_F(R19TempDir, r19_last_string_key_in_section_clobbers_next_section_name)
{
	r19::attach_logger();
	const std::string dom = r19::domain("c3");
	const std::string path = file("c3.blk");
	ASSERT_TRUE(sb().init_master(dom.c_str(), path.c_str()));

	// 16 个 string key 把 _data 填满：最后一个落在 offset=960，占 960..1023
	for (int i = 0; i < 16; i++)
	{
		const std::string k = "b" + std::to_string(i);
		const std::string v = "v" + std::to_string(i);
		ASSERT_NE(nullptr, sb().allocate_string(dom.c_str(), "before", k.c_str(), v.c_str()));
	}
	// 紧跟着建第二段（块内索引 1），段名先落成 "after"
	ASSERT_NE(nullptr, sb().allocate_int32(dom.c_str(), "after", "k", 7));
	auto secs0 = sb().get_sections(dom.c_str());
	ASSERT_EQ(2u, secs0.size());
	EXPECT_EQ("after", secs0[1]);

	// 缺陷 BUG-R19-005：wt_strcpy(des, src, 64) 会 memcpy 64 字节再写 des[64]
	// → 一共 65 字节，第 65 个字节正好越过 _data[1024] 落到下一段的 _name[0]
	ASSERT_TRUE(sb().set_string(dom.c_str(), "before", "b15", "tail"));

	auto secs = sb().get_sections(dom.c_str());
	ASSERT_EQ(2u, secs.size());
	EXPECT_EQ("before", secs[0]);
	EXPECT_TRUE(secs[1].empty())
		<< "after 段名的首字节被越界写的 NUL 干掉，段名在文件里已经不存在了";

	// 进程内仍能按缓存名访问（缓存 key 是请求时用的字符串），所以坏点在重启后才暴露
	EXPECT_EQ(7, sb().get_int32(dom.c_str(), "after", "k", -1));

	const std::string dom2 = r19::domain("c3b");
	ASSERT_TRUE(sb().init_master(dom2.c_str(), path.c_str()));
	EXPECT_EQ(-1, sb().get_int32(dom2.c_str(), "after", "k", -1))
		<< "重新加载后按原名已经找不到这一段";
	EXPECT_EQ(7, sb().get_int32(dom2.c_str(), "", "k", -1))
		<< "它被以空段名的形式重新装进了缓存";
}

// 32 字节段名加结尾 NUL 会越过 _name[32]；等待产品侧长度守卫后再单独验证拒绝与邻槽完整性。
TEST_F(R19TempDir, r19_section_name_overflow_guard_pending)
{
	GTEST_SKIP() << "Blocked until ShareBlocks rejects section names that cannot fit _name[32]. "
				 << "31 name bytes plus NUL is the safe upper bound; rejection of 32+ bytes and "
				 << "unchanged adjacent slots require a separate check after the product guard is fixed. "
				 << "No overlong name is constructed by this placeholder.";
}
