/*
 * R14 lane —— src/WtLatencyUFT/UftLatencyTool.cpp（整文件 0.00%，191 行未覆盖）
 *
 * 机制同 m_LoaderRunner：coverage/CMakeLists.txt 的 WT_MAIN_MODULES 循环对本模块目录
 * （main.cpp + UftLatencyTool.cpp）追加 -Dmain=wt_coverage_main_wtlatencyuft（全小写）。
 *
 * 为什么**不调**改名后的入口：UftLatencyTool.cpp 同目录的 main.cpp:25 `int main()` 无条件做
 *   test_uft()  ->  UftLatencyTool.cpp:28~34  runner.init(); runner.run();
 *   run() 里 :317 _parsers.run() / :318 _traders.run() / :320 _engine.run() 起线程，
 *   :322 theParser->run(_times) 是造 tick 的死循环；
 * 而 main.cpp:31 之后还有 `getchar()` —— 没有 tty 输入时直接阻塞整个 gtest 宿主。
 * 所以本文件只测 uft::UftLatencyTool 的构造/析构与 init() 的配置缺失早退支。
 *
 * 分支决定权：init() 读的是硬编码相对路径 "logcfg.yaml"/"config.yaml"（:200/:204），
 * 结果完全取决于运行目录里有没有这两个文件 —— 用例自己 chdir 进一个刚建的空目录，
 * 让「文件不存在」成为本用例可证伪的前提，退出时恢复原 cwd 并删掉临时目录。
 * 不起线程、不 sleep、不读挂钟。
 */
#include <gtest/gtest.h>

#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "WtLatencyUFT/UftLatencyTool.h"

namespace {

/* 把进程 cwd 换到一个空的私有目录，析构时恢复并删除 */
class ScopedEmptyCwd
{
private:
	char	m_backup[4096];
	char	m_enter[4096];
	bool	m_entered;
	bool	m_removed;

public:
	explicit ScopedEmptyCwd(const char* tag) : m_entered(false), m_removed(false)
	{
		m_backup[0] = '\0';
		m_enter[0] = '\0';
		if (getcwd(m_backup, sizeof(m_backup) - 1) == nullptr)
			return;

		std::string dir = std::string(m_backup) + "/r14_lazy_uft_" + tag + "_emptydir";
		rmdir(dir.c_str());					// 上一次异常退出可能留下的残骸（不存在则忽略）
		if (mkdir(dir.c_str(), 0755) != 0)
			return;
		if (chdir(dir.c_str()) != 0)
			return;

		m_entered = true;
		// 记下真正进入的目录名，恢复后再删
		if (getcwd(m_enter, sizeof(m_enter) - 1) != nullptr)
			;
	}

	~ScopedEmptyCwd()
	{
		if (m_entered)
			chdir(m_backup);
		if (m_entered && m_enter[0] != '\0')
			m_removed = (rmdir(m_enter) == 0);
	}

	bool ok() const { return m_entered; }
	bool cleaned() const { return m_removed; }

	ScopedEmptyCwd(const ScopedEmptyCwd&) = delete;
	ScopedEmptyCwd& operator=(const ScopedEmptyCwd&) = delete;
};

} // namespace

/* -------------------------------------------------------------------------
 * 一、uft::UftLatencyTool 构造/析构（UftLatencyTool.cpp:189~196）
 * ---------------------------------------------------------------------- */

TEST(r14_lazy_uft_tool, ctor_dtor_roundtrip_only_builds_member_managers)
{
	// :189~191 空构造体：只构造 TraderAdapterMgr/ParserAdapterMgr/UftStrategyMgr/WtUftEngine/WTSBaseDataMgr
	EXPECT_NO_THROW({ uft::UftLatencyTool tool; (void)tool; });
	// 第二次构造走同一条路径，验证没有「构造一次才干净」的隐藏状态
	EXPECT_NO_THROW({ uft::UftLatencyTool tool; (void)tool; });
}

/* -------------------------------------------------------------------------
 * 二、init() 的配置缺失早退（UftLatencyTool.cpp:198~209）
 * ---------------------------------------------------------------------- */

TEST(r14_lazy_uft_tool, init_without_config_yaml_returns_false_in_empty_cwd)
{
	ScopedEmptyCwd guard("init1");
	// 前提必须成立，否则后面的分支就不是本用例控制的
	ASSERT_TRUE(guard.ok()) << "无法进入/退出临时空目录，用例前提不成立";

	// :200 WTSLogger::init("logcfg.yaml") 文件不存在 -> 内部告警后返回
	// :204 load_from_file("config.yaml") -> WTSCfgLoader.cpp:300 exists() 为假 -> NULL
	// :205~208 记 error 后 return false
	uft::UftLatencyTool tool;
	EXPECT_FALSE(tool.init());
}

TEST(r14_lazy_uft_tool, init_is_repeatable_and_still_false_without_config)
{
	ScopedEmptyCwd guard("init2");
	ASSERT_TRUE(guard.ok());

	uft::UftLatencyTool tool;
	// 两次调用喂的是同一份「不存在」的输入 -> 结论必须一致，不能因为第一次失败就改变状态
	EXPECT_FALSE(tool.init());
	EXPECT_FALSE(tool.init());
}

TEST(r14_lazy_uft_tool, empty_cwd_is_really_empty_and_restored)
{
	char before[4096] = {0};
	ASSERT_NE(getcwd(before, sizeof(before)), nullptr);
	{
		ScopedEmptyCwd guard("check");
		ASSERT_TRUE(guard.ok());
		// 本用例的前提：目录里既没有 config.yaml 也没有 logcfg.yaml
		EXPECT_NE(::access("config.yaml", F_OK), 0);
		EXPECT_NE(::access("logcfg.yaml", F_OK), 0);
		EXPECT_TRUE(guard.cleaned() == false) << "仍在目录内部就能 rmdir，说明 guard 语义不对";
	}
	char after[4096] = {0};
	ASSERT_NE(getcwd(after, sizeof(after)), nullptr);
	EXPECT_STREQ(before, after) << "guard 退出后没有把 cwd 还原";
}

/* -------------------------------------------------------------------------
 * 三、DISABLED_：必须起引擎才能进的分支
 *
 * 触发条件（任一成立即不能在启用用例里跑）：
 *  D1 wt_coverage_main_wtlatencyuft()（main.cpp:25~32）：test_uft() -> init()+run()，
 *     之后 main.cpp:31 getchar() 在无 tty 的测试宿主里永久阻塞；启用即挂死。
 *     涉及行：main.cpp:25~32（5 行）+ UftLatencyTool.cpp:28~34（5 行）。
 *  D2 init() 的成功支（:211~259）：需要一份真实 config.yaml，且 :254 initEngine(_config->get("env"))
 *     在没有 env 段时把 NULL 传给 :279 _engine.init(cfg,...)（WtUftEngine 未做 NULL 防护），
 *     配置齐备则 :255 initModules() 的 :290/:297 ParserAdapter/TraderAdapter::initExt
 *     会向引擎/交易器注册并起线程。
 *     可回收行数估计：:211~259（49 行）+ :262~283（18 行）+ :286~303（16 行）≈ 83 行。
 *  D3 run()（:305~327）：:309 CpuHelper::bind_core 需要特权、:317~322 起三个线程并跑死循环。
 *     估计 23 行。
 *  D4 :38~186 的 TestParser/TestTrader/TestStrategy 是文件内类（头文件不暴露），
 *     只能在引擎回调里被驱动；用例侧无法构造 → 约 148 行同样要等引擎取样。
 * ---------------------------------------------------------------------- */

TEST(r14_lazy_uft_tool, DISABLED_entry_blocks_on_getchar)
{
	// D1：改名后的入口是 int wt_coverage_main_wtlatencyuft()，无参、内部 getchar()
	EXPECT_TRUE(true);
}

TEST(r14_lazy_uft_tool, DISABLED_init_success_needs_real_config_and_engine)
{
	// D2：需要真实 config.yaml + env 段，且引擎未 init 时传 NULL 会踩空
	uft::UftLatencyTool tool;
	EXPECT_TRUE(tool.init());
}

TEST(r14_lazy_uft_tool, DISABLED_run_starts_threads_and_busy_loop)
{
	// D3：_parsers.run()/_traders.run()/_engine.run()/theParser->run(_times)
	uft::UftLatencyTool tool;
	tool.run();
	EXPECT_TRUE(true);
}
