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
#include <filesystem>
#include <limits>
#include <system_error>
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

	// init_master 将读入的段标为本轮尚未使用；get_sections 只枚举本轮已激活的段。
	EXPECT_TRUE(sb().get_sections(dom.c_str()).empty())
		<< "keep 尚未在本轮被激活，当前枚举口径应跳过它";

	// 段级时间戳仍是从文件里读到的 12345（L199），commit 之后必须严格变大
	EXPECT_EQ(12345u, sb().get_section_updatetime(dom.c_str(), "keep"));
	ASSERT_TRUE(sb().commit_section(dom.c_str(), "keep"));
	EXPECT_GT(sb().get_section_updatetime(dom.c_str(), "keep"), 12345u);

	// master 不得调用 slave 的索引重建路径。
	EXPECT_FALSE(sb().update_slave(dom.c_str(), false));
	ASSERT_TRUE(sb().delete_section(dom.c_str(), "keep"));
	EXPECT_FALSE(sb().update_slave(dom.c_str(), false));
	EXPECT_EQ(-1, sb().get_int32(dom.c_str(), "keep", "a", -1));
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
	// 同一进程的第二个 master 必须被拒绝，不能把首个 master 尚未激活的段压缩掉。
	EXPECT_FALSE(sb().init_master(domY.c_str(), path.c_str()));
	EXPECT_EQ(101, sb().get_int32(domX.c_str(), "p0", "a", -1));
	EXPECT_EQ(202, sb().get_int32(domX.c_str(), "p1", "b", -1));
}

TEST_F(R19TempDir, r19_block_version_overflow_rejects_commit_and_delete_without_mutation)
{
	r19::attach_logger();
	const std::string dom = r19::domain("version_overflow");
	const std::string path = file("version_overflow.blk");
	ASSERT_TRUE(sb().init_master(dom.c_str(), path.c_str()));
	ASSERT_NE(nullptr, sb().allocate_int32(dom.c_str(), "live", "value", 73));

	BoostMappingFile mapping;
	ASSERT_TRUE(mapping.map(path.c_str()));
	ShmBlock* block = static_cast<ShmBlock*>(mapping.addr());
	ASSERT_EQ(1u, block->_count);
	const uint64_t sectionTimeBefore = block->_sections[0]._updatetime;
	block->_updatetime = (std::numeric_limits<uint64_t>::max)();
	mapping.sync();

	EXPECT_FALSE(sb().commit_section(dom.c_str(), "live"));
	EXPECT_EQ((std::numeric_limits<uint64_t>::max)(), block->_updatetime);
	EXPECT_EQ(sectionTimeBefore, block->_sections[0]._updatetime);
	EXPECT_FALSE(sb().delete_section(dom.c_str(), "live"));
	EXPECT_EQ((std::numeric_limits<uint64_t>::max)(), block->_updatetime);
	EXPECT_EQ(1u, block->_sections[0]._state);
	EXPECT_EQ(73, sb().get_int32(dom.c_str(), "live", "value", -1));
}

// String slots accept at most 63 bytes, and a full section's final slot must not overwrite its neighbor.
TEST_F(R19TempDir, r19_string_slot_boundaries_preserve_neighbors_and_persist)
{
	r19::attach_logger();
	const std::string dom = r19::domain("c3");
	const std::string path = file("c3.blk");
	ASSERT_TRUE(sb().init_master(dom.c_str(), path.c_str()));

	const std::string maxValue(63, 'L');
	// 16 个 string key 把 _data 填满：最后一个落在 offset=960，占 960..1023。
	// 初始化时一并覆盖空字符串、短值和 63 字节上限。
	for (int i = 0; i < 16; i++)
	{
		const std::string k = "b" + std::to_string(i);
		std::string v = "v" + std::to_string(i);
		if (i == 0)
			v = maxValue;
		else if (i == 1)
			v.clear();
		else if (i == 2)
			v = "short";
		ASSERT_NE(nullptr, sb().allocate_string(dom.c_str(), "before", k.c_str(), v.c_str()));
	}
	// 紧跟着建第二段（块内索引 1），段名先落成 "after"
	ASSERT_NE(nullptr, sb().allocate_int32(dom.c_str(), "after", "k", 7));
	ASSERT_TRUE(sb().set_string(dom.c_str(), "before", "b15", maxValue.c_str()));

	auto secs = sb().get_sections(dom.c_str());
	ASSERT_EQ(2u, secs.size());
	EXPECT_EQ("before", secs[0]);
	EXPECT_EQ("after", secs[1]);
	EXPECT_STREQ(maxValue.c_str(), sb().get_string(dom.c_str(), "before", "b0"));
	EXPECT_STREQ("", sb().get_string(dom.c_str(), "before", "b1"));
	EXPECT_STREQ("short", sb().get_string(dom.c_str(), "before", "b2"));
	EXPECT_STREQ(maxValue.c_str(), sb().get_string(dom.c_str(), "before", "b15"));
	EXPECT_EQ(7, sb().get_int32(dom.c_str(), "after", "k", -1));
	ASSERT_TRUE(sb().set_string(dom.c_str(), "before", "b14", "tail"));
	EXPECT_STREQ("tail", sb().get_string(dom.c_str(), "before", "b14"));
	ASSERT_TRUE(sb().set_string(dom.c_str(), "before", "b14", ""));
	EXPECT_STREQ("", sb().get_string(dom.c_str(), "before", "b14"));
	ASSERT_TRUE(sb().set_string(dom.c_str(), "before", "b14", "tail"));

	// Reject invalid values before make_valid: neither existing slots nor new section/key
	// metadata may be touched when an overlong value is supplied.
	BoostMappingFile snapshotMapping;
	ASSERT_TRUE(snapshotMapping.map(path.c_str()));
	ASSERT_EQ(sizeof(ShmBlock), snapshotMapping.size());
	auto snapshot = [&snapshotMapping]() {
		std::vector<char> bytes(snapshotMapping.size());
		std::memcpy(bytes.data(), snapshotMapping.addr(), bytes.size());
		return bytes;
	};
	auto expect_unchanged = [&snapshotMapping](const std::vector<char>& before) {
		EXPECT_EQ(0, std::memcmp(before.data(), snapshotMapping.addr(), before.size()));
	};

	const std::string value64(64, 'X');
	const std::string value80(80, 'Y');
	auto before = snapshot();
	EXPECT_FALSE(sb().set_string(dom.c_str(), "before", "b15", nullptr));
	expect_unchanged(before);
	before = snapshot();
	EXPECT_EQ(nullptr, sb().allocate_string(dom.c_str(), "new_section", "new_key", nullptr));
	expect_unchanged(before);

	before = snapshot();
	EXPECT_FALSE(sb().set_string(dom.c_str(), "before", "b15", value64.c_str()));
	expect_unchanged(before);
	EXPECT_STREQ(maxValue.c_str(), sb().get_string(dom.c_str(), "before", "b15"));

	before = snapshot();
	EXPECT_FALSE(sb().set_string(dom.c_str(), "before", "b15", value80.c_str()));
	expect_unchanged(before);
	EXPECT_STREQ(maxValue.c_str(), sb().get_string(dom.c_str(), "before", "b15"));

	before = snapshot();
	EXPECT_EQ(nullptr, sb().allocate_string(dom.c_str(), "after", "new_key", value64.c_str()));
	expect_unchanged(before);

	before = snapshot();
	EXPECT_EQ(nullptr, sb().allocate_string(dom.c_str(), "new_section", "new_key", value80.c_str()));
	expect_unchanged(before);
	EXPECT_TRUE(r19::has_log("exceeds 63 bytes"));

	// Persist the full-slot update and reload its image through a fresh mapping/domain.
	ASSERT_TRUE(sb().commit_section(dom.c_str(), "before"));
	ASSERT_TRUE(sb().commit_section(dom.c_str(), "after"));
	snapshotMapping.sync();
	snapshotMapping.close();
	const std::string reopenedPath = file("c3-reopened.blk");
	std::error_code copyError;
	ASSERT_TRUE(std::filesystem::copy_file(path, reopenedPath, std::filesystem::copy_options::overwrite_existing, copyError))
		<< copyError.message();
	const std::string dom2 = r19::domain("c3b");
	ASSERT_TRUE(sb().init_master(dom2.c_str(), reopenedPath.c_str()));
	EXPECT_STREQ(maxValue.c_str(), sb().get_string(dom2.c_str(), "before", "b15"));
	EXPECT_STREQ("", sb().get_string(dom2.c_str(), "before", "b1"));
	EXPECT_EQ(7, sb().get_int32(dom2.c_str(), "after", "k", -1));
}

// _name[32] reserves one byte for NUL, so 31 is the longest accepted name.
TEST_F(R19TempDir, r19_section_name_boundary_and_overlong_rejection_preserve_block)
{
	r19::attach_logger();
	const std::string dom = r19::domain("c4");
	const std::string path = file("c4.blk");
	ASSERT_TRUE(sb().init_master(dom.c_str(), path.c_str()));

	const std::string maxName(31, 'n');
	ASSERT_NE(nullptr, sb().allocate_int32(dom.c_str(), maxName.c_str(), "value", 31));
	ASSERT_NE(nullptr, sb().allocate_int32(dom.c_str(), "neighbor", "value", 41));

	auto sections = sb().get_sections(dom.c_str());
	ASSERT_EQ(2u, sections.size());
	EXPECT_EQ(maxName, sections[0]);
	EXPECT_EQ("neighbor", sections[1]);
	EXPECT_EQ(31, sb().get_int32(dom.c_str(), maxName.c_str(), "value", -1));
	EXPECT_EQ(41, sb().get_int32(dom.c_str(), "neighbor", "value", -1));

	// Snapshot the mapped block so a rejected name must leave the count, metadata,
	// values, and adjacent section slots byte-for-byte unchanged.
	BoostMappingFile snapshotMapping;
	ASSERT_TRUE(snapshotMapping.map(path.c_str()));
	ASSERT_EQ(sizeof(ShmBlock), snapshotMapping.size());
	std::vector<char> before(snapshotMapping.size());
	std::memcpy(before.data(), snapshotMapping.addr(), before.size());

	const std::string tooLong32(32, 'x');
	EXPECT_EQ(nullptr, sb().allocate_int32(dom.c_str(), tooLong32.c_str(), "value", 99));
	EXPECT_EQ(0, std::memcmp(before.data(), snapshotMapping.addr(), before.size()));

	const std::string tooLong48(48, 'y');
	EXPECT_EQ(nullptr, sb().allocate_int32(dom.c_str(), tooLong48.c_str(), "value", 100));
	EXPECT_EQ(0, std::memcmp(before.data(), snapshotMapping.addr(), before.size()));

	sections = sb().get_sections(dom.c_str());
	ASSERT_EQ(2u, sections.size());
	EXPECT_EQ(maxName, sections[0]);
	EXPECT_EQ("neighbor", sections[1]);
	EXPECT_EQ(31, sb().get_int32(dom.c_str(), maxName.c_str(), "value", -1));
	EXPECT_EQ(41, sb().get_int32(dom.c_str(), "neighbor", "value", -1));
	EXPECT_TRUE(r19::has_log("exceeds 31 bytes"));
}

TEST_F(R19TempDir, r19_string_entrypoints_reject_numeric_slots_without_mutation)
{
	r19::attach_logger();
	const std::string dom = r19::domain("c5");
	const std::string path = file("c5.blk");
	ASSERT_TRUE(sb().init_master(dom.c_str(), path.c_str()));

	ASSERT_NE(nullptr, sb().allocate_int32(dom.c_str(), "mixed", "left", 111));
	ASSERT_NE(nullptr, sb().allocate_string(dom.c_str(), "mixed", "middle", "keep"));
	ASSERT_NE(nullptr, sb().allocate_int32(dom.c_str(), "mixed", "right", 222));
	// Put the final int32 key at offset 1016, the final allocated key with only 8 bytes left.
	for (int i = 0; i < 118; ++i)
	{
		const std::string key = "d" + std::to_string(i);
		ASSERT_NE(nullptr, sb().allocate_double(dom.c_str(), "mixed", key.c_str(), i));
	}
	ASSERT_NE(nullptr, sb().allocate_int32(dom.c_str(), "mixed", "tail", 5150));

	auto keys = sb().get_keys(dom.c_str(), "mixed");
	auto key_info = [&keys](const char* name) -> KeyInfo* {
		for (KeyInfo* key : keys)
			if (key != nullptr && std::strcmp(key->_key, name) == 0)
				return key;
		return nullptr;
	};
	ASSERT_NE(nullptr, key_info("left"));
	ASSERT_NE(nullptr, key_info("middle"));
	ASSERT_NE(nullptr, key_info("right"));
	ASSERT_NE(nullptr, key_info("tail"));
	EXPECT_EQ(0u, key_info("left")->_offset);
	EXPECT_EQ(4u, key_info("middle")->_offset);
	EXPECT_EQ(68u, key_info("right")->_offset);
	EXPECT_EQ(1016u, key_info("tail")->_offset);

	BoostMappingFile mapping;
	ASSERT_TRUE(mapping.map(path.c_str()));
	ASSERT_EQ(sizeof(ShmBlock), mapping.size());
	auto snapshot = [&mapping]() {
		std::vector<char> bytes(mapping.size());
		std::memcpy(bytes.data(), mapping.addr(), bytes.size());
		return bytes;
	};
	auto expect_unchanged = [&mapping](const std::vector<char>& before) {
		EXPECT_EQ(0, std::memcmp(before.data(), mapping.addr(), before.size()));
	};

	auto before = snapshot();
	EXPECT_FALSE(sb().set_string(dom.c_str(), "mixed", "left", "overwrite"));
	expect_unchanged(before);
	before = snapshot();
	EXPECT_EQ(nullptr, sb().allocate_string(dom.c_str(), "mixed", "left", "overwrite"));
	expect_unchanged(before);
	before = snapshot();
	EXPECT_EQ(nullptr, sb().allocate_string(dom.c_str(), "mixed", "left", "overwrite", true));
	expect_unchanged(before);

	before = snapshot();
	EXPECT_FALSE(sb().set_string(dom.c_str(), "mixed", "tail", "overwrite"));
	expect_unchanged(before);
	before = snapshot();
	EXPECT_EQ(nullptr, sb().allocate_string(dom.c_str(), "mixed", "tail", "overwrite"));
	expect_unchanged(before);
	before = snapshot();
	EXPECT_EQ(nullptr, sb().allocate_string(dom.c_str(), "mixed", "tail", "overwrite", true));
	expect_unchanged(before);

	EXPECT_EQ(111, sb().get_int32(dom.c_str(), "mixed", "left", -1));
	EXPECT_STREQ("keep", sb().get_string(dom.c_str(), "mixed", "middle"));
	EXPECT_EQ(222, sb().get_int32(dom.c_str(), "mixed", "right", -1));
	EXPECT_EQ(5150, sb().get_int32(dom.c_str(), "mixed", "tail", -1));
}

TEST_F(R19TempDir, r19_string_entrypoints_reject_out_of_range_existing_offsets)
{
	r19::attach_logger();
	const std::string dom = r19::domain("c6");
	const std::string path = file("c6.blk");
	ASSERT_TRUE(sb().init_master(dom.c_str(), path.c_str()));
	ASSERT_NE(nullptr, sb().allocate_string(dom.c_str(), "bad_offsets", "near_end", "near"));
	ASSERT_NE(nullptr, sb().allocate_string(dom.c_str(), "bad_offsets", "overflow", "overflow"));
	ASSERT_NE(nullptr, sb().allocate_int32(dom.c_str(), "bad_offsets", "neighbor", 909));

	auto keys = sb().get_keys(dom.c_str(), "bad_offsets");
	auto key_info = [&keys](const char* name) -> KeyInfo* {
		for (KeyInfo* key : keys)
			if (key != nullptr && std::strcmp(key->_key, name) == 0)
				return key;
		return nullptr;
	};
	ASSERT_NE(nullptr, key_info("near_end"));
	ASSERT_NE(nullptr, key_info("overflow"));
	key_info("near_end")->_offset = 1000;
	key_info("overflow")->_offset = UINT32_MAX;

	BoostMappingFile mapping;
	ASSERT_TRUE(mapping.map(path.c_str()));
	ASSERT_EQ(sizeof(ShmBlock), mapping.size());
	auto snapshot = [&mapping]() {
		std::vector<char> bytes(mapping.size());
		std::memcpy(bytes.data(), mapping.addr(), bytes.size());
		return bytes;
	};
	auto expect_unchanged = [&mapping](const std::vector<char>& before) {
		EXPECT_EQ(0, std::memcmp(before.data(), mapping.addr(), before.size()));
	};

	auto before = snapshot();
	EXPECT_FALSE(sb().set_string(dom.c_str(), "bad_offsets", "near_end", "changed"));
	expect_unchanged(before);
	before = snapshot();
	EXPECT_EQ(nullptr, sb().allocate_string(dom.c_str(), "bad_offsets", "near_end", "changed"));
	expect_unchanged(before);
	before = snapshot();
	EXPECT_EQ(nullptr, sb().allocate_string(dom.c_str(), "bad_offsets", "near_end", "changed", true));
	expect_unchanged(before);

	before = snapshot();
	EXPECT_FALSE(sb().set_string(dom.c_str(), "bad_offsets", "overflow", "changed"));
	expect_unchanged(before);
	before = snapshot();
	EXPECT_EQ(nullptr, sb().allocate_string(dom.c_str(), "bad_offsets", "overflow", "changed", true));
	expect_unchanged(before);

	EXPECT_EQ(909, sb().get_int32(dom.c_str(), "bad_offsets", "neighbor", -1));
}

TEST_F(R19TempDir, r19_make_valid_rejections_leave_section_state_and_block_unchanged)
{
	r19::attach_logger();
	const std::string master = r19::domain("c7m");
	const std::string slave = r19::domain("c7s");
	const std::string path = file("c7.blk");
	ASSERT_TRUE(sb().init_master(master.c_str(), path.c_str()));

	ASSERT_NE(nullptr, sb().allocate_int32(master.c_str(), "bad_offset", "existing", 1));
	for (int i = 0; i < 16; ++i)
	{
		const std::string key = "s" + std::to_string(i);
		ASSERT_NE(nullptr, sb().allocate_string(master.c_str(), "full_space", key.c_str(), "x"));
	}
	for (int i = 0; i < MAX_KEY_CNT; ++i)
	{
		const std::string key = "k" + std::to_string(i);
		ASSERT_NE(nullptr, sb().allocate_int32(master.c_str(), "full_keys", key.c_str(), i));
	}
	ASSERT_NE(nullptr, sb().allocate_int32(master.c_str(), "inactive", "existing", 9));

	BoostMappingFile mapping;
	ASSERT_TRUE(mapping.map(path.c_str()));
	ASSERT_EQ(sizeof(ShmBlock), mapping.size());
	ShmBlock* block = static_cast<ShmBlock*>(mapping.addr());
	auto find_section = [block](const char* name) -> SecInfo* {
		for (uint32_t i = 0; i < block->_count; ++i)
			if (std::strcmp(block->_sections[i]._name, name) == 0)
				return &block->_sections[i];
		return nullptr;
	};
	ASSERT_NE(nullptr, find_section("bad_offset"));
	ASSERT_NE(nullptr, find_section("full_space"));
	ASSERT_NE(nullptr, find_section("full_keys"));
	ASSERT_NE(nullptr, find_section("inactive"));
	EXPECT_EQ(1024u, find_section("full_space")->_offset);
	EXPECT_EQ(MAX_KEY_CNT, find_section("full_keys")->_count);

	// Simulate corrupt persisted metadata and inactive sections after setup.
	find_section("bad_offset")->_offset = UINT32_MAX;
	find_section("bad_offset")->_state = 0;
	find_section("full_space")->_state = 0;
	find_section("full_keys")->_state = 0;
	ASSERT_TRUE(sb().init_slave(slave.c_str(), path.c_str()));
	find_section("inactive")->_state = 0;

	auto snapshot = [&mapping]() {
		std::vector<char> bytes(mapping.size());
		std::memcpy(bytes.data(), mapping.addr(), bytes.size());
		return bytes;
	};
	auto expect_unchanged = [&mapping](const std::vector<char>& before) {
		EXPECT_EQ(0, std::memcmp(before.data(), mapping.addr(), before.size()));
	};

	auto before = snapshot();
	EXPECT_FALSE(sb().set_int32(master.c_str(), "bad_offset", "new", 2));
	expect_unchanged(before);
	before = snapshot();
	EXPECT_FALSE(sb().set_int32(master.c_str(), "full_space", "new", 2));
	expect_unchanged(before);
	before = snapshot();
	EXPECT_FALSE(sb().set_int32(master.c_str(), "full_keys", "new", 2));
	expect_unchanged(before);
	before = snapshot();
	EXPECT_FALSE(sb().set_int32(slave.c_str(), "inactive", "new", 2));
	expect_unchanged(before);
}
