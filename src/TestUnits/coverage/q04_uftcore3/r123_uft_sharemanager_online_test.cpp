/*!
 * \file r123_uft_sharemanager_online_test.cpp
 * \brief 第二十波 R123 - ShareManager 成功装载 WtShareHelper 后的转发面（fork 隔离）
 *
 * 覆盖（只读生产代码）:
 *  - ShareManager.cpp:151-158 commit_param_watcher 在已初始化但未 init_domain 时
 *    调 WtShareHelper::commit_section 返回 false，同时登记 _secnames。
 *  - ShareManager.cpp:171-266 六个 set_value 重载：域不存在时 helper 返回 false。
 *  - ShareManager.cpp:280-380 六个 get_value 重载：域不存在时原样返回默认值。
 *  - ShareManager.cpp:396-506 六个 allocate_value 重载：域不存在时返回 nullptr。
 *  - ShareManager.cpp:78-114 start_watching：起线程、遍历 _secnames 调
 *    _get_section_updatetime（域不存在 -> 0）、在 microsecs>0 时休眠。
 *
 * 为什么 fork：ShareManager 是进程级单例，一旦成功 initialize 就没有公开的反初始化入口。
 * 在父进程装载会破坏 r11/r85 对「未初始化态」的断言；子进程完成断言后 exit(0)，
 * 父进程的单例始终未初始化。
 *
 * 安全边界：只 dlopen WtShareHelper.dylib 并调 get/set/allocate/commit；
 * 不调 init_domain/init_master，不创建共享内存或磁盘映射段，不接网络。
 */
#include "r123_uft_common.h"

#include "WtUftCore/ShareManager.h"

#include <gtest/gtest.h>

#include <cstring>
#include <functional>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using namespace wtp;

namespace {

struct R123SmReport
{
	int32_t	fail_line = 0;
	int32_t	marks = 0;
};

#define R123_SM_CHK(rep, cond) \
	do { if (!(cond)) { if ((rep).fail_line == 0) (rep).fail_line = __LINE__; } } while (0)

class R123ShareManagerOnlineTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_lib = r123::findLib("libWtShareHelper.dylib");
	}

	void run_child(const std::function<void(R123SmReport&)>& fn)
	{
		if (_lib.empty())
			GTEST_SKIP() << "libWtShareHelper.dylib not found in this build";

		int fds[2] = { -1, -1 };
		ASSERT_EQ(::pipe(fds), 0);

		fflush(nullptr);
		pid_t pid = ::fork();
		ASSERT_GE(pid, 0);
		if (pid == 0)
		{
			::close(fds[0]);
			R123SmReport rep;
			fn(rep);
			ssize_t nw = ::write(fds[1], &rep, sizeof(rep));
			::close(fds[1]);
			::exit(nw == (ssize_t)sizeof(rep) ? 0 : 3);
		}

		::close(fds[1]);
		R123SmReport rep;
		memset(&rep, 0, sizeof(rep));
		ssize_t nr = ::read(fds[0], &rep, sizeof(rep));
		::close(fds[0]);

		int status = 0;
		ASSERT_EQ(::waitpid(pid, &status, 0), pid);
		ASSERT_TRUE(WIFEXITED(status)) << "子进程异常退出, status=" << status;
		ASSERT_EQ(WEXITSTATUS(status), 0);
		ASSERT_EQ(nr, (ssize_t)sizeof(rep)) << "子进程未回传报告";
		EXPECT_EQ(rep.fail_line, 0) << "子进程检查失败行 " << rep.fail_line;
		EXPECT_EQ(rep.marks, 20);
	}

	std::string _lib;
};

// 已装载 WtShareHelper、但从未 init_domain：所有读回到默认值，所有写/分配失败。
// 这条路径把 ShareManager 的 19 个轻量转发入口一次走完，且不产生任何共享内存。
TEST_F(R123ShareManagerOnlineTest, initialized_wrappers_fall_back_without_domain)
{
	run_child([this](R123SmReport& r) {
		ShareManager& sm = ShareManager::self();

		// ShareManager.cpp:26-66 成功装载
		R123_SM_CHK(r, sm.initialize(_lib.c_str()));

		// :151-158 域不存在 -> commit_section false，但 _secnames 仍登记
		R123_SM_CHK(r, !sm.commit_param_watcher("r123_sec"));

		// :280-380 六个 get_value 重载
		R123_SM_CHK(r, strcmp(sm.get_value("sec", "str", "dflt"), "dflt") == 0);
		R123_SM_CHK(r, sm.get_value("sec", "i32", (int32_t)-11) == -11);
		R123_SM_CHK(r, sm.get_value("sec", "i64", (int64_t)-9876543210LL) == -9876543210LL);
		R123_SM_CHK(r, sm.get_value("sec", "u32", (uint32_t)4000000001U) == 4000000001U);
		R123_SM_CHK(r, sm.get_value("sec", "u64", (uint64_t)18446744073709551610ULL) == 18446744073709551610ULL);
		R123_SM_CHK(r, sm.get_value("sec", "dbl", 2.75) == 2.75);

		// :171-266 六个 set_value 重载（域不存在 -> false）
		R123_SM_CHK(r, !sm.set_value("sec", "dbl", 1.0));
		R123_SM_CHK(r, !sm.set_value("sec", "i32", (int32_t)1));
		R123_SM_CHK(r, !sm.set_value("sec", "i64", (int64_t)1));
		R123_SM_CHK(r, !sm.set_value("sec", "u32", (uint32_t)1));
		R123_SM_CHK(r, !sm.set_value("sec", "u64", (uint64_t)1));
		R123_SM_CHK(r, !sm.set_value("sec", "str", "x"));

		// :396-506 六个 allocate_value 重载（域不存在 -> nullptr）
		R123_SM_CHK(r, sm.allocate_value("sec", "str", "x") == nullptr);
		R123_SM_CHK(r, sm.allocate_value("sec", "i32", (int32_t)1) == nullptr);
		R123_SM_CHK(r, sm.allocate_value("sec", "i64", (int64_t)1) == nullptr);
		R123_SM_CHK(r, sm.allocate_value("sec", "u32", (uint32_t)1) == nullptr);
		R123_SM_CHK(r, sm.allocate_value("sec", "u64", (uint64_t)1) == nullptr);
		R123_SM_CHK(r, sm.allocate_value("sec", "dbl", 1.0) == nullptr);

		r.marks = 20;
	});
}

// ShareManager.cpp:83-114 start_watching：_secnames 非空 -> 工作线程遍历并查询更新时间，
// 域不存在时返回 0（不会触发 _engine->notify_params_update）；microsecs>0 走休眠分支。
// 子进程 exit 时单例析构 join，线程只活几毫秒。
TEST_F(R123ShareManagerOnlineTest, start_watching_without_domain)
{
	run_child([this](R123SmReport& r) {
		ShareManager& sm = ShareManager::self();
		R123_SM_CHK(r, sm.initialize(_lib.c_str()));
		R123_SM_CHK(r, !sm.commit_param_watcher("r123_watch"));
		R123_SM_CHK(r, sm.start_watching(5000));
		// 重复 start 只返回 true，不重复建线程
		R123_SM_CHK(r, sm.start_watching(5000));

		r.marks = 20;
	});
}

}	// namespace
