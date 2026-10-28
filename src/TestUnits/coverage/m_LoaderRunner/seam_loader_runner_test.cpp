/*
 * 通路探针（非行为断言）：验证「带 main() 的可执行体」能在不改产品的前提下进入覆盖率测试——
 * 生产 TU 以 -Dmain=<模块>_coverage_main 编译，测试按受控 argv 调用它。
 * 选 --help 分支：LoaderRunner.cpp 该分支会 return 0，能走完 cppcli 参数定义与 parse 又不会阻塞。
 */
#include <gtest/gtest.h>

#include <vector>

int wt_coverage_main_loaderrunner(int argc, char* argv[]);

TEST(seam_loader_runner, help_path_returns_without_blocking)
{
	std::vector<char*> args;
	args.push_back(const_cast<char*>("loader_runner"));
	args.push_back(const_cast<char*>("-h"));

	int rc = wt_coverage_main_loaderrunner(static_cast<int>(args.size()), args.data());
	EXPECT_EQ(rc, 0);
}
