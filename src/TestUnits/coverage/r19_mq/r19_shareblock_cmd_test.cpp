/*!
 * \file r19_shareblock_cmd_test.cpp
 * \brief lane R19：ShareBlocks 的命令通道（init_cmder / add_cmd / get_cmd）
 *
 * 覆盖 src/WtShareHelper/ShareBlocks.cpp：
 * - L675-718 init_cmder（建块 L685-691、容量为 0 时 placement-new L697-698、
 *            cmder 落 pid L700-705、下标归一化 L709-715）
 * - L720-749 add_cmd（未知域 L722-724、pid 不符 L731-737、写入与推进 L743-748）
 * - L751-791 get_cmd（未知域/空块/cmder 拒读 L753-764、冷启动 L767-771、
 *            首次对齐 L772-776、999999 接续 L777-781、追平 L782-784、跨圈取模 L786-790）
 * 映射文件一律走显式 path，落在用例私有临时目录里。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <unistd.h>

#include "r19_support.h"

#include "WtShareHelper/ShareBlocks.h"

using namespace shareblock;

namespace {
ShareBlocks& sb() { return ShareBlocks::one(); }

/// 造一份命令块镜像：容量/读写下标/pid/槽位内容全部手工摆好，
/// 用来覆盖「开机时块里已经有历史下标」这类 init_cmder 归一化分支
inline std::vector<char> craft_cmd_block(uint32_t readable, uint32_t writable,
	uint32_t cmdpid, int slot, const char* text)
{
	std::vector<char> buf(sizeof(CmdBlock), 0);
	CmdBlock* blk = reinterpret_cast<CmdBlock*>(buf.data());
	blk->_capacity = MAX_KEY_CNT;		// 128，与 _CmdBlock<128> 一致
	blk->_readable = readable;
	blk->_writable = writable;
	blk->_cmdpid = cmdpid;
	if (slot >= 0 && text != NULL)
	{
		std::strncpy(blk->_commands[slot]._command, text, MAX_CMD_SIZE - 1);
		blk->_commands[slot]._state = 1;
	}
	return buf;
}
} // namespace

// 未初始化的命令域：写被拒、读回空串且不动 lastIdx
TEST_F(R19TempDir, r19_cmd_unknown_name_add_false_get_empty)
{
	const std::string ghost = r19::domain("d0");
	EXPECT_FALSE(sb().add_cmd(ghost.c_str(), "ping"));

	uint32_t idx = 12345u;
	const char* got = sb().get_cmd(ghost.c_str(), idx);
	EXPECT_STREQ("", got) << "L753-755：域不存在返回空串";
	EXPECT_EQ(12345u, idx) << "lastIdx 必须原样保留";

	// _block 为空（init_cmder 之后 block 被置空没有入口，这里只验不崩）
	idx = UINT32_MAX;
	EXPECT_STREQ("", sb().get_cmd(r19::domain("d0b").c_str(), idx));
}

// 冷启动 + 消费者中途接入：第一条已入队的命令会被跳过（BUG-R19-006）
TEST_F(R19TempDir, r19_cmd_cmder_writes_and_consumer_joining_later_skips_first)
{
	const std::string domC = r19::domain("d1c");
	const std::string domK = r19::domain("d1k");
	const std::string path = file("d1.blk");

	std::error_code ec;
	ASSERT_TRUE(sb().init_cmder(domC.c_str(), true, path.c_str()));
	ASSERT_TRUE(std::filesystem::exists(path, ec));
	EXPECT_EQ(sizeof(CmdBlock), (size_t)std::filesystem::file_size(path, ec))
		<< "L689 truncate_file(sizeof(CmdBlock))";

	// 新块：_capacity==0 → L697-698 placement-new（_readable=UINT32_MAX, _writable=0）
	//       且 isCmder=true → L702/704 写入本进程 pid
	EXPECT_TRUE(sb().add_cmd(domC.c_str(), "c0")) << "L720-748：pid 相符才允许写";

	// 消费者（同文件、非 cmder）中途接入
	ASSERT_TRUE(sb().init_cmder(domK.c_str(), false, path.c_str()));

	uint32_t idx = UINT32_MAX;
	EXPECT_STREQ("", sb().get_cmd(domK.c_str(), idx))
		<< "L772-776：首帧只做对齐，索引 0 上的 c0 永远不会被投喂";
	EXPECT_EQ(0u, idx);

	// 下一条命令才能被读到
	EXPECT_TRUE(sb().add_cmd(domC.c_str(), "c1"));
	EXPECT_STREQ("c1", sb().get_cmd(domK.c_str(), idx));
	EXPECT_EQ(1u, idx);

	// 追平之后返回空串且不动下标（L782-784）
	EXPECT_STREQ("", sb().get_cmd(domK.c_str(), idx));
	EXPECT_EQ(1u, idx);

	// 强行把 lastIdx 拨回 0 也拿不到 c0：它会直接被当成「上一条」再发一次 c1
	idx = 0u;
	EXPECT_STREQ("c1", sb().get_cmd(domK.c_str(), idx)) << "c0 已经不可达";
	EXPECT_EQ(1u, idx);

	// 正常推进到第 3 条
	EXPECT_TRUE(sb().add_cmd(domC.c_str(), "c2"));
	EXPECT_STREQ("c2", sb().get_cmd(domK.c_str(), idx));
	EXPECT_EQ(2u, idx);

	// cmder 自己不需要取指令（L763-764）
	idx = 0u;
	EXPECT_STREQ("", sb().get_cmd(domC.c_str(), idx));
	EXPECT_EQ(0u, idx) << "cmd 角色直接拒读，lastIdx 不动";
}

// 空块冷启动：readable==UINT32_MAX → 999999 哨兵 → 首条命令正常投喂
TEST_F(R19TempDir, r19_cmd_cold_start_sentinel_then_delivers_first_command)
{
	const std::string domL = r19::domain("d2l");
	const std::string domM = r19::domain("d2m");
	const std::string path = file("d2.blk");

	// 先以「消费者」身份建块：isCmder=false → _cmdpid 保持 0
	ASSERT_TRUE(sb().init_cmder(domL.c_str(), false, path.c_str()));

	uint32_t idx = UINT32_MAX;
	EXPECT_STREQ("", sb().get_cmd(domL.c_str(), idx));
	EXPECT_EQ(999999u, idx) << "L767-771：刚建块，历史命令全部作废，置 999999 哨兵";

	// 再以「下达者」身份接管同一份文件（走 L700-705 写 pid、L710 保持哨兵）
	ASSERT_TRUE(sb().init_cmder(domM.c_str(), true, path.c_str()));
	ASSERT_TRUE(sb().add_cmd(domM.c_str(), "first"));

	// 999999 分支：L777-781 → 直接投喂 _commands[0]
	EXPECT_STREQ("first", sb().get_cmd(domL.c_str(), idx));
	EXPECT_EQ(0u, idx);

	// 追平 → 空串
	EXPECT_STREQ("", sb().get_cmd(domL.c_str(), idx));

	// 下一条继续正常投喂
	ASSERT_TRUE(sb().add_cmd(domM.c_str(), "second"));
	EXPECT_STREQ("second", sb().get_cmd(domL.c_str(), idx));
	EXPECT_EQ(1u, idx);

	// 同名重复 init_cmder → L678-679 直接 true，不重复建映射
	EXPECT_TRUE(sb().init_cmder(domL.c_str(), false, path.c_str()));
}

// 外来 pid 的命令块：add_cmd 被 pid 闸门拦下（L731-737）
TEST_F(R19TempDir, r19_cmd_foreign_pid_blocks_add_and_init_normalizes_indexes)
{
	const std::string domP = r19::domain("d3");
	const std::string path = file("d3.blk");

	const uint32_t foreign = (uint32_t)::getpid() + 1u;
	std::vector<char> buf = craft_cmd_block(5u, 5u, foreign, 5, "atFive");
	ASSERT_TRUE(r19::write_raw_shm_block(path, buf.data(), buf.size()));

	ASSERT_TRUE(sb().init_cmder(domP.c_str(), false, path.c_str()));
	// L709-715：5 % 128 == 5，readable 5 不大于 writable 5 → 都不动

	// 不是本机 pid → 拒写
	EXPECT_FALSE(sb().add_cmd(domP.c_str(), "nope"));

	// 读侧：lastIdx=4 < readable=5 → 取 _commands[5]（L786-790）
	uint32_t idx = 4u;
	EXPECT_STREQ("atFive", sb().get_cmd(domP.c_str(), idx));
	EXPECT_EQ(5u, idx);
	EXPECT_STREQ("", sb().get_cmd(domP.c_str(), idx));
	EXPECT_EQ(5u, idx);
}

// writable 已经跨圈：init_cmder 把下标拉回「readable 之后」，读取按 capacity 取模
TEST_F(R19TempDir, r19_cmd_wrapped_block_reads_by_capacity_modulo)
{
	const std::string domW = r19::domain("d4");
	const std::string path = file("d4.blk");

	// _writable=130（=2 圈外）、_readable=3、槽 2 里放一条历史命令
	std::vector<char> buf = craft_cmd_block(3u, 130u, (uint32_t)::getpid(), 2, "wrapped");
	ASSERT_TRUE(r19::write_raw_shm_block(path, buf.data(), buf.size()));

	ASSERT_TRUE(sb().init_cmder(domW.c_str(), false, path.c_str()));
	// L709：130 % 128 = 2；L710-714：readable 3 > writable 2 → writable 补回 128 = 130

	uint32_t idx = 1u;
	EXPECT_STREQ("wrapped", sb().get_cmd(domW.c_str(), idx));
	EXPECT_EQ(2u, idx) << "1 < readable(3) → lastIdx++=2 → _commands[2 % 128]";

	// 本机 pid 相符 → 可以写；写入落在 130 % 128 = 2 号槽，把历史覆盖掉
	ASSERT_TRUE(sb().add_cmd(domW.c_str(), "new130"));
	idx = 129u;
	EXPECT_STREQ("new130", sb().get_cmd(domW.c_str(), idx));
	EXPECT_EQ(130u, idx) << "L743-747：wIdx=130 → 槽位 2；L789 再按 130 % 128 取回";

	// 追平
	EXPECT_STREQ("", sb().get_cmd(domW.c_str(), idx));
	EXPECT_EQ(130u, idx);

	// 换个名字以 cmder 身份接管同一份文件：L700-705 会把 pid 改成自己，
	// 更关键的是 L709-714 会对共享块里的下标「再归一化」一次
	const std::string domW2 = r19::domain("d4b");
	ASSERT_TRUE(sb().init_cmder(domW2.c_str(), true, path.c_str()));
	// 此刻文件里 _writable 由 131 被改成 131 % 128 = 3，_readable 由 130 改成 2；
	// add_cmd 再写一次，_readable 直接落到 3（缺陷 BUG-R19-007）
	EXPECT_TRUE(sb().add_cmd(domW2.c_str(), "mine"));

	uint32_t idx2 = 130u;
	EXPECT_STREQ("", sb().get_cmd(domW.c_str(), idx2))
		<< "readable 被第二个映射改小之后，老消费者的 lastIdx 再也追不上";
	EXPECT_EQ(130u, idx2);

	// 把 lastIdx 拨回 2 反而能读到刚落在槽位 3 的命令，证明共享下标已被重置
	idx2 = 2u;
	EXPECT_STREQ("mine", sb().get_cmd(domW.c_str(), idx2));
	EXPECT_EQ(3u, idx2);
}

/**
 * \DISABLED 触发条件（本轮刻意不跑，会写坏映射块/越界）：
 * add_cmd（ShareBlocks.cpp L746）用的是不受限的 strcpy，把命令拷进
 * CmdInfo::_command（char[64]，ShareBlocks.h L84）。
 * 传入长度 >= 64 字节的命令就会连续越过 _state 与下一条命令的槽位；
 * 若写入的正是最后一个槽（idx 127），还会越过映射文件尾 36 字节 → SIGSEGV。
 * 下面这条用例就是把长度 100 的命令喂进去，属于未定义行为，只能留在源码评审里。
 */
TEST_F(R19TempDir, DISABLED_r19_cmd_longer_than_64_bytes_writes_past_the_block)
{
	const std::string domC = r19::domain("d5");
	const std::string path = file("d5.blk");
	ASSERT_TRUE(sb().init_cmder(domC.c_str(), true, path.c_str()));
	const std::string tooLong(100, 'A');
	EXPECT_TRUE(sb().add_cmd(domC.c_str(), tooLong.c_str()));
}
