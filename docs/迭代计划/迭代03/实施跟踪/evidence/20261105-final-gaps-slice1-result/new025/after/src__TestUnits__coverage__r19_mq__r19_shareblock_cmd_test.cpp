/*!
 * \file r19_shareblock_cmd_test.cpp
 * \brief lane R19：ShareBlocks 的命令通道（init_cmder / add_cmd / get_cmd）
 *
 * 覆盖 src/WtShareHelper/ShareBlocks.cpp：
 * - init_cmder（建块、进程间冷启动锁、容量为 0 时 placement-new、cmder 落 pid）
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
#ifndef _WIN32
#include <sys/wait.h>
#endif

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

// 现状刻画：ShareBlocks.cpp 将新启动时丢弃旧命令作为策略；公开接口未明确新消费者是否应重放历史命令。
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
TEST_F(R19TempDir, r19_cmd_foreign_pid_blocks_add_and_init_preserves_indexes)
{
	const std::string domP = r19::domain("d3");
	const std::string path = file("d3.blk");

	const uint32_t foreign = r19::process_id() + 1u;
	std::vector<char> buf = craft_cmd_block(5u, 5u, foreign, 5, "atFive");
	ASSERT_TRUE(r19::write_raw_shm_block(path, buf.data(), buf.size()));

	ASSERT_TRUE(sb().init_cmder(domP.c_str(), false, path.c_str()));
	BoostMappingFile inspect;
	ASSERT_TRUE(inspect.map(path.c_str()));
	const CmdBlock* block = static_cast<const CmdBlock*>(inspect.addr());
	ASSERT_NE(nullptr, block);
	EXPECT_EQ(5u, block->_writable);
	EXPECT_EQ(5u, block->_readable);

	// 不是本机 pid → 拒写
	EXPECT_FALSE(sb().add_cmd(domP.c_str(), "nope"));

	// 读侧：lastIdx=4 < readable=5 → 取 _commands[5]（L786-790）
	uint32_t idx = 4u;
	EXPECT_STREQ("atFive", sb().get_cmd(domP.c_str(), idx));
	EXPECT_EQ(5u, idx);
	EXPECT_STREQ("", sb().get_cmd(domP.c_str(), idx));
	EXPECT_EQ(5u, idx);
}

// 跨圈的共享序号在 attach 后保持单调；物理槽位仍按 capacity 取模
TEST_F(R19TempDir, r19_cmd_wrapped_block_reads_by_capacity_modulo)
{
	const std::string domW = r19::domain("d4");
	const std::string path = file("d4.blk");

	// _writable=130（=2 圈外）、_readable=3、槽 2 里放一条历史命令
	std::vector<char> buf = craft_cmd_block(3u, 130u, r19::process_id(), 2, "wrapped");
	ASSERT_TRUE(r19::write_raw_shm_block(path, buf.data(), buf.size()));

	ASSERT_TRUE(sb().init_cmder(domW.c_str(), false, path.c_str()));
	// Attach 不得把共享单调序号 130/3 改写为槽位下标 2/3。
	BoostMappingFile inspect;
	ASSERT_TRUE(inspect.map(path.c_str()));
	CmdBlock* block = static_cast<CmdBlock*>(inspect.addr());
	ASSERT_NE(nullptr, block);
	EXPECT_EQ(130u, block->_writable);
	EXPECT_EQ(3u, block->_readable);

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

	// 换个名字以 cmder 身份 attach 同一份活动队列。
	const std::string domW2 = r19::domain("d4b");
	ASSERT_TRUE(sb().init_cmder(domW2.c_str(), true, path.c_str()));
	EXPECT_EQ(131u, block->_writable);
	EXPECT_EQ(130u, block->_readable);
	EXPECT_TRUE(sb().add_cmd(domW2.c_str(), "mine"));
	EXPECT_EQ(132u, block->_writable);
	EXPECT_EQ(131u, block->_readable);

	uint32_t idx2 = 130u;
	EXPECT_STREQ("mine", sb().get_cmd(domW.c_str(), idx2))
		<< "旧消费者应按单调序号读到 attach 后发布的命令";
	EXPECT_EQ(131u, idx2);
	EXPECT_STREQ("", sb().get_cmd(domW.c_str(), idx2));
	EXPECT_EQ(131u, idx2);
}

// 两个独立进程同时冷启动同一文件时，初始化锁只允许一个进程创建/初始化块。
#ifndef _WIN32
TEST_F(R19TempDir, r19_cmd_concurrent_cold_initialization_is_serialized)
{
	const std::string readerOne = r19::domain("d6_reader_one");
	const std::string readerTwo = r19::domain("d6_reader_two");
	const std::string path = file("d6_cold.blk");
	int gate[2] = { -1, -1 };
	ASSERT_EQ(0, ::pipe(gate));

	auto initialize_after_gate = [&](const std::string& domain) {
		::close(gate[1]);
		char start = 0;
		if (::read(gate[0], &start, 1) != 1)
			::_exit(2);
		::close(gate[0]);
		if (!sb().init_cmder(domain.c_str(), false, path.c_str()))
			::_exit(3);
		BoostMappingFile mapped;
		if (!mapped.map(path.c_str()) || mapped.size() < sizeof(CmdBlock))
			::_exit(4);
		const CmdBlock* block = static_cast<const CmdBlock*>(mapped.addr());
		if (block->_capacity != MAX_KEY_CNT || block->_writable != 0 || block->_readable != UINT32_MAX)
			::_exit(5);
		::_exit(0);
	};

	const pid_t first = ::fork();
	if (first == 0)
		initialize_after_gate(readerOne);
	if (first < 0)
	{
		::close(gate[0]);
		::close(gate[1]);
		FAIL() << "fork for first initializer failed";
	}

	const pid_t second = ::fork();
	if (second == 0)
		initialize_after_gate(readerTwo);
	if (second < 0)
	{
		::close(gate[0]);
		const char start = 'x';
		(void)::write(gate[1], &start, 1);
		::close(gate[1]);
		int status = 0;
		(void)::waitpid(first, &status, 0);
		FAIL() << "fork for second initializer failed";
	}

	::close(gate[0]);
	const char starts[2] = { 'x', 'y' };
	const ssize_t started = ::write(gate[1], starts, sizeof(starts));
	::close(gate[1]);
	int firstStatus = 0;
	int secondStatus = 0;
	ASSERT_EQ(first, ::waitpid(first, &firstStatus, 0));
	ASSERT_EQ(second, ::waitpid(second, &secondStatus, 0));
	ASSERT_EQ(static_cast<ssize_t>(sizeof(starts)), started);
	ASSERT_TRUE(WIFEXITED(firstStatus));
	ASSERT_TRUE(WIFEXITED(secondStatus));
	EXPECT_EQ(0, WEXITSTATUS(firstStatus));
	EXPECT_EQ(0, WEXITSTATUS(secondStatus));

	BoostMappingFile inspect;
	ASSERT_TRUE(inspect.map(path.c_str()));
	const CmdBlock* block = static_cast<const CmdBlock*>(inspect.addr());
	ASSERT_NE(nullptr, block);
	EXPECT_EQ(static_cast<uint32_t>(MAX_KEY_CNT), block->_capacity);
	EXPECT_EQ(0u, block->_writable);
	EXPECT_EQ(UINT32_MAX, block->_readable);
}

// 新 producer 在独立进程 attach 并接管后，旧 consumer 的单调游标仍能读取下一条命令。
TEST_F(R19TempDir, r19_cmd_cross_process_producer_takeover_preserves_consumer_cursor)
{
	const std::string oldProducer = r19::domain("d6_old_producer");
	const std::string consumer = r19::domain("d6_consumer");
	const std::string path = file("d6.blk");
	std::vector<char> buf = craft_cmd_block(130u, 131u, r19::process_id(), 2, "beforeAttach");
	ASSERT_TRUE(r19::write_raw_shm_block(path, buf.data(), buf.size()));
	ASSERT_TRUE(sb().init_cmder(oldProducer.c_str(), true, path.c_str()));
	ASSERT_TRUE(sb().init_cmder(consumer.c_str(), false, path.c_str()));

	uint32_t lastIdx = 129u;
	EXPECT_STREQ("beforeAttach", sb().get_cmd(consumer.c_str(), lastIdx));
	ASSERT_EQ(130u, lastIdx);

	const pid_t child = ::fork();
	ASSERT_GE(child, 0);
	if (child == 0)
	{
		const std::string newProducer = r19::domain("d6_new_producer");
		if (!sb().init_cmder(newProducer.c_str(), true, path.c_str()))
			::_exit(2);
		if (!sb().add_cmd(newProducer.c_str(), "afterAttach"))
			::_exit(3);
		::_exit(0);
	}

	int childStatus = 0;
	ASSERT_EQ(child, ::waitpid(child, &childStatus, 0));
	ASSERT_TRUE(WIFEXITED(childStatus));
	ASSERT_EQ(0, WEXITSTATUS(childStatus));

	// The child owns the producer PID now; the prior producer is fenced out.
	EXPECT_FALSE(sb().add_cmd(oldProducer.c_str(), "staleProducer"));
	EXPECT_STREQ("afterAttach", sb().get_cmd(consumer.c_str(), lastIdx));
	EXPECT_EQ(131u, lastIdx);

	BoostMappingFile inspect;
	ASSERT_TRUE(inspect.map(path.c_str()));
	const CmdBlock* block = static_cast<const CmdBlock*>(inspect.addr());
	ASSERT_NE(nullptr, block);
	EXPECT_EQ(132u, block->_writable);
	EXPECT_EQ(131u, block->_readable);
}
#else
TEST_F(R19TempDir, r19_cmd_concurrent_cold_initialization_is_serialized)
{
	GTEST_SKIP() << "fork-based independent-process regression is unavailable on Windows";
}

TEST_F(R19TempDir, r19_cmd_cross_process_producer_takeover_preserves_consumer_cursor)
{
	GTEST_SKIP() << "fork-based independent-process regression is unavailable on Windows";
}
#endif

// 命令字段是 char[64]，含 NUL 后最多存 63 字节。拒绝非法输入时，
// readable/writable 及目标和相邻槽都必须保持不变。
TEST_F(R19TempDir, r19_cmd_capacity_boundaries_reject_without_cursor_or_neighbor_mutation)
{
	const std::string domC = r19::domain("d5");
	const std::string domK = r19::domain("d5_reader");
	const std::string path = file("d5.blk");
	ASSERT_TRUE(sb().init_cmder(domC.c_str(), true, path.c_str()));
	ASSERT_TRUE(sb().init_cmder(domK.c_str(), false, path.c_str()));

	uint32_t lastIdx = UINT32_MAX;
	EXPECT_STREQ("", sb().get_cmd(domK.c_str(), lastIdx));
	EXPECT_EQ(999999u, lastIdx) << "consumer cold-start sentinel follows the existing get_cmd contract";

	BoostMappingFile inspect;
	ASSERT_TRUE(inspect.map(path.c_str()));
	CmdBlock* block = static_cast<CmdBlock*>(inspect.addr());
	ASSERT_NE(nullptr, block);
	ASSERT_EQ(0u, block->_writable);
	ASSERT_EQ(UINT32_MAX, block->_readable);

	block->_commands[0]._state = 0xA1B2C3D4u;
	std::strcpy(block->_commands[0]._command, "target-before");
	block->_commands[1]._state = 0x11223344u;
	std::strcpy(block->_commands[1]._command, "neighbor-before");

	auto expectUnchanged = [&]() {
		EXPECT_EQ(0u, block->_writable);
		EXPECT_EQ(UINT32_MAX, block->_readable);
		EXPECT_EQ(0xA1B2C3D4u, block->_commands[0]._state);
		EXPECT_STREQ("target-before", block->_commands[0]._command);
		EXPECT_EQ(0x11223344u, block->_commands[1]._state);
		EXPECT_STREQ("neighbor-before", block->_commands[1]._command);
	};

	EXPECT_FALSE(sb().add_cmd(nullptr, "null-name"));
	expectUnchanged();
	EXPECT_FALSE(sb().add_cmd(domC.c_str(), nullptr));
	expectUnchanged();

	const std::string tooLong64(static_cast<std::size_t>(MAX_CMD_SIZE), 'B');
	EXPECT_FALSE(sb().add_cmd(domC.c_str(), tooLong64.c_str()));
	expectUnchanged();
	const std::string tooLong65(static_cast<std::size_t>(MAX_CMD_SIZE + 1), 'C');
	EXPECT_FALSE(sb().add_cmd(domC.c_str(), tooLong65.c_str()));
	expectUnchanged();
	EXPECT_STREQ("", sb().get_cmd(domK.c_str(), lastIdx));
	EXPECT_EQ(999999u, lastIdx) << "rejected commands must not become readable";

	const std::string maxValid(static_cast<std::size_t>(MAX_CMD_SIZE - 1), 'A');
	ASSERT_TRUE(sb().add_cmd(domC.c_str(), maxValid.c_str()));
	EXPECT_EQ(1u, block->_writable);
	EXPECT_EQ(0u, block->_readable);
	EXPECT_EQ(0u, block->_commands[0]._state);
	EXPECT_STREQ(maxValid.c_str(), block->_commands[0]._command);
	EXPECT_EQ('\0', block->_commands[0]._command[MAX_CMD_SIZE - 1]);
	EXPECT_EQ(0x11223344u, block->_commands[1]._state);
	EXPECT_STREQ("neighbor-before", block->_commands[1]._command);
	EXPECT_STREQ(maxValid.c_str(), sb().get_cmd(domK.c_str(), lastIdx));
	EXPECT_EQ(0u, lastIdx);
	EXPECT_STREQ("", sb().get_cmd(domK.c_str(), lastIdx));
	EXPECT_EQ(0u, lastIdx);
}
