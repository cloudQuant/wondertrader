/*
 * R6 lane —— src/WtRunner/（可执行体 + WtRunner 类）
 *
 * 形状照抄 m_LoaderRunner/seam_loader_runner_test.cpp：构建期由 coverage/CMakeLists.txt
 * 对本模块目录下的每个 .cpp 追加 -Dmain=wt_coverage_main_wtrunner（小写、实测 LoaderRunner 已跑通），
 * 于是 main.cpp 的入口符号变成 wt_coverage_main_wtrunner，与本文件的 gtest_main 同链一个二进制。
 *
 * 安全口径（本轮硬规则）：
 *  - 只调用能自行终止、不起线程、不阻塞、不 exit() 宿主的路径；
 *  - main.cpp 里 -h/--help 分支：R6 当时以为它在 `WtRunner runner;` 之前 return 0，
 *    R28 实测更正 —— cppcli 在 parse() 内部就 std::exit(0)（Share/cppcli.hpp:1897-1900），
 *    main.cpp:86-87 的 return 0 是死代码，所以本文件的三条 help 用例已转 DISABLED_，
 *    help 分支的覆盖改由 r28_runner_main_test.cpp 用 fork 子进程钉住；
 *  - 其余入口分支（无参/有 -c 无 -l/配置缺失）会继续走到 runner.run(false)：
 *      run() 里 _engine 从未被 initEngine() 赋值（config() 提前 return false）→ 空指针调用，
 *      且 _parsers.run()/_traders.run() 会起线程、末尾 while(!_to_exit) 睡 10ms 死循环。
 *    这些一律 DISABLED_，触发条件写在名字下面的注释里。
 *  - WtRunner.cpp 侧只走「不依赖 libWtCore 运行期」的成员函数（构造/析构/init/config 早退），
 *    并且必须堆上构造（见下方 BUG-R28-001 说明）。
 *
 * 关联缺陷见 docs/迭代计划/迭代01/bugs/R6_porter_runner.md（BUG-R6-011 起）。
 */
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "WtCore/WtHelper.h"
#include "WtRunner/WtRunner.h"

/* 生产 TU（main.cpp）经 -Dmain=... 改名后的入口；与测试自己的 gtest_main 不冲突 */
int wt_coverage_main_wtrunner(int argc, char* argv[]);

/* 生产 TU（WtRunner.cpp:26）的自由函数，未在 WtRunner.h 里声明，按定义处重新声明 */
const char* getBinDir();

/* 生产 TU（WtRunner.cpp:614）的级别->标签表，handleLogAppend 按 ll-100 取用 */
extern const char* LOG_TAGS[];

namespace {

/* 用受控 argv 调入口；argv[0] 只是程序名，cppcli 不校验它 */
int call_entry(std::vector<const char*> words)
{
	std::vector<char*> argv;
	argv.reserve(words.size());
	for (const char* w : words)
		argv.push_back(const_cast<char*>(w));
	return wt_coverage_main_wtrunner(static_cast<int>(argv.size()), argv.data());
}

} // namespace

/* -------------------------------------------------------------------------
 * 一、入口 --help 分支（main.cpp:51~87）
 *
 * R28 更正（见 manifests/R28_runner_mains.md 的诊断一节）：本族的 -h/--help
 * 断言原来写成 EXPECT_EQ(call_entry(...), 0)，实测判决是 **NORUN 而不是 PASS**。
 * 原因是 cppcli 的 help 处理在 src/Share/cppcli.hpp:1897-1900 里
 * `std::cout << buildHelpDoc(); std::exit(0);` —— 它在 opt.parse() 内部就把
 * 整个宿主进程收尾了，main.cpp:86-87 的 `return 0` 是永远走不到的死代码。
 * 所以这三条改成 DISABLED_（help 分支的真实覆盖由 r28_runner_main_test.cpp
 * 用 fork 子进程的方式钉住），下面三条保留作为「为什么不能在本进程里调」的记录。
 * ---------------------------------------------------------------------- */

TEST(r6_wtrunner_entry, DISABLED_short_help_returns_zero_before_touching_runner)
{
	// main.cpp:86 if(hParam->exists()) return 0;  —— 实际到不了，cppcli 已 exit(0)
	EXPECT_EQ(call_entry({"wt_runner", "-h"}), 0);
}

TEST(r6_wtrunner_entry, DISABLED_long_help_returns_zero)
{
	EXPECT_EQ(call_entry({"wt_runner", "--help"}), 0);
}

TEST(r6_wtrunner_entry, DISABLED_help_wins_over_config_and_logcfg_words)
{
	// -h 与 -c/-l 同时给出：parse() 后 hParam->exists() 仍为真，早退先于任何文件访问
	EXPECT_EQ(call_entry({"wt_runner", "-c", "./nonexistent_r6.yaml",
		"-l", "./nonexistent_log_r6.yaml", "-h"}), 0);
}

/* -------------------------------------------------------------------------
 * 二、WtRunner.cpp:26~37 getBinDir()
 * ---------------------------------------------------------------------- */

TEST(r6_wtrunner_binroot, getBinDir_is_cwd_and_stable_across_calls)
{
	const char* first = getBinDir();
	ASSERT_NE(first, nullptr);
	// 第二次调用走 if(basePath.empty()) 的假分支，返回同一块内存
	const char* second = getBinDir();
	EXPECT_EQ(first, second) << "getBinDir() 的 static 缓存每次都重新算路径";
	EXPECT_GT(std::string(first).size(), 1u) << "初始工作目录不该是空串或单字符";
}

/* -------------------------------------------------------------------------
 * 三、WtRunner.cpp:41~65 构造/析构 + init()
 *
 * R28 更正：下面四条原来是 `WtRunner runner;`（测试栈帧上构造），实测一律
 * **SIGKILL(9)** —— 不是原注释说的「空指针段错误」，进程在构造/析构阶段就被打死，
 * profraw 归零，WtRunner.cpp 一行都记不上。同一类型改成堆上 new 出来
 * ctor/dtor 全程正常（实测对照见 manifests/R28_runner_mains.md），
 * 所以这里统一换成 unique_ptr 持有（BUG-R28-001）。
 * ---------------------------------------------------------------------- */

TEST(r6_wtrunner_init, ctor_dtor_roundtrip_routes_declared_levels_only)
{
	std::unique_ptr<WtRunner> runner(new WtRunner());
	// WtRunner.cpp:626 取用式是 LOG_TAGS[ll-100]；Includes/WTSTypes.h:128 LL_ALL=100..LL_NONE=106
	for (uint32_t ll = LL_ALL; ll <= LL_NONE; ll++)
	{
		const uint32_t idx = ll - 100u;
		ASSERT_LT(idx, 7u) << "级别枚举与 LOG_TAGS 长度不同步: ll=" << ll;
		// 未 init 通知器时 _mq_sid==0，notify_log 立刻返回（WtCore/EventNotifier.cpp:116）
		EXPECT_NO_THROW(runner->handleLogAppend(static_cast<WTSLogLevel>(ll), "r6 probe"))
			<< "ll=" << ll;
	}
}

TEST(r6_wtrunner_init, init_with_missing_logcfg_returns_and_stamps_inst_dir)
{
	std::unique_ptr<WtRunner> runner(new WtRunner());
	WtHelper::setInstDir("");
	// WtRunner.cpp:57 WTSLogger::init(不存在的路径) -> 内部告警；:59 setInstDir；:61 warn
	EXPECT_NO_THROW(runner->init("./r6_missing_logcfg.yaml"));
	// :59 的 setInstDir 是无条件执行的，能反证 init() 没在中途崩
	EXPECT_FALSE(WtHelper::getInstDir().empty());
}

/* -------------------------------------------------------------------------
 * 四、WtRunner.cpp:67~74 config() 加载失败早退
 * ---------------------------------------------------------------------- */

TEST(r6_wtrunner_config, missing_file_returns_false)
{
	std::unique_ptr<WtRunner> runner(new WtRunner());
	EXPECT_FALSE(runner->config("./r6_definitely_absent_cfg.yaml"));
}

TEST(r6_wtrunner_config, log_tag_table_matches_the_seven_declared_levels)
{
	// WtRunner.cpp:614-622 与 WtPorter 侧同名表内容一致，但各 TU 各定义一份
	EXPECT_STREQ(LOG_TAGS[0], "all");
	EXPECT_STREQ(LOG_TAGS[1], "debug");
	EXPECT_STREQ(LOG_TAGS[4], "error");
	EXPECT_STREQ(LOG_TAGS[6], "none");
}

TEST(r6_wtrunner_config, config_error_path_leaves_object_reusable)
{
	std::unique_ptr<WtRunner> runner(new WtRunner());
	ASSERT_FALSE(runner->config("./r6_definitely_absent_cfg.yaml"));
	// 第二次加载不同路径仍走同一条早退支，不因为第一次失败而改变行为
	EXPECT_FALSE(runner->config("./r6_another_absent_cfg.yaml"));
}

/* -------------------------------------------------------------------------
 * 五、DISABLED_：入口的其余分支
 *
 * 触发条件（任一成立即不能在启用用例里跑）：
 *  D0 (R28 实测补充) 只要 main.cpp:104 的 `WtRunner runner;` 被执行到，进程就被
 *     **SIGKILL(9)** 打死（0.1s 内消失、无 core、peak RSS 只 32MB，不是栈溢出，
 *     sizeof(WtRunner)=5232）。堆上 new 同一个类则完全正常 → BUG-R28-001。
 *     也就是说下面 D1~D3 描述的「到底走到哪一步」在本机根本观察不到，
 *     进程在更早的位置就没了。
 *  D1 argc==1 或只给 -c/-l：hParam 不存在 -> 落到 runner.config(filename)；
 *     配置文件不存在 -> config() 在 :74 return false（_config 保持 NULL）
 *     -> main.cpp:125 runner.run(false) -> :585 _engine->run()，_engine 从未赋值（NULL）
 *     -> 段错误，直接打死 gtest 宿主进程（catch(...) 抓不到 SIGSEGV）。
 *  D2 配置解析成功但缺 basefiles -> WtRunner.cpp:77~78 cfgBF 为 NULL 后立刻
 *     cfgBF->get("session") -> 空指针解引用，同样打死宿主。
 *  D3 配置齐备 -> initEngine()/run() 起引擎线程 + while(!_to_exit) 每 10ms 轮询的死循环，
 *     且需要真实 shm 与行情源；启用用例里等于挂死。
 * 这些分支的覆盖率只能留给主控裁定（进程外采样或改产品注入点）。
 * ---------------------------------------------------------------------- */

TEST(r6_wtrunner_entry, DISABLED_bare_argv_falls_through_to_null_engine_run)
{
	// D1：无参数 -> ./logcfg.yaml + ./config.yaml -> run(false) 空指针；见上方触发条件
	EXPECT_EQ(call_entry({"wt_runner"}), 0);
}

TEST(r6_wtrunner_entry, DISABLED_missing_config_file_still_runs)
{
	// D1：显式给一个不存在的 -c，行为与裸参数同路径
	EXPECT_EQ(call_entry({"wt_runner", "-c", "./r6_absent_dtcfg.yaml"}), 0);
}

TEST(r6_wtrunner_config, DISABLED_empty_object_config_derefs_null_basefiles)
{
	// D2：{} 是合法 YAML，_config 非空；缺 "basefiles" -> WtRunner.cpp:78 空指针
	WtRunner runner;
	EXPECT_FALSE(runner.config("./r6_empty_cfg.yaml"));
}
