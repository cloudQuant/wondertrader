/*!
 * \file r19_shareblock_master_test.cpp
 * \brief lane R19：shareblock::ShareBlocks 主端（master）分配/读写/容量边界
 *
 * 覆盖产品文件与行段（src/WtShareHelper/ShareBlocks.cpp）：
 * - L9-85   init_master（新建映射、幂等早退 L12-13、key 装载 L61-82）
 * - L252-345 make_valid 的新建/复用/容量三分支
 * - L347-381 check_valid 的域/段/key/类型四道闸门
 * - L383-433 get_sections / get_keys
 * - L435-535 六个 allocate_*
 * - L537-613 六个 set_*
 * - L615-673 六个 get_*
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>
#include <filesystem>

#include "r19_support.h"

#include "WtShareHelper/ShareBlocks.h"

using namespace shareblock;

namespace {

ShareBlocks& sb() { return ShareBlocks::one(); }

} // namespace

// ---------------------------------------------------------------- 建块与幂等
TEST_F(R19TempDir, r19_init_master_creates_block_file_and_is_idempotent)
{
	r19::attach_logger();
	const std::string dom = r19::domain("a1");
	const std::string path = file("a1.blk");

	std::error_code ec;
	ASSERT_FALSE(std::filesystem::exists(path, ec)) << "用例自己造的临时文件，建块前必须不存在";

	// ShareBlocks.cpp L19-25：文件不存在 → BoostFile 建文件并 truncate 到 sizeof(ShmBlock)
	ASSERT_TRUE(sb().init_master(dom.c_str(), path.c_str()));
	ASSERT_TRUE(std::filesystem::exists(path, ec));
	EXPECT_EQ(sizeof(ShmBlock), (size_t)std::filesystem::file_size(path, ec))
		<< "init_master 用 sizeof(ShmBlock) 定长建块（L23）";

	// L12-13：同一个域再次 init_master → _block 已存在 → 直接 true，不重复建映射
	EXPECT_TRUE(sb().init_master(dom.c_str(), path.c_str()));

	// 空块：_count==0 → 没有任何 section（L396-402 循环不进入）
	EXPECT_TRUE(sb().get_sections(dom.c_str()).empty());

	// ShareBlocks.h L184-187 的 _ShmPair 构造只初始化 _block/_master，
	// _blocktime 是从分配器拿到的随机值 → 「master 首帧 + 非强制 update_slave」
	// 的结果不可复现（缺陷 BUG-R19-009），这里只断言强制刷新这条确定路径。
	EXPECT_TRUE(sb().update_slave(dom.c_str(), true));

	// L193-195：段不存在 → 时间戳 0
	EXPECT_EQ(0u, sb().get_section_updatetime(dom.c_str(), "nothing"));

	// L212-217：段不存在 → commit 失败并写 "Secion ... not exists"
	EXPECT_FALSE(sb().commit_section(dom.c_str(), "nothing"));
	EXPECT_TRUE(r19::has_log("not exists"));
}

// ---------------------------------------------------------------- 六种类型往返
TEST_F(R19TempDir, r19_master_allocate_set_get_exact_roundtrip)
{
	r19::attach_logger();
	const std::string dom = r19::domain("a2");
	const std::string path = file("a2.blk");
	ASSERT_TRUE(sb().init_master(dom.c_str(), path.c_str()));

	// ---- allocate_*：新建 key 时用 initVal 填充（各 allocate 的 _type==0 分支）
	int32_t* pi = sb().allocate_int32(dom.c_str(), "s2", "i32", -2147483647 - 1);
	ASSERT_NE(nullptr, pi);
	EXPECT_EQ(-2147483647 - 1, *pi);

	int64_t* pj = sb().allocate_int64(dom.c_str(), "s2", "i64", -9000000000LL);
	ASSERT_NE(nullptr, pj);
	EXPECT_EQ(-9000000000LL, *pj);

	uint32_t* pu = sb().allocate_uint32(dom.c_str(), "s2", "u32", 4294967295u);
	ASSERT_NE(nullptr, pu);
	EXPECT_EQ(4294967295u, *pu);

	uint64_t* pv = sb().allocate_uint64(dom.c_str(), "s2", "u64", 18446744073709551615ULL);
	ASSERT_NE(nullptr, pv);
	EXPECT_EQ(18446744073709551615ULL, *pv);

	// double 只做定长搬运，没有累加，所以用可精确表示的 0.1 直接比对位值
	double* pd = sb().allocate_double(dom.c_str(), "s2", "dbl", 0.1);
	ASSERT_NE(nullptr, pd);
	EXPECT_DOUBLE_EQ(0.1, *pd);

	const char* ps = sb().allocate_string(dom.c_str(), "s2", "str", "alpha");
	ASSERT_NE(nullptr, ps);
	EXPECT_STREQ("alpha", ps);

	// ---- set_* 覆盖后 get_* 必须读到新值
	ASSERT_TRUE(sb().set_int32(dom.c_str(), "s2", "i32", 12345));
	EXPECT_EQ(12345, sb().get_int32(dom.c_str(), "s2", "i32", -1));
	ASSERT_TRUE(sb().set_int64(dom.c_str(), "s2", "i64", 9223372036854775807LL));
	EXPECT_EQ(9223372036854775807LL, sb().get_int64(dom.c_str(), "s2", "i64", 0));
	ASSERT_TRUE(sb().set_uint32(dom.c_str(), "s2", "u32", 7u));
	EXPECT_EQ(7u, sb().get_uint32(dom.c_str(), "s2", "u32", 0u));
	ASSERT_TRUE(sb().set_uint64(dom.c_str(), "s2", "u64", 1ULL << 63));
	EXPECT_EQ(1ULL << 63, sb().get_uint64(dom.c_str(), "s2", "u64", 0ULL));
	ASSERT_TRUE(sb().set_double(dom.c_str(), "s2", "dbl", 3.75));
	EXPECT_DOUBLE_EQ(3.75, sb().get_double(dom.c_str(), "s2", "dbl", 0.0));
	ASSERT_TRUE(sb().set_string(dom.c_str(), "s2", "str", "beta"));
	EXPECT_STREQ("beta", sb().get_string(dom.c_str(), "s2", "str", "?"));

	// 返回的指针就是映射区里的同一块存储：写指针 = 写共享内存
	EXPECT_EQ(*pi, sb().get_int32(dom.c_str(), "s2", "i32", -1));
	EXPECT_EQ((const void*)ps, (const void*)sb().get_string(dom.c_str(), "s2", "str", ""));

	// 通过指针做十次 0.1 累加：0.1 不可精确表示，累加残差约 1.1e-16，
	// 所以这里按规则用 EXPECT_NEAR(期望 1.0, 实际, 1e-9)
	double* acc = sb().allocate_double(dom.c_str(), "s2", "acc", 0.0);
	ASSERT_NE(nullptr, acc);
	for (int i = 0; i < 10; i++)
		*acc += 0.1;
	EXPECT_NEAR(1.0, sb().get_double(dom.c_str(), "s2", "acc", 0.0), 1e-9);

	// 枚举：一个段、7 个 key
	auto secs = sb().get_sections(dom.c_str());
	ASSERT_EQ(1u, secs.size());
	EXPECT_EQ("s2", secs[0]);
	EXPECT_EQ(7u, sb().get_keys(dom.c_str(), "s2").size());

	// L294-303 未触发：同段名继续写入仍然成功
	EXPECT_TRUE(sb().set_int32(dom.c_str(), "s2", "i32", 1));
}

// ---------------------------------------------------------------- key 复用
TEST_F(R19TempDir, r19_reallocate_existing_key_keeps_stored_value)
{
	r19::attach_logger();
	const std::string dom = r19::domain("a3");
	const std::string path = file("a3.blk");
	ASSERT_TRUE(sb().init_master(dom.c_str(), path.c_str()));

	int32_t* first = sb().allocate_int32(dom.c_str(), "s3", "k", 5);
	ASSERT_NE(nullptr, first);
	EXPECT_EQ(5, *first);

	// make_valid L307-342：key 已存在 → 直接复用旧 KeyInfo；
	// allocate_int32 L459 的 `keyInfo->_type == 0` 为假 → 不写 initVal
	int32_t* again = sb().allocate_int32(dom.c_str(), "s3", "k", 777);
	ASSERT_NE(nullptr, again);
	EXPECT_EQ(first, again) << "同一个 key 必须落回同一格存储";
	EXPECT_EQ(5, *again) << "已存在的 key 不会被新的 initVal 覆盖（L459 条件为假）";
	EXPECT_EQ(5, sb().get_int32(dom.c_str(), "s3", "k", -1));

	// key 数量不增长（L336 只在新建分支 ++）
	EXPECT_EQ(1u, sb().get_keys(dom.c_str(), "s3").size());

	const char* s1 = sb().allocate_string(dom.c_str(), "s3", "s", "one");
	ASSERT_NE(nullptr, s1);
	const char* s2 = sb().allocate_string(dom.c_str(), "s3", "s", "two");
	ASSERT_NE(nullptr, s2);
	EXPECT_STREQ("one", s2) << "string 同样只在 _type==0 时用初始值";

	// ---- bForceWrite=true 让 allocate_* 的 `|| bForceWrite` 成立，用新初始值覆写已有值
	double* p = sb().allocate_double(dom.c_str(), "s3f", "d", 1.5);
	ASSERT_NE(nullptr, p);
	EXPECT_DOUBLE_EQ(1.5, *p);
	double* q = sb().allocate_double(dom.c_str(), "s3f", "d", 9.25, true);
	ASSERT_NE(nullptr, q);
	EXPECT_EQ(p, q) << "强制写不会另开格子";
	EXPECT_DOUBLE_EQ(9.25, *q);
	EXPECT_DOUBLE_EQ(9.25, sb().get_double(dom.c_str(), "s3f", "d", 0.0));

	EXPECT_EQ(42, *sb().allocate_int32(dom.c_str(), "s3", "k", 42, true));
	EXPECT_EQ(42, sb().get_int32(dom.c_str(), "s3", "k", -1)) << "只有 bForceWrite 才允许改已有值";

	const char* s3 = sb().allocate_string(dom.c_str(), "s3f", "s2", "keep");
	ASSERT_NE(nullptr, s3);
	EXPECT_STREQ("keep", s3);
	EXPECT_STREQ("force", sb().allocate_string(dom.c_str(), "s3f", "s2", "force", true));

	// 两个段都还在，缓存枚举口径不变
	EXPECT_EQ(2u, sb().get_sections(dom.c_str()).size());
}

// ---------------------------------------------------------------- 读侧闸门
TEST_F(R19TempDir, r19_check_valid_rejects_type_mismatch_and_missing_keys)
{
	r19::attach_logger();
	const std::string dom = r19::domain("a5");
	const std::string path = file("a5.blk");
	ASSERT_TRUE(sb().init_master(dom.c_str(), path.c_str()));

	ASSERT_NE(nullptr, sb().allocate_int32(dom.c_str(), "s5", "i", 6));
	ASSERT_NE(nullptr, sb().allocate_string(dom.c_str(), "s5", "s", "txt"));

	// check_valid L376-377：类型不符 → nullptr → get_* 回落 defVal
	// i 是 SMVT_INT32(1)，用其它类型的读法都必须落回默认值
	EXPECT_EQ(-7, sb().get_int64(dom.c_str(), "s5", "i", -7));
	EXPECT_EQ(4000000000u, sb().get_uint32(dom.c_str(), "s5", "i", 4000000000u));
	EXPECT_EQ(12345u, sb().get_uint64(dom.c_str(), "s5", "i", 12345u));
	// s 是 SMVT_STRING(6)，用数值类型读也必须回落默认值
	EXPECT_EQ(-33, sb().get_int32(dom.c_str(), "s5", "s", -33));
	EXPECT_EQ(-9.5, sb().get_double(dom.c_str(), "s5", "s", -9.5));
	// 类型正确时才读得到真实值
	EXPECT_EQ(6, sb().get_int32(dom.c_str(), "s5", "i", -1));
	EXPECT_STREQ("txt", sb().get_string(dom.c_str(), "s5", "s", "?"));

	// check_valid L369-372：key 不存在
	EXPECT_EQ(-11, sb().get_int32(dom.c_str(), "s5", "nokey", -11));
	EXPECT_STREQ("kk", sb().get_string(dom.c_str(), "s5", "nokey", "kk"));

	// check_valid L357-360：段不存在（域存在）
	EXPECT_EQ(-12, sb().get_int32(dom.c_str(), "nosection", "i", -12));
	EXPECT_STREQ("dd", sb().get_string(dom.c_str(), "nosection", "i", "dd"));
	EXPECT_EQ(0u, sb().get_section_updatetime(dom.c_str(), "nosection"));
	EXPECT_EQ(0u, sb().get_keys(dom.c_str(), "nosection").size());

	// check_valid L349-351：域不存在 → 全部回落默认值
	const std::string ghost = dom + "_ghost";
	EXPECT_EQ(-15, sb().get_int32(ghost.c_str(), "s5", "i", -15));
	EXPECT_EQ((uint64_t)0, sb().get_section_updatetime(ghost.c_str(), "s5"));
	EXPECT_TRUE(sb().get_sections(ghost.c_str()).empty());
	EXPECT_TRUE(r19::has_log("Domain"));   // get_sections L390 的 LL_ERROR
	EXPECT_TRUE(r19::has_log("not initialized"));
	EXPECT_EQ(0u, sb().get_keys(ghost.c_str(), "s5").size());

	// set_* 走 make_valid L254-256：域不存在 → nullptr → false
	EXPECT_FALSE(sb().set_int32(ghost.c_str(), "s", "k", 1));
	EXPECT_FALSE(sb().set_string(ghost.c_str(), "s", "k", "v"));
	EXPECT_FALSE(sb().delete_section(ghost.c_str(), "s"));   // L230-234
	EXPECT_TRUE(r19::has_log("not initialized"));
}

// ---------------------------------------------------------------- 容量边界
TEST_F(R19TempDir, r19_key_slot_limit_is_128_per_section)
{
	r19::attach_logger();
	const std::string dom = r19::domain("a6");
	const std::string path = file("a6.blk");
	ASSERT_TRUE(sb().init_master(dom.c_str(), path.c_str()));

	// MAX_KEY_CNT=128（ShareBlocks.h L19），int32 每个 4 字节，128 个只占 512 字节，
	// 所以只会撞到「key 槽位耗尽」这一条（ShareBlocks.cpp L317-321）
	for (int i = 0; i < MAX_KEY_CNT; i++)
	{
		const std::string k = "k" + std::to_string(i);
		int32_t* p = sb().allocate_int32(dom.c_str(), "full", k.c_str(), i);
		ASSERT_NE(nullptr, p) << "第 " << i << " 个 key 应能分配";
		EXPECT_EQ(i, *p);
	}
	EXPECT_EQ((size_t)MAX_KEY_CNT, sb().get_keys(dom.c_str(), "full").size());

	// 第 129 个：make_valid L317-321 → nullptr
	EXPECT_EQ(nullptr, sb().allocate_int32(dom.c_str(), "full", "overflow", 1));
	EXPECT_FALSE(sb().set_int32(dom.c_str(), "full", "overflow", 1));
	EXPECT_FALSE(sb().set_string(dom.c_str(), "full", "overflow", "x"));
	EXPECT_TRUE(r19::has_log("no spare slot for new kv pair"));

	// 槽位耗尽不影响既有 key 的读写
	EXPECT_EQ(0, sb().get_int32(dom.c_str(), "full", "k0", -1));
	EXPECT_TRUE(sb().set_int32(dom.c_str(), "full", "k0", 9));
	EXPECT_EQ(9, sb().get_int32(dom.c_str(), "full", "k0", -1));

	// 换一个段仍然可用（槽位是按段统计的）
	EXPECT_NE(nullptr, sb().allocate_int32(dom.c_str(), "other", "k", 3));
}

TEST_F(R19TempDir, r19_string_byte_budget_stops_at_1024_per_section)
{
	r19::attach_logger();
	const std::string dom = r19::domain("a7");
	const std::string path = file("a7.blk");
	ASSERT_TRUE(sb().init_master(dom.c_str(), path.c_str()));

	// SMVT_SIZES[SMVT_STRING]==64（ShareBlocks.h L31），_data[1024] → 只放得下 16 个
	// 第 16 个：_offset=960，960+64==1024，不满足 L323 的 `> 1024` → 放行
	for (int i = 0; i < 16; i++)
	{
		const std::string k = "s" + std::to_string(i);
		const std::string v = "v" + std::to_string(i);
		const char* p = sb().allocate_string(dom.c_str(), "bytes", k.c_str(), v.c_str());
		ASSERT_NE(nullptr, p) << "第 " << i << " 个 string key 应能分配";
		EXPECT_STREQ(v.c_str(), p);
	}
	// 第 17 个：_offset=1024 → 1024+64 > 1024 → L323-327 拒绝
	EXPECT_EQ(nullptr, sb().allocate_string(dom.c_str(), "bytes", "s16", "nope"));
	EXPECT_FALSE(sb().set_string(dom.c_str(), "bytes", "s16", "nope"));
	EXPECT_TRUE(r19::has_log("no enough bytes for new kv pair"));

	// 拒绝之后前 16 个值不受影响
	for (int i = 0; i < 16; i++)
	{
		const std::string k = "s" + std::to_string(i);
		const std::string v = "v" + std::to_string(i);
		EXPECT_STREQ(v.c_str(), sb().get_string(dom.c_str(), "bytes", k.c_str(), "<miss>"));
	}
	EXPECT_EQ(16u, sb().get_keys(dom.c_str(), "bytes").size());
}

TEST_F(R19TempDir, r19_section_slot_limit_64_and_delete_does_not_reclaim)
{
	r19::attach_logger();
	const std::string dom = r19::domain("a8");
	const std::string path = file("a8.blk");
	ASSERT_TRUE(sb().init_master(dom.c_str(), path.c_str()));

	// MAX_SEC_CNT=64（ShareBlocks.h L18）：64 个段各带 1 个 key
	for (int i = 0; i < MAX_SEC_CNT; i++)
	{
		const std::string s = "sec" + std::to_string(i);
		int32_t* p = sb().allocate_int32(dom.c_str(), s.c_str(), "v", i);
		ASSERT_NE(nullptr, p) << "第 " << i << " 个段应能建";
		EXPECT_EQ(i, *p);
	}
	EXPECT_EQ((size_t)MAX_SEC_CNT, sb().get_sections(dom.c_str()).size());

	// 第 65 个段：make_valid L273-278 → nullptr
	EXPECT_EQ(nullptr, sb().allocate_int32(dom.c_str(), "sec64", "v", 1));
	EXPECT_FALSE(sb().set_int32(dom.c_str(), "sec64", "v", 1));
	EXPECT_TRUE(r19::has_log("No spare slot for new section"));

	// delete_section（L227-250）只把 _state 置 2、不回收 _count 槽位
	// → 全删之后依然建不了新段：见缺陷 BUG-R19-003
	for (int i = 0; i < MAX_SEC_CNT; i++)
	{
		const std::string s = "sec" + std::to_string(i);
		ASSERT_TRUE(sb().delete_section(dom.c_str(), s.c_str())) << s;
	}
	EXPECT_TRUE(sb().get_sections(dom.c_str()).empty()) << "state!=1 的段不进枚举（L398）";
	EXPECT_EQ(nullptr, sb().allocate_int32(dom.c_str(), "after_delete", "v", 1))
		<< "段槽位未被 delete_section 释放";
	EXPECT_TRUE(r19::has_log("No spare slot for new section"));

	// 删一个不存在的段：L238-242 走 WARN 分支但返回 true
	EXPECT_TRUE(sb().delete_section(dom.c_str(), "already_gone"));
	EXPECT_TRUE(r19::has_log("already_gone not exists"));
}
